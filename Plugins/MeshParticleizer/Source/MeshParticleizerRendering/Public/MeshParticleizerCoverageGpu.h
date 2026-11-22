#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"

class FMaterialRenderProxy;
class FSceneView;
class UMaterialInterface;

// Game Threadの準備で専用PSを要求する。非同期コンパイル中はbReady=falseで待つ。
MESHPARTICLEIZERRENDERING_API bool PrepareMeshParticleizerCoverageMaterial(UMaterialInterface* Material, bool& bReady, FString& OutError);

inline constexpr uint32 MeshParticleizerCoverageWidth = 512;

// 候補、骨、スロット入力とRHIの確保単位を含む被覆面。共有Prepared Textureは別途数える。
MESHPARTICLEIZERRENDERING_API int64 MeshParticleizerCoverageGpuBytes(uint32 Count, uint32 BoneCount, uint32 MaterialSlots);

// 描画入力を候補へ渡す索引。解像度によらず候補数で上限を決める。
MESHPARTICLEIZERRENDERING_API uint32 MeshParticleizerCoverageBucketCount(uint32 Count);
MESHPARTICLEIZERRENDERING_API int64 MeshParticleizerCoverageIndexBytes(uint32 Count);

// 動的Maskのスロットを0、それ以外を1で初期化し、後続のViewを最大値で重ねる。
MESHPARTICLEIZERRENDERING_API bool AddMeshParticleizerCoverageClearPass(FRDGBuilder& GraphBuilder,
	uint32 Count, uint32 MaterialSlots, FRDGTextureRef Positions, FRDGBufferRef DynamicSlots, FRDGTextureRef Coverage);

// GPU側の表面評価から渡す4行。位置は評価するViewのTranslated World座標。
// UV微分は画面上の三角形から求め、候補ごとの2x2補助画素へ展開する。
struct FMeshParticleizerCoverageSample
{
	FVector4f PositionAndSlot;
	FVector4f VertexNormal;
	FVector4f UVAndDdx;
	FVector4f Ddy;
};
static_assert(sizeof(FMeshParticleizerCoverageSample) == 4 * sizeof(FVector4f));

// Preparedの固定データと当フレームの姿勢。GPU資源の所有・容量予約は呼出側が持つ。
// TriangleDataは頂点UVを2行に格納し、Staticだけ位置3行・法線3行を続ける。
struct FMeshParticleizerCoverageGeometry
{
	uint32 Count = 0;
	uint32 BoneCount = 0;
	uint32 TriangleCount = 0;
	FTransform LocalToWorld = FTransform::Identity;
	FRDGTextureRef Positions = nullptr;
	FRDGTextureRef Normals = nullptr;
	FRDGTextureRef Coordinates = nullptr;
	FRDGTextureRef TriangleData = nullptr;
	FRDGTextureRef SkinVertices = nullptr;
	FRDGTextureRef SkinTriangles = nullptr;
	FRDGBufferRef Bones = nullptr;
	FRDGBufferRef DynamicSlots = nullptr;
	uint32 MaterialSlots = 0;
};

// 候補の姿勢・画面上のUV微分を作る。投影面積のない候補は無効なSlotで返す。
// SamplesはCount * sizeof(FMeshParticleizerCoverageSample)以上を確保する。
MESHPARTICLEIZERRENDERING_API bool AddMeshParticleizerCoverageGeometryPass(FRDGBuilder& GraphBuilder,
	const FSceneView& View, const FMeshParticleizerCoverageGeometry& Geometry, FRDGBufferRef Samples);

// 元三角形のUV・微分を実際の描画領域で補間し、候補へ渡す。画素がない候補は解析入力を保持。
// Heads/Linksは呼出側で予約・所有し、View間でも再利用する。CPU読戻しや一時バッファ確保はない。
MESHPARTICLEIZERRENDERING_API bool AddMeshParticleizerCoverageRasterInputPass(FRDGBuilder& GraphBuilder,
	const FSceneView& View, const FIntRect& RasterRect, const FMeshParticleizerCoverageGeometry& Geometry,
	FRDGBufferRef Samples, FRDGBufferRef Heads, FRDGBufferRef Links);

// Masked材の元のOpacity Maskを評価し、Clip値以上の候補に1を書く。
// UAV出力は呼出側で0に初期化する。複数Viewは最大値で統合する。
// 入力の材質・シェーダーが不適切ならパスを追加せずfalseを返す。
MESHPARTICLEIZERRENDERING_API bool AddMeshParticleizerCoveragePass(FRDGBuilder& GraphBuilder,
	const FSceneView& View, const FMaterialRenderProxy* MaterialProxy, uint32 Slot,
	FRDGBufferRef Samples, uint32 Count, FRDGTextureRef Coverage);

#if WITH_EDITOR
// 描画中のUV・微分を三角形番号と画素で候補へ転送するEditor診断。転送自体にCPU読戻しはない。
// Samplesは既存の解析入力で初期化する。転送できない候補はその入力を保持する。
// ScratchBytesは索引と一致回数だけ。OutLinksを指定すると未転送候補のGPU索引を確認できる。
MESHPARTICLEIZERRENDERING_API bool AddMeshParticleizerCoverageRasterInputDiagnosticPass(FRDGBuilder& GraphBuilder,
	const FSceneView& View, FRDGBufferRef Vertices, uint32 VertexCount, FRDGTextureRef CandidateNormals,
	FRDGBufferRef Samples, uint32 Count, FRDGBufferRef& OutMatches, uint64& OutScratchBytes,
	FRDGBufferRef* OutLinks = nullptr);

enum class EMeshParticleizerCoverageDiagnosticMode : uint8
{
	RasterImplicit = 1,
	RasterExplicit = 2,
	Candidate = 3
};

// 独立した描画比較用。RGBAへ被覆・raw Mask・Clip後の値・有効画素を返す。
// RasterではSamplesを3頂点単位で描く。暗黙の微分と明示したddx/ddyを分けて照合する。
// Raster入力だけは原点移動前のワールド座標。標準Local VFと同じGPU演算で原点移動する。
// RasterSamplesの読戻しは通常の候補と同じTranslated World座標で返す。
MESHPARTICLEIZERRENDERING_API bool AddMeshParticleizerCoverageDiagnosticPass(FRDGBuilder& GraphBuilder,
	const FSceneView& View, const FMaterialRenderProxy* MaterialProxy, FRDGBufferRef Samples,
	uint32 Count, FRDGTextureRef Output, EMeshParticleizerCoverageDiagnosticMode Mode,
	FRDGBufferRef RasterSamples = nullptr);
#endif
