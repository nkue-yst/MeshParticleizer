#include "MeshParticleizerRestingGpu.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT(MeshParticleizerResting);

class FMeshParticleizerRestingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMeshParticleizerRestingCS);
	SHADER_USE_PARAMETER_STRUCT(FMeshParticleizerRestingCS, FGlobalShader);

public:
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, Phase)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, SlotOffset)
		SHADER_PARAMETER(uint32, SourceIndex)
		SHADER_PARAMETER(uint32, RestingLimit)
		SHADER_PARAMETER(uint32, Digit)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Records)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Scratch)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FMeshParticleizerRestingCS, "/Plugin/MeshParticleizer/Private/RestingBudget.usf", "MainCS", SF_Compute);

void AddMeshParticleizerRestingPasses(FRDGBuilder& GraphBuilder, TConstArrayView<FRDGBufferRef> Buffers,
	FRDGBufferRef Scratch, uint32 RestingLimit)
{
	if (Buffers.IsEmpty() == true)
	{
		return;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, MeshParticleizerResting, "MeshParticleizer Resting");

	const auto Shader = TShaderMapRef<FMeshParticleizerRestingCS>(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto Dispatch = [&](FRDGBufferRef Buffer, uint32 Phase, uint32 Offset, uint32 Digit, uint32 SourceIndex = 0) {
		auto* Parameters = GraphBuilder.AllocParameters<FMeshParticleizerRestingCS::FParameters>();
		Parameters->Phase = Phase;
		Parameters->Count = Buffer->Desc.NumElements / 4;
		Parameters->SlotOffset = Offset;
		Parameters->SourceIndex = SourceIndex;
		Parameters->RestingLimit = RestingLimit;
		Parameters->Digit = Digit;
		Parameters->Records = GraphBuilder.CreateUAV(Buffer, PF_R32_UINT);
		Parameters->Scratch = GraphBuilder.CreateUAV(Scratch, PF_R32_UINT);
		const auto Groups = Phase == 1 || Phase == 3 || Phase == 5
			? FMath::DivideAndRoundUp(Parameters->Count, 256u)
			: 1u;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("MeshParticleizer Resting %u", Phase),
			Shader, Parameters, FIntVector(Groups, 1, 1));
	};
	auto DispatchSources = [&](uint32 Phase, uint32 Digit) {
		auto Offset = uint32{0};
		for (auto Index = 0; Index < Buffers.Num(); ++Index)
		{
			auto* Buffer = Buffers[Index];
			Dispatch(Buffer, Phase, Offset, Digit, Index);
			Offset += Buffer->Desc.NumElements / 4;
		}
		return Offset;
	};

	Dispatch(Buffers[0], 0, 0, 0);
	const auto Candidates = DispatchSources(1, 0);

	// 全候補が静止しても上限内なら、選別用の走査を省く。
	if (Candidates <= RestingLimit)
	{
		return;
	}

	Dispatch(Buffers[0], 2, 0, 0);
	for (auto Digit = uint32{0}; Digit < 7; ++Digit)
	{
		DispatchSources(3, Digit);
		Dispatch(Buffers[0], 4, 0, Digit);
	}
	DispatchSources(5, 0);
}
