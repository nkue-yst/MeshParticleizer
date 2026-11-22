#include "MeshParticleizeSource.h"
#include "NiagaraDataInterfaceMeshParticleizerCollision.h"
#include "NiagaraDataInterfaceMeshParticleizerBudget.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerParticleStyle.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "MeshParticleizerGpuState.h"
#include "MeshParticleizerSimulation.h"
#include "MeshParticleizerAssetCache.h"
#include "MeshParticleizerSurfaceTexture.h"
#include "NiagaraDataInterfaceMeshParticleizerColor.h"
#include "NiagaraDataInterfaceMeshParticleizerCoverage.h"
#include "MeshParticleizerProfiling.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraDataInterfaceArrayFloat.h"
#include "StaticMeshResources.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace MeshParticleizerPlayback
{
#if !UE_BUILD_SHIPPING
	// 比較計測専用。開始時に固定し、所有中の消失材を通常の入力検査に通す。
	static TAutoConsoleVariable<int32> RetainSourceForProfiling(
		TEXT("mp.Profile.RetainSource"), 0,
		TEXT("Keep directional live sources visible with the dissolve shader during profiling. Latched at start; unavailable in Shipping."),
		ECVF_Default);
#endif
	FMeshParticleizerResolvedGround FindGround(UMeshComponent* Mesh, TConstArrayView<FMeshParticleizerTarget> Targets)
	{
		// 中央と四隅だけを調べる。凹凸を単一平面で隠さず、床なしとして診断する。
		const auto Bounds = Mesh->Bounds;
		const FVector2D Offsets[] = {{0, 0}, {-0.8, -0.8}, {-0.8, 0.8}, {0.8, -0.8}, {0.8, 0.8}};
		auto Query = FCollisionQueryParams(SCENE_QUERY_STAT(MeshParticleizerGround), false);

		// 別Actorの装備と、各OwnerのCapsule等も床検出から除く。
		for (const auto& Target : Targets)
		{
			auto* Part = Target.Mesh.Get();
			if (Part != nullptr)
			{
				Query.AddIgnoredComponent(Part);
				Query.AddIgnoredActor(Part->GetOwner());
			}
		}
		auto Result = FMeshParticleizerResolvedGround{};
		auto CenterHeight = 0.0;
		auto bFirst = true;
		for (const auto& Offset : Offsets)
		{
			const auto Start = Bounds.Origin + FVector(Bounds.BoxExtent.X * Offset.X, Bounds.BoxExtent.Y * Offset.Y, 0);
			auto Hit = FHitResult{};
			if (Mesh->GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0, 0, 5000), ECC_WorldStatic, Query) == false)
			{
				Result.Status = EMeshParticleizerGroundStatus::NoHit;
				Result.Diagnostics = TEXT("No collision plane; an automatic ground probe found no surface within 5000 cm. Particles fall freely.");
				return Result;
			}
			if (Hit.bStartPenetrating == true)
			{
				Result.Status = EMeshParticleizerGroundStatus::InitialOverlap;
				Result.Diagnostics = TEXT("No collision plane; an automatic ground probe starts inside collision. Move the source clear or specify a known ground height. Particles fall freely.");
				return Result;
			}
			if (Hit.ImpactNormal.Z < 0.99)
			{
				Result.Status = EMeshParticleizerGroundStatus::UnsupportedSlope;
				Result.Diagnostics = TEXT("No collision plane; an automatic ground probe hit a slope outside the horizontal-plane tolerance. Particles fall freely.");
				return Result;
			}
			if (bFirst == false && FMath::Abs(Hit.ImpactPoint.Z - CenterHeight) > 2)
			{
				Result.Status = EMeshParticleizerGroundStatus::UnevenHeights;
				Result.Diagnostics = TEXT("No collision plane; automatic ground heights differ from the center by more than 2 cm. Particles fall freely.");
				return Result;
			}
			// 隣り合う点の差を積み重ねず、中央の高さへすべてを照合する。
			if (bFirst == true)
			{
				CenterHeight = Hit.ImpactPoint.Z;
			}
			bFirst = false;
		}

		// 全点が一致してから採用する。途中失敗では中央の高さも公開しない。
		Result.Status = EMeshParticleizerGroundStatus::Detected;
		Result.bEnabled = true;
		Result.Height = CenterHeight;
		Result.Diagnostics = FString::Printf(TEXT("Simple Ground automatically detected at Z=%.1f cm. "), Result.Height);
		return Result;
	}
}

bool UMeshParticleizeSource::ValidateSource(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared, FString& OutError) const
{
	// 準備中と開始時に同じ入力を検証する。所有する消失材だけを元材質として扱う。
	auto* Static = Cast<UStaticMeshComponent>(Mesh);
	auto* Skinned = Cast<USkinnedMeshComponent>(Mesh);
	UStaticMesh* StaticAsset = Static != nullptr ? Static->GetStaticMesh().Get() : nullptr;
	auto* SkeletalAsset = Skinned != nullptr ? Cast<USkeletalMesh>(Skinned->GetSkinnedAsset()) : nullptr;
	if (IsValid(Mesh) == false || (StaticAsset == nullptr && SkeletalAsset == nullptr) || IsValid(Prepared) == false)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("A valid Static or Skeletal Mesh component and prepared mesh are required.");
		}
		return false;
	}
	if (Prepared->DataVersion != UMeshParticleizerPreparedMesh::CurrentDataVersion)
	{
		OutError = TEXT("Prepared data uses an older generator. Prepare this mesh again before playback.");
		return false;
	}
	if (Prepared->ValidateResourceManifest(OutError) == false)
	{
		return false;
	}
	if ((StaticAsset != nullptr && Prepared->SourceMesh.ToSoftObjectPath() != FSoftObjectPath(StaticAsset)) ||
		(SkeletalAsset != nullptr && Prepared->SkeletalMesh.ToSoftObjectPath() != FSoftObjectPath(SkeletalAsset)) ||
		Prepared->SampleCount <= 0 || Prepared->DissolveMaterials.Num() != Mesh->GetNumMaterials())
	{
		OutError = TEXT("Prepared data does not match this mesh or material layout.");
		return false;
	}
	const auto* StaticRender = StaticAsset != nullptr ? StaticAsset->GetRenderData() : nullptr;
	const auto* SkeletalRender = SkeletalAsset != nullptr ? SkeletalAsset->GetResourceForRendering() : nullptr;
	if (Prepared->SamplingLOD < 0 ||
		(StaticAsset != nullptr && (StaticRender == nullptr || StaticRender->LODResources.IsValidIndex(Prepared->SamplingLOD) == false)) ||
		(SkeletalAsset != nullptr && (SkeletalRender == nullptr || SkeletalRender->LODRenderData.IsValidIndex(Prepared->SamplingLOD) == false)))
	{
		OutError = TEXT("The prepared sampling LOD is unavailable for this source mesh.");
		return false;
	}
#if WITH_EDITOR
	if (Prepared->ValidatePreparation(OutError) == false)
	{
		return false;
	}
	if ((StaticAsset != nullptr && (StaticRender == nullptr || Prepared->SourceFingerprint != UMeshParticleizerPreparedMesh::MakeSourceFingerprint(StaticRender->DerivedDataKey, Prepared->SamplingLOD))) ||
		(SkeletalAsset != nullptr && (SkeletalRender == nullptr || Prepared->SourceFingerprint != UMeshParticleizerPreparedMesh::MakeSourceFingerprint(SkeletalRender->DerivedDataKey, Prepared->SamplingLOD))))
	{
		OutError = TEXT("Source mesh changed. Generate current preparation data before playback.");
		return false;
	}
