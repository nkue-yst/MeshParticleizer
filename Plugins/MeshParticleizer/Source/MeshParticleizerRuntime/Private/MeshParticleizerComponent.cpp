#include "MeshParticleizerComponent.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerPreset.h"
#include "MeshParticleizerOriginLibrary.h"
#include "MeshParticleizerParticleStyle.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "MeshParticleizerAssetCache.h"
#include "Components/MeshComponent.h"
#include "Curves/CurveFloat.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"

UMeshParticleizerComponent::UMeshParticleizerComponent()
{
	this->PrimaryComponentTick.bCanEverTick = false;
	this->PrimaryComponentTick.bStartWithTickEnabled = false;
}

TArray<FMeshParticleizerTarget> UMeshParticleizerComponent::ResolveTargets() const
{
	if (this->ConfiguredTargets.IsEmpty() == false)
	{
		return this->ConfiguredTargets;
	}

	auto Entry = FMeshParticleizerTarget{};
	Entry.Mesh = Cast<UMeshComponent>(this->Target.GetComponent(this->GetOwner()));
	Entry.PreparedData = this->PreparedData;
	return {Entry};
}

FMeshParticleizerHitOrigin UMeshParticleizerComponent::CaptureRegisteredHitOrigin(const FHitResult& Hit,
	bool bProjectToSurface, double MaxProjectionDistance) const
{
	auto Result = FMeshParticleizerHitOrigin{};
	if (IsInGameThread() == false || IsValid(Hit.GetComponent()) == false)
	{
		Result.Diagnostics = TEXT("Capture a hit on a registered source mesh on the game thread after pose update.");
		return Result;
	}

	// 本体以外への命中も、命中した登録部品の準備データへ対応させる。
	for (const auto& Entry : this->ResolveTargets())
	{
		if (Entry.Mesh.Get() == Hit.GetComponent())
		{
			return bProjectToSurface == true
				? UMeshParticleizerOriginLibrary::CaptureSurfaceHitOrigin(Hit, Entry.PreparedData.Get(), MaxProjectionDistance)
				: UMeshParticleizerOriginLibrary::CaptureHitOrigin(Hit);
		}
	}

	Result.Diagnostics = TEXT("The hit mesh is not registered with this Mesh Particleizer Component.");
	return Result;
}

void UMeshParticleizerComponent::BeginPlay()
{
	Super::BeginPlay();
	this->StartTracking();
}

