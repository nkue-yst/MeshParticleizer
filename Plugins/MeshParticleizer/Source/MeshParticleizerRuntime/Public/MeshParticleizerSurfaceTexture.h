#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MeshParticleizerSurfaceTexture.generated.h"

class UTexture2D;
class UTexture;
class UTextureRenderTarget2D;
class UMeshParticleizerWorldSubsystem;
class UMeshParticleizerAssetCache;
struct FMeshParticleizerRenderAssetResidency;
struct FMeshParticleizerColorPublication;

// 差し替え前に容量を予約し、使用中のMipを保持する。利用者と再生側が参照を所有する。
UCLASS(BlueprintType)
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerSurfaceTexture : public UObject
{
	GENERATED_BODY()

public:
	// falseで診断が空なら非同期のMip読込中。Readyになってから色設定へ渡す。
	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Surface Color")
	bool IsReady(FString& OutError) const;

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Surface Color")
	UTexture* GetTexture() const
	{
		return this->Texture;
	}

	// 入力側の描画命令を発行してから呼ぶ。次のワールド更新で公開し、Pause中は待機する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color")
	bool PublishColor(FString& OutError);

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Surface Color")
	bool IsColorProvider() const
	{
		return this->ProviderInput != nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "Mesh Particleizer|Surface Color")
	bool IsHDRProvider() const
	{
		return this->IsColorProvider() && this->bPreserveHDR;
	}

	uint64 GetRevision() const
	{
		return this->Revision;
	}

	bool BelongsTo(const UMeshParticleizerWorldSubsystem* Manager) const;
	virtual void BeginDestroy() override;

private:
	friend class UMeshParticleizerWorldSubsystem;
	bool Initialize(UMeshParticleizerWorldSubsystem* Manager, UTexture2D* InTexture, FString& OutError);
	bool InitializeProvider(UMeshParticleizerWorldSubsystem* Manager, UTextureRenderTarget2D* Input, FString& OutError, bool bInPreserveHDR);
	bool ValidateProvider(FString& OutError) const;
	void PublishPending();

	UPROPERTY(Transient)
	TObjectPtr<UTexture> Texture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ProviderInput;

	TWeakObjectPtr<UMeshParticleizerAssetCache> Cache;
	TSharedPtr<FMeshParticleizerRenderAssetResidency> Residency;
	int64 ReservedBytes = 0;
	int64 ProviderInputBytes = 0;
	uint64 Revision = 0;
	FIntPoint ProviderSize = FIntPoint::ZeroValue;
	uint8 ProviderFormat = 0;
	bool bProviderSRGB = false;
	bool bPreserveHDR = false;
	TSharedPtr<FMeshParticleizerColorPublication, ESPMode::ThreadSafe> Publication;
};
