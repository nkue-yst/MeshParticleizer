#include "MeshParticleizerSurfaceProjection.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerSurfaceProjectionTest, "MeshParticleizer.Core.SurfaceProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerSurfaceProjectionTest::RunTest(const FString& Parameters)
{
	// 骨の混合率が三頂点で異なる512面。粒子候補やテクスチャ解像度は探索に渡さない。
	constexpr auto TriangleCount = 512;
	constexpr auto VertexCount = TriangleCount * 3;
	auto Vertices = TArray<FLinearColor>{};
	auto Triangles = TArray<FLinearColor>{};
	Vertices.SetNumZeroed(VertexCount * UMeshParticleizerPreparedMesh::SkinVertexStride);
	for (auto Triangle = 0; Triangle < TriangleCount; ++Triangle)
	{
		Triangles.Emplace(Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2, 0);
		const auto Base = FVector((Triangle % 32) * 6, (Triangle / 32) * 6, (Triangle % 3) * 2);
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Offset = (Triangle * 3 + Corner) * UMeshParticleizerPreparedMesh::SkinVertexStride;
			const auto Point = Base + (Corner == 0 ? FVector::ZeroVector : (Corner == 1 ? FVector(2, 0, 0) : FVector(0, 2, 0)));
			Vertices[Offset] = FLinearColor(Point.X, Point.Y, Point.Z, 1);
			Vertices[Offset + 2] = FLinearColor(0, 1, 0, 0);
			const auto Mix = Corner * 0.35f;
			const auto Sum = 0.99998f + Corner * 0.00002f;
			Vertices[Offset + 3] = FLinearColor((1 - Mix) * Sum, Mix * Sum, 0, 0);
		}
	}
	auto Projection = FMeshParticleizerSurfaceProjection{};
	auto Error = FString{};
	if (this->TestTrue(TEXT("Register the complete mesh topology"), Projection.Build(Vertices, VertexCount, Triangles, TriangleCount, 2, Error)) == false)
	{
		this->AddError(Error);
		return false;
	}

	// UPROPERTYの保存経路を通し、再読込後も探索木が有効であることを確認する。
	auto Bytes = TArray<uint8>{};
	auto Writer = FMemoryWriter(Bytes);
	FMeshParticleizerSurfaceProjection::StaticStruct()->SerializeItem(Writer, &Projection, nullptr);
	auto Restored = FMeshParticleizerSurfaceProjection{};
	auto Reader = FMemoryReader(Bytes);
	FMeshParticleizerSurfaceProjection::StaticStruct()->SerializeItem(Reader, &Restored, nullptr);
	this->TestTrue(TEXT("Projection data survives serialization"), Restored.IsReady());

	auto LargestVisited = 0;
	auto MaxPositionError = 0.0;
	const auto Bary = FVector(0.2, 0.3, 0.5);
	for (auto Pose = 0; Pose < 3; ++Pose)
	{
		auto Bones = TArray<FMatrix44f>{FMatrix44f::Identity, FMatrix44f::Identity};
		if (Pose > 0)
		{
			Bones[0] = FMatrix44f(FTransform(FRotator(15, 20, -5), FVector(-7, 3, 9)).ToMatrixWithScale());
			Bones[1] = FMatrix44f(FTransform(FRotator(-10, 35, 25), FVector(17, -5, 6), FVector(1.2, 0.8, 1.1)).ToMatrixWithScale());
		}
		const auto World = Pose == 2 ? FTransform(FRotator(25, -40, 15), FVector(1000000, -2000000, 300), FVector(-2, 0.5, 3)).ToMatrixWithScale() : FMatrix::Identity;
		const auto SkinPosition = [&Vertices, &Bones, &World](int32 Vertex) {
			const auto Offset = Vertex * UMeshParticleizerPreparedMesh::SkinVertexStride;
			const auto& P = Vertices[Offset];
			const auto Reference = FVector(P.R, P.G, P.B);
			const auto& W = Vertices[Offset + 3];
			const auto Local = FVector(FMatrix(Bones[0]).TransformPosition(Reference)) * W.R +
				FVector(FMatrix(Bones[1]).TransformPosition(Reference)) * W.G;
			return FVector(World.TransformPosition(Local));
		};
		for (const auto Triangle : {0, 31, 255, 511})
		{
			const auto A = SkinPosition(Triangle * 3);
			const auto B = SkinPosition(Triangle * 3 + 1);
			const auto C = SkinPosition(Triangle * 3 + 2);
			const auto Surface = A * Bary.X + B * Bary.Y + C * Bary.Z;
			const auto Point = Surface + FVector::CrossProduct(B - A, C - A).GetSafeNormal() * 0.25;
			auto Result = FMeshParticleizerProjectionResult{};
			if (this->TestTrue(TEXT("Find a posed triangle under mixed skinning and world scale"), Restored.Project(Bones, World, Point, 0.3, Result)) == false)
			{
				continue;
			}

			// 全面探索を独立した対照にし、保守的境界が正しい面を落としていないか調べる。
			auto BruteDistance = TNumericLimits<double>::Max();
			for (auto Other = 0; Other < TriangleCount; ++Other)
			{
				const auto Closest = FMath::ClosestPointOnTriangleToPoint(Point, SkinPosition(Other * 3), SkinPosition(Other * 3 + 1), SkinPosition(Other * 3 + 2));
				BruteDistance = FMath::Min(BruteDistance, FVector::DistSquared(Point, Closest));
			}
			this->TestTrue(TEXT("BVH nearest distance agrees with exhaustive reference"), FMath::IsNearlyEqual(FVector::DistSquared(Point, Result.WorldPosition), BruteDistance, 0.000001));
			if (Result.Triangle == Triangle)
			{
				auto Expected = FVector::ZeroVector;
				for (auto Corner = 0; Corner < 3; ++Corner)
				{
					const auto& P = Vertices[(Triangle * 3 + Corner) * UMeshParticleizerPreparedMesh::SkinVertexStride];
					Expected += FVector(P.R, P.G, P.B) * Bary[Corner];
				}
				MaxPositionError = FMath::Max(MaxPositionError, FVector::Distance(Expected, Result.ReferencePosition));
				this->TestTrue(TEXT("Barycentric mapping restores reference space"), Result.ReferencePosition.Equals(Expected, 0.0001));
			}
			LargestVisited = FMath::Max(LargestVisited, Result.TestedTriangles);
			if (Pose == 0)
			{
				this->TestTrue(TEXT("Spatial query avoids exhaustive triangle skinning"), Result.TestedTriangles < TriangleCount / 4);
			}
			this->TestFalse(TEXT("World-space distance limit rejects a distant surface"), Restored.Project(Bones, World, Point, 0.01, Result));
			this->TestEqual(TEXT("Failed projection clears the previous triangle"), Result.Triangle, INDEX_NONE);
		}
	}

	// 壊れた更新は採用せず、既存の登録データを保つ。
	Triangles[0].R = VertexCount;
	this->TestFalse(TEXT("Invalid topology cannot replace registered data"), Projection.Build(Vertices, VertexCount, Triangles, TriangleCount, 2, Error));
	this->TestTrue(TEXT("Failed rebuild preserves previous valid data"), Projection.IsReady());
	auto Result = FMeshParticleizerProjectionResult{};
	const auto Bones = TArray<FMatrix44f>{FMatrix44f::Identity, FMatrix44f::Identity};
	this->TestFalse(TEXT("Nonfinite query is rejected"), Projection.Project(Bones, FMatrix::Identity, FVector(std::numeric_limits<double>::quiet_NaN(), 0, 0), 1, Result));
	this->TestFalse(TEXT("Singular source transform is rejected"), Projection.Project(Bones, FTransform(FVector::ZeroVector).ToMatrixWithScale() * FScaleMatrix(FVector::ZeroVector), FVector::ZeroVector, 1, Result));
	this->AddInfo(FString::Printf(TEXT("Projection reference error %.9f cm; largest query evaluated %d/%d triangles; serialized data %d bytes."),
		MaxPositionError, LargestVisited, TriangleCount, Bytes.Num()));
	return true;
}
#endif
