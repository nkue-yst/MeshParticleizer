#pragma once

#include "MeshParticleizerPreparedMesh.h"

#if WITH_EDITOR
// 準備データと描画Styleで同じ依存収集を使う。元材の表示用資源だけを別に保護する。
class FMeshParticleizerResourceCollector
{
public:
	FMeshParticleizerResourceCollector(TArray<FMeshParticleizerResource>& Resources, FString& Error)
		: Resources(Resources), Error(Error)
	{
	}

	bool Add(UObject* Asset, bool bDisplay = false);
	bool AddMaterial(UMaterialInterface* Material, bool bDisplay = false);
	bool AddSystem(UNiagaraSystem* System);

private:
	TArray<FMeshParticleizerResource>& Resources;
	FString& Error;
};
#endif
