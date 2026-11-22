#include "MeshParticleizerWorldSubsystem.h"
#include "MeshParticleizerDistanceField.h"
#include "MeshParticleizerSceneGpu.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerReleaseFence.h"
#include "MeshParticleizerAssetCache.h"
#include "MeshParticleizerSurfaceTexture.h"
#include "MeshParticleizerGpuState.h"
#include "MeshParticleizerSimulation.h"
#include "MeshParticleizeSource.h"
#include "Components/MeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "MeshParticleizerProfiling.h"
#include "Kismet/GameplayStatics.h"
#include "Rendering/SkeletalMeshRenderData.h"

CSV_DEFINE_CATEGORY(MeshParticleizer, true);

UMeshParticleizeOperation* UMeshParticleizerWorldSubsystem::StartParticleize(UMeshComponent* Mesh,
	UMeshParticleizerPreparedMesh* Prepared, const FMeshParticleizeOptions& Options, FString& OutError)
{
	auto Target = FMeshParticleizerTarget{};
	Target.Mesh = Mesh;
	Target.PreparedData = Prepared;
	return this->StartInternal({Target}, Options, nullptr, OutError);
}

UMeshParticleizeOperation* UMeshParticleizerWorldSubsystem::StartParticleizeTargets(const TArray<FMeshParticleizerTarget>& Targets,
	const FMeshParticleizeOptions& Options, FString& OutError)
{
	return this->StartInternal(Targets, Options, nullptr, OutError);
}

UMeshParticleizeOperation* UMeshParticleizerWorldSubsystem::PrepareParticleize(UMeshComponent* Mesh,
	UMeshParticleizerPreparedMesh* Prepared, FString& OutError)
{
	auto Target = FMeshParticleizerTarget{};
	Target.Mesh = Mesh;
	Target.PreparedData = Prepared;
	return this->PrepareParticleizeTargets({Target}, OutError);
}

bool UMeshParticleizerWorldSubsystem::ValidateTargetWorlds(const TArray<FMeshParticleizerTarget>& Targets, FString& OutError) const
{
	if (Targets.IsEmpty() == true || this->GetWorld()->IsGameWorld() == false || this->GetWorld()->GetNetMode() == NM_DedicatedServer)
	{
		OutError = TEXT("Register targets in a rendering game world.");
		return false;
	}
	return FMeshParticleizerTarget::ValidateTargets(Targets, this->GetWorld(), OutError);
}

UMeshParticleizeOperation* UMeshParticleizerWorldSubsystem::PrepareParticleizeTargets(
	const TArray<FMeshParticleizerTarget>& Targets, FString& OutError, const FMeshParticleizeOptions* Options)
{
	OutError.Reset();
	if (this->ValidateTargetWorlds(Targets, OutError) == false)
	{
		return nullptr;
	}

	// 準備だけでは予算と再生寿命を確保しない。呼出側が表示の復元責任を持つ。
	auto* Operation = NewObject<UMeshParticleizeOperation>(this);
	if (Options != nullptr && (Operation->PrepareStyle(*Options, OutError) == false || Operation->PrepareTiming(*Options, OutError) == false))
	{
		return nullptr;
	}
	if (Operation->PrepareTargets(Targets, OutError) == false)
	{
		return nullptr;
	}
	return Operation;
}

UMeshParticleizeOperation* UMeshParticleizerWorldSubsystem::StartPreparedParticleize(UMeshParticleizeOperation* Operation,
	const FMeshParticleizeOptions& Options, FString& OutError)
{
	OutError.Reset();
	if (IsValid(Operation) == false || Operation->GetOuter() != this || Operation->State != EMeshParticleizeState::Ready ||
		Operation->bStarted == true || Operation->bReserved == true)
	{
		OutError = TEXT("A ready operation owned by this world is required.");
		return nullptr;
	}

	return this->StartInternal(Operation->Targets, Options, Operation, OutError);
}

