// Rendered map export: the world seen from straight above, every building as the game draws it.
//
// A transient orthographic scene capture walks the map in a 32 x 32 grid of tiles, row by row and a few tiles a
// frame, skipping tiles with no buildings. It draws only buildings (a show-only list of every buildable's meshes
// plus the instance managers that draw foundations, walls and most machine bodies), so the terrain the game has
// streamed in around the player doesn't patch the image. The realistic imagery goes underneath instead, when the
// map uses it and this build has it.
//
// Each tile is captured at twice the output resolution with a margin all round, as linear HDR with coverage in
// alpha, then cropped, averaged down, exposed and tone mapped on a worker. One exposure, measured once on the
// densest tile, serves every tile so the seams don't show. A finished row of tiles is laid over the terrain and
// streamed into the PNG while the next row is captured.

#include "AtlasSubsystem.h"

#include "AbstractInstanceManager.h"
#include "AtlasPng.h"
#include "AtlasRealistic.h"
#include "AtlasTypes.h"
#include "Async/ParallelFor.h"
#include "Buildables/FGBuildable.h"
#include "Components/MeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "FactoryAtlas.h"
#include "FGBuildableSubsystem.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Tasks/Task.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

using namespace Atlas;

namespace AtlasRenderDetail
{
	constexpr int32 Tiles = 32;               // Per side at every size, so a tile is always about 234 m
	constexpr int32 Supersample = 2;          // Captured at twice the output resolution, then averaged
	constexpr float MarginFraction = 0.125f;  // Captured round each tile and cropped, so screen-space effects see their neighbours
	constexpr double TickBudgetSeconds = 0.05;
	constexpr int32 MaxRowsQueued = 2;        // Rows of tiles waiting to be written before capturing waits
	constexpr float ExposureKey = 0.18f;      // Average brightness of the buildings once exposed: middle grey
	constexpr double LightweightReachCm = 6000.0; // A record holds a building's centre height; how far a foundation or wall may reach from it
	constexpr double AboveCm = 1000.0;        // The camera sits this far above the tile's highest building
	constexpr double MinDepthCm = 10000.0;
	constexpr double SpreadOutCm = 150000.0;  // A mesh wider than this would sit in most tiles; it goes in every capture instead

	/** The colour a linear value shows as: the ACES fit the engine's own tone curve is close to. */
	float ToneMap(float X)
	{
		X = FMath::Max(X, 0.f);
		return FMath::Clamp((X * (2.51f * X + 0.03f)) / (X * (2.43f * X + 0.59f) + 0.14f), 0.f, 1.f);
	}

	FString ExportPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir()
			/ FString::Printf(TEXT("Atlas_Rendered_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
	}
}

namespace Atlas
{
	struct FRenderTile
	{
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Components;
		double TopCm = -UE_BIG_NUMBER;
		double BottomCm = UE_BIG_NUMBER;
		int32 Records = 0;
		bool bUsed = false;
	};

	/** What the worker tasks share. It outlives the job if they are still running when it ends. */
	struct FRenderShared
	{
		FPngStreamWriter Png;
		std::atomic<bool> bFailed{ false };
		std::atomic<int32> RowsQueued{ 0 };
		const UAtlasTileSet* Terrain = nullptr; // Kept alive by the job, which waits for the last write before letting go
		int32 TerrainLevel = 0;
		IImageWrapperModule* ImageWrappers = nullptr;
	};

	struct FRenderJob
	{
		TSharedPtr<FExportState> State;
		TSharedPtr<FRenderShared> Shared;
		TStrongObjectPtr<UAtlasTileSet> Terrain;
		TWeakObjectPtr<ASceneCapture2D> Capture;
		TStrongObjectPtr<UTextureRenderTarget2D> Target;

		/** Drawn in every capture: the instance managers' meshes, which span the whole map. */
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Always;
		TArray<FRenderTile> Grid;

		int32 Size = 0;           // Output pixels a side
		int32 TilePixels = 0;     // Output pixels a tile
		int32 CapturePixels = 0;  // Capture pixels a tile, before the margin
		int32 MarginPixels = 0;   // Capture pixels of margin on each side
		int32 TargetPixels = 0;   // The render target's side
		double TileCm = 0.0;
		double TargetCm = 0.0;    // World width the render target covers, margin included
		float Exposure = 1.f;
		bool bCalibrated = false;

