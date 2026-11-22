#include "MeshParticleizerParticleStyle.h"
#include "MeshParticleizerGpuMemory.h"
#include "NiagaraDataInterfaceMeshParticleizerCollision.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "Materials/MaterialInterface.h"
#if WITH_EDITOR
#include "MeshParticleizerResourceManifest.h"
#include "Misc/DataValidation.h"
#endif

bool UMeshParticleizerParticleStyle::ValidateResourceManifest(FString& OutError) const
{
	OutError.Reset();
	if (this->ParticleSystem.IsNull() == true || this->GdfParticleSystem.IsNull() == true || this->ParticleSystem == this->GdfParticleSystem)
	{
		OutError = TEXT("Particle Style requires separate normal and GDF systems.");
		return false;
	}
	auto Seen = TSet<FSoftObjectPath>{};
	for (const auto& Entry : this->Resources)
	{
		const auto Path = Entry.Asset.ToSoftObjectPath();
		if (Path.IsAsset() == false || Entry.GpuBytes < 0 || Entry.bDisplayResource == true || Seen.Contains(Path) == true)
		{
			OutError = TEXT("Particle Style has an invalid resource manifest. Generate it again.");
			return false;
		}
		Seen.Add(Path);
	}

	// 読込前にシステム参照を検証し、読込後は描画資産の差し替えによる予約漏れも検出する。
	auto Required = TArray<FSoftObjectPath>{this->ParticleSystem.ToSoftObjectPath(), this->GdfParticleSystem.ToSoftObjectPath()};
	for (const auto* System : {this->ParticleSystem.Get(), this->GdfParticleSystem.Get()})
	{
		if (System == nullptr)
		{
			continue;
		}
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
					OutError = TEXT("Particle Style renderer is missing.");
					return false;
				}
				auto Meshes = TArray<UObject*>{};
				Renderer->GetUsedMeshes(nullptr, Meshes);
				for (const auto* Mesh : Meshes)
				{
					Required.AddUnique(FSoftObjectPath(Mesh));
				}
				auto Materials = TArray<UMaterialInterface*>{};
				Renderer->GetUsedMaterials(nullptr, Materials);
				for (const auto* Material : Materials)
				{
					Required.AddUnique(FSoftObjectPath(Material));
				}
			}
		}
	}
	for (const auto& Path : Required)
	{
		if (Seen.Contains(Path) == false)
		{
			OutError = TEXT("Particle Style is missing a required resource. Generate it again.");
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR
bool UMeshParticleizerParticleStyle::SetSystems(UNiagaraSystem* Normal, UNiagaraSystem* Gdf, FString& OutError)
{
	OutError.Reset();
	if (Normal == nullptr || Gdf == nullptr || Normal == Gdf)
	{
		OutError = TEXT("Select separate normal and GDF particle systems.");
		return false;
	}
	const auto Collision = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCollision::StaticClass()), TEXT("User.Collision"));
	auto Captured = TArray<FMeshParticleizerResource>{};
	auto Collector = FMeshParticleizerResourceCollector(Captured, OutError);
	for (auto* System : {Normal, Gdf})
	{
		auto Memory = FMeshParticleizerGpuMemory{};
		if (System->IsReadyToRun() == false || System->IsValid() == false ||
			((System->GetExposedParameters().IndexOf(Collision) != INDEX_NONE) != (System == Gdf)))
		{
			OutError = TEXT("Compile valid normal and GDF systems before creating Particle Style.");
			return false;
		}
		if (FMeshParticleizerGpuMemory::Build(System, 0, 0, 1, Memory, OutError) == false || Collector.AddSystem(System) == false)
		{
			return false;
		}
	}

	Captured.Sort([](const auto& A, const auto& B) {
		return A.Asset.ToSoftObjectPath().ToString() < B.Asset.ToSoftObjectPath().ToString();
	});
	this->ParticleSystem = Normal;
	this->GdfParticleSystem = Gdf;
	this->Resources = MoveTemp(Captured);
	return true;
}

EDataValidationResult UMeshParticleizerParticleStyle::IsDataValid(FDataValidationContext& Context) const
{
	const auto Base = Super::IsDataValid(Context);
	auto Error = FString{};
	if (this->ValidateResourceManifest(Error) == false)
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return Base == EDataValidationResult::Invalid ? Base : EDataValidationResult::Valid;
}
#endif
