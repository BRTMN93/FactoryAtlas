#pragma once

#include "CoreMinimal.h"
#include "AtlasTypes.h"
#include "FGFactoryColoringTypes.h"
#include "ItemAmount.h"
#include "FGLightweightBuildableSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "Containers/Ticker.h"
#include "Tasks/Task.h"
#include "AtlasSubsystem.generated.h"

class AFGBuildable;
class UFGFactoryConnectionComponent;
class UFGConnectionComponent;
class UFGCircuitConnectionComponent;
class AFGBuildableManufacturer;
class AFGGameState;
class UFGBlueprintDescriptor;
class UTexture2D;
class USoundBase;
struct FFactoryCustomizationData;
struct FRuntimeBuildableInstanceData;

namespace Atlas
{
	/** The sounds Ctrl-clicks on the map make. */
	enum class EClipboardSound : uint8 { Copy, Paste, Error, Count };

	/** SplineComponent is for paths built from a plain spline component, such as truck paths. */
	enum class EKind : uint8 { Box, Spline, SplineComponent, Wire, Beam, Skip };

	struct FType
	{
		UClass* Class = nullptr;
		FText Name;
		ECategory Category = ECategory::Other;
		EKind Kind = EKind::Box;
		float HalfWidthCm = 0.f; // Splines, wires and beams
		FVector Center = FVector::ZeroVector;
		FVector2f Half = FVector2f(100.f, 100.f);
		float HalfZCm = 0.f; // Half the box's height; records keep its centre, so a floor's top is centre plus this
		FColor Color = FColor::White;
		int32 Count = 0;
		bool bHidden = false;
	};

	struct FGrid
	{
		TArray<FVector4f> Cells[GridCells];
		uint32 Versions[GridCells] = {};
		int32 ZLow = 0;
		int32 ZHigh = 0;
		int64 Total = 0;
	};

	struct FOp
	{
		FVector4f Record;
		bool bAdd;
	};

	struct FHover
	{
		bool bValid = false;
		int32 Type = 0;
		float HeightMetres = 0.f;
		FVector2f WorldMetres = FVector2f::ZeroVector;
		FVector4f Record = FVector4f::Zero();
		FVector4f Source = FVector4f::Zero(); // The building's own record, before it was recoloured for the highlight
		bool bMarker = false; // A collectible rather than a building, so nothing gets highlighted
		int32 Flags = 0;      // Marker flags, see AtlasTypes.h
	};

	/** What runs between two ports. Each kind only connects to its own. */
	enum class EPortKind : uint8
	{
		Belt,
		Pipe,
		Hypertube,
		Rail,
		Count
	};

	/** A free port of a building on the map: where a belt, pipe, hypertube or track can start or end. */
	struct FPort
	{
		TWeakObjectPtr<UFGConnectionComponent> Connection;
		TWeakObjectPtr<AFGBuildable> Owner;
		FVector Location = FVector::ZeroVector;
		FVector Normal = FVector::ForwardVector;
		FVector2f Uv = FVector2f::ZeroVector;
		EPortKind Kind = EPortKind::Belt;
		bool bOutput = false;
		bool bAnyFlow = false; // Connects to anything of its kind: pipe ports without a set direction, hypertubes, tracks
	};

	/** A belt being dragged from one port to another on the map, and what the game's hologram says about it. */
	struct FBeltDrag
	{
		bool bPressed = false;  // The button went down on a port; not yet a drag until it moves
		bool bActive = false;   // Moving: the hologram is out and the preview shows
		EView View = EView::Game;
		FPort Start;
		FVector2f PressScreen = FVector2f::ZeroVector;
		bool bHasTarget = false;
		FPort Target;
		bool bReady = false;    // The hologram has settled on the current target, so Valid and Reason can be trusted
		bool bValid = false;
		FText Reason;
		TArray<FVector> Points; // The hologram's spline, in world space, for the preview
		FText BeltName;
		FText ModeName;
		bool bHasGround = false; // No port under the cursor, but ground the line can end on
		bool bClipping = false; // Valid only because clipping is allowed: clearance overlaps are dropped before building
	};

	/** What the configure panel shows for the selected building. Read through the game's own UFUNCTIONs. */
	struct FFactoryState
	{
		bool bValid = false;
		bool bAuthority = false;
		bool bFactory = false; // A machine, so the clock, pause and recipe lines apply
		bool bCanChangeClock = false;
		bool bCanBoost = false;
		bool bPaused = false;
		float Current = 1.f;
		float Pending = 1.f;
		float Min = 0.01f;
		float Max = 1.f;
		float Boost = 1.f;
		float MaxBoost = 1.f;
		float Productivity = 0.f;
		int32 Shards = 0;
		FText Name;

		bool bHasRecipes = false; // A manufacturer, so it runs a recipe the player can pick
		UClass* Recipe = nullptr;
		FText RecipeName;
		FString RecipeSummary;
	};

	struct FRecipeOption
	{
		UClass* Recipe = nullptr;
		FText Name;
		FString Summary;
	};

	/** One color, pattern or material the selected building can take. */
	struct FCustomizeOption
	{
		UClass* Desc = nullptr; // nullptr for "no pattern"
		FText Name;
		FLinearColor Primary = FLinearColor::White;   // Swatches only
		FLinearColor Secondary = FLinearColor::White;
		UTexture2D* Icon = nullptr;                   // Patterns and materials
	};

	/**
	 * What the selected building accepts, worked out once per selection. An empty list means the building does
	 * not take that kind of customization, and the panel leaves that section out.
	 */
	struct FCustomizeOptions
	{
		TArray<FCustomizeOption> Swatches;
		TArray<FCustomizeOption> Patterns;
		TArray<FCustomizeOption> Materials;
		/** The game's custom color swatch, when it is unlocked and the building takes it. Finishes go with it. */
		UClass* CustomSwatch = nullptr;
		TArray<FCustomizeOption> Finishes;
		/** Index into Materials of the one the building is made of now. */
		int32 CurrentMaterial = INDEX_NONE;
		/** Why materials are offered but cannot be changed right now, for example with other players connected. */
		FText MaterialNote;
	};

	/**
	 * The selected building's own settings beyond production: names, load modes, switches, flow limits, lights.
	 * Each b-flag says the building has that setting; the panel shows only those. Read every frame.
	 */
	struct FControls
	{
		bool bHasName = false;   // Train, truck and drone stations, and anything with a building tag
		FText Name;
		bool bHasLoadMode = false; // Freight and fluid platforms, truck stations
		bool bLoadMode = true;
		bool bHasSwitch = false;   // Power switches
		bool bSwitchOn = false;
		bool bHasFlowLimit = false; // Pumps and valves, in m3/s
		float FlowLimit = -1.f;     // -1 means no limit
		float MaxFlow = 0.f;
		bool bHasLight = false;
		bool bLightOn = true;
		bool bNightOnly = false;
		float Intensity = 1.f;
		int32 LightColorSlot = 0;
		TArray<FLinearColor> LightColors; // The game's light color slots, shared by every light

		bool Any() const { return bHasName || bHasLoadMode || bHasSwitch || bHasFlowLimit || bHasLight; }
	};

	/** The selected building's customization as it is now. Read every frame. */
	struct FCustomizeState
	{
		UClass* Swatch = nullptr;
		UClass* Pattern = nullptr;
		UClass* Material = nullptr;
		uint8 PatternRotation = 0;
		bool bCustom = false; // Painted with the custom color swatch; the colors below apply
		FLinearColor CustomPrimary = FLinearColor::White;   // Linear, as the game stores them
		FLinearColor CustomSecondary = FLinearColor::White;
		UClass* Finish = nullptr;
	};

