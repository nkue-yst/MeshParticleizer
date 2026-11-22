#include "MeshParticleizerCoverageValidation.h"
#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerCoverageGpu.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "Serialization/JsonSerializer.h"
#include "ShaderCompiler.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace MeshParticleizerCoverageValidation
{
	FTextureRHIRef GetNoiseTexture()
	{
		if (GEngine == nullptr)
		{
			return {};
		}
		GEngine->LoadBlueNoiseTexture(false);
		auto* Texture = GEngine->BlueNoiseScalarTexture.Get();
		return Texture != nullptr && Texture->GetResource() != nullptr ? Texture->GetResource()->TextureRHI : FTextureRHIRef{};
	}

	void SetViewResources(FViewUniformShaderParameters& Uniforms, FRHITexture* Texture)
	{
		// UEのBlueNoise.cppと同じ正方形の時系列配置。永続ViewStateのないSceneCaptureはframe 0。
		const auto Size = Texture->GetSizeXYZ();
		Uniforms.BlueNoiseScalarTexture = Texture;
		Uniforms.BlueNoiseDimensions = FIntVector(Size.X, Size.X, Size.Y / Size.X);
		Uniforms.BlueNoiseModuloMasks = Uniforms.BlueNoiseDimensions - FIntVector(1);
		Uniforms.StateFrameIndex = 0;
		// FViewUniformの既定値はBilinear。標準描画と同じWorldグループの異方性・Mip Biasを使う。
		Uniforms.MaterialTextureBilinearWrapedSampler = Wrap_WorldGroupSettings->SamplerStateRHI;
		Uniforms.MaterialTextureBilinearClampedSampler = Clamp_WorldGroupSettings->SamplerStateRHI;
	}

	TStrongObjectPtr<UMaterialInstanceConstant> CreateInstance(UMaterialInterface* Material)
	{
		// 元の式とMI値を保ち、診断専用Usageだけを一時複製へ設定する。MIが親をGCから保護する。
		auto Parent = TStrongObjectPtr<UMaterial>(DuplicateObject<UMaterial>(Material->GetMaterial(), GetTransientPackage()));
		Parent->SetFlags(RF_Transient);
		Parent->ClearFlags(RF_Public | RF_Standalone);
		Parent->bCanMaskedBeAssumedOpaque = false;
		Parent->SetUsageByFlag(MATUSAGE_NiagaraMeshParticles, true);
		UMaterialEditingLibrary::RecompileMaterial(Parent.Get());
		auto Instance = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
		Instance->SetParentEditorOnly(Parent.Get());
		Instance->CopyMaterialUniformParametersEditorOnly(Material, true);
		Instance->BasePropertyOverrides.bOverride_OpacityMaskClipValue = true;
		Instance->BasePropertyOverrides.OpacityMaskClipValue = Material->GetOpacityMaskClipValue();
		Instance->PostEditChange();
		GShaderCompilingManager->FinishAllCompilation();

		// 診断中にミップ未取得の結果を採用しない。常駐要求は短時間で失効する。
		auto Textures = TArray<UTexture*>{};
		Instance->GetUsedTextures(Textures);
		for (auto* Texture : Textures)
		{
			if (auto* Texture2D = Cast<UTexture2D>(Texture))
			{
				Texture2D->SetForceMipLevelsToBeResident(30);
				Texture2D->WaitForStreaming();
			}
		}
		return Instance;
	}
}

