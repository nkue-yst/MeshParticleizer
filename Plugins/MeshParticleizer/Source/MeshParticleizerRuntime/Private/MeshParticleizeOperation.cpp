#include "MeshParticleizeOperation.h"
#include "MeshParticleizeSource.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerParticleStyle.h"
#include "MeshParticleizerCoverageGpu.h"
#include "Materials/Material.h"
#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerDistanceField.h"
#include "MeshParticleizerSceneGpu.h"
#include "NiagaraDataInterfaceMeshParticleizerCollision.h"
#include "Components/StaticMeshComponent.h"
#include "MeshParticleizerGpuState.h"
#include "MeshParticleizerSimulation.h"
#include "MeshParticleizerAssetCache.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "Components/MeshComponent.h"
#include "Curves/CurveFloat.h"
#include "NiagaraSystem.h"
#include "Engine/World.h"

void UMeshParticleizeOperation::GetParticleizeStatus(EMeshParticleizeState& OutState,
	float& OutProgress, float& OutRemainingTime) const
{
	OutState = this->State;
	OutProgress = this->Progress;
	OutRemainingTime = 0;
	auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	if (Manager == nullptr || Manager->GetWorld() == nullptr || this->State == EMeshParticleizeState::Finished ||
		this->State == EMeshParticleizeState::Cancelled || this->State == EMeshParticleizeState::Failed)
	{
		return;
	}

	OutRemainingTime = static_cast<float>(FMath::Max(0.0, this->FinishTime - Manager->GetWorld()->GetTimeSeconds()));
}

bool UMeshParticleizeOperation::ValidateTargets(const TArray<FMeshParticleizerTarget>& InTargets, FString& OutError) const
{
	if (InTargets.IsEmpty() == true || InTargets.Num() != this->Sources.Num())
	{
		OutError = TEXT("Register at least one source and prepare the whole group together.");
		return false;
	}
	const auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	if (FMeshParticleizerTarget::ValidateTargets(InTargets, Manager != nullptr ? Manager->GetWorld() : nullptr, OutError) == false)
	{
		return false;
	}

	for (auto Index = 0; Index < InTargets.Num(); ++Index)
	{
		const auto& Target = InTargets[Index];
		auto* Mesh = Target.Mesh.Get();
		if (this->Sources[Index]->Source != Target.Mesh || this->Sources[Index]->PreparedMesh != Target.PreparedData.Get())
		{
			OutError = TEXT("Prepared source bindings changed. Configure and prepare the complete group again.");
			return false;
		}
		if (this->Sources[Index]->ValidateSource(Mesh, Target.PreparedData.Get(), OutError) == false)
		{
			return false;
		}
	}
	return true;
}

bool UMeshParticleizeOperation::SetSurfaceColorParameters(int32 SourceIndex, int32 MaterialSlot,
	const FMeshParticleizerSurfaceColorParameters& Parameters, FString& OutError)
{
	OutError.Reset();
	if (this->Sources.IsValidIndex(SourceIndex) == false || this->bConverted == true ||
		(this->State != EMeshParticleizeState::Ready && this->State != EMeshParticleizeState::Converting))
	{
		OutError = TEXT("Surface color requires a prepared source that is ready or still converting.");
		return false;
	}
	return this->Sources[SourceIndex]->SetSurfaceColorParameters(MaterialSlot, Parameters, this->bStarted, OutError);
}

bool UMeshParticleizeOperation::SetTargets(const TArray<FMeshParticleizerTarget>& InTargets, FString& OutError)
{
	if (this->Sources.IsEmpty() == false)
	{
		return this->ValidateTargets(InTargets, OutError);
	}
	for (const auto& Target : InTargets)
	{
		auto* Source = NewObject<UMeshParticleizeSource>(this);
		Source->Source = Target.Mesh;
		Source->PreparedMesh = Target.PreparedData.Get();
		this->Sources.Add(Source);
	}
	if (this->ValidateTargets(InTargets, OutError) == false)
	{
		this->Sources.Reset();
		return false;
	}
	this->Targets = InTargets;
	auto Resources = TArray<FMeshParticleizerResource>{};
	for (const auto& Target : InTargets)
	{
		Resources.Append(Target.PreparedData.Get()->Resources);
	}
	if (this->PreparedStyle != nullptr)
	{
		Resources.Append(this->PreparedStyle->Resources);
	}
	auto* Cache = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->GetAssetCache();
	if (Cache->Acquire(this, Resources, OutError) == false || Cache->IsReady(this, OutError) == false)
	{
		Cache->Release(this);
		this->Sources.Reset();
		this->Targets.Reset();
		if (OutError.IsEmpty() == true)
		{
			OutError = TEXT("Shared assets are still loading or initializing GPU resources. Prepare the complete group before starting.");
		}
		return false;
	}
	return true;
}

bool UMeshParticleizeOperation::PrepareTargets(const TArray<FMeshParticleizerTarget>& InTargets, FString& OutError)
{
	// 全対象の検証を済ませてから材質を一括で準備する。
	if (this->SetTargets(InTargets, OutError) == false)
	{
		return false;
	}
	for (const auto& Source : this->Sources)
	{
		if (Source->PrepareMaterials(Source->Source.Get(), Source->PreparedMesh, OutError) == false)
		{
			this->RestoreSourceMaterials();
			this->ReleaseResources();
			return false;
		}
	}
	return true;
}