		int32 Next = 0;           // The next tile, row by row
		TSharedPtr<TArray64<FColor>> Row;
		TArray<UE::Tasks::FTask> RowTasks;
		UE::Tasks::FTask LastWrite;
		bool bCaptured = false;   // Every tile is in; waiting for the file to be finished
	};
}

namespace AtlasRenderDetail
{
	bool CaptureTile(FRenderJob& J, USceneCaptureComponent2D& Capture, int32 Index, TArray<FFloat16Color>& Out)
	{
		const FRenderTile& Tile = J.Grid[Index];
		const double CenterX = MapWestCm + (Index % Tiles + 0.5) * J.TileCm;
		const double CenterY = MapNorthCm + (Index / Tiles + 0.5) * J.TileCm;
		const double Top = Tile.TopCm + AboveCm;
		const double Depth = FMath::Max(Top - Tile.BottomCm + AboveCm, MinDepthCm);

		// Looking straight down with north up: the camera's up is -Y and its right +X, as on the map.
		Capture.SetWorldLocationAndRotation(FVector(CenterX, CenterY, Top), FRotator(-90.0, -90.0, 0.0));
		Capture.ShowOnlyComponents = J.Always;
		Capture.ShowOnlyComponents.Append(Tile.Components);
		// A near plane at the camera and a far one just past the lowest building, so depth keeps its precision.
		const double Half = J.TargetCm * 0.5;
		Capture.OrthoWidth = float(J.TargetCm);
		Capture.CustomProjectionMatrix = FReversedZOrthoMatrix(Half, Half, 1.0 / Depth, 0.0);
		Capture.CaptureScene();

		FTextureRenderTargetResource* Resource = J.Target->GameThread_GetRenderTargetResource();
		return Resource && Resource->ReadFloat16Pixels(Out) && Out.Num() == J.TargetPixels * J.TargetPixels;
	}

	/** One exposure for the whole export, from the tile with the most buildings, so the tiles match. */
	void Calibrate(FRenderJob& J, USceneCaptureComponent2D& Capture)
	{
		int32 Best = INDEX_NONE;
		for (int32 i = 0; i < J.Grid.Num(); i++)
		{
			if (J.Grid[i].bUsed && (Best == INDEX_NONE || J.Grid[i].Records > J.Grid[Best].Records))
			{
				Best = i;
			}
		}
		TArray<FFloat16Color> Pixels;
		if (Best == INDEX_NONE || !CaptureTile(J, Capture, Best, Pixels))
		{
			return;
		}
		double SumLog = 0.0;
		int64 Count = 0;
		for (const FFloat16Color& P : Pixels)
		{
			if (P.A.GetFloat() < 0.5f)
			{
				const float Luminance = 0.2126f * P.R.GetFloat() + 0.7152f * P.G.GetFloat() + 0.0722f * P.B.GetFloat();
				if (FMath::IsFinite(Luminance) && Luminance > 0.f)
				{
					SumLog += FMath::Loge(FMath::Max(double(Luminance), 1e-4));
					Count++;
				}
			}
		}
		if (Count > 64)
		{
			J.Exposure = float(FMath::Clamp(ExposureKey / FMath::Exp(SumLog / Count), 1e-6, 1e6));
		}
		UE_LOG(LogFactoryAtlas, Display, TEXT("Rendered export: exposure %g from tile %d (%lld lit pixels)"), J.Exposure, Best, Count);
	}

