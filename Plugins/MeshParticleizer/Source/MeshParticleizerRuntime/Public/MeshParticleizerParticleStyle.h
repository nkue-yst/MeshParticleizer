#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerParticleStyle.generated.h"

// 元表面の準備データから独立した描画設定。Presetや再生設定から共有する。
UCLASS(BlueprintType)
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerParticleStyle : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playback")
	TSoftObjectPtr<UNiagaraSystem> ParticleSystem;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playback")
	TSoftObjectPtr<UNiagaraSystem> GdfParticleSystem;

	UPROPERTY(VisibleAnywhere, Category = "Cache", AdvancedDisplay)
	TArray<FMeshParticleizerResource> Resources;

	bool ValidateResourceManifest(FString& OutError) const;

#if WITH_EDITOR
	// コンパイル済みの通常/GDFシステムと全依存を検査してから一括設定する。
	bool SetSystems(UNiagaraSystem* Normal, UNiagaraSystem* Gdf, FString& OutError);
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
