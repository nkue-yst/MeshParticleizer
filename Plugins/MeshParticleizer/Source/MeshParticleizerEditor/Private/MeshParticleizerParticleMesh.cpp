#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerSurfaceBaker.h"
#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerParticleStyle.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraDataInterfaceMeshParticleizerCollision.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"

namespace MeshParticleizerPreparation
{
	bool SaveAsset(UObject* Asset);

	bool CompileParticleSystems(UNiagaraSystem* ParticleSystem, UNiagaraSystem* GdfParticleSystem,
		int32 BoneCount, int32 Slots, FString& OutError)
	{
		// 通常・GDFの役割と容量モデルを先に照合し、未対応Rendererを保存しない。
		const auto Collision = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCollision::StaticClass()), TEXT("User.Collision"));
		for (auto* System : {ParticleSystem, GdfParticleSystem})
		{
			System->RequestCompile(false);
			System->WaitForCompilationComplete(true, false);
			auto Meshes = TArray<UStaticMesh*>{};
			for (const auto& Handle : System->GetEmitterHandles())
			{
				const auto* Emitter = Handle.GetInstance().GetEmitterData();
				if (Emitter == nullptr)
				{
					continue;
				}
				for (const auto* Renderer : Emitter->GetRenderers())
				{
					if (Renderer == nullptr)
					{
						continue;
					}
					auto Used = TArray<UObject*>{};
					Renderer->GetUsedMeshes(nullptr, Used);
					for (auto* Asset : Used)
					{
						if (auto* Mesh = Cast<UStaticMesh>(Asset))
						{
							Meshes.AddUnique(Mesh);
						}
					}
				}
			}
			FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
			const auto bHasCollision = System->GetExposedParameters().IndexOf(Collision) != INDEX_NONE;
			if (bHasCollision != (System == GdfParticleSystem))
			{
				OutError = TEXT("Choose a non-GDF system first and a GDF system second.");
				return false;
			}
			auto Memory = FMeshParticleizerGpuMemory{};
			if (FMeshParticleizerGpuMemory::Build(System, BoneCount, 0, Slots, Memory, OutError) == false)
			{
				return false;
			}
		}

		return true;
	}
}

UStaticMesh* UMeshParticleizerPreparationLibrary::CreateParticleMesh(const FString& AssetPath, bool bFlake, FString& OutError)
{
	OutError.Reset();
	if (AssetPath.StartsWith(TEXT("/MeshParticleizer/")) == false || FPackageName::IsValidLongPackageName(AssetPath) == false ||
		FPackageName::DoesPackageExist(AssetPath) || FindPackage(nullptr, *AssetPath) != nullptr)
	{
		OutError = TEXT("Choose an unused /MeshParticleizer/ asset path.");
		return nullptr;
	}

	// 低ポリの粒と不規則な薄片。色は再生時の元表面色から与える。
	const auto Vertices = bFlake
		? TArray<FVector3f>{{-0.6f, -0.45f, 0.04f}, {0.55f, -0.3f, -0.04f}, {0.45f, 0.5f, 0.02f}, {-0.3f, 0.4f, -0.02f}}
		: TArray<FVector3f>{{0, 0, 0.65f}, {0, 0, -0.6f}, {0.5f, 0, 0}, {0, 0.42f, 0}, {-0.4f, 0, 0}, {0, -0.5f, 0}};
	const auto Triangles = bFlake ? TArray<FIntVector>{{0, 1, 2}, {0, 2, 3}}
								  : TArray<FIntVector>{{0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {0, 5, 2}, {1, 3, 2}, {1, 4, 3}, {1, 5, 4}, {1, 2, 5}};
	auto Owner = TStrongObjectPtr<UStaticMesh>(NewObject<UStaticMesh>(CreatePackage(*AssetPath),
		*FPackageName::GetLongPackageAssetName(AssetPath), RF_Public | RF_Standalone));
	auto* Mesh = Owner.Get();
	Mesh->bSupportRayTracing = false;
	Mesh->GetNaniteSettings().bEnabled = false;
	Mesh->NeverStream = true;
	Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), TEXT("Particle"), TEXT("Particle")));
	Mesh->SetNumSourceModels(1);
	Mesh->GetSourceModel(0).BuildSettings.bGenerateLightmapUVs = false;
	auto* Description = Mesh->CreateMeshDescription(0);
	auto Attributes = FStaticMeshAttributes(*Description);
	Attributes.Register();
	auto Positions = Attributes.GetVertexPositions();
	auto Normals = Attributes.GetVertexInstanceNormals();
	auto Tangents = Attributes.GetVertexInstanceTangents();
	auto Signs = Attributes.GetVertexInstanceBinormalSigns();
	auto Colors = Attributes.GetVertexInstanceColors();
	auto UVs = Attributes.GetVertexInstanceUVs();
	UVs.SetNumChannels(1);
	const auto Group = Description->CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Particle");

	for (const auto& Triangle : Triangles)
	{
		const auto Tangent = (Vertices[Triangle.Y] - Vertices[Triangle.X]).GetSafeNormal();
		const auto Normal = FVector3f::CrossProduct(Tangent, Vertices[Triangle.Z] - Vertices[Triangle.X]).GetSafeNormal();
		auto Corners = TArray<FVertexInstanceID>{};
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Vertex = Description->CreateVertex();
			Positions[Vertex] = Vertices[Triangle[Corner]];
			const auto Instance = Description->CreateVertexInstance(Vertex);
			Normals[Instance] = Normal;
			Tangents[Instance] = Tangent;
			Signs[Instance] = 1;
			Colors[Instance] = FVector4f(1, 1, 1, 1);
			UVs.Set(Instance, 0, FVector2f(Corner == 1 ? 1.f : 0.f, Corner == 2 ? 1.f : 0.f));
			Corners.Add(Instance);
		}
		Description->CreatePolygon(Group, Corners);
	}
	Mesh->CommitMeshDescription(0);
	Mesh->Build(false);
	FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});

	auto Bytes = int64{0};
	if (MeshParticleizerAssetGpuBytes(Mesh, Bytes, OutError) == false)
	{
		return nullptr;
	}
	if (MeshParticleizerPreparation::SaveAsset(Mesh) == false)
	{
		OutError = TEXT("Could not save the generated particle mesh.");
		return nullptr;
	}
	return Mesh;
}

