#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerTypes.h"
#include "UObject/Object.h"
#include "MeshParticleizerAssetCache.generated.h"

struct FStreamableHandle;
class FMeshParticleizerReleaseFence;
struct FMeshParticleizerRenderAssetResidency;

USTRUCT()
struct FMeshParticleizerCachedAsset
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UObject> Asset;

	TWeakObjectPtr<UObject> RetiringAsset;
	TSharedPtr<FMeshParticleizerReleaseFence> Fence;
	TSharedPtr<FMeshParticleizerRenderAssetResidency> Residency;
	int64 GpuBytes = 0;
	uint64 LastUse = 0;
	int32 Users = 0;
	bool bLoading = false;
	bool bRetiring = false;
	FString Error;
};

// 資産ごとに一度だけ計上し、弱い利用者参照と強いキャッシュ参照を分ける。
UCLASS()
class UMeshParticleizerAssetCache : public UObject
{
	GENERATED_BODY()

public:
	bool Acquire(UObject* Owner, TConstArrayView<FMeshParticleizerResource> Resources, FString& OutError, bool bDisplayOnly = false);
	void Release(UObject* Owner);
	bool IsReady(const UObject* Owner, FString& OutError) const;
	bool SetLimit(int64 Bytes, FString& OutError);
	void TrimUnused();
	void Poll();
	bool HasPendingReleases() const;
	FMeshParticleizerAssetCacheUsage GetUsage() const;
	void Shutdown();

private:
	void CompleteLoads();
	void PruneOwners();
	void Evict(const FSoftObjectPath& Path);

	struct FLoadBatch
	{
		TArray<FSoftObjectPath> Paths;
		TSharedPtr<FStreamableHandle> Handle;
	};

	UPROPERTY(Transient)
	TMap<FSoftObjectPath, FMeshParticleizerCachedAsset> Entries;

	TMap<TWeakObjectPtr<const UObject>, TArray<FSoftObjectPath>> Owners;
	TArray<FLoadBatch> Loads;
	int64 LimitBytes = 512ll * 1024 * 1024;
	uint64 Serial = 0;
	static constexpr int32 AssetLimit = 2048;
};
