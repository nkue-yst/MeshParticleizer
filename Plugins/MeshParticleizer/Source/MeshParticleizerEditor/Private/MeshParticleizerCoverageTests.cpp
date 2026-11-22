#include "MeshParticleizerCoverageGpu.h"
#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerCoverageValidation.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MaterialEditingLibrary.h"
#include "Math/OrthoMatrix.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MeshDescription.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "ShaderCompiler.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace MeshParticleizerCoverageTests
{
	bool RasterInputTransfer()
	{
		auto bAccepted = false;
		ENQUEUE_RENDER_COMMAND(MeshParticleizerRasterInputTest)([&bAccepted](FRHICommandListImmediate& RHICmdList) {
			auto Family = FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(nullptr, nullptr,
				FEngineShowFlags(ESFIM_Game))
					.SetTime(FGameTime::GetTimeSinceAppStart()));
			auto Options = FSceneViewInitOptions{};
			Options.ViewFamily = &Family;
			// ViewRectの原点が0以外でも、候補と実描画が同じ画素を指す。
			Options.SetViewRectangle(FIntRect(16, 24, 80, 88));
			Options.ViewRotationMatrix = FMatrix::Identity;
			Options.ProjectionMatrix = FReversedZPerspectiveMatrix(PI / 4, 1, 1, 1);
			auto View = FSceneView(Options);
			auto Uniforms = FViewUniformShaderParameters{};
			View.SetupCommonViewUniformBufferParameters(Uniforms, FIntPoint(80, 88), 1, View.UnscaledViewRect,
				View.ViewMatrices, View.ViewMatrices);
			View.ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Uniforms, UniformBuffer_SingleFrame);

			// 同じ画素に重なる前後の面と、同じ面の複数候補を別々のIDへ返す。
			auto Vertices = TArray<FMeshParticleizerCoverageSample>{};
			for (auto Triangle = 0; Triangle < 2; ++Triangle)
			{
				for (const auto P : {FVector3f(-10, -10, 10), FVector3f(10, -10, 10), FVector3f(0, 10, 10)})
				{
					auto& Vertex = Vertices.AddDefaulted_GetRef();
					Vertex.PositionAndSlot = FVector4f(P * float(Triangle + 1), 0);
					Vertex.VertexNormal = FVector4f(0, 0, 1, 0);
					Vertex.UVAndDdx = Triangle == 0 ? FVector4f(0.25f, 0.5f, 0, 0) : FVector4f(0.75f, 0.125f, 0, 0);
					Vertex.Ddy = FVector4f(0, 0, 0, 0);
				}
			}
			// 候補は面2の内側でも、その画素中心は隣の面3に入る。別面のUVで埋めない。
			const FVector3f BoundaryVertices[] = {FVector3f(-10, -10, 10), FVector3f(0.15f, -10, 10), FVector3f(0.15f, 10, 10),
				FVector3f(0.15f, -10, 10), FVector3f(10, -10, 10), FVector3f(0.15f, 10, 10)};
			for (auto Index = 0; Index < UE_ARRAY_COUNT(BoundaryVertices); ++Index)
			{
				auto& Vertex = Vertices.AddDefaulted_GetRef();
				Vertex.PositionAndSlot = FVector4f(BoundaryVertices[Index], 0);
				Vertex.VertexNormal = FVector4f(0, 0, 1, 0);
				Vertex.UVAndDdx = Index < 3 ? FVector4f(0.125f, 0.875f, 0, 0) : FVector4f(0.875f, 0.25f, 0, 0);
			}
			const FVector4f Positions[] = {FVector4f(0.15625f, -0.15625f, 10, 0), FVector4f(0.3125f, -0.3125f, 20, 0),
				FVector4f(0.15625f, -0.15625f, 10, 0), FVector4f(200, 0, 10, 0), FVector4f(0, 0, -10, 0),
				FVector4f(0, 0, 0.5f, 0), FVector4f(0, 0, 10, -1),
				FVector4f(0.149f, -0.15625f, 10, 0), FVector4f(0.15625f, -0.15625f, 10, 0)};
			constexpr auto Count = UE_ARRAY_COUNT(Positions);
			auto Inputs = TArray<FMeshParticleizerCoverageSample>{};
			FVector4f Normals[Count];
			for (auto Index = 0; Index < Count; ++Index)
			{
				Inputs.Add({Positions[Index], FVector4f(0, 0, 1, 0), FVector4f(0.9f, 0.8f, 0.7f, 0.6f), FVector4f(0.5f, 0.4f, 0.3f, 0.2f)});
				Normals[Index] = FVector4f(0, 0, 1, Index == 7 ? 2.f : Index == 8 ? 3.f
						: Index == 1                                              ? 1.f
																				  : 0.f);
			}

			auto Graph = FRDGBuilder(RHICmdList);
			auto* VertexBuffer = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Vertices.Num() * 4), TEXT("RasterInputTest.Vertices"));
			Graph.QueueBufferUpload(VertexBuffer, Vertices.GetData(), Vertices.Num() * sizeof(FMeshParticleizerCoverageSample));
			auto* Samples = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Count * 4), TEXT("RasterInputTest.Samples"));
			Graph.QueueBufferUpload(Samples, Inputs.GetData(), Inputs.Num() * sizeof(FMeshParticleizerCoverageSample));
			auto NormalRHI = RHICmdList.CreateTexture(FRHITextureCreateDesc::Create2D(TEXT("RasterInputTest.Normals"), Count, 1, PF_A32B32G32R32F)
																						  .SetFlags(TexCreate_ShaderResource)
																						  .SetInitialState(ERHIAccess::CopyDest));
			RHICmdList.UpdateTexture2D(NormalRHI, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Count, 1), sizeof(Normals), reinterpret_cast<const uint8*>(Normals));
			RHICmdList.Transition(FRHITransitionInfo(NormalRHI, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
			auto* NormalTexture = Graph.RegisterExternalTexture(CreateRenderTarget(NormalRHI, TEXT("RasterInputTest.Normals")));
			FRDGBufferRef Matches = nullptr;
			auto ScratchBytes = uint64{0};
			bAccepted = AddMeshParticleizerCoverageRasterInputDiagnosticPass(Graph, View, VertexBuffer, Vertices.Num(),
				NormalTexture, Samples, Count, Matches, ScratchBytes);
			if (bAccepted == false || Matches == nullptr)
			{
				return;
			}

			auto SampleReadback = FRHIGPUBufferReadback(TEXT("RasterInputTest.SamplesReadback"));
			auto MatchReadback = FRHIGPUBufferReadback(TEXT("RasterInputTest.MatchesReadback"));
			AddEnqueueCopyPass(Graph, &SampleReadback, Samples, Count * sizeof(FMeshParticleizerCoverageSample));
			AddEnqueueCopyPass(Graph, &MatchReadback, Matches, Count * sizeof(uint32));
			Graph.Execute();
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			const auto* Actual = static_cast<const FMeshParticleizerCoverageSample*>(SampleReadback.Lock(Count * sizeof(FMeshParticleizerCoverageSample)));
			const auto* Hits = static_cast<const uint32*>(MatchReadback.Lock(Count * sizeof(uint32)));
			bAccepted &= ScratchBytes == 308;
			for (auto Index = 0; Index < Count; ++Index)
			{
				const auto bTransferred = Index < 3 || Index == 8;
				bAccepted &= Hits[Index] == (bTransferred ? 1u : 0u);
				bAccepted &= FMemory::Memcmp(&Actual[Index], &Inputs[Index], sizeof(FVector4f) * 2) == 0;
				if (bTransferred == true)
				{
					const auto ExpectedUV = Index == 8 ? FVector4f(0.875f, 0.25f, 0, 0) : Index == 1 ? FVector4f(0.75f, 0.125f, 0, 0)
																									 : FVector4f(0.25f, 0.5f, 0, 0);
					bAccepted &= Actual[Index].UVAndDdx.Equals(ExpectedUV, 0.000001f) &&
						Actual[Index].Ddy.Equals(FVector4f(0, 0, 0.3f, 0.2f), 0.000001f);
				}
				else
				{
					bAccepted &= FMemory::Memcmp(&Actual[Index], &Inputs[Index], sizeof(FMeshParticleizerCoverageSample)) == 0;
				}
			}
			MatchReadback.Unlock();
			SampleReadback.Unlock();
		});
		FlushRenderingCommands();
		return bAccepted;
	}

	// 評価パスだけを検証する。実行時のスキニングと候補選択は別の結合検証で扱う。
	bool Render(const FMaterialRenderProxy* Proxy, bool bSecondView, TArray<FLinearColor>& OutPixels)
	{
		auto bAccepted = false;
		ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageTest)([Proxy, bSecondView, &OutPixels, &bAccepted](FRHICommandListImmediate& RHICmdList) {
			auto Family = FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(nullptr, nullptr,
				FEngineShowFlags(ESFIM_Game))
					.SetTime(FGameTime::GetTimeSinceAppStart()));
			auto Options = FSceneViewInitOptions{};
			Options.ViewFamily = &Family;
			Options.SetViewRectangle(FIntRect(0, 0, 64, 64));
			Options.ViewRotationMatrix = FMatrix::Identity;
			Options.ProjectionMatrix = FReversedZPerspectiveMatrix(PI / 4, 1, 1, 1);
			auto View = FSceneView(Options);
			auto Uniforms = FViewUniformShaderParameters{};
			View.SetupCommonViewUniformBufferParameters(Uniforms, FIntPoint(64, 64), 1, View.UnscaledViewRect,
				View.ViewMatrices, View.ViewMatrices);
			View.ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Uniforms, UniformBuffer_SingleFrame);

			auto Graph = FRDGBuilder(RHICmdList);
			const FMeshParticleizerCoverageSample Points[] =
				{
				{FVector4f(0, 0, -100, 0), FVector4f(0, 0, 1, 0), FVector4f(0.25f, 0.5f, 0.01f, 0), FVector4f(0, 0.01f, 0, 0)},
				{FVector4f(0, 0, -100, 0), FVector4f(0, 0, 1, 0), FVector4f(0.75f, 0.5f, 0.01f, 0), FVector4f(0, 0.01f, 0, 0)},
				{FVector4f(0, 0, -100, 0), FVector4f(1, 0, 0, 0), FVector4f(0.75f, 0.5f, 0.01f, 0), FVector4f(0, 0.01f, 0, 0)},
				{FVector4f(0, 0, -100, 1), FVector4f(0, 0, 1, 0), FVector4f(0.75f, 0.5f, 0.01f, 0), FVector4f(0, 0.01f, 0, 0)},
					{FVector4f(0, 0, -100, -1), FVector4f(0, 0, 1, 0), FVector4f(0.75f, 0.5f, 0.01f, 0), FVector4f(0, 0.01f, 0, 0)}};
			constexpr auto Count = UE_ARRAY_COUNT(Points);
			auto* Samples = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Count * 4), TEXT("MeshParticleizer.CoverageTestSamples"));
			Graph.QueueBufferUpload(Samples, Points, sizeof(Points));
			auto* Output = Graph.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(Count, 1), PF_R32_FLOAT,
												   FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV),
				TEXT("MeshParticleizer.CoverageTest"));
			AddClearRenderTargetPass(Graph, Output, FLinearColor::Black);
			bAccepted = AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, Samples, Count, Output);

			if (bSecondView == true)
			{
				auto OtherPoints = TArray<FMeshParticleizerCoverageSample>(Points, UE_ARRAY_COUNT(Points));
				for (auto& Point : OtherPoints)
				{
					Point.PositionAndSlot.X = -100;
					Point.PositionAndSlot.Z = 0;
				}
				auto* OtherSamples = Graph.CreateBuffer(Samples->Desc, TEXT("MeshParticleizer.CoverageTestOtherView"));
				Graph.QueueBufferUpload(OtherSamples, OtherPoints.GetData(), OtherPoints.Num() * sizeof(FMeshParticleizerCoverageSample));
				bAccepted &= AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, OtherSamples, Count, Output);
			}

			TRefCountPtr<IPooledRenderTarget> Readback;
			Graph.QueueTextureExtraction(Output, &Readback);
			Graph.Execute();
			auto Flags = FReadSurfaceDataFlags(RCM_MinMax);
			Flags.SetLinearToGamma(false);
			RHICmdList.ReadSurfaceData(Readback->GetRHI(), FIntRect(0, 0, Count, 1), OutPixels, Flags);
		});
		FlushRenderingCommands();
		return bAccepted;
	}

	enum class EGeometryView : uint8
	{
		Front,
		Back,
		Side,
		Horizon,
		Orthographic
	};

	bool Geometry(bool bSkinned, EGeometryView ViewCase, TArray<FMeshParticleizerCoverageSample>& OutSamples,
		FVector3f& OutPosition, FVector3f& OutNormal, FVector4f& OutDerivatives)
	{
		auto bAccepted = false;
		ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageGeometryTest)([bSkinned, ViewCase, &OutSamples, &OutPosition,
																		 &OutNormal, &OutDerivatives, &bAccepted](FRHICommandListImmediate& RHICmdList) {
			const auto Origin = ViewCase == EGeometryView::Horizon ? FVector::ZeroVector : FVector(1.e10, -2.e10, 3.e10);
			auto Component = FTransform(FQuat::Identity, Origin, FVector(2, 3, 0.5));
			auto Family = FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(nullptr, nullptr,
				FEngineShowFlags(ESFIM_Game))
					.SetTime(FGameTime::GetTimeSinceAppStart()));
			auto Options = FSceneViewInitOptions{};
			Options.ViewFamily = &Family;
			Options.ViewOrigin = Origin;
			Options.SetViewRectangle(FIntRect(0, 0, 64, 64));
			Options.ViewRotationMatrix = FMatrix::Identity;
			Options.ProjectionMatrix = FReversedZPerspectiveMatrix(PI / 4, 1, 1, 1);
			if (ViewCase == EGeometryView::Back)
			{
				Options.ViewRotationMatrix.M[0][0] = -1;
				Options.ViewRotationMatrix.M[2][2] = -1;
			}
			else if (ViewCase == EGeometryView::Side)
			{
				Options.ViewRotationMatrix = FMatrix(FPlane(0, 0, 1, 0), FPlane(0, 1, 0, 0),
					FPlane(-1, 0, 0, 0), FPlane(0, 0, 0, 1));
			}
			else if (ViewCase == EGeometryView::Orthographic)
			{
				Options.ProjectionMatrix = FReversedZOrthoMatrix(10, 10, 0.01, 0);
			}
			auto View = FSceneView(Options);
			auto Uniforms = FViewUniformShaderParameters{};

			// 内部解像度を半分にし、UnscaledViewRectを微分に誤用していないことも確認する。
			View.SetupCommonViewUniformBufferParameters(Uniforms, FIntPoint(32, 32), 1, FIntRect(0, 0, 32, 32),
				View.ViewMatrices, View.ViewMatrices);
			View.ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Uniforms, UniformBuffer_SingleFrame);

			const FVector3f Reference[] = {FVector3f(-1, -1, 10), FVector3f(1, -1, 11), FVector3f(-1, 1, 12)};
			const auto ReferenceNormal = FVector3f(1, 0, 1).GetSafeNormal();

			// 副画素の丸め境界と原点変換の精度を混同しないよう、移動は2進数で正確な値にする。
			const auto Pose = FTransform3f(FQuat4f(FVector3f(0, 1, 0), 0.7f), FVector3f(0.125f, 0, 0.25f)).ToMatrixWithScale();
			const float BoneWeights[] = {0.2f, 0.7f, 1.f};
			const auto Bary = FVector3f(0.2f, 0.3f, 0.5f);
			FVector3f Posed[3], Normals[3];
			for (auto Corner = 0; Corner < 3; ++Corner)
			{
				Posed[Corner] = bSkinned ? FMath::Lerp(Reference[Corner], FVector3f(Pose.TransformPosition(Reference[Corner])), BoneWeights[Corner]) : Reference[Corner];
				Normals[Corner] = bSkinned ? FMath::Lerp(ReferenceNormal, FVector3f(Pose.TransformVector(ReferenceNormal)), BoneWeights[Corner]).GetSafeNormal() : ReferenceNormal;
			}
			const auto Local = Posed[0] * Bary.X + Posed[1] * Bary.Y + Posed[2] * Bary.Z;
			// w=0の候補も、カメラを含まない面上ならUV微分の極限は0になる。
			const auto Offset = ViewCase == EGeometryView::Horizon ? FVector3f(0, 0, -Local.Z * 0.5f) : FVector3f::ZeroVector;
			Component.SetTranslation(Origin + FVector(Offset));
			// CPU比較は巨大な絶対座標への加算を避け、期待する相対座標を直接求める。
			OutPosition = Local * FVector3f(2, 3, 0.5) + Offset;
			OutNormal = (Normals[0] * Bary.X + Normals[1] * Bary.Y + Normals[2] * Bary.Z).GetSafeNormal();

			// double精度の画面補間を参照にし、副画素・画素中心・coarse微分を照合する。
			FVector4d Clip[3];
			FVector2d Screen[3];
			FVector3d OriginalWeights[] = {FVector3d(1, 0, 0), FVector3d(0, 1, 0), FVector3d(0, 0, 1)};
			const auto CenterClip = View.ViewMatrices.GetTranslatedWorldToClip().TransformFVector4(FVector4d(FVector3d(OutPosition), 1));
			auto bRaster = CenterClip.W > 0 && CenterClip.Z <= CenterClip.W;
			for (auto Corner = 0; Corner < 3; ++Corner)
			{
				Clip[Corner] = View.ViewMatrices.GetTranslatedWorldToClip().TransformFVector4(FVector4d(FVector3d(Posed[Corner] * FVector3f(2, 3, 0.5) + Offset), 1));
				Screen[Corner] = FVector2d(Clip[Corner].X, -Clip[Corner].Y) * (16.0 / Clip[Corner].W) + FVector2d(16, 16);
			}
			auto TriangleWeights = [&](FVector2d Pixel) {
				const auto A = Screen[1] - Screen[0];
				const auto B = Screen[2] - Screen[0];
				const auto P = Pixel - Screen[0];
				const auto Det = A.X * B.Y - A.Y * B.X;
				auto Weights = FVector3d(0, (P.X * B.Y - P.Y * B.X) / Det, (A.X * P.Y - A.Y * P.X) / Det);
				Weights.X = 1 - Weights.Y - Weights.Z;
				Weights /= FVector3d(Clip[0].W, Clip[1].W, Clip[2].W);
				Weights /= Weights.X + Weights.Y + Weights.Z;
				return Weights;
			};
			if (bRaster == true)
			{
				// CPU参照も近面で切り出す。新しい頂点は元3頂点の重みとして保持する。
				struct FCorner
				{
					FVector4d Clip;
					FVector3d Weights;
				};
				auto Polygon = TArray<FCorner>{};
				for (auto Corner = 0; Corner < 3; ++Corner)
				{
					const auto Previous = (Corner + 2) % 3;
					const auto A = Clip[Previous].W - Clip[Previous].Z;
					const auto B = Clip[Corner].W - Clip[Corner].Z;
					if ((A >= 0) != (B >= 0))
					{
						const auto Alpha = A / (A - B);
						Polygon.Add({FMath::Lerp(Clip[Previous], Clip[Corner], Alpha), FMath::Lerp(OriginalWeights[Previous], OriginalWeights[Corner], Alpha)});
					}
					if (B >= 0)
					{
						Polygon.Add({Clip[Corner], OriginalWeights[Corner]});
					}
				}
				bRaster = Polygon.Num() >= 3;
				const auto Center = FVector2d(CenterClip.X, -CenterClip.Y) * (16.0 / CenterClip.W) + FVector2d(16, 16);
				const auto Pixel = FVector2d(FMath::FloorToDouble(Center.X) + 0.5, FMath::FloorToDouble(Center.Y) + 0.5);
				for (auto Fan = 1; Fan + 1 < Polygon.Num(); ++Fan)
				{
					const int32 Indices[] = {0, Fan, Fan + 1};
					for (auto Corner = 0; Corner < 3; ++Corner)
					{
						Clip[Corner] = Polygon[Indices[Corner]].Clip;
						OriginalWeights[Corner] = Polygon[Indices[Corner]].Weights;
						Screen[Corner] = FVector2d(Clip[Corner].X, -Clip[Corner].Y) * (16.0 / Clip[Corner].W) + FVector2d(16, 16);
						Screen[Corner].X = FMath::RoundHalfToEven(Screen[Corner].X * 256) / 256;
						Screen[Corner].Y = FMath::RoundHalfToEven(Screen[Corner].Y * 256) / 256;
					}
					if (TriangleWeights(Pixel).GetMin() >= 0)
					{
						break;
					}
				}
			}
			auto AtPixel = [&](FVector2d Pixel) {
				const auto Weights = TriangleWeights(Pixel);
				return OriginalWeights[0] * Weights.X + OriginalWeights[1] * Weights.Y + OriginalWeights[2] * Weights.Z;
			};
			OutDerivatives = FVector4f(0, 0, 0, 0);
			if (ViewCase != EGeometryView::Horizon)
			{
				const auto Center = FVector2d(CenterClip.X, -CenterClip.Y) * (16.0 / CenterClip.W) + FVector2d(16, 16);
				auto Dx = FVector3d::ZeroVector;
				auto Dy = FVector3d::ZeroVector;
				if (bRaster == true)
				{
					const auto Pixel = FVector2d(FMath::FloorToDouble(Center.X) + 0.5, FMath::FloorToDouble(Center.Y) + 0.5);
					const auto Quad = FVector2d(FMath::FloorToDouble(Pixel.X * 0.5) * 2 + 0.5, FMath::FloorToDouble(Pixel.Y * 0.5) * 2 + 0.5);
					const auto Weights = FVector3f(AtPixel(Pixel));
					OutPosition = (Posed[0] * Weights.X + Posed[1] * Weights.Y + Posed[2] * Weights.Z) * FVector3f(2, 3, 0.5) + Offset;
					OutNormal = (Normals[0] * Weights.X + Normals[1] * Weights.Y + Normals[2] * Weights.Z).GetSafeNormal();
					Dx = AtPixel(Quad + FVector2d(1, 0)) - AtPixel(Quad);
					Dy = AtPixel(Quad + FVector2d(0, 1)) - AtPixel(Quad);
				}
				else
				{
					Dx = (AtPixel(Center + FVector2d(0.0001, 0)) - AtPixel(Center - FVector2d(0.0001, 0))) / 0.0002;
					Dy = (AtPixel(Center + FVector2d(0, 0.0001)) - AtPixel(Center - FVector2d(0, 0.0001))) / 0.0002;
				}
				OutDerivatives = FVector4f(Dx.Y, Dx.Z, Dy.Y, Dy.Z);
			}

			auto Graph = FRDGBuilder(RHICmdList);
			auto Texture = [&](const TCHAR* Name, const TArray<FVector4f>& Rows) {
				auto RHI = RHICmdList.CreateTexture(FRHITextureCreateDesc::Create2D(Name, Rows.Num(), 1, PF_A32B32G32R32F)
						.SetFlags(TexCreate_ShaderResource)
						.SetInitialState(ERHIAccess::CopyDest));
				RHICmdList.UpdateTexture2D(RHI, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Rows.Num(), 1), Rows.Num() * sizeof(FVector4f), reinterpret_cast<const uint8*>(Rows.GetData()));
				RHICmdList.Transition(FRHITransitionInfo(RHI, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				return Graph.RegisterExternalTexture(CreateRenderTarget(RHI, Name));
			};
			auto Input = FMeshParticleizerCoverageGeometry{};
			Input.Count = 4;
			Input.LocalToWorld = Component;
			const auto RefPoint = Reference[0] * Bary.X + Reference[1] * Bary.Y + Reference[2] * Bary.Z;
			Input.Positions = Texture(TEXT("CoverageTest.Position"), {FVector4f(RefPoint, 2), FVector4f(RefPoint, 2), FVector4f(RefPoint, 2), FVector4f(RefPoint, 2)});
			Input.Normals = Texture(TEXT("CoverageTest.Normal"), {FVector4f(ReferenceNormal, 0), FVector4f(ReferenceNormal, 1), FVector4f(ReferenceNormal, 2), FVector4f(ReferenceNormal, 99)});
			const auto UV = FVector4f(Bary.Y, Bary.Z, Bary.X, Bary.Y);
			Input.Coordinates = Texture(TEXT("CoverageTest.UV"), {UV, UV, UV, UV});
			const auto UV01 = FVector4f(0, 0, 1, 0);
			const auto UV2 = FVector4f(0, 1, 2, 1);
			if (bSkinned)
			{
				Input.BoneCount = 2;
				Input.TriangleData = Texture(TEXT("CoverageTest.Edges"), {UV01, UV2, UV01, FVector4f(2, 0, 2, 1), UV01, UV2});
				Input.SkinTriangles = Texture(TEXT("CoverageTest.Triangles"), {FVector4f(0, 1, 2, 2), FVector4f(0, 1, 2, 2), FVector4f(0, 0, 0, 2)});
				auto Vertices = TArray<FVector4f>{};
				Vertices.SetNumZeroed(24);
				for (auto Corner = 0; Corner < 3; ++Corner)
				{
					Vertices[Corner * 8] = FVector4f(Reference[Corner], 1);
					Vertices[Corner * 8 + 1] = FVector4f(ReferenceNormal, 0);
					Vertices[Corner * 8 + 2] = FVector4f(0, 1, 0, 0);
					Vertices[Corner * 8 + 3] = FVector4f(1 - BoneWeights[Corner], BoneWeights[Corner], 0, 0);
				}
				Input.SkinVertices = Texture(TEXT("CoverageTest.Vertices"), Vertices);
				const FVector4f Bones[] = {FVector4f(1, 0, 0, 0), FVector4f(0, 1, 0, 0), FVector4f(0, 0, 1, 0),
					FVector4f(Pose.M[0][0], Pose.M[1][0], Pose.M[2][0], Pose.M[3][0]),
					FVector4f(Pose.M[0][1], Pose.M[1][1], Pose.M[2][1], Pose.M[3][1]),
					FVector4f(Pose.M[0][2], Pose.M[1][2], Pose.M[2][2], Pose.M[3][2])};
				Input.Bones = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 6), TEXT("CoverageTest.Bones"));
				Graph.QueueBufferUpload(Input.Bones, Bones, sizeof(Bones));
			}
			else
			{
				auto Rows = TArray<FVector4f>{};
				for (auto Triangle = 0; Triangle < 3; ++Triangle)
				{
					Rows.Add(UV01);
					Rows.Add(Triangle == 1 ? FVector4f(2, 0, 2, 1) : UV2);
					Rows.Add(FVector4f(Reference[0], 0));
					Rows.Add(FVector4f(Reference[1], 0));
					Rows.Add(FVector4f(Reference[Triangle == 2 ? 1 : 2], 0));
					for (auto Corner = 0; Corner < 3; ++Corner)
					{
						Rows.Add(FVector4f(ReferenceNormal, 0));
					}
				}
				Input.TriangleData = Texture(TEXT("CoverageTest.TriangleData"), Rows);
			}
			auto* Output = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 16), TEXT("CoverageTest.Geometry"));
			bAccepted = AddMeshParticleizerCoverageGeometryPass(Graph, View, Input, Output);
			Input.TriangleCount = 3;
			Input.MaterialSlots = 3;
			const uint32 Slots[] = {0, 0, 1};
			Input.DynamicSlots = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 3), TEXT("CoverageTest.Slots"));
			Graph.QueueBufferUpload(Input.DynamicSlots, Slots, sizeof(Slots));
			auto* Heads = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshParticleizerCoverageBucketCount(Input.Count)), TEXT("CoverageTest.Heads"));
			auto* Links = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FUintVector4), Input.Count), TEXT("CoverageTest.Links"));
			bAccepted &= AddMeshParticleizerCoverageRasterInputPass(Graph, View, FIntRect(0, 0, 32, 32), Input, Output, Heads, Links);
			auto Readback = FRHIGPUBufferReadback(TEXT("CoverageTest.Readback"));
			AddEnqueueCopyPass(Graph, &Readback, Output, 4 * sizeof(FMeshParticleizerCoverageSample));
			Graph.Execute();
			// Fenceはコマンドを提出しない。同期診断では提出を済ませてから待つ。
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			OutSamples.SetNum(4);
			FMemory::Memcpy(OutSamples.GetData(), Readback.Lock(4 * sizeof(FMeshParticleizerCoverageSample)), 4 * sizeof(FMeshParticleizerCoverageSample));
			Readback.Unlock();
		});
		FlushRenderingCommands();
		return bAccepted;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerCoverageTest, "MeshParticleizer.Editor.MaterialCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerCoverageTest::RunTest(const FString& Parameters)
{
	using namespace MeshParticleizerCoverageTests;
	this->TestTrue(TEXT("Raster inputs preserve overlapping triangles, duplicate candidates, nonzero view origins and unmatched boundary inputs"), RasterInputTransfer());
	auto Material = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
	Material->BlendMode = BLEND_Masked;
	Material->OpacityMaskClipValue = 0.5f;
	auto* UV = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureCoordinate::StaticClass()));
	auto* Red = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionComponentMask::StaticClass()));
	Red->Input.Connect(0, UV);
	Red->R = true;
	Red->G = false;
	auto* Camera = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionCameraVectorWS::StaticClass());
	auto* Normal = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVertexNormalWS::StaticClass());
	auto* Dot = Cast<UMaterialExpressionDotProduct>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionDotProduct::StaticClass()));
	Dot->A.Connect(0, Camera);
	Dot->B.Connect(0, Normal);
	auto* Mask = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMultiply::StaticClass()));
	Mask->A.Connect(0, Dot);
	Mask->B.Connect(0, Red);
	auto* Gain = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionScalarParameter::StaticClass()));
	Gain->ParameterName = TEXT("CoverageGain");
	Gain->DefaultValue = 1;
	auto* Result = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMultiply::StaticClass()));
	Result->A.Connect(0, Mask);
	Result->B.Connect(0, Gain);
	auto* Progress = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionScalarParameter::StaticClass()));
	Progress->ParameterName = TEXT("MP_Progress");
	Progress->DefaultValue = -1;
	auto* Dissolve = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionCustom::StaticClass()));
	Dissolve->Inputs.SetNum(2);
	Dissolve->Inputs[0].InputName = TEXT("Mask");
	Dissolve->Inputs[0].Input.Connect(0, Result);
	Dissolve->Inputs[1].InputName = TEXT("Progress");
	Dissolve->Inputs[1].Input.Connect(0, Progress);
	Dissolve->OutputType = CMOT_Float1;
	Dissolve->Code = TEXT("#if MP_EVALUATE_COVERAGE\nreturn Mask;\n#else\nreturn Mask * (Progress < 0 ? 1 : 0);\n#endif\n");
	Material->GetEditorOnlyData()->OpacityMask.Connect(0, Dissolve);
	Material->SetMaterialUsage(MATUSAGE_NiagaraMeshParticles);
	UMaterialEditingLibrary::RecompileMaterial(Material.Get());
	GShaderCompilingManager->FinishAllCompilation();
	auto Instance = TStrongObjectPtr<UMaterialInstanceDynamic>(UMaterialInstanceDynamic::Create(Material.Get(), GetTransientPackage()));

	auto Pixels = TArray<FLinearColor>{};
	if (this->TestTrue(TEXT("Compiled original material evaluates on GPU"), Render(Instance->GetRenderProxy(), false, Pixels)) == false ||
			this->TestEqual(TEXT("Five coverage samples read back"), Pixels.Num(), 5) == false)
	{
		return false;
	}
	this->TestEqual(TEXT("UV hole remains empty"), Pixels[0].R, 0.f);
	this->TestEqual(TEXT("Front-facing visible surface survives clip"), Pixels[1].R, 1.f);
	this->TestEqual(TEXT("Camera/normal-dependent hole remains empty"), Pixels[2].R, 0.f);
	this->TestEqual(TEXT("Other slot is not evaluated"), Pixels[3].R, 0.f);
	this->TestEqual(TEXT("Invalid slot is not evaluated as slot zero"), Pixels[4].R, 0.f);

	this->TestTrue(TEXT("Two views evaluate successfully"), Render(Instance->GetRenderProxy(), true, Pixels));
	this->TestEqual(TEXT("First view remains covered"), Pixels[1].R, 1.f);
	this->TestEqual(TEXT("Second view adds coverage"), Pixels[2].R, 1.f);
	Instance->SetScalarParameterValue(TEXT("MP_Progress"), 1);
	this->TestTrue(TEXT("Display MID can be evaluated after full dissolve"), Render(Instance->GetRenderProxy(), false, Pixels));
	this->TestTrue(TEXT("Coverage ignores only the plugin dissolve"), Pixels.Num() == 5 && Pixels[0].R == 0 && Pixels[1].R == 1 && Pixels[2].R == 0 && Pixels[3].R == 0 && Pixels[4].R == 0);
	Instance->SetScalarParameterValue(TEXT("CoverageGain"), 0);
	this->TestTrue(TEXT("Updated uniform is evaluated"), Render(Instance->GetRenderProxy(), false, Pixels));
	this->TestTrue(TEXT("Zero gain removes all coverage"), Pixels.Num() == 5 && Pixels.ContainsByPredicate([](const FLinearColor& Color) {
		return Color.R != 0;
	}) == false);

	// 定数1でも生成する消失材と同じ設定でMaskedを維持し、専用PSを用意する。
	auto Constant = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
	Constant->BlendMode = BLEND_Masked;
	Constant->bCanMaskedBeAssumedOpaque = true;
	UMaterialEditingLibrary::RecompileMaterial(Constant.Get());
	GShaderCompilingManager->FinishAllCompilation();
	this->TestFalse(TEXT("Missing coverage shader is not replaced by a fallback"), Render(Constant->GetRenderProxy(), false, Pixels));
	Constant->bCanMaskedBeAssumedOpaque = false;
	Constant->SetMaterialUsage(MATUSAGE_NiagaraMeshParticles);
	UMaterialEditingLibrary::RecompileMaterial(Constant.Get());
	GShaderCompilingManager->FinishAllCompilation();
	this->TestTrue(TEXT("Prepared constant Masked variant compiles"), Render(Constant->GetRenderProxy(), false, Pixels));
	this->TestTrue(TEXT("Constant mask keeps all matching samples"), Pixels.Num() == 5 && Pixels[0].R == 1 && Pixels[1].R == 1 && Pixels[2].R == 1 && Pixels[3].R == 0 && Pixels[4].R == 0);

	for (const auto bSkinned : {false, true})
	{
		for (const auto ViewCase : {EGeometryView::Front, EGeometryView::Back, EGeometryView::Side,
			EGeometryView::Horizon, EGeometryView::Orthographic})
		{
			auto Samples = TArray<FMeshParticleizerCoverageSample>{};
			auto Position = FVector3f::ZeroVector;
			auto ExpectedNormal = FVector3f::ZeroVector;
			auto Derivatives = FVector4f(0, 0, 0, 0);
			const auto Prefix = FString::Printf(TEXT("%s view %d: "), bSkinned ? TEXT("Skeletal") : TEXT("Static"), int32(ViewCase));
			if (this->TestTrue(Prefix + TEXT("GPU geometry evaluates"), Geometry(bSkinned, ViewCase, Samples, Position, ExpectedNormal, Derivatives)) == false ||
					this->TestEqual(Prefix + TEXT("Four samples returned"), Samples.Num(), 4) == false)
				{
					return false;
				}
			this->TestTrue(Prefix + FString::Printf(TEXT("Large world position preserves local precision: GPU %.9g %.9g %.9g, CPU %.9g %.9g %.9g"), Samples[0].PositionAndSlot.X, Samples[0].PositionAndSlot.Y, Samples[0].PositionAndSlot.Z, Position.X, Position.Y, Position.Z), FVector3f(Samples[0].PositionAndSlot).Equals(Position, 0.00002f));
		this->TestTrue(Prefix + TEXT("Vertex normal follows UE normalization and no-scale transform"), FVector3f(Samples[0].VertexNormal).Equals(ExpectedNormal, 0.00002f));
			const auto Actual = FVector4f(Samples[0].UVAndDdx.Z, Samples[0].UVAndDdx.W, Samples[0].Ddy.X, Samples[0].Ddy.Y);
			this->TestTrue(Prefix + TEXT("Projected UV gradients match raster quads or the non-raster limit"), Actual.Equals(Derivatives, 0.00002f));
			this->TestEqual(Prefix + TEXT("Material slot survives"), Samples[0].PositionAndSlot.W, 2.f);
			this->TestEqual(Prefix + TEXT("Degenerate UV is still evaluable"), Samples[1].PositionAndSlot.W, 2.f);
			this->TestTrue(Prefix + TEXT("Constant V has zero gradients"), FMath::Abs(Samples[1].UVAndDdx.W) < 0.000001f && FMath::Abs(Samples[1].Ddy.Y) < 0.000001f);
			this->TestEqual(Prefix + TEXT("Degenerate projected triangle is excluded"), Samples[2].PositionAndSlot.W, -1.f);
			this->TestEqual(Prefix + TEXT("Out-of-range triangle is excluded"), Samples[3].PositionAndSlot.W, -1.f);
		}
	}

	// 通常のメッシュ描画を独立した参照にし、穴のある材と全被覆の両方を確認する。
	auto Dither = TStrongObjectPtr<UMaterial>(DuplicateObject<UMaterial>(Material.Get(), GetTransientPackage()));
	Dither->DitherOpacityMask = true;
	UMaterialEditingLibrary::RecompileMaterial(Dither.Get());
	GShaderCompilingManager->FinishAllCompilation();
	const auto Directory = FPaths::ProjectSavedDir() / TEXT("Tests/MaterialCoverage");
	IFileManager::Get().MakeDirectory(*Directory, true);
	for (auto* Original : {Material.Get(), Constant.Get(), Dither.Get()})
	{
		const auto Name = Original == Dither.Get() ? TEXT("Dither") : Original == Material.Get() ? TEXT("UVMask") : TEXT("Constant");
		const auto Json = UMeshParticleizerPreparationLibrary::CompareMaterialCoverageRender(Original,
			FVector::UpVector, Directory / (FString(Name) + TEXT(".png")), 64);
		auto Report = TSharedPtr<FJsonObject>{};
		if (this->TestTrue(FString(Name) + TEXT(" standard renderer comparison completes"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report) && Report.IsValid()) == false)
		{
			this->AddError(Json);
			return false;
		}
		this->TestTrue(FString(Name) + TEXT(" reference renderer produced covered pixels"), Report->GetNumberField(TEXT("referenceCovered")) > 0);
		this->TestTrue(FString(Name) + TEXT(" mask agrees with standard rasterization"), Report->GetBoolField(TEXT("exactMatch")));
		this->TestEqual(FString(Name) + TEXT(" raster probe agrees with the independent renderer"), Report->GetIntegerField(TEXT("rasterReferenceMismatches")), 0);
		this->TestEqual(FString(Name) + TEXT(" explicit gradient probe agrees with the independent renderer"), Report->GetIntegerField(TEXT("explicitReferenceMismatches")), 0);
		this->TestEqual(FString(Name) + TEXT(" captured raster inputs agree when replayed as candidates"), Report->GetIntegerField(TEXT("replayedReferenceMismatches")), 0);
		this->TestEqual(FString(Name) + TEXT(" GPU raster input transfer agrees with independent rendering"), Report->GetIntegerField(TEXT("rasterTransferReferenceMismatches")), 0);
		this->TestEqual(FString(Name) + TEXT(" GPU raster input transfer finds every eligible candidate"), Report->GetIntegerField(TEXT("rasterTransferMissing")), 0);
		this->TestEqual(FString(Name) + TEXT(" GPU raster input transfer writes each candidate once"), Report->GetIntegerField(TEXT("rasterTransferMultiple")), 0);
		this->TestEqual(FString(Name) + TEXT(" GPU raster input transfer keeps positions and normals"), Report->GetIntegerField(TEXT("rasterTransferPositionNormalChanges")), 0);
		this->TestEqual(FString(Name) + TEXT(" GPU raster input transfer keeps unmatched inputs"), Report->GetIntegerField(TEXT("rasterTransferUnmatchedChanges")), 0);
		this->TestEqual(FString(Name) + TEXT(" diagnostic output preserves the runtime result"), Report->GetIntegerField(TEXT("diagnosticCandidateMismatches")), 0);
	}
	// 原点・水平面に依存しない比較を保ち、細い面のUV継ぎ目を一緒に描いて検査する。
	auto Patch = TStrongObjectPtr<UStaticMesh>(NewObject<UStaticMesh>());
	Patch->GetStaticMaterials().Add(FStaticMaterial(Material.Get(), TEXT("Source"), TEXT("Source")));
	auto Description = FMeshDescription{};
	auto Attributes = FStaticMeshAttributes(Description);
	Attributes.Register();
	auto PatchUVs = Attributes.GetVertexInstanceUVs();
	PatchUVs.SetNumChannels(1);
	const auto Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Source");
	const auto Pose = FTransform(FRotator(20, 35, -10), FVector(11, -7, 150));
	// UEの片面描画で法線側が表になる頂点順を使う。
	const FVector2f Corners[] = {{0, 0}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}};
	for (auto Triangle = 0; Triangle < 2; ++Triangle)
	{
		auto Instances = TArray<FVertexInstanceID>{};
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto PatchUV = Corners[Triangle * 3 + Corner];
			const auto Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions()[Vertex] = FVector3f(Pose.TransformPosition(FVector(PatchUV.X * 60 - 30, PatchUV.Y * 4 - 2, 0)));
			const auto VertexInstance = Description.CreateVertexInstance(Vertex);
			Attributes.GetVertexInstanceNormals()[VertexInstance] = FVector3f(Pose.TransformVectorNoScale(FVector::UpVector));
			Attributes.GetVertexInstanceTangents()[VertexInstance] = FVector3f(Pose.TransformVectorNoScale(FVector::ForwardVector));
			Attributes.GetVertexInstanceBinormalSigns()[VertexInstance] = 1;
			Attributes.GetVertexInstanceColors()[VertexInstance] = FVector4f(1, 1, 1, 1);
			PatchUVs.Set(VertexInstance, 0, PatchUV + FVector2f(Triangle * 0.4f, 0));
			Instances.Add(VertexInstance);
		}
		Description.CreatePolygon(Group, Instances);
	}
	auto Build = UStaticMesh::FBuildMeshDescriptionsParams{};
	Build.bFastBuild = true;
	Build.bMarkPackageDirty = false;
	Build.bCommitMeshDescription = false;
	Build.bAllowCpuAccess = true;
	if (this->TestTrue(TEXT("Tilted thin seam fixture builds"), Patch->BuildFromMeshDescriptions({&Description}, Build)) == false)
	{
		return false;
	}
	const auto PatchJson = MeshParticleizerCoverageValidation::CompareRender(Material.Get(), Patch.Get(),
		Pose.TransformVectorNoScale(FVector::UpVector), Pose.GetLocation(), Directory / TEXT("ThinUVSeam.png"), 128, 80);
	FFileHelper::SaveStringToFile(PatchJson, *(Directory / TEXT("ThinUVSeam.json")));
	auto PatchReport = TSharedPtr<FJsonObject>{};
	if (this->TestTrue(TEXT("Tilted thin seam comparison completes"),
						   FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PatchJson), PatchReport) && PatchReport.IsValid()) == false)
	{
		this->AddError(PatchJson);
		return false;
	}
	this->TestTrue(TEXT("Thin seam fixture has covered and transparent samples"),
		PatchReport->GetIntegerField(TEXT("referenceCovered")) > 0 && PatchReport->GetIntegerField(TEXT("referenceCovered")) < PatchReport->GetIntegerField(TEXT("comparedPixels")));
	this->TestEqual(TEXT("Seam fixture has no overlapping triangle interiors"), PatchReport->GetIntegerField(TEXT("overlappingInteriorPixels")), 0);
	this->TestTrue(TEXT("Translated tilted thin triangles and UV seam agree including edges"), PatchReport->GetBoolField(TEXT("wholeImageExactMatch")));
	this->TestEqual(TEXT("Thin seam input transfer preserves positions and normals"), PatchReport->GetIntegerField(TEXT("rasterTransferPositionNormalChanges")), 0);

	// 視点で穴が変わる材を独立描画と照合し、共有バッファの合成と逆順評価を検査する。
	const auto MultiViewJson = UMeshParticleizerPreparationLibrary::CompareMaterialCoverageViews(Material.Get(),
		{FVector::UpVector, FVector(FMath::Sqrt(3.0) / 2, 0, 0.5)}, Directory / TEXT("MultiView.png"), 32, 256, 150);
	auto MultiViewReport = TSharedPtr<FJsonObject>{};
	if (this->TestTrue(TEXT("Independent multi-view comparison completes"),
						   FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MultiViewJson), MultiViewReport) && MultiViewReport.IsValid()) == false)
	{
		this->AddError(MultiViewJson);
		return false;
	}
	this->TestTrue(TEXT("Independent camera references change coverage for some shared candidates"), MultiViewReport->GetIntegerField(TEXT("viewDependentCandidates")) > 0);
	this->TestEqual(TEXT("Both camera references cover the complete candidate domain"), MultiViewReport->GetIntegerField(TEXT("comparedUnion")), 32 * 32);
	this->TestTrue(TEXT("Each view and union agree with independent rendering"), MultiViewReport->GetBoolField(TEXT("exactMatch")));
	this->TestEqual(TEXT("Shared-buffer accumulation preserves all per-view coverage"), MultiViewReport->GetIntegerField(TEXT("accumulationMismatches")), 0);
	this->TestEqual(TEXT("Reversing view order preserves coverage"), MultiViewReport->GetIntegerField(TEXT("orderMismatches")), 0);

	// 負のMip Biasは微分の縮小へ置換しない。異方性が強い面を独立描画と比較する。
	constexpr auto TextureSize = 256;
	auto Texels = TArray64<uint8>{};
	Texels.SetNumUninitialized(TextureSize * TextureSize * 4);
	for (auto Y = 0; Y < TextureSize; ++Y)
	for (auto X = 0; X < TextureSize; ++X)
		{
		const auto Value = uint8(((X / 4 + Y / 16) & 1) != 0 ? 255 : 0);
		const auto Index = (Y * TextureSize + X) * 4;
		Texels[Index] = Texels[Index + 1] = Texels[Index + 2] = Value;
		Texels[Index + 3] = 255;
		}
	auto Texture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(TextureSize, TextureSize, PF_B8G8R8A8, NAME_None, Texels));
	if (this->TestTrue(TEXT("Texture bias fixture is available"), Texture.IsValid()) == false)
	{
		return false;
	}
	Texture->SRGB = false;
	Texture->UpdateResource();
	auto Biased = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
	Biased->BlendMode = BLEND_Masked;
	Biased->bCanMaskedBeAssumedOpaque = false;
	Biased->DitherOpacityMask = true;
	auto* TextureSample = Cast<UMaterialExpressionTextureSample>(UMaterialEditingLibrary::CreateMaterialExpression(Biased.Get(), UMaterialExpressionTextureSample::StaticClass()));
	TextureSample->Texture = Texture.Get();
	TextureSample->SamplerType = SAMPLERTYPE_LinearColor;
	TextureSample->MipValueMode = TMVM_MipBias;
	TextureSample->ConstMipValue = -1;
	Biased->GetEditorOnlyData()->OpacityMask.Connect(1, TextureSample);
	UMaterialEditingLibrary::RecompileMaterial(Biased.Get());
	GShaderCompilingManager->FinishAllCompilation();
	const auto BiasedJson = UMeshParticleizerPreparationLibrary::CompareMaterialCoverageRender(Biased.Get(),
		FVector(FMath::Sin(FMath::DegreesToRadians(80.0)), 0, FMath::Cos(FMath::DegreesToRadians(80.0))),
		Directory / TEXT("NegativeMipBias.png"), 128, 60);
	auto BiasedReport = TSharedPtr<FJsonObject>{};
	if (this->TestTrue(TEXT("Negative mip bias comparison completes"),
						   FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BiasedJson), BiasedReport) && BiasedReport.IsValid()) == false)
	{
		this->AddError(BiasedJson);
		return false;
	}
	this->TestTrue(TEXT("Negative mip bias produces covered pixels"), BiasedReport->GetNumberField(TEXT("referenceCovered")) > 0);
	this->TestTrue(TEXT("Negative mip bias keeps original texture sampling"), BiasedReport->GetBoolField(TEXT("exactMatch")));
	this->TestEqual(TEXT("Negative mip bias diagnostic preserves runtime result"), BiasedReport->GetIntegerField(TEXT("diagnosticCandidateMismatches")), 0);
	return true;
}
#endif
