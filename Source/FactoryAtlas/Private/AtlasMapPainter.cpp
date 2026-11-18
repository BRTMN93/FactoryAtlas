#include "AtlasMapPainter.h"

#include "AtlasSubsystem.h"
#include "AtlasTypes.h"
#include "Algo/BinarySearch.h"
#include "Algo/StableSort.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/Texture2D.h"
#include "FactoryAtlas.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/ISlateRHIRendererModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "SlateMaterialBrush.h"
#include "UObject/Package.h"

using namespace Atlas;

// Named, not anonymous: unity builds put files with helpers of the same names next to this one.
namespace AtlasPainterDetail
{
	constexpr int64 UploadBudgetBytes = 16 * 1024 * 1024;

	// Recipe icons appear from this zoom on, sized to the building's shorter side within these limits.
	constexpr float IconMinPixelsPerMetre = 1.5f;
	constexpr float IconFill = 0.7f;
	constexpr float IconMinPixels = 12.f;
	constexpr float IconMaxPixels = 48.f;

	// Status lights only show from this zoom on, the same as recipe icons: zoomed out they are just noise.
	constexpr float StatusMinPixelsPerMetre = 1.5f;

	ISlateRHIRendererModule& Renderer()
	{
		return FModuleManager::Get().LoadModuleChecked<ISlateRHIRendererModule>(TEXT("SlateRHIRenderer"));
	}
}

bool FAtlasMapPainter::HasAnythingToDraw(const UAtlasSubsystem& Atlas)
{
	return Atlas.IsLayerEnabled() || Atlas.IsRadiationShown() || Atlas.AnyMarkersShown();
}

void FAtlasMapPainter::Reset()
{
	Slabs.Reset();
	CellVersions.Reset();
	CellSlabs.Reset();
	DirtySlabs = FSlabMask();
	StatusCells.Reset();
	HitsBuffer.Reset();
	HitsVersion = 0;
	HitsCount = 0;
	RadiationBuffer.Reset();
	RadiationVersion = 0;
	RadiationCount = 0;
	MarkerBuffer.Reset();
	MarkerVersion = 0;
	MarkerCount = 0;
	SelectionBuffer.Reset();
	bSelectionReady = false;
	HighlightBuffer.Reset();
	bHighlightReady = false;
	IconBrushes.Reset();
	IconTextures.Reset();
}

bool FAtlasMapPainter::EnsureMaterial(UAtlasSubsystem& Atlas)
{
	if (Material && MaterialHandle.IsValid())
	{
		return true;
	}
	if (bMaterialFailed)
	{
		return false;
	}

	UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/FactoryAtlas/M_AtlasInstances.M_AtlasInstances"));
	if (!Parent)
	{
		bMaterialFailed = true;
		UE_LOG(LogFactoryAtlas, Error, TEXT("M_AtlasInstances did not load"));
		return false;
	}

	Material.Reset(UMaterialInstanceDynamic::Create(Parent, GetTransientPackage()));
	Material->SetTextureParameterValue(TEXT("Palette"), Atlas.GetPalette());
	Brush = MakeShared<FSlateMaterialBrush>(*Material.Get(), FVector2f(64.f, 64.f));
	MaterialHandle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush);

	StatusMaterial.Reset(UMaterialInstanceDynamic::Create(Parent, GetTransientPackage()));
	StatusMaterial->SetTextureParameterValue(TEXT("Palette"), Atlas.GetPalette());
	StatusMaterial->SetScalarParameterValue(TEXT("StatusMode"), 1.f);
	StatusBrush = MakeShared<FSlateMaterialBrush>(*StatusMaterial.Get(), FVector2f(64.f, 64.f));
	StatusHandle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*StatusBrush);
	return MaterialHandle.IsValid();
}

void FAtlasMapPainter::SetScalar(const TCHAR* Name, float Value)
{
	Material->SetScalarParameterValue(Name, Value);
	if (StatusMaterial)
	{
		StatusMaterial->SetScalarParameterValue(Name, Value);
	}
}

void FAtlasMapPainter::UpdateParameters(UAtlasSubsystem& Atlas)
{
	const FVector2f Filter = Atlas.GetHeightFilter();
	if (Filter.X != AppliedLow || Filter.Y != AppliedHigh)
	{
		AppliedLow = Filter.X;
		AppliedHigh = Filter.Y;
		SetScalar(TEXT("ZMin"), float(MetresToZ(Filter.X)));
		SetScalar(TEXT("ZMax"), float(MetresToZ(Filter.Y)));
	}

	const int32 Paint = Atlas.IsPaintMode() ? 1 : 0;
	if (Paint != AppliedPaint)
	{
		AppliedPaint = Paint;
		SetScalar(TEXT("PaintMode"), float(Paint));
	}

	const FSettings& Settings = Atlas.GetSettings();
	const int32 Outlines = Settings.bOutlines ? 1 : 0;
	if (Outlines != AppliedOutlines)
	{
		AppliedOutlines = Outlines;
		SetScalar(TEXT("Outlines"), float(Outlines));
	}
	if (Settings.Opacity != AppliedOpacity)
	{
		AppliedOpacity = Settings.Opacity;
		// Status lights stay solid however faint the buildings are drawn.
		Material->SetScalarParameterValue(TEXT("Opacity"), Settings.Opacity);
	}
}

