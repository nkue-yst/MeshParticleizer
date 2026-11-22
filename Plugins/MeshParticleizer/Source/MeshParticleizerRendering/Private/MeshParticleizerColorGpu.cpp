#include "MeshParticleizerColorGpu.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RenderUtils.h"
#include "RHIStaticStates.h"

class FMeshParticleizerColorCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerColorCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerColorCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, Slot)
		SHADER_PARAMETER(uint32, Enabled)
		SHADER_PARAMETER(uint32, ClampColor)
		SHADER_PARAMETER(FVector2f, UVScale)
		SHADER_PARAMETER(FVector2f, UVOffset)
		SHADER_PARAMETER(FVector3f, ColorScale)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Positions)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Coordinates)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SourceTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SourceSampler)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, Colors)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerColorCS, "/Plugin/MeshParticleizer/Private/SurfaceColor.usf", "MainCS", SF_Compute);

void AddMeshParticleizerColorPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Colors, uint32 Count,
	uint32 Slot, FRHITexture* Positions, FRHITexture* Coordinates, FRHITexture* Texture,
	FRHISamplerState* Sampler, FVector2f UVScale, FVector2f UVOffset, FVector3f ColorScale, bool bClampColor)
{
	auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerColorCS::FParameters>();
	Parameters->Count = Count;
	Parameters->Slot = Slot;
	Parameters->Enabled = Texture != nullptr ? 1 : 0;
	Parameters->ClampColor = bClampColor ? 1 : 0;
	Parameters->UVScale = UVScale;
	Parameters->UVOffset = UVOffset;
	Parameters->ColorScale = ColorScale;
	Parameters->Positions = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Positions, TEXT("MeshParticleizer.ColorPositions")));
	Parameters->Coordinates = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Coordinates, TEXT("MeshParticleizer.ColorCoordinates")));
	Parameters->SourceTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(
		Texture != nullptr ? Texture : Positions, TEXT("MeshParticleizer.ColorSource")));
	Parameters->SourceSampler = Sampler != nullptr ? Sampler : TStaticSamplerState<SF_Point>::GetRHI();
	Parameters->Colors = GraphBuilder.CreateUAV(Colors, PF_A32B32G32R32F);

	const auto Shader = TShaderMapRef<FMeshParticleizerColorCS>(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MeshParticleizer Surface Color Slot %u", Slot),
		Shader, Parameters, FIntVector(FMath::DivideAndRoundUp(Count, 64u), 1, 1));
}

class FMeshParticleizerColorPublishCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerColorPublishCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerColorPublishCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, Size)
		SHADER_PARAMETER(uint32, PreserveHDR)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Input)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Output)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerColorPublishCS, "/Plugin/MeshParticleizer/Private/ColorPublish.usf", "MainCS", SF_Compute);

void AddMeshParticleizerColorPublishPass(FRDGBuilder& GraphBuilder, FRHITexture* Input, FRHITexture* Output, bool bPreserveHDR)
{
	auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerColorPublishCS::FParameters>();
	Parameters->Size = Output->GetDesc().Extent;
	Parameters->PreserveHDR = bPreserveHDR ? 1 : 0;
	Parameters->Input = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Input, TEXT("MeshParticleizer.ProviderInput")));
	auto* Destination = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Output, TEXT("MeshParticleizer.ProviderPublished")));
	Parameters->Output = GraphBuilder.CreateUAV(Destination);
	const auto Shader = TShaderMapRef<FMeshParticleizerColorPublishCS>(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MeshParticleizer Publish Color"), Shader, Parameters,
		FIntVector(FMath::DivideAndRoundUp(Parameters->Size.X, 8), FMath::DivideAndRoundUp(Parameters->Size.Y, 8), 1));
	GraphBuilder.SetTextureAccessFinal(Destination, ERHIAccess::SRVMask);
}
