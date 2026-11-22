#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerCoverageValidation.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

namespace MeshParticleizerCoverageTopology
{
	struct FTriangle
	{
		int32 Index = INDEX_NONE;
		FVector3f Position[3];
		FVector2f UV[3];
		FVector3f Normal[3];
		uint32 Vertices[3] = {};
		double LongestEdge = 0;
		double Aspect = 0;
	};

	FTriangle ReadTriangle(const FSkeletalMeshLODRenderData& LOD, const TArray<uint32>& Indices, int32 Index)
	{
		auto Triangle = FTriangle{};
		Triangle.Index = Index;
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Vertex = Indices[Index * 3 + Corner];
			Triangle.Vertices[Corner] = Vertex;
			Triangle.Position[Corner] = LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex);
			Triangle.UV[Corner] = LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, 0);
			Triangle.Normal[Corner] = LOD.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex);
			Triangle.LongestEdge = FMath::Max(Triangle.LongestEdge,
				FVector3d(Triangle.Position[Corner] - LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Indices[Index * 3 + (Corner + 1) % 3])).Length());
		}
		const auto DoubleArea = FVector3d::CrossProduct(FVector3d(Triangle.Position[1] - Triangle.Position[0]),
			FVector3d(Triangle.Position[2] - Triangle.Position[0]))
									.Length();
		Triangle.Aspect = DoubleArea > 0 ? Triangle.LongestEdge * Triangle.LongestEdge / DoubleArea : 0;
		return Triangle;
	}

	TSharedPtr<FJsonObject> DescribeTriangle(const FTriangle& Triangle)
	{
		auto Report = MakeShared<FJsonObject>();
		Report->SetNumberField(TEXT("triangle"), Triangle.Index);
		Report->SetNumberField(TEXT("longestEdgeCm"), Triangle.LongestEdge);
		Report->SetNumberField(TEXT("aspect"), Triangle.Aspect);
		auto Normal = FVector3d::CrossProduct(FVector3d(Triangle.Position[1] - Triangle.Position[0]),
			FVector3d(Triangle.Position[2] - Triangle.Position[0]))
						  .GetSafeNormal();
		if (FVector3d::DotProduct(Normal, FVector3d(Triangle.Normal[0] + Triangle.Normal[1] + Triangle.Normal[2])) < 0)
		{
			Normal *= -1;
		}
		const auto Tangent = FVector3d(Triangle.Position[1] - Triangle.Position[0]).GetSafeNormal();
		for (const auto& Entry : {TPair<const TCHAR*, FVector3d>(TEXT("normal"), Normal), {TEXT("tangent"), Tangent}})
		{
			auto Values = TArray<TSharedPtr<FJsonValue>>{};
			for (auto Axis = 0; Axis < 3; ++Axis)
			{
				Values.Add(MakeShared<FJsonValueNumber>(Entry.Value[Axis]));
			}
			Report->SetArrayField(Entry.Key, Values);
		}
		return Report;
	}

	FString Serialize(const TSharedPtr<FJsonObject>& Report)
	{
		auto Json = FString{};
		FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
		return Json;
	}
}

