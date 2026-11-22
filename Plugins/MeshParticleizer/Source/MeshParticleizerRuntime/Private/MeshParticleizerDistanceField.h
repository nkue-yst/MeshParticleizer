#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerTypes.h"
#include <atomic>

class FSceneInterface;
class FGlobalDistanceFieldParameterData;

// シーン所有のGDFを借りる。テクスチャを保持せず、描画スレッドから容量と準備状態だけを公開する。
struct FMeshParticleizerDistanceField
{
	std::atomic<int64> TextureBytes{0};
	std::atomic<int64> ReservedBytes{0};
	std::atomic<uint64> Frame{0};
	std::atomic<int32> Clipmaps{0};
	std::atomic<float> VoxelSizeCm{0};
	std::atomic<bool> Fault{false};

	static TSharedRef<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> Observe(FSceneInterface* Scene);
	static int64 Measure(const FGlobalDistanceFieldParameterData* Data);
	static bool IsEnabled();
	static EMeshParticleizerGdfStatus GetSettingsStatus();
	static EMeshParticleizerGdfStatus GetStatus(const FMeshParticleizerDistanceField* Field);
	static FString DescribeStatus(EMeshParticleizerGdfStatus Status);
};