#endif
	auto* System = this->PlaybackSystem != nullptr ? this->PlaybackSystem.Get() : Prepared->ParticleSystem.Get();
	auto* Positions = Prepared->Positions.Get();
	auto* Normals = Prepared->Normals.Get();
	auto* Colors = Prepared->Colors.Get();
	if (System == nullptr || System->IsReadyToRun() == false || System->IsValid() == false ||
		Positions == nullptr || Normals == nullptr || Colors == nullptr ||
		(SkeletalAsset != nullptr && (Prepared->Coordinates.IsValid() == false || Prepared->SkinVertices.IsValid() == false || Prepared->SkinTriangles.IsValid() == false)))
	{
		OutError = TEXT("Particle resources are not ready. Call Prepare For Particleize before starting.");
		return false;
	}
	if (System->GetExposedParameters().IndexOf(FNiagaraVariable(
												   FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerBudget::StaticClass()), TEXT("User.Budget"))) == INDEX_NONE)
	{
		OutError = TEXT("Particle system does not support the shared resting budget. Upgrade or regenerate it before playback.");
		return false;
	}
	if (System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.ManagedLifetime"))) == INDEX_NONE)
	{
		OutError = TEXT("Particle system does not support managed pooling. Upgrade or regenerate it before playback.");
		return false;
	}
	if (System->GetExposedParameters().IndexOf(FNiagaraVariable(
												   FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat4::StaticClass()), TEXT("User.SurfaceTints"))) == INDEX_NONE)
	{
		OutError = TEXT("Particle system needs the surface color input. Upgrade or regenerate it before playback.");
		return false;
	}
	if (Prepared->NeedsSurfaceColorCache() && (Prepared->Coordinates.IsValid() == false || System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceColor"))) == INDEX_NONE))
	{
		OutError = TEXT("Texture color requires the prepared UV data and upgraded particle system.");
		return false;
	}
	if (Prepared->NeedsRuntimeCoverage() && (Prepared->CoverageTriangles.IsValid() == false || Prepared->Coordinates.IsValid() == false || System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCoverage::StaticClass()), TEXT("User.SurfaceCoverage"))) == INDEX_NONE))
	{
		OutError = TEXT("Runtime coverage requires complete prepared geometry and an upgraded particle system.");
		return false;
	}
	for (const auto* Name : {TEXT("User.ProgressX"), TEXT("User.ProgressY"), TEXT("User.ProgressZ")})
	{
		if (System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), Name)) == INDEX_NONE)
		{
			OutError = TEXT("Particle system uses an older progress layout. Upgrade or regenerate the system before playback.");
			return false;
		}
	}
	if (System->GetExposedParameters().IndexOf(FNiagaraVariable(
												   FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat4::StaticClass()), TEXT("User.ProgressCurve"))) == INDEX_NONE)
	{
		OutError = TEXT("Particle system does not support progress curves. Upgrade or regenerate it before playback.");
		return false;
	}
	if (System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.OriginCount"))) == INDEX_NONE)
	{
		OutError = TEXT("Particle system does not support multiple origins. Upgrade or regenerate it before playback.");
		return false;
	}
	for (const auto* Name : {TEXT("User.SleepSpeed"), TEXT("User.SleepDuration"), TEXT("User.CollisionRadiusScale")})
	{
		if (System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Name)) == INDEX_NONE)
		{
			OutError = TEXT("Particle system uses an older contact layout. Upgrade or regenerate it before playback.");
			return false;
		}
	}

	const auto bPrepared = this->DissolveMaterials.IsEmpty() == false;
	if (bPrepared == true && (this->Source.Get() != Mesh || this->PreparedMesh != Prepared || this->DissolveMaterials.Num() != Mesh->GetNumMaterials() || this->OriginalMaterials.Num() != Mesh->GetNumMaterials()))
	{
		OutError = TEXT("Prepared material binding belongs to another source or layout.");
		return false;
	}
	for (auto Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
	{
		if (Prepared->TextureColorSources.IsValidIndex(Slot) == false || Prepared->TextureColorSources[Slot].HasCurrentDependencyAnalysis() == false)
		{
			OutError = FString::Printf(TEXT("Material slot %d has missing or outdated dependency analysis. Upgrade its prepared data before playback."), Slot);
			return false;
		}
		auto* Original = bPrepared ? this->OriginalMaterials[Slot].Get() : Mesh->GetMaterial(Slot);

		// 同じ準備材から作ったMIDは許可する。別の静的インスタンスや材質の差し替えは別準備を要求する。
		auto* Parent = Original;
		while (const auto* Dynamic = Cast<UMaterialInstanceDynamic>(Parent))
		{
			Parent = Dynamic->Parent;
		}
		if (Prepared->DissolveMaterials[Slot].IsValid() == false || Prepared->SourceMaterials.IsValidIndex(Slot) == false ||
			(FSoftObjectPath(Original) != Prepared->SourceMaterials[Slot].ToSoftObjectPath() &&
				FSoftObjectPath(Parent) != Prepared->SourceMaterials[Slot].ToSoftObjectPath()) ||
			(bPrepared == true && Mesh->GetMaterial(Slot) != this->DissolveMaterials[Slot]))
		{
			OutError = FString::Printf(TEXT("Material slot %d changed or is unsupported. Restore or prepare this material."), Slot);
			return false;
		}
		if (Prepared->ValidateBakedMaterialParameters(Slot, Original,
				bPrepared ? this->DissolveMaterials[Slot].Get() : nullptr, OutError, true) == false)
		{
			return false;
		}
	}
	return true;
}

