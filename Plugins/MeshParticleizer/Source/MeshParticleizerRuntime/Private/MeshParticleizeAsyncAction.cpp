#include "MeshParticleizeAsyncAction.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerPreset.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "MeshParticleizerComponent.h"
#include "Components/MeshComponent.h"

namespace
{
	bool ResolvePlaybackSettings(const FMeshParticleizeOptions& Defaults, UMeshParticleizerPreset* Preset,
		float Duration, float Lifetime, const FMeshParticleizerOriginOverride& Origin,
		FMeshParticleizeOptions& OutSettings, FString& OutError)
	{
		auto Settings = Defaults;
		if (Preset != nullptr && Preset->CopyOptions(Settings, OutError) == false)
		{
			return false;
		}
		if (FMath::IsFinite(Duration) == false || FMath::IsFinite(Lifetime) == false ||
			(Duration < 0 && Duration != -1) || (Lifetime < 0 && Lifetime != -1))
		{
			OutError = TEXT("Times must be finite, non-negative seconds, or -1 to keep the preset/component value.");
			return false;
		}

		// 時間と起点だけをコピーへ上書きし、共有Presetは編集しない。
		if (Duration != -1)
		{
			Settings.ConversionDuration = Duration;
		}
		if (Lifetime != -1)
		{
			Settings.ParticleLifetime = Lifetime;
		}
		if (Origin.bOverride == true)
		{
			Settings.Origin = Origin.Mode;
			Settings.bShareOrigin = Origin.bShareOrigin;
			Settings.Direction = Origin.Direction;
			Settings.bDirectionInWorldSpace = Origin.bDirectionInWorldSpace;
			Settings.LocalOrigin = Origin.LocalOrigin;
			Settings.LocalOrigins = Origin.LocalOrigins;
			Settings.OriginBoneOrSocket = Origin.BoneOrSocket;
			Settings.HitOrigin = Origin.Hit;
			Settings.FallbackBone = Origin.FallbackBone;
		}
		if (Settings.Validate(OutError) == false)
		{
			return false;
		}
		OutSettings = MoveTemp(Settings);
		return true;
	}
}

UMeshParticleizePrepareAsyncAction* UMeshParticleizePrepareAsyncAction::PrepareForParticleize(
	UMeshParticleizerComponent* Component, UMeshComponent* Mesh,
	TSoftObjectPtr<UMeshParticleizerPreparedMesh> PreparedData, UMeshParticleizerPreset* Preset)
{
	auto* Action = NewObject<UMeshParticleizePrepareAsyncAction>();
	Action->Preparation = Component;
	Action->Target.Mesh = Mesh;
	Action->Target.PreparedData = PreparedData;
	Action->bConfigureTarget = Mesh != nullptr || PreparedData.IsNull() == false;
	if (IsValid(Component) == true)
	{
		Action->Settings = Component->Options;
		Action->RegisterWithGameInstance(Component);
	}
	else
	{
		Action->SetupError = TEXT("Select a registered particleizer component.");
	}
	if (Action->SetupError.IsEmpty() == true && Preset != nullptr)
	{
		Preset->CopyOptions(Action->Settings, Action->SetupError);
	}
	return Action;
}

void UMeshParticleizePrepareAsyncAction::Activate()
{
	if (this->bActivated == true)
	{
		return;
	}
	this->bActivated = true;
	if (this->SetupError.IsEmpty() == false)
	{
		this->HandleFailed(this->SetupError);
		return;
	}
	if (IsValid(this->Preparation) == false || this->Preparation->IsRegistered() == false ||
		this->Preparation->GetPreparationState() == EMeshParticleizerPreparationState::Preparing)
	{
		this->HandleFailed(TEXT("Use a registered component that is not already preparing."));
		return;
	}

	// 不正なPresetや不完全な対象指定では、既存の準備と登録を変更しない。
	auto Error = FString{};
	if (this->Settings.Validate(Error) == false)
	{
		this->HandleFailed(Error);
		return;
	}
	auto* Existing = this->Preparation->GetLastOperation();
	if (Existing != nullptr && Existing->GetState() != EMeshParticleizeState::Finished &&
		Existing->GetState() != EMeshParticleizeState::Cancelled && Existing->GetState() != EMeshParticleizeState::Failed)
	{
		this->HandleFailed(TEXT("Restore or cancel the active operation before preparing this component again."));
		return;
	}
	if (this->bConfigureTarget == true && this->Preparation->ConfigureTargets({this->Target}, Error) == false)
	{
		this->HandleFailed(Error);
		return;
	}
	if (this->Preparation->SetOptions(this->Settings, Error) == false)
	{
		this->HandleFailed(Error);
		return;
	}

	this->Preparation->OnPrepared.AddDynamic(this, &UMeshParticleizePrepareAsyncAction::HandlePrepared);
	this->Preparation->OnPreparationFailed.AddDynamic(this, &UMeshParticleizePrepareAsyncAction::HandleFailed);
	if (this->Preparation->GetPreparationState() == EMeshParticleizerPreparationState::Ready)
	{
		this->HandlePrepared();
	}
	else
	{
		this->Preparation->PrepareForParticleize();
	}
}