void UMeshParticleizerComponent::StartTracking()
{
	if (this->GetWorld() == nullptr || this->GetWorld()->IsGameWorld() == false || this->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	auto Sources = TArray<TWeakObjectPtr<UMeshComponent>>{};
	for (const auto& Entry : this->ResolveTargets())
	{
		Sources.Add(Entry.Mesh);
	}
	if (Sources == this->TrackedSources)
	{
		return;
	}

	// 登録先が変わったときだけ、旧対象の姿勢・速度履歴を解放する。
	this->ReleaseFullPose();
	auto* Manager = this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>();
	for (const auto& Previous : this->TrackedSources)
	{
		if (auto* Source = Previous.Get())
		{
			Manager->ReleaseSourceHistory(Source);
		}
	}

	// 新しい対象の履歴を取得し、分離時の速度に使えるようにする。
	this->TrackedSources = MoveTemp(Sources);
	for (const auto& Entry : this->TrackedSources)
	{
		if (auto* Source = Entry.Get())
		{
			Manager->AcquireSourceHistory(Source);
		}
	}
}

bool UMeshParticleizerComponent::Configure(UMeshComponent* Source,
	TSoftObjectPtr<UMeshParticleizerPreparedMesh> Prepared, FString& OutError)
{
	auto Entry = FMeshParticleizerTarget{};
	Entry.Mesh = Source;
	Entry.PreparedData = Prepared;
	return this->ConfigureTargets({Entry}, OutError);
}

bool UMeshParticleizerComponent::ConfigureTargets(const TArray<FMeshParticleizerTarget>& Sources, FString& OutError)
{
	if (this->State == EMeshParticleizerPreparationState::Preparing ||
		(this->LastOperation != nullptr && this->LastOperation->GetState() != EMeshParticleizeState::Finished &&
			this->LastOperation->GetState() != EMeshParticleizeState::Cancelled))
	{
		OutError = TEXT("Configure valid targets while idle.");
		return false;
	}

	if (FMeshParticleizerTarget::ValidateTargets(Sources, this->GetWorld(), OutError) == false)
	{
		return false;
	}

	this->ReleasePreparedOperation();
	this->ReleaseFullPose();
	this->ConfiguredTargets = Sources;
	this->PreparedData = Sources[0].PreparedData;
	this->StartTracking();

	// 対象の変更で古くなったロード結果と準備状態を破棄する。
	this->DataLoad.Reset();
	this->ReleaseResourceRegistration();
	this->State = EMeshParticleizerPreparationState::Unprepared;
	OutError.Reset();
	return true;
}

bool UMeshParticleizerComponent::SetOptions(const FMeshParticleizeOptions& Settings, FString& OutError)
{
	if (Settings.Validate(OutError) == false)
	{
		return false;
	}
	const auto bCurveChanged = Settings.ParticleStyle != this->Options.ParticleStyle || Settings.ProgressCurve != this->Options.ProgressCurve || Settings.Collision != this->Options.Collision ||
		Settings.Motion != this->Options.Motion || (Settings.ParticleLifetime > 0) != (this->Options.ParticleLifetime > 0) ||
		(Settings.MaxParticles > 0) != (this->Options.MaxParticles > 0);
	if (bCurveChanged == true && this->State == EMeshParticleizerPreparationState::Preparing)
	{
		OutError = TEXT("Restore Mesh to cancel preparation before changing preparation-dependent settings.");
		return false;
	}
	if (bCurveChanged == true && this->State == EMeshParticleizerPreparationState::Ready)
	{
		this->ReleasePreparedOperation();
		this->ReleaseFullPose();
		this->State = EMeshParticleizerPreparationState::Unprepared;
	}
	this->Options = Settings;
	OutError.Reset();
	return true;
}

bool UMeshParticleizerComponent::ApplyPreset(UMeshParticleizerPreset* Preset, FString& OutError)
{
	if (Preset == nullptr)
	{
		OutError = TEXT("Select a particleizer preset.");
		return false;
	}
	auto Settings = FMeshParticleizeOptions{};
	return Preset->CopyOptions(Settings, OutError) && this->SetOptions(Settings, OutError);
}

void UMeshParticleizerComponent::PrepareForParticleize()
{
	if (this->State == EMeshParticleizerPreparationState::Preparing)
	{
		return;
	}
	if (this->GetWorld() == nullptr || this->GetWorld()->IsGameWorld() == false || this->GetNetMode() == NM_DedicatedServer)
	{
		this->FailPreparation(TEXT("Preparation requires a rendering game world."));
		return;
	}
	auto Paths = TArray<FSoftObjectPath>{};
	auto Error = FString{};
	if (this->Options.Validate(Error) == false)
	{
		this->FailPreparation(Error);
		return;
	}
	if (this->Options.ProgressCurve.IsNull() == false)
	{
		Paths.Add(this->Options.ProgressCurve.ToSoftObjectPath());
	}
	if (this->Options.ParticleStyle.IsNull() == false)
	{
		Paths.Add(this->Options.ParticleStyle.ToSoftObjectPath());
	}
	for (const auto& Entry : this->ResolveTargets())
	{
		if (Entry.Mesh.IsValid() == false || Entry.PreparedData.IsNull() == true)
		{
			this->FailPreparation(TEXT("Every registered target requires a valid source and prepared data."));
			return;
		}
		Paths.AddUnique(Entry.PreparedData.ToSoftObjectPath());
	}

	// 非同期ロードを始める前に、スキン付き対象の全ボーン姿勢を確保する。
	this->State = EMeshParticleizerPreparationState::Preparing;
	this->Diagnostics = TEXT("Loading prepared data and particle resources.");
	this->StartTracking();
	if (this->bHoldsFullPose == false)
	{
		auto* Manager = this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>();
		for (const auto& Source : this->TrackedSources)
		{
			Manager->AcquireFullPose(Source.Get());
		}
		this->bHoldsFullPose = true;
	}

	this->PreparationDeadline = FPlatformTime::Seconds() + 30;
	this->DataLoad = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths,
		FStreamableDelegate::CreateUObject(this, &UMeshParticleizerComponent::LoadResources));
}

