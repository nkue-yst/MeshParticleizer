#include "MeshParticleizerSurfaceBaker.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Engine/SkeletalMesh.h"
#include "MeshDescription.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshAttributes.h"

bool MeshParticleizerPreparation::BuildSkeletalSource(USkeletalMesh* Mesh, FMeshDescription& OutDescription,
	FSurfaceSource& OutSource, FString& OutError, int32 SamplingLOD)
{
	const auto* RenderData = Mesh != nullptr ? Mesh->GetResourceForRendering() : nullptr;
	if (RenderData == nullptr || RenderData->LODRenderData.IsValidIndex(SamplingLOD) == false)
	{
		OutError = TEXT("Select an available built Skeletal Mesh sampling LOD.");
		return false;
	}
	const auto& LOD = RenderData->LODRenderData[SamplingLOD];
	const auto& Buffers = LOD.StaticVertexBuffers;
	const auto& Skin = LOD.SkinWeightVertexBuffer;
	const auto VertexCount = LOD.GetNumVertices();
	const auto InfluenceCount = Skin.GetMaxBoneInfluences();
	const auto BoneCount = Mesh->GetRefSkeleton().GetRawBoneNum();
	auto Indices = TArray<uint32>{};
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	if (VertexCount == 0 || BoneCount == 0 || Indices.IsEmpty() || Skin.GetNumVertices() != VertexCount ||
		InfluenceCount > UMeshParticleizerPreparedMesh::MaxSkinInfluences || Buffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		OutError = TEXT("The sampling LOD requires UVs, skin weights and at most 12 bone influences per vertex.");
		return false;
	}

	// 描画用の頂点番号を保持する。結合や再ビルドで色・ウェイトの対応を変えない。
	OutDescription.Empty();
	auto Attributes = FStaticMeshAttributes(OutDescription);
	Attributes.Register();
	auto Positions = Attributes.GetVertexPositions();
	auto Normals = Attributes.GetVertexInstanceNormals();
	auto Tangents = Attributes.GetVertexInstanceTangents();
	auto Signs = Attributes.GetVertexInstanceBinormalSigns();
	auto Colors = Attributes.GetVertexInstanceColors();
	auto UVs = Attributes.GetVertexInstanceUVs();
	auto SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	UVs.SetNumChannels(Buffers.StaticMeshVertexBuffer.GetNumTexCoords());
	OutSource = FSurfaceSource{};
	OutSource.Asset = Mesh;
	OutSource.Description = &OutDescription;
	OutSource.Fingerprint = UMeshParticleizerPreparedMesh::MakeSourceFingerprint(RenderData->DerivedDataKey, SamplingLOD);
	OutSource.SamplingLOD = SamplingLOD;
	OutSource.Bounds = FBox(ForceInit);
	OutSource.BoneCount = BoneCount;
	OutSource.SkinVertices.SetNumZeroed(FMath::DivideAndRoundUp(static_cast<int32>(VertexCount) * UMeshParticleizerPreparedMesh::SkinVertexStride, 512) * 512);
	OutSource.SkinTriangles.SetNumZeroed(FMath::DivideAndRoundUp(Indices.Num() / 3, 512) * 512);
	auto Groups = TArray<FPolygonGroupID>{};
	for (auto Slot = 0; Slot < Mesh->GetMaterials().Num(); ++Slot)
	{
		auto Group = OutDescription.CreatePolygonGroup();
		const auto Name = FName(FString::Printf(TEXT("MP_Slot_%d"), Slot));
		SlotNames[Group] = Name;
		Groups.Add(Group);
		OutSource.MaterialSlotNames.Add(Name);
		OutSource.Materials.Add(Mesh->GetMaterials()[Slot].MaterialInterface);
	}
	for (uint32 Index = 0; Index < VertexCount; ++Index)
	{
		const auto Vertex = OutDescription.CreateVertex();
		Positions[Vertex] = Buffers.PositionVertexBuffer.VertexPosition(Index);
		OutSource.Bounds += FVector(Positions[Vertex]);
	}

	const auto* LODInfo = Mesh->GetLODInfo(SamplingLOD);
	for (auto SectionIndex = 0; SectionIndex < LOD.RenderSections.Num(); ++SectionIndex)
	{
		const auto& Section = LOD.RenderSections[SectionIndex];
		if (Section.bDisabled == true || Section.NumTriangles == 0)
		{
			continue;
		}
		const auto MaterialIndex = LODInfo != nullptr && LODInfo->LODMaterialMap.IsValidIndex(SectionIndex) && LODInfo->LODMaterialMap[SectionIndex] != INDEX_NONE ? LODInfo->LODMaterialMap[SectionIndex] : Section.MaterialIndex;
		if (Groups.IsValidIndex(MaterialIndex) == false || Section.HasClothingData() == true)
		{
			OutError = TEXT("The sampling LOD has an invalid material slot or simulated cloth. The bone skinning path does not include cloth deformation.");
			return false;
		}

		OutSource.EnabledSections.Add(SectionIndex);

		// セクション内の骨番号を、元Skeleton全体の番号へ一度だけ変換して保存する。
		for (uint32 VertexIndex = Section.BaseVertexIndex; VertexIndex < Section.BaseVertexIndex + Section.NumVertices; ++VertexIndex)
		{
			const auto Offset = VertexIndex * UMeshParticleizerPreparedMesh::SkinVertexStride;
			const auto Position = Buffers.PositionVertexBuffer.VertexPosition(VertexIndex);
			const auto Normal = FVector3f(Buffers.StaticMeshVertexBuffer.VertexTangentZ(VertexIndex));
			OutSource.SkinVertices[Offset] = FLinearColor(Position.X, Position.Y, Position.Z, 1);
			OutSource.SkinVertices[Offset + 1] = FLinearColor(Normal.X, Normal.Y, Normal.Z, 0);
			auto WeightSum = 0.0f;
			for (uint32 Influence = 0; Influence < InfluenceCount; ++Influence)
			{
				const auto Weight = Skin.GetBoneWeight(VertexIndex, Influence) / 65535.0f;
				if (Weight == 0)
				{
					continue;
				}
				const auto LocalBone = Skin.GetBoneIndex(VertexIndex, Influence);
				if (Section.BoneMap.IsValidIndex(LocalBone) == false || Section.BoneMap[LocalBone] >= BoneCount)
				{
					OutError = TEXT("A skin influence does not match the skeleton bone map.");
					return false;
				}
				OutSource.SkinVertices[Offset + 2 + (Influence / 4) * 2].Component(Influence % 4) = Section.BoneMap[LocalBone];
				OutSource.SkinVertices[Offset + 3 + (Influence / 4) * 2].Component(Influence % 4) = Weight;
				WeightSum += Weight;
			}
			if (FMath::IsNearlyEqual(WeightSum, 1.0f, 0.01f) == false)
			{
				OutError = TEXT("A vertex has invalid total skin weight.");
				return false;
			}
		}

		for (uint32 Offset = Section.BaseIndex; Offset < Section.BaseIndex + Section.NumTriangles * 3; Offset += 3)
		{
			auto Instances = TArray<FVertexInstanceID, TInlineAllocator<3>>{};
			for (auto Corner = 0; Corner < 3; ++Corner)
			{
				const auto Index = Indices[Offset + Corner];
				const auto Instance = OutDescription.CreateVertexInstance(FVertexID(Index));
				Instances.Add(Instance);
				const auto X = FVector3f(Buffers.StaticMeshVertexBuffer.VertexTangentX(Index));
				const auto Y = FVector3f(Buffers.StaticMeshVertexBuffer.VertexTangentY(Index));
				const auto Z = FVector3f(Buffers.StaticMeshVertexBuffer.VertexTangentZ(Index));
				Normals[Instance] = Z;
				Tangents[Instance] = X;
				Signs[Instance] = FVector3f::DotProduct(FVector3f::CrossProduct(X, Y), Z) < 0 ? -1 : 1;
				const auto Color = Buffers.ColorVertexBuffer.GetNumVertices() == VertexCount ? Buffers.ColorVertexBuffer.VertexColor(Index).ReinterpretAsLinear() : FLinearColor::White;
				Colors[Instance] = FVector4f(Color.R, Color.G, Color.B, Color.A);
				for (auto Channel = 0; Channel < UVs.GetNumChannels(); ++Channel)
				{
					UVs.Set(Instance, Channel, Buffers.StaticMeshVertexBuffer.GetVertexUV(Index, Channel));
				}
			}
			const auto Triangle = OutDescription.CreateTriangle(Groups[MaterialIndex], Instances);
			OutSource.SkinTriangles[Triangle.GetValue()] = FLinearColor(Indices[Offset], Indices[Offset + 1], Indices[Offset + 2], MaterialIndex);
		}
	}
	if (OutDescription.Triangles().Num() == 0)
	{
		OutError = TEXT("The sampling LOD has no enabled triangles.");
		return false;
	}
	OutError.Reset();
	return true;
}
