#pragma once

#include "CoreMinimal.h"
#include "RenderCommandFence.h"

// Niagaraの破棄要求とGPU完了の間は、元の割当を予約したままにする。
class FMeshParticleizerReleaseFence
{
public:
	FMeshParticleizerReleaseFence();
	bool Poll();

private:
	struct FGpuState;
	FRenderCommandFence RenderCommands;
	TSharedPtr<FGpuState, ESPMode::ThreadSafe> GpuState;
};