	/** Everything on the Settings tab. Saved between sessions. */
	struct FSettings
	{
		// Basic map (Mods > Atlas): just the buildings. While on, SyncModConfig turns the hover, icons, lights, ports
		// and spoilers below off over the menu's own values, and the map takes no clicks or tool keys.
		bool bBasic = false;
		bool bHoverLabel = true;
		bool bHoverHeight = true;
		bool bHoverCoordinates = true;
		bool bHighlight = true;
		bool bOutlines = true;
		bool bRadiation = false;
		bool bRecipeIcons = true;
		bool bStatusLights = true; // A light in each machine's corner showing what it is doing
		bool bHideSpoilers = false; // Spoiler-free: no collectibles, resource nodes, wells, spore flowers, gas pillars or radioactivity
		// Free ports drawn on the map, by kind. A hidden kind can't be dragged from either.
		bool bBeltPorts = true;
		bool bPipePorts = true;
		bool bHypertubePorts = true;
		bool bRailPorts = true;
		bool bRealisticTerrain = true; // Satisfactory Calculator's realistic imagery under the map instead of the game's own
		float Opacity = 1.f;
		int32 ExportSize = 8192; // 4096, 8192 or 32768
		bool bExportRendered = false; // Export the buildings as the game draws them, not as flat shapes
		int32 ZoomFactor = 4; // Times the game's own zoom limit
		int32 BeltTierFromTop = 0; // Belts built from the map: 0 is the highest unlocked Mk, 1 the one below, and so on
		int32 PipeTierFromTop = 0; // The same for pipes
		bool bAllowClipping = true; // Build from the map through other buildings, ignoring only clearance overlaps
	};

	/** A production building and the icon of what it makes, drawn on top of it when the map is zoomed in. */
	struct FRecipeIcon
	{
		FVector Location = FVector::ZeroVector;
		float HalfCm = 0.f; // Half the building's shorter side
		int32 Type = 0;
		UTexture2D* Icon = nullptr;
	};

	struct FExportState
	{
		std::atomic<int32> Stage{ 0 }; // 0 idle, 1 working, 2 done, 3 failed
		FString Path;
		// A rendered export's progress: tiles captured of those with buildings, then rows written.
		bool bRendered = false;
		std::atomic<int32> TilesDone{ 0 };
		std::atomic<int32> TilesTotal{ 0 };
		std::atomic<int32> RowsDone{ 0 };
		int32 Rows = 0;
	};

	struct FRenderJob;

	/** Which way a measure box's copies run, as seen on the map. */
	enum class EMeasureDirection : uint8 { Right, Down, Left, Up, Count };

	/**
	 * A rectangle measured on the map. Map metres run east (X) and south (Y) from the map's north-west corner.
	 * The box lives in its own grid: Origin and Angle place it, Pitch is the snapping step, and Min and Max are
	 * its corners in that grid's metres.
	 */
	struct FMeasureBox
	{
		FVector2f Origin = FVector2f::ZeroVector;
		float Angle = 0.f;
		float Pitch = 1.f;
		bool bFoundationGrid = false;
		FVector2f Min = FVector2f::ZeroVector;
		FVector2f Max = FVector2f::ZeroVector;
		int32 Copies = 0;
		EMeasureDirection Direction = EMeasureDirection::Right;

		// Worked out when the box, the buildings or what the map shows change
		bool bHasHeight = false;
		float LowMetres = 0.f;
		float HighMetres = 0.f;
		int32 Floors = 0;
		TArray<bool> Blocked; // One per copy

		FVector2f Size() const { return Max - Min; }
		/** A point in this box's grid, in map metres. */
		FVector2f ToMap(const FVector2f& Local) const;
		FVector2f ToLocal(const FVector2f& Map) const;
		/** Where copy Index (0 is the box itself) is shifted to, in the box's grid. */
		FVector2f CopyOffset(int32 Index) const;
		/** The four corners of copy Index, in map metres, going round. */
		void Corners(int32 Index, FVector2f Out[4]) const;
	};

	struct FMeasureLine
	{
		FVector2f A = FVector2f::ZeroVector;
		FVector2f B = FVector2f::ZeroVector;
	};

	/** Measure mode and everything measured, for one view. */
	struct FMeasureView
	{
		bool bMode = false;
		TArray<FMeasureBox> Boxes;
		TArray<FMeasureLine> Lines;
		int32 Active = INDEX_NONE;

		bool bPressed = false;
		bool bLineDrag = false;
		bool bMoved = false;
		FVector2f PressMetres = FVector2f::ZeroVector;
		FMeasureBox Draft;
		FMeasureLine DraftLine;

		/** Buildings in the way of a copy, as BlockedType records. */
		TArray<FVector4f> Hits;
		uint32 HitsVersion = 0;
		uint64 StatsKey = 0;
	};

	/**
	 * Power wire mode (AtlasWires.cpp): click a building, then another, and a power line runs between them through
	 * the game's own wire hologram. Clicking the ground puts a power pole there, as the build gun does. Each wire
	 * starts the next from where it ended, so a run of clicks chains buildings together.
	 */
	struct FWireTool
	{
		bool bMode = false;
		EView View = EView::Game;
		bool bHasStart = false;
		TWeakObjectPtr<UFGCircuitConnectionComponent> Start;
		TWeakObjectPtr<AFGBuildable> StartOwner;
		FVector StartLocation = FVector::ZeroVector;
		bool bHasTarget = false; // A building with a power connection under the cursor
		TWeakObjectPtr<AFGBuildable> Target;
		TWeakObjectPtr<UFGCircuitConnectionComponent> TargetConnection;
		bool bHasGround = false; // No building, but ground a pole can go on
		FVector EndLocation = FVector::ZeroVector;
		bool bReady = false;     // The hologram has settled on what is under the cursor, so Valid and Reason can be trusted
		bool bValid = false;
		bool bClipping = false;
		FText Reason;
		FText WireName;
		FString CostText;
		float LengthMetres = 0.f;
	};

	/** A building picked for dismantling: an actor, or a foundation or wall the game keeps as an instance. */
	struct FDismantleTarget
	{
		TWeakObjectPtr<AFGBuildable> Actor;
		FLightweightBuildableInstanceRef Lightweight;
		bool bLightweight = false;
		UClass* Class = nullptr;
		int32 Index = INDEX_NONE; // The instance's index, for a lightweight
		TArray<FVector4f> Records; // Its shapes on the map, drawn red while picked
	};

	/**
	 * What a pick on the map is for: dismantling it (Alt+F), changing the recipe of every machine in it (Alt+R), or
	 * wiring everything in it onto one power circuit (Alt+C).
	 */
	enum class EPickPurpose : uint8 { Dismantle, Recipe, Power };

	/** One power line connect mode would build, between two connections of picked buildings. */
	struct FPlannedWire
	{
		TWeakObjectPtr<UFGCircuitConnectionComponent> From;
		TWeakObjectPtr<UFGCircuitConnectionComponent> To;
		FVector FromLocation = FVector::ZeroVector;
		FVector ToLocation = FVector::ZeroVector;
		float Metres = 0.f;
		TArray<FItemAmount> Cost;
	};

	/** What connect mode would do with the pick: the wires, what they cost, and what they can't reach. */
	struct FPowerPlan
	{
		TArray<FPlannedWire> Wires;
		int32 Buildings = 0;      // Picked buildings with a power connection
		int32 GroupsBefore = 0;   // Separate circuits among them now; unpowered buildings count one each
		int32 GroupsAfter = 0;    // Left once the wires are built; 1 when everything joins
		float Metres = 0.f;
		FString CostText;
		int32 Affordable = 0;     // How many of the wires, in order, the inventory and Dimensional Depot pay for
		FString ShortText;        // What runs out first, when not all are affordable
		TArray<bool> Stranded;    // Per picked building: outside the biggest group once built, drawn red
	};

	/** One kind of machine in a recipe pick, and the recipes it can be switched to. */
	struct FRecipeGroup
	{
		UClass* Class = nullptr;
		FText Name;
		int32 Count = 0;
		TArray<FRecipeOption> Options;   // By name; the summary is at 100% clock
		TMap<UClass*, int32> Running;    // How many of the group run each recipe; nullptr for none
	};

	/**
	 * Dismantle mode (AtlasDismantle.cpp): click buildings, or drag a box over them, to pick them; Enter dismantles
	 * what is picked through the game's own dismantle, with the refund going to the inventory as the build gun's does.
	 * Recipe mode picks the same way, machines only, and switches every picked machine of a kind to a chosen recipe.
	 */
	struct FDismantleTool
	{
		bool bMode = false;
		EPickPurpose Purpose = EPickPurpose::Dismantle;
		EView View = EView::Game;
		TArray<FRecipeGroup> RecipeGroups; // Recipe mode: the picked machines by kind, most first
		FPowerPlan Power;                  // Connect mode: the wires that join the pick
		TArray<FDismantleTarget> Targets;
		bool bPressed = false;
		bool bBoxing = false;  // The press moved, so it is a box rather than a click
		bool bRemove = false;  // Shift: the box takes buildings out of the pick
		FVector2f PressMetres = FVector2f::ZeroVector;
		bool bCapped = false;  // A box found more than one pick may hold
		/** While a box is dragged: the pick as releasing now would leave it, so the map and card show it live. */
		TArray<FDismantleTarget> Preview;
		bool bPreview = false;
		bool bPreviewCapped = false;
		/** What the map and the cursor card show: the preview while a box is dragged, else the pick. */
		const TArray<FDismantleTarget>& Shown() const { return bPreview ? Preview : Targets; }
		bool ShownCapped() const { return bPreview ? bPreviewCapped : bCapped; }
		FString RefundText;
		/** How many of each building the pick holds, machines first, ready for the cursor card. */
		TArray<TPair<FString, FLinearColor>> CountLines;
	};

