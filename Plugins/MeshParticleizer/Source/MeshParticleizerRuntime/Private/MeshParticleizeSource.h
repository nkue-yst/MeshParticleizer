#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerTypes.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerMaterialInputs.h"
#include "SceneTypes.h"
#include "UObject/Object.h"
#include "MeshParticleizeSource.generated.h"

class UMeshComponent;
class USkinnedMeshComponent;
class UMeshParticleizerPreparedMesh;
class UNiagaraComponent;
class UNiagaraSystem;
class UMaterialInterface;
class UMaterialInstanceDynamic;
struct FMeshParticleizerGpuAllocation;
class UMeshParticleizerSimulation;
class UMeshParticleizerSurfaceTexture;
class UNiagaraDataInterfaceMeshParticleizerCoverage;

// 一部品の描画と復元状態。時間、予算、通知は所有するOperationがまとめて管理する。
UCLASS()
class UMeshParticleizeSource : public UObject
{
	GENERATED_BODY()

private:
	friend class UMeshParticleizeOperation;
	friend class UMeshParticleizerWorldSubsystem;
	bool ValidateSource(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared, FString& OutError) const;
	bool ValidateStart(const FMeshParticleizeOptions& InOptions, FString& OutError) const;
	bool SetSurfaceColorParameters(int32 MaterialSlot, const FMeshParticleizerSurfaceColorParameters& Parameters,
		bool bDeferToUpdate, FString& OutError);
	void UploadSurfaceColorParameters();
	bool PrepareMaterials(UMeshComponent* Mesh, UMeshParticleizerPreparedMesh* Prepared, FString& OutError);
	bool Initialize(const FMeshParticleizeOptions& InOptions, const FMeshParticleizeSchedule& InSchedule,
		const FMeshParticleizerProgressSpace& ProgressSpace, const FMeshParticleizerResolvedOrigin& Origin,
		TConstArrayView<FMeshParticleizerTarget> Targets, int32 ParticleCount, FString& OutError);
	void Activate();
	void Update(double Now, float Progress, bool bReadSource);
	void StopDetachment();
	void StopReading();
	bool ReleaseResources();
	void UpdateTransformParameters(const FTransform& Current, const FTransform& Previous);
	void UpdateSourceMotion(UMeshComponent* Mesh);
	void ReleaseSourceHistory();
	void RestoreShadowCache();
	void RestoreSourceMaterials();
	void RestoreDistanceField();
	bool IsSourceAvailable(UMeshComponent* Mesh, FString& OutError) const;
	bool FreezeSource(FString& OutError);
	void ReleaseFrozenSource();

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerPreparedMesh> PreparedMesh;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Particles;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraDataInterfaceMeshParticleizerCoverage> SurfaceCoverage;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraSystem> PlaybackSystem;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerSimulation> Simulation;

	UPROPERTY(Transient)
	TObjectPtr<UMeshComponent> FrozenMesh;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> OriginalMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> DissolveMaterials;

	TArray<FVector4f> SurfaceTints;
	TArray<FVector4f> ProviderUVs;
	TArray<uint64> SurfaceColorRevisions;
	TArray<FVector4f> EmissionUVs;
	TArray<uint64> EmissionRevisions;
	TArray<TArray<FMeshParticleizerMaterialParameterKey>> MaterialParameterKeys;
	TArray<FMeshParticleizerMaterialInputs> MaterialInputs;
	bool bSurfaceTintsDirty = false;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizerSurfaceTexture>> SurfaceTextures;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizerSurfaceTexture>> EmissionProviders;

	TWeakObjectPtr<UMeshComponent> Source;
	TWeakObjectPtr<USkinnedMeshComponent> PoseLeader;
	FMeshParticleizeOptions Options;
	FMeshParticleizerResolvedOrigin ResolvedOrigin;
	FMeshParticleizerResolvedGround ResolvedGround;
	FMeshParticleizeSchedule Schedule;
	TSharedPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> GpuAllocation;
	FMeshParticleizerTransformHistory FrozenTransforms;
	FMeshParticleizerBoneHistory FrozenBones;
	FVector FrozenFallbackCenter = FVector::ZeroVector;
	double PreviousElapsed = 0;
	uint64 UploadedBoneSerial = MAX_uint64;
	FString Diagnostics;
	int32 GrantedParticles = 0;
	bool bTracksSource = false;
	bool bRetainSourceForProfiling = false;
	bool bDistanceFieldOverridden = false;
	bool bShadowCacheOverridden = false;
	bool bSourceVisibilityOverridden = false;
	bool bOriginalVisible = true;
	bool bOriginalHiddenShadow = false;
	bool bFrozenMotionUploaded = false;
	EShadowCacheInvalidationBehavior OriginalShadowCache = EShadowCacheInvalidationBehavior::Auto;
};
