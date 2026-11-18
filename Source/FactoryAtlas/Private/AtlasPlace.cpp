// Placing buildings from the map: a building in hand follows the cursor as a ghost, landing on the floor the height
// filter shows, and a click builds it. Pick one from the list, or middle click a machine on the map to take it in hand
// with its settings, so every copy comes out with the same recipe, clock speed and paint.
//
// Everything goes through the game's own hologram, as belts and wires from the map do: snapping to the foundation
// grid, rotation, clearance, the verdict and the cost are the build gun's. Single player and host only, like them.

#include "AtlasSubsystem.h"

#include "AtlasBuildShared.h"
#include "AtlasTypes.h"
#include "Algo/Sort.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "FGFactoryConnectionComponent.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Buildables/FGBuildableFactoryBuilding.h"
#include "Buildables/FGBuildableFoundation.h"
#include "Buildables/FGBuildableGenerator.h"
#include "Buildables/FGBuildableLadder.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePipelineAttachment.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildableStair.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableTrainPlatform.h"
#include "Buildables/FGBuildableWalkway.h"
#include "Buildables/FGBuildableWire.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "FGConnectionComponent.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "FactoryAtlas.h"
#include "FGBlueprintSettings.h"
#include "FGFactoryBlueprintTypes.h"
#include "Hologram/FGBlueprintHologram.h"
#include "FGBuildableBeam.h"
#include "FGBuildablePillar.h"
#include "FGBuildableSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGBlueprintProxy.h"
#include "FGConstructDisqualifier.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "FGSplineBuildableInterface.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGHologram.h"
#include "ItemAmount.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGItemDescriptor.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"

using namespace Atlas;

namespace AtlasPlaceDetail
{
	constexpr int32 PlaceSettle = 2;          // Frames for the hologram to catch up after it moves
	constexpr int32 PasteDelay = 2;           // Frames between building a sampled copy and pasting its settings
	constexpr uint64 PlaceMapGoneFrames = 30; // A view not painted for this long was closed
	constexpr int32 QuarterTurn = 90;
	constexpr int32 DefaultFineStep = 10;
	constexpr int32 MaxRowSpots = 40;         // The most buildings one drag lays out
	constexpr double ManifoldReachCm = 3000.0; // How far from the row a machine port may be to line up with
	constexpr double ManifoldHeightCm = 600.0; // and how far above or below it

	FVector PlaceUvToWorld(const FVector2f& Uv, double Z)
	{
		return FVector(MapWestCm + double(Uv.X) * MapWidthCm, MapNorthCm + double(Uv.Y) * MapHeightCm, Z);
	}

	/**
	 * Foundations, and buildings that stand on a floor: machines, generators, storage, splitters, poles and the like.
	 * Walls, stairs and other structure are out, and so is anything drawn as a line, which has its own map tool.
	 */
	bool IsPlaceableClass(UClass* Class)
	{
		if (!Class || !Class->IsChildOf<AFGBuildable>())
		{
			return false;
		}
		if (Class->IsChildOf<AFGBuildableFoundation>())
		{
			return true;
		}
		if (GetDefault<AFGBuildable>(Class)->ManagedByLightweightBuildableSubsystem())
		{
			return false;
		}
		if (Class->IsChildOf<AFGBuildableFactoryBuilding>() || Class->IsChildOf<AFGBuildablePillar>() || Class->IsChildOf<AFGBuildableStair>()
			|| Class->IsChildOf<AFGBuildableWalkway>() || Class->IsChildOf<AFGBuildableLadder>() || Class->IsChildOf<AFGBuildableBeam>()
			|| Class->IsChildOf<AFGBuildableWire>() || Class->IsChildOf<AFGBuildableConveyorLift>()
			|| Class->IsChildOf<AFGVehiclePathNode>() || Class->IsChildOf<AFGVehiclePathSegment>()
			|| Class->ImplementsInterface(UFGSplineBuildableInterface::StaticClass()))
		{
			return false;
		}
		const FString Name = Class->GetName();
		static const TCHAR* const Structural[] = { TEXT("Roof"), TEXT("Fence"), TEXT("Railing"), TEXT("Barrier"), TEXT("Road"), TEXT("VehiclePath"), TEXT("FlowIndicator") };
		for (const TCHAR* Word : Structural)
		{
			if (Name.Contains(Word))
			{
				return false;
			}
		}
		return true;
	}

	/** The picker's groups, in the order it shows them. */
	const TCHAR* const Groups[] = { TEXT("Production"), TEXT("Extraction"), TEXT("Power"), TEXT("Logistics"), TEXT("Storage"), TEXT("Transport"), TEXT("Foundations"), TEXT("Other") };

	int32 GroupOf(UClass* Class)
	{
		if (Class->IsChildOf<AFGBuildableManufacturer>()) { return 0; }
		if (Class->IsChildOf<AFGBuildableResourceExtractorBase>()) { return 1; }
		if (Class->IsChildOf<AFGBuildableGenerator>() || Class->IsChildOf<AFGBuildablePowerPole>() || Class->GetName().Contains(TEXT("Power"))) { return 2; }
		if (Class->IsChildOf<AFGBuildableConveyorAttachment>() || Class->IsChildOf<AFGBuildablePipelineAttachment>()) { return 3; }
		if (Class->IsChildOf<AFGBuildableStorage>()) { return 4; }
		if (Class->IsChildOf<AFGBuildableTrainPlatform>() || Class->IsChildOf<AFGBuildableDockingStation>()) { return 5; }
		if (Class->IsChildOf<AFGBuildableFoundation>()) { return 6; }
		return 7;
	}

	/** The building a recipe makes, or none when it makes something else. */
	TSubclassOf<AFGBuildable> BuildableOf(UClass* Recipe, TSubclassOf<UFGItemDescriptor>* OutDescriptor = nullptr)
	{
		if (!Recipe || !Recipe->IsChildOf<UFGRecipe>())
		{
			return nullptr;
		}
		const TArray<FItemAmount> Products = UFGRecipe::GetProducts(TSubclassOf<UFGRecipe>(Recipe));
		if (Products.IsEmpty() || !Products[0].ItemClass || !Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass()))
		{
			return nullptr;
		}
		if (OutDescriptor)
		{
			*OutDescriptor = Products[0].ItemClass;
		}
		return UFGBuildingDescriptor::GetBuildableClass(TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass));
	}

	/** Adds Template as a port, placed by Transform relative to the building, when it is one. */
	void AddTemplatePort(const UActorComponent* Template, const FTransform& Transform, TArray<FPort>& Out)
	{
		UFGConnectionComponent* Connection = const_cast<UFGConnectionComponent*>(Cast<UFGConnectionComponent>(Template));
		FPort Port;
		if (!Connection || !AtlasBeltDetail::DescribePort(Connection, Port) || Port.Kind == EPortKind::Rail)
		{
			return;
		}
		// A template sits nowhere in the world, so where it is comes from its place in the building.
		Port.Connection.Reset();
		Port.Location = Transform.GetLocation();
		Port.Normal = Transform.GetRotation().GetForwardVector();
		Out.Add(Port);
	}

	/**
	 * A building's belt, pipe and hypertube ports, relative to the building, read from its class before any copy of it
	 * exists: the blueprint's component tree, with child blueprints' overrides, and components made in C++.
	 */
	void GatherTemplatePorts(UClass* Class, TArray<FPort>& Out)
	{
		Out.Reset();
		TArray<const UBlueprintGeneratedClass*> Blueprints; // Most derived first
		for (UClass* Current = Class; Current; Current = Current->GetSuperClass())
		{
			if (const UBlueprintGeneratedClass* Blueprint = Cast<UBlueprintGeneratedClass>(Current))
			{
				Blueprints.Add(Blueprint);
			}
		}
		for (int32 Level = 0; Level < Blueprints.Num(); Level++)
		{
			const USimpleConstructionScript* Script = Blueprints[Level]->SimpleConstructionScript;
			if (!Script)
			{
				continue;
			}
			// A child blueprint may move a component it inherits; the most derived override wins.
			const auto TemplateOf = [&](const USCS_Node* Node) -> const UActorComponent*
			{
				for (int32 Child = 0; Child < Level; Child++)
				{
					if (const UInheritableComponentHandler* Handler = Blueprints[Child]->InheritableComponentHandler)
					{
						if (const UActorComponent* Overridden = Handler->GetOverridenComponentTemplate(FComponentKey(Node)))
						{
							return Overridden;
						}
					}
				}
				return Node->ComponentTemplate;
			};
			TFunction<void(const USCS_Node*, const FTransform&)> Walk = [&](const USCS_Node* Node, const FTransform& Parent)
			{
				const UActorComponent* Template = TemplateOf(Node);
				FTransform Transform = Parent;
				if (const USceneComponent* Scene = Cast<USceneComponent>(Template))
				{
					Transform = Scene->GetRelativeTransform() * Parent;
				}
				AddTemplatePort(Template, Transform, Out);
				for (const USCS_Node* Child : Node->GetChildNodes())
				{
					if (Child)
					{
						Walk(Child, Transform);
					}
				}
			};
			for (const USCS_Node* Root : Script->GetRootNodes())
			{
				if (Root)
				{
					Walk(Root, FTransform::Identity);
				}
			}
		}
		// Components made in C++ live on the class default object, attached as they will be.
		TArray<UActorComponent*> Natives;
		GetDefault<AActor>(Class)->GetComponents(Natives);
		for (const UActorComponent* Native : Natives)
		{
			const USceneComponent* Scene = Cast<USceneComponent>(Native);
			FTransform Transform = FTransform::Identity;
			for (const USceneComponent* Link = Scene; Link; Link = Link->GetAttachParent())
			{
				Transform = Transform * Link->GetRelativeTransform();
			}
			AddTemplatePort(Native, Transform, Out);
		}
	}

	/** The unlocked recipe that builds this class: the one it was built with when that is still available. */
	UClass* RecipeFor(UWorld* World, UClass* Class, TSubclassOf<UFGRecipe> BuiltWith)
	{
		AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
		if (!Recipes || !Class)
		{
			return nullptr;
		}
		const TArray<TSubclassOf<UFGRecipe>> Available = Recipes->GetAllAvailableRecipes();
		if (BuiltWith && Available.Contains(BuiltWith) && BuildableOf(BuiltWith) == Class)
		{
			return BuiltWith;
		}
		for (const TSubclassOf<UFGRecipe>& Recipe : Available)
		{
			if (BuildableOf(Recipe) == Class)
			{
				return Recipe;
			}
		}
		return nullptr;
	}

	/** Live instances of a lightweight class, so a build that hands back no actor can still be seen to have worked. */
	int32 CountLightweights(UWorld* World, UClass* Class)
	{
		AFGLightweightBuildableSubsystem* Lightweights = World ? AFGLightweightBuildableSubsystem::Get(World) : nullptr;
		const TArray<FRuntimeBuildableInstanceData>* Instances = Lightweights ? Lightweights->GetAllLightweightBuildableInstances().Find(Class) : nullptr;
		int32 Count = 0;
		for (int32 Index = 0; Instances && Index < Instances->Num(); Index++)
		{
			Count += (*Instances)[Index].IsValid() ? 1 : 0;
		}
		return Count;
	}
}
using namespace AtlasPlaceDetail;