UMeshParticleizeOperation* UMeshParticleizerWorldSubsystem::StartInternal(const TArray<FMeshParticleizerTarget>& Targets,
	const FMeshParticleizeOptions& Options,
	UMeshParticleizeOperation* PreparedOperation, FString& OutError)
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, Start);
	OutError.Reset();
	if (this->ValidateTargetWorlds(Targets, OutError) == false)
	{
		return nullptr;
	}
	this->ReclaimDeadParticles();
	this->PollReleases();
	this->PollSimulationPool();
	for (const auto& Existing : this->ActiveOperations)
	{
		if (Existing->bConverted == true || Existing->State == EMeshParticleizeState::Draining)
		{
			continue;
		}

		// 要求全体が同じ再生に含まれる場合だけ、その既存ハンドルを返す。
		auto OverlapCount = 0;
		auto FirstOverlap = int32{INDEX_NONE};
		for (auto Index = 0; Index < Targets.Num(); ++Index)
		{
			if (Existing->ContainsSource(Targets[Index].Mesh.Get()) == true)
			{
				++OverlapCount;
				if (FirstOverlap == INDEX_NONE)
				{
					FirstOverlap = Index;
				}
			}
		}
		if (OverlapCount == 0)
		{
			continue;
		}
		if (OverlapCount == Targets.Num())
		{
			OutError = TEXT("Every requested mesh already belongs to the same active operation. The returned handle controls that complete group; no new playback was started.");
			return Existing;
		}

		OutError = FString::Printf(TEXT("Source %d '%s' belongs to an active operation, but only %d of %d requested targets belong to that operation. Start the complete group while idle; no targets were changed."),
			FirstOverlap, *GetNameSafe(Targets[FirstOverlap].Mesh.Get()), OverlapCount, Targets.Num());
		return nullptr;
	}

	auto* Operation = PreparedOperation != nullptr ? PreparedOperation : NewObject<UMeshParticleizeOperation>(this);
	if (Operation->Initialize(Targets, Options, OutError) == false)
	{
		// 未変更の準備済みOperationは、入力を直して再試行できる状態を保つ。
		if (PreparedOperation == nullptr)
		{
			Operation->RestoreSourceMaterials();
			Operation->ReleaseResources();
		}
		return nullptr;
	}
	this->ActiveOperations.Add(Operation);
	return Operation;
}

void UMeshParticleizerWorldSubsystem::Tick(float DeltaTime)
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, Update);
	this->PollReleases();
	this->PollSimulationPool();
	this->ReclaimDeadParticles();
	if (this->AssetCache != nullptr)
	{
		this->AssetCache->Poll();
	}
	this->UpdateRenderReservation();
	if (UGameplayStatics::IsGamePaused(this->GetWorld()) == true)
	{
		return;
	}

	// 公開画像を先に更新し、その版をこのフレームの表示とGPU粒子へ同時に渡す。
	auto Providers = MoveTemp(this->PendingColorProviders);
	this->PendingColorProviders.Reset();
	for (const auto& Provider : Providers)
	{
		if (auto* Registration = Provider.Get())
		{
			Registration->PublishPending();
		}
	}

	// TickableWorldSubsystemはPostPhysicsの後。物理で確定した位置を通知前に記録する。
	for (auto It = this->TrackedSources.CreateIterator(); It; ++It)
	{
		auto* Source = It.Key().Get();
		if (Source == nullptr)
		{
			this->ReleaseTrackedPose(It.Value());
			It.RemoveCurrent();
			continue;
		}
		this->CaptureSource(Source, It.Value());
	}

	// 通知から再生・中断しても列挙を壊さないよう、当該フレームの対象を固定する。
	const auto Snapshot = this->ActiveOperations;
	for (const auto& Operation : Snapshot)
	{
		Operation->Advance(this->GetWorld()->GetTimeSeconds());
	}
}

bool UMeshParticleizerWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::GamePreview || Super::DoesSupportWorldType(WorldType);
}

bool UMeshParticleizerWorldSubsystem::IsTickable() const
{
	return this->IsTemplate() == false && (this->PendingColorProviders.IsEmpty() == false || this->ActiveOperations.IsEmpty() == false || this->TrackedSources.IsEmpty() == false || this->ReleasingOperations.IsEmpty() == false || this->RetiringSimulations.IsEmpty() == false || (this->AssetCache != nullptr && this->AssetCache->HasPendingReleases() == true));
}

bool UMeshParticleizerWorldSubsystem::IsTickableWhenPaused() const
{
	return this->ActiveOperations.IsEmpty() == false || this->ReleasingOperations.IsEmpty() == false || this->RetiringSimulations.IsEmpty() == false ||
		(this->AssetCache != nullptr && this->AssetCache->HasPendingReleases() == true);
}

TStatId UMeshParticleizerWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(MeshParticleizerWorldSubsystem, STATGROUP_Tickables);
}

void UMeshParticleizerWorldSubsystem::Release(UMeshParticleizeOperation* Operation, bool bWaitForGpu)
{
	this->ActiveOperations.Remove(Operation);
	if (bWaitForGpu == true)
	{
		Operation->ReleaseFence = MakeShared<FMeshParticleizerReleaseFence>();
		this->ReleasingOperations.Add(Operation);
	}
	else
	{
		this->CompleteRelease(Operation);
	}
}

