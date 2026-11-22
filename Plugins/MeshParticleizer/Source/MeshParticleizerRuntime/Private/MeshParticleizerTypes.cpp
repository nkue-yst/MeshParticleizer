#include "MeshParticleizerTypes.h"
#include "Components/MeshComponent.h"

bool FMeshParticleizerTarget::ValidateTargets(TConstArrayView<FMeshParticleizerTarget> Targets,
	const UWorld* World, FString& OutError)
{
	OutError.Reset();
	if (World == nullptr || Targets.IsEmpty() == true)
	{
		OutError = TEXT("Register at least one target in a valid world.");
		return false;
	}

	// 全対象を確認してから登録を置き換える。失敗理由は配列の順番と部品へ対応させる。
	auto Seen = TSet<UMeshComponent*>{};
	for (auto Index = 0; Index < Targets.Num(); ++Index)
	{
		const auto& Target = Targets[Index];
		auto* Mesh = Target.Mesh.Get();
		const TCHAR* Reason = nullptr;
		if (Mesh == nullptr)
		{
			Reason = TEXT("select a valid mesh component");
		}
		else if (Mesh->GetWorld() != World)
		{
			Reason = TEXT("the mesh belongs to a different world");
		}
		else if (Seen.Contains(Mesh) == true)
		{
			Reason = TEXT("the mesh is already registered earlier in this array");
		}
		else if (Target.PreparedData.IsNull() == true)
		{
			Reason = TEXT("select prepared data");
		}
		else if (FMath::IsFinite(Target.Importance) == false || Target.Importance <= 0)
		{
			Reason = TEXT("Importance must be finite and greater than zero");
		}
		if (Reason != nullptr)
		{
			OutError = FString::Printf(TEXT("Source %d '%s': %s."), Index, *GetNameSafe(Mesh), Reason);
			return false;
		}
		Seen.Add(Mesh);
	}
	return true;
}

namespace MeshParticleizerHistory
{
template <typename MatrixGetter>
bool CaptureBones(FMeshParticleizerBoneHistory& History, int32 BoneCount, uint32 Revision, double Time, MatrixGetter GetMatrix)
	{
	if (BoneCount == 0 || FMath::IsFinite(Time) == false)
		{
		History.Reset();
		return false;
		}
		if (History.bHasCurrent == true && (Time < History.CurrentTime || History.Current.Num() != BoneCount * 3))
		{
			History.Reset();
		}
		if (History.bHasCurrent == true && Revision == History.CurrentRevision)
		{
			return true;
		}

	// 同じゲーム時刻で姿勢が変わっても、ゼロ秒の速度履歴は作らない。
	if (History.bHasCurrent == true && Time > History.CurrentTime)
		{
		Swap(History.Current, History.Previous);
		History.PreviousTime = History.CurrentTime;
		History.bHasPrevious = true;
		}
	// 仮想ボーンには描画頂点のウェイトがないため、実ボーンだけを送る。
	History.Current.SetNumUninitialized(BoneCount * 3);
	for (auto Bone = 0; Bone < BoneCount; ++Bone)
		{
		auto Matrix = FMatrix44f::Identity;
		if (GetMatrix(Bone, Matrix) == false || Matrix.ContainsNaN() == true)
			{
			History.Reset();
			return false;
			}
		for (auto Axis = 0; Axis < 3; ++Axis)
			{
			History.Current[Bone * 3 + Axis] = FVector4f(Matrix.M[0][Axis], Matrix.M[1][Axis], Matrix.M[2][Axis], Matrix.M[3][Axis]);
			}
		}
		if (History.Previous.Num() != History.Current.Num())
		{
			History.bHasPrevious = false;
		}
	History.CurrentTime = Time;
	History.CurrentRevision = Revision;
	History.bHasCurrent = true;
	++History.Serial;
	return true;
	}
}