bool UMeshParticleizeOperation::PrepareTiming(const FMeshParticleizeOptions& InOptions, FString& OutError)
{
	auto Curve = TSharedPtr<const FMeshParticleizerProgressCurve>{};
	if (InOptions.Validate(OutError) == false ||
		FMeshParticleizerProgressCurve::Compile(InOptions.ProgressCurve, Curve, OutError) == false)
	{
		return false;
	}
	this->Schedule.Curve = MoveTemp(Curve);
	this->PreparedCurvePath = InOptions.ProgressCurve.ToSoftObjectPath();
	this->bTimingPrepared = true;
	return true;
}

bool UMeshParticleizeOperation::PrepareStyle(const FMeshParticleizeOptions& InOptions, FString& OutError)
{
	auto* Style = InOptions.ParticleStyle.Get();
	if (InOptions.ParticleStyle.IsNull() == false && Style == nullptr)
	{
		OutError = TEXT("Particle Style is not loaded. Prepare For Particleize before starting.");
		return false;
	}
	if (this->Sources.IsEmpty() == false && Style != this->PreparedStyle)
	{
		OutError = TEXT("Restore and prepare again after changing Particle Style.");
		return false;
	}
	if (Style != nullptr && Style->ValidateResourceManifest(OutError) == false)
	{
		return false;
	}

	this->PreparedStyle = Style;
	return true;
}

bool UMeshParticleizeOperation::PrepareRendering(const FMeshParticleizeOptions& InOptions, bool bStarting, bool& bReady, FString& OutError)
{
	bReady = false;
	if (this->bCollisionPrepared == true && this->PreparedCollision != InOptions.Collision)
	{
		OutError = TEXT("Restore and prepare again after changing the collision mode.");
		return false;
	}
	this->PreparedCollision = InOptions.Collision;
	const auto bNeedsCollision = InOptions.Motion != EMeshParticleizerMotion::Drift &&
		InOptions.ParticleLifetime > 0 && InOptions.MaxParticles > 0;
	auto bUseGdf = bNeedsCollision && InOptions.Collision != EMeshParticleizerCollision::SimpleGround;
	auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	auto Field = TSharedPtr<FMeshParticleizerDistanceField, ESPMode::ThreadSafe>{};
	if (bUseGdf == true)
	{
		Field = Manager->GetDistanceField();
		const auto Status = FMeshParticleizerDistanceField::GetStatus(Field.Get());
		if (Status == EMeshParticleizerGdfStatus::WaitingForRender)
		{
			return true;
		}
		if (Status != EMeshParticleizerGdfStatus::Ready)
		{
			if (InOptions.Collision == EMeshParticleizerCollision::GlobalDistanceField)
			{
				OutError = TEXT("GDF collision unavailable. ") + FMeshParticleizerDistanceField::DescribeStatus(Status);
				return false;
			}
			bUseGdf = false;
			this->CollisionDiagnostics = TEXT("Auto selected Simple Ground: ") + FMeshParticleizerDistanceField::DescribeStatus(Status) + TEXT(" ");
		}
	}

	// 全部品の専用システムを確かめてから、元メッシュの距離場参加を一括で止める。
	for (const auto& Source : this->Sources)
	{
		auto* System = this->PreparedStyle != nullptr
			? (bUseGdf ? this->PreparedStyle->GdfParticleSystem.Get() : this->PreparedStyle->ParticleSystem.Get())
			: (bUseGdf ? Source->PreparedMesh->GdfParticleSystem.Get() : Source->PreparedMesh->ParticleSystem.Get());
		if (System == nullptr)
		{
			OutError = TEXT("The selected collision system is not loaded. Upgrade and prepare the dependencies again.");
			return false;
		}
#if WITH_EDITOR
		System->PollForCompilationComplete();
#endif
		if (System->IsReadyToRun() == false)
		{
			return true;
		}
		if (System->IsValid() == false || (bUseGdf == true && System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCollision::StaticClass()), TEXT("User.Collision"))) == INDEX_NONE))
		{
			OutError = TEXT("The GDF system is invalid or its collision input was replaced. Upgrade the generated system.");
			return false;
		}
		Source->PlaybackSystem = System;
		// Providerは準備完了後に設定する。開始時は描画待機・GPU割当前に入力を検査する。
		if (bStarting == true && Source->ValidateStart(InOptions, OutError) == false)
		{
			return false;
		}
		for (auto Slot = 0; Slot < Source->PreparedMesh->TextureColorSources.Num(); ++Slot)
		{
			if (Source->PreparedMesh->TextureColorSources[Slot].bRequiresRuntimeCoverage == false)
			{
				continue;
			}
			if (Source->DissolveMaterials[Slot]->GetMaterial()->DitherOpacityMask == true)
			{
				auto bNoiseReady = false;
				if (Manager->PrepareCoverageNoise(bNoiseReady, OutError) == false)
				{
					return false;
				}
				if (bNoiseReady == false)
				{
					return true;
				}
			}
			auto bCoverageReady = false;
			if (PrepareMeshParticleizerCoverageMaterial(Source->DissolveMaterials[Slot], bCoverageReady, OutError) == false)
			{
				return false;
			}
			if (bCoverageReady == false)
			{
				return true;
			}
		}
	}
	this->ResolvedCollision = bUseGdf ? EMeshParticleizerCollision::GlobalDistanceField : EMeshParticleizerCollision::SimpleGround;
	if (bUseGdf == true)
	{
		if (this->CollisionReadyFrame == 0)
		{
			for (const auto& Source : this->Sources)
			{
				auto* Mesh = Source->Source.Get();
				if (Mesh != nullptr && Mesh->bAffectDistanceFieldLighting == true)
				{
					Source->bDistanceFieldOverridden = true;
					Mesh->SetAffectDistanceFieldLighting(false);
				}
			}
			// UE 5.8の段階更新は最大6クリップマップ。最も遅い更新周期とProxy反映分を待つ。
			// 全面更新を強制せず、シーン全体の描画設定も変更しない。
			this->CollisionReadyFrame = Field->Frame.load() + (uint64{1} << FMath::Clamp(Field->Clipmaps.load() - 1, 0, 5)) + 3;
		}
		if (Field->Frame.load() < this->CollisionReadyFrame)
		{
			return true;
		}
		this->CollisionDiagnostics = FString::Printf(TEXT("GDF collision selected; nearest grid cell %.2f cm. Thin surfaces and gaps below the field resolution may be missed. Shared query textures and one retiring generation require reservation. "), Field->VoxelSizeCm.load());
	}
	else
	{
		for (const auto& Source : this->Sources)
		{
			Source->RestoreDistanceField();
		}
		this->CollisionReadyFrame = 0;
	}
	// 描画情報を非同期で待つ。準備だけでは共通GPU容量を予約しない。
	if (InOptions.ParticleLifetime > 0 && InOptions.MaxParticles > 0 &&
		(Manager->Budget.ReservedSimulationBytes < Manager->Budget.SimulationMemoryLimitBytes || Manager->IdleSimulations.IsEmpty() == false))
	{
		auto Draws = FMeshParticleizerDrawGpuSnapshot{};
		const auto Scene = Manager->GetSceneGpu()->Read(&Draws);
		if (Scene.Frame == 0 || Scene.Frame != Draws.Frame || Draws.Views <= 0)
		{
			return true;
		}
		if (Draws.bSupportedLayout == false)
		{
			OutError = TEXT("Rendering capacity requires deferred shading and non-immediate RDG without uniform-buffer-view instance culling.");
			return false;
		}
	}
	this->bCollisionPrepared = true;
	bReady = true;
	return true;
}

