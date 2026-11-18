// The realistic map layer: Satisfactory Calculator's imagery, used with permission, streamed tile by tile.
//
// Each zoom level is one UAtlasTileSet of JPEG tiles (about 60 MB for levels 3 to 7). Only the tiles in view
// are decoded, on worker threads, into small mipmapped textures kept in a capped cache. A tile that is not
// ready yet shows the coarser level beneath it, so panning and zooming never show holes.

#include "AtlasRealistic.h"

#include "AtlasTypes.h"
#include "FactoryAtlas.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Containers/Queue.h"
#include "Modules/ModuleManager.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Tasks/Task.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

void UAtlasTileSet::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	if (Ar.IsObjectReferenceCollector())
	{
		return; // Only bytes in here, no references
	}
	int32 Version = 1;
	Ar << Version;
	if (Ar.IsLoading() && Version != 1)
	{
		Offsets.Reset();
		Data.Reset();
		return;
	}
	Ar << Offsets;
	Ar << Data;
}

TConstArrayView<uint8> UAtlasTileSet::GetTile(int32 X, int32 Y) const
{
	const int32 Col = X - First;
	const int32 Row = Y - First;
	if (Col < 0 || Row < 0 || Col >= Count || Row >= Count || Offsets.Num() != Count * Count + 1)
	{
		return {};
	}
	const int32 Index = Row * Count + Col;
	const int32 Start = Offsets[Index];
	const int32 End = Offsets[Index + 1];
	if (Start < 0 || End <= Start || End > Data.Num())
	{
		return {};
	}
	return TConstArrayView<uint8>(Data.GetData() + Start, End - Start);
}

namespace Atlas::Realistic
{
	const TCHAR* const Credit = TEXT("Realistic map imagery © Satisfactory Calculator (satisfactory-calculator.com), used with permission");
}

// Named, not anonymous: unity builds put this file next to others with helpers of the same names.
namespace AtlasRealisticDetail
{
	constexpr int32 MinLevel = 3;
	constexpr int32 MaxLevel = 7;
	constexpr int32 TilePixels = 256;
	constexpr int32 TileMips = 9;            // 256 down to 1
	constexpr double RasterPixels = 40960.0; // Level 8, the whole layer
	constexpr double BorderPixels = 4096.0;  // Around Atlas's map rectangle
	constexpr double MapPixels = 32768.0;    // Atlas's map rectangle

	constexpr int32 MaxTiles = 320;          // About 110 MB of textures at most
	constexpr int32 MaxInFlight = 16;        // Decodes queued on worker threads
	constexpr int32 MaxUploadsPerPaint = 12; // Textures made per paint, to keep frames even
	constexpr double IdleSeconds = 20.0;     // A tile out of view this long is freed

	FString PackageName(int32 Level)
	{
		return FString::Printf(TEXT("/FactoryAtlas/Realistic/T_Realistic_L%d"), Level);
	}

	FString AssetName(int32 Level)
	{
		return FString::Printf(TEXT("T_Realistic_L%d"), Level);
	}

	/** Level-8 pixels a tile at this level spans. */
	double TileSpan(int32 Level)
	{
		return double(TilePixels << (8 - Level));
	}

	uint64 TileKey(int32 Level, int32 X, int32 Y)
	{
		return (uint64(Level) << 40) | (uint64(uint32(X)) << 20) | uint64(uint32(Y));
	}

	struct FDecodedTile
	{
		uint64 Key = 0;
		TArray<TArray<uint8>> Mips; // BGRA, 256 px down to 1
	};

	/** Shared with the workers, so it outlives a Release while decodes are still running. */
	struct FResults
	{
		TQueue<TUniquePtr<FDecodedTile>, EQueueMode::Mpsc> Done;
		std::atomic<int32> InFlight{ 0 };
	};

	struct FTile
	{
		TStrongObjectPtr<UTexture2D> Texture;
		FSlateBrush Brush;
		uint64 LastFrame = 0;
		double LastSeconds = 0.0;
		bool bReady = false;
	};

