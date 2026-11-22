#include "MeshParticleizerCoverageGpu.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "DynamicRHI.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT(MeshParticleizerCoverageClear);
DECLARE_GPU_STAT(MeshParticleizerCoverageGeometry);

uint32 MeshParticleizerCoverageBucketCount(uint32 Count)
{
	return Count > 0 && Count <= 250000 ? FMath::RoundUpToPowerOfTwo(Count * 2) : 0;
}

int64 MeshParticleizerCoverageIndexBytes(uint32 Count)
{
	const auto Buckets = MeshParticleizerCoverageBucketCount(Count);
	return Buckets > 0 ? int64{Buckets} * sizeof(uint32) + int64{Count} * sizeof(FUintVector4) : 0;
}

int64 MeshParticleizerCoverageGpuBytes(uint32 Count, uint32 BoneCount, uint32 MaterialSlots)
{
	if (Count == 0 || Count > 250000 || MaterialSlots == 0 || GDynamicRHI == nullptr)
	{
		return 0;
	}
	const auto Desc = FRHITextureCreateDesc::Create2D(TEXT("MeshParticleizer.CoverageReservation"),
														  MeshParticleizerCoverageWidth, FMath::DivideAndRoundUp(Count, MeshParticleizerCoverageWidth), PF_R32_FLOAT)
														  .SetFlags(TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV)
														  .SetInitialState(ERHIAccess::SRVMask);
	return int64{Count} * sizeof(FMeshParticleizerCoverageSample) + int64{BoneCount} * 3 * sizeof(FVector4f) +
		int64{MaterialSlots} * sizeof(uint32) + static_cast<int64>(RHICalcTexturePlatformSize(Desc).Size) + MeshParticleizerCoverageIndexBytes(Count);
}

class FMeshParticleizerCoverageClearCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerCoverageClearCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerCoverageClearCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, MaterialSlots)
		SHADER_PARAMETER(uint32, CoverageWidth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DynamicSlots)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, Coverage)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerCoverageClearCS,
	"/Plugin/MeshParticleizer/Private/SurfaceCoverageClear.usf", "MainCS", SF_Compute);

bool AddMeshParticleizerCoverageClearPass(FRDGBuilder& GraphBuilder, uint32 Count, uint32 MaterialSlots,
	FRDGTextureRef Positions, FRDGBufferRef DynamicSlots, FRDGTextureRef Coverage)
{
	if (Count == 0 || MaterialSlots == 0 || Positions == nullptr || DynamicSlots == nullptr || Coverage == nullptr ||
		uint64(Positions->Desc.Extent.X) * Positions->Desc.Extent.Y < Count || Positions->Desc.Format != PF_A32B32G32R32F ||
		DynamicSlots->Desc.GetSize() < uint64(MaterialSlots) * sizeof(uint32) || Coverage->Desc.Format != PF_R32_FLOAT ||
		uint64(Coverage->Desc.Extent.X) * Coverage->Desc.Extent.Y < Count)
	{
		return false;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, MeshParticleizerCoverageClear, "MeshParticleizer Coverage Clear");
	auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerCoverageClearCS::FParameters>();
	Parameters->Count = Count;
	Parameters->MaterialSlots = MaterialSlots;
	Parameters->CoverageWidth = Coverage->Desc.Extent.X;
	Parameters->Positions = Positions;
	Parameters->DynamicSlots = GraphBuilder.CreateSRV(DynamicSlots, PF_R32_UINT);
	Parameters->Coverage = GraphBuilder.CreateUAV(Coverage);
	const auto Shader = TShaderMapRef<FMeshParticleizerCoverageClearCS>(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MeshParticleizer Coverage Clear"), Shader, Parameters,
		FIntVector(FMath::DivideAndRoundUp(Count, 64u), 1, 1));
	return true;
}

class FMeshParticleizerCoverageGeometryCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerCoverageGeometryCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerCoverageGeometryCS, FGlobalShader);

public:
	class FSkinned : SHADER_PERMUTATION_BOOL("COVERAGE_SKINNED");
	class FFilterSlots : SHADER_PERMUTATION_BOOL("COVERAGE_FILTER_SLOTS");
	using FPermutationDomain = TShaderPermutationDomain<FSkinned, FFilterSlots>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, BoneCount)
		SHADER_PARAMETER(uint32, MaterialSlots)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DynamicSlots)
		SHADER_PARAMETER(FMatrix44f, LocalToTranslatedWorld)
		SHADER_PARAMETER(FMatrix44f, LocalToWorldNoScale)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Positions)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Normals)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Coordinates)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TriangleData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkinVertices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SkinTriangles)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, Bones)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, Samples)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerCoverageGeometryCS,
	"/Plugin/MeshParticleizer/Private/SurfaceCoverageGeometry.usf", "MainCS", SF_Compute);

