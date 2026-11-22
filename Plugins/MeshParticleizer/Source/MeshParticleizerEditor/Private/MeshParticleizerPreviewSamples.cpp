#include "MeshParticleizerPreview.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SceneManagement.h"
#include "SceneView.h"

namespace
{
	bool ReadSamples(const TSoftObjectPtr<UTexture2D>& Reference, int32 MinimumCount, TArray64<uint8>& Bytes)
	{
		auto* Texture = Reference.LoadSynchronous();
		return Texture != nullptr && Texture->Source.GetFormat() == TSF_RGBA32F &&
			Texture->Source.GetMipData(Bytes, 0) == true &&
			Bytes.Num() / static_cast<int64>(sizeof(FLinearColor)) >= MinimumCount;
	}
}

void UMeshParticleizerPreview::ClearSamples()
{
	this->PreparedSamples.Reset();
	this->SampleSlotCounts.Reset();
	this->SamplePoints.Reset();
	this->SampleBoneMatrices.Reset();
	this->SampleMesh.Reset();
	this->bSamplesDirty = true;
}

bool UMeshParticleizerPreview::PrepareSamples(FString& OutError)
{
	if (this->Prepared == nullptr || this->Prepared->SampleCount < 1 || this->Prepared->SampleCount > 250000)
	{
		OutError = TEXT("Select current prepared mesh data before viewing samples.");
		return false;
	}
	if (this->PreparedSamples.Num() == this->Prepared->SampleCount)
	{
		return true;
	}

	// 準備時の対応を一度だけ読む。描画フレームでGPU読戻しやTexture展開を行わない。
	TArray64<uint8> Positions, Normals, Coordinates, Triangles;
	const auto bSkinned = this->Prepared->SkeletalMesh.IsNull() == false;
	if (ReadSamples(this->Prepared->Positions, this->Prepared->SampleCount, Positions) == false ||
		(bSkinned == true && (ReadSamples(this->Prepared->Normals, this->Prepared->SampleCount, Normals) == false || ReadSamples(this->Prepared->Coordinates, this->Prepared->SampleCount, Coordinates) == false || ReadSamples(this->Prepared->SkinTriangles, 1, Triangles) == false)))
	{
		OutError = TEXT("Prepared sample source pixels are unavailable. Prepare this mesh again in the editor.");
		return false;
	}
	const auto* PositionData = reinterpret_cast<const FLinearColor*>(Positions.GetData());
	const auto* NormalData = reinterpret_cast<const FLinearColor*>(Normals.GetData());
	const auto* CoordinateData = reinterpret_cast<const FLinearColor*>(Coordinates.GetData());
	const auto* TriangleData = reinterpret_cast<const FLinearColor*>(Triangles.GetData());
	const auto TriangleCount = Triangles.Num() / static_cast<int64>(sizeof(FLinearColor));
	const auto* Skinned = Cast<USkinnedMeshComponent>(this->Mesh);
	const auto* Asset = Skinned != nullptr ? Cast<USkeletalMesh>(Skinned->GetSkinnedAsset()) : nullptr;
	const auto* RenderData = Asset != nullptr ? Asset->GetResourceForRendering() : nullptr;
	if (bSkinned == true && (RenderData == nullptr || RenderData->LODRenderData.IsValidIndex(this->Prepared->SamplingLOD) == false))
	{
		OutError = TEXT("The prepared skeletal mesh has no sampling LOD render data.");
		return false;
	}

	// 壊れた入力で途中までの点を採用しない。全候補のslotと三角形対応を検証する。
	auto Samples = TArray<FPreparedSample>{};
	auto Counts = TArray<int32>{};
	Samples.Reserve(this->Prepared->SampleCount);
	Counts.SetNumZeroed(this->Prepared->SourceMaterials.Num());
	for (auto Index = 0; Index < this->Prepared->SampleCount; ++Index)
	{
		const auto P = PositionData[Index];
		auto Sample = FPreparedSample{};
		Sample.Position = FVector3f(P.R, P.G, P.B);
		if (Sample.Position.ContainsNaN() == true || FMath::IsFinite(P.A) == false ||
			P.A < 0 || P.A >= Counts.Num() || P.A != FMath::FloorToFloat(P.A))
		{
			OutError = TEXT("Prepared sample position or material slot is invalid.");
			return false;
		}
		Sample.MaterialSlot = static_cast<int32>(P.A);
		if (bSkinned == true)
		{
			const auto TriangleId = NormalData[Index].A;
			const auto UV = CoordinateData[Index];
			Sample.Barycentrics = FVector3f(UV.B, UV.A, 1 - UV.B - UV.A);
			if (FMath::IsFinite(TriangleId) == false || TriangleId < 0 || TriangleId >= TriangleCount ||
				TriangleId != FMath::FloorToFloat(TriangleId) || Sample.Barycentrics.ContainsNaN() == true ||
				Sample.Barycentrics.GetMin() < -0.0001f || Sample.Barycentrics.GetMax() > 1.0001f)
			{
				OutError = TEXT("Prepared sample triangle or barycentric coordinates are invalid.");
				return false;
			}
			const auto Triangle = TriangleData[static_cast<int32>(TriangleId)];
			for (auto Corner = 0; Corner < 3; ++Corner)
			{
				const auto Vertex = Triangle.Component(Corner);
				if (FMath::IsFinite(Vertex) == false || Vertex < 0 || Vertex >= RenderData->LODRenderData[this->Prepared->SamplingLOD].GetNumVertices() ||
					Vertex != FMath::FloorToFloat(Vertex))
				{
					OutError = TEXT("Prepared sample references an unavailable sampling LOD vertex.");
					return false;
				}
				Sample.Vertices[Corner] = static_cast<int32>(Vertex);
			}
		}
		++Counts[Sample.MaterialSlot];
		Samples.Add(Sample);
	}
	this->PreparedSamples = MoveTemp(Samples);
	this->SampleSlotCounts = MoveTemp(Counts);
	this->bSamplesDirty = true;
	OutError.Reset();
	return true;
}