bool UMeshParticleizeSource::PrepareMaterials(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared, FString& OutError)
{
	if (this->ValidateSource(Mesh, Prepared, OutError) == false)
	{
		return false;
	}
	if (this->DissolveMaterials.IsEmpty() == false)
	{
		return true;
	}

	CSV_SCOPED_TIMING_STAT(MeshParticleizer, SetupMaterials);
	this->Source = Mesh;
	this->PreparedMesh = Prepared;
	this->SurfaceTints.Init(FVector4f(1, 1, 1, 1), Mesh->GetNumMaterials());
	this->SurfaceTextures.SetNum(Mesh->GetNumMaterials());
	this->ProviderUVs.Init(FVector4f(1, 1, 0, 0), Mesh->GetNumMaterials());
	this->SurfaceColorRevisions.Init(0, Mesh->GetNumMaterials());
	this->EmissionProviders.SetNum(Mesh->GetNumMaterials());
	this->EmissionUVs.Init(FVector4f(1, 1, 0, 0), Mesh->GetNumMaterials());
	this->EmissionRevisions.Init(0, Mesh->GetNumMaterials());
	for (auto Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
	{
		this->OriginalMaterials.Add(Mesh->GetMaterial(Slot));
		this->MaterialInputs.AddDefaulted();
		this->MaterialParameterKeys.Add(FMeshParticleizerMaterialParameterKey::Gather(this->OriginalMaterials.Last()));
		UMaterialInstanceDynamic* Material = nullptr;
		{
			CSV_SCOPED_TIMING_STAT(MeshParticleizer, CreateMaterial);
			Material = UMaterialInstanceDynamic::Create(Prepared->DissolveMaterials[Slot].Get(), this);
		}

		// 準備完了からProvider登録までの表示を保つ。資源参照や穴に影響する値はコピーしない。
		const auto& ColorSource = Prepared->TextureColorSources[Slot];
		for (const auto Name : ColorSource.ProviderScalarParameters)
		{
			auto Value = 0.f;
			if (this->OriginalMaterials[Slot]->GetScalarParameterValue(Name, Value))
			{
				Material->SetScalarParameterValue(Name, Value);
			}
		}
		for (const auto Name : ColorSource.ProviderVectorParameters)
		{
			auto Value = FLinearColor{};
			if (this->OriginalMaterials[Slot]->GetVectorParameterValue(Name, Value))
			{
				Material->SetVectorParameterValue(Name, Value);
			}
		}

		// 準備中は元の穴と全表面を保ち、割り当てに伴うPSO準備を開始前に済ませる。
		this->MaterialInputs[Slot].SetScalar(Material, TEXT("MP_Progress"), -1);
		if (Prepared->bSupportsSurfaceTint == true)
		{
			this->MaterialInputs[Slot].SetVector(Material, TEXT("MP_SurfaceTint"), FLinearColor::White);
		}
		this->DissolveMaterials.Add(Material);
		// Operationが終了しても、消失材が元メッシュに残る間は表示資産を保護する。
		if (this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->GetAssetCache()->Acquire(
			Material, Prepared->Resources, OutError, true) == false)
		{
			this->RestoreSourceMaterials();
			return false;
		}
		{
			CSV_SCOPED_TIMING_STAT(MeshParticleizer, AssignMaterial);
			Mesh->SetMaterial(Slot, Material);
		}
	}
	return true;
}

bool UMeshParticleizeSource::ValidateStart(const FMeshParticleizeOptions& InOptions, FString& OutError) const
{
	auto* Mesh = this->Source.Get();
	if (this->ValidateSource(Mesh, this->PreparedMesh, OutError) == false)
	{
		return false;
	}
	if (InOptions.Motion == EMeshParticleizerMotion::Drift && InOptions.DriftGravityScale > 0 &&
		InOptions.MaxParticles > 0 && InOptions.ParticleLifetime > 0)
	{
		const auto* System = this->PlaybackSystem.Get();
		const auto Version = FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.DriftGravityVersion"));
		if (System == nullptr || System->GetExposedParameters().GetParameterValue<int32>(Version) != 1)
		{
			OutError = TEXT("Drift gravity requires an upgraded particle system. Upgrade or regenerate the playback system before starting.");
			return false;
		}
	}

	for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
	{
		if (this->MaterialInputs[Slot].Validate(this->DissolveMaterials[Slot], Slot, OutError) == false)
		{
			return false;
		}
	}
	if (InOptions.bInheritSourceEmission == true)
	{
		const auto* Emission = this->PreparedMesh->SourceEmission.Get();
		const auto* System = this->PlaybackSystem.Get();
		if ((Emission != nullptr && (Emission->SRGB == true || Emission->GetSizeX() != 512 || Emission->GetSizeY() != FMath::DivideAndRoundUp(this->PreparedMesh->SampleCount, 512))) || System == nullptr ||
			System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.InheritSourceEmission"))) == INDEX_NONE)
		{
			OutError = TEXT("Original emission requires an HDR-prepared source and upgraded particle system. Enable Bake original emission during preparation.");
			return false;
		}
		if (this->PreparedMesh->bSupportsEmissionProvider &&
			(System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceEmission"))) == INDEX_NONE ||
			System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.HasBakedEmission"))) == INDEX_NONE))
		{
			OutError = TEXT("Original emission requires the upgraded HDR provider particle input.");
			return false;
		}
		// 同期開始では消失材の準備前にここへ来る。登録配列の有無によらず全スロットを検証する。
		if (Emission == nullptr)
		{
			for (auto Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
			{
				if (this->EmissionProviders.IsValidIndex(Slot) == false || this->EmissionProviders[Slot] == nullptr)
				{
					OutError = FString::Printf(TEXT("Material slot %d requires an HDR-prepared source or a ready HDR provider."), Slot);
					return false;
				}
			}
		}
	}
	if (this->PreparedMesh->bSupportsSurfaceTint == true)
	{
		for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
		{
			auto Tint = FLinearColor{};
			if (this->SurfaceTints.IsValidIndex(Slot) == false ||
				this->DissolveMaterials[Slot]->GetVectorParameterValue(TEXT("MP_SurfaceTint"), Tint) == false ||
				Tint != FLinearColor(this->SurfaceTints[Slot]))
			{
				OutError = TEXT("The registered surface tint was changed externally. Use Set Surface Color Parameters.");
				return false;
			}
		}
	}
	auto* Skinned = Cast<USkinnedMeshComponent>(Mesh);
	if (Skinned != nullptr && this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->IsBonePoseReady(Skinned, this->PreparedMesh->BoneCount) == false)
	{
		OutError = TEXT("Prepare the source's LOD0 bone pose before starting.");
		return false;
	}
	for (auto Slot = 0; Slot < this->SurfaceTextures.Num(); ++Slot)
	{
		const auto* Registration = this->SurfaceTextures[Slot].Get();
		const auto* EmissionProvider = this->EmissionProviders[Slot].Get();
		const auto bOriginalUnlit = this->OriginalMaterials[Slot]->GetShadingModels().HasShadingModel(MSM_Unlit);
		if (InOptions.bInheritSourceEmission == true && bOriginalUnlit == true &&
			((Registration != nullptr && Registration != EmissionProvider) || this->SurfaceTints[Slot] != FVector4f(1, 1, 1, 1)))
		{
			OutError = TEXT("Baked Unlit emission cannot follow a registered surface color change. Use unchanged surface color or disable original emission inheritance.");
			return false;
		}
		if (EmissionProvider != nullptr && EmissionProvider->IsReady(OutError) == false)
		{
			return false;
		}
		if (this->PreparedMesh->ValidateBakedMaterialParameters(Slot, this->OriginalMaterials[Slot], this->DissolveMaterials[Slot],
				OutError, Registration != nullptr && Registration->IsColorProvider() && (InOptions.bInheritSourceEmission == false || bOriginalUnlit == false || EmissionProvider != nullptr)) == false)
		{
			return false;
		}
		const auto& Dependencies = this->PreparedMesh->TextureColorSources[Slot];
		if (Dependencies.bRequiresColorProvider == true && (Registration == nullptr || Registration->IsColorProvider() == false))
		{
			OutError = FString::Printf(TEXT("Material slot %d: %s"), Slot, *Dependencies.DependencyDiagnostics);
			return false;
		}
		if (Registration != nullptr && Registration->IsReady(OutError) == false)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Wait for registered surface texture mips before starting.");
			}
			return false;
		}
		if (this->PreparedMesh->bSupportsColorProvider == true)
		{
			const auto bProvider = Registration != nullptr && Registration->IsColorProvider();
			auto Enabled = 0.f;
			auto UV = FLinearColor{};
			UTexture* Published = nullptr;
			auto* Expected = bProvider ? Registration->GetTexture() : this->PreparedMesh->Colors.Get();
			if (this->DissolveMaterials[Slot]->GetScalarParameterValue(TEXT("MP_UseColorProvider"), Enabled) == false ||
				Enabled != (bProvider ? 1.f : 0.f) ||
				this->DissolveMaterials[Slot]->GetVectorParameterValue(TEXT("MP_ColorProviderUV"), UV) == false ||
				UV != FLinearColor(this->ProviderUVs[Slot]) ||
				this->DissolveMaterials[Slot]->GetTextureParameterValue(TEXT("MP_ColorProviderTexture"), Published) == false || Published != Expected)
			{
				OutError = TEXT("The registered Color Provider material input changed externally. Use Set Surface Color Parameters.");
				return false;
			}
		}
		if (this->PreparedMesh->bSupportsEmissionProvider == true)
		{
			auto Enabled = 0.f;
			auto UV = FLinearColor{};
			UTexture* Published = nullptr;
			auto* Expected = EmissionProvider != nullptr ? EmissionProvider->GetTexture() : this->PreparedMesh->Colors.Get();
			if (this->DissolveMaterials[Slot]->GetScalarParameterValue(TEXT("MP_UseEmissionProvider"), Enabled) == false ||
				Enabled != (EmissionProvider != nullptr ? 1.f : 0.f) ||
				this->DissolveMaterials[Slot]->GetVectorParameterValue(TEXT("MP_EmissionProviderUV"), UV) == false || UV != FLinearColor(this->EmissionUVs[Slot]) ||
				this->DissolveMaterials[Slot]->GetTextureParameterValue(TEXT("MP_EmissionProviderTexture"), Published) == false || Published != Expected)
			{
				OutError = TEXT("The registered HDR provider material input changed externally. Use Set Surface Color Parameters.");
				return false;
			}
		}
		if (this->PreparedMesh->TextureColorSources.IsValidIndex(Slot) == false)
		{
			continue;
		}
		const auto& Binding = this->PreparedMesh->TextureColorSources[Slot];
		if (Binding.RegisteredTextureParameter.IsNone())
		{
			continue;
		}
		UTexture* Current = nullptr;
		auto* Expected = Registration != nullptr && Registration->IsColorProvider() == false ? Registration->GetTexture() : Binding.Texture.Get();
		if (this->DissolveMaterials[Slot]->GetTextureParameterValue(Binding.RegisteredTextureParameter, Current) == false || Current != Expected)
		{
			OutError = TEXT("The registered surface texture was changed externally. Use Set Surface Color Parameters.");
			return false;
		}
	}
	return true;
}

bool UMeshParticleizeSource::SetSurfaceColorParameters(int32 MaterialSlot,
	const FMeshParticleizerSurfaceColorParameters& Parameters, bool bDeferToUpdate, FString& OutError)
{
	const auto& Tint = Parameters.Tint;
	if (FMath::IsFinite(Tint.R) == false || FMath::IsFinite(Tint.G) == false || FMath::IsFinite(Tint.B) == false ||
		Tint.R < 0 || Tint.G < 0 || Tint.B < 0)
	{
		OutError = TEXT("Surface Tint requires finite, non-negative RGB values.");
		return false;
	}
	auto* Mesh = this->Source.Get();
	auto Current = FLinearColor{};
	if (Mesh == nullptr || this->PreparedMesh == nullptr || this->PreparedMesh->bSupportsSurfaceTint == false ||
		this->DissolveMaterials.IsValidIndex(MaterialSlot) == false || this->SurfaceTints.IsValidIndex(MaterialSlot) == false ||
		Mesh->GetMaterial(MaterialSlot) != this->DissolveMaterials[MaterialSlot] ||
		this->DissolveMaterials[MaterialSlot]->GetVectorParameterValue(TEXT("MP_SurfaceTint"), Current) == false)
	{
		OutError = TEXT("Surface Tint requires a valid material slot prepared with the registered surface color input.");
		return false;
	}
	// 未適用の正規更新は許可し、外部変更を次の公開で上書きして隠さない。
	if (this->MaterialInputs[MaterialSlot].Validate(this->DissolveMaterials[MaterialSlot], MaterialSlot, OutError) == false)
	{
		return false;
	}

	// HDR公開は別の登録として検証し、すべて通るまでは前の入力を維持する。
	auto* Emission = Parameters.EmissionProvider.Get();
	const auto EmissionUV = Emission != nullptr ? FVector4f(Parameters.EmissionUVScale.X, Parameters.EmissionUVScale.Y,
													  Parameters.EmissionUVOffset.X, Parameters.EmissionUVOffset.Y)
												: FVector4f(1, 1, 0, 0);
	if (Emission != nullptr && (this->PreparedMesh->bSupportsEmissionProvider == false || Emission->IsHDRProvider() == false || Emission->BelongsTo(this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()) == false || FMath::IsFinite(EmissionUV.X) == false || FMath::IsFinite(EmissionUV.Y) == false || FMath::IsFinite(EmissionUV.Z) == false || FMath::IsFinite(EmissionUV.W) == false))
	{
		OutError = TEXT("Original emission needs an HDR provider from this world, an upgraded prepared mesh and finite UV0 scale/offset.");
		return false;
	}
	if (Emission != nullptr && Emission->IsReady(OutError) == false)
	{
		return false;
	}
	const auto bUnlit = this->OriginalMaterials[MaterialSlot]->GetShadingModels().HasShadingModel(MSM_Unlit);
	auto* Texture = Parameters.Texture.Get();
	auto ColorUV = Texture != nullptr && Texture->IsColorProvider() ? FVector4f(Parameters.ProviderUVScale.X, Parameters.ProviderUVScale.Y,
																		  Parameters.ProviderUVOffset.X, Parameters.ProviderUVOffset.Y)
																	: FVector4f(1, 1, 0, 0);
	// Unlitは発光そのものが表面色。二つの異なる公開値を同じ材質へ割り当てない。
	if (bUnlit && Emission != nullptr)
	{
		if (Texture != nullptr && (Texture != Emission || ColorUV != EmissionUV))
		{
			OutError = TEXT("Unlit color and original emission must use the same HDR provider and UV transform. Leave Texture unset to bind both together.");
			return false;
		}
		Texture = Emission;
		ColorUV = EmissionUV;
	}
	if (this->Options.bInheritSourceEmission == true && this->Particles != nullptr &&
		((bUnlit && ((Texture != nullptr && Texture != Emission) || Tint.R != 1 || Tint.G != 1 || Tint.B != 1)) ||
			(this->PreparedMesh->SourceEmission.IsNull() && Emission == nullptr)))
	{
		OutError = TEXT("Baked Unlit emission requires unchanged source color while converting.");
		return false;
	}
	if (this->PreparedMesh->ValidateBakedMaterialParameters(MaterialSlot, this->OriginalMaterials[MaterialSlot], this->DissolveMaterials[MaterialSlot],
			OutError, Texture != nullptr && Texture->IsColorProvider()) == false)
	{
		return false;
	}
	if (this->PreparedMesh->TextureColorSources.IsValidIndex(MaterialSlot) &&
		this->PreparedMesh->TextureColorSources[MaterialSlot].bRequiresColorProvider == true &&
		(Texture == nullptr || Texture->IsColorProvider() == false))
	{
		OutError = this->PreparedMesh->TextureColorSources[MaterialSlot].DependencyDiagnostics;
		return false;
	}
	if (auto* Registration = Texture)
	{
		const auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
		const auto* Binding = this->PreparedMesh->TextureColorSources.IsValidIndex(MaterialSlot)
			? &this->PreparedMesh->TextureColorSources[MaterialSlot]
			: nullptr;
		auto* Original = Binding != nullptr ? Binding->Texture.Get() : nullptr;
		auto* Replacement = Registration->GetTexture();
		if (Registration->BelongsTo(Manager) == false)
		{
			OutError = TEXT("Surface color registration must belong to this world.");
			return false;
		}
		if (Registration->IsColorProvider() == true)
		{
			const auto UV = ColorUV;
			if (this->PreparedMesh->bSupportsColorProvider == false || FMath::IsFinite(UV.X) == false || FMath::IsFinite(UV.Y) == false ||
				FMath::IsFinite(UV.Z) == false || FMath::IsFinite(UV.W) == false)
			{
				OutError = TEXT("Color Provider requires an upgraded prepared mesh and finite UV0 scale/offset.");
				return false;
			}
		}
		else if (Binding == nullptr || Binding->bCompatible == false || Binding->RegisteredTextureParameter.IsNone() ||
			Original == nullptr || Replacement == nullptr || Original->SRGB != Replacement->SRGB ||
			Registration->BelongsTo(Manager) == false)
		{
			OutError = TEXT("Texture color needs a compatible prepared slot and a registration from this world with matching sRGB encoding.");
			return false;
		}
		if (Registration->IsReady(OutError) == false)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("Registered surface texture mips are still loading. Wait for Is Ready.");
			}
			return false;
		}
	}

	this->SurfaceTextures[MaterialSlot] = Texture;
	this->ProviderUVs[MaterialSlot] = ColorUV;
	this->EmissionProviders[MaterialSlot] = Emission;
	this->EmissionUVs[MaterialSlot] = EmissionUV;
	this->SurfaceTints[MaterialSlot] = FVector4f(Tint.R, Tint.G, Tint.B, 1);
	this->bSurfaceTintsDirty = true;
	if (bDeferToUpdate == false)
	{
		this->UploadSurfaceColorParameters();
	}
	return true;
}

void UMeshParticleizeSource::UploadSurfaceColorParameters()
{
	for (auto Slot = 0; Slot < this->SurfaceTextures.Num(); ++Slot)
	{
		const auto* Registration = this->SurfaceTextures[Slot].Get();
		const auto Revision = Registration != nullptr ? Registration->GetRevision() : 0;
		if (this->SurfaceColorRevisions[Slot] != Revision)
		{
			this->bSurfaceTintsDirty = true;
		}
		const auto* Emission = this->EmissionProviders[Slot].Get();
		if (this->EmissionRevisions[Slot] != (Emission != nullptr ? Emission->GetRevision() : 0))
		{
			this->bSurfaceTintsDirty = true;
		}
	}
	if (this->bSurfaceTintsDirty == false)
	{
		return;
	}

	// 進行マスクと同じ更新箇所で両方へ渡し、呼出し時刻による1フレームのずれを防ぐ。
	if (this->Particles != nullptr)
	{
		UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector4(this->Particles, TEXT("User.SurfaceTints"), MakeArrayView(this->SurfaceTints));
		const auto Variable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceColor"));
		auto* Color = Cast<UNiagaraDataInterfaceMeshParticleizerColor>(this->Particles->GetOverrideParameters().GetDataInterface(Variable));
		if (Color != nullptr)
		{
			auto Bindings = TArray<FMeshParticleizerGpuColorBinding>{};
			Bindings.SetNum(this->SurfaceTextures.Num());
			for (auto Slot = 0; Slot < Bindings.Num(); ++Slot)
			{
				auto* Registration = this->SurfaceTextures[Slot].Get();
				if (Registration == nullptr)
				{
					continue;
				}
				auto& Binding = Bindings[Slot];
				Binding.Texture = Registration->GetTexture();
				Binding.Revision = Registration->GetRevision();
				if (Registration->IsColorProvider() == true)
				{
					const auto UV = this->ProviderUVs[Slot];
					Binding.UVScale = FVector2D(UV.X, UV.Y);
					Binding.UVOffset = FVector2D(UV.Z, UV.W);
				}
				else
				{
					Binding.UVScale = this->PreparedMesh->TextureColorSources[Slot].UVScale;
					Binding.ColorScale = this->PreparedMesh->TextureColorSources[Slot].ColorScale;
				}
				if (this->Simulation != nullptr)
				{
					this->Simulation->SurfaceTextures.AddUnique(Registration);
				}
			}
			Color->SetBindings(Bindings);
		}
		const auto EmissionVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceEmission"));
		auto* Emission = Cast<UNiagaraDataInterfaceMeshParticleizerColor>(this->Particles->GetOverrideParameters().GetDataInterface(EmissionVariable));
		if (Emission != nullptr)
		{
			auto Bindings = TArray<FMeshParticleizerGpuColorBinding>{};
			Bindings.SetNum(this->EmissionProviders.Num());
			for (auto Slot = 0; Slot < Bindings.Num(); ++Slot)
			{
				auto* Registration = this->EmissionProviders[Slot].Get();
				if (Registration == nullptr)
				{
					continue;
				}
				auto& Binding = Bindings[Slot];
				Binding.Texture = Registration->GetTexture();
				Binding.Revision = Registration->GetRevision();
				const auto UV = this->EmissionUVs[Slot];
				Binding.UVScale = FVector2D(UV.X, UV.Y);
				Binding.UVOffset = FVector2D(UV.Z, UV.W);
				if (this->Simulation != nullptr)
				{
					this->Simulation->SurfaceTextures.AddUnique(Registration);
				}
			}
			Emission->SetBindings(Bindings);
		}
	}
	if (this->PreparedMesh->bSupportsSurfaceTint == true)
	{
		for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
		{
			auto* Registration = this->SurfaceTextures[Slot].Get();
			const auto bProvider = Registration != nullptr && Registration->IsColorProvider();
			this->SurfaceColorRevisions[Slot] = Registration != nullptr ? Registration->GetRevision() : 0;
			auto* Emission = this->EmissionProviders[Slot].Get();
			this->EmissionRevisions[Slot] = Emission != nullptr ? Emission->GetRevision() : 0;
			if (this->PreparedMesh->bSupportsEmissionProvider == true)
			{
				this->MaterialInputs[Slot].SetTexture(this->DissolveMaterials[Slot], TEXT("MP_EmissionProviderTexture"), Emission != nullptr ? Emission->GetTexture() : this->PreparedMesh->Colors.Get());
				this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_EmissionProviderUV"), FLinearColor(this->EmissionUVs[Slot]));
				this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_UseEmissionProvider"), Emission != nullptr ? 1.f : 0.f);
			}
			this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_SurfaceTint"), FLinearColor(this->SurfaceTints[Slot]));
			if (this->PreparedMesh->bSupportsColorProvider == true)
			{
				this->MaterialInputs[Slot].SetTexture(this->DissolveMaterials[Slot], TEXT("MP_ColorProviderTexture"), bProvider ? Registration->GetTexture() : this->PreparedMesh->Colors.Get());
				this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_ColorProviderUV"), FLinearColor(this->ProviderUVs[Slot]));
				this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_UseColorProvider"), bProvider ? 1.f : 0.f);
			}
			if (this->PreparedMesh->TextureColorSources.IsValidIndex(Slot))
			{
				const auto& Binding = this->PreparedMesh->TextureColorSources[Slot];
				if (Binding.RegisteredTextureParameter.IsNone() == false)
				{
					this->MaterialInputs[Slot].SetTexture(this->DissolveMaterials[Slot], Binding.RegisteredTextureParameter,
						Registration != nullptr && bProvider == false ? Registration->GetTexture() : Binding.Texture.Get());
				}
			}
		}
	}
	this->bSurfaceTintsDirty = false;
}

bool UMeshParticleizeSource::Initialize(const FMeshParticleizeOptions& InOptions,
	const FMeshParticleizeSchedule& InSchedule, const FMeshParticleizerProgressSpace& ProgressSpace,
	const FMeshParticleizerResolvedOrigin& Origin, TConstArrayView<FMeshParticleizerTarget> Targets, int32 ParticleCount, FString& OutError)
{
	auto* Mesh = this->Source.Get();
	auto* Prepared = this->PreparedMesh.Get();
	auto* Skinned = Cast<USkinnedMeshComponent>(Mesh);
	auto* SkeletalAsset = Skinned != nullptr ? Cast<USkeletalMesh>(Skinned->GetSkinnedAsset()) : nullptr;
	auto* System = this->PlaybackSystem.Get();
	auto* Positions = Prepared->Positions.Get();
	auto* Normals = Prepared->Normals.Get();
	auto* Colors = Prepared->Colors.Get();
	auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	const auto ColorVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceColor"));
	if (Prepared->NeedsRuntimeCoverage() && System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCoverage::StaticClass()), TEXT("User.SurfaceCoverage"))) == INDEX_NONE)
	{
		OutError = TEXT("The selected playback system requires the runtime coverage input. Upgrade it before playback.");
		return false;
	}
	if (Prepared->NeedsSurfaceColorCache() && System->GetExposedParameters().IndexOf(ColorVariable) == INDEX_NONE)
	{
		OutError = TEXT("The selected playback system needs the surface texture input. Upgrade or regenerate it before playback.");
		return false;
	}

	this->GrantedParticles = ParticleCount;
	this->PoseLeader = Skinned != nullptr ? Skinned->LeaderPoseComponent : nullptr;
	this->Diagnostics.Reset();
	for (auto Slot = 0; Slot < this->SurfaceTextures.Num(); ++Slot)
	{
		auto Mismatch = FString{};
		if (this->SurfaceTextures[Slot] != nullptr && this->SurfaceTextures[Slot]->IsColorProvider() &&
			Prepared->ValidateBakedMaterialParameters(Slot, this->OriginalMaterials[Slot], this->DissolveMaterials[Slot], Mismatch) == false)
		{
			this->Diagnostics += FString::Printf(TEXT("Material slot %d uses registered Color Provider fallback for changed color parameters. "), Slot);
		}
	}
	this->Source = Mesh;
	this->PreparedMesh = Prepared;
	this->Options = InOptions;