bool AddMeshParticleizerCoverageGeometryPass(FRDGBuilder& GraphBuilder, const FSceneView& View,
	const FMeshParticleizerCoverageGeometry& Geometry, FRDGBufferRef Samples)
{
	auto HasRows = [](FRDGTextureRef Texture, uint32 Rows) {
		return Texture != nullptr && Texture->Desc.Format == PF_A32B32G32R32F &&
			uint64(Texture->Desc.Extent.X) * Texture->Desc.Extent.Y >= Rows;
	};
	const auto bSkinned = Geometry.BoneCount > 0;
	const auto bFilterSlots = Geometry.DynamicSlots != nullptr;
	if (bFilterSlots && (Geometry.MaterialSlots == 0 || Geometry.DynamicSlots->Desc.GetSize() < uint64(Geometry.MaterialSlots) * sizeof(uint32)))
	{
		return false;
	}
	if (Geometry.Count == 0 || Geometry.Count > 250000 || Geometry.LocalToWorld.ContainsNaN() ||
		Geometry.LocalToWorld.GetScale3D().GetAbsMin() <= SMALL_NUMBER ||
		HasRows(Geometry.Positions, Geometry.Count) == false || HasRows(Geometry.Normals, Geometry.Count) == false ||
		HasRows(Geometry.Coordinates, Geometry.Count) == false || HasRows(Geometry.TriangleData, bSkinned ? 2 : 8) == false ||
		Samples == nullptr || Samples->Desc.GetSize() < uint64(Geometry.Count) * sizeof(FMeshParticleizerCoverageSample) ||
		View.ViewUniformBuffer.IsValid() == false)
	{
		return false;
	}
	if (bSkinned && (HasRows(Geometry.SkinVertices, 8) == false || HasRows(Geometry.SkinTriangles, 1) == false || Geometry.Bones == nullptr || Geometry.Bones->Desc.GetSize() < uint64(Geometry.BoneCount) * 3 * sizeof(FVector4f)))
	{
		return false;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, MeshParticleizerCoverageGeometry, "MeshParticleizer Coverage Geometry");

	// 大きなワールド座標の差分はdoubleで取り、Translated Worldへ移してからfloat化する。
	auto Transform = Geometry.LocalToWorld.ToMatrixWithScale();
	Transform.SetOrigin(Transform.GetOrigin() + View.ViewMatrices.GetPreViewTranslation());
	auto NormalTransform = Geometry.LocalToWorld.ToMatrixWithScale();
	NormalTransform.RemoveScaling();
	NormalTransform.SetOrigin(FVector::ZeroVector);
	auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerCoverageGeometryCS::FParameters>();
	Parameters->Count = Geometry.Count;
	Parameters->BoneCount = Geometry.BoneCount;
	Parameters->MaterialSlots = Geometry.MaterialSlots;
	Parameters->DynamicSlots = bFilterSlots ? GraphBuilder.CreateSRV(Geometry.DynamicSlots, PF_R32_UINT) : nullptr;
	Parameters->LocalToTranslatedWorld = FMatrix44f(Transform);
	Parameters->LocalToWorldNoScale = FMatrix44f(NormalTransform);
	Parameters->View = View.ViewUniformBuffer;
	Parameters->Positions = Geometry.Positions;
	Parameters->Normals = Geometry.Normals;
	Parameters->Coordinates = Geometry.Coordinates;
	Parameters->TriangleData = Geometry.TriangleData;
	Parameters->SkinVertices = bSkinned ? Geometry.SkinVertices : Geometry.Positions;
	Parameters->SkinTriangles = bSkinned ? Geometry.SkinTriangles : Geometry.Positions;

	// Staticのコンパイル経路には骨の入力を含めない。
	Parameters->Bones = bSkinned ? GraphBuilder.CreateSRV(Geometry.Bones, PF_A32B32G32R32F) : nullptr;
	Parameters->Samples = GraphBuilder.CreateUAV(Samples, PF_A32B32G32R32F);
	auto Permutation = FMeshParticleizerCoverageGeometryCS::FPermutationDomain{};
	Permutation.Set<FMeshParticleizerCoverageGeometryCS::FSkinned>(bSkinned);
	Permutation.Set<FMeshParticleizerCoverageGeometryCS::FFilterSlots>(bFilterSlots);
	const auto Shader = TShaderMapRef<FMeshParticleizerCoverageGeometryCS>(GetGlobalShaderMap(View.GetFeatureLevel()), Permutation);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MeshParticleizer Coverage Geometry"), Shader, Parameters,
		FIntVector(FMath::DivideAndRoundUp(Geometry.Count, 64u), 1, 1));
	return true;
}