bool FMeshParticleizerBoneHistory::Capture(TConstArrayView<FTransform> Transforms,
	TConstArrayView<FMatrix44f> InverseBind, uint32 Revision, double Time)
{
	if (Transforms.Num() < InverseBind.Num())
	{
		this->Reset();
		return false;
	}
	return MeshParticleizerHistory::CaptureBones(*this, InverseBind.Num(), Revision, Time,
		[Transforms, InverseBind](int32 Bone, FMatrix44f& Matrix) {
			if (Transforms[Bone].ContainsNaN() == true)
			{
				return false;
			}
			Matrix = InverseBind[Bone] * FMatrix44f(Transforms[Bone].ToMatrixWithScale());
			return true;
		});
}

bool FMeshParticleizerBoneHistory::CaptureMatrices(TConstArrayView<FMatrix44f> Matrices, uint32 Revision, double Time)
{
	return MeshParticleizerHistory::CaptureBones(*this, Matrices.Num(), Revision, Time,
		[Matrices](int32 Bone, FMatrix44f& Matrix) {
			Matrix = Matrices[Bone];
			return true;
		});
}

void FMeshParticleizerBoneHistory::Reset()
{
	this->bHasCurrent = false;
	this->bHasPrevious = false;
	++this->Serial;
}

bool FMeshParticleizerBoneHistory::HasVelocity() const
{
	return this->bHasCurrent == true && this->bHasPrevious == true && this->CurrentTime - this->PreviousTime > UE_SMALL_NUMBER;
}

bool FMeshParticleizeOptions::Validate(FString& OutError) const
{
	const auto StylePath = this->ParticleStyle.ToSoftObjectPath();
	if (StylePath.IsNull() == false && (StylePath.IsAsset() == false || StylePath.GetLongPackageName() == TEXT("/Engine/Transient")))
	{
		OutError = TEXT("Particle Style must reference a saved asset.");
		return false;
	}

	if (FMeshParticleizerQualityLimits::Find(this->Quality) == nullptr ||
		StaticEnum<EMeshParticleizerPriority>()->IsValidEnumValue(static_cast<int64>(this->Priority)) == false ||
		StaticEnum<EMeshParticleizerOrigin>()->IsValidEnumValue(static_cast<int64>(this->Origin)) == false ||
		StaticEnum<EMeshParticleizerMotion>()->IsValidEnumValue(static_cast<int64>(this->Motion)) == false ||
		StaticEnum<EMeshParticleizerDisappear>()->IsValidEnumValue(static_cast<int64>(this->Disappear)) == false ||
		StaticEnum<EMeshParticleizerCollision>()->IsValidEnumValue(static_cast<int64>(this->Collision)) == false ||
		StaticEnum<EMeshParticleizerGround>()->IsValidEnumValue(static_cast<int64>(this->Ground)) == false)
	{
		OutError = TEXT("An option contains an unknown mode.");
		return false;
	}

	if (FMath::IsFinite(this->ConversionDuration) == false || this->ConversionDuration < 0 ||
		FMath::IsFinite(this->ParticleLifetime) == false || this->ParticleLifetime < 0 ||
		FMath::IsFinite(this->FadeDuration) == false || this->FadeDuration < 0 || FMath::IsFinite(this->GroundHeight) == false)
	{
		OutError = TEXT("Conversion duration and particle lifetime must be finite and non-negative.");
		return false;
	}

	if (this->Direction.ContainsNaN() == true || this->LocalOrigin.ContainsNaN() == true ||
		(this->Origin == EMeshParticleizerOrigin::Direction && this->Direction.IsNearlyZero() == true))
	{
		OutError = TEXT("Origin must be finite; direction must be non-zero.");
		return false;
	}

	if (this->Origin == EMeshParticleizerOrigin::MultiplePoints)
	{
		if (this->LocalOrigins.Num() < 1 || this->LocalOrigins.Num() > 4 ||
			this->LocalOrigins.ContainsByPredicate([](const FVector& Point) {
				return Point.ContainsNaN();
			}))
		{
			OutError = TEXT("Multiple Points requires between one and four finite reference-space positions.");
			return false;
		}
	}

	if (this->MeshAngularVelocity.ContainsNaN() == true || this->MeshAngularVelocity.GetAbsMax() > 36000 ||
		FMath::IsFinite(this->ParticleSize) == false || this->ParticleSize <= 0 ||
		FMath::IsFinite(this->Emissive) == false || this->Emissive < 0 ||
		FMath::IsFinite(this->InheritVelocityScale) == false || this->InheritVelocityScale < 0 ||
		this->MaxParticles < 0 || this->Tint.R < 0 || this->Tint.G < 0 || this->Tint.B < 0 || this->Tint.A < 0 ||
		FMath::IsFinite(this->Tint.R) == false || FMath::IsFinite(this->Tint.G) == false ||
		FMath::IsFinite(this->Tint.B) == false || FMath::IsFinite(this->Tint.A) == false)
	{
		OutError = TEXT("Particle appearance, velocity scale and budget must contain valid finite values.");
		return false;
	}

	const float NonNegative[] = {this->NormalSpeed, this->ScatterSpeed, this->RandomSpeed, this->NoiseAcceleration,
		this->Drag, this->Friction, this->Restitution, this->MaxInitialSpeed,
		this->CollisionRadiusScale, this->SleepSpeed, this->SleepDuration, this->DriftGravityScale};
	for (const auto Value : NonNegative)
	{
		if (FMath::IsFinite(Value) == false || Value < 0)
		{
			OutError = TEXT("Motion and collision values must be finite and non-negative.");
			return false;
		}
	}
	if (this->Gravity.ContainsNaN() == true || this->Wind.ContainsNaN() == true || this->InitialVelocity.ContainsNaN() == true ||
		this->Restitution > 1 || this->Friction > 1)
	{
		OutError = TEXT("Motion vectors must be finite; restitution and friction must be between zero and one.");
		return false;
	}
	const auto Radius = this->ParticleSize * this->CollisionRadiusScale;
	const auto AppliedGravity = FVector3f(this->GetEffectiveGravity());
	if (AppliedGravity.ContainsNaN() == true)
	{
		OutError = TEXT("The effective gravity must fit finite GPU float components.");
		return false;
	}
	if (FMath::IsFinite(Radius) == false ||
		(this->Ground == EMeshParticleizerGround::ExplicitHeight && FMath::IsFinite(this->GroundHeight + Radius) == false))
	{
		OutError = TEXT("Particle size, collision radius and ground height produce an unrepresentable contact plane.");
		return false;
	}

	OutError.Reset();
	return true;
}

