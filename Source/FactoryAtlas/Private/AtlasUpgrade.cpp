// Upgrading a belt line from the map: with a belt or lift selected, a trace follows the items downstream and every
// belt and lift on the way is replaced by the chosen tier. Each replacement goes through the game's own upgrade
// hologram, as aiming the build gun at the belt does, so items stay on the belt and connections carry over. The cost
// is the new tier's, less what the old belt gives back, from the inventory and then the Dimensional Depot.
// Single player and host only, like building.

#include "AtlasSubsystem.h"

#include "AtlasBuildShared.h"
#include "AtlasTypes.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildablePassthrough.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGun.h"
#include "FactoryAtlas.h"
#include "FGBlueprintProxy.h"
#include "FGBuildableSubsystem.h"
#include "FGCentralStorageSubsystem.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGDismantleInterface.h"
#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Hologram/FGConveyorBeltHologram.h"
#include "Hologram/FGConveyorLiftHologram.h"
#include "Hologram/FGHologram.h"
#include "ItemAmount.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/UnrealType.h"

using namespace Atlas;

namespace AtlasUpgradeDetail
{
	/** Frames are kept smooth: at most this many replacements, or this long, each frame. */
	constexpr int32 PerFrame = 16;
	constexpr double FrameBudgetSeconds = 0.008;

	struct FConveyorRecipe
	{
		UClass* Recipe = nullptr;
		UClass* Buildable = nullptr;
		float Speed = 0.f;
	};

