#include "MeshParticleizerPointField.h"

double MeshParticleizerPointField::MaximumDistance(const FBox& Bounds, TConstArrayView<FVector> Points)
{
	// 各起点に最も近い領域を平面で区切る。領域内の距離の最大値は、その頂点にある。
	// 起点は最大4点。表面サンプルやメッシュ頂点を走査せず、開始時だけ定数規模で求める。
	auto MaximumSquared = 0.0;
	const auto Tolerance = FMath::Max(Bounds.GetSize().GetMax(), 1.0) * 0.000001;
	for (auto Index = 0; Index < Points.Num(); ++Index)
	{
		auto Planes = TArray<FPlane, TInlineAllocator<9>>{
			FPlane(1, 0, 0, Bounds.Max.X), FPlane(-1, 0, 0, -Bounds.Min.X),
			FPlane(0, 1, 0, Bounds.Max.Y), FPlane(0, -1, 0, -Bounds.Min.Y),
			FPlane(0, 0, 1, Bounds.Max.Z), FPlane(0, 0, -1, -Bounds.Min.Z)};
		for (auto Other = 0; Other < Points.Num(); ++Other)
		{
			const auto Difference = Points[Other] - Points[Index];
			if (Other == Index || Difference.IsZero() == true)
			{
				continue;
			}
			const auto Normal = Difference.GetUnsafeNormal();
			Planes.Add(FPlane(Normal, FVector::DotProduct(Normal, (Points[Index] + Points[Other]) * 0.5)));
		}

		for (auto A = 0; A < Planes.Num(); ++A)
		{
			for (auto B = A + 1; B < Planes.Num(); ++B)
			{
				for (auto C = B + 1; C < Planes.Num(); ++C)
				{
					const auto BC = FVector::CrossProduct(Planes[B], Planes[C]);
					const auto Determinant = FVector::DotProduct(Planes[A], BC);
					if (FMath::Abs(Determinant) <= 1.e-12)
					{
						continue;
					}
					const auto Point = (BC * Planes[A].W + FVector::CrossProduct(Planes[C], Planes[A]) * Planes[B].W +
										   FVector::CrossProduct(Planes[A], Planes[B]) * Planes[C].W) /
						Determinant;
					if (Planes.ContainsByPredicate([&Point, Tolerance](const FPlane& Plane) {
							return Plane.PlaneDot(Point) > Tolerance;
						}))
					{
						continue;
					}
					const auto Bounded = Point.ComponentMax(Bounds.Min).ComponentMin(Bounds.Max);
					auto NearestSquared = TNumericLimits<double>::Max();
					for (const auto& Origin : Points)
					{
						NearestSquared = FMath::Min(NearestSquared, FVector::DistSquared(Bounded, Origin));
					}
					MaximumSquared = FMath::Max(MaximumSquared, NearestSquared);
				}
			}
		}
	}
	return FMath::Max(FMath::Sqrt(MaximumSquared), 0.0001);
}