	/** One tier a belt line can be changed to: the belt, and the lift of the same speed when one is unlocked. */
	struct FUpgradeTier
	{
		UClass* BeltRecipe = nullptr;
		UClass* LiftRecipe = nullptr;
		UClass* Belt = nullptr; // What the recipes build
		UClass* Lift = nullptr;
		FText Name;
		float Speed = 0.f;
		int32 Changes = 0;   // Belts and lifts on the traced line this tier would replace
		int32 LiftsStay = 0; // Lifts it can't replace, with no lift of this speed unlocked
		FString CostText;    // What replacing them costs, net of what the old ones give back
	};

	/** What one belt or lift replacement came to. */
	enum class EUpgradeResult : uint8 { Done, Skipped, Unaffordable, Failed };

	/**
	 * Upgrading a belt line (AtlasUpgrade.cpp): from the selected belt or lift, a trace follows the items downstream,
	 * and every belt and lift on the way is replaced by the chosen tier through the game's own upgrade.
	 */
	struct FUpgradeTool
	{
		bool bOpen = false;
		bool bThroughAttachments = false; // Carry on through splitters and mergers, down every output
		TArray<TWeakObjectPtr<AFGBuildable>> Line; // In trace order, the selected one first
		int32 Belts = 0;
		int32 Lifts = 0;
		float LengthMetres = 0.f;
		FString EndText; // Where the line stops
		bool bCapped = false;
		TArray<FUpgradeTier> Tiers;

		bool bRunning = false;
		int32 Tier = INDEX_NONE;
		int32 Next = 0;
		int32 Done = 0;
		int32 Skipped = 0;
		FText FirstSkip;
		TMap<UClass*, int32> Overflow; // Given back but didn't fit in the inventory
	};

	/** Where a spot in a row stands: not looked at yet, the game would build it, it wouldn't, or it is built. */
	enum class EPlaceSpot : uint8 { Unknown, Valid, Blocked, Built };

	/** One building of a row dragged out in place mode. */
	struct FPlaceSpot
	{
		FVector Target = FVector::ZeroVector; // Where the cursor would be to place it
		FTransform Ghost = FTransform::Identity; // Where the hologram put it, once looked at
		EPlaceSpot State = EPlaceSpot::Unknown;
		FText Reason;
		TWeakObjectPtr<AFGBuildable> Building; // Once built
	};

	/** A building the place picker offers: an unlocked recipe whose product stands on a floor. */
	struct FPlaceOption
	{
		UClass* Recipe = nullptr;
		FText Name;
		const TCHAR* Group = TEXT("");
	};

	/**
	 * Place mode (AtlasPlace.cpp): a building in hand follows the cursor through the game's own hologram, landing
	 * on the floor the height filter shows, and a click builds it. A building sampled from the map comes with its
	 * settings: every copy gets them through the clipboard, as a Ctrl + left click would paste them.
	 */
	struct FPlaceTool
	{
		bool bMode = false;
		EView View = EView::Game;
		UClass* Recipe = nullptr; // What is in hand; none until one is picked or sampled
		FText Name;
		bool bSampled = false;    // Copies get the sampled building's settings
		/** A blueprint in hand, taken from the build gun; Recipe is then the game's shared blueprint recipe. */
		TWeakObjectPtr<UFGBlueprintDescriptor> Blueprint;
		bool bBlueprint = false; // Stays set if the descriptor goes away, so the hologram is never made without it
		bool IsBlueprint() const { return bBlueprint; }
		bool bHasGround = false;  // Something to stand on under the cursor
		bool bReady = false;      // The hologram has settled where the cursor is, so Valid and Reason can be trusted
		bool bValid = false;
		FText Reason;
		FString CostText;
		/** Where the hologram is now, for the ghost on the map: its footprint's centre, half size and yaw. */
		bool bHasGhost = false;
		FVector GhostCenter = FVector::ZeroVector;
		FVector2f GhostHalf = FVector2f::ZeroVector;
		float GhostYaw = 0.f; // Radians
		FTransform GhostTransform = FTransform::Identity;
		FVector GhostBoxCenter = FVector::ZeroVector; // The footprint's centre relative to the building
		/** The building's belt, pipe and hypertube ports, relative to the building, drawn on the ghost. */
		TArray<FPort> GhostPorts;
		int32 Built = 0;      // Placed since this building was taken in hand
		float HeightMetres = 0.f; // Where it will stand, for the card by the cursor
		/** A row being dragged out, each spot checked with the hologram in turn, then built on release. */
		bool bRow = false;
		bool bRowBuilding = false;
		TArray<FPlaceSpot> Row;
		int32 RowBuilt = 0;
		int32 RowFailed = 0;
		FText RowReason; // Why the first spot that failed while building did
		/** Ctrl held on a row of splitters or mergers: lined up with the machines' ports, then belted to them and each other. */
		bool bManifold = false;
		int32 ManifoldPorts = 0;   // Machine ports the row lined up with
		bool bManifoldBelting = false;
		int32 ManifoldBelts = 0;   // Belts built so far
		int32 ManifoldPlanned = 0;
		int32 ManifoldFailed = 0;
		FText ManifoldReason;
	};
}

/**
 * Owns every map record for the loaded world.
 *
 * Nothing here ticks. The first time the map opens, the world is scanned once; from then on building changes
 * are appended to a queue in constant time. The queue is applied on a worker thread, which also keeps each
 * cell sorted by height, so the game thread never pays for it. A worker owns the grid while a job runs, so the
 * game thread only reads it when IsBusy() is false.
 */
