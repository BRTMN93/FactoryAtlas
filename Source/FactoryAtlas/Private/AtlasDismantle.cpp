// Dismantling from the map: in dismantle mode, click buildings or drag a box over them to pick them, then Enter
// dismantles what is picked. Everything goes through the game's own dismantle, so a building that refuses (a
// disqualifier, or one that can't be dismantled at all) stays, and the refund is the build gun's: into the inventory,
// with anything that doesn't fit left in a crate by the player. Single player and host only, like building.
// Recipe mode (Alt+R) shares the picking, machines only; the recipe change itself is in AtlasSubsystem.cpp.

#include "AtlasSubsystem.h"

#include "AtlasTypes.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Engine/World.h"
#include "Equipment/FGBuildGun.h"
#include "FactoryAtlas.h"
#include "FGCharacterPlayer.h"
#include "FGBlueprintProxy.h"
#include "FGConstructDisqualifier.h"
#include "FGDismantleInterface.h"
#include "FGInventoryComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "ItemAmount.h"
#include "Resources/FGItemDescriptor.h"

using namespace Atlas;

namespace AtlasDismantleDetail
{
	/** One building is the same as another when it is the same actor, or the same instance of the same class. */
	bool SameTarget(const FDismantleTarget& A, const FDismantleTarget& B)
	{
		if (A.bLightweight != B.bLightweight)
		{
			return false;
		}
		return A.bLightweight ? (A.Class == B.Class && A.Index == B.Index) : A.Actor == B.Actor;
	}

	uint64 TargetKey(const FDismantleTarget& Target)
	{
		return Target.bLightweight ? (uint64(reinterpret_cast<UPTRINT>(Target.Class)) ^ (uint64(uint32(Target.Index)) << 1) ^ 1ull)
			: uint64(reinterpret_cast<UPTRINT>(Target.Actor.Get()));
	}

	/** What a foundation or wall gives back: what it cost, unless building is free. */
	void LightweightRefund(UWorld* World, const FLightweightBuildableInstanceRef& Instance, bool bNoBuildCost, TArray<FInventoryStack>& Out)
	{
		const TSubclassOf<UFGRecipe> Recipe = Instance.GetBuiltWithRecipe();
		if (bNoBuildCost || !Recipe)
		{
			return;
		}
		for (const FItemAmount& Item : UFGRecipe::GetIngredients(World, Recipe))
		{
			if (Item.ItemClass && Item.Amount > 0)
			{
				Out.Add(FInventoryStack(Item.Amount, Item.ItemClass));
			}
		}
	}

	/** "120 Concrete, 40 Iron Plate and 3 more", largest first. */
	FString DescribeItems(const TMap<UClass*, int32>& Totals)
	{
		TArray<TPair<UClass*, int32>> Sorted;
		for (const TPair<UClass*, int32>& Pair : Totals)
		{
			Sorted.Add(Pair);
		}
		Sorted.Sort([](const TPair<UClass*, int32>& A, const TPair<UClass*, int32>& B) { return A.Value > B.Value; });
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Sorted.Num() && Index < 3; Index++)
		{
			Parts.Add(FString::Printf(TEXT("%d %s"), Sorted[Index].Value,
				*UFGItemDescriptor::GetItemName(TSubclassOf<UFGItemDescriptor>(Sorted[Index].Key)).ToString()));
		}
		FString Text = FString::Join(Parts, TEXT(", "));
		if (Sorted.Num() > 3)
		{
			Text += FString::Printf(TEXT(" and %d more"), Sorted.Num() - 3);
		}
		return Text;
	}
}
using namespace AtlasDismantleDetail;

