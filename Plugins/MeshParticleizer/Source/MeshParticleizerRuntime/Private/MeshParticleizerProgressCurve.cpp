#include "MeshParticleizerTypes.h"
#include "Curves/CurveFloat.h"

namespace MeshParticleizerCurve
{
	float Bezier(const FVector4f& P, float Alpha)
	{
		const auto A = FMath::Lerp(P.X, P.Y, Alpha);
		const auto B = FMath::Lerp(P.Y, P.Z, Alpha);
		const auto C = FMath::Lerp(P.Z, P.W, Alpha);
		return FMath::Lerp(FMath::Lerp(A, B, Alpha), FMath::Lerp(B, C, Alpha), Alpha);
	}

	float InverseBezier(const FVector4f& P, float Value)
	{
		if (Value <= P.X)
		{
			return 0;
		}
		if (Value >= P.W)
		{
			return 1;
		}
		auto Lower = 0.0f;
		auto Upper = 1.0f;
		// GPUと同じ回数・同じ比較で、最初に目標値へ達する位置を求める。
		for (auto Iteration = 0; Iteration < 24; ++Iteration)
		{
			const auto Middle = (Lower + Upper) * 0.5f;
			if (Bezier(P, Middle) < Value)
			{
				Lower = Middle;
			}
			else
			{
				Upper = Middle;
			}
		}
		return Upper;
	}

	bool IsNondecreasing(const FVector4f& P)
	{
		if (P.ContainsNaN() == true)
		{
			return false;
		}
		// 導関数は二次式。端点と区間内の極小を調べ、サンプル間の逆行も検出する。
		const auto D0 = static_cast<double>(P.Y) - P.X;
		const auto D1 = static_cast<double>(P.Z) - P.Y;
		const auto D2 = static_cast<double>(P.W) - P.Z;
		if (D0 < 0 || D2 < 0)
		{
			return false;
		}
		const auto A = D0 - 2 * D1 + D2;
		const auto B = 2 * (D1 - D0);
		if (A > 0)
		{
			const auto T = -B / (2 * A);
			if (T > 0 && T < 1 && (A * T + B) * T + D0 < 0)
			{
				return false;
			}
		}
		return true;
	}

	FVector2f Tangent(float Slope, float Weight, bool bWeighted, float Duration)
	{
		if (bWeighted == false)
		{
			return FVector2f(Duration / 3, Slope * Duration / 3);
		}
		const auto Angle = FMath::Atan(Slope);
		return FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * Weight;
	}
}

