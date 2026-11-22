#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerGpuMemory.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#if WITH_EDITOR
#include "MeshParticleizerResourceManifest.h"

#endif

bool UMeshParticleizerPreparedMesh::GetScaledSurfaceArea(const FVector& Scale, double& OutArea, FString& OutError) const
{
	OutArea = 0;
	OutError.Reset();
	const auto Absolute = Scale.GetAbs();
	if (Scale.ContainsNaN() == true || Absolute.GetMin() <= SMALL_NUMBER)
	{
		OutError = TEXT("Source scale must be finite and non-zero on every axis.");
		return false;
	}
	if (FMath::IsFinite(this->SurfaceArea) == false || this->SurfaceArea < 0)
	{
		OutError = TEXT("Prepared surface area is invalid. Prepare this source again.");
		return false;
	}

	// 等方倍率では旧Preparedの面積をそのまま利用できる。
	auto Area = 0.0;
	if (Absolute.X == Absolute.Y && Absolute.X == Absolute.Z)
	{
		Area = static_cast<double>(this->SurfaceArea) * Absolute.X * Absolute.X;
	}
	else
	{
		if (this->SurfaceAreaVectors.IsEmpty() == true)
		{
			OutError = TEXT("Non-uniform source scale requires prepared triangle areas. Prepare this source again in a new folder.");
			return false;
		}

		// cofactorで面積ベクトルを変換する。鏡映と軸ごとの倍率を同じ式で扱う。
		const auto AreaScale = FVector(Absolute.Y * Absolute.Z, Absolute.X * Absolute.Z, Absolute.X * Absolute.Y);
		auto ReferenceArea = 0.0;
		for (const auto& Vector : this->SurfaceAreaVectors)
		{
			const auto TriangleArea = Vector.Length();
			if (Vector.ContainsNaN() == true || FMath::IsFinite(TriangleArea) == false || TriangleArea <= 0)
			{
				OutError = TEXT("Prepared triangle area is invalid. Prepare this source again.");
				return false;
			}
			ReferenceArea += TriangleArea;
			Area += (Vector * AreaScale).Length();
		}
		if (FMath::IsFinite(ReferenceArea) == false ||
			FMath::IsNearlyEqual(ReferenceArea, static_cast<double>(this->SurfaceArea), FMath::Max(0.000001, ReferenceArea * 0.00001)) == false)
		{
			OutError = TEXT("Prepared triangle areas do not match the reference area. Prepare this source again.");
			return false;
		}
	}

	if (FMath::IsFinite(Area) == false || Area < 0)
	{
		OutError = TEXT("Scaled surface area exceeds the supported numeric range.");
		return false;
	}
	OutArea = Area;
	return true;
}

bool UMeshParticleizerPreparedMesh::HasTextureColorBindings() const
{
	return this->TextureColorSources.ContainsByPredicate([](const auto& Source) {
		return Source.bCompatible == true && Source.RegisteredTextureParameter.IsNone() == false;
	});
}

TArray<FMeshParticleizerMaterialParameterKey> FMeshParticleizerMaterialParameterKey::Gather(const UMaterialInterface* Material)
{
	auto Result = TArray<FMeshParticleizerMaterialParameterKey>{};
	if (Material == nullptr)
	{
		return Result;
	}
#if WITH_EDITORONLY_DATA
	constexpr auto ParameterTypes = EMaterialParameterType::Num;
#else
	constexpr auto ParameterTypes = EMaterialParameterType::NumRuntime;
#endif
	for (auto TypeIndex = 0; TypeIndex < static_cast<int32>(ParameterTypes); ++TypeIndex)
	{
		const auto Type = static_cast<EMaterialParameterType>(TypeIndex);
		auto Parameters = TMap<FMaterialParameterInfo, FMaterialParameterMetadata>{};
		Material->GetAllParametersOfType(Type, Parameters);
		for (const auto& Parameter : Parameters)
		{
			Result.Add({Type, Parameter.Key});
		}
	}
	return Result;
}