	struct FState
	{
		int32 Generation = 0;
		TStrongObjectPtr<UAtlasTileSet> Sets[MaxLevel + 1];
		bool bRequested[MaxLevel + 1] = {};
		TMap<uint64, FTile> Tiles;
		TSharedRef<FResults, ESPMode::ThreadSafe> Results = MakeShared<FResults, ESPMode::ThreadSafe>();
	};

	// A raw pointer, never destroyed at exit: its strong object pointers must not outlive the object system.
	FState* GState = nullptr;
	int32 NextGeneration = 1;
	bool bMissing = false;
	bool bLoggedMissing = false;

	FState& GetState()
	{
		if (!GState)
		{
			GState = new FState();
			GState->Generation = NextGeneration++;
		}
		return *GState;
	}

	void RequestLevels(FState& S)
	{
		for (int32 Level = MinLevel; Level <= MaxLevel; Level++)
		{
			if (S.bRequested[Level])
			{
				continue;
			}
			S.bRequested[Level] = true;
			const int32 Generation = S.Generation;
			LoadPackageAsync(PackageName(Level), FLoadPackageAsyncDelegate::CreateLambda(
				[Level, Generation](const FName&, UPackage* Package, EAsyncLoadingResult::Type Result)
				{
					if (!GState || GState->Generation != Generation)
					{
						return; // Released since
					}
					UAtlasTileSet* Set = Result == EAsyncLoadingResult::Succeeded && Package
						? FindObject<UAtlasTileSet>(Package, *AssetName(Level)) : nullptr;
					if (!Set || Set->Level != Level || Set->Count <= 0 || Set->Offsets.Num() != Set->Count * Set->Count + 1)
					{
						bMissing = true;
						if (!bLoggedMissing)
						{
							bLoggedMissing = true;
							UE_LOG(LogFactoryAtlas, Warning, TEXT("Realistic map level %d did not load (%s); this build has no realistic imagery"),
								Level, Set ? TEXT("malformed") : TEXT("missing"));
						}
						return;
					}
					GState->Sets[Level].Reset(Set);
					UE_LOG(LogFactoryAtlas, Display, TEXT("Realistic map level %d loaded: %d tiles, %.1f MB"),
						Level, Set->Count * Set->Count, Set->Data.Num() / 1048576.0);
				}));
		}
	}

	IImageWrapperModule* ImageWrappers()
	{
		// Loaded here, on the game thread; the workers only use it.
		static IImageWrapperModule* Module = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		return Module;
	}

