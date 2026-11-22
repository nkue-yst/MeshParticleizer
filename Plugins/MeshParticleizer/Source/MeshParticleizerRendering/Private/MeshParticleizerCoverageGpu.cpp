#include "MeshParticleizerCoverageGpu.h"
#include "GlobalShader.h"
#include "CommonRenderResources.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShader.h"
#include "MaterialShared.h"
#include "PipelineStateCache.h"
#include "PrimitiveUniformShaderParameters.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT(MeshParticleizerCoverageMaterial);

class FMeshParticleizerCoverageVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerCoverageVS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerCoverageVS, FGlobalShader);

public:
	class FRasterProbe : SHADER_PERMUTATION_BOOL("COVERAGE_RASTER_PROBE");
	using FPermutationDomain = TShaderPermutationDomain<FRasterProbe>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, CoverageSamples)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
#if !WITH_EDITOR
		if (FPermutationDomain(Parameters.PermutationId).Get<FRasterProbe>() == true)
		{
			return false;
		}
#endif
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
		Environment.SetDefine(TEXT("COVERAGE_VERTEX_SHADER"), 1);
	}
};

class FMeshParticleizerCoveragePS : public FMaterialShader
{
	DECLARE_SHADER_TYPE(FMeshParticleizerCoveragePS, Material);

public:
	class FProbeMode : SHADER_PERMUTATION_INT("COVERAGE_PROBE_MODE", 4);
	using FPermutationDomain = TShaderPermutationDomain<FProbeMode>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, CoverageCount)
		SHADER_PARAMETER(uint32, CoverageSlot)
		SHADER_PARAMETER(uint32, CoverageWidth)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, CoverageSamples)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RasterSamples)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CoverageOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, CoverageDetails)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_REF(FPrimitiveUniformShaderParameters, Primitive)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	FMeshParticleizerCoveragePS() = default;
	FMeshParticleizerCoveragePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMaterialShader(Initializer)
	{
		this->CoverageMaterial.Bind(Initializer.ParameterMap, TEXT("Material"));
		this->CoverageVertexMaterial.Bind(Initializer.ParameterMap, TEXT("MaterialVS"));
		this->Bindings.BindForLegacyShaderParameters(this, Initializer.PermutationId, Initializer.ParameterMap,
			*FParameters::FTypeInfo::GetStructMetadata(), false);
	}

	static bool ShouldCompilePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
#if !WITH_EDITOR
		if (FPermutationDomain(Parameters.PermutationId).Get<FProbeMode>() != 0)
		{
			return false;
		}
#endif
		// 専用の消失材に付ける既存Usageを利用し、すべての表面材への追加コンパイルを避ける。
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) &&
			Parameters.MaterialParameters.MaterialDomain == MD_Surface &&
			Parameters.MaterialParameters.BlendMode == BLEND_Masked &&
			Parameters.MaterialParameters.bIsUsedWithNiagaraMeshParticles;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, Environment);
		Environment.SetDefine(TEXT("COVERAGE_VERTEX_SHADER"), 0);
		const auto ProbeMode = FPermutationDomain(Parameters.PermutationId).Get<FProbeMode>();
		Environment.SetDefine(TEXT("COVERAGE_RASTER_PROBE"), ProbeMode == 1 || ProbeMode == 2);
		Environment.SetDefine(TEXT("SCENE_TEXTURES_DISABLED"), 1);

		// 通常経路は候補ごとの2x2画素で元のSample/SampleBiasを保つ。
		Environment.SetDefine(TEXT("USE_ANALYTIC_DERIVATIVES"), ProbeMode == 2);
		Environment.SetDefine(TEXT("MP_EVALUATE_COVERAGE"), 1);
	}

	bool MatchesUniformBuffer(FRHIPixelShader* Shader, const FRHIUniformBuffer* Uniform) const
	{
		if (this->CoverageMaterial.IsBound() == false)
		{
			return true;
		}
		const auto& Hashes = Shader->GetShaderResourceTable().ResourceTableLayoutHashes;
		const auto Index = this->CoverageMaterial.GetBaseIndex();
		return Uniform != nullptr && Hashes.IsValidIndex(Index) &&
			(Hashes[Index] == 0 || Hashes[Index] == Uniform->GetLayout().GetHash());
	}

	void SetParameters(FRHIBatchedShaderParameters& Parameters, FRHIUniformBuffer* Uniform, FRHIUniformBuffer* VertexUniform)
	{
		SetUniformBufferParameter(Parameters, this->CoverageMaterial, Uniform);
		if (VertexUniform != nullptr)
		{
			SetUniformBufferParameter(Parameters, this->CoverageVertexMaterial, VertexUniform);
		}
	}

