// Power wires from the map, like Factorio's wire shortcut: in wire mode, click a building, then another, and a power
// line runs between them. Clicking the ground puts a power pole there instead, as the build gun does. Each wire then
// starts the next from where it ended, so a run of clicks chains buildings together.
//
// Everything goes through the game's own wire hologram, so the length limit, the cost, the free connection check and
// the automatic pole are the build gun's. Single player and host only, like belts: the hologram is driven here, on
// the machine that has authority.

#include "AtlasSubsystem.h"

#include "AtlasBuildShared.h"
#include "AtlasTypes.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableBlueprintDesigner.h"
#include "Buildables/FGBuildableCircuitBridge.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "Buildables/FGBuildableWire.h"
#include "FGCentralStorageSubsystem.h"
#include "FGInventoryComponent.h"
#include "FGPowerConnectionComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGun.h"
#include "FactoryAtlas.h"
#include "FGBuildableSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGCircuitConnectionComponent.h"
#include "FGConstructDisqualifier.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Hologram/FGHologram.h"
#include "Hologram/FGWireHologram.h"
#include "ItemAmount.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGItemDescriptor.h"

using namespace Atlas;

namespace AtlasWireDetail
{
	constexpr int32 WireSettle = 2;          // Frames for the hologram and its pole to catch up after a change
	constexpr int32 WireHitVariants = 3;     // 0 a trace onto the building, 1 a hit made up from the actor, 2 also set directly
	constexpr uint64 WireMapGoneFrames = 30; // A view not painted for this long was closed

	/** The unlocked recipe that builds a power line. The plain power line wins over any other wire. */
	TSubclassOf<UFGRecipe> WireRecipe(UWorld* World)
	{
		AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
		if (!Recipes)
		{
			return nullptr;
		}
		TSubclassOf<UFGRecipe> Best;
		FString BestName;
		bool bBestPlain = false;
		for (const TSubclassOf<UFGRecipe>& Recipe : Recipes->GetAllAvailableRecipes())
		{
			const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
			if (Products.IsEmpty() || !Products[0].ItemClass || !Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass()))
			{
				continue;
			}
			const TSubclassOf<AFGBuildable> Buildable = UFGBuildingDescriptor::GetBuildableClass(TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass));
			if (!Buildable || !Buildable->IsChildOf(AFGBuildableWire::StaticClass()))
			{
				continue;
			}
			const FString Name = Buildable->GetName();
			const bool bPlain = Name.Contains(TEXT("PowerLine"));
			if (!Best || (bPlain && !bBestPlain) || (bPlain == bBestPlain && Name < BestName))
			{
				Best = Recipe;
				BestName = Name;
				bBestPlain = bPlain;
			}
		}
		return Best;
	}

	/** The power connection on a building nearest a point, one with a free slot first. Hidden ones, such as a track's, don't count. */
	UFGCircuitConnectionComponent* PickConnection(const AActor* Actor, const FVector& Near)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TInlineComponentArray<UFGCircuitConnectionComponent*> Connections(Actor);
		UFGCircuitConnectionComponent* Best = nullptr;
		double BestDistance = 0.0;
		bool bBestFree = false;
		for (UFGCircuitConnectionComponent* Connection : Connections)
		{
			if (!Connection || Connection->IsHidden())
			{
				continue;
			}
			const bool bFree = Connection->GetNumFreeConnections() > 0;
			const double Distance = FVector::DistSquared(Connection->GetComponentLocation(), Near);
			if (!Best || (bFree && !bBestFree) || (bFree == bBestFree && Distance < BestDistance))
			{
				Best = Connection;
				BestDistance = Distance;
				bBestFree = bFree;
			}
		}
		return Best;
	}

	/**
	 * What the build gun sees aimed at a building's power connection. Variant 0 is a real trace down onto the
	 * building, naming it past the instance manager it collides through; 1 and up a hit made up from the actor.
	 */
	FHitResult WireHit(AActor* Owner, const FVector& Location, const AFGHologram* Hologram, int32 Variant)
	{
		FHitResult Hit;
		if (!Owner)
		{
			return Hit;
		}
		UWorld* World = Owner->GetWorld();
		ECollisionChannel Channel;
		if (Variant == 0 && World && AtlasBeltDetail::FindBuildGunChannel(Channel))
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(AtlasWire), true);
			if (Hologram)
			{
				Params.AddIgnoredActor(Hologram);
				for (AFGHologram* Child : Hologram->GetHologramChildren())
				{
					Params.AddIgnoredActor(Child);
				}
			}
			const FVector Top = Location + FVector(0.0, 0.0, 300.0);
			const FVector Bottom = Location - FVector(0.0, 0.0, 300.0);
			// Whatever else is in the way, such as a roof or a walkway, is stepped past.
			for (int32 Try = 0; Try < 4; Try++)
			{
				FHitResult Trace;
				if (!World->LineTraceSingleByChannel(Trace, Top, Bottom, Channel, Params))
				{
					break;
				}
				if (AtlasBeltDetail::HitOwner(Trace) == Owner)
				{
					Trace.HitObjectHandle = FActorInstanceHandle(Owner);
					return Trace;
				}
				if (!Trace.GetComponent())
				{
					break;
				}
				Params.AddIgnoredComponent(Trace.GetComponent());
			}
		}
		UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Owner->GetRootComponent());
		if (!Primitive)
		{
			Primitive = Owner->FindComponentByClass<UPrimitiveComponent>();
		}
		Hit.bBlockingHit = true;
		Hit.HitObjectHandle = FActorInstanceHandle(Owner);
		Hit.Component = Primitive;
		Hit.Location = Location;
		Hit.ImpactPoint = Location;
		Hit.Normal = FVector::UpVector;
		Hit.ImpactNormal = FVector::UpVector;
		Hit.TraceStart = Location + FVector(0.0, 0.0, 300.0);
		Hit.TraceEnd = Location - FVector(0.0, 0.0, 300.0);
		Hit.Distance = 300.f;
		return Hit;
	}

	FVector UvToWorld(const FVector2f& Uv, double Z)
	{
		return FVector(MapWestCm + double(Uv.X) * MapWidthCm, MapNorthCm + double(Uv.Y) * MapHeightCm, Z);
	}

	bool IsSnappedTo(const AFGWireHologram* Live, int32 End, const AActor* Owner)
	{
		const UFGCircuitConnectionComponent* Connection = Live ? Live->GetConnection(End) : nullptr;
		return Connection && Owner && Connection->GetOwner() == Owner;
	}
}
using namespace AtlasWireDetail;