	/** Every unlocked belt and lift tier, slowest first: belts get a lift of the same speed when one is unlocked. */
	TArray<FUpgradeTier> UnlockedTiers(UWorld* World)
	{
		TArray<FUpgradeTier> Out;
		AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
		if (!Recipes)
		{
			return Out;
		}
		TArray<FConveyorRecipe> Belts;
		TArray<FConveyorRecipe> Lifts;
		for (const TSubclassOf<UFGRecipe>& Recipe : Recipes->GetAllAvailableRecipes())
		{
			const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
			if (Products.IsEmpty() || !Products[0].ItemClass || !Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass()))
			{
				continue;
			}
			UClass* Buildable = UFGBuildingDescriptor::GetBuildableClass(TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass));
			if (!Buildable)
			{
				continue;
			}
			TArray<FConveyorRecipe>* Into = Buildable->IsChildOf<AFGBuildableConveyorBelt>() ? &Belts
				: Buildable->IsChildOf<AFGBuildableConveyorLift>() ? &Lifts : nullptr;
			// One recipe per building; a second one for the same building changes nothing here.
			if (Into && !Into->ContainsByPredicate([Buildable](const FConveyorRecipe& Other) { return Other.Buildable == Buildable; }))
			{
				Into->Add({ Recipe.Get(), Buildable, GetDefault<AFGBuildableConveyorBase>(Buildable)->GetSpeed() });
			}
		}
		Belts.Sort([](const FConveyorRecipe& A, const FConveyorRecipe& B) { return A.Speed < B.Speed; });
		for (const FConveyorRecipe& Belt : Belts)
		{
			FUpgradeTier& Tier = Out.AddDefaulted_GetRef();
			Tier.BeltRecipe = Belt.Recipe;
			Tier.Belt = Belt.Buildable;
			Tier.Speed = Belt.Speed;
			Tier.Name = UFGRecipe::GetRecipeName(TSubclassOf<UFGRecipe>(Belt.Recipe));
			for (const FConveyorRecipe& Lift : Lifts)
			{
				if (FMath::IsNearlyEqual(Lift.Speed, Belt.Speed, 0.5f))
				{
					Tier.LiftRecipe = Lift.Recipe;
					Tier.Lift = Lift.Buildable;
					break;
				}
			}
		}
		return Out;
	}

	/** The connections items leave a building by. */
	void Outputs(AFGBuildable* Building, TArray<UFGFactoryConnectionComponent*>& Out)
	{
		Out.Reset();
		TInlineComponentArray<UFGFactoryConnectionComponent*> Connections(Building);
		for (UFGFactoryConnectionComponent* Connection : Connections)
		{
			if (Connection && Connection->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT)
			{
				Out.Add(Connection);
			}
		}
	}

	FString ItemName(UClass* Item)
	{
		return UFGItemDescriptor::GetItemName(TSubclassOf<UFGItemDescriptor>(Item)).ToString();
	}

	/** "Costs 40 Steel Beam, 20 Rubber and 2 more; gives back 80 Iron Plate", largest first. */
	FString DescribeNet(const TMap<UClass*, int32>& Net)
	{
		TArray<TPair<UClass*, int32>> Costs;
		TArray<TPair<UClass*, int32>> Returns;
		for (const TPair<UClass*, int32>& Pair : Net)
		{
			if (Pair.Key && Pair.Value > 0)
			{
				Costs.Add(Pair);
			}
			else if (Pair.Key && Pair.Value < 0)
			{
				Returns.Add(TPair<UClass*, int32>(Pair.Key, -Pair.Value));
			}
		}
		const auto List = [](TArray<TPair<UClass*, int32>>& Items)
		{
			Items.Sort([](const TPair<UClass*, int32>& A, const TPair<UClass*, int32>& B) { return A.Value > B.Value; });
			TArray<FString> Parts;
			for (int32 Index = 0; Index < Items.Num() && Index < 3; Index++)
			{
				Parts.Add(FString::Printf(TEXT("%d %s"), Items[Index].Value, *ItemName(Items[Index].Key)));
			}
			FString Text = FString::Join(Parts, TEXT(", "));
			if (Items.Num() > 3)
			{
				Text += FString::Printf(TEXT(" and %d more"), Items.Num() - 3);
			}
			return Text;
		};
		FString Text;
		if (!Costs.IsEmpty())
		{
			Text = TEXT("Costs ") + List(Costs);
		}
		if (!Returns.IsEmpty())
		{
			Text += Text.IsEmpty() ? TEXT("Gives back ") : TEXT("; gives back ");
			Text += List(Returns);
		}
		return Text.IsEmpty() ? FString(TEXT("Costs nothing")) : Text;
	}

	FStructProperty* LiftTopProperty()
	{
		FStructProperty* Top = FindFProperty<FStructProperty>(AFGConveyorLiftHologram::StaticClass(), TEXT("mTopTransform"));
		return Top && Top->Struct == TBaseStructure<FTransform>::Get() ? Top : nullptr;
	}

	/**
	 * Gives a lift hologram the old lift's height and floor holes. The game's upgrade takes them from where the build
	 * gun aims, which a hit made up from the lift's own place doesn't carry.
	 */
	bool CopyLiftShape(AFGBuildableConveyorLift* Lift, AFGHologram* Hologram)
	{
		FStructProperty* Top = LiftTopProperty();
		FArrayProperty* Slots = FindFProperty<FArrayProperty>(AFGConveyorLiftHologram::StaticClass(), TEXT("mSnappedPassthroughs"));
		FObjectPropertyBase* Element = Slots ? CastField<FObjectPropertyBase>(Slots->Inner) : nullptr;
		if (!Hologram->IsA<AFGConveyorLiftHologram>() || !Top || !Element)
		{
			return false;
		}
		const FTransform Value = Lift->GetTopTransform();
		Top->CopyCompleteValue(Top->ContainerPtrToValuePtr<void>(Hologram), &Value);
		const TArray<AFGBuildablePassthrough*> Holes = Lift->GetSnappedPassthroughs();
		FScriptArrayHelper Array(Slots, Slots->ContainerPtrToValuePtr<void>(Hologram));
		Array.Resize(Holes.Num());
		for (int32 Index = 0; Index < Holes.Num(); Index++)
		{
			Element->SetObjectPropertyValue(Array.GetRawPtr(Index), Holes[Index]);
		}
		return true;
	}

	bool LiftShapeKept(AFGBuildableConveyorLift* Lift, AFGHologram* Hologram)
	{
		FStructProperty* Top = LiftTopProperty();
		return Top && Top->ContainerPtrToValuePtr<FTransform>(Hologram)->Equals(Lift->GetTopTransform(), 0.1f);
	}

	/** The same shape is built where the old one stood, so overlapping what the old one overlapped is no reason to stop. */
	bool IsIgnoredForUpgrade(const TSubclassOf<UFGConstructDisqualifier>& Disqualifier)
	{
		// Atlas prices the upgrade itself, net of the refund; the hologram only knows the gross cost.
		return !Disqualifier || Disqualifier->IsChildOf(UFGCDUnaffordable::StaticClass()) || AtlasBeltDetail::IsClippingDisqualifier(Disqualifier);
	}
}
using namespace AtlasUpgradeDetail;