#if !UE_BUILD_SHIPPING
	this->bRetainSourceForProfiling = MeshParticleizerPlayback::RetainSourceForProfiling.GetValueOnGameThread() != 0 &&
		Origin.Mode == EMeshParticleizerOrigin::Direction && InOptions.bFreezeAtStart == false;
#endif
	this->ResolvedOrigin = Origin;
	this->Schedule = InSchedule;
	Manager->AcquireSourceHistory(Mesh);
	Manager->AcquireFullPose(Mesh);
	this->bTracksSource = true;
	if (Manager->FindSourceHistory(Mesh)->HasVelocity() == false)
	{
		this->Diagnostics += TEXT("No two-frame surface history; using initial, physics point or component velocity until history is ready. ");
	}
	// Opacity Maskの変化はVSMの自動判定に含まれないため、分離中だけ影を更新する。
	this->OriginalShadowCache = Mesh->ShadowCacheInvalidationBehavior;
	this->bShadowCacheOverridden = true;
	Mesh->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
	Mesh->MarkRenderStateDirty();

	if (InOptions.bFreezeAtStart == true && this->FreezeSource(OutError) == false)
	{
		this->RestoreShadowCache();
		this->RestoreSourceMaterials();
		this->ReleaseResources();
		return false;
	}

	{
		CSV_SCOPED_TIMING_STAT(MeshParticleizer, SetupNiagara);

		// 分離後の描画コンポーネントは元Actorへ付けず、ワールドが所有する。
		this->Particles = this->Simulation != nullptr ? this->Simulation->Particles.Get() : nullptr;
		if (this->Particles == nullptr)
		{
			this->Particles = NewObject<UNiagaraComponent>(Manager->GetWorld());
			this->Particles->SetAutoActivate(false);
			this->Particles->SetAsset(System);
			if (this->Simulation != nullptr)
			{
				this->Simulation->Particles = this->Particles;
			}
		}
		if (this->Simulation != nullptr)
		{
			// 待機中の参照を新しい資産へ引き継ぐ。キャッシュは古い資産のGC/GPU待ちも計上する。
			Manager->GetAssetCache()->Release(this->Simulation);
			auto Resources = Prepared->Resources;
			if (const auto* Style = InOptions.ParticleStyle.Get())
			{
				Resources.Append(Style->Resources);
			}
			if (Manager->GetAssetCache()->Acquire(this->Simulation, Resources, OutError) == false)
			{
				return false;
			}
		}
		const auto BudgetVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerBudget::StaticClass()), TEXT("User.Budget"));
		auto* GpuBudget = this->Simulation != nullptr && this->Simulation->bReusable == true
			? Cast<UNiagaraDataInterfaceMeshParticleizerBudget>(this->Particles->GetOverrideParameters().GetDataInterface(BudgetVariable))
			: nullptr;
		if (GpuBudget == nullptr)
		{
			GpuBudget = NewObject<UNiagaraDataInterfaceMeshParticleizerBudget>(this->Particles);
			this->Particles->GetOverrideParameters().SetDataInterface(GpuBudget, BudgetVariable);
		}
		this->GpuAllocation = MakeShared<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>(this->GrantedParticles);
		GpuBudget->Configure(this->GrantedParticles, static_cast<int32>(InOptions.Priority), InSchedule.StartTime,
			Manager->GetGpuState(), this->GpuAllocation.ToSharedRef());
		if (System->GetExposedParameters().IndexOf(ColorVariable) != INDEX_NONE)
		{
			auto* Color = Cast<UNiagaraDataInterfaceMeshParticleizerColor>(this->Particles->GetOverrideParameters().GetDataInterface(ColorVariable));
			if (Color == nullptr)
			{
				Color = NewObject<UNiagaraDataInterfaceMeshParticleizerColor>(this->Particles);
				this->Particles->GetOverrideParameters().SetDataInterface(Color, ColorVariable);
			}
			Color->Configure(Positions, Prepared->Coordinates.Get(), this->Simulation != nullptr && this->Simulation->bSurfaceColorCache,
				this->GpuAllocation.ToSharedRef());
		}
		if (this->Simulation != nullptr)
		{
			const auto CoverageVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCoverage::StaticClass()), TEXT("User.SurfaceCoverage"));
			if (System->GetExposedParameters().IndexOf(CoverageVariable) != INDEX_NONE)
			{
				this->SurfaceCoverage = Cast<UNiagaraDataInterfaceMeshParticleizerCoverage>(this->Particles->GetOverrideParameters().GetDataInterface(CoverageVariable));
				if (this->SurfaceCoverage == nullptr)
				{
					this->SurfaceCoverage = NewObject<UNiagaraDataInterfaceMeshParticleizerCoverage>(this->Particles);
					this->Particles->GetOverrideParameters().SetDataInterface(this->SurfaceCoverage, CoverageVariable);
				}
				if (this->SurfaceCoverage->Configure(Prepared, this->DissolveMaterials, this->Simulation->bSurfaceCoverage, this->GpuAllocation.ToSharedRef(), OutError) == false)
				{
					return false;
				}
			}
			this->Simulation->CoverageMaterials = this->Simulation->bSurfaceCoverage ? this->DissolveMaterials : TArray<TObjectPtr<UMaterialInstanceDynamic>>{};
			const auto EmissionVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceEmission"));
			if (System->GetExposedParameters().IndexOf(EmissionVariable) != INDEX_NONE)
			{
				auto* Emission = Cast<UNiagaraDataInterfaceMeshParticleizerColor>(this->Particles->GetOverrideParameters().GetDataInterface(EmissionVariable));
				if (Emission == nullptr)
				{
					Emission = NewObject<UNiagaraDataInterfaceMeshParticleizerColor>(this->Particles);
					this->Particles->GetOverrideParameters().SetDataInterface(Emission, EmissionVariable);
				}
				Emission->Configure(Positions, Prepared->Coordinates.Get(), this->Simulation->bSourceEmissionCache, this->GpuAllocation.ToSharedRef(), true);
			}
			// 全DIが旧割当を取得してから置き換え、Render Threadとの競合で再利用元を失わないようにする。
			this->Simulation->Allocation = this->GpuAllocation;
			this->Simulation->SurfaceTextures.Reset();
		}
		if (InOptions.Collision == EMeshParticleizerCollision::GlobalDistanceField)
		{
			const auto CollisionVariable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCollision::StaticClass()), TEXT("User.Collision"));
			auto* Collision = Cast<UNiagaraDataInterfaceMeshParticleizerCollision>(this->Particles->GetOverrideParameters().GetDataInterface(CollisionVariable));
			if (Collision == nullptr)
			{
				Collision = NewObject<UNiagaraDataInterfaceMeshParticleizerCollision>(this->Particles);
				this->Particles->GetOverrideParameters().SetDataInterface(Collision, CollisionVariable);
			}
			Collision->Configure(Manager->GetDistanceField());
		}
		this->Particles->SetAllowScalability(false);
		// PostPhysics後の履歴と消失マスクを、同じフレームのGPU更新へ渡す。
		this->Particles->SetTickBehavior(ENiagaraTickBehavior::ForceTickLast);

		// プールから再利用しても前回の色を残さず、今回の登録スロット数で上書きする。
		this->bSurfaceTintsDirty = true;
		this->UploadSurfaceColorParameters();
		this->Particles->SetVariableInt(TEXT("User.Count"), this->GrantedParticles);
		this->Particles->SetVariableInt(TEXT("User.Mode"), static_cast<int32>(Origin.Mode));
		this->Particles->SetVariableInt(TEXT("User.Motion"), static_cast<int32>(InOptions.Motion));
		this->Particles->SetVariableInt(TEXT("User.Disappear"), static_cast<int32>(InOptions.Disappear));
		this->Particles->SetVariableVec3(TEXT("User.BoundsMin"), ProgressSpace.Bounds.Min);
		this->Particles->SetVariableVec3(TEXT("User.BoundsMax"), ProgressSpace.Bounds.Max);
		this->Particles->SetVariableVec3(TEXT("User.Direction"), Origin.ReferenceDirection);
		this->Particles->SetVariableVec3(TEXT("User.Origin"), Origin.ReferencePosition);
		this->Particles->SetVariableInt(TEXT("User.OriginCount"), FMath::Max(Origin.ReferencePoints.Num(), 1));
		this->Particles->SetVariableFloat(TEXT("User.OriginDistance"), Origin.ReferenceDistance);
		for (auto Index = 1; Index < 4; ++Index)
		{
			const auto Point = Origin.ReferencePoints.IsValidIndex(Index) ? Origin.ReferencePoints[Index] : FVector::ZeroVector;
			this->Particles->SetVariableVec3(FName(FString::Printf(TEXT("User.Origin%d"), Index)), Point);
		}
		this->Particles->SetVariableFloat(TEXT("User.Seed"), InOptions.Seed);
		this->Particles->SetVariableFloat(TEXT("User.Duration"), InOptions.ConversionDuration);
		const auto* Curve = InSchedule.Curve.Get();
		this->Particles->SetVariableInt(TEXT("User.CurveSegmentCount"), Curve != nullptr ? Curve->GetSegments().Num() / 2 : 0);
		if (Curve != nullptr)
		{
			UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector4(this->Particles, TEXT("User.ProgressCurve"), MakeArrayView(Curve->GetSegments()));
		}
		this->Particles->SetVariableFloat(TEXT("User.Lifetime"), InOptions.ParticleLifetime);
		this->Particles->SetVariableFloat(TEXT("User.TotalDuration"), FMath::Max(InOptions.ConversionDuration + InOptions.ParticleLifetime, 0.001f));
		this->Particles->SetVariableFloat(TEXT("User.ParticleSize"), InOptions.ParticleSize);
		if (System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.MeshRendererVersion"))) != INDEX_NONE)
		{
			this->Particles->SetVariableVec3(TEXT("User.MeshAngularVelocity"), InOptions.MeshAngularVelocity);
			this->Particles->SetVariableFloat(TEXT("User.AlignMeshToSurface"), InOptions.bAlignMeshToSurface ? 1.f : 0.f);
		}
		this->Particles->SetVariableFloat(TEXT("User.FadeDuration"), InOptions.FadeDuration);
		this->Particles->SetVariableFloat(TEXT("User.Emissive"), InOptions.Emissive);
		this->Particles->SetVariableFloat(TEXT("User.InheritSourceEmission"), InOptions.bInheritSourceEmission ? 1.f : 0.f);
		this->Particles->SetVariableFloat(TEXT("User.HasBakedEmission"), Prepared->SourceEmission.IsNull() == false ? 1.f : 0.f);
		this->Particles->SetVariableFloat(TEXT("User.InheritVelocity"), InOptions.InheritVelocityScale);
		this->Particles->SetVariableFloat(TEXT("User.NormalSpeed"), InOptions.NormalSpeed);
		this->Particles->SetVariableFloat(TEXT("User.ScatterSpeed"), InOptions.ScatterSpeed);
		this->Particles->SetVariableFloat(TEXT("User.RandomSpeed"), InOptions.RandomSpeed);
		this->Particles->SetVariableFloat(TEXT("User.NoiseAcceleration"), InOptions.NoiseAcceleration);
		this->Particles->SetVariableFloat(TEXT("User.Drag"), InOptions.Drag);
		this->Particles->SetVariableFloat(TEXT("User.Restitution"), InOptions.Restitution);
		this->Particles->SetVariableFloat(TEXT("User.Friction"), InOptions.Friction);
		this->Particles->SetVariableFloat(TEXT("User.CollisionRadiusScale"), InOptions.CollisionRadiusScale);
		this->Particles->SetVariableFloat(TEXT("User.SleepSpeed"), InOptions.SleepSpeed);
		this->Particles->SetVariableFloat(TEXT("User.SleepDuration"), InOptions.SleepDuration);
		this->Particles->SetVariableFloat(TEXT("User.MaxInitialSpeed"), InOptions.MaxInitialSpeed);
		this->Particles->SetVariableVec3(TEXT("User.Gravity"), InOptions.GetEffectiveGravity());
		this->Particles->SetVariableVec3(TEXT("User.Wind"), InOptions.Wind);
		this->Particles->SetVariableFloat(TEXT("User.SourceAvailable"), 1);
		this->Particles->SetVariableFloat(TEXT("User.Elapsed"), 0);
		this->Particles->SetVariableFloat(TEXT("User.PreviousElapsed"), 0);
		this->Particles->SetVariableLinearColor(TEXT("User.Tint"), InOptions.Tint);

		// 公開する床判定とGPUへ渡す値を同じ結果から設定する。
		this->ResolvedGround = FMeshParticleizerResolvedGround{};
		if (InOptions.Collision == EMeshParticleizerCollision::GlobalDistanceField)
		{
			this->ResolvedGround.Status = EMeshParticleizerGroundStatus::GlobalDistanceField;
			this->ResolvedGround.Diagnostics = TEXT("GDF collision; source excluded from distance-field lighting. ");
		}
		else if (InOptions.Ground == EMeshParticleizerGround::ExplicitHeight)
		{
			this->ResolvedGround.Status = EMeshParticleizerGroundStatus::ExplicitHeight;
			this->ResolvedGround.bEnabled = true;
			this->ResolvedGround.Height = InOptions.GroundHeight;
			this->ResolvedGround.Diagnostics = FString::Printf(TEXT("Simple Ground explicitly set at Z=%.1f cm. "), InOptions.GroundHeight);
		}
		else if (InOptions.Ground == EMeshParticleizerGround::Automatic)
		{
			this->ResolvedGround = MeshParticleizerPlayback::FindGround(Mesh, Targets);
		}
		else
		{
			this->ResolvedGround.Status = EMeshParticleizerGroundStatus::Disabled;
			this->ResolvedGround.Diagnostics = TEXT("No collision plane; ground collision is disabled. Particles fall freely.");
		}
		this->Particles->SetVariableFloat(TEXT("User.GroundEnabled"), this->ResolvedGround.bEnabled ? 1 : 0);
		this->Particles->SetVariableFloat(TEXT("User.GroundHeight"), this->ResolvedGround.Height);
		this->Diagnostics += this->ResolvedGround.Diagnostics;

		UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.PositionData"), Positions);
		UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.NormalData"), Normals);
		UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.ColorData"), Colors);
		UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.EmissionData"), InOptions.bInheritSourceEmission && Prepared->SourceEmission.Get() != nullptr ? Prepared->SourceEmission.Get() : Colors);
		this->Particles->SetVariableFloat(TEXT("User.Skinned"), SkeletalAsset != nullptr ? 1 : 0);
		if (SkeletalAsset != nullptr)
		{
			UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.CoordinateData"), Prepared->Coordinates.Get());
			UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.SkinVertices"), Prepared->SkinVertices.Get());
			UNiagaraFunctionLibrary::SetTextureObject(this->Particles, TEXT("User.SkinTriangles"), Prepared->SkinTriangles.Get());
			this->Diagnostics += TEXT("Bone skinning uses LOD0 while converting; pose and LOD policy are restored afterwards. ");
		}
		this->UpdateSourceMotion(Mesh);
		if (this->Particles->IsRegistered() == false)
		{
			this->Particles->RegisterComponentWithWorld(Manager->GetWorld());
		}
	}

	{
		CSV_SCOPED_TIMING_STAT(MeshParticleizer, ConfigureMaterial);

		// マスクとGPUの分離順序へ同じ固定変換を渡す。移動・姿勢更新では上書きしない。
		const FName ParticleNames[] = {TEXT("User.ProgressX"), TEXT("User.ProgressY"), TEXT("User.ProgressZ")};
		const FName MaterialNames[] = {TEXT("MP_ProgressX"), TEXT("MP_ProgressY"), TEXT("MP_ProgressZ")};
		const auto& Matrix = ProgressSpace.SourceToGroup;
		for (auto Axis = 0; Axis < 3; ++Axis)
		{
			const auto Row = FVector4(Matrix.M[0][Axis], Matrix.M[1][Axis], Matrix.M[2][Axis], Matrix.M[3][Axis]);
			this->Particles->SetVariableVec4(ParticleNames[Axis], Row);
			for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
			{
				this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], MaterialNames[Axis], FLinearColor(Row));
			}
		}
		// 保持計測でも負の進行率でシェーダーを短絡せず、端面の順序値を0より大きくする。
		const auto DisplayPadding = this->bRetainSourceForProfiling ? FVector(1) : FVector::ZeroVector;
		for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
		{
			this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_BoundsMin"), FLinearColor(ProgressSpace.Bounds.Min - DisplayPadding));
			this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_BoundsMax"), FLinearColor(ProgressSpace.Bounds.Max + DisplayPadding));
			this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_Direction"), FLinearColor(Origin.ReferenceDirection));
			this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], TEXT("MP_Origin"), FLinearColor(Origin.ReferencePosition));
			this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_OriginCount"), FMath::Max(Origin.ReferencePoints.Num(), 1));
			this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_OriginDistance"), Origin.ReferenceDistance);
			for (auto Index = 1; Index < 4; ++Index)
			{
				const auto Point = Origin.ReferencePoints.IsValidIndex(Index) ? Origin.ReferencePoints[Index] : FVector::ZeroVector;
				this->MaterialInputs[Slot].SetVector(this->DissolveMaterials[Slot], FName(FString::Printf(TEXT("MP_Origin%d"), Index)), FLinearColor(Point));
			}
			this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_Mode"), static_cast<int32>(Origin.Mode));
			this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_Seed"), InOptions.Seed);
			this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_InheritSourceEmission"), InOptions.bInheritSourceEmission ? 1.f : 0.f);
		}
	}

	return true;
}