FString UMeshParticleizerPreparationLibrary::ProbeMaterialCoverage(UMaterialInterface* Material,
	const TArray<FVector>& ViewDirections, const FString& OutputPng, int32 Resolution)
{
	if (Material == nullptr || Material->GetMaterial() == nullptr || Material->GetBlendMode() != BLEND_Masked ||
		Resolution < 16 || Resolution > 512 || ViewDirections.IsEmpty() || ViewDirections.Num() > 8 ||
		ViewDirections.ContainsByPredicate([](const FVector& Direction) {
			return Direction.ContainsNaN() || Direction.IsNearlyZero();
		}) ||
		FPaths::GetExtension(OutputPng).Equals(TEXT("png"), ESearchCase::IgnoreCase) == false)
	{
		return TEXT("Coverage probe requires a Masked material, 1..8 finite nonzero directions, 16..512 resolution and a PNG path.");
	}

	auto Instance = MeshParticleizerCoverageValidation::CreateInstance(Material);
	const auto Noise = MeshParticleizerCoverageValidation::GetNoiseTexture();
	if (Noise.IsValid() == false)
	{
		return TEXT("UE blue noise texture is unavailable.");
	}

	const auto Size = FIntPoint(Resolution * ViewDirections.Num(), Resolution);
	auto Samples = TArray<FMeshParticleizerCoverageSample>{};
	Samples.SetNum(Size.X * Size.Y);
	for (auto Y = 0; Y < Size.Y; ++Y)
	{
		for (auto X = 0; X < Size.X; ++X)
		{
			auto& Sample = Samples[Y * Size.X + X];
			Sample.PositionAndSlot = FVector4f(FVector3f(-ViewDirections[X / Resolution].GetSafeNormal() * 100), 0);
			Sample.VertexNormal = FVector4f(0, 0, 1, 0);
			Sample.UVAndDdx = FVector4f((X % Resolution + 0.5f) / Resolution, (Y + 0.5f) / Resolution, 1.f / Resolution, 0);
			Sample.Ddy = FVector4f(0, 1.f / Resolution, 0, 0);
		}
	}

	auto Pixels = TArray<FLinearColor>{};
	auto bAccepted = false;
	int64 SampleBytes = 0;
	int64 TextureBytes = 0;
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageProbe)([Proxy = Instance->GetRenderProxy(), Size, Noise,
															  Samples = MoveTemp(Samples), &Pixels, &bAccepted, &SampleBytes, &TextureBytes](FRHICommandListImmediate& RHICmdList) {
		auto Family = FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(nullptr, nullptr,
			FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime::GetTimeSinceAppStart()));
		auto Options = FSceneViewInitOptions{};
		Options.ViewFamily = &Family;
		Options.SetViewRectangle(FIntRect(FIntPoint::ZeroValue, Size));
		Options.ViewRotationMatrix = FMatrix::Identity;
		Options.ProjectionMatrix = FReversedZPerspectiveMatrix(PI / 4, 1, 1, 1);
		auto View = FSceneView(Options);
		auto Uniforms = FViewUniformShaderParameters{};
		View.SetupCommonViewUniformBufferParameters(Uniforms, Size, 1, View.UnscaledViewRect, View.ViewMatrices, View.ViewMatrices);
		MeshParticleizerCoverageValidation::SetViewResources(Uniforms, Noise);
		View.ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Uniforms, UniformBuffer_SingleFrame);

		auto Graph = FRDGBuilder(RHICmdList);
		auto* Buffer = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Samples.Num() * 4), TEXT("MeshParticleizer.CoverageProbeSamples"));
		Graph.QueueBufferUpload(Buffer, Samples.GetData(), Samples.Num() * sizeof(FMeshParticleizerCoverageSample));
		auto* Output = Graph.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_R32_FLOAT, FClearValueBinding::Black,
											   TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("MeshParticleizer.CoverageProbe"));
		AddClearRenderTargetPass(Graph, Output, FLinearColor::Black);
		bAccepted = AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, Buffer, Samples.Num(), Output);
		SampleBytes = Buffer->Desc.GetSize();
		TRefCountPtr<IPooledRenderTarget> Readback;
		Graph.QueueTextureExtraction(Output, &Readback);
		Graph.Execute();
		TextureBytes = RHICalcTexturePlatformSize(Readback->GetRHI()->GetDesc(), 0).Size;
		auto Flags = FReadSurfaceDataFlags(RCM_MinMax);
		Flags.SetLinearToGamma(false);
		RHICmdList.ReadSurfaceData(Readback->GetRHI(), FIntRect(FIntPoint::ZeroValue, Size), Pixels, Flags);
	});
	FlushRenderingCommands();
	if (bAccepted == false || Pixels.Num() != Size.X * Size.Y)
	{
		return TEXT("Coverage shader or GPU readback is unavailable.");
	}

	auto Counts = TArray<int32>{};
	Counts.Init(0, ViewDirections.Num());
	auto Image = TArray<FColor>{};
	Image.Reserve(Pixels.Num());
	for (auto Index = 0; Index < Pixels.Num(); ++Index)
	{
		const auto bCovered = Pixels[Index].R >= 0.5f;
		Counts[(Index % Size.X) / Resolution] += bCovered ? 1 : 0;
		Image.Add(bCovered ? FColor::White : FColor::Black);
	}
	auto Compressed = TArray64<uint8>{};
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, MakeArrayView(Image), Compressed);
	if (FFileHelper::SaveArrayToFile(Compressed, *OutputPng) == false)
	{
		return TEXT("Coverage PNG could not be saved.");
	}

	auto Report = MakeShared<FJsonObject>();
	Report->SetBoolField(TEXT("success"), true);
	Report->SetStringField(TEXT("scope"), TEXT("UV0 plane material evaluation; not skeletal playback or runtime budget validation"));
	Report->SetStringField(TEXT("material"), Material->GetPathName());
	Report->SetNumberField(TEXT("resolution"), Resolution);
	Report->SetNumberField(TEXT("materialQuality"), IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaterialQualityLevel"))->GetInt());
	Report->SetNumberField(TEXT("sampleBufferBytes"), SampleBytes);
	Report->SetNumberField(TEXT("coverageTextureBytes"), TextureBytes);
	auto Values = TArray<TSharedPtr<FJsonValue>>{};
	for (const auto Count : Counts)
	{
		Values.Add(MakeShared<FJsonValueNumber>(Count));
	}
	Report->SetArrayField(TEXT("coveredSamples"), Values);
	Report->SetStringField(TEXT("image"), OutputPng);
	auto Json = FString{};
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

FString UMeshParticleizerPreparationLibrary::CompareMaterialCoverageRender(UMaterialInterface* Material,
	FVector ViewDirection, const FString& OutputPng, int32 Resolution, float CameraDistance)
{
	if (ViewDirection.Z <= 0)
	{
		return TEXT("Plane comparison requires an upper-hemisphere direction.");
	}
	auto Plane = TStrongObjectPtr<UStaticMesh>(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	return MeshParticleizerCoverageValidation::CompareRender(Material, Plane.Get(), ViewDirection,
		FVector::ZeroVector, OutputPng, Resolution, CameraDistance);
}

FString MeshParticleizerCoverageValidation::CompareRender(UMaterialInterface* Material, UStaticMesh* Mesh,
	FVector ViewDirection, FVector CameraTarget, const FString& OutputPng, int32 Resolution, float CameraDistance)
{
	if (Material == nullptr || Material->GetMaterial() == nullptr || Material->GetBlendMode() != BLEND_Masked ||
		ViewDirection.ContainsNaN() || ViewDirection.IsNearlyZero() || CameraTarget.ContainsNaN() ||
		FMath::IsFinite(CameraDistance) == false || CameraDistance <= 0 || CameraDistance > 100000 ||
		Resolution < 16 || Resolution > 500 || FPaths::GetExtension(OutputPng).Equals(TEXT("png"), ESearchCase::IgnoreCase) == false)
	{
		return TEXT("Render comparison requires a Masked material, finite camera values, distance in (0, 100000], 16..500 resolution and a PNG path.");
	}
	if (Mesh == nullptr || Mesh->GetRenderData() == nullptr || Mesh->GetRenderData()->LODResources.IsEmpty())
	{
		return TEXT("Validation mesh render data is unavailable.");
	}
	const auto& LOD = Mesh->GetRenderData()->LODResources[0];
	const auto Indices = LOD.IndexBuffer.GetArrayView();
	if (Indices.Num() == 0 || Indices.Num() % 3 != 0 || LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		return TEXT("Validation mesh requires UV0 triangles.");
	}
	const auto TriangleCount = Indices.Num() / 3;

	// 参照は未変更の元材とUE標準Static Mesh描画。候補評価用の複製とは別の経路で深度を得る。
	auto Instance = MeshParticleizerCoverageValidation::CreateInstance(Material);
	const auto Noise = MeshParticleizerCoverageValidation::GetNoiseTexture();
	if (Noise.IsValid() == false)
	{
		return TEXT("UE blue noise texture is unavailable.");
	}
	auto Scene = FPreviewScene(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false).SetTransactional(false));
	auto Component = TStrongObjectPtr<UStaticMeshComponent>(NewObject<UStaticMeshComponent>());
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Material);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Scene.AddComponent(Component.Get(), FTransform::Identity);
	auto Target = TStrongObjectPtr<UTextureRenderTarget2D>(NewObject<UTextureRenderTarget2D>());
	Target->ClearColor = FLinearColor(1.e8f, 0, 0, 1);
	Target->InitCustomFormat(Resolution, Resolution, PF_R32_FLOAT, true);
	Target->UpdateResourceImmediate(true);
	auto Capture = TStrongObjectPtr<USceneCaptureComponent2D>(NewObject<USceneCaptureComponent2D>());
	Capture->bCaptureEveryFrame = false;
	Capture->bCaptureOnMovement = false;
	Capture->CaptureSource = SCS_SceneDepth;
	Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	Capture->ShowOnlyComponents.Add(Component.Get());
	Capture->TextureTarget = Target.Get();
	Capture->FOVAngle = 60;
	Capture->bUseCustomProjectionMatrix = true;
	Capture->CustomProjectionMatrix = FReversedZPerspectiveMatrix(PI / 6, 1, 1, 1);
	Capture->ShowFlags.SetAntiAliasing(false);
	Capture->ShowFlags.SetTemporalAA(false);
	Capture->ShowFlags.SetMotionBlur(false);
	Capture->ShowFlags.SetScreenPercentage(false);
	const auto CameraLocation = CameraTarget + ViewDirection.GetSafeNormal() * CameraDistance;
	const auto Rotation = FRotationMatrix::MakeFromX(-ViewDirection.GetSafeNormal()).ToQuat();
	Scene.AddComponent(Capture.Get(), FTransform(Rotation, CameraLocation));

	// 初回描画で必要になったUE標準Shaderも待つ。代替材の深度を参照結果にしない。
	Capture->CaptureScene();
	FlushRenderingCommands();
	GShaderCompilingManager->FinishAllCompilation();
	{
		auto Recreate = FComponentRecreateRenderStateContext(Component.Get());
	}
	Capture->CaptureScene();
	FlushRenderingCommands();
	auto Reference = TArray<FLinearColor>{};
	auto ReadFlags = FReadSurfaceDataFlags(RCM_MinMax);
	ReadFlags.SetLinearToGamma(false);
	if (Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Reference, ReadFlags) == false || Reference.Num() != Resolution * Resolution)
	{
		return TEXT("Scene capture depth readback failed.");
	}

	// SceneCaptureと同じViewで画素中心を逆投影し、元三角形との交点を独立に計算する。
	const auto ViewRotation = FTransform(Rotation).ToInverseMatrixWithScale() * FMatrix(FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
	const auto Projection = Capture->CustomProjectionMatrix;
	const auto ViewProjection = FTranslationMatrix(-CameraLocation) * ViewRotation * Projection;
	const auto Rect = FIntRect(0, 0, Resolution, Resolution);
	const auto Count = Resolution * Resolution;
	auto Positions = TArray<FVector4f>{};
	auto Normals = TArray<FVector4f>{};
	auto Coordinates = TArray<FVector4f>{};
	Positions.Init(FVector4f(0, 0, 0, -1), Count);
	Normals.Init(FVector4f(0, 0, 1, 0), Count);
	Coordinates.SetNumZeroed(Count);
	auto Edges = TArray<FVector4f>{};
	for (auto Triangle = 0; Triangle < TriangleCount; ++Triangle)
	{
		const auto I0 = Indices[Triangle * 3];
		const auto I1 = Indices[Triangle * 3 + 1];
		const auto I2 = Indices[Triangle * 3 + 2];
		const auto UV0 = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(I0, 0);
		const auto UV1 = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(I1, 0);
		const auto UV2 = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(I2, 0);
		Edges.Add(FVector4f(UV0.X, UV0.Y, UV1.X, UV1.Y));
		Edges.Add(FVector4f(UV2.X, UV2.Y, 0, 1));
		for (const auto Vertex : {I0, I1, I2})
		{
			Edges.Add(FVector4f(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex), 0));
		}
		for (const auto Vertex : {I0, I1, I2})
		{
			Edges.Add(FVector4f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex), 0));
		}
	}
	const auto Inverse = ViewProjection.Inverse();
	auto Eligible = TBitArray<>(false, Count);
	auto BoundaryDistances = TArray<FVector2d>{};
	BoundaryDistances.SetNumZeroed(Count);
	auto OverlappingInteriorPixels = 0;
	for (auto Y = 0; Y < Resolution; ++Y)
	{
		for (auto X = 0; X < Resolution; ++X)
		{
			// FSceneViewの逆投影ヘルパーは小数画素を切り捨てるため、画素中心を直接逆変換する。
			auto Unproject = [&](double Depth) {
				const auto Clip = FVector4((X + 0.5) * 2 / Resolution - 1, 1 - (Y + 0.5) * 2 / Resolution, Depth, 1);
				const auto World = Inverse.TransformFVector4(Clip);
				return FVector(World) / World.W;
			};
			const auto RayOrigin = Unproject(1);
			const auto RayDirection = (Unproject(0.5) - RayOrigin).GetSafeNormal();
			const auto Index = Y * Resolution + X;
			auto NearestDistance = TNumericLimits<double>::Max();
			auto InteriorHits = 0;
			for (auto Triangle = 0; Triangle < TriangleCount; ++Triangle)
			{
				const auto I0 = Indices[Triangle * 3];
				const auto I1 = Indices[Triangle * 3 + 1];
				const auto I2 = Indices[Triangle * 3 + 2];
				const auto A = FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I0));
				const auto E1 = FVector(Edges[Triangle * 8 + 3]) - A;
				const auto E2 = FVector(Edges[Triangle * 8 + 4]) - A;
				const auto FaceNormal = FVector::CrossProduct(E1, E2).GetSafeNormal();
				const auto Denominator = FVector::DotProduct(FaceNormal, RayDirection);
				if (FMath::Abs(Denominator) <= 1.e-12)
				{
					continue;
				}
				const auto Distance = FVector::DotProduct(FaceNormal, A - RayOrigin) / Denominator;
				if (Distance <= 0)
				{
					continue;
				}
				const auto Point = RayOrigin + RayDirection * Distance;
				const auto Delta = Point - A;
				const auto AA = E1.SizeSquared();
				const auto BB = E2.SizeSquared();
				const auto AB = FVector::DotProduct(E1, E2);
				if (AA * BB - AB * AB <= 0)
				{
					continue;
				}
				const auto DA = FVector::DotProduct(Delta, E1);
				const auto DB = FVector::DotProduct(Delta, E2);
				const auto W1 = (DA * BB - DB * AB) / (AA * BB - AB * AB);
				const auto W2 = (DB * AA - DA * AB) / (AA * BB - AB * AB);
				const auto W0 = 1 - W1 - W2;
				if (FMath::Min3(W0, W1, W2) < 0)
				{
					continue;
				}
				InteriorHits += FMath::Min3(W0, W1, W2) > 1.e-5;
				if (Distance >= NearestDistance)
				{
					continue;
				}
				NearestDistance = Distance;
				const auto UV = FVector2D(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(I0, 0)) * W0 +
					FVector2D(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(I1, 0)) * W1 +
					FVector2D(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(I2, 0)) * W2;
				const auto Normal = FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(I0)) * W0 +
					FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(I1)) * W1 +
					FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(I2)) * W2;
				Positions[Index] = FVector4f(FVector3f(Point), 0);
				Normals[Index] = FVector4f(Normal.GetSafeNormal(), Triangle);
				Coordinates[Index] = FVector4f(UV.X, UV.Y, W0, W1);

				// 連続面の所属と画素上の辺距離を保持し、副画素の境界を後から識別する。
				FVector2d Screen[3];
				auto bProjectable = true;
				for (auto Corner = 0; Corner < 3; ++Corner)
				{
					const auto Position = FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[Triangle * 3 + Corner]));
					const auto Clip = ViewProjection.TransformFVector4(FVector4(Position, 1));
					if (Clip.W <= 0)
					{
						bProjectable = false;
						break;
					}
					Screen[Corner] = FVector2d(Clip.X, -Clip.Y) * (Resolution * 0.5 / Clip.W) + FVector2d(Resolution * 0.5);
				}
				auto EdgeDistance = TNumericLimits<double>::Max();
				for (auto Corner = 0; bProjectable == true && Corner < 3; ++Corner)
				{
					const auto Edge = Screen[(Corner + 1) % 3] - Screen[Corner];
					const auto Offset = FVector2d(X + 0.5, Y + 0.5) - Screen[Corner];
					if (Edge.SizeSquared() > 0)
					{
						EdgeDistance = FMath::Min(EdgeDistance, FMath::Abs(Edge.X * Offset.Y - Edge.Y * Offset.X) / Edge.Size());
					}
				}
				// 近面で切り出す面の輪郭は、この未クリップ頂点からの距離では表さない。
				BoundaryDistances[Index] = FVector2d(FMath::Min3(W0, W1, W2), bProjectable ? EdgeDistance : -1.0);

				// 三角形の共有辺と輪郭はラスタライズの辺規則から分け、元Maskの穴を比較する。
				Eligible[Index] = FMath::Min3(W0, W1, W2) > 1.e-5;
			}
			OverlappingInteriorPixels += InteriorHits > 1;
		}
	}

	auto RasterVertices = TArray<FMeshParticleizerCoverageSample>{};
	for (auto Vertex = 0; Vertex < Indices.Num(); ++Vertex)
	{
		const auto Source = Indices[Vertex];
		const auto UV = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Source, 0);
		auto& Sample = RasterVertices.AddDefaulted_GetRef();
		Sample.PositionAndSlot = FVector4f(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Source), 0);
		Sample.VertexNormal = FVector4f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Source), 0);
		Sample.UVAndDdx = FVector4f(UV.X, UV.Y, 0, 0);
		Sample.Ddy = FVector4f(0, 0, 0, 0);
	}
	auto Evaluated = TArray<FLinearColor>{};
	auto CandidateDetails = TArray<FLinearColor>{};
	auto RasterDetails = TArray<FLinearColor>{};
	auto ExplicitDetails = TArray<FLinearColor>{};
	auto ReplayedDetails = TArray<FLinearColor>{};
	TArray<FLinearColor> CorrectedDetails[3];
	auto TransferredDetails = TArray<FLinearColor>{};
	auto TransferredInputs = TArray<FMeshParticleizerCoverageSample>{};
	auto TransferMatches = TArray<uint32>{};
	auto TransferLinks = TArray<FUintVector4>{};
	auto TransferScratchBytes = uint64{0};
	auto CandidateInputs = TArray<FMeshParticleizerCoverageSample>{};
	auto RasterInputs = TArray<FMeshParticleizerCoverageSample>{};
	auto bAccepted = false;
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageRenderComparison)([Proxy = Instance->GetRenderProxy(), Rect, Noise,
																		 CameraLocation, ViewRotation, Projection, Positions = MoveTemp(Positions), Normals = MoveTemp(Normals),
																		 Coordinates = MoveTemp(Coordinates), Edges = MoveTemp(Edges), RasterVertices = MoveTemp(RasterVertices),
																		 &Evaluated, &CandidateDetails, &RasterDetails, &ExplicitDetails, &ReplayedDetails,
																		 &CandidateInputs, &RasterInputs, &CorrectedDetails, &TransferredDetails, &TransferredInputs,
																		 &TransferMatches, &TransferLinks, &TransferScratchBytes, &bAccepted](FRHICommandListImmediate& RHICmdList) {
		auto Family = FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(nullptr, nullptr,
			FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime::GetTimeSinceAppStart()));
		auto Options = FSceneViewInitOptions{};
		Options.ViewFamily = &Family;
		Options.ViewOrigin = CameraLocation;
		Options.SetViewRectangle(Rect);
		Options.ViewRotationMatrix = ViewRotation;
		Options.ProjectionMatrix = Projection;
		auto View = FSceneView(Options);
		auto Uniforms = FViewUniformShaderParameters{};
		View.SetupCommonViewUniformBufferParameters(Uniforms, Rect.Size(), 1, Rect, View.ViewMatrices, View.ViewMatrices);
		MeshParticleizerCoverageValidation::SetViewResources(Uniforms, Noise);
		View.ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Uniforms, UniformBuffer_SingleFrame);
		auto Graph = FRDGBuilder(RHICmdList);
		auto Texture = [&](const TCHAR* Name, const TArray<FVector4f>& Rows) {
			const auto Width = Rows.Num() > Rect.Width() ? Rect.Width() : Rows.Num();
			const auto Height = FMath::DivideAndRoundUp(Rows.Num(), Width);
			auto RHI = RHICmdList.CreateTexture(FRHITextureCreateDesc::Create2D(Name, Width, Height, PF_A32B32G32R32F)
					.SetFlags(TexCreate_ShaderResource)
					.SetInitialState(ERHIAccess::CopyDest));
			RHICmdList.UpdateTexture2D(RHI, 0, FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height), Width * sizeof(FVector4f), reinterpret_cast<const uint8*>(Rows.GetData()));
			RHICmdList.Transition(FRHITransitionInfo(RHI, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
			return Graph.RegisterExternalTexture(CreateRenderTarget(RHI, Name));
		};
		auto Input = FMeshParticleizerCoverageGeometry{};
		Input.Count = Positions.Num();
		Input.TriangleCount = Edges.Num() / 8;
		Input.MaterialSlots = 1;
		const uint32 DynamicSlot = 1;
		Input.DynamicSlots = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CoverageCompare.Slots"));
		Graph.QueueBufferUpload(Input.DynamicSlots, &DynamicSlot, sizeof(DynamicSlot));
		Input.Positions = Texture(TEXT("CoverageCompare.Position"), Positions);
		Input.Normals = Texture(TEXT("CoverageCompare.Normal"), Normals);
		Input.Coordinates = Texture(TEXT("CoverageCompare.UV"), Coordinates);
		Input.TriangleData = Texture(TEXT("CoverageCompare.Edges"), Edges);
		auto* Samples = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Input.Count * 4), TEXT("CoverageCompare.Samples"));
		auto* Output = Graph.CreateTexture(FRDGTextureDesc::Create2D(Rect.Size(), PF_R32_FLOAT, FClearValueBinding::Black,
											   TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("CoverageCompare.Mask"));
		AddClearRenderTargetPass(Graph, Output, FLinearColor::Black);
		auto* RasterHeads = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshParticleizerCoverageBucketCount(Input.Count)), TEXT("CoverageCompare.Heads"));
		auto* RasterLinks = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FUintVector4), Input.Count), TEXT("CoverageCompare.Links"));
		bAccepted = AddMeshParticleizerCoverageGeometryPass(Graph, View, Input, Samples) &&
			AddMeshParticleizerCoverageRasterInputPass(Graph, View, Rect, Input, Samples, RasterHeads, RasterLinks) &&
			AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, Samples, Input.Count, Output);
		// 同じ材質を通常の三角形補間でも評価し、候補入力と材質評価の差を分ける。
		auto* VertexBuffer = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), RasterVertices.Num() * 4), TEXT("CoverageCompare.Vertices"));
		Graph.QueueBufferUpload(VertexBuffer, RasterVertices.GetData(), RasterVertices.Num() * sizeof(FMeshParticleizerCoverageSample));
		auto* CandidateOutput = Graph.CreateTexture(FRDGTextureDesc::Create2D(Rect.Size(), PF_A32B32G32R32F, FClearValueBinding::Black,
														TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("CoverageCompare.CandidateDetails"));
		auto* RasterOutput = Graph.CreateTexture(CandidateOutput->Desc, TEXT("CoverageCompare.RasterDetails"));
		auto* ExplicitOutput = Graph.CreateTexture(CandidateOutput->Desc, TEXT("CoverageCompare.ExplicitDetails"));
		auto* ReplayedOutput = Graph.CreateTexture(CandidateOutput->Desc, TEXT("CoverageCompare.ReplayedDetails"));
		auto* CapturedSamples = Graph.CreateBuffer(Samples->Desc, TEXT("CoverageCompare.RasterInputs"));
		AddClearUAVPass(Graph, Graph.CreateUAV(CapturedSamples, PF_A32B32G32R32F), 0u);
		AddClearRenderTargetPass(Graph, CandidateOutput, FLinearColor::Black);
		AddClearRenderTargetPass(Graph, RasterOutput, FLinearColor::Black);
		AddClearRenderTargetPass(Graph, ExplicitOutput, FLinearColor::Black);
		AddClearRenderTargetPass(Graph, ReplayedOutput, FLinearColor::Black);
		bAccepted = bAccepted && AddMeshParticleizerCoverageDiagnosticPass(Graph, View, Proxy, Samples, Input.Count, CandidateOutput, EMeshParticleizerCoverageDiagnosticMode::Candidate) &&
			AddMeshParticleizerCoverageDiagnosticPass(Graph, View, Proxy, VertexBuffer, RasterVertices.Num(), RasterOutput,
				EMeshParticleizerCoverageDiagnosticMode::RasterImplicit) &&
			AddMeshParticleizerCoverageDiagnosticPass(Graph, View, Proxy, VertexBuffer, RasterVertices.Num(), ExplicitOutput,
				EMeshParticleizerCoverageDiagnosticMode::RasterExplicit, CapturedSamples) &&
			AddMeshParticleizerCoverageDiagnosticPass(Graph, View, Proxy, CapturedSamples, Input.Count, ReplayedOutput,
				EMeshParticleizerCoverageDiagnosticMode::Candidate);
		// GPU上の索引で実際の描画入力を候補へ転送する。CPU置換診断から独立して評価する。
		auto* TransferredSamples = Graph.CreateBuffer(Samples->Desc, TEXT("CoverageCompare.TransferredSamples"));
		AddCopyBufferPass(Graph, TransferredSamples, Samples);
		FRDGBufferRef Matches = nullptr;
		FRDGBufferRef Links = nullptr;
		bAccepted &= AddMeshParticleizerCoverageRasterInputDiagnosticPass(Graph, View, VertexBuffer, RasterVertices.Num(),
			Input.Normals, TransferredSamples, Input.Count, Matches, TransferScratchBytes, &Links);
		auto* TransferredOutput = Graph.CreateTexture(CandidateOutput->Desc, TEXT("CoverageCompare.TransferredDetails"));
		AddClearRenderTargetPass(Graph, TransferredOutput, FLinearColor::Black);
		bAccepted &= AddMeshParticleizerCoverageDiagnosticPass(Graph, View, Proxy, TransferredSamples, Input.Count,
			TransferredOutput, EMeshParticleizerCoverageDiagnosticMode::Candidate);
		if (bAccepted == false || Matches == nullptr || Links == nullptr)
		{
			return;
		}

		auto TransferredInputReadback = FRHIGPUBufferReadback(TEXT("CoverageCompare.TransferredInputsReadback"));
		auto TransferMatchReadback = FRHIGPUBufferReadback(TEXT("CoverageCompare.TransferMatchesReadback"));
		auto TransferLinkReadback = FRHIGPUBufferReadback(TEXT("CoverageCompare.TransferLinksReadback"));
		AddEnqueueCopyPass(Graph, &TransferredInputReadback, TransferredSamples, Input.Count * sizeof(FMeshParticleizerCoverageSample));
		AddEnqueueCopyPass(Graph, &TransferMatchReadback, Matches, Input.Count * sizeof(uint32));
		AddEnqueueCopyPass(Graph, &TransferLinkReadback, Links, Input.Count * sizeof(FUintVector4));
		TRefCountPtr<IPooledRenderTarget> TransferredReadback;
		Graph.QueueTextureExtraction(TransferredOutput, &TransferredReadback);

		auto CandidateInputReadback = FRHIGPUBufferReadback(TEXT("CoverageCompare.CandidateInputsReadback"));
		auto RasterInputReadback = FRHIGPUBufferReadback(TEXT("CoverageCompare.RasterInputsReadback"));
		const auto InputBytes = Input.Count * sizeof(FMeshParticleizerCoverageSample);
		AddEnqueueCopyPass(Graph, &CandidateInputReadback, Samples, InputBytes);
		AddEnqueueCopyPass(Graph, &RasterInputReadback, CapturedSamples, InputBytes);
		TRefCountPtr<IPooledRenderTarget> Readback, CandidateReadback, RasterReadback, ExplicitReadback, ReplayedReadback;
		Graph.QueueTextureExtraction(Output, &Readback);
		Graph.QueueTextureExtraction(CandidateOutput, &CandidateReadback);
		Graph.QueueTextureExtraction(RasterOutput, &RasterReadback);
		Graph.QueueTextureExtraction(ExplicitOutput, &ExplicitReadback);
		Graph.QueueTextureExtraction(ReplayedOutput, &ReplayedReadback);
		Graph.Execute();
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		CandidateInputs.SetNum(Input.Count);
		RasterInputs.SetNum(Input.Count);
		FMemory::Memcpy(CandidateInputs.GetData(), CandidateInputReadback.Lock(InputBytes), InputBytes);
		CandidateInputReadback.Unlock();
		FMemory::Memcpy(RasterInputs.GetData(), RasterInputReadback.Lock(InputBytes), InputBytes);
		RasterInputReadback.Unlock();
		TransferredInputs.SetNum(Input.Count);
		TransferMatches.SetNum(Input.Count);
		FMemory::Memcpy(TransferredInputs.GetData(), TransferredInputReadback.Lock(InputBytes), InputBytes);
		TransferredInputReadback.Unlock();
		FMemory::Memcpy(TransferMatches.GetData(), TransferMatchReadback.Lock(Input.Count * sizeof(uint32)), Input.Count * sizeof(uint32));
		TransferMatchReadback.Unlock();
		TransferLinks.SetNum(Input.Count);
		FMemory::Memcpy(TransferLinks.GetData(), TransferLinkReadback.Lock(Input.Count * sizeof(FUintVector4)), Input.Count * sizeof(FUintVector4));
		TransferLinkReadback.Unlock();
		auto Flags = FReadSurfaceDataFlags(RCM_MinMax);
		Flags.SetLinearToGamma(false);
		RHICmdList.ReadSurfaceData(Readback->GetRHI(), Rect, Evaluated, Flags);
		RHICmdList.ReadSurfaceData(TransferredReadback->GetRHI(), Rect, TransferredDetails, Flags);
		RHICmdList.ReadSurfaceData(CandidateReadback->GetRHI(), Rect, CandidateDetails, Flags);
		RHICmdList.ReadSurfaceData(RasterReadback->GetRHI(), Rect, RasterDetails, Flags);
		RHICmdList.ReadSurfaceData(ExplicitReadback->GetRHI(), Rect, ExplicitDetails, Flags);
		RHICmdList.ReadSurfaceData(ReplayedReadback->GetRHI(), Rect, ReplayedDetails, Flags);

		// UVだけ、微分だけ、両方を実描画の値へ替え、位置・法線とTexture入力の差を分ける。
		// 同期読戻し後のEditor診断だけで実行し、製品の評価・確保へ持ち込まない。
		auto CorrectionGraph = FRDGBuilder(RHICmdList);
		TArray<FMeshParticleizerCoverageSample> CorrectedInputs[3];
		TRefCountPtr<IPooledRenderTarget> CorrectedReadbacks[3];
		for (auto Mode = 0; Mode < 3; ++Mode)
		{
			CorrectedInputs[Mode] = CandidateInputs;
			for (auto Index = 0; Index < CandidateInputs.Num(); ++Index)
			{
				auto& Corrected = CorrectedInputs[Mode][Index];
				const auto& Raster = RasterInputs[Index];
				if (Mode != 1)
				{
					Corrected.UVAndDdx.X = Raster.UVAndDdx.X;
					Corrected.UVAndDdx.Y = Raster.UVAndDdx.Y;
				}
				if (Mode != 0)
				{
					Corrected.UVAndDdx.Z = Raster.UVAndDdx.Z;
					Corrected.UVAndDdx.W = Raster.UVAndDdx.W;
					Corrected.Ddy = Raster.Ddy;
				}
			}
			auto* CorrectedBuffer = CorrectionGraph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Input.Count * 4), TEXT("CoverageCompare.CorrectedInputs"));
			CorrectionGraph.QueueBufferUpload(CorrectedBuffer, CorrectedInputs[Mode].GetData(), InputBytes);
			auto* CorrectedOutput = CorrectionGraph.CreateTexture(FRDGTextureDesc::Create2D(Rect.Size(), PF_A32B32G32R32F,
																	  FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV),
				TEXT("CoverageCompare.CorrectedDetails"));
			AddClearRenderTargetPass(CorrectionGraph, CorrectedOutput, FLinearColor::Black);
			bAccepted &= AddMeshParticleizerCoverageDiagnosticPass(CorrectionGraph, View, Proxy, CorrectedBuffer, Input.Count,
				CorrectedOutput, EMeshParticleizerCoverageDiagnosticMode::Candidate);
			CorrectionGraph.QueueTextureExtraction(CorrectedOutput, &CorrectedReadbacks[Mode]);
		}
		CorrectionGraph.Execute();
		for (auto Mode = 0; Mode < 3; ++Mode)
		{
			RHICmdList.ReadSurfaceData(CorrectedReadbacks[Mode]->GetRHI(), Rect, CorrectedDetails[Mode], Flags);
		}
	});
	FlushRenderingCommands();
	if (bAccepted == false || Evaluated.Num() != Count || CandidateDetails.Num() != Count || RasterDetails.Num() != Count ||
		ExplicitDetails.Num() != Count || ReplayedDetails.Num() != Count || CandidateInputs.Num() != Count || RasterInputs.Num() != Count ||
		TransferredDetails.Num() != Count || TransferredInputs.Num() != Count || TransferMatches.Num() != Count || TransferLinks.Num() != Count ||
		CorrectedDetails[0].Num() != Count || CorrectedDetails[1].Num() != Count || CorrectedDetails[2].Num() != Count)
	{
		return TEXT("Candidate evaluation or diagnostic GPU readback failed.");
	}

	// 左から元材の深度、候補判定、差分。赤は穴からの発生、青は残すべき候補の欠落。
	auto Image = TArray<FColor>{};
	Image.SetNum(Count * 3);
	auto TransferImage = TArray<FColor>{};
	TransferImage.SetNum(Count * 3);
	auto WholeImage = TArray<FColor>{};
	WholeImage.SetNum(Count * 3);
	auto WholeFalsePositive = 0;
	auto WholeFalseNegative = 0;
	auto WholeRasterMismatches = 0;
	auto WholeMismatchDetails = TArray<TSharedPtr<FJsonValue>>{};
	auto TransferReferenceMismatches = 0;
	auto TransferMissing = 0;
	auto TransferMultiple = 0;
	auto TransferPositionNormalChanges = 0;
	auto TransferUnmatchedChanges = 0;
	auto TransferInputMismatches = 0;
	auto TransferUnmatchedDetails = TArray<TSharedPtr<FJsonValue>>{};
	auto Compared = 0;
	auto ReferenceCovered = 0;
	auto CandidateCovered = 0;
	auto FalsePositive = 0;
	auto FalseNegative = 0;
	auto RasterReferenceMismatches = 0;
	auto DiagnosticCandidateMismatches = 0;
	auto ExplicitReferenceMismatches = 0;
	auto ReplayedReferenceMismatches = 0;
	int32 CorrectedReferenceMismatches[3] = {};
	auto MaximumRawMaskDifference = 0.f;
	auto MaximumImplicitExplicitDifference = 0.f;
	auto MaximumUVDifference = 0.f;
	auto MaximumGradientDifference = 0.f;
	auto Mismatches = TArray<TSharedPtr<FJsonValue>>{};
	auto CandidateMismatches = TArray<TSharedPtr<FJsonValue>>{};
	for (auto Index = 0; Index < Count; ++Index)
	{
		// 深度の上限を固定の観測距離にしない。遠方試験も背景のクリア値と区別する。
		const auto bReference = Reference[Index].R > 0 && Reference[Index].R < Target->ClearColor.R * 0.5f;
		const auto bCandidate = Evaluated[Index].R >= 0.5f;

		// 従来の内部画素比較を保ち、除外していた共有辺・輪郭・背景も別欄で比較する。
		WholeFalsePositive += bCandidate && bReference == false;
		WholeFalseNegative += bReference && bCandidate == false;
		WholeRasterMismatches += (RasterDetails[Index].R >= 0.5f) != bReference;
		if (bReference != bCandidate && WholeMismatchDetails.Num() < 128)
		{
			auto Detail = MakeShared<FJsonObject>();
			Detail->SetNumberField(TEXT("x"), Index % Resolution);
			Detail->SetNumberField(TEXT("y"), Index / Resolution);
			Detail->SetBoolField(TEXT("interiorEligible"), Eligible[Index]);
			Detail->SetBoolField(TEXT("referenceCovered"), bReference);
			Detail->SetBoolField(TEXT("candidateCovered"), bCandidate);
			Detail->SetBoolField(TEXT("candidateAvailable"), CandidateInputs[Index].PositionAndSlot.W >= 0);
			Detail->SetNumberField(TEXT("candidateTriangle"), TransferLinks[Index].Z);
			Detail->SetNumberField(TEXT("rasterTriangle"), RasterInputs[Index].VertexNormal.W);
			WholeMismatchDetails.Add(MakeShared<FJsonValueObject>(Detail));
		}
		auto Difference = bReference ? FColor::White : FColor::Black;
		auto TransferDifference = Difference;
		const auto bTransferred = TransferredDetails[Index].R >= 0.5f;
		const auto& Transferred = TransferredInputs[Index];
		const auto& Original = CandidateInputs[Index];
		TransferPositionNormalChanges += FMemory::Memcmp(&Transferred.PositionAndSlot, &Original.PositionAndSlot, sizeof(FVector4f) * 2) != 0;
		TransferUnmatchedChanges += TransferMatches[Index] == 0 && FMemory::Memcmp(&Transferred, &Original, sizeof(FMeshParticleizerCoverageSample)) != 0;
		if (Eligible[Index])
		{
			const auto bRaster = RasterDetails[Index].R >= 0.5f;
			const auto bExplicit = ExplicitDetails[Index].R >= 0.5f;
			RasterReferenceMismatches += bRaster != bReference;
			TransferReferenceMismatches += bTransferred != bReference;
			TransferMissing += TransferMatches[Index] == 0;
			TransferMultiple += TransferMatches[Index] > 1;
			if (TransferMatches[Index] == 0 && TransferUnmatchedDetails.Num() < 128)
			{
				auto Detail = MakeShared<FJsonObject>();
				Detail->SetNumberField(TEXT("x"), Index % Resolution);
				Detail->SetNumberField(TEXT("y"), Index / Resolution);
				Detail->SetNumberField(TEXT("gpuKeyX"), TransferLinks[Index].X);
				Detail->SetNumberField(TEXT("gpuKeyY"), TransferLinks[Index].Y);
				Detail->SetNumberField(TEXT("candidateTriangle"), TransferLinks[Index].Z);
				Detail->SetNumberField(TEXT("rasterTriangle"), RasterInputs[Index].VertexNormal.W);
				Detail->SetNumberField(TEXT("analyticMinimumBarycentric"), BoundaryDistances[Index].X);
				Detail->SetNumberField(TEXT("analyticEdgeDistancePixels"), BoundaryDistances[Index].Y);
				Detail->SetBoolField(TEXT("rasterFragment"), RasterDetails[Index].A > 0.5f);
				Detail->SetBoolField(TEXT("referenceCovered"), bReference);
				Detail->SetBoolField(TEXT("candidateCovered"), bCandidate);
				Detail->SetBoolField(TEXT("transferredCovered"), bTransferred);
				TransferUnmatchedDetails.Add(MakeShared<FJsonValueObject>(Detail));
			}
			TransferInputMismatches += FMemory::Memcmp(&Transferred.UVAndDdx, &RasterInputs[Index].UVAndDdx, sizeof(FVector4f)) != 0 ||
				Transferred.Ddy.X != RasterInputs[Index].Ddy.X || Transferred.Ddy.Y != RasterInputs[Index].Ddy.Y;
			if (bReference != bTransferred)
			{
				TransferDifference = bTransferred ? FColor::Red : FColor::Blue;
			}
			ExplicitReferenceMismatches += bExplicit != bReference;
			ReplayedReferenceMismatches += (ReplayedDetails[Index].R >= 0.5f) != bReference;
			for (auto Mode = 0; Mode < 3; ++Mode)
			{
				CorrectedReferenceMismatches[Mode] += (CorrectedDetails[Mode][Index].R >= 0.5f) != bReference;
			}
			const auto& CandidateInput = CandidateInputs[Index];
			const auto& RasterInput = RasterInputs[Index];
			const auto UVDifference = CandidateInput.UVAndDdx - RasterInput.UVAndDdx;
			const auto DdyDifference = CandidateInput.Ddy - RasterInput.Ddy;
			MaximumUVDifference = FMath::Max3(MaximumUVDifference, FMath::Abs(UVDifference.X), FMath::Abs(UVDifference.Y));
			MaximumGradientDifference = FMath::Max3(MaximumGradientDifference, FMath::Abs(UVDifference.Z), FMath::Abs(UVDifference.W));
			MaximumGradientDifference = FMath::Max3(MaximumGradientDifference, FMath::Abs(DdyDifference.X), FMath::Abs(DdyDifference.Y));
			DiagnosticCandidateMismatches += (CandidateDetails[Index].R >= 0.5f) != bCandidate;
			MaximumRawMaskDifference = FMath::Max(MaximumRawMaskDifference, FMath::Abs(CandidateDetails[Index].G - RasterDetails[Index].G));
			MaximumImplicitExplicitDifference = FMath::Max(MaximumImplicitExplicitDifference, FMath::Abs(ExplicitDetails[Index].G - RasterDetails[Index].G));

			// 診断専用SampleGradの差が多くても、製品経路の差分位置を埋もれさせない。
			const auto bRecordCandidate = bReference != bCandidate && CandidateMismatches.Num() < 128;
			const auto bRecordDiagnostic = (bReference != bCandidate || bReference != bRaster || bReference != bExplicit) && Mismatches.Num() < 128;
			if (bRecordCandidate == true || bRecordDiagnostic == true)
			{
				auto Detail = MakeShared<FJsonObject>();
				Detail->SetNumberField(TEXT("x"), Index % Resolution);
				Detail->SetNumberField(TEXT("y"), Index / Resolution);
				Detail->SetBoolField(TEXT("referenceCovered"), bReference);
				Detail->SetBoolField(TEXT("candidateCovered"), bCandidate);
				Detail->SetBoolField(TEXT("rasterProbeCovered"), bRaster);
				Detail->SetNumberField(TEXT("candidateMask"), CandidateDetails[Index].G);
				Detail->SetNumberField(TEXT("candidateClipped"), CandidateDetails[Index].B);
				Detail->SetNumberField(TEXT("rasterMask"), RasterDetails[Index].G);
				Detail->SetNumberField(TEXT("rasterClipped"), RasterDetails[Index].B);
				Detail->SetBoolField(TEXT("explicitCovered"), bExplicit);
				Detail->SetNumberField(TEXT("explicitMask"), ExplicitDetails[Index].G);
				Detail->SetNumberField(TEXT("explicitClipped"), ExplicitDetails[Index].B);
				Detail->SetNumberField(TEXT("replayedMask"), ReplayedDetails[Index].G);
				Detail->SetBoolField(TEXT("replayedCovered"), ReplayedDetails[Index].R >= 0.5f);
				auto Corrections = TArray<TSharedPtr<FJsonValue>>{};
				for (auto Mode = 0; Mode < 3; ++Mode)
				{
					const auto& Pixel = CorrectedDetails[Mode][Index];
					auto Correction = MakeShared<FJsonObject>();
					Correction->SetBoolField(TEXT("covered"), Pixel.R >= 0.5f);
					Correction->SetNumberField(TEXT("mask"), Pixel.G);
					Correction->SetNumberField(TEXT("clipped"), Pixel.B);
					Corrections.Add(MakeShared<FJsonValueObject>(Correction));
				}
				Detail->SetArrayField(TEXT("correctedUVThenGradientsThenBoth"), Corrections);
				auto AddVector = [&Detail](const TCHAR* Name, const FVector4f& Value) {
					auto Values = TArray<TSharedPtr<FJsonValue>>{};
					for (auto ComponentIndex = 0; ComponentIndex < 4; ++ComponentIndex)
					{
						Values.Add(MakeShared<FJsonValueNumber>(Value[ComponentIndex]));
					}
					Detail->SetArrayField(Name, Values);
				};
				AddVector(TEXT("candidateUVAndDdx"), CandidateInput.UVAndDdx);
				AddVector(TEXT("rasterUVAndDdx"), RasterInput.UVAndDdx);
				AddVector(TEXT("candidateDdy"), CandidateInput.Ddy);
				AddVector(TEXT("rasterDdy"), RasterInput.Ddy);
				if (bRecordDiagnostic == true)
				{
					Mismatches.Add(MakeShared<FJsonValueObject>(Detail));
				}
				if (bRecordCandidate == true)
				{
					CandidateMismatches.Add(MakeShared<FJsonValueObject>(Detail));
				}
			}
			++Compared;
			ReferenceCovered += bReference;
			CandidateCovered += bCandidate;
			FalsePositive += bCandidate && bReference == false;
			FalseNegative += bReference && bCandidate == false;
			if (bReference != bCandidate)
			{
				Difference = bCandidate ? FColor::Red : FColor::Blue;
			}
		}
		else
		{
			Difference = FColor(48, 48, 48);
			TransferDifference = Difference;
		}
		const auto Pixel = (Index / Resolution) * Resolution * 3 + Index % Resolution;
		Image[Pixel] = bReference ? FColor::White : FColor::Black;
		Image[Pixel + Resolution] = bCandidate ? FColor::White : FColor::Black;
		Image[Pixel + Resolution * 2] = Difference;
		WholeImage[Pixel] = Image[Pixel];
		WholeImage[Pixel + Resolution] = Image[Pixel + Resolution];
		WholeImage[Pixel + Resolution * 2] = bReference != bCandidate ? (bCandidate ? FColor::Red : FColor::Blue) : (bReference ? FColor::White : FColor::Black);
		TransferImage[Pixel] = bReference ? FColor::White : FColor::Black;
		TransferImage[Pixel + Resolution] = bTransferred ? FColor::White : FColor::Black;
		TransferImage[Pixel + Resolution * 2] = TransferDifference;
	}
	auto Compressed = TArray64<uint8>{};
	FImageUtils::PNGCompressImageArray(Resolution * 3, Resolution, MakeArrayView(Image), Compressed);
	if (FFileHelper::SaveArrayToFile(Compressed, *OutputPng) == false)
	{
		return TEXT("Comparison PNG could not be saved.");
	}
	const auto TransferPng = FPaths::ChangeExtension(OutputPng, TEXT("")) + TEXT("_transfer.png");
	FImageUtils::PNGCompressImageArray(Resolution * 3, Resolution, MakeArrayView(TransferImage), Compressed);
	if (FFileHelper::SaveArrayToFile(Compressed, *TransferPng) == false)
	{
		return TEXT("Transfer comparison PNG could not be saved.");
	}
	const auto WholePng = FPaths::ChangeExtension(OutputPng, TEXT("")) + TEXT("_whole.png");
	FImageUtils::PNGCompressImageArray(Resolution * 3, Resolution, MakeArrayView(WholeImage), Compressed);
	if (FFileHelper::SaveArrayToFile(Compressed, *WholePng) == false)
	{
		return TEXT("Whole-image comparison PNG could not be saved.");
	}
	auto Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("wholeImageComparedPixels"), Count);
	Report->SetNumberField(TEXT("wholeImageFalsePositive"), WholeFalsePositive);
	Report->SetNumberField(TEXT("wholeImageFalseNegative"), WholeFalseNegative);
	Report->SetNumberField(TEXT("wholeImageRasterReferenceMismatches"), WholeRasterMismatches);
	Report->SetBoolField(TEXT("wholeImageExactMatch"), WholeFalsePositive == 0 && WholeFalseNegative == 0);
	Report->SetArrayField(TEXT("wholeImageMismatchDetails"), WholeMismatchDetails);
	Report->SetStringField(TEXT("wholeImage"), WholePng);
	Report->SetNumberField(TEXT("rasterTransferReferenceMismatches"), TransferReferenceMismatches);
	Report->SetNumberField(TEXT("rasterTransferMissing"), TransferMissing);
	Report->SetNumberField(TEXT("rasterTransferMultiple"), TransferMultiple);
	Report->SetNumberField(TEXT("rasterTransferInputMismatches"), TransferInputMismatches);
	Report->SetNumberField(TEXT("rasterTransferPositionNormalChanges"), TransferPositionNormalChanges);
	Report->SetNumberField(TEXT("rasterTransferUnmatchedChanges"), TransferUnmatchedChanges);
	Report->SetArrayField(TEXT("rasterTransferUnmatchedDetails"), TransferUnmatchedDetails);
	Report->SetNumberField(TEXT("rasterTransferScratchBytes"), static_cast<double>(TransferScratchBytes));
	Report->SetStringField(TEXT("rasterTransferImage"), TransferPng);
	Report->SetBoolField(TEXT("success"), Compared > 0 && DiagnosticCandidateMismatches == 0 && OverlappingInteriorPixels == 0);
	Report->SetNumberField(TEXT("overlappingInteriorPixels"), OverlappingInteriorPixels);
	Report->SetBoolField(TEXT("exactMatch"), FalsePositive == 0 && FalseNegative == 0);
	Report->SetStringField(TEXT("scope"), TEXT("Unoccluded UV0 mesh: UE SceneCapture depth versus runtime GPU geometry/material coverage; exactMatch excludes edges, wholeImageExactMatch includes all pixels"));
	Report->SetNumberField(TEXT("sourceTriangles"), TriangleCount);
	Report->SetStringField(TEXT("material"), Material->GetPathName());
	Report->SetNumberField(TEXT("resolution"), Resolution);
	Report->SetNumberField(TEXT("cameraDistance"), CameraDistance);
	Report->SetNumberField(TEXT("materialQuality"), IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaterialQualityLevel"))->GetInt());
	Report->SetBoolField(TEXT("ditherOpacityMask"), Material->GetMaterial()->DitherOpacityMask);
	Report->SetNumberField(TEXT("comparedPixels"), Compared);
	Report->SetNumberField(TEXT("referenceCovered"), ReferenceCovered);
	Report->SetNumberField(TEXT("candidateCovered"), CandidateCovered);
	Report->SetNumberField(TEXT("falsePositive"), FalsePositive);
	Report->SetNumberField(TEXT("falseNegative"), FalseNegative);
	Report->SetNumberField(TEXT("rasterReferenceMismatches"), RasterReferenceMismatches);
	Report->SetNumberField(TEXT("explicitReferenceMismatches"), ExplicitReferenceMismatches);
	Report->SetNumberField(TEXT("maximumImplicitExplicitDifference"), MaximumImplicitExplicitDifference);
	Report->SetNumberField(TEXT("replayedReferenceMismatches"), ReplayedReferenceMismatches);
	Report->SetNumberField(TEXT("correctedUVReferenceMismatches"), CorrectedReferenceMismatches[0]);
	Report->SetNumberField(TEXT("correctedGradientsReferenceMismatches"), CorrectedReferenceMismatches[1]);
	Report->SetNumberField(TEXT("correctedBothReferenceMismatches"), CorrectedReferenceMismatches[2]);
	Report->SetNumberField(TEXT("maximumUVDifference"), MaximumUVDifference);
	Report->SetNumberField(TEXT("maximumGradientDifference"), MaximumGradientDifference);
	Report->SetNumberField(TEXT("diagnosticCandidateMismatches"), DiagnosticCandidateMismatches);
	Report->SetNumberField(TEXT("maximumRawMaskDifference"), MaximumRawMaskDifference);
	Report->SetArrayField(TEXT("mismatchDetails"), Mismatches);
	Report->SetArrayField(TEXT("candidateMismatchDetails"), CandidateMismatches);
	Report->SetStringField(TEXT("image"), OutputPng);
	auto Json = FString{};
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
	return Json;
}