bool UMeshParticleizeOperation::Initialize(const TArray<FMeshParticleizerTarget>& InTargets,
	const FMeshParticleizeOptions& InOptions, FString& OutError)
{
	if (InOptions.Validate(OutError) == false || this->PrepareStyle(InOptions, OutError) == false ||
		this->SetTargets(InTargets, OutError) == false)
	{
		return false;
	}
	if ((this->bTimingPrepared == false || this->PreparedCurvePath != InOptions.ProgressCurve.ToSoftObjectPath()) &&
		this->PrepareTiming(InOptions, OutError) == false)
	{
		return false;
	}
	auto bCollisionReady = false;
	auto CollisionOptions = InOptions;
	if (this->bCollisionPrepared == false && InOptions.Collision == EMeshParticleizerCollision::Auto)
	{
		CollisionOptions.Collision = EMeshParticleizerCollision::SimpleGround;
		this->CollisionDiagnostics = TEXT("Auto selected Simple Ground: source exclusion was not prepared before this synchronous start. ");
	}
	if (this->PrepareRendering(CollisionOptions, true, bCollisionReady, OutError) == false)
	{
		return false;
	}
	if (bCollisionReady == false)
	{
		OutError = TEXT("Rendering preparation is pending. Use Prepare For Particleize and wait for OnPrepared before starting.");
		return false;
	}

	auto Allocation = TArray<FMeshParticleizerAllocation>{};
	auto ResolvedAllocations = TArray<FMeshParticleizerSourceAllocation>{};
	auto WeightSum = 0.0;
	auto Capacity = int64{0};
	for (auto Index = 0; Index < this->Sources.Num(); ++Index)
	{
		const auto& Source = this->Sources[Index];
		auto Resolved = FMeshParticleizerSourceAllocation{};
		Resolved.StartScale = Source->Source->GetComponentScale();
		if (Source->PreparedMesh->GetScaledSurfaceArea(Resolved.StartScale, Resolved.ScaledArea, OutError) == false)
		{
			OutError = FString::Printf(TEXT("Source %d (%s): %s"), Index, *Source->Source->GetName(), *OutError);
			return false;
		}
		Resolved.ReferenceArea = Source->PreparedMesh->SurfaceArea;
		Resolved.Importance = InTargets[Index].Importance;
		Resolved.CandidateLimit = Source->PreparedMesh->SampleCount;
		const auto Weight = FMath::Max(Resolved.ScaledArea, 0.000001) * Resolved.Importance;
		WeightSum += Weight;
		if (FMath::IsFinite(WeightSum) == false)
		{
			OutError = FString::Printf(TEXT("Source %d (%s): scaled area and importance exceed the supported numeric range."), Index, *Source->Source->GetName());
			return false;
		}
		Allocation.Add({Weight, Resolved.CandidateLimit});
		ResolvedAllocations.Add(Resolved);
		Capacity += Source->PreparedMesh->SampleCount;
	}

	auto ProgressSpaces = TArray<FMeshParticleizerProgressSpace>{};
	if (FMeshParticleizerProgressSpace::Resolve(InTargets, InOptions.bShareOrigin, ProgressSpaces, OutError) == false)
	{
		return false;
	}
	auto ResolvedOrigins = TArray<FMeshParticleizerResolvedOrigin>{};
	if (FMeshParticleizerResolvedOrigin::Resolve(InTargets, InOptions, ProgressSpaces, ResolvedOrigins, OutError) == false)
	{
		return false;
	}

	// 粒子数と容量の両方を開始前に決める。未分離候補も全寿命分のバッファを持つ。
	auto* Manager = Cast<UMeshParticleizerWorldSubsystem>(this->GetOuter());
	const auto Requested = static_cast<int32>(FMath::Min<int64>(InOptions.MaxParticles, Capacity));
	const auto* QualityLimits = FMeshParticleizerQualityLimits::Find(InOptions.Quality);
	const auto TargetLimit = FMath::Min(QualityLimits->TargetParticleLimit, Manager->Budget.TargetParticleLimit);
	auto Candidate = FMath::Min3(Requested, TargetLimit, FMath::Max(0, Manager->Budget.ParticleLimit - Manager->Budget.ReservedParticles));
	const auto AvailableBytes = Manager->Budget.SimulationMemoryLimitBytes - Manager->Budget.ReservedSimulationBytes;
	const auto bNoGpuCapacity = InOptions.ParticleLifetime > 0 && Candidate > 0 &&
		AvailableBytes == 0 && Manager->IdleSimulations.IsEmpty();
	if (bNoGpuCapacity == true)
	{
		Candidate = 0;
	}
	auto Memory = TArray<FMeshParticleizerGpuMemory>{};
	for (const auto& Source : this->Sources)
	{
		auto Model = FMeshParticleizerGpuMemory{};
		if (InOptions.ParticleLifetime > 0 && Candidate > 0 &&
			FMeshParticleizerGpuMemory::Build(Source->PlaybackSystem.Get(), Source->PreparedMesh->BoneCount,
				this->Schedule.Curve.IsValid() ? this->Schedule.Curve->GetSegments().Num() : 0,
				Source->PreparedMesh->SourceMaterials.Num(), Model, OutError) == false)
		{
			return false;
		}
		Model.bSurfaceColorCache = Source->PreparedMesh->NeedsSurfaceColorCache();
		Model.bSourceEmissionCache = InOptions.bInheritSourceEmission && Source->PreparedMesh->bSupportsEmissionProvider;
		Model.bSurfaceCoverage = Source->PreparedMesh->NeedsRuntimeCoverage();
		Memory.Add(Model);
	}
	auto DistributeAndMeasure = [&Allocation, &Memory, &InOptions](int32 Count) {
		FMeshParticleizerAllocation::Distribute(Count, Allocation);
		auto Bytes = int64{0};
		if (InOptions.ParticleLifetime <= 0)
		{
			return Bytes;
		}
		for (auto Index = 0; Index < Allocation.Num(); ++Index)
		{
			Bytes += Memory[Index].Estimate(Allocation[Index].Granted);
		}
		return Bytes;
	};
	auto Bytes = DistributeAndMeasure(Candidate);
	auto Reused = TArray<UMeshParticleizerSimulation*>{};
	const auto CurveVectors = this->Schedule.Curve.IsValid() ? this->Schedule.Curve->GetSegments().Num() : 0;
	auto MatchPool = [&SourceList = this->Sources, &IdlePool = Manager->IdleSimulations, &Allocation, &Memory, &Reused, CurveVectors]() {
		Reused.Init(nullptr, SourceList.Num());
		auto Credit = int64{0};
		for (auto Index = 0; Index < SourceList.Num(); ++Index)
		{
			const auto* Prepared = SourceList[Index]->PreparedMesh.Get();
			for (const auto& Simulation : IdlePool)
			{
				if (Reused.Contains(Simulation.Get()) == false && Simulation->Matches(SourceList[Index]->PlaybackSystem.Get(), Allocation[Index].Granted, Prepared->BoneCount, CurveVectors, Memory[Index].Estimate(Allocation[Index].Granted), Memory[Index].bSurfaceColorCache, Memory[Index].bSourceEmissionCache, Memory[Index].bSurfaceCoverage, Prepared->SourceMaterials.Num()))
				{
					Reused[Index] = Simulation;
					Credit += Simulation->ReservationBytes;
					break;
				}
			}
		}
		return Credit;
	};
	auto Credit = InOptions.ParticleLifetime > 0 ? MatchPool() : 0;
	auto ExistingMeshInstances = int64{0};
	auto ExistingMeshSections = int64{0};
	const auto ExistingSimulations = Manager->CountSimulations(ExistingMeshInstances, ExistingMeshSections);
	const auto SharedSettings = FMeshParticleizerSharedGpuSettings::Read();
	const auto PreviousShared = Manager->SharedGpuMemory;
	auto Shared = PreviousShared;
	auto Draws = FMeshParticleizerDrawGpuSnapshot{};
	const auto Scene = Manager->GetSceneGpu()->Read(&Draws);
	const auto GdfBytes = this->ResolvedCollision == EMeshParticleizerCollision::GlobalDistanceField
		? Manager->GetDistanceField()->TextureBytes.load()
		: 0;
	auto MeasureShared = [&Allocation, &Reused, &Shared, &PreviousShared, &SharedSettings, &OutError, &InOptions, &Scene, &Draws, &Memory, ExistingSimulations, ExistingMeshInstances, ExistingMeshSections, GdfBytes](bool bReuse) {
		Shared = PreviousShared;
		if (InOptions.ParticleLifetime <= 0)
		{
			return true;
		}
		auto Added = 0;
		auto MeshInstances = ExistingMeshInstances;
		auto MeshSections = ExistingMeshSections;
		auto bUsingGpu = false;
		for (auto Index = 0; Index < Allocation.Num(); ++Index)
		{
			if (Allocation[Index].Granted <= 0)
			{
				continue;
			}
			bUsingGpu = true;
			if (bReuse == false || Reused.IsValidIndex(Index) == false || Reused[Index] == nullptr)
			{
				++Added;
				MeshInstances += int64{Allocation[Index].Granted} * Memory[Index].MeshSections;
				MeshSections += Memory[Index].MeshSections;
			}
		}
		if (bUsingGpu == false)
		{
			return true;
		}
		// 同じシーンの問い合わせ資源は一度だけ予約し、更新中の旧世代1組も含める。
		Shared.DistanceFieldBytes = FMath::Max(Shared.DistanceFieldBytes, GdfBytes * 2);
		if (Shared.Grow(ExistingSimulations + Added, SharedSettings, OutError) == false)
		{
			return false;
		}

		// Niagaraと姿勢固定用proxyの追加余地を、同frameの未反映・待機中の分も含めて取る。
		// 現行Spriteはinstance-cullingのmesh drawを追加しないが、Scene共通maskは増え得る。
		const auto ComponentsPerView = int64{ExistingSimulations + Added} * 2;
		const auto ScenePrimitives = (ComponentsPerView + MeshSections) * Draws.Views;
		const auto SceneInstances = (ComponentsPerView + MeshInstances) * Draws.Views;
		auto Predicted = Draws;
		if (Scene.Frame != Draws.Frame)
		{
			OutError = TEXT("Rendering snapshot changed during preparation. Retry after the next rendering frame.");
			return false;
		}
		if (Shared.GrowScene(Scene, ScenePrimitives, SceneInstances, SceneInstances * Scene.MeshPayloadStride, OutError, Draws.Views) == false ||
			Predicted.AddMeshDemand(ComponentsPerView, 0, {}, OutError) == false)
		{
			return false;
		}
		const FName MeshPasses[] = {TEXT("DepthPass"), TEXT("BasePass"), TEXT("Velocity")};
		if (MeshSections > 0 && Predicted.AddMeshDemand(MeshInstances, MeshSections, MeshPasses, OutError) == false)
		{
			return false;
		}
		return Shared.GrowDraw(Predicted, OutError);
	};
	if (MeasureShared(true) == false)
	{
		return false;
	}
	const auto SharedIncrease = Shared.GetReservationBytes() - Manager->Budget.SharedGpuReservationBytes;
	const auto bMemoryReduced = bNoGpuCapacity || Bytes - Credit + SharedIncrease > AvailableBytes;
	if (bMemoryReduced == true)
	{
		// 一致しない待機容量は古い順に回収する。Fence前の容量を今回の空きへ加えない。
		auto PendingBytes = int64{0};
		for (auto Index = 0; Index < Manager->IdleSimulations.Num() && Bytes - Credit + SharedIncrease > AvailableBytes + PendingBytes;)
		{
			auto* Simulation = Manager->IdleSimulations[Index].Get();
			if (Reused.Contains(Simulation) == true)
			{
				++Index;
				continue;
			}
			PendingBytes += Simulation->ReservationBytes;
			Manager->EvictSimulation(Index);
		}
		auto Fits = 0;
		auto TooLarge = Candidate;
		while (Fits + 1 < TooLarge)
		{
			const auto Trial = Fits + (TooLarge - Fits) / 2;
			const auto TrialBytes = DistributeAndMeasure(Trial);
			if (MeasureShared(false) == false)
			{
				return false;
			}
			if (TrialBytes + Shared.GetReservationBytes() - Manager->Budget.SharedGpuReservationBytes <= AvailableBytes)
			{
				Fits = Trial;
			}
			else
			{
				TooLarge = Trial;
			}
		}
		// 部品間の端数配分は単調とは限らない。収まると確認した候補だけを採用する。
		Candidate = Fits;
		Bytes = DistributeAndMeasure(Candidate);
		Credit = MatchPool();
		if (MeasureShared(true) == false)
		{
			return false;
		}
	}

	// グループ全体を原子的に予約し、途中で一部だけ開始する状態を作らない。
	if (Manager->Budget.Reserve(Candidate, this->GrantedParticles, this->Sources.Num(), Bytes - Credit, Shared.GetReservationBytes()) == false)
	{
		OutError = TEXT("Source or simulation memory budget, including pending GPU releases, cannot accommodate this entire group.");
		return false;
	}
	this->bReserved = true;
	Manager->SharedGpuMemory = Shared;
	if (GdfBytes > 0)
	{
		Manager->GetDistanceField()->ReservedBytes.store(Shared.DistanceFieldBytes);
		Manager->GetDistanceField()->Fault.store(false);
	}
	this->ReservedParticles = this->GrantedParticles;
	this->SimulationReservationBytes = Bytes;
	for (auto Index = 0; Index < this->Sources.Num(); ++Index)
	{
		const auto SourceBytes = InOptions.ParticleLifetime > 0 ? Memory[Index].Estimate(Allocation[Index].Granted) : 0;
		if (SourceBytes == 0)
		{
			continue;
		}
		auto* Simulation = Reused.IsValidIndex(Index) ? Reused[Index] : nullptr;
		if (Simulation != nullptr)
		{
			Manager->IdleSimulations.Remove(Simulation);
		}
		else
		{
			Simulation = NewObject<UMeshParticleizerSimulation>(Manager);
			Simulation->ReservationBytes = SourceBytes;
			Simulation->ParticleCount = Allocation[Index].Granted;
			Simulation->MeshSections = Memory[Index].MeshSections;
			Simulation->BoneCount = this->Sources[Index]->PreparedMesh->BoneCount;
			Simulation->CurveVectorCount = CurveVectors;
			Simulation->bSurfaceColorCache = Memory[Index].bSurfaceColorCache;
			Simulation->bSourceEmissionCache = Memory[Index].bSourceEmissionCache;
			Simulation->bSurfaceCoverage = Memory[Index].bSurfaceCoverage;
			Simulation->CoverageMaterialSlots = Memory[Index].CoverageMaterialSlots;
		}
		this->Sources[Index]->Simulation = Simulation;
	}
	this->Options = InOptions;
	this->Schedule = {Manager->GetWorld()->GetTimeSeconds(), InOptions.ConversionDuration, InOptions.ParticleLifetime, this->Schedule.Curve};
	this->FinishTime = this->Schedule.StartTime + InOptions.ConversionDuration + InOptions.ParticleLifetime;
	this->Diagnostics = this->GrantedParticles < InOptions.MaxParticles ? TEXT("Reduced quality: group particle allocation reduced. ") : TEXT("");
	this->Diagnostics += this->CollisionDiagnostics;
	if (Capacity < InOptions.MaxParticles)
	{
		this->Diagnostics += TEXT("Prepared samples limit the requested density. ");
	}
	if (Requested > TargetLimit)
	{
		this->Diagnostics += TEXT("The target limit of the requested or world quality level reduced density. ");
	}
	if (bMemoryReduced == true)
	{
		this->Diagnostics += TEXT("Simulation GPU memory limit reduced the allocation before start. ");
	}
	if (bMemoryReduced == true && GdfBytes > 0)
	{
		this->Diagnostics += TEXT("GDF memory requirement: ") + Manager->GetBudgetUsage().GdfDiagnostics + TEXT(" ");
	}
	for (auto Index = 0; Index < ResolvedOrigins.Num(); ++Index)
	{
		if ((InOptions.bShareOrigin == false || Index == 0) && ResolvedOrigins[Index].Diagnostics.IsEmpty() == false)
		{
			this->Diagnostics += ResolvedOrigins[Index].Diagnostics + TEXT(" ");
		}
	}

	auto SourceOptions = InOptions;
	SourceOptions.Collision = this->ResolvedCollision;
	for (auto Index = 0; Index < this->Sources.Num(); ++Index)
	{
		auto* Source = this->Sources[Index].Get();
		if (Source->PrepareMaterials(Source->Source.Get(), Source->PreparedMesh, OutError) == false ||
			Source->Initialize(SourceOptions, this->Schedule, ProgressSpaces[Index], ResolvedOrigins[Index], InTargets, Allocation[Index].Granted, OutError) == false)
		{
			this->RestoreSourceMaterials();
			this->ReleaseResources();
			this->State = EMeshParticleizeState::Failed;
			return false;
		}
		this->Diagnostics += Source->Diagnostics;
	}

	// 成功した開始の結果だけを公開し、終了・中断後も照合できるよう保持する。
	for (auto Index = 0; Index < ResolvedAllocations.Num(); ++Index)
	{
		ResolvedAllocations[Index].bResolved = true;
		ResolvedAllocations[Index].GrantedParticles = Allocation[Index].Granted;
	}
	this->SourceAllocations = MoveTemp(ResolvedAllocations);

	// 全部品の表示とGPU入力が揃ってから再生する。通知は次のワールド更新から行う。
	for (const auto& Source : this->Sources)
	{
		Source->Activate();
	}
	this->StartResult.bSucceeded = true;
	this->StartResult.bReducedQuality = this->GrantedParticles < InOptions.MaxParticles;
	this->StartResult.AppliedQuality = static_cast<EMeshParticleizerQuality>(FMath::Min(static_cast<uint8>(InOptions.Quality), static_cast<uint8>(Manager->Budget.Quality)));
	this->StartResult.AppliedCollision = this->ResolvedCollision;
	this->StartResult.RequestedParticles = InOptions.MaxParticles;
	this->StartResult.GrantedParticles = this->GrantedParticles;
	return true;
}

