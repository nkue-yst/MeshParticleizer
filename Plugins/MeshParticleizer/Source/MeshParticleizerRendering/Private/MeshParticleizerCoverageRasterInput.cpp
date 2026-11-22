#include "MeshParticleizerCoverageGpu.h"

#include "CommonRenderResources.h"
#include "Math/DoubleFloat.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT(MeshParticleizerCoverageIndex);
DECLARE_GPU_STAT(MeshParticleizerCoverageRaster);

class FMeshParticleizerCoverageIndexCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerCoverageIndexCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerCoverageIndexCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, BucketMask)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CandidateNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, InputSamples)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, BuildHeads)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint4>, BuildLinks)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FMeshParticleizerCoverageRasterInputVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerCoverageRasterInputVS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerCoverageRasterInputVS, FGlobalShader);

public:
	class FPrepared : SHADER_PERMUTATION_INT("COVERAGE_PREPARED", 3);
	using FPermutationDomain = TShaderPermutationDomain<FPrepared>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, Vertices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TriangleData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkinVertices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkinTriangles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, Bones)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DynamicSlots)
		SHADER_PARAMETER(uint32, BoneCount)
		SHADER_PARAMETER(uint32, MaterialSlots)
		SHADER_PARAMETER(FMatrix44f, LocalToRelativeWorld)
		SHADER_PARAMETER(FVector3f, WorldOrigin)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
#if !WITH_EDITOR
		if (FPermutationDomain(Parameters.PermutationId).Get<FPrepared>() == 0)
		{
			return false;
		}
#endif
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& Environment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, Environment);
		Environment.SetDefine(TEXT("COVERAGE_SKINNED"), FPermutationDomain(Parameters.PermutationId).Get<FPrepared>() == 2);
	}
};

class FMeshParticleizerCoverageRasterInputPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerCoverageRasterInputPS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerCoverageRasterInputPS, FGlobalShader);

public:
	class FRecordMatches : SHADER_PERMUTATION_BOOL("COVERAGE_RECORD_MATCHES");
	using FPermutationDomain = TShaderPermutationDomain<FRecordMatches>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, BucketMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, Heads)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, Links)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, Samples)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Matches)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
#if !WITH_EDITOR
		if (FPermutationDomain(Parameters.PermutationId).Get<FRecordMatches>())
		{
			return false;
		}
#endif
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerCoverageIndexCS, "/Plugin/MeshParticleizer/Private/SurfaceCoverageRasterInput.usf", "BuildCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerCoverageRasterInputVS, "/Plugin/MeshParticleizer/Private/SurfaceCoverageRasterInput.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerCoverageRasterInputPS, "/Plugin/MeshParticleizer/Private/SurfaceCoverageRasterInput.usf", "MainPS", SF_Pixel);

BEGIN_SHADER_PARAMETER_STRUCT(FMeshParticleizerCoverageRasterInputParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FMeshParticleizerCoverageRasterInputVS::FParameters, Vertex)
	SHADER_PARAMETER_STRUCT_INCLUDE(FMeshParticleizerCoverageRasterInputPS::FParameters, Pixel)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

