#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizeAsyncAction.generated.h"

class UMeshParticleizerComponent;
class UMeshParticleizerPreset;

// Asyncノードは最初の通知から出力ピンを作る。ReasonはFinished/Cancelledで参照する。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FMeshParticleizeAsyncEvent, UMeshParticleizeOperation*, Operation,
	EMeshParticleizeFinishReason, Reason, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FMeshParticleizerPreparationEvent, UMeshParticleizerComponent*, Component, const FString&, Diagnostics);

// 開始要求にだけ適用する起点。無効ならPresetまたはComponentの値を保つ。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerOriginOverride
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin")
	bool bOverride = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride"))
	EMeshParticleizerOrigin Mode = EMeshParticleizerOrigin::Direction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride"))
	bool bShareOrigin = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride && Mode == EMeshParticleizerOrigin::Direction", EditConditionHides))
	FVector Direction = FVector(0, 0, -1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride && Mode == EMeshParticleizerOrigin::Direction", EditConditionHides))
	bool bDirectionInWorldSpace = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride && Mode == EMeshParticleizerOrigin::Point", EditConditionHides))
	FVector LocalOrigin = FVector::ZeroVector;

	// 入力のコピーを開始前に検証する。実行中の起点配列は公開しない。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride && Mode == EMeshParticleizerOrigin::MultiplePoints", EditConditionHides))
	TArray<FVector> LocalOrigins;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride && Mode == EMeshParticleizerOrigin::BoneOrSocket", EditConditionHides))
	FName BoneOrSocket;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "bOverride && Mode == EMeshParticleizerOrigin::Hit", EditConditionHides))
	FMeshParticleizerHitOrigin Hit;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Origin", meta = (EditCondition = "bOverride"))
	FName FallbackBone;
};

// 呼出し側のComponentを準備し、通知後も登録と履歴を保持する。
UCLASS()
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizePrepareAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMeshParticleizerPreparationEvent Prepared;

	UPROPERTY(BlueprintAssignable)
	FMeshParticleizerPreparationEvent Failed;

	// MeshとPreparedDataを省略すると登録済みの対象を使う。Presetは要求時点でコピーする。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer", meta = (BlueprintInternalUseOnly = "true", DisplayName = "Prepare For Particleize"))
	static UMeshParticleizePrepareAsyncAction* PrepareForParticleize(UMeshParticleizerComponent* Component,
		UMeshComponent* Mesh = nullptr, TSoftObjectPtr<UMeshParticleizerPreparedMesh> PreparedData = nullptr,
		UMeshParticleizerPreset* Preset = nullptr);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandlePrepared();

	UFUNCTION()
	void HandleFailed(const FString& Error);

	void ReleaseBindings();

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerComponent> Preparation;

	UPROPERTY(Transient)
	FMeshParticleizerTarget Target;

	FMeshParticleizeOptions Settings;
	FString SetupError;
	bool bConfigureTarget = false;
	bool bActivated = false;
};

UCLASS()
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizeAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMeshParticleizeAsyncEvent Started;

	UPROPERTY(BlueprintAssignable)
	FMeshParticleizeAsyncEvent MeshConverted;

	UPROPERTY(BlueprintAssignable)
	FMeshParticleizeAsyncEvent Cancelled;

	UPROPERTY(BlueprintAssignable)
	FMeshParticleizeAsyncEvent Finished;

	UPROPERTY(BlueprintAssignable)
	FMeshParticleizeAsyncEvent Failed;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Advanced", meta = (BlueprintInternalUseOnly = "true", DisplayName = "Particleize Mesh (Options)"))
	static UMeshParticleizeAsyncAction* ParticleizeMesh(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared,
		FMeshParticleizeOptions Options);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer", meta = (BlueprintInternalUseOnly = "true"))
	static UMeshParticleizeAsyncAction* ParticleizeTargets(const TArray<FMeshParticleizerTarget>& Targets,
		FMeshParticleizeOptions Options);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer", meta = (BlueprintInternalUseOnly = "true"))
	static UMeshParticleizeAsyncAction* ParticleizeMeshWithPreset(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared,
		UMeshParticleizerPreset* Preset);

	// 時間の-1はPresetまたはComponentの値を使う。0は即時完了として扱う。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer", meta = (BlueprintInternalUseOnly = "true", AdvancedDisplay = "Origin", AutoCreateRefTerm = "Origin", CPP_Default_ConversionDuration = "-1.0", CPP_Default_ParticleLifetime = "-1.0", CPP_Default_Preset = "None"))
	static UMeshParticleizeAsyncAction* StartParticleize(UMeshParticleizerComponent* Component,
		float ConversionDuration, float ParticleLifetime, UMeshParticleizerPreset* Preset,
		const FMeshParticleizerOriginOverride& Origin);

	// 履歴を登録していない対象には、既存の単発経路と同じ診断を返す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer", meta = (BlueprintInternalUseOnly = "true", DisplayName = "Particleize Mesh", AdvancedDisplay = "Origin", AutoCreateRefTerm = "Origin", CPP_Default_ConversionDuration = "-1.0", CPP_Default_ParticleLifetime = "-1.0", CPP_Default_Preset = "None"))
	static UMeshParticleizeAsyncAction* ParticleizeMeshSimple(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared,
		float ConversionDuration, float ParticleLifetime, UMeshParticleizerPreset* Preset,
		const FMeshParticleizerOriginOverride& Origin);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandlePrepared();

	UFUNCTION()
	void HandlePreparationFailed(const FString& Error);

	void ReleasePreparation();

	UFUNCTION()
	void HandleStarted(UMeshParticleizeOperation* InOperation);

	UFUNCTION()
	void HandleConverted(UMeshParticleizeOperation* InOperation);

	UFUNCTION()
	void HandleCancelled(UMeshParticleizeOperation* InOperation);

	UFUNCTION()
	void HandleFinished(UMeshParticleizeOperation* InOperation, EMeshParticleizeFinishReason Reason);

	UPROPERTY(Transient)
	TArray<FMeshParticleizerTarget> Sources;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizeOperation> Operation;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerComponent> Preparation;

	FMeshParticleizeOptions Settings;
	FString SetupError;
	bool bUseRegisteredComponent = false;
	bool bOwnsPreparation = false;
	bool bActivated = false;
};