void UMeshParticleizePrepareAsyncAction::HandlePrepared()
{
	auto* Component = this->Preparation.Get();
	const auto Diagnostics = Component->GetPreparationDiagnostics();
	this->ReleaseBindings();
	this->Prepared.Broadcast(Component, Diagnostics);
	this->SetReadyToDestroy();
}

void UMeshParticleizePrepareAsyncAction::HandleFailed(const FString& Error)
{
	auto* Component = this->Preparation.Get();
	const auto Diagnostics = Error;
	this->ReleaseBindings();
	this->Failed.Broadcast(Component, Diagnostics);
	this->SetReadyToDestroy();
}

void UMeshParticleizePrepareAsyncAction::ReleaseBindings()
{
	if (IsValid(this->Preparation) == true)
	{
		this->Preparation->OnPrepared.RemoveAll(this);
		this->Preparation->OnPreparationFailed.RemoveAll(this);
	}
	this->Preparation = nullptr;
}

UMeshParticleizeAsyncAction* UMeshParticleizeAsyncAction::ParticleizeMesh(UMeshComponent* Mesh,
	UMeshParticleizerPreparedMesh* Prepared, FMeshParticleizeOptions Options)
{
	auto Target = FMeshParticleizerTarget{};
	Target.Mesh = Mesh;
	Target.PreparedData = Prepared;
	return UMeshParticleizeAsyncAction::ParticleizeTargets({Target}, Options);
}

UMeshParticleizeAsyncAction* UMeshParticleizeAsyncAction::ParticleizeTargets(
	const TArray<FMeshParticleizerTarget>& Targets, FMeshParticleizeOptions Options)
{
	auto* Action = NewObject<UMeshParticleizeAsyncAction>();
	Action->Sources = Targets;
	Action->Settings = Options;
	if (Targets.IsEmpty() == false && Targets[0].Mesh.IsValid() == true)
	{
		Action->RegisterWithGameInstance(Targets[0].Mesh.Get());
	}
	return Action;
}

UMeshParticleizeAsyncAction* UMeshParticleizeAsyncAction::ParticleizeMeshWithPreset(UMeshComponent* Mesh,
	UMeshParticleizerPreparedMesh* Prepared, UMeshParticleizerPreset* Preset)
{
	// 遅延準備中に資産が編集されても、要求時点の設定を使う。
	auto Settings = FMeshParticleizeOptions{};
	auto Error = FString{};
	if (Preset == nullptr)
	{
		Error = TEXT("Select a particleizer preset.");
	}
	else
	{
		Preset->CopyOptions(Settings, Error);
	}
	auto* Action = UMeshParticleizeAsyncAction::ParticleizeMesh(Mesh, Prepared, Settings);
	Action->SetupError = Error;
	return Action;
}

UMeshParticleizeAsyncAction* UMeshParticleizeAsyncAction::StartParticleize(UMeshParticleizerComponent* Component,
	float ConversionDuration, float ParticleLifetime, UMeshParticleizerPreset* Preset,
	const FMeshParticleizerOriginOverride& Origin)
{
	auto* Action = NewObject<UMeshParticleizeAsyncAction>();
	Action->Preparation = Component;
	Action->bUseRegisteredComponent = true;
	if (IsValid(Component) == false)
	{
		Action->SetupError = TEXT("Select a registered particleizer component.");
	}
	else
	{
		Action->RegisterWithGameInstance(Component);
		ResolvePlaybackSettings(Component->Options, Preset, ConversionDuration, ParticleLifetime,
			Origin, Action->Settings, Action->SetupError);
	}
	return Action;
}

UMeshParticleizeAsyncAction* UMeshParticleizeAsyncAction::ParticleizeMeshSimple(UMeshComponent* Mesh,
	UMeshParticleizerPreparedMesh* Prepared, float ConversionDuration, float ParticleLifetime,
	UMeshParticleizerPreset* Preset, const FMeshParticleizerOriginOverride& Origin)
{
	auto Settings = FMeshParticleizeOptions{};
	auto Error = FString{};
	ResolvePlaybackSettings(Settings, Preset, ConversionDuration, ParticleLifetime, Origin, Settings, Error);
	auto* Action = UMeshParticleizeAsyncAction::ParticleizeMesh(Mesh, Prepared, Settings);
	Action->SetupError = Error;
	return Action;
}

