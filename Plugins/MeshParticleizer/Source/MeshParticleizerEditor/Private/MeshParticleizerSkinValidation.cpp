#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceArrayFunctionLibrary.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheFunctionLibrary.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/StrongObjectPtr.h"

namespace MeshParticleizerSkinValidation
{
float ReadFloat(UNiagaraComponent* Particles, FName Name, bool& bValid)
	{
	auto bFound = false;
	const auto Value = Particles->GetVariableFloat(Name, bFound);
	bValid &= bFound;
	return Value;
	}

FMatrix ReadTransform(UNiagaraComponent* Particles, const FName (&Names)[3], bool& bValid)
	{
	auto Matrix = FMatrix::Identity;
	for (auto Axis = 0; Axis < 3; ++Axis)
		{
		auto bFound = false;
		const auto Row = Particles->GetVariableVec4(Names[Axis], bFound);
		bValid &= bFound;
			for (auto Column = 0; Column < 4; ++Column)
			{
				Matrix.M[Column][Axis] = Row[Column];
			}
		}
	return Matrix;
	}

TArray<FMatrix44f> ReadBones(UNiagaraComponent* Particles, FName Name)
	{
	const auto Rows = UNiagaraDataInterfaceArrayFunctionLibrary::GetNiagaraArrayVector4(Particles, Name);
	auto Matrices = TArray<FMatrix44f>{};
	for (auto Bone = 0; Bone < Rows.Num() / 3; ++Bone)
		{
		auto Matrix = FMatrix44f::Identity;
		for (auto Axis = 0; Axis < 3; ++Axis)
			{
				for (auto Column = 0; Column < 4; ++Column)
				{
					Matrix.M[Column][Axis] = Rows[Bone * 3 + Axis][Column];
				}
			}
		Matrices.Add(Matrix);
		}
	return Matrices;
	}

FString Compare(USkinnedMeshComponent* Source, UMeshParticleizerPreparedMesh* Prepared,
	UNiagaraComponent* Particles, bool bVelocity)
	{
	auto* Mesh = Source != nullptr ? Cast<USkeletalMesh>(Source->GetSkinnedAsset()) : nullptr;
	if (Mesh == nullptr || Prepared == nullptr || Particles == nullptr || Prepared->SkeletalMesh.Get() != Mesh ||
		Mesh->GetResourceForRendering() == nullptr || Mesh->GetResourceForRendering()->LODRenderData.IsValidIndex(Prepared->SamplingLOD) == false)
		{
		return TEXT("Invalid skeletal validation inputs.");
		}

	// 準備時に保存した対応を読む。比較値の変形自体はUE標準のCPU評価を使う。
	TArray64<uint8> NormalBytes, CoordinateBytes, TriangleBytes;
	auto* Normals = Prepared->Normals.LoadSynchronous();
	auto* Coordinates = Prepared->Coordinates.LoadSynchronous();
	auto* Triangles = Prepared->SkinTriangles.LoadSynchronous();
	if (Normals == nullptr || Coordinates == nullptr || Triangles == nullptr ||
		Normals->Source.GetFormat() != TSF_RGBA32F || Coordinates->Source.GetFormat() != TSF_RGBA32F || Triangles->Source.GetFormat() != TSF_RGBA32F ||
		Normals->Source.GetMipData(NormalBytes, 0) == false || Coordinates->Source.GetMipData(CoordinateBytes, 0) == false ||
		Triangles->Source.GetMipData(TriangleBytes, 0) == false ||
		NormalBytes.Num() / static_cast<int64>(sizeof(FLinearColor)) < Prepared->SampleCount ||
		CoordinateBytes.Num() / static_cast<int64>(sizeof(FLinearColor)) < Prepared->SampleCount)
		{
		return TEXT("Prepared texture source pixels are unavailable.");
		}
	const auto* NormalData = reinterpret_cast<const FLinearColor*>(NormalBytes.GetData());
	const auto* CoordinateData = reinterpret_cast<const FLinearColor*>(CoordinateBytes.GetData());
	const auto* TriangleData = reinterpret_cast<const FLinearColor*>(TriangleBytes.GetData());

	auto Cache = TStrongObjectPtr<UNiagaraSimCache>(NewObject<UNiagaraSimCache>());
	auto Parameters = FNiagaraSimCacheCreateParameters{};
	Parameters.AttributeCaptureMode = ENiagaraSimCacheAttributeCaptureMode::All;
	UNiagaraSimCache* Captured = nullptr;
	if (UNiagaraSimCacheFunctionLibrary::CaptureNiagaraSimCacheImmediate(Cache.Get(), Parameters, Particles, Captured) == false)
		{
		return TEXT("Could not capture live particles.");
		}
	auto Positions = TArray<FVector>{};
	auto Indices = TArray<int32>{};
		if (bVelocity == true)
		{
			Cache->ReadVectorAttribute(Positions, TEXT("Velocity"), TEXT("Surface"));
		}
		else
		{
			Cache->ReadPositionAttribute(Positions, TEXT("Position"), TEXT("Surface"));
		}
	Cache->ReadIntAttribute(Indices, TEXT("SampleIndex"), TEXT("Surface"));
		if (Positions.IsEmpty() || Positions.Num() != Indices.Num())
		{
			return TEXT("Particle attributes are missing.");
		}
	auto Detached = TArray<float>{};
	Cache->ReadFloatAttribute(Detached, TEXT("Detached"), TEXT("Surface"));
		if (Detached.Num() != Indices.Num())
		{
			return TEXT("Particle detachment state is missing.");
		}

	auto Matrices = TArray<FMatrix44f>{};
	auto PreviousMatrices = TArray<FMatrix44f>{};
	auto CurrentTransform = Source->GetComponentTransform().ToMatrixWithScale();
	auto PreviousTransform = CurrentTransform;
	auto PoseDelta = 0.0f;
	auto BoneDelta = 0.0f;
	auto InheritScale = 1.0f;
	auto SpeedLimit = 0.0f;
	auto PaletteError = 0.0f;
	if (bVelocity == true)
		{
		// D=0で分離を終えたDIの二姿勢を使い、後続のアニメーションと比較時刻を混ぜない。
		Matrices = ReadBones(Particles, TEXT("User.Bones"));
		PreviousMatrices = ReadBones(Particles, TEXT("User.PreviousBones"));
		const FName CurrentNames[] = {TEXT("User.TransformX"), TEXT("User.TransformY"), TEXT("User.TransformZ")};
		const FName PreviousNames[] = {TEXT("User.PreviousX"), TEXT("User.PreviousY"), TEXT("User.PreviousZ")};
		auto bValid = true;
		CurrentTransform = ReadTransform(Particles, CurrentNames, bValid);
		PreviousTransform = ReadTransform(Particles, PreviousNames, bValid);
		PoseDelta = ReadFloat(Particles, TEXT("User.PoseDeltaSeconds"), bValid);
		BoneDelta = ReadFloat(Particles, TEXT("User.BoneDeltaSeconds"), bValid);
		InheritScale = ReadFloat(Particles, TEXT("User.InheritVelocity"), bValid);
		SpeedLimit = ReadFloat(Particles, TEXT("User.MaxInitialSpeed"), bValid);
		const auto Duration = ReadFloat(Particles, TEXT("User.Duration"), bValid);
		if (bValid == false || PoseDelta <= 0 || BoneDelta <= 0 || (Duration != 0 && Cast<UPoseableMeshComponent>(Source) == nullptr) ||
			Matrices.Num() != Prepared->BoneCount || PreviousMatrices.Num() != Matrices.Num())
			{
			return TEXT("Velocity comparison requires D=0 or a frozen display, and two valid source poses.");
			}
		}
	else
		{
		Source->GetCurrentRefToLocalMatrices(Matrices, Prepared->SamplingLOD);
		const auto Uploaded = ReadBones(Particles, TEXT("User.Bones"));
			if (Uploaded.Num() != Matrices.Num())
			{
				return TEXT("Captured and current bone palette sizes differ.");
			}
		// UE標準パレットはLODで使う骨だけを評価する。未使用骨の初期値は比較しない。
		for (const auto Bone : Mesh->GetResourceForRendering()->LODRenderData[Prepared->SamplingLOD].ActiveBoneIndices)
			{
			for (auto Row = 0; Row < 4; ++Row)
				{
				for (auto Axis = 0; Axis < 3; ++Axis)
					{
					PaletteError = FMath::Max(PaletteError, FMath::Abs(Uploaded[Bone].M[Row][Axis] - Matrices[Bone].M[Row][Axis]));
					}
				}
			}
		}
	auto& LOD = Mesh->GetResourceForRendering()->LODRenderData[Prepared->SamplingLOD];
	auto* Weights = Source->GetSkinWeightBuffer(Prepared->SamplingLOD);
		if (Weights == nullptr)
		{
			return TEXT("Source skin weights are unavailable.");
		}

	// 指定した前後行列を使うUE標準の評価経路。全頂点のCPU評価はEditor診断に限定する。
	auto CurrentVertices = TArray<FVector3f>{};
	auto PreviousVertices = TArray<FVector3f>{};
	USkinnedMeshComponent::ComputeSkinnedPositions(Source, CurrentVertices, Matrices, LOD, *Weights);
		if (bVelocity == true)
		{
			USkinnedMeshComponent::ComputeSkinnedPositions(Source, PreviousVertices, PreviousMatrices, LOD, *Weights);
		}
	if (CurrentVertices.Num() != static_cast<int32>(LOD.GetNumVertices()) ||
		(bVelocity == true && PreviousVertices.Num() != CurrentVertices.Num()))
		{
		return TEXT("CPU skinning reference is unavailable.");
		}

	auto Errors = TArray<double>{};
	auto Sum = 0.0;
	auto MinimumSpeed = TNumericLimits<double>::Max();
	auto MaximumSpeed = 0.0;
	for (auto Particle = 0; Particle < Positions.Num(); ++Particle)
		{
		// 段階分離の固定姿勢では、まだ生成されていない候補を比較へ含めない。
			if (Detached[Particle] < 0.5f)
			{
				continue;
			}
		const auto Sample = Indices[Particle];
			if (Sample < 0 || Sample >= Prepared->SampleCount)
			{
				return TEXT("Invalid particle sample ID.");
			}
		const auto TriangleIndex = static_cast<int32>(NormalData[Sample].A);
			if (TriangleIndex < 0 || TriangleIndex >= TriangleBytes.Num() / static_cast<int64>(sizeof(FLinearColor)))
			{
				return TEXT("Invalid triangle ID.");
			}
		const auto Triangle = TriangleData[TriangleIndex];
		const auto UV = CoordinateData[Sample];
		const float Bary[] = {UV.B, UV.A, 1 - UV.B - UV.A};
		auto Position = FVector3f::ZeroVector;
		auto PreviousPosition = FVector3f::ZeroVector;
		for (auto Corner = 0; Corner < 3; ++Corner)
			{
			const auto Vertex = static_cast<int32>(Triangle.Component(Corner));
				if (Vertex < 0 || static_cast<uint32>(Vertex) >= LOD.GetNumVertices())
				{
					return TEXT("Invalid vertex ID.");
				}
			Position += CurrentVertices[Vertex] * Bary[Corner];
			if (bVelocity == true)
				{
				PreviousPosition += PreviousVertices[Vertex] * Bary[Corner];
				}
			}
		auto Expected = FVector(CurrentTransform.TransformPosition(FVector(Position)));
		if (bVelocity == true)
			{
			const auto Before = FVector(PreviousTransform.TransformPosition(FVector(Position)));
			const auto LocalVelocity = FVector(Position - PreviousPosition) / BoneDelta;
			Expected = ((Expected - Before) / PoseDelta + FVector(PreviousTransform.TransformVector(LocalVelocity))) * InheritScale;
			Expected = Expected.GetClampedToMaxSize(SpeedLimit);
			MinimumSpeed = FMath::Min(MinimumSpeed, Expected.Size());
			MaximumSpeed = FMath::Max(MaximumSpeed, Expected.Size());
			}
		const auto Error = FVector::Distance(Expected, Positions[Particle]);
			if (FMath::IsFinite(Error) == false)
			{
				return TEXT("Non-finite GPU position.");
			}
		Errors.Add(Error);
		Sum += Error;
		}
		if (Errors.IsEmpty())
		{
			return TEXT("No detached particles are available for comparison.");
		}
	Errors.Sort();
	if (bVelocity == true)
		{
		return FString::Printf(TEXT("{\"count\":%d,\"mean_cm_s\":%.9f,\"p95_cm_s\":%.9f,\"max_cm_s\":%.9f,\"minimum_speed_cm_s\":%.6f,\"maximum_speed_cm_s\":%.6f,\"pose_delta_s\":%.9f,\"bone_delta_s\":%.9f}"),
			Errors.Num(), Sum / Errors.Num(), Errors[FMath::FloorToInt((Errors.Num() - 1) * 0.95)], Errors.Last(),
			MinimumSpeed, MaximumSpeed, PoseDelta, BoneDelta);
		}
	return FString::Printf(TEXT("{\"count\":%d,\"mean_cm\":%.9f,\"p95_cm\":%.9f,\"max_cm\":%.9f,\"palette_matrix_max_difference\":%.9f}"),
		Errors.Num(), Sum / Errors.Num(), Errors[FMath::FloorToInt((Errors.Num() - 1) * 0.95)], Errors.Last(), PaletteError);
	}
}

FString UMeshParticleizerPreparationLibrary::CompareSkeletalParticlePositions(USkinnedMeshComponent* Source,
	UMeshParticleizerPreparedMesh* Prepared, UNiagaraComponent* Particles)
{
	return MeshParticleizerSkinValidation::Compare(Source, Prepared, Particles, false);
}

FString UMeshParticleizerPreparationLibrary::CompareSkeletalParticleVelocities(USkinnedMeshComponent* Source,
	UMeshParticleizerPreparedMesh* Prepared, UNiagaraComponent* Particles)
{
	return MeshParticleizerSkinValidation::Compare(Source, Prepared, Particles, true);
}