void FAtlasMapPainter::Upload(UAtlasSubsystem& Atlas)
{
	int64 Budget = AtlasPainterDetail::UploadBudgetBytes;
	if (const FGrid* Grid = Atlas.GetGrid())
	{
		UploadSlabs(*Grid, Budget);
	}
	if (const FGrid* Status = Atlas.GetStatusGrid())
	{
		UploadGrid(*Status, StatusCells, Budget);
	}
	else
	{
		StatusCells.Reset();
	}
}

void FAtlasMapPainter::UploadGrid(const FGrid& Grid, TArray<FCellBuffer>& Out, int64& Budget)
{
	Out.SetNum(GridCells);
	for (int32 i = 0; i < GridCells && Budget > 0; i++)
	{
		FCellBuffer& Cell = Out[i];
		if (Cell.Version == Grid.Versions[i])
		{
			continue;
		}

		const TArray<FVector4f>& Source = Grid.Cells[i];
		Cell.Version = Grid.Versions[i];
		Cell.NumInstances = Source.Num();
		if (Source.IsEmpty())
		{
			Cell.Buffer.Reset();
			continue;
		}

		FSlateInstanceBufferData Data = Source;
		if (!Cell.Buffer)
		{
			Cell.Buffer = AtlasPainterDetail::Renderer().CreateInstanceBuffer(Data.Num());
		}
		Cell.Buffer->Update(Data);
		Budget -= int64(Source.Num()) * sizeof(FVector4f);
	}
}

void FAtlasMapPainter::UploadSlabs(const FGrid& Grid, int64& Budget)
{
	if (Slabs.Num() != SlabCount)
	{
		Slabs.SetNum(SlabCount);
		CellVersions.Init(0, GridCells);
		CellSlabs.SetNum(GridCells);
		DirtySlabs = FSlabMask();
	}

	// A changed cell dirties the slabs it was in and the slabs it is in now: its records may have moved, gone or come.
	for (int32 i = 0; i < GridCells; i++)
	{
		if (CellVersions[i] == Grid.Versions[i])
		{
			continue;
		}
		CellVersions[i] = Grid.Versions[i];
		FSlabMask Now;
		for (const FVector4f& Record : Grid.Cells[i])
		{
			const int32 Slab = DecodeZ(Record) / SlabSteps;
			Now.Words[Slab >> 6] |= uint64(1) << (Slab & 63);
		}
		for (int32 Word = 0; Word < SlabWords; Word++)
		{
			DirtySlabs.Words[Word] |= CellSlabs[i].Words[Word] | Now.Words[Word];
		}
		CellSlabs[i] = Now;
	}

	for (int32 Slab = 0; Slab < SlabCount && Budget > 0; Slab++)
	{
		const int32 Word = Slab >> 6;
		const uint64 Bit = uint64(1) << (Slab & 63);
		if (!(DirtySlabs.Words[Word] & Bit))
		{
			continue;
		}
		DirtySlabs.Words[Word] &= ~Bit;

		// Each cell is sorted by height, so its part of the slab is one contiguous run.
		const int32 Low = Slab * SlabSteps;
		const int32 High = Low + SlabSteps;
		FSlateInstanceBufferData Data;
		for (int32 i = 0; i < GridCells; i++)
		{
			if (!(CellSlabs[i].Words[Word] & Bit))
			{
				continue;
			}
			const TArray<FVector4f>& Cell = Grid.Cells[i];
			const int32 First = Algo::LowerBoundBy(Cell, Low, &DecodeZ);
			const int32 Last = Algo::LowerBoundBy(Cell, High, &DecodeZ);
			Data.Append(Cell.GetData() + First, Last - First);
		}
		Algo::StableSortBy(Data, &DecodeZ);

		FSlab& Out = Slabs[Slab];
		Out.NumInstances = Data.Num();
		if (Data.IsEmpty())
		{
			continue;
		}
		if (!Out.Buffer)
		{
			Out.Buffer = AtlasPainterDetail::Renderer().CreateInstanceBuffer(Data.Num());
		}
		Budget -= int64(Data.Num()) * sizeof(FVector4f);
		Out.Buffer->Update(Data);
	}
}

void FAtlasMapPainter::UpdateBuffer(const TArray<FVector4f>& Source, uint32 Version, uint32& AppliedVersion, int32& Count,
	TSharedPtr<ISlateUpdatableInstanceBuffer>& Buffer)
{
	if (Version == AppliedVersion)
	{
		return;
	}
	AppliedVersion = Version;
	Count = Source.Num();
	if (Source.IsEmpty())
	{
		return;
	}
	if (!Buffer)
	{
		Buffer = AtlasPainterDetail::Renderer().CreateInstanceBuffer(Source.Num());
	}
	FSlateInstanceBufferData Data = Source;
	Buffer->Update(Data);
}