FVector FMeshParticleizeOptions::GetEffectiveGravity() const
{
	return this->Motion == EMeshParticleizerMotion::Drift ? this->Gravity * this->DriftGravityScale : this->Gravity;
}

double FMeshParticleizeSchedule::Progress(double Now) const
{
	const auto Time = this->ConversionDuration == 0 ? 1.0 : FMath::Clamp((Now - this->StartTime) / this->ConversionDuration, 0.0, 1.0);
	return this->Curve.IsValid() ? this->Curve->Evaluate(Time) : Time;
}

double FMeshParticleizeSchedule::DetachTime(double Order) const
{
	const auto Progress = FMath::Clamp(Order, 0.0, 1.0);
	const auto Time = this->Curve.IsValid() ? this->Curve->Inverse(Progress) : Progress;
	return this->StartTime + Time * this->ConversionDuration;
}

double FMeshParticleizeSchedule::ParticleAge(double Now, double Order) const
{
	return FMath::Max(0.0, Now - this->DetachTime(Order));
}

bool FMeshParticleizeSchedule::IsConverted(double Now) const
{
	return Now >= this->StartTime + this->ConversionDuration;
}

bool FMeshParticleizeSchedule::IsFinished(double Now) const
{
	return Now >= this->StartTime + this->ConversionDuration + this->ParticleLifetime;
}