	/** Crops, averages down, exposes and tone maps one capture into its place in the row. Straight alpha: coverage. */
	void ProcessTile(const TArray<FFloat16Color>& In, int32 TargetPixels, int32 Margin, int32 TilePixels, float Exposure,
		TArray64<FColor>& Row, int32 RowWidth, int32 Column)
	{
		constexpr float Samples = float(Supersample * Supersample);
		for (int32 Y = 0; Y < TilePixels; Y++)
		{
			FColor* Out = Row.GetData() + int64(Y) * RowWidth + int64(Column) * TilePixels;
			for (int32 X = 0; X < TilePixels; X++)
			{
				float R = 0.f, G = 0.f, B = 0.f, Cover = 0.f;
				for (int32 SY = 0; SY < Supersample; SY++)
				{
					const FFloat16Color* Line = In.GetData() + int64(Margin + Y * Supersample + SY) * TargetPixels + Margin + X * Supersample;
					for (int32 SX = 0; SX < Supersample; SX++)
					{
						const FFloat16Color& P = Line[SX];
						// The capture keeps the scene's alpha inverted: 1 where nothing was drawn.
						const float Covered = 1.f - FMath::Clamp(P.A.GetFloat(), 0.f, 1.f);
						if (Covered <= 0.f)
						{
							continue;
						}
						const float PR = P.R.GetFloat(), PG = P.G.GetFloat(), PB = P.B.GetFloat();
						if (FMath::IsFinite(PR) && FMath::IsFinite(PG) && FMath::IsFinite(PB))
						{
							R += FMath::Min(PR, 65504.f);
							G += FMath::Min(PG, 65504.f);
							B += FMath::Min(PB, 65504.f);
						}
						Cover += Covered;
					}
				}
				if (Cover < 0.02f)
				{
					Out[X] = FColor(0, 0, 0, 0);
					continue;
				}
				// The colour is over black, so dividing by coverage gives the building's own.
				const float Scale = Exposure / Cover;
				FColor Color = FLinearColor(ToneMap(R * Scale), ToneMap(G * Scale), ToneMap(B * Scale)).ToFColorSRGB();
				Color.A = uint8(FMath::Clamp(FMath::RoundToInt32(Cover / Samples * 255.f), 0, 255));
				Out[X] = Color;
			}
		}
	}

	/** Puts the realistic imagery under a row band, bilinear from the level nearest the export's resolution. */
	void LayOverTerrain(const FRenderShared& S, TArray64<FColor>& Row, int32 Size, int32 Top, int32 Rows)
	{
		const int32 Side = Realistic::TileSide;
		const double Scale = double(Realistic::MapPixelsOf(S.TerrainLevel)) / Size;
		const double Border = Realistic::BorderPixelsOf(S.TerrainLevel);
		const auto ToLevel = [Scale, Border](int32 Pixel) { return (Pixel + 0.5) * Scale + Border - 0.5; };

		const int32 FirstX = FMath::FloorToInt32(ToLevel(0)) / Side;
		const int32 LastX = (FMath::FloorToInt32(ToLevel(Size - 1)) + 1) / Side;
		const int32 FirstY = FMath::FloorToInt32(ToLevel(Top)) / Side;
		const int32 LastY = (FMath::FloorToInt32(ToLevel(Top + Rows - 1)) + 1) / Side;
		const int32 Cols = LastX - FirstX + 1;
		const int32 TileRows = LastY - FirstY + 1;

		TArray<TArray64<uint8>> Decoded;
		Decoded.SetNum(Cols * TileRows);
		ParallelFor(Cols * TileRows, [&](int32 i)
		{
			const TConstArrayView<uint8> Bytes = S.Terrain->GetTile(FirstX + i % Cols, FirstY + i / Cols);
			const TSharedPtr<IImageWrapper> Wrapper = Bytes.IsEmpty() ? nullptr : S.ImageWrappers->CreateImageWrapper(EImageFormat::JPEG);
			TArray64<uint8> Raw;
			if (Wrapper && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) && Wrapper->GetWidth() == Side && Wrapper->GetHeight() == Side
				&& Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) && Raw.Num() == int64(Side) * Side * 4)
			{
				Decoded[i] = MoveTemp(Raw);
			}
		});

		// BGRA bytes are an FColor's own layout. Null where the imagery has no tile.
		const auto Texel = [&](int32 X, int32 Y) -> const FColor*
		{
			const int32 Col = FMath::Clamp(X / Side - FirstX, 0, Cols - 1);
			const int32 RowIndex = FMath::Clamp(Y / Side - FirstY, 0, TileRows - 1);
			const TArray64<uint8>& Tile = Decoded[RowIndex * Cols + Col];
			return Tile.IsEmpty() ? nullptr : reinterpret_cast<const FColor*>(Tile.GetData()) + (Y % Side) * Side + X % Side;
		};