void FAtlasMapPainter::UpdateSingle(const FVector4f& Record, FVector4f& Applied, bool& bReady, TSharedPtr<ISlateUpdatableInstanceBuffer>& Buffer)
{
	if (bReady && Record == Applied)
	{
		return;
	}
	Applied = Record;
	bReady = true;
	if (!Buffer)
	{
		Buffer = AtlasPainterDetail::Renderer().CreateInstanceBuffer(1);
	}
	FSlateInstanceBufferData Data;
	Data.Add(Record);
	Buffer->Update(Data);
}

const FSlateBrush* FAtlasMapPainter::IconBrush(UTexture2D* Icon)
{
	if (const TSharedPtr<FSlateBrush>* Found = IconBrushes.Find(Icon))
	{
		return Found->Get();
	}
	TSharedPtr<FSlateBrush> Made = MakeShared<FSlateBrush>();
	Made->SetResourceObject(Icon);
	Made->ImageSize = FVector2D(Icon->GetSizeX(), Icon->GetSizeY());
	IconBrushes.Add(Icon, Made);
	IconTextures.Emplace(Icon);
	return Made.Get();
}

int32 FAtlasMapPainter::PaintRecipeIcons(UAtlasSubsystem& Atlas, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	FVector2f Origin, FVector2f Scale, float PixelsPerMetre, const FSlateRect& CullingRect)
{
	using namespace AtlasPainterDetail;
	static const FSlateRoundedBoxBrush Backdrop(FLinearColor::White, 5.f);
	const FLinearColor BackdropColor(0.02f, 0.025f, 0.03f, 0.72f);

	const FVector2f Filter = Atlas.GetHeightFilter();
	const int32 ZLow = MetresToZ(Filter.X);
	const int32 ZHigh = MetresToZ(Filter.Y);
	const TArray<FType>& Types = Atlas.GetTypes();
	bool bDrew = false;
	for (const FRecipeIcon& Entry : Atlas.GetRecipeIcons())
	{
		if (!Types.IsValidIndex(Entry.Type) || Types[Entry.Type].bHidden || Atlas.IsCategoryHidden(Types[Entry.Type].Category))
		{
			continue;
		}
		const int32 Z = MetresToZ(float(Entry.Location.Z / 100.0));
		if (Z < ZLow || Z > ZHigh)
		{
			continue;
		}

		const float Size = FMath::Clamp(Entry.HalfCm * 2.f / 100.f * PixelsPerMetre * IconFill, IconMinPixels, IconMaxPixels);
		const FVector2f Uv(float((Entry.Location.X - MapWestCm) / MapWidthCm), float((Entry.Location.Y - MapNorthCm) / MapHeightCm));
		const FVector2f Center = Origin + Scale * Uv;
		const float Pad = FMath::RoundToFloat(Size * 0.14f);
		const float Reach = Size * 0.5f + Pad;
		if (Center.X + Reach < CullingRect.Left || Center.X - Reach > CullingRect.Right || Center.Y + Reach < CullingRect.Top || Center.Y - Reach > CullingRect.Bottom)
		{
			continue;
		}

		const FVector2f Corner(FMath::RoundToFloat(Center.X - Size * 0.5f), FMath::RoundToFloat(Center.Y - Size * 0.5f));
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId, FPaintGeometry(Corner - FVector2f(Pad), FVector2f(Size + Pad * 2.f), 1.f),
			&Backdrop, ESlateDrawEffect::None, BackdropColor);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, FPaintGeometry(Corner, FVector2f(Size), 1.f), IconBrush(Entry.Icon));
		bDrew = true;
	}
	return bDrew ? LayerId + 1 : LayerId - 1;
}

namespace AtlasPortColors
{
	/** Belts: orange out, blue in. Pipes: greens. Hypertubes: violet. Tracks: pale grey. */
	FLinearColor PortColor(const FPort& Port)
	{
		switch (Port.Kind)
		{
		case EPortKind::Pipe:
			return Port.bAnyFlow ? FLinearColor(0.45f, 0.9f, 0.5f, 1.f) : Port.bOutput ? FLinearColor(0.7f, 0.95f, 0.25f, 1.f) : FLinearColor(0.15f, 0.8f, 0.6f, 1.f);
		case EPortKind::Hypertube:
			return FLinearColor(0.75f, 0.5f, 1.f, 1.f);
		case EPortKind::Rail:
			return FLinearColor(0.85f, 0.85f, 0.8f, 1.f);
		default:
			return Port.bOutput ? FLinearColor(1.f, 0.55f, 0.15f, 1.f) : FLinearColor(0.25f, 0.75f, 1.f, 1.f);
		}
	}
}