FMeshParticleizeResult UMeshParticleizeOperation::GetResult() const
{
	auto Snapshot = this->StartResult;
	Snapshot.Diagnostics = this->Diagnostics;
	return Snapshot;
}

bool UMeshParticleizeOperation::ContainsSource(UMeshComponent* Mesh) const
{
	for (const auto& Source : this->Sources)
	{
		if (Source->Source.Get() == Mesh)
		{
			return true;
		}
	}
	return false;
}

int32 UMeshParticleizeOperation::GetGrantedParticlesForSource(UMeshComponent* Mesh) const
{
	for (const auto& Source : this->Sources)
	{
		if (Source->Source.Get() == Mesh)
		{
			return Source->GrantedParticles;
		}
	}
	return 0;
}

FMeshParticleizerSourceAllocation UMeshParticleizeOperation::GetSourceAllocation(int32 SourceIndex) const
{
	if (this->SourceAllocations.IsValidIndex(SourceIndex) == true)
	{
		return this->SourceAllocations[SourceIndex];
	}

	auto Result = FMeshParticleizerSourceAllocation{};
	Result.Diagnostics = this->Sources.IsValidIndex(SourceIndex) == true
		? TEXT("Source allocation is available after a successful start.")
		: TEXT("Source index is outside this operation's registered group.");
	return Result;
}