private:
	LAYOUT_FIELD(FShaderUniformBufferParameter, CoverageMaterial);
	LAYOUT_FIELD(FShaderUniformBufferParameter, CoverageVertexMaterial);
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerCoverageVS, "/Plugin/MeshParticleizer/Private/SurfaceCoverage.usf", "MainVS", SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(, FMeshParticleizerCoveragePS, TEXT("/Plugin/MeshParticleizer/Private/SurfaceCoverage.usf"), TEXT("MainPS"), SF_Pixel);

bool PrepareMeshParticleizerCoverageMaterial(UMaterialInterface* Material, bool& bReady, FString& OutError)
{
	bReady = false;
	auto* Resource = Material != nullptr ? Material->GetMaterialResource(GMaxRHIShaderPlatform) : nullptr;
	if (Resource == nullptr)
	{
		OutError = TEXT("The prepared coverage material has no rendering resource.");
		return false;
	}
	auto Types = FMaterialShaderTypes{};
	Types.AddShaderType<FMeshParticleizerCoveragePS>();
	if (Resource->ShouldCacheShaders(Types, nullptr) == false)
	{
		OutError = TEXT("The prepared coverage material does not support its required shader. Prepare the source again.");
		return false;
	}
	// UEのOn-demandコンパイルを準備中に起動し、初回のGPU再生へ持ち込まない。
	bReady = Resource->HasShaders(Types, nullptr);
	return true;
}

