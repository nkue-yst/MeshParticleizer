#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MeshParticleizerTypes.h"
#include "MeshParticleizerOriginLibrary.generated.h"

UCLASS()
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerOriginLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// 成功したTraceまたはHitイベントで呼び出し、返り値をOptions.HitOriginへ渡す。
	// 元Actorの生存を延長しない。初期重なりは表面の命中点がないため代替起点を使う。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Origin")
	static FMeshParticleizerHitOrigin CaptureHitOrigin(const FHitResult& Hit);

	// 選択したサンプリングLODの面へ投影する。LOD0の完全な骨姿勢を更新してから呼ぶ。
	// Morph、Cloth、Deformer、WPO等は成功扱いにせず、通常の起点解決で指定代替へ進む。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Origin", meta = (AdvancedDisplay = "MaxProjectionDistance"))
	static FMeshParticleizerHitOrigin CaptureSurfaceHitOrigin(const FHitResult& Hit,
		const UMeshParticleizerPreparedMesh* Prepared, double MaxProjectionDistance = 10);

	// 全点を検証してからOptionsへ一括設定する。失敗した場合は元の設定を保つ。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Origin")
	static bool SetPointOrigins(UPARAM(ref) FMeshParticleizeOptions& Options, const TArray<FVector>& Points, FString& OutError);
};
