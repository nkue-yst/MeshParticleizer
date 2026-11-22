#include "MeshParticleizerPreview.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerParticleStyle.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "StaticMeshCompiler.h"
#include "Components/MeshComponent.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/StrongObjectPtr.h"

bool UMeshParticleizerPreview::PrepareParticleDisplayMaterials(EMeshParticleizerPreviewDisplay Mode, FString& OutError)
{
	// 選択中のStyleを通常再生と揃える。材のコンパイルでGCされてもStyleを保持する。
	const auto Style = TStrongObjectPtr<UMeshParticleizerParticleStyle>(this->Options.ParticleStyle.LoadSynchronous());
	if (this->Options.ParticleStyle.IsNull() == false && Style.IsValid() == false)
	{
		OutError = TEXT("Load the selected Particle Style before changing the particle view.");
		return false;
	}

	for (const auto& Reference : {Style.IsValid() ? Style->ParticleSystem : this->Prepared->ParticleSystem,
		Style.IsValid() ? Style->GdfParticleSystem : this->Prepared->GdfParticleSystem})
	{
		auto* System = Reference.LoadSynchronous();
		if (System == nullptr)
		{
			OutError = TEXT("Load the prepared particle systems before changing the particle view.");
			return false;
		}
		const auto SystemGuard = TStrongObjectPtr<UNiagaraSystem>(System);
		const auto bHasFar = System->GetExposedParameters().GetParameterValue<int32>(
			FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.FarSpriteVersion"))) == 1;
		for (const auto bFar : {false, true})
		{
			if (bFar == true && bHasFar == false)
			{
				continue;
			}
			auto& Cached = bFar ? (Mode == EMeshParticleizerPreviewDisplay::Velocity ? this->FarParticleVelocityMaterials : this->FarParticleColorMaterials) : (Mode == EMeshParticleizerPreviewDisplay::Velocity ? this->ParticleVelocityMaterials : this->ParticleColorMaterials);
			if (Cached.Contains(System))
			{
				continue;
			}
			const auto Binding = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), bFar ? TEXT("User.FarParticleMaterial") : TEXT("User.ParticleMaterial"));
			auto* Source = static_cast<UMaterialInterface*>(nullptr);
			auto* ParticleMesh = static_cast<UStaticMesh*>(nullptr);
			auto RendererCount = 0;
			for (const auto& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetIsEnabled() == false)
				{
					continue;
				}
				const auto* Data = Handle.GetInstance().GetEmitterData();
				if (Data == nullptr)
				{
					continue;
				}
				for (auto* Renderer : Data->GetRenderers())
				{
					if (Renderer == nullptr || Renderer->GetIsEnabled() == false)
					{
						continue;
					}
					++RendererCount;
					const auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer);
					if (Sprite != nullptr && Sprite->MaterialUserParamBinding.Parameter == Binding)
					{
						Source = Sprite->Material;
					}
					const auto* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer);
					if (MeshRenderer != nullptr && MeshRenderer->bOverrideMaterials == true && MeshRenderer->OverrideMaterials.Num() == 1 &&
						MeshRenderer->OverrideMaterials[0].UserParamBinding.Parameter == Binding && MeshRenderer->Meshes.Num() == 1 &&
						MeshRenderer->Meshes[0].Mesh != nullptr)
					{
						Source = MeshRenderer->OverrideMaterials[0].ExplicitMat;
						ParticleMesh = MeshRenderer->Meshes[0].Mesh;
					}
				}
			}
			if (RendererCount != (bHasFar ? 2 : 1) || Source == nullptr || System->GetExposedParameters().IndexOf(Binding) == INDEX_NONE)
			{
				OutError = TEXT("Upgrade the generated particle systems to enable their particle material binding.");
				return false;
			}

			if (Mode == EMeshParticleizerPreviewDisplay::Velocity)
			{
				const auto Version = FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.PreviewDataVersion"));
				if (System->GetExposedParameters().GetParameterValue<int32>(Version) != 1)
				{
					OutError = TEXT("Upgrade the generated particle systems to expose stable sample IDs for velocity display.");
					return false;
				}
				if (ParticleMesh != nullptr)
				{
					// 生成メッシュの各三角形を表示用の大きな三角形へ写し、矢印全体を覆う。
					// 別のUV配置を半分の矢印として表示せず、対応する生成メッシュの選択を求める。
					FStaticMeshCompilingManager::Get().FinishCompilation({ParticleMesh});
					const auto* RenderData = ParticleMesh->GetRenderData();
					const auto* LOD = RenderData != nullptr && RenderData->LODResources.Num() == 1 ? &RenderData->LODResources[0] : nullptr;
					if (LOD == nullptr || LOD->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
					{
						OutError = TEXT("Velocity display requires a compiled generated particle mesh.");
						return false;
					}
					const FVector2f Expected[] = {{0, 0}, {1, 0}, {0, 1}};
					for (auto Index = 0; Index < LOD->IndexBuffer.GetNumIndices(); ++Index)
					{
						if (LOD->VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(LOD->IndexBuffer.GetIndex(Index), 0).Equals(Expected[Index % 3]) == false)
						{
							OutError = TEXT("Mesh velocity arrows require the triangle UVs produced by Create Particle Mesh. Surface Color and Effect remain available for other meshes.");
							return false;
						}
					}
				}
				auto FarDistance = 0.f;
				if (bFar == true && Source->GetScalarParameterValue(FMaterialParameterInfo(TEXT("MP_FarSpriteDistance")), FarDistance) == false)
				{
					OutError = TEXT("The far Sprite material is missing its switching distance.");
					return false;
				}
				auto* Velocity = this->CreateVelocityMaterial(ParticleMesh != nullptr, OutError, FarDistance);
				if (Velocity == nullptr)
				{
					return false;
				}
				Cached.Add(System, Velocity);
				continue;
			}

			// 標準粒子材の形状・寿命Fadeを残し、保存済みの継承色をUnlitで描く。
			const auto SourceGuard = TStrongObjectPtr<UMaterialInterface>(Source);
			auto MaterialGuard = TStrongObjectPtr<UMaterial>(DuplicateObject<UMaterial>(Source->GetMaterial(), this));
			auto* Material = MaterialGuard.Get();
			Material->ClearFlags(RF_Public | RF_Standalone);
			Material->SetFlags(RF_Transient);
			Material->SetShadingModel(MSM_Unlit);
			auto* Color = Cast<UMaterialExpressionDynamicParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionDynamicParameter::StaticClass()));
			Color->ParameterIndex = 0;
			Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(4, Color);
			UMaterialEditingLibrary::RecompileMaterial(Material);
			if (auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
			{
				Resource->FinishCompilation();
				if (Resource->GetCompileErrors().IsEmpty() == false)
				{
					OutError = TEXT("Particle surface color material compilation failed: ") + FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
					return false;
				}
			}
			Cached.Add(System, Material);
		}
	}
	return true;
}