static bool AddRasterInputs(FRDGBuilder& GraphBuilder, const FSceneView& View, const FIntRect& Rect,
	FRDGBufferRef Vertices, uint32 VertexCount, FRDGTextureRef CandidateNormals, FRDGBufferRef Samples,
	uint32 Count, FRDGBufferRef Heads, FRDGBufferRef Links, FRDGBufferRef Matches,
	const FMeshParticleizerCoverageGeometry* Geometry)
{
	const auto Buckets = MeshParticleizerCoverageBucketCount(Count);
	if (Buckets == 0 || VertexCount == 0 || VertexCount % 3 != 0 ||
		Samples == nullptr || Samples->Desc.GetSize() < uint64(Count) * sizeof(FMeshParticleizerCoverageSample) ||
		CandidateNormals == nullptr || CandidateNormals->Desc.Format != PF_A32B32G32R32F ||
		uint64(CandidateNormals->Desc.Extent.X) * CandidateNormals->Desc.Extent.Y < Count ||
		Heads == nullptr || Heads->Desc.GetSize() < uint64(Buckets) * sizeof(uint32) ||
		Links == nullptr || Links->Desc.GetSize() < uint64(Count) * sizeof(FUintVector4) ||
		Rect.IsEmpty() || Rect.Min.X < 0 || Rect.Min.Y < 0 || View.ViewUniformBuffer.IsValid() == false)
	{
		return false;
	}

	const auto ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	// 各Viewの索引構築とラスタ転送を分けてGPU費用を記録する。
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, MeshParticleizerCoverageIndex, "MeshParticleizer Coverage Index");

		// 各Viewで索引を再構築する。未登録の候補は従来の解析値を保持する。
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Heads, PF_R32_UINT), MAX_uint32);
		if (Matches != nullptr)
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Matches, PF_R32_UINT), 0u);
		}
		auto* IndexParameters = GraphBuilder.AllocParameters<FMeshParticleizerCoverageIndexCS::FParameters>();
		IndexParameters->Count = Count;
		IndexParameters->BucketMask = Buckets - 1;
		IndexParameters->View = View.ViewUniformBuffer;
		IndexParameters->CandidateNormals = CandidateNormals;
		IndexParameters->InputSamples = GraphBuilder.CreateSRV(Samples, PF_A32B32G32R32F);
		IndexParameters->BuildHeads = GraphBuilder.CreateUAV(Heads, PF_R32_UINT);
		IndexParameters->BuildLinks = GraphBuilder.CreateUAV(Links, PF_R32G32B32A32_UINT);
		const auto IndexShader = TShaderMapRef<FMeshParticleizerCoverageIndexCS>(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MeshParticleizer Coverage Raster Index"), IndexShader,
			IndexParameters, FIntVector(FMath::DivideAndRoundUp(Count, 64u), 1, 1));
	}

	// 準備済み頂点を直接描く。CPU頂点配列と検査用バッファはEditor診断だけに残す。
	RDG_EVENT_SCOPE_STAT(GraphBuilder, MeshParticleizerCoverageRaster, "MeshParticleizer Coverage Raster");
	auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerCoverageRasterInputParameters>();
	Parameters->Vertex.View = View.ViewUniformBuffer;
	Parameters->Vertex.Vertices = Vertices != nullptr ? GraphBuilder.CreateSRV(Vertices, PF_A32B32G32R32F) : nullptr;
	auto VertexPermutation = FMeshParticleizerCoverageRasterInputVS::FPermutationDomain{};
	if (Geometry != nullptr)
	{
		const auto bSkinned = Geometry->BoneCount > 0;
		const auto Origin = FDFVector3(Geometry->LocalToWorld.GetTranslation()).High;
		const auto Transform = FDFMatrix::MakeToRelativeWorldMatrix(Origin, Geometry->LocalToWorld.ToMatrixWithScale());
		Parameters->Vertex.LocalToRelativeWorld = Transform.M;
		Parameters->Vertex.WorldOrigin = Transform.PostTranslation;
		Parameters->Vertex.TriangleData = Geometry->TriangleData;
		Parameters->Vertex.SkinVertices = bSkinned ? Geometry->SkinVertices : nullptr;
		Parameters->Vertex.SkinTriangles = bSkinned ? Geometry->SkinTriangles : nullptr;
		Parameters->Vertex.Bones = bSkinned ? GraphBuilder.CreateSRV(Geometry->Bones, PF_A32B32G32R32F) : nullptr;
		Parameters->Vertex.DynamicSlots = GraphBuilder.CreateSRV(Geometry->DynamicSlots, PF_R32_UINT);
		Parameters->Vertex.BoneCount = Geometry->BoneCount;
		Parameters->Vertex.MaterialSlots = Geometry->MaterialSlots;
		VertexPermutation.Set<FMeshParticleizerCoverageRasterInputVS::FPrepared>(bSkinned ? 2 : 1);
	}
	Parameters->Pixel.Count = Count;
	Parameters->Pixel.BucketMask = Buckets - 1;
	Parameters->Pixel.Heads = GraphBuilder.CreateSRV(Heads, PF_R32_UINT);
	Parameters->Pixel.Links = GraphBuilder.CreateSRV(Links, PF_R32G32B32A32_UINT);
	Parameters->Pixel.Samples = GraphBuilder.CreateUAV(Samples, PF_R32_FLOAT);
	Parameters->Pixel.Matches = Matches != nullptr ? GraphBuilder.CreateUAV(Matches, PF_R32_UINT) : nullptr;
	auto PixelPermutation = FMeshParticleizerCoverageRasterInputPS::FPermutationDomain{};
	PixelPermutation.Set<FMeshParticleizerCoverageRasterInputPS::FRecordMatches>(Matches != nullptr);
	const auto VertexShader = TShaderMapRef<FMeshParticleizerCoverageRasterInputVS>(ShaderMap, VertexPermutation);
	const auto PixelShader = TShaderMapRef<FMeshParticleizerCoverageRasterInputPS>(ShaderMap, PixelPermutation);
	GraphBuilder.AddPass(RDG_EVENT_NAME("MeshParticleizer Coverage Raster Inputs"), Parameters, ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Rect, VertexCount](FRHICommandList& RHICmdList) {
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
			RHICmdList.SetViewport(Rect.Min.X, Rect.Min.Y, 0, Rect.Max.X, Rect.Max.Y, 1);
			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), Parameters->Vertex);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), Parameters->Pixel);
			RHICmdList.DrawPrimitive(0, VertexCount / 3, 1);
		});
	return true;
}

