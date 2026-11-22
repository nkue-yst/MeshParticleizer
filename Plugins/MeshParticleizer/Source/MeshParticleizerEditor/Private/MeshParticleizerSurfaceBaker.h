#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerSurfaceColor.h"

class UStaticMesh;
class USkeletalMesh;
class UMaterialInterface;
struct FMeshDescription;
class IMaterialBakingModule;

namespace MeshParticleizerPreparation
{
	// Descriptionは呼出側がベイクと保存の間保持する。材質と骨の識別子は元資産に対応する。
	struct FSurfaceSource
	{
		UObject* Asset = nullptr;
		const FMeshDescription* Description = nullptr;
		TArray<UMaterialInterface*> Materials;
		TArray<FName> MaterialSlotNames;
		FString Fingerprint;
		FBox Bounds;
		TArray<FLinearColor> SkinVertices;
		TArray<FLinearColor> SkinTriangles;
		int32 BoneCount = 0;
		int32 SamplingLOD = 0;
		TArray<int32> EnabledSections;
		bool bUseUnlitEmissive = false;
		bool bBakeSourceEmission = false;
	};

	// 資産の保存と切り離し、同じベイク経路を準備ツールと精度検証から使う。
	struct FSurfaceSamples
	{
		TArray<FLinearColor> Positions;
		TArray<FLinearColor> Normals;
		TArray<FLinearColor> Colors;
		TArray<FLinearColor> Emission;
		TArray<FLinearColor> Coordinates;
		TArray<FLinearColor> CoverageTriangles;
		int32 CoverageTriangleCount = 0;
		TArray<UMaterialInterface*> Materials;
		double SurfaceArea = 0;
		TArray<FVector> SurfaceAreaVectors;
		TArray<FMeshParticleizerTextureColorSource> ColorSources;
		int32 Width = 512;
		int32 Height = 0;
	};

	bool BakeSurfaceSamples(UStaticMesh* Mesh, int32 SampleCount, int32 Seed, FSurfaceSamples& OutSamples, FString& OutError,
		bool bUseUnlitEmissive = false, bool bBakeSourceEmission = false);
	bool BakeSurfaceSamples(const FSurfaceSource& Source, int32 SampleCount, int32 Seed, FSurfaceSamples& OutSamples, FString& OutError);
	bool BuildStaticSource(UStaticMesh* Mesh, FMeshDescription& OutDescription, FSurfaceSource& OutSource, FString& OutError, int32 SamplingLOD = 0);
	bool BuildSkeletalSource(USkeletalMesh* Mesh, FMeshDescription& OutDescription, FSurfaceSource& OutSource, FString& OutError, int32 SamplingLOD = 0);
	bool AnalyzeSurfaceDependencies(UMaterialInterface* Material, bool bUseUnlitEmissive,
		FMeshParticleizerTextureColorSource& OutSource, FString& OutError);
	bool AnalyzeSurfaceMaterials(TConstArrayView<UMaterialInterface*> Materials, bool bUseUnlitEmissive, bool bBakeSourceEmission,
		TArray<FMeshParticleizerTextureColorSource>& OutSources, FString& OutError);
	bool ValidateSourceEmissionDependencies(UMaterialInterface* Material, FString& OutError);
	bool ReadEmissionBakeState(IMaterialBakingModule& Baker, bool& bOutEnabled, FString& OutError);
}
