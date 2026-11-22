#include "MeshParticleizerPreview.h"
#include "MeshParticleizerFarSprite.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParticleColor.h"
#include "Materials/MaterialExpressionParticleDirection.h"
#include "Materials/MaterialExpressionParticlePositionWS.h"
#include "Materials/MaterialExpressionParticleSpeed.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	template <typename T>
	T* AddExpression(UMaterial* Material)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass()));
	}

	void AddInput(UMaterialExpressionCustom* Target, FName Name, UMaterialExpression* Source, int32 Output = 0)
	{
		auto& Input = Target->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(Output, Source);
	}
}

bool UMeshParticleizerPreview::SetVelocityDisplay(float MaxSpeed, int32 SampleStride, FString& OutError)
{
	if (this->bClosed == true || FMath::IsFinite(MaxSpeed) == false || MaxSpeed < 1 || MaxSpeed > 100000 ||
		SampleStride < 1 || SampleStride > 4096)
	{
		OutError = TEXT("Use a speed scale of 1–100000 cm/s and a sample stride of 1–4096 in an open preview.");
		return false;
	}

	this->VelocityMaxSpeed = MaxSpeed;
	this->VelocitySampleStride = SampleStride;
	for (const auto* Materials : {&this->ParticleVelocityMaterials, &this->FarParticleVelocityMaterials})
	{
		for (const auto& Entry : *Materials)
		{
			if (auto* Material = Cast<UMaterialInstanceDynamic>(Entry.Value))
			{
				Material->SetScalarParameterValue(TEXT("MP_PreviewVelocityMaxSpeed"), MaxSpeed);
				Material->SetScalarParameterValue(TEXT("MP_PreviewVelocityStride"), SampleStride);
			}
		}
	}
	OutError.Reset();
	return true;
}

