#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "UObject/StrongObjectPtr.h"

class UMaterialInterface;
class UMaterialInstanceConstant;
class UStaticMesh;
class FViewUniformShaderParameters;

// 独立描画との比較で共通に使う一時材とView資源。保存資産は変更しない。
namespace MeshParticleizerCoverageValidation
{
	FTextureRHIRef GetNoiseTexture();
	void SetViewResources(FViewUniformShaderParameters& Uniforms, FRHITexture* Texture);
	TStrongObjectPtr<UMaterialInstanceConstant> CreateInstance(UMaterialInterface* Material);

	// 遮蔽のない診断用の面を元材で描き、各画素の候補評価と独立に照合する。
	FString CompareRender(UMaterialInterface* Material, UStaticMesh* Mesh, FVector ViewDirection,
		FVector CameraTarget, const FString& OutputPng, int32 Resolution, float CameraDistance);
}