void UMeshParticleizerWorldSubsystem::PollReleases()
{
	// 最後のソースが無くなっても、集計用のGPUコピーが残る間はその容量を返さない。
	if (this->ActiveOperations.IsEmpty() == true && this->GpuState.IsValid() == true &&
		this->GpuState->ReadbackPending.load() == true)
	{
		return;
	}

	for (auto Index = this->ReleasingOperations.Num() - 1; Index >= 0; --Index)
	{
		auto* Operation = this->ReleasingOperations[Index].Get();
		if (Operation->ReleaseFence->Poll() == false)
		{
			continue;
		}

		this->CompleteRelease(Operation);
		Operation->ReleaseFence.Reset();
		this->ReleasingOperations.RemoveAtSwap(Index);
	}
}

void UMeshParticleizerWorldSubsystem::CompleteRelease(UMeshParticleizeOperation* Operation)
{
	auto RetainedBytes = int64{0};
	for (const auto& Source : Operation->Sources)
	{
		auto* Simulation = Source->Simulation.Get();
		if (Simulation == nullptr)
		{
			continue;
		}
		if (this->bShuttingDown == false && Simulation->bReusable == true)
		{
			this->IdleSimulations.Add(Simulation);
			RetainedBytes += Simulation->ReservationBytes;
		}
		else
		{
			this->GetAssetCache()->Release(Simulation);
		}
		Source->Simulation = nullptr;
	}
	this->Budget.Release(Operation->ReservedParticles, Operation->Sources.Num(), Operation->SimulationReservationBytes - RetainedBytes);
	Operation->ReservedParticles = 0;
	this->GetAssetCache()->Release(Operation);

	// 品質ごとのソース上限を待機件数の上限にも用いる。古い容量から手放す。
	while (this->IdleSimulations.Num() > this->Budget.SourceLimit)
	{
		this->EvictSimulation(0);
	}
}

void UMeshParticleizerWorldSubsystem::EvictSimulation(int32 Index)
{
	auto* Simulation = this->IdleSimulations[Index].Get();
	Simulation->Destroy();
	Simulation->ReleaseFence = MakeShared<FMeshParticleizerReleaseFence>();
	this->RetiringSimulations.Add(Simulation);
	this->IdleSimulations.RemoveAt(Index);
}

void UMeshParticleizerWorldSubsystem::TrimSimulationPool()
{
	while (this->IdleSimulations.IsEmpty() == false)
	{
		this->EvictSimulation(0);
	}
}

void UMeshParticleizerWorldSubsystem::PollSimulationPool()
{
	for (auto Index = this->RetiringSimulations.Num() - 1; Index >= 0; --Index)
	{
		auto* Simulation = this->RetiringSimulations[Index].Get();
		if (Simulation->ReleaseFence->Poll() == false)
		{
			continue;
		}
		this->Budget.Release(0, 0, Simulation->ReservationBytes);
		this->GetAssetCache()->Release(Simulation);
		this->RetiringSimulations.RemoveAtSwap(Index);
	}
}

void UMeshParticleizerWorldSubsystem::ReclaimDeadParticles()
{
	for (const auto& Operation : this->ActiveOperations)
	{
		Operation->ReclaimDeadParticles();
	}
}

int32 UMeshParticleizerWorldSubsystem::CountSimulations(int64& OutMeshInstances, int64& OutMeshSections) const
{
	// 同frameの未描画分と待機・退役中も追加需要へ含め、開始順による過少予約を防ぐ。
	OutMeshInstances = 0;
	OutMeshSections = 0;
	auto Count = 0;
	auto Add = [&Count, &OutMeshInstances, &OutMeshSections](const UMeshParticleizerSimulation* Simulation) {
		if (Simulation == nullptr)
		{
			return;
		}
		++Count;
		OutMeshInstances += int64{Simulation->ParticleCount} * Simulation->MeshSections;
		OutMeshSections += Simulation->MeshSections;
	};
	for (const auto& Simulation : this->IdleSimulations)
	{
		Add(Simulation);
	}
	for (const auto& Simulation : this->RetiringSimulations)
	{
		Add(Simulation);
	}
	for (const auto* Operations : {&this->ActiveOperations, &this->ReleasingOperations})
	{
		for (const auto& Operation : *Operations)
		{
			for (const auto& Source : Operation->Sources)
			{
				Add(Source->Simulation);
			}
		}
	}
	return Count;
}