bool UMeshParticleizerPreview::SetSampleDisplay(int32 MaterialSlot, int32 Stride, FString& OutError)
{
	if (this->bClosed == true || MaterialSlot < -1 || Stride < 1 || Stride > 4096 ||
		(MaterialSlot >= 0 && (this->Prepared == nullptr || this->Prepared->SourceMaterials.IsValidIndex(MaterialSlot) == false)))
	{
		OutError = TEXT("Choose a prepared material slot or all slots, and a sample stride from 1 to 4096.");
		return false;
	}
	this->SampleMaterialSlot = MaterialSlot;
	this->SampleDisplayStride = Stride;
	this->bSamplesDirty = true;
	return this->UpdateSamples(OutError);
}

int32 UMeshParticleizerPreview::GetPreparedSampleCount(int32 MaterialSlot) const
{
	if (MaterialSlot == INDEX_NONE)
	{
		return this->PreparedSamples.Num();
	}
	return this->SampleSlotCounts.IsValidIndex(MaterialSlot) ? this->SampleSlotCounts[MaterialSlot] : 0;
}

bool UMeshParticleizerPreview::UpdateSamples(FString& OutError)
{
	if (this->DisplayMode != EMeshParticleizerPreviewDisplay::Samples || this->bClosed == true)
	{
		this->SamplePoints.Reset();
		this->SampleBoneMatrices.Reset();
		this->SampleMesh.Reset();
		this->bSamplesDirty = true;
		return true;
	}
	if (this->PrepareSamples(OutError) == false)
	{
		return false;
	}
	auto* Display = this->Operation != nullptr ? this->Operation->GetDisplayMeshForEditor() : nullptr;
	if (Display == nullptr)
	{
		Display = this->Mesh;
	}
	if (Display == nullptr)
	{
		OutError = TEXT("The sample display mesh is unavailable.");
		return false;
	}

	// 固定姿勢では描画用PoseableMeshを参照する。停止中は同じ座標を再計算しない。
	auto* Skinned = Cast<USkinnedMeshComponent>(Display);
	auto Matrices = TArray<FMatrix44f>{};
	if (Skinned != nullptr)
	{
		Skinned->GetCurrentRefToLocalMatrices(Matrices, this->Prepared->SamplingLOD);
	}
	const auto Transform = Display->GetComponentTransform();
	if (this->bSamplesDirty == false && this->SampleMesh == Display && this->SampleTransform.Equals(Transform) == true &&
		this->SampleBoneMatrices == Matrices)
	{
		return true;
	}

	auto Vertices = TArray<FVector3f>{};
	if (Skinned != nullptr)
	{
		const auto* Asset = Cast<USkeletalMesh>(Skinned->GetSkinnedAsset());
		const auto* Data = Asset != nullptr ? Asset->GetResourceForRendering() : nullptr;
		auto* Weights = Skinned->GetSkinWeightBuffer(this->Prepared->SamplingLOD);
		if (Data == nullptr || Data->LODRenderData.IsValidIndex(this->Prepared->SamplingLOD) == false || Weights == nullptr || Matrices.Num() != this->Prepared->BoneCount)
		{
			OutError = TEXT("sampling LOD skinning is unavailable for the sample display.");
			return false;
		}
		USkinnedMeshComponent::ComputeSkinnedPositions(Skinned, Vertices, Matrices, Data->LODRenderData[this->Prepared->SamplingLOD], *Weights);
		if (Vertices.Num() != static_cast<int32>(Data->LODRenderData[this->Prepared->SamplingLOD].GetNumVertices()))
		{
			OutError = TEXT("Could not evaluate the sample display pose.");
			return false;
		}
	}

	this->SamplePoints.Reset();
	for (auto Index = 0; Index < this->PreparedSamples.Num(); Index += this->SampleDisplayStride)
	{
		const auto& Sample = this->PreparedSamples[Index];
		if (this->SampleMaterialSlot != INDEX_NONE && this->SampleMaterialSlot != Sample.MaterialSlot)
		{
			continue;
		}
		auto Position = Sample.Position;
		if (Skinned != nullptr)
		{
			Position = Vertices[Sample.Vertices.X] * Sample.Barycentrics.X + Vertices[Sample.Vertices.Y] * Sample.Barycentrics.Y +
				Vertices[Sample.Vertices.Z] * Sample.Barycentrics.Z;
		}
		auto& Point = this->SamplePoints.AddDefaulted_GetRef();
		Point.SampleId = Index;
		Point.MaterialSlot = Sample.MaterialSlot;
		Point.WorldPosition = Transform.TransformPosition(FVector(Position));
	}
	this->SampleMesh = Display;
	this->SampleTransform = Transform;
	this->SampleBoneMatrices = MoveTemp(Matrices);
	this->bSamplesDirty = false;
	OutError.Reset();
	return true;
}

FLinearColor UMeshParticleizerPreview::GetSampleSlotColor(int32 MaterialSlot)
{
	return FLinearColor::MakeFromHSV8(static_cast<uint8>((MaterialSlot * 97 + 24) % 256), 180, 255);
}

void UMeshParticleizerPreview::DrawSamples(const FSceneView* View, FPrimitiveDrawInterface* PDI) const
{
	if (this->DisplayMode != EMeshParticleizerPreviewDisplay::Samples || View == nullptr || PDI == nullptr)
	{
		return;
	}
	for (const auto& Point : this->SamplePoints)
	{
		// 表面との深度競合だけを避け、裏側の候補は通常の深度判定で隠す。
		const auto Bias = (View->ViewMatrices.GetViewOrigin() - Point.WorldPosition).GetSafeNormal() * 0.05;
		PDI->DrawPoint(Point.WorldPosition + Bias, GetSampleSlotColor(Point.MaterialSlot), 3, SDPG_World);
	}
}