void UMeshParticleizeSource::Activate()
{
	if (this->GrantedParticles > 0 && this->Options.ParticleLifetime > 0)
	{
		CSV_SCOPED_TIMING_STAT(MeshParticleizer, ActivateNiagara);
		if (this->Simulation != nullptr && this->Simulation->bReusable == true)
		{
			this->Simulation->PrepareForReuse();
		}
		this->Particles->Activate(true);
		this->Particles->SetRenderingEnabled(true);
		this->Particles->SetVisibility(true);
		if (this->Simulation != nullptr)
		{
			this->Simulation->bReusable = true;
		}
	}
	if (this->Options.bFreezeAtStart == true)
	{
		this->ReleaseSourceHistory();
	}
}

void UMeshParticleizeSource::UpdateTransformParameters(const FTransform& Current, const FTransform& Previous)
{
	if (this->SurfaceCoverage != nullptr)
	{
		this->SurfaceCoverage->SetTransform(Current);
	}
	// HLSLの行ベクトルへ平行移動を含める。Actor速度を重ねて加算しない。
	const auto Now = Current.ToMatrixWithScale();
	const auto Before = Previous.ToMatrixWithScale();
	const auto Normal = Now.Inverse().GetTransposed();
	const FName CurrentNames[] = {TEXT("User.TransformX"), TEXT("User.TransformY"), TEXT("User.TransformZ")};
	const FName PreviousNames[] = {TEXT("User.PreviousX"), TEXT("User.PreviousY"), TEXT("User.PreviousZ")};
	const FName NormalNames[] = {TEXT("User.NormalX"), TEXT("User.NormalY"), TEXT("User.NormalZ")};
	for (auto Axis = 0; Axis < 3; ++Axis)
	{
		this->Particles->SetVariableVec4(CurrentNames[Axis], FVector4(Now.M[0][Axis], Now.M[1][Axis], Now.M[2][Axis], Now.M[3][Axis]));
		this->Particles->SetVariableVec4(PreviousNames[Axis], FVector4(Before.M[0][Axis], Before.M[1][Axis], Before.M[2][Axis], Before.M[3][Axis]));
		this->Particles->SetVariableVec3(NormalNames[Axis], FVector(Normal.M[0][Axis], Normal.M[1][Axis], Normal.M[2][Axis]));
	}
}

