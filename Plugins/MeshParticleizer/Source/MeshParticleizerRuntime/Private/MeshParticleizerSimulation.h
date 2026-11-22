#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MeshParticleizerSimulation.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class UMeshParticleizerSurfaceTexture;
class UMaterialInstanceDynamic;
class FMeshParticleizerReleaseFence;
struct FMeshParticleizerGpuAllocation;

// ワールド内で再利用する一つのGPU容量。待機中も資産と容量の所有者を維持する。
UCLASS()
class UMeshParticleizerSimulation : public UObject
{
	GENERATED_BODY()

public:
	bool Matches(UNiagaraSystem* System, int32 Count, int32 Bones, int32 CurveVectors, int64 Bytes, bool bColorCache,
		bool bEmissionCache = false, bool bCoverage = false, int32 CoverageSlots = 0) const;
	void Stop();
	void PrepareForReuse();
	void Destroy();

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Particles;

	// 過去の差し替えもGPU完了まで保持する。待機容量の再利用時に参照を入れ替える。
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMeshParticleizerSurfaceTexture>> SurfaceTextures;

	// CoverageのRender Proxyを、元Actorの復元・破棄後もGPU完了まで有効に保つ。
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> CoverageMaterials;

	TSharedPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> Allocation;
	TSharedPtr<FMeshParticleizerReleaseFence> ReleaseFence;
	int64 ReservationBytes = 0;
	int32 ParticleCount = 0;
	int32 MeshSections = 0;
	int32 BoneCount = 0;
	int32 CurveVectorCount = 0;
	bool bReusable = false;
	bool bSurfaceColorCache = false;
	bool bSourceEmissionCache = false;
	bool bSurfaceCoverage = false;
	int32 CoverageMaterialSlots = 0;
};
