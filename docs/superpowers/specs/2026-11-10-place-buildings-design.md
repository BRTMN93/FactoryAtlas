# Placing buildings from the Atlas map

Date: 2026-11-10. Status: approved, not started. The user delegated approval to Claude before going offline, so the calls marked **(my call)** have not been checked by them yet.

Decisions the user made are stated plainly. Anything marked **(my call)** was filled in by Claude and can be changed.

## Intent

Drop machines onto foundations you have already built, from the map, instead of flying over with the build gun.
Most of a factory is rows of the same machine on the same recipe, so the fast path is: sample a machine on the map,
then click to place copies of it, each one coming out already set up with the same recipe, clock speed and paint.

Like belts and wires from the map, this is convenience at a distance, not a cheat. The map never builds anything
the build gun would refuse, and it charges what the build gun would charge.

## Goals

- A place mode on the map. While it is on, a ghost of the chosen building follows the cursor, R rotates it, and a
  left click builds it.
- The building lands on the floor the height slider is showing: Atlas looks straight down at the cursor from the
  top of the slider's range, the way wire mode already finds the ground for a pole.
- The ghost is green when the game would accept it and red when it would refuse, with the game's own reason by the
  cursor. Clicking on red does nothing but show the reason.
- Snapping, rotation steps, clearance and validity are all the game hologram's, so a machine placed on a foundation
  sits on the foundation grid exactly as it would with the build gun.
- Two ways to choose what to place:
  - **Sample:** click an existing machine on the map to load it, with its recipe, clock speed and paint. Every copy
    placed gets those settings.
  - **Picker:** a plain list of unlocked buildings, for anything not on the map yet. These place with default
    settings.
- After a build, place mode stays on with the same building ready, so a row of ten constructors is ten clicks.
  Right click drops the building in hand, Esc leaves place mode. **(my call, matching the wire tool)**
- Paid like belts and wires: player inventory first, then the Dimensional Depot.
- Works in both the game map and the separate map window (`Atlas::EView::Game` and `EView::Window`).

## Non-goals

- Foundations, walls, ramps, roofs, beams and other structural pieces. The user builds floors themselves.
- Belts, lifts, pipes, hypertubes, rail and power lines. They already have their own map tools.
- Blueprints. Stamping them, with repeats like the measure tool's copies, is a natural next step but not this one.
- Moving, rotating or dismantling existing buildings.
- Auto-connecting belts, pipes or power to the new machine.
- Multiplayer clients. Single player and host only, the same check as belts (`CanBuildBelts`, AtlasBelts.cpp:451).
- Promising placement on bare terrain far from the player. See Open item 2.
- Pieces that attach to other buildings rather than stand on a floor: splitters and mergers onto belts, wall
  outlets, wall conveyor holes. If the game's hologram happens to accept them from a top-down hit, fine, but they
  are not promised. **(my call)**

## Anti-goals (would make this a failure even if it works)

- The ghost showing green for a spot the game then refuses, or red for one it would accept.
- Building without paying, or paying without building.
- A sampled copy that comes out with settings the game's own clipboard would not give it, for example a clock
  above 100% without power shards to back it.
- Any cost while not in place mode. The hologram exists only while place mode is on and is destroyed when it ends.
- Moving the player's camera, aim, or build gun state, or swapping what is in the build gun.

## Constraints found in the code

- Building from the map already has a working pattern in `AtlasWires.cpp` and `AtlasBelts.cpp`:
  `AFGHologram::SpawnHologramFromRecipe` with the player's build gun, a hit fed to `UpdateHologramPlacement`, a few
  frames to settle (`WireSettle`, `bReady`) before the verdict is trusted, then `Construct` and `PayBuildCost`
  (AtlasBelts.cpp:1114). Place mode should reuse that lifecycle rather than start a new one.
- The ground trace for poles is at AtlasWires.cpp:419-452: build gun channel from `AtlasBeltDetail::FindBuildGunChannel`,
  top and bottom from the height filter, hologram and its children ignored, then
  `AtlasBeltDetail::ResolveForPlacement` so a hit on the abstract instance manager names the real building.
  Foundations are lightweight buildables, so that resolve step is what lets a machine hologram snap to them.
- Wires cap the top of the trace at 20 m above the wire's start. Placement has no start point, so the top is just
  the top of the height filter. With the slider fully open, that means the highest floor under the cursor, which
  is the behavior to expect and to say in the key hint. **(my call)**
- Settings already travel through the game's factory clipboard: `PlayerState->CopyFactoryClipboard` and
  `PasteFactoryClipboard` (AtlasCustomize.cpp:948, :1078), plus Atlas's own copy of customization data for paint.
  A sampled placement should apply settings through that same path, not by setting recipe and clock by hand, so
  whatever the game does with power shards and somersloops is what happens.