void UAtlasSubsystem::SetPlacing(EView View, bool bOn)
{
	if (!bOn)
	{
		if (!Place.bMode || Place.View != View)
		{
			return;
		}
		if (PlaceTicker.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(PlaceTicker);
			PlaceTicker.Reset();
		}
		DestroyPlaceHologram();
		if (Place.bManifoldBelting)
		{
			CancelBeltDrag();
		}
		ManifoldJobs.Reset();
		Place = FPlaceTool();
		bRowPressed = false;
		RowCursor = INDEX_NONE;
		// Copies built a moment ago still get their settings, unless the world is going away.
		if (GetWorld() && !GetWorld()->bIsTearingDown)
		{
			FlushPlacePaste(true);
		}
		PlacePastes.Reset();
		return;
	}
	if (!CanBuildBelts())
	{
		ShowToast(FText::FromString(TEXT("Buildings can only be placed from the map in single player or as the host.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	if (Place.bMode)
	{
		SetPlacing(Place.View, false);
	}
	SetWiring(Wire.View, false);
	SetMeasuring(View, false);
	SetDismantling(Dismantle.View, false);
	CancelBeltDrag();
	Place = FPlaceTool();
	Place.bMode = true;
	Place.View = View;
	bPlaceAimDirty = true;
	if (!PlaceTicker.IsValid())
	{
		PlaceTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) { return TickPlace(); }));
	}
}

void UAtlasSubsystem::SetPlaceRecipe(EView View, UClass* Recipe)
{
	TakeInHand(View, Recipe, false);
}

void UAtlasSubsystem::TakeInHand(EView View, UClass* Recipe, bool bSampled)
{
	TSubclassOf<UFGItemDescriptor> Descriptor;
	const TSubclassOf<AFGBuildable> Buildable = BuildableOf(Recipe, &Descriptor);
	if (!Buildable || !IsPlaceableClass(Buildable))
	{
		return;
	}
	if (!IsPlacing(View))
	{
		SetPlacing(View, true);
		if (!IsPlacing(View))
		{
			return;
		}
	}
	if (Place.bRowBuilding)
	{
		return;
	}
	CancelPlaceRow();
	Place.Recipe = Recipe;
	Place.Blueprint.Reset();
	Place.bBlueprint = false;
	Place.bSampled = bSampled;
	Place.Built = 0;
	Place.Name = Descriptor ? UFGItemDescriptor::GetItemName(Descriptor) : UFGRecipe::GetRecipeName(TSubclassOf<UFGRecipe>(Recipe));

	// The ghost's footprint: the building's clearance, as the map draws placed buildings.
	const FBox Box = GetDefault<AFGBuildable>(Buildable)->GetCombinedClearanceBox();
	if (Box.IsValid)
	{
		PlaceBoxCenter = Box.GetCenter();
		PlaceBoxHalf = FVector2f(FMath::Max(float(Box.GetExtent().X), 25.f), FMath::Max(float(Box.GetExtent().Y), 25.f));
	}
	else
	{
		PlaceBoxCenter = FVector::ZeroVector;
		PlaceBoxHalf = FVector2f(200.f, 200.f);
	}
	Place.GhostBoxCenter = PlaceBoxCenter;
	Place.GhostHalf = PlaceBoxHalf;
	GatherTemplatePorts(Buildable, Place.GhostPorts);
	UE_LOG(LogFactoryAtlas, Display, TEXT("Place: %s in hand%s, %d ports"), *Buildable->GetName(), bSampled ? TEXT(" (sampled)") : TEXT(""), Place.GhostPorts.Num());
	SpawnPlaceHologram();
}

UFGBlueprintDescriptor* UAtlasSubsystem::BuildGunBlueprint(UClass*& OutRecipe) const
{
	OutRecipe = nullptr;
	AFGCharacterPlayer* Player = BeltPlayer();
	AFGBuildGun* Gun = Player ? Player->GetBuildGun() : nullptr;
	UFGBuildGunStateBuild* Build = Gun ? Cast<UFGBuildGunStateBuild>(Gun->GetBuildGunStateFor(EBuildGunState::BGS_BUILD)) : nullptr;
	if (!Build)
	{
		return nullptr;
	}
	UFGBlueprintDescriptor* Descriptor = Build->GetActiveBlueprintDescriptor();
	UClass* Recipe = Build->GetActiveRecipe();
	const TSoftClassPtr<UFGRecipe>& Shared = UFGBlueprintSettings::Get()->mBlueprintRecipeClass;
	UClass* BlueprintRecipe = Shared.Get() ? Shared.Get() : Shared.LoadSynchronous();
	// Building with the blueprint now, or was just before the map took over: the build state remembers both.
	const bool bBuilding = Gun->IsEquipped() && (Gun->IsInState(EBuildGunState::BGS_BUILD)
		|| (Gun->IsInState(EBuildGunState::BGS_NONE) && Gun->GetPreviousStateEnum() == EBuildGunState::BGS_BUILD));
	UE_LOG(LogFactoryAtlas, Verbose, TEXT("Place: build gun equipped %d, state build %d, previous %d, recipe %s, blueprint %s"),
		Gun->IsEquipped() ? 1 : 0, Gun->IsInState(EBuildGunState::BGS_BUILD) ? 1 : 0, int32(Gun->GetPreviousStateEnum()),
		*GetNameSafe(Recipe), Descriptor ? *Descriptor->GetBlueprintNameAsString() : TEXT("none"));
	if (!bBuilding || !Descriptor || !Recipe || Recipe != BlueprintRecipe)
	{
		return nullptr;
	}
	OutRecipe = Recipe;
	return Descriptor;
}

bool UAtlasSubsystem::TakeBuildGunBlueprint(EView View)
{
	UClass* Recipe = nullptr;
	UFGBlueprintDescriptor* Descriptor = BuildGunBlueprint(Recipe);
	if (!Descriptor || !CanBuildBelts())
	{
		return false;
	}
	if (!IsPlacing(View))
	{
		SetPlacing(View, true);
		if (!IsPlacing(View))
		{
			return false;
		}
	}
	if (Place.bRowBuilding)
	{
		return false;
	}
	CancelPlaceRow();
	Place.Recipe = Recipe;
	Place.Blueprint = Descriptor;
	Place.bBlueprint = true;
	Place.bSampled = false;
	Place.Built = 0;
	Place.Name = FText::FromString(Descriptor->GetBlueprintNameAsString());
	Place.GhostPorts.Reset();
	// The footprint is the blueprint's box, in whole foundations; SpawnPlaceHologram refines it from the hologram.
	const FIntVector Size = Descriptor->GetDimensionsOnInstance();
	PlaceBoxCenter = FVector::ZeroVector;
	PlaceBoxHalf = FVector2f(FMath::Max(Size.X, 1) * 400.f, FMath::Max(Size.Y, 1) * 400.f);
	Place.GhostBoxCenter = PlaceBoxCenter;
	Place.GhostHalf = PlaceBoxHalf;
	UE_LOG(LogFactoryAtlas, Display, TEXT("Place: blueprint %s in hand from the build gun, %dx%dx%d"), *Place.Name.ToString(), Size.X, Size.Y, Size.Z);
	SpawnPlaceHologram();
	ShowToast(FText::Format(FText::FromString(TEXT("Blueprint {0} in hand. Click to place it, R to turn, right click to put it down.")), Place.Name));
	return true;
}

void UAtlasSubsystem::OnMapOpened(EView View)
{
	if (Settings.bBasic)
	{
		return; // No placing from the map: the build gun's blueprint stays in the build gun
	}
	// After this paint, not during it: taking it in hand spawns a hologram.
	if (MapOpenTicker.IsValid())
	{
		return;
	}
	MapOpenTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this, View](float)
	{
		MapOpenTicker.Reset();
		// Another view placing, or another map tool already on here, keeps what it has.
		if (Place.bMode || IsWiring(View) || IsMeasuring(View) || IsPicking(View))
		{
			return false;
		}
		TakeBuildGunBlueprint(View);
		return false;
	}));
}

