#pragma once

#include "CoreMinimal.h"
#include "MeshParticleizerTypes.generated.h"

class UMeshComponent;
class UMeshParticleizerPreparedMesh;
class UCurveFloat;
class UMeshParticleizerSurfaceTexture;
class UMeshParticleizerParticleStyle;
class UWorld;

// 元表面と分離時の色へ共通に掛ける。粒子だけの追加演出Tintとは別の入力。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerSurfaceColorParameters
{
	GENERATED_BODY()

	// RGBのみ使用し、透明度や元のOpacity Maskは変更しない。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Color")
	FLinearColor Tint = FLinearColor::White;

	// Register Surface Texture / Register Color Providerで準備した入力。未指定なら元の色へ戻す。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Color")
	TObjectPtr<UMeshParticleizerSurfaceTexture> Texture;

	// Color ProviderをUV0で参照する変換。通常のTexture差し替えは準備時の座標を使う。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Color")
	FVector2D ProviderUVScale = FVector2D(1, 1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Color")
	FVector2D ProviderUVOffset = FVector2D::ZeroVector;

	// HDRモードで登録した公開画像。元の発光を継承する再生へ渡し、分離後は値を固定する。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Emission")
	TObjectPtr<UMeshParticleizerSurfaceTexture> EmissionProvider;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Emission")
	FVector2D EmissionUVScale = FVector2D(1, 1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Emission")
	FVector2D EmissionUVOffset = FVector2D::ZeroVector;
};

// 登録する部品を明示する。姿勢の共有元や装備を自動で対象へ加えない。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerTarget
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Particleizer")
	TWeakObjectPtr<UMeshComponent> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Particleizer")
	TSoftObjectPtr<UMeshParticleizerPreparedMesh> PreparedData;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Particleizer", meta = (ClampMin = "0.001"))
	float Importance = 1;

	// 登録・準備・開始で共通に検査する。Soft Referenceの同期ロードは行わない。
	static bool ValidateTargets(TConstArrayView<FMeshParticleizerTarget> Targets, const UWorld* World, FString& OutError);
};

UENUM(BlueprintType)
enum class EMeshParticleizerOrigin : uint8
{
	Direction,
	CenterOut,
	OutsideIn,
	Point,
	Noise,
	BoneOrSocket,
	Hit,
	MultiplePoints
};

UENUM(BlueprintType)
enum class EMeshParticleizerOriginAccuracy : uint8
{
	Unavailable,
	ReferenceSpace,
	BoneOrSocket,
	StaticHit,
	BoneApproximation,
	SurfaceProjection
};

// ヒットした時点で参照姿勢へ戻す。準備待ちの間に元メッシュが動いても位置を失わない。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerHitOrigin
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	TWeakObjectPtr<UMeshComponent> SourceMesh;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FVector ReferencePosition = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FName BoneName;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	EMeshParticleizerOriginAccuracy Accuracy = EMeshParticleizerOriginAccuracy::Unavailable;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FString Diagnostics;

	// 精密投影で採用した準備データの三角形。PhysicsのFaceIndexとは別の番号。
	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	int32 TriangleIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FVector Barycentrics = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FVector ProjectedWorldPosition = FVector::ZeroVector;

	// 入力した命中点から描画用三角形までの距離。投影結果自体の精度誤差ではない。
	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	double ProjectionDistance = 0;

	UPROPERTY()
	TSoftObjectPtr<UObject> SourceAsset;
};

UENUM(BlueprintType)
enum class EMeshParticleizerMotion : uint8
{
	Drift,
	Fall,
	Settle,
	Scatter
};

UENUM(BlueprintType)
enum class EMeshParticleizerDisappear : uint8
{
	Fade,
	Shrink,
	FadeAndShrink,
	Instant
};

UENUM(BlueprintType)
enum class EMeshParticleizerCollision : uint8
{
	SimpleGround,
	GlobalDistanceField,
	Auto
};

// 設定の有効性と描画側の観測を区別する。Readyでも粒子用の空き容量は別途必要。
UENUM(BlueprintType)
enum class EMeshParticleizerGdfStatus : uint8
{
	NotObserved,
	GenerateMeshDistanceFieldsDisabled,
	GlobalDistanceFieldDisabled,
	UpdatesDisabled,
	ClipmapUpdatesDisabled,
	WaitingForRender,
	Unavailable,
	Ready
};