UCLASS()
class FACTORYATLAS_API UAtlasSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	static UAtlasSubsystem* GetActive() { return Active; }

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Data
	void OnMapOpened();
	/** A view's map went away: lets go of any port, wire, hologram or drag it was holding. */
	void OnMapClosed(Atlas::EView View);
	void EnsureScanned();
	void Pump();
	bool IsScanned() const { return bScanned; }
	bool IsBusy() const { return bJobActive; }
	const Atlas::FGrid* GetGrid() const { return bJobActive ? nullptr : Grid.Get(); }

	void HandleLightweight(UClass* Class, const FRuntimeBuildableInstanceData& Data, bool bAdded);

	// View state
	const TArray<Atlas::FType>& GetTypes() const { return Types; }
	bool IsCategoryHidden(Atlas::ECategory Category) const { return HiddenCategories[int32(Category)]; }
	void SetCategoryHidden(Atlas::ECategory Category, bool bHidden);
	void SetTypeHidden(int32 Type, bool bHidden);
	bool IsPaintMode() const { return bPaintMode; }
	void SetPaintMode(bool bEnabled);
	bool IsLayerEnabled() const { return bLayerEnabled; }
	void SetLayerEnabled(bool bEnabled);
	bool IsPanelOpen() const { return bPanelOpen; }
	void SetPanelOpen(bool bOpen);
	/** Basic map opens with the panel minimized. Not saved, so turning Basic map off brings back the usual state. */
	void MinimizePanelForBasic() { if (Settings.bBasic) { bPanelOpen = false; } }
	/** The map fills the whole map screen, without the game's window frame and filter list. */
	bool IsFullscreen() const { return bFullscreen; }
	void SetFullscreen(bool bInFullscreen);
	/** The building panel's color, pattern and material section is open rather than folded to one row. */
	bool IsAppearanceOpen() const { return bAppearanceOpen; }
	void SetAppearanceOpen(bool bOpen);
	const Atlas::FSettings& GetSettings() const { return Settings; }
	Atlas::FSettings& EditSettings() { return Settings; }
	void ResetSettings();
	void SaveSettings() const;
	/** Pulls the toggles that live in the mod settings menu (Mods > Atlas) into Settings. */
	void SyncModConfig();
	/** Basic map is on: the map shows the buildings and takes no clicks or tool keys. */
	bool IsBasic() const { return Settings.bBasic; }
	/** Lets go of every tool and the selection, for Basic map. */
	void LeaveMapTools();

	// Colours chosen by the player, per category or per building type
	FColor GetCategoryDisplayColor(Atlas::ECategory Category) const;
	bool HasCategoryColor(Atlas::ECategory Category) const { return CategoryColorOverrides[int32(Category)].IsSet(); }
	bool HasTypeColor(int32 Type) const;
	void SetCategoryColor(Atlas::ECategory Category, TOptional<FColor> Color);
	void SetTypeColor(int32 Type, TOptional<FColor> Color);
	void ResetColors();

	/** Circles around radioactive items, refreshed at most once a second. */
	const TArray<FVector4f>& GetRadiation(uint32& OutVersion);

	/** Uncollected Somersloops, Mercer Spheres, hard drives and power slugs, refreshed every few seconds. */
	const TArray<FVector4f>& GetMarkers(uint32& OutVersion);
	bool IsCollectibleShown(Atlas::ECollectible Kind) const { return !Settings.bHideSpoilers && ((CollectibleMask >> int32(Kind)) & 1); }
	void SetCollectibleShown(Atlas::ECollectible Kind, bool bShown);
	bool AnyMarkersShown() const { return !Settings.bHideSpoilers && (CollectibleMask != 0 || FeatureMask != 0); }
	bool IsRadiationShown() const { return Settings.bRadiation && !Settings.bHideSpoilers; }
	bool AreSpoilersHidden() const { return Settings.bHideSpoilers; }
	void SetSpoilersHidden(bool bHidden);
	int32 GetCollectibleCount(Atlas::ECollectible Kind) const { return CollectibleCounts[int32(Kind)]; }
	/** How far the collectible counts can be trusted. A multiplayer client cannot know everything the host knows. */
	enum class ECollectibleData : uint8 { None, Exact, ClientPartial };
	ECollectibleData GetCollectibleData() const { return CollectibleData; }
	/** The counts are only gathered while at least one collectible is shown. */
	bool AnyCollectibleShown() const { return !Settings.bHideSpoilers && CollectibleMask != 0; }

	/** Name for the hover label, building or collectible. */
	FText GetHoverName() const;

	// Resource nodes, wells, spore flowers and gas pillars
	struct FResourceKind
	{
		UClass* Descriptor = nullptr;
		FText Name;
		FColor Color;
		bool bWell = false;
		int32 Count = 0;
	};
	bool IsFeatureShown(Atlas::EFeature Feature) const { return !Settings.bHideSpoilers && ((FeatureMask >> int32(Feature)) & 1); }
	void SetFeatureShown(Atlas::EFeature Feature, bool bShown);
	int32 GetFeatureCount(Atlas::EFeature Feature) const { return FeatureCounts[int32(Feature)]; }
	/** Resource kinds found so far, in marker order (ResourceFirst + index). */
	const TArray<FResourceKind>& GetResourceKinds() const { return ResourceKinds; }
	bool IsResourceHidden(UClass* Descriptor) const;
	void SetResourceHidden(UClass* Descriptor, bool bHidden);
	bool IsMarkerShown(int32 Type) const;

	// Selection and configuration
	/** Selects the building under the cursor. Returns false when it has nothing to configure or customize. */
	bool SelectHovered();
	void ClearSelection();
	bool HasSelection() const;
	const FVector4f& GetSelectionRecord() const { return SelectionRecord; }
	/** Where the map last drew the selected building, in window pixels, and how far it reaches from there. */
	void SetSelectionAnchor(const FVector2f& Absolute, float RadiusPixels, Atlas::EView View = Atlas::EView::Game)
	{
		SelectionAnchor[int32(View)] = Absolute;
		SelectionAnchorRadius[int32(View)] = RadiusPixels;
	}
	const FVector2f& GetSelectionAnchor(Atlas::EView View = Atlas::EView::Game) const { return SelectionAnchor[int32(View)]; }
	float GetSelectionAnchorRadius(Atlas::EView View = Atlas::EView::Game) const { return SelectionAnchorRadius[int32(View)]; }
	Atlas::FFactoryState GetSelectionState() const;
	void SetSelectedClock(float Potential);
	void SetSelectedPaused(bool bPaused);

	/** Recipes the selected manufacturer can run, as the game offers them at the machine. */
	TArray<Atlas::FRecipeOption> GetSelectedRecipeOptions() const;
	/** Changes the recipe the same way the machine's own screen does. Returns false if the game refused. */
	bool SetSelectedRecipe(UClass* Recipe);

	// Customization of the selection: colors, patterns and materials
	/** Goes up whenever the selection or the options it offers change, so the panel knows to rebuild its lists. */
	uint32 GetSelectionVersion() const { return SelectionVersion; }
	const Atlas::FCustomizeOptions& GetCustomizeOptions() const { return CustomizeOptions; }
	Atlas::FCustomizeState GetCustomizeState() const;
	void SetSelectedSwatch(UClass* Swatch);
	/** Paints the selection with the custom swatch. Colors are linear; Finish is nullptr or one of the offered finishes. */
	void SetSelectedCustomColor(const FLinearColor& Primary, const FLinearColor& Secondary, UClass* Finish);
	/** nullptr removes the pattern. */
	void SetSelectedPattern(UClass* Pattern);
	void RotateSelectedPattern();
	void SetSelectedMaterial(UClass* Material);
	/** The last customization the game refused, with the reason. Empty after one succeeds. */
	const FText& GetCustomizeError() const { return CustomizeError; }

	// Building settings: names, load modes, switches, flow limits, lights. All need the host.
	Atlas::FControls GetControls() const;
	void SetSelectedName(const FText& Name);
	void SetSelectedLoadMode(bool bLoad);
	void SetSelectedSwitch(bool bOn);
	/** m3/s; negative removes the limit. */
	void SetSelectedFlowLimit(float Limit);
	void SetSelectedLight(bool bOn, bool bNightOnly, float Intensity, int32 ColorSlot);

	// Copy and paste. Machine settings go through the game's own clipboard, the one Ctrl+C and Ctrl+V on a
	// building use, so the two share it. Color, pattern and material are kept here, since the game has no clipboard for them.
	bool CanCopySelected() const;
	void CopySelected();
	/** True when the clipboard holds anything the selection can take. */
	bool CanPasteToSelected() const;
	void PasteToSelected();
	/** What the last copy or paste did, for the panel. Cleared when the selection changes. */
	const FText& GetClipboardStatus() const { return ClipboardStatus; }
	void SetClipboardStatus(const FText& Status) { ClipboardStatus = Status; }

	/**
	 * Ctrl-clicks on the map: copy from, or paste onto, the building under the cursor without selecting it, so
	 * the panel stays as it was. Done collects what a drag-paste has reached, so each building is pasted once.
	 * False when there is no usable building there. What happened is shown as a toast.
	 */
	bool CopyHovered();
	bool PasteHovered(TSet<const void*>* Done = nullptr);
	void ShowToast(const FText& Text, bool bError = false);
	/** Plays one of the Ctrl-click sounds. A sound asked for again within a moment of the last one is skipped. */
	void PlayClipboardSound(Atlas::EClipboardSound Sound);
	/** A short message by the cursor, while it lasts. */
	bool GetToast(FText& OutText, bool& bOutError, float& OutAlpha) const;
	/** Tells one selected building from another: its actor, or its instance's data for a lightweight. */
	const void* GetSelectionIdentity() const;

	/** The map widget's Slate widget, used to tell clicks on the map from clicks on menus drawn over it. */
	void SetMapWidget(const TSharedPtr<SWidget>& Widget, Atlas::EView View = Atlas::EView::Game) { MapWidget[int32(View)] = Widget; }
	bool IsPointOnMap(const FVector2f& ScreenPosition, Atlas::EView View = Atlas::EView::Game) const;

	/** Lowest and highest building on the map, in metres. */
	FVector2f GetHeightBounds() const;
	FVector2f GetHeightFilter() const { return FVector2f(FilterLow, FilterHigh); }
	void SetHeightFilter(float Low, float High);
	/**
	 * Alt + wheel: moves the height filter one floor up (Direction > 0) or down. Floors are the foundation tops
	 * around the cursor. Past the top or bottom floor the filter opens up to every height again.
	 */
	void StepFloor(Atlas::EView View, int32 Direction);

	UTexture2D* GetPalette();
	void FlushPalette();

	/** Changes whenever the list of building types or their counts change; each menu keeps the one it built. */
	uint32 GetMenuVersion();

	// Hover
	void UpdateHover(FVector2f Uv, float PixelsPerMetre, Atlas::EView View = Atlas::EView::Game);
	void ClearHover() { Hover = Atlas::FHover(); }
	/** Clears the hover only if this view set it, so the other view's stays. */
	void ClearHover(Atlas::EView View) { if (HoverView == View) { ClearHover(); } }
	const Atlas::FHover& GetHover() const { return Hover; }
	/** The view the hover was found in; only that view draws its label and highlight. */
	Atlas::EView GetHoverView() const { return HoverView; }
	void SetPointerOverPanel(bool bOver, Atlas::EView View = Atlas::EView::Game);
	bool IsPointerOverPanel(Atlas::EView View = Atlas::EView::Game) const { return bPointerOverPanel[int32(View)]; }
	/** During a drag-paste the hover is worked out every frame rather than a few times a second, so no building is skipped. */
	void SetHoverEveryFrame(bool bEveryFrame) { bHoverEveryFrame = bEveryFrame; }
	bool WantsHoverEveryFrame() const { return bHoverEveryFrame; }

	// Export
	void StartExport();
	FText GetExportStatus() const;
	bool IsExporting() const { return ExportState && ExportState->Stage.load() == 1; }

	// Recipe icons
	/** Production buildings with a recipe set. Rebuilt at most twice a second, so recipe changes show up quickly. */
	const TArray<Atlas::FRecipeIcon>& GetRecipeIcons();

	// Building belts from the map (AtlasBelts.cpp)
	/** Port nodes show from this zoom on, in pixels per metre. */
	static constexpr float PortMinPixelsPerMetre = 1.5f;
	/** Where the cursor is on a view's map, set by that view every paint, so ports under it can be found. */
	void SetMapCursor(Atlas::EView View, FVector2f Uv, float PixelsPerMetre);
	/** Free conveyor ports of buildings the map shows, or none when belts can't be built here (no authority). */
	const TArray<Atlas::FPort>& GetPorts();
	/** The port under the cursor in this view, or INDEX_NONE. During a drag, only ports the belt could end on. */
	int32 GetPortUnderCursor(Atlas::EView View);
	bool CanBuildBelts() const;
	/** A press on the map: true when it landed on a port, which then owns the press until release. */
	bool PressBeltPort(Atlas::EView View, const FVector2f& ScreenPosition);
	/** The pointer moved with the button held; past a few pixels the press becomes a drag. */
	void MoveBeltPress(const FVector2f& ScreenPosition);
	/** The button came up. True when the press never moved, so it counts as an ordinary click on the building. */
	bool ReleaseBeltPress();
	void CancelBeltDrag();
	void CycleBeltBuildMode();
	void CycleBeltTier();
	bool IsBeltPressed() const { return BeltDrag.bPressed; }
	bool IsDraggingBelt() const { return BeltDrag.bActive; }
	const Atlas::FBeltDrag& GetBeltDrag() const { return BeltDrag; }
	/** While dragging: whether the drag could end on this port. */
	bool IsTargetFor(const Atlas::FPort& Candidate) const;
	FVector2f GetMapCursorUv(Atlas::EView View) const { return MapCursorUv[int32(View)]; }
	/** Where map UV (0,0) is drawn and how many pixels the whole map spans, in paint space, set by each view every paint. */
	void SetMapTransform(Atlas::EView View, FVector2f Origin, FVector2f Scale)
	{
		MapOrigin[int32(View)] = Origin;
		MapScale[int32(View)] = Scale;
	}
	/** A point in map metres, in the view's paint space. */
	FVector2f MetresToPaint(Atlas::EView View, const FVector2f& Metres) const
	{
		return MapOrigin[int32(View)] + MapScale[int32(View)] * (Metres / Atlas::MapWidthMetres);
	}

	// Power wires (AtlasWires.cpp)
	bool IsWiring(Atlas::EView View) const { return Wire.bMode && Wire.View == View; }
	/** Wire mode on or off for a view. On turns measure mode off there; only one view wires at a time. */
	void SetWiring(Atlas::EView View, bool bOn);
	/** A click on the map in wire mode: starts a wire on the building under the cursor, or ends it there. */
	void ClickWire(Atlas::EView View);
	/** Lets go of the wire being run, staying in wire mode. False when there was none. */
	bool DropWire();
	const Atlas::FWireTool& GetWire() const { return Wire; }

	// Placing buildings (AtlasPlace.cpp)
	bool IsPlacing(Atlas::EView View) const { return Place.bMode && Place.View == View; }
	/** Place mode on or off for a view. On turns wire and measure mode off; only one view places at a time. */
	void SetPlacing(Atlas::EView View, bool bOn);
	/** Puts the building a recipe makes in hand, with default settings, turning place mode on. */
	void SetPlaceRecipe(Atlas::EView View, UClass* Recipe);
	/**
	 * Takes the blueprint the build gun holds in hand on the map, turning place mode on. False, saying nothing, when
	 * the build gun holds no blueprint.
	 */
	bool TakeBuildGunBlueprint(Atlas::EView View);
	/** Middle click: takes the building under the cursor in hand with its settings. False when there is none to take. */
	bool SampleHovered(Atlas::EView View);
	/** A click on the map in place mode: builds what is in hand where the ghost is. */
	void ClickPlace(Atlas::EView View);
	/** Puts down what is in hand, staying in place mode. False when nothing was. */
	bool DropPlace();
	/** A left press on the map with a building in hand: true when it may become a row drag. */
	bool PressPlaceRow(Atlas::EView View);
	/** The press moved: lays the row out from where the press began to the cursor. */
	void MovePlaceRow(Atlas::EView View);
	/** The press ended: true when it was a row, which is then built. False means an ordinary click. */
	bool ReleasePlaceRow(Atlas::EView View);
	/** Drops the row being dragged, or stops building one. */
	void CancelPlaceRow();
	/** R turns the building in hand a quarter turn, or one fine step; Direction is 1 or -1. */
	void RotatePlace(int32 Direction, bool bFine);
	const Atlas::FPlaceTool& GetPlace() const { return Place; }
	/** Everything the picker offers, by group then name. */
	TArray<Atlas::FPlaceOption> GetPlaceOptions() const;

	// Dismantling (AtlasDismantle.cpp)
	static constexpr int32 MaxDismantleTargets = 1000;
	/** Either pick mode, dismantle or recipe, is on in this view: clicks and boxes pick buildings. */
	bool IsPicking(Atlas::EView View) const { return Dismantle.bMode && Dismantle.View == View; }
	bool IsDismantling(Atlas::EView View) const { return IsPicking(View) && Dismantle.Purpose == Atlas::EPickPurpose::Dismantle; }
	bool IsChangingRecipes(Atlas::EView View) const { return IsPicking(View) && Dismantle.Purpose == Atlas::EPickPurpose::Recipe; }
	bool IsConnecting(Atlas::EView View) const { return IsPicking(View) && Dismantle.Purpose == Atlas::EPickPurpose::Power; }
	/** A pick mode on or off for a view. On turns the other map tools off, and starts an empty pick; off ends either mode. */
	void SetPicking(Atlas::EView View, Atlas::EPickPurpose Purpose, bool bOn);
	void SetDismantling(Atlas::EView View, bool bOn) { SetPicking(View, Atlas::EPickPurpose::Dismantle, bOn); }
	void SetChangingRecipes(Atlas::EView View, bool bOn) { SetPicking(View, Atlas::EPickPurpose::Recipe, bOn); }
	void SetConnecting(Atlas::EView View, bool bOn) { SetPicking(View, Atlas::EPickPurpose::Power, bOn); }
	/** Connect mode: builds the planned power lines (AtlasWires.cpp), paid like the build gun, then clears the pick. */
	void ConnectPicked();
	/** Goes up whenever the recipe pick's groups change, so the recipe list knows to rebuild. */
	uint32 GetRecipeGroupsVersion() const { return RecipeGroupsVersion; }
	/** Recipe mode: switches every picked machine of the class to Recipe, as each machine's own screen would. */
	void SetPickedRecipe(UClass* MachineClass, UClass* Recipe);
	const Atlas::FDismantleTool& GetDismantle() const { return Dismantle; }
	/** A left press on the map in dismantle mode: a click picks or unpicks, a drag boxes. bRemove boxes buildings out. */
	bool PressDismantle(Atlas::EView View, bool bRemove);
	void MoveDismantle(Atlas::EView View);
	void ReleaseDismantle(Atlas::EView View);
	void CancelDismantleBox();
	/** Unpicks everything. False when nothing was picked. */
	bool ClearDismantle();
	/** Dismantles everything picked. */
	void DismantlePicked();
	/** The picked buildings' shapes in red, for the map. */
	const TArray<FVector4f>& GetDismantleRecords(uint32& OutVersion) const
	{
		OutVersion = DismantleVersion;
		return DismantleRecords;
	}

	// Upgrading a belt line (AtlasUpgrade.cpp)
	static constexpr int32 MaxUpgradeLine = 2000;
	/** True when the selection is a belt or a lift, so its line can be upgraded. */
	bool IsSelectedConveyor() const;
	bool IsUpgradeOpen() const { return Upgrade.bOpen; }
	/** Opening traces the line from the selection and shows it on the map; closing drops the trace. */
	void SetUpgradeOpen(bool bOpen);
	void SetUpgradeThroughAttachments(bool bThrough);
	const Atlas::FUpgradeTool& GetUpgrade() const { return Upgrade; }
	/** Goes up whenever the trace or its tiers change, so the panel knows to rebuild its list. */
	uint32 GetUpgradeVersion() const { return UpgradeVersion; }
	/** Traces the line again and replaces every belt and lift on it with the given tier, a few each frame. */
	void StartUpgrade(int32 Tier);
	/** The traced line's shapes in the accent colour, for the map. */
	const TArray<FVector4f>& GetUpgradeRecords(uint32& OutVersion) const
	{
		OutVersion = UpgradeVersion;
		return UpgradeRecords;
	}

	// Status lights (AtlasStatus.cpp)
	/** One record per machine with a light, drawn by the status material. Null when the lights are off. */
	const Atlas::FGrid* GetStatusGrid() const { return bStatusActive ? StatusGrid.Get() : nullptr; }
	/** The light on the hovered building, None when it has none or the lights are off. */
	Atlas::EStatusLight GetHoverStatus() const;
	/** atlas.StatusDump: every machine's status by class, and whether the lights agree with it. */
	void DumpStatuses();

	// Measure tool (AtlasMeasure.cpp)
	bool IsMeasuring(Atlas::EView View) const { return Measure[int32(View)].bMode; }
	void SetMeasuring(Atlas::EView View, bool bOn);
	const Atlas::FMeasureView& GetMeasure(Atlas::EView View) const { return Measure[int32(View)]; }
	/** A left press on the map in measure mode: starts a box, or a line when bLine. */
	void PressMeasure(Atlas::EView View, bool bLine);
	void MoveMeasure(Atlas::EView View);
	/** Ends the drag. A press that never moved makes the box under the cursor the active one. */
	void ReleaseMeasure(Atlas::EView View);
	void CancelMeasureDrag(Atlas::EView View);
	void SetMeasureCopies(Atlas::EView View, int32 Copies);
	void SetMeasureDirection(Atlas::EView View, Atlas::EMeasureDirection Direction);
	void RemoveActiveMeasure(Atlas::EView View);
	void ClearMeasures(Atlas::EView View);
	/** Brings heights, floors and blocked copies up to date when the boxes, buildings or filters changed. */
	void UpdateMeasureStats(Atlas::EView View);
	const TArray<FVector4f>& GetMeasureHits(Atlas::EView View, uint32& OutVersion) const
	{
		OutVersion = Measure[int32(View)].HitsVersion;
		return Measure[int32(View)].Hits;
	}

