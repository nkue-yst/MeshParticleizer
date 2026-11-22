#include "MeshParticleizerPreview.h"
#include "MeshParticleizerPreset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

bool UMeshParticleizerPreview::ApplyPreset(UMeshParticleizerPreset* Asset, FString& OutError)
{
	if (this->bClosed == true || Asset == nullptr)
	{
		this->Error = OutError = TEXT("Select a preset in an open preview.");
		return false;
	}
	auto Settings = FMeshParticleizeOptions{};
	if (Asset->CopyOptions(Settings, OutError) == false || this->SetOptions(Settings, OutError) == false)
	{
		this->Error = OutError;
		return false;
	}
	this->Preset = Asset;
	return true;
}

UMeshParticleizerPreset* UMeshParticleizerPreview::SavePreset(const FString& PackagePath, FString& OutError)
{
	if (this->bClosed == true || PackagePath.StartsWith(TEXT("/Game/")) == false || FPackageName::IsValidLongPackageName(PackagePath) == false || FPackageName::DoesPackageExist(PackagePath) == true || FindPackage(nullptr, *PackagePath) != nullptr)
	{
		this->Error = OutError = TEXT("Choose a new /Game/ asset path. Existing assets are not overwritten.");
		return nullptr;
	}
	if (UMeshParticleizerPreset::ValidateSettings(this->Options, OutError) == false)
	{
		this->Error = OutError;
		return nullptr;
	}

	// 書込み成功まで公開しない。失敗時も元の再生設定と選択中のプリセットを保つ。
	auto* Package = CreatePackage(*PackagePath);
	auto Asset = TStrongObjectPtr<UMeshParticleizerPreset>(NewObject<UMeshParticleizerPreset>(Package,
		*FPackageName::GetLongPackageAssetName(PackagePath), RF_Public | RF_Standalone));
	Asset->SetOptions(this->Options, OutError);
	auto Args = FSavePackageArgs{};
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const auto Filename = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	if (UPackage::SavePackage(Package, Asset.Get(), *Filename, Args) == false)
	{
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		Package->SetDirtyFlag(false);
		this->Error = OutError = TEXT("Could not save the preset. Check the destination folder and try a new name.");
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Asset.Get());
	this->Preset = Asset.Get();
	this->Error.Reset();
	OutError.Reset();
	return Asset.Get();
}