void UMeshParticleizerPreview::ClearParticleDisplay()
{
	if (auto* Particles = this->DisplayParticles.Get())
	{
		Particles->SetVariableMaterial(TEXT("User.ParticleMaterial"), this->SavedParticleMaterial);
		const auto FarBinding = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.FarParticleMaterial"));
		if (Particles->GetAsset() != nullptr && Particles->GetAsset()->GetExposedParameters().IndexOf(FarBinding) != INDEX_NONE)
		{
			Particles->SetVariableMaterial(FarBinding.GetName(), this->SavedFarParticleMaterial);
		}
		Particles->MarkRenderStateDirty();
	}
	this->DisplayParticles.Reset();
	this->SavedParticleMaterial = nullptr;
	this->SavedFarParticleMaterial = nullptr;
	this->bParticleRenderStatePending = false;
}

void UMeshParticleizerPreview::UpdateParticleDisplay()
{
	auto* Particles = this->Operation != nullptr ? this->Operation->GetParticlesForEditor() : nullptr;
	if ((this->DisplayMode != EMeshParticleizerPreviewDisplay::SurfaceColor && this->DisplayMode != EMeshParticleizerPreviewDisplay::Velocity) || Particles == nullptr || this->bClosed == true)
	{
		this->ClearParticleDisplay();
		return;
	}
	auto& Cached = this->DisplayMode == EMeshParticleizerPreviewDisplay::Velocity ? this->ParticleVelocityMaterials : this->ParticleColorMaterials;
	auto& FarCached = this->DisplayMode == EMeshParticleizerPreviewDisplay::Velocity ? this->FarParticleVelocityMaterials : this->FarParticleColorMaterials;
	auto* FarMaterial = FarCached.Find(Particles->GetAsset());
	auto* Material = Cached.Find(Particles->GetAsset());
	if (Material == nullptr)
	{
		return;
	}
	const auto Binding = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.ParticleMaterial"));
	const auto FarBinding = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.FarParticleMaterial"));
	if (this->DisplayParticles != Particles)
	{
		this->ClearParticleDisplay();
		this->SavedParticleMaterial = Cast<UMaterialInterface>(Particles->GetOverrideParameters().GetUObject(Binding));
		this->SavedFarParticleMaterial = FarMaterial != nullptr ? Cast<UMaterialInterface>(Particles->GetOverrideParameters().GetUObject(FarBinding)) : nullptr;
		this->DisplayParticles = Particles;
		this->AppliedParticleDisplayMode = EMeshParticleizerPreviewDisplay::Effect;
	}
	if (this->AppliedParticleDisplayMode != this->DisplayMode)
	{
		this->AppliedParticleDisplayMode = this->DisplayMode;
		Particles->SetVariableMaterial(Binding.GetName(), Material->Get());
		if (FarMaterial != nullptr)
		{
			Particles->SetVariableMaterial(FarBinding.GetName(), FarMaterial->Get());
		}
		this->bParticleRenderStatePending = true;
	}
	// Niagaraの遅延Setter適用後に描画だけを作り直す。停止中もSimulationを進めない。
	if (this->bParticleRenderStatePending == true && Particles->GetOverrideParameters().GetUObject(Binding) == Material->Get() &&
		(FarMaterial == nullptr || Particles->GetOverrideParameters().GetUObject(FarBinding) == FarMaterial->Get()))
	{
		Particles->MarkRenderStateDirty();
		this->bParticleRenderStatePending = false;
	}
}

