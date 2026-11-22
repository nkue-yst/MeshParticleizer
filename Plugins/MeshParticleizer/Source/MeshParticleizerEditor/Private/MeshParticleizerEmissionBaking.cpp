#include "MeshParticleizerSurfaceBaker.h"
#include "IMaterialBakingModule.h"
#include "MaterialBakingStructures.h"
#include "Materials/Material.h"

bool MeshParticleizerPreparation::ReadEmissionBakeState(IMaterialBakingModule& Baker, bool& bOutEnabled, FString& OutError)
{
	// UEに共有HDR設定のgetterがないため、公開APIの1画素出力から現在の状態を読む。
	// 既定値や他プラグインの使用履歴を仮定せず、変更前の状態を復元できる場合だけ進む。
	auto Material = FMaterialData{};
	Material.Material = UMaterial::GetDefaultMaterial(MD_Surface);
	Material.PropertySizes.Add(MP_EmissiveColor, FIntPoint(1, 1));
	Material.bPerformShrinking = false;
	Material.bPerformBorderSmear = false;
	auto Mesh = FMeshData{};
	Mesh.TextureCoordinateBox = FBox2D(FVector2D::ZeroVector, FVector2D(1, 1));
	auto Output = TArray<FBakeOutput>{};
	Baker.BakeMaterials({&Material}, {&Mesh}, Output);
	if (Output.Num() != 1 || Output[0].PropertyData.FindRef(MP_EmissiveColor).Num() != 1)
	{
		OutError = TEXT("Could not read the material baker's current HDR output state.");
		return false;
	}

	const auto* HDR = Output[0].HDRPropertyData.Find(MP_EmissiveColor);
	if (HDR != nullptr && HDR->Num() != 1)
	{
		OutError = TEXT("The material baker returned an unexpected HDR probe layout.");
		return false;
	}
	bOutEnabled = HDR != nullptr;
	return true;
}