bool UAtlasSubsystem::IsSelectedConveyor() const
{
	const AFGBuildable* Building = bSelectedLightweight ? nullptr : Selected.Get();
	return IsValid(Building) && Building->IsA<AFGBuildableConveyorBase>();
}

void UAtlasSubsystem::SetUpgradeOpen(bool bOpen)
{
	if (bOpen && (!IsSelectedConveyor() || Upgrade.bOpen))
	{
		return;
	}
	Upgrade.bOpen = bOpen;
	if (bOpen)
	{
		TraceUpgrade();
		return;
	}
	if (!Upgrade.bRunning)
	{
		Upgrade.Line.Reset();
		Upgrade.Tiers.Reset();
	}
	RefreshUpgradeRecords();
}

void UAtlasSubsystem::SetUpgradeThroughAttachments(bool bThrough)
{
	if (Upgrade.bRunning || Upgrade.bThroughAttachments == bThrough)
	{
		return;
	}
	Upgrade.bThroughAttachments = bThrough;
	if (Upgrade.bOpen)
	{
		TraceUpgrade();
	}
}

void UAtlasSubsystem::TraceUpgrade()
{
	Upgrade.Line.Reset();
	Upgrade.Belts = 0;
	Upgrade.Lifts = 0;
	Upgrade.LengthMetres = 0.f;
	Upgrade.EndText.Reset();
	Upgrade.bCapped = false;
	Upgrade.Tiers.Reset();

	AFGBuildable* Start = IsSelectedConveyor() ? Selected.Get() : nullptr;
	if (!Start)
	{
		RefreshUpgradeRecords();
		return;
	}

	// Breadth first from the selection: belts and lifts go on the line, splitters and mergers are passed through when
	// asked, and anything else is where the line ends.
	TArray<AFGBuildable*> Queue = { Start };
	TSet<AFGBuildable*> Seen = { Start };
	TArray<FString> Ends;
	int32 LooseEnds = 0;
	TArray<UFGFactoryConnectionComponent*> Exits;
	for (int32 Head = 0; Head < Queue.Num(); Head++)
	{
		AFGBuildable* Building = Queue[Head];
		if (AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(Building))
		{
			Upgrade.Line.Add(Building);
			if (Conveyor->IsA<AFGBuildableConveyorLift>())
			{
				Upgrade.Lifts++;
			}
			else
			{
				Upgrade.Belts++;
			}
			Upgrade.LengthMetres += Conveyor->GetLength() / 100.f;
		}
		Outputs(Building, Exits);
		for (UFGFactoryConnectionComponent* Exit : Exits)
		{
			UFGFactoryConnectionComponent* Partner = Exit->IsConnected() ? Exit->GetConnection() : nullptr;
			AFGBuildable* Next = Partner ? Cast<AFGBuildable>(Partner->GetOwner()) : nullptr;
			if (!Next)
			{
				LooseEnds++;
				continue;
			}
			if (Seen.Contains(Next))
			{
				continue; // A loop, or a merger reached twice
			}
			Seen.Add(Next);
			const bool bConveyor = Next->IsA<AFGBuildableConveyorBase>();
			if (bConveyor || (Upgrade.bThroughAttachments && Next->IsA<AFGBuildableConveyorAttachment>()))
			{
				if (bConveyor && Upgrade.Line.Num() + (Queue.Num() - Head) > MaxUpgradeLine)
				{
					Upgrade.bCapped = true;
					continue;
				}
				Queue.Add(Next);
				continue;
			}
			Ends.Add(Types[TypeFor(Next->GetClass())].Name.ToString());
		}
	}

	// Where it stops, as a player would say it.
	TArray<FString> Unique;
	for (const FString& End : Ends)
	{
		Unique.AddUnique(End);
	}
	if (Ends.Num() == 1 && LooseEnds == 0)
	{
		Upgrade.EndText = FString::Printf(TEXT("Ends at the %s."), *Ends[0]);
	}
	else if (Ends.IsEmpty())
	{
		Upgrade.EndText = LooseEnds > 0 ? TEXT("Ends loose.") : TEXT("Runs in a loop.");
	}
	else
	{
		TArray<FString> Shown;
		for (int32 Index = 0; Index < Unique.Num() && Index < 3; Index++)
		{
			Shown.Add(Unique[Index]);
		}
		Upgrade.EndText = FString::Printf(TEXT("Ends at %d places: %s%s."), Ends.Num() + LooseEnds, *FString::Join(Shown, TEXT(", ")),
			Unique.Num() > 3 ? TEXT(" and more") : TEXT(""));
	}

	// What each tier would do to it.
	AFGCharacterPlayer* Player = BeltPlayer();
	UFGInventoryComponent* Inventory = Player && Player->GetBuildGun() ? Player->GetBuildGun()->GetInventory() : nullptr;
	const bool bFree = Inventory && Inventory->GetNoBuildCost();
	Upgrade.Tiers = UnlockedTiers(GetWorld());
	for (FUpgradeTier& Tier : Upgrade.Tiers)
	{
		TMap<UClass*, int32> Net;
		for (const TWeakObjectPtr<AFGBuildable>& Weak : Upgrade.Line)
		{
			AFGBuildable* Building = Weak.Get();
			if (!Building)
			{
				continue;
			}
			const bool bLift = Building->IsA<AFGBuildableConveyorLift>();
			UClass* Target = bLift ? Tier.Lift : Tier.Belt;
			if (!Target)
			{
				Tier.LiftsStay += bLift ? 1 : 0;
				continue;
			}
			if (Building->GetClass() == Target)
			{
				continue;
			}
			Tier.Changes++;
			if (!bFree)
			{
				UpgradeNetCost(Building, bLift ? Tier.LiftRecipe : Tier.BeltRecipe, Net);
			}
		}
		Tier.CostText = Tier.Changes == 0 ? FString() : bFree ? FString(TEXT("Free, with no build cost on")) : DescribeNet(Net);
	}
	RefreshUpgradeRecords();
}