void UAtlasSubsystem::SetPicking(EView View, EPickPurpose Purpose, bool bOn)
{
	if (!bOn)
	{
		if (!Dismantle.bMode || Dismantle.View != View)
		{
			return;
		}
		StopDismantleTicker();
		Dismantle = FDismantleTool();
		RefreshDismantle();
		return;
	}
	if (!CanBuildBelts())
	{
		ShowToast(FText::FromString(Purpose == EPickPurpose::Recipe
			? TEXT("Recipes can only be changed from the map in single player or as the host.")
			: Purpose == EPickPurpose::Power
			? TEXT("Power lines can only be built from the map in single player or as the host.")
			: TEXT("Buildings can only be dismantled from the map in single player or as the host.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	if (Dismantle.bMode)
	{
		SetPicking(Dismantle.View, Dismantle.Purpose, false);
	}
	SetWiring(Wire.View, false);
	SetMeasuring(View, false);
	SetPlacing(Place.View, false);
	CancelBeltDrag();
	Dismantle = FDismantleTool();
	Dismantle.bMode = true;
	Dismantle.Purpose = Purpose;
	Dismantle.View = View;
	RefreshDismantle();
}

bool UAtlasSubsystem::PressDismantle(EView View, bool bRemove)
{
	if (!IsPicking(View))
	{
		return false;
	}
	Dismantle.bPressed = true;
	Dismantle.bBoxing = false;
	Dismantle.bRemove = bRemove;
	Dismantle.PressMetres = CursorMetres(View);
	return true;
}

void UAtlasSubsystem::MoveDismantle(EView View)
{
	if (!IsPicking(View) || !Dismantle.bPressed)
	{
		return;
	}
	Dismantle.bBoxing = true;
	// The box shows what it would pick while it is dragged, not only once it is let go.
	if (!DismantleTicker.IsValid())
	{
		DismantlePreviewNext = 0.0;
		DismantleTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float) { return TickDismantle(); }));
	}
}

bool UAtlasSubsystem::TickDismantle()
{
	if (!Dismantle.bMode || !Dismantle.bBoxing)
	{
		DismantleTicker.Reset();
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	const FVector2f End = CursorMetres(Dismantle.View);
	if (Now < DismantlePreviewNext || (Dismantle.bPreview && End == DismantlePreviewEnd))
	{
		return true;
	}
	DismantlePreviewEnd = End;
	Dismantle.Preview = Dismantle.Targets;
	ApplyDismantleBox(Dismantle.Preview, End, Dismantle.bRemove, Dismantle.bPreviewCapped);
	Dismantle.bPreview = true;
	RefreshDismantle();
	// A big save takes a while to sweep; leave the game most of the frame time between sweeps.
	const double Took = FPlatformTime::Seconds() - Now;
	DismantlePreviewNext = Now + FMath::Max(1.0 / 30.0, Took * 3.0);
	return true;
}

void UAtlasSubsystem::StopDismantleTicker()
{
	if (DismantleTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DismantleTicker);
		DismantleTicker.Reset();
	}
}

void UAtlasSubsystem::CancelDismantleBox()
{
	StopDismantleTicker();
	const bool bHadPreview = Dismantle.bPreview;
	Dismantle.bPressed = false;
	Dismantle.bBoxing = false;
	Dismantle.bPreview = false;
	Dismantle.bPreviewCapped = false;
	Dismantle.Preview.Reset();
	if (bHadPreview)
	{
		RefreshDismantle(); // Back to the pick as it was
	}
}

void UAtlasSubsystem::ReleaseDismantle(EView View)
{
	if (!IsPicking(View) || !Dismantle.bPressed)
	{
		CancelDismantleBox();
		return;
	}
	const bool bBox = Dismantle.bBoxing;
	const bool bRemove = Dismantle.bRemove;
	StopDismantleTicker();
	Dismantle.bPressed = false;
	Dismantle.bBoxing = false;
	Dismantle.bPreview = false;
	Dismantle.bPreviewCapped = false;
	Dismantle.Preview.Reset();

	if (!bBox)
	{
		// A click picks the building under the cursor, or unpicks it when it was picked.
		FDismantleTarget Target;
		if (!FindHoveredTarget(Target))
		{
			return;
		}
		if (!PickAccepts(Target))
		{
			ShowToast(FText::FromString(TEXT("Only machines that run a recipe can be picked here.")), true);
			PlayClipboardSound(EClipboardSound::Error);
			return;
		}
		const int32 Found = Dismantle.Targets.IndexOfByPredicate([&Target](const FDismantleTarget& Other) { return SameTarget(Other, Target); });

		// A building from a blueprint, or a row built from the map, brings its whole group, as the game's blueprint
		// dismantle does.
		AFGBlueprintProxy* Group = nullptr;
		if (Target.bLightweight)
		{
			const FRuntimeBuildableInstanceData* Data = Target.Lightweight.ResolveBuildableInstanceData();
			Group = Data ? Data->BlueprintProxy.Get() : nullptr;
		}
		else if (AFGBuildable* Building = Target.Actor.Get())
		{
			Group = Building->GetBlueprintProxy();
		}
		if (IsValid(Group))
		{
			TArray<FDismantleTarget> Members;
			for (AFGBuildable* Member : Group->GetBuildables())
			{
				FDismantleTarget One;
				if (TargetForActor(Member, One) && PickAccepts(One))
				{
					Members.Add(MoveTemp(One));
				}
			}
			for (const FBuildableClassLightweightIndices& Entry : Group->GetLightweightClassAndIndices())
			{
				for (const int32 Index : Entry.Indices)
				{
					FDismantleTarget One;
					if (TargetForInstance(Entry.BuildableClass.Get(), Index, One) && PickAccepts(One))
					{
						Members.Add(MoveTemp(One));
					}
				}
			}
			TSet<uint64> Keys;
			for (const FDismantleTarget& Member : Members)
			{
				Keys.Add(TargetKey(Member));
			}
			if (Found != INDEX_NONE)
			{
				Dismantle.Targets.RemoveAll([&Keys](const FDismantleTarget& Other) { return Keys.Contains(TargetKey(Other)); });
			}
			else
			{
				TSet<uint64> Have;
				for (const FDismantleTarget& Other : Dismantle.Targets)
				{
					Have.Add(TargetKey(Other));
				}
				int32 Added = 0;
				for (FDismantleTarget& Member : Members)
				{
					if (Dismantle.Targets.Num() >= MaxDismantleTargets)
					{
						Dismantle.bCapped = true;
						break;
					}
					if (!Have.Contains(TargetKey(Member)))
					{
						Dismantle.Targets.Add(MoveTemp(Member));
						Added++;
					}
				}
				ShowToast(FText::FromString(FString::Printf(TEXT("Picked a group of %d."), Members.Num())));
			}
			RefreshDismantle();
			return;
		}
		if (Found != INDEX_NONE)
		{
			Dismantle.Targets.RemoveAt(Found);
		}
		else if (Dismantle.Targets.Num() < MaxDismantleTargets)
		{
			Dismantle.Targets.Add(MoveTemp(Target));
		}
		else
		{
			ShowToast(FText::FromString(FString::Printf(TEXT("A pick holds at most %d buildings."), MaxDismantleTargets)), true);
			PlayClipboardSound(EClipboardSound::Error);
			return;
		}
		RefreshDismantle();
		return;
	}

	ApplyDismantleBox(Dismantle.Targets, CursorMetres(View), bRemove, Dismantle.bCapped);
	if (!bRemove && Dismantle.bCapped)
	{
		ShowToast(FText::FromString(FString::Printf(TEXT("A pick holds at most %d buildings; the box had more."), MaxDismantleTargets)), true);
	}
	RefreshDismantle();
}

void UAtlasSubsystem::ApplyDismantleBox(TArray<FDismantleTarget>& Into, const FVector2f& End, bool bRemove, bool& bOutCapped)
{
	const FVector2f Min(FMath::Min(Dismantle.PressMetres.X, End.X), FMath::Min(Dismantle.PressMetres.Y, End.Y));
	const FVector2f Max(FMath::Max(Dismantle.PressMetres.X, End.X), FMath::Max(Dismantle.PressMetres.Y, End.Y));
	TArray<FDismantleTarget> Found;
	bool bCapped = false;
	if (bRemove)
	{
		// Every building in the box comes out of the pick, however many there are.
		CollectTargetsInBox(Min, Max, Found, MAX_int32, bCapped, Dismantle.Purpose);
		TSet<uint64> Out;
		for (const FDismantleTarget& Target : Found)
		{
			Out.Add(TargetKey(Target));
		}
		Into.RemoveAll([&Out](const FDismantleTarget& Target) { return Out.Contains(TargetKey(Target)); });
		bOutCapped = false;
		return;
	}
	CollectTargetsInBox(Min, Max, Found, MaxDismantleTargets - Into.Num(), bCapped, Dismantle.Purpose);
	TSet<uint64> Have;
	for (const FDismantleTarget& Target : Into)
	{
		Have.Add(TargetKey(Target));
	}
	for (FDismantleTarget& Target : Found)
	{
		if (!Have.Contains(TargetKey(Target)))
		{
			Have.Add(TargetKey(Target));
			Into.Add(MoveTemp(Target));
		}
	}
	bOutCapped = bCapped;
}

bool UAtlasSubsystem::ClearDismantle()
{
	if (Dismantle.Targets.IsEmpty())
	{
		return false;
	}
	Dismantle.Targets.Reset();
	Dismantle.bCapped = false;
	RefreshDismantle();
	return true;
}

bool UAtlasSubsystem::PickAccepts(const FDismantleTarget& Target) const
{
	if (Dismantle.Purpose == EPickPurpose::Dismantle)
	{
		return true;
	}
	const AFGBuildable* Building = Target.bLightweight ? nullptr : Target.Actor.Get();
	if (Dismantle.Purpose == EPickPurpose::Power)
	{
		return IsWirable(Building);
	}
	return IsValid(Building) && Building->IsA<AFGBuildableManufacturer>();
}

void UAtlasSubsystem::RefreshDismantle()
{
	// The recipe list and the power plan follow the pick itself, not a box still being dragged.
	const bool bRecipe = Dismantle.Purpose == EPickPurpose::Recipe;
	const bool bPower = Dismantle.Purpose == EPickPurpose::Power;
	if (!Dismantle.bPreview)
	{
		GroupRecipes();
		RecipeGroupsVersion++;
		PlanPower();
	}

	// The picked shapes on the map: red to dismantle, the accent colour for a recipe change or a connection. A
	// building connect mode can't reach is red.
	const TArray<FDismantleTarget>& Shown = Dismantle.Shown();
	const TArray<bool>& Stranded = Dismantle.Power.Stranded;
	DismantleRecords.Reset();
	for (int32 Index = 0; Index < Shown.Num(); Index++)
	{
		const bool bRed = bPower ? (!Dismantle.bPreview && Stranded.IsValidIndex(Index) && Stranded[Index]) : !bRecipe;
		for (const FVector4f& Record : Shown[Index].Records)
		{
			const FDecoded Decoded(Record);
			DismantleRecords.Add(Encode(Decoded.X, Decoded.Y, Decoded.Z, Decoded.HalfX, Decoded.HalfY, Decoded.Angle, bRed ? BlockedType : HighlightType, 0));
		}
	}
	DismantleVersion++;

	// What dismantling it all would give back, as the game works it out.
	Dismantle.RefundText.Reset();
	Dismantle.CountLines.Reset();
	if (Dismantle.Shown().IsEmpty())
	{
		return;
	}
	if (bPower)
	{
		return; // The card shows the plan instead
	}
	CountDismantle();
	if (bRecipe)
	{
		return;
	}
	AFGCharacterPlayer* Player = BeltPlayer();
	UFGInventoryComponent* Inventory = Player && Player->GetBuildGun() ? Player->GetBuildGun()->GetInventory() : nullptr;
	const bool bNoBuildCost = Inventory && Inventory->GetNoBuildCost();
	TMap<UClass*, int32> Totals;
	TArray<FInventoryStack> Refund;
	for (const FDismantleTarget& Target : Dismantle.Shown())
	{
		Refund.Reset();
		if (Target.bLightweight)
		{
			LightweightRefund(GetWorld(), Target.Lightweight, bNoBuildCost, Refund);
		}
		else if (AFGBuildable* Building = Target.Actor.Get())
		{
			IFGDismantleInterface::Execute_GetDismantleRefund(Building, Refund, bNoBuildCost);
		}
		for (const FInventoryStack& Stack : Refund)
		{
			if (UClass* Item = Stack.Item.GetItemClass())
			{
				Totals.FindOrAdd(Item) += Stack.NumItems;
			}
		}
	}
	Dismantle.RefundText = DescribeItems(Totals);
}

void UAtlasSubsystem::CountDismantle()
{
	// One row per building, and per recipe for a machine, so a box over a factory reads like its plan:
	// "26  Constructor: Wire". Machines first, then logistics, then everything else, most first within each.
	struct FRow
	{
		int32 TypeId = 0;
		FString Text;
		int32 Count = 0;
	};
	TMap<FString, FRow> Rows;
	for (const FDismantleTarget& Target : Dismantle.Shown())
	{
		UClass* Class = Target.Class;
		AFGBuildable* Building = Target.bLightweight ? nullptr : Target.Actor.Get();
		if (!Class || (!Target.bLightweight && !IsValid(Building)))
		{
			continue;
		}
		const int32 TypeId = TypeFor(Class);
		FString Text = Types[TypeId].Name.ToString();
		if (const AFGBuildableManufacturer* Machine = Cast<AFGBuildableManufacturer>(Building))
		{
			const TSubclassOf<UFGRecipe> Recipe = Machine->GetCurrentRecipe();
			Text += Recipe ? FString::Printf(TEXT(": %s"), *UFGRecipe::GetRecipeName(Recipe).ToString()) : FString(TEXT(": no recipe"));
		}
		FRow& Row = Rows.FindOrAdd(Text);
		Row.TypeId = TypeId;
		Row.Text = MoveTemp(Text);
		Row.Count++;
	}

	const auto Rank = [](ECategory Category)
	{
		switch (Category)
		{
		case ECategory::Production: return 0;
		case ECategory::Extraction: return 1;
		case ECategory::Generators: return 2;
		case ECategory::Storage: return 3;
		case ECategory::Stations: return 4;
		case ECategory::Logistics: return 5;
		default: return 6;
		}
	};
	TArray<FRow> Sorted;
	Rows.GenerateValueArray(Sorted);
	Sorted.Sort([this, &Rank](const FRow& A, const FRow& B)
	{
		const int32 RankA = Rank(Types[A.TypeId].Category);
		const int32 RankB = Rank(Types[B.TypeId].Category);
		if (RankA != RankB)
		{
			return RankA < RankB;
		}
		return A.Count != B.Count ? A.Count > B.Count : A.Text < B.Text;
	});

	static constexpr int32 MaxRows = 8;
	for (int32 Index = 0; Index < Sorted.Num() && Index < MaxRows; Index++)
	{
		const FRow& Row = Sorted[Index];
		Dismantle.CountLines.Add({ FString::Printf(TEXT("%d  %s"), Row.Count, *Row.Text), FLinearColor(CategoryColor(Types[Row.TypeId].Category)) });
	}
	if (Sorted.Num() > MaxRows)
	{
		int32 Rest = 0;
		for (int32 Index = MaxRows; Index < Sorted.Num(); Index++)
		{
			Rest += Sorted[Index].Count;
		}
		Dismantle.CountLines.Add({ FString::Printf(TEXT("%d  in %d more kinds"), Rest, Sorted.Num() - MaxRows), FLinearColor(FColor(170, 170, 170)) });
	}
}

void UAtlasSubsystem::DismantlePicked()
{
	if (!Dismantle.bMode || Dismantle.Purpose != EPickPurpose::Dismantle)
	{
		return;
	}
	if (Dismantle.Targets.IsEmpty())
	{
		ShowToast(FText::FromString(TEXT("Nothing picked. Click buildings or drag a box over them first.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	AFGCharacterPlayer* Player = BeltPlayer();
	UFGInventoryComponent* Inventory = Player && Player->GetBuildGun() ? Player->GetBuildGun()->GetInventory() : nullptr;
	UWorld* World = GetWorld();
	if (!CanBuildBelts() || !Inventory || !World)
	{
		ShowToast(FText::FromString(TEXT("Buildings can only be dismantled from the map in single player or as the host.")), true);
		PlayClipboardSound(EClipboardSound::Error);
		return;
	}
	const bool bNoBuildCost = Inventory->GetNoBuildCost();

	// The game asks some buildings whether they may go, given everything else going with them.
	TArray<AActor*> Everything;
	for (const FDismantleTarget& Target : Dismantle.Targets)
	{
		if (AFGBuildable* Building = Target.Actor.Get())
		{
			Everything.Add(Building);
		}
	}

	TArray<FInventoryStack> Refunds;
	int32 Done = 0;
	int32 Refused = 0;
	FText FirstReason;
	const auto Refuse = [&Refused, &FirstReason](const FText& Reason)
	{
		Refused++;
		if (FirstReason.IsEmpty())
		{
			FirstReason = Reason;
		}
	};
	for (const FDismantleTarget& Target : Dismantle.Targets)
	{
		if (Target.bLightweight)
		{
			if (!Target.Lightweight.IsValid())
			{
				continue; // Already gone
			}
			TArray<FInventoryStack> Refund;
			LightweightRefund(World, Target.Lightweight, bNoBuildCost, Refund);
			FLightweightBuildableInstanceRef Instance = Target.Lightweight;
			if (Instance.Remove())
			{
				Refunds.Append(Refund);
				Done++;
			}
			else
			{
				Refuse(FText::FromString(TEXT("The game wouldn't remove it.")));
			}
			continue;
		}
		AFGBuildable* Building = Target.Actor.Get();
		if (!IsValid(Building))
		{
			continue;
		}
		if (!IFGDismantleInterface::Execute_CanDismantle(Building))
		{
			Refuse(FText::Format(FText::FromString(TEXT("{0} can't be dismantled.")), Types[TypeFor(Building->GetClass())].Name));
			continue;
		}
		if (IFGDismantleInterface::Execute_SupportsDismantleDisqualifiers(Building))
		{
			TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
			IFGDismantleInterface::Execute_GetDismantleDisqualifiers(Building, Disqualifiers, Everything);
			if (!Disqualifiers.IsEmpty())
			{
				Refuse(UFGConstructDisqualifier::GetDisqualifyingText(Disqualifiers[0]));
				continue;
			}
		}
		// Whatever comes apart with it, as the build gun takes it.
		TArray<AActor*> Children;
		IFGDismantleInterface::Execute_GetChildDismantleActors(Building, Children);
		for (AActor* Child : Children)
		{
			if (IsValid(Child) && Child->Implements<UFGDismantleInterface>() && IFGDismantleInterface::Execute_CanDismantle(Child))
			{
				IFGDismantleInterface::Execute_GetDismantleRefund(Child, Refunds, bNoBuildCost);
				IFGDismantleInterface::Execute_Dismantle(Child);
			}
		}
		IFGDismantleInterface::Execute_GetDismantleRefund(Building, Refunds, bNoBuildCost);
		IFGDismantleInterface::Execute_Dismantle(Building);
		Done++;
	}

	// Into the inventory, as the build gun's refund goes; what doesn't fit is left in a crate by the player.
	TArray<FInventoryStack> Leftover;
	for (const FInventoryStack& Stack : Refunds)
	{
		if (!Stack.HasItems())
		{
			continue;
		}
		const int32 Added = Inventory->AddStack(Stack, true);
		if (Added < Stack.NumItems)
		{
			Leftover.Add(FInventoryStack(Stack.NumItems - Added, Stack.Item.GetItemClass()));
		}
	}
	if (!Leftover.IsEmpty())
	{
		FDismantleHelpers::DropRefundOnGroundNoActor(World, Player->GetActorLocation(), nullptr, Leftover, Player);
	}

	UE_LOG(LogFactoryAtlas, Display, TEXT("Dismantle: %d dismantled, %d refused%s"), Done, Refused, Leftover.IsEmpty() ? TEXT("") : TEXT(", refund overflow in a crate"));
	FString Text = FString::Printf(TEXT("Dismantled %d."), Done);
	if (Refused > 0)
	{
		Text += FString::Printf(TEXT(" %d stayed: %s"), Refused, *FirstReason.ToString());
	}
	if (!Leftover.IsEmpty())
	{
		Text += TEXT(" Some of the refund didn't fit and is in a crate next to you.");
	}
	ShowToast(FText::FromString(Text), Done == 0);
	PlayClipboardSound(Done == 0 ? EClipboardSound::Error : EClipboardSound::Paste);
	Dismantle.Targets.Reset();
	Dismantle.bCapped = false;
	RefreshDismantle();
	bPortsDirty = true;
	ClearSelection();
}