	void StartDecode(FState& S, const UAtlasTileSet& Set, int32 Level, int32 X, int32 Y, uint64 Key)
	{
		TArray<uint8> Bytes(Set.GetTile(X, Y));
		IImageWrapperModule* Module = ImageWrappers();
		TSharedRef<FResults, ESPMode::ThreadSafe> Results = S.Results;
		Results->InFlight++;
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [Bytes = MoveTemp(Bytes), Module, Results, Key]()
		{
			TUniquePtr<FDecodedTile> Out = MakeUnique<FDecodedTile>();
			Out->Key = Key;
			const TSharedPtr<IImageWrapper> Wrapper = Module->CreateImageWrapper(EImageFormat::JPEG);
			TArray64<uint8> Raw;
			if (Wrapper && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
				&& Wrapper->GetWidth() == TilePixels && Wrapper->GetHeight() == TilePixels
				&& Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) && Raw.Num() == int64(TilePixels) * TilePixels * 4)
			{
				// Mipmaps by 2x2 averaging, so the tiles stay smooth when the map is zoomed out past them.
				Out->Mips.SetNum(TileMips);
				Out->Mips[0] = TArray<uint8>(Raw.GetData(), int32(Raw.Num()));
				for (int32 Mip = 1, Size = TilePixels / 2; Mip < TileMips; Mip++, Size /= 2)
				{
					const TArray<uint8>& Src = Out->Mips[Mip - 1];
					TArray<uint8>& Dst = Out->Mips[Mip];
					Dst.SetNumUninitialized(Size * Size * 4);
					const int32 SrcSize = Size * 2;
					for (int32 Y = 0; Y < Size; Y++)
					{
						for (int32 X = 0; X < Size; X++)
						{
							const uint8* A = &Src[((Y * 2) * SrcSize + X * 2) * 4];
							const uint8* B = A + SrcSize * 4;
							uint8* D = &Dst[(Y * Size + X) * 4];
							for (int32 C = 0; C < 4; C++)
							{
								D[C] = uint8((A[C] + A[C + 4] + B[C] + B[C + 4] + 2) / 4);
							}
						}
					}
				}
			}
			Results->Done.Enqueue(MoveTemp(Out));
			Results->InFlight--;
		});
	}

	UTexture2D* MakeTexture(const FDecodedTile& Decoded)
	{
		// No image data here: given data, CreateTransient calls UpdateResource itself, and the render thread then
		// reads PlatformData->Mips while the loop below grows that array. A reallocation in between hands it a
		// freed mip of size 0, and D3D12 asserts on the 0x0 texture. So fill every mip first, then make the
		// resource once.
		UTexture2D* Texture = UTexture2D::CreateTransient(TilePixels, TilePixels, PF_B8G8R8A8);
		if (!Texture)
		{
			return nullptr;
		}
		FTexturePlatformData* Platform = Texture->GetPlatformData();
		{
			FByteBulkData& Bulk = Platform->Mips[0].BulkData;
			Bulk.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Bulk.Realloc(Decoded.Mips[0].Num()), Decoded.Mips[0].GetData(), Decoded.Mips[0].Num());
			Bulk.Unlock();
		}
		for (int32 Mip = 1, Size = TilePixels / 2; Mip < Decoded.Mips.Num(); Mip++, Size /= 2)
		{
			FTexture2DMipMap* Level = new FTexture2DMipMap(Size, Size, 1);
			Platform->Mips.Add(Level);
			Level->BulkData.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Level->BulkData.Realloc(Decoded.Mips[Mip].Num()), Decoded.Mips[Mip].GetData(), Decoded.Mips[Mip].Num());
			Level->BulkData.Unlock();
		}
		Texture->SRGB = true;
		Texture->Filter = TF_Trilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->LODGroup = TEXTUREGROUP_UI;
		Texture->NeverStream = true;
		Texture->UpdateResource();
		return Texture;
	}

	void TakeResults(FState& S)
	{
		TUniquePtr<FDecodedTile> Decoded;
		for (int32 Made = 0; Made < MaxUploadsPerPaint && S.Results->Done.Dequeue(Decoded); )
		{
			FTile* Tile = S.Tiles.Find(Decoded->Key);
			if (!Tile || Tile->bReady)
			{
				continue; // Freed while it was decoding
			}
			if (Decoded->Mips.Num() != TileMips)
			{
				S.Tiles.Remove(Decoded->Key); // Corrupt tile: asked for again next paint, cheap enough
				continue;
			}
			UTexture2D* Texture = MakeTexture(*Decoded);
			if (!Texture)
			{
				S.Tiles.Remove(Decoded->Key);
				continue;
			}
			Tile->Texture.Reset(Texture);
			Tile->Brush.SetResourceObject(Texture);
			Tile->Brush.ImageSize = FVector2f(float(TilePixels));
			Tile->Brush.DrawAs = ESlateBrushDrawType::Image;
			Tile->Brush.Tiling = ESlateBrushTileType::NoTile;
			Tile->bReady = true;
			Made++;
		}
	}

	void Evict(FState& S)
	{
		const double Now = FPlatformTime::Seconds();
		const uint64 Recent = GFrameCounter > 1 ? GFrameCounter - 1 : 0;
		for (auto It = S.Tiles.CreateIterator(); It; ++It)
		{
			if (It->Value.LastFrame < Recent && Now - It->Value.LastSeconds > IdleSeconds)
			{
				It.RemoveCurrent();
			}
		}
		if (S.Tiles.Num() <= MaxTiles)
		{
			return;
		}
		// Over the cap: the least recently drawn go first, never one drawn this frame or the last.
		TArray<TPair<uint64, uint64>> ByAge; // Last frame, key
		for (const TPair<uint64, FTile>& Pair : S.Tiles)
		{
			if (Pair.Value.LastFrame < Recent)
			{
				ByAge.Emplace(Pair.Value.LastFrame, Pair.Key);
			}
		}
		ByAge.Sort([](const TPair<uint64, uint64>& A, const TPair<uint64, uint64>& B) { return A.Key < B.Key; });
		for (int32 i = 0; i < ByAge.Num() && S.Tiles.Num() > MaxTiles; i++)
		{
			S.Tiles.Remove(ByAge[i].Value);
		}
	}

	/** The tiles of one level that a view shows, as an inclusive range of SCIM tile columns and rows. */
	struct FRange
	{
		int32 X0 = 0, Y0 = 0, X1 = -1, Y1 = -1;
		bool IsEmpty() const { return X1 < X0 || Y1 < Y0; }
	};

	FRange VisibleTiles(const UAtlasTileSet& Set, int32 Level, const FVector2D& PixelMin, const FVector2D& PixelMax)
	{
		const double Span = TileSpan(Level);
		FRange Range;
		Range.X0 = FMath::Max(Set.First, FMath::FloorToInt32(PixelMin.X / Span));
		Range.Y0 = FMath::Max(Set.First, FMath::FloorToInt32(PixelMin.Y / Span));
		Range.X1 = FMath::Min(Set.First + Set.Count - 1, FMath::FloorToInt32(PixelMax.X / Span));
		Range.Y1 = FMath::Min(Set.First + Set.Count - 1, FMath::FloorToInt32(PixelMax.Y / Span));
		return Range;
	}
}