- Map clicks reach Atlas through `FAtlasClickProcessor` (AtlasMapHud.cpp, around :3466), which already swallows
  presses so the game's map does not pan or open the marker menu. Keys are handled at AtlasMapHud.cpp:829-870 and
  :3632 onwards. R and T are already used during a belt drag; place mode is exclusive with belt drags, wire mode and
  measure mode, so R can mean rotate while placing.
- The mode tiles (Wire, Measure) are built at AtlasMapHud.cpp:719 and only show when `CanBuildBelts()` is true.
- Unlocked recipes come from `AFGRecipeManager` (see AtlasBelts.cpp:105 and the `IsUnlocked` helper in
  AtlasCustomize.cpp:37). Modded buildings are in scope wherever their recipe is unlocked, as Atlas already draws
  them.
- Paint geometry is relative to the window being painted while the cursor is in desktop pixels; converting the
  cursor to map coordinates must use `GetTickSpaceGeometry()`, as the belt spec notes.

## Approaches

**A. The game's hologram does everything (picked).** Spawn the real hologram for the building, feed it a "build gun
pointing straight down here" hit as the cursor moves, and draw the ghost where the hologram actually ended up,
colored by the game's verdict. Sampled settings go on after `Construct`, through the factory clipboard. Costs CPU only
while placing. Snapping is whatever the game does with that hit, so whether rows line up like they do with the build
gun is found out in game (Open item 1).

**B. Atlas previews, the hologram confirms.** Atlas does its own fast grid snapping for a smooth ghost and asks the
hologram only when the cursor settles and on click. Smoother, but Atlas's guess and the game's answer can disagree,
so the ghost jumps or turns red late. That breaks the first anti-goal.

**Ruled out: Atlas spawns the building itself.** Fastest and fully controlled, but it skips the game's checks. That
is the shortcut the belt spec named as a failure, and it is how saves end up with overlapping machines.

The user picked A: exact game behavior is the whole promise, and belts and wires have already shown the pattern works.

## Decided

- Machines onto existing floors; structural pieces are out.
- Height from the height slider, looking straight down.
- Approach A.
- Sampling a machine carries its recipe, clock speed and paint into every copy; a picker covers buildings not on
  the map.
- Single player and host only.

## Left to the builder

- **Keys.** Alt + Q toggles place mode, mirroring the game's Q build menu and Atlas's Alt + W for wires, plus a
  Place tile next to Wire and Measure. Middle click on a machine samples it, as in the game. R rotates one step,
  Shift + R the other way. Space + drag pans, as in measure mode. All **(my call)**; middle click needs checking
  against whatever the game's map does with it.
- **The ghost's look.** Draw the hologram's footprint with the building's own map shape, tinted green or red, so it
  reads as the building it will become. A sampled ghost could show its recipe icon when zoomed in, like placed
  machines do.
- **The picker.** Unlocked building recipes whose product is not structural and not a belt, pipe, rail or wire,
  grouped by the game's build menu categories, with a search box. Where it lives (the right-hand panel or a popup by
  the Place tile) is open.
- **Terrain.** If the trace hits terrain and the hologram accepts it, build there: refusing it would be stricter than
  the build gun. If the trace hits nothing, show "Nothing to build on here." as wires do. **(my call, made while the
  user was offline. Earlier in the conversation this was put as "no foundation, red"; foundations-only is a small
  change if they want it back.)**
- **Rotation.** Use the hologram's own rotate input so the steps match the build gun, including any snapping to the
  foundation's 90 degree steps.
- **Sampling and the clipboard.** Middle click does what Ctrl + right click already does (copy the machine's settings
  and look) and also loads that building into place mode. So the clipboard and the building in hand never disagree,
  and Ctrl + left click pasting still works on the copies afterwards. **(my call)**
- **Buildings with no factory settings.** Sampling a storage container or anything else the factory clipboard does
  not cover carries only the building and its paint.
- **Cost.** Show the cost in the hint by the cursor, and turn red with the game's reason when it can't be paid, as
  belts and wires do.
- **Settle and throttle.** Re-place the hologram only when the cursor has moved to a new spot, and trust the verdict
  only after it has settled, as wires do.

## As built (unreleased, not yet tested in game)

Built on 2026-11-10 while the user was offline, in `Private/AtlasPlace.cpp` plus the HUD and painter. Where the
build differs from the notes above, the build is what stands:

- **Keys.** Alt + Q or the Place tile toggles place mode, opening the picker when nothing is in hand. Q opens and
  closes the picker. R turns a quarter turn, Shift + R the other way, Ctrl + R one fine step (the hologram's own
  rotation step, or 10 degrees). Left click builds, a left drag pans (as in wire mode, not Space + drag), right click
  puts down what is in hand, Esc closes the picker, then puts down, then leaves. Middle click on a machine takes it
  in hand, in place mode or not.