FMeshParticleizerBudgetUsage UMeshParticleizerWorldSubsystem::GetBudgetUsage() const
{
	auto Result = FMeshParticleizerBudgetUsage{};
	Result.Quality = this->Budget.Quality;
	Result.ParticleLimit = this->Budget.ParticleLimit;
	Result.TargetParticleLimit = this->Budget.TargetParticleLimit;
	Result.SourceLimit = this->Budget.SourceLimit;
	Result.RestingParticleLimit = FMeshParticleizerQualityLimits::Find(this->Budget.Quality)->RestingParticleLimit;
	if (this->GpuState.IsValid() == true)
	{
		const auto Lock = FScopeLock(&this->GpuState->Mutex);
		Result.bGpuCountsAvailable = this->GpuState->PublishedSerial > 0;
		Result.GpuCountsRenderFrame = this->GpuState->RenderFrame;
		Result.GpuUnseparatedParticles = this->GpuState->Counts[1];
		Result.GpuLiveParticles = this->GpuState->Counts[2] + this->GpuState->Counts[3] + this->GpuState->Counts[4];
		Result.GpuRestingParticles = this->GpuState->Counts[3];
		Result.GpuBudgetFadingParticles = this->GpuState->Counts[4];
		Result.GpuBudgetBufferBytes = this->GpuState->BufferBytes;
	}
	Result.ReservedParticles = this->Budget.ReservedParticles;
	Result.ReservedSources = this->Budget.ActiveSources;
	Result.ReleasingOperations = this->ReleasingOperations.Num();
	Result.SimulationMemoryLimitBytes = this->Budget.SimulationMemoryLimitBytes;
	Result.ReservedSimulationBytes = this->Budget.ReservedSimulationBytes;
	Result.SharedGpuReservationBytes = this->Budget.SharedGpuReservationBytes;
	Result.SceneGpuReservationBytes = this->SharedGpuMemory.GetSceneReservationBytes();
	Result.DrawGpuReservationBytes = this->SharedGpuMemory.GetDrawReservationBytes();
	Result.GdfReservationBytes = this->SharedGpuMemory.DistanceFieldBytes;
	Result.GdfTextureBytes = this->DistanceField.IsValid() ? this->DistanceField->TextureBytes.load() : 0;
	Result.GdfVoxelSizeCm = this->DistanceField.IsValid() ? this->DistanceField->VoxelSizeCm.load() : 0;
	Result.GdfStatus = FMeshParticleizerDistanceField::GetStatus(this->DistanceField.Get());
	Result.GdfDiagnostics = FMeshParticleizerDistanceField::DescribeStatus(Result.GdfStatus);
	if (Result.GdfStatus == EMeshParticleizerGdfStatus::Ready)
	{
		const auto RequiredShared = Result.GdfTextureBytes * 2;
		Result.GdfMinimumMemoryLimitBytes = Result.ReservedSimulationBytes + FMath::Max(int64{0}, RequiredShared - Result.GdfReservationBytes);
		Result.GdfDiagnostics += FString::Printf(TEXT(" Query textures: %lld bytes; GDF reservation including one retiring generation: %lld bytes. Minimum memory limit including current reservations: %lld bytes; current limit: %lld bytes. Additional particle and rendering buffers require extra capacity."),
			Result.GdfTextureBytes, RequiredShared, Result.GdfMinimumMemoryLimitBytes, Result.SimulationMemoryLimitBytes);
	}
	Result.PooledSimulations = this->IdleSimulations.Num();
	Result.RetiringSimulations = this->RetiringSimulations.Num();
	for (const auto& Simulation : this->IdleSimulations)
	{
		Result.PooledSimulationBytes += Simulation->ReservationBytes;
	}
	for (const auto& Simulation : this->RetiringSimulations)
	{
		Result.ReleasingSimulationBytes += Simulation->ReservationBytes;
	}
	for (const auto& Operation : this->ReleasingOperations)
	{
		Result.ReleasingParticles += Operation->ReservedParticles;
		Result.ReleasingSources += Operation->Sources.Num();
		Result.ReleasingSimulationBytes += Operation->SimulationReservationBytes;
	}
	return Result;
}