void UAtlasSubsystem::RefreshUpgradeRecords()
{
	UpgradeRecords.Reset();
	if (Upgrade.bOpen)
	{
		for (const TWeakObjectPtr<AFGBuildable>& Weak : Upgrade.Line)
		{
			FDismantleTarget Target;
			if (TargetForActor(Weak.Get(), Target))
			{
				for (const FVector4f& Record : Target.Records)
				{
					const FDecoded Decoded(Record);
					UpgradeRecords.Add(Encode(Decoded.X, Decoded.Y, Decoded.Z, Decoded.HalfX, Decoded.HalfY, Decoded.Angle, HighlightType, 0));
				}
			}
		}
	}
	UpgradeVersion++;
}

void UAtlasSubsystem::UpgradeNetCost(AFGBuildable* Old, UClass* Recipe, TMap<UClass*, int32>& InOutNet) const
{
	if (!IsValid(Old) || !Recipe)
	{
		return;
	}
	// What the old one gives back is the game's own figure, already scaled by its length. The new one is built to the
	// same length, so it costs its recipe by the same scale. The hologram's own cost can't be used: made up from the
	// old belt's place rather than dragged out, it prices every belt as one piece.
	TArray<FInventoryStack> Returns;
	Old->GetDismantleRefundReturns(Returns);
	for (const FInventoryStack& Stack : Returns)
	{
		if (UClass* Item = Stack.Item.GetItemClass())
		{
			InOutNet.FindOrAdd(Item) -= Stack.NumItems;
		}
	}
	const int32 Scale = FMath::Max(1, Old->GetDismantleRefundReturnsMultiplier());
	for (const FItemAmount& Item : UFGRecipe::GetIngredients(GetWorld(), TSubclassOf<UFGRecipe>(Recipe)))
	{
		if (Item.ItemClass && Item.Amount > 0)
		{
			InOutNet.FindOrAdd(Item.ItemClass.Get()) += Item.Amount * Scale;
		}
	}
}

