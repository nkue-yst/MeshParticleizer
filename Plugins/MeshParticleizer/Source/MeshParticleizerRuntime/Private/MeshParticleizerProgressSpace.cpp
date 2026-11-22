#include "MeshParticleizerTypes.h"
#include "MeshParticleizerReference.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Components/MeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"

namespace MeshParticleizerReference
{
	bool IsInvertible(const FMatrix& Matrix)
	{
		return Matrix.ContainsNaN() == false && FMath::IsNearlyZero(Matrix.Determinant(), 1.e-12) == false;
	}

	bool GetSocketTransform(const USceneComponent* Component, FName Name, FMatrix& OutTransform)
	{
		if (Component == nullptr || Name.IsNone() == true || Component->DoesSocketExist(Name) == false)
		{
			return false;
		}
		if (const auto* Skinned = Cast<USkinnedMeshComponent>(Component))
		{
			auto SocketLocal = FTransform::Identity;
			int32 BoneIndex = INDEX_NONE;
			if (Skinned->GetSocketInfoByName(Name, SocketLocal, BoneIndex) == nullptr)
			{
				BoneIndex = Skinned->GetBoneIndex(Skinned->GetSocketBoneName(Name));
			}
			const auto* Asset = Skinned->GetSkinnedAsset();
			if (Asset == nullptr || Asset->GetRefBasesInvMatrix().IsValidIndex(BoneIndex) == false)
			{
				return false;
			}
			OutTransform = SocketLocal.ToMatrixWithScale() * FMatrix(Asset->GetRefBasesInvMatrix()[BoneIndex]).Inverse();
		}
		else
		{
			OutTransform = Component->GetSocketTransform(Name, RTS_Component).ToMatrixWithScale();
		}
		return IsInvertible(OutTransform);
	}

	bool ReferenceToWorld(const USceneComponent* Component, TMap<const USceneComponent*, FMatrix>& Cache,
		FMatrix& OutMatrix, FString& OutError)
	{
		if (const auto* Cached = Cache.Find(Component))
		{
			OutMatrix = *Cached;
			return true;
		}

		OutMatrix = Component->GetComponentTransform().ToMatrixWithScale();
		const auto* Parent = Component->GetAttachParent();
		if (Parent != nullptr)
		{
			auto ParentReference = FMatrix::Identity;
			if (ReferenceToWorld(Parent, Cache, ParentReference, OutError) == false)
			{
				return false;
			}

			const auto SocketName = Component->GetAttachSocketName();
			const auto CurrentSocket = Parent->GetSocketTransform(SocketName, RTS_World).ToMatrixWithScale();
			if (IsInvertible(CurrentSocket) == false)
			{
				OutError = TEXT("Shared origin requires finite, non-zero attachment scales.");
				return false;
			}
			auto ReferenceSocket = Parent->GetSocketTransform(SocketName, RTS_Component).ToMatrixWithScale();
			if (SocketName.IsNone() == false && Parent->DoesSocketExist(SocketName) == true)
			{
				// 装備の取付位置を動作中の骨から参照姿勢へ戻す。未登録の親も座標だけ参照する。
				if (GetSocketTransform(Parent, SocketName, ReferenceSocket) == false)
				{
					OutError = TEXT("Attachment bone or socket has no usable reference transform for the shared origin.");
					return false;
				}
			}
			OutMatrix = OutMatrix * CurrentSocket.Inverse() * ReferenceSocket * ParentReference;
		}
		if (IsInvertible(OutMatrix) == false)
		{
			OutError = TEXT("Shared origin requires finite, non-zero source scales.");
			return false;
		}
		Cache.Add(Component, OutMatrix);
		return true;
	}
}

bool FMeshParticleizerProgressSpace::Resolve(TConstArrayView<FMeshParticleizerTarget> Targets, bool bShareOrigin,
	TArray<FMeshParticleizerProgressSpace>& OutSpaces, FString& OutError)
{
	OutSpaces.Reset();
	auto Spaces = TArray<FMeshParticleizerProgressSpace>{};
	for (const auto& Target : Targets)
	{
		const auto* Data = Target.PreparedData.Get();
		if (Target.Mesh.IsValid() == false || Data == nullptr || Data->BoundsMin.ContainsNaN() == true || Data->BoundsMax.ContainsNaN() == true ||
			Data->BoundsMin.X > Data->BoundsMax.X || Data->BoundsMin.Y > Data->BoundsMax.Y || Data->BoundsMin.Z > Data->BoundsMax.Z)
		{
			OutError = TEXT("Progress coordinates require valid source components and prepared bounds.");
			return false;
		}
		auto& Space = Spaces.AddDefaulted_GetRef();
		Space.Bounds = FBox(Data->BoundsMin, Data->BoundsMax);
	}

	if (bShareOrigin == true && Targets.Num() > 1)
	{
		auto Cache = TMap<const USceneComponent*, FMatrix>{};
		auto GroupReference = FMatrix::Identity;
		if (MeshParticleizerReference::ReferenceToWorld(Targets[0].Mesh.Get(), Cache, GroupReference, OutError) == false)
		{
			return false;
		}
		const auto WorldToGroup = GroupReference.Inverse();
		auto GroupBounds = FBox(ForceInit);
		for (auto Index = 0; Index < Targets.Num(); ++Index)
		{
			auto Reference = FMatrix::Identity;
			if (MeshParticleizerReference::ReferenceToWorld(Targets[Index].Mesh.Get(), Cache, Reference, OutError) == false)
			{
				return false;
			}
			auto& Space = Spaces[Index];

			// 行列のまま合成し、回転と非一様スケールで生じるせん断を失わない。
			Space.SourceToGroup = Index == 0 ? FMatrix::Identity : Reference * WorldToGroup;
			GroupBounds += Space.Bounds.TransformBy(Space.SourceToGroup);
		}
		for (auto& Space : Spaces)
		{
			Space.Bounds = GroupBounds;
		}
	}
	OutSpaces = MoveTemp(Spaces);
	OutError.Reset();
	return true;
}