void UAtlasSubsystem::SetWiring(EView View, bool bOn)
{
	if (!bOn)
	{
		if (!Wire.bMode || Wire.View != View)
		{
			return;
		}
		if (WireTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(WireTicker);
			WireTicker.Reset();
		}
		DestroyWireHologram();
		Wire = FWireTool();
		return;
	}
	if (!CanBuildBelts())
	{
		ShowToast(FText::FromString(TEXT("Power lines can only be built from the map in single player or as the host.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	if (Wire.bMode)
	{
		SetWiring(Wire.View, false);
	}
	SetMeasuring(View, false);
	SetPlacing(Place.View, false);
	SetDismantling(Dismantle.View, false);
	CancelBeltDrag();
	Wire = FWireTool();
	Wire.bMode = true;
	Wire.View = View;
	if (const TSubclassOf<UFGRecipe> Recipe = WireRecipe(GetWorld()))
	{
		Wire.WireName = UFGRecipe::GetRecipeName(Recipe);
	}
	bWireHoverResolved = false;
	WireHoverActor.Reset();
	if (!WireTicker.IsValid())
	{
		WireTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) { return TickWire(); }));
	}
}

bool UAtlasSubsystem::DropWire()
{
	if (!Wire.bHasStart)
	{
		return false;
	}
	DestroyWireHologram();
	Wire.bHasStart = false;
	Wire.Start.Reset();
	Wire.StartOwner.Reset();
	Wire.bReady = false;
	Wire.bValid = false;
	Wire.Reason = FText::GetEmpty();
	return true;
}

void UAtlasSubsystem::DestroyWireHologram()
{
	if (AFGHologram* Live = WireHologram.Get())
	{
		Live->Destroy();
	}
	WireHologram.Reset();
}

void UAtlasSubsystem::SetWireStart(UFGCircuitConnectionComponent* Connection)
{
	Wire.Start = Connection;
	Wire.StartOwner = Cast<AFGBuildable>(Connection->GetOwner());
	Wire.StartLocation = Connection->GetComponentLocation();
	Wire.bHasStart = true;
	WireHitVariant = 0;
	SpawnWireHologram();
}

bool UAtlasSubsystem::SpawnWireHologram()
{
	DestroyWireHologram();
	Wire.bReady = false;
	Wire.bValid = false;
	Wire.bClipping = false;
	Wire.Reason = FText::GetEmpty();
	Wire.CostText.Reset();
	WireStage = 0;
	WireWaitFrames = WireSettle;
	WireSettleFrames = WireSettle;
	WireEndVariant = 0;

	AFGCharacterPlayer* Player = BeltPlayer();
	const TSubclassOf<UFGRecipe> Recipe = WireRecipe(GetWorld());
	if (!Player || !Player->GetBuildGun() || !Recipe)
	{
		Wire.Reason = FText::FromString(Recipe ? TEXT("Can't build right now.") : TEXT("No power line unlocked yet."));
		Wire.bReady = true;
		WireStage = -1;
		return false;
	}
	Wire.WireName = UFGRecipe::GetRecipeName(Recipe);
	AFGHologram* Live = AFGHologram::SpawnHologramFromRecipe(Recipe, Player->GetBuildGun(), Wire.StartLocation, Player);
	if (!Live || !Live->IsA<AFGWireHologram>())
	{
		if (Live)
		{
			Live->Destroy();
		}
		Wire.Reason = FText::FromString(TEXT("The game made no power line hologram."));
		Wire.bReady = true;
		WireStage = -1;
		return false;
	}
	WireHologram = Live;
	return true;
}

void UAtlasSubsystem::ResolveWireTarget()
{
	// Hover finds the record under the cursor a few times a second; the building behind it is looked up only when
	// that record changes.
	const bool bOver = Hover.bValid && !Hover.bMarker && HoverView == Wire.View;
	if (!bOver)
	{
		bWireHoverResolved = false;
		WireHoverActor.Reset();
	}
	else if (!bWireHoverResolved || Hover.Source != WireHoverSource)
	{
		WireHoverSource = Hover.Source;
		WireHoverActor = FindHoveredActor();
		bWireHoverResolved = true;
	}
	AFGBuildable* Actor = bOver ? WireHoverActor.Get() : nullptr;
	const FVector2f Uv = MapCursorUv[int32(Wire.View)];
	const FVector Cursor = UvToWorld(Uv, Actor ? Actor->GetActorLocation().Z : Wire.StartLocation.Z);
	UFGCircuitConnectionComponent* Connection = PickConnection(Actor, Cursor);
	const bool bOnStart = Wire.bHasStart && Actor && Actor == Wire.StartOwner.Get();

	const bool bHasTarget = Connection && !bOnStart;
	const bool bHasGround = !bHasTarget && !bOnStart;
	const bool bChanged = bHasTarget != Wire.bHasTarget || bHasGround != Wire.bHasGround || (bHasTarget && Actor != Wire.Target.Get());
	Wire.bHasTarget = bHasTarget;
	Wire.bHasGround = bHasGround;
	Wire.Target = bHasTarget ? Actor : nullptr;
	Wire.TargetConnection = bHasTarget ? Connection : nullptr;
	Wire.EndLocation = bHasTarget ? Connection->GetComponentLocation() : Cursor;
	if (bChanged)
	{
		WireSettleFrames = WireSettle;
		WireEndVariant = 0;
		Wire.bReady = false;
	}
}

bool UAtlasSubsystem::TickWire()
{
	if (!Wire.bMode)
	{
		WireTicker.Reset();
		return false;
	}
	// The map was closed, or building from it stopped being possible. The hologram is in the world, so nothing may
	// be left behind.
	if (GFrameCounter - MapPaintFrame[int32(Wire.View)] > WireMapGoneFrames || !CanBuildBelts())
	{
		WireTicker.Reset();
		DestroyWireHologram();
		Wire = FWireTool();
		return false;
	}

	ResolveWireTarget();
	AFGWireHologram* Live = Cast<AFGWireHologram>(WireHologram.Get());
	if (!Wire.bHasStart || WireStage < 0 || !Live)
	{
		return true;
	}
	if (--WireWaitFrames > 0)
	{
		return true;
	}
	WireWaitFrames = 1;

	switch (WireStage)
	{
	case 0:
	{
		// The start, on the building the first click picked.
		Live->ResetConstructDisqualifiers();
		Live->SetActorHiddenInGame(false);
		Live->UpdateHologramPlacement(WireHit(Wire.StartOwner.Get(), Wire.StartLocation, Live, FMath::Min(WireHitVariant, 1)));
		if (WireHitVariant >= 2)
		{
			Live->SetConnection(0, Wire.Start.Get());
		}
		WireStage = 1;
		WireWaitFrames = WireSettle;
		return true;
	}
	case 1:
		if (!IsSnappedTo(Live, 0, Wire.StartOwner.Get()))
		{
			UE_LOG(LogFactoryAtlas, Display, TEXT("Wire: aim %d didn't snap to %s (snapped to %s)"), WireHitVariant,
				*GetNameSafe(Wire.StartOwner.Get()), *GetNameSafe(Live->GetConnection(0)));
			if (++WireHitVariant < WireHitVariants)
			{
				WireStage = 0;
				return true;
			}
			Wire.Reason = FText::FromString(TEXT("Can't start a power line on that building."));
			Wire.bReady = true;
			Wire.bValid = false;
			WireStage = -1;
			return true;
		}
		// The game may have picked another of the building's connections, nearer where it was aimed.
		Wire.Start = Live->GetConnection(0);
		Wire.StartLocation = Wire.Start->GetComponentLocation();
		Live->DoMultiStepPlacement(false);
		WireStage = 2;
		WireWaitFrames = WireSettle;
		return true;
	default:
		break;
	}

	// The end follows the cursor every frame.
	if (Wire.bHasTarget)
	{
		Live->ResetConstructDisqualifiers();
		Live->SetActorHiddenInGame(false);
		Live->UpdateHologramPlacement(WireHit(Wire.Target.Get(), Wire.EndLocation, Live, FMath::Min(WireEndVariant, 1)));
		if (WireEndVariant >= 2)
		{
			Live->SetConnection(1, Wire.TargetConnection.Get());
		}
	}
	else if (Wire.bHasGround)
	{
		// Where the build gun would land looking straight down at the cursor, within the map's height filter and
		// no more than 20 m above the start, so a pole lands on the floor being worked on rather than a roof.
		UWorld* World = GetWorld();
		ECollisionChannel Channel;
		FHitResult Hit;
		bool bHit = false;
		if (World && AtlasBeltDetail::FindBuildGunChannel(Channel))
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(AtlasWireGround), true);
			Params.AddIgnoredActor(Live);
			for (AFGHologram* Child : Live->GetHologramChildren())
			{
				Params.AddIgnoredActor(Child);
			}
			const double Top = FMath::Min(double(FilterHigh) * 100.0 + 50.0, Wire.StartLocation.Z + 2000.0);
			const double Bottom = double(FilterLow) * 100.0 - 50.0;
			const FVector2f Uv = MapCursorUv[int32(Wire.View)];
			bHit = World->LineTraceSingleByChannel(Hit, UvToWorld(Uv, Top), UvToWorld(Uv, Bottom), Channel, Params) && Live->IsValidHitResult(Hit);
		}
		if (!bHit)
		{
			Wire.bReady = true;
			Wire.bValid = false;
			Wire.Reason = FText::FromString(TEXT("Nothing to build on here."));
			Wire.CostText.Reset();
			return true;
		}
		AtlasBeltDetail::ResolveForPlacement(Hit);
		Wire.EndLocation = Hit.ImpactPoint;
		Live->ResetConstructDisqualifiers();
		Live->SetActorHiddenInGame(false);
		Live->UpdateHologramPlacement(Hit);
	}
	else
	{
		// Over the building the wire starts from.
		Wire.bReady = true;
		Wire.bValid = false;
		Wire.Reason = FText::GetEmpty();
		Wire.CostText.Reset();
		return true;
	}

	if (WireSettleFrames > 0)
	{
		WireSettleFrames--;
		return true;
	}
	ValidateWire();
	// Aimed at a building but not on it: try the next way of aiming before giving the verdict.
	if (Wire.bHasTarget && !IsSnappedTo(Live, 1, Wire.Target.Get()) && WireEndVariant + 1 < WireHitVariants && !Wire.bValid)
	{
		WireEndVariant++;
		WireSettleFrames = WireSettle;
		Wire.bReady = false;
	}
	return true;
}