bool UMeshParticleizerPreview::PrepareDisplayMaterials(EMeshParticleizerPreviewDisplay Mode, FString& OutError)
{
	if (this->Prepared == nullptr)
	{
		OutError = TEXT("Select prepared mesh data before changing the diagnostic view.");
		return false;
	}
	const auto bSurfaceColor = Mode == EMeshParticleizerPreviewDisplay::SurfaceColor || Mode == EMeshParticleizerPreviewDisplay::Velocity;
	if (bSurfaceColor == true && this->PrepareParticleDisplayMaterials(Mode, OutError) == false)
	{
		return false;
	}
	auto& Cached = bSurfaceColor ? this->SurfaceColorMaterials : this->OrderMaterials;
	if (Cached.Num() == this->Prepared->DissolveMaterials.Num())
	{
		return true;
	}

	// 準備材の式と静的パラメータを複製する。通常再生の材質・依存関係・保存資産は変えない。
	auto Materials = TArray<TStrongObjectPtr<UMaterialInstanceDynamic>>{};
	for (const auto& Reference : this->Prepared->DissolveMaterials)
	{
		auto* Source = Cast<UMaterialInstanceConstant>(Reference.LoadSynchronous());
		if (Source == nullptr)
		{
			OutError = TEXT("Diagnostic views require a generated prepared material instance.");
			return false;
		}
		// RecompileMaterial内のTexture Streaming更新はGCを行う。公開前の一時オブジェクトも保持する。
		const auto SourceGuard = TStrongObjectPtr<UMaterialInstanceConstant>(Source);
		auto Parent = TStrongObjectPtr<UMaterial>(DuplicateObject<UMaterial>(Source->GetMaterial(), this));
		auto* Material = Parent.Get();
		Material->ClearFlags(RF_Public | RF_Standalone);
		Material->SetFlags(RF_Transient);
		auto* Mask = static_cast<UMaterialExpressionCustom*>(nullptr);
		for (const auto& Expression : Material->GetExpressions())
		{
			auto* Candidate = Cast<UMaterialExpressionCustom>(Expression);
			if (Candidate != nullptr && Candidate->Description == TEXT("Mesh Particleizer stable reference-space mask"))
			{
				Mask = Candidate;
				break;
			}
		}
		if (Mask == nullptr)
		{
			OutError = TEXT("The prepared material has no stable reference-space mask.");
			return false;
		}

		// 同じ入力と共通関数から順序を描く。Opacityは元の穴と現在の消失範囲を保つ。
		auto MaskInput = *Material->GetExpressionInputForProperty(MP_OpacityMask);
		auto OffsetInput = *Material->GetExpressionInputForProperty(MP_WorldPositionOffset);
		if (Material->bUseMaterialAttributes == true)
		{
			auto* Get = Cast<UMaterialExpressionGetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionGetMaterialAttributes::StaticClass()));
			const auto* Attributes = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
			Get->MaterialAttributes.Connect(Attributes->OutputIndex, Attributes->Expression);
			MaskInput.Connect(Get->CreateOrGetOutputAttribute(MP_OpacityMask), Get);
			OffsetInput.Connect(Get->CreateOrGetOutputAttribute(MP_WorldPositionOffset), Get);
		}
		Material->bUseMaterialAttributes = false;
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_Unlit);
		Material->bDisableDepthTest = false;
		Material->bEnableResponsiveAA = true;
		Material->GetExpressionInputForProperty(MP_MaterialAttributes)->Expression = nullptr;
		*Material->GetExpressionInputForProperty(MP_WorldPositionOffset) = OffsetInput;

		if (bSurfaceColor == true)
		{
			// 登録済み表面色とSurfaceTintだけを使う。元材の発光や演出Tintは含めない。
			auto* Surface = static_cast<UMaterialExpressionMultiply*>(nullptr);
			for (const auto& Expression : Material->GetExpressions())
			{
				auto* Multiply = Cast<UMaterialExpressionMultiply>(Expression);
				auto* Tint = Multiply != nullptr ? Cast<UMaterialExpressionVectorParameter>(Multiply->B.Expression) : nullptr;
				if (Tint != nullptr && Tint->ParameterName == TEXT("MP_SurfaceTint") && Tint->Desc == TEXT("Mesh Particleizer registered surface tint"))
				{
					Surface = Multiply;
					break;
				}
			}
			if (Surface == nullptr)
			{
				OutError = TEXT("Prepare this mesh again to register its inherited surface color.");
				return false;
			}
			auto* Color = Cast<UMaterialExpressionSaturate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSaturate::StaticClass()));
			Color->Input.Connect(0, Surface);
			Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Color);
		}
		else
		{
			auto* Color = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass()));
			Color->Description = TEXT("Mesh Particleizer preview order");
			Color->Inputs = Mask->Inputs;
			Color->IncludeFilePaths = Mask->IncludeFilePaths;
			Color->OutputType = CMOT_Float3;
			Color->Code = TEXT("float3 P = MP_ProgressPosition(Pos, ProgressX, ProgressY, ProgressZ);\n")
				TEXT("float Q = MP_Order(P, BoundsMin, BoundsMax, Direction, Origin, (int)Mode, Seed, Origin1, Origin2, Origin3, (int)OriginCount, OriginDistance);\n")
				TEXT("return Q < 0.5 ? lerp(float3(0.015,0.12,0.8),float3(0.02,0.8,0.5),Q*2) : lerp(float3(0.02,0.8,0.5),float3(1,0.12,0.015),(Q-0.5)*2);");
			Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Color);
		}

		auto* Opacity = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass()));
		Opacity->OutputType = CMOT_Float1;
		Opacity->Inputs.Reset();
		auto& Input = Opacity->Inputs.AddDefaulted_GetRef();
		Input.InputName = TEXT("Mask");
		Input.Input = MaskInput;
		Opacity->Code = FString::Printf(TEXT("return Mask >= %.9f ? 1.0 : 0.0;"), Source->GetOpacityMaskClipValue());
		Material->GetExpressionInputForProperty(MP_Opacity)->Connect(0, Opacity);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		auto InstanceGuard = TStrongObjectPtr<UMaterialInstanceConstant>(DuplicateObject<UMaterialInstanceConstant>(Source, this));
		auto* Instance = InstanceGuard.Get();
		Instance->ClearFlags(RF_Public | RF_Standalone);
		Instance->SetFlags(RF_Transient);
		Instance->SetParentEditorOnly(Material);
		Instance->BasePropertyOverrides.bOverride_BlendMode = false;
		Instance->BasePropertyOverrides.bOverride_ShadingModel = false;
		Instance->PostEditChange();
		auto* Resource = Instance->GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource == nullptr)
		{
			Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
		}
		if (Resource != nullptr)
		{
			Resource->FinishCompilation();
			if (Resource->GetCompileErrors().IsEmpty() == false)
			{
				OutError = TEXT("Preview material compilation failed: ") + FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
				return false;
			}
		}
		Materials.Emplace(UMaterialInstanceDynamic::Create(Instance, this));
	}
	Cached.Reset();
	for (const auto& Material : Materials)
	{
		Cached.Add(Material.Get());
	}
	OutError.Reset();
	return true;
}