void UMeshParticleizerComponent::LoadResources()
{
	if (this->State != EMeshParticleizerPreparationState::Preparing)
	{
		return;
	}

	// スタイルと各対象の依存資産を検査し、共有キャッシュにまとめて登録する。
	auto Resources = TArray<FMeshParticleizerResource>{};
	if (this->Options.ParticleStyle.IsNull() == false)
	{
		auto* Style = this->Options.ParticleStyle.Get();
		auto Error = FString{};
		if (Style == nullptr || Style->ValidateResourceManifest(Error) == false)
		{
			this->FailPreparation(Style == nullptr ? TEXT("Particle Style could not be loaded.") : Error);
			return;
		}
		Resources.Append(Style->Resources);
	}

	for (const auto& Entry : this->ResolveTargets())
	{
		auto* Data = Entry.PreparedData.Get();
		if (Data == nullptr || Data->DataVersion != UMeshParticleizerPreparedMesh::CurrentDataVersion || Data->Resources.IsEmpty())
		{
			this->FailPreparation(TEXT("Prepared data or its particle system reference is unavailable. Prepare every target again."));
			return;
		}
		auto Error = FString{};
		if (Data->ValidateResourceManifest(Error) == false)
		{
			this->FailPreparation(Error);
			return;
		}
		Resources.Append(Data->Resources);
	}

	auto Error = FString{};
	if (this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>()->GetAssetCache()->Acquire(this, Resources, Error) == false)
	{
		this->FailPreparation(Error);
		return;
	}
	this->CheckResources();
}