FString UMeshParticleizerPreparationLibrary::InspectSkeletalCoverageTopology(USkeletalMesh* Mesh, int32 MaterialSlot, int32 Limit)
{
	using namespace MeshParticleizerCoverageTopology;
	const auto* RenderData = Mesh != nullptr ? Mesh->GetResourceForRendering() : nullptr;
	if (RenderData == nullptr || RenderData->LODRenderData.IsEmpty() ||
		Mesh->GetMaterials().IsValidIndex(MaterialSlot) == false || Limit < 1 || Limit > 16)
	{
		return TEXT("Topology inspection requires built LOD0, a valid material slot and limit 1..16.");
	}
	const auto& LOD = RenderData->LODRenderData[0];
	if (LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		return TEXT("LOD0 has no UV0.");
	}
	auto Indices = TArray<uint32>{};
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	auto Triangles = TArray<FTriangle>{};
	for (const auto& Section : LOD.RenderSections)
	{
		if (Section.bDisabled == true || Section.MaterialIndex != MaterialSlot)
		{
			continue;
		}
		for (auto Offset = Section.BaseIndex; Offset < Section.BaseIndex + Section.NumTriangles * 3; Offset += 3)
		{
			Triangles.Add(ReadTriangle(LOD, Indices, Offset / 3));
		}
	}

	// 描画頂点の番号が違っても、位置が一致する辺を突き合わせてUVの不連続を探す。
	struct FEdge
	{
		int32 Triangle;
		FVector2f A;
		FVector2f B;
	};
	auto PositionIds = TMap<FVector3f, uint32>{};
	auto Edges = TMap<uint64, TArray<FEdge>>{};
	auto Degenerate = 0;
	for (auto Index = 0; Index < Triangles.Num(); ++Index)
	{
		const auto& Triangle = Triangles[Index];
		if (Triangle.Aspect == 0)
		{
			++Degenerate;
			continue;
		}
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			uint32 Id[2];
			for (auto End = 0; End < 2; ++End)
			{
				const auto Position = Triangle.Position[(Corner + End) % 3];
				const auto Next = PositionIds.Num();
				Id[End] = PositionIds.FindOrAdd(Position, Next);
			}
			if (Id[0] == Id[1])
			{
				continue;
			}
			const auto Reverse = Id[0] > Id[1];
			const auto Key = (uint64(FMath::Min(Id[0], Id[1])) << 32) | FMath::Max(Id[0], Id[1]);
			Edges.FindOrAdd(Key).Add({Index, Triangle.UV[(Corner + (Reverse ? 1 : 0)) % 3], Triangle.UV[(Corner + (Reverse ? 0 : 1)) % 3]});
		}
	}
	struct FSeam
	{
		int32 A;
		int32 B;
		double Delta;
	};
	auto Seams = TArray<FSeam>{};
	for (const auto& Entry : Edges)
	{
		const auto& Shared = Entry.Value;
		for (auto A = 0; A < Shared.Num(); ++A)
		{
			for (auto B = A + 1; B < Shared.Num(); ++B)
			{
			const auto Delta = FMath::Max((Shared[A].A - Shared[B].A).Length(), (Shared[A].B - Shared[B].B).Length());
				if (Delta > 1.e-6)
				{
					Seams.Add({Shared[A].Triangle, Shared[B].Triangle, Delta});
				}
			}
		}
	}
	Seams.Sort([&Triangles](const FSeam& A, const FSeam& B) {
		return A.Delta != B.Delta ? A.Delta > B.Delta : Triangles[A.A].Index < Triangles[B.A].Index;
	});
	auto SeamReports = TArray<TSharedPtr<FJsonValue>>{};
	for (auto Index = 0; Index < FMath::Min(Limit, Seams.Num()); ++Index)
	{
		const auto& Seam = Seams[Index];
		auto Report = MakeShared<FJsonObject>();
		Report->SetObjectField(TEXT("a"), DescribeTriangle(Triangles[Seam.A]));
		Report->SetObjectField(TEXT("b"), DescribeTriangle(Triangles[Seam.B]));
		Report->SetNumberField(TEXT("uvDelta"), Seam.Delta);
		SeamReports.Add(MakeShared<FJsonValueObject>(Report));
	}
	Triangles.Sort([](const FTriangle& A, const FTriangle& B) {
		return A.Aspect != B.Aspect ? A.Aspect > B.Aspect : A.Index < B.Index;
	});
	auto ThinReports = TArray<TSharedPtr<FJsonValue>>{};
	for (auto Index = 0; Index < FMath::Min(Limit, Triangles.Num()); ++Index)
	{
		ThinReports.Add(MakeShared<FJsonValueObject>(DescribeTriangle(Triangles[Index])));
	}
	auto Report = MakeShared<FJsonObject>();
	Report->SetBoolField(TEXT("success"), true);
	Report->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	Report->SetNumberField(TEXT("slot"), MaterialSlot);
	Report->SetNumberField(TEXT("triangles"), Triangles.Num());
	Report->SetNumberField(TEXT("degenerateTriangles"), Degenerate);
	Report->SetNumberField(TEXT("uvSeamPairs"), Seams.Num());
	Report->SetArrayField(TEXT("thinTriangles"), ThinReports);
	Report->SetArrayField(TEXT("seams"), SeamReports);
	return MeshParticleizerCoverageTopology::Serialize(Report);
}