int32 FAtlasMapPainter::PaintPorts(UAtlasSubsystem& Atlas, EView View, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	FVector2f Origin, FVector2f Scale, const FSlateRect& CullingRect)
{
	static const FSlateRoundedBoxBrush Disc(FLinearColor::White, 8.f);
	const auto PortColor = [](const FPort& Port) { return AtlasPortColors::PortColor(Port); };
	const FLinearColor Edge(0.02f, 0.02f, 0.025f, 0.9f);
	constexpr float NodeSize = 10.f;
	constexpr float HotSize = 15.f;
	constexpr float TickLength = 11.f;

	const TArray<FPort>& Ports = Atlas.GetPorts();
	const FBeltDrag& Drag = Atlas.GetBeltDrag();
	const bool bDragging = Drag.bActive;
	// Placing, a port can't be dragged from, so none lights up under the cursor.
	const int32 Hot = Atlas.IsPlacing(View) ? INDEX_NONE : Atlas.GetPortUnderCursor(View);
	const auto ToScreen = [&](FVector2f Uv) { return Origin + Scale * Uv; };

	const int32 NodeLayer = LayerId + 1;
	for (int32 i = 0; i < Ports.Num(); i++)
	{
		const FPort& Port = Ports[i];
		const FVector2f Center = ToScreen(Port.Uv);
		if (Center.X + HotSize < CullingRect.Left || Center.X - HotSize > CullingRect.Right || Center.Y + HotSize < CullingRect.Top || Center.Y - HotSize > CullingRect.Bottom)
		{
			continue;
		}
		const bool bStart = bDragging && Port.Connection == Drag.Start.Connection;
		// While dragging, only the ports the line could end on stay bright.
		const float Alpha = bDragging && !bStart && !Atlas.IsTargetFor(Port) ? 0.25f : 1.f;
		const float Size = (i == Hot || bStart) ? HotSize : NodeSize;
		const FLinearColor Color = PortColor(Port);
		const FLinearColor Fill = (i == Hot || bStart) ? FLinearColor::White : Color;

		// Which way the port faces, so you can tell which side a line leaves from.
		const FVector2f Facing = FVector2f(float(Port.Normal.X), float(Port.Normal.Y)).GetSafeNormal();
		if (!Facing.IsNearlyZero())
		{
			TArray<FVector2f> Tick = { Center, Center + Facing * (Size * 0.5f + TickLength) };
			FSlateDrawElement::MakeLines(OutDrawElements, NodeLayer, FPaintGeometry(), Tick, ESlateDrawEffect::None,
				Color.CopyWithNewOpacity(Alpha), true, 2.5f);
		}
		FSlateDrawElement::MakeBox(OutDrawElements, NodeLayer + 1, FPaintGeometry(Center - FVector2f(Size * 0.5f + 1.5f), FVector2f(Size + 3.f), 1.f),
			&Disc, ESlateDrawEffect::None, Edge.CopyWithNewOpacity(Edge.A * Alpha));
		FSlateDrawElement::MakeBox(OutDrawElements, NodeLayer + 2, FPaintGeometry(Center - FVector2f(Size * 0.5f), FVector2f(Size), 1.f),
			&Disc, ESlateDrawEffect::None, Fill.CopyWithNewOpacity(Alpha));
	}
	int32 TopLayer = NodeLayer + 2;

	if (!bDragging)
	{
		return TopLayer;
	}

	// The line: the hologram's own spline once it has settled on a port, otherwise a line to the cursor.
	TArray<FVector2f> Line;
	FLinearColor Color(1.f, 1.f, 1.f, 0.6f);
	float Thickness = 2.f;
	if (Drag.Points.Num() >= 2)
	{
		for (const FVector& Point : Drag.Points)
		{
			Line.Add(ToScreen(FVector2f(float((Point.X - MapWestCm) / MapWidthCm), float((Point.Y - MapNorthCm) / MapHeightCm))));
		}
		Thickness = 4.f;
		// Ending on a port or on the ground, the game's verdict colors it once it has settled.
		Color = !Drag.bReady ? FLinearColor(0.85f, 0.85f, 0.85f, 0.8f)
			: Drag.bValid ? FLinearColor(0.35f, 0.92f, 0.45f, 1.f) : FLinearColor(1.f, 0.25f, 0.2f, 1.f);
	}
	else
	{
		Line = { ToScreen(Drag.Start.Uv), ToScreen(Drag.bHasTarget ? Drag.Target.Uv : Atlas.GetMapCursorUv(Drag.View)) };
		if (Drag.bReady && !Drag.bValid && !Drag.Reason.IsEmpty())
		{
			Color = FLinearColor(1.f, 0.25f, 0.2f, 1.f);
		}
	}
	TopLayer++;
	FSlateDrawElement::MakeLines(OutDrawElements, TopLayer, FPaintGeometry(), Line, ESlateDrawEffect::None, Color, true, Thickness);
	return TopLayer;
}

