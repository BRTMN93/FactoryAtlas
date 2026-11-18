#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AtlasEditorTools.generated.h"

class UMaterial;
class UMaterialExpression;

/// Python cannot name the hidden MP_CustomizedUVs enum entries, so create_assets.py wires them through here.
UCLASS()
class FACTORYATLAS_API UAtlasEditorTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "Factory Atlas")
	static bool ConnectCustomizedUV(UMaterial* Material, UMaterialExpression* Expression, int32 Index);

	/**
	 * Packs one level of realistic map tiles, TilesDir/<Level>/<x>/<y>.jpg as written by fetch_realistic_tiles.py,
	 * into a UAtlasTileSet saved as PackageName. Python cannot fill 45 MB of bytes quickly, hence C++.
	 * Returns how many tiles went in, or -1 on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "Factory Atlas")
	static int32 CreateTileLevel(const FString& PackageName, const FString& TilesDir, int32 Level);
};
