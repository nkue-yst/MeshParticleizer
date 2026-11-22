#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerSurfaceProjection.generated.h"

USTRUCT()
struct FMeshParticleizerProjectionVertex
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;

	UPROPERTY()
	uint16 Bones[12]{};

	UPROPERTY()
	float Weights[12]{};

	bool Serialize(FArchive& Archive);
};

template <>
struct TStructOpsTypeTraits<FMeshParticleizerProjectionVertex> : public TStructOpsTypeTraitsBase2<FMeshParticleizerProjectionVertex>
{
	enum
	{
		WithSerializer = true
	};
};

USTRUCT()
struct FMeshParticleizerProjectionNode
{
	GENERATED_BODY()

	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	UPROPERTY()
	int32 First = 0;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	int32 Left = INDEX_NONE;

	UPROPERTY()
	int32 Right = INDEX_NONE;

	UPROPERTY()
	TArray<uint16> Bones;

	// 元ウェイトの丸めを保持し、合計が厳密に1でなくても探索境界を狭めない。
	UPROPERTY()
	double MinWeight = 1;

	UPROPERTY()
	double MaxWeight = 1;
};

struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerProjectionResult
{
	FVector ReferencePosition = FVector::ZeroVector;
	FVector WorldPosition = FVector::ZeroVector;
	FVector Barycentrics = FVector::ZeroVector;
	int32 Triangle = INDEX_NONE;
	int32 TestedTriangles = 0;
	int32 VisitedNodes = 0;
};

// 準備資産に登録する選択LODの探索木。粒子候補数とは独立し、Cook後もCPUから参照できる。
USTRUCT()
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerSurfaceProjection
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	bool Build(TConstArrayView<FLinearColor> SkinVertices, int32 VertexCount,
		TConstArrayView<FLinearColor> SkinTriangles, int32 TriangleCount, int32 InBoneCount, FString& OutError);
#endif

	bool Project(TConstArrayView<FMatrix44f> Bones, const FMatrix& SourceToWorld,
		const FVector& WorldPoint, double MaxDistance, FMeshParticleizerProjectionResult& OutResult) const;
	bool IsReady() const
	{
		return this->bValid;
	}
	int32 GetTriangleCount() const
	{
		return this->Triangles.Num();
	}
	int32 GetBoneCount() const
	{
		return this->BoneCount;
	}
	void PostSerialize(const FArchive& Archive);

private:
	bool ValidateGeometry() const;
	bool Validate() const;

	UPROPERTY()
	int32 Version = 0;

	UPROPERTY()
	int32 BoneCount = 0;

	UPROPERTY()
	TArray<FMeshParticleizerProjectionVertex> Vertices;

	UPROPERTY()
	TArray<FIntVector> Triangles;

	UPROPERTY()
	TArray<int32> Order;

	UPROPERTY()
	TArray<FMeshParticleizerProjectionNode> Nodes;

	bool bValid = false;
};

template <>
struct TStructOpsTypeTraits<FMeshParticleizerSurfaceProjection> : public TStructOpsTypeTraitsBase2<FMeshParticleizerSurfaceProjection>
{
	enum
	{
		WithPostSerialize = true
};
};
