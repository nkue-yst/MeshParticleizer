#include "MeshParticleizerSimulation.h"
#include "MeshParticleizerGpuState.h"
#include "NiagaraComponent.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraComputeExecutionContext.h"

bool UMeshParticleizerSimulation::Matches(UNiagaraSystem* System, int32 Count, int32 Bones, int32 CurveVectors, int64 Bytes,
	bool bColorCache, bool bEmissionCache, bool bCoverage, int32 CoverageSlots) const
{
	return this->bReusable == true && this->Particles != nullptr && this->Particles->GetAsset() == System &&
		this->ParticleCount == Count && this->BoneCount == Bones && this->CurveVectorCount == CurveVectors &&
		this->ReservationBytes == Bytes && this->bSurfaceColorCache == bColorCache && this->bSourceEmissionCache == bEmissionCache &&
		this->bSurfaceCoverage == bCoverage && (bCoverage == false || this->CoverageMaterialSlots == CoverageSlots);
}

void UMeshParticleizerSimulation::Stop()
{
	if (this->Allocation.IsValid() == true)
	{
		this->Allocation->bActive.store(false);
	}
	const auto Controller = this->Particles != nullptr ? this->Particles->GetSystemInstanceController() : nullptr;
	if (Controller.IsValid() == true)
	{
		Controller->WaitForConcurrentTickAndFinalize();
	}
	this->bReusable = this->bReusable == true && Controller.IsValid() == true && Controller->IsComplete() == false;
	if (this->bReusable == false)
	{
		this->Destroy();
		return;
	}

	// 完了させるとNiagaraがGPUバッファを破棄する。停止と非表示で容量を残す。
	this->Particles->SetPaused(true);
	this->Particles->SetRenderingEnabled(false);
	this->Particles->SetVisibility(false);
}

void UMeshParticleizerSimulation::PrepareForReuse()
{
	const auto Controller = this->Particles != nullptr ? this->Particles->GetSystemInstanceController() : nullptr;
	if (Controller.IsValid() == false)
	{
		return;
	}
	Controller->WaitForConcurrentTickAndFinalize();

	// 待機中のワールド原点移動を次のソースへ持ち越さない。
	this->Particles->SetWorldTransform(FTransform::Identity);
	Controller->OnPooledReuse(*this->Particles->GetWorld());

	// UE 5.8ではGPU Tick生成前の確保計算が前回の数を読む。標準の計数Fenceを先に無効化し、
	// リセット済みのTotalSpawnedParticlesを使わせる。次のGPU Tick完了でNiagaraが再公開する。
	auto* Instance = Controller->GetSystemInstance_Unsafe();
	for (const auto& Emitter : Instance->GetEmitters())
	{
		if (auto* Context = Emitter->GetGPUContext())
		{
			Context->ParticleCountReadFence.fetch_add(1, std::memory_order_relaxed);
		}
	}
}

void UMeshParticleizerSimulation::Destroy()
{
	this->bReusable = false;
	if (this->Allocation.IsValid() == true)
	{
		this->Allocation->bActive.store(false);
	}
	if (this->Particles != nullptr)
	{
		this->Particles->DestroyComponent();
		this->Particles = nullptr;
	}
	this->Allocation.Reset();
}
