#include "MeshParticleizerOriginLibrary.h"
#include "MeshParticleizerReference.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkeletalMesh.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerProfiling.h"
#include "MeshParticleizerPointField.h"
#include "Materials/Material.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace MeshParticleizerOrigin
{
	UObject* GetMeshAsset(const UMeshComponent* Mesh)
	{
		if (const auto* Static = Cast<UStaticMeshComponent>(Mesh))
		{
			return Static->GetStaticMesh().Get();
		}
		if (const auto* Skinned = Cast<USkinnedMeshComponent>(Mesh))
		{
			return Skinned->GetSkinnedAsset();
		}
		return nullptr;
	}

	bool HasBonePose(const USkinnedMeshComponent* Mesh, int32 BoneIndex)
	{
		if (Mesh->IsRegistered() == false || BoneIndex == INDEX_NONE)
		{
			return false;
		}
		const auto* Leader = Mesh->LeaderPoseComponent.Get();
		if (Leader == nullptr)
		{
			return Mesh->GetComponentSpaceTransforms().IsValidIndex(BoneIndex);
		}
		if (Leader->IsRegistered() == false)
		{
			return false;
		}

		// 未共有の骨はUEが共通祖先から補う。祖先の姿勢もなければ近似成功と扱わない。
		const auto& Map = Mesh->GetLeaderBoneMap();
		const auto& Skeleton = Mesh->GetSkinnedAsset()->GetRefSkeleton();
		for (auto Bone = BoneIndex; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
		{
			if (Map.IsValidIndex(Bone) == true && Leader->GetComponentSpaceTransforms().IsValidIndex(Map[Bone]) == true)
			{
				return true;
			}
		}
		return false;
	}

	bool SetBoneOrigin(UMeshComponent* Mesh, FName Name, const FMeshParticleizerProgressSpace& Space,
		FMeshParticleizerResolvedOrigin& Result)
	{
		auto Reference = FMatrix::Identity;
		if (MeshParticleizerReference::GetSocketTransform(Mesh, Name, Reference) == false)
		{
			return false;
		}
		Result.Mode = EMeshParticleizerOrigin::Point;
		Result.Accuracy = EMeshParticleizerOriginAccuracy::BoneOrSocket;
		Result.ReferencePosition = FVector(Space.SourceToGroup.TransformPosition(Reference.GetOrigin()));
		Result.OriginMesh = Mesh;
		Result.BoneOrSocket = Name;
		return true;
	}

	void UseFallback(UMeshComponent* Mesh, const FMeshParticleizeOptions& Options,
		const FMeshParticleizerProgressSpace& Space, FMeshParticleizerResolvedOrigin& Result)
	{
		Result.bUsedFallback = true;
		if (SetBoneOrigin(Mesh, Options.FallbackBone, Space, Result) == true)
		{
			Result.Diagnostics += FString::Printf(TEXT(" Using fallback bone/socket '%s'."), *Options.FallbackBone.ToString());
			return;
		}
		Result.Mode = EMeshParticleizerOrigin::CenterOut;
		Result.Accuracy = EMeshParticleizerOriginAccuracy::ReferenceSpace;
		Result.ReferencePosition = Space.Bounds.GetCenter();
		Result.OriginMesh = Mesh;
		Result.BoneOrSocket = NAME_None;
		Result.Diagnostics += TEXT(" Using the reference bounds center.");
	}
}

FMeshParticleizerHitOrigin UMeshParticleizerOriginLibrary::CaptureHitOrigin(const FHitResult& Hit)
{
	auto Result = FMeshParticleizerHitOrigin{};
	auto* Mesh = Cast<UMeshComponent>(Hit.GetComponent());
	// Component直接のTraceはBlockingフラグを立てない。成功した命中の参照と座標を検証する。
	if (Hit.bStartPenetrating == true || IsValid(Mesh) == false || Hit.ImpactPoint.ContainsNaN() == true)
	{
		Result.Diagnostics = TEXT("A successful hit with a finite impact point on the source mesh is required; initial overlaps have no surface point.");
		return Result;
	}
	Result.SourceMesh = Mesh;
	Result.SourceAsset = MeshParticleizerOrigin::GetMeshAsset(Mesh);
	if (Result.SourceAsset.IsNull() == true)
	{
		Result.Diagnostics = TEXT("Hit origin requires a Static or Skeletal Mesh asset.");
		return Result;
	}

	auto WorldToReference = FMatrix::Identity;
	if (const auto* Skinned = Cast<USkinnedMeshComponent>(Mesh))
	{
		const auto BoneIndex = Skinned->GetBoneIndex(Hit.BoneName);
		const auto& InverseBind = Skinned->GetSkinnedAsset()->GetRefBasesInvMatrix();
		if (InverseBind.IsValidIndex(BoneIndex) == false || MeshParticleizerOrigin::HasBonePose(Skinned, BoneIndex) == false)
		{
			Result.Diagnostics = TEXT("Hit bone or its current pose is unavailable for reference-space mapping.");
			return Result;
		}
		const auto BoneWorld = Skinned->GetBoneTransform(BoneIndex).ToMatrixWithScale();
		const auto ReferenceInverse = FMatrix(InverseBind[BoneIndex]);
		if (MeshParticleizerReference::IsInvertible(BoneWorld) == false || MeshParticleizerReference::IsInvertible(ReferenceInverse) == false)
		{
			Result.Diagnostics = TEXT("Hit bone transform cannot be inverted.");
			return Result;
		}
		WorldToReference = BoneWorld.Inverse() * ReferenceInverse.Inverse();
		Result.BoneName = Hit.BoneName;
		Result.Accuracy = EMeshParticleizerOriginAccuracy::BoneApproximation;
		Result.Diagnostics = TEXT("Hit origin uses a bone-local approximation, not an exact surface projection.");
	}
	else
	{
		const auto Transform = Mesh->GetComponentTransform().ToMatrixWithScale();
		if (MeshParticleizerReference::IsInvertible(Transform) == false)
		{
			Result.Diagnostics = TEXT("Hit mesh transform cannot be inverted.");
			return Result;
		}
		WorldToReference = Transform.Inverse();
		Result.Accuracy = EMeshParticleizerOriginAccuracy::StaticHit;
	}
	Result.ReferencePosition = FVector(WorldToReference.TransformPosition(Hit.ImpactPoint));
	if (Result.ReferencePosition.ContainsNaN() == true)
	{
		Result.Accuracy = EMeshParticleizerOriginAccuracy::Unavailable;
		Result.Diagnostics = TEXT("Hit origin produced an invalid reference position.");
	}
	return Result;
}

FMeshParticleizerHitOrigin UMeshParticleizerOriginLibrary::CaptureSurfaceHitOrigin(const FHitResult& Hit,
	const UMeshParticleizerPreparedMesh* Prepared, double MaxProjectionDistance)
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, ProjectHit);

	auto Result = FMeshParticleizerHitOrigin{};
	if (IsInGameThread() == false || Hit.bStartPenetrating == true || Hit.ImpactPoint.ContainsNaN() == true ||
		FMath::IsFinite(MaxProjectionDistance) == false || MaxProjectionDistance < 0)
	{
		Result.Diagnostics = TEXT("Surface projection requires a finite hit, a nonnegative distance and the game thread after pose update.");
		return Result;
	}
	auto* Mesh = Cast<USkinnedMeshComponent>(Hit.GetComponent());
	if (Mesh == nullptr)
	{
		return CaptureHitOrigin(Hit);
	}
	Result.SourceMesh = Mesh;
	Result.SourceAsset = Mesh->GetSkinnedAsset();
	Result.BoneName = Hit.BoneName;
	auto* Asset = Cast<USkeletalMesh>(Mesh->GetSkinnedAsset());
	const auto* Render = Asset != nullptr ? Asset->GetResourceForRendering() : nullptr;
	if (IsValid(Prepared) == false || Asset == nullptr || Prepared->SkeletalMesh.ToSoftObjectPath() != FSoftObjectPath(Asset) ||
		Prepared->DataVersion != UMeshParticleizerPreparedMesh::CurrentDataVersion || Prepared->SurfaceProjection.IsReady() == false ||
		Prepared->SurfaceProjection.GetBoneCount() != Prepared->BoneCount || Render == nullptr || Render->LODRenderData.IsValidIndex(Prepared->SamplingLOD) == false)
	{
		Result.Diagnostics = TEXT("Surface projection data is missing or does not match this mesh. Prepare surface projection data before use.");
		return Result;
	}