bool UMeshParticleizerWorldSubsystem::SetQuality(EMeshParticleizerQuality Quality, FString& OutError)
{
	OutError.Reset();
	const auto* Limits = FMeshParticleizerQualityLimits::Find(Quality);
	if (Limits == nullptr)
	{
		OutError = TEXT("Unknown Mesh Particleizer quality level.");
		return false;
	}
	this->ReclaimDeadParticles();

	// 全体の予約が少なくても、一つの対象だけが新しい上限を超える場合は切り替えない。
	for (const auto* Operations : {&this->ActiveOperations, &this->ReleasingOperations})
	{
		for (const auto& Operation : *Operations)
		{
			if (Operation->ReservedParticles > Limits->TargetParticleLimit)
			{
				OutError = TEXT("A current or releasing operation exceeds the target limit of this quality level.");
				return false;
			}
		}
	}
	if (this->Budget.SetQuality(Quality, OutError) == false)
	{
		return false;
	}
	while (this->IdleSimulations.Num() > this->Budget.SourceLimit)
	{
		this->EvictSimulation(0);
	}
	if (this->GpuState.IsValid() == true)
	{
		this->GpuState->RestingLimit.store(Limits->RestingParticleLimit);
	}
	return true;
}

TSharedRef<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> UMeshParticleizerWorldSubsystem::GetDistanceField()
{
	if (this->DistanceField.IsValid() == false)
	{
		this->DistanceField = FMeshParticleizerDistanceField::Observe(this->GetWorld()->Scene);
	}
	return this->DistanceField.ToSharedRef();
}

TSharedRef<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe> UMeshParticleizerWorldSubsystem::GetSceneGpu()
{
	if (this->SceneGpu.IsValid() == false)
	{
		this->SceneGpu = FMeshParticleizerSceneGpu::Observe(this->GetWorld()->Scene);
	}
	return this->SceneGpu.ToSharedRef();
}

TSharedRef<FMeshParticleizerGpuState, ESPMode::ThreadSafe> UMeshParticleizerWorldSubsystem::GetGpuState()
{
	if (this->GpuState.IsValid() == false)
	{
		this->GpuState = MakeShared<FMeshParticleizerGpuState, ESPMode::ThreadSafe>();
		this->GpuState->RestingLimit.store(FMeshParticleizerQualityLimits::Find(this->Budget.Quality)->RestingParticleLimit);
	}
	return this->GpuState.ToSharedRef();
}

bool UMeshParticleizerWorldSubsystem::SetSimulationMemoryLimit(int64 Bytes, FString& OutError)
{
	OutError.Reset();
	if (Bytes >= 0 && Bytes < this->Budget.SharedGpuReservationBytes)
	{
		OutError = FString::Printf(TEXT("Memory limit must include %lld bytes of shared Niagara GPU reservation retained until this world ends. Trimming the simulation pool does not release it."),
			this->Budget.SharedGpuReservationBytes);
		return false;
	}
	if (Bytes < 0 || Bytes < this->Budget.ReservedSimulationBytes)
	{
		OutError = TEXT("Memory limit must include active, pooled and pending GPU reservations. Trim the simulation pool and wait for release before lowering it further.");
		return false;
	}
	this->Budget.SimulationMemoryLimitBytes = Bytes;
	return true;
}

UMeshParticleizerAssetCache* UMeshParticleizerWorldSubsystem::GetAssetCache()
{
	if (this->AssetCache == nullptr)
	{
		this->AssetCache = NewObject<UMeshParticleizerAssetCache>(this);
	}
	return this->AssetCache;
}

UMeshParticleizerSurfaceTexture* UMeshParticleizerWorldSubsystem::RegisterSurfaceTexture(UTexture2D* Texture, FString& OutError)
{
	if (this->bShuttingDown == true)
	{
		OutError = TEXT("This world is shutting down.");
		return nullptr;
	}
	auto* Registration = NewObject<UMeshParticleizerSurfaceTexture>(this);
	return Registration->Initialize(this, Texture, OutError) ? Registration : nullptr;
}

bool UMeshParticleizerWorldSubsystem::PrepareCoverageNoise(bool& bReady, FString& OutError)
{
	bReady = false;
	OutError.Reset();
	if (this->bShuttingDown == true)
	{
		OutError = TEXT("This world is shutting down.");
		return false;
	}
	auto* Texture = GEngine != nullptr ? GEngine->BlueNoiseScalarTexture.Get() : nullptr;
	// UEの描画初期化を待つ。再生開始から同期ロードや別のノイズ確保を起こさない。
	if (Texture == nullptr || Texture->GetResource() == nullptr)
	{
		return true;
	}
	if (this->CoverageNoise == nullptr)
	{
		this->CoverageNoise = this->RegisterSurfaceTexture(Texture, OutError);
		if (this->CoverageNoise == nullptr)
		{
			return false;
		}
	}
	if (this->CoverageNoise->GetTexture() != Texture)
	{
		OutError = TEXT("UE's shared blue noise texture changed after reservation. Recreate this world before playback.");
		return false;
	}
	bReady = this->CoverageNoise->IsReady(OutError);
	return OutError.IsEmpty();
}

