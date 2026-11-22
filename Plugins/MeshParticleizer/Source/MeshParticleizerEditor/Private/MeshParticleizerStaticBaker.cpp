#include "MeshParticleizerSurfaceBaker.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"

bool MeshParticleizerPreparation::BuildStaticSource(UStaticMesh* Mesh, FMeshDescription& OutDescription,
	FSurfaceSource& OutSource, FString& OutError, int32 SamplingLOD)
{
	const auto* Render = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
	if (Render == nullptr || Render->LODResources.IsValidIndex(SamplingLOD) == false)
	{
		OutError = TEXT("Select an available built Static Mesh sampling LOD.");
		return false;
	}
	const auto& LOD = Render->LODResources[SamplingLOD];
	const auto& Buffers = LOD.VertexBuffers;
	const auto Indices = LOD.IndexBuffer.GetArrayView();
	const auto VertexCount = static_cast<uint32>(LOD.GetNumVertices());
	if (VertexCount == 0 || Indices.Num() == 0 || Buffers.StaticMeshVertexBuffer.GetNumTexCoords() == 0)
	{
		OutError = TEXT("The sampling LOD requires built vertices, triangles and UV0.");
		return false;
	}

	// 自動簡略化LODも、描画用に確定した形状・材質割当から同じ経路で準備する。
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
	OutSource.SamplingLOD = SamplingLOD;
	OutSource.Fingerprint = UMeshParticleizerPreparedMesh::MakeSourceFingerprint(Render->DerivedDataKey, SamplingLOD);
	OutSource.Bounds = Mesh->GetBoundingBox();
	auto Groups = TArray<FPolygonGroupID>{};
	for (auto Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
	{
		const auto Group = OutDescription.CreatePolygonGroup();
		const auto Name = FName(FString::Printf(TEXT("MP_Slot_%d"), Slot));
		SlotNames[Group] = Name;
		Groups.Add(Group);
		OutSource.MaterialSlotNames.Add(Name);
		OutSource.Materials.Add(Mesh->GetStaticMaterials()[Slot].MaterialInterface);
	}
	for (uint32 Index = 0; Index < VertexCount; ++Index)
	{
		Positions[OutDescription.CreateVertex()] = Buffers.PositionVertexBuffer.VertexPosition(Index);
	}

	for (auto SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
	{
		const auto& Section = LOD.Sections[SectionIndex];
		if (Section.NumTriangles == 0)
		{
			continue;
		}
		if (Groups.IsValidIndex(Section.MaterialIndex) == false ||
			static_cast<uint64>(Section.FirstIndex) + static_cast<uint64>(Section.NumTriangles) * 3 > static_cast<uint64>(Indices.Num()))
		{
			OutError = TEXT("The sampling LOD has an invalid section or material slot.");
			return false;
		}
		OutSource.EnabledSections.Add(SectionIndex);
		for (uint32 Offset = Section.FirstIndex; Offset < Section.FirstIndex + Section.NumTriangles * 3; Offset += 3)
		{
			auto Instances = TArray<FVertexInstanceID, TInlineAllocator<3>>{};
			for (auto Corner = 0; Corner < 3; ++Corner)
			{
				const auto Index = Indices[Offset + Corner];
				if (Index >= VertexCount)
				{
					OutError = TEXT("The sampling LOD has an invalid vertex index.");
					return false;
				}
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
			OutDescription.CreateTriangle(Groups[Section.MaterialIndex], Instances);
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