bool AddMeshParticleizerCoverageRasterInputPass(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FIntRect& RasterRect, const FMeshParticleizerCoverageGeometry& Geometry,
	FRDGBufferRef Samples, FRDGBufferRef Heads, FRDGBufferRef Links)
{
	auto HasRows = [](FRDGTextureRef Texture, uint64 Rows) {
		return Texture != nullptr && Texture->Desc.Format == PF_A32B32G32R32F &&
			uint64(Texture->Desc.Extent.X) * Texture->Desc.Extent.Y >= Rows;
	};
	const auto bSkinned = Geometry.BoneCount > 0;
	if (Geometry.TriangleCount == 0 || Geometry.TriangleCount > MAX_uint32 / 3 ||
		Geometry.LocalToWorld.ContainsNaN() || Geometry.LocalToWorld.GetScale3D().GetAbsMin() <= SMALL_NUMBER ||
		HasRows(Geometry.TriangleData, uint64(Geometry.TriangleCount) * (bSkinned ? 2 : 8)) == false ||
		Geometry.MaterialSlots == 0 || Geometry.DynamicSlots == nullptr ||
		Geometry.DynamicSlots->Desc.GetSize() < uint64(Geometry.MaterialSlots) * sizeof(uint32))
	{
		return false;
	}
	if (bSkinned && (HasRows(Geometry.SkinVertices, 8) == false || HasRows(Geometry.SkinTriangles, Geometry.TriangleCount) == false || Geometry.Bones == nullptr || Geometry.Bones->Desc.GetSize() < uint64(Geometry.BoneCount) * 3 * sizeof(FVector4f)))
	{
		return false;
	}

	return AddRasterInputs(GraphBuilder, View, RasterRect, nullptr, Geometry.TriangleCount * 3,
		Geometry.Normals, Samples, Geometry.Count, Heads, Links, nullptr, &Geometry);
}

#if WITH_EDITOR
bool AddMeshParticleizerCoverageRasterInputDiagnosticPass(FRDGBuilder& GraphBuilder, const FSceneView& View,
	FRDGBufferRef Vertices, uint32 VertexCount, FRDGTextureRef CandidateNormals, FRDGBufferRef Samples,
	uint32 Count, FRDGBufferRef& OutMatches, uint64& OutScratchBytes, FRDGBufferRef* OutLinks)
{
	OutMatches = nullptr;
	OutScratchBytes = 0;
	if (OutLinks != nullptr)
	{
		*OutLinks = nullptr;
	}
	const auto Buckets = MeshParticleizerCoverageBucketCount(Count);
	if (Buckets == 0 || Vertices == nullptr ||
		Vertices->Desc.GetSize() < uint64(VertexCount) * sizeof(FMeshParticleizerCoverageSample))
	{
		return false;
	}

	auto* Heads = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Buckets), TEXT("CoverageRaster.Heads"));
	auto* Links = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FUintVector4), Count), TEXT("CoverageRaster.Links"));

	// 未登録キーは明示的な無効値にする。追加初期化と索引読戻しはEditor診断だけ。
	if (OutLinks != nullptr)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Links, PF_R32G32B32A32_UINT), MAX_uint32);
		*OutLinks = Links;
	}
	OutMatches = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Count), TEXT("CoverageRaster.Matches"));
	OutScratchBytes = Heads->Desc.GetSize() + Links->Desc.GetSize() + OutMatches->Desc.GetSize();
	return AddRasterInputs(GraphBuilder, View, View.UnscaledViewRect, Vertices, VertexCount, CandidateNormals,
		Samples, Count, Heads, Links, OutMatches, nullptr);
}
#endif