#if WITH_EDITOR
	if (Prepared->SourceFingerprint != UMeshParticleizerPreparedMesh::MakeSourceFingerprint(Render->DerivedDataKey, Prepared->SamplingLOD))
	{
		Result.Diagnostics = TEXT("Source mesh changed after surface projection preparation. Prepare this mesh again.");
		return Result;
	}
#endif

	// 対応しない変形を無視して、精密な描画面を取得したことにしない。
	const auto* Leader = Mesh->LeaderPoseComponent.Get();
	if (Mesh->IsRegistered() == false || Mesh->GetPredictedLODLevel() != 0 ||
		(Leader != nullptr && (Leader->IsRegistered() == false || Leader->GetPredictedLODLevel() != 0)))
	{
		Result.Diagnostics = TEXT("Surface projection requires a registered LOD0 source and leader after pose update.");
		return Result;
	}
	if (Mesh->GetMeshDeformerInstanceForLOD(0) != nullptr ||
		(Mesh->ExternalMorphSets.IsValidIndex(0) == true && Mesh->ExternalMorphSets[0].IsEmpty() == false) ||
		Mesh->MorphTargetWeights.ContainsByPredicate([](float Weight) {
			return Weight != 0;
		}))
	{
		Result.Diagnostics = TEXT("Surface projection requires LOD0 with the prepared skin weights and standard bone deformation; morphs, deformer and skin weight overrides are unsupported.");
		return Result;
	}
	// 描画LODの変形も確認する。低いSampling LODだけでは描画側の差を見落とす。
	const auto LODs = Prepared->SamplingLOD == 0 ? TArray<int32>{0} : TArray<int32>{0, Prepared->SamplingLOD};
	for (const auto LOD : LODs)
	{
		const auto& LODData = Render->LODRenderData[LOD];
		if (Mesh->GetSkinWeightBuffer(LOD) != &LODData.SkinWeightVertexBuffer)
		{
			Result.Diagnostics = FString::Printf(TEXT("Surface projection does not include skin weight overrides on LOD %d."), LOD);
			return Result;
		}
		for (const auto& Section : LODData.RenderSections)
		{
			if (Section.HasClothingData() == true)
			{
				Result.Diagnostics = FString::Printf(TEXT("Surface projection does not include cloth deformation on LOD %d."), LOD);
				return Result;
			}
		}
	}
	for (auto Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
	{
		const auto* Interface = Mesh->GetMaterial(Slot);
		const auto* Material = Interface != nullptr ? Interface->GetMaterial() : nullptr;
		if (Material == nullptr || Material->HasVertexPositionOffsetConnected() == true || Material->HasDisplacementConnected() == true)
		{
			Result.Diagnostics = TEXT("Surface projection requires materials without world-position offset or displacement.");
			return Result;
		}
	}
	for (const auto Bone : Render->LODRenderData[Prepared->SamplingLOD].ActiveBoneIndices)
	{
		if (MeshParticleizerOrigin::HasBonePose(Mesh, Bone) == false)
		{
			Result.Diagnostics = TEXT("The current LOD0 bone pose is unavailable for surface projection.");
			return Result;
		}
	}

	auto Matrices = TArray<FMatrix44f>{};
	Mesh->GetCurrentRefToLocalMatrices(Matrices, Prepared->SamplingLOD);
	auto Projection = FMeshParticleizerProjectionResult{};
	if (Prepared->SurfaceProjection.Project(Matrices, Mesh->GetComponentTransform().ToMatrixWithScale(),
		Hit.ImpactPoint, MaxProjectionDistance, Projection) == false)
	{
		Result.Diagnostics = TEXT("No valid skinned surface was found within the projection distance. The configured fallback origin will be used.");
		return Result;
	}

	Result.Accuracy = EMeshParticleizerOriginAccuracy::SurfaceProjection;
	Result.ReferencePosition = Projection.ReferencePosition;
	Result.TriangleIndex = Projection.Triangle;
	Result.Barycentrics = Projection.Barycentrics;
	Result.ProjectedWorldPosition = Projection.WorldPosition;
	Result.ProjectionDistance = FVector::Distance(Hit.ImpactPoint, Projection.WorldPosition);
	Result.Diagnostics = FString::Printf(TEXT("Projected onto sampling LOD %d with the current complete bone pose and captured its reference-space barycentric position."), Prepared->SamplingLOD);
	return Result;
}

