#include "AtlasEditorTools.h"

#include "AtlasRealistic.h"
#include "FactoryAtlas.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

bool UAtlasEditorTools::ConnectCustomizedUV(UMaterial* Material, UMaterialExpression* Expression, int32 Index)
{
#if WITH_EDITOR
	if (!Material || !Expression || Index < 0 || Index > 7)
	{
		return false;
	}
	FExpressionInput* Input = Material->GetExpressionInputForProperty(EMaterialProperty(MP_CustomizedUVs0 + Index));
	if (!Input)
	{
		return false;
	}
	Input->Connect(0, Expression);
	return true;
#else
	return false;
#endif
}

int32 UAtlasEditorTools::CreateTileLevel(const FString& PackageName, const FString& TilesDir, int32 Level)
{
#if WITH_EDITOR
	if (Level < 3 || Level > 7 || !FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogFactoryAtlas, Error, TEXT("CreateTileLevel: bad level %d or package name %s"), Level, *PackageName);
		return -1;
	}

	// The block of SCIM tiles touching Atlas's map rectangle; the same sums as fetch_realistic_tiles.py.
	const int32 Span = 256 << (8 - Level);
	const int32 First = 4096 / Span;
	const int32 Last = (40960 - 4096 - 1) / Span;
	const int32 Count = Last - First + 1;

	TArray<int32> Offsets;
	TArray<uint8> Data;
	Offsets.Reserve(Count * Count + 1);
	int32 Tiles = 0;
	for (int32 Y = First; Y <= Last; Y++)
	{
		for (int32 X = First; X <= Last; X++)
		{
			Offsets.Add(Data.Num());
			const FString File = FPaths::Combine(TilesDir, FString::FromInt(Level), FString::FromInt(X), FString::Printf(TEXT("%d.jpg"), Y));
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent) && Bytes.Num() > 0)
			{
				Data.Append(Bytes);
				Tiles++;
			}
		}
	}
	Offsets.Add(Data.Num());
	if (Tiles == 0)
	{
		UE_LOG(LogFactoryAtlas, Error, TEXT("CreateTileLevel: no tiles for level %d under %s"), Level, *TilesDir);
		return -1;
	}

	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	const FString Name = FPackageName::GetShortName(PackageName);
	UAtlasTileSet* Set = FindObject<UAtlasTileSet>(Package, *Name);
	if (!Set)
	{
		Set = NewObject<UAtlasTileSet>(Package, *Name, RF_Public | RF_Standalone);
	}
	Set->Level = Level;
	Set->First = First;
	Set->Count = Count;
	Set->Offsets = MoveTemp(Offsets);
	Set->Data = MoveTemp(Data);
	Set->MarkPackageDirty();

	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.Error = GLog;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Set, *Filename, Args))
	{
		UE_LOG(LogFactoryAtlas, Error, TEXT("CreateTileLevel: could not save %s"), *Filename);
		return -1;
	}
	UE_LOG(LogFactoryAtlas, Display, TEXT("CreateTileLevel: level %d, %d of %d tiles, %.1f MB -> %s"),
		Level, Tiles, Count * Count, Set->Data.Num() / 1048576.0, *Filename);
	return Tiles;
#else
	return -1;
#endif
}
