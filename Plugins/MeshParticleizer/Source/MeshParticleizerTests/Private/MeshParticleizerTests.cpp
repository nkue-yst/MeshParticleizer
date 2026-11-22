#include "MeshParticleizerTypes.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerSceneGpu.h"
#include "Curves/CurveFloat.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEmitter.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshCompiler.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_MODULE(FDefaultModuleImpl, MeshParticleizerTests)

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerTimingTest, "MeshParticleizer.Core.Timing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerTimingTest::RunTest(const FString& Parameters)
{
	const FMeshParticleizeSchedule Schedule{10.0, 2.0, 3.0};
	this->TestEqual(TEXT("Mid conversion"), Schedule.Progress(11), 0.5);
	this->TestEqual(TEXT("Per-particle detach time"), Schedule.DetachTime(0.25), 10.5);
	this->TestEqual(TEXT("Delayed frame counts toward age"), Schedule.ParticleAge(11.0, 0.25), 0.5);
	this->TestFalse(TEXT("Not finished at source conversion"), Schedule.IsFinished(12));
	this->TestTrue(TEXT("Finished at D + L"), Schedule.IsFinished(15));

	const FMeshParticleizeSchedule Immediate{10.0, 0.0, 0.0};
	this->TestEqual(TEXT("Zero duration avoids division"), Immediate.Progress(10), 1.0);
	this->TestTrue(TEXT("Zero duration converts immediately"), Immediate.IsConverted(10));
	this->TestTrue(TEXT("Zero lifetime finishes immediately"), Immediate.IsFinished(10));

	auto Options = FMeshParticleizeOptions{};
	auto Error = FString{};
	this->TestTrue(TEXT("Defaults are valid"), Options.Validate(Error));
	Options.Priority = static_cast<EMeshParticleizerPriority>(255);
	this->TestFalse(TEXT("Unknown priority rejected before GPU use"), Options.Validate(Error));
	Options.Priority = EMeshParticleizerPriority::Normal;
	Options.ParticleLifetime = std::numeric_limits<float>::quiet_NaN();
	this->TestFalse(TEXT("NaN rejected before mutation"), Options.Validate(Error));
	Options.ParticleLifetime = -1;
	this->TestFalse(TEXT("Negative duration rejected"), Options.Validate(Error));

	// 静止時間0は最初の接地で判定する。速度0は静止判定を無効にする。
	Options = FMeshParticleizeOptions{};
	Options.SleepDuration = 0;
	Options.SleepSpeed = 0;
	Options.CollisionRadiusScale = 0;
	this->TestTrue(TEXT("Contact boundary values are valid"), Options.Validate(Error));
	Options.SleepDuration = -0.01f;
	this->TestFalse(TEXT("Negative sleep duration rejected"), Options.Validate(Error));
	Options.SleepDuration = 0.15f;
	Options.SleepSpeed = std::numeric_limits<float>::infinity();
	this->TestFalse(TEXT("Infinite sleep speed rejected"), Options.Validate(Error));
	Options.SleepSpeed = 5;
	Options.CollisionRadiusScale = std::numeric_limits<float>::quiet_NaN();
	this->TestFalse(TEXT("Nonfinite collision radius rejected"), Options.Validate(Error));
	Options.CollisionRadiusScale = 0.5f;
	Options.Friction = 1.01f;
	this->TestFalse(TEXT("Friction above one rejected"), Options.Validate(Error));
	Options.Friction = 1;
	this->TestTrue(TEXT("Unit friction is supported"), Options.Validate(Error));
	Options.ParticleSize = Options.CollisionRadiusScale = std::numeric_limits<float>::max();
	this->TestFalse(TEXT("Overflowing collision radius rejected before GPU use"), Options.Validate(Error));

	auto Asset = TStrongObjectPtr<UCurveFloat>(NewObject<UCurveFloat>());
	auto& Rich = Asset->FloatCurve;
	auto First = FRichCurveKey(0, 0);
	auto Last = FRichCurveKey(1, 1);
	First.InterpMode = RCIM_Cubic;
	Rich.SetKeys({First, Last});
	auto Curve = TSharedPtr<const FMeshParticleizerProgressCurve>{};
	if (this->TestTrue(TEXT("Smooth monotone curve compiles"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error)) == false)
	{
		return false;
	}
	const FMeshParticleizeSchedule Curved{10, 2, 3, Curve};
	this->TestNearlyEqual(TEXT("Curve controls mesh progress"), Curved.Progress(10.5), 0.15625, 0.000001);
	this->TestNearlyEqual(TEXT("Inverse curve controls detach time"), Curved.DetachTime(0.15625), 10.5, 0.000001);
	this->TestNearlyEqual(TEXT("Late frame age uses curved detach time"), Curved.ParticleAge(11, 0.15625), 0.5, 0.000001);
	this->TestTrue(TEXT("Curve preserves D + L finish"), Curved.IsFinished(15));
	this->TestEqual(TEXT("Order zero detaches at start"), Curved.DetachTime(0), 10.0);
	this->TestEqual(TEXT("Curved zero duration reaches endpoint"), (FMeshParticleizeSchedule{10, 0, 0, Curve}).Progress(10), 1.0);

	// 重み付きの接線もUE標準の曲線と一致し、順序値から同じ時刻へ戻る。
	First.TangentWeightMode = RCTWM_WeightedLeave;
	First.LeaveTangentWeight = 0.2f;
	Last.TangentWeightMode = RCTWM_WeightedArrive;
	Last.ArriveTangentWeight = 0.7f;
	Rich.SetKeys({First, Last});
	if (this->TestTrue(TEXT("Weighted monotone curve compiles"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error)) == false)
	{
		return false;
	}
	for (auto Index = 1; Index < 100; ++Index)
	{
		const auto Time = Index / 100.0f;
		this->TestNearlyEqual(TEXT("Weighted shape matches UE curve"), Curve->Evaluate(Time), Rich.Eval(Time), 0.00002f);
		this->TestNearlyEqual(TEXT("Weighted inverse returns logical time"), Curve->Inverse(Curve->Evaluate(Time)), Time, 0.00002f);
	}

	Rich.SetKeys({FRichCurveKey(0, 0), FRichCurveKey(0.25f, 0.5f), FRichCurveKey(0.75f, 0.5f), FRichCurveKey(1, 1)});
	if (this->TestTrue(TEXT("Plateau compiles"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error)) == false)
	{
		return false;
	}
	this->TestNearlyEqual(TEXT("Plateau uses first reaching time"), Curve->Inverse(0.5f), 0.25f, 0.000001f);
	this->TestEqual(TEXT("Plateau remains still"), Curve->Evaluate(0.5f), 0.5f);
	this->TestTrue(TEXT("Next order waits until after plateau"), Curve->Inverse(0.5001f) > 0.75f);

	First = FRichCurveKey(0, 0);
	First.InterpMode = RCIM_Constant;
	Rich.SetKeys({First, FRichCurveKey(0.3f, 0.6f), FRichCurveKey(1, 1)});
	if (this->TestTrue(TEXT("Step curve compiles"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error)) == false)
	{
		return false;
	}
	this->TestEqual(TEXT("Step keeps progress before key"), Curve->Evaluate(0.299f), 0.0f);
	this->TestEqual(TEXT("Step changes exactly at key"), Curve->Evaluate(0.3f), 0.6f);
	this->TestNearlyEqual(TEXT("Orders crossed by step share its time"), Curve->Inverse(0.1f), 0.3f, 0.000001f);
	const auto Snapshot = Curve;

	First = FRichCurveKey(0, 0);
	Last = FRichCurveKey(1, 1);
	First.InterpMode = RCIM_Cubic;
	First.LeaveTangent = Last.ArriveTangent = 4;
	Rich.SetKeys({First, Last});
	this->TestFalse(TEXT("Reversal between increasing keys is rejected"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error));
	this->TestFalse(TEXT("Invalid curve has no compiled result"), Curve.IsValid());
	this->TestEqual(TEXT("Editing asset does not change prepared snapshot"), Snapshot->Evaluate(0.299f), 0.0f);
	First.LeaveTangent = Last.ArriveTangent = 3;
	Rich.SetKeys({First, Last});
	this->TestTrue(TEXT("Stationary derivative without reversal is allowed"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error));

	First.LeaveTangent = Last.ArriveTangent = 0;
	First.TangentWeightMode = RCTWM_WeightedLeave;
	Last.TangentWeightMode = RCTWM_WeightedArrive;
	First.LeaveTangentWeight = Last.ArriveTangentWeight = 1.5f;
	Rich.SetKeys({First, Last});
	this->TestFalse(TEXT("Weighted time fold is rejected"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error));
	Rich.SetKeys({FRichCurveKey(0, 0), FRichCurveKey(0.3f, 0.6f), FRichCurveKey(0.6f, 0.5f), FRichCurveKey(1, 1)});
	this->TestFalse(TEXT("Decreasing keys are rejected"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error));
	Rich.SetKeys({FRichCurveKey(0, 0.1f), FRichCurveKey(1, 1)});
	this->TestFalse(TEXT("Invalid endpoints are not normalized"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error));
	Rich.SetKeys({FRichCurveKey(0, 0), FRichCurveKey(0.5f, std::numeric_limits<float>::quiet_NaN()), FRichCurveKey(1, 1)});
	this->TestFalse(TEXT("Nonfinite curve key is rejected"), FMeshParticleizerProgressCurve::Compile(Asset.Get(), Curve, Error));
	this->TestFalse(TEXT("Unloaded curve never blocks to load"), FMeshParticleizerProgressCurve::Compile(TSoftObjectPtr<UCurveFloat>(FSoftObjectPath(TEXT("/MeshParticleizer/MissingProgressCurve.MissingProgressCurve"))), Curve, Error));
	this->TestTrue(TEXT("No curve keeps linear behavior"), FMeshParticleizerProgressCurve::Compile(nullptr, Curve, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerMotionHistoryTest, "MeshParticleizer.Core.MotionHistory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerMotionHistoryTest::RunTest(const FString& Parameters)
{
	auto History = FMeshParticleizerTransformHistory{};
	History.Capture(FTransform(FVector::ZeroVector), 0, FIntVector::ZeroValue);
	this->TestFalse(TEXT("A single pose cannot supply velocity"), History.HasVelocity());
	History.Capture(FTransform(FVector(120, 0, 0)), 1, FIntVector::ZeroValue);
	this->TestTrue(TEXT("Two source updates supply velocity before playback"), History.HasVelocity());
	this->TestEqual(TEXT("Translation is measured in game seconds"),
		(History.Current.GetLocation() - History.Previous.GetLocation()) / (History.CurrentTime - History.PreviousTime), FVector(120, 0, 0));

	// 原点移動、同じ時刻の再呼出し、テレポートを通常の移動履歴へ混ぜない。
	History.Capture(FTransform(FVector(-760, 0, 0)), 2, FIntVector(1000, 0, 0));
	this->TestEqual(TEXT("Origin shift preserves translation velocity"), History.Current.GetLocation() - History.Previous.GetLocation(), FVector(120, 0, 0));
	History.Capture(FTransform(FVector(-760, 0, 0)), 2, FIntVector(1000, 0, 0));
	this->TestEqual(TEXT("Repeated time does not manufacture a new sample"), History.PreviousTime, 1.0);
	auto FrozenHistory = History;
	FrozenHistory.Rebase(FIntVector(2000, 0, 0));
	this->TestEqual(TEXT("Frozen origin adjustment preserves captured time"), FrozenHistory.CurrentTime, 2.0);
	this->TestEqual(TEXT("Frozen origin adjustment preserves previous time"), FrozenHistory.PreviousTime, 1.0);
	this->TestEqual(TEXT("Frozen origin adjustment preserves pre-freeze velocity"),
		FrozenHistory.Current.GetLocation() - FrozenHistory.Previous.GetLocation(), FVector(120, 0, 0));
	this->TestEqual(TEXT("Frozen position follows world origin"), FrozenHistory.Current.GetLocation(), FVector(-1760, 0, 0));
	History.Reset();
	History.Capture(FTransform(FVector(100000, 0, 0)), 3, FIntVector(1000, 0, 0));
	this->TestFalse(TEXT("Teleport requires a new valid pair"), History.HasVelocity());
	this->TestTrue(TEXT("Teleport suppresses stale fallback velocity"), History.bDiscontinuous);
	History.Capture(FTransform(FVector(100120, 0, 0)), 4, FIntVector(1000, 0, 0));
	this->TestTrue(TEXT("Normal history resumes after teleport"), History.HasVelocity());
	this->TestFalse(TEXT("Discontinuity clears after a valid pair"), History.bDiscontinuous);

	// 回転速度は表面点ごとに異なる。Actorの平行移動をもう一度加えない。
	History = FMeshParticleizerTransformHistory{};
	History.Capture(FTransform::Identity, 0, FIntVector::ZeroValue);
	History.Capture(FTransform(FRotator(0, 1, 0)), 1.0 / 60, FIntVector::ZeroValue);
	const auto Tip = FVector(100, 0, 0);
	const auto Velocity = (History.Current.TransformPosition(Tip) - History.Previous.TransformPosition(Tip)) * 60;
	this->TestTrue(TEXT("Rotating tip follows tangential speed"), FMath::Abs(Velocity.Size() - FMath::DegreesToRadians(60.0) * 100) < 0.1);
	this->TestEqual(TEXT("Rotation center has no translation velocity"), History.Current.GetLocation() - History.Previous.GetLocation(), FVector::ZeroVector);
	History.Capture(FTransform::Identity, std::numeric_limits<double>::quiet_NaN(), FIntVector::ZeroValue);
	this->TestFalse(TEXT("Non-finite time invalidates history"), History.HasVelocity());

	// 更新間引きで同じ姿勢が繰り返されても、実際の骨更新間隔を失わない。
	auto Bones = FMeshParticleizerBoneHistory{};
	auto Pose = TArray<FTransform>{FTransform(FVector(20, 0, 0)), FTransform::Identity};
	const auto Bind = TArray<FMatrix44f>{FMatrix44f(FTransform(FVector(-10, 0, 0)).ToMatrixWithScale())};
	this->TestTrue(TEXT("First bone pose accepted"), Bones.Capture(Pose, Bind, 1, 1));
	this->TestEqual(TEXT("Virtual bones do not add skinning rows"), Bones.Current.Num(), 3);
	this->TestEqual(TEXT("Inverse reference pose is applied"), Bones.Current[0].W, 10.0f);
	Pose[0].SetTranslation(FVector(30, 0, 0));
	Bones.Capture(Pose, Bind, 2, 2);
	Bones.Capture(Pose, Bind, 2, 3);
	this->TestTrue(TEXT("Two actual bone poses retain velocity"), Bones.HasVelocity());
	this->TestEqual(TEXT("Skipped update does not advance pose time"), Bones.CurrentTime, 2.0);
	this->TestEqual(TEXT("Previous bone pose is retained"), Bones.Previous[0].W, 10.0f);
	Bones.Reset();
	Bones.Capture(Pose, Bind, 2, 3);
	this->TestFalse(TEXT("Teleport invalidates bone velocity"), Bones.HasVelocity());

	// Leader Poseの行列は既に部品の逆参照姿勢を含む。二重に適用しない。
	Bones.Reset();
	auto Mapped = TArray<FMatrix44f>{FMatrix44f(FTransform(FVector(40, 0, 0)).ToMatrixWithScale())};
	this->TestTrue(TEXT("Mapped skinning matrices accepted"), Bones.CaptureMatrices(Mapped, 7, 4));
	this->TestEqual(TEXT("Mapped matrix translation is preserved"), Bones.Current[0].W, 40.0f);
	Mapped[0].M[3][0] = 60;
	Bones.CaptureMatrices(Mapped, 8, 5);
	Bones.CaptureMatrices(Mapped, 8, 6);
	this->TestTrue(TEXT("Mapped poses retain actual update velocity"), Bones.HasVelocity());
	this->TestEqual(TEXT("Repeated leader revision preserves time"), Bones.CurrentTime, 5.0);
	this->TestEqual(TEXT("Previous mapped pose is retained"), Bones.Previous[0].W, 40.0f);
	Mapped[0].M[0][0] = std::numeric_limits<float>::quiet_NaN();
	this->TestFalse(TEXT("Invalid mapped matrix is rejected"), Bones.CaptureMatrices(Mapped, 9, 7));
	this->TestFalse(TEXT("Invalid mapped matrix clears velocity"), Bones.HasVelocity());

	auto Settings = FMeshParticleizeOptions{};
	auto Error = FString{};
	Settings.Restitution = 1.1f;
	this->TestFalse(TEXT("Energy-increasing bounce is rejected"), Settings.Validate(Error));
	Settings.Restitution = 0;
	Settings.NormalSpeed = Settings.ScatterSpeed = Settings.RandomSpeed = Settings.NoiseAcceleration = Settings.Drag = 0;
	Settings.Gravity = Settings.Wind = FVector::ZeroVector;
	this->TestTrue(TEXT("Inertia-only configuration is supported"), Settings.Validate(Error));

	// 漂いの既定挙動を保ち、明示した弱い重力だけを足す。落下モードの重力は変えない。
	Settings.Gravity = FVector(0, 0, -980);
	this->TestEqual(TEXT("Legacy drift has no gravity"), Settings.GetEffectiveGravity(), FVector::ZeroVector);
	Settings.DriftGravityScale = 0.1f;
	this->TestTrue(TEXT("Weak drift gravity is valid"), Settings.Validate(Error));
	this->TestTrue(TEXT("Drift scales gravity"), Settings.GetEffectiveGravity().Equals(FVector(0, 0, -98), 0.0001));
	Settings.Motion = EMeshParticleizerMotion::Fall;
	this->TestEqual(TEXT("Fall keeps full gravity"), Settings.GetEffectiveGravity(), Settings.Gravity);
	Settings.DriftGravityScale = -1;
	this->TestFalse(TEXT("Negative drift scale is rejected"), Settings.Validate(Error));
	Settings.DriftGravityScale = std::numeric_limits<float>::infinity();
	this->TestFalse(TEXT("Infinite drift scale is rejected"), Settings.Validate(Error));
	Settings.Motion = EMeshParticleizerMotion::Drift;
	Settings.DriftGravityScale = std::numeric_limits<float>::max();
	this->TestFalse(TEXT("GPU gravity overflow is rejected"), Settings.Validate(Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerBudgetTest, "MeshParticleizer.Core.Budget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerBudgetTest::RunTest(const FString& Parameters)
{
	// 三角形の面積倍率は法線の向きに依存する。Boundsや体積による近似を検出する。
	auto AreaData = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	AreaData->SurfaceArea = 6;
	AreaData->SurfaceAreaVectors = {FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0), FVector(0, 0, 1), FVector(0, 0, -1)};
	auto ScaledArea = 0.0;
	auto AreaError = FString{};
	this->TestTrue(TEXT("Cube nonuniform area resolves"), AreaData->GetScaledSurfaceArea(FVector(2, 3, 4), ScaledArea, AreaError));
	this->TestEqual(TEXT("Cube scaled area sums xy, xz and yz faces"), ScaledArea, 52.0);
	this->TestTrue(TEXT("Mirrored cube area resolves"), AreaData->GetScaledSurfaceArea(FVector(-2, 3, -4), ScaledArea, AreaError));
	this->TestEqual(TEXT("Reflection preserves positive area"), ScaledArea, 52.0);
	AreaData->SurfaceAreaVectors = {FVector(1, 1, 1)};
	AreaData->SurfaceArea = FMath::Sqrt(3.0f);
	this->TestTrue(TEXT("Oblique surface area resolves"), AreaData->GetScaledSurfaceArea(FVector(2, 3, 4), ScaledArea, AreaError));
	this->TestTrue(TEXT("Oblique surface uses area vector length"), FMath::IsNearlyEqual(ScaledArea, FMath::Sqrt(244.0), 0.000001));
	AreaData->SurfaceAreaVectors.Reset();
	AreaData->SurfaceArea = 6;
	this->TestTrue(TEXT("Legacy uniform scale stays compatible"), AreaData->GetScaledSurfaceArea(FVector(-2, 2, 2), ScaledArea, AreaError));
	this->TestEqual(TEXT("Uniform area uses the squared scale"), ScaledArea, 24.0);
	this->TestFalse(TEXT("Legacy nonuniform scale requires reprepare"), AreaData->GetScaledSurfaceArea(FVector(2, 3, 4), ScaledArea, AreaError));
	this->TestEqual(TEXT("Rejected area does not retain the prior value"), ScaledArea, 0.0);
	this->TestTrue(TEXT("Missing area data gives an actionable diagnostic"), AreaError.Contains(TEXT("Prepare this source again")));
	this->TestFalse(TEXT("Zero scale is rejected"), AreaData->GetScaledSurfaceArea(FVector(0, 1, 1), ScaledArea, AreaError));
	this->TestFalse(TEXT("Nonfinite scale is rejected"), AreaData->GetScaledSurfaceArea(FVector(std::numeric_limits<double>::infinity()), ScaledArea, AreaError));
	this->TestFalse(TEXT("Overflowed uniform area is rejected"), AreaData->GetScaledSurfaceArea(FVector(1e200), ScaledArea, AreaError));
	AreaData->SurfaceAreaVectors = {FVector(0, 0, 5)};
	this->TestFalse(TEXT("Partial triangle area data is rejected"), AreaData->GetScaledSurfaceArea(FVector(2, 3, 4), ScaledArea, AreaError));
	AreaData->SurfaceAreaVectors = {FVector(std::numeric_limits<double>::quiet_NaN(), 0, 0)};
	this->TestFalse(TEXT("Nonfinite triangle area is rejected"), AreaData->GetScaledSurfaceArea(FVector(2, 3, 4), ScaledArea, AreaError));

	// 品質切替は粒子・ソース・容量をまとめて検証し、拒否時に既存予約を変えない。
	auto QualityBudget = FMeshParticleizerBudget{};
	auto QualityError = FString{};
	this->TestEqual(TEXT("Fast resting limit"), FMeshParticleizerQualityLimits::Find(EMeshParticleizerQuality::Fast)->RestingParticleLimit, 10000);
	this->TestEqual(TEXT("Balanced resting limit"), FMeshParticleizerQualityLimits::Find(EMeshParticleizerQuality::Balanced)->RestingParticleLimit, 50000);
	this->TestEqual(TEXT("High resting limit"), FMeshParticleizerQualityLimits::Find(EMeshParticleizerQuality::High)->RestingParticleLimit, 250000);
	this->TestTrue(TEXT("High quality applies"), QualityBudget.SetQuality(EMeshParticleizerQuality::High, QualityError));
	this->TestEqual(TEXT("High particle limit"), QualityBudget.ParticleLimit, 1000000);
	this->TestEqual(TEXT("High target limit"), QualityBudget.TargetParticleLimit, 250000);
	this->TestEqual(TEXT("High source limit"), QualityBudget.SourceLimit, 64);
	this->TestEqual(TEXT("High simulation memory limit"), QualityBudget.SimulationMemoryLimitBytes, 768ll * 1024 * 1024);
	auto QualityGranted = 0;
	this->TestTrue(TEXT("Per-target quality caps even a larger request"), QualityBudget.Reserve(1000000, QualityGranted));
	this->TestEqual(TEXT("High request is capped at one target"), QualityGranted, 250000);
	this->TestFalse(TEXT("Cannot lower quality below reserved particles"), QualityBudget.SetQuality(EMeshParticleizerQuality::Balanced, QualityError));
	this->TestEqual(TEXT("Rejected quality preserves source limit"), QualityBudget.SourceLimit, 64);
	this->TestEqual(TEXT("Rejected quality preserves particle reservations"), QualityBudget.ReservedParticles, 250000);
	QualityBudget.Release(QualityGranted);
	this->TestTrue(TEXT("Source-only workload reserves slots"), QualityBudget.Reserve(0, QualityGranted, 17));
	this->TestFalse(TEXT("Source-only reservations prevent unsafe downgrade"), QualityBudget.SetQuality(EMeshParticleizerQuality::Fast, QualityError));
	QualityBudget.Release(0, 17);
	this->TestTrue(TEXT("Memory workload reserves capacity"), QualityBudget.Reserve(1, QualityGranted, 1, 129ll * 1024 * 1024));
	this->TestFalse(TEXT("GPU bytes prevent unsafe downgrade"), QualityBudget.SetQuality(EMeshParticleizerQuality::Fast, QualityError));
	this->TestEqual(TEXT("Rejected quality preserves byte reservations"), QualityBudget.ReservedSimulationBytes, 129ll * 1024 * 1024);
	QualityBudget.Release(QualityGranted, 1, 129ll * 1024 * 1024);
	this->TestTrue(TEXT("Released workload can switch to Fast"), QualityBudget.SetQuality(EMeshParticleizerQuality::Fast, QualityError));
	this->TestEqual(TEXT("Fast particle limit"), QualityBudget.ParticleLimit, 50000);
	this->TestEqual(TEXT("Fast target limit"), QualityBudget.TargetParticleLimit, 10000);
	this->TestEqual(TEXT("Fast source limit"), QualityBudget.SourceLimit, 16);
	this->TestEqual(TEXT("Fast simulation memory limit"), QualityBudget.SimulationMemoryLimitBytes, 128ll * 1024 * 1024);
	this->TestFalse(TEXT("Unknown quality is rejected"), QualityBudget.SetQuality(static_cast<EMeshParticleizerQuality>(255), QualityError));
	this->TestEqual(TEXT("Unknown quality leaves the selected tier intact"), QualityBudget.Quality, EMeshParticleizerQuality::Fast);
	this->TestTrue(TEXT("Balanced quality restores defaults"), QualityBudget.SetQuality(EMeshParticleizerQuality::Balanced, QualityError));
	this->TestEqual(TEXT("Balanced particle limit"), QualityBudget.ParticleLimit, 200000);
	this->TestEqual(TEXT("Balanced target limit"), QualityBudget.TargetParticleLimit, 100000);
	this->TestEqual(TEXT("Balanced source limit"), QualityBudget.SourceLimit, 32);
	this->TestEqual(TEXT("Balanced simulation memory limit"), QualityBudget.SimulationMemoryLimitBytes, 256ll * 1024 * 1024);

	auto Budget = FMeshParticleizerBudget{100, 2};
	auto First = 0;
	auto Second = 0;
	auto Rejected = 0;
	this->TestTrue(TEXT("First source accepted"), Budget.Reserve(80, First));
	this->TestTrue(TEXT("Second source reduced"), Budget.Reserve(80, Second));
	this->TestEqual(TEXT("Reserved candidates count toward cap"), Second, 20);
	this->TestFalse(TEXT("Source cap enforced"), Budget.Reserve(1, Rejected));
	Budget.Release(First);
	this->TestEqual(TEXT("Only released allocation becomes available"), Budget.ReservedParticles, 20);
	this->TestTrue(TEXT("Released source slot reused"), Budget.Reserve(80, First));
	this->TestEqual(TEXT("Cap preserved on reuse"), Budget.ReservedParticles, 100);

	auto GroupBudget = FMeshParticleizerBudget{100, 3};
	this->TestTrue(TEXT("Group reserves all source slots together"), GroupBudget.Reserve(90, First, 2));
	this->TestFalse(TEXT("Group source limit is atomic"), GroupBudget.Reserve(10, Rejected, 2));
	this->TestEqual(TEXT("Rejected group retains source count"), GroupBudget.ActiveSources, 2);
	this->TestEqual(TEXT("Rejected group retains particle count"), GroupBudget.ReservedParticles, 90);
	GroupBudget.Release(First, 2);
	this->TestEqual(TEXT("Group releases all source slots"), GroupBudget.ActiveSources, 0);

	// 容量不足は粒子・ソースの予約を部分的に変更しない。
	GroupBudget.SimulationMemoryLimitBytes = 4096;
	this->TestTrue(TEXT("GPU memory reservation fits"), GroupBudget.Reserve(10, First, 1, 3000));
	this->TestFalse(TEXT("GPU memory limit rejects atomically"), GroupBudget.Reserve(10, Rejected, 1, 1097));
	this->TestEqual(TEXT("Rejected memory allocation preserves bytes"), GroupBudget.ReservedSimulationBytes, int64{3000});
	this->TestEqual(TEXT("Rejected memory allocation preserves particles"), GroupBudget.ReservedParticles, 10);
	GroupBudget.Release(First, 1, 3000);
	this->TestEqual(TEXT("Released memory becomes available"), GroupBudget.ReservedSimulationBytes, int64{0});

	// 消滅した枠の返却は、まだ使用するソース数とバッファ容量を減らさない。
	this->TestTrue(TEXT("Reservation before partial retirement"), GroupBudget.Reserve(100, First, 1, 3000));
	GroupBudget.Release(60, 0, 0);
	this->TestEqual(TEXT("Surviving candidates remain reserved"), GroupBudget.ReservedParticles, 40);
	this->TestEqual(TEXT("Partial retirement retains source"), GroupBudget.ActiveSources, 1);
	this->TestEqual(TEXT("Partial retirement retains GPU bytes"), GroupBudget.ReservedSimulationBytes, int64{3000});
	this->TestTrue(TEXT("A new operation reuses only retired particle capacity"), GroupBudget.Reserve(80, Second, 1, 1000));
	this->TestEqual(TEXT("New operation is limited to the released slots"), Second, 60);
	GroupBudget.Release(40, 1, 3000);
	this->TestEqual(TEXT("Original completion keeps new reservation"), GroupBudget.ReservedParticles, 60);
	this->TestEqual(TEXT("Original completion keeps new GPU bytes"), GroupBudget.ReservedSimulationBytes, int64{1000});
	GroupBudget.Release(Second, 1, 1000);

	// 64粒子境界は余剰1要素で次の群へ進む。Float/Int/Halfは別々に丸める。
	auto Memory = FMeshParticleizerGpuMemory{};
	Memory.FloatComponents = 17;
	Memory.IntComponents = 1;
	Memory.HalfComponents = 3;
	Memory.ArrayBytes = 48;
	this->TestEqual(TEXT("Zero particles do not allocate GPU buffers"), Memory.Estimate(0), int64{0});
	this->TestEqual(TEXT("Three surfaces and budget records include their capacity"), Memory.Estimate(63), int64{49152 + 48 + 63 * 16 + 65536});
	this->TestEqual(TEXT("Extra GPU element advances the thread group"), Memory.Estimate(64), int64{61440 + 48 + 64 * 16 + 65536});
	Memory.bPersistentIDs = true;
	this->TestEqual(TEXT("Persistent IDs include three maps and one free list"), Memory.Estimate(64), int64{61440 + 48 + 16384 + 64 * 16 + 65536});

	// 容量を追加する描画設定は開始前に拒否し、計上外のGPU確保を防ぐ。
	auto* System = LoadObject<UNiagaraSystem>(nullptr, TEXT("/MeshParticleizer/Core/NS_MeshParticleizer.NS_MeshParticleizer"));
	auto Error = FString{};
	auto Model = FMeshParticleizerGpuMemory{};
	if (this->TestTrue(TEXT("Generated Sprite memory configuration is supported"),
						   FMeshParticleizerGpuMemory::Build(System, 0, 0, 1, Model, Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(System->GetEmitterHandles()[0].GetInstance().GetEmitterData()->GetRenderers()[0]);
	if (this->TestNotNull(TEXT("Generated renderer is a Sprite"), Sprite) == false)
	{
		return false;
	}
	const auto SavedDistanceCulling = Sprite->bEnableCameraDistanceCulling;
	Sprite->bEnableCameraDistanceCulling = true;
	this->TestFalse(TEXT("Additional GPU culling cannot bypass accounting"), FMeshParticleizerGpuMemory::Build(System, 0, 0, 1, Model, Error));
	Sprite->bEnableCameraDistanceCulling = SavedDistanceCulling;
	const auto SavedSort = Sprite->SortMode;
	const auto SavedSortOnlyTranslucent = Sprite->bSortOnlyWhenTranslucent;
	Sprite->SortMode = ENiagaraSortMode::ViewDistance;
	Sprite->bSortOnlyWhenTranslucent = false;
	this->TestFalse(TEXT("Additional GPU sorting cannot bypass accounting"), FMeshParticleizerGpuMemory::Build(System, 0, 0, 1, Model, Error));
	Sprite->SortMode = SavedSort;
	Sprite->bSortOnlyWhenTranslucent = SavedSortOnlyTranslucent;
	this->TestTrue(TEXT("Restored renderer remains supported"), FMeshParticleizerGpuMemory::Build(System, 0, 0, 1, Model, Error));
	auto MoreSlots = FMeshParticleizerGpuMemory{};
	this->TestTrue(TEXT("Surface slots participate in memory accounting"), FMeshParticleizerGpuMemory::Build(System, 0, 0, 7, MoreSlots, Error));
	this->TestEqual(TEXT("Six extra surface tints reserve six float4 values"), MoreSlots.ArrayBytes - Model.ArrayBytes, int64{6 * 16});
	const auto BakedBytes = Model.Estimate(2000);
	Model.bSurfaceColorCache = true;
	this->TestEqual(TEXT("Texture color reserves one float4 per candidate"), Model.Estimate(2000) - BakedBytes, int64{2000 * 16});
	Model.bSourceEmissionCache = true;
	this->TestEqual(TEXT("Dynamic HDR emission reserves a separate float4 per candidate"), Model.Estimate(2000) - BakedBytes, int64{2000 * 32});
	this->TestEqual(TEXT("Zero candidates do not allocate a color cache"), Model.Estimate(0), int64{0});
	this->TestFalse(TEXT("Missing material slots cannot bypass accounting"), FMeshParticleizerGpuMemory::Build(System, 0, 0, 0, MoreSlots, Error));

	// 小メッシュの通常profileを受理し、影や複数LODへ変更した場合は未計上のまま通さない。
	auto* MeshSystem = LoadObject<UNiagaraSystem>(nullptr, TEXT("/MeshParticleizer/Mesh/Grain/NS_MeshParticleizer.NS_MeshParticleizer"));
	auto* ParticleMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/MeshParticleizer/Mesh/SM_Grain.SM_Grain"));
	if (this->TestNotNull(TEXT("Generated particle mesh exists"), ParticleMesh) == false)
	{
		return false;
	}
	FStaticMeshCompilingManager::Get().FinishCompilation({ParticleMesh});
	auto MeshModel = FMeshParticleizerGpuMemory{};
	if (this->TestTrue(TEXT("Generated mesh memory configuration is supported"),
						   FMeshParticleizerGpuMemory::Build(MeshSystem, 0, 0, 1, MeshModel, Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(MeshSystem->GetEmitterHandles()[0].GetInstance().GetEmitterData()->GetRenderers()[0]);
	if (this->TestNotNull(TEXT("Generated renderer is a mesh"), MeshRenderer) == false)
	{
		return false;
	}
	this->TestEqual(TEXT("Mesh sections feed prospective scene and draw demands"), MeshModel.MeshSections, 1);
	MeshRenderer->bCastShadows = true;
	this->TestFalse(TEXT("Unaccounted shadow pass rejects"), FMeshParticleizerGpuMemory::Build(MeshSystem, 0, 0, 1, MeshModel, Error));
	MeshRenderer->bCastShadows = false;
	MeshRenderer->Meshes[0].LODMode = ENiagaraMeshLODMode::PerParticle;
	this->TestFalse(TEXT("Unaccounted per-particle LOD draws reject"), FMeshParticleizerGpuMemory::Build(MeshSystem, 0, 0, 1, MeshModel, Error));
	MeshRenderer->Meshes[0].LODMode = ENiagaraMeshLODMode::LODLevel;
	this->TestTrue(TEXT("Restored mesh remains supported"), FMeshParticleizerGpuMemory::Build(MeshSystem, 0, 0, 1, MeshModel, Error));

	// 遠距離の追加Rendererを重複Simulationとして数えず、未計上のsort設定は拒否する。
	auto* FarSystem = LoadObject<UNiagaraSystem>(nullptr, TEXT("/MeshParticleizer/Mesh/Distance/Grain/NS_MeshParticleizer.NS_MeshParticleizer"));
	auto FarModel = FMeshParticleizerGpuMemory{};
	if (this->TestTrue(TEXT("Near mesh and far Sprite share the supported GPU model"),
						   FMeshParticleizerGpuMemory::Build(FarSystem, 0, 0, 1, FarModel, Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("Distance switch keeps one mesh scene demand"), FarModel.MeshSections, 1);
	this->TestEqual(TEXT("Sprite size retains two additional float components"), FarModel.FloatComponents, MeshModel.FloatComponents + 2);
	this->TestEqual(TEXT("Integer particle storage stays shared"), FarModel.IntComponents, MeshModel.IntComponents);
	this->TestTrue(TEXT("Sprite size capacity is included without duplicating the simulation"),
		FarModel.Estimate(6000) > MeshModel.Estimate(6000) && FarModel.Estimate(6000) < 2 * MeshModel.Estimate(6000));
	auto* FarEmitter = FarSystem->GetEmitterHandles()[0].GetInstance().GetEmitterData();
	auto* NearRenderer = Cast<UNiagaraMeshRendererProperties>(FarEmitter->GetRenderers()[0]);
	auto* FarRenderer = Cast<UNiagaraSpriteRendererProperties>(FarEmitter->GetRenderers()[1]);
	if (this->TestNotNull(TEXT("Near renderer"), NearRenderer) == false || this->TestNotNull(TEXT("Far renderer"), FarRenderer) == false)
	{
		return false;
	}
	const auto Distance = NearRenderer->MaxCameraDistance;
	NearRenderer->MaxCameraDistance += 1.f;
	this->TestFalse(TEXT("Mismatched near/far boundaries reject"), FMeshParticleizerGpuMemory::Build(FarSystem, 0, 0, 1, FarModel, Error));
	NearRenderer->MaxCameraDistance = Distance;
	NearRenderer->Meshes[0].PivotOffsetSpace = ENiagaraMeshPivotOffsetSpace::World;
	this->TestFalse(TEXT("World-space pivot cannot misalign distance boundaries"), FMeshParticleizerGpuMemory::Build(FarSystem, 0, 0, 1, FarModel, Error));
	NearRenderer->Meshes[0].PivotOffsetSpace = ENiagaraMeshPivotOffsetSpace::Mesh;
	FarRenderer->bEnableCameraDistanceCulling = true;
	this->TestFalse(TEXT("Far Sprite cannot allocate an unbudgeted sort buffer"), FMeshParticleizerGpuMemory::Build(FarSystem, 0, 0, 1, FarModel, Error));
	FarRenderer->bEnableCameraDistanceCulling = false;
	const auto FarMaterial = FarRenderer->Material;
	FarRenderer->Material = Sprite->Material;
	this->TestFalse(TEXT("Missing far material contract rejects"), FMeshParticleizerGpuMemory::Build(FarSystem, 0, 0, 1, FarModel, Error));
	FarRenderer->Material = FarMaterial;
	this->TestTrue(TEXT("Restored distance switch remains supported"), FMeshParticleizerGpuMemory::Build(FarSystem, 0, 0, 1, FarModel, Error));

	auto MeshOptions = FMeshParticleizeOptions{};
	MeshOptions.MeshAngularVelocity.X = std::numeric_limits<double>::infinity();
	this->TestFalse(TEXT("Nonfinite mesh spin rejects"), MeshOptions.Validate(Error));

	// 共通容量は最初のGPU利用で一度だけ予約し、個別バッファを返しても保持する。
	auto SharedSettings = FMeshParticleizerSharedGpuSettings{};
	auto Shared = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("No GPU simulations require no shared allocation"), Shared.Grow(0, SharedSettings, Error));
	this->TestEqual(TEXT("Dissolve-only shared capacity is zero"), Shared.GetReservationBytes(), int64{0});
	this->TestTrue(TEXT("One Sprite simulation has a shared estimate"), Shared.Grow(1, SharedSettings, Error));
	const auto SingleSharedBytes = Shared.GetReservationBytes();
	this->TestEqual(TEXT("Default counter allocation follows Niagara's minimum"), Shared.CounterCapacity, int64{2048});
	this->TestTrue(TEXT("Shared estimate covers counters, multi-view, readback and draw args"), SingleSharedBytes >= 3 * 8192 + 5120);
	this->TestTrue(TEXT("More sources share one common pool"), Shared.Grow(6, SharedSettings, Error));
	const auto SixSharedBytes = Shared.GetReservationBytes();
	this->TestTrue(TEXT("Six sources do not multiply the common minimum six times"), SixSharedBytes < SingleSharedBytes * 2);
	Shared.Grow(1, SharedSettings, Error);
	this->TestEqual(TEXT("Smaller replay retains the shared high-water mark"), Shared.GetReservationBytes(), SixSharedBytes);
	SharedSettings.MinimumCounters = 4096;
	SharedSettings.MinimumIndirectArgs = 1024;
	this->TestTrue(TEXT("Larger engine allocation settings increase the reservation"), Shared.Grow(1, SharedSettings, Error));
	this->TestTrue(TEXT("Changed minima are included"), Shared.GetReservationBytes() >= 3 * 4096 * 4 + 1024 * 20);
	const auto BeforeInvalidShared = Shared.GetReservationBytes();
	SharedSettings.CounterSlack = std::numeric_limits<float>::quiet_NaN();
	this->TestFalse(TEXT("Invalid engine settings reject before committing shared capacity"), Shared.Grow(1, SharedSettings, Error));
	this->TestEqual(TEXT("Rejected shared estimate preserves the prior state"), Shared.GetReservationBytes(), BeforeInvalidShared);
	SharedSettings = FMeshParticleizerSharedGpuSettings{};
	SharedSettings.IndirectGrowth = std::numeric_limits<float>::max();
	this->TestFalse(TEXT("Buffer capacity overflow rejects"), Shared.Grow(1, SharedSettings, Error));

	// GDFの入力容量は粒子数と独立し、複数ソース間で一度だけ計上する。
	auto GdfShared = FMeshParticleizerSharedGpuMemory{};
	GdfShared.Grow(1, FMeshParticleizerSharedGpuSettings{}, Error);
	GdfShared.DistanceFieldBytes = 70ll * 1024 * 1024 * 2;
	auto GdfBudget = FMeshParticleizerBudget{};
	auto GdfGranted = 0;
	GdfBudget.SimulationMemoryLimitBytes = GdfShared.GetReservationBytes() + 100;
	this->TestFalse(TEXT("One byte over GDF and private capacity rejects atomically"), GdfBudget.Reserve(1, GdfGranted, 1, 101, GdfShared.GetReservationBytes()));
	this->TestEqual(TEXT("Rejected GDF start leaves no shared reservation"), GdfBudget.SharedGpuReservationBytes, int64{0});
	this->TestTrue(TEXT("GDF and private capacity fit at the boundary"), GdfBudget.Reserve(1, GdfGranted, 1, 100, GdfShared.GetReservationBytes()));
	this->TestTrue(TEXT("Another source reuses the same GDF reservation"), GdfBudget.Reserve(1, GdfGranted, 1, 0, GdfShared.GetReservationBytes()));
	GdfBudget.Release(2, 2, 100);
	this->TestEqual(TEXT("GDF remains reserved after source retirement"), GdfBudget.ReservedSimulationBytes, GdfShared.GetReservationBytes());
	this->TestFalse(TEXT("Quality cannot shrink below retained GDF capacity"), GdfBudget.SetQuality(EMeshParticleizerQuality::Fast, Error));

	// GPU Sceneは物理コミットと追加の描画要求で予約する。2GiBの仮想枠を容量へ加えない。
	auto Scene = FMeshParticleizerSceneGpuSnapshot{};
	Scene.Frame = 1;
	Scene.PrimitiveStride = 672;
	Scene.InstanceStride = 48;
	Scene.MeshPayloadStride = 3;
	Scene.VirtualBytes = 6ll * 1024 * 1024 * 1024;
	Scene.TileSizeLog2 = 12;
	Scene.CommitAlignment = 65536;
	auto SceneShared = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Observed GPU Scene accepts mesh instance demand"), SceneShared.GrowScene(Scene, 1, 50000, 150000, Error));
	this->TestEqual(TEXT("Tiled instance storage commits a 16 MiB chunk"), SceneShared.SceneInstanceBytes, int64{16 * 1024 * 1024});
	this->TestEqual(TEXT("Payload grows to a power of two in float4s"), SceneShared.ScenePayloadBytes, int64{4 * 1024 * 1024});
	this->TestEqual(TEXT("Primitive commit uses RHI pages"), SceneShared.ScenePrimitiveBytes, int64{196608});
	const auto SceneReservation = SceneShared.GetSceneReservationBytes();
	this->TestEqual(TEXT("Primitive uploads reserve pooled data/index sizes for static and view updates"), SceneShared.SceneUploadBytes, int64{4112});
	this->TestEqual(TEXT("Resident scene buffers and uploads include one retiring generation"), SceneReservation, int64{42344480});
	this->TestEqual(TEXT("Scene reservation reaches the shared total"), SceneShared.GetReservationBytes(), SceneReservation);
	this->TestTrue(TEXT("Later smaller demand is valid"), SceneShared.GrowScene(Scene, 0, 0, 0, Error));
	this->TestEqual(TEXT("Smaller demand retains the scene high water mark"), SceneShared.GetSceneReservationBytes(), SceneReservation);
	auto UploadViews = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Two views retain simultaneous primitive upload pairs"), UploadViews.GrowScene(Scene, 1, 1, 3, Error, 2));
	this->TestEqual(TEXT("Static plus two views cover power-of-two upload pool sizes"), UploadViews.SceneUploadBytes, int64{6168});
	this->TestFalse(TEXT("Unbounded view count rejects upload overflow"), UploadViews.GrowScene(Scene, 1, 1, 3, Error, MAX_int32));
	this->TestEqual(TEXT("Rejected upload growth is atomic"), UploadViews.SceneUploadBytes, int64{6168});
	Scene.Frame = 0;
	this->TestFalse(TEXT("Unobserved scene cannot reserve zero bytes"), SceneShared.GrowScene(Scene, 1, 1, 3, Error));
	Scene.Frame = 1;
	this->TestFalse(TEXT("Negative instance demand rejects"), SceneShared.GrowScene(Scene, 1, -1, 0, Error));
	this->TestFalse(TEXT("Oversized instance request rejects"), SceneShared.GrowScene(Scene, 1, MAX_int32, 0, Error));
	this->TestEqual(TEXT("Rejected scene demand does not change reservation"), SceneShared.GetSceneReservationBytes(), SceneReservation);
	Scene.RetainedUploadBytes = MAX_int64;
	this->TestFalse(TEXT("Upload overflow rejects before addition"), SceneShared.GrowScene(Scene, 1, 1, 3, Error));
	Scene.RetainedUploadBytes = 8192;
	this->TestTrue(TEXT("Retained upload pools and new primitive upload coexist"), SceneShared.GrowScene(Scene, 1, 1, 3, Error));
	this->TestEqual(TEXT("Existing upload bytes are not replaced by new demand"), SceneShared.SceneUploadBytes, int64{8192 + 4112});
	Scene.RetainedUploadBytes = 0;
	Scene.TileSizeLog2 = -1;
	Scene.CommitAlignment = 16;
	auto Untiled = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Non-tiled scene follows power-of-two allocation"), Untiled.GrowScene(Scene, 1, 50000, 150000, Error));
	this->TestEqual(TEXT("Non-tiled instance capacity uses the current transform stride"), Untiled.SceneInstanceBytes, int64{65536 * 48});
	Scene.InstanceBytes = 32ll * 1024 * 1024;
	this->TestTrue(TEXT("Existing world allocation is included"), Untiled.GrowScene(Scene, 1, 1, 3, Error));
	this->TestEqual(TEXT("Observed world high water mark is not lost"), Untiled.SceneInstanceBytes, Scene.InstanceBytes);

	auto SceneBudget = FMeshParticleizerBudget{};
	SceneBudget.SimulationMemoryLimitBytes = SceneReservation + 100;
	auto SceneGranted = 0;
	this->TestFalse(TEXT("Scene and private bytes reject atomically over budget"), SceneBudget.Reserve(1, SceneGranted, 1, 101, SceneReservation));
	this->TestEqual(TEXT("Rejected scene allocation keeps budget empty"), SceneBudget.ReservedSimulationBytes, int64{0});
	this->TestTrue(TEXT("Scene reservation fits exactly with private bytes"), SceneBudget.Reserve(1, SceneGranted, 1, 100, SceneReservation));
	this->TestTrue(TEXT("Second source shares the same scene capacity"), SceneBudget.Reserve(1, SceneGranted, 1, 0, SceneReservation));
	SceneBudget.Release(2, 2, 100);
	this->TestEqual(TEXT("Scene capacity survives source release"), SceneBudget.ReservedSimulationBytes, SceneReservation);

	// 小bufferはframe数倍しない。サイズ変更とpass・View減少でも、過去のpoolを失わない。
	auto Draws = FMeshParticleizerDrawGpuSnapshot{};
	Draws.Frame = 1;
	Draws.Views = 1;
	auto& DrawPass = Draws.Passes.AddDefaulted_GetRef();
	DrawPass.Name = TEXT("BasePass");
	DrawPass.bSubmitted = true;
	DrawPass.PooledBufferBytes = {4, 65537};
	DrawPass.GpuWrittenBytes = 4;
	auto DrawShared = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Draw pool accepts an observed footprint"), DrawShared.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("64 KiB classes retain 1 plus 1+2 plus 1 pages"), DrawShared.GetDrawReservationBytes(), int64{10 * 65536});
	this->TestTrue(TEXT("Stable draw footprint reuses its pool"), DrawShared.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Stable frames do not duplicate the pool"), DrawShared.GetDrawReservationBytes(), int64{10 * 65536});
	Draws.Passes[0].PooledBufferBytes = {65537, 4};
	this->TestTrue(TEXT("Moving pressure between buffers keeps both peaks"), DrawShared.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Per-buffer peaks survive lower total input"), DrawShared.GetDrawReservationBytes(), int64{14 * 65536});
	const auto SecondView = Draws.Passes[0];
	Draws.Passes.Add(SecondView);
	Draws.Views = 2;
	this->TestTrue(TEXT("Two views reserve separate unmerged inputs"), DrawShared.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Added view includes merged and split pool layouts"), DrawShared.GetDrawReservationBytes(), int64{48 * 65536});
	const auto DrawReservation = DrawShared.GetDrawReservationBytes();
	Draws.Passes.Empty();
	this->TestTrue(TEXT("An empty later frame keeps retained buffers"), DrawShared.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Pass removal does not release pool reservation"), DrawShared.GetReservationBytes(), DrawReservation);
	Draws.Frame = 0;
	this->TestFalse(TEXT("Unobserved draw pool is not free"), DrawShared.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Rejected draw snapshot keeps reservation"), DrawShared.GetReservationBytes(), DrawReservation);
	Draws.Frame = 2;
	auto& LargePass = Draws.Passes.AddDefaulted_GetRef();
	LargePass.Name = TEXT("LargePass");
	LargePass.bSubmitted = true;
	LargePass.PooledBufferBytes = {33ll * 65536};
	LargePass.GpuWrittenBytes = 4;
	auto LargeDraw = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Large buffers cap the retained size count"), LargeDraw.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("32 retained sizes cover pages 2 through 33"), LargeDraw.GetDrawReservationBytes(), int64{1122 * 65536});
	LargePass.PooledBufferBytes[0] = MAX_int64;
	this->TestFalse(TEXT("Oversized pool input rejects before rounding"), LargeDraw.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Rejected input cannot increase the high water mark"), LargeDraw.GetDrawReservationBytes(), int64{1122 * 65536});

	// family共通のView入力を各Viewで重複させず、GPU maskも同じpool寿命で計上する。
	auto ViewObserver = FMeshParticleizerSceneGpu{};
	auto ViewBuffers = FMeshParticleizerDrawGpuPass{};
	ViewBuffers.Name = TEXT("SharedViewData");
	ViewBuffers.bSubmitted = true;
	ViewBuffers.PooledBufferBytes = {1056, 66 * 1056, 4};
	ViewBuffers.GpuWrittenBytes = 4;
	ViewObserver.PublishDraws(1, true, {ViewBuffers}, 0);
	ViewObserver.Publish(Scene, 2);
	ViewObserver.PublishDraws(1, true, {});
	this->TestEqual(TEXT("Draw inputs alone do not publish an incomplete first frame"), ViewObserver.Read(&Draws).Frame, uint64{0});
	ViewObserver.PublishDraws(1, true, {});
	ViewObserver.Read(&Draws);
	this->TestEqual(TEXT("Family buffers do not increase primary view count"), Draws.Views, 2);
	this->TestEqual(TEXT("Family buffers are published once"), Draws.Passes.Num(), 1);
	auto ViewMemory = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Shared view buffers participate in draw capacity"), ViewMemory.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Shared views include rounded culling and GPU mask buffers"), ViewMemory.GetDrawReservationBytes(), int64{12 * 65536});

	// 次frameの途中で読む順序を再現し、ReadyからStartへの読み取りが後退しないことを確かめる。
	ViewObserver.PublishDraws(2, false, {}, 0);
	const auto PreviousScene = ViewObserver.Read(&Draws);
	this->TestEqual(TEXT("Pending draw update preserves the complete scene frame"), PreviousScene.Frame, uint64{1});
	this->TestEqual(TEXT("Pending draw update preserves matching draw frame"), Draws.Frame, PreviousScene.Frame);
	this->TestEqual(TEXT("Pending frame cannot expose a partial view count"), Draws.Views, 2);
	Scene.Frame = 2;
	ViewObserver.Publish(Scene, 1);
	this->TestEqual(TEXT("Family update alone preserves the previous complete frame"), ViewObserver.Read(&Draws).Frame, uint64{1});
	ViewObserver.PublishDraws(2, true, {});
	this->TestEqual(TEXT("Completed frame publishes scene and draws together"), ViewObserver.Read(&Draws).Frame, uint64{2});
	this->TestEqual(TEXT("Published draw frame matches scene"), Draws.Frame, uint64{2});
	this->TestFalse(TEXT("Unsupported family layout survives later view publication"), ViewMemory.GrowDraw(Draws, Error));
	this->TestEqual(TEXT("Unsupported family retains its previous reservation"), ViewMemory.GetDrawReservationBytes(), int64{12 * 65536});

	// 同じframeの追加familyでも途中の共通入力を見せず、主View数を合算する。
	ViewObserver.PublishDraws(2, true, {ViewBuffers}, 0);
	ViewObserver.Publish(Scene, 2);
	ViewObserver.PublishDraws(2, true, {});
	ViewObserver.Read(&Draws);
	this->TestEqual(TEXT("Incomplete second family retains the previous view count"), Draws.Views, 1);
	ViewObserver.PublishDraws(2, true, {});
	ViewObserver.Read(&Draws);
	this->TestEqual(TEXT("Completed families aggregate their primary views"), Draws.Views, 3);
	this->TestEqual(TEXT("Completed families preserve matching frame IDs"), Draws.Frame, uint64{2});

	// 2+2 pageから3+1、4 pageへの再統合で、同じサイズの複数bufferもpoolに残る。
	auto MergedDraws = FMeshParticleizerDrawGpuSnapshot{};
	MergedDraws.Frame = 1;
	MergedDraws.Views = 1;
	auto MergePass = FMeshParticleizerDrawGpuPass{};
	MergePass.Name = TEXT("DepthPass");
	MergePass.bSubmitted = true;
	MergePass.PooledBufferBytes = {2 * 65536};
	MergePass.GpuWrittenBytes = 4;
	MergedDraws.Passes.Add(MergePass);
	MergePass.Name = TEXT("BasePass");
	MergedDraws.Passes.Add(MergePass);
	auto MergedMemory = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Deferred merging is included before allocation"), MergedMemory.GrowDraw(MergedDraws, Error));
	this->TestEqual(TEXT("Merged role reserves 13 pages and output role 4 pages"), MergedMemory.GetDrawReservationBytes(), int64{34 * 65536});
	MergedDraws.Passes[0].PooledBufferBytes.Add(4);
	this->TestFalse(TEXT("Changed retained buffer layout rejects atomically"), MergedMemory.GrowDraw(MergedDraws, Error));
	this->TestEqual(TEXT("Rejected layout preserves capacity"), MergedMemory.GetDrawReservationBytes(), int64{34 * 65536});

	auto Prospective = FMeshParticleizerDrawGpuSnapshot{};
	Prospective.Frame = 1;
	Prospective.Views = 2;
	ViewBuffers.Views = 2;
	ViewBuffers.Instances = 21;
	Prospective.Passes.Add(ViewBuffers);
	const FName ProspectivePasses[] = {TEXT("DepthPass"), TEXT("BasePass"), TEXT("Velocity")};
	this->TestTrue(TEXT("Future mesh demand creates absent passes for both views"),
		Prospective.AddMeshDemand(50000, 1, ProspectivePasses, Error));
	this->TestEqual(TEXT("Three passes in two views plus one shared family"), Prospective.Passes.Num(), 7);
	this->TestEqual(TEXT("Dynamic scene instances are distinct per view"), Prospective.Passes[0].Instances, int64{100021});
	this->TestEqual(TEXT("Both primary views receive the larger deformation mask"), Prospective.Passes[0].GpuWrittenBytes, int64{25008});
	const auto FirstMeshInput = Prospective.Passes[1].GpuWrittenBytes;
	this->TestTrue(TEXT("Further demand adds into existing buffers"), Prospective.AddMeshDemand(50000, 1, ProspectivePasses, Error));
	this->TestEqual(TEXT("Further demand does not duplicate pass records"), Prospective.Passes.Num(), 7);
	this->TestEqual(TEXT("Instance IDs add to the earlier footprint"), Prospective.Passes[1].GpuWrittenBytes, FirstMeshInput + int64{200000});
	auto ProspectiveMemory = FMeshParticleizerSharedGpuMemory{};
	this->TestTrue(TEXT("Forecast feeds the shared pool reservation"), ProspectiveMemory.GrowDraw(Prospective, Error));
	this->TestFalse(TEXT("Negative mesh demand rejects"), Prospective.AddMeshDemand(-1, 1, ProspectivePasses, Error));
	this->TestFalse(TEXT("Unbounded scene instances reject"), Prospective.AddMeshDemand(MAX_int64, 1, ProspectivePasses, Error));
	this->TestEqual(TEXT("Rejected forecast retains previous data"), Prospective.Passes[1].GpuWrittenBytes, FirstMeshInput + int64{200000});
	Prospective.Passes[1].PooledBufferBytes[0] = MAX_uint32;
	const auto BeforeRejectedMask = Prospective.Passes[0].GpuWrittenBytes;
	this->TestFalse(TEXT("Draw input overflow rejects"), Prospective.AddMeshDemand(1, 1, ProspectivePasses, Error));
	this->TestEqual(TEXT("Rejected forecast does not partially grow the shared mask"), Prospective.Passes[0].GpuWrittenBytes, BeforeRejectedMask);

	auto SharedBudget = FMeshParticleizerBudget{};
	SharedBudget.SimulationMemoryLimitBytes = SingleSharedBytes + 100;
	auto SharedGranted = 0;
	this->TestFalse(TEXT("Shared overhead participates in the initial fit decision"), SharedBudget.Reserve(1, SharedGranted, 1, 101, SingleSharedBytes));
	this->TestEqual(TEXT("Rejected first allocation does not create a shared floor"), SharedBudget.SharedGpuReservationBytes, int64{0});
	this->TestEqual(TEXT("Rejected allocation does not consume sources"), SharedBudget.ActiveSources, 0);
	this->TestTrue(TEXT("Private and shared capacity can exactly fill the budget"), SharedBudget.Reserve(1, SharedGranted, 1, 100, SingleSharedBytes));
	this->TestTrue(TEXT("Reusing shared capacity does not charge it again"), SharedBudget.Reserve(1, SharedGranted, 1, 0, SingleSharedBytes));
	SharedBudget.Release(2, 2, 100);
	this->TestEqual(TEXT("Individual release retains world-owned capacity"), SharedBudget.ReservedSimulationBytes, SingleSharedBytes);
	this->TestFalse(TEXT("Shared growth and private bytes are checked atomically"), SharedBudget.Reserve(1, SharedGranted, 1, 100, SingleSharedBytes + 1));
	this->TestEqual(TEXT("Rejected growth keeps the previous common reservation"), SharedBudget.SharedGpuReservationBytes, SingleSharedBytes);

	this->TestTrue(TEXT("Observed shared growth uses remaining capacity"), SharedBudget.GrowSharedReservation(SingleSharedBytes + 100));
	this->TestEqual(TEXT("Shared growth adds no source"), SharedBudget.ActiveSources, 0);
	this->TestEqual(TEXT("Shared growth adds no particle"), SharedBudget.ReservedParticles, 0);
	this->TestFalse(TEXT("Observed shared overflow rejects"), SharedBudget.GrowSharedReservation(SingleSharedBytes + 101));
	this->TestTrue(TEXT("Smaller observed demand retains the high water mark"), SharedBudget.GrowSharedReservation(SingleSharedBytes));
	this->TestEqual(TEXT("Observed rejection and shrink preserve the reserved total"), SharedBudget.ReservedSimulationBytes, SingleSharedBytes + int64{100});
	auto SceneOnly = FMeshParticleizerDrawGpuSnapshot{};
	SceneOnly.Frame = 1;
	SceneOnly.Views = 2;
	SceneOnly.Passes = {ViewBuffers};
	this->TestTrue(TEXT("Scene-only components need no mesh pass"), SceneOnly.AddMeshDemand(50, 0, {}, Error));
	this->TestEqual(TEXT("Scene-only growth does not create a mesh pass"), SceneOnly.Passes.Num(), 1);
	this->TestEqual(TEXT("Scene-only growth extends the common mask"), SceneOnly.Passes[0].GpuWrittenBytes, int64{32});

	auto Allocation = TArray<FMeshParticleizerAllocation>{{1, 100}, {3, 100}};
	FMeshParticleizerAllocation::Distribute(100, Allocation);
	this->TestEqual(TEXT("Surface weight receives proportional share"), Allocation[0].Granted, 25);
	this->TestEqual(TEXT("Importance receives proportional share"), Allocation[1].Granted, 75);
	Allocation[1].Capacity = 10;
	FMeshParticleizerAllocation::Distribute(100, Allocation);
	this->TestEqual(TEXT("Unused capacity is reassigned"), Allocation[0].Granted, 90);
	this->TestEqual(TEXT("Prepared sample capacity is respected"), Allocation[1].Granted, 10);
	FMeshParticleizerAllocation::Distribute(1, Allocation);
	this->TestEqual(TEXT("Small budget preserves total count"), Allocation[0].Granted + Allocation[1].Granted, 1);
	Allocation = {{1, 10}, {1, 10}, {1, 10}};
	FMeshParticleizerAllocation::Distribute(2, Allocation);
	this->TestTrue(TEXT("Equal-weight rounding does not favor the same source twice"),
		Allocation[0].Granted == 1 && Allocation[1].Granted == 1 && Allocation[2].Granted == 0);
	return true;
}
#endif