UENUM(BlueprintType)
enum class EMeshParticleizerGround : uint8
{
	Disabled,
	Automatic,
	ExplicitHeight
};

// 単一の水平面を採用できたかと、採用しなかった理由。
UENUM(BlueprintType)
enum class EMeshParticleizerGroundStatus : uint8
{
	NotEvaluated,
	Disabled,
	ExplicitHeight,
	Detected,
	NoHit,
	InitialOverlap,
	UnsupportedSlope,
	UnevenHeights,
	GlobalDistanceField
};

// 開始時の判定を保持する。HeightはbEnabledがtrueの場合だけ有効。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerResolvedGround
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	EMeshParticleizerGroundStatus Status = EMeshParticleizerGroundStatus::NotEvaluated;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	bool bEnabled = false;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	float Height = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Collision")
	FString Diagnostics;
};

UENUM(BlueprintType)
enum class EMeshParticleizerQuality : uint8
{
	Fast,
	Balanced,
	High
};

UENUM(BlueprintType)
enum class EMeshParticleizerPriority : uint8
{
	Low,
	Normal,
	High
};

// 全体の品質と、各要求の品質で共通に使う初期上限。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerQualityLimits
{
	int32 ParticleLimit;
	int32 TargetParticleLimit;
	int32 SourceLimit;
	int32 RestingParticleLimit;
	int64 SimulationMemoryLimitBytes;

	static const FMeshParticleizerQualityLimits* Find(EMeshParticleizerQuality Quality);
};

UENUM(BlueprintType)
enum class EMeshParticleizeState : uint8
{
	Ready,
	Converting,
	Converted,
	Draining,
	Finished,
	Cancelled,
	Failed
};

USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizeOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timing", meta = (ClampMin = "0"))
	float ConversionDuration = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timing", meta = (ClampMin = "0"))
	float ParticleLifetime = 3.0f;

	// 時間と値を0〜1で作る単調増加カーブ。未指定なら等速で進める。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timing")
	TSoftObjectPtr<UCurveFloat> ProgressCurve;

	// 開始時の表示姿勢を固定し、停止直前の慣性を分離後も使う。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	bool bFreezeAtStart = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin")
	EMeshParticleizerOrigin Origin = EMeshParticleizerOrigin::Direction;

	// 登録順の先頭を基準に、全部品の参照姿勢で一つの進行範囲を作る。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin")
	bool bShareOrigin = true;

	// 共有時は先頭のメッシュ、それ以外は各メッシュの参照座標で固定する。
	// 下向きなら上から消える。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin")
	FVector Direction = FVector(0, 0, -1);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "Origin == EMeshParticleizerOrigin::Direction", EditConditionHides))
	bool bDirectionInWorldSpace = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin")
	FVector LocalOrigin = FVector::ZeroVector;

	// 共有時は先頭、それ以外は各メッシュの参照座標。BlueprintではSet Point Originsで設定する。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Origin", meta = (EditCondition = "Origin == EMeshParticleizerOrigin::MultiplePoints", EditConditionHides))
	TArray<FVector> LocalOrigins;

	// 共有時は先頭のメッシュで探し、共有しない場合は各部品で探す。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "Origin == EMeshParticleizerOrigin::BoneOrSocket", EditConditionHides))
	FName OriginBoneOrSocket;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Origin", meta = (EditCondition = "Origin == EMeshParticleizerOrigin::Hit", EditConditionHides))
	FMeshParticleizerHitOrigin HitOrigin;

	// 指定した起点を使えない場合の代替。見つからない場合は範囲の中心を使う。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Origin", meta = (EditCondition = "Origin == EMeshParticleizerOrigin::BoneOrSocket || Origin == EMeshParticleizerOrigin::Hit", EditConditionHides))
	FName FallbackBone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	EMeshParticleizerMotion Motion = EMeshParticleizerMotion::Drift;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	EMeshParticleizerDisappear Disappear = EMeshParticleizerDisappear::Fade;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (ClampMin = "0"))
	float FadeDuration = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (ClampMin = "0.01"))
	float ParticleSize = 0.7f;

	// 未指定なら準備データの描画を使う。変更後は依存資産を準備してから開始する。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	TSoftObjectPtr<UMeshParticleizerParticleStyle> ParticleStyle;

	// メッシュ粒子のローカルZ軸を、分離した表面法線へ向ける。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance|Mesh")
	bool bAlignMeshToSurface = true;

	// ワールド軸の回転速度（度／秒）。静止した粒子は回転も止める。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance|Mesh")
	FVector MeshAngularVelocity = FVector(25, 40, 15);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	FLinearColor Tint = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance", meta = (ClampMin = "0"))
	float Emissive = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Appearance")
	bool bInheritSourceEmission = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision")
	EMeshParticleizerCollision Collision = EMeshParticleizerCollision::SimpleGround;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision", meta = (EditCondition = "Collision != EMeshParticleizerCollision::GlobalDistanceField", EditConditionHides))
	EMeshParticleizerGround Ground = EMeshParticleizerGround::Automatic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision", meta = (EditCondition = "Ground == EMeshParticleizerGround::ExplicitHeight"))
	float GroundHeight = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (ClampMin = "0"))
	float InheritVelocityScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = "0"))
	float NormalSpeed = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = "0"))
	float ScatterSpeed = 140.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = "0"))
	float RandomSpeed = 18.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	FVector Gravity = FVector(0, 0, -980);

	// 漂いに加える重力の倍率。既定0で既存の浮遊を保ち、灰には弱い落下を設定する。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = "0", EditCondition = "Motion == EMeshParticleizerMotion::Drift", EditConditionHides))
	float DriftGravityScale = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion")
	FVector Wind = FVector(18, 4, 6);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = "0"))
	float NoiseAcceleration = 28.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion", meta = (ClampMin = "0"))
	float Drag = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision", meta = (ClampMin = "0", ClampMax = "1"))
	float Restitution = 0.15f;

	// 接触時の法線方向の力に対する、接線方向の減速の比。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Collision", meta = (ClampMin = "0", ClampMax = "1"))
	float Friction = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Collision", meta = (ClampMin = "0"))
	float CollisionRadiusScale = 0.5f;

	// 0なら自動で静止させない。静止後も粒子の寿命は進む。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Collision", meta = (ClampMin = "0", EditCondition = "Motion == EMeshParticleizerMotion::Settle", EditConditionHides))
	float SleepSpeed = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Collision", meta = (ClampMin = "0", EditCondition = "Motion == EMeshParticleizerMotion::Settle", EditConditionHides))
	float SleepDuration = 0.15f;

	// 有効な履歴がない単発呼出しで使う。通常の差分速度へ重ねて加算しない。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced")
	bool bUseInitialVelocity = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (EditCondition = "bUseInitialVelocity"))
	FVector InitialVelocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (ClampMin = "0"))
	float MaxInitialSpeed = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", meta = (ClampMin = "0"))
	int32 MaxParticles = 100000;

	// ワールドの品質を引き上げず、この要求の対象上限をさらに制限する。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Quality")
	EMeshParticleizerQuality Quality = EMeshParticleizerQuality::Balanced;

	// 床残留の上限を超えたとき、低優先度から古い静止粒子を減らす。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Quality")
	EMeshParticleizerPriority Priority = EMeshParticleizerPriority::Normal;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced")
	int32 Seed = 1337;

	bool Validate(FString& OutError) const;

	FVector GetEffectiveGravity() const;
};

// 登録順のソースの開始時配分。終了後も面積・重要度・採用数を保持する。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerSourceAllocation
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	bool bResolved = false;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	double ReferenceArea = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	double ScaledArea = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	FVector StartScale = FVector::OneVector;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	float Importance = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 CandidateLimit = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 GrantedParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	FString Diagnostics;
};

// 開始時に確定した結果。品質調整を診断文の解析なしで取得できる。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizeResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	bool bSucceeded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	bool bReducedQuality = false;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	EMeshParticleizerQuality AppliedQuality = EMeshParticleizerQuality::Balanced;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	EMeshParticleizerCollision AppliedCollision = EMeshParticleizerCollision::SimpleGround;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	int32 RequestedParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	int32 GrantedParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Mesh Particleizer")
	FString Diagnostics;
};