UMeshParticleizerSurfaceTexture* UMeshParticleizerWorldSubsystem::RegisterColorProvider(UTextureRenderTarget2D* Texture, FString& OutError, bool bPreserveHDR)
{
	if (this->bShuttingDown == true)
	{
		OutError = TEXT("This world is shutting down.");
		return nullptr;
	}
	auto* Registration = NewObject<UMeshParticleizerSurfaceTexture>(this);
	return Registration->InitializeProvider(this, Texture, OutError, bPreserveHDR) ? Registration : nullptr;
}

FMeshParticleizerAssetCacheUsage UMeshParticleizerWorldSubsystem::GetAssetCacheUsage()
{
	this->GetAssetCache()->Poll();
	return this->AssetCache->GetUsage();
}

bool UMeshParticleizerWorldSubsystem::SetAssetCacheLimit(int64 Bytes, FString& OutError)
{
	return this->GetAssetCache()->SetLimit(Bytes, OutError);
}

void UMeshParticleizerWorldSubsystem::TrimUnusedAssets()
{
	this->GetAssetCache()->TrimUnused();
}

void UMeshParticleizerWorldSubsystem::PostInitialize()
{
	Super::PostInitialize();

	// 最初の単発開始でも、描画済みのWorldなら待機なしで容量を判定できるよう観測を始める。
	if (this->GetWorld()->Scene != nullptr && this->GetWorld()->GetNetMode() != NM_DedicatedServer)
	{
		this->GetSceneGpu();
	}
}

void UMeshParticleizerWorldSubsystem::UpdateRenderReservation()
{
	if (this->SceneGpu.IsValid() == false || this->SharedGpuMemory.GetSceneReservationBytes() == 0)
	{
		return;
	}
	auto Draws = FMeshParticleizerDrawGpuSnapshot{};
	const auto Scene = this->SceneGpu->Read(&Draws);
	// callback間の途中のsnapshotは次frameで読む。通常再生から描画threadをFlushしない。
	if (Scene.Frame == 0 || Scene.Frame != Draws.Frame || Draws.Views <= 0)
	{
		return;
	}
	auto Required = this->SharedGpuMemory;
	auto Error = FString{};
	if (Required.GrowScene(Scene, 0, 0, 0, Error, Draws.Views) == true && Required.GrowDraw(Draws, Error) == true &&
		this->Budget.GrowSharedReservation(Required.GetReservationBytes()) == true)
	{
		this->SharedGpuMemory = MoveTemp(Required);
		return;
	}

	// 外部のScene変更でも未予約容量を使い続けない。解放待ちは従来のFence経路で保持する。
	const auto Operations = this->ActiveOperations;
	for (const auto& Operation : Operations)
	{
		if (Operation->SimulationReservationBytes <= 0)
		{
			continue;
		}
		Operation->Diagnostics += TEXT(" Shared rendering capacity became unsupported or exceeded the remaining GPU budget. Playback cancelled and mesh restored. ") + Error;
		Operation->Cancel(true);
	}
}

void UMeshParticleizerWorldSubsystem::Deinitialize()
{
	this->bShuttingDown = true;
	this->PendingColorProviders.Reset();
	const auto Snapshot = this->ActiveOperations;
	for (const auto& Operation : Snapshot)
	{
		Operation->ReleaseResources();
	}
	for (const auto& Operation : this->ReleasingOperations)
	{
		for (const auto& Source : Operation->Sources)
		{
			if (Source->Simulation != nullptr)
			{
				Source->Simulation->Destroy();
				Source->Simulation = nullptr;
			}
		}
	}
	for (const auto& Simulation : this->IdleSimulations)
	{
		Simulation->Destroy();
	}
	this->IdleSimulations.Reset();
	this->RetiringSimulations.Reset();
	for (auto& Entry : this->TrackedSources)
	{
		this->ReleaseTrackedPose(Entry.Value);
		if (auto* Source = Entry.Key.Get())
		{
			Source->TransformUpdated.RemoveAll(this);
		}
	}
	this->TrackedSources.Reset();

	// ワールド終了後は再割当しない。NiagaraとRHIが残る描画資源の破棄を引き継ぐ。
	this->ReleasingOperations.Reset();
	this->CoverageNoise = nullptr;
	if (this->AssetCache != nullptr)
	{
		this->AssetCache->Shutdown();
	}
	this->Budget = FMeshParticleizerBudget{};
	this->SharedGpuMemory = FMeshParticleizerSharedGpuMemory{};
	this->DistanceField.Reset();
	this->SceneGpu.Reset();
	Super::Deinitialize();
}