namespace Atlas::Realistic
{
	int32 Paint(FVector2f Origin, FVector2f Scale, const FSlateRect& CullingRect, FSlateWindowElementList& Out, int32 LayerId)
	{
		using namespace AtlasRealisticDetail;
		if (bMissing || FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y))
		{
			return INDEX_NONE;
		}
		FState& S = GetState();
		RequestLevels(S);
		TakeResults(S);
		Evict(S);
		if (!S.Sets[MinLevel].IsValid())
		{
			return INDEX_NONE; // Still loading; the coarsest level is the floor everything else stands on
		}

		// What of the map rectangle is on screen, in paint space and then in level-8 pixels.
		const FVector2f End = Origin + Scale;
		const FSlateRect MapRect(FMath::Min(Origin.X, End.X), FMath::Min(Origin.Y, End.Y), FMath::Max(Origin.X, End.X), FMath::Max(Origin.Y, End.Y));
		const FSlateRect Clip = MapRect.IntersectionWith(CullingRect);
		if (Clip.IsEmpty())
		{
			return LayerId;
		}
		const auto ToPixels = [&](float X, float Y)
		{
			return FVector2D(BorderPixels + double((X - Origin.X) / Scale.X) * MapPixels, BorderPixels + double((Y - Origin.Y) / Scale.Y) * MapPixels);
		};
		const FVector2D A = ToPixels(Clip.Left, Clip.Top);
		const FVector2D B = ToPixels(Clip.Right, Clip.Bottom);
		const FVector2D PixelMin(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
		const FVector2D PixelMax(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y));
		const FVector2D PixelCentre = (PixelMin + PixelMax) * 0.5;

		// The level whose pixels come closest to the screen's. Past level 7 the imagery has no more detail.
		const double ScreenPerPixel = FMath::Abs(double(Scale.X)) / MapPixels;
		int32 Target = FMath::Clamp(FMath::RoundToInt32(8.0 + FMath::Log2(ScreenPerPixel)), MinLevel, MaxLevel);
		while (Target > MinLevel && !S.Sets[Target].IsValid())
		{
			Target--; // Not loaded yet
		}

		// Paint-space edges of tile column or row I at a level, rounded so neighbours share them exactly.
		const auto Edge = [&](int32 Level, int32 I, bool bY)
		{
			const double Pixel = I * TileSpan(Level);
			const double Uv = (Pixel - BorderPixels) / MapPixels;
			return bY ? FMath::RoundToFloat(Origin.Y + float(Uv) * Scale.Y) : FMath::RoundToFloat(Origin.X + float(Uv) * Scale.X);
		};