bool UMeshParticleizerPreparedMesh::ValidateBakedMaterialParameters(int32 Slot,
	const UMaterialInterface* Original, const UMaterialInterface* Display, FString& OutError, bool bAllowColorProvider,
	const TArray<FMeshParticleizerMaterialParameterKey>* CachedParameters) const
{
	OutError.Reset();
	const auto* Baked = this->DissolveMaterials.IsValidIndex(Slot) ? this->DissolveMaterials[Slot].Get() : nullptr;
	if (Original == nullptr || Baked == nullptr)
	{
		OutError = FString::Printf(TEXT("Load the original and baked material for slot %d before validation."), Slot);
		return false;
	}

	if (Original->GetMaterial()->DitherOpacityMask != Baked->GetMaterial()->DitherOpacityMask ||
		(Display != nullptr && Display->GetMaterial()->DitherOpacityMask != Baked->GetMaterial()->DitherOpacityMask))
	{
		OutError = FString::Printf(TEXT("Material slot %d changed Dither Opacity Mask. Prepare this material again."), Slot);
		return false;
	}

	if (Original->GetShadingModels().HasShadingModel(MSM_Unlit) && this->bUsesUnlitEmissive == false)
	{
		OutError = FString::Printf(TEXT("Slot %d is Unlit. Prepare it again with Use Unlit Emissive enabled; upgrading dependencies cannot replace baked Base Color."), Slot);
		return false;
	}

	// 同名でもレイヤーとインデックスが違う値は区別し、継承した値も比較する。
	// 元材の宣言だけを辿るため、消失用に追加したMP_Progress等は比較に混ざらない。
	const auto LocalParameters = CachedParameters == nullptr ? FMeshParticleizerMaterialParameterKey::Gather(Original)
															 : TArray<FMeshParticleizerMaterialParameterKey>{};
	const auto& Parameters = CachedParameters != nullptr ? *CachedParameters : LocalParameters;
	for (const auto& Parameter : Parameters)
	{
		const auto Type = Parameter.Type;
		auto OriginalValue = FMaterialParameterMetadata{};
		if (Original->GetParameterValue(Type, Parameter.Info, OriginalValue) == false)
		{
			OutError = FString::Printf(TEXT("Material slot %d lost parameter '%s'. Prepare this material again."), Slot, *Parameter.Info.ToString());
			return false;
		}
		{
			auto Expected = FMaterialParameterMetadata{};
			auto Current = FMaterialParameterMetadata{};
			const auto bHasExpected = Baked->GetParameterValue(Type, Parameter.Info, Expected);
			const auto bHasCurrent = Display == nullptr || Display->GetParameterValue(Type, Parameter.Info, Current);
			if (bHasExpected == false || bHasCurrent == false || OriginalValue.Value != Expected.Value ||
				(Display != nullptr && Current.Value != Expected.Value))
			{
				const auto* Source = this->TextureColorSources.IsValidIndex(Slot) ? &this->TextureColorSources[Slot] : nullptr;
				const auto bColorOnly = Source != nullptr && Source->HasCurrentDependencyAnalysis() &&
					Parameter.Info.Association == EMaterialParameterAssociation::GlobalParameter &&
					((Type == EMaterialParameterType::Scalar && Source->ProviderScalarParameters.Contains(Parameter.Info.Name)) ||
					 (Type == EMaterialParameterType::Vector && Source->ProviderVectorParameters.Contains(Parameter.Info.Name)));
				auto IsFinite = [Type](const FMaterialParameterValue& Value) {
					if (Type == EMaterialParameterType::Scalar)
					{
						return FMath::IsFinite(Value.AsScalar());
					}
					if (Type != EMaterialParameterType::Vector)
					{
						return false;
					}
					const auto Color = Value.AsLinearColor();
					return FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G) && FMath::IsFinite(Color.B) && FMath::IsFinite(Color.A);
				};
				if (bAllowColorProvider && this->bSupportsColorProvider && bColorOnly && bHasExpected && bHasCurrent &&
					IsFinite(OriginalValue.Value) && (Display == nullptr || IsFinite(Current.Value)))
				{
					continue;
				}

				OutError = FString::Printf(TEXT("Material slot %d parameter '%s' no longer matches its baked surface color. %s"),
					Slot, *Parameter.Info.ToString(), bColorOnly ? TEXT("Register a ready Color Provider or restore the prepared value.") : TEXT("Restore the prepared values or prepare this material again."));
				return false;
			}
		}
	}
	return true;
}