void UMeshParticleizerPreview::ClearDisplayOverlay()
{
	if (auto* Display = this->DisplayMesh.Get())
	{
		for (auto Slot = 0; Slot < this->SavedSlotOverlays.Num(); ++Slot)
		{
			Display->SetOverlayMaterial(this->SavedSlotOverlays[Slot], true, Slot);
		}
	}

	// 描画Meshが先に失われた場合も、動的ProviderのTexture参照を解放する。
	if (this->SavedSlotOverlays.IsEmpty() == false)
	{
		for (const auto& Material : this->OrderMaterials)
		{
			Material->ClearParameterValues();
		}
		for (const auto& Material : this->SurfaceColorMaterials)
		{
			Material->ClearParameterValues();
		}
	}
	this->DisplayMesh.Reset();
	this->SavedSlotOverlays.Reset();
}

bool UMeshParticleizerPreview::UpdateDisplay(FString& OutError)
{
	this->UpdateParticleDisplay();
	if (this->UpdateSamples(OutError) == false)
	{
		return false;
	}
	if ((this->DisplayMode != EMeshParticleizerPreviewDisplay::Order && this->DisplayMode != EMeshParticleizerPreviewDisplay::SurfaceColor &&
		this->DisplayMode != EMeshParticleizerPreviewDisplay::Velocity) ||
		this->Mesh == nullptr || this->bClosed == true)
	{
		this->ClearDisplayOverlay();
		return true;
	}
	if (this->PrepareDisplayMaterials(this->DisplayMode, OutError) == false)
	{
		return false;
	}
	auto& Materials = this->DisplayMode == EMeshParticleizerPreviewDisplay::Order ? this->OrderMaterials : this->SurfaceColorMaterials;

	auto* Display = this->Operation != nullptr ? this->Operation->GetDisplayMeshForEditor() : this->Mesh.Get();
	if (Display == nullptr)
	{
		this->ClearDisplayOverlay();
		return true;
	}
	if (Display->GetNumMaterials() != Materials.Num())
	{
		this->ClearDisplayOverlay();
		OutError = TEXT("The display mesh no longer matches the prepared material slots.");
		return false;
	}
	if (this->DisplayMesh != Display || this->AppliedDisplayMode != this->DisplayMode)
	{
		this->ClearDisplayOverlay();
		this->DisplayMesh = Display;
		this->AppliedDisplayMode = this->DisplayMode;
		for (auto Slot = 0; Slot < Display->GetNumMaterials(); ++Slot)
		{
			this->SavedSlotOverlays.Add(Display->GetOverlayMaterial(true, Slot));
			Display->SetOverlayMaterial(Materials[Slot], true, Slot);
		}
	}

	// 再生中は実描画MIDから入力を写す。切替で再生やGPU予算を初期化しない。
	if (this->Operation != nullptr && this->Operation->GetState() != EMeshParticleizeState::Cancelled &&
		this->Operation->GetState() != EMeshParticleizeState::Finished)
	{
		for (auto Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			Materials[Slot]->CopyMaterialUniformParameters(Display->GetMaterial(Slot));
		}
		return true;
	}

	// 開始前も通常再生の起点解決を使う。プレビューは一部品なので進行座標は単位行列。
	auto Target = FMeshParticleizerTarget{};
	Target.Mesh = this->Mesh;
	Target.PreparedData = this->Prepared;
	const auto Targets = TArray<FMeshParticleizerTarget>{Target};
	auto Spaces = TArray<FMeshParticleizerProgressSpace>{};
	auto Origins = TArray<FMeshParticleizerResolvedOrigin>{};
	if (FMeshParticleizerProgressSpace::Resolve(Targets, false, Spaces, OutError) == false ||
		FMeshParticleizerResolvedOrigin::Resolve(Targets, this->Options, Spaces, Origins, OutError) == false)
	{
		return false;
	}
	const auto& Origin = Origins[0];
	for (auto Slot = 0; Slot < Materials.Num(); ++Slot)
	{
		auto* Material = Materials[Slot].Get();
		Material->CopyMaterialUniformParameters(Display->GetMaterial(Slot));
		Material->SetVectorParameterValue(TEXT("MP_BoundsMin"), FLinearColor(Spaces[0].Bounds.Min));
		Material->SetVectorParameterValue(TEXT("MP_BoundsMax"), FLinearColor(Spaces[0].Bounds.Max));
		Material->SetVectorParameterValue(TEXT("MP_ProgressX"), FLinearColor(1, 0, 0, 0));
		Material->SetVectorParameterValue(TEXT("MP_ProgressY"), FLinearColor(0, 1, 0, 0));
		Material->SetVectorParameterValue(TEXT("MP_ProgressZ"), FLinearColor(0, 0, 1, 0));
		Material->SetVectorParameterValue(TEXT("MP_Direction"), FLinearColor(Origin.ReferenceDirection));
		Material->SetVectorParameterValue(TEXT("MP_Origin"), FLinearColor(Origin.ReferencePosition));
		Material->SetScalarParameterValue(TEXT("MP_OriginCount"), FMath::Max(Origin.ReferencePoints.Num(), 1));
		Material->SetScalarParameterValue(TEXT("MP_OriginDistance"), Origin.ReferenceDistance);
		for (auto Index = 1; Index < 4; ++Index)
		{
			const auto Point = Origin.ReferencePoints.IsValidIndex(Index) ? Origin.ReferencePoints[Index] : FVector::ZeroVector;
			Material->SetVectorParameterValue(FName(FString::Printf(TEXT("MP_Origin%d"), Index)), FLinearColor(Point));
		}
		Material->SetScalarParameterValue(TEXT("MP_Mode"), static_cast<int32>(Origin.Mode));
		Material->SetScalarParameterValue(TEXT("MP_Seed"), this->Options.Seed);
		Material->SetScalarParameterValue(TEXT("MP_Progress"), -1);
	}
	return true;
}
