#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "MeshParticleizerSurfaceTextureAsyncAction.generated.h"

class UMeshParticleizerSurfaceTexture;
class UTexture;
class UTexture2D;
class UTextureRenderTarget2D;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FMeshParticleizerSurfaceTextureEvent,
	UMeshParticleizerSurfaceTexture*, Registration, const FString&, Diagnostics);

// 登録と初回の利用可能待ちをまとめる。公開後の画像更新はPublish Colorで明示する。
UCLASS(meta = (ExposedAsyncProxy = AsyncAction))
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerSurfaceTextureAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FMeshParticleizerSurfaceTextureEvent Ready;

	UPROPERTY(BlueprintAssignable)
	FMeshParticleizerSurfaceTextureEvent Failed;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Register Surface Texture (Async)"))
	static UMeshParticleizerSurfaceTextureAsyncAction* RegisterSurfaceTexture(UObject* WorldContextObject, UTexture2D* Texture);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject", DisplayName = "Register Color Provider (Async)", AdvancedDisplay = "bPreserveHDR"))
	static UMeshParticleizerSurfaceTextureAsyncAction* RegisterColorProvider(UObject* WorldContextObject,
		UTextureRenderTarget2D* Input, bool bPreserveHDR = false);

	// 待機を中止し、以後の通知を止める。共有資源の使用者には影響しない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Surface Color")
	void Cancel();

	virtual void Activate() override;
	virtual void BeginDestroy() override;

private:
	static UMeshParticleizerSurfaceTextureAsyncAction* CreateRequest(UObject* WorldContextObject,
		UTexture* Input, bool bProvider, bool bHDR);
	void CheckReady();
	void Finish(const FString& Error);
	void ReleaseBindings();
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

	// 要求中だけ画像と登録を保持し、成功時はReadyの受取側へ所有を渡す。
	UPROPERTY(Transient)
	TObjectPtr<UTexture> InputTexture;

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerSurfaceTexture> Registration;

	TWeakObjectPtr<UWorld> RequestWorld;
	FTimerHandle ReadinessTimer;
	FDelegateHandle WorldCleanupHandle;
	double Deadline = 0;
	bool bColorProvider = false;
	bool bPreserveHDR = false;
	bool bActivated = false;
	bool bCompleted = false;
};
