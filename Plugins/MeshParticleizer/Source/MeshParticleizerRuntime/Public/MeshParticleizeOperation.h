#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerTypes.h"
#include "UObject/Object.h"
#include "MeshParticleizeOperation.generated.h"

class UMeshComponent;
class UNiagaraComponent;
class UMeshParticleizerWorldSubsystem;
class UMeshParticleizeSource;
class FMeshParticleizerReleaseFence;

UENUM(BlueprintType)
enum class EMeshParticleizeFinishReason : uint8
{
	Completed,
	Cancelled,
	SourceLost,
	SourceChanged
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMeshParticleizeEvent, UMeshParticleizeOperation*, Operation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FMeshParticleizeFinishedEvent, UMeshParticleizeOperation*, Operation, EMeshParticleizeFinishReason, Reason);

// ワールドが再生中だけ保持するハンドル。呼び出し側は終了後も状態を参照できる。
UCLASS(BlueprintType)
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizeOperation : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Mesh Particleizer")
	FMeshParticleizeEvent OnStarted;

	UPROPERTY(BlueprintAssignable, Category = "Mesh Particleizer")
	FMeshParticleizeEvent OnMeshConverted;

	UPROPERTY(BlueprintAssignable, Category = "Mesh Particleizer")
	FMeshParticleizeEvent OnCancelled;

	UPROPERTY(BlueprintAssignable, Category = "Mesh Particleizer")
	FMeshParticleizeFinishedEvent OnFinished;

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	EMeshParticleizeState GetState() const
	{
		return this->State;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	float GetProgress() const
	{
		return this->Progress;
	}

	// 残り時間はPauseと時間倍率を含むWorldの論理時間で返す。終了後は0。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	void GetParticleizeStatus(EMeshParticleizeState& OutState, float& OutProgress, float& OutRemainingTime) const;

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	int32 GetGrantedParticleCount() const
	{
		return this->GrantedParticles;
	}

	// GPUで消滅を確認した分を除く現在の粒子枠。開始時の採用数は変えない。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Budget")
	int32 GetReservedParticleCount() const
	{
		return this->ReservedParticles;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Budget")
	int64 GetSimulationReservationBytes() const
	{
		return this->SimulationReservationBytes;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	int32 GetSourceCount() const
	{
		return this->Sources.Num();
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	int32 GetGrantedParticlesForSource(UMeshComponent* Mesh) const;

	// 面積はcm²。開始時のスケールを使い、再生中の変形で配分し直さない。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Budget")
	FMeshParticleizerSourceAllocation GetSourceAllocation(int32 SourceIndex = 0) const;

	// 登録順の部品を指定する。共有時はどの部品でも同じ進行座標・起点を返す。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Origin")
	FMeshParticleizerResolvedOrigin GetResolvedOrigin(int32 SourceIndex = 0) const;

	// 登録順の部品の床判定を返す。終了後も開始時の結果を保持する。
	// bEnabledは水平面の有効状態であり、粒子の生存数やGDFの有効状態ではない。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Collision")
	FMeshParticleizerResolvedGround GetResolvedGround(int32 SourceIndex = 0) const;

#if WITH_EDITOR
	// Editorの表示補助用。姿勢固定中は実際に描画している複製を返す。
	UMeshComponent* GetDisplayMeshForEditor(int32 SourceIndex = 0) const;
	UNiagaraComponent* GetParticlesForEditor(int32 SourceIndex = 0) const;
#endif

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer", meta = (DisplayName = "Get Particleize Diagnostics"))
	FString GetDiagnostics() const
	{
		return this->Diagnostics;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	FMeshParticleizeResult GetResult() const;

	// Readyまたは変換中に呼ぶ。変換中の変更は次の表面更新で揃えて適用する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color")
	bool SetSurfaceColorParameters(int32 SourceIndex, int32 MaterialSlot,
		const FMeshParticleizerSurfaceColorParameters& Parameters, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer", meta = (DisplayName = "Cancel Particleize"))
	void Cancel(bool bRestoreMesh = true);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	void RestoreMesh();

private:
	friend class UMeshParticleizerWorldSubsystem;
	friend class UMeshParticleizerComponent;

	// 開始前に、登録対象と再生に必要な資源を揃える。
	bool SetTargets(const TArray<FMeshParticleizerTarget>& InTargets, FString& OutError);
	bool ValidateTargets(const TArray<FMeshParticleizerTarget>& InTargets, FString& OutError) const;
	bool PrepareTargets(const TArray<FMeshParticleizerTarget>& InTargets, FString& OutError);
	bool PrepareRendering(const FMeshParticleizeOptions& InOptions, bool bStarting, bool& bReady, FString& OutError);
	bool PrepareTiming(const FMeshParticleizeOptions& InOptions, FString& OutError);
	bool PrepareStyle(const FMeshParticleizeOptions& InOptions, FString& OutError);
	bool Initialize(const TArray<FMeshParticleizerTarget>& InTargets, const FMeshParticleizeOptions& InOptions, FString& OutError);
	bool ContainsSource(UMeshComponent* Mesh) const;

	// 再生中の更新と、終了時の表示・資源の解放を管理する。
	void Advance(double Now);
	void Finish(EMeshParticleizeFinishReason Reason);
	void ReleaseResources();
	void ReclaimDeadParticles();
	void RestoreSourceMaterials();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizeSource>> Sources;

	UPROPERTY(Transient)
	TArray<FMeshParticleizerTarget> Targets;

	// 準備からOperation終了までStyleを保持し、GPU資産は共通cacheで所有する。
	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerParticleStyle> PreparedStyle;

	FMeshParticleizeOptions Options;
	FMeshParticleizeResult StartResult;
	TArray<FMeshParticleizerSourceAllocation> SourceAllocations;

	// 開始時に確定した進行カーブと衝突設定を再生中に参照する。
	FMeshParticleizeSchedule Schedule;
	FSoftObjectPath PreparedCurvePath;
	bool bTimingPrepared = false;
	bool bCollisionPrepared = false;
	EMeshParticleizerCollision PreparedCollision = EMeshParticleizerCollision::SimpleGround;
	EMeshParticleizerCollision ResolvedCollision = EMeshParticleizerCollision::SimpleGround;
	uint64 CollisionReadyFrame = 0;
	FString CollisionDiagnostics;

	double FinishTime = 0;
	FString Diagnostics;
	float Progress = 0;
	int32 GrantedParticles = 0;
	int32 ReservedParticles = 0;
	int64 SimulationReservationBytes = 0;
	EMeshParticleizeState State = EMeshParticleizeState::Ready;
	EMeshParticleizeFinishReason FinishReason = EMeshParticleizeFinishReason::Completed;
	bool bReserved = false;
	bool bStarted = false;
	bool bConverted = false;

	// 終了後のGPU処理を待つ間はワールドがOperationを保持する。
	TSharedPtr<FMeshParticleizerReleaseFence> ReleaseFence;
};