void FMeshParticleizerTransformHistory::Capture(const FTransform& Transform, double Time, const FIntVector& WorldOrigin)
{
	if (Transform.ContainsNaN() == true || FMath::IsFinite(Time) == false)
	{
		this->Reset();
		return;
	}
	if (this->bHasCurrent == true && Time < this->CurrentTime)
	{
		this->Reset();
	}

	this->Rebase(WorldOrigin);
	if (this->bHasCurrent == true && Time > this->CurrentTime)
	{
		this->Previous = this->Current;
		this->PreviousTime = this->CurrentTime;
		this->bHasPrevious = true;
		this->bDiscontinuous = false;
	}
	this->Current = Transform;
	this->CurrentTime = Time;
	this->bHasCurrent = true;
}

void FMeshParticleizerTransformHistory::Rebase(const FIntVector& WorldOrigin)
{
	// 固定姿勢でも履歴の時刻を進めず、前後を現在の原点へ揃える。
	if (this->bHasCurrent == true)
	{
		const auto Offset = FVector(this->Origin) - FVector(WorldOrigin);
		this->Current.AddToTranslation(Offset);
		this->Previous.AddToTranslation(Offset);
	}
	this->Origin = WorldOrigin;
}

void FMeshParticleizerTransformHistory::Reset()
{
	this->bHasCurrent = false;
	this->bHasPrevious = false;
	this->bDiscontinuous = true;
}

bool FMeshParticleizerTransformHistory::HasVelocity() const
{
	return this->bHasPrevious == true && this->CurrentTime - this->PreviousTime > UE_SMALL_NUMBER;
}

const FMeshParticleizerQualityLimits* FMeshParticleizerQualityLimits::Find(EMeshParticleizerQuality Quality)
{
	static constexpr FMeshParticleizerQualityLimits Fast{50000, 10000, 16, 10000, 128ll * 1024 * 1024};
	static constexpr FMeshParticleizerQualityLimits Balanced{200000, 100000, 32, 50000, 256ll * 1024 * 1024};
	static constexpr FMeshParticleizerQualityLimits High{1000000, 250000, 64, 250000, 768ll * 1024 * 1024};
	switch (Quality)
	{
	case EMeshParticleizerQuality::Fast:
		return &Fast;
	case EMeshParticleizerQuality::Balanced:
		return &Balanced;
	case EMeshParticleizerQuality::High:
		return &High;
	default:
		return nullptr;
	}
}

bool FMeshParticleizerBudget::SetQuality(EMeshParticleizerQuality InQuality, FString& OutError)
{
	OutError.Reset();
	const auto* Limits = FMeshParticleizerQualityLimits::Find(InQuality);
	if (Limits == nullptr)
	{
		OutError = TEXT("Unknown Mesh Particleizer quality level.");
		return false;
	}
	if (this->ReservedParticles > Limits->ParticleLimit || this->ActiveSources > Limits->SourceLimit ||
		this->ReservedSimulationBytes > Limits->SimulationMemoryLimitBytes)
	{
		OutError = TEXT("Quality limits must include all active and pending GPU reservations. Retry after resources are released.");
		return false;
	}

	// 全条件が収まった場合だけ一度に切り替える。個別の容量上書きも品質の既定値へ戻す。
	this->Quality = InQuality;
	this->ParticleLimit = Limits->ParticleLimit;
	this->TargetParticleLimit = Limits->TargetParticleLimit;
	this->SourceLimit = Limits->SourceLimit;
	this->SimulationMemoryLimitBytes = Limits->SimulationMemoryLimitBytes;
	return true;
}