void UMeshParticleizeSource::UpdateSourceMotion(UMeshComponent* Mesh)
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, SourceMotion);
	const auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	if (this->bFrozenMotionUploaded == true)
	{
		// 骨と速度履歴を固定したまま、原点移動だけ描画とGPUへ反映する。
		const auto Origin = Manager->GetWorld()->OriginLocation;
		if (this->FrozenTransforms.Origin != Origin)
		{
			this->FrozenFallbackCenter += FVector(this->FrozenTransforms.Origin) - FVector(Origin);
			this->FrozenTransforms.Rebase(Origin);
			this->UpdateTransformParameters(this->FrozenTransforms.Current,
				this->FrozenTransforms.HasVelocity() ? this->FrozenTransforms.Previous : this->FrozenTransforms.Current);
			this->Particles->SetVariableVec3(TEXT("User.FallbackCenter"), this->FrozenFallbackCenter);
			if (this->FrozenMesh != nullptr)
			{
				this->FrozenMesh->SetWorldTransform(this->FrozenTransforms.Current);
			}
		}
		return;
	}
	const auto* History = this->Options.bFreezeAtStart ? &this->FrozenTransforms : Manager->FindSourceHistory(Mesh);
	const auto bHasHistory = History != nullptr && History->HasVelocity() == true;
	const auto Current = History != nullptr && History->bHasCurrent == true ? History->Current : Mesh->GetComponentTransform();
	const auto Previous = bHasHistory ? History->Previous : Current;
	this->UpdateTransformParameters(Current, Previous);
	this->Particles->SetVariableFloat(TEXT("User.HistoryValid"), bHasHistory ? 1 : 0);
	this->Particles->SetVariableFloat(TEXT("User.PoseDeltaSeconds"), bHasHistory ? History->CurrentTime - History->PreviousTime : 0);
	this->Particles->SetVariableFloat(TEXT("User.PosePreviousElapsed"), bHasHistory ? History->PreviousTime - this->Schedule.StartTime : 0);

	if (this->PreparedMesh->BoneCount > 0)
	{
		const auto* Bones = this->Options.bFreezeAtStart ? &this->FrozenBones : Manager->FindBoneHistory(Mesh);
		if (Bones != nullptr && Bones->bHasCurrent == true)
		{
			const auto bBoneVelocity = Bones->HasVelocity();
			this->Particles->SetVariableFloat(TEXT("User.BoneHistoryValid"), bBoneVelocity ? 1 : 0);
			this->Particles->SetVariableFloat(TEXT("User.BoneDeltaSeconds"), bBoneVelocity ? Bones->CurrentTime - Bones->PreviousTime : 0);
			this->Particles->SetVariableFloat(TEXT("User.BonePreviousElapsed"), bBoneVelocity ? Bones->PreviousTime - this->Schedule.StartTime : 0);
			if (this->UploadedBoneSerial != Bones->Serial)
			{
				CSV_SCOPED_TIMING_STAT(MeshParticleizer, BoneUpload);
				UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector4(this->Particles, TEXT("User.Bones"), MakeArrayView(Bones->Current));
				UNiagaraDataInterfaceArrayFunctionLibrary::SetNiagaraArrayVector4(this->Particles, TEXT("User.PreviousBones"), MakeArrayView(bBoneVelocity ? Bones->Previous : Bones->Current));
				if (this->SurfaceCoverage != nullptr)
				{
					this->SurfaceCoverage->SetBones(Bones->Current);
				}
				this->UploadedBoneSerial = Bones->Serial;
			}
		}
	}

	// 履歴がない場合だけ代替速度を選ぶ。テレポート直後のComponent速度は使わない。
	auto Velocity = FVector::ZeroVector;
	auto Angular = FVector::ZeroVector;
	auto Center = Current.GetLocation();
	if (bHasHistory == false)
	{
		if (this->Options.bUseInitialVelocity == true)
		{
			Velocity = this->Options.InitialVelocity;
		}
		else if (History == nullptr || History->bDiscontinuous == false)
		{
			if (Mesh->IsSimulatingPhysics() == true)
			{
				Velocity = Mesh->GetPhysicsLinearVelocity();
				Angular = Mesh->GetPhysicsAngularVelocityInRadians();
				Center = Mesh->GetCenterOfMass();
			}
			else
			{
				Velocity = Mesh->GetComponentVelocity();
			}
		}
	}
	if (Velocity.ContainsNaN() == true)
	{
		Velocity = FVector::ZeroVector;
	}
	if (Angular.ContainsNaN() == true)
	{
		Angular = FVector::ZeroVector;
	}
	this->Particles->SetVariableVec3(TEXT("User.FallbackVelocity"), Velocity);
	this->Particles->SetVariableVec3(TEXT("User.FallbackAngularVelocity"), Angular);
	this->Particles->SetVariableVec3(TEXT("User.FallbackCenter"), Center);
	this->FrozenFallbackCenter = Center;
	this->bFrozenMotionUploaded = this->Options.bFreezeAtStart;
}

