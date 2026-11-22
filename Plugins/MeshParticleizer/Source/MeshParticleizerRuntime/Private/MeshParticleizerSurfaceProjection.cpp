#include "MeshParticleizerSurfaceProjection.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerReference.h"
#include "Algo/Sort.h"

bool FMeshParticleizerProjectionVertex::Serialize(FArchive& Archive)
{
	// 頂点ごとのPropertyタグを繰り返さず、探索データの保存容量を抑える。
	Archive << this->Position;
	for (auto& Bone : this->Bones)
	{
		Archive << Bone;
	}
	for (auto& Weight : this->Weights)
	{
		Archive << Weight;
	}
	return true;
}

bool FMeshParticleizerSurfaceProjection::ValidateGeometry() const
{
	if (this->Version != 1 || this->BoneCount <= 0 || this->Vertices.IsEmpty() || this->Triangles.IsEmpty())
	{
		return false;
	}
	auto Referenced = TBitArray<>(false, this->Vertices.Num());
	for (const auto& Triangle : this->Triangles)
	{
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			if (this->Vertices.IsValidIndex(Triangle[Corner]) == false)
			{
				return false;
			}
			Referenced[Triangle[Corner]] = true;
		}
	}
	for (auto Index = 0; Index < this->Vertices.Num(); ++Index)
	{
		// 無効セクションだけが使う頂点には、描画用ウェイトを保存していない。
		if (Referenced[Index] == false)
		{
			continue;
		}
		const auto& Vertex = this->Vertices[Index];
		if (Vertex.Position.ContainsNaN() == true)
		{
			return false;
		}
		auto Sum = 0.0;
		for (auto Influence = 0; Influence < 12; ++Influence)
		{
			const auto Weight = Vertex.Weights[Influence];
			if (FMath::IsFinite(Weight) == false || Weight < 0 || (Weight > 0 && Vertex.Bones[Influence] >= this->BoneCount))
			{
				return false;
			}
			Sum += Weight;
		}
		if (FMath::IsNearlyEqual(Sum, 1.0, 0.01) == false)
		{
			return false;
		}
	}
	return true;
}

bool FMeshParticleizerSurfaceProjection::Validate() const
{
	if (this->ValidateGeometry() == false || this->Nodes.IsEmpty() || this->Order.Num() != this->Triangles.Num())
	{
		return false;
	}
	auto Seen = TBitArray<>(false, this->Triangles.Num());
	for (const auto Triangle : this->Order)
	{
		if (this->Triangles.IsValidIndex(Triangle) == false || Seen[Triangle] == true)
		{
			return false;
		}
		Seen[Triangle] = true;
	}
	for (auto Index = 0; Index < this->Nodes.Num(); ++Index)
	{
		const auto& Node = this->Nodes[Index];
		if (Node.Bounds.IsValid == false || Node.Bounds.Min.ContainsNaN() == true || Node.Bounds.Max.ContainsNaN() == true ||
			Node.First < 0 || Node.Count <= 0 || int64(Node.First) + Node.Count > this->Order.Num() || Node.Bones.IsEmpty() ||
			FMath::IsFinite(Node.MinWeight) == false || FMath::IsFinite(Node.MaxWeight) == false ||
			Node.MinWeight <= 0 || Node.MaxWeight < Node.MinWeight)
		{
			return false;
		}
		for (const auto Bone : Node.Bones)
		{
			if (Bone >= this->BoneCount)
			{
				return false;
			}
		}
		if (Node.Left == INDEX_NONE && Node.Right == INDEX_NONE)
		{
			continue;
		}
		if (Node.Left <= Index || Node.Right <= Index || this->Nodes.IsValidIndex(Node.Left) == false ||
			this->Nodes.IsValidIndex(Node.Right) == false)
		{
			return false;
		}
		const auto& Left = this->Nodes[Node.Left];
		const auto& Right = this->Nodes[Node.Right];
		if (Left.First != Node.First || Right.First != Left.First + Left.Count || Left.Count + Right.Count != Node.Count)
		{
			return false;
		}
	}
	return this->Nodes[0].First == 0 && this->Nodes[0].Count == this->Triangles.Num();
}

void FMeshParticleizerSurfaceProjection::PostSerialize(const FArchive& Archive)
{
	if (Archive.IsLoading() == true)
	{
		this->bValid = this->Validate();
	}
}

