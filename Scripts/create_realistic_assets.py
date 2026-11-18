# Packs the realistic map tiles into /FactoryAtlas/Realistic/T_Realistic_L3..L7. Re-runnable.
# Run fetch_realistic_tiles.py first; it writes the tiles to <repo>/Saved/RealisticTiles.
#   UnrealEditor-Cmd.exe C:\SF\SML\FactoryGame.uproject -run=pythonscript
#     -script=C:\SF\Mods\FactoryAtlas\Scripts\create_realistic_assets.py -unattended -nosplash -NoLogTimes -abslog=<log>
#
# The imagery is Satisfactory Calculator's, used in Atlas with permission. The assets made here are ignored by
# git (Content/Realistic/) and ship only inside the packaged mod.
import os
import unreal

plugin = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir() + "Mods/GameFeatures/FactoryAtlas/")
tiles = os.environ.get("ATLAS_REALISTIC_TILES") or os.path.join(plugin, "Saved", "RealisticTiles")
if not os.path.isdir(tiles):
    unreal.log_error(f"ATLAS: no tiles at {tiles}; run Scripts/fetch_realistic_tiles.py first")
else:
    total = 0
    for level in range(3, 8):
        n = unreal.AtlasEditorTools.create_tile_level(f"/FactoryAtlas/Realistic/T_Realistic_L{level}", tiles, level)
        if n < 0:
            unreal.log_error(f"ATLAS: realistic level {level} failed")
        else:
            total += n
            unreal.log(f"ATLAS: realistic level {level}: {n} tiles")
    unreal.log(f"ATLAS: realistic assets done, {total} tiles")