static bool AddCoveragePass(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FMaterialRenderProxy* MaterialProxy, uint32 Slot, FRDGBufferRef Samples, uint32 Count,
	FRDGTextureRef Coverage, uint32 ProbeMode, FRDGBufferRef RasterSamples = nullptr)
{
	const auto bRasterizeTriangles = ProbeMode == 1 || ProbeMode == 2;
	if (MaterialProxy == nullptr || Samples == nullptr || Coverage == nullptr || Count == 0 ||
		View.ViewUniformBuffer.IsValid() == false ||
		(Coverage->Desc.Format != PF_R32_FLOAT && Coverage->Desc.Format != PF_A32B32G32R32F) ||
		(bRasterizeTriangles == false && uint64(Coverage->Desc.Extent.X) * Coverage->Desc.Extent.Y < Count) ||
		(bRasterizeTriangles == true && Count % 3 != 0) ||
		Samples->Desc.GetSize() < uint64(Count) * sizeof(FMeshParticleizerCoverageSample))
	{
		return false;
	}
	if (bRasterizeTriangles == false &&
		(EnumHasAnyFlags(Coverage->Desc.Flags, TexCreate_UAV) == false ||
			Coverage->Desc.Extent.GetMax() > int32(GMaxTextureDimensions / 2)))
	{
		return false;
	}
	if (ProbeMode == 2 && (RasterSamples == nullptr || RasterSamples->Desc.GetSize() < uint64(Coverage->Desc.Extent.X) * Coverage->Desc.Extent.Y * sizeof(FMeshParticleizerCoverageSample)))
	{
		return false;
	}

	// フォールバック材で成功したことにせず、要求した材のコンパイル済みPSを要求する。
	const auto* Material = MaterialProxy->GetMaterialNoFallback(View.GetFeatureLevel());
	if (Material == nullptr)
	{
		return false;
	}
	// 直前に作成・更新したMIDも同じシェーダーマップのUniformで評価する。
	if (MaterialProxy->UpdateUniformExpressionCacheIfNeeded(GraphBuilder.RHICmdList, View.GetFeatureLevel()) != Material)
	{
		return false;
	}
	auto Permutation = FMeshParticleizerCoveragePS::FPermutationDomain{};
	Permutation.Set<FMeshParticleizerCoveragePS::FProbeMode>(ProbeMode);
	auto Types = FMaterialShaderTypes{};
	Types.AddShaderType<FMeshParticleizerCoveragePS>(Permutation.ToDimensionValueId());
	auto Shaders = FMaterialShaders{};
	if (Material->TryGetShaders(Types, nullptr, Shaders) == false)
	{
		return false;
	}
	TShaderRef<FMeshParticleizerCoveragePS> PixelShader;
	Shaders.TryGetPixelShader(PixelShader);
	if (PixelShader.IsValid() == false)
	{
		return false;
	}
	// パス実行時にProxyを引き直さず、選んだPSと同時点のUniformを保持する。
	// MPCなど追加の外部Uniformを必要とするMaskは準備時にも拒否する。
	const auto& Cache = MaterialProxy->UniformExpressionCache[View.GetFeatureLevel()];
	const auto Uniform = Cache.UniformBuffer;
	const auto VertexUniform = Cache.UniformBufferVS;
	if (Cache.ParameterCollections.IsEmpty() == false || PixelShader->MatchesUniformBuffer(PixelShader.GetPixelShader(), Uniform) == false)
	{
		UE_LOG(LogTemp, Error, TEXT("Coverage material Uniform layout is unavailable or mismatched: %s"), *MaterialProxy->GetFriendlyName());
		return false;
	}
	auto VertexPermutation = FMeshParticleizerCoverageVS::FPermutationDomain{};
	VertexPermutation.Set<FMeshParticleizerCoverageVS::FRasterProbe>(bRasterizeTriangles);
	const auto VertexShader = TShaderMapRef<FMeshParticleizerCoverageVS>(GetGlobalShaderMap(View.GetFeatureLevel()), VertexPermutation);

	auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerCoveragePS::FParameters>();
	Parameters->CoverageCount = Count;
	Parameters->CoverageSlot = Slot;
	Parameters->CoverageWidth = Coverage->Desc.Extent.X;
	Parameters->CoverageSamples = GraphBuilder.CreateSRV(Samples, PF_A32B32G32R32F);
	Parameters->RasterSamples = RasterSamples != nullptr ? GraphBuilder.CreateUAV(RasterSamples, PF_A32B32G32R32F) : nullptr;
	Parameters->CoverageOutput = ProbeMode == 0 ? GraphBuilder.CreateUAV(Coverage) : nullptr;
	Parameters->CoverageDetails = ProbeMode == 3 ? GraphBuilder.CreateUAV(Coverage) : nullptr;
	Parameters->View = View.ViewUniformBuffer;
	Parameters->Primitive = GIdentityPrimitiveUniformBuffer.GetUniformBufferRef();
	if (bRasterizeTriangles == true)
	{
		Parameters->RenderTargets[0] = FRenderTargetBinding(Coverage, ERenderTargetLoadAction::ELoad);
	}
	// Raster診断ではSamplesをVSが読む。PSだけを見てRDG参照を取り除かない。
	if (bRasterizeTriangles == false)
	{
		ClearUnusedGraphResources(PixelShader, Parameters);
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, MeshParticleizerCoverageMaterial, "MeshParticleizer Coverage Material");
	GraphBuilder.AddPass(RDG_EVENT_NAME("MeshParticleizer Coverage Slot %u", Slot), Parameters, ERDGPassFlags::Raster,
		[Parameters, PixelShader, VertexShader, Uniform, VertexUniform, Size = Coverage->Desc.Extent,
			bRasterizeTriangles, Count](FRHICommandList& RHICmdList) {
			auto Pipeline = FGraphicsPipelineStateInitializer{};
			RHICmdList.ApplyCachedRenderTargets(Pipeline);
			Pipeline.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
			Pipeline.BlendState = TStaticBlendState<>::GetRHI();
			Pipeline.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
			Pipeline.PrimitiveType = PT_TriangleList;
			Pipeline.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
			Pipeline.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			Pipeline.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			SetGraphicsPipelineState(RHICmdList, Pipeline, 0);
			const auto QuadScale = bRasterizeTriangles ? 1 : 2;
			RHICmdList.SetViewport(0, 0, 0, Size.X * QuadScale, Size.Y * QuadScale, 1);
			if (bRasterizeTriangles == true)
			{
				auto VertexParameters = FMeshParticleizerCoverageVS::FParameters{};
				VertexParameters.CoverageSamples = Parameters->CoverageSamples;
				VertexParameters.View = Parameters->View;
				SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VertexParameters);
			}
			SetShaderParametersMixedPS(RHICmdList, PixelShader, *Parameters, Uniform.GetReference(), VertexUniform.GetReference());
			RHICmdList.DrawPrimitive(0, bRasterizeTriangles ? Count / 3 : 1, 1);
		});
	return true;
}

bool AddMeshParticleizerCoveragePass(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FMaterialRenderProxy* MaterialProxy, uint32 Slot, FRDGBufferRef Samples, uint32 Count, FRDGTextureRef Coverage)
{
	if (Coverage == nullptr || Coverage->Desc.Format != PF_R32_FLOAT)
	{
		return false;
	}
	return AddCoveragePass(GraphBuilder, View, MaterialProxy, Slot, Samples, Count, Coverage, 0);
}

#if WITH_EDITOR
bool AddMeshParticleizerCoverageDiagnosticPass(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FMaterialRenderProxy* MaterialProxy, FRDGBufferRef Samples, uint32 Count, FRDGTextureRef Output,
	EMeshParticleizerCoverageDiagnosticMode Mode, FRDGBufferRef RasterSamples)
{
	if (Output == nullptr || Output->Desc.Format != PF_A32B32G32R32F || uint32(Mode) < 1 || uint32(Mode) > 3)
	{
		return false;
	}
	return AddCoveragePass(GraphBuilder, View, MaterialProxy, 0, Samples, Count, Output, uint32(Mode), RasterSamples);
}
#endif