int32 FAtlasMapPainter::PaintWire(const UAtlasSubsystem& Atlas, EView View, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	FVector2f Origin, FVector2f Scale) const
{
	static const FSlateRoundedBoxBrush Disc(FLinearColor::White, 8.f);
	const FWireTool& Wire = Atlas.GetWire();
	if (!Wire.bHasStart)
	{
		return LayerId;
	}
	const auto ToScreen = [&](const FVector& Location)
	{
		return Origin + Scale * FVector2f(float((Location.X - MapWestCm) / MapWidthCm), float((Location.Y - MapNorthCm) / MapHeightCm));
	};
	const FVector2f Start = ToScreen(Wire.StartLocation);
	const bool bOnStart = !Wire.bHasTarget && !Wire.bHasGround;
	const FVector2f End = bOnStart ? Origin + Scale * Atlas.GetMapCursorUv(View) : ToScreen(Wire.EndLocation);
	// Settled on something, the game's verdict colors it; until then, and over its own start, it is pale.
	const FLinearColor Color = !Wire.bReady || bOnStart ? FLinearColor(0.85f, 0.85f, 0.85f, 0.8f)
		: Wire.bValid ? FLinearColor(0.35f, 0.92f, 0.45f, 1.f) : FLinearColor(1.f, 0.25f, 0.2f, 1.f);
	const FLinearColor Power(1.f, 0.78f, 0.2f, 1.f);
	const FLinearColor Edge(0.02f, 0.02f, 0.025f, 0.9f);

	const TArray<FVector2f> Line = { Start, End };
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, FPaintGeometry(), Line, ESlateDrawEffect::None, Edge, true, 5.f);
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, FPaintGeometry(), Line, ESlateDrawEffect::None, Color, true, 2.5f);
	const auto Dot = [&](const FVector2f& Center, float Size, const FLinearColor& Fill)
	{
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 3, FPaintGeometry(Center - FVector2f(Size * 0.5f + 1.5f), FVector2f(Size + 3.f), 1.f),
			&Disc, ESlateDrawEffect::None, Edge);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 4, FPaintGeometry(Center - FVector2f(Size * 0.5f), FVector2f(Size), 1.f),
			&Disc, ESlateDrawEffect::None, Fill);
	};
	Dot(Start, 12.f, Power);
	if (!bOnStart)
	{
		// A pole on the ground is a smaller dot in the verdict's color.
		Dot(End, Wire.bHasTarget ? 12.f : 9.f, Wire.bHasTarget ? Power : Color);
	}
	return LayerId + 4;
}