void UMeshParticleizerComponent::CheckResources()
{
	if (this->State != EMeshParticleizerPreparationState::Preparing)
	{
		return;
	}
	auto* World = this->GetWorld();
	if (World == nullptr)
	{
		this->FailPreparation(TEXT("Source world is unavailable."));
		return;
	}
	auto* Manager = World->GetSubsystem<UMeshParticleizerWorldSubsystem>();
	auto CacheError = FString{};
	if (Manager->GetAssetCache()->IsReady(this, CacheError) == false)
	{
		if (CacheError.IsEmpty() == false)
		{
			this->FailPreparation(CacheError);
			return;
		}
		if (FPlatformTime::Seconds() >= this->PreparationDeadline)
		{
			this->FailPreparation(TEXT("Shared resource loading timed out."));
			return;
		}
		World->GetTimerManager().SetTimer(this->ReadinessTimer, this, &UMeshParticleizerComponent::CheckResources, 0.05f, false);
		return;
	}
	const auto Targets = this->ResolveTargets();
	auto bAllReady = true;
	auto Systems = TSet<UNiagaraSystem*>{};
	for (auto Index = 0; Index < Targets.Num(); ++Index)
	{
		const auto& Entry = Targets[Index];
		auto* Data = Entry.PreparedData.Get();
		auto* System = Data != nullptr ? Data->ParticleSystem.Get() : nullptr;
		if (Entry.Mesh.IsValid() == false || Data == nullptr || System == nullptr || Data->Positions.IsValid() == false ||
			Data->Normals.IsValid() == false || Data->Colors.IsValid() == false ||
			(Data->BoneCount > 0 && (Data->Coordinates.IsValid() == false || Data->SkinVertices.IsValid() == false || Data->SkinTriangles.IsValid() == false)))
		{
			this->FailPreparation(FString::Printf(TEXT("Source %d or its particle resources could not be loaded."), Index));
			return;
		}

		// 同じシステムを共有する部品のコンパイル確認は一度にまとめる。
#if WITH_EDITOR
		if (Systems.Contains(System) == false)
		{
			System->PollForCompilationComplete();
		}
#endif
		Systems.Add(System);
		const auto bPoseReady = Data->BoneCount == 0 || Manager->IsBonePoseReady(Cast<USkinnedMeshComponent>(Entry.Mesh.Get()), Data->BoneCount);
		if (System->IsReadyToRun() == false || bPoseReady == false)
		{
			bAllReady = false;
			this->Diagnostics = FString::Printf(TEXT("Waiting for source %d: %s."), Index,
				bPoseReady ? TEXT("particle system compilation") : TEXT("complete LOD0 bone pose"));
		}
		else if (System->IsValid() == false)
		{
			this->FailPreparation(FString::Printf(TEXT("Source %d particle system GPU compilation failed."), Index));
			return;
		}
	}
	if (bAllReady == false)
	{
		if (FPlatformTime::Seconds() >= this->PreparationDeadline)
		{
			this->FailPreparation(TEXT("Preparation timed out after 30 seconds. ") + this->Diagnostics);
			return;
		}
		World->GetTimerManager().SetTimer(this->ReadinessTimer, this, &UMeshParticleizerComponent::CheckResources, 0.05f, false);
		return;
	}

	auto Error = FString{};
	if (this->PreparedOperation == nullptr)
	{
		this->PreparedOperation = Manager->PrepareParticleizeTargets(Targets, Error, &this->Options);
	}
	else if (this->PreparedOperation->ValidateTargets(Targets, Error) == false)
	{
		this->FailPreparation(Error);
		return;
	}
	else if (this->PreparedOperation->PrepareTiming(this->Options, Error) == false)
	{
		this->FailPreparation(Error);
		return;
	}
	if (this->PreparedOperation == nullptr)
	{
		this->FailPreparation(Error);
		return;
	}
	auto bCollisionReady = false;
	if (this->PreparedOperation->PrepareRendering(this->Options, false, bCollisionReady, Error) == false)
	{
		this->FailPreparation(Error);
		return;
	}
	if (bCollisionReady == false)
	{
		if (FPlatformTime::Seconds() >= this->PreparationDeadline)
		{
			this->FailPreparation(TEXT("Rendering preparation timed out. Check material shader compilation and the selected collision view resources."));
			return;
		}
		this->Diagnostics = TEXT("Waiting for particle/material shaders and selected collision resources.");
		World->GetTimerManager().SetTimer(this->ReadinessTimer, this, &UMeshParticleizerComponent::CheckResources, 0.05f, false);
		return;
	}
	// 準備結果を公開してから通知する。受信側はその場で再生を開始できる。
	this->ReleaseResourceRegistration();
	this->State = EMeshParticleizerPreparationState::Ready;
	this->Diagnostics = TEXT("Ready");
	this->OnPrepared.Broadcast();
}

UMeshParticleizeOperation* UMeshParticleizerComponent::StartParticleize(FString& OutError)
{
	if (this->State != EMeshParticleizerPreparationState::Ready || this->PreparedOperation == nullptr || this->GetWorld() == nullptr)
	{
		OutError = TEXT("Call Prepare For Particleize and wait for On Prepared before starting.");
		return nullptr;
	}

	auto* Manager = this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>();
	auto* Operation = Manager->StartPreparedParticleize(this->PreparedOperation, this->Options, OutError);

	// 再開始が拒否されても、直前の表示状態を復元するハンドルを失わない。
	if (Operation != nullptr)
	{
		this->LastOperation = Operation;
		if (OutError.IsEmpty() == true)
		{
			this->PreparedOperation = nullptr;
			this->ReleaseFullPose();

			// 開始へ渡した準備は消費済み。次回もGDF除外や描画資源の待機を通常の準備で確認する。
			this->State = EMeshParticleizerPreparationState::Unprepared;
			this->Diagnostics = TEXT("Preparation consumed by playback. Prepare again before another start; the async Start Particleize node does this automatically.");
		}
	}
	else if (this->PreparedOperation != nullptr)
	{
		this->ReleasePreparedOperation();
		this->ReleaseFullPose();
		this->State = EMeshParticleizerPreparationState::Unprepared;
		this->Diagnostics = OutError;
	}
	return Operation;
}

