#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class FSceneInterface;
class ISceneViewExtension;

// 描画コマンドの入力量。RDGの統合・再利用前の値で、物理プール容量とは分ける。
struct FMeshParticleizerDrawGpuPass
{
	FName Name;
	bool bSubmitted = false;
	int64 Instances = 0;
	int64 Draws = 0;
	int64 InstanceIdBytes = 0;
	int64 CommandBytes = 0;
	int64 LoadBalancerBytes = 0;
	int64 CompactionBytes = 0;
	int32 Views = 0;

	// deferred統合前の各buffer要求。SharedViewDataはView共通の3入力と変形maskを表す。
	TArray<int64> PooledBufferBytes;
	int64 GpuWrittenBytes = 0;
};

struct FMeshParticleizerDrawGpuSnapshot
{
	uint64 Frame = 0;
	int32 Views = 0;
	bool bSupportedLayout = true;
	TArray<FMeshParticleizerDrawGpuPass> Passes;

	// 各主Viewへ追加入力を予測する。section0／pass空はScene要素のみで、共有maskを更新する。
	// 小型メッシュはソートなしを前提とし、既存の描画入力へ加算する。
	MESHPARTICLEIZERRENDERING_API bool AddMeshDemand(int64 InstancesPerView, int64 SectionsPerView,
		TConstArrayView<FName> MeshPasses, FString& OutError);
};

// 同じ描画フレームのGPU Scene。予約済み仮想アドレス量と物理コミット量を区別する。
struct FMeshParticleizerSceneGpuSnapshot
{
	uint64 Frame = 0;
	int64 PrimitiveBytes = 0;
	int64 InstanceBytes = 0;
	int64 PayloadBytes = 0;
	// Sceneが保持するscatter/index upload対。Graph内で既に退役した対は別の一時容量。
	int64 RetainedUploadBytes = 0;
	int64 VirtualBytes = 0;
	int64 Primitives = 0;
	int64 Instances = 0;
	int64 PayloadFloat4s = 0;
	uint32 PrimitiveStride = 0;
	uint32 InstanceStride = 0;
	uint32 MeshPayloadStride = 0;
	int32 TileSizeLog2 = -1;
	uint32 CommitAlignment = 16;

	int64 GetCommittedBytes() const
	{
		return this->PrimitiveBytes + this->InstanceBytes + this->PayloadBytes;
	}
};

// Worldが所有し、描画側は弱参照だけを持つ。通常再生でGPU同期やバッファ保持を行わない。
class MESHPARTICLEIZERRENDERING_API FMeshParticleizerSceneGpu
{
public:
	static TSharedRef<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe> Observe(FSceneInterface* Scene);
	FMeshParticleizerSceneGpuSnapshot Read(FMeshParticleizerDrawGpuSnapshot* OutDraws = nullptr) const;
	// familyの主View数を通知し、全Viewが揃った時点で同じframeの情報を公開する。
	void Publish(const FMeshParticleizerSceneGpuSnapshot& Value, int32 FamilyViews);
	void PublishDraws(uint64 Frame, bool bSupportedLayout, TArray<FMeshParticleizerDrawGpuPass>&& Passes, int32 RenderedViews = 1);

private:
	// Mutex保持中だけ呼び、収集中の値を外部へ出さない。
	void PublishIfComplete();

	mutable FCriticalSection Mutex;
	FMeshParticleizerSceneGpuSnapshot Snapshot;
	FMeshParticleizerDrawGpuSnapshot DrawSnapshot;
	FMeshParticleizerDrawGpuSnapshot PendingDrawSnapshot;
	FMeshParticleizerSceneGpuSnapshot PendingSnapshot;
	int32 ExpectedViews = 0;
	TSharedPtr<ISceneViewExtension, ESPMode::ThreadSafe> Observer;
};