void UAtlasSubsystem::ValidateWire()
{
	AFGWireHologram* Live = Cast<AFGWireHologram>(WireHologram.Get());
	AFGCharacterPlayer* Player = BeltPlayer();
	if (!Live || !Player || !Player->GetBuildGun())
	{
		return;
	}
	// Validation refuses anything hidden, and children such as the automatic pole stay hidden unless shown.
	Live->ResetConstructDisqualifiers();
	for (AFGHologram* Child : Live->GetHologramChildren())
	{
		if (Child && !Child->IsDisabled())
		{
			Child->SetActorHiddenInGame(false);
			Child->ResetConstructDisqualifiers();
		}
	}
	Live->ValidatePlacementAndCost(Player->GetBuildGun()->GetInventory());

	TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
	Live->GetConstructDisqualifiers(Disqualifiers);
	for (AFGHologram* Child : Live->GetHologramChildren())
	{
		if (Child && !Child->IsDisabled())
		{
			TArray<TSubclassOf<UFGConstructDisqualifier>> ChildDisqualifiers;
			Child->GetConstructDisqualifiers(ChildDisqualifiers);
			Disqualifiers.Append(ChildDisqualifiers);
		}
	}
	// With clipping allowed, only what isn't a clearance overlap can refuse it, as for belts.
	TArray<TSubclassOf<UFGConstructDisqualifier>> Blocking;
	for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
	{
		if (!Settings.bAllowClipping || !AtlasBeltDetail::IsClippingDisqualifier(Disqualifier))
		{
			Blocking.Add(Disqualifier);
		}
	}

	const bool bSnapped = !Wire.bHasTarget || IsSnappedTo(Live, 1, Wire.Target.Get());
	Wire.bReady = true;
	Wire.bClipping = false;
	if (bSnapped && Live->CanConstruct())
	{
		Wire.bValid = true;
		Wire.Reason = FText::GetEmpty();
	}
	else if (bSnapped && Blocking.IsEmpty() && !Disqualifiers.IsEmpty())
	{
		Wire.bValid = true;
		Wire.bClipping = true;
		Wire.Reason = FText::GetEmpty();
	}
	else
	{
		Wire.bValid = false;
		Wire.Reason = !Blocking.IsEmpty() ? UFGConstructDisqualifier::GetDisqualifyingText(Blocking[0])
			: FText::FromString(bSnapped ? TEXT("Can't build this power line.") : TEXT("Can't connect to that building."));
	}

	if (const UFGCircuitConnectionComponent* End = Live->GetConnection(1))
	{
		Wire.EndLocation = End->GetComponentLocation();
	}
	Wire.LengthMetres = float(FVector::Dist(Wire.StartLocation, Wire.EndLocation) / 100.0);
	TArray<FString> Parts;
	for (const FItemAmount& Item : Live->GetCost(true))
	{
		if (Item.ItemClass && Item.Amount > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d %s"), Item.Amount, *UFGItemDescriptor::GetItemName(Item.ItemClass).ToString()));
		}
	}
	Wire.CostText = FString::Join(Parts, TEXT(", "));
}