FMeshParticleizerResolvedGround UMeshParticleizeOperation::GetResolvedGround(int32 SourceIndex) const
{
	if (this->Sources.IsValidIndex(SourceIndex) == true)
	{
		return this->Sources[SourceIndex]->ResolvedGround;
	}

	auto Result = FMeshParticleizerResolvedGround{};
	Result.Diagnostics = TEXT("Source index is outside this operation's registered group.");
	return Result;
}

FMeshParticleizerResolvedOrigin UMeshParticleizeOperation::GetResolvedOrigin(int32 SourceIndex) const
{
	if (this->Sources.IsValidIndex(SourceIndex) == true)
	{
		return this->Sources[SourceIndex]->ResolvedOrigin;
	}
	auto Result = FMeshParticleizerResolvedOrigin{};
	Result.Diagnostics = TEXT("Source index is outside this operation's registered group.");
	return Result;
}

#if WITH_EDITOR
UMeshComponent* UMeshParticleizeOperation::GetDisplayMeshForEditor(int32 SourceIndex) const
{
	if (this->Sources.IsValidIndex(SourceIndex) == false)
	{
		return nullptr;
	}
	const auto* Source = this->Sources[SourceIndex].Get();
	return Source->FrozenMesh != nullptr ? Source->FrozenMesh.Get() : Source->Source.Get();
}