bool UMeshParticleizerOriginLibrary::SetPointOrigins(FMeshParticleizeOptions& Options, const TArray<FVector>& Points, FString& OutError)
{
	auto Candidate = Options;
	Candidate.Origin = EMeshParticleizerOrigin::MultiplePoints;
	Candidate.LocalOrigins = Points;
	if (Candidate.Validate(OutError) == false)
	{
		return false;
	}
	Options = MoveTemp(Candidate);
	return true;
}

bool FMeshParticleizerResolvedOrigin::Resolve(TConstArrayView<FMeshParticleizerTarget> Targets,
	const FMeshParticleizeOptions& Options, TConstArrayView<FMeshParticleizerProgressSpace> Spaces,
	TArray<FMeshParticleizerResolvedOrigin>& OutOrigins, FString& OutError)
{
	OutOrigins.Reset();
	OutError.Reset();
	if (Targets.IsEmpty() == true || Targets.Num() != Spaces.Num() || Options.Validate(OutError) == false)
	{
		if (OutError.IsEmpty() == true)
		{
			OutError = TEXT("Origin resolution requires matching registered sources and progress spaces.");
		}
		return false;
	}
	auto Origins = TArray<FMeshParticleizerResolvedOrigin>{};
	for (auto Index = 0; Index < Targets.Num(); ++Index)
	{
		if (Options.bShareOrigin == true && Index > 0)
		{
			Origins.Add(FMeshParticleizerResolvedOrigin(Origins[0]));
			continue;
		}
		auto* Mesh = Targets[Index].Mesh.Get();
		if (Mesh == nullptr)
		{
			OutError = TEXT("Origin source is no longer available.");
			return false;
		}
		const auto& Space = Spaces[Index];
		auto Result = FMeshParticleizerResolvedOrigin{};
		Result.Mode = Options.Origin;
		Result.Accuracy = EMeshParticleizerOriginAccuracy::ReferenceSpace;
		Result.ReferenceMesh = Mesh;
		Result.OriginMesh = Mesh;
		Result.ReferencePosition = Options.Origin == EMeshParticleizerOrigin::Point ? Options.LocalOrigin : Space.Bounds.GetCenter();
		Result.ReferenceDirection = Options.Direction.GetSafeNormal();

		if (Options.Origin == EMeshParticleizerOrigin::MultiplePoints)
		{
			Result.ReferencePoints = Options.LocalOrigins;
			Result.ReferencePosition = Result.ReferencePoints[0];
			Result.ReferenceDistance = MeshParticleizerPointField::MaximumDistance(Space.Bounds, Result.ReferencePoints);
		}
		else if (Options.Origin == EMeshParticleizerOrigin::Direction && Options.bDirectionInWorldSpace == true)
		{
			const auto Transform = Mesh->GetComponentTransform().ToMatrixWithScale();
			if (MeshParticleizerReference::IsInvertible(Transform) == false)
			{
				OutError = TEXT("World direction requires a finite, non-zero source transform.");
				return false;
			}
			// 進行平面の法線として変換し、非一様スケールでもワールド方向を保つ。
			Result.ReferenceDirection = FVector(Transform.GetTransposed().TransformVector(Options.Direction)).GetSafeNormal();
		}
		else if (Options.Origin == EMeshParticleizerOrigin::BoneOrSocket)
		{
			if (MeshParticleizerOrigin::SetBoneOrigin(Mesh, Options.OriginBoneOrSocket, Space, Result) == false)
			{
				Result.Diagnostics = FString::Printf(TEXT("Origin bone/socket '%s' is unavailable."), *Options.OriginBoneOrSocket.ToString());
				MeshParticleizerOrigin::UseFallback(Mesh, Options, Space, Result);
			}
		}
		else if (Options.Origin == EMeshParticleizerOrigin::Hit)
		{
			const auto& Hit = Options.HitOrigin;
			int32 HitIndex = INDEX_NONE;
			for (auto TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
			{
				if (Targets[TargetIndex].Mesh == Hit.SourceMesh && (Options.bShareOrigin == true || TargetIndex == Index))
				{
					HitIndex = TargetIndex;
					break;
				}
			}
			const auto bCaptured = Hit.Accuracy == EMeshParticleizerOriginAccuracy::StaticHit ||
				Hit.Accuracy == EMeshParticleizerOriginAccuracy::BoneApproximation || Hit.Accuracy == EMeshParticleizerOriginAccuracy::SurfaceProjection;
			if (HitIndex != INDEX_NONE && bCaptured == true && Hit.ReferencePosition.ContainsNaN() == false &&
				Hit.SourceAsset.ToSoftObjectPath() == FSoftObjectPath(MeshParticleizerOrigin::GetMeshAsset(Hit.SourceMesh.Get())))
			{
				Result.Mode = EMeshParticleizerOrigin::Point;
				Result.Accuracy = Hit.Accuracy;
				Result.ReferencePosition = FVector(Spaces[HitIndex].SourceToGroup.TransformPosition(Hit.ReferencePosition));
				Result.OriginMesh = Hit.SourceMesh;
				Result.BoneOrSocket = Hit.BoneName;
				Result.Diagnostics = Hit.Diagnostics;
			}
			else
			{
				Result.Diagnostics = TEXT("Captured hit is unavailable for this source or its mesh asset changed. ") + Hit.Diagnostics;
				MeshParticleizerOrigin::UseFallback(Mesh, Options, Space, Result);
			}
		}
		if (Result.ReferencePosition.ContainsNaN() == true || Result.ReferenceDirection.ContainsNaN() == true ||
			(Result.Mode == EMeshParticleizerOrigin::MultiplePoints && FMath::IsFinite(Result.ReferenceDistance) == false) ||
			(Result.Mode == EMeshParticleizerOrigin::Direction && Result.ReferenceDirection.IsNearlyZero() == true))
		{
			OutError = TEXT("Resolved origin contains an invalid position or direction.");
			return false;
		}
		Origins.Add(MoveTemp(Result));
	}
	OutOrigins = MoveTemp(Origins);
	OutError.Reset();
	return true;
}