#if WITH_EDITOR
bool FMeshParticleizerSurfaceProjection::Build(TConstArrayView<FLinearColor> SkinVertices, int32 VertexCount,
	TConstArrayView<FLinearColor> SkinTriangles, int32 TriangleCount, int32 InBoneCount, FString& OutError)
{
	OutError.Reset();
	if (VertexCount <= 0 || int64(VertexCount) * UMeshParticleizerPreparedMesh::SkinVertexStride > SkinVertices.Num() ||
		TriangleCount <= 0 || TriangleCount > SkinTriangles.Num() || InBoneCount <= 0 || InBoneCount > MAX_uint16)
	{
		OutError = TEXT("Surface projection requires complete sampling LOD topology and skin weights.");
		return false;
	}
	auto Candidate = FMeshParticleizerSurfaceProjection{};
	Candidate.Version = 1;
	Candidate.BoneCount = InBoneCount;
	Candidate.Vertices.SetNum(VertexCount);
	for (auto Index = 0; Index < VertexCount; ++Index)
	{
		const auto Offset = Index * UMeshParticleizerPreparedMesh::SkinVertexStride;
		const auto& Position = SkinVertices[Offset];
		auto& Vertex = Candidate.Vertices[Index];
		Vertex.Position = FVector(Position.R, Position.G, Position.B);
		for (auto Influence = 0; Influence < 12; ++Influence)
		{
			const auto Bone = SkinVertices[Offset + 2 + (Influence / 4) * 2].Component(Influence % 4);
			const auto Weight = SkinVertices[Offset + 3 + (Influence / 4) * 2].Component(Influence % 4);
			if (FMath::IsFinite(Bone) == false || Bone < 0 || Bone >= InBoneCount || Bone != FMath::FloorToFloat(Bone))
			{
				OutError = TEXT("Surface projection found an invalid bone index.");
				return false;
			}
			Vertex.Bones[Influence] = static_cast<uint16>(Bone);
			Vertex.Weights[Influence] = Weight;
		}
	}
	for (auto Index = 0; Index < TriangleCount; ++Index)
	{
		const auto& Source = SkinTriangles[Index];
		auto Triangle = FIntVector{};
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			const auto Value = Source.Component(Corner);
			if (FMath::IsFinite(Value) == false || Value < 0 || Value >= VertexCount || Value != FMath::FloorToFloat(Value))
			{
				OutError = TEXT("Surface projection found an invalid triangle index.");
				return false;
			}
			Triangle[Corner] = static_cast<int32>(Value);
		}
		Candidate.Triangles.Add(Triangle);
		Candidate.Order.Add(Index);
	}
	if (Candidate.ValidateGeometry() == false)
	{
		OutError = TEXT("Surface projection found invalid vertex positions or skin weights.");
		return false;
	}

	// 参照姿勢の位置で二分する。各節の影響骨を保存し、変形後も保守的な境界を求める。
	TFunction<int32(int32, int32)> BuildNode;
	BuildNode = [&Candidate, &BuildNode](int32 First, int32 Count) {
		const auto NodeIndex = Candidate.Nodes.AddDefaulted();
		auto& Node = Candidate.Nodes[NodeIndex];
		Node.First = First;
		Node.Count = Count;
		auto BoneSet = TSet<uint16>{};
		for (auto Offset = First; Offset < First + Count; ++Offset)
		{
			const auto& Triangle = Candidate.Triangles[Candidate.Order[Offset]];
			for (auto Corner = 0; Corner < 3; ++Corner)
			{
				const auto& Vertex = Candidate.Vertices[Triangle[Corner]];
				Node.Bounds += Vertex.Position;
				auto Sum = 0.0;
				for (auto Influence = 0; Influence < 12; ++Influence)
				{
					if (Vertex.Weights[Influence] > 0)
					{
						BoneSet.Add(Vertex.Bones[Influence]);
					}
					Sum += Vertex.Weights[Influence];
				}
				Node.MinWeight = FMath::Min(Node.MinWeight, Sum);
				Node.MaxWeight = FMath::Max(Node.MaxWeight, Sum);
			}
		}
		Node.Bones = BoneSet.Array();
		Node.Bones.Sort();
		if (Count > 8)
		{
			const auto Extent = Node.Bounds.GetExtent();
			const auto Axis = Extent.X >= Extent.Y && Extent.X >= Extent.Z ? 0 : (Extent.Y >= Extent.Z ? 1 : 2);
			Algo::Sort(MakeArrayView(Candidate.Order.GetData() + First, Count), [&Candidate, Axis](int32 A, int32 B) {
				const auto Centroid = [&Candidate, Axis](int32 Triangle) {
						const auto& T = Candidate.Triangles[Triangle];
						return Candidate.Vertices[T.X].Position[Axis] + Candidate.Vertices[T.Y].Position[Axis] + Candidate.Vertices[T.Z].Position[Axis];
					};
					const auto AC = Centroid(A);
					const auto BC = Centroid(B);
					return AC == BC ? A < B : AC < BC;
				});
			const auto Left = BuildNode(First, Count / 2);
			const auto Right = BuildNode(First + Count / 2, Count - Count / 2);
			Candidate.Nodes[NodeIndex].Left = Left;
			Candidate.Nodes[NodeIndex].Right = Right;
		}
		return NodeIndex;
	};
	BuildNode(0, TriangleCount);
	Candidate.bValid = Candidate.Validate();
	if (Candidate.bValid == false)
	{
		OutError = TEXT("Surface projection data has invalid positions, weights or bounds.");
		return false;
	}
	*this = MoveTemp(Candidate);
	return true;
}
#endif