UNiagaraComponent* UMeshParticleizeOperation::GetParticlesForEditor(int32 SourceIndex) const
{
	return this->Sources.IsValidIndex(SourceIndex) ? this->Sources[SourceIndex]->Particles.Get() : nullptr;
}
#endif

void UMeshParticleizeOperation::Advance(double Now)
{
	if (this->State == EMeshParticleizeState::Finished || this->State == EMeshParticleizeState::Cancelled)
	{
		return;
	}
	if (this->Sources.ContainsByPredicate([](const auto& Source) {
			return Source->GpuAllocation.IsValid() && Source->GpuAllocation->bCoverageFault.load();
		}))
	{
		this->Diagnostics += TEXT(" Surface coverage became unavailable or exceeded its reservation. Playback cancelled and mesh restored.");
		this->Cancel(true);
		return;
	}
	if (this->ResolvedCollision == EMeshParticleizerCollision::GlobalDistanceField && this->GrantedParticles > 0)
	{
		const auto Field = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->GetDistanceField();
		const auto bSourceReenabled = this->Sources.ContainsByPredicate([](const auto& Source) {
			return Source->Source.IsValid() == true && Source->Source->bAffectDistanceFieldLighting == true;
		});
		if (Field->Fault.load() == true || FMeshParticleizerDistanceField::IsEnabled() == false || bSourceReenabled == true)
		{
			this->Diagnostics += TEXT(" GDF became unavailable, exceeded its reservation, or a source rejoined the field. Playback cancelled and mesh restored.");
			this->Cancel(true);
			return;
		}
	}
	if (this->bStarted == false)
	{
		this->bStarted = true;
		this->State = EMeshParticleizeState::Converting;
		this->OnStarted.Broadcast(this);
		if (this->State == EMeshParticleizeState::Cancelled)
		{
			return;
		}
	}

	if (this->bConverted == false && this->State != EMeshParticleizeState::Draining)
	{
		for (const auto& Source : this->Sources)
		{
			auto* Mesh = Source->Source.Get();
			auto SourceError = FString{};
			if (Source->IsSourceAvailable(Mesh, SourceError) == true)
			{
				continue;
			}

			this->State = EMeshParticleizeState::Draining;
			this->FinishReason = Mesh == nullptr ? EMeshParticleizeFinishReason::SourceLost : EMeshParticleizeFinishReason::SourceChanged;
			this->Diagnostics += Mesh == nullptr ? TEXT(" A registered source was destroyed; detached particles continue.")
												 : TEXT(" A registered source mesh, pose or material changed; new detachment stopped.");
			if (SourceError.IsEmpty() == false)
			{
				this->Diagnostics += TEXT(" ") + SourceError;
			}
			this->FinishTime = FMath::Min(this->FinishTime, Now + this->Options.ParticleLifetime);
			for (const auto& Part : this->Sources)
			{
				Part->StopDetachment();
			}
			break;
		}
	}
	const auto bReadSource = this->bConverted == false && this->State != EMeshParticleizeState::Draining;
	if (bReadSource == true)
	{
		this->Progress = this->Schedule.Progress(Now);
	}
	for (const auto& Source : this->Sources)
	{
		Source->Update(Now, this->Progress, bReadSource);
	}

	// 全部品の最終姿勢と消失を反映してから一度だけ通知する。通知内で元Actorを破棄できる。
	if (bReadSource == true && this->Schedule.IsConverted(Now))
	{
		this->bConverted = true;
		this->State = EMeshParticleizeState::Converted;
		for (const auto& Source : this->Sources)
		{
			Source->StopReading();
		}
		this->OnMeshConverted.Broadcast(this);
		if (this->State == EMeshParticleizeState::Cancelled)
		{
			return;
		}
	}
	if (Now >= this->FinishTime)
	{
		this->Finish(this->FinishReason);
	}
}

