#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;
class UTexture;
struct FMeshParticleizerSceneGpuSnapshot;
struct FMeshParticleizerDrawGpuSnapshot;

// Render TargetはMip、MSAA、解決用の別面を含む。通常Textureは全Mipの容量を返す。
MESHPARTICLEIZERRUNTIME_API int64 MeshParticleizerTextureGpuBytes(const UTexture* Texture);

// 共有資産の予約量。粒子用Static Meshは全LODのラスタ用バッファとストリーミングの旧面を含む。
MESHPARTICLEIZERRUNTIME_API bool MeshParticleizerAssetGpuBytes(const UObject* Asset, int64& OutBytes, FString& OutError);

// ソースごとの粒子・履歴・カーブ用バッファ。ワールド共通分は下のモデルで一度だけ予約する。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerGpuMemory
{
	uint32 FloatComponents = 0;
	uint32 IntComponents = 0;
	uint32 HalfComponents = 0;
	uint32 AllocationAlignment = 4096;
	int32 MinimumInstances = 0;
	int32 MeshSections = 0;
	int64 ArrayBytes = 0;
	bool bPersistentIDs = false;
	bool bSurfaceColorCache = false;
	bool bSourceEmissionCache = false;
	bool bSurfaceCoverage = false;
	uint32 CoverageBoneCount = 0;
	uint32 CoverageMaterialSlots = 0;

	static bool Build(UNiagaraSystem* System, int32 BoneCount, int32 CurveVectorCount, int32 MaterialSlots,
		FMeshParticleizerGpuMemory& OutMemory, FString& OutError);
	int64 Estimate(int32 ParticleCount) const;
};

// Niagaraの確保設定。開始前に読むため、判定中に値が変わっても一つの要求内では揃う。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerSharedGpuSettings
{
	int32 MinimumCounters = 2048;
	int32 CounterIncrement = 64;
	float CounterSlack = 1.5f;
	int32 MinimumIndirectArgs = 256;
	float IndirectGrowth = 2.0f;
	int32 MaxPendingTicks = 10;

	static FMeshParticleizerSharedGpuSettings Read();
};

// 生成済みSprite経路が使うワールド共通容量。最大値を保持し、Trimでは返さない。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerSharedGpuMemory
{
	int64 CounterCapacity = 0;
	int64 IndirectCapacity = 0;
	int64 IndirectPoolBytes = 0;
	int64 TaskBytes = 0;
	int64 DistanceFieldBytes = 0;
	int64 ScenePrimitiveBytes = 0;
	int64 SceneInstanceBytes = 0;
	int64 ScenePayloadBytes = 0;
	int64 SceneUploadBytes = 0;
	int64 DrawPoolBytes = 0;
	TMap<FName, TArray<int64>> DrawBufferHighWater;
	TMap<FName, int32> DrawBufferCounts;

	bool Grow(int32 Simulations, const FMeshParticleizerSharedGpuSettings& Settings, FString& OutError);

	// 共有Sceneの現容量へ追加要求を加える。描画の一時バッファは別途計上する。
	bool GrowScene(const FMeshParticleizerSceneGpuSnapshot& Scene, int64 AdditionalPrimitives,
		int64 AdditionalInstances, int64 AdditionalPayloadFloat4s, FString& OutError, int32 Views = 1);
	// 観測済み主Viewと共有Viewデータのpool・GPU出力の上界。upload heapは別枠。
	bool GrowDraw(const FMeshParticleizerDrawGpuSnapshot& Draws, FString& OutError);
	int64 GetSceneReservationBytes() const;
	int64 GetDrawReservationBytes() const;
	int64 GetReservationBytes() const;
};