void UAtlasSubsystem::ClickWire(EView View)
{
	if (!IsWiring(View))
	{
		return;
	}
	ResolveWireTarget();
	if (!Wire.bHasStart)
	{
		UFGCircuitConnectionComponent* Connection = Wire.bHasTarget ? Wire.TargetConnection.Get() : nullptr;
		if (!Connection)
		{
			ShowToast(FText::FromString(TEXT("Click a building with a power connection to start a wire.")), true);
			PlayClipboardSound(EClipboardSound::Error);
			return;
		}
		if (Connection->GetNumFreeConnections() <= 0)
		{
			ShowToast(FText::Format(FText::FromString(TEXT("{0} has no free power connection.")), GetHoverName()), true);
			PlayClipboardSound(EClipboardSound::Error);
			return;
		}
		SetWireStart(Connection);
		PlayClipboardSound(EClipboardSound::Copy);
		return;
	}
	if (Wire.bHasTarget || Wire.bHasGround)
	{
		BuildWire();
	}
}

void UAtlasSubsystem::BuildWire()
{
	AFGWireHologram* Live = Cast<AFGWireHologram>(WireHologram.Get());
	AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(GetWorld());
	if (!Live || !Buildables || WireStage < 2)
	{
		if (!Wire.Reason.IsEmpty())
		{
			ShowToast(Wire.Reason, true);
			PlayClipboardSound(EClipboardSound::Error);
		}
		return;
	}
	if (!Wire.bReady)
	{
		ShowToast(FText::FromString(TEXT("Still checking that wire, click again.")));
		return;
	}
	ValidateWire();
	if (!Wire.bValid)
	{
		UE_LOG(LogFactoryAtlas, Display, TEXT("Wire: click refused: %s"), *Wire.Reason.ToString());
		ShowToast(Wire.Reason, true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	if (Wire.bClipping)
	{
		// Clipping allowed: drop the clearance complaints so the game goes ahead. Nothing else was left.
		Live->ResetConstructDisqualifiers();
		for (AFGHologram* Child : Live->GetHologramChildren())
		{
			if (Child)
			{
				Child->ResetConstructDisqualifiers();
			}
		}
	}

	const bool bToGround = !Wire.bHasTarget;
	UFGCircuitConnectionComponent* EndConnection = bToGround ? nullptr : Live->GetConnection(1);
	// The click that ends the wire; one with an automatic pole may ask for one more.
	bool bDone = false;
	for (int32 Step = 0; Step < 3 && !bDone; Step++)
	{
		bDone = Live->DoMultiStepPlacement(false);
	}
	if (!bDone)
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Wire: not finished after 3 steps"));
		ShowToast(FText::FromString(TEXT("The power line wasn't finished.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		SpawnWireHologram();
		return;
	}

	const TArray<FItemAmount> Cost = Live->GetCost(true);
	TArray<AActor*> Children;
	AActor* Built = Live->Construct(Children, Buildables->GetNewNetConstructionID());
	if (!Built)
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Wire: Construct returned nothing"));
		ShowToast(FText::FromString(TEXT("The game built nothing.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		SpawnWireHologram();
		return;
	}
	if (!PayBuildCost(Cost))
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Wire: built %s but couldn't take its whole cost"), *Built->GetName());
	}

	// The next wire starts where this one ended: on the building, or on the pole just placed.
	UFGCircuitConnectionComponent* Next = EndConnection;
	if (bToGround)
	{
		for (AActor* Child : Children)
		{
			if (Child && Child != Built)
			{
				if (UFGCircuitConnectionComponent* Found = PickConnection(Child, Child->GetActorLocation()))
				{
					Next = Found;
					break;
				}
			}
		}
	}
	UE_LOG(LogFactoryAtlas, Display, TEXT("Wire: built %s from %s to %s (%.0f m)%s"), *Built->GetName(), *GetNameSafe(Wire.StartOwner.Get()),
		bToGround ? TEXT("a new pole") : *GetNameSafe(Wire.Target.Get()), Wire.LengthMetres, Wire.bClipping ? TEXT(", clipping") : TEXT(""));
	PlayClipboardSound(EClipboardSound::Paste);

	const FText Name = Wire.WireName.IsEmpty() ? FText::FromString(TEXT("Power Line")) : Wire.WireName;
	if (IsValid(Next) && Next->GetNumFreeConnections() > 0)
	{
		ShowToast(FText::Format(FText::FromString(bToGround ? TEXT("Built {0} and a power pole.") : TEXT("Built {0}.")), Name));
		SetWireStart(Next);
	}
	else
	{
		// Nowhere left to go on from: the wire ends here and the next click starts a new one.
		ShowToast(FText::Format(FText::FromString(TEXT("Built {0}. That end has no free connection, so the chain stops.")), Name));
		DropWire();
	}
}

// Connect mode (Alt+C): pick buildings as dismantle mode does, and Atlas plans the power lines that put them all on
// one circuit. The plan is a minimum spanning tree over the picked buildings, which keeps the wiring short and its
// wires from crossing, with a few rules on top so it reads like hand wiring and never strands anything:
// - Buildings already on the same circuit are one group, so no wire is built that would change nothing.
// - Each building only looks at its nearest neighbours in reach, and a wire scores a little longer the more diagonal
//   it runs and a little shorter when it reaches a power pole, so rows chain straight and poles become hubs.
// - A connection's free slots are respected, and a join that would use up a group's last free slot while other
//   groups still wait is put off until nothing else is left.
// The wires are then spawned directly between the two connections, as SmartMCP does, and paid like the build gun's.
namespace AtlasConnectDetail
{
	constexpr int32 Neighbours = 10;          // Candidate wires kept per building, nearest first
	constexpr double PoleBonus = 0.85;        // A wire to a power pole scores this much of its length
	constexpr double DiagonalPenalty = 0.1;   // A 45 degree wire scores this much longer than a straight one
	constexpr double ReachSlackCm = 2000.0;   // Buildings are first compared by their middles, ports can be this far off

	struct FWirePort
	{
		UFGPowerConnectionComponent* Component = nullptr;
		FVector Location = FVector::ZeroVector;
		EPowerConnectionType Type = EPowerConnectionType::PCT_Default;
		int32 Free = 0;
		float Bias = 0.f;
	};

	struct FWireNode
	{
		int32 Target = INDEX_NONE;
		AFGBuildable* Actor = nullptr;
		AFGBuildableBlueprintDesigner* Designer = nullptr;
		FVector Location = FVector::ZeroVector;
		TArray<FWirePort> Ports;
		bool bPole = false;
		bool bTower = false;
	};

	struct FWireEdge
	{
		int32 A = 0;
		int32 B = 0;
		double Score = 0.0;
	};

	/** Groups that are already, or will be, on one circuit, with how many free connection slots each has left. */
	struct FWireGroups
	{
		TArray<int32> Parent;
		TArray<int32> Free;
		TArray<int32> Size;
		int32 Count = 0;

		explicit FWireGroups(const TArray<FWireNode>& Nodes)
		{
			Count = Nodes.Num();
			for (int32 Index = 0; Index < Nodes.Num(); Index++)
			{
				Parent.Add(Index);
				Size.Add(1);
				int32 Slots = 0;
				for (const FWirePort& Port : Nodes[Index].Ports)
				{
					Slots += FMath::Max(0, Port.Free);
				}
				Free.Add(Slots);
			}
		}

		int32 Find(int32 Index)
		{
			while (Parent[Index] != Index)
			{
				Parent[Index] = Parent[Parent[Index]];
				Index = Parent[Index];
			}
			return Index;
		}

		/** Joins two groups; UsedSlots is what the joining wire takes from them. */
		void Join(int32 A, int32 B, int32 UsedSlots)
		{
			A = Find(A);
			B = Find(B);
			if (A == B)
			{
				return;
			}
			if (Size[A] < Size[B])
			{
				Swap(A, B);
			}
			Parent[B] = A;
			Size[A] += Size[B];
			Free[A] += Free[B] - UsedSlots;
			Count--;
		}
	};

	bool Compatible(EPowerConnectionType A, EPowerConnectionType B)
	{
		return A == B || A == EPowerConnectionType::PCT_Any || B == EPowerConnectionType::PCT_Any;
	}

	/** The free, compatible pair of connections that a wire would join, nearest first, within the game's reach. */
	bool BestPorts(const FWireNode& A, const FWireNode& B, const AFGBuildableWire& Defaults, int32& OutA, int32& OutB, double& OutLength)
	{
		double Best = TNumericLimits<double>::Max();
		OutA = OutB = INDEX_NONE;
		for (int32 IndexA = 0; IndexA < A.Ports.Num(); IndexA++)
		{
			const FWirePort& PortA = A.Ports[IndexA];
			if (PortA.Free <= 0)
			{
				continue;
			}
			for (int32 IndexB = 0; IndexB < B.Ports.Num(); IndexB++)
			{
				const FWirePort& PortB = B.Ports[IndexB];
				if (PortB.Free <= 0 || !Compatible(PortA.Type, PortB.Type))
				{
					continue;
				}
				const bool bTower = PortA.Type == EPowerConnectionType::PCT_PowerTower || PortB.Type == EPowerConnectionType::PCT_PowerTower;
				const double Length = FVector::Dist(PortA.Location, PortB.Location);
				if (Length > (bTower ? Defaults.mMaxPowerTowerLength : Defaults.mMaxLength))
				{
					continue;
				}
				const double Score = Length + PortA.Bias + PortB.Bias;
				if (Score < Best)
				{
					Best = Score;
					OutA = IndexA;
					OutB = IndexB;
					OutLength = Length;
				}
			}
		}
		return OutA != INDEX_NONE;
	}

	/** The wire class an unlocked power line recipe builds, for its reach and cost per length. */
	const AFGBuildableWire* WireDefaults(TSubclassOf<UFGRecipe> Recipe, TSubclassOf<AFGBuildableWire>& OutClass)
	{
		OutClass = nullptr;
		const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>();
		if (Products.IsEmpty() || !Products[0].ItemClass || !Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass()))
		{
			return nullptr;
		}
		const TSubclassOf<AFGBuildable> Buildable = UFGBuildingDescriptor::GetBuildableClass(TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass));
		if (!Buildable || !Buildable->IsChildOf(AFGBuildableWire::StaticClass()))
		{
			return nullptr;
		}
		OutClass = TSubclassOf<AFGBuildableWire>(Buildable.Get());
		return GetDefault<AFGBuildableWire>(Buildable.Get());
	}

	/** "84 Cable, 12 Quickwire", largest first. */
	FString DescribeCost(const TMap<UClass*, int32>& Totals)
	{
		TArray<TPair<UClass*, int32>> Sorted;
		for (const TPair<UClass*, int32>& Pair : Totals)
		{
			Sorted.Add(Pair);
		}
		Sorted.Sort([](const TPair<UClass*, int32>& A, const TPair<UClass*, int32>& B) { return A.Value > B.Value; });
		TArray<FString> Parts;
		for (const TPair<UClass*, int32>& Pair : Sorted)
		{
			Parts.Add(FString::Printf(TEXT("%d %s"), Pair.Value, *UFGItemDescriptor::GetItemName(TSubclassOf<UFGItemDescriptor>(Pair.Key)).ToString()));
		}
		return FString::Join(Parts, TEXT(", "));
	}
}

bool UAtlasSubsystem::IsWirable(const AFGBuildable* Building)
{
	if (!IsValid(Building) || Building->IsA<AFGBuildableWire>() || Building->IsA<AFGBuildableCircuitBridge>())
	{
		return false;
	}
	TInlineComponentArray<UFGPowerConnectionComponent*> Connections(Building);
	for (const UFGPowerConnectionComponent* Connection : Connections)
	{
		if (Connection && !Connection->IsHidden())
		{
			return true;
		}
	}
	return false;
}

void UAtlasSubsystem::PlanPower()
{
	using namespace AtlasConnectDetail;
	FPowerPlan& Plan = Dismantle.Power;
	Plan = FPowerPlan();
	const TArray<FDismantleTarget>& Targets = Dismantle.Targets;
	if (Dismantle.Purpose != EPickPurpose::Power || Targets.IsEmpty())
	{
		return;
	}
	Plan.Stranded.Init(false, Targets.Num());

	UWorld* World = GetWorld();
	TSubclassOf<AFGBuildableWire> WireClass;
	const TSubclassOf<UFGRecipe> Recipe = WireRecipe(World);
	const AFGBuildableWire* Defaults = WireDefaults(Recipe, WireClass);
	if (!Defaults)
	{
		Plan.ShortText = TEXT("No power line unlocked yet.");
		return;
	}

	// Every picked building with a connection a wire can reach.
	TArray<FWireNode> Nodes;
	for (int32 Index = 0; Index < Targets.Num(); Index++)
	{
		AFGBuildable* Building = Targets[Index].Actor.Get();
		if (!IsWirable(Building))
		{
			continue;
		}
		FWireNode Node;
		Node.Target = Index;
		Node.Actor = Building;
		Node.Designer = Building->GetBlueprintDesigner();
		Node.Location = Building->GetActorLocation();
		Node.bPole = Building->IsA<AFGBuildablePowerPole>();
		TInlineComponentArray<UFGPowerConnectionComponent*> Connections(Building);
		for (UFGPowerConnectionComponent* Connection : Connections)
		{
			if (Connection && !Connection->IsHidden())
			{
				FWirePort Port;
				Port.Component = Connection;
				Port.Location = Connection->GetComponentLocation();
				Port.Type = Connection->GetPowerConnectionType();
				Port.Free = Connection->GetNumFreeConnections();
				Port.Bias = Connection->GetConnectionDistanceBias();
				Node.bTower |= Port.Type == EPowerConnectionType::PCT_PowerTower;
				Node.Ports.Add(Port);
			}
		}
		Nodes.Add(MoveTemp(Node));
	}
	Plan.Buildings = Nodes.Num();
	if (Nodes.IsEmpty())
	{
		return;
	}

	// Buildings already on one circuit are one group.
	FWireGroups Circuits(Nodes);
	TMap<int32, int32> FirstOnCircuit;
	for (int32 Index = 0; Index < Nodes.Num(); Index++)
	{
		for (const FWirePort& Port : Nodes[Index].Ports)
		{
			const int32 Circuit = Port.Component->GetCircuitID();
			if (Circuit == INDEX_NONE)
			{
				continue;
			}
			if (const int32* First = FirstOnCircuit.Find(Circuit))
			{
				Circuits.Join(*First, Index, 0);
			}
			else
			{
				FirstOnCircuit.Add(Circuit, Index);
			}
		}
	}
	Plan.GroupsBefore = Circuits.Count;

	// Candidate wires: each building to its nearest neighbours in other groups, as far as a wire reaches.
	TArray<FWireEdge> Edges;
	TSet<uint64> Seen;
	TArray<TPair<double, int32>> Near;
	for (int32 A = 0; A < Nodes.Num() && Circuits.Count > 1; A++)
	{
		Near.Reset();
		const int32 GroupA = Circuits.Find(A);
		for (int32 B = 0; B < Nodes.Num(); B++)
		{
			if (B == A || Circuits.Find(B) == GroupA || Nodes[B].Designer != Nodes[A].Designer)
			{
				continue;
			}
			const double Reach = (Nodes[A].bTower && Nodes[B].bTower ? Defaults->mMaxPowerTowerLength : Defaults->mMaxLength) + ReachSlackCm;
			const double DistanceSquared = FVector::DistSquared(Nodes[A].Location, Nodes[B].Location);
			if (DistanceSquared <= Reach * Reach)
			{
				Near.Add({ DistanceSquared, B });
			}
		}
		Near.Sort([](const TPair<double, int32>& X, const TPair<double, int32>& Y) { return X.Key < Y.Key; });
		for (int32 Index = 0; Index < Near.Num() && Index < Neighbours; Index++)
		{
			const int32 B = Near[Index].Value;
			const uint64 Key = (uint64(uint32(FMath::Min(A, B))) << 32) | uint32(FMath::Max(A, B));
			if (Seen.Contains(Key))
			{
				continue;
			}
			Seen.Add(Key);
			int32 PortA, PortB;
			double Length = 0.0;
			if (!BestPorts(Nodes[A], Nodes[B], *Defaults, PortA, PortB, Length))
			{
				continue;
			}
			const FVector Delta = Nodes[B].Ports[PortB].Location - Nodes[A].Ports[PortA].Location;
			const double Long = FMath::Max(FMath::Abs(Delta.X), FMath::Abs(Delta.Y));
			const double Diagonal = Long > 1.0 ? FMath::Min(FMath::Abs(Delta.X), FMath::Abs(Delta.Y)) / Long : 0.0;
			double Score = Length * (1.0 + DiagonalPenalty * Diagonal);
			if (Nodes[A].bPole || Nodes[B].bPole)
			{
				Score *= PoleBonus;
			}
			Edges.Add({ A, B, Score });
		}
	}
	Edges.Sort([](const FWireEdge& X, const FWireEdge& Y) { return X.Score < Y.Score; });

	// Shortest first, as long as both ends still have a free slot. The first pass won't leave a group without a free
	// slot while others still wait to join it; the second takes whatever is left.
	TArray<bool> Taken;
	Taken.Init(false, Edges.Num());
	for (int32 Pass = 0; Pass < 2 && Circuits.Count > 1; Pass++)
	{
		for (int32 Index = 0; Index < Edges.Num() && Circuits.Count > 1; Index++)
		{
			const FWireEdge& Edge = Edges[Index];
			const int32 GroupA = Circuits.Find(Edge.A);
			const int32 GroupB = Circuits.Find(Edge.B);
			if (Taken[Index] || GroupA == GroupB)
			{
				continue;
			}
			int32 PortA, PortB;
			double Length = 0.0;
			if (!BestPorts(Nodes[Edge.A], Nodes[Edge.B], *Defaults, PortA, PortB, Length))
			{
				continue;
			}
			if (Pass == 0 && Circuits.Count > 2 && Circuits.Free[GroupA] + Circuits.Free[GroupB] - 2 <= 0)
			{
				continue;
			}
			Taken[Index] = true;
			FWirePort& From = Nodes[Edge.A].Ports[PortA];
			FWirePort& To = Nodes[Edge.B].Ports[PortB];
			From.Free--;
			To.Free--;
			Circuits.Join(GroupA, GroupB, 2);

			FPlannedWire Line;
			Line.From = From.Component;
			Line.To = To.Component;
			Line.FromLocation = From.Location;
			Line.ToLocation = To.Location;
			Line.Metres = float(Length / 100.0);
			const int32 Multiplier = AFGBuildable::GetCostMultiplierForLength(float(Length), Defaults->mLengthPerCost);
			for (const FItemAmount& Item : UFGRecipe::GetIngredients(World, Recipe))
			{
				if (Item.ItemClass && Item.Amount > 0)
				{
					Line.Cost.Add(FItemAmount(Item.ItemClass, Item.Amount * Multiplier));
				}
			}
			Plan.Metres += Line.Metres;
			Plan.Wires.Add(MoveTemp(Line));
		}
	}
	Plan.GroupsAfter = Circuits.Count;

	// Whatever isn't in the biggest group once built is out of reach, and drawn red.
	if (Circuits.Count > 1)
	{
		int32 Biggest = Circuits.Find(0);
		for (int32 Index = 0; Index < Nodes.Num(); Index++)
		{
			const int32 Group = Circuits.Find(Index);
			if (Circuits.Size[Group] > Circuits.Size[Biggest])
			{
				Biggest = Group;
			}
		}
		for (int32 Index = 0; Index < Nodes.Num(); Index++)
		{
			Plan.Stranded[Nodes[Index].Target] = Circuits.Find(Index) != Biggest;
		}
	}

	// What it costs, and how far the inventory and the Dimensional Depot go, wire by wire in build order.
	AFGCharacterPlayer* Player = BeltPlayer();
	UFGInventoryComponent* Inventory = Player && Player->GetBuildGun() ? Player->GetBuildGun()->GetInventory() : nullptr;
	const AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
	const bool bFree = !Inventory || Inventory->GetNoBuildCost();
	TMap<UClass*, int32> Totals;
	bool bShort = false;
	for (const FPlannedWire& Planned : Plan.Wires)
	{
		for (const FItemAmount& Item : Planned.Cost)
		{
			int32& Total = Totals.FindOrAdd(Item.ItemClass);
			Total += Item.Amount;
			if (!bFree && !bShort)
			{
				const int32 Have = Inventory->GetNumItems(Item.ItemClass) + (Depot ? Depot->GetNumItemsFromCentralStorage(Item.ItemClass) : 0);
				if (Total > Have)
				{
					bShort = true;
					Plan.ShortText = FString::Printf(TEXT("Not enough %s: builds the first %d"), *UFGItemDescriptor::GetItemName(Item.ItemClass).ToString(), Plan.Affordable);
				}
			}
		}
		if (!bShort)
		{
			Plan.Affordable++;
		}
	}
	if (!bFree)
	{
		Plan.CostText = DescribeCost(Totals);
	}
}

void UAtlasSubsystem::ConnectPicked()
{
	using namespace AtlasConnectDetail;
	if (!Dismantle.bMode || Dismantle.Purpose != EPickPurpose::Power)
	{
		return;
	}
	const FPowerPlan& Plan = Dismantle.Power;
	if (Plan.Wires.IsEmpty())
	{
		const TCHAR* Why = Plan.Buildings == 0 ? TEXT("Nothing picked. Click buildings or drag a box over them first.")
			: Plan.GroupsBefore <= 1 ? TEXT("Everything picked is already on one power circuit.")
			: !Plan.ShortText.IsEmpty() ? *Plan.ShortText
			: TEXT("Nothing here can be joined: too far apart, or no free connections. A power pole between them helps.");
		ShowToast(FText::FromString(Why), Plan.GroupsBefore > 1 || Plan.Buildings == 0);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	UWorld* World = GetWorld();
	AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(World);
	AFGCharacterPlayer* Player = BeltPlayer();
	UFGInventoryComponent* Inventory = Player && Player->GetBuildGun() ? Player->GetBuildGun()->GetInventory() : nullptr;
	TSubclassOf<AFGBuildableWire> WireClass;
	const TSubclassOf<UFGRecipe> Recipe = WireRecipe(World);
	if (!CanBuildBelts() || !Buildables || !Inventory || !WireDefaults(Recipe, WireClass) || !WireClass)
	{
		ShowToast(FText::FromString(TEXT("Power lines can only be built from the map in single player or as the host.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	const AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(World);
	const bool bFree = Inventory->GetNoBuildCost();
	const auto CanPay = [Inventory, Depot](const TArray<FItemAmount>& Cost)
	{
		for (const FItemAmount& Item : Cost)
		{
			if (Inventory->GetNumItems(Item.ItemClass) + (Depot ? Depot->GetNumItemsFromCentralStorage(Item.ItemClass) : 0) < Item.Amount)
			{
				return false;
			}
		}
		return true;
	};

	int32 BuiltCount = 0;
	int32 Skipped = 0;
	float Metres = 0.f;
	bool bShort = false;
	for (const FPlannedWire& Planned : Plan.Wires)
	{
		UFGCircuitConnectionComponent* From = Planned.From.Get();
		UFGCircuitConnectionComponent* To = Planned.To.Get();
		if (!IsValid(From) || !IsValid(To) || From->GetNumFreeConnections() <= 0 || To->GetNumFreeConnections() <= 0)
		{
			Skipped++;
			continue;
		}
		if (!bFree && !CanPay(Planned.Cost))
		{
			bShort = true;
			break;
		}
		// Spawned, then connected: the order the game's own directly spawned wires use.
		AFGBuildable* Owner = Cast<AFGBuildable>(From->GetOwner());
		AFGBuildableBlueprintDesigner* Designer = Owner ? Owner->GetBlueprintDesigner() : nullptr;
		const FTransform Transform(FRotator::ZeroRotator, (From->GetComponentLocation() + To->GetComponentLocation()) * 0.5);
		AFGBuildable* Spawned = Buildables->BeginSpawnBuildable(WireClass, Transform);
		AFGBuildableWire* Built = Cast<AFGBuildableWire>(Spawned);
		if (!Built)
		{
			if (Spawned)
			{
				Spawned->Destroy();
			}
			Skipped++;
			continue;
		}
		Built->SetBuiltWithRecipe(Recipe); // So it refunds like a hand-built wire
		if (Designer)
		{
			Built->SetInsideBlueprintDesigner(Designer); // Must be set before BeginPlay
		}
		Built->FinishSpawning(Transform);
		if (Designer)
		{
			Designer->OnBuildableConstructedInsideDesigner(Built);
		}
		if (!Built->Connect(From, To))
		{
			Built->Destroy();
			Skipped++;
			continue;
		}
		if (!PayBuildCost(Planned.Cost))
		{
			UE_LOG(LogFactoryAtlas, Warning, TEXT("Connect: built %s but couldn't take its whole cost"), *Built->GetName());
		}
		for (UFGCircuitConnectionComponent* End : { From, To })
		{
			if (AFGBuildablePowerPole* Pole = Cast<AFGBuildablePowerPole>(End->GetOwner()))
			{
				Pole->MarkConnectionsDirty();
			}
		}
		BuiltCount++;
		Metres += Planned.Metres;
	}

	UE_LOG(LogFactoryAtlas, Display, TEXT("Connect: %d power lines built (%.0f m), %d skipped, %d of %d groups left%s"),
		BuiltCount, Metres, Skipped, Plan.GroupsAfter, Plan.GroupsBefore, bShort ? TEXT(", ran out of materials") : TEXT(""));
	FString Text = FString::Printf(TEXT("Built %d power lines, %.0f m."), BuiltCount, Metres);
	if (bShort)
	{
		Text += FString::Printf(TEXT(" Ran out of materials with %d to go."), Plan.Wires.Num() - BuiltCount - Skipped);
	}
	if (Skipped > 0)
	{
		Text += FString::Printf(TEXT(" %d couldn't be built."), Skipped);
	}
	if (Plan.GroupsAfter > 1)
	{
		Text += FString::Printf(TEXT(" %d groups were out of reach; a power pole between them helps."), Plan.GroupsAfter - 1);
	}
	ShowToast(FText::FromString(Text), BuiltCount == 0);
	PlayClipboardSound(BuiltCount == 0 ? EClipboardSound::Error : EClipboardSound::Paste);

	// The circuits catch up with the new wires on their own time, so the pick is done rather than planned again.
	Dismantle.Targets.Reset();
	Dismantle.bCapped = false;
	RefreshDismantle();
	bPortsDirty = true;
}