void UMeshParticleizeSource::ReleaseSourceHistory()
{
	if (this->bTracksSource == false)
	{
		return;
	}
	this->bTracksSource = false;
	if (auto* Mesh = this->Source.Get())
	{
		auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
		Manager->ReleaseFullPose(Mesh);
		Manager->ReleaseSourceHistory(Mesh);
	}
}

void UMeshParticleizeSource::RestoreDistanceField()
{
	auto* Mesh = this->Source.Get();
	if (this->bDistanceFieldOverridden == true)
	{
		// 自分が無効化した値だけを戻す。外部から再有効化された場合は触らない。
		if (Mesh != nullptr && Mesh->bAffectDistanceFieldLighting == false)
		{
			Mesh->SetAffectDistanceFieldLighting(true);
		}
		this->bDistanceFieldOverridden = false;
	}
}

void UMeshParticleizeSource::RestoreSourceMaterials()
{
	this->RestoreDistanceField();
	auto* Mesh = this->Source.Get();
	for (auto Slot = 0; Slot < this->OriginalMaterials.Num(); ++Slot)
	{
		// 外部で差し替えたマテリアルは復元処理で上書きしない。
		if (Mesh != nullptr && Mesh->GetMaterial(Slot) == this->DissolveMaterials[Slot])
		{
			Mesh->SetMaterial(Slot, this->OriginalMaterials[Slot]);
		}
		this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->GetAssetCache()->Release(this->DissolveMaterials[Slot]);
	}
	this->OriginalMaterials.Reset();
	this->DissolveMaterials.Reset();
	this->MaterialParameterKeys.Reset();
	this->MaterialInputs.Reset();
	this->SurfaceTints.Reset();
	this->SurfaceTextures.Reset();
	this->ProviderUVs.Reset();
	this->SurfaceColorRevisions.Reset();
	this->EmissionProviders.Reset();
	this->EmissionUVs.Reset();
	this->EmissionRevisions.Reset();
	this->bSurfaceTintsDirty = false;
}

