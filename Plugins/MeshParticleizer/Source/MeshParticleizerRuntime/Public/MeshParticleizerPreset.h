#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MeshParticleizerTypes.h"
#include "MeshParticleizerPreset.generated.h"

// 再利用する既定値。適用時にコピーし、再生中の調整を共有資産へ書き戻さない。
UCLASS(BlueprintType)
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mesh Particleizer", meta = (ShowOnlyInnerProperties))
	FMeshParticleizeOptions Options;

	// コピーした値はSet Members in Mesh Particleize Optionsで上書きしてから使用できる。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preset")
	bool CopyOptions(FMeshParticleizeOptions& OutOptions, FString& OutError) const;

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preset")
	bool SetOptions(const FMeshParticleizeOptions& Settings, FString& OutError);

	static bool ValidateSettings(const FMeshParticleizeOptions& Settings, FString& OutError);

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif
};