		ParallelFor(Rows, [&](int32 R)
		{
			const double FY = ToLevel(Top + R);
			const int32 Y0 = FMath::FloorToInt32(FY);
			const float WY = float(FY - Y0);
			FColor* Out = Row.GetData() + int64(R) * Size;
			for (int32 X = 0; X < Size; X++)
			{
				if (Out[X].A == 255)
				{
					continue; // A building covers it
				}
				const double FX = ToLevel(X);
				const int32 X0 = FMath::FloorToInt32(FX);
				const float WX = float(FX - X0);
				const FColor* C00 = Texel(X0, Y0);
				const FColor* C10 = Texel(X0 + 1, Y0);
				const FColor* C01 = Texel(X0, Y0 + 1);
				const FColor* C11 = Texel(X0 + 1, Y0 + 1);
				if (!C00 || !C10 || !C01 || !C11)
				{
					continue;
				}
				const FColor Above = Out[X];
				const float A = Above.A / 255.f;
				const auto Over = [WX, WY, A](uint8 V00, uint8 V10, uint8 V01, uint8 V11, uint8 Top)
				{
					const float Under = FMath::Lerp(FMath::Lerp(float(V00), float(V10), WX), FMath::Lerp(float(V01), float(V11), WX), WY);
					return uint8(FMath::Clamp(FMath::RoundToInt32(FMath::Lerp(Under, float(Top), A)), 0, 255));
				};
				Out[X] = FColor(
					Over(C00->R, C10->R, C01->R, C11->R, Above.R),
					Over(C00->G, C10->G, C01->G, C11->G, Above.G),
					Over(C00->B, C10->B, C01->B, C11->B, Above.B),
					255);
			}
		});
	}

	void QueueRowWrite(FRenderJob& J, int32 TileRow)
	{
		TArray<UE::Tasks::FTask> Before = MoveTemp(J.RowTasks);
		J.RowTasks.Reset();
		if (J.LastWrite.IsValid())
		{
			Before.Add(J.LastWrite); // Rows go into the file in order
		}
		J.Shared->RowsQueued++;
		J.LastWrite = UE::Tasks::Launch(UE_SOURCE_LOCATION,
			[Shared = J.Shared, Row = J.Row, State = J.State, Size = J.Size, Top = TileRow * J.TilePixels, Rows = J.TilePixels]()
			{
				if (!Shared->bFailed.load())
				{
					if (Shared->Terrain)
					{
						LayOverTerrain(*Shared, *Row, Size, Top, Rows);
					}
					if (!Shared->Png.WriteRows(Row->GetData(), Rows))
					{
						Shared->bFailed.store(true);
					}
				}
				State->RowsDone.store(Top + Rows);
				Shared->RowsQueued--;
			}, Before);
		J.Row.Reset();
	}
}