void UMeshParticleizeOperation::RestoreMesh()
{
	if (this->State == EMeshParticleizeState::Ready || this->State == EMeshParticleizeState::Converting ||
		this->State == EMeshParticleizeState::Draining)
	{
		this->Cancel(true);
		return;
	}
	this->RestoreSourceMaterials();
}

void UMeshParticleizeOperation::RestoreSourceMaterials()
{
	for (const auto& Source : this->Sources)
	{
		Source->RestoreShadowCache();
		Source->RestoreSourceMaterials();
	}
}

void UMeshParticleizeOperation::Cancel(bool bRestoreMesh)
{
	if (this->State == EMeshParticleizeState::Finished || this->State == EMeshParticleizeState::Cancelled)
	{
		return;
	}
	this->State = EMeshParticleizeState::Cancelled;
	for (const auto& Source : this->Sources)
	{
		Source->StopReading();
	}
	if (bRestoreMesh == true)
	{
		this->RestoreSourceMaterials();
	}
	this->OnCancelled.Broadcast(this);
	this->Finish(EMeshParticleizeFinishReason::Cancelled);
}

void UMeshParticleizeOperation::Finish(EMeshParticleizeFinishReason Reason)
{
	if (Reason != EMeshParticleizeFinishReason::Cancelled)
	{
		this->State = EMeshParticleizeState::Finished;
	}
	this->ReleaseResources();
	this->OnFinished.Broadcast(this, Reason);
}