bool UMeshParticleizeSource::IsSourceAvailable(UMeshComponent* Mesh, FString& OutError) const
{
	OutError.Reset();
	if (Mesh == nullptr)
	{
		return false;
	}
	// 表示材の所有権と元材の固定入力を毎回確かめ、変更された表面から分離を続けない。
	if (Mesh->GetNumMaterials() != this->DissolveMaterials.Num())
	{
		OutError = TEXT("The source material slot count changed.");
		return false;
	}
	for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
	{
		if (Mesh->GetMaterial(Slot) != this->DissolveMaterials[Slot] ||
			(this->FrozenMesh != nullptr && this->FrozenMesh->GetMaterial(Slot) != this->DissolveMaterials[Slot]) ||
			this->DissolveMaterials[Slot]->Parent != this->PreparedMesh->DissolveMaterials[Slot].Get())
		{
			OutError = FString::Printf(TEXT("Material slot %d was replaced externally."), Slot);
			return false;
		}
		if (this->MaterialInputs[Slot].Validate(this->DissolveMaterials[Slot], Slot, OutError) == false)
		{
			return false;
		}
		auto* OriginalParent = this->OriginalMaterials[Slot].Get();
		while (const auto* Dynamic = Cast<UMaterialInstanceDynamic>(OriginalParent))
		{
			OriginalParent = Dynamic->Parent;
		}
		if (OriginalParent != this->PreparedMesh->SourceMaterials[Slot].Get())
		{
			OutError = FString::Printf(TEXT("Material slot %d changed its source parent."), Slot);
			return false;
		}
		const auto* Registration = this->SurfaceTextures[Slot].Get();
		const auto bUnlit = this->OriginalMaterials[Slot]->GetShadingModels().HasShadingModel(MSM_Unlit);
		const auto bAllowProvider = Registration != nullptr && Registration->IsColorProvider() &&
			(this->Options.bInheritSourceEmission == false || bUnlit == false || this->EmissionProviders[Slot] != nullptr);
		if (this->PreparedMesh->ValidateBakedMaterialParameters(Slot, this->OriginalMaterials[Slot], this->DissolveMaterials[Slot],
				OutError, bAllowProvider, &this->MaterialParameterKeys[Slot]) == false)
		{
			return false;
		}
	}

	for (const auto& Registration : this->EmissionProviders)
	{
		if (Registration != nullptr && Registration->IsReady(OutError) == false)
		{
			return false;
		}
	}
	for (const auto& Registration : this->SurfaceTextures)
	{
		if (Registration != nullptr && Registration->IsReady(OutError) == false)
		{
			return false;
		}
	}
	if (const auto* Static = Cast<UStaticMeshComponent>(Mesh))
	{
		return Static->GetStaticMesh() != nullptr && Static->GetStaticMesh() == this->PreparedMesh->SourceMesh.Get();
	}

	auto* Skinned = Cast<USkinnedMeshComponent>(Mesh);
	if (this->Options.bFreezeAtStart == true)
	{
		return this->FrozenMesh != nullptr && Skinned != nullptr && Skinned->GetSkinnedAsset() != nullptr &&
			Skinned->GetSkinnedAsset() == this->PreparedMesh->SkeletalMesh.Get();
	}
	const auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	return Skinned != nullptr && Skinned->GetSkinnedAsset() != nullptr && Skinned->GetSkinnedAsset() == this->PreparedMesh->SkeletalMesh.Get() &&
		Skinned->LeaderPoseComponent == this->PoseLeader && Manager->IsBonePoseReady(Skinned, this->PreparedMesh->BoneCount);
}

void UMeshParticleizeSource::RestoreShadowCache()
{
	if (this->bShadowCacheOverridden == false)
	{
		return;
	}
	this->bShadowCacheOverridden = false;
	if (auto* Mesh = this->Source.Get())
	{
		if (Mesh->ShadowCacheInvalidationBehavior == EShadowCacheInvalidationBehavior::Always)
		{
			Mesh->ShadowCacheInvalidationBehavior = this->OriginalShadowCache;

			// 最終マスクを反映したキャッシュを作り直し、その後は毎フレーム無効化しない。
			Mesh->MarkRenderStateDirty();
		}
	}
}

void UMeshParticleizeSource::Update(double Now, float Progress, bool bReadSource)
{
	const auto Elapsed = FMath::Max(0.0, Now - this->Schedule.StartTime);
	this->Particles->SetVariableFloat(TEXT("User.Elapsed"), Elapsed);
	this->Particles->SetVariableFloat(TEXT("User.PreviousElapsed"), this->PreviousElapsed);
	if (bReadSource == true)
	{
		this->UploadSurfaceColorParameters();
		this->UpdateSourceMotion(this->Source.Get());
		for (auto Slot = 0; Slot < this->DissolveMaterials.Num(); ++Slot)
		{
			this->MaterialInputs[Slot].SetScalar(this->DissolveMaterials[Slot], TEXT("MP_Progress"), this->bRetainSourceForProfiling ? 0.f : Progress);
		}
	}
	else if (this->SurfaceCoverage != nullptr)
	{
		// 変換完了を通知するフレームの評価は残し、その次の更新から分離前の走査を省く。
		this->SurfaceCoverage->StopEvaluating();
	}
	this->PreviousElapsed = Elapsed;
}

void UMeshParticleizeSource::StopReading()
{
	this->ReleaseSourceHistory();
	this->RestoreShadowCache();
	this->ReleaseFrozenSource();
}

void UMeshParticleizeSource::StopDetachment()
{
	if (this->Particles != nullptr)
	{
		this->Particles->SetVariableFloat(TEXT("User.SourceAvailable"), 0);
	}
	this->StopReading();
	this->RestoreSourceMaterials();
}

bool UMeshParticleizeSource::ReleaseResources()
{
	this->StopReading();
	const auto bMayHaveGpuWork = this->Particles != nullptr && this->GrantedParticles > 0 && this->Options.ParticleLifetime > 0;
	if (this->Simulation != nullptr)
	{
		this->Simulation->Stop();
	}
	else if (this->Particles != nullptr)
	{
		// Niagara自身の非同期解放を利用し、ゲームスレッドからGPUをFlushしない。
		this->Particles->DestroyComponent();
	}
	this->Particles = nullptr;
	this->SurfaceCoverage = nullptr;

	// 待機できる容量はSimulationが持つ。終了したOperationは次の再生を参照しない。
	this->GpuAllocation.Reset();
	return bMayHaveGpuWork;
}
