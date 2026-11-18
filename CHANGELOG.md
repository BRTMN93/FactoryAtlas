# Changelog

## 0.6.0

**Basic map**
- Mods > Atlas > Basic map turns the map into just the buildings. The hover label and highlight, recipe icons,
  status lights, ports, collectibles, resources and hazards are all off, and the map takes no clicks or tool keys:
  no settings, copying, pasting, building or tools, and the tool tiles and key hints along the bottom are gone.
  Zooming, panning, Alt + scroll floors and the Atlas panel still work.
- The Atlas panel opens minimized while Basic map is on. Show opens it as usual.
- Your other settings stay as they were and come back when Basic map is turned off.

**Rendered map export**
- Settings > Map image has a new choice, Flat or Rendered. Rendered exports every building the way the game draws
  it, seen from straight above, instead of as colored shapes. With the realistic terrain on, the buildings sit on
  Satisfactory Calculator's map; with the game's map they sit on a transparent background.
- The whole world is captured in tiles, skipping the parts with no buildings, and the image is written while it
  renders. It takes a few minutes, and the game stutters until it is done. The panel shows which tile it is on.
- Rendered exports draw every building, whatever the layers and height slider show. Lighting is the game's at the
  time of the export, so export during the day.

**Bigger exports**
- Map images can now be 32K (32768 px a side, about 23 cm a pixel) as well as 4K and 8K, flat or rendered. Exports
  are written a band at a time, so even 32K needs only a few hundred MB of memory. A 32K image is a gigapixel, and
  not every image viewer opens one.

## 0.5.1

**Fixes**
- Fixed a crash to desktop that could happen now and then when opening the map with the realistic terrain on. The
  realistic map's tiles could reach the graphics card before they were fully made, which crashed the game.

