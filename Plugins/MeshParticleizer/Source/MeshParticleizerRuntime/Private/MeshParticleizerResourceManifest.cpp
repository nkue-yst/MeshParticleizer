#include "MeshParticleizerResourceManifest.h"

#if WITH_EDITOR
#include "MeshParticleizerGpuMemory.h"
#include "Engine/Texture.h"
#include "Engine/TextureCollection.h"
#include "Materials/MaterialInstance.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"

bool FMeshParticleizerResourceCollector::Add(UObject* Asset, bool bDisplay)
{
	if (Asset == nullptr)
	{
		this->Error = TEXT("Load every resource before capturing its cache manifest.");
		return false;
	}
	const auto Path = FSoftObjectPath(Asset);
	auto* Existing = this->Resources.FindByPredicate([&Path](const auto& Entry) {
		return Entry.Asset.ToSoftObjectPath() == Path;
	});
	if (Existing != nullptr)
	{
		Existing->bDisplayResource |= bDisplay;
		return true;
	}

	auto Bytes = int64{0};
	if (MeshParticleizerAssetGpuBytes(Asset, Bytes, this->Error) == false)
	{
		return false;
	}
	auto& Entry = this->Resources.AddDefaulted_GetRef();
	Entry.Asset = Asset;
	Entry.GpuBytes = Bytes;
	Entry.bDisplayResource = bDisplay;
	return true;
}

bool FMeshParticleizerResourceCollector::AddMaterial(UMaterialInterface* Material, bool bDisplay)
{
	if (this->Add(Material, bDisplay) == false)
	{
		return false;
	}
	auto Textures = TSet<const UTexture*>{};
	auto Visited = TSet<UMaterialInterface*>{};
	auto AddCollection = [this, &Textures, bDisplay](UTextureCollection* Collection) {
		if (Collection == nullptr)
		{
			return true;
		}
		if (this->Add(Collection, bDisplay) == false)
		{
			return false;
		}
		for (const auto& Texture : Collection->Textures)
		{
			if (Texture != nullptr)
			{
				Textures.Add(Texture.Get());
			}
		}
		return true;
	};

	// シェーダー完成前も親材の既定値と各階層の上書き参照を数える。
	auto* Current = Material;
	while (Current != nullptr && Visited.Contains(Current) == false)
	{
		Visited.Add(Current);
		if (this->Add(Current, bDisplay) == false)
		{
			return false;
		}
		Current->GetReferencedTexturesAndOverrides(Textures);
		for (const auto& Collection : Current->GetReferencedTextureCollections())
		{
			if (AddCollection(Collection) == false)
			{
				return false;
			}
		}
		auto* Instance = Cast<UMaterialInstance>(Current);
		if (Instance != nullptr)
		{
			for (const auto& Value : Instance->TextureCollectionParameterValues)
			{
				if (AddCollection(Value.ParameterValue) == false)
				{
					return false;
				}
			}
		}
		Current = Instance != nullptr ? Instance->Parent.Get() : nullptr;
	}
	for (const auto* Texture : Textures)
	{
		if (this->Add(const_cast<UTexture*>(Texture), bDisplay) == false)
		{
			return false;
		}
	}
	return true;
}

bool FMeshParticleizerResourceCollector::AddSystem(UNiagaraSystem* System)
{
	if (this->Add(System) == false)
	{
		return false;
	}
	for (const auto& Handle : System->GetEmitterHandles())
	{
		const auto* Emitter = Handle.GetInstance().GetEmitterData();
		if (Emitter == nullptr)
		{
			continue;
		}
		for (auto* Renderer : Emitter->GetRenderers())
		{
			if (Renderer == nullptr)
			{
				this->Error = TEXT("Particle renderer is missing.");
				return false;
			}
			auto Meshes = TArray<UObject*>{};
			Renderer->GetUsedMeshes(nullptr, Meshes);
			for (auto* Mesh : Meshes)
			{
				if (this->Add(Mesh) == false)
				{
					return false;
				}
			}
			auto Materials = TArray<UMaterialInterface*>{};
			Renderer->GetUsedMaterials(nullptr, Materials);
			for (auto* Material : Materials)
			{
				if (this->AddMaterial(Material) == false)
				{
					return false;
				}
			}
		}
	}
	return true;
}
#endif
