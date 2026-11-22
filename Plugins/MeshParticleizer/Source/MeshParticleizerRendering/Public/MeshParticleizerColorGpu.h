#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "RHIResources.h"

// 更新したスロットだけをサンプル色へ反映する。入力Textureの寿命と容量予約は呼出し側が持つ。
MESHPARTICLEIZERRENDERING_API void AddMeshParticleizerColorPass(FRDGBuilder& GraphBuilder,
	FRDGBufferRef Colors, uint32 Count, uint32 Slot, FRHITexture* Positions, FRHITexture* Coordinates,
	FRHITexture* Texture, FRHISamplerState* Sampler, FVector2f UVScale, FVector2f UVOffset, FVector3f ColorScale, bool bClampColor = true);

// 公開画像を線形RGBへ揃える。ゲーム側入力の更新はこのPassより先に発行する。
MESHPARTICLEIZERRENDERING_API void AddMeshParticleizerColorPublishPass(FRDGBuilder& GraphBuilder,
	FRHITexture* Input, FRHITexture* Output, bool bPreserveHDR = false);