**Compatibility**
- Roads built with [Road Maker](https://ficsit.app/mod/RoadMaker) are left off Atlas's map for now. That mod draws
  its roads on the map itself, and the two drawn on top of each other looked wrong. They don't appear in the layer
  list either.

**Settings**
- The Status lights setting is now called Status lights when zoomed in, and its description says so. That is how
  the lights already behaved in 0.5.0.

## 0.5.0

**Realistic terrain**
- Realistic is the default terrain and the first choice under Settings > Terrain. It puts Satisfactory
  Calculator's realistic map under the buildings instead of the game's own map image, in the map screen and in
  the second window. Used with permission from Satisfactory Calculator (satisfactory-calculator.com), and
  credited on the map while it is on.
- Tiles stream in as they come into view, sharper as you zoom in, with the coarser image filling in until they're
  ready. It shows the whole map, explored or not.
- With it on, the second window shows terrain without the in-game map having been opened first.

**Hiding ports**
- Mod settings > Atlas > Ports turns the map's free port markers on or off by kind: belt, pipe, hypertube and
  railway. It covers ports on machines as well as on belt and pipe ends. A hidden kind can't be dragged from the map
  until it's shown again.

**Floors**
- Alt + mouse wheel over the map steps through the floors of the factory under the cursor: up a floor, down a
  floor, and past the top or bottom back to every height. A floor is a height where foundations stand around the
  cursor; it shows its own foundations and everything on them, up to the next floor. The height slider follows.

**Connecting power in bulk**
- Press Alt + C or click the Connect tile. Pick buildings the way dismantle mode does: click them, drag a box,
  Shift + drag to take them back out. Only buildings with a power connection are picked; power switches are left out
  so their two sides stay apart.
- Atlas plans the shortest wiring that puts everything picked on one circuit and draws it on the map. Buildings
  already on the same circuit aren't wired again, rows chain along straight lines, and power poles become hubs.
  Free connection slots and the game's wire length are respected. Whatever can't be reached shows red, with a hint
  to place a pole between.
- The card shows how many power lines, how long, and what they cost. Enter builds them, paid from your inventory
  and then the Dimensional Depot; if materials run out it builds the shortest joins first. Single player and host
  only.

**Changing recipes in bulk**
- Press Alt + R or click the Recipes tile. Pick machines the way dismantle mode picks buildings: click them, drag a
  box over them, Shift + drag to take them back out. Only machines that run a recipe are picked. They are
  highlighted, and the card lists them by kind and recipe.
- A list above the hint bar shows each picked kind of machine with the recipes it can run, and how many already run
  each. Click a recipe to switch every picked machine of that kind to it.
- Each machine changes the way it does at its own screen: its items go to your inventory first. Machines already on
  that recipe are left alone. Single player and host only.

**Fixes**
- The parts the HUB comes with (storage box, locker, work bench, terminal and the like) no longer show on the map
  as buildings of their own.

**Upgrading a belt line**
- Click a belt or lift on the map, then Upgrade on its card. Atlas follows the items downstream from there and
  highlights every belt and lift on the way; the card says how many, how long, and where the line ends. Pick a tier
  to replace them all.
- Through splitters and mergers (off by default) carries the trace on down every output, so a whole manifold goes
  in one click.
- Each tier shows what it would replace and what that costs, less what the old belts give back. Lifts change to the
  lift of the same speed, and stay as they are when that lift isn't unlocked yet.
- Uses the game's own upgrade, as the build gun does: items stay on the belts and connections carry over. The cost
  comes from your inventory, then the Dimensional Depot; it stops cleanly if you run out. What comes back goes to
  your inventory, and anything that doesn't fit is left in a crate next to you.
- Belts with a throughput monitor are left for the build gun. At most 2000 belts and lifts per line. Single player
  and host only.

**Dismantling from the map**
- Press Alt + F or click the Dismantle tile. Click buildings to pick them, drag a box to pick everything the map shows
  inside it, Shift + drag to take buildings back out. Picked buildings turn red and the card shows what you'll get
  back. Enter dismantles them, right click clears the pick, Space + drag pans.
- Uses the game's own dismantle: a building the game won't let go of stays, with its reason. The refund goes to your
  inventory, and anything that doesn't fit is left in a crate next to you. At most 1000 buildings per pick. Hide
  layers or narrow the height slider to keep a box off things you want to keep.

**Manifolds**
- Hold Ctrl while dragging a row of splitters or mergers: each one lines up opposite a free machine port along the
  drag, and once built they are belted to each other and to the machines, through the game's own belt hologram, at
  your chosen belt tier. Splitters feed machine inputs; mergers take from machine outputs.

**Placing buildings from the map**
- Press Alt + Q or click the Place tile, pick a building, and click on the map to build it. It lands on the floor the
  height slider shows, looking straight down at the cursor. R turns it a quarter turn, Shift + R the other way,
  Ctrl + R one fine step.
- Middle click a machine on the map to take it in hand with its settings: every copy you place comes out with the
  same recipe, clock speed and paint, pasted the way Ctrl + left click pastes them.
- A ghost shows where it will go: green when the game would build it, red with the game's reason when not. Placing
  goes through the game's own hologram and costs what the build gun would, from your inventory and then the
  Dimensional Depot.
- Drag to build a row: from where you press, along the building's side, one building apart. Each spot turns green
  or red as the game checks it, and letting go builds the green ones. Space + drag pans while placing.
- A row stays at the height of its first piece, like the game's zoop: foundations can run out over open air, and
  machines need something to stand on at that height.
- Blueprints: choose a blueprint in the build gun as usual, then open the map. It is in hand straight away, with a
  ghost of its box, and places like a building: click to build, R to turn, drag for a row. Alt + Q also takes it
  when nothing else is in hand. Built through the game's own blueprint hologram, at the blueprint's cost.
- A foundation held just past the edge of a floor snaps alongside it at the floor's height, instead of dropping to
  the ground below.
- A row is built as one blueprint group (with a manifold's belts), so the game's blueprint dismantle takes it in one
  go, and clicking one piece in Atlas's dismantle mode picks the whole group.
- R turns the building in hand again after picking it from the list.
- The picker opens with its search box ready: type a name and press Enter for the first match.
- The ghost shows the building's belt, pipe and hypertube ports, and other buildings' free ports stay in view.
- Machines, generators, storage, splitters, poles, foundations and ramps. Walls and other structure are left to the
  build gun. Middle click a foundation to place more of it in the same paint. Single player and host only.

## 0.4.0

**Building from the map**
- Power wires: press Alt + W or click the Wire tile, then click one building and another to run a power line
  between them. Click the ground to place a power pole there. Each wire starts the next from where it ended, so a
  run of clicks chains buildings together. Right click drops the wire, Esc stops.
- Belts, pipes, hypertubes and railway: drag from a free port to another port, or to the ground, to build a line
  between them. R cycles the build mode, T the tier (Mk).
- Everything built from the map goes through the game's own holograms, so length limits, free connections and the
  cost are the build gun's. The cost comes from your inventory, then the Dimensional Depot.
- Allow clipping (on by default): lines built from the map may pass through other buildings.
- Single player and host only.

**Measure tool**
- Press G or click the Measure tile, then drag to measure a box. Its corners snap to the foundation grid under where
  the drag starts, so rotated builds measure in whole foundations. The label shows metres, foundations, floors and
  the height range of what is inside.
- Copies: tile up to 16 outlines of the box in any direction. A copy that would land on existing buildings turns
  red, so you can see where a build can be repeated. Hidden layers don't count as being in the way.
- Shift + drag measures a straight line. Space + drag pans the map while measuring.

**Status lights**
- A small light in each machine's corner: green producing, purple overclocked, yellow standby, red error. They show
  once the map is zoomed in, from the same zoom as recipe icons. The hover label shows the status in words.
- On by default. Turn them off under Mods > Atlas > Map.

**Map**
- Second window: the whole Atlas map in a separate window, for a second monitor, with its own pan and zoom.
- Mod settings menu (Mods > Atlas) for the display options: hover label, height, coordinates, highlight, building
  outlines, recipe icons, status lights.
- Spoiler-free option that hides collectibles, resource nodes, wells, spore flowers, gas pillars and radioactivity.
- Copy and paste only between buildings it fits: a machine's settings go wherever the game's own clipboard takes
  them, and a look copied from a foundation or wall goes onto others of the same category.
- The shortcut bar sits above the game's marker bar.

## 0.2.0

- Belts, pipes, rails, roads and wires are drawn as overlapping capsules, so paths read as one smooth band.
- The map shortcuts show as a key hint bar along the bottom of the map.
- The panel no longer flashes on clicks.
- Fullscreen: the map no longer runs past the bottom of the screen.

## 0.1.0

- First release. Every building drawn live on the in-game map with GPU instancing, with layers, a height filter,
  custom and paint colors, hover info, building configuration, collectibles, resource nodes, radioactivity and PNG
  export.