		const double Now = FPlatformTime::Seconds();
		const auto Touch = [&](int32 Level, int32 X, int32 Y, TArray<TPair<double, FIntVector>>& Wanted) -> FTile*
		{
			const uint64 Key = TileKey(Level, X, Y);
			FTile* Tile = S.Tiles.Find(Key);
			if (!Tile)
			{
				if (S.Sets[Level]->GetTile(X, Y).IsEmpty())
				{
					return nullptr;
				}
				const double Span = TileSpan(Level);
				const FVector2D Centre((X + 0.5) * Span, (Y + 0.5) * Span);
				Wanted.Emplace(FVector2D::DistSquared(Centre, PixelCentre), FIntVector(Level, X, Y));
				return nullptr;
			}
			Tile->LastFrame = GFrameCounter;
			Tile->LastSeconds = Now;
			return Tile->bReady ? Tile : nullptr;
		};

		int32 Drawn = 0;
		const auto Draw = [&](const FTile& Tile, int32 Level, int32 X, int32 Y, int32 Layer)
		{
			Drawn++;
			const float Left = Edge(Level, X, false), Right = Edge(Level, X + 1, false);
			const float Top = Edge(Level, Y, true), Bottom = Edge(Level, Y + 1, true);
			const FVector2f Corner(FMath::Min(Left, Right), FMath::Min(Top, Bottom));
			const FVector2f Size(FMath::Abs(Right - Left), FMath::Abs(Bottom - Top));
			FSlateDrawElement::MakeBox(Out, Layer, FPaintGeometry(Corner, Size, 1.f), &Tile.Brush);
		};

		TArray<TPair<double, FIntVector>> Wanted;
		const FRange TargetRange = VisibleTiles(*S.Sets[Target], Target, PixelMin, PixelMax);
		if (TargetRange.IsEmpty())
		{
			return LayerId;
		}

		// Which target tiles are ready. The coarser levels are drawn only where one is not.
		const int32 Cols = TargetRange.X1 - TargetRange.X0 + 1;
		const int32 Rows = TargetRange.Y1 - TargetRange.Y0 + 1;
		TArray<FTile*> Ready;
		Ready.SetNumZeroed(Cols * Rows);
		bool bAllReady = true;
		for (int32 Y = TargetRange.Y0; Y <= TargetRange.Y1; Y++)
		{
			for (int32 X = TargetRange.X0; X <= TargetRange.X1; X++)
			{
				FTile* Tile = Touch(Target, X, Y, Wanted);
				Ready[(Y - TargetRange.Y0) * Cols + (X - TargetRange.X0)] = Tile;
				bAllReady &= Tile != nullptr || S.Sets[Target]->GetTile(X, Y).IsEmpty();
			}
		}

		Out.PushClip(FSlateClippingZone(Clip));
		int32 Layer = LayerId;
		if (!bAllReady)
		{
			for (int32 Level = MinLevel; Level < Target; Level++)
			{
				if (!S.Sets[Level].IsValid())
				{
					continue;
				}
				const int32 Shift = Target - Level;
				const FRange Range = VisibleTiles(*S.Sets[Level], Level, PixelMin, PixelMax);
				for (int32 Y = Range.Y0; Y <= Range.Y1; Y++)
				{
					for (int32 X = Range.X0; X <= Range.X1; X++)
					{
						// Needed only if one of its target-level tiles in view is not ready.
						bool bNeeded = false;
						const int32 CX0 = FMath::Max(X << Shift, TargetRange.X0), CX1 = FMath::Min(((X + 1) << Shift) - 1, TargetRange.X1);
						const int32 CY0 = FMath::Max(Y << Shift, TargetRange.Y0), CY1 = FMath::Min(((Y + 1) << Shift) - 1, TargetRange.Y1);
						for (int32 CY = CY0; CY <= CY1 && !bNeeded; CY++)
						{
							for (int32 CX = CX0; CX <= CX1 && !bNeeded; CX++)
							{
								bNeeded = !Ready[(CY - TargetRange.Y0) * Cols + (CX - TargetRange.X0)];
							}
						}
						if (!bNeeded)
						{
							continue;
						}
						// The coarsest level is always fetched, so there is something under everything. Others are
						// drawn only if still cached from an earlier zoom.
						TArray<TPair<double, FIntVector>> Ignored;
						FTile* Tile = Touch(Level, X, Y, Level == MinLevel ? Wanted : Ignored);
						if (Tile)
						{
							Draw(*Tile, Level, X, Y, Layer);
						}
					}
				}
				Layer++;
			}
		}
		for (int32 Y = TargetRange.Y0; Y <= TargetRange.Y1; Y++)
		{
			for (int32 X = TargetRange.X0; X <= TargetRange.X1; X++)
			{
				if (const FTile* Tile = Ready[(Y - TargetRange.Y0) * Cols + (X - TargetRange.X0)])
				{
					Draw(*Tile, Target, X, Y, Layer);
				}
			}
		}
		Out.PopClip();