void UMeshParticleizerWorldSubsystem::AcquireSourceHistory(USceneComponent* Source)
{
	if (IsValid(Source) == false || Source->GetWorld() != this->GetWorld())
	{
		return;
	}
	auto& Tracked = this->TrackedSources.FindOrAdd(Source);
	if (Tracked.Users == 0)
	{
		this->CaptureSource(Source, Tracked);
		Source->TransformUpdated.AddUObject(this, &UMeshParticleizerWorldSubsystem::SourceTransformChanged);
	}
	++Tracked.Users;
}

void UMeshParticleizerWorldSubsystem::ReleaseSourceHistory(USceneComponent* Source)
{
	auto* Tracked = this->TrackedSources.Find(Source);
	if (Tracked == nullptr)
	{
		return;
	}
	if (--Tracked->Users == 0)
	{
		this->ReleaseTrackedPose(*Tracked);
		Source->TransformUpdated.RemoveAll(this);
		this->TrackedSources.Remove(Source);
	}
}

const FMeshParticleizerTransformHistory* UMeshParticleizerWorldSubsystem::FindSourceHistory(USceneComponent* Source) const
{
	const auto* Tracked = this->TrackedSources.Find(Source);
	return Tracked != nullptr ? &Tracked->History : nullptr;
}

void UMeshParticleizerWorldSubsystem::SourceTransformChanged(USceneComponent* Source, EUpdateTransformFlags Flags, ETeleportType Teleport)
{
	if (Teleport == ETeleportType::None)
	{
		return;
	}
	if (auto* Tracked = this->TrackedSources.Find(Source))
	{
		Tracked->History.Reset();
		Tracked->Bones.Reset();
	}
}

void UMeshParticleizerWorldSubsystem::CaptureSource(USceneComponent* Source, FTrackedSource& Tracked)
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, CaptureHistory);
	const auto Time = this->GetWorld()->GetTimeSeconds();
	Tracked.History.Capture(Source->GetComponentTransform(), Time, this->GetWorld()->OriginLocation);
	auto* Skinned = Cast<USkinnedMeshComponent>(Source);
	if (Skinned == nullptr)
	{
		return;
	}
	auto* Mesh = Skinned != nullptr ? Cast<USkeletalMesh>(Skinned->GetSkinnedAsset()) : nullptr;
	if (Mesh == nullptr)
	{
		Tracked.Bones.Reset();
		return;
	}
	auto* Leader = Skinned->LeaderPoseComponent.Get();
	auto* LeaderAsset = Leader != nullptr ? Leader->GetSkinnedAsset() : nullptr;
	if (Tracked.CapturedAsset != Mesh || Tracked.CapturedLeader != Leader || Tracked.CapturedLeaderAsset != LeaderAsset)
	{
		// 別の姿勢供給元との間で速度差分を作らない。
		Tracked.Bones.Reset();
		Tracked.CapturedAsset = Mesh;
		Tracked.CapturedLeader = Leader;
		Tracked.CapturedLeaderAsset = LeaderAsset;
	}
	if (Skinned->GetPredictedLODLevel() != 0 ||
		(Tracked.PoseUsers > 0 && Tracked.PoseLeader != Skinned->LeaderPoseComponent))
	{
		Tracked.Bones.Reset();
		return;
	}

	const auto Revision = Skinned->GetBoneTransformRevisionNumber();
	if (Leader == nullptr)
	{
		Tracked.Bones.Capture(Skinned->GetComponentSpaceTransforms(), Mesh->GetRefBasesInvMatrix(), Revision, Time);
		return;
	}
	const auto* RenderData = Mesh->GetResourceForRendering();
	if (LeaderAsset == nullptr || Leader->GetWorld() != this->GetWorld() || Leader->GetPredictedLODLevel() != 0 ||
		Leader->GetNumComponentSpaceTransforms() < LeaderAsset->GetRefBasesInvMatrix().Num() ||
		Skinned->GetLeaderBoneMap().Num() != Mesh->GetRefSkeleton().GetNum() ||
		RenderData == nullptr || RenderData->LODRenderData.IsEmpty())
	{
		Tracked.Bones.Reset();
		return;
	}

	// UEの描画用対応表を使い、部品固有の参照姿勢と欠落ボーンの扱いを一致させる。
	// 更新されていない姿勢の行列を毎フレーム作り直さない。
	if (Tracked.Bones.bHasCurrent == true && Tracked.Bones.CurrentRevision == Revision && Time >= Tracked.Bones.CurrentTime)
	{
		return;
	}
	Skinned->GetCurrentRefToLocalMatrices(Tracked.SkinningMatrices, 0);
	Tracked.Bones.CaptureMatrices(Tracked.SkinningMatrices, Revision, Time);
}

