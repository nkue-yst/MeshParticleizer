#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceDynamic;
class UTexture;

// 次回の更新予約ではなく、表示材へ最後に適用した管理入力を保持する。
struct FMeshParticleizerMaterialInputs
{
	void SetScalar(UMaterialInstanceDynamic* Material, FName Name, float Value);
	void SetVector(UMaterialInstanceDynamic* Material, FName Name, const FLinearColor& Value);
	void SetTexture(UMaterialInstanceDynamic* Material, FName Name, UTexture* Value);
	bool Validate(const UMaterialInstanceDynamic* Material, int32 Slot, FString& OutError) const;

private:
	TMap<FName, float> Scalars;
	TMap<FName, FLinearColor> Vectors;

	// 常駐の所有権はMIDと登録に残し、検証のために旧Textureの寿命を延ばさない。
	TMap<FName, TWeakObjectPtr<UTexture>> Textures;
};
