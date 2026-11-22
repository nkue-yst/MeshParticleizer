#include "MeshParticleizeSource.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

bool UMeshParticleizeSource::FreezeSource(FString& OutError)
{
	auto* Mesh = this->Source.Get();
	auto* Manager = this->GetTypedOuter<UMeshParticleizerWorldSubsystem>();
	const auto* History = Manager->FindSourceHistory(Mesh);
	if (History == nullptr || History->bHasCurrent == false)
	{
		OutError = TEXT("Freeze At Start requires a captured source transform.");
		return false;
	}
	this->FrozenTransforms = *History;

	// 描画と粒子は同じPostPhysics履歴を使う。元のAnimationや物理設定は変更しない。
	if (auto* Skinned = Cast<USkinnedMeshComponent>(Mesh))
	{
		auto* Asset = Cast<USkeletalMesh>(Skinned->GetSkinnedAsset());
		const auto* Bones = Manager->FindBoneHistory(Mesh);
		if (Asset == nullptr || Bones == nullptr || Bones->bHasCurrent == false ||
			Bones->Current.Num() != Asset->GetRefBasesInvMatrix().Num() * 3)
		{
			OutError = TEXT("Freeze At Start requires the source's complete bone pose.");
			return false;
		}
		this->FrozenBones = *Bones;
		auto* Pose = NewObject<UPoseableMeshComponent>(Manager->GetWorld(), NAME_None, RF_Transient);
		Pose->SetSkinnedAssetAndUpdate(Asset);
		Pose->SetForcedLOD(1);
		if (Pose->AllocateTransformData() == false)
		{
			OutError = TEXT("The frozen source pose could not be allocated.");
			return false;
		}

		// スキニング行列から表示用の骨姿勢へ戻す。CPUでは頂点を評価しない。
		auto ComponentPose = TArray<FTransform>{};
		const auto& InverseBind = Asset->GetRefBasesInvMatrix();
		ComponentPose.Reserve(InverseBind.Num());
		for (auto Bone = 0; Bone < InverseBind.Num(); ++Bone)
		{
			auto Skinning = FMatrix44f::Identity;
			for (auto Axis = 0; Axis < 3; ++Axis)
			{
				for (auto Row = 0; Row < 4; ++Row)
				{
					Skinning.M[Row][Axis] = Bones->Current[Bone * 3 + Axis][Row];
				}
			}
			ComponentPose.Add(FTransform(FMatrix(InverseBind[Bone].Inverse() * Skinning)));
		}
		Pose->BoneSpaceTransforms = Asset->GetRefSkeleton().GetRefBonePose();
		for (auto Bone = 0; Bone < ComponentPose.Num(); ++Bone)
		{
			const auto Parent = Asset->GetRefSkeleton().GetParentIndex(Bone);
			Pose->BoneSpaceTransforms[Bone] = Parent == INDEX_NONE ? ComponentPose[Bone]
																   : ComponentPose[Bone].GetRelativeTransform(ComponentPose[Parent]);
		}
		Pose->RefreshBoneTransforms();
		this->FrozenMesh = Pose;
	}
	else if (auto* Static = Cast<UStaticMeshComponent>(Mesh))
	{
		auto* Copy = NewObject<UStaticMeshComponent>(Manager->GetWorld(), NAME_None, RF_Transient);
		Copy->SetStaticMesh(Static->GetStaticMesh());
		Copy->SetForcedLodModel(1);
		this->FrozenMesh = Copy;
	}
	else
	{
		OutError = TEXT("Freeze At Start requires a Static or Skeletal Mesh source.");
		return false;
	}

	// 固定した描画専用Componentをワールドへ登録する。当たり判定は元の責務に残す。
	this->FrozenMesh->PrimaryComponentTick.bCanEverTick = false;
	this->FrozenMesh->PrimaryComponentTick.bStartWithTickEnabled = false;
	this->FrozenMesh->SetMobility(EComponentMobility::Movable);
	this->FrozenMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	this->FrozenMesh->SetWorldTransform(this->FrozenTransforms.Current);
	this->FrozenMesh->SetVisibility(Mesh->GetVisibleFlag());
	this->FrozenMesh->SetHiddenInGame(Mesh->bHiddenInGame);
	this->FrozenMesh->SetAffectDistanceFieldLighting(Mesh->bAffectDistanceFieldLighting);
	this->FrozenMesh->SetCastShadow(Mesh->CastShadow);
	this->FrozenMesh->SetCastHiddenShadow(Mesh->bCastHiddenShadow);
	this->FrozenMesh->SetRenderInMainPass(Mesh->bRenderInMainPass);
	this->FrozenMesh->SetRenderInDepthPass(Mesh->bRenderInDepthPass);
	this->FrozenMesh->SetRenderCustomDepth(Mesh->bRenderCustomDepth);
	this->FrozenMesh->SetCustomDepthStencilValue(Mesh->CustomDepthStencilValue);
	this->FrozenMesh->SetLightingChannels(Mesh->LightingChannels.bChannel0, Mesh->LightingChannels.bChannel1, Mesh->LightingChannels.bChannel2);
	this->FrozenMesh->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
	for (auto Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
	{
		this->FrozenMesh->SetMaterial(Slot, Mesh->GetMaterial(Slot));
	}
	this->FrozenMesh->RegisterComponentWithWorld(Manager->GetWorld());

	this->bOriginalVisible = Mesh->GetVisibleFlag();
	this->bOriginalHiddenShadow = Mesh->bCastHiddenShadow;
	this->bSourceVisibilityOverridden = true;
	Mesh->SetVisibility(false);
	Mesh->SetCastHiddenShadow(false);
	this->Diagnostics += TEXT("Freeze At Start holds the rendered pose and pre-freeze inertia; source lifetime remains required until conversion. ");
	return true;
}

void UMeshParticleizeSource::ReleaseFrozenSource()
{
	if (this->FrozenMesh != nullptr)
	{
		this->FrozenMesh->DestroyComponent();
		this->FrozenMesh = nullptr;
	}

	if (this->bSourceVisibilityOverridden == false)
	{
		return;
	}
	this->bSourceVisibilityOverridden = false;
	if (auto* Mesh = this->Source.Get())
	{
		if (Mesh->GetVisibleFlag() == false)
		{
			Mesh->SetVisibility(this->bOriginalVisible);
		}
		if (Mesh->bCastHiddenShadow == false)
		{
			Mesh->SetCastHiddenShadow(this->bOriginalHiddenShadow);
		}
	}
	this->FrozenBones.Current.Empty();
	this->FrozenBones.Previous.Empty();
}