bool UAtlasSubsystem::SampleHovered(EView View)
{
	if (!CanBuildBelts() || !Hover.bValid || Hover.bMarker || HoverView != View || !Types.IsValidIndex(Hover.Type))
	{
		return false;
	}
	// Foundations are instances rather than actors, so their class comes from the map record.
	AFGBuildable* Building = FindHoveredActor();
	UClass* Class = Building ? Building->GetClass() : Types[Hover.Type].Class;
	if (!IsPlaceableClass(Class))
	{
		ShowToast(FText::FromString(TEXT("Only machines, foundations and other buildings that stand on a floor can be placed from the map.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return true;
	}
	UClass* Recipe = RecipeFor(GetWorld(), Class, Building ? Building->GetBuiltWithRecipe() : nullptr);
	if (!Recipe)
	{
		ShowToast(FText::FromString(TEXT("That building isn't unlocked for building.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return true;
	}
	// The clipboard takes the sample's settings and look, as Ctrl + right click would, so the copies get them and
	// Ctrl + left click still pastes the same.
	CopyHovered();
	TakeInHand(View, Recipe, true);
	if (IsPlacing(View) && Place.Recipe == Recipe)
	{
		ShowToast(FText::Format(FText::FromString(TEXT("{0} in hand, with its settings.")), Place.Name));
		PlayClipboardSound(EClipboardSound::Copy);
	}
	return true;
}

bool UAtlasSubsystem::DropPlace()
{
	// Belts being laid, or a row being dragged or built, go first; the building stays in hand.
	if (Place.bManifoldBelting)
	{
		ManifoldJobs.Reset();
		CancelBeltDrag();
		NextManifoldBelt();
		return true;
	}
	if (Place.bRow || bRowPressed)
	{
		CancelPlaceRow();
		return true;
	}
	if (!Place.Recipe)
	{
		return false;
	}
	DestroyPlaceHologram();
	const EView View = Place.View;
	Place = FPlaceTool();
	Place.bMode = true;
	Place.View = View;
	return true;
}

void UAtlasSubsystem::RotatePlace(int32 Direction, bool bFine)
{
	AFGHologram* Live = PlaceHologram.Get();
	if (!Live || Place.bRow)
	{
		return;
	}
	// The scroll value is degrees, applied the next time the hologram is aimed; set directly rather than through
	// ScrollRotate, whose step the hologram may read its own way.
	const int32 Fine = Live->GetRotationStep() > 0 ? Live->GetRotationStep() : DefaultFineStep;
	PlaceRotation = ((PlaceRotation + (Direction >= 0 ? 1 : -1) * (bFine ? Fine : QuarterTurn)) % 360 + 360) % 360;
	Live->SetScrollRotateValue(PlaceRotation);
	bPlaceAimDirty = true;
	UE_LOG(LogFactoryAtlas, Display, TEXT("Place: turned to %d degrees"), PlaceRotation);
}

void UAtlasSubsystem::DestroyPlaceHologram()
{
	if (AFGHologram* Live = PlaceHologram.Get())
	{
		Live->Destroy();
	}
	PlaceHologram.Reset();
	Place.bHasGhost = false;
}

bool UAtlasSubsystem::SpawnPlaceHologram()
{
	DestroyPlaceHologram();
	Place.bReady = false;
	Place.bValid = false;
	Place.bHasGround = false;
	Place.Reason = FText::GetEmpty();
	Place.CostText.Reset();
	bPlaceAimDirty = true;

	AFGCharacterPlayer* Player = BeltPlayer();
	if (!Player || !Player->GetBuildGun() || !Place.Recipe)
	{
		Place.Reason = FText::FromString(TEXT("Can't build right now."));
		Place.bReady = true;
		return false;
	}
	// A blueprint hologram is told which blueprint before it begins play, which is when it loads it.
	UFGBlueprintDescriptor* Blueprint = Place.Blueprint.Get();
	if (Place.IsBlueprint() && !Blueprint)
	{
		Place.Reason = FText::FromString(TEXT("That blueprint is gone. Choose it again in the build gun."));
		Place.bReady = true;
		return false;
	}
	TFunction<void(AFGHologram*)> PreSpawn;
	if (Blueprint)
	{
		PreSpawn = [Blueprint](AFGHologram* Spawning)
		{
			if (AFGBlueprintHologram* AsBlueprint = Cast<AFGBlueprintHologram>(Spawning))
			{
				AsBlueprint->SetBlueprintDescriptor(Blueprint);
			}
		};
	}
	const FVector At = PlaceUvToWorld(MapCursorUv[int32(Place.View)], double(FilterHigh) * 100.0);
	AFGHologram* Live = AFGHologram::SpawnHologramFromRecipe(TSubclassOf<UFGRecipe>(Place.Recipe), Player->GetBuildGun(), At, Player, PreSpawn);
	if (!Live || (Blueprint && !Live->IsA<AFGBlueprintHologram>()))
	{
		if (Live)
		{
			Live->Destroy();
		}
		Place.Reason = FText::FromString(Blueprint ? TEXT("The game made no hologram for this blueprint.") : TEXT("The game made no hologram for this building."));
		Place.bReady = true;
		return false;
	}
	if (Blueprint)
	{
		// The ghost is the blueprint's box in whole foundations, centred on the hologram, which aligns itself with
		// that box. Only a blueprint that reports no size falls back to what the hologram's meshes fill. The bounds go
		// in the log either way, to check the centring against.
		const FBox Local = Live->CalculateComponentsBoundingBoxInLocalSpace(true, false);
		const FIntVector Size = Blueprint->GetDimensionsOnInstance();
		UE_LOG(LogFactoryAtlas, Display, TEXT("Place: blueprint hologram bounds %s to %s, size %dx%d foundations"),
			*Local.Min.ToCompactString(), *Local.Max.ToCompactString(), Size.X, Size.Y);
		if ((Size.X <= 0 || Size.Y <= 0) && Local.IsValid && Local.GetExtent().X >= 100.0 && Local.GetExtent().Y >= 100.0)
		{
			PlaceBoxCenter = FVector(Local.GetCenter().X, Local.GetCenter().Y, 0.0);
			PlaceBoxHalf = FVector2f(float(Local.GetExtent().X), float(Local.GetExtent().Y));
			Place.GhostBoxCenter = PlaceBoxCenter;
			Place.GhostHalf = PlaceBoxHalf;
		}
	}
	Live->SetScrollRotateValue(PlaceRotation);
	// A sample's look goes on the hologram, so the copy is built in it, the way the build gun builds in the chosen
	// swatch. Foundations, which have no actor to paste onto afterwards, get their paint this way.
	if (Place.bSampled && CopiedCustomization.IsSet())
	{
		if (AFGBuildableHologram* Buildable = Cast<AFGBuildableHologram>(Live))
		{
			Buildable->SetCustomizationData(CopiedCustomization.GetValue());
		}
	}
	PlaceHologram = Live;
	return true;
}

void UAtlasSubsystem::FlushPlacePaste(bool bAll)
{
	for (int32 Index = 0; Index < PlacePastes.Num();)
	{
		if (!bAll && PlacePastes[Index].Frames > 0)
		{
			Index++;
			continue;
		}
		const FPlacePaste Paste = PlacePastes[Index];
		PlacePastes.RemoveAt(Index);
		AFGBuildable* Target = Paste.Building.Get();
		if (!Target)
		{
			continue;
		}
		ActOnActor(Target, [this]() { PasteToSelected(); });
		FText Text;
		bool bError = false;
		float Alpha = 0.f;
		if (GetToast(Text, bError, Alpha) && bError)
		{
			UE_LOG(LogFactoryAtlas, Display, TEXT("Place: %s built, but its settings didn't paste: %s"), *Target->GetName(), *Text.ToString());
			ShowToast(FText::Format(FText::FromString(TEXT("Built {0}, but its settings didn't paste: {1}")), Paste.Name, Text), true);
			PlayClipboardSound(EClipboardSound::Error);
		}
		else if (Paste.bAnnounce)
		{
			ShowToast(FText::Format(FText::FromString(TEXT("Built {0} with the sample's settings.")), Paste.Name));
		}
		else
		{
			ShowToast(FText::GetEmpty());
		}
	}
}

bool UAtlasSubsystem::TickPlace()
{
	// A sampled copy gets its settings once it has had a moment to come into the world.
	for (FPlacePaste& Paste : PlacePastes)
	{
		Paste.Frames--;
	}
	if (!PlacePastes.IsEmpty())
	{
		FlushPlacePaste(false);
	}

	if (!Place.bMode)
	{
		PlaceTicker.Reset();
		return false;
	}
	// The map was closed, or building from it stopped being possible. The hologram is in the world, so nothing may
	// be left behind.
	if (GFrameCounter - MapPaintFrame[int32(Place.View)] > PlaceMapGoneFrames || !CanBuildBelts())
	{
		FlushPlacePaste(true);
		PlaceTicker.Reset();
		DestroyPlaceHologram();
		Place = FPlaceTool();
		bRowPressed = false;
		RowCursor = INDEX_NONE;
		return false;
	}
	if (!Place.Recipe)
	{
		return true;
	}
	if (Place.bManifoldBelting)
	{
		// The belt tool reports each belt; once it is idle again, the next one starts.
		if (!BeltDrag.bActive)
		{
			NextManifoldBelt();
		}
		return true;
	}
	if (Place.bRow)
	{
		bool bSplitter = false;
		if (bRowPressed && !Place.bRowBuilding && Place.bManifold != (FSlateApplication::Get().GetModifierKeys().IsControlDown() && PlaceIsAttachment(bSplitter)))
		{
			MovePlaceRow(Place.View);
		}
		TickPlaceRow();
		return true;
	}

	AFGHologram* Live = PlaceHologram.Get();
	if (!Live)
	{
		return true;
	}
	const FVector2f Uv = MapCursorUv[int32(Place.View)];
	const FVector2f Filter(FilterLow, FilterHigh);
	if (bPlaceAimDirty || Uv != PlaceAimUv || Filter != PlaceAimFilter)
	{
		PlaceAimUv = Uv;
		PlaceAimFilter = Filter;
		bPlaceAimDirty = false;
		AimPlaceAt(PlaceUvToWorld(PlaceAimUv, 0.0));
		return true;
	}
	if (!Place.bHasGround || Place.bReady)
	{
		return true;
	}
	if (PlaceSettleFrames > 0)
	{
		PlaceSettleFrames--;
		return true;
	}
	ValidatePlace();
	return true;
}

bool UAtlasSubsystem::AimPlaceAt(const FVector& Spot)
{
	AFGHologram* Live = PlaceHologram.Get();
	UWorld* World = GetWorld();
	if (!Live || !World)
	{
		return false;
	}
	// Where the build gun would land looking straight down at the spot, from the top of the height filter: the
	// floor the map is showing, not a roof above it.
	ECollisionChannel Channel;
	FHitResult Hit;
	bool bHit = false;
	if (AtlasBeltDetail::FindBuildGunChannel(Channel))
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AtlasPlace), true);
		Params.AddIgnoredActor(Live);
		for (AFGHologram* Child : Live->GetHologramChildren())
		{
			Params.AddIgnoredActor(Child);
		}
		const double Top = double(FilterHigh) * 100.0 + 50.0;
		const double Bottom = double(FilterLow) * 100.0 - 50.0;
		bHit = World->LineTraceSingleByChannel(Hit, FVector(Spot.X, Spot.Y, Top), FVector(Spot.X, Spot.Y, Bottom), Channel, Params)
			&& Live->IsValidHitResult(Hit);
	}
	// Just past the edge of a floor, looking down finds the ground far below it; a foundation then goes beside the
	// floor at its height instead.
	const TSubclassOf<AFGBuildable> InHand = Place.IsBlueprint() ? nullptr : BuildableOf(Place.Recipe);
	if (InHand && InHand->IsChildOf<AFGBuildableFoundation>())
	{
		FHitResult Side;
		if (FindFoundationSideHit(Spot, bHit ? Hit.ImpactPoint.Z : -1e9, Live, Side) && Live->IsValidHitResult(Side))
		{
			Hit = Side;
			bHit = true;
		}
	}
	if (!bHit)
	{
		Live->SetActorHiddenInGame(true);
		Place.bHasGround = false;
		Place.bHasGhost = false;
		Place.bReady = true;
		Place.bValid = false;
		Place.Reason = FText::FromString(TEXT("Nothing to build on here."));
		Place.CostText.Reset();
		return false;
	}
	// Kept unresolved: resolving swaps a foundation instance for a temporary stand-in actor that the game soon
	// recycles, so a row reusing this hit later resolves it again for a live one.
	PlaceLastHit = Hit;
	AtlasBeltDetail::ResolveForPlacement(Hit);
	Live->ResetConstructDisqualifiers();
	Live->SetActorHiddenInGame(false);
	Live->UpdateHologramPlacement(Hit);
	Place.bHasGround = true;
	Place.bReady = false;
	PlaceSettleFrames = PlaceSettle;

	// The ghost follows at once; its color waits for the verdict.
	const FTransform Transform = Live->GetActorTransform();
	Place.HeightMetres = float(Transform.GetLocation().Z / 100.0);
	Place.GhostTransform = Transform;
	Place.GhostCenter = Transform.TransformPosition(PlaceBoxCenter);
	Place.GhostHalf = PlaceBoxHalf;
	Place.GhostYaw = FMath::DegreesToRadians(float(Transform.Rotator().Yaw));
	Place.bHasGhost = true;
	return true;
}

void UAtlasSubsystem::ValidatePlace()
{
	AFGHologram* Live = PlaceHologram.Get();
	AFGCharacterPlayer* Player = BeltPlayer();
	if (!Live || !Player || !Player->GetBuildGun())
	{
		return;
	}
	// Validation refuses anything hidden, and children stay hidden unless shown.
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

	// Unlike lines from the map, a building never clips: the build gun's verdict stands as it is.
	Place.bReady = true;
	if (Live->IsUpgrade())
	{
		Place.bValid = false;
		Place.Reason = FText::FromString(TEXT("That would replace the building already there."));
	}
	else if (Live->CanConstruct())
	{
		Place.bValid = true;
		Place.Reason = FText::GetEmpty();
	}
	else
	{
		Place.bValid = false;
		Place.Reason = !Disqualifiers.IsEmpty() ? UFGConstructDisqualifier::GetDisqualifyingText(Disqualifiers[0])
			: FText::FromString(TEXT("Can't build this here."));
	}

	// Where it settled, after any snapping to the grid under it.
	const FTransform Transform = Live->GetActorTransform();
	Place.GhostTransform = Transform;
	Place.GhostCenter = Transform.TransformPosition(PlaceBoxCenter);
	Place.GhostYaw = FMath::DegreesToRadians(float(Transform.Rotator().Yaw));

	TArray<FString> Parts;
	for (const FItemAmount& Item : Live->GetCost(true))
	{
		if (Item.ItemClass && Item.Amount > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d %s"), Item.Amount, *UFGItemDescriptor::GetItemName(Item.ItemClass).ToString()));
		}
	}
	Place.CostText = FString::Join(Parts, TEXT(", "));
}

void UAtlasSubsystem::ClickPlace(EView View)
{
	if (!IsPlacing(View) || Place.bRow || Place.bManifoldBelting)
	{
		return;
	}
	if (!Place.Recipe)
	{
		ShowToast(FText::FromString(TEXT("Pick a building with Q, or middle click a machine on the map to take it in hand.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	if (!Place.bHasGround || !PlaceHologram.IsValid())
	{
		ShowToast(Place.Reason.IsEmpty() ? FText::FromString(TEXT("Nothing to build on here.")) : Place.Reason, true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	if (!Place.bReady || bPlaceAimDirty)
	{
		ShowToast(FText::FromString(TEXT("Still checking that spot, click again.")));
		return;
	}
	ValidatePlace();
	if (!Place.bValid)
	{
		UE_LOG(LogFactoryAtlas, Display, TEXT("Place: click refused: %s"), *Place.Reason.ToString());
		ShowToast(Place.Reason, true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	BuildPlace();
}

void UAtlasSubsystem::BuildPlace()
{
	FText Error;
	if (!ConstructPlace(Error))
	{
		ShowToast(Error, true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	PlayClipboardSound(EClipboardSound::Paste);
	ShowToast(FText::Format(FText::FromString(TEXT("Built {0}.")), Place.Name));
}

bool UAtlasSubsystem::ConstructPlace(FText& OutError, AActor** OutBuilt)
{
	AFGHologram* Live = PlaceHologram.Get();
	AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(GetWorld());
	if (!Live || !Buildables)
	{
		OutError = FText::FromString(TEXT("Can't build right now."));
		return false;
	}
	// The click that places it; the game says when it has all it needs. A foundation takes one more, for its zoop.
	bool bDone = false;
	for (int32 Step = 0; Step < 3 && !bDone; Step++)
	{
		bDone = Live->DoMultiStepPlacement(false);
	}
	if (!bDone)
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Place: %s not finished after 3 steps"), *GetNameSafe(Place.Recipe));
		OutError = FText::Format(FText::FromString(TEXT("The {0} wasn't finished.")), Place.Name);
		SpawnPlaceHologram();
		return false;
	}

	// Foundations are kept as instances, and building one may hand back no actor; their count says it worked.
	UClass* BuildClass = Live->GetBuildClass();
	const bool bLightweight = BuildClass && BuildClass->IsChildOf<AFGBuildable>() && GetDefault<AFGBuildable>(BuildClass)->ManagedByLightweightBuildableSubsystem();
	const int32 Before = bLightweight ? CountLightweights(GetWorld(), BuildClass) : 0;
	const FTransform Where = Live->GetActorTransform();
	const TArray<FItemAmount> Cost = Live->GetCost(true);
	TArray<AActor*> Children;
	AActor* Built = Live->Construct(Children, Buildables->GetNewNetConstructionID());
	const bool bBuilt = Built || (bLightweight && CountLightweights(GetWorld(), BuildClass) > Before);
	if (!bBuilt)
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Place: Construct built nothing for %s"), *GetNameSafe(Place.Recipe));
		OutError = FText::FromString(TEXT("The game built nothing."));
		SpawnPlaceHologram();
		return false;
	}
	// Construct doesn't charge.
	if (!PayBuildCost(Cost))
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Place: built %s but couldn't take its whole cost"), *GetNameSafe(Built ? static_cast<const UObject*>(Built) : static_cast<const UObject*>(BuildClass)));
	}
	Place.Built++;
	if (OutBuilt)
	{
		*OutBuilt = Built;
	}
	// A row of more than one goes into one blueprint group, so the game's dismantle (and Atlas's) takes it as a whole.
	// A blueprint is already its own group, and groups don't nest.
	if (Place.bRow && Place.Row.Num() > 1 && !Place.IsBlueprint())
	{
		GroupPlaced(Built, BuildClass, bLightweight, Where);
	}
	UE_LOG(LogFactoryAtlas, Display, TEXT("Place: built %s at %s%s"), *GetNameSafe(Built ? static_cast<const UObject*>(Built) : static_cast<const UObject*>(BuildClass)),
		*Place.GhostTransform.GetLocation().ToCompactString(), Place.bSampled && !bLightweight ? TEXT(", settings to follow") : TEXT(""));
	if (Place.bSampled && !bLightweight)
	{
		if (AFGBuildable* Building = Cast<AFGBuildable>(Built))
		{
			FPlacePaste& Paste = PlacePastes.AddDefaulted_GetRef();
			Paste.Building = Building;
			Paste.Frames = PasteDelay;
			Paste.Name = Place.Name;
			Paste.bAnnounce = !Place.bRow;
		}
	}
	bPortsDirty = true;

	// The next one is ready at once, facing the same way.
	SpawnPlaceHologram();
	return true;
}

bool UAtlasSubsystem::PressPlaceRow(EView View)
{
	if (!IsPlacing(View) || !Place.Recipe || !Place.bHasGhost || !PlaceHologram.IsValid() || Place.bRow || Place.bManifoldBelting)
	{
		return false;
	}
	bRowPressed = true;
	RowOrigin = Place.GhostTransform;
	RowOriginHit = PlaceLastHit;
	RowAxis = -1;
	RowCursor = INDEX_NONE;
	return true;
}

void UAtlasSubsystem::MovePlaceRow(EView View)
{
	if (!bRowPressed || !IsPlacing(View) || Place.bRowBuilding)
	{
		return;
	}
	// Along whichever of the building's own sides the drag follows more closely, one building length apart, rounded
	// up to whole metres so each lands on the same foundation grid.
	const FVector Origin = RowOrigin.GetLocation();
	const FVector AxisX = FVector(RowOrigin.GetUnitAxis(EAxis::X).X, RowOrigin.GetUnitAxis(EAxis::X).Y, 0.0).GetSafeNormal();
	const FVector AxisY = FVector(RowOrigin.GetUnitAxis(EAxis::Y).X, RowOrigin.GetUnitAxis(EAxis::Y).Y, 0.0).GetSafeNormal();
	const FVector Cursor = PlaceUvToWorld(MapCursorUv[int32(View)], Origin.Z);
	const FVector Delta(Cursor.X - Origin.X, Cursor.Y - Origin.Y, 0.0);
	const double AlongX = Delta | AxisX;
	const double AlongY = Delta | AxisY;
	const int32 Axis = FMath::Abs(AlongX) >= FMath::Abs(AlongY) ? 0 : 1;
	const double Along = Axis == 0 ? AlongX : AlongY;
	const int32 Sign = Along >= 0.0 ? 1 : -1;
	const double Half = Axis == 0 ? PlaceBoxHalf.X : PlaceBoxHalf.Y;
	const double Step = FMath::Max(100.0, FMath::CeilToDouble(Half * 2.0 / 100.0 - 0.01) * 100.0);
	const int32 Count = FMath::Clamp(FMath::FloorToInt32(FMath::Abs(Along) / Step + 0.5) + 1, 1, MaxRowSpots);

	if (Axis != RowAxis || Sign != RowSign || Step != RowStep)
	{
		Place.Row.Reset();
		RowAxis = Axis;
		RowSign = Sign;
		RowStep = Step;
		RowCursor = INDEX_NONE;
	}
	if (Place.Row.Num() > Count)
	{
		Place.Row.SetNum(Count);
		if (RowCursor >= Count)
		{
			RowCursor = INDEX_NONE;
		}
	}
	const FVector Direction = (Axis == 0 ? AxisX : AxisY) * double(Sign);

	// Ctrl on a row of splitters or mergers: one opposite each free machine port along the drag instead, so each
	// belts straight across to its machine.
	bool bSplitter = false;
	const bool bManifold = FSlateApplication::Get().GetModifierKeys().IsControlDown() && PlaceIsAttachment(bSplitter);
	if (bManifold)
	{
		if (bPortsDirty)
		{
			RebuildPorts();
		}
		const double Length = FMath::Abs(Along);
		TArray<double> Offsets;
		for (const FPort& Port : AllPorts)
		{
			// A splitter row feeds machine inputs; a merger row takes from machine outputs.
			if (Port.Kind != EPortKind::Belt || Port.bOutput == bSplitter || !Port.Owner.IsValid() || AtlasBeltDetail::IsPortConnected(Port.Connection.Get())
				|| Port.Owner->IsA<AFGBuildableConveyorAttachment>() || FMath::Abs(Port.Location.Z - Origin.Z) > ManifoldHeightCm)
			{
				continue;
			}
			const FVector Offset(Port.Location.X - Origin.X, Port.Location.Y - Origin.Y, 0.0);
			const double T = Offset | Direction;
			if (T < -100.0 || T > Length + 100.0)
			{
				continue;
			}
			const FVector Across = Offset - Direction * T;
			const double Distance = Across.Size();
			const FVector Facing = FVector(Port.Normal.X, Port.Normal.Y, 0.0).GetSafeNormal();
			// Facing the row, and not on top of it.
			if (Distance < 100.0 || Distance > ManifoldReachCm || (Facing | (-Across / Distance)) < 0.7)
			{
				continue;
			}
			Offsets.Add(T);
		}
		Offsets.Sort();
		TArray<double> Unique;
		for (const double T : Offsets)
		{
			if (Unique.IsEmpty() || T - Unique.Last() > 150.0)
			{
				Unique.Add(T);
			}
		}
		if (Unique.Num() > MaxRowSpots)
		{
			Unique.SetNum(MaxRowSpots);
		}
		if (!Unique.IsEmpty())
		{
			// Laid out again from scratch whenever the ports it lines up with change.
			bool bSame = Place.bManifold && Place.Row.Num() == Unique.Num();
			for (int32 Index = 0; bSame && Index < Unique.Num(); Index++)
			{
				bSame = FVector::DistSquared2D(Place.Row[Index].Target, Origin + Direction * Unique[Index]) < 1.0;
			}
			if (!bSame)
			{
				Place.Row.Reset();
				RowCursor = INDEX_NONE;
				for (const double T : Unique)
				{
					FPlaceSpot& Spot = Place.Row.AddDefaulted_GetRef();
					Spot.Target = Origin + Direction * T;
					Spot.Ghost = FTransform(RowOrigin.GetRotation(), Spot.Target);
				}
			}
			Place.bManifold = true;
			Place.ManifoldPorts = Unique.Num();
			Place.bRow = true;
			PickRowFloor();
			return;
		}
	}
	if (Place.bManifold)
	{
		// Ctrl let go, or no machine ports along the drag: back to evenly spaced, still belted together when Ctrl is held.
		Place.Row.Reset();
		RowCursor = INDEX_NONE;
	}
	Place.bManifold = bManifold;
	Place.ManifoldPorts = 0;
	while (Place.Row.Num() < Count)
	{
		const int32 Index = Place.Row.Num();
		FPlaceSpot& Spot = Place.Row.AddDefaulted_GetRef();
		Spot.Target = Origin + Direction * (Step * Index);
		Spot.Ghost = FTransform(RowOrigin.GetRotation(), Spot.Target);
	}
	Place.bRow = true;
	PickRowFloor();
}

bool UAtlasSubsystem::ReleasePlaceRow(EView View)
{
	const bool bWasPressed = bRowPressed;
	bRowPressed = false;
	if (!bWasPressed || !Place.bRow || !IsPlacing(View) || Place.bRowBuilding)
	{
		return false;
	}
	Place.bRowBuilding = true;
	Place.RowBuilt = 0;
	Place.RowFailed = 0;
	Place.RowReason = FText::GetEmpty();
	RowCursor = INDEX_NONE;
	return true;
}

void UAtlasSubsystem::CancelPlaceRow()
{
	bRowPressed = false;
	if (Place.bRowBuilding)
	{
		FinishPlaceRow(true);
		return;
	}
	if (!Place.bRow)
	{
		return;
	}
	Place.bRow = false;
	Place.Row.Reset();
	RowCursor = INDEX_NONE;
	RowAxis = -1;
	bPlaceAimDirty = true;
}

void UAtlasSubsystem::FinishPlaceRow(bool bStopped)
{
	const int32 Total = Place.Row.Num();
	const int32 Built = Place.RowBuilt;
	if (Built == 0)
	{
		ShowToast(!Place.RowReason.IsEmpty() ? Place.RowReason : FText::FromString(TEXT("Nothing was built.")), true);
		PlayClipboardSound(EClipboardSound::Error);
	}
	else if (Built < Total)
	{
		const FText Why = bStopped ? FText::FromString(TEXT("Stopped.")) : Place.RowReason;
		ShowToast(FText::Format(FText::FromString(TEXT("Built {0} of {1} {2}. {3}")), Built, Total, Place.Name, Why), true);
		PlayClipboardSound(EClipboardSound::Paste);
	}
	else
	{
		ShowToast(FText::Format(FText::FromString(TEXT("Built {0} {1}.")), Built, Place.Name));
		PlayClipboardSound(EClipboardSound::Paste);
	}
	UE_LOG(LogFactoryAtlas, Display, TEXT("Place: row of %d %s, %d built%s"), Total, *Place.Name.ToString(), Built, bStopped ? TEXT(", stopped") : TEXT(""));
	if (Place.bManifold && Built > 0 && !bStopped)
	{
		PlanManifold();
	}
	if (!Place.bManifoldBelting)
	{
		PlaceGroup.Reset();
	}
	Place.bRow = false;
	Place.bRowBuilding = false;
	Place.Row.Reset();
	RowCursor = INDEX_NONE;
	RowAxis = -1;
	bPlaceAimDirty = true;
}

void UAtlasSubsystem::TickPlaceRow()
{
	// One hologram walks the row: to each spot, a moment to settle, then the game's verdict. Dragging, that only
	// colors the spot; after release, a spot the game accepts is built.
	if (!PlaceHologram.IsValid() && !SpawnPlaceHologram())
	{
		if (Place.bRowBuilding)
		{
			FinishPlaceRow(true);
		}
		else
		{
			CancelPlaceRow();
		}
		return;
	}
	if (!Place.Row.IsValidIndex(RowCursor))
	{
		RowCursor = INDEX_NONE;
		for (int32 Index = 0; Index < Place.Row.Num(); Index++)
		{
			const EPlaceSpot State = Place.Row[Index].State;
			if (State == EPlaceSpot::Unknown || (Place.bRowBuilding && State == EPlaceSpot::Valid))
			{
				RowCursor = Index;
				RowStage = 0;
				break;
			}
		}
		if (RowCursor == INDEX_NONE)
		{
			if (Place.bRowBuilding)
			{
				FinishPlaceRow(false);
			}
			return;
		}
	}

	FPlaceSpot& Spot = Place.Row[RowCursor];
	const auto Block = [this, &Spot](const FText& Reason)
	{
		Spot.State = EPlaceSpot::Blocked;
		Spot.Reason = Reason;
		if (Place.bRowBuilding)
		{
			Place.RowFailed++;
			if (Place.RowReason.IsEmpty())
			{
				Place.RowReason = Reason;
			}
		}
		RowCursor = INDEX_NONE;
	};
	if (RowStage == 0)
	{
		if (!AimPlaceRowSpot(Spot))
		{
			Block(Place.Reason);
			return;
		}
		RowStage = 1;
		RowWait = PlaceSettle;
		return;
	}
	if (--RowWait > 0)
	{
		return;
	}
	ValidatePlace();
	Spot.Ghost = Place.GhostTransform;
	if (!Place.bValid)
	{
		Block(Place.Reason);
		return;
	}
	// Snapping moves a piece a little, never across the map: if the game put it far from its spot, build nothing.
	const FVector Settled = Place.GhostTransform.GetLocation();
	const double Reach = FMath::Max3(400.0, double(PlaceBoxHalf.X), double(PlaceBoxHalf.Y)) * 2.0;
	if (FVector::DistSquared2D(Settled, Spot.Target) > Reach * Reach || FMath::Abs(Settled.Z - RowOrigin.GetLocation().Z) > 2000.0)
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Place: row spot aimed at %s settled at %s, refused"), *Spot.Target.ToCompactString(), *Settled.ToCompactString());
		Block(FText::FromString(TEXT("The game put it somewhere else.")));
		return;
	}
	if (!Place.bRowBuilding)
	{
		Spot.State = EPlaceSpot::Valid;
		Spot.Reason = FText::GetEmpty();
		RowCursor = INDEX_NONE;
		return;
	}
	FText Error;
	AActor* Built = nullptr;
	if (ConstructPlace(Error, &Built))
	{
		Spot.State = EPlaceSpot::Built;
		Spot.Building = Cast<AFGBuildable>(Built);
		Place.RowBuilt++;
		RowCursor = INDEX_NONE;
	}
	else
	{
		Block(Error);
	}
}

TArray<FPlaceOption> UAtlasSubsystem::GetPlaceOptions() const
{
	TArray<FPlaceOption> Options;
	UWorld* World = GetWorld();
	AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
	if (!Recipes)
	{
		return Options;
	}
	TSet<UClass*> Seen;
	TArray<int32> GroupIndex;
	for (const TSubclassOf<UFGRecipe>& Recipe : Recipes->GetAllAvailableRecipes())
	{
		TSubclassOf<UFGItemDescriptor> Descriptor;
		const TSubclassOf<AFGBuildable> Buildable = BuildableOf(Recipe, &Descriptor);
		if (!Buildable || !IsPlaceableClass(Buildable) || Seen.Contains(Buildable.Get()))
		{
			continue;
		}
		Seen.Add(Buildable.Get());
		const FText Name = Descriptor ? UFGItemDescriptor::GetItemName(Descriptor) : UFGRecipe::GetRecipeName(Recipe);
		// Each material of a foundation or wall is its own building with the same name; the list shows one, the
		// plain one, whose class name is the shortest (Build_Foundation_8x1_01 against Build_Foundation_Concrete_8x1).
		// Another material comes from middle clicking a building of it on the map.
		const int32 Same = Options.IndexOfByPredicate([&Name](const FPlaceOption& Other) { return Other.Name.EqualTo(Name); });
		if (Same != INDEX_NONE)
		{
			const TSubclassOf<AFGBuildable> Kept = BuildableOf(Options[Same].Recipe);
			if (Kept && Kept->GetName().Len() > Buildable->GetName().Len())
			{
				Options[Same].Recipe = Recipe;
			}
			continue;
		}
		FPlaceOption& Option = Options.AddDefaulted_GetRef();
		Option.Recipe = Recipe;
		Option.Name = Name;
		const int32 Group = GroupOf(Buildable);
		Option.Group = Groups[Group];
		GroupIndex.Add(Group);
	}
	TArray<int32> Order;
	for (int32 i = 0; i < Options.Num(); i++)
	{
		Order.Add(i);
	}
	Algo::Sort(Order, [&](int32 A, int32 B)
	{
		if (GroupIndex[A] != GroupIndex[B])
		{
			return GroupIndex[A] < GroupIndex[B];
		}
		return Options[A].Name.CompareTo(Options[B].Name) < 0;
	});
	TArray<FPlaceOption> Sorted;
	for (int32 Index : Order)
	{
		Sorted.Add(Options[Index]);
	}
	return Sorted;
}

bool UAtlasSubsystem::PlaceIsAttachment(bool& bOutSplitter) const
{
	const TSubclassOf<AFGBuildable> Buildable = Place.IsBlueprint() ? nullptr : BuildableOf(Place.Recipe);
	if (!Buildable || !Buildable->IsChildOf<AFGBuildableConveyorAttachment>())
	{
		return false;
	}
	// One way in and several out is a splitter; the other way round, a merger.
	int32 Inputs = 0;
	int32 Outputs = 0;
	for (const FPort& Port : Place.GhostPorts)
	{
		if (Port.Kind == EPortKind::Belt)
		{
			(Port.bOutput ? Outputs : Inputs)++;
		}
	}
	if (Inputs == 0 || Outputs == 0 || Inputs == Outputs)
	{
		return false;
	}
	bOutSplitter = Outputs > Inputs;
	return true;
}

void UAtlasSubsystem::BeltPortsOf(AFGBuildable* Building, TArray<FPort>& Out) const
{
	Out.Reset();
	if (!IsValid(Building))
	{
		return;
	}
	TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Building);
	for (UFGFactoryConnectionComponent* Connection : Connections)
	{
		FPort Port;
		if (AtlasBeltDetail::DescribePort(Connection, Port) && Port.Kind == EPortKind::Belt)
		{
			Port.Owner = Building;
			Port.Uv = FVector2f(float((Port.Location.X - MapWestCm) / MapWidthCm), float((Port.Location.Y - MapNorthCm) / MapHeightCm));
			Out.Add(Port);
		}
	}
}

void UAtlasSubsystem::PlanManifold()
{
	ManifoldJobs.Reset();
	Place.ManifoldBelts = 0;
	Place.ManifoldFailed = 0;
	Place.ManifoldReason = FText::GetEmpty();
	bool bSplitter = false;
	if (!PlaceIsAttachment(bSplitter))
	{
		return;
	}
	TArray<AFGBuildable*> Pieces;
	for (const FPlaceSpot& Spot : Place.Row)
	{
		if (AFGBuildable* Building = Spot.Building.Get())
		{
			Pieces.Add(Building);
		}
	}
	TArray<TArray<FPort>> PortsOf;
	PortsOf.SetNum(Pieces.Num());
	for (int32 Index = 0; Index < Pieces.Num(); Index++)
	{
		BeltPortsOf(Pieces[Index], PortsOf[Index]);
	}
	TSet<const UFGConnectionComponent*> Used;
	const auto Flat = [](const FVector& Vector) { return FVector(Vector.X, Vector.Y, 0.0).GetSafeNormal(); };

	// Along the row: the output of one piece that faces the next, to the input of the next that faces back. Which
	// way the row carries is whichever way its pieces line up best, kept for the whole row.
	int32 Flow = 0; // 1 from the first piece to the last, -1 the other way
	for (int32 Index = 0; Index + 1 < Pieces.Num(); Index++)
	{
		const FVector Along = Flat(Pieces[Index + 1]->GetActorLocation() - Pieces[Index]->GetActorLocation());
		const auto Best = [&](const TArray<FPort>& From, const TArray<FPort>& To, const FVector& Direction, const FPort*& OutFrom, const FPort*& OutTo)
		{
			double BestScore = 1.2; // Both ports must face the other piece fairly squarely
			OutFrom = nullptr;
			OutTo = nullptr;
			for (const FPort& Out : From)
			{
				if (!Out.bOutput || Used.Contains(Out.Connection.Get()))
				{
					continue;
				}
				for (const FPort& In : To)
				{
					if (In.bOutput || Used.Contains(In.Connection.Get()))
					{
						continue;
					}
					const double Score = (Flat(Out.Normal) | Direction) + (Flat(In.Normal) | -Direction);
					if (Score > BestScore)
					{
						BestScore = Score;
						OutFrom = &Out;
						OutTo = &In;
					}
				}
			}
			return BestScore;
		};
		const FPort* ForwardOut = nullptr;
		const FPort* ForwardIn = nullptr;
		const FPort* BackOut = nullptr;
		const FPort* BackIn = nullptr;
		const double Forward = Flow >= 0 ? Best(PortsOf[Index], PortsOf[Index + 1], Along, ForwardOut, ForwardIn) : 0.0;
		const double Back = Flow <= 0 ? Best(PortsOf[Index + 1], PortsOf[Index], -Along, BackOut, BackIn) : 0.0;
		if (ForwardOut && (!BackOut || Forward >= Back))
		{
			Flow = 1;
			ManifoldJobs.Add({ *ForwardOut, *ForwardIn });
			Used.Add(ForwardOut->Connection.Get());
			Used.Add(ForwardIn->Connection.Get());
		}
		else if (BackOut)
		{
			Flow = -1;
			ManifoldJobs.Add({ *BackOut, *BackIn });
			Used.Add(BackOut->Connection.Get());
			Used.Add(BackIn->Connection.Get());
		}
	}

	// Across to the machines: each piece's free side port to the nearest free machine port facing it.
	RebuildPorts();
	TSet<const AFGBuildable*> Row;
	for (AFGBuildable* Piece : Pieces)
	{
		Row.Add(Piece);
	}
	for (int32 Index = 0; Index < Pieces.Num(); Index++)
	{
		const FPort* BestOwn = nullptr;
		const FPort* BestMachine = nullptr;
		double BestDistance = ManifoldReachCm;
		for (const FPort& Own : PortsOf[Index])
		{
			// A splitter gives from an output; a merger takes into an input.
			if (Own.bOutput != bSplitter || Used.Contains(Own.Connection.Get()))
			{
				continue;
			}
			for (const FPort& Machine : AllPorts)
			{
				if (Machine.Kind != EPortKind::Belt || Machine.bOutput == bSplitter || !Machine.Owner.IsValid() || Row.Contains(Machine.Owner.Get())
					|| Used.Contains(Machine.Connection.Get()) || AtlasBeltDetail::IsPortConnected(Machine.Connection.Get())
					|| Machine.Owner->IsA<AFGBuildableConveyorAttachment>() || FMath::Abs(Machine.Location.Z - Own.Location.Z) > ManifoldHeightCm)
				{
					continue;
				}
				const FVector Offset(Machine.Location.X - Own.Location.X, Machine.Location.Y - Own.Location.Y, 0.0);
				const double Distance = Offset.Size();
				if (Distance < 1.0 || Distance >= BestDistance)
				{
					continue;
				}
				const FVector Direction = Offset / Distance;
				if ((Flat(Own.Normal) | Direction) < 0.7 || (Flat(Machine.Normal) | -Direction) < 0.7)
				{
					continue;
				}
				BestDistance = Distance;
				BestOwn = &Own;
				BestMachine = &Machine;
			}
		}
		if (BestOwn)
		{
			ManifoldJobs.Add(bSplitter ? TPair<FPort, FPort>(*BestOwn, *BestMachine) : TPair<FPort, FPort>(*BestMachine, *BestOwn));
			Used.Add(BestOwn->Connection.Get());
			Used.Add(BestMachine->Connection.Get());
		}
	}
	Place.ManifoldPlanned = ManifoldJobs.Num();
	UE_LOG(LogFactoryAtlas, Display, TEXT("Manifold: %d pieces, %d belts planned"), Pieces.Num(), ManifoldJobs.Num());
	if (!ManifoldJobs.IsEmpty())
	{
		Place.bManifoldBelting = true;
		Place.Built = Pieces.Num();
		DestroyPlaceHologram(); // Out of the belts' way; a new one comes once they are done
	}
	else
	{
		ShowToast(FText::Format(FText::FromString(TEXT("Built {0} {1}, but found nothing to belt them to.")), Pieces.Num(), Place.Name), true);
	}
}

void UAtlasSubsystem::NextManifoldBelt()
{
	// Each belt goes through the belt tool, as a drag between the two ports would.
	while (!ManifoldJobs.IsEmpty())
	{
		const TPair<FPort, FPort> Belt = ManifoldJobs[0];
		ManifoldJobs.RemoveAt(0);
		if (!Belt.Key.Connection.IsValid() || !Belt.Value.Connection.IsValid() || AtlasBeltDetail::IsPortConnected(Belt.Key.Connection.Get()) || AtlasBeltDetail::IsPortConnected(Belt.Value.Connection.Get()))
		{
			continue;
		}
		StartAutoBelt(Belt.Key, Belt.Value);
		return;
	}
	Place.bManifoldBelting = false;
	PlaceGroup.Reset();
	const int32 Pieces = Place.Built;
	if (Place.ManifoldFailed > 0)
	{
		ShowToast(FText::Format(FText::FromString(TEXT("Built {0} {1} and {2} of {3} belts. {4}")), Pieces, Place.Name, Place.ManifoldBelts,
			Place.ManifoldPlanned, Place.ManifoldReason), true);
	}
	else
	{
		ShowToast(FText::Format(FText::FromString(TEXT("Built {0} {1} and {2} belts.")), Pieces, Place.Name, Place.ManifoldBelts));
	}
	PlayClipboardSound(Place.ManifoldBelts > 0 ? EClipboardSound::Paste : EClipboardSound::Error);
	UE_LOG(LogFactoryAtlas, Display, TEXT("Manifold: %d of %d belts built"), Place.ManifoldBelts, Place.ManifoldPlanned);
	bPlaceAimDirty = true;
	SpawnPlaceHologram();
}

bool UAtlasSubsystem::AimPlaceRowSpot(const FPlaceSpot& Spot)
{
	AFGHologram* Live = PlaceHologram.Get();
	UWorld* World = GetWorld();
	if (!Live || !World)
	{
		return false;
	}
	// Every piece of a row keeps the first one's height and grid, as the game's zoop extends a line of foundations:
	// the hologram is aimed where the first went, then moved along the row. Foundations may run out over open air,
	// as a zoop does; anything else needs something to stand on at that height.
	const FVector Offset(Spot.Target.X - RowOrigin.GetLocation().X, Spot.Target.Y - RowOrigin.GetLocation().Y, 0.0);
	const TSubclassOf<AFGBuildable> Buildable = Place.IsBlueprint() ? nullptr : BuildableOf(Place.Recipe);
	if (!Buildable || !Buildable->IsChildOf<AFGBuildableFoundation>())
	{
		// A machine (or a blueprint) stands on the row's floor at its own spot, aimed there as the build gun would be.
		ECollisionChannel Channel;
		FHitResult Floor;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(AtlasPlaceRowFloor), true);
		Params.AddIgnoredActor(Live);
		for (AFGHologram* Child : Live->GetHologramChildren())
		{
			Params.AddIgnoredActor(Child);
		}
		if (!AtlasBeltDetail::FindBuildGunChannel(Channel)
			|| !World->LineTraceSingleByChannel(Floor, FVector(Spot.Target.X, Spot.Target.Y, RowFloorZ + 150.0), FVector(Spot.Target.X, Spot.Target.Y, RowFloorZ - 150.0), Channel, Params)
			|| !Live->IsValidHitResult(Floor))
		{
			Place.bReady = true;
			Place.bValid = false;
			Place.Reason = FText::FromString(TEXT("Nothing to stand on at this height."));
			return false;
		}
		AtlasBeltDetail::ResolveForPlacement(Floor);
		Live->ResetConstructDisqualifiers();
		Live->SetActorHiddenInGame(false);
		Live->UpdateHologramPlacement(Floor);
		Place.bHasGround = true;
		Place.bReady = false;
		PlaceSettleFrames = PlaceSettle;
		const FTransform Transform = Live->GetActorTransform();
		Place.GhostTransform = Transform;
		Place.GhostCenter = Transform.TransformPosition(PlaceBoxCenter);
		Place.GhostYaw = FMath::DegreesToRadians(float(Transform.Rotator().Yaw));
		Place.bHasGhost = true;
		return true;
	}
	// Resolved afresh for every spot: the stand-in the first aim got has been recycled and parked far away by now,
	// and a hologram snapped to it built the whole row out of sight.
	FHitResult Origin = RowOriginHit;
	AtlasBeltDetail::ResolveForPlacement(Origin);
	Live->ResetConstructDisqualifiers();
	Live->SetActorHiddenInGame(false);
	Live->UpdateHologramPlacement(Origin);
	Live->SetActorLocation(Live->GetActorLocation() + Offset);
	Place.bHasGround = true;
	Place.bReady = false;
	PlaceSettleFrames = PlaceSettle;
	const FTransform Transform = Live->GetActorTransform();
	Place.GhostTransform = Transform;
	Place.GhostCenter = Transform.TransformPosition(PlaceBoxCenter);
	Place.GhostYaw = FMath::DegreesToRadians(float(Transform.Rotator().Yaw));
	Place.bHasGhost = true;
	return true;
}

bool UAtlasSubsystem::FindFoundationSideHit(const FVector& Spot, double BelowZ, const AFGHologram* Live, FHitResult& Out) const
{
	const FGrid* Current = GetGrid();
	UWorld* World = GetWorld();
	ECollisionChannel Channel;
	if (!Current || !World || !AtlasBeltDetail::FindBuildGunChannel(Channel))
	{
		return false;
	}
	constexpr float ReachMetres = 3.f; // How far past a floor's edge still counts as beside it
	const FVector2f Cursor(float((Spot.X - MapWestCm) / MapWidthCm) * MapWidthMetres, float((Spot.Y - MapNorthCm) / MapHeightCm) * MapWidthMetres);
	const int32 CenterX = FMath::Clamp(int32(Cursor.X / MapWidthMetres * GridSize), 0, GridSize - 1);
	const int32 CenterY = FMath::Clamp(int32(Cursor.Y / MapWidthMetres * GridSize), 0, GridSize - 1);

	// The nearest visible foundation standing higher than what is under the cursor, by its map shape.
	float BestDistance = ReachMetres;
	FVector2f BestPoint = FVector2f::ZeroVector;
	double BestTop = 0.0;
	bool bFound = false;
	for (int32 CellY = FMath::Max(0, CenterY - 1); CellY <= FMath::Min(GridSize - 1, CenterY + 1); CellY++)
	{
		for (int32 CellX = FMath::Max(0, CenterX - 1); CellX <= FMath::Min(GridSize - 1, CenterX + 1); CellX++)
		{
			for (const FVector4f& Packed : Current->Cells[CellY * GridSize + CellX])
			{
				const FDecoded Record(Packed);
				if (!Types.IsValidIndex(Record.Type) || Types[Record.Type].Category != ECategory::Foundations || !IsRecordShown(Record))
				{
					continue;
				}
				const double Top = double(Record.Z) * ZStepCm - ZBaseCm;
				if (Top <= BelowZ + 100.0)
				{
					continue;
				}
				const FVector2f Center = FVector2f(float(Record.X), float(Record.Y)) / float(PositionMax) * MapWidthMetres;
				const float Angle = float(Record.Angle) / 1024.f * UE_TWO_PI;
				float Sin, Cos;
				FMath::SinCos(&Sin, &Cos, Angle);
				const FVector2f Delta = Cursor - Center;
				const FVector2f Local(Delta.X * Cos + Delta.Y * Sin, -Delta.X * Sin + Delta.Y * Cos);
				const FVector2f Half(Record.HalfX * 0.1f, Record.HalfY * 0.1f);
				const FVector2f Clamped(FMath::Clamp(Local.X, -Half.X, Half.X), FMath::Clamp(Local.Y, -Half.Y, Half.Y));
				const float Distance = FVector2f::Distance(Local, Clamped);
				// Over it, looking down already finds it.
				if (Distance <= 0.01f || Distance > BestDistance + 0.01f || (FMath::IsNearlyEqual(Distance, BestDistance, 0.01f) && Top <= BestTop))
				{
					continue;
				}
				BestDistance = Distance;
				BestTop = Top;
				BestPoint = Center + FVector2f(Clamped.X * Cos - Clamped.Y * Sin, Clamped.X * Sin + Clamped.Y * Cos);
				bFound = true;
			}
		}
	}
	if (!bFound)
	{
		return false;
	}

	// Across at the floor's height toward its nearest edge, so the hit is on its side, facing the cursor.
	const FVector From(Spot.X, Spot.Y, 0.0);
	const FVector Edge(MapWestCm + double(BestPoint.X) / MapWidthMetres * MapWidthCm, MapNorthCm + double(BestPoint.Y) / MapWidthMetres * MapHeightCm, 0.0);
	const FVector Direction = (Edge - From).GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AtlasPlaceSide), true);
	if (Live)
	{
		Params.AddIgnoredActor(Live);
		for (AFGHologram* Child : Live->GetHologramChildren())
		{
			Params.AddIgnoredActor(Child);
		}
	}
	// Where a foundation's pivot sits differs, so a few heights around its recorded one are tried.
	for (const double Drop : { -50.0, -150.0, 50.0, -350.0 })
	{
		const double Z = BestTop + Drop;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, FVector(From.X, From.Y, Z) - Direction * 100.0, FVector(Edge.X, Edge.Y, Z) + Direction * 300.0, Channel, Params)
			&& FMath::Abs(Hit.ImpactNormal.Z) < 0.5)
		{
			// Left unresolved; the caller resolves it right before use.
			Out = Hit;
			return true;
		}
	}
	return false;
}

void UAtlasSubsystem::GroupPlaced(AActor* Built, UClass* BuildClass, bool bLightweight, const FTransform& Where)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	AFGBlueprintProxy* Group = PlaceGroup.Get();
	if (!Group)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Group = World->SpawnActor<AFGBlueprintProxy>(AFGBlueprintProxy::StaticClass(), Where, Params);
		if (!IsValid(Group))
		{
			UE_LOG(LogFactoryAtlas, Warning, TEXT("Place: couldn't make a blueprint group for the row"));
			return;
		}
		PlaceGroup = Group;
	}
	if (!bLightweight)
	{
		AFGBuildable* Building = Cast<AFGBuildable>(Built);
		if (IsValid(Building) && !Building->GetBlueprintProxy() && !Building->GetBlueprintDesigner())
		{
			Building->SetBlueprintProxy(Group);
			Group->RegisterBuildable(Building);
		}
		return;
	}
	// A foundation is an instance by now, not an actor: find the one just made where the hologram stood, and give it
	// the group the way the game does for a blueprint's foundations.
	AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
	const TArray<FRuntimeBuildableInstanceData>* Instances = Lightweights && BuildClass ? Lightweights->GetAllLightweightBuildableInstances().Find(BuildClass) : nullptr;
	for (int32 Index = Instances ? Instances->Num() - 1 : -1; Index >= 0; Index--)
	{
		const FRuntimeBuildableInstanceData& Data = (*Instances)[Index];
		if (!Data.IsValid() || FVector::DistSquared(Data.Transform.GetLocation(), Where.GetLocation()) > 25.0)
		{
			continue;
		}
		if (!Data.BlueprintProxy)
		{
			const_cast<FRuntimeBuildableInstanceData&>(Data).BlueprintProxy = Group;
			Group->RegisterLightweightInstance(BuildClass, Index);
		}
		return;
	}
	UE_LOG(LogFactoryAtlas, Display, TEXT("Place: built %s, but couldn't find its instance to group it"), *GetNameSafe(BuildClass));
}

void UAtlasSubsystem::PickRowFloor()
{
	RowFloorZ = RowOrigin.GetLocation().Z;
	Place.HeightMetres = float(RowFloorZ / 100.0);
	UWorld* World = GetWorld();
	ECollisionChannel Channel;
	if (!World || Place.Row.IsEmpty() || !AtlasBeltDetail::FindBuildGunChannel(Channel))
	{
		return;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AtlasPlaceRowPick), true);
	if (AFGHologram* Live = PlaceHologram.Get())
	{
		Params.AddIgnoredActor(Live);
		for (AFGHologram* Child : Live->GetHologramChildren())
		{
			Params.AddIgnoredActor(Child);
		}
	}
	// Half-metre steps: floors a little apart still count as one. The most spots win; a tie goes to the higher.
	TMap<int32, int32> Votes;
	TMap<int32, double> Heights;
	const double Top = double(FilterHigh) * 100.0 + 50.0;
	const double Bottom = double(FilterLow) * 100.0 - 50.0;
	for (const FPlaceSpot& Spot : Place.Row)
	{
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, FVector(Spot.Target.X, Spot.Target.Y, Top), FVector(Spot.Target.X, Spot.Target.Y, Bottom), Channel, Params))
		{
			const int32 Bucket = FMath::RoundToInt32(Hit.ImpactPoint.Z / 50.0);
			Votes.FindOrAdd(Bucket)++;
			Heights.FindOrAdd(Bucket) = Hit.ImpactPoint.Z;
		}
	}
	int32 BestBucket = 0;
	int32 BestVotes = 0;
	for (const TPair<int32, int32>& Vote : Votes)
	{
		if (Vote.Value > BestVotes || (Vote.Value == BestVotes && Vote.Key > BestBucket))
		{
			BestBucket = Vote.Key;
			BestVotes = Vote.Value;
		}
	}
	if (BestVotes > 0)
	{
		RowFloorZ = Heights[BestBucket];
		Place.HeightMetres = float(RowFloorZ / 100.0);
	}
}
