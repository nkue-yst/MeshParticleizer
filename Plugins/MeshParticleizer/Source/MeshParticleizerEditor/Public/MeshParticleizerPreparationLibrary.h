#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MeshParticleizerSurfaceColor.h"
#include "MeshParticleizerPreparationLibrary.generated.h"

class UStaticMesh;
class USkeletalMesh;
class UMeshParticleizerPreparedMesh;
class UMeshParticleizerParticleStyle;
class UNiagaraSystem;
class UNiagaraComponent;
class USkinnedMeshComponent;
class UMeshComponent;
class UMaterialInterface;
class UMeshParticleizerPreview;

UCLASS()
class MESHPARTICLEIZEREDITOR_API UMeshParticleizerPreparationLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Content BrowserとToolsメニューから同じ準備画面を開く。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static void OpenPreparationWindow(UObject* Mesh);

	// 専用のGamePreviewワールドで、準備済みデータの再生設定を確認する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preview")
	static UMeshParticleizerPreview* OpenPreviewWindow(UMeshParticleizerPreparedMesh* Prepared = nullptr);

	// ベイクや保存を行わず、材質の対応可否と再生前に必要なProviderを全スロット分確認する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static FString InspectMaterialSupport(UObject* Mesh, bool bUseUnlitEmissive = false, bool bBakeSourceEmission = false);

	// 保存した基準と元資産を照合し、再生成が必要な依存を一覧にする。基準は更新しない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static FString InspectPreparedData(UMeshParticleizerPreparedMesh* Prepared);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static void OpenPreparedValidationWindow(const TArray<UMeshParticleizerPreparedMesh*>& Prepared);

	// 元アセットは変更せず、未使用の出力先へ準備データを生成する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static UMeshParticleizerPreparedMesh* PrepareStaticMesh(UStaticMesh* Mesh, const FString& Destination,
		int32 SampleCount, int32 Seed, FString& OutError, bool bUseUnlitEmissive = false, bool bBakeSourceEmission = false, int32 SamplingLOD = 0);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static UMeshParticleizerPreparedMesh* PrepareSkeletalMesh(USkeletalMesh* Mesh, const FString& Destination,
		int32 SampleCount, int32 Seed, FString& OutError, bool bUseUnlitEmissive = false, bool bBakeSourceEmission = false, int32 SamplingLOD = 0);

	// 配布プラグイン内に汎用GPUシステムを生成する。既存資産は上書きしない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static UNiagaraSystem* CreateParticleSystem(const FString& Destination, FString& OutError, bool bGlobalDistanceField = false, UStaticMesh* ParticleMesh = nullptr);

	// 外部アセットに依存しない粒・薄片を、新しいプラグイン内パスへ保存する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static UStaticMesh* CreateParticleMesh(const FString& AssetPath, bool bFlake, FString& OutError);

	// 準備データに依存しない描画Styleを作り、Presetから同じ形状を共有する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static UMeshParticleizerParticleStyle* CreateParticleStyle(const FString& AssetPath,
		UNiagaraSystem* ParticleSystem, UNiagaraSystem* GdfParticleSystem, FString& OutError);

	// ベイク済み表面を共有し、検証した再生システムを使う別資産を新しいパスへ保存する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static UMeshParticleizerPreparedMesh* CreatePreparedParticleVariant(UMeshParticleizerPreparedMesh* Prepared,
		const FString& AssetPath, UNiagaraSystem* ParticleSystem, UNiagaraSystem* GdfParticleSystem, FString& OutError);

	// 生成済みGPUシステムへ共通座標の入力を追加し、再コンパイルして保存する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static bool UpgradeParticleSystem(UNiagaraSystem* System, FString& OutError);

	// 版2の消失材を更新し、Cook依存を補う。表面サンプルと色は焼き直さない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static bool UpgradePreparedDependencies(UMeshParticleizerPreparedMesh* Prepared, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static bool RefreshPreparedResourceManifest(UMeshParticleizerPreparedMesh* Prepared, FString& OutError);

	// 元材を変更せず、同じテクスチャと色係数で再現できる単純な色グラフを調べる。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static FMeshParticleizerTextureColorSource AnalyzeTextureColorSource(UMaterialInterface* Material, bool bUseUnlitEmissive = false);

	// 表面サンプルと色を焼き直さず、既存Skeletal準備資産へ精密ヒット用の探索木を追加する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Preparation")
	static bool PrepareSurfaceProjection(UMeshParticleizerPreparedMesh* Prepared, FString& OutError);

	// GPUコンパイルの成否を確認し、生成HLSLをSavedへ書き出す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString InspectParticleSystem(UNiagaraSystem* System);

	// Editor診断専用。UV0平面の元Maskを各視線方向で評価し、白黒PNGと集計を返す。
	// 姿勢変形・粒子再生の検証ではない。元材は変更せず、一時複製でシェーダーを準備する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString ProbeMaterialCoverage(UMaterialInterface* Material, const TArray<FVector>& ViewDirections,
		const FString& OutputPng, int32 Resolution = 256);

	// Editor診断専用。元材を貼った平面のUE標準深度描画と、同じ画素のGPU候補判定を照合する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareMaterialCoverageRender(UMaterialInterface* Material, FVector ViewDirection,
		const FString& OutputPng, int32 Resolution = 256, float CameraDistance = 150);

	// Editor診断専用。同じ候補を複数カメラの独立描画へ照合し、合成と評価順序を検査する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareMaterialCoverageViews(UMaterialInterface* Material, const TArray<FVector>& ViewDirections,
		const FString& OutputPng, int32 CandidateResolution = 96, int32 ViewResolution = 256, float CameraDistance = 150);

	// Editor診断専用。LOD0の細い面とUV継ぎ目を調べ、描画比較に使う元三角形番号を返す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString InspectSkeletalCoverageTopology(USkeletalMesh* Mesh, int32 MaterialSlot, int32 Limit = 4);

	// Editor診断専用。元の1三角形を一時メッシュへ複写し、遮蔽を分けて元材の描画と照合する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareSkeletalCoverageTriangle(USkeletalMesh* Mesh, int32 TriangleIndex, FVector ViewDirection,
		const FString& OutputPng, int32 Resolution = 256, float CameraDistanceScale = 2);

	// Editor診断専用。同じ材質の最大2面を一緒に描き、実メッシュのUV継ぎ目を照合する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareSkeletalCoveragePatch(USkeletalMesh* Mesh, const TArray<int32>& TriangleIndices, FVector ViewDirection,
		const FString& OutputPng, int32 Resolution = 256, float CameraDistanceScale = 2);

	// Editor診断専用。SIEではなくGame Viewportを使うPIEを開始要求する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static bool StartValidationPlay(FString& OutError);

	// 分割画面も含む実際のGame Viewportを次の描画で撮影する。保存完了は呼出側で確認する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics", meta = (WorldContext = "WorldContextObject"))
	static bool RequestValidationScreenshot(UObject* WorldContextObject, const FString& OutputPng, FString& OutError);

	// Editor診断専用。描画処理を同期し、GPUバッファの容量だけを読む。粒子の読戻しは行わない。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString InspectSimulationMemory(UNiagaraComponent* Particles);

	// 読込や描画待ちを起こさず、準備資産のTexture初期化状態を観測する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString InspectPreparedTextureReadiness(UMeshParticleizerPreparedMesh* Prepared);

	// 初回GPU dispatchの入力を記録する。開始時は描画待ちを入れず、終了後に結果を回収する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static bool BeginSpawnDiagnostics(int32 MaxRecords = 64);

	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString EndSpawnDiagnostics();

	// Editor診断専用。候補順の動的Mask判定を同期取得し、粒子の候補IDと照合する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static TArray<float> ReadRuntimeCoverage(UNiagaraComponent* Particles);

	// Editor診断専用。ワールドのカウンタと、他ワールドを含むRHI共通資源を区別して返す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics", meta = (WorldContext = "WorldContextObject"))
	static FString InspectSharedGpuMemory(UObject* WorldContextObject, int32 AdditionalMeshInstances = 0, int32 AdditionalMeshSections = 0);

	// 外部アセットを含まない色検証用メッシュを生成する。元の色と粒子色の比較に使う。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static UStaticMesh* CreateColorValidationMesh(const FString& Destination, FString& OutError);

	// Editor診断専用。GPU候補IDに対応するベイクRGB（表面色または元発光）と材質スロット(W)を返す。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static TArray<FVector4> ReadPreparedSurfaceColors(UMeshParticleizerPreparedMesh* Prepared, const TArray<int32>& SampleIndices, bool bSourceEmission = false);

	// 停止した姿勢・外力ゼロの検証用。同期GPU読戻しはこのEditor診断だけで行う。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareSkeletalParticlePositions(USkinnedMeshComponent* Source,
		UMeshParticleizerPreparedMesh* Prepared, UNiagaraComponent* Particles);

	// 同じ姿勢のUE標準スキニングへ表面投影を照合する。全面評価はEditor診断だけで行う。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareSurfaceProjection(USkinnedMeshComponent* Source, UMeshParticleizerPreparedMesh* Prepared);

	// D=0、外力ゼロで分離した粒子を、同じ二姿勢のUE標準スキニング速度と比較する。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareSkeletalParticleVelocities(USkinnedMeshComponent* Source,
		UMeshParticleizerPreparedMesh* Prepared, UNiagaraComponent* Particles);

	// 消失材の固定座標から求めた順序とGPU候補を比較する。ノイズ以外のEditor診断用。
	UFUNCTION(BlueprintCallable, Category = "Mesh Particleizer|Diagnostics")
	static FString CompareParticleProgress(UMeshComponent* Display,
		UMeshParticleizerPreparedMesh* Prepared, UNiagaraComponent* Particles);
};