bool UMeshParticleizerPreparedMesh::ValidateResourceManifest(FString& OutError) const
{
	OutError.Reset();
	auto Seen = TSet<FSoftObjectPath>{};
	for (const auto& Entry : this->Resources)
	{
		const auto Path = Entry.Asset.ToSoftObjectPath();
		if (Path.IsNull() || Entry.GpuBytes < 0 || Seen.Contains(Path))
		{
			OutError = TEXT("The shared resource manifest has invalid or duplicate entries. Prepare this mesh again.");
			return false;
		}
		Seen.Add(Path);
	}

	auto Required = TArray<FSoftObjectPath>{this->ParticleSystem.ToSoftObjectPath(), this->GdfParticleSystem.ToSoftObjectPath(), this->Positions.ToSoftObjectPath(),
		this->Normals.ToSoftObjectPath(), this->Colors.ToSoftObjectPath()};
	if (this->SourceEmission.IsNull() == false)
	{
		Required.Add(this->SourceEmission.ToSoftObjectPath());
	}
	if (this->NeedsRuntimeCoverage())
	{
		if (this->CoverageDataVersion != CurrentCoverageDataVersion || this->CoverageTriangleCount <= 0)
		{
			OutError = TEXT("Runtime coverage requires complete surface candidates and current triangle interpolation data. Prepare this mesh again.");
			return false;
		}
		Required.Append({this->CoverageTriangles.ToSoftObjectPath(), this->Coordinates.ToSoftObjectPath()});
	}
	if (this->BoneCount > 0)
	{
		Required.Append({this->Coordinates.ToSoftObjectPath(), this->SkinVertices.ToSoftObjectPath(), this->SkinTriangles.ToSoftObjectPath()});
	}
	else if (this->NeedsSurfaceColorCache())
	{
		Required.Add(this->Coordinates.ToSoftObjectPath());
	}
	// Systemの編集で追加された粒子メッシュも、読込後の開始検査で予約漏れを検出する。
	for (const auto* Playback : {this->ParticleSystem.Get(), this->GdfParticleSystem.Get()})
	{
		if (Playback == nullptr)
		{
			continue;
		}
		for (const auto& Handle : Playback->GetEmitterHandles())
		{
			const auto* Emitter = Handle.GetInstance().GetEmitterData();
			if (Emitter == nullptr)
			{
				continue;
			}
			for (const auto* Renderer : Emitter->GetRenderers())
			{
				auto Meshes = TArray<UObject*>{};
				Renderer->GetUsedMeshes(nullptr, Meshes);
				for (const auto* Mesh : Meshes)
				{
					Required.AddUnique(FSoftObjectPath(Mesh));
				}
			}
		}
	}
	auto Display = TSet<FSoftObjectPath>{};
	for (const auto& Material : this->SourceMaterials)
	{
		Display.Add(Material.ToSoftObjectPath());
	}
	for (const auto& Material : this->DissolveMaterials)
	{
		Display.Add(Material.ToSoftObjectPath());
	}
	Required.Append(Display.Array());
	for (const auto& Path : Required)
	{
		const auto* Entry = this->Resources.FindByPredicate([&Path](const auto& Resource) {
			return Resource.Asset.ToSoftObjectPath() == Path;
		});
		if (Path.IsNull() || Entry == nullptr || (Display.Contains(Path) && Entry->bDisplayResource == false))
		{
			OutError = TEXT("The shared resource manifest is missing a required asset. Upgrade or prepare this mesh again.");
			return false;
		}
	}
	return true;
}

#if WITH_EDITOR
bool UMeshParticleizerPreparedMesh::CaptureResourceManifest(FString& OutError)
{
	auto Result = TArray<FMeshParticleizerResource>{};
	auto Collector = FMeshParticleizerResourceCollector(Result, OutError);

	OutError.Reset();
	auto* System = this->ParticleSystem.Get();
	if (Collector.Add(System, false) == false || Collector.Add(this->Positions.Get(), false) == false ||
		Collector.Add(this->Normals.Get(), false) == false || Collector.Add(this->Colors.Get(), false) == false)
	{
		return false;
	}
	if (this->SourceEmission.IsNull() == false && Collector.Add(this->SourceEmission.Get(), false) == false)
	{
		return false;
	}
	if (this->NeedsRuntimeCoverage() && (Collector.Add(this->CoverageTriangles.Get(), false) == false || Collector.Add(this->Coordinates.Get(), false) == false))
	{
		return false;
	}
	if (this->BoneCount > 0 && (Collector.Add(this->Coordinates.Get(), false) == false || Collector.Add(this->SkinVertices.Get(), false) == false || Collector.Add(this->SkinTriangles.Get(), false) == false))
	{
		return false;
	}
	if (this->BoneCount == 0 && this->NeedsSurfaceColorCache() && Collector.Add(this->Coordinates.Get(), false) == false)
	{
		return false;
	}
	for (const auto& Material : this->SourceMaterials)
	{
		if (Collector.AddMaterial(Material.Get(), true) == false)
		{
			return false;
		}
	}
	for (const auto& Material : this->DissolveMaterials)
	{
		if (Collector.AddMaterial(Material.Get(), true) == false)
		{
			return false;
		}
	}
	for (auto* Playback : {System, this->GdfParticleSystem.Get()})
	{
		if (Collector.AddSystem(Playback) == false)
		{
			return false;
		}
	}

	Result.Sort([](const auto& A, const auto& B) {
		return A.Asset.ToSoftObjectPath().ToString() < B.Asset.ToSoftObjectPath().ToString();
	});
	this->Resources = MoveTemp(Result);
	return true;
}
#endif