int32 FAtlasMapPainter::PaintPlace(const UAtlasSubsystem& Atlas, EView View, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	FVector2f Origin, FVector2f Scale) const
{
	static const FSlateColorBrush Fill(FLinearColor::White);
	static const FSlateRoundedBoxBrush Disc(FLinearColor::White, 8.f);
	const FPlaceTool& Place = Atlas.GetPlace();
	if (!Place.Recipe)
	{
		return LayerId;
	}
	const auto ToScreen = [&](const FVector& Location)
	{
		return Origin + Scale * FVector2f(float((Location.X - MapWestCm) / MapWidthCm), float((Location.Y - MapNorthCm) / MapHeightCm));
	};
	const FLinearColor Pale(0.85f, 0.85f, 0.85f, 1.f);
	const FLinearColor Good(0.35f, 0.92f, 0.45f, 1.f);
	const FLinearColor Bad(1.f, 0.25f, 0.2f, 1.f);
	const FLinearColor Edge(0.02f, 0.02f, 0.025f, 0.9f);
	const FVector2f PixelsPerCm(FMath::Abs(Scale.X) / float(MapWidthCm), FMath::Abs(Scale.Y) / float(MapHeightCm));
	// Never smaller than a few pixels, so the ghost stays visible zoomed out.
	const FVector2f Half(FMath::Max(Place.GhostHalf.X * PixelsPerCm.X, 4.f), FMath::Max(Place.GhostHalf.Y * PixelsPerCm.Y, 4.f));
	constexpr float NodeSize = 9.f;
	constexpr float TickLength = 10.f;

	// The footprint where the hologram has the building, with its belt, pipe and hypertube ports, so it can be lined
	// up with what is already there.
	const auto Ghost = [&](const FTransform& Transform, const FLinearColor& Color)
	{
		const FVector2f Center = ToScreen(Transform.TransformPosition(Place.GhostBoxCenter));
		const float Yaw = FMath::DegreesToRadians(float(Transform.Rotator().Yaw));
		const float Cos = FMath::Cos(Yaw);
		const float Sin = FMath::Sin(Yaw);
		const auto Corner = [&](float X, float Y) { return Center + FVector2f(X * Cos - Y * Sin, X * Sin + Y * Cos); };
		const TArray<FVector2f> Outline = { Corner(-Half.X, -Half.Y), Corner(Half.X, -Half.Y), Corner(Half.X, Half.Y), Corner(-Half.X, Half.Y), Corner(-Half.X, -Half.Y) };
		FSlateDrawElement::MakeRotatedBox(OutDrawElements, LayerId + 1, FPaintGeometry(Center - Half, Half * 2.f, 1.f), &Fill,
			ESlateDrawEffect::None, Yaw, TOptional<FVector2f>(), FSlateDrawElement::RelativeToElement, Color.CopyWithNewOpacity(0.35f));
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 2, FPaintGeometry(), Outline, ESlateDrawEffect::None, Edge, true, 4.f);
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 3, FPaintGeometry(), Outline, ESlateDrawEffect::None, Color, true, 2.f);
		for (const FPort& Port : Place.GhostPorts)
		{
			const FVector2f At = ToScreen(Transform.TransformPosition(Port.Location));
			const FVector World = Transform.TransformVectorNoScale(Port.Normal);
			const FVector2f Facing = FVector2f(float(World.X), float(World.Y)).GetSafeNormal();
			const FLinearColor PortColor = AtlasPortColors::PortColor(Port);
			if (!Facing.IsNearlyZero())
			{
				const TArray<FVector2f> Tick = { At, At + Facing * (NodeSize * 0.5f + TickLength) };
				FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 4, FPaintGeometry(), Tick, ESlateDrawEffect::None, PortColor, true, 2.5f);
			}
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 5, FPaintGeometry(At - FVector2f(NodeSize * 0.5f + 1.5f), FVector2f(NodeSize + 3.f), 1.f),
				&Disc, ESlateDrawEffect::None, Edge);
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 6, FPaintGeometry(At - FVector2f(NodeSize * 0.5f), FVector2f(NodeSize), 1.f),
				&Disc, ESlateDrawEffect::None, PortColor);
		}
	};

	if (Place.bRow)
	{
		// Each spot of the row as the game judged it; built ones are on the map already.
		for (const FPlaceSpot& Spot : Place.Row)
		{
			if (Spot.State != EPlaceSpot::Built)
			{
				Ghost(Spot.Ghost, Spot.State == EPlaceSpot::Valid ? Good : Spot.State == EPlaceSpot::Blocked ? Bad : Pale);
			}
		}
	}
	else if (Place.bHasGhost)
	{
		// Settled, the game's verdict colors it; until then it is pale.
		Ghost(Place.GhostTransform, !Place.bReady ? Pale : Place.bValid ? Good : Bad);
	}
	return LayerId + 6;
}

int64 FAtlasMapPainter::DrawCells(const TArray<FCellBuffer>& Buffers, const FSlateResourceHandle& Handle, FSlateWindowElementList& OutDrawElements, int32 Layer,
	const TArray<FSlateVertex>& Vertices, FVector2f Origin, FVector2f Scale, const FSlateRect& CullingRect) const
{
	static const TArray<SlateIndex> Indices = { 0, 1, 2, 0, 2, 3 };
	const float CellUv = 1.f / GridSize;
	const float Margin = CellUv * 0.5f; // Long shapes overhang their cell
	int64 Drawn = 0;
	for (int32 i = 0; i < Buffers.Num(); i++)
	{
		const FCellBuffer& Cell = Buffers[i];
		if (!Cell.Buffer || Cell.NumInstances == 0)
		{
			continue;
		}

		const FVector2f UvMin = FVector2f(float(i % GridSize), float(i / GridSize)) * CellUv - FVector2f(Margin);
		const FVector2f Min = Origin + Scale * UvMin;
		const FVector2f Max = Min + Scale * (CellUv + Margin * 2.f);
		if (Max.X < CullingRect.Left || Min.X > CullingRect.Right || Max.Y < CullingRect.Top || Min.Y > CullingRect.Bottom)
		{
			continue;
		}

		FSlateDrawElement::MakeCustomVerts(OutDrawElements, Layer, Handle, Vertices, Indices, Cell.Buffer.Get(), 0, uint32(Cell.NumInstances));
		Drawn += Cell.NumInstances;
	}
	return Drawn;
}

