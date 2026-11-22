#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

// 各ソースの状態を集計し、低優先度・古い静止時刻の順に超過分を選ぶ。
// Buffersは1候補につきuintを4個持つ。末尾にソース別の消滅数を置く。
inline constexpr uint32 MeshParticleizerGpuMaxSources = 64;
inline constexpr uint32 MeshParticleizerGpuDeathOffset = 272;
inline constexpr uint32 MeshParticleizerGpuScratchCount = MeshParticleizerGpuDeathOffset + MeshParticleizerGpuMaxSources;

MESHPARTICLEIZERRENDERING_API void AddMeshParticleizerRestingPasses(FRDGBuilder& GraphBuilder,
	TConstArrayView<FRDGBufferRef> Buffers, FRDGBufferRef Scratch, uint32 RestingLimit);
