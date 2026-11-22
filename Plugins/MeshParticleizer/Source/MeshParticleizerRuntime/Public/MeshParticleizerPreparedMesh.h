#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MeshParticleizerSurfaceProjection.h"
#include "MeshParticleizerSurfaceColor.h"
#include "MaterialShared.h"
#include "MeshParticleizerPreparedMesh.generated.h"

class UStaticMesh;
class USkeletalMesh;
class UTexture2D;
class UMaterialInterface;
class UNiagaraSystem;

// 再生中の検証では宣言一覧を作り直さず、準備時に確定した名前・型・レイヤーを使う。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerMaterialParameterKey
{
	EMaterialParameterType Type;
	FMaterialParameterInfo Info;

	static TArray<FMeshParticleizerMaterialParameterKey> Gather(const UMaterialInterface* Material);
};

// 読み込む前に共有容量を予約するための一覧。描画材から参照するテクスチャも含む。
USTRUCT()
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerResource
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Cache")
	TSoftObjectPtr<UObject> Asset;

	UPROPERTY(VisibleAnywhere, Category = "Cache")
	int64 GpuBytes = 0;

	UPROPERTY(VisibleAnywhere, Category = "Cache")
	bool bDisplayResource = false;
};

UCLASS(BlueprintType)
class MESHPARTICLEIZERRUNTIME_API UMeshParticleizerPreparedMesh : public UDataAsset
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentDataVersion = 7;
	static constexpr int32 CurrentCoverageDataVersion = 3;
	static constexpr int32 SkinVertexStride = 8;
	static constexpr int32 MaxSkinInfluences = 12;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	TSoftObjectPtr<UStaticMesh> SourceMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	TSoftObjectPtr<USkeletalMesh> SkeletalMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	FString SourceFingerprint;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	int32 DataVersion = 0;

	// 表面の対応はこのLODで固定する。骨姿勢のLOD0保持とは独立する。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	int32 SamplingLOD = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Source")
	TArray<int32> EnabledSections;

	static FString MakeSourceFingerprint(const FString& RenderKey, int32 LOD)
	{
		return LOD == 0 ? RenderKey : RenderKey + FString::Printf(TEXT("|SamplingLOD=%d"), LOD);
	}

	// Cookが再生システムと依存資産を辿れるよう、準備資産へ参照を保存する。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playback")
	TSoftObjectPtr<UNiagaraSystem> ParticleSystem;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Playback")
	TSoftObjectPtr<UNiagaraSystem> GdfParticleSystem;

	UPROPERTY(VisibleAnywhere, Category = "Cache", AdvancedDisplay)
	TArray<FMeshParticleizerResource> Resources;

	bool ValidateResourceManifest(FString& OutError) const;

	// 消失材へ保存したベイク時の値と照合する。再生中はキャッシュした宣言一覧を渡す。
	bool ValidateBakedMaterialParameters(int32 Slot, const UMaterialInterface* Original,
		const UMaterialInterface* Display, FString& OutError, bool bAllowColorProvider = false,
		const TArray<FMeshParticleizerMaterialParameterKey>* CachedParameters = nullptr) const;

#if WITH_EDITOR
	// 準備ツールと検証用。必要資産を読み込み済みにして呼ぶ。
	bool CaptureResourceManifest(FString& OutError);

	// ベイク完了時だけ記録する。依存一覧の更新では古いデータの基準を付け替えない。
	bool CapturePreparationDependencies(FString& OutError);
	void GetPreparationIssues(TArray<FString>& OutIssues) const;
	bool ValidatePreparation(FString& OutError) const;
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Category = "Preparation", AdvancedDisplay)
	int32 DependencyFingerprintVersion = 0;

	// 資産の識別子と準備時点の内容。Editor専用の診断情報は配布時に除く。
	UPROPERTY(VisibleAnywhere, Category = "Preparation", AdvancedDisplay)
	TMap<FString, FString> PreparationDependencies;
#endif

	// 同じIDの位置、法線、色をテクスチャの同じ画素へ格納する。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	TSoftObjectPtr<UTexture2D> Positions;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	TSoftObjectPtr<UTexture2D> Normals;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	TSoftObjectPtr<UTexture2D> Colors;

	// 任意の元発光。表面色の0..1制限や演出用の追加発光とは独立した線形HDR値。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	TSoftObjectPtr<UTexture2D> SourceEmission;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	TSoftObjectPtr<UTexture2D> Coordinates;

	// 頂点UV2行の末尾にSlotと有効フラグを格納。Staticは位置3行・法線3行を続ける。
	// Skeletalの位置は固定頂点と当フレームの骨から求める。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	TSoftObjectPtr<UTexture2D> CoverageTriangles;

	// 全表面の候補と現在の頂点形式を要求し、旧資産の版だけ更新して対応済みにしない。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	int32 CoverageDataVersion = 0;

	// 元の三角形IDの範囲。Texture末尾のパディングを描画しない。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	int32 CoverageTriangleCount = 0;

	// 選択LODの描画頂点・三角形を固定し、参照面のサンプルと対応させる。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skinning")
	TSoftObjectPtr<UTexture2D> SkinVertices;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skinning")
	TSoftObjectPtr<UTexture2D> SkinTriangles;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skinning")
	int32 BoneCount = 0;

	// 未作成の既存資産は通常のボーン近似を使える。精密投影の呼出しだけ準備不足を返す。
	UPROPERTY()
	FMeshParticleizerSurfaceProjection SurfaceProjection;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	int32 SampleCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	float SurfaceArea = 0;

	// 選択LODの三角形の面積ベクトル。非一様スケール時も体積やBoundsで近似しない。
	UPROPERTY()
	TArray<FVector> SurfaceAreaVectors;

	bool GetScaledSurfaceArea(const FVector& Scale, double& OutArea, FString& OutError) const;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	FVector BoundsMin = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface")
	FVector BoundsMax = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Materials")
	TArray<TSoftObjectPtr<UMaterialInterface>> SourceMaterials;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Materials")
	TArray<TSoftObjectPtr<UMaterialInterface>> DissolveMaterials;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Materials")
	bool bSupportsSurfaceTint = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Materials")
	bool bSupportsColorProvider = false;

	// 消失材と粒子に、表面色とは独立したHDR公開画像を結び付ける。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bSupportsEmissionProvider = false;

	// UnlitのEmissiveを0..1の表面色として明示的に準備した。元のHDR発光の継承とは別。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Surface Color")
	bool bUsesUnlitEmissive = false;

	// スロット順の色取得候補。既存資産では空でも従来のベイク再生を維持する。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Materials")
	TArray<FMeshParticleizerTextureColorSource> TextureColorSources;

	bool HasTextureColorBindings() const;
	bool NeedsRuntimeCoverage() const
	{
		return this->TextureColorSources.ContainsByPredicate([](const FMeshParticleizerTextureColorSource& Slot) {
			return Slot.bRequiresRuntimeCoverage;
		});
	}
	bool NeedsSurfaceColorCache() const
	{
		return this->bSupportsColorProvider || this->HasTextureColorBindings();
	}
};