int32 FAtlasMapPainter::Paint(UAtlasSubsystem& Atlas, EView View, FVector2f Origin, FVector2f Scale, const FSlateRect& CullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, bool bHighlightHover, const TCHAR* LogName)
{
	if (!HasAnythingToDraw(Atlas) || !EnsureMaterial(Atlas))
	{
		return LayerId;
	}

	const double PaintStart = FPlatformTime::Seconds();
	const bool bBuildings = Atlas.IsLayerEnabled();
	const float PixelsPerMetre = FMath::Abs(Scale.X) / MapWidthMetres;
	Atlas.FlushPalette();
	UpdateParameters(Atlas);
	Upload(Atlas);

	// The map-to-window transform rides on the four corner vertices, so panning touches no GPU data.
	static const TArray<SlateIndex> Indices = { 0, 1, 2, 0, 2, 3 };
	TArray<FSlateVertex> Vertices;
	Vertices.SetNumUninitialized(4);
	const FVector2f Corners[4] = { { -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } };
	for (int32 i = 0; i < 4; i++)
	{
		FSlateVertex& Vertex = Vertices[i];
		Vertex.TexCoords[0] = Scale.X;
		Vertex.TexCoords[1] = Scale.Y;
		Vertex.TexCoords[2] = Origin.X;
		Vertex.TexCoords[3] = Origin.Y;
		Vertex.MaterialTexCoords = Corners[i];
		Vertex.Position = Corners[i];
		Vertex.Color = FColor::White;
		Vertex.SecondaryColor = FColor::White;
		Vertex.PixelSize[0] = 0;
		Vertex.PixelSize[1] = 0;
	}

	const int32 DrawLayer = LayerId + 1;
	int64 DrawnInstances = 0;
	if (bBuildings)
	{
		// Lowest slab first. No culling: a slab spans the map, and off-screen quads cost the GPU next to nothing.
		for (const FSlab& Slab : Slabs)
		{
			if (Slab.Buffer && Slab.NumInstances > 0)
			{
				FSlateDrawElement::MakeCustomVerts(OutDrawElements, DrawLayer, MaterialHandle, Vertices, Indices, Slab.Buffer.Get(), 0, uint32(Slab.NumInstances));
				DrawnInstances += Slab.NumInstances;
			}
		}
	}

	int32 TopLayer = DrawLayer;
	if (Atlas.IsRadiationShown())
	{
		uint32 Version = 0;
		const TArray<FVector4f>& Circles = Atlas.GetRadiation(Version);
		UpdateBuffer(Circles, Version, RadiationVersion, RadiationCount, RadiationBuffer);
		if (RadiationBuffer && RadiationCount > 0)
		{
			TopLayer = DrawLayer + 1;
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, RadiationBuffer.Get(), 0, uint32(RadiationCount));
		}
	}

	if (bBuildings && Atlas.GetSettings().bRecipeIcons && PixelsPerMetre >= AtlasPainterDetail::IconMinPixelsPerMetre)
	{
		TopLayer = PaintRecipeIcons(Atlas, OutDrawElements, TopLayer + 1, Origin, Scale, PixelsPerMetre, CullingRect);
	}

	if (bBuildings && StatusHandle.IsValid() && Atlas.GetStatusGrid() && !StatusCells.IsEmpty()
		&& PixelsPerMetre >= AtlasPainterDetail::StatusMinPixelsPerMetre)
	{
		TopLayer = TopLayer + 1;
		DrawCells(StatusCells, StatusHandle, OutDrawElements, TopLayer, Vertices, Origin, Scale, CullingRect);
	}

	if (Atlas.AnyMarkersShown())
	{
		uint32 Version = 0;
		const TArray<FVector4f>& Points = Atlas.GetMarkers(Version);
		UpdateBuffer(Points, Version, MarkerVersion, MarkerCount, MarkerBuffer);
		if (MarkerBuffer && MarkerCount > 0)
		{
			TopLayer = TopLayer + 1;
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, MarkerBuffer.Get(), 0, uint32(MarkerCount));
		}
	}

	{
		// Buildings in the way of a measure tool copy.
		uint32 Version = 0;
		const TArray<FVector4f>& Hits = Atlas.GetMeasureHits(View, Version);
		UpdateBuffer(Hits, Version, HitsVersion, HitsCount, HitsBuffer);
		if (HitsBuffer && HitsCount > 0)
		{
			TopLayer = TopLayer + 1;
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, HitsBuffer.Get(), 0, uint32(HitsCount));
		}
	}

	if (Atlas.IsPicking(View))
	{
		// Buildings picked for dismantling, in red, or for a recipe change, in the accent colour.
		uint32 Version = 0;
		const TArray<FVector4f>& Picked = Atlas.GetDismantleRecords(Version);
		UpdateBuffer(Picked, Version, DismantleVersion, DismantleCount, DismantleBuffer);
		if (DismantleBuffer && DismantleCount > 0)
		{
			TopLayer = TopLayer + 1;
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, DismantleBuffer.Get(), 0, uint32(DismantleCount));
		}
		// The power lines connect mode would build, in the power colour; those past what can be paid for, pale.
		const FDismantleTool& Tool = Atlas.GetDismantle();
		if (Atlas.IsConnecting(View) && !Tool.bPreview && !Tool.Power.Wires.IsEmpty())
		{
			const auto ToScreen = [&](const FVector& Location)
			{
				return Origin + Scale * FVector2f(float((Location.X - MapWestCm) / MapWidthCm), float((Location.Y - MapNorthCm) / MapHeightCm));
			};
			const FLinearColor Edge(0.02f, 0.02f, 0.025f, 0.9f);
			const FLinearColor Power(1.f, 0.78f, 0.2f, 1.f);
			const FLinearColor Unpaid(0.85f, 0.85f, 0.85f, 0.6f);
			const TArray<FPlannedWire>& Wires = Tool.Power.Wires;
			for (int32 Index = 0; Index < Wires.Num(); Index++)
			{
				const TArray<FVector2f> Line = { ToScreen(Wires[Index].FromLocation), ToScreen(Wires[Index].ToLocation) };
				FSlateDrawElement::MakeLines(OutDrawElements, TopLayer + 1, FPaintGeometry(), Line, ESlateDrawEffect::None, Edge, true, 4.f);
				FSlateDrawElement::MakeLines(OutDrawElements, TopLayer + 2, FPaintGeometry(), Line, ESlateDrawEffect::None,
					Index < Tool.Power.Affordable ? Power : Unpaid, true, 2.f);
			}
			TopLayer += 2;
		}
	}

	if (Atlas.IsUpgradeOpen())
	{
		// The belt line an upgrade would replace, in the accent colour.
		uint32 Version = 0;
		const TArray<FVector4f>& Line = Atlas.GetUpgradeRecords(Version);
		UpdateBuffer(Line, Version, UpgradeVersion, UpgradeCount, UpgradeBuffer);
		if (UpgradeBuffer && UpgradeCount > 0)
		{
			TopLayer = TopLayer + 1;
			FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, UpgradeBuffer.Get(), 0, uint32(UpgradeCount));
		}
	}

	if (Atlas.HasSelection())
	{
		UpdateSingle(Atlas.GetSelectionRecord(), SelectionRecord, bSelectionReady, SelectionBuffer);
		TopLayer = TopLayer + 1;
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, SelectionBuffer.Get(), 0, 1);
	}

	const FHover& Hover = Atlas.GetHover();
	if (bHighlightHover && Hover.bValid && !Hover.bMarker && Atlas.GetSettings().bHighlight && !Atlas.IsPlacing(View))
	{
		UpdateSingle(Hover.Record, HighlightRecord, bHighlightReady, HighlightBuffer);
		TopLayer = TopLayer + 1;
		FSlateDrawElement::MakeCustomVerts(OutDrawElements, TopLayer, MaterialHandle, Vertices, Indices, HighlightBuffer.Get(), 0, 1);
	}

	// Wire and place mode hide the belt ports: a click there connects power or builds, it doesn't start a belt.
	if (Atlas.IsWiring(View))
	{
		TopLayer = PaintWire(Atlas, View, OutDrawElements, TopLayer, Origin, Scale);
	}
	else if (Atlas.IsPicking(View))
	{
		// The box being dragged, red when it takes buildings out of the pick.
		const FDismantleTool& Tool = Atlas.GetDismantle();
		if (Tool.bBoxing)
		{
			const FVector2f A = Origin + Scale * (Tool.PressMetres / MapWidthMetres);
			const FVector2f B = Origin + Scale * Atlas.GetMapCursorUv(View);
			const TArray<FVector2f> Box = { A, FVector2f(B.X, A.Y), B, FVector2f(A.X, B.Y), A };
			const FLinearColor Color = Tool.bRemove ? FLinearColor(0.6f, 0.75f, 1.f, 1.f) : FLinearColor(1.f, 0.3f, 0.25f, 1.f);
			FSlateDrawElement::MakeLines(OutDrawElements, TopLayer + 1, FPaintGeometry(), Box, ESlateDrawEffect::None, FLinearColor(0.02f, 0.02f, 0.025f, 0.9f), true, 4.f);
			FSlateDrawElement::MakeLines(OutDrawElements, TopLayer + 2, FPaintGeometry(), Box, ESlateDrawEffect::None, Color, true, 2.f);
			TopLayer += 2;
		}
	}
	else
	{
		// Placing, the free ports stay in view to line the building up with, but can't be dragged from.
		if (PixelsPerMetre >= UAtlasSubsystem::PortMinPixelsPerMetre || Atlas.IsDraggingBelt())
		{
			TopLayer = PaintPorts(Atlas, View, OutDrawElements, TopLayer, Origin, Scale, CullingRect);
		}
		if (Atlas.IsPlacing(View))
		{
			TopLayer = PaintPlace(Atlas, View, OutDrawElements, TopLayer, Origin, Scale);
		}
	}

	PaintSeconds += FPlatformTime::Seconds() - PaintStart;
	PeakInstances = FMath::Max(PeakInstances, DrawnInstances);
	if (++PaintFrames >= 300)
	{
		UE_LOG(LogFactoryAtlas, Display, TEXT("%s: %.1f us per frame, peak %lld quads drawn, now %.2f px per metre"),
			LogName, PaintSeconds / PaintFrames * 1e6, PeakInstances, PixelsPerMetre);
		PaintFrames = 0;
		PaintSeconds = 0.0;
		PeakInstances = 0;
	}

	return TopLayer;
}
