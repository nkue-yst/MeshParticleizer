#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerTypes.h"
#include "UObject/Object.h"
#include "MeshParticleizerPreview.generated.h"

class FAdvancedPreviewScene;
class UAnimationAsset;
class UMeshParticleizerComponent;
class UMeshParticleizerPreparedMesh;
class UMeshParticleizerPreset;
class UMeshParticleizeOperation;
class UMeshComponent;
class APlayerState;
class UMaterialInterface;
class UNiagaraComponent;
class UNiagaraSystem;
class UMaterialInstanceDynamic;
class UMeshParticleizerSurfaceTexture;
class SWindow;
class FSceneView;
class FPrimitiveDrawInterface;

UENUM(BlueprintType)
enum class EMeshParticleizerPreviewMovement : uint8
{
	Stationary,
	Translate,
	Rotate
};

UENUM(BlueprintType)
enum class EMeshParticleizerPreviewDisplay : uint8
{
	Effect,
	Load,
	Order,
	SurfaceColor,
	Velocity,
	Samples
};

USTRUCT(BlueprintType)
struct MESHPARTICLEIZEREDITOR_API FMeshParticleizerPreviewSamplePoint
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Sample")
	int32 SampleId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Sample")
	int32 MaterialSlot = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Sample")
	FVector WorldPosition = FVector::ZeroVector;
};

