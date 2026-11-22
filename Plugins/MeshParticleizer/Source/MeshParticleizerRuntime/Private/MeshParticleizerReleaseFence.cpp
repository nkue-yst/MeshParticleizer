#include "MeshParticleizerReleaseFence.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

struct FMeshParticleizerReleaseFence::FGpuState
{
	FGPUFenceRHIRef Fence;
	TAtomic<bool> bPollQueued = false;
	TAtomic<bool> bComplete = false;
};

FMeshParticleizerReleaseFence::FMeshParticleizerReleaseFence()
{
	// NiagaraとSceneProxyの別コマンド列も含め、先行命令のRHIへの送出を待つ。
	this->RenderCommands.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
}

bool FMeshParticleizerReleaseFence::Poll()
{
	if (this->RenderCommands.IsFenceComplete() == false)
	{
		return false;
	}

	if (this->GpuState.IsValid() == false)
	{
		this->GpuState = MakeShared<FGpuState, ESPMode::ThreadSafe>();
		this->GpuState->bPollQueued.Store(true);
		ENQUEUE_RENDER_COMMAND(MeshParticleizerWriteReleaseFence)(
			[State = this->GpuState](FRHICommandListImmediate& RHICmdList) {
				State->Fence = RHICreateGPUFence(TEXT("MeshParticleizerRelease"));
				RHICmdList.WriteGPUFence(State->Fence);
				State->bPollQueued.Store(false);
			});
		return false;
	}
	if (this->GpuState->bComplete.Load() == true)
	{
		return true;
	}

	// 一度に一つだけ確認する。待機やFlush、UObjectのキャプチャは行わない。
	if (this->GpuState->bPollQueued.Exchange(true) == false)
	{
		ENQUEUE_RENDER_COMMAND(MeshParticleizerPollReleaseFence)(
			[State = this->GpuState](FRHICommandListImmediate& RHICmdList) {
				if (State->Fence->NumPendingWriteCommands.GetValue() == 0 && State->Fence->Poll() == true)
				{
					State->bComplete.Store(true);
				}
				State->bPollQueued.Store(false);
			});
	}
	return false;
}