void UAtlasSubsystem::StartRenderExport()
{
	using namespace AtlasRenderDetail;

	UWorld* World = GetWorld();
	const FGrid* Current = GetGrid();
	AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
	if (!World || !Current || !Buildables || RenderJob || IsRunningDedicatedServer())
	{
		return;
	}

	TSharedPtr<FRenderJob> NewJob = MakeShared<FRenderJob>();
	FRenderJob& J = *NewJob;
	J.Shared = MakeShared<FRenderShared>();
	J.Size = Settings.ExportSize;
	J.TilePixels = J.Size / Tiles;
	J.CapturePixels = J.TilePixels * Supersample;
	J.MarginPixels = FMath::Max(8, FMath::RoundToInt32(J.CapturePixels * MarginFraction));
	J.TargetPixels = J.CapturePixels + J.MarginPixels * 2;
	J.TileCm = MapWidthCm / Tiles; // The map is square to within a centimetre
	J.TargetCm = J.TileCm * J.TargetPixels / J.CapturePixels;
	J.Grid.SetNum(Tiles * Tiles);

	// Every tile a box reaches, its margin included, since a building just outside a tile still shows in it.
	const double MarginCm = (J.TargetCm - J.TileCm) * 0.5;
	const auto ForTiles = [&J, MarginCm](double MinX, double MinY, double MaxX, double MaxY, TFunctionRef<void(FRenderTile&)> Visit)
	{
		const double Left = (MinX - MarginCm - MapWestCm) / J.TileCm;
		const double Right = (MaxX + MarginCm - MapWestCm) / J.TileCm;
		const double Upper = (MinY - MarginCm - MapNorthCm) / J.TileCm;
		const double Lower = (MaxY + MarginCm - MapNorthCm) / J.TileCm;
		if (Right < 0.0 || Lower < 0.0 || Left >= Tiles || Upper >= Tiles)
		{
			return;
		}
		const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Left), 0, Tiles - 1);
		const int32 X1 = FMath::Clamp(FMath::FloorToInt32(Right), 0, Tiles - 1);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(Upper), 0, Tiles - 1);
		const int32 Y1 = FMath::Clamp(FMath::FloorToInt32(Lower), 0, Tiles - 1);
		for (int32 Y = Y0; Y <= Y1; Y++)
		{
			for (int32 X = X0; X <= X1; X++)
			{
				Visit(J.Grid[Y * Tiles + X]);
			}
		}
	};

	// The map's own records say where buildings are, foundations and walls included, and roughly how high.
	for (const TArray<FVector4f>& Cell : Current->Cells)
	{
		for (const FVector4f& Packed : Cell)
		{
			const FDecoded Record(Packed);
			if (Record.Type >= AnyMarkerFirst)
			{
				continue; // Markers, radiation and highlights, not buildings
			}
			const double X = MapWestCm + double(Record.X) / PositionMax * MapWidthCm;
			const double Y = MapNorthCm + double(Record.Y) / PositionMax * MapHeightCm;
			const double Z = Record.Z * ZStepCm - ZBaseCm;
			const double Radius = FMath::Sqrt(double(Record.HalfX * Record.HalfX + Record.HalfY * Record.HalfY)) * 10.0;
			ForTiles(X - Radius, Y - Radius, X + Radius, Y + Radius, [Z](FRenderTile& Tile)
			{
				Tile.bUsed = true;
				Tile.Records++;
				Tile.TopCm = FMath::Max(Tile.TopCm, Z + LightweightReachCm);
				Tile.BottomCm = FMath::Min(Tile.BottomCm, Z - LightweightReachCm);
			});
		}
	}

	// Each buildable's own meshes go in the captures of the tiles they reach; their bounds give the true heights.
	TArray<UMeshComponent*> Meshes;
	for (AFGBuildable* Buildable : Buildables->GetAllBuildablesRef())
	{
		if (!IsValid(Buildable) || Buildable->IsHidden())
		{
			continue;
		}
		Meshes.Reset();
		Buildable->GetComponents<UMeshComponent>(Meshes);
		for (UMeshComponent* Mesh : Meshes)
		{
			if (!Mesh || !Mesh->IsRegistered() || !Mesh->IsVisible())
			{
				continue;
			}
			const FBox Box = Mesh->Bounds.GetBox();
			if (Box.GetSize().X > SpreadOutCm || Box.GetSize().Y > SpreadOutCm)
			{
				J.Always.Add(Mesh);
				continue;
			}
			ForTiles(Box.Min.X, Box.Min.Y, Box.Max.X, Box.Max.Y, [Mesh, &Box](FRenderTile& Tile)
			{
				Tile.bUsed = true;
				Tile.Components.Add(Mesh);
				Tile.TopCm = FMath::Max(Tile.TopCm, Box.Max.Z);
				Tile.BottomCm = FMath::Min(Tile.BottomCm, Box.Min.Z);
			});
		}
	}

	// The instance managers draw lightweight buildings and the static parts of most others, across the whole map.
	const auto AddAlways = [&J, &Meshes](AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}
		Meshes.Reset();
		Actor->GetComponents<UMeshComponent>(Meshes);
		for (UMeshComponent* Mesh : Meshes)
		{
			if (Mesh && Mesh->IsRegistered() && !Mesh->IsA<ULightweightCollisionComponent>())
			{
				J.Always.Add(Mesh);
			}
		}
	};
	AddAlways(AAbstractInstanceManager::GetInstanceManager(World));
	AddAlways(Buildables);
	if (const FObjectProperty* Property = FindFProperty<FObjectProperty>(AFGBuildableSubsystem::StaticClass(), TEXT("mBuildableInstancesActor")))
	{
		AddAlways(Cast<AActor>(Property->GetObjectPropertyValue_InContainer(Buildables)));
	}

	int32 Used = 0;
	for (const FRenderTile& Tile : J.Grid)
	{
		Used += Tile.bUsed ? 1 : 0;
	}

	// The realistic level whose pixels are nearest the export's: 4096 and 8192 match exactly, 32768 doubles level 7.
	if (Settings.bRealisticTerrain)
	{
		const int32 Level = FMath::Clamp(FMath::RoundToInt32(FMath::Log2(double(J.Size) / 128.0)), Realistic::CoarsestLevel, Realistic::FinestLevel);
		if (UAtlasTileSet* Set = Realistic::LoadLevelNow(Level))
		{
			J.Terrain.Reset(Set);
			J.Shared->Terrain = Set;
			J.Shared->TerrainLevel = Level;
		}
	}
	// Loaded here, on the game thread; the workers only use it.
	J.Shared->ImageWrappers = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Target->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
	Target->InitCustomFormat(J.TargetPixels, J.TargetPixels, PF_FloatRGBA, true);
	Target->UpdateResourceImmediate(true);
	J.Target.Reset(Target);

	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transient;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), FTransform::Identity, Spawn);
	USceneCaptureComponent2D* Capture = CaptureActor ? CaptureActor->GetCaptureComponent2D() : nullptr;
	if (!Capture)
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Rendered export: the scene capture could not be made"));
		return;
	}
	J.Capture = CaptureActor;
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->bAlwaysPersistRenderingState = false;
	Capture->bExcludeFromSceneTextureExtents = true; // A one-off, so the main view's buffers don't grow to match
	Capture->TextureTarget = Target;
	Capture->CaptureSource = SCS_SceneColorHDR;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->ProjectionType = ECameraProjectionMode::Orthographic;
	Capture->bAutoCalculateOrthoPlanes = false;
	Capture->bUpdateOrthoPlanes = false;
	Capture->bUseCustomProjectionMatrix = true;
	Capture->LODDistanceFactor = 0.5f; // Finer meshes than a view from this height would pick

	// Nothing that differs from tile to tile or hides the buildings: no fog, sky, eye adaptation or effects.
	TArray<FEngineShowFlagsSetting> Flags;
	for (const TCHAR* Name : { TEXT("Fog"), TEXT("Atmosphere"), TEXT("Cloud"), TEXT("VolumetricFog"), TEXT("Particles"),
		TEXT("EyeAdaptation"), TEXT("MotionBlur"), TEXT("TemporalAA"), TEXT("AntiAliasing"), TEXT("Bloom"),
		TEXT("LensFlares"), TEXT("DepthOfField"), TEXT("ScreenSpaceReflections") })
	{
		FEngineShowFlagsSetting Flag;
		Flag.ShowFlagName = Name;
		Flag.Enabled = false;
		Flags.Add(Flag);
	}
	Capture->SetShowFlagSettings(Flags);

	// A fixed exposure, so the colour is the same in every tile, and no Lumen, which needs a history a one-off
	// capture doesn't have.
	FPostProcessSettings& Post = Capture->PostProcessSettings;
	Post.bOverride_AutoExposureMethod = true;
	Post.AutoExposureMethod = AEM_Manual;
	Post.bOverride_AutoExposureBias = true;
	Post.AutoExposureBias = 0.f;
	Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post.AutoExposureApplyPhysicalCameraExposure = 0;
	Post.bOverride_DynamicGlobalIlluminationMethod = true;
	Post.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
	Post.bOverride_ReflectionMethod = true;
	Post.ReflectionMethod = EReflectionMethod::None;
	Capture->PostProcessBlendWeight = 1.f;

	TSharedPtr<FExportState> State = MakeShared<FExportState>();
	State->Stage.store(1);
	State->Path = ExportPath();
	State->bRendered = true;
	State->TilesTotal.store(Used);
	State->Rows = J.Size;
	J.State = State;
	ExportState = State;

	if (!J.Shared->Png.Open(State->Path, J.Size, J.Size))
	{
		CaptureActor->Destroy();
		State->Stage.store(3);
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Rendered export: could not write %s"), *State->Path);
		return;
	}

	UE_LOG(LogFactoryAtlas, Display, TEXT("Rendered export: %d px, %d of %d tiles to capture at %d px, %d meshes always drawn, terrain %s"),
		J.Size, Used, Tiles * Tiles, J.TargetPixels, J.Always.Num(), J.Terrain.IsValid() ? TEXT("realistic") : TEXT("none"));
	RenderJob = NewJob;
	RenderTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float DeltaSeconds)
	{
		return TickRenderExport(DeltaSeconds);
	}));
}