bool FMeshParticleizerProgressCurve::Compile(TSoftObjectPtr<UCurveFloat> Curve,
	TSharedPtr<const FMeshParticleizerProgressCurve>& OutCurve, FString& OutError)
{
	OutCurve.Reset();
	OutError.Reset();
	if (Curve.IsNull() == true)
	{
		return true;
	}
	if (Curve.IsValid() == false)
	{
		OutError = TEXT("Progress Curve is not loaded. Prepare For Particleize must finish before starting.");
		return false;
	}
	const auto& Keys = Curve->FloatCurve.GetConstRefOfKeys();
	if (Keys.Num() < 2 || Keys[0].Time != 0 || Keys[0].Value != 0 || Keys.Last().Time != 1 || Keys.Last().Value != 1)
	{
		OutError = TEXT("Progress Curve must start at (time 0, value 0) and end at (time 1, value 1).");
		return false;
	}

	auto Compiled = MakeShared<FMeshParticleizerProgressCurve>();
	for (auto Index = 0; Index + 1 < Keys.Num(); ++Index)
	{
		const auto& First = Keys[Index];
		const auto& Last = Keys[Index + 1];
		if (FMath::IsFinite(First.Time) == false || FMath::IsFinite(Last.Time) == false ||
			FMath::IsFinite(First.Value) == false || FMath::IsFinite(Last.Value) == false ||
			Last.Time <= First.Time || Last.Value < First.Value)
		{
			OutError = FString::Printf(TEXT("Progress Curve interval %d has invalid keys or decreases. Fix the curve; it is not normalized automatically."), Index);
			return false;
		}

		const auto Duration = Last.Time - First.Time;
		auto Times = FVector4f(First.Time, First.Time + Duration / 3, Last.Time - Duration / 3, Last.Time);
		auto Values = FVector4f(First.Value, FMath::Lerp(First.Value, Last.Value, 1.0f / 3),
			FMath::Lerp(First.Value, Last.Value, 2.0f / 3), Last.Value);
		if (First.InterpMode == RCIM_Constant)
		{
			// 階段状の区間を垂直な線として保持し、ジャンプ時刻を丸めない。
			Times = FVector4f(Last.Time, Last.Time, Last.Time, Last.Time);
		}
		else if (First.InterpMode != RCIM_Linear)
		{
			const auto bLeaveWeighted = First.TangentWeightMode == RCTWM_WeightedLeave || First.TangentWeightMode == RCTWM_WeightedBoth;
			const auto bArriveWeighted = Last.TangentWeightMode == RCTWM_WeightedArrive || Last.TangentWeightMode == RCTWM_WeightedBoth;
			const auto Leave = MeshParticleizerCurve::Tangent(First.LeaveTangent, First.LeaveTangentWeight, bLeaveWeighted, Duration);
			const auto Arrive = MeshParticleizerCurve::Tangent(Last.ArriveTangent, Last.ArriveTangentWeight, bArriveWeighted, Duration);
			Times.Y = First.Time + Leave.X;
			Times.Z = Last.Time - Arrive.X;
			Values.Y = First.Value + Leave.Y;
			Values.Z = Last.Value - Arrive.Y;
		}
		if (MeshParticleizerCurve::IsNondecreasing(Times) == false || MeshParticleizerCurve::IsNondecreasing(Values) == false)
		{
			OutError = FString::Printf(TEXT("Progress Curve interval %d reverses time or progress between keys, or has invalid tangents. Fix its tangents."), Index);
			return false;
		}
		Compiled->Segments.Add(Times);
		Compiled->Segments.Add(Values);
	}
	OutCurve = MoveTemp(Compiled);
	return true;
}

float FMeshParticleizerProgressCurve::Evaluate(float Time) const
{
	if (this->Segments.IsEmpty() == true)
	{
		return FMath::Clamp(Time, 0.0f, 1.0f);
	}
	if (Time <= 0)
	{
		return 0;
	}
	if (Time >= 1)
	{
		return 1;
	}
	auto Lower = 0;
	auto Upper = this->Segments.Num() / 2 - 1;
	while (Lower < Upper)
	{
		const auto Middle = (Lower + Upper) / 2;
		if (this->Segments[Middle * 2].W <= Time)
		{
			Lower = Middle + 1;
		}
		else
		{
			Upper = Middle;
		}
	}
	const auto& Times = this->Segments[Lower * 2];
	const auto& Values = this->Segments[Lower * 2 + 1];
	if (Time < Times.X)
	{
		return Values.X;
	}
	return MeshParticleizerCurve::Bezier(Values, MeshParticleizerCurve::InverseBezier(Times, Time));
}

float FMeshParticleizerProgressCurve::Inverse(float Progress) const
{
	if (this->Segments.IsEmpty() == true)
	{
		return FMath::Clamp(Progress, 0.0f, 1.0f);
	}
	if (Progress <= 0)
	{
		return 0;
	}
	auto Lower = 0;
	auto Upper = this->Segments.Num() / 2 - 1;
	while (Lower < Upper)
	{
		const auto Middle = (Lower + Upper) / 2;
		if (this->Segments[Middle * 2 + 1].W < Progress)
		{
			Lower = Middle + 1;
		}
		else
		{
			Upper = Middle;
		}
	}
	return MeshParticleizerCurve::Bezier(this->Segments[Lower * 2],
		MeshParticleizerCurve::InverseBezier(this->Segments[Lower * 2 + 1], Progress));
}