		// Coarse tiles first, then the nearest to the middle of the view.
		Wanted.Sort([](const TPair<double, FIntVector>& L, const TPair<double, FIntVector>& R)
		{
			return L.Value.X != R.Value.X ? L.Value.X < R.Value.X : L.Key < R.Key;
		});
		for (const TPair<double, FIntVector>& Want : Wanted)
		{
			if (S.Results->InFlight.load() >= MaxInFlight)
			{
				break;
			}
			const FIntVector& T = Want.Value;
			const uint64 Key = TileKey(T.X, T.Y, T.Z);
			if (S.Tiles.Contains(Key))
			{
				continue;
			}
			FTile& Tile = S.Tiles.Add(Key);
			Tile.LastFrame = GFrameCounter;
			Tile.LastSeconds = Now;
			StartDecode(S, *S.Sets[T.X], T.X, T.Y, T.Z, Key);
		}
		// Nothing decoded yet for this part of the map: let the caller show its own terrain meanwhile.
		return Drawn > 0 ? Layer : INDEX_NONE;
	}

	void PaintCredit(const FSlateRect& Area, float DrawScale, FSlateWindowElementList& Out, int32 LayerId)
	{
		using namespace AtlasRealisticDetail;
		static const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 8);
		const float Inset = 10.f * DrawScale;
		const float Height = 14.f * DrawScale;
		const FVector2f Where(Area.Left + Inset, Area.Bottom - Inset - Height);
		const FVector2f Size(Area.GetSize().X - Inset * 2.f, Height);
		const FString Text(Credit);
		FSlateDrawElement::MakeText(Out, LayerId, FPaintGeometry(Where + FVector2f(1.f, 1.f) * DrawScale, Size, DrawScale), Text, Font,
			ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.75f));
		FSlateDrawElement::MakeText(Out, LayerId + 1, FPaintGeometry(Where, Size, DrawScale), Text, Font,
			ESlateDrawEffect::None, FLinearColor(0.92f, 0.93f, 0.94f, 0.9f));
	}

	bool IsAvailable()
	{
		using namespace AtlasRealisticDetail;
		return !bMissing;
	}

	UAtlasTileSet* LoadLevelNow(int32 Level)
	{
		using namespace AtlasRealisticDetail;
		if (Level < MinLevel || Level > MaxLevel)
		{
			return nullptr;
		}
		if (GState && GState->Sets[Level].IsValid())
		{
			return GState->Sets[Level].Get();
		}
		UAtlasTileSet* Set = LoadObject<UAtlasTileSet>(nullptr, *(PackageName(Level) + TEXT(".") + AssetName(Level)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Set || Set->Level != Level || Set->Count <= 0 || Set->Offsets.Num() != Set->Count * Set->Count + 1)
		{
			UE_LOG(LogFactoryAtlas, Display, TEXT("Realistic map level %d is not in this build; the export has no terrain"), Level);
			return nullptr;
		}
		return Set;
	}

	void Release()
	{
		using namespace AtlasRealisticDetail;
		if (GState)
		{
			const int32 Tiles = GState->Tiles.Num();
			delete GState; // Decodes still running finish into their own queue, which they keep alive
			GState = nullptr;
			UE_LOG(LogFactoryAtlas, Display, TEXT("Realistic map released (%d tiles)"), Tiles);
		}
	}
}
