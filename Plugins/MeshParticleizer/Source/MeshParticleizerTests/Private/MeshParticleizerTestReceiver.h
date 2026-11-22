#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizerSurfaceTexture.h"
#include "MeshParticleizerTestReceiver.generated.h"

UCLASS()
class UMeshParticleizerTestReceiver : public UObject
{
	GENERATED_BODY()

public:
	TArray<FName> Events;
	TWeakObjectPtr<AActor> DestroyOnConverted;
	EMeshParticleizeFinishReason Reason = EMeshParticleizeFinishReason::Completed;
	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizeOperation> FailedOperation;

	FString FailureMessage;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerSurfaceTexture> SurfaceRegistration;

	UFUNCTION()
	void SurfaceReady(UMeshParticleizerSurfaceTexture* Registration, const FString& Diagnostics)
	{
		this->Events.Add(TEXT("SurfaceReady"));
		this->SurfaceRegistration = Registration;
	}

	UFUNCTION()
	void SurfaceFailed(UMeshParticleizerSurfaceTexture* Registration, const FString& Diagnostics)
	{
		this->Events.Add(TEXT("SurfaceFailed"));
		this->SurfaceRegistration = Registration;
		this->FailureMessage = Diagnostics;
	}

	UFUNCTION()
	void Started(UMeshParticleizeOperation* Operation)
	{
		this->Events.Add(TEXT("Started"));
	}

	UFUNCTION()
	void Converted(UMeshParticleizeOperation* Operation);

	UFUNCTION()
	void Cancelled(UMeshParticleizeOperation* Operation)
	{
		this->Events.Add(TEXT("Cancelled"));
	}

	UFUNCTION()
	void Finished(UMeshParticleizeOperation* Operation, EMeshParticleizeFinishReason InReason)
	{
		this->Events.Add(TEXT("Finished"));
		this->Reason = InReason;
	}

	void Bind(UMeshParticleizeOperation* Operation);

	UFUNCTION()
	void Failed(UMeshParticleizeOperation* Operation, EMeshParticleizeFinishReason InReason, const FString& Message)
	{
		this->Events.Add(TEXT("Failed"));
		this->FailedOperation = Operation;
		this->FailureMessage = Message;
	}

	UFUNCTION()
	void AsyncStarted(UMeshParticleizeOperation* Operation, EMeshParticleizeFinishReason InReason, const FString& Message)
	{
		this->Started(Operation);
	}

	UFUNCTION()
	void AsyncConverted(UMeshParticleizeOperation* Operation, EMeshParticleizeFinishReason InReason, const FString& Message)
	{
		this->Converted(Operation);
	}

	UFUNCTION()
	void AsyncFinished(UMeshParticleizeOperation* Operation, EMeshParticleizeFinishReason InReason, const FString& Message)
	{
		this->Finished(Operation, InReason);
	}

	UFUNCTION()
	void Prepared(class UMeshParticleizerComponent* Component, const FString& Diagnostics)
	{
		this->Events.Add(TEXT("Prepared"));
	}

	UFUNCTION()
	void PreparationFailed(class UMeshParticleizerComponent* Component, const FString& Diagnostics)
	{
		this->Events.Add(TEXT("PreparationFailed"));
		this->FailureMessage = Diagnostics;
	}
};