UMaterialInstanceDynamic* UMeshParticleizerPreview::CreateVelocityMaterial(bool bMesh, FString& OutError, float FarDistance)
{
	// 再コンパイル中のGCでも、一時材と式の所有権を維持する。
	auto Guard = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>(this, NAME_None, RF_Transient));
	auto* Material = Guard.Get();
	Material->SetShadingModel(MSM_Unlit);
	Material->BlendMode = BLEND_Masked;
	Material->bCanMaskedBeAssumedOpaque = false;
	Material->SetUsageByFlag(bMesh ? MATUSAGE_NiagaraMeshParticles : MATUSAGE_NiagaraSprites, true);
	Material->TwoSided = true;

	// GPUの実速度を直接参照する。継承発光RGBは残し、未使用Aのサンプル番号で間引く。
	auto* UV = AddExpression<UMaterialExpressionTextureCoordinate>(Material);

	// 三角形(0,0)-(2,0)-(0,2)は、Spriteと同じ0..1の矢印領域をすべて覆う。
	UV->UTiling = UV->VTiling = bMesh ? 2.f : 1.f;
	auto* Direction = AddExpression<UMaterialExpressionParticleDirection>(Material);
	auto* Speed = AddExpression<UMaterialExpressionParticleSpeed>(Material);
	auto* Color = AddExpression<UMaterialExpressionParticleColor>(Material);
	auto* Sample = AddExpression<UMaterialExpressionDynamicParameter>(Material);
	Sample->ParameterIndex = 1;
	auto* ViewDirection = AddExpression<UMaterialExpressionTransform>(Material);
	ViewDirection->TransformSourceType = TRANSFORMSOURCE_World;
	ViewDirection->TransformType = TRANSFORM_View;
	ViewDirection->Input.Connect(0, Direction);
	auto* Maximum = AddExpression<UMaterialExpressionScalarParameter>(Material);
	Maximum->ParameterName = TEXT("MP_PreviewVelocityMaxSpeed");
	Maximum->DefaultValue = this->VelocityMaxSpeed;
	auto* Stride = AddExpression<UMaterialExpressionScalarParameter>(Material);
	Stride->ParameterName = TEXT("MP_PreviewVelocityStride");
	Stride->DefaultValue = this->VelocitySampleStride;

	auto* Emission = AddExpression<UMaterialExpressionCustom>(Material);
	Emission->OutputType = CMOT_Float3;
	Emission->Inputs.Reset();
	Emission->IncludeFilePaths = {TEXT("/Plugin/MeshParticleizer/Private/ParticleizerPreviewVelocity.ush")};
	AddInput(Emission, TEXT("Speed"), Speed);
	AddInput(Emission, TEXT("Maximum"), Maximum);
	Emission->Code = TEXT("return MP_PreviewVelocityColor(Speed, Maximum);");
	Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Emission);

	auto* Mask = AddExpression<UMaterialExpressionCustom>(Material);
	Mask->OutputType = CMOT_Float1;
	Mask->Inputs.Reset();
	Mask->IncludeFilePaths = Emission->IncludeFilePaths;
	AddInput(Mask, TEXT("UV"), UV);
	AddInput(Mask, TEXT("ViewDirection"), ViewDirection);
	AddInput(Mask, TEXT("Speed"), Speed);
	AddInput(Mask, TEXT("Index"), Sample, 3);
	AddInput(Mask, TEXT("Stride"), Stride);
	AddInput(Mask, TEXT("Alpha"), Color, 4);
	Mask->Code = TEXT("return MP_PreviewVelocityMask(UV, ViewDirection, Speed, Index, Stride, Alpha);");
	auto* Dither = AddExpression<UMaterialExpressionMaterialFunctionCall>(Material);
	Dither->SetMaterialFunction(LoadObject<UMaterialFunction>(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA.DitherTemporalAA")));
	Dither->FunctionInputs[0].Input.Connect(0, Mask);
	Material->GetExpressionInputForProperty(MP_OpacityMask)->Connect(0, Dither);

	// 元のQuadを粒子中心へ戻してから、速度に沿う表示用Quadを置く。粒子座標は変更しない。
	auto* Offset = AddExpression<UMaterialExpressionCustom>(Material);
	Offset->OutputType = CMOT_Float3;
	Offset->Inputs = Mask->Inputs;
	Offset->IncludeFilePaths = Mask->IncludeFilePaths;
	AddInput(Offset, TEXT("Maximum"), Maximum);
	Offset->Code = TEXT("return MP_PreviewVelocityOffset(UV, ViewDirection, Speed, Maximum, Index, Stride, Alpha);");
	auto* WorldOffset = AddExpression<UMaterialExpressionTransform>(Material);
	WorldOffset->TransformSourceType = TRANSFORMSOURCE_View;
	WorldOffset->TransformType = TRANSFORM_World;
	WorldOffset->Input.Connect(0, Offset);
	auto* Vertex = AddExpression<UMaterialExpressionWorldPosition>(Material);
	Vertex->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	auto* Position = AddExpression<UMaterialExpressionParticlePositionWS>(Material);
	auto* OriginalOffset = AddExpression<UMaterialExpressionSubtract>(Material);
	OriginalOffset->A.Connect(0, Vertex);
	OriginalOffset->B.Connect(0, Position);
	auto* Delta = AddExpression<UMaterialExpressionSubtract>(Material);
	Delta->A.Connect(0, WorldOffset);
	Delta->B.Connect(0, OriginalOffset);
	Material->GetExpressionInputForProperty(MP_WorldPositionOffset)->Connect(0, Delta);

	if (FarDistance > 0.f)
	{
		MeshParticleizerConfigureFarSpriteMaterial(Material, FarDistance, false);
	}
	UMaterialEditingLibrary::RecompileMaterial(Material);
	if (auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		Resource->FinishCompilation();
		if (Resource->GetCompileErrors().IsEmpty() == false)
		{
			OutError = TEXT("Velocity material compilation failed: ") + FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
			return nullptr;
		}
	}
	OutError.Reset();
	return UMaterialInstanceDynamic::Create(Material, this);
}
