#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AtlasRealistic.generated.h"

class FSlateRect;
class FSlateWindowElementList;

/**
 * One zoom level of the realistic map layer, as JPEG tiles.
 *
 * The imagery is Satisfactory Calculator's (https://satisfactory-calculator.com), used with its author's
 * permission and credited wherever it is shown. Scripts/fetch_realistic_tiles.py downloads it and
 * Scripts/create_realistic_assets.py packs each level into one of these. The assets are not in git.
 *
 * Tiles follow SCIM's own grid: 256 px each, over a raster of 40960 px at level 8 that is Atlas's map
 * rectangle plus a 4096 px border all round. A level holds the square block of tiles touching the map.
 */
UCLASS()
class FACTORYATLAS_API UAtlasTileSet : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	int32 Level = 0;

	/** SCIM tile column and row of the block's north-west tile. */
	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	int32 First = 0;

	/** Tiles along each side of the block. */
	UPROPERTY(VisibleAnywhere, Category = "Atlas")
	int32 Count = 0;

	/** Count * Count + 1 offsets into Data, row by row. Tile i is Data[Offsets[i], Offsets[i + 1]); empty means none. */
	TArray<int32> Offsets;
	TArray<uint8> Data;

	/** Offsets and Data are serialized by hand: as properties, 45 MB of bytes would go through one by one. */
	virtual void Serialize(FArchive& Ar) override;

	/** The JPEG bytes of SCIM tile X, Y, or an empty view when the block has no such tile. */
	TConstArrayView<uint8> GetTile(int32 X, int32 Y) const;
};

namespace Atlas::Realistic
{
	/** Shown on the map while the layer is on, and next to its setting. The permission to use it asked for this. */
	extern const TCHAR* const Credit;

	/**
	 * Draws the realistic layer for a view whose map UV 0..1 spans Origin to Origin + Scale in paint space.
	 * Tiles are clipped to the map rectangle and the culling rect, streamed in as they come into view, and
	 * drawn over coarser ones until they are ready. Uses a few layers from LayerId up and returns the last one,
	 * or INDEX_NONE while nothing can be drawn yet (the assets are still loading, or missing from this build),
	 * so the caller can show something else.
	 */
	int32 Paint(FVector2f Origin, FVector2f Scale, const FSlateRect& CullingRect, FSlateWindowElementList& Out, int32 LayerId);

	/** The credit line, in the bottom-left corner of Area. DrawScale is the view's DPI scale. */
	void PaintCredit(const FSlateRect& Area, float DrawScale, FSlateWindowElementList& Out, int32 LayerId);

	/** False once a level failed to load: the build was made without the imagery. */
	bool IsAvailable();

	/** Frees every tile and level. They load again the next time the layer is drawn. */
	void Release();

	/**
	 * One zoom level, loaded on the spot for an export, or null when this build has no imagery. Game thread only;
	 * the caller keeps it alive. Level L is a raster of 160 << L px in TileSide px tiles: Atlas's map rectangle,
	 * MapPixelsOf(L) wide, with BorderPixelsOf(L) all round it.
	 */
	UAtlasTileSet* LoadLevelNow(int32 Level);
	constexpr int32 TileSide = 256;
	constexpr int32 FinestLevel = 7;
	constexpr int32 CoarsestLevel = 3;
	inline int32 MapPixelsOf(int32 Level) { return 32768 >> (8 - Level); }
	inline int32 BorderPixelsOf(int32 Level) { return 4096 >> (8 - Level); }
}