UMeshParticleizerPreparedMesh* UMeshParticleizerPreparationLibrary::CreatePreparedParticleVariant(
	UMeshParticleizerPreparedMesh* Prepared, const FString& AssetPath, UNiagaraSystem* ParticleSystem,
	UNiagaraSystem* GdfParticleSystem, FString& OutError)
{
	OutError.Reset();
	if (Prepared == nullptr || ParticleSystem == nullptr || GdfParticleSystem == nullptr || ParticleSystem == GdfParticleSystem)
	{
		OutError = TEXT("Select prepared mesh data and both particle systems.");
		return nullptr;
	}
	if ((AssetPath.StartsWith(TEXT("/Game/")) == false && AssetPath.StartsWith(TEXT("/MeshParticleizer/")) == false) ||
		FPackageName::IsValidLongPackageName(AssetPath) == false || FPackageName::DoesPackageExist(AssetPath) == true ||
		FindPackage(nullptr, *AssetPath) != nullptr)
	{
		OutError = TEXT("Choose an unused /Game/ or /MeshParticleizer/ asset path.");
		return nullptr;
	}
	if (Prepared->ValidatePreparation(OutError) == false)
	{
		return nullptr;
	}

	if (MeshParticleizerPreparation::CompileParticleSystems(ParticleSystem, GdfParticleSystem,
			Prepared->BoneCount, Prepared->SourceMaterials.Num(), OutError) == false)
	{
		return nullptr;
	}

	// 元資産の版・表面・依存基準を保持し、変更した参照に合わせて常駐一覧だけを作り直す。
	auto Owner = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(DuplicateObject<UMeshParticleizerPreparedMesh>(Prepared,
		CreatePackage(*AssetPath), *FPackageName::GetLongPackageAssetName(AssetPath)));
	auto* Variant = Owner.Get();
	Variant->SetFlags(RF_Public | RF_Standalone);
	Variant->ParticleSystem = ParticleSystem;
	Variant->GdfParticleSystem = GdfParticleSystem;
	if (RefreshPreparedResourceManifest(Variant, OutError) == false)
	{
		return nullptr;
	}
	if (Variant->ValidatePreparation(OutError) == false)
	{
		return nullptr;
	}
	if (MeshParticleizerPreparation::SaveAsset(Variant) == false)
	{
		OutError = TEXT("Could not save the prepared particle variant.");
		return nullptr;
	}
	return Variant;
}

UMeshParticleizerParticleStyle* UMeshParticleizerPreparationLibrary::CreateParticleStyle(const FString& AssetPath,
	UNiagaraSystem* ParticleSystem, UNiagaraSystem* GdfParticleSystem, FString& OutError)
{
	OutError.Reset();
	if (ParticleSystem == nullptr || GdfParticleSystem == nullptr || ParticleSystem == GdfParticleSystem)
	{
		OutError = TEXT("Select separate normal and GDF particle systems.");
		return nullptr;
	}
	if ((AssetPath.StartsWith(TEXT("/Game/")) == false && AssetPath.StartsWith(TEXT("/MeshParticleizer/")) == false) ||
		FPackageName::IsValidLongPackageName(AssetPath) == false || FPackageName::DoesPackageExist(AssetPath) == true ||
		FindPackage(nullptr, *AssetPath) != nullptr)
	{
		OutError = TEXT("Choose an unused /Game/ or /MeshParticleizer/ asset path.");
		return nullptr;
	}
	if (MeshParticleizerPreparation::CompileParticleSystems(ParticleSystem, GdfParticleSystem, 0, 1, OutError) == false)
	{
		return nullptr;
	}

	auto Owner = TStrongObjectPtr<UMeshParticleizerParticleStyle>(NewObject<UMeshParticleizerParticleStyle>(CreatePackage(*AssetPath),
		*FPackageName::GetLongPackageAssetName(AssetPath), RF_Public | RF_Standalone));
	auto* Style = Owner.Get();
	if (Style->SetSystems(ParticleSystem, GdfParticleSystem, OutError) == false)
	{
		return nullptr;
	}
	if (MeshParticleizerPreparation::SaveAsset(Style) == false)
	{
		OutError = TEXT("Could not save Particle Style.");
		return nullptr;
	}
	return Style;
}
