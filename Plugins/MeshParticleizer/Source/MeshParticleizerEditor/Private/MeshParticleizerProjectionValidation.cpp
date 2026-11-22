#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerOriginLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerSurfaceBaker.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshRenderData.h"

FString UMeshParticleizerPreparationLibrary::CompareSurfaceProjection(USkinnedMeshComponent* Source,
	UMeshParticleizerPreparedMesh* Prepared)
{
	auto* Mesh = Source != nullptr ? Cast<USkeletalMesh>(Source->GetSkinnedAsset()) : nullptr;
	if (Mesh == nullptr || Prepared == nullptr || Prepared->SkeletalMesh.Get() != Mesh ||
		Prepared->SurfaceProjection.IsReady() == false || Mesh->GetResourceForRendering() == nullptr ||
		Mesh->GetResourceForRendering()->LODRenderData.IsValidIndex(Prepared->SamplingLOD) == false)
	{
		return TEXT("Invalid surface projection validation inputs.");
	}

	auto Description = FMeshDescription{};
	auto Surface = MeshParticleizerPreparation::FSurfaceSource{};
	auto Error = FString{};
	if (MeshParticleizerPreparation::BuildSkeletalSource(Mesh, Description, Surface, Error, Prepared->SamplingLOD) == false)
	{
		return Error;
	}
	auto& LOD = Mesh->GetResourceForRendering()->LODRenderData[Prepared->SamplingLOD];
	auto* Weights = Source->GetSkinWeightBuffer(Prepared->SamplingLOD);
	if (Weights == nullptr)
	{
		return TEXT("Skin weights are unavailable.");
	}
	auto Matrices = TArray<FMatrix44f>{};
	Source->GetCurrentRefToLocalMatrices(Matrices, Prepared->SamplingLOD);
	auto Skinned = TArray<FVector3f>{};
	USkinnedMeshComponent::ComputeSkinnedPositions(Source, Skinned, Matrices, LOD, *Weights);
	if (Skinned.Num() != static_cast<int32>(LOD.GetNumVertices()))
	{
		return TEXT("UE skinning reference is incomplete.");
	}

	// 全頂点のUE標準評価を対照にする。製品API内では全面評価もGPU同期も行わない。
	auto WorldVertices = TArray<FVector>{};
	for (const auto& Position : Skinned)
	{
		WorldVertices.Add(Source->GetComponentTransform().TransformPosition(FVector(Position)));
	}
	auto Timings = TArray<double>{};
	auto MaxWorldError = 0.0;
	auto MaxReferenceError = 0.0;
	auto MaxDistance = 0.0;
	auto MaxTested = 0;
	auto MaxNodes = 0;
	const auto TriangleCount = Description.Triangles().Num();
	const auto Step = FMath::Max(1, TriangleCount / 128);
	const auto Bary = FVector(0.2, 0.3, 0.5);
	for (auto TriangleIndex = 0; TriangleIndex < TriangleCount && Timings.Num() < 128; TriangleIndex += Step)
	{
		const auto& Triangle = Surface.SkinTriangles[TriangleIndex];
		const auto A = WorldVertices[static_cast<int32>(Triangle.R)];
		const auto B = WorldVertices[static_cast<int32>(Triangle.G)];
		const auto C = WorldVertices[static_cast<int32>(Triangle.B)];
		const auto Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
		if (Normal.IsNearlyZero() == true)
		{
			continue;
		}
		const auto Point = A * Bary.X + B * Bary.Y + C * Bary.Z + Normal * 0.1;
		auto Hit = FHitResult(Source->GetOwner(), Source, Point, Normal);
		Hit.bBlockingHit = true;
		const auto Started = FPlatformTime::Seconds();
		const auto Result = UMeshParticleizerOriginLibrary::CaptureSurfaceHitOrigin(Hit, Prepared, 1);
		const auto Milliseconds = (FPlatformTime::Seconds() - Started) * 1000;
		if (Result.Accuracy != EMeshParticleizerOriginAccuracy::SurfaceProjection)
		{
			return Result.Diagnostics;
		}
		if (Result.TriangleIndex < 0 || Result.TriangleIndex >= TriangleCount)
		{
			return TEXT("Projection returned an invalid prepared triangle.");
		}
		Timings.Add(Milliseconds);

		const auto& ProjectedTriangle = Surface.SkinTriangles[Result.TriangleIndex];
		auto ExpectedWorld = FVector::ZeroVector;
		auto ExpectedReference = FVector::ZeroVector;
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Vertex = static_cast<int32>(ProjectedTriangle.Component(Corner));
			ExpectedWorld += WorldVertices[Vertex] * Result.Barycentrics[Corner];
			ExpectedReference += FVector(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex)) * Result.Barycentrics[Corner];
		}
		MaxWorldError = FMath::Max(MaxWorldError, FVector::Distance(ExpectedWorld, Result.ProjectedWorldPosition));
		MaxReferenceError = FMath::Max(MaxReferenceError, FVector::Distance(ExpectedReference, Result.ReferencePosition));
		MaxDistance = FMath::Max(MaxDistance, Result.ProjectionDistance);

		// 探索量の診断は別呼出しにし、公開APIの時間へ二重計上しない。
		auto Probe = FMeshParticleizerProjectionResult{};
		Prepared->SurfaceProjection.Project(Matrices, Source->GetComponentTransform().ToMatrixWithScale(), Point, 1, Probe);
		MaxTested = FMath::Max(MaxTested, Probe.TestedTriangles);
		MaxNodes = FMath::Max(MaxNodes, Probe.VisitedNodes);
	}
	if (Timings.IsEmpty() == true)
	{
		return TEXT("No nondegenerate triangle could be validated.");
	}
	const auto First = Timings[0];
	Timings.Sort();
	return FString::Printf(TEXT("{\"queries\":%d,\"triangles\":%d,\"max_world_error_cm\":%.9f,\"max_reference_error_cm\":%.9f,\"max_projection_distance_cm\":%.9f,\"api_first_ms\":%.6f,\"api_p95_ms\":%.6f,\"api_max_ms\":%.6f,\"max_tested_triangles\":%d,\"max_visited_nodes\":%d}"),
		Timings.Num(), TriangleCount, MaxWorldError, MaxReferenceError, MaxDistance, First,
		Timings[FMath::FloorToInt((Timings.Num() - 1) * 0.95)], Timings.Last(), MaxTested, MaxNodes);
}