private:
	UFUNCTION()
	void OnBuildableAdded(AFGBuildable* Buildable);
	UFUNCTION()
	void OnBuildableRemoved(AFGBuildable* Buildable);

	int32 TypeFor(UClass* Class);
	int32 PaintFor(const FFactoryCustomizationData& Data);
	/** True for a placed building Atlas should draw: not a lightweight stand-in, and not a part the HUB spawns for itself. */
	static bool IsMappedActor(const AFGBuildable* Buildable);
	void QueueActor(AFGBuildable* Buildable, bool bAdded);
	void Scan();
	void LaunchJob(TSharedPtr<Atlas::FGrid> Replacement);
	void FinishJob();
	void RebuildPalette();
	void LoadSettings();
	void ApplyTypeColor(int32 Type);
	void BindBuildableDelegates(bool bBind);

	inline static UAtlasSubsystem* Active = nullptr;

	TSharedPtr<Atlas::FGrid> Grid;
	TSharedPtr<Atlas::FGrid> PendingGrid;
	TArray<Atlas::FOp> PendingOps;
	UE::Tasks::FTask Job;
	TSharedPtr<std::atomic<bool>> JobRunning;
	bool bJobActive = false;
	bool bScanned = false;
	bool bBound = false;
	bool bWantsRescan = false;
	double LastScanSeconds = 0.0;
	int32 BuiltStress = 0;

	TArray<Atlas::FType> Types;
	TMap<UClass*, int32> TypeIndex;

	TArray<FColor> PaintColors;
	TMap<FColor, int32> PaintIndex;
	TMap<UClass*, int32> SwatchIndex;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Palette;

	/** Loaded on first use, by EClipboardSound. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundBase>> ClipboardSounds;
	double LastClipboardSoundSeconds = 0.0;
	TArray<FColor> PaletteData;
	bool bPaletteDirty = true;

	bool HiddenCategories[int32(Atlas::ECategory::Count)] = {};
	TSet<FString> HiddenTypePaths;
	bool bPaintMode = false;
	bool bLayerEnabled = true;
	bool bPanelOpen = true;
	bool bFullscreen = false;
	bool bAppearanceOpen = false;
	bool bMenuDirty = true;
	Atlas::FSettings Settings;
	double LastConfigSyncSeconds = -1000.0;

	TOptional<FColor> CategoryColorOverrides[int32(Atlas::ECategory::Count)];
	TMap<FString, FColor> TypeColorOverrides;

	TArray<FVector4f> Radiation;
	uint32 RadiationVersion = 0;
	double LastRadiationSeconds = -1.0;

	void RebuildMarkers();
	TArray<FVector4f> Markers;
	uint32 MarkerVersion = 0;
	double LastMarkerSeconds = -1000.0;
	uint32 CollectibleMask = 0;
	int32 CollectibleCounts[int32(Atlas::ECollectible::Count)] = {};
	TMap<UClass*, int32> MarkerOfClass;

	void GatherCollectibles();
	ECollectibleData CollectibleData = ECollectibleData::None;
	// The game records collected pickups and looted drop pods only on the host and never replicates
	// that, so a client learns them itself: pickups it sees destroyed, drop pods it sees looted.
	void OnActorDestroyed(AActor* Actor);
	FDelegateHandle ActorDestroyedHandle;
	TSet<FGuid> ClientGonePickups;
	TSet<FGuid> ClientLootedPods;
	void GatherWorldFeatures();
	int32 ResourceMarkerFor(UClass* Descriptor, bool bWell);
	void LoadFeatureCache();
	void SaveFeatureCache() const;
	uint32 FeatureMask = 0;
	int32 FeatureCounts[int32(Atlas::EFeature::Count)] = {};
	TArray<FResourceKind> ResourceKinds;
	TSet<FString> HiddenResourcePaths;
	/** Spore flowers and gas pillars seen so far, by actor name. They only exist while their part of the world is loaded. */
	TMap<FString, FVector> SeenSporeFlowers;
	TMap<FString, FVector> SeenGasPillars;
	bool bFeatureCacheLoaded = false;

	TWeakObjectPtr<AFGBuildable> Selected;
	/** Foundations, walls and the like have no actor; the game keeps them as instances of their class. */
	FLightweightBuildableInstanceRef SelectedLightweight;
	bool bSelectedLightweight = false;
	uint32 SelectionVersion = 0;
	Atlas::FCustomizeOptions CustomizeOptions;
	FText CustomizeError;
	/** Keeps the option icons loaded; the descriptors hold them only through soft pointers. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> CustomizeIcons;
	void RefreshCustomizeOptions();
	void SetSelectedPatternRotation(uint8 Rotation);
	/** The copied look: whole customization data, plus the material by descriptor and name, since material families differ. */
	TOptional<FFactoryCustomizationData> CopiedCustomization;
	UClass* CopiedMaterial = nullptr;
	FText CopiedMaterialName;
	/** The machine's recipe and clock, applied after the game's own paste, which leaves the recipe alone from here. */
	UClass* CopiedRecipe = nullptr;
	TOptional<float> CopiedClock;
	/** What the clipboard was copied from, so it only pastes onto the same kind of building. */
	UClass* CopiedClass = nullptr;
	Atlas::ECategory CopiedCategory = Atlas::ECategory::Other;
	bool bCopiedSettings = false; // Machine settings, recipe or clock came along, not just the look
	/** Whether the selection is a building the clipboard may paste onto. */
	bool PasteFitsSelection() const;
	UClass* SelectionClass() const;
	/** Selects the hovered building, runs Action on it, then puts the previous selection back. */
	bool ActOnHovered(TFunctionRef<void()> Action, TSet<const void*>* Done);
	/** The same for whatever Select picks; Select returns false when there is nothing to act on. */
	bool ActOnSelection(TFunctionRef<bool()> Select, TFunctionRef<void()> Action, TSet<const void*>* Done);
	FText Toast;
	bool bToastError = false;
	double ToastUntil = 0.0;
	FText ClipboardStatus;
	/** Applies new customization data to the selection and redraws it on the map. */
	bool ApplySelectedCustomization(const FFactoryCustomizationData& Data);
	FVector4f SelectionRecord = FVector4f::Zero();
	FVector2f SelectionAnchor[int32(Atlas::EView::Count)] = {};
	float SelectionAnchorRadius[int32(Atlas::EView::Count)] = {};
	TWeakPtr<SWidget> MapWidget[int32(Atlas::EView::Count)];
	float FilterLow = Atlas::HeightFilterMinMetres;
	float FilterHigh = Atlas::HeightFilterMaxMetres;

	TSet<TWeakObjectPtr<AFGBuildableManufacturer>> Manufacturers;
	TArray<Atlas::FRecipeIcon> RecipeIcons;
	TMap<UClass*, UTexture2D*> RecipeIconCache;
	double RecipeIconsSeconds = -1.0;
	bool bRecipeIconsDirty = true;

	Atlas::FHover Hover;
	Atlas::EView HoverView = Atlas::EView::Game;
	bool bPointerOverPanel[int32(Atlas::EView::Count)] = {};
	uint32 MenuVersion = 0;
	bool bHoverEveryFrame = false;

	// Belts from the map
	FVector2f MapCursorUv[int32(Atlas::EView::Count)] = {};
	float MapCursorPixelsPerMetre[int32(Atlas::EView::Count)] = {};
	TArray<Atlas::FPort> AllPorts; // Every free port, rebuilt when buildings change
	TArray<Atlas::FPort> Ports;    // Those the map shows this frame
	TArray<Atlas::FPort> NoPorts;
	uint64 PortsFrame = MAX_uint64;
	bool bPortsDirty = true;
	double PortsBuiltSeconds = -1000.0;
	Atlas::FBeltDrag BeltDrag;
	TWeakObjectPtr<class AFGHologram> BeltHologram;
	TArray<TSubclassOf<class UFGBuildGunModeDescriptor>> BeltModes;
	int32 BeltModeIndex = 0;
	uint8 BeltModeKind = 0; // The port kind BeltModeIndex was chosen for
	int32 BeltStage = 0;
	int32 BeltWaitFrames = 0;
	int32 BeltStepTries = 0;
	int32 BeltHitVariant = 0; // Which way of aiming at a port the hologram accepted, see HitAtPort
	int32 BeltSettleFrames = 0;
	FTSTicker::FDelegateHandle BeltTicker;
	/** The belt tool is building a line between two given ports on its own, for a manifold, not following the cursor. */
	bool bBeltAuto = false;
	bool StartAutoBelt(const Atlas::FPort& Start, const Atlas::FPort& Target);
	/** The automatic belt has the game's verdict: builds it when valid. */
	void FinishAutoBelt();
	void RebuildPorts();
	bool IsPortShown(const Atlas::FPort& Port);
	int32& TierFromTop(Atlas::EPortKind Kind);
	TSubclassOf<class UFGRecipe> PickBeltRecipe(FText& OutName) const;
	bool SpawnBeltHologram();
	void DestroyBeltHologram();
	bool TickBelt();
	bool PlaceBeltEnd(const Atlas::FPort& Port);
	void ValidateBelt();
	void PreviewBeltAtCursor();
	void ReadBeltPreview();
	void BuildBelt();
	class AFGCharacterPlayer* BeltPlayer() const;
	/** Takes a build's cost as the build gun does: inventory first, then the Dimensional Depot. False if short. */
	bool PayBuildCost(const TArray<struct FItemAmount>& Cost);
	uint64 MapPaintFrame[int32(Atlas::EView::Count)] = {};

	// Power wires
	Atlas::FWireTool Wire;
	TWeakObjectPtr<class AFGHologram> WireHologram;
	FTSTicker::FDelegateHandle WireTicker;
	int32 WireStage = 0;
	int32 WireWaitFrames = 0;
	int32 WireSettleFrames = 0;
	int32 WireHitVariant = 0;
	int32 WireEndVariant = 0;
	FVector4f WireHoverSource = FVector4f::Zero();
	TWeakObjectPtr<AFGBuildable> WireHoverActor;
	bool bWireHoverResolved = false;
	bool TickWire();
	bool SpawnWireHologram();
	void DestroyWireHologram();
	void ResolveWireTarget();
	void ValidateWire();
	void BuildWire();
	void SetWireStart(UFGCircuitConnectionComponent* Connection);
	/** The building whose record is under the cursor, for buildings that are actors (not foundations or walls). */
	AFGBuildable* FindHoveredActor();

	// Dismantling
	Atlas::FDismantleTool Dismantle;
	TArray<FVector4f> DismantleRecords;
	uint32 DismantleVersion = 0;
	uint32 RecipeGroupsVersion = 0;
	FTSTicker::FDelegateHandle DismantleTicker;
	FVector2f DismantlePreviewEnd = FVector2f::ZeroVector;
	double DismantlePreviewNext = 0.0;
	/** While a box is dragged, works out what it would pick, as often as the save's size allows. */
	bool TickDismantle();
	void StopDismantleTicker();
	/** Puts the box from the press to End into a pick, or takes it out of one. */
	void ApplyDismantleBox(TArray<Atlas::FDismantleTarget>& Into, const FVector2f& End, bool bRemove, bool& bOutCapped);
	void RefreshDismantle();
	/** Fills the pick's per-building counts. */
	void CountDismantle();
	/** Recipe mode: groups the picked machines by kind, with the recipes each kind can run. */
	void GroupRecipes();
	/** Whether the current pick mode takes this building: anything to dismantle, only manufacturers for recipes. */
	bool PickAccepts(const Atlas::FDismantleTarget& Target) const;
	/** Connect mode: plans the power lines that join the pick into one circuit (AtlasWires.cpp). */
	void PlanPower();
	/** Changes one machine's recipe the way its own screen does: its items go to the player first. */
	bool SetMachineRecipe(AFGBuildable* Building, UClass* Recipe);
	/** The building under the cursor as a dismantle target, with its map shapes. */
	bool FindHoveredTarget(Atlas::FDismantleTarget& Out);
	/** A dismantle target for a given actor, or a given instance of a lightweight class, with its map shapes. */
	bool TargetForActor(AFGBuildable* Building, Atlas::FDismantleTarget& Out);
	bool TargetForInstance(UClass* Class, int32 Index, Atlas::FDismantleTarget& Out);
	/** Every building the map shows with a shape inside the box (map metres), up to Limit. */
	void CollectTargetsInBox(const FVector2f& Min, const FVector2f& Max, TArray<Atlas::FDismantleTarget>& Out, int32 Limit, bool& bOutCapped,
		Atlas::EPickPurpose Purpose = Atlas::EPickPurpose::Dismantle);
	/** Whether a building can be wired: a power connection a wire can reach, and not a switch, whose two sides must stay apart. */
	static bool IsWirable(const AFGBuildable* Building);

	// Upgrading a belt line
	Atlas::FUpgradeTool Upgrade;
	TArray<FVector4f> UpgradeRecords;
	uint32 UpgradeVersion = 0;
	FTSTicker::FDelegateHandle UpgradeTicker;
	/** Follows the selection downstream into Upgrade.Line, and works out what each tier would do to it. */
	void TraceUpgrade();
	void RefreshUpgradeRecords();
	bool TickUpgrade();
	void FinishUpgrade(const FText& Stopped);
	void StopUpgradeTicker();
	/** Replaces one belt or lift with the tier's, through the game's upgrade hologram. OutNew is the replacement. */
	Atlas::EUpgradeResult UpgradeOne(AFGBuildable* Old, const Atlas::FUpgradeTier& Tier, FText& OutReason, AFGBuildable*& OutNew);
	/** What replacing Old with Recipe's building costs, less what Old gives back. Negative amounts come back. */
	void UpgradeNetCost(AFGBuildable* Old, UClass* Recipe, TMap<UClass*, int32>& InOutNet) const;

	// Placing buildings
	Atlas::FPlaceTool Place;
	TWeakObjectPtr<class AFGHologram> PlaceHologram;
	FTSTicker::FDelegateHandle PlaceTicker;
	int32 PlaceSettleFrames = 0;
	/** What the hologram was last placed for; any change places it again. */
	FVector2f PlaceAimUv = FVector2f(-1.f, -1.f);
	FVector2f PlaceAimFilter = FVector2f::ZeroVector;
	bool bPlaceAimDirty = true;
	/** The hologram's scroll rotation, kept so each new hologram faces the way the last one did. */
	int32 PlaceRotation = 0;
	/** The footprint of the building in hand, from its clearance box, relative to the actor. */
	FVector PlaceBoxCenter = FVector::ZeroVector;
	FVector2f PlaceBoxHalf = FVector2f::ZeroVector;
	/** A building just placed from a sample, given the sample's settings a couple of frames later. */
	struct FPlacePaste
	{
		TWeakObjectPtr<AFGBuildable> Building;
		int32 Frames = 0;
		FText Name;
		bool bAnnounce = false; // Say so when it worked; a row says its own
	};
	TArray<FPlacePaste> PlacePastes;
	/** Gives copies waiting for the sample's settings their settings: those whose wait is over, or all. */
	void FlushPlacePaste(bool bAll);
	// The row being dragged
	bool bRowPressed = false;
	FTransform RowOrigin = FTransform::Identity;
	int32 RowAxis = -1; // 0 the building's X, 1 its Y
	int32 RowSign = 1;
	double RowStep = 0.0;
	int32 RowCursor = INDEX_NONE; // The spot the hologram is on
	int32 RowStage = 0;
	int32 RowWait = 0;
	void TickPlaceRow();
	/** A manifold row was built: works out the belts between its pieces and to the machines, and starts on them. */
	void PlanManifold();
	/** Starts the next manifold belt, or says what was built when there are none left. */
	void NextManifoldBelt();
	TArray<TPair<Atlas::FPort, Atlas::FPort>> ManifoldJobs; // Output port first, then the input it runs to
	/** The free belt ports of a building, as the belt tool describes them. */
	void BeltPortsOf(AFGBuildable* Building, TArray<Atlas::FPort>& Out) const;
	/** Whether the building in hand is a splitter or merger whose row can be a manifold, and which way it carries. */
	bool PlaceIsAttachment(bool& bOutSplitter) const;
	void FinishPlaceRow(bool bStopped);
	/** The game's blueprint group a row (and its manifold belts) is built into, so it dismantles as one. */
	TWeakObjectPtr<class AFGBlueprintProxy> PlaceGroup;
	/** Puts something just built from a row into the row's group, making the group with the first one. */
	void GroupPlaced(AActor* Built, UClass* BuildClass, bool bLightweight, const FTransform& Where);
	/** Builds what the hologram holds. False, with the reason, when the game built nothing. */
	bool ConstructPlace(FText& OutError, AActor** OutBuilt = nullptr);
	bool TickPlace();
	bool SpawnPlaceHologram();
	void DestroyPlaceHologram();
	/** Puts the hologram where the build gun would land looking down at Spot. False when there is nothing there. */
	bool AimPlaceAt(const FVector& Spot);
	/** The hit the hologram was last aimed with, and the one the row being dragged started from. */
	FHitResult PlaceLastHit;
	/**
	 * Holding a foundation beside a visible foundation that stands higher than what is under the cursor: the hit on
	 * that foundation's side facing the cursor, so the new one snaps alongside it at its height, as aiming at a
	 * foundation's edge does with the build gun.
	 */
	bool FindFoundationSideHit(const FVector& Spot, double BelowZ, const AFGHologram* Live, FHitResult& Out) const;
	FHitResult RowOriginHit;
	/** The floor a row of machines stands on: the height most of its spots look down onto, in cm. */
	double RowFloorZ = 0.0;
	/** Looks down at each spot of the row and keeps the height most of them share, so one hole can't sink the row. */
	void PickRowFloor();
	/** Aims the hologram where the row's first piece went, then moves it along the row to Spot, keeping its height. */
	bool AimPlaceRowSpot(const Atlas::FPlaceSpot& Spot);
	void ValidatePlace();
	void BuildPlace();
	void TakeInHand(Atlas::EView View, UClass* Recipe, bool bSampled);
	/** The blueprint the build gun holds, and the recipe it builds with; null when it holds none. */
	UFGBlueprintDescriptor* BuildGunBlueprint(UClass*& OutRecipe) const;
	/** A map view was just opened: the build gun's blueprint, if any, goes in hand there. */
	void OnMapOpened(Atlas::EView View);
	FTSTicker::FDelegateHandle MapOpenTicker;
	/** Selects Actor, runs Action on it, then puts the previous selection back, like a Ctrl-click does. */
	bool ActOnActor(AFGBuildable* Actor, TFunctionRef<void()> Action);

	TSharedPtr<Atlas::FExportState> ExportState;

	// Rendered export (AtlasRender.cpp): the world captured from above, tile by tile, a few tiles a frame.
	void StartRenderExport();
	bool TickRenderExport(float DeltaSeconds);
	/** Ends a rendered export, waiting for writes still running. A failed one leaves no file. */
	void StopRenderExport(bool bFailed);
	TSharedPtr<Atlas::FRenderJob> RenderJob;
	FTSTicker::FDelegateHandle RenderTicker;

	FVector2f MapOrigin[int32(Atlas::EView::Count)] = {};
	FVector2f MapScale[int32(Atlas::EView::Count)] = {};
	/** Goes up whenever the grid changes, so the measure tool knows to look again. */
	uint32 GridChanges = 0;
	/** Goes up whenever the palette is rebuilt, which is when what the map shows changes. */
	uint32 PaletteBuilds = 0;

	// Status lights
	struct FStatusEntry
	{
		FVector4f Record = FVector4f::Zero(); // The building's record, paint bits cleared
		/** Written by the game's status event, which may fire off the game thread; read by UpdateStatusLights. */
		TSharedPtr<std::atomic<uint8>> Wanted;
		uint8 Shown = 0;
		FDelegateHandle Handle;
	};
	TMap<TWeakObjectPtr<AFGBuildable>, FStatusEntry> StatusEntries;
	TSharedPtr<Atlas::FGrid> StatusGrid;
	/** Set by any status event, so the lights are only looked through when something changed. */
	TSharedPtr<std::atomic<bool>> StatusChanged;
	bool bStatusActive = false;
	bool bStatusDirty = true;
	int8 StatusSafe = -1; // -1 not checked yet, 0 unsafe on this game version, 1 checked
	double LastStatusApplySeconds = 0.0;
	void UpdateStatusLights();
	void RebuildStatusLights();
	void ClearStatusLights();
	void AddStatusLight(AFGBuildable* Buildable);
	void RemoveStatusLight(AFGBuildable* Buildable);
	bool HasStatusLight(AFGBuildable* Buildable);
	bool IsStatusEventSafe();
	/** The building's single map record, for buildings drawn as one box. */
	bool RecordOf(AFGBuildable* Buildable, FVector4f& Out);
	void SetStatusRecord(const FVector4f& Record, uint8 Code, bool bAdd);

	// Measure tool
	Atlas::FMeasureView Measure[int32(Atlas::EView::Count)];
	/** The topmost visible foundation at a map point, for a measure box's grid. */
	bool FindFoundationAt(const FVector2f& Metres, FVector4f& Out) const;
	bool IsRecordShown(const Atlas::FDecoded& Record) const;
	void SnapDraft(Atlas::FMeasureView& State, const FVector2f& Metres) const;
	FVector2f CursorMetres(Atlas::EView View) const { return MapCursorUv[int32(View)] * Atlas::MapWidthMetres; }
};
