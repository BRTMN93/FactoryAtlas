"""Download SCIM's realistic map layer and re-encode it as JPEG tiles for Atlas.

The realistic layer is Satisfactory Calculator's (https://satisfactory-calculator.com) work, used in
Atlas with its author's permission. It is not ours to redistribute anywhere else: the tiles and the
assets made from them stay out of git (Saved/ and Content/Realistic/ are ignored) and ship only
inside the packaged mod.

    python -I Scripts/fetch_realistic_tiles.py [out_dir]     (default: Saved/RealisticTiles)

SCIM serves 256 px Leaflet tiles, levels 3 to 8, over a 40960 px raster: Atlas's map rectangle plus
a 4096 px border on every side. Level 8 adds almost nothing over level 7 (measured 2026-11-15: on
land, level 8 differs from level 7 enlarged by about 41 dB), so levels 3 to 7 are fetched, and only
tiles that touch Atlas's map rectangle. The PNGs are lossless RGBA but always opaque; as JPEG they
are about a tenth of the size.

Already downloaded tiles are skipped, so the script can be stopped and run again.
"""
import io
import pathlib
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

from PIL import Image

URL = "https://static.satisfactory-calculator.com/imgMap/realisticLayer/Stable/{z}/{x}/{y}.png"
LEVELS = range(3, 8)
QUALITY = 85
WORKERS = 4  # Their CDN, their bandwidth: keep it gentle

RASTER = 40960          # Pixels across the whole layer at level 8
BORDER = 4096           # Pixels of padding around Atlas's map rectangle at level 8


def tiles_for(z):
    """The tile columns (and rows: the layer is square) at level z that touch the map rectangle."""
    tile = 256 << (8 - z)  # Level-8 pixels per tile at this level
    first = BORDER // tile
    last = (RASTER - BORDER - 1) // tile
    return range(first, last + 1)


def fetch(z, x, y, out, stats, lock):
    path = out / str(z) / str(x) / f"{y}.jpg"
    if path.exists():
        with lock:
            stats["skipped"] += 1
        return
    request = urllib.request.Request(URL.format(z=z, x=x, y=y), headers={"User-Agent": "FactoryAtlas tile import"})
    for attempt in range(4):
        try:
            data = urllib.request.urlopen(request, timeout=30).read()
            break
        except urllib.error.HTTPError as error:
            if error.code == 404:
                with lock:
                    stats["missing"] += 1
                return
            time.sleep(2 ** attempt)
        except Exception:
            time.sleep(2 ** attempt)
    else:
        with lock:
            stats["failed"] += 1
        print(f"  failed {z}/{x}/{y}", flush=True)
        return

    image = Image.open(io.BytesIO(data)).convert("RGB")
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    image.save(temp, "JPEG", quality=QUALITY, optimize=True)
    temp.replace(path)
    with lock:
        stats["fetched"] += 1
        stats["bytes"] += path.stat().st_size


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "Saved" / "RealisticTiles"
    out.mkdir(parents=True, exist_ok=True)

    jobs = [(z, x, y) for z in LEVELS for x in tiles_for(z) for y in tiles_for(z)]
    print(f"{len(jobs)} tiles to {out}", flush=True)
    stats = {"fetched": 0, "skipped": 0, "missing": 0, "failed": 0, "bytes": 0}
    lock = threading.Lock()
    started = time.time()
    with ThreadPoolExecutor(WORKERS) as pool:
        futures = [pool.submit(fetch, z, x, y, out, stats, lock) for z, x, y in jobs]
        for n, future in enumerate(futures, 1):
            future.result()
            if n % 250 == 0:
                print(f"  {n}/{len(jobs)}  {stats}  {time.time() - started:.0f}s", flush=True)

    total = sum(p.stat().st_size for p in out.rglob("*.jpg"))
    print(f"done in {time.time() - started:.0f}s: {stats}; {total / 1048576:.1f} MB of JPEG on disk", flush=True)
    return 1 if stats["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