// 開始時に一度だけ作る進行座標。実際の分離位置と慣性の座標から独立させる。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerProgressSpace
{
	FMatrix SourceToGroup = FMatrix::Identity;
	FBox Bounds = FBox(ForceInit);

	static bool Resolve(TConstArrayView<FMeshParticleizerTarget> Targets, bool bShareOrigin,
		TArray<FMeshParticleizerProgressSpace>& OutSpaces, FString& OutError);
};

// 実際に使用した進行座標と起点。共有時は全部品で同じ結果を返す。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerResolvedOrigin
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	EMeshParticleizerOrigin Mode = EMeshParticleizerOrigin::Direction;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	EMeshParticleizerOriginAccuracy Accuracy = EMeshParticleizerOriginAccuracy::Unavailable;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	TWeakObjectPtr<UMeshComponent> ReferenceMesh;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	TWeakObjectPtr<UMeshComponent> OriginMesh;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FVector ReferencePosition = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	TArray<FVector> ReferencePoints;

	// 複数点からの最小距離が、進行範囲内で取り得る最大値。
	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	float ReferenceDistance = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FVector ReferenceDirection = FVector(0, 0, -1);

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FName BoneOrSocket;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	bool bUsedFallback = false;

	UPROPERTY(BlueprintReadOnly, Category = "Origin")
	FString Diagnostics;

	static bool Resolve(TConstArrayView<FMeshParticleizerTarget> Targets, const FMeshParticleizeOptions& Options,
		TConstArrayView<FMeshParticleizerProgressSpace> Spaces, TArray<FMeshParticleizerResolvedOrigin>& OutOrigins,
		FString& OutError);
};

// 準備時のカーブを独立して保持する。各区間の時間と値のBezier制御点をGPUにも渡す。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerProgressCurve
{
	static bool Compile(TSoftObjectPtr<UCurveFloat> Curve, TSharedPtr<const FMeshParticleizerProgressCurve>& OutCurve, FString& OutError);
	float Evaluate(float Time) const;
	float Inverse(float Progress) const;
	const TArray<FVector4f>& GetSegments() const
	{
		return this->Segments;
	}

private:
	TArray<FVector4f> Segments;
};

// 論理時刻から通知を判定する。GPU読み戻しや描画可否には依存しない。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizeSchedule
{
	double StartTime = 0;
	double ConversionDuration = 0;
	double ParticleLifetime = 0;
	TSharedPtr<const FMeshParticleizerProgressCurve> Curve;

	double Progress(double Now) const;
	double DetachTime(double Order) const;
	double ParticleAge(double Now, double Order) const;
	bool IsConverted(double Now) const;
	bool IsFinished(double Now) const;
};

// 同じゲーム時刻と原点系で二つの表面Transformを保持する。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerTransformHistory
{
	void Capture(const FTransform& Transform, double Time, const FIntVector& WorldOrigin);
	void Rebase(const FIntVector& WorldOrigin);
	void Reset();
	bool HasVelocity() const;

	FTransform Current = FTransform::Identity;
	FTransform Previous = FTransform::Identity;
	double CurrentTime = 0;
	double PreviousTime = 0;
	FIntVector Origin = FIntVector::ZeroValue;
	bool bHasCurrent = false;
	bool bHasPrevious = false;
	bool bDiscontinuous = false;
};

// 更新された骨姿勢だけを履歴へ進める。CPUでは頂点を変形せず、骨行列のみ用意する。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerBoneHistory
{
	bool Capture(TConstArrayView<FTransform> Transforms, TConstArrayView<FMatrix44f> InverseBind,
		uint32 Revision, double Time);
	bool CaptureMatrices(TConstArrayView<FMatrix44f> Matrices, uint32 Revision, double Time);
	void Reset();
	bool HasVelocity() const;

	TArray<FVector4f> Current;
	TArray<FVector4f> Previous;
	double CurrentTime = 0;
	double PreviousTime = 0;
	uint64 Serial = 0;
	uint32 CurrentRevision = 0;
	bool bHasCurrent = false;
	bool bHasPrevious = false;
};

