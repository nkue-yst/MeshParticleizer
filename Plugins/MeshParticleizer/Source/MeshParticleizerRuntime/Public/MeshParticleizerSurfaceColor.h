#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerSurfaceColor.generated.h"

class UTexture2D;

// 単純な色グラフの解析結果。対応判定だけでは再生経路を切り替えない。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerTextureColorSource
{
	GENERATED_BODY()

	static constexpr int32 CurrentDependencyAnalysisVersion = 3;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bCompatible = false;

	// 実際に使う色・Opacity Maskを解析済みか。未解析を固定色として再生しない。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bDependenciesAnalyzed = false;

	// 品質分岐とディザ依存を確認していない既存データは、再解析するまで再生しない。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	int32 DependencyAnalysisVersion = 0;

	bool HasCurrentDependencyAnalysis() const
	{
		return this->bDependenciesAnalyzed && this->DependencyAnalysisVersion == CurrentDependencyAnalysisVersion;
	}

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bRequiresColorProvider = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bUsesFrozenInputs = false;

	// 元Maskを現在のビューと姿勢で評価する。準備候補は穴を除かず全面から採る。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bRequiresRuntimeCoverage = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FString DependencyDiagnostics;

	// 色以外へ影響しないGlobalの数値パラメータだけ、Provider登録時に変更を許可する。
	// 旧資産では空のため、解析を更新するまでは従来どおり不一致を拒否する。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	TArray<FName> ProviderScalarParameters;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	TArray<FName> ProviderVectorParameters;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FName TextureParameter;

	// 生成済み消失材の登録入力。未生成の場合はNoneのままにする。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FName RegisteredTextureParameter;

	// パラメータでないTexture Sampleは、生成する消失材側でこのノードを登録入力へ接続する。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FGuid SampleExpressionGuid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bRequiresTextureParameter = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	TSoftObjectPtr<UTexture2D> Texture;

	// UV0へ掛ける固定倍率。元メッシュのUVは書き換えない。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FVector2D UVScale = FVector2D(1, 1);

	// 元材の定数・継承したパラメータ値を合成した、登録Tintより前の係数。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FLinearColor ColorScale = FLinearColor::White;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	FString Diagnostics;
};