void UAtlasSubsystem::StartUpgrade(int32 Tier)
{
	if (Upgrade.bRunning)
	{
		return;
	}
	if (!CanBuildBelts())
	{
		ShowToast(FText::FromString(TEXT("Belts can only be upgraded from the map in single player or as the host.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	// The world may have moved on since the panel traced it.
	TraceUpgrade();
	if (!Upgrade.Tiers.IsValidIndex(Tier) || Upgrade.Line.IsEmpty())
	{
		return;
	}
	const FUpgradeTier& Chosen = Upgrade.Tiers[Tier];
	if (Chosen.Changes == 0)
	{
		ShowToast(FText::FromString(Chosen.LiftsStay > 0
			? FString::Printf(TEXT("Nothing to change: the belts are already %s, and no lift of that speed is unlocked."), *Chosen.Name.ToString())
			: FString::Printf(TEXT("Everything on the line is already %s."), *Chosen.Name.ToString())), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	Upgrade.bRunning = true;
	Upgrade.Tier = Tier;
	Upgrade.Next = 0;
	Upgrade.Done = 0;
	Upgrade.Skipped = 0;
	Upgrade.FirstSkip = FText::GetEmpty();
	Upgrade.Overflow.Reset();
	UE_LOG(LogFactoryAtlas, Display, TEXT("Upgrade: %d belts and %d lifts to %s"), Upgrade.Belts, Upgrade.Lifts, *Chosen.Name.ToString());
	UpgradeTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) { return TickUpgrade(); }));
}

void UAtlasSubsystem::StopUpgradeTicker()
{
	if (UpgradeTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(UpgradeTicker);
		UpgradeTicker.Reset();
	}
}

bool UAtlasSubsystem::TickUpgrade()
{
	if (!Upgrade.bRunning || !Upgrade.Tiers.IsValidIndex(Upgrade.Tier))
	{
		UpgradeTicker.Reset();
		Upgrade.bRunning = false;
		return false;
	}
	// A copy: the tier list is traced again when the run ends, but not before.
	const FUpgradeTier Tier = Upgrade.Tiers[Upgrade.Tier];
	const double Start = FPlatformTime::Seconds();
	int32 Count = 0;
	while (Upgrade.Next < Upgrade.Line.Num() && Count < PerFrame && FPlatformTime::Seconds() - Start < FrameBudgetSeconds)
	{
		const int32 Index = Upgrade.Next++;
		AFGBuildable* Old = Upgrade.Line[Index].Get();
		if (!IsValid(Old))
		{
			continue;
		}
		const bool bWasSelected = !bSelectedLightweight && Selected.Get() == Old;
		FText Reason;
		AFGBuildable* New = nullptr;
		switch (UpgradeOne(Old, Tier, Reason, New))
		{
		case EUpgradeResult::Done:
			Count++;
			Upgrade.Done++;
			Upgrade.Line[Index] = New;
			if (bWasSelected)
			{
				// The panel stays on the line rather than closing with the belt it was opened on.
				Selected = New;
				RefreshCustomizeOptions();
				SelectionVersion++;
			}
			break;
		case EUpgradeResult::Unaffordable:
			FinishUpgrade(Reason);
			return false;
		case EUpgradeResult::Skipped:
		case EUpgradeResult::Failed:
			if (!Reason.IsEmpty())
			{
				Upgrade.Skipped++;
				if (Upgrade.FirstSkip.IsEmpty())
				{
					Upgrade.FirstSkip = Reason;
				}
				UE_LOG(LogFactoryAtlas, Display, TEXT("Upgrade: %s stayed: %s"), *Old->GetName(), *Reason.ToString());
			}
			break;
		}
	}
	if (Upgrade.Next >= Upgrade.Line.Num())
	{
		FinishUpgrade(FText::GetEmpty());
		return false;
	}
	return true;
}

void UAtlasSubsystem::FinishUpgrade(const FText& Stopped)
{
	// Only ever reached from the ticker, which is told to stop by its return value.
	UpgradeTicker.Reset();
	const FString TierName = Upgrade.Tiers.IsValidIndex(Upgrade.Tier) ? Upgrade.Tiers[Upgrade.Tier].Name.ToString() : FString();
	Upgrade.bRunning = false;

	// What came back but didn't fit is left in a crate by the player, as the build gun's refund is.
	TArray<FInventoryStack> Leftover;
	for (const TPair<UClass*, int32>& Pair : Upgrade.Overflow)
	{
		if (Pair.Key && Pair.Value > 0)
		{
			Leftover.Add(FInventoryStack(Pair.Value, TSubclassOf<UFGItemDescriptor>(Pair.Key)));
		}
	}
	Upgrade.Overflow.Reset();
	AFGCharacterPlayer* Player = BeltPlayer();
	if (!Leftover.IsEmpty() && Player && GetWorld())
	{
		FDismantleHelpers::DropRefundOnGroundNoActor(GetWorld(), Player->GetActorLocation(), nullptr, Leftover, Player);
	}

	UE_LOG(LogFactoryAtlas, Display, TEXT("Upgrade: %d replaced, %d stayed%s%s"), Upgrade.Done, Upgrade.Skipped,
		Stopped.IsEmpty() ? TEXT("") : TEXT(", stopped: "), *Stopped.ToString());
	FString Text = FString::Printf(TEXT("Upgraded %d to %s."), Upgrade.Done, *TierName);
	if (!Stopped.IsEmpty())
	{
		Text += FString::Printf(TEXT(" Stopped: %s"), *Stopped.ToString());
	}
	if (Upgrade.Skipped > 0)
	{
		Text += FString::Printf(TEXT(" %d stayed: %s"), Upgrade.Skipped, *Upgrade.FirstSkip.ToString());
	}
	if (!Leftover.IsEmpty())
	{
		Text += TEXT(" Some of what came back didn't fit and is in a crate next to you.");
	}
	const bool bNothing = Upgrade.Done == 0;
	ShowToast(FText::FromString(Text), bNothing || !Stopped.IsEmpty());
	PlayClipboardSound(bNothing ? EClipboardSound::Error : EClipboardSound::Paste);
	bPortsDirty = true;

	// The panel shows the line as it is now.
	if (Upgrade.bOpen && IsSelectedConveyor())
	{
		TraceUpgrade();
	}
	else
	{
		Upgrade.bOpen = false;
		Upgrade.Line.Reset();
		Upgrade.Tiers.Reset();
		RefreshUpgradeRecords();
	}
}

EUpgradeResult UAtlasSubsystem::UpgradeOne(AFGBuildable* Old, const FUpgradeTier& Tier, FText& OutReason, AFGBuildable*& OutNew)
{
	OutNew = nullptr;
	AFGBuildableConveyorBase* Conveyor = Cast<AFGBuildableConveyorBase>(Old);
	if (!IsValid(Conveyor) || Conveyor->IsActorBeingDestroyed())
	{
		return EUpgradeResult::Skipped;
	}
	AFGBuildableConveyorLift* OldLift = Cast<AFGBuildableConveyorLift>(Old);
	UClass* Recipe = OldLift ? Tier.LiftRecipe : Tier.BeltRecipe;
	UClass* Target = OldLift ? Tier.Lift : Tier.Belt;
	if (!Recipe || !Target)
	{
		OutReason = FText::FromString(FString::Printf(TEXT("no lift as fast as %s is unlocked."), *Tier.Name.ToString()));
		return EUpgradeResult::Skipped;
	}
	if (Old->GetClass() == Target)
	{
		return EUpgradeResult::Skipped; // Already this tier, which is no reason to mention
	}
	if (Conveyor->HasAnyThroughputMonitorsAttached())
	{
		// The game's upgrade has the monitor to carry over too, which a replacement made from the map doesn't do.
		OutReason = FText::FromString(TEXT("belts with a throughput monitor are left for the build gun."));
		return EUpgradeResult::Skipped;
	}

	AFGCharacterPlayer* Player = BeltPlayer();
	AFGBuildGun* BuildGun = Player ? Player->GetBuildGun() : nullptr;
	UFGInventoryComponent* Inventory = BuildGun ? BuildGun->GetInventory() : nullptr;
	AFGBuildableSubsystem* Buildables = AFGBuildableSubsystem::Get(GetWorld());
	if (!Inventory || !Buildables)
	{
		OutReason = FText::FromString(TEXT("can't build right now."));
		return EUpgradeResult::Failed;
	}

	// Checked before anything is touched, so running short stops the line cleanly.
	const bool bFree = Inventory->GetNoBuildCost();
	TMap<UClass*, int32> Net;
	if (!bFree)
	{
		UpgradeNetCost(Old, Recipe, Net);
		AFGCentralStorageSubsystem* Depot = AFGCentralStorageSubsystem::Get(GetWorld());
		for (const TPair<UClass*, int32>& Pair : Net)
		{
			if (Pair.Value <= 0)
			{
				continue;
			}
			const TSubclassOf<UFGItemDescriptor> Item(Pair.Key);
			const int32 Have = Inventory->GetNumItems(Item) + (Depot ? Depot->GetNumItemsFromCentralStorage(Item) : 0);
			if (Have < Pair.Value)
			{
				OutReason = FText::FromString(FString::Printf(TEXT("ran out of %s."), *ItemName(Pair.Key)));
				return EUpgradeResult::Unaffordable;
			}
		}
	}

	// The game's upgrade hologram, aimed at the old one as the build gun would be.
	AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(TSubclassOf<UFGRecipe>(Recipe), BuildGun, Old->GetActorLocation(), Player);
	if (!Hologram)
	{
		OutReason = FText::FromString(TEXT("the game made no hologram for it."));
		return EUpgradeResult::Failed;
	}
	const auto Fail = [&Hologram, &OutReason](const FText& Reason)
	{
		if (IsValid(Hologram))
		{
			Hologram->Destroy();
		}
		OutReason = Reason;
		return EUpgradeResult::Failed;
	};
	Hologram->SetInsideBlueprintDesigner(Old->GetBlueprintDesigner());
	if (OldLift && !CopyLiftShape(OldLift, Hologram))
	{
		return Fail(FText::FromString(TEXT("the game's lift hologram isn't one Atlas knows how to fill in.")));
	}
	UPrimitiveComponent* Component = Old->FindComponentByClass<UPrimitiveComponent>();
	if (!Component)
	{
		return Fail(FText::FromString(TEXT("it has nothing to aim at.")));
	}
	FHitResult Hit(Old, Component, Old->GetActorLocation(), Old->GetActorRotation().Vector());
	Hit.bBlockingHit = true;

	// As the build gun's frame does: placement, which is where the hologram takes the old one as its upgrade target,
	// then the steps a click would take, then validation.
	AFGConveyorBeltHologram* UpgradeHologram = Cast<AFGConveyorBeltHologram>(Hologram);
	Hologram->ResetConstructDisqualifiers();
	Hologram->UpdateHologramPlacement(Hit);
	if (Hologram->GetUpgradedActor() != Old)
	{
		return Fail(FText::FromString(TEXT("the game wouldn't take it as an upgrade.")));
	}
	if (UpgradeHologram)
	{
		UpgradeHologram->GenerateAndUpdateSpline(Hit);
	}
	// What a hologram sent to the server goes through, which works out what the replacement connects to.
	Hologram->PreConstructMessageSerialization();
	Hologram->PostConstructMessageDeserialization();
	for (int32 Step = 0; Step < 8 && Hologram->CanTakeNextBuildStep(); Step++)
	{
		if (Hologram->DoMultiStepPlacement(true))
		{
			break;
		}
	}
	if (UpgradeHologram)
	{
		UpgradeHologram->GenerateAndUpdateSpline(Hit);
	}
	if (OldLift && !LiftShapeKept(OldLift, Hologram))
	{
		return Fail(FText::FromString(TEXT("the replacement lift came out a different height.")));
	}
	Hologram->ValidatePlacementAndCost(Inventory);
	if (Hologram->GetUpgradedActor() != Old || Hologram->GetBuildClass() != Target)
	{
		return Fail(FText::FromString(TEXT("the game wouldn't take it as an upgrade.")));
	}
	if (!Hologram->CanConstruct())
	{
		TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
		Hologram->GetConstructDisqualifiers(Disqualifiers);
		for (AFGHologram* Child : Hologram->GetHologramChildren())
		{
			if (Child)
			{
				TArray<TSubclassOf<UFGConstructDisqualifier>> ChildDisqualifiers;
				Child->GetConstructDisqualifiers(ChildDisqualifiers);
				Disqualifiers.Append(ChildDisqualifiers);
			}
		}
		for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
		{
			if (!IsIgnoredForUpgrade(Disqualifier))
			{
				return Fail(UFGConstructDisqualifier::GetDisqualifyingText(Disqualifier));
			}
		}
		Hologram->ResetConstructDisqualifiers();
		for (AFGHologram* Child : Hologram->GetHologramChildren())
		{
			if (Child)
			{
				Child->ResetConstructDisqualifiers();
			}
		}
	}

	// Paid before building, and given back if the game builds nothing.
	TArray<FItemAmount> Cost;
	for (const TPair<UClass*, int32>& Pair : Net)
	{
		if (Pair.Value > 0)
		{
			Cost.Add(FItemAmount(TSubclassOf<UFGItemDescriptor>(Pair.Key), Pair.Value));
		}
	}
	if (!PayBuildCost(Cost))
	{
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Upgrade: couldn't take the whole cost of replacing %s"), *Old->GetName());
	}
	const auto GiveBack = [this, Inventory](UClass* Item, int32 Amount)
	{
		const int32 Added = Inventory->AddStack(FInventoryStack(Amount, TSubclassOf<UFGItemDescriptor>(Item)), true);
		if (Added < Amount)
		{
			Upgrade.Overflow.FindOrAdd(Item) += Amount - Added;
		}
	};

	AFGBlueprintProxy* Group = Old->GetBlueprintProxy();
	IFGDismantleInterface::Execute_PreUpgrade(Old);
	TArray<AActor*> Children;
	AActor* Built = Hologram->Construct(Children, Buildables->GetNewNetConstructionID());
	Hologram->Destroy();
	AFGBuildable* New = Cast<AFGBuildable>(Built);
	if (!New)
	{
		for (const FItemAmount& Item : Cost)
		{
			GiveBack(Item.ItemClass.Get(), Item.Amount);
		}
		OutReason = FText::FromString(TEXT("the game built nothing."));
		UE_LOG(LogFactoryAtlas, Warning, TEXT("Upgrade: Construct returned nothing for %s"), *Old->GetName());
		return EUpgradeResult::Failed;
	}

	// The old one hands its items and connections over, then goes.
	IFGDismantleInterface::Execute_Upgrade(Old, New);
	if (IsValid(Old) && !Old->IsActorBeingDestroyed())
	{
		Old->Destroy();
	}
	// A belt from a blueprint stays in its group, so the game's blueprint dismantle still takes the whole thing.
	if (IsValid(Group) && !New->GetBlueprintProxy() && !New->GetBlueprintDesigner())
	{
		New->SetBlueprintProxy(Group);
		Group->RegisterBuildable(New);
	}
	for (const TPair<UClass*, int32>& Pair : Net)
	{
		if (Pair.Value < 0)
		{
			GiveBack(Pair.Key, -Pair.Value);
		}
	}
	OutNew = New;
	return EUpgradeResult::Done;
}