// ウィンドウが保持する一時セッション。編集中のレベルへActorや再生設定を置かない。
UCLASS(Transient, BlueprintType)
class MESHPARTICLEIZEREDITOR_API UMeshParticleizerPreview : public UObject
{
	GENERATED_BODY()

public:
	UMeshParticleizerPreview();
	virtual ~UMeshParticleizerPreview() override;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetPreparedData(UMeshParticleizerPreparedMesh* Data, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetOptions(const FMeshParticleizeOptions& Settings, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool ApplyPreset(UMeshParticleizerPreset* Asset, FString& OutError);

	// 新しい保存先だけを受け付ける。既存プリセットは直接編集せず、コピーを保存する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	UMeshParticleizerPreset* SavePreset(const FString& PackagePath, FString& OutError);

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	TSoftObjectPtr<UMeshParticleizerPreset> Preset;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetAnimation(UAnimationAsset* Asset, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetPlaybackSpeed(float Speed, FString& OutError);

	// UV0の色を出力する材質を指定する。公開画像は通常のProvider容量へ予約する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetColorProvider(int32 MaterialSlot, UMaterialInterface* Material, FString& OutError);

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	TMap<int32, TSoftObjectPtr<UMaterialInterface>> ColorProviders;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool Replay(FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool Seek(float Seconds, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	void Restore();

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	void SetPaused(bool bPause);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	void SetFloorVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	void SetMovement(EMeshParticleizerPreviewMovement Movement);

	EMeshParticleizerPreviewMovement GetMovement() const
	{
		return this->SourceMovement;
	}

	// 表示だけを切り替え、停止中の姿勢・粒子・予算を作り直さない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetDisplayMode(EMeshParticleizerPreviewDisplay Mode, FString& OutError);

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	EMeshParticleizerPreviewDisplay DisplayMode = EMeshParticleizerPreviewDisplay::Effect;

	// 表示尺度だけを更新する。通常再生の速度・候補数・予約量には触れない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetVelocityDisplay(float MaxSpeed, int32 SampleStride, FString& OutError);

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	float VelocityMaxSpeed = 500;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	int32 VelocitySampleStride = 256;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetSampleDisplay(int32 MaterialSlot, int32 Stride, FString& OutError);

	// コピーを返し、描画中の点配列をBlueprintから変更させない。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Preview")
	TArray<FMeshParticleizerPreviewSamplePoint> GetSamplePoints() const
	{
		return this->SamplePoints;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Preview")
	int32 GetPreparedSampleCount(int32 MaterialSlot = -1) const;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	int32 SampleMaterialSlot = -1;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	int32 SampleDisplayStride = 10;

	int32 GetDisplayedSampleCount() const
	{
		return this->SamplePoints.Num();
	}
	static FLinearColor GetSampleSlotColor(int32 MaterialSlot);
	void DrawSamples(const FSceneView* View, FPrimitiveDrawInterface* PDI) const;

	// 起点は参照座標で保存する。ハンドルの編集開始時に再生とメッシュの移動を戻す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool EditOrigin(int32 PointIndex, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool SetOriginHandlePosition(FVector WorldPosition, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	void StopEditingOrigin()
	{
		this->bEditingOrigin = false;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Preview")
	FVector GetOriginHandlePosition() const;

	bool CanEditOrigin() const;
	FSimpleMulticastDelegate OnOptionsChanged;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	bool bEditingOrigin = false;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	int32 OriginPointIndex = 0;

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Preview")
	FString GetStatus() const;

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Preview")
	FMeshParticleizerBudgetUsage GetBudgetUsage() const;

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Preview")
	FMeshParticleizerAssetCacheUsage GetAssetCacheUsage() const;

	// 表示中のプレビューをPNGへ保存する。既存の証拠画像は上書きしない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	bool CaptureWindow(const FString& OutputPng, FString& OutError) const;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	void Close();

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	TObjectPtr<UMeshParticleizerPreparedMesh> Prepared;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	TObjectPtr<UMeshComponent> Mesh;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	TObjectPtr<UMeshParticleizeOperation> Operation;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	FMeshParticleizeOptions Options;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	TSoftObjectPtr<UAnimationAsset> Animation;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	float Elapsed = 0;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	float PlaybackSpeed = 1;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	bool bPaused = true;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "Preview")
	bool bFloorVisible = true;

	FAdvancedPreviewScene* GetScene();
	void Advance(float DeltaSeconds);
	float GetDuration() const
	{
		return this->Options.ConversionDuration + this->Options.ParticleLifetime;
	}
	void SetWindow(const TSharedRef<SWindow>& InWindow)
	{
		this->Window = InWindow;
	}
	void ReleaseScene();

protected:
	virtual void BeginDestroy() override;

private:
	void ResetPose();
	bool PrepareColorProviders(FString& OutError);

	bool PrepareSamples(FString& OutError);
	bool UpdateSamples(FString& OutError);
	void ClearSamples();

	struct FPreparedSample
	{
		FVector3f Position = FVector3f::ZeroVector;
		FIntVector Vertices = FIntVector::ZeroValue;
		FVector3f Barycentrics = FVector3f::ZeroVector;
		int32 MaterialSlot = 0;
	};
	TArray<FPreparedSample> PreparedSamples;
	TArray<int32> SampleSlotCounts;
	TArray<FMeshParticleizerPreviewSamplePoint> SamplePoints;
	TArray<FMatrix44f> SampleBoneMatrices;
	TWeakObjectPtr<UMeshComponent> SampleMesh;
	FTransform SampleTransform;
	bool bSamplesDirty = true;

	bool PrepareDisplayMaterials(EMeshParticleizerPreviewDisplay Mode, FString& OutError);
	bool PrepareParticleDisplayMaterials(EMeshParticleizerPreviewDisplay Mode, FString& OutError);
	UMaterialInstanceDynamic* CreateVelocityMaterial(bool bMesh, FString& OutError, float FarDistance = 0.f);
	bool UpdateDisplay(FString& OutError);
	void ClearDisplayOverlay();
	void ClearParticleDisplay();
	void UpdateParticleDisplay();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> OrderMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> SavedSlotOverlays;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SurfaceColorMaterials;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UNiagaraSystem>, TObjectPtr<UMaterialInterface>> ParticleColorMaterials;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UNiagaraSystem>, TObjectPtr<UMaterialInterface>> ParticleVelocityMaterials;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UNiagaraSystem>, TObjectPtr<UMaterialInterface>> FarParticleColorMaterials;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UNiagaraSystem>, TObjectPtr<UMaterialInterface>> FarParticleVelocityMaterials;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SavedParticleMaterial;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SavedFarParticleMaterial;

	TWeakObjectPtr<UMeshComponent> DisplayMesh;
	TWeakObjectPtr<UNiagaraComponent> DisplayParticles;
	EMeshParticleizerPreviewDisplay AppliedDisplayMode = EMeshParticleizerPreviewDisplay::Effect;
	EMeshParticleizerPreviewDisplay AppliedParticleDisplayMode = EMeshParticleizerPreviewDisplay::Effect;
	bool bParticleRenderStatePending = false;

	TSharedPtr<FAdvancedPreviewScene> Scene;
	TWeakPtr<SWindow> Window;

	UPROPERTY(Transient)
	TObjectPtr<AActor> Subject;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerComponent> Player;

	// GameModeを持たないWorldでも、UE標準のPause判定を成立させる。
	UPROPERTY(Transient)
	TObjectPtr<APlayerState> PauseOwner;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UMeshParticleizerSurfaceTexture>> BoundColorProviders;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UMaterialInterface>> LoadedProviderMaterials;

	FString Error;
	FVector BaseLocation = FVector::ZeroVector;
	EMeshParticleizerPreviewMovement SourceMovement = EMeshParticleizerPreviewMovement::Stationary;
	float SceneTime = 0;
	float SeekTarget = -1;
	int32 PreparationFrames = 0;
	bool bStartPending = false;
	bool bClosed = false;
	bool bAdvancing = false;
};