bool UAtlasSubsystem::TickRenderExport(float DeltaSeconds)
{
	using namespace AtlasRenderDetail;

	if (!RenderJob)
	{
		RenderTicker.Reset();
		return false;
	}
	FRenderJob& J = *RenderJob;

	if (J.bCaptured)
	{
		if (!J.LastWrite.IsValid() || J.LastWrite.IsCompleted())
		{
			StopRenderExport(J.Shared->bFailed.load());
			return false;
		}
		return true;
	}

	USceneCaptureComponent2D* Capture = J.Capture.IsValid() ? J.Capture->GetCaptureComponent2D() : nullptr;
	if (!Capture || !J.Target.IsValid() || J.Shared->bFailed.load())
	{
		StopRenderExport(true);
		return false;
	}

	if (!J.bCalibrated)
	{
		Calibrate(J, *Capture);
		J.bCalibrated = true;
		return true; // That was this frame's capture
	}

	const double Start = FPlatformTime::Seconds();
	while (J.Next < Tiles * Tiles && J.Shared->RowsQueued.load() < MaxRowsQueued && FPlatformTime::Seconds() - Start < TickBudgetSeconds)
	{
		const int32 Index = J.Next++;
		const int32 Column = Index % Tiles;
		if (Column == 0)
		{
			J.Row = MakeShared<TArray64<FColor>>();
			J.Row->SetNumZeroed(int64(J.Size) * J.TilePixels);
		}

		if (J.Grid[Index].bUsed)
		{
			TSharedPtr<TArray<FFloat16Color>> Pixels = MakeShared<TArray<FFloat16Color>>();
			if (!CaptureTile(J, *Capture, Index, *Pixels))
			{
				UE_LOG(LogFactoryAtlas, Warning, TEXT("Rendered export: tile %d could not be read back"), Index);
				StopRenderExport(true);
				return false;
			}
			J.RowTasks.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION,
				[Pixels, Row = J.Row, Target = J.TargetPixels, Margin = J.MarginPixels, TilePixels = J.TilePixels,
				Exposure = J.Exposure, Size = J.Size, Column]()
				{
					ProcessTile(*Pixels, Target, Margin, TilePixels, Exposure, *Row, Size, Column);
				}));
			J.State->TilesDone++;
		}

		if (Column == Tiles - 1)
		{
			QueueRowWrite(J, Index / Tiles);
		}
	}

	if (J.Next >= Tiles * Tiles)
	{
		// Every tile is in: finish the file once the last row is written, and let the capture go.
		J.bCaptured = true;
		TArray<UE::Tasks::FTask> Before;
		if (J.LastWrite.IsValid())
		{
			Before.Add(J.LastWrite);
		}
		J.LastWrite = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Shared = J.Shared]()
		{
			const bool bClosed = Shared->Png.Close();
			if (!bClosed)
			{
				Shared->bFailed.store(true);
			}
		}, Before);
		if (J.Capture.IsValid())
		{
			J.Capture->Destroy();
		}
	}
	return true;
}

void UAtlasSubsystem::StopRenderExport(bool bFailed)
{
	if (RenderTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RenderTicker);
		RenderTicker.Reset();
	}
	if (!RenderJob)
	{
		return;
	}
	TSharedPtr<FRenderJob> Ending = MoveTemp(RenderJob);
	RenderJob.Reset();

	if (bFailed || !Ending->bCaptured)
	{
		Ending->Shared->bFailed.store(true); // Queued rows skip their work; the unfinished file is deleted
	}
	if (Ending->Capture.IsValid())
	{
		Ending->Capture->Destroy();
	}
	// The writes read the terrain the job keeps alive, so they finish before it goes. Once failed, they are quick.
	if (Ending->LastWrite.IsValid())
	{
		Ending->LastWrite.Wait();
	}

	const bool bSaved = !Ending->Shared->bFailed.load();
	Ending->State->Stage.store(bSaved ? 2 : 3);
	UE_LOG(LogFactoryAtlas, Display, TEXT("Rendered export %s: %s"), bSaved ? TEXT("saved") : TEXT("failed"), *Ending->State->Path);
}