bool FMeshParticleizerBudget::Reserve(int32 Requested, int32& OutGranted, int32 SourceCount, int64 SimulationBytes, int64 SharedBytes)
{
	OutGranted = 0;
	const auto SharedIncrease = FMath::Max(SharedBytes, this->SharedGpuReservationBytes) - this->SharedGpuReservationBytes;
	const auto Available = this->SimulationMemoryLimitBytes - this->ReservedSimulationBytes;
	if (Requested < 0 || SourceCount <= 0 || SourceCount > this->SourceLimit - this->ActiveSources ||
		SimulationBytes < 0 || SharedBytes < 0 || SharedIncrease > Available || SimulationBytes > Available - SharedIncrease)
	{
		return false;
	}

	OutGranted = FMath::Min3(Requested, this->TargetParticleLimit, FMath::Max(0, this->ParticleLimit - this->ReservedParticles));
	this->ReservedParticles += OutGranted;
	this->ActiveSources += SourceCount;
	this->ReservedSimulationBytes += SimulationBytes + SharedIncrease;
	this->SharedGpuReservationBytes += SharedIncrease;
	return true;
}

bool FMeshParticleizerBudget::GrowSharedReservation(int64 SharedBytes)
{
	if (SharedBytes < 0)
	{
		return false;
	}
	const auto Increase = FMath::Max(SharedBytes, this->SharedGpuReservationBytes) - this->SharedGpuReservationBytes;
	if (Increase > this->SimulationMemoryLimitBytes - this->ReservedSimulationBytes)
	{
		return false;
	}

	// ソース数・粒子数は変えず、観測された共通容量の増分だけを原子的に予約する。
	this->ReservedSimulationBytes += Increase;
	this->SharedGpuReservationBytes += Increase;
	return true;
}

void FMeshParticleizerBudget::Release(int32 Granted, int32 SourceCount, int64 SimulationBytes)
{
	this->ReservedParticles = FMath::Max(0, this->ReservedParticles - FMath::Max(0, Granted));
	this->ActiveSources = FMath::Max(0, this->ActiveSources - FMath::Max(0, SourceCount));
	this->ReservedSimulationBytes = FMath::Max(this->SharedGpuReservationBytes, this->ReservedSimulationBytes - FMath::Max<int64>(0, SimulationBytes));
}

void FMeshParticleizerAllocation::Distribute(int32 Total, TArrayView<FMeshParticleizerAllocation> Sources)
{
	for (auto& Source : Sources)
	{
		Source.Granted = 0;
	}
	auto Remaining = FMath::Max(0, Total);
	while (Remaining > 0)
	{
		auto WeightSum = 0.0;
		for (const auto& Source : Sources)
		{
			if (Source.Granted < Source.Capacity && FMath::IsFinite(Source.Weight) == true && Source.Weight > 0)
			{
				WeightSum += Source.Weight;
			}
		}
		if (WeightSum <= 0 || FMath::IsFinite(WeightSum) == false)
		{
			return;
		}

		// 上限へ達した部品の余りは、次の周回で残りの部品へ配る。
		const auto Available = Remaining;
		auto bCapped = false;
		auto Remainders = TArray<TPair<double, int32>, TInlineAllocator<32>>{};
		for (auto Index = 0; Index < Sources.Num(); ++Index)
		{
			auto& Source = Sources[Index];
			if (Source.Granted >= Source.Capacity || FMath::IsFinite(Source.Weight) == false || Source.Weight <= 0)
			{
				continue;
			}
			const auto Share = Available * (Source.Weight / WeightSum);
			const auto Capacity = Source.Capacity - Source.Granted;
			bCapped |= Share > Capacity;
			const auto WholeShare = FMath::FloorToInt(Share);
			const auto Grant = FMath::Min(Remaining, FMath::Min(Capacity, WholeShare));
			Source.Granted += Grant;
			Remaining -= Grant;
			if (Source.Granted < Source.Capacity)
			{
				Remainders.Emplace(Share - WholeShare, Index);
			}
		}
		if (bCapped == true)
		{
			continue;
		}

		// 整数化の端数は大きい順へ一つずつ配る。同値は登録順にして再現性を保つ。
		Remainders.Sort([](const auto& Left, const auto& Right) {
			return Left.Key == Right.Key ? Left.Value < Right.Value : Left.Key > Right.Key;
		});
		for (const auto& Remainder : Remainders)
		{
			if (Remaining == 0)
			{
				break;
			}
			++Sources[Remainder.Value].Granted;
			--Remaining;
		}
	}
}