const FMeshParticleizerBoneHistory* UMeshParticleizerWorldSubsystem::FindBoneHistory(USceneComponent* Source) const
{
	const auto* Tracked = this->TrackedSources.Find(Source);
	return Tracked != nullptr ? &Tracked->Bones : nullptr;
}

bool UMeshParticleizerWorldSubsystem::IsBonePoseReady(USkinnedMeshComponent* Source, int32 BoneCount) const
{
	if (IsValid(Source) == false || Source->GetPredictedLODLevel() != 0)
	{
		return false;
	}
	const auto* Tracked = this->TrackedSources.Find(Source);
	auto* Leader = Source->LeaderPoseComponent.Get();
	return Tracked != nullptr && Tracked->Bones.bHasCurrent == true && Tracked->Bones.Current.Num() == BoneCount * 3 &&
		Tracked->CapturedAsset == Source->GetSkinnedAsset() && Tracked->CapturedLeader == Source->LeaderPoseComponent &&
		(Leader == nullptr || (Leader->GetPredictedLODLevel() == 0 && Tracked->CapturedLeaderAsset == Leader->GetSkinnedAsset()));
}

void UMeshParticleizerWorldSubsystem::AcquireFullPose(USceneComponent* Source)
{
	auto* Skinned = Cast<USkinnedMeshComponent>(Source);
	auto* Tracked = this->TrackedSources.Find(Source);
	if (Skinned == nullptr || Tracked == nullptr)
	{
		return;
	}
	if (Tracked->PoseUsers++ != 0)
	{
		return;
	}

	// 同じ本体を複数の衣服が参照しても、最後の利用者が離れるまで姿勢を維持する。
	Tracked->PoseComponent = Skinned;
	Tracked->PoseLeader = Skinned->LeaderPoseComponent;
	this->AcquirePoseLease(Skinned);
	if (auto* Leader = Tracked->PoseLeader.Get())
	{
		this->AcquirePoseLease(Leader);
	}
}

void UMeshParticleizerWorldSubsystem::ReleaseFullPose(USceneComponent* Source)
{
	auto* Tracked = this->TrackedSources.Find(Source);
	if (Tracked == nullptr || Tracked->PoseUsers == 0 || --Tracked->PoseUsers != 0)
	{
		return;
	}
	this->ReleaseTrackedPose(*Tracked);
}

void UMeshParticleizerWorldSubsystem::ReleaseTrackedPose(FTrackedSource& Tracked)
{
	this->ReleasePoseLease(Tracked.PoseComponent);
	this->ReleasePoseLease(Tracked.PoseLeader);
	Tracked.PoseComponent.Reset();
	Tracked.PoseLeader.Reset();
	Tracked.PoseUsers = 0;
}

void UMeshParticleizerWorldSubsystem::AcquirePoseLease(USkinnedMeshComponent* Source)
{
	if (IsValid(Source) == false)
	{
		return;
	}
	auto& Lease = this->FullPoseLeases.FindOrAdd(Source);
	if (Lease.Users++ != 0)
	{
		return;
	}
	Lease.OriginalLOD = Source->GetForcedLOD();
	Lease.OriginalVisibility = Source->VisibilityBasedAnimTickOption;
	Source->SetForcedLOD(1);
	Source->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
}

void UMeshParticleizerWorldSubsystem::ReleasePoseLease(TWeakObjectPtr<USkinnedMeshComponent> Source)
{
	auto* Lease = this->FullPoseLeases.Find(Source);
	if (Lease == nullptr || --Lease->Users != 0)
	{
		return;
	}

	if (auto* Skinned = Source.Get())
	{
		if (Skinned->GetForcedLOD() == 1)
		{
			Skinned->SetForcedLOD(Lease->OriginalLOD);
		}
		if (Skinned->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones)
		{
			Skinned->VisibilityBasedAnimTickOption = Lease->OriginalVisibility;
		}
	}
	this->FullPoseLeases.Remove(Source);
}