bool UMeshParticleizerComponent::SetSurfaceColorParameters(int32 SourceIndex, int32 MaterialSlot,
	const FMeshParticleizerSurfaceColorParameters& Parameters, FString& OutError)
{
	auto* Operation = this->PreparedOperation != nullptr ? this->PreparedOperation.Get() : this->LastOperation.Get();
	if (Operation == nullptr)
	{
		OutError = TEXT("Prepare the source before setting its surface color.");
		return false;
	}
	return Operation->SetSurfaceColorParameters(SourceIndex, MaterialSlot, Parameters, OutError);
}

void UMeshParticleizerComponent::RestoreMesh()
{
	const auto bWasPreparing = this->State == EMeshParticleizerPreparationState::Preparing;
	if (this->PreparedOperation != nullptr || this->State == EMeshParticleizerPreparationState::Preparing)
	{
		this->State = EMeshParticleizerPreparationState::Unprepared;
		if (this->GetWorld() != nullptr)
		{
			this->GetWorld()->GetTimerManager().ClearTimer(this->ReadinessTimer);
		}
		if (this->DataLoad.IsValid() == true)
		{
			this->DataLoad->CancelHandle();
		}
		this->ReleaseResourceRegistration();
		this->ReleasePreparedOperation();
		this->ReleaseFullPose();
	}

	if (this->LastOperation != nullptr)
	{
		this->LastOperation->RestoreMesh();
	}

	if (bWasPreparing == true)
	{
		this->Diagnostics = TEXT("Preparation cancelled by Restore Mesh.");
		this->OnPreparationFailed.Broadcast(this->Diagnostics);
	}
}

void UMeshParticleizerComponent::ReleasePreparedOperation()
{
	if (this->PreparedOperation == nullptr)
	{
		return;
	}
	this->PreparedOperation->RestoreSourceMaterials();
	this->PreparedOperation->ReleaseResources();
	this->PreparedOperation = nullptr;
}

void UMeshParticleizerComponent::FailPreparation(const FString& Error)
{
	this->Diagnostics = Error;
	this->ReleasePreparedOperation();
	this->ReleaseFullPose();
	this->ReleaseResourceRegistration();
	this->State = EMeshParticleizerPreparationState::Failed;
	this->OnPreparationFailed.Broadcast(Error);
}

void UMeshParticleizerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	this->ReleasePreparedOperation();
	this->ReleaseFullPose();
	const auto bWasPreparing = this->State == EMeshParticleizerPreparationState::Preparing;
	this->State = EMeshParticleizerPreparationState::Unprepared;
	if (this->GetWorld() != nullptr)
	{
		this->GetWorld()->GetTimerManager().ClearTimer(this->ReadinessTimer);
	}
	if (this->DataLoad.IsValid() == true)
	{
		this->DataLoad->CancelHandle();
	}
	this->ReleaseResourceRegistration();
	if (auto* Manager = this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>())
	{
		for (const auto& Entry : this->TrackedSources)
		{
			if (auto* Source = Entry.Get())
			{
				Manager->ReleaseSourceHistory(Source);
			}
		}
	}
	this->TrackedSources.Reset();

	// 分離済みの演出は元Actorの寿命へ巻き戻さない。
	this->DataLoad.Reset();
	Super::EndPlay(EndPlayReason);
	if (bWasPreparing == true)
	{
		this->OnPreparationFailed.Broadcast(TEXT("Source ended play during preparation."));
	}
}

void UMeshParticleizerComponent::ReleaseResourceRegistration()
{
	if (this->GetWorld() == nullptr)
	{
		return;
	}
	if (auto* Manager = this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>())
	{
		Manager->GetAssetCache()->Release(this);
	}
}

void UMeshParticleizerComponent::ReleaseFullPose()
{
	if (this->bHoldsFullPose == false)
	{
		return;
	}
	this->bHoldsFullPose = false;
	if (this->GetWorld() == nullptr)
	{
		return;
	}
	if (auto* Manager = this->GetWorld()->GetSubsystem<UMeshParticleizerWorldSubsystem>())
	{
		for (const auto& Entry : this->TrackedSources)
		{
			if (auto* Source = Entry.Get())
			{
				Manager->ReleaseFullPose(Source);
			}
		}
	}
}