FString UMeshParticleizerPreparationLibrary::CompareSkeletalCoverageTriangle(USkeletalMesh* Mesh, int32 TriangleIndex,
	FVector ViewDirection, const FString& OutputPng, int32 Resolution, float CameraDistanceScale)
{
	return CompareSkeletalCoveragePatch(Mesh, {TriangleIndex}, ViewDirection, OutputPng, Resolution, CameraDistanceScale);
}

FString UMeshParticleizerPreparationLibrary::CompareSkeletalCoveragePatch(USkeletalMesh* Mesh, const TArray<int32>& TriangleIndices,
	FVector ViewDirection, const FString& OutputPng, int32 Resolution, float CameraDistanceScale)
{
	using namespace MeshParticleizerCoverageTopology;
	const auto* RenderData = Mesh != nullptr ? Mesh->GetResourceForRendering() : nullptr;
	if (RenderData == nullptr || RenderData->LODRenderData.IsEmpty() || TriangleIndices.IsEmpty() || TriangleIndices.Num() > 2 ||
		(TriangleIndices.Num() == 2 && TriangleIndices[0] == TriangleIndices[1]) ||
		FMath::IsFinite(CameraDistanceScale) == false || CameraDistanceScale <= 0 || CameraDistanceScale > 100)
	{
		return TEXT("Patch comparison requires built LOD0, 1..2 distinct triangles and camera distance scale in (0,100].");
	}
	const auto& LOD = RenderData->LODRenderData[0];
	auto Indices = TArray<uint32>{};
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	if (TriangleIndices.ContainsByPredicate([&Indices](int32 Index) {
			return Index < 0 || Index >= Indices.Num() / 3;
		}) ||
		LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		return TEXT("Triangle or UV0 is unavailable.");
	}
	auto Slot = int32(INDEX_NONE);
	auto Triangles = TArray<FTriangle>{};
	for (const auto TriangleIndex : TriangleIndices)
	{
		auto TriangleSlot = int32(INDEX_NONE);
		for (const auto& Section : LOD.RenderSections)
		{
			if (Section.bDisabled == false && uint32(TriangleIndex) >= Section.BaseIndex / 3 &&
				uint32(TriangleIndex) < Section.BaseIndex / 3 + Section.NumTriangles)
			{
				TriangleSlot = Section.MaterialIndex;
				break;
			}
		}
		if (TriangleSlot == INDEX_NONE || (Slot != INDEX_NONE && Slot != TriangleSlot))
		{
			return TEXT("Patch triangles require the same enabled material slot.");
		}
		Slot = TriangleSlot;
		auto Triangle = ReadTriangle(LOD, Indices, TriangleIndex);
		if (Triangle.Aspect == 0)
		{
			return TEXT("Degenerate source triangle has no drawable surface.");
		}
		Triangles.Add(Triangle);
	}
	if (Mesh->GetMaterials().IsValidIndex(Slot) == false)
	{
		return TEXT("Triangle has no enabled material section.");
	}
	auto* Material = Mesh->GetMaterials()[Slot].MaterialInterface.Get();
	if (Material == nullptr || Material->GetBlendMode() != BLEND_Masked)
	{
		return TEXT("Triangle requires its original Masked material.");
	}

	// 形状を平面へ作り直さず、LOD0の位置・UV・法線・接線を一時資産へそのまま複写する。
	auto Patch = TStrongObjectPtr<UStaticMesh>(NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient));
	Patch->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Source"), TEXT("Source")));
	auto Description = FMeshDescription{};
	auto Attributes = FStaticMeshAttributes(Description);
	Attributes.Register();
	auto UVs = Attributes.GetVertexInstanceUVs();
	UVs.SetNumChannels(1);
	const auto Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Source");
	auto Center = FVector::ZeroVector;
	auto LongestEdge = 0.0;
	for (const auto& Triangle : Triangles)
	{
		Center += FVector(Triangle.Position[0] + Triangle.Position[1] + Triangle.Position[2]) / (3 * Triangles.Num());
		LongestEdge = FMath::Max(LongestEdge, Triangle.LongestEdge);
		auto Instances = TArray<FVertexInstanceID>{};
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions()[Vertex] = Triangle.Position[Corner];
			const auto Instance = Description.CreateVertexInstance(Vertex);
			const auto Source = Triangle.Vertices[Corner];
			const auto X = FVector3f(LOD.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Source));
			const auto Y = FVector3f(LOD.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Source));
			Attributes.GetVertexInstanceNormals()[Instance] = Triangle.Normal[Corner];
			Attributes.GetVertexInstanceTangents()[Instance] = X;
			Attributes.GetVertexInstanceBinormalSigns()[Instance] = FVector3f::DotProduct(FVector3f::CrossProduct(X, Y), Triangle.Normal[Corner]) < 0 ? -1 : 1;
			Attributes.GetVertexInstanceColors()[Instance] = FVector4f(1, 1, 1, 1);
			UVs.Set(Instance, 0, Triangle.UV[Corner]);
			Instances.Add(Instance);
		}
		Description.CreatePolygon(Group, Instances);
	}
	auto Params = UStaticMesh::FBuildMeshDescriptionsParams{};
	Params.bMarkPackageDirty = false;
	Params.bCommitMeshDescription = false;
	Params.bFastBuild = true;
	Params.bAllowCpuAccess = true;
	auto& Precision = Params.PerLODOverrides.AddDefaulted_GetRef();
	Precision.bUseHighPrecisionTangentBasis = LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetUseHighPrecisionTangentBasis();
	Precision.bUseFullPrecisionUVs = true;
	if (Patch->BuildFromMeshDescriptions({&Description}, Params) == false)
	{
		return TEXT("Transient triangle mesh build failed.");
	}
	const auto& Built = Patch->GetRenderData()->LODResources[0].VertexBuffers;
	auto MaxPositionError = 0.0;
	auto MaxUVError = 0.0;
	auto MaxNormalError = 0.0;
	for (auto Index = 0; Index < Triangles.Num() * 3; ++Index)
	{
		const auto& Triangle = Triangles[Index / 3];
		const auto Corner = Index % 3;
		MaxPositionError = FMath::Max(MaxPositionError, double((Built.PositionVertexBuffer.VertexPosition(Index) - Triangle.Position[Corner]).Length()));
		MaxUVError = FMath::Max(MaxUVError, double((Built.StaticMeshVertexBuffer.GetVertexUV(Index, 0) - Triangle.UV[Corner]).Length()));
		MaxNormalError = FMath::Max(MaxNormalError, double((FVector3f(Built.StaticMeshVertexBuffer.VertexTangentZ(Index)) - Triangle.Normal[Corner]).Length()));
	}
	const auto Distance = FMath::Max(2.0, LongestEdge * CameraDistanceScale);
	const auto Raw = MeshParticleizerCoverageValidation::CompareRender(Material, Patch.Get(), ViewDirection, Center, OutputPng, Resolution, Distance);
	auto Report = TSharedPtr<FJsonObject>{};
	if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Report) == false || Report.IsValid() == false)
	{
		return Raw;
	}
	Report->SetObjectField(TEXT("sourceTriangle"), DescribeTriangle(Triangles[0]));
	auto PatchTriangles = TArray<TSharedPtr<FJsonValue>>{};
	for (const auto& Triangle : Triangles)
	{
		PatchTriangles.Add(MakeShared<FJsonValueObject>(DescribeTriangle(Triangle)));
	}
	Report->SetArrayField(TEXT("patchTriangles"), PatchTriangles);
	Report->SetStringField(TEXT("sourceMesh"), Mesh->GetPathName());
	Report->SetNumberField(TEXT("sourceSlot"), Slot);
	Report->SetNumberField(TEXT("copyPositionErrorCm"), MaxPositionError);
	Report->SetNumberField(TEXT("copyUVError"), MaxUVError);
	Report->SetNumberField(TEXT("copyNormalError"), MaxNormalError);
	Report->SetStringField(TEXT("scope"), TEXT("Original LOD0 reference-pose patch; source positions and attributes copied to an independent UE Static Mesh capture; success requires no interior overlap; no full-mesh occlusion or animation acceptance"));
	return MeshParticleizerCoverageTopology::Serialize(Report);
}
