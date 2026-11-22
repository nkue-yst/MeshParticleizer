#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerCoverageGpu.h"
#include "MeshParticleizerCoverageValidation.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
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

namespace MeshParticleizerCoverageViews
{
	struct FObservation
	{
		FVector Location;
		FMatrix Rotation;
		FMatrix ViewProjection;
		TArray<FLinearColor> Reference;
		TArray<FLinearColor> Solid;
		TArray<FLinearColor> Actual;
		TArray<FUintVector4> Keys;
	};
}

FString UMeshParticleizerPreparationLibrary::CompareMaterialCoverageViews(UMaterialInterface* Material,
	const TArray<FVector>& ViewDirections, const FString& OutputPng, int32 CandidateResolution,
	int32 ViewResolution, float CameraDistance)
{
	using namespace MeshParticleizerCoverageViews;
	if (Material == nullptr || Material->GetMaterial() == nullptr || Material->GetBlendMode() != BLEND_Masked ||
		ViewDirections.Num() < 2 || ViewDirections.Num() > 4 ||
		ViewDirections.ContainsByPredicate([](const FVector& D) {
			return D.ContainsNaN() || D.IsNearlyZero() || D.Z <= 0;
		}) ||
		CandidateResolution < 16 || CandidateResolution > 128 || ViewResolution < 32 || ViewResolution > 512 ||
		FMath::IsFinite(CameraDistance) == false || CameraDistance <= 0 || CameraDistance > 100000 ||
		FPaths::GetExtension(OutputPng).Equals(TEXT("png"), ESearchCase::IgnoreCase) == false)
	{
		return TEXT("View comparison requires a Masked material, 2..4 finite upper-hemisphere directions, bounded resolutions, positive distance and PNG path.");
	}

	auto Plane = TStrongObjectPtr<UStaticMesh>(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	if (Plane.IsValid() == false || Plane->GetRenderData() == nullptr || Plane->GetRenderData()->LODResources.IsEmpty())
	{
		return TEXT("Engine validation plane is unavailable.");
	}
	const auto& LOD = Plane->GetRenderData()->LODResources[0];
	const auto Indices = LOD.IndexBuffer.GetArrayView();
	if (Indices.Num() != 6 || LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		return TEXT("View comparison requires the two-triangle UV0 plane.");
	}

	// カメラに依存しないUV格子で同じ候補を作り、各Viewへ同じ順序で渡す。
	const auto Count = CandidateResolution * CandidateResolution;
	auto Positions = TArray<FVector4f>{};
	auto Normals = TArray<FVector4f>{};
	auto Coordinates = TArray<FVector4f>{};
	auto Triangles = TArray<FVector4f>{};
	Positions.Init(FVector4f(0, 0, 0, -1), Count);
	Normals.SetNumZeroed(Count);
	Coordinates.SetNumZeroed(Count);
	for (auto Triangle = 0; Triangle < 2; ++Triangle)
	{
		FVector2d UV[3];
		FVector3f P[3], N[3];
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Vertex = Indices[Triangle * 3 + Corner];
			UV[Corner] = FVector2d(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 0));
			P[Corner] = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex);
			N[Corner] = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex);
		}
		Triangles.Add(FVector4f(UV[0].X, UV[0].Y, UV[1].X, UV[1].Y));
		Triangles.Add(FVector4f(UV[2].X, UV[2].Y, 0, 1));
		for (const auto& Position : P)
		{
			Triangles.Add(FVector4f(Position, 0));
		}
		for (const auto& Normal : N)
		{
			Triangles.Add(FVector4f(Normal, 0));
		}
		const auto A = UV[1] - UV[0];
		const auto B = UV[2] - UV[0];
		const auto Det = A.X * B.Y - A.Y * B.X;
		if (Det == 0)
		{
			return TEXT("Validation plane has degenerate UVs.");
		}
		for (auto Index = 0; Index < Count; ++Index)
		{
			if (Positions[Index].W >= 0)
			{
				continue;
			}
			const auto SampleUV = FVector2d((Index % CandidateResolution + 0.5) / CandidateResolution,
				(Index / CandidateResolution + 0.5) / CandidateResolution);
			const auto Delta = SampleUV - UV[0];
			const auto W1 = (Delta.X * B.Y - Delta.Y * B.X) / Det;
			const auto W2 = (A.X * Delta.Y - A.Y * Delta.X) / Det;
			const auto W0 = 1 - W1 - W2;
			if (FMath::Min3(W0, W1, W2) < 0)
			{
				continue;
			}
			Positions[Index] = FVector4f(P[0] * W0 + P[1] * W1 + P[2] * W2, 0);
			Normals[Index] = FVector4f((N[0] * W0 + N[1] * W1 + N[2] * W2).GetSafeNormal(), Triangle);
			Coordinates[Index] = FVector4f(SampleUV.X, SampleUV.Y, W0, W1);
		}
	}
	if (Positions.ContainsByPredicate([](const FVector4f& P) {
			return P.W < 0;
		}))
	{
		return TEXT("UV grid has unmapped candidates.");
	}

	// 参照はUE標準Static Mesh描画。穴なし深度も取得し、描画断片がない画素を別集計する。
	auto Instance = MeshParticleizerCoverageValidation::CreateInstance(Material);
	const auto Noise = MeshParticleizerCoverageValidation::GetNoiseTexture();
	if (Noise.IsValid() == false)
	{
		return TEXT("UE blue noise texture is unavailable.");
	}
	auto Scene = FPreviewScene(FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(false).SetTransactional(false));
	auto Component = TStrongObjectPtr<UStaticMeshComponent>(NewObject<UStaticMeshComponent>());
	Component->SetStaticMesh(Plane.Get());
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Scene.AddComponent(Component.Get(), FTransform::Identity);
	auto Target = TStrongObjectPtr<UTextureRenderTarget2D>(NewObject<UTextureRenderTarget2D>());
	Target->ClearColor = FLinearColor(1.e8f, 0, 0, 1);
	Target->InitCustomFormat(ViewResolution, ViewResolution, PF_R32_FLOAT, true);
	Target->UpdateResourceImmediate(true);
	auto Capture = TStrongObjectPtr<USceneCaptureComponent2D>(NewObject<USceneCaptureComponent2D>());
	Capture->bCaptureEveryFrame = Capture->bCaptureOnMovement = false;
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
	Scene.AddComponent(Capture.Get(), FTransform::Identity);
	auto Cases = TArray<FObservation>{};
	auto ReadFlags = FReadSurfaceDataFlags(RCM_MinMax);
	ReadFlags.SetLinearToGamma(false);
	for (const auto& Direction : ViewDirections)
	{
		auto& Case = Cases.AddDefaulted_GetRef();
		Case.Location = Direction.GetSafeNormal() * CameraDistance;
		const auto Rotation = FRotationMatrix::MakeFromX(-Direction.GetSafeNormal()).ToQuat();
		Case.Rotation = FTransform(Rotation).ToInverseMatrixWithScale() * FMatrix(FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
		Case.ViewProjection = FTranslationMatrix(-Case.Location) * Case.Rotation * Capture->CustomProjectionMatrix;
		Capture->SetWorldTransform(FTransform(Rotation, Case.Location));
		for (auto Mode = 0; Mode < 2; ++Mode)
		{
			Component->SetMaterial(0, Mode == 0 ? Material : UMaterial::GetDefaultMaterial(MD_Surface));
			Capture->CaptureScene();
			FlushRenderingCommands();
			GShaderCompilingManager->FinishAllCompilation();
			{
				auto Recreate = FComponentRecreateRenderStateContext(Component.Get());
			}
			Capture->CaptureScene();
			FlushRenderingCommands();
			auto& Pixels = Mode == 0 ? Case.Reference : Case.Solid;
			if (Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, ReadFlags) == false ||
				Pixels.Num() != ViewResolution * ViewResolution)
			{
				return TEXT("Independent view depth readback failed.");
			}
		}
	}

	auto Union = TArray<FLinearColor>{};
	auto Reverse = TArray<FLinearColor>{};
	auto bAccepted = true;
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageViews)([Proxy = Instance->GetRenderProxy(), Noise, &Cases, &Union, &Reverse,
															  &bAccepted, &Positions, &Normals, &Coordinates, &Triangles, CandidateResolution, ViewResolution,
															  Projection = Capture->CustomProjectionMatrix](FRHICommandListImmediate& RHICmdList) {
		auto Family = FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(nullptr, nullptr,
			FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime::GetTimeSinceAppStart()));
		auto Views = TArray<TUniquePtr<FSceneView>>{};
		const auto Rect = FIntRect(0, 0, ViewResolution, ViewResolution);
		for (const auto& Case : Cases)
		{
			auto Options = FSceneViewInitOptions{};
			Options.ViewFamily = &Family;
			Options.ViewOrigin = Case.Location;
			Options.ViewRotationMatrix = Case.Rotation;
			Options.ProjectionMatrix = Projection;
			Options.SetViewRectangle(Rect);
			auto View = MakeUnique<FSceneView>(Options);
			auto Uniforms = FViewUniformShaderParameters{};
			View->SetupCommonViewUniformBufferParameters(Uniforms, Rect.Size(), 1, Rect, View->ViewMatrices, View->ViewMatrices);
			MeshParticleizerCoverageValidation::SetViewResources(Uniforms, Noise);
			View->ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(Uniforms, UniformBuffer_SingleFrame);
			Views.Add(MoveTemp(View));
		}
		auto Graph = FRDGBuilder(RHICmdList);
		auto Texture = [&](const TCHAR* Name, const TArray<FVector4f>& Rows) {
			const auto Width = FMath::Min(CandidateResolution, Rows.Num());
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
		Input.TriangleCount = 2;
		Input.MaterialSlots = 1;
		const uint32 DynamicSlot = 1;
		Input.DynamicSlots = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CoverageViews.Slots"));
		Graph.QueueBufferUpload(Input.DynamicSlots, &DynamicSlot, sizeof(DynamicSlot));
		Input.Positions = Texture(TEXT("CoverageViews.Positions"), Positions);
		Input.Normals = Texture(TEXT("CoverageViews.Normals"), Normals);
		Input.Coordinates = Texture(TEXT("CoverageViews.Coordinates"), Coordinates);
		Input.TriangleData = Texture(TEXT("CoverageViews.Triangles"), Triangles);
		auto* Samples = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Input.Count * 4), TEXT("CoverageViews.Samples"));
		auto* Heads = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshParticleizerCoverageBucketCount(Input.Count)), TEXT("CoverageViews.Heads"));
		auto* Links = Graph.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FUintVector4), Input.Count), TEXT("CoverageViews.Links"));
		const auto OutputSize = FIntPoint(CandidateResolution, CandidateResolution);
		const auto Desc = FRDGTextureDesc::Create2D(OutputSize, PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV);
		auto* Combined = Graph.CreateTexture(Desc, TEXT("CoverageViews.Union"));
		auto* Reversed = Graph.CreateTexture(Desc, TEXT("CoverageViews.Reverse"));
		bAccepted = AddMeshParticleizerCoverageClearPass(Graph, Input.Count, 1, Input.Positions, Input.DynamicSlots, Combined) &&
			AddMeshParticleizerCoverageClearPass(Graph, Input.Count, 1, Input.Positions, Input.DynamicSlots, Reversed);
		auto IndividualReadbacks = TArray<TRefCountPtr<IPooledRenderTarget>>{};
		auto KeyReadbacks = TArray<TUniquePtr<FRHIGPUBufferReadback>>{};
		IndividualReadbacks.SetNum(Views.Num());
		for (auto ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			// 同じ候補・索引バッファをView間で使い回し、個別評価と最大値合成を比較する。
			const auto& View = *Views[ViewIndex];
			AddClearUAVPass(Graph, Graph.CreateUAV(Links, PF_R32G32B32A32_UINT), MAX_uint32);
			bAccepted &= AddMeshParticleizerCoverageGeometryPass(Graph, View, Input, Samples) &&
				AddMeshParticleizerCoverageRasterInputPass(Graph, View, Rect, Input, Samples, Heads, Links) &&
				AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, Samples, Input.Count, Combined);
			auto* Individual = Graph.CreateTexture(Desc, TEXT("CoverageViews.Individual"));
			AddClearRenderTargetPass(Graph, Individual, FLinearColor::Black);
			bAccepted &= AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, Samples, Input.Count, Individual);
			Graph.QueueTextureExtraction(Individual, &IndividualReadbacks[ViewIndex]);
			auto Reader = MakeUnique<FRHIGPUBufferReadback>(TEXT("CoverageViews.Keys"));
			AddEnqueueCopyPass(Graph, Reader.Get(), Links, Input.Count * sizeof(FUintVector4));
			KeyReadbacks.Add(MoveTemp(Reader));
		}
		for (auto ViewIndex = Views.Num() - 1; ViewIndex >= 0; --ViewIndex)
		{
			const auto& View = *Views[ViewIndex];
			bAccepted &= AddMeshParticleizerCoverageGeometryPass(Graph, View, Input, Samples) &&
				AddMeshParticleizerCoverageRasterInputPass(Graph, View, Rect, Input, Samples, Heads, Links) &&
				AddMeshParticleizerCoveragePass(Graph, View, Proxy, 0, Samples, Input.Count, Reversed);
		}
		if (bAccepted == false)
		{
			return;
		}
		TRefCountPtr<IPooledRenderTarget> UnionReadback, ReverseReadback;
		Graph.QueueTextureExtraction(Combined, &UnionReadback);
		Graph.QueueTextureExtraction(Reversed, &ReverseReadback);
		Graph.Execute();
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		auto Flags = FReadSurfaceDataFlags(RCM_MinMax);
		Flags.SetLinearToGamma(false);
		const auto OutputRect = FIntRect(FIntPoint::ZeroValue, OutputSize);
		for (auto Index = 0; Index < Cases.Num(); ++Index)
		{
			RHICmdList.ReadSurfaceData(IndividualReadbacks[Index]->GetRHI(), OutputRect, Cases[Index].Actual, Flags);
			Cases[Index].Keys.SetNum(Input.Count);
			FMemory::Memcpy(Cases[Index].Keys.GetData(), KeyReadbacks[Index]->Lock(Input.Count * sizeof(FUintVector4)), Input.Count * sizeof(FUintVector4));
			KeyReadbacks[Index]->Unlock();
		}
		RHICmdList.ReadSurfaceData(UnionReadback->GetRHI(), OutputRect, Union, Flags);
		RHICmdList.ReadSurfaceData(ReverseReadback->GetRHI(), OutputRect, Reverse, Flags);
	});
	FlushRenderingCommands();
	if (bAccepted == false || Union.Num() != Count || Reverse.Num() != Count ||
		Cases.ContainsByPredicate([Count](const FObservation& Case) {
			return Case.Actual.Num() != Count || Case.Keys.Num() != Count;
		}))
	{
		return TEXT("Multi-view GPU evaluation or readback failed.");
	}

	// 独立したCPU投影で参照画素を選ぶ。描画断片がない候補を合格数へ混ぜない。
	auto ExpectedUnion = TBitArray<>(false, Count);
	auto AllComparable = TBitArray<>(true, Count);
	auto ActualUnion = TBitArray<>(false, Count);
	auto ReferenceViewCounts = TArray<int32>{};
	ReferenceViewCounts.SetNumZeroed(Count);
	auto ViewReports = TArray<TSharedPtr<FJsonValue>>{};
	auto Image = TArray<FColor>{};
	Image.Init(FColor(48, 48, 48), Count * 3 * (Cases.Num() + 1));
	auto WritePixel = [&](int32 ViewIndex, int32 Index, bool Expected, bool Actual, bool Comparable) {
		const auto Pixel = (ViewIndex * CandidateResolution + Index / CandidateResolution) * CandidateResolution * 3 + Index % CandidateResolution;
		Image[Pixel] = Expected ? FColor::White : FColor::Black;
		Image[Pixel + CandidateResolution] = Actual ? FColor::White : FColor::Black;
		Image[Pixel + CandidateResolution * 2] = Comparable == false ? FColor(48, 48, 48) : Expected != Actual ? (Actual ? FColor::Red : FColor::Blue)
																											   : (Expected ? FColor::White : FColor::Black);
	};
	auto AllViewExact = true;
	for (auto ViewIndex = 0; ViewIndex < Cases.Num(); ++ViewIndex)
	{
		const auto& Case = Cases[ViewIndex];
		auto Compared = 0;
		auto FalsePositive = 0;
		auto FalseNegative = 0;
		auto MappingMismatches = 0;
		auto Details = TArray<TSharedPtr<FJsonValue>>{};
		for (auto Index = 0; Index < Count; ++Index)
		{
			const auto Actual = Case.Actual[Index].R >= 0.5f;
			ActualUnion[Index] = ActualUnion[Index] || Actual;
			const auto Clip = Case.ViewProjection.TransformFVector4(FVector4(FVector(Positions[Index]), 1));
			auto X = -1;
			auto Y = -1;
			if (Clip.W > 0 && Clip.Z <= Clip.W)
			{
				const auto Screen = FVector2d(Clip.X, -Clip.Y) * (ViewResolution * 0.5 / Clip.W) + FVector2d(ViewResolution * 0.5);
				if (Screen.X >= 0 && Screen.Y >= 0 && Screen.X < ViewResolution && Screen.Y < ViewResolution)
				{
					X = FMath::FloorToInt(Screen.X);
					Y = FMath::FloorToInt(Screen.Y);
				}
			}
			const auto InBounds = X >= 0 && Y >= 0;
			const auto Pixel = InBounds ? Y * ViewResolution + X : 0;
			const auto HasFragment = InBounds && Case.Solid[Pixel].R > 0 && Case.Solid[Pixel].R < 5.e7f;
			const auto Expected = HasFragment && Case.Reference[Pixel].R > 0 && Case.Reference[Pixel].R < 5.e7f;
			AllComparable[Index] = AllComparable[Index] && HasFragment;
			ExpectedUnion[Index] = ExpectedUnion[Index] || Expected;
			ReferenceViewCounts[Index] += Expected;
			const auto& Key = Case.Keys[Index];
			const auto MappingMismatch = InBounds && (Key.X != uint32(X) || Key.Y != uint32(Y) || Key.Z != uint32(Normals[Index].W));
			MappingMismatches += MappingMismatch;
			if (HasFragment == true)
			{
				++Compared;
				FalsePositive += Actual && Expected == false;
				FalseNegative += Expected && Actual == false;
			}
			if ((MappingMismatch || (HasFragment && Expected != Actual)) && Details.Num() < 128)
			{
				auto Detail = MakeShared<FJsonObject>();
				Detail->SetNumberField(TEXT("candidate"), Index);
				Detail->SetNumberField(TEXT("x"), X);
				Detail->SetNumberField(TEXT("y"), Y);
				Detail->SetNumberField(TEXT("gpuX"), Key.X);
				Detail->SetNumberField(TEXT("gpuY"), Key.Y);
				Detail->SetBoolField(TEXT("mappingMismatch"), MappingMismatch);
				Detail->SetBoolField(TEXT("reference"), Expected);
				Detail->SetBoolField(TEXT("actual"), Actual);
				Details.Add(MakeShared<FJsonValueObject>(Detail));
			}
			WritePixel(ViewIndex, Index, Expected, Actual, HasFragment);
		}
		auto Report = MakeShared<FJsonObject>();
		Report->SetNumberField(TEXT("view"), ViewIndex);
		Report->SetNumberField(TEXT("compared"), Compared);
		Report->SetNumberField(TEXT("withoutReferenceFragment"), Count - Compared);
		Report->SetNumberField(TEXT("falsePositive"), FalsePositive);
		Report->SetNumberField(TEXT("falseNegative"), FalseNegative);
		Report->SetNumberField(TEXT("mappingMismatches"), MappingMismatches);
		Report->SetArrayField(TEXT("details"), Details);
		ViewReports.Add(MakeShared<FJsonValueObject>(Report));
		AllViewExact &= Compared > 0 && FalsePositive == 0 && FalseNegative == 0 && MappingMismatches == 0;
	}
	auto ComparedUnion = 0;
	auto UnionMismatches = 0;
	auto AccumulationMismatches = 0;
	auto OrderMismatches = 0;
	auto ViewDependentCandidates = 0;
	for (auto Index = 0; Index < Count; ++Index)
	{
		const auto Actual = Union[Index].R >= 0.5f;
		AccumulationMismatches += Actual != ActualUnion[Index];
		OrderMismatches += Actual != (Reverse[Index].R >= 0.5f);
		if (AllComparable[Index] == true)
		{
			++ComparedUnion;
			UnionMismatches += Actual != ExpectedUnion[Index];
			ViewDependentCandidates += ReferenceViewCounts[Index] > 0 && ReferenceViewCounts[Index] < Cases.Num();
		}
		WritePixel(Cases.Num(), Index, ExpectedUnion[Index], Actual, AllComparable[Index]);
	}
	auto Compressed = TArray64<uint8>{};
	FImageUtils::PNGCompressImageArray(CandidateResolution * 3, CandidateResolution * (Cases.Num() + 1), MakeArrayView(Image), Compressed);
	if (FFileHelper::SaveArrayToFile(Compressed, *OutputPng) == false)
	{
		return TEXT("Multi-view PNG could not be saved.");
	}
	auto Report = MakeShared<FJsonObject>();
	Report->SetBoolField(TEXT("success"), true);
	Report->SetBoolField(TEXT("exactMatch"), AllViewExact && ComparedUnion > 0 && UnionMismatches == 0 && AccumulationMismatches == 0 && OrderMismatches == 0);
	Report->SetStringField(TEXT("scope"), TEXT("Fixed UV0-plane candidates versus independent UE camera captures; union reference requires a surface fragment in every view"));
	Report->SetNumberField(TEXT("candidates"), Count);
	Report->SetNumberField(TEXT("viewResolution"), ViewResolution);
	Report->SetNumberField(TEXT("comparedUnion"), ComparedUnion);
	Report->SetNumberField(TEXT("viewDependentCandidates"), ViewDependentCandidates);
	Report->SetNumberField(TEXT("unionReferenceMismatches"), UnionMismatches);
	Report->SetNumberField(TEXT("accumulationMismatches"), AccumulationMismatches);
	Report->SetNumberField(TEXT("orderMismatches"), OrderMismatches);
	Report->SetArrayField(TEXT("views"), ViewReports);
	Report->SetStringField(TEXT("image"), OutputPng);
	auto Json = FString{};
	FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
	return Json;
}
