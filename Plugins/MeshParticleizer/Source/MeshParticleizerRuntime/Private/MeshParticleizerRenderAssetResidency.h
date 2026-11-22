#pragma once

#include "CoreMinimal.h"
#include "Engine/StreamableRenderAsset.h"

// Textureと粒子メッシュは複数ワールドで共有される。最後の利用者まで元の常駐指定を保持する。
struct FMeshParticleizerRenderAssetResidency
{
	TWeakObjectPtr<UStreamableRenderAsset> Asset;
	bool bOriginalForced = false;

	~FMeshParticleizerRenderAssetResidency()
	{
		if (auto* Current = this->Asset.Get(); Current != nullptr && Current->bForceMiplevelsToBeResident == true)
		{
			Current->bForceMiplevelsToBeResident = this->bOriginalForced;
		}
	}

	static TSharedRef<FMeshParticleizerRenderAssetResidency> Acquire(UStreamableRenderAsset* Asset)
	{
		static TMap<TWeakObjectPtr<UStreamableRenderAsset>, TWeakPtr<FMeshParticleizerRenderAssetResidency>> Leases;
		for (auto It = Leases.CreateIterator(); It; ++It)
		{
			if (It.Value().IsValid() == false)
			{
				It.RemoveCurrent();
			}
		}
		if (const auto Existing = Leases.FindRef(Asset).Pin(); Existing.IsValid() == true)
		{
			return Existing.ToSharedRef();
		}

		auto Lease = MakeShared<FMeshParticleizerRenderAssetResidency>();
		Lease->Asset = Asset;
		Lease->bOriginalForced = Asset->bForceMiplevelsToBeResident;
		Asset->bForceMiplevelsToBeResident = true;
		Leases.Add(Asset, Lease);
		return Lease;
	}
};