void UMeshParticleizeAsyncAction::Activate()
{
	if (this->bActivated == true)
	{
		return;
	}
	this->bActivated = true;
	if (this->SetupError.IsEmpty() == false)
	{
		this->HandlePreparationFailed(this->SetupError);
		return;
	}
	auto Error = FString{};
	if (this->bUseRegisteredComponent == true)
	{
		if (IsValid(this->Preparation) == false || this->Preparation->IsRegistered() == false ||
			this->Preparation->GetPreparationState() == EMeshParticleizerPreparationState::Preparing)
		{
			this->HandlePreparationFailed(TEXT("Use a registered component and wait for its current preparation to complete."));
			return;
		}
		auto* Existing = this->Preparation->GetLastOperation();
		if (Existing != nullptr && Existing->GetState() != EMeshParticleizeState::Finished &&
			Existing->GetState() != EMeshParticleizeState::Cancelled && Existing->GetState() != EMeshParticleizeState::Failed)
		{
			this->Operation = Existing;
			this->HandlePreparationFailed(TEXT("This component already has an active operation. Cancel it before restarting."));
			return;
		}
		if (this->Preparation->SetOptions(this->Settings, Error) == false)
		{
			this->HandlePreparationFailed(Error);
			return;
		}
	}
	else
	{
		auto* Mesh = this->Sources.IsEmpty() == false ? this->Sources[0].Mesh.Get() : nullptr;
		if (Mesh == nullptr || Mesh->GetWorld() == nullptr || Mesh->GetOwner() == nullptr || this->Settings.Validate(Error) == false)
		{
			this->HandlePreparationFailed(Error.IsEmpty() == false ? Error : TEXT("Source world is unavailable."));
			return;
		}

		this->Preparation = NewObject<UMeshParticleizerComponent>(Mesh->GetOwner());
		this->bOwnsPreparation = true;
		this->Preparation->RegisterComponent();
		if (this->Preparation->ConfigureTargets(this->Sources, Error) == false ||
			this->Preparation->SetOptions(this->Settings, Error) == false)
		{
			this->HandlePreparationFailed(Error);
			return;
		}
	}

	// Readyなら資源を再生成せず、依存設定が変わった場合だけ通常の準備を待つ。
	this->Preparation->OnPrepared.AddDynamic(this, &UMeshParticleizeAsyncAction::HandlePrepared);
	this->Preparation->OnPreparationFailed.AddDynamic(this, &UMeshParticleizeAsyncAction::HandlePreparationFailed);
	if (this->Preparation->GetPreparationState() == EMeshParticleizerPreparationState::Ready)
	{
		this->HandlePrepared();
	}
	else
	{
		this->Preparation->PrepareForParticleize();
	}
}

void UMeshParticleizeAsyncAction::HandlePrepared()
{
	auto Error = FString{};
	this->Operation = this->Preparation->StartParticleize(Error);
	if (this->Operation == nullptr || Error.IsEmpty() == false)
	{
		this->HandlePreparationFailed(Error);
		return;
	}

	// Startは次のワールド更新から通知するため、D=L=0でも全ピンを購読できる。
	this->Operation->OnStarted.AddDynamic(this, &UMeshParticleizeAsyncAction::HandleStarted);
	this->Operation->OnMeshConverted.AddDynamic(this, &UMeshParticleizeAsyncAction::HandleConverted);
	this->Operation->OnCancelled.AddDynamic(this, &UMeshParticleizeAsyncAction::HandleCancelled);
	this->Operation->OnFinished.AddDynamic(this, &UMeshParticleizeAsyncAction::HandleFinished);
	this->ReleasePreparation();
}

void UMeshParticleizeAsyncAction::HandlePreparationFailed(const FString& Error)
{
	this->ReleasePreparation();
	this->Failed.Broadcast(this->Operation, EMeshParticleizeFinishReason::Completed, Error);
	this->SetReadyToDestroy();
}

void UMeshParticleizeAsyncAction::ReleasePreparation()
{
	if (IsValid(this->Preparation) == true)
	{
		this->Preparation->OnPrepared.RemoveAll(this);
		this->Preparation->OnPreparationFailed.RemoveAll(this);
		if (this->bOwnsPreparation == true && this->Preparation->IsBeingDestroyed() == false)
		{
			this->Preparation->DestroyComponent();
		}
	}
	this->Preparation = nullptr;
}

void UMeshParticleizeAsyncAction::HandleStarted(UMeshParticleizeOperation* InOperation)
{
	this->Started.Broadcast(InOperation, EMeshParticleizeFinishReason::Completed, InOperation->GetDiagnostics());
}

void UMeshParticleizeAsyncAction::HandleConverted(UMeshParticleizeOperation* InOperation)
{
	this->MeshConverted.Broadcast(InOperation, EMeshParticleizeFinishReason::Completed, InOperation->GetDiagnostics());
}

void UMeshParticleizeAsyncAction::HandleCancelled(UMeshParticleizeOperation* InOperation)
{
	this->Cancelled.Broadcast(InOperation, EMeshParticleizeFinishReason::Cancelled, InOperation->GetDiagnostics());
}

void UMeshParticleizeAsyncAction::HandleFinished(UMeshParticleizeOperation* InOperation, EMeshParticleizeFinishReason Reason)
{
	this->Finished.Broadcast(InOperation, Reason, InOperation->GetDiagnostics());
	InOperation->OnStarted.RemoveAll(this);
	InOperation->OnMeshConverted.RemoveAll(this);
	InOperation->OnCancelled.RemoveAll(this);
	InOperation->OnFinished.RemoveAll(this);
	this->SetReadyToDestroy();
}