bool FMeshParticleizerSurfaceProjection::Project(TConstArrayView<FMatrix44f> Bones, const FMatrix& SourceToWorld,
	const FVector& WorldPoint, double MaxDistance, FMeshParticleizerProjectionResult& OutResult) const
{
	OutResult = FMeshParticleizerProjectionResult{};
	if (this->bValid == false || Bones.Num() < this->BoneCount || WorldPoint.ContainsNaN() == true ||
		FMath::IsFinite(MaxDistance) == false || MaxDistance < 0 || FMath::IsFinite(MaxDistance * MaxDistance) == false ||
		MeshParticleizerReference::IsInvertible(SourceToWorld) == false)
	{
		return false;
	}
	for (const auto Bone : this->Nodes[0].Bones)
	{
		if (Bones[Bone].ContainsNaN() == true)
		{
			return false;
		}
	}

	const auto DistanceToNode = [this, Bones, &SourceToWorld, &WorldPoint](int32 Index) {
		const auto& Node = this->Nodes[Index];
		auto Bounds = FBox(ForceInit);
		for (const auto Bone : Node.Bones)
		{
			Bounds += Node.Bounds.TransformBy(FMatrix(Bones[Bone]));
		}
		auto Weighted = FBox(Bounds.Min * Node.MinWeight, Bounds.Max * Node.MinWeight);
		Weighted += FBox(Bounds.Min * Node.MaxWeight, Bounds.Max * Node.MaxWeight);
		return Weighted.TransformBy(SourceToWorld).ExpandBy(0.0001).ComputeSquaredDistanceToPoint(WorldPoint);
	};
	auto Skinned = TMap<int32, FVector>{};
	const auto Position = [this, Bones, &SourceToWorld, &Skinned](int32 Index) {
		if (const auto* Existing = Skinned.Find(Index))
		{
			return *Existing;
		}
		const auto& Vertex = this->Vertices[Index];
		auto Local = FVector::ZeroVector;
		for (auto Influence = 0; Influence < 12; ++Influence)
		{
			if (Vertex.Weights[Influence] > 0)
			{
				Local += FVector(FMatrix(Bones[Vertex.Bones[Influence]]).TransformPosition(Vertex.Position)) * Vertex.Weights[Influence];
			}
		}
		const auto World = FVector(SourceToWorld.TransformPosition(Local));
		Skinned.Add(Index, World);
		return World;
	};

	// 近い節から進み、現在の最短距離より遠い節を除く。触れた三角形の頂点だけ変形する。
	struct FPending
	{
		int32 Node;
		double Distance;
	};
	auto Pending = TArray<FPending, TInlineAllocator<64>>{};
	auto Best = MaxDistance * MaxDistance;
	Pending.Add({0, DistanceToNode(0)});
	while (Pending.IsEmpty() == false)
	{
		const auto Current = Pending.Pop(EAllowShrinking::No);
		if (FMath::IsFinite(Current.Distance) == false || Current.Distance > Best)
		{
			continue;
		}
		++OutResult.VisitedNodes;
		const auto& Node = this->Nodes[Current.Node];
		if (Node.Left != INDEX_NONE)
		{
			auto Left = FPending{Node.Left, DistanceToNode(Node.Left)};
			auto Right = FPending{Node.Right, DistanceToNode(Node.Right)};
			if (Left.Distance > Right.Distance)
			{
				Swap(Left, Right);
			}
			Pending.Add(Right);
			Pending.Add(Left);
			continue;
		}
		for (auto Offset = Node.First; Offset < Node.First + Node.Count; ++Offset)
		{
			const auto TriangleIndex = this->Order[Offset];
			const auto& Triangle = this->Triangles[TriangleIndex];
			const auto A = Position(Triangle.X);
			const auto B = Position(Triangle.Y);
			const auto C = Position(Triangle.Z);
			++OutResult.TestedTriangles;
			if (A.ContainsNaN() == true || B.ContainsNaN() == true || C.ContainsNaN() == true ||
				FVector::CrossProduct(B - A, C - A).SizeSquared() <= UE_DOUBLE_SMALL_NUMBER)
			{
				continue;
			}
			const auto Closest = FMath::ClosestPointOnTriangleToPoint(WorldPoint, A, B, C);
			const auto Distance = FVector::DistSquared(WorldPoint, Closest);
			if (FMath::IsFinite(Distance) == false || Distance > Best ||
				(Distance == Best && OutResult.Triangle != INDEX_NONE && TriangleIndex >= OutResult.Triangle))
			{
				continue;
			}
			const auto Bary = FMath::ComputeBaryCentric2D(Closest, A, B, C);
			if (Bary.ContainsNaN() == true)
			{
				continue;
			}
			Best = Distance;
			OutResult.Triangle = TriangleIndex;
			OutResult.Barycentrics = Bary;
			OutResult.WorldPosition = Closest;
			OutResult.ReferencePosition = this->Vertices[Triangle.X].Position * Bary.X +
				this->Vertices[Triangle.Y].Position * Bary.Y + this->Vertices[Triangle.Z].Position * Bary.Z;
		}
	}
	return OutResult.Triangle != INDEX_NONE;
}