- **The ghost** is the footprint (the building's clearance box) as a translucent box with an outline and a tick
  toward the building's +X, pale until the game's verdict, then green or red. It is not drawn with the building's
  map shape, so a building never built before needs no new map type.
- **The picker** is a popup above the Place tile: groups Production, Extraction, Power, Logistics, Storage,
  Transport, Other, with a search box.
- **No clipping.** The "allow clipping" setting is for lines; a building from the map takes the game's verdict as
  it is, and an upgrade (placing over the same building) is refused.
- **Sampled settings** are pasted two frames after the building is built (open item 3), through the same paste as
  Ctrl + left click, by selecting the new building for a moment and putting the old selection back.

### Added after the first test (2026-11-10)

The user asked for these after trying it:

- **No hover highlight** while placing, and the ghost draws its belt, pipe and hypertube ports, read from the
  building's blueprint templates. Other buildings' free ports stay in view but can't be dragged from.
- **Search focus.** Alt + Q (and Q) opens the picker with the cursor in the search box; Enter picks the first match;
  Esc in the box closes the picker.
- **Rows.** A left drag with a building in hand lays out a row from the press point, along whichever of the
  building's sides the drag follows more closely, one footprint apart rounded up to whole metres, at most 40. One
  hologram walks the row checking each spot (green, red with the reason, pale while unchecked). Release builds the
  spots the game accepts, in order, one every few frames, so running out of materials stops it with the game's
  reason. Pan is now Space + drag while placing. **(my call: spacing by footprint, not by the foundation grid)**
- **Foundations.** `AFGBuildableFoundation` and subclasses (ramps included) are placeable. They are lightweight
  instances, so a build is confirmed by the instance count going up rather than by `Construct` returning an actor.
  A sampled building's look is put on the hologram (`SetCustomizationData`), so foundations are built in the
  sample's paint without a paste afterwards. Walls stay out.

### Dismantle mode and manifolds (2026-11-10)

Asked for directly; built without a separate design pass.

- **Dismantle** (`Private/AtlasDismantle.cpp`): Alt + F or the Dismantle tile. Click picks or unpicks, a drag picks
  every building with a shape centred in the box that the map is showing (layers and height filter respected),
  Shift + drag unpicks. Picked shapes are drawn as `BlockedType` records. Enter dismantles through
  `IFGDismantleInterface` (CanDismantle, dismantle disqualifiers with the whole pick as context, child actors, refund,
  Dismantle); foundations and walls through `FLightweightBuildableInstanceRef::Remove()` with their recipe as the
  refund. Refunds go to the inventory, overflow into a crate by the player via
  `FDismantleHelpers::DropRefundOnGroundNoActor`. **(my calls: 1000 per pick, Enter to confirm, crate by the player
  rather than at the buildings)**
- **Manifolds**: Ctrl while dragging a splitter or merger row. Spots go opposite each free machine port (inputs for
  splitters, outputs for mergers) that faces the drag line within 30 m and 6 m of height; with none, the row is
  evenly spaced. After the row is built, belts run between neighbouring pieces (the output facing the next piece to
  the input facing back, one flow direction for the whole row) and from each piece's free side port to the nearest
  facing machine port. Each belt goes through the belt tool's own hologram as an automatic drag (`StartAutoBelt`),
  one at a time.

### Blueprints from the build gun (2026-11-11)

Asked for directly: "choose a blueprint normally, then open the map and place it".

- Atlas never picks the blueprint itself. When a map view opens (a paint after 30 or more frames without one), it
  reads `UFGBuildGunStateBuild::GetActiveBlueprintDescriptor()` and takes that blueprint in hand when the build gun
  is equipped, in its build state (or just left it for none, in case the map moves it there), and its active recipe
  is `UFGBlueprintSettings::mBlueprintRecipeClass`. Alt + Q with nothing in hand does the same. The build gun is
  not touched. **(my call: on map open, not only on Alt + Q)**
- The hologram is `SpawnHologramFromRecipe` with the shared blueprint recipe and a pre-spawn hook calling
  `AFGBlueprintHologram::SetBlueprintDescriptor`, so the blueprint is set before the hologram begins play.
- Ghost: the blueprint's box, `GetDimensionsOnInstance()` foundations of 8 m, centred on the actor. The hologram's
  local component bounds are logged beside it to check the centring, and used only if the size is zero.
- Rows work as for machines. Blueprints skip Atlas's row group, since each is already its own blueprint group.
- Not checked yet: whether the build gun is still in its build state while the map is open, whether
  `DoMultiStepPlacement` finishes a blueprint in one step, and whether the box is centred on the actor.

## Open items, to check in game

1. Whether a machine hologram given a top-down hit lines up with its neighbours (the build gun's guidelines) and
   snaps to the foundation grid. If it snaps but does not show guidelines, decide whether Atlas draws them.
2. Whether bare terrain far from the player has collision loaded. Foundations are expected to be fine.
3. Whether the factory clipboard paste works on a machine in the same frame it was built, or needs to wait a tick.
4. What the clipboard does with an overclocked sample when the player has no spare power shards: the copy should get
   whatever the game gives it, and the toast should say so if it is less than the sample.
5. Middle click on the game's map: confirm the game does nothing with it before using it for sampling.