// 終了通知と資源の再利用を分けて確認できる読み取り専用の使用状況。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerBudgetUsage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	EMeshParticleizerQuality Quality = EMeshParticleizerQuality::Balanced;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 ParticleLimit = 0;

	// 一回の要求へ登録した全部品の合計上限。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 TargetParticleLimit = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 SourceLimit = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 RestingParticleLimit = 0;

	// 非同期のGPU集計。割当予約や解放待ちとは別で、数フレーム前の状態を表す。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	bool bGpuCountsAvailable = false;

	// 集計元の描画フレーム。非同期読戻しの遅延を診断・計測時に区別する。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 GpuCountsRenderFrame = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 GpuUnseparatedParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 GpuLiveParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 GpuRestingParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 GpuBudgetFadingParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 GpuBudgetBufferBytes = 0;

	// 未分離・生存中・解放待ちを含み、GPUで消滅を確認した分は返す。生存粒子数とは別。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 ReservedParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 ReservedSources = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 ReleasingParticles = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 ReleasingSources = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 ReleasingOperations = 0;

	// 個別バッファ、プール、解放待ちとワールド共通予約の合計。共有アセットキャッシュは別枠。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 SimulationMemoryLimitBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 ReservedSimulationBytes = 0;

	// ReservedSimulationBytesの内数。UE側に残るため、最初のGPU利用からワールド終了まで保持する。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 SharedGpuReservationBytes = 0;

	// SharedGpuReservationBytesの内数。Scene本体とprimitive転送pool、描画poolの保持・退役を含む。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 SceneGpuReservationBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 DrawGpuReservationBytes = 0;

	// SharedGpuReservationBytesの内数。GDF問い合わせ用テクスチャと更新時の旧世代1組。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 GdfReservationBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 GdfTextureBytes = 0;

	// 最も近いクリップマップのセル幅。遠方ではさらに粗くなる。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	float GdfVoxelSizeCm = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	EMeshParticleizerGdfStatus GdfStatus = EMeshParticleizerGdfStatus::NotObserved;

	// Ready時のみ有効。現在の全予約とGDFの追加共有予約の合計。新しい粒子・描画バッファは別途必要。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 GdfMinimumMemoryLimitBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	FString GdfDiagnostics;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 ReleasingSimulationBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 PooledSimulations = 0;

	// ReservedSimulationBytesの内数。待機中も物理GPU容量を占有する。
	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 PooledSimulationBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 RetiringSimulations = 0;
};

// キャッシュが予約する共有資産容量。解放待ちはGCとGPU完了まで合計に含む。
USTRUCT(BlueprintType)
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerAssetCacheUsage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 LimitBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 ReservedBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 PinnedBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 LoadingBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int64 RetiringBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 CachedAssets = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Budget")
	int32 RetiringAssets = 0;
};

// 未分離候補も開始時に予約し、解放前の枠を再割当しない。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerBudget
{
	int32 ParticleLimit = 200000;
	int32 SourceLimit = 32;
	int32 TargetParticleLimit = 100000;
	EMeshParticleizerQuality Quality = EMeshParticleizerQuality::Balanced;
	int32 ReservedParticles = 0;
	int32 ActiveSources = 0;

	int64 SimulationMemoryLimitBytes = 256ll * 1024 * 1024;
	int64 ReservedSimulationBytes = 0;
	int64 SharedGpuReservationBytes = 0;

	bool SetQuality(EMeshParticleizerQuality InQuality, FString& OutError);
	bool Reserve(int32 Requested, int32& OutGranted, int32 SourceCount = 1, int64 SimulationBytes = 0, int64 SharedBytes = 0);
	bool GrowSharedReservation(int64 SharedBytes);
	void Release(int32 Granted, int32 SourceCount = 1, int64 SimulationBytes = 0);
};

// 共通予算を面積と重要度で配り、各部品の候補数を超えない。
struct MESHPARTICLEIZERRUNTIME_API FMeshParticleizerAllocation
{
	double Weight = 1;
	int32 Capacity = 0;
	int32 Granted = 0;

	static void Distribute(int32 Total, TArrayView<FMeshParticleizerAllocation> Sources);
};
