#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerTypes.h"
#include "MeshParticleizerGpuMemory.h"
#include "Components/SceneComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Subsystems/WorldSubsystem.h"
#include "MeshParticleizerWorldSubsystem.generated.h"

class UMeshParticleizeOperation;
class UMeshComponent;
class UMeshParticleizerPreparedMesh;
class UMeshParticleizerAssetCache;
class UMeshParticleizerSimulation;
class UMeshParticleizerSurfaceTexture;
class UTexture2D;
class UTextureRenderTarget2D;
struct FMeshParticleizerGpuState;
struct FMeshParticleizerDistanceField;
class FMeshParticleizerSceneGpu;

UCLASS()
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// 通知は次のワールド更新から発火する。呼出直後に購読できる。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	UMeshParticleizeOperation* StartParticleize(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared,
		const FMeshParticleizeOptions& Options, FString& OutError);

	// 全対象が同じ再生に属する二重要求だけ既存ハンドルを返す。操作範囲は元のグループ全体。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	UMeshParticleizeOperation* StartParticleizeTargets(const TArray<FMeshParticleizerTarget>& Targets,
		const FMeshParticleizeOptions& Options, FString& OutError);

	// Readyの間は呼出側が保持する。不要になったらRestoreMeshで表示を復元する。
	UMeshParticleizeOperation* PrepareParticleize(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared, FString& OutError);
	UMeshParticleizeOperation* PrepareParticleizeTargets(const TArray<FMeshParticleizerTarget>& Targets, FString& OutError,
		const FMeshParticleizeOptions* Options = nullptr);
	UMeshParticleizeOperation* StartPreparedParticleize(UMeshParticleizeOperation* Operation,
		const FMeshParticleizeOptions& Options, FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Budget")
	FMeshParticleizerBudgetUsage GetBudgetUsage() const;

	// 対象・全体・ソース・容量の上限をまとめて切り替える。既存の予約を下回る変更は拒否する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Budget")
	bool SetQuality(EMeshParticleizerQuality Quality, FString& OutError);

	// 個別・プール・解放待ち・ワールド共通予約を下回る変更は拒否する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Budget")
	bool SetSimulationMemoryLimit(int64 Bytes, FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Budget")
	FMeshParticleizerAssetCacheUsage GetAssetCacheUsage();

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Budget")
	bool SetAssetCacheLimit(int64 Bytes, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Budget")
	void TrimUnusedAssets();

	// 待機中のシミュレーションを解放する。容量はGPUのFence完了後に返す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Budget")
	void TrimSimulationPool();

	UMeshParticleizerAssetCache* GetAssetCache();

	// UE所有のノイズを一度だけ予約・常駐させる。ワールド終了まで共有容量を保持する。
	bool PrepareCoverageNoise(bool& bReady, FString& OutError);

	// 読込済みTextureの容量を予約し、Mip常駐を要求する。返ったハンドルのIsReadyを待つ。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color")
	UMeshParticleizerSurfaceTexture* RegisterSurfaceTexture(UTexture2D* Texture, FString& OutError);

	// 入力と公開画像の容量を予約する。入力は描画済みの2D Render Targetを渡す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color")
	UMeshParticleizerSurfaceTexture* RegisterColorProvider(UTextureRenderTarget2D* Texture, FString& OutError, bool bPreserveHDR = false);
	TSharedRef<FMeshParticleizerGpuState, ESPMode::ThreadSafe> GetGpuState();

	TSharedRef<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> GetDistanceField();
	TSharedRef<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe> GetSceneGpu();

	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableWhenPaused() const override;
	virtual TStatId GetStatId() const override;
	virtual void PostInitialize() override;
	virtual void Deinitialize() override;

	// 専用プレビューでも通常の履歴・容量・常駐管理を使う。
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	void AcquireSourceHistory(USceneComponent* Source);
	void ReleaseSourceHistory(USceneComponent* Source);
	const FMeshParticleizerTransformHistory* FindSourceHistory(USceneComponent* Source) const;
	const FMeshParticleizerBoneHistory* FindBoneHistory(USceneComponent* Source) const;
	bool IsBonePoseReady(USkinnedMeshComponent* Source, int32 BoneCount) const;
	void AcquireFullPose(USceneComponent* Source);
	void ReleaseFullPose(USceneComponent* Source);

private:
	friend class UMeshParticleizeOperation;
	friend class UMeshParticleizerSurfaceTexture;
	TArray<TWeakObjectPtr<UMeshParticleizerSurfaceTexture>> PendingColorProviders;
	bool ValidateTargetWorlds(const TArray<FMeshParticleizerTarget>& Targets, FString& OutError) const;
	UMeshParticleizeOperation* StartInternal(const TArray<FMeshParticleizerTarget>& Targets,
		const FMeshParticleizeOptions& Options, UMeshParticleizeOperation* PreparedOperation, FString& OutError);
	void Release(UMeshParticleizeOperation* Operation, bool bWaitForGpu);
	void PollReleases();
	void ReclaimDeadParticles();
	void CompleteRelease(UMeshParticleizeOperation* Operation);
	void EvictSimulation(int32 Index);
	void PollSimulationPool();
	void UpdateRenderReservation();
	int32 CountSimulations(int64& OutMeshInstances, int64& OutMeshSections) const;
	void SourceTransformChanged(USceneComponent* Source, EUpdateTransformFlags Flags, ETeleportType Teleport);

	struct FTrackedSource
	{
		FMeshParticleizerTransformHistory History;
		FMeshParticleizerBoneHistory Bones;
		int32 Users = 0;
		int32 PoseUsers = 0;
		TWeakObjectPtr<USkinnedMeshComponent> PoseComponent;
		TWeakObjectPtr<USkinnedMeshComponent> PoseLeader;
		TWeakObjectPtr<USkinnedMeshComponent> CapturedLeader;
		TWeakObjectPtr<USkinnedAsset> CapturedAsset;
		TWeakObjectPtr<USkinnedAsset> CapturedLeaderAsset;
		TArray<FMatrix44f> SkinningMatrices;
	};
	struct FFullPoseLease
	{
		int32 Users = 0;
		int32 OriginalLOD = 0;
		EVisibilityBasedAnimTickOption OriginalVisibility = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	};
	void CaptureSource(USceneComponent* Source, FTrackedSource& Tracked);
	void AcquirePoseLease(USkinnedMeshComponent* Source);
	void ReleasePoseLease(TWeakObjectPtr<USkinnedMeshComponent> Source);
	void ReleaseTrackedPose(FTrackedSource& Tracked);
	TMap<TWeakObjectPtr<USceneComponent>, FTrackedSource> TrackedSources;
	TMap<TWeakObjectPtr<USkinnedMeshComponent>, FFullPoseLease> FullPoseLeases;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizeOperation>> ActiveOperations;

	// 終了通知済みでもGPUの処理が終わるまでは、資産の参照と割当を保持する。
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizeOperation>> ReleasingOperations;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizerSimulation>> IdleSimulations;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizerSimulation>> RetiringSimulations;

	bool bShuttingDown = false;

	TSharedPtr<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> DistanceField;
	TSharedPtr<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe> SceneGpu;
	FMeshParticleizerBudget Budget;
	FMeshParticleizerSharedGpuMemory SharedGpuMemory;
	TSharedPtr<FMeshParticleizerGpuState, ESPMode::ThreadSafe> GpuState;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerAssetCache> AssetCache;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerSurfaceTexture> CoverageNoise;
};
