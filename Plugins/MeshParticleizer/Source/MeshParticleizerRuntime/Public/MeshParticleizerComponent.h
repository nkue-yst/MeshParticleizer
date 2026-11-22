#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "MeshParticleizerTypes.h"
#include "MeshParticleizerComponent.generated.h"

class UMeshComponent;
class UMeshParticleizerPreparedMesh;
class UMeshParticleizerPreset;
class UMeshParticleizeOperation;
struct FStreamableHandle;

UENUM(BlueprintType)
enum class EMeshParticleizerPreparationState : uint8
{
	Unprepared,
	Preparing,
	Ready,
	Failed
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FMeshParticleizerPreparedEvent);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FMeshParticleizerPreparationFailedEvent, const FString&, Reason);

// Actor側の導入窓口。資産は先に読み込み、再生の寿命はワールドへ渡す。
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMeshParticleizerComponent();

	UPROPERTY(EditAnywhere, Category = "Mesh Particleizer", meta = (UseComponentPicker, AllowedClasses = "/Script/Engine.MeshComponent"))
	FComponentReference Target;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mesh Particleizer")
	TSoftObjectPtr<UMeshParticleizerPreparedMesh> PreparedData;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mesh Particleizer")
	FMeshParticleizeOptions Options;

	UPROPERTY(BlueprintAssignable, Category = "Mesh Particleizer")
	FMeshParticleizerPreparedEvent OnPrepared;

	UPROPERTY(BlueprintAssignable, Category = "Mesh Particleizer")
	FMeshParticleizerPreparationFailedEvent OnPreparationFailed;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	bool Configure(UMeshComponent* Source, TSoftObjectPtr<UMeshParticleizerPreparedMesh> Prepared, FString& OutError);

	// 配列順がSource Indexになる。不成立時は現在の登録と準備状態を保持する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	bool ConfigureTargets(const TArray<FMeshParticleizerTarget>& Sources, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	bool SetOptions(const FMeshParticleizeOptions& Settings, FString& OutError);

	// 検証した既定値をOptionsへコピーする。準備依存の変更はSetOptionsと同じ規則で扱う。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preset")
	bool ApplyPreset(UMeshParticleizerPreset* Preset, FString& OutError);

	// 準備後の命中時に呼ぶ。登録対象のデータを使い、同期ロードや起点設定の変更は行わない。
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Mesh Particleizer|Origin", meta = (AdvancedDisplay = "MaxProjectionDistance"))
	FMeshParticleizerHitOrigin CaptureRegisteredHitOrigin(const FHitResult& Hit,
		bool bProjectToSurface = true, double MaxProjectionDistance = 10) const;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Advanced", meta = (DisplayName = "Prepare For Particleize (Events)"))
	void PrepareForParticleize();

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Advanced", meta = (DisplayName = "Start Particleize (Operation)"))
	UMeshParticleizeOperation* StartParticleize(FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	UMeshParticleizeOperation* GetLastOperation() const
	{
		return this->LastOperation;
	}

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color")
	bool SetSurfaceColorParameters(int32 SourceIndex, int32 MaterialSlot,
		const FMeshParticleizerSurfaceColorParameters& Parameters, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer")
	void RestoreMesh();

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer")
	EMeshParticleizerPreparationState GetPreparationState() const
	{
		return this->State;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer", meta = (DisplayName = "Get Particleize Diagnostics"))
	FString GetPreparationDiagnostics() const
	{
		return this->Diagnostics;
	}

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TArray<FMeshParticleizerTarget> ResolveTargets() const;
	void StartTracking();
	void ReleaseFullPose();
	void ReleasePreparedOperation();

	void LoadResources();
	void CheckResources();
	void FailPreparation(const FString& Error);
	void ReleaseResourceRegistration();

	UPROPERTY(Transient)
	TArray<FMeshParticleizerTarget> ConfiguredTargets;

	// 姿勢履歴を追跡している対象メッシュ。
	TArray<TWeakObjectPtr<UMeshComponent>> TrackedSources;

	// 非同期ロードと準備完了の待機に使う。
	TSharedPtr<FStreamableHandle> DataLoad;
	FTimerHandle ReadinessTimer;
	double PreparationDeadline = 0;
	bool bHoldsFullPose = false;

	FString Diagnostics;
	EMeshParticleizerPreparationState State = EMeshParticleizerPreparationState::Unprepared;

	// Readyまでの表示準備はComponentが保持し、開始成功時にワールドへ渡す。
	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizeOperation> PreparedOperation;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizeOperation> LastOperation;
};