void UMeshParticleizeOperation::ReleaseResources()
{
	this->ReclaimDeadParticles();
	auto bWaitForGpu = false;
	for (const auto& Source : this->Sources)
	{
		bWaitForGpu |= Source->ReleaseResources();
	}
	if (this->bReserved == true)
	{
		this->bReserved = false;
		Cast<UMeshParticleizerWorldSubsystem>(this->GetOuter())->Release(this, bWaitForGpu);
	}
	else if (this->ReleaseFence.IsValid() == false)
	{
		this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->GetAssetCache()->Release(this);
	}
}

void UMeshParticleizeOperation::ReclaimDeadParticles()
{
	if (this->bReserved == false)
	{
		return;
	}

	// 累積死亡数はこの再生に属するGPUフェンス完了後の値。フェード中は含まれない。
	auto Dead = 0;
	for (const auto& Source : this->Sources)
	{
		if (Source->GpuAllocation.IsValid() == true)
		{
			Dead += Source->GpuAllocation->DeadParticles.load();
		}
	}
	const auto Remaining = this->GrantedParticles - Dead;
	if (Remaining >= this->ReservedParticles)
	{
		return;
	}

	// 同じ報告を重ねて受けても差分だけを返す。ソースとGPU容量は解放完了まで保持する。
	this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->Budget.Release(this->ReservedParticles - Remaining, 0, 0);
	this->ReservedParticles = Remaining;
}
