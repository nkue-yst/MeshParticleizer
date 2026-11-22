#include "MeshParticleizerSurfaceTextureAsyncAction.h"
#include "MeshParticleizerSurfaceTexture.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

UMeshParticleizerSurfaceTextureAsyncAction* UMeshParticleizerSurfaceTextureAsyncAction::CreateRequest(
	UObject* WorldContextObject, UTexture* Input, bool bProvider, bool bHDR)
{
	auto* Action = NewObject<UMeshParticleizerSurfaceTextureAsyncAction>();
	Action->InputTexture = Input;
	Action->bColorProvider = bProvider;
	Action->bPreserveHDR = bHDR;
	Action->RequestWorld = GEngine != nullptr
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (Action->RequestWorld.IsValid() == true)
	{
		Action->RegisterWithGameInstance(Action->RequestWorld.Get());
	}
	return Action;
}

UMeshParticleizerSurfaceTextureAsyncAction* UMeshParticleizerSurfaceTextureAsyncAction::RegisterSurfaceTexture(
	UObject* WorldContextObject, UTexture2D* Texture)
{
	return UMeshParticleizerSurfaceTextureAsyncAction::CreateRequest(WorldContextObject, Texture, false, false);
}

UMeshParticleizerSurfaceTextureAsyncAction* UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(
	UObject* WorldContextObject, UTextureRenderTarget2D* Input, bool bPreserveHDR)
{
	return UMeshParticleizerSurfaceTextureAsyncAction::CreateRequest(WorldContextObject, Input, true, bPreserveHDR);
}

void UMeshParticleizerSurfaceTextureAsyncAction::Activate()
{
	if (this->bActivated == true || this->bCompleted == true)
	{
		return;
	}
	this->bActivated = true;
	auto* World = this->RequestWorld.Get();
	if (World == nullptr || World->IsGameWorld() == false || World->GetNetMode() == NM_DedicatedServer)
	{
		this->Finish(TEXT("Surface texture registration requires a rendering game world."));
		return;
	}

	// 容量と入力の判定は同期登録と共用し、待機ノード側では資源を作り直さない。
	auto* Manager = World->GetSubsystem<UMeshParticleizerWorldSubsystem>();
	if (Manager == nullptr)
	{
		this->Finish(TEXT("The particleizer world is no longer available."));
		return;
	}
	auto Error = FString{};
	this->Registration = this->bColorProvider == true
		? Manager->RegisterColorProvider(Cast<UTextureRenderTarget2D>(this->InputTexture), Error, this->bPreserveHDR)
		: Manager->RegisterSurfaceTexture(Cast<UTexture2D>(this->InputTexture), Error);
	if (this->Registration == nullptr)
	{
		this->Finish(Error);
		return;
	}

	this->Deadline = World->GetTimeSeconds() + 30;
	this->WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this,
		&UMeshParticleizerSurfaceTextureAsyncAction::HandleWorldCleanup);
	this->CheckReady();
}

void UMeshParticleizerSurfaceTextureAsyncAction::CheckReady()
{
	if (this->bCompleted == true)
	{
		return;
	}
	auto* World = this->RequestWorld.Get();
	if (World == nullptr)
	{
		this->Cancel();
		return;
	}

	auto Error = FString{};
	if (this->Registration->IsReady(Error) == true || Error.IsEmpty() == false)
	{
		this->Finish(Error);
		return;
	}
	if (World->GetTimeSeconds() >= this->Deadline)
	{
		this->Finish(TEXT("Surface texture registration timed out after 30 seconds. Check mip residency and Color Provider publication resources."));
		return;
	}

	// ワールドのTimerと時刻を使い、Pause中は初回公開と待機期限の両方を保留する。
	World->GetTimerManager().SetTimer(this->ReadinessTimer, this,
		&UMeshParticleizerSurfaceTextureAsyncAction::CheckReady, 0.05f, false);
}

void UMeshParticleizerSurfaceTextureAsyncAction::Finish(const FString& Error)
{
	if (this->bCompleted == true)
	{
		return;
	}
	this->bCompleted = true;
	const auto Diagnostics = Error;
	auto Result = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(Diagnostics.IsEmpty() == true ? this->Registration.Get() : nullptr);
	this->ReleaseBindings();
	if (Diagnostics.IsEmpty() == true)
	{
		this->Ready.Broadcast(Result.Get(), Diagnostics);
	}
	else
	{
		this->Failed.Broadcast(nullptr, Diagnostics);
	}
	this->SetReadyToDestroy();
}

void UMeshParticleizerSurfaceTextureAsyncAction::ReleaseBindings()
{
	if (auto* World = this->RequestWorld.Get())
	{
		World->GetTimerManager().ClearTimer(this->ReadinessTimer);
	}
	FWorldDelegates::OnWorldCleanup.Remove(this->WorldCleanupHandle);
	this->WorldCleanupHandle.Reset();
	this->RequestWorld.Reset();
	this->Registration = nullptr;
	this->InputTexture = nullptr;
}

void UMeshParticleizerSurfaceTextureAsyncAction::Cancel()
{
	if (this->bCompleted == true)
	{
		return;
	}
	this->bCompleted = true;
	this->ReleaseBindings();
	this->SetReadyToDestroy();
}

void UMeshParticleizerSurfaceTextureAsyncAction::HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (World == this->RequestWorld.Get())
	{
		this->Cancel();
	}
}

void UMeshParticleizerSurfaceTextureAsyncAction::BeginDestroy()
{
	this->Cancel();
	Super::BeginDestroy();
}
