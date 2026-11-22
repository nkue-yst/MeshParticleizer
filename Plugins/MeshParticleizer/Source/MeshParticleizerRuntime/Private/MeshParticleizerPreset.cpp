#include "MeshParticleizerPreset.h"
#include "MeshParticleizerParticleStyle.h"
#include "Curves/CurveFloat.h"
#include "Misc/DataValidation.h"
#include "UObject/ObjectSaveContext.h"

bool UMeshParticleizerPreset::ValidateSettings(const FMeshParticleizeOptions& Settings, FString& OutError)
{
	if (Settings.Validate(OutError) == false)
	{
		return false;
	}

	// 特定Worldのヒットは使い回せない。起点モードを保存し、ヒット情報は呼出しごとに渡す。
	if (Settings.HitOrigin.SourceMesh.IsExplicitlyNull() == false)
	{
		OutError = TEXT("A preset cannot store a hit from a world component. Set the hit on the copied playback options.");
		return false;
	}
	const auto CurvePath = Settings.ProgressCurve.ToSoftObjectPath();
	if (CurvePath.IsNull() == false && (CurvePath.IsAsset() == false || CurvePath.GetLongPackageName() == TEXT("/Engine/Transient")))
	{
		OutError = TEXT("Save the progress curve as an asset before storing it in a preset.");
		return false;
	}
	OutError.Reset();
	return true;
}

bool UMeshParticleizerPreset::CopyOptions(FMeshParticleizeOptions& OutOptions, FString& OutError) const
{
	if (ValidateSettings(this->Options, OutError) == false)
	{
		return false;
	}
	OutOptions = this->Options;
	return true;
}

bool UMeshParticleizerPreset::SetOptions(const FMeshParticleizeOptions& Settings, FString& OutError)
{
	if (ValidateSettings(Settings, OutError) == false)
	{
		return false;
	}
	this->Options = Settings;
	return true;
}

#if WITH_EDITOR
EDataValidationResult UMeshParticleizerPreset::IsDataValid(FDataValidationContext& Context) const
{
	const auto Base = Super::IsDataValid(Context);
	auto Error = FString{};
	if (ValidateSettings(this->Options, Error) == false)
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	if (this->Options.ProgressCurve.IsNull() == false)
	{
		const auto* Curve = this->Options.ProgressCurve.LoadSynchronous();
		if (Curve == nullptr || Curve->IsAsset() == false || Curve->IsEditorOnly() == true)
		{
			Context.AddError(FText::FromString(TEXT("The progress curve must resolve to a saved runtime asset before Cook.")));
			return EDataValidationResult::Invalid;
		}
	}
	if (this->Options.ParticleStyle.IsNull() == false)
	{
		const auto* Style = this->Options.ParticleStyle.LoadSynchronous();
		if (Style == nullptr || Style->IsAsset() == false || Style->IsEditorOnly() == true || Style->ValidateResourceManifest(Error) == false)
		{
			Context.AddError(FText::FromString(TEXT("Particle Style must resolve to a saved runtime asset with current dependencies before Cook. ") + Error));
			return EDataValidationResult::Invalid;
		}
	}
	return Base == EDataValidationResult::Invalid ? Base : EDataValidationResult::Valid;
}

void UMeshParticleizerPreset::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);
	if (SaveContext.IsCooking() == true)
	{
		auto Context = FDataValidationContext{};
		if (this->IsDataValid(Context) == EDataValidationResult::Invalid)
		{
			auto Warnings = TArray<FText>{};
			auto Errors = TArray<FText>{};
			Context.SplitIssues(Warnings, Errors);
			for (const auto& Error : Errors)
			{
				UE_LOG(LogTemp, Error, TEXT("Mesh Particleizer cannot cook %s: %s"), *this->GetPathName(), *Error.ToString());
			}
		}
	}
}
#endif
