#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Components/MeshComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraComponent.h"
#include "NiagaraSimCache.h"
#include "NiagaraSimCacheFunctionLibrary.h"
#include "UObject/StrongObjectPtr.h"

FString UMeshParticleizerPreparationLibrary::CompareParticleProgress(UMeshComponent* Display,
	UMeshParticleizerPreparedMesh* Prepared, UNiagaraComponent* Particles)
{
	if (Display == nullptr || Prepared == nullptr || Particles == nullptr || Display->GetNumMaterials() == 0)
	{
		return TEXT("Invalid progress validation inputs.");
	}
	auto bFound = false;
	const auto Mode = Particles->GetVariableInt(TEXT("User.Mode"), bFound);
	if (bFound == false || Mode < 0 || (Mode > 3 && Mode != 7))
	{
		return TEXT("Progress comparison requires direction, center, outside or point modes.");
	}

	const FName RowNames[] = {TEXT("ProgressX"), TEXT("ProgressY"), TEXT("ProgressZ")};
	const FName VectorNames[] = {TEXT("BoundsMin"), TEXT("BoundsMax"), TEXT("Direction"), TEXT("Origin")};
	auto Transform = FMatrix::Identity;
	FVector Values[4];
	auto Points = TArray<FVector>{};
	auto PointDistance = 0.0f;
	for (auto Slot = 0; Slot < Display->GetNumMaterials(); ++Slot)
	{
		auto* Material = Cast<UMaterialInstanceDynamic>(Display->GetMaterial(Slot));
		if (Material == nullptr)
		{
			return TEXT("Display does not use the prepared dynamic material.");
		}
		for (auto Axis = 0; Axis < 3; ++Axis)
		{
			auto Value = FLinearColor{};
			if (Material->GetVectorParameterValue(FMaterialParameterInfo(FName(FString(TEXT("MP_")) + RowNames[Axis].ToString())), Value) == false)
			{
				return TEXT("Display material is missing shared progress inputs.");
			}
			const auto Row = Particles->GetVariableVec4(FName(FString(TEXT("User.")) + RowNames[Axis].ToString()), bFound);
			if (bFound == false || FVector4(Value).Equals(Row, 0.00001) == false)
			{
				return TEXT("Material and GPU progress transforms differ.");
			}
			for (auto Column = 0; Column < 4; ++Column)
			{
				Transform.M[Column][Axis] = Value.Component(Column);
			}
		}
		for (auto Index = 0; Index < 4; ++Index)
		{
			auto Value = FLinearColor{};
			if (Material->GetVectorParameterValue(FMaterialParameterInfo(FName(FString(TEXT("MP_")) + VectorNames[Index].ToString())), Value) == false)
			{
				return TEXT("Display material is missing progress bounds or origin.");
			}
			Values[Index] = FVector(Value);
			const auto Input = Particles->GetVariableVec3(FName(FString(TEXT("User.")) + VectorNames[Index].ToString()), bFound);
			if (bFound == false || Values[Index].Equals(Input, 0.00001) == false)
			{
				return TEXT("Material and GPU progress bounds or origin differ.");
			}
		}
		if (Material->K2_GetScalarParameterValue(TEXT("MP_Mode")) != Mode)
		{
			return TEXT("Material and GPU origin modes differ.");
		}
		if (Mode == 7)
		{
			const auto Count = Particles->GetVariableInt(TEXT("User.OriginCount"), bFound);
			if (bFound == false || Count < 1 || Count > 4 || Material->K2_GetScalarParameterValue(TEXT("MP_OriginCount")) != Count)
			{
				return TEXT("Material and GPU origin counts differ or are invalid.");
			}
			PointDistance = Particles->GetVariableFloat(TEXT("User.OriginDistance"), bFound);
			if (bFound == false || FMath::IsFinite(PointDistance) == false || PointDistance <= 0 ||
				Material->K2_GetScalarParameterValue(TEXT("MP_OriginDistance")) != PointDistance)
			{
				return TEXT("Material and GPU distance scales differ.");
			}
			Points = {Values[3]};
			for (auto Index = 1; Index < Count; ++Index)
			{
				const auto Name = FString::Printf(TEXT("Origin%d"), Index);
				auto Value = FLinearColor{};
				if (Material->GetVectorParameterValue(FMaterialParameterInfo(FName(TEXT("MP_") + Name)), Value) == false)
				{
					return TEXT("Material origin point is missing.");
				}
				const auto Input = Particles->GetVariableVec3(FName(TEXT("User.") + Name), bFound);
				if (bFound == false || FVector(Value).Equals(Input, 0.00001) == false)
				{
					return TEXT("Material and GPU origin points differ.");
				}
				Points.Add(FVector(Value));
			}
		}
	}

	TArray64<uint8> Bytes;
	auto* Texture = Prepared->Positions.LoadSynchronous();
	if (Texture == nullptr || Texture->Source.GetFormat() != TSF_RGBA32F || Texture->Source.GetMipData(Bytes, 0) == false ||
		Bytes.Num() / static_cast<int64>(sizeof(FLinearColor)) < Prepared->SampleCount)
	{
		return TEXT("Prepared reference positions are unavailable.");
	}
	const auto* Positions = reinterpret_cast<const FLinearColor*>(Bytes.GetData());
	auto Cache = TStrongObjectPtr<UNiagaraSimCache>(NewObject<UNiagaraSimCache>());
	auto Parameters = FNiagaraSimCacheCreateParameters{};
	Parameters.AttributeCaptureMode = ENiagaraSimCacheAttributeCaptureMode::All;
	UNiagaraSimCache* Captured = nullptr;
	if (UNiagaraSimCacheFunctionLibrary::CaptureNiagaraSimCacheImmediate(Cache.Get(), Parameters, Particles, Captured) == false)
	{
		return TEXT("Could not capture live particle orders.");
	}
	auto Orders = TArray<float>{};
	auto Indices = TArray<int32>{};
	Cache->ReadFloatAttribute(Orders, TEXT("Order"), TEXT("Surface"));
	Cache->ReadIntAttribute(Indices, TEXT("SampleIndex"), TEXT("Surface"));
	if (Orders.IsEmpty() || Orders.Num() != Indices.Num())
	{
		return TEXT("Particle order attributes are missing.");
	}

	// マテリアル入力を使う独立したCPU比較。通常再生には頂点評価や同期読戻しを追加しない。
	const auto Center = (Values[0] + Values[1]) * 0.5;
	const auto Extent = ((Values[1] - Values[0]) * 0.5).ComponentMax(FVector(0.0001));
	const auto Start = Mode == 3 ? Values[3] : Center;
	const auto Distance = FMath::Max((Values[0] - Start).GetAbs().ComponentMax((Values[1] - Start).GetAbs()).Length(), 0.0001);
	const auto Span = FMath::Max(FVector::DotProduct(Values[2].GetAbs(), Extent) * 2, 0.0001);
	auto MaximumError = 0.0;
	auto MinimumOrder = 1.0;
	auto MaximumOrder = 0.0;
	for (auto Index = 0; Index < Orders.Num(); ++Index)
	{
		const auto Sample = Indices[Index];
		if (Sample < 0 || Sample >= Prepared->SampleCount)
		{
			return TEXT("Invalid particle sample ID.");
		}
		const auto Point = FVector(Transform.TransformPosition(FVector(Positions[Sample])));
		auto Order = Mode == 0 ? FVector::DotProduct(Point - Center, Values[2]) / Span + 0.5 : (Point - Start).Length() / Distance;
		if (Mode == 7)
		{
			auto Nearest = TNumericLimits<double>::Max();
			for (const auto& Origin : Points)
			{
				Nearest = FMath::Min(Nearest, FVector::Distance(Point, Origin));
			}
			Order = Nearest / PointDistance;
		}
		Order = FMath::Clamp(Order, 0.0, 1.0);
		if (Mode == 2)
		{
			Order = 1 - Order;
		}
		if (FMath::IsFinite(Orders[Index]) == false)
		{
			return TEXT("GPU order is not finite.");
		}
		MaximumError = FMath::Max(MaximumError, FMath::Abs(Order - Orders[Index]));
		MinimumOrder = FMath::Min<double>(MinimumOrder, Orders[Index]);
		MaximumOrder = FMath::Max<double>(MaximumOrder, Orders[Index]);
	}
	return FString::Printf(TEXT("{\"samples\":%d,\"max_order_error\":%.9f,\"min_order\":%.9f,\"max_order\":%.9f,\"material_inputs_match\":true}"),
		Orders.Num(), MaximumError, MinimumOrder, MaximumOrder);
}
