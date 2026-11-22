#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerSurfaceBaker.h"
#include "MeshParticleizerPreparedMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "IMaterialBakingModule.h"
#include "HAL/IConsoleManager.h"
#include "MaterialBakingStructures.h"
#include "MaterialBakingHelpers.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionGetMaterialAttributes.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionPreSkinnedPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MeshElementRemappings.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "TextureCompiler.h"
#include "NiagaraSystem.h"
#include "UObject/SavePackage.h"

namespace MeshParticleizerPreparation
{
	struct FTriangle
	{
		FVector3f Positions[3];
		FVector3f Normals[3];
		FVector2f UVs[3];
		FVector2f BakeUVs[3];
		int32 MaterialIndex = 0;
		int32 TriangleId = 0;
		double CumulativeArea = 0;
	};

	bool SaveAsset(UObject* Asset)
	{
		auto* Package = Asset->GetOutermost();
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		auto Args = FSavePackageArgs{};
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const auto Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Package, Asset, *Filename, Args);
	}

	UTexture2D* CreateDataTexture(const FString& Path, const TArray<FLinearColor>& Pixels, int32 Width, int32 Height)
	{
		auto* Texture = NewObject<UTexture2D>(CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path), RF_Public | RF_Standalone);
		Texture->Source.Init(Width, Height, 1, 1, TSF_RGBA32F, reinterpret_cast<const uint8*>(Pixels.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_HDR_F32;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->Filter = TF_Nearest;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->NeverStream = true;
		Texture->PostEditChange();
		return Texture;
	}

	void AddMaskInput(UMaterialExpressionCustom* Mask, const TCHAR* Name, UMaterialExpression* Expression)
	{
		auto& Input = Mask->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(0, Expression);
	}

	void AddMaskVector(UMaterial* Material, UMaterialExpressionCustom* Mask, const TCHAR* Name, const FVector& Default)
	{
		auto* Parameter = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass()));
		Parameter->ParameterName = FName(FString(TEXT("MP_")) + Name);
		Parameter->DefaultValue = FLinearColor(Default.X, Default.Y, Default.Z, 0);
		AddMaskInput(Mask, Name, Parameter);
	}

	void AddMaskScalar(UMaterial* Material, UMaterialExpressionCustom* Mask, const TCHAR* Name, float Default)
	{
		auto* Parameter = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass()));
		Parameter->ParameterName = FName(FString(TEXT("MP_")) + Name);
		Parameter->DefaultValue = Default;
		AddMaskInput(Mask, Name, Parameter);
	}

	void ConfigureProgressSpace(UMaterial* Material, UMaterialExpressionCustom* Mask)
	{
		const FName Names[] = {TEXT("ProgressX"), TEXT("ProgressY"), TEXT("ProgressZ")};
		for (auto Axis = 0; Axis < 3; ++Axis)
		{
			const auto Name = Names[Axis];
			if (Mask->Inputs.ContainsByPredicate([Name](const FCustomInput& Input) {
					return Input.InputName == Name;
				}))
			{
				continue;
			}

			auto* Parameter = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass()));
			Parameter->ParameterName = FName(FString(TEXT("MP_")) + Name.ToString());
			Parameter->DefaultValue = FLinearColor(Axis == 0 ? 1 : 0, Axis == 1 ? 1 : 0, Axis == 2 ? 1 : 0, 0);
			auto& Input = Mask->Inputs.AddDefaulted_GetRef();
			Input.InputName = Name;
			Input.Input.Connect(5, Parameter); // RGBAで平行移動も渡す。
		}
		for (const auto* Name : {TEXT("Origin1"), TEXT("Origin2"), TEXT("Origin3")})
		{
			if (Mask->Inputs.ContainsByPredicate([Name](const FCustomInput& Input) {
					return Input.InputName == Name;
				}) == false)
			{
				AddMaskVector(Material, Mask, Name, FVector::ZeroVector);
			}
		}
		for (const auto* Name : {TEXT("OriginCount"), TEXT("OriginDistance")})
		{
			if (Mask->Inputs.ContainsByPredicate([Name](const FCustomInput& Input) {
					return Input.InputName == Name;
				}) == false)
			{
				AddMaskScalar(Material, Mask, Name, 1);
			}
		}
		// 同じMIDで元の穴を評価するパスでは、粒子化用の消失だけを外す。
		Mask->Code = TEXT("#if MP_EVALUATE_COVERAGE\nreturn 1;\n#else\nfloat3 P = MP_ProgressPosition(Pos, ProgressX, ProgressY, ProgressZ); float Q = MP_Order(P, BoundsMin, BoundsMax, Direction, Origin, (int)Mode, Seed, Origin1, Origin2, Origin3, (int)OriginCount, OriginDistance); return Progress < 0 ? 1 : (Progress >= 1 ? 0 : (Q > Progress ? 1 : 0));\n#endif\n");
	}

	bool CompileDissolveMaterial(UMaterial* Material, FString& OutError)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			Resource->FinishCompilation();
			if (Resource->GetCompileErrors().IsEmpty() == false)
			{
				OutError = FString::Printf(TEXT("Dissolve material compilation failed: %s"), *FString::Join(Resource->GetCompileErrors(), TEXT("\n")));
				return false;
			}
		}
		return true;
	}

	bool ConfigureSurfaceTint(UMaterial* Material, FString& OutError)
	{
		for (const auto& Expression : Material->GetExpressions())
		{
			const auto* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression);
			if (Parameter != nullptr && Parameter->ParameterName == TEXT("MP_SurfaceTint"))
			{
				if (Parameter->Desc == TEXT("Mesh Particleizer registered surface tint"))
				{
					return true;
				}
				OutError = TEXT("MP_SurfaceTint is reserved for the generated surface color input. Rename the source parameter before preparation.");
				return false;
			}
		}

		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass()));
		Tint->ParameterName = TEXT("MP_SurfaceTint");
		Tint->DefaultValue = FLinearColor::White;
		Tint->Desc = TEXT("Mesh Particleizer registered surface tint");
		auto* Surface = Cast<UMaterialExpressionSaturate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSaturate::StaticClass()));
		auto* Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
		Multiply->A.Connect(0, Surface);
		Multiply->B.Connect(0, Tint);

		// 選んだ表面色をベイクと同じ範囲へ揃えてからTintを掛け、他の属性は保持する。
		const auto Property = Material->GetShadingModels().HasShadingModel(MSM_Unlit) ? MP_EmissiveColor : MP_BaseColor;
		if (Material->bUseMaterialAttributes == true)
		{
			auto* Attributes = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
			auto* Get = Cast<UMaterialExpressionGetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionGetMaterialAttributes::StaticClass()));
			Get->MaterialAttributes.Connect(Attributes->OutputIndex, Attributes->Expression);
			Surface->Input.Connect(Get->CreateOrGetOutputAttribute(Property), Get);
			auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSetMaterialAttributes::StaticClass()));
			Set->Inputs[0] = *Attributes;
			Set->ConnectInputAttribute(Property, Multiply);
			Attributes->Connect(0, Set);
		}
		else
		{
			auto& Base = Property == MP_EmissiveColor ? Material->GetEditorOnlyData()->EmissiveColor : Material->GetEditorOnlyData()->BaseColor;
			if (Base.UseConstant == true || Base.Expression == nullptr)
			{
				auto* Constant = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass()));
				Constant->Constant = Base.UseConstant == true ? Base.Constant : FLinearColor(FMaterialAttributeDefinitionMap::GetDefaultValue(Property));
				Surface->Input.Connect(0, Constant);
			}
			else
			{
				Surface->Input = Base;
			}
			Base.UseConstant = false;
			Base.Connect(0, Multiply);
		}
		return true;
	}

	UMaterialExpressionLinearInterpolate* CreateProviderSelection(UMaterial* Material, UTexture2D* DefaultTexture,
		const FExpressionInput& Original, const FName* Names, const FString& Marker)
	{
		auto* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		Sample->ParameterName = Names[0];
		Sample->Texture = DefaultTexture;
		Sample->SamplerType = SAMPLERTYPE_LinearColor;
		Sample->Desc = Marker;
		auto* UV = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass());
		auto* Transform = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass()));
		Transform->ParameterName = Names[1];
		Transform->DefaultValue = FLinearColor(1, 1, 0, 0);
		Transform->Desc = Marker;
		auto* Coordinates = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass()));
		Coordinates->OutputType = CMOT_Float2;
		Coordinates->Code = TEXT("return UV * Transform.xy + Transform.zw;");
		AddMaskInput(Coordinates, TEXT("UV"), UV);
		AddMaskInput(Coordinates, TEXT("Transform"), Transform);
		Coordinates->Inputs.Last().Input.Connect(5, Transform);
		Sample->Coordinates.Connect(0, Coordinates);
		auto* Enabled = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass()));
		Enabled->ParameterName = Names[2];
		Enabled->DefaultValue = 0;
		Enabled->Desc = Marker;
		auto* Select = Cast<UMaterialExpressionLinearInterpolate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionLinearInterpolate::StaticClass()));
		Select->A = Original;
		Select->B.Connect(0, Sample);
		Select->Alpha.Connect(0, Enabled);
		return Select;
	}

	bool ConfigureColorProvider(UMaterial* Material, UTexture2D* DefaultTexture, FString& OutError)
	{
		const FName Names[] = {TEXT("MP_ColorProviderTexture"), TEXT("MP_ColorProviderUV"), TEXT("MP_UseColorProvider")};
		const auto Marker = FString(TEXT("Mesh Particleizer registered color provider"));
		auto Existing = TSet<FName>{};
		UMaterialExpressionVectorParameter* Tint = nullptr;
		for (const auto& Expression : Material->GetExpressions())
		{
			if (Expression->HasAParameterName() && MakeArrayView(Names).Contains(Expression->GetParameterName()))
			{
				if (Expression->Desc != Marker || Existing.Contains(Expression->GetParameterName()))
				{
					OutError = TEXT("Color Provider parameters conflict with the source material. Rename the reserved parameters before preparation.");
					return false;
				}
				Existing.Add(Expression->GetParameterName());
			}
			auto* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression);
			if (Parameter != nullptr && Parameter->ParameterName == TEXT("MP_SurfaceTint") && Parameter->Desc == TEXT("Mesh Particleizer registered surface tint"))
			{
				Tint = Parameter;
			}
		}
		if (Existing.Num() == UE_ARRAY_COUNT(Names))
		{
			return true;
		}
		UMaterialExpressionSaturate* Surface = nullptr;
		for (const auto& Expression : Material->GetExpressions())
		{
			const auto* Multiply = Cast<UMaterialExpressionMultiply>(Expression);
			if (Multiply != nullptr && Multiply->B.Expression == Tint && Tint != nullptr)
			{
				Surface = Cast<UMaterialExpressionSaturate>(Multiply->A.Expression);
			}
		}
		if (Existing.IsEmpty() == false || Surface == nullptr || DefaultTexture == nullptr || DefaultTexture->SRGB == true)
		{
			OutError = TEXT("Color Provider needs a complete generated surface tint graph and a linear default texture.");
			return false;
		}

		// 選んだ表面色だけを公開色へ切り替え、透明部分と元パラメータを保持する。
		auto* Select = CreateProviderSelection(Material, DefaultTexture, Surface->Input, Names, Marker);
		Surface->Input.Connect(0, Select);
		return true;
	}

	bool ConfigureTextureColor(UMaterial* Material, FMeshParticleizerTextureColorSource& Binding, FString& OutError)
	{
		if (Binding.bCompatible == false)
		{
			return true;
		}
		const auto Name = FName(TEXT("MP_SurfaceTexture"));
		const auto Marker = FString(TEXT("Mesh Particleizer registered surface texture ")) + Binding.SampleExpressionGuid.ToString();
		UMaterialExpressionTextureSample* Sample = nullptr;
		for (const auto& Expression : Material->GetExpressions())
		{
			if (Expression->HasAParameterName() && Expression->GetParameterName() == Name)
			{
				if (Expression->IsA<UMaterialExpressionTextureSampleParameter2D>() && Expression->Desc == Marker)
				{
					Binding.RegisteredTextureParameter = Name;
					return true;
				}
				OutError = TEXT("MP_SurfaceTexture conflicts with an unregistered parameter or an older color graph. Prepare the source again.");
				return false;
			}
			if (Expression->MaterialExpressionGuid == Binding.SampleExpressionGuid)
			{
				Sample = Cast<UMaterialExpressionTextureSample>(Expression);
			}
		}
		if (Sample == nullptr)
		{
			OutError = TEXT("The generated material no longer contains the analyzed color sample. Prepare the source again.");
			return false;
		}

		// 元資産と元パラメータを残し、生成材内の対応サンプルだけを登録入力へ置き換える。
		auto* Parameter = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		Parameter->ParameterName = Name;
		Parameter->Desc = Marker;
		Parameter->Texture = Binding.Texture.Get();
		Parameter->SamplerType = Sample->SamplerType;
		Parameter->SamplerSource = Sample->SamplerSource;
		Parameter->Coordinates = Sample->Coordinates;
		Parameter->ConstCoordinate = Sample->ConstCoordinate;
		Parameter->AutomaticViewMipBias = Sample->AutomaticViewMipBias;
		for (const auto& Expression : Material->GetExpressions())
		{
			for (auto It = FExpressionInputIterator(Expression); It; ++It)
			{
				if (It.Input->Expression == Sample)
				{
					It.Input->Expression = Parameter;
				}
			}
		}
		for (auto Property = 0; Property < MP_MAX; ++Property)
		{
			auto* Input = Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Property));
			if (Input != nullptr && Input->Expression == Sample)
			{
				Input->Expression = Parameter;
			}
		}
		Binding.RegisteredTextureParameter = Name;
		return true;
	}

	bool ConfigureSourceEmission(UMaterial* Material, UTexture2D* DefaultTexture, FString& OutError)
	{
		const FName Names[] = {TEXT("MP_EmissionProviderTexture"), TEXT("MP_EmissionProviderUV"), TEXT("MP_UseEmissionProvider")};
		const auto Marker = FString(TEXT("Mesh Particleizer registered emission provider"));
		auto Existing = TSet<FName>{};
		UMaterialExpressionScalarParameter* Inherit = nullptr;
		for (const auto& Expression : Material->GetExpressions())
		{
			if (Expression->HasAParameterName() && MakeArrayView(Names).Contains(Expression->GetParameterName()))
			{
				if (Expression->Desc != Marker || Existing.Contains(Expression->GetParameterName()))
				{
					OutError = TEXT("HDR provider parameters conflict with the source material. Rename the reserved parameters.");
					return false;
				}
				Existing.Add(Expression->GetParameterName());
			}
			if (Expression->HasAParameterName() && Expression->GetParameterName() == TEXT("MP_InheritSourceEmission"))
			{
				if (Inherit != nullptr || Expression->IsA<UMaterialExpressionScalarParameter>() == false)
				{
					OutError = TEXT("Original emission playback parameter is ambiguous.");
					return false;
				}
				Inherit = Cast<UMaterialExpressionScalarParameter>(Expression);
			}
		}
		if (Existing.Num() == UE_ARRAY_COUNT(Names))
		{
			return true;
		}
		if (Existing.IsEmpty() == false || DefaultTexture == nullptr || DefaultTexture->SRGB)
		{
			OutError = TEXT("HDR provider needs complete generated inputs and a linear default texture.");
			return false;
		}

		// Litは表面色と独立して発光だけを置換する。他のMaterial Attributesをそのまま渡す。
		if (Material->GetShadingModels().HasShadingModel(MSM_Unlit) == false)
		{
			auto Original = FExpressionInput{};
			auto* Attributes = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
			auto& Emissive = Material->GetEditorOnlyData()->EmissiveColor;
			if (Material->bUseMaterialAttributes)
			{
				auto* Get = Cast<UMaterialExpressionGetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionGetMaterialAttributes::StaticClass()));
				Get->MaterialAttributes.Connect(Attributes->OutputIndex, Attributes->Expression);
				Original.Connect(Get->CreateOrGetOutputAttribute(MP_EmissiveColor), Get);
			}
			else if (Emissive.UseConstant || Emissive.Expression == nullptr)
			{
				auto* Constant = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass()));
				Constant->Constant = Emissive.UseConstant ? Emissive.Constant : FLinearColor::Black;
				Original.Connect(0, Constant);
			}
			else
			{
				Original = Emissive;
			}
			auto* Select = CreateProviderSelection(Material, DefaultTexture, Original, Names, Marker);
			if (Material->bUseMaterialAttributes)
			{
				auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSetMaterialAttributes::StaticClass()));
				Set->Inputs[0] = *Attributes;
				Set->ConnectInputAttribute(MP_EmissiveColor, Select);
				Attributes->Connect(0, Set);
			}
			else
			{
				Emissive.UseConstant = false;
				Emissive.Connect(0, Select);
			}
			return true;
		}

		UMaterialExpressionMultiply* TintMultiply = nullptr;
		for (const auto& Expression : Material->GetExpressions())
		{
			auto* Multiply = Cast<UMaterialExpressionMultiply>(Expression);
			const auto* Tint = Multiply != nullptr ? Cast<UMaterialExpressionVectorParameter>(Multiply->B.Expression) : nullptr;
			if (Tint != nullptr && Tint->ParameterName == TEXT("MP_SurfaceTint") && Tint->Desc == TEXT("Mesh Particleizer registered surface tint"))
			{
				TintMultiply = Multiply;
				break;
			}
		}
		auto* HDRSelect = TintMultiply != nullptr ? Cast<UMaterialExpressionLinearInterpolate>(TintMultiply->A.Expression) : nullptr;
		auto* Surface = TintMultiply != nullptr ? Cast<UMaterialExpressionSaturate>(HDRSelect != nullptr ? HDRSelect->A.Expression : TintMultiply->A.Expression) : nullptr;
		if (Surface == nullptr)
		{
			OutError = TEXT("Original emission needs the generated surface color graph.");
			return false;
		}
		if ((Inherit != nullptr && (HDRSelect == nullptr || HDRSelect->Alpha.Expression != Inherit)) ||
			(HDRSelect != nullptr && Inherit == nullptr))
		{
			OutError = TEXT("MP_InheritSourceEmission conflicts with the generated HDR selector.");
			return false;
		}
		const auto* ColorSelect = Cast<UMaterialExpressionLinearInterpolate>(Surface->Input.Expression);
		const auto* ColorEnabled = ColorSelect != nullptr ? Cast<UMaterialExpressionScalarParameter>(ColorSelect->Alpha.Expression) : nullptr;
		if (ColorEnabled == nullptr || ColorEnabled->ParameterName != TEXT("MP_UseColorProvider"))
		{
			OutError = TEXT("Original emission needs the generated color provider graph.");
			return false;
		}

		// 継承を有効にしたUnlit材は、残っている元メッシュもHDRのまま表示する。
		auto* Provider = CreateProviderSelection(Material, DefaultTexture, ColorSelect->A, Names, Marker);
		if (HDRSelect == nullptr)
		{
			Inherit = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass()));
			Inherit->ParameterName = TEXT("MP_InheritSourceEmission");
			Inherit->DefaultValue = 0;
			Inherit->Desc = Marker;
			HDRSelect = Cast<UMaterialExpressionLinearInterpolate>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionLinearInterpolate::StaticClass()));
			HDRSelect->A.Connect(0, Surface);
			HDRSelect->Alpha.Connect(0, Inherit);
			TintMultiply->A.Connect(0, HDRSelect);
		}
		HDRSelect->B.Connect(0, Provider);
		return true;
	}

	UMaterialInterface* CreateDissolveMaterial(UMaterialInterface* Source, const FString& Path,
		const FVector& BoundsMin, const FVector& BoundsMax, bool bSkeletal, TArray<UObject*>& OutAssets,
		FMeshParticleizerTextureColorSource& ColorBinding, UTexture2D* DefaultTexture, FString& OutError)
	{
		const auto ParentPath = Path + TEXT("_Master");
		auto* Material = DuplicateObject<UMaterial>(Source->GetMaterial(), CreatePackage(*ParentPath), *FPackageName::GetLongPackageAssetName(ParentPath));
		Material->SetFlags(RF_Public | RF_Standalone);
		Material->BlendMode = BLEND_Masked;

		// 複製元の不透明最適化を解除し、動的なOpacity Maskを描画に反映する。
		Material->bCanMaskedBeAssumedOpaque = false;
		if (bSkeletal == true)
		{
			Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
		}
		if (ColorBinding.bRequiresRuntimeCoverage)
		{
			Material->SetUsageByFlag(MATUSAGE_NiagaraMeshParticles, true);
		}

		auto* Mask = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, 600));
		Mask->Description = TEXT("Mesh Particleizer stable reference-space mask");
		Mask->OutputType = CMOT_Float1;
		Mask->Inputs.Reset();
		Mask->IncludeFilePaths.Add(TEXT("/Plugin/MeshParticleizer/Private/ParticleizerCommon.ush"));

		// 参照姿勢の位置を頂点からピクセルへ渡し、変形後も同じ分離順序を保つ。
		auto* ReferencePosition = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionPreSkinnedPosition::StaticClass());
		auto* Interpolator = Cast<UMaterialExpressionVertexInterpolator>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexInterpolator::StaticClass()));
		Interpolator->Input.Connect(0, ReferencePosition);
		AddMaskInput(Mask, TEXT("Pos"), Interpolator);
		AddMaskVector(Material, Mask, TEXT("BoundsMin"), BoundsMin);
		AddMaskVector(Material, Mask, TEXT("BoundsMax"), BoundsMax);
		AddMaskVector(Material, Mask, TEXT("Direction"), FVector(0, 0, -1));
		AddMaskVector(Material, Mask, TEXT("Origin"), FVector::ZeroVector);
		AddMaskScalar(Material, Mask, TEXT("Mode"), 0);
		AddMaskScalar(Material, Mask, TEXT("Seed"), 1337);
		AddMaskScalar(Material, Mask, TEXT("Progress"), -1);
		ConfigureProgressSpace(Material, Mask);

		// 既存の透明部分を保持して消失マスクを掛ける。
		auto* OriginalMask = Material->GetExpressionInputForProperty(MP_OpacityMask);
		if (Material->bUseMaterialAttributes == true)
		{
			auto* OriginalAttributes = Material->GetExpressionInputForProperty(MP_MaterialAttributes);
			auto* Get = Cast<UMaterialExpressionGetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionGetMaterialAttributes::StaticClass()));
			Get->MaterialAttributes.Connect(OriginalAttributes->OutputIndex, OriginalAttributes->Expression);
			const auto MaskOutput = Get->CreateOrGetOutputAttribute(MP_OpacityMask);
			auto* Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
			Multiply->A.Connect(MaskOutput, Get);
			Multiply->B.Connect(0, Mask);
			auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSetMaterialAttributes::StaticClass()));
			Set->Inputs[0] = *OriginalAttributes;
			Set->ConnectInputAttribute(MP_OpacityMask, Multiply);
			OriginalAttributes->Connect(0, Set);
		}
		else if (OriginalMask->Expression != nullptr)
		{
			auto* Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
			Multiply->A = *OriginalMask;
			Multiply->B.Connect(0, Mask);
			OriginalMask->Connect(0, Multiply);
		}
		else
		{
			OriginalMask->Connect(0, Mask);
		}
		if (ConfigureSurfaceTint(Material, OutError) == false || ConfigureTextureColor(Material, ColorBinding, OutError) == false ||
			ConfigureColorProvider(Material, DefaultTexture, OutError) == false ||
			ConfigureSourceEmission(Material, DefaultTexture, OutError) == false ||
			CompileDissolveMaterial(Material, OutError) == false)
		{
			return nullptr;
		}

		auto* Instance = NewObject<UMaterialInstanceConstant>(CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path), RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(Material);
		Instance->CopyMaterialUniformParametersEditorOnly(Source, true);
		Instance->PostEditChange();
		OutAssets.Add(Material);
		OutAssets.Add(Instance);
		return Instance;
	}
}

static UMeshParticleizerPreparedMesh* PrepareSource(
	const MeshParticleizerPreparation::FSurfaceSource& Source, const FString& Destination, int32 SampleCount, int32 Seed, FString& OutError)
{
	using namespace MeshParticleizerPreparation;
	OutError.Reset();
	if (Source.Asset == nullptr || Source.Description == nullptr || SampleCount < 1 || SampleCount > 250000 ||
		Destination.StartsWith(TEXT("/Game/")) == false || FPackageName::IsValidLongPackageName(Destination) == false)
	{
		OutError = TEXT("Select a mesh with available sampling LOD data, 1..250000 samples and a new /Game/ output folder.");
		return nullptr;
	}

	const auto DataPath = Destination / TEXT("DA_Prepared");
	if (FPackageName::DoesPackageExist(DataPath) || FindPackage(nullptr, *DataPath) != nullptr)
	{
		OutError = TEXT("Output already exists. Use a new preparation folder to preserve existing assets.");
		return nullptr;
	}
	auto Samples = FSurfaceSamples{};
	if (BakeSurfaceSamples(Source, SampleCount, Seed, Samples, OutError) == false)
	{
		return nullptr;
	}

	auto Assets = TArray<UObject*>{};
	auto* Data = NewObject<UMeshParticleizerPreparedMesh>(CreatePackage(*DataPath), TEXT("DA_Prepared"), RF_Public | RF_Standalone);
	Data->SourceMesh = Cast<UStaticMesh>(Source.Asset);
	Data->SkeletalMesh = Cast<USkeletalMesh>(Source.Asset);
	Data->DataVersion = UMeshParticleizerPreparedMesh::CurrentDataVersion;
	Data->ParticleSystem = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/MeshParticleizer/Core/NS_MeshParticleizer.NS_MeshParticleizer")));
	Data->GdfParticleSystem = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/MeshParticleizer/CoreGDF/NS_MeshParticleizer.NS_MeshParticleizer")));
	Data->SourceFingerprint = Source.Fingerprint;
	Data->SamplingLOD = Source.SamplingLOD;
	Data->EnabledSections = Source.EnabledSections;
	Data->SampleCount = SampleCount;
	Data->SurfaceArea = Samples.SurfaceArea;
	Data->SurfaceAreaVectors = MoveTemp(Samples.SurfaceAreaVectors);
	Data->BoundsMin = Source.Bounds.Min;
	Data->BoundsMax = Source.Bounds.Max;
	Data->BoneCount = Source.BoneCount;
	Data->bUsesUnlitEmissive = Source.bUseUnlitEmissive;
	if (Source.BoneCount > 0 && Data->SurfaceProjection.Build(Source.SkinVertices, Source.Description->Vertices().Num(), Source.SkinTriangles, Source.Description->Triangles().Num(), Source.BoneCount, OutError) == false)
	{
		return nullptr;
	}
	auto* PositionTexture = CreateDataTexture(Destination / TEXT("T_Positions"), Samples.Positions, Samples.Width, Samples.Height);
	auto* NormalTexture = CreateDataTexture(Destination / TEXT("T_Normals"), Samples.Normals, Samples.Width, Samples.Height);
	auto* ColorTexture = CreateDataTexture(Destination / TEXT("T_Colors"), Samples.Colors, Samples.Width, Samples.Height);
	auto* CoordinateTexture = CreateDataTexture(Destination / TEXT("T_Coordinates"), Samples.Coordinates, Samples.Width, Samples.Height);
	Data->Positions = PositionTexture;
	Data->Normals = NormalTexture;
	Data->Colors = ColorTexture;
	Data->Coordinates = CoordinateTexture;
	Assets.Append({PositionTexture, NormalTexture, ColorTexture, CoordinateTexture});
	if (Samples.CoverageTriangles.IsEmpty() == false)
	{
		auto* Triangles = CreateDataTexture(Destination / TEXT("T_CoverageTriangles"), Samples.CoverageTriangles, 512, Samples.CoverageTriangles.Num() / 512);
		Data->CoverageTriangles = Triangles;
		Data->CoverageDataVersion = UMeshParticleizerPreparedMesh::CurrentCoverageDataVersion;
		Data->CoverageTriangleCount = Samples.CoverageTriangleCount;
		Assets.Add(Triangles);
	}
	if (Source.bBakeSourceEmission == true)
	{
		auto* Emission = CreateDataTexture(Destination / TEXT("T_SourceEmission"), Samples.Emission, Samples.Width, Samples.Height);
		Data->SourceEmission = Emission;
		Assets.Add(Emission);
	}
	if (Source.BoneCount > 0)
	{
		auto* Vertices = CreateDataTexture(Destination / TEXT("T_SkinVertices"), Source.SkinVertices, 512, Source.SkinVertices.Num() / 512);
		auto* Triangles = CreateDataTexture(Destination / TEXT("T_SkinTriangles"), Source.SkinTriangles, 512, Source.SkinTriangles.Num() / 512);
		Data->SkinVertices = Vertices;
		Data->SkinTriangles = Triangles;
		Assets.Append({Vertices, Triangles});
	}
	for (auto Slot = 0; Slot < Samples.Materials.Num(); ++Slot)
	{
		Data->SourceMaterials.Add(Samples.Materials[Slot]);
		Data->TextureColorSources.Add(Samples.ColorSources[Slot]);
		auto* DissolveMaterial = CreateDissolveMaterial(Samples.Materials[Slot], Destination / FString::Printf(TEXT("MI_Dissolve_%d"), Slot), Data->BoundsMin, Data->BoundsMax, Source.BoneCount > 0, Assets, Data->TextureColorSources.Last(), ColorTexture, OutError);
		if (DissolveMaterial == nullptr)
		{
			return nullptr;
		}
		Data->DissolveMaterials.Add(DissolveMaterial);
		if (Data->ValidateBakedMaterialParameters(Slot, Samples.Materials[Slot], nullptr, OutError) == false)
		{
			return nullptr;
		}
	}
	Data->bSupportsSurfaceTint = true;
	Data->bSupportsColorProvider = true;
	Data->bSupportsEmissionProvider = true;
	if (UMeshParticleizerPreparationLibrary::RefreshPreparedResourceManifest(Data, OutError) == false)
	{
		return nullptr;
	}
	if (Data->CapturePreparationDependencies(OutError) == false)
	{
		return nullptr;
	}
	Assets.Add(Data);
	for (auto* Asset : Assets)
	{
		if (SaveAsset(Asset) == false)
		{
			OutError = FString::Printf(TEXT("Failed to save %s. Preparation is incomplete."), *Asset->GetPathName());
			return nullptr;
		}
	}
	return Data;
}

bool MeshParticleizerPreparation::BakeSurfaceSamples(UStaticMesh* Mesh, int32 SampleCount, int32 Seed, FSurfaceSamples& OutSamples, FString& OutError,
	bool bUseUnlitEmissive, bool bBakeSourceEmission)
{
	auto Description = FMeshDescription{};
	auto Source = FSurfaceSource{};
	if (BuildStaticSource(Mesh, Description, Source, OutError) == false)
	{
		return false;
	}
	Source.bUseUnlitEmissive = bUseUnlitEmissive;
	Source.bBakeSourceEmission = bBakeSourceEmission;
	return BakeSurfaceSamples(Source, SampleCount, Seed, OutSamples, OutError);
}

UMeshParticleizerPreparedMesh* UMeshParticleizerPreparationLibrary::PrepareStaticMesh(
	UStaticMesh* Mesh, const FString& Destination, int32 SampleCount, int32 Seed, FString& OutError, bool bUseUnlitEmissive, bool bBakeSourceEmission, int32 SamplingLOD)
{
	auto Description = FMeshDescription{};
	auto Source = MeshParticleizerPreparation::FSurfaceSource{};
	if (MeshParticleizerPreparation::BuildStaticSource(Mesh, Description, Source, OutError, SamplingLOD) == false)
	{
		return nullptr;
	}
	Source.bUseUnlitEmissive = bUseUnlitEmissive;
	Source.bBakeSourceEmission = bBakeSourceEmission;
	return PrepareSource(Source, Destination, SampleCount, Seed, OutError);
}

UMeshParticleizerPreparedMesh* UMeshParticleizerPreparationLibrary::PrepareSkeletalMesh(
	USkeletalMesh* Mesh, const FString& Destination, int32 SampleCount, int32 Seed, FString& OutError, bool bUseUnlitEmissive, bool bBakeSourceEmission, int32 SamplingLOD)
{
	auto Description = FMeshDescription{};
	auto Source = MeshParticleizerPreparation::FSurfaceSource{};
	if (MeshParticleizerPreparation::BuildSkeletalSource(Mesh, Description, Source, OutError, SamplingLOD) == false)
	{
		return nullptr;
	}
	Source.bUseUnlitEmissive = bUseUnlitEmissive;
	Source.bBakeSourceEmission = bBakeSourceEmission;
	return PrepareSource(Source, Destination, SampleCount, Seed, OutError);
}

bool MeshParticleizerPreparation::BakeSurfaceSamples(const FSurfaceSource& Source, int32 SampleCount, int32 Seed, FSurfaceSamples& OutSamples, FString& OutError)
{
	OutSamples = FSurfaceSamples{};
	if (Source.Asset == nullptr || SampleCount < 1 || SampleCount > 250000)
	{
		OutError = TEXT("A mesh and 1..250000 samples are required.");
		return false;
	}

	const auto* SourceDescription = Source.Description;
	if (SourceDescription == nullptr)
	{
		OutError = TEXT("The sampling LOD mesh description is unavailable.");
		return false;
	}

	// 元のUVや頂点は変更せず、作業用コピーへ重ならないベイク座標を追加する。
	auto BakeDescription = *SourceDescription;
	auto Remappings = FElementIDRemappings{};
	BakeDescription.Compact(Remappings);
	const auto* Description = &BakeDescription;
	auto OriginalTriangleIds = TArray<int32>{};
	OriginalTriangleIds.SetNum(Description->Triangles().Num());
	for (const auto Id : SourceDescription->Triangles().GetElementIDs())
	{
		OriginalTriangleIds[Remappings.GetRemappedTriangleID(Id).GetValue()] = Id.GetValue();
	}

	// 全スロットの消失と色取得が成立してから生成へ進む。
	auto SourceMaterials = Source.Materials;
	if (AnalyzeSurfaceMaterials(SourceMaterials, Source.bUseUnlitEmissive, Source.bBakeSourceEmission, OutSamples.ColorSources, OutError) == false)
	{
		return false;
	}

	const auto Attributes = FStaticMeshConstAttributes(*Description);
	const auto Positions = Attributes.GetVertexPositions();
	const auto Normals = Attributes.GetVertexInstanceNormals();
	const auto UVs = Attributes.GetVertexInstanceUVs();
	const auto MaterialNames = Attributes.GetPolygonGroupMaterialSlotNames();
	if (UVs.GetNumChannels() == 0)
	{
		OutError = TEXT("UV0 is required for the initial baked surface color path.");
		return false;
	}

	constexpr auto BakeResolution = 1024;
	auto UVOptions = FStaticMeshOperations::FGenerateUVOptions{};
	UVOptions.TextureResolution = BakeResolution;
	UVOptions.UVMethod = FStaticMeshOperations::EGenerateUVMethod::Legacy;
	UVOptions.bMergeTrianglesWithIdenticalAttributes = false;
	auto BakeUVs = TArray<FVector2D>{};
	if (FStaticMeshOperations::GenerateUV(*Description, UVOptions, BakeUVs) == false)
	{
		OutError = TEXT("Could not create non-overlapping surface color coordinates.");
		return false;
	}
	auto MutableUVs = FStaticMeshAttributes(BakeDescription).GetVertexInstanceUVs();
	const auto BakeChannel = MutableUVs.GetNumChannels();
	MutableUVs.SetNumChannels(BakeChannel + 1);
	for (const auto Instance : Description->VertexInstances().GetElementIDs())
	{
		MutableUVs.Set(Instance, BakeChannel, FVector2f(BakeUVs[Instance.GetValue()]));
	}

	auto Triangles = TArray<FTriangle>{};
	auto TotalArea = 0.0;
	for (const auto Id : Description->Triangles().GetElementIDs())
	{
		auto Triangle = FTriangle{};
		Triangle.TriangleId = OriginalTriangleIds[Id.GetValue()];
		Triangle.MaterialIndex = Source.MaterialSlotNames.Find(MaterialNames[Description->GetTrianglePolygonGroup(Id)]);
		if (SourceMaterials.IsValidIndex(Triangle.MaterialIndex) == false)
		{
			OutError = TEXT("A polygon group does not match a material slot.");
			return false;
		}
		const auto Instances = Description->GetTriangleVertexInstances(Id);
		for (auto Corner = 0; Corner < 3; ++Corner)
		{
			Triangle.Positions[Corner] = Positions[Description->GetVertexInstanceVertex(Instances[Corner])];
			Triangle.Normals[Corner] = Normals[Instances[Corner]];
			Triangle.UVs[Corner] = UVs.Get(Instances[Corner], 0);
			Triangle.BakeUVs[Corner] = MutableUVs.Get(Instances[Corner], BakeChannel);
			if (Triangle.Positions[Corner].ContainsNaN() || Triangle.Normals[Corner].ContainsNaN() || Triangle.UVs[Corner].ContainsNaN())
			{
				OutError = TEXT("Mesh contains non-finite position, normal or UV data.");
				return false;
			}
		}
		const auto AreaVector = FVector::CrossProduct(FVector(Triangle.Positions[1]) - FVector(Triangle.Positions[0]),
									FVector(Triangle.Positions[2]) - FVector(Triangle.Positions[0])) *
			0.5;
		const auto Area = AreaVector.Length();
		if (Area > SMALL_NUMBER)
		{
			TotalArea += Area;
			Triangle.CumulativeArea = TotalArea;
			Triangles.Add(Triangle);
			OutSamples.SurfaceAreaVectors.Add(AreaVector);
		}
	}
	if (Triangles.IsEmpty() || FMath::IsFinite(TotalArea) == false)
	{
		OutError = TEXT("Mesh has no finite non-degenerate surface area.");
		return false;
	}

	if (OutSamples.ColorSources.ContainsByPredicate([](const auto& Slot) {
			return Slot.bRequiresRuntimeCoverage;
		}))
	{
		auto MaxTriangle = 0;
		for (const auto& Triangle : Triangles)
		{
			MaxTriangle = FMath::Max(MaxTriangle, Triangle.TriangleId);
		}
		const auto Stride = Source.BoneCount > 0 ? 2 : 8;
		const auto Rows = (int64{MaxTriangle} + 1) * Stride;
		if (Rows > 512ll * GMaxTextureDimensions)
		{
			OutError = TEXT("Runtime coverage geometry exceeds the supported prepared texture size.");
			return false;
		}
		OutSamples.CoverageTriangles.SetNumZeroed(FMath::DivideAndRoundUp(static_cast<int32>(Rows), 512) * 512);
		OutSamples.CoverageTriangleCount = MaxTriangle + 1;
		for (const auto& Triangle : Triangles)
		{
			const auto Offset = Triangle.TriangleId * Stride;

			// ラスタ補間に元頂点の値を使う。候補や辺からの逆算による丸めを避ける。
			OutSamples.CoverageTriangles[Offset] = FLinearColor(Triangle.UVs[0].X, Triangle.UVs[0].Y, Triangle.UVs[1].X, Triangle.UVs[1].Y);
			OutSamples.CoverageTriangles[Offset + 1] = FLinearColor(Triangle.UVs[2].X, Triangle.UVs[2].Y, Triangle.MaterialIndex, 1);
			if (Source.BoneCount == 0)
			{
				for (auto Corner = 0; Corner < 3; ++Corner)
				{
					const auto Position = Triangle.Positions[Corner];
					const auto Normal = Triangle.Normals[Corner];
					OutSamples.CoverageTriangles[Offset + 2 + Corner] = FLinearColor(Position.X, Position.Y, Position.Z, 0);
					OutSamples.CoverageTriangles[Offset + 5 + Corner] = FLinearColor(Normal.X, Normal.Y, Normal.Z, 0);
				}
			}
		}
	}

	// LitはBase Color、明示選択したUnlitはEmissiveを照明なしで取得する。
	auto MaterialSettings = TArray<FMaterialData>{};
	auto MeshSettings = TArray<FMeshData>{};
	MaterialSettings.SetNum(SourceMaterials.Num());
	MeshSettings.SetNum(SourceMaterials.Num());
	auto MaterialPointers = TArray<FMaterialData*>{};
	auto MeshPointers = TArray<FMeshData*>{};
	for (auto Slot = 0; Slot < SourceMaterials.Num(); ++Slot)
	{
		MaterialSettings[Slot].Material = SourceMaterials[Slot];
		const auto Property = SourceMaterials[Slot]->GetShadingModels().HasShadingModel(MSM_Unlit) ? MP_EmissiveColor : MP_BaseColor;
		MaterialSettings[Slot].PropertySizes.Add(Property, FIntPoint(BakeResolution, BakeResolution));
		MaterialSettings[Slot].PropertySizes.Add(MP_OpacityMask, FIntPoint(BakeResolution, BakeResolution));
		if (Source.bBakeSourceEmission == true)
		{
			MaterialSettings[Slot].PropertySizes.Add(MP_EmissiveColor, FIntPoint(BakeResolution, BakeResolution));
		}
		MaterialSettings[Slot].bPerformShrinking = false;
		MaterialSettings[Slot].bPerformBorderSmear = false;
		MeshSettings[Slot].MeshDescription = Description;
		MeshSettings[Slot].Mesh = Cast<UStaticMesh>(Source.Asset);
		MeshSettings[Slot].TextureCoordinateIndex = BakeChannel;
		// Bakerが要求する値はスロット番号ではなく、MeshDescriptionのPolygonGroup ID。
		for (const auto Group : Description->PolygonGroups().GetElementIDs())
		{
			if (Source.MaterialSlotNames.Find(MaterialNames[Group]) == Slot)
			{
				MeshSettings[Slot].MaterialIndices.Add(Group.GetValue());
			}
		}
		MeshSettings[Slot].PrimitiveData = FPrimitiveData(FBoxSphereBounds(Source.Bounds));
		MaterialPointers.Add(&MaterialSettings[Slot]);
		MeshPointers.Add(&MeshSettings[Slot]);
	}
	auto Outputs = TArray<FBakeOutput>{};
	auto& Baker = FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
	auto bPreviousHDR = false;
	if (Source.bBakeSourceEmission == true && ReadEmissionBakeState(Baker, bPreviousHDR, OutError) == false)
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (Source.bBakeSourceEmission == true)
		{
			Baker.SetEmissiveHDR(bPreviousHDR);
		}
	};
	if (Source.bBakeSourceEmission == true)
	{
		Baker.SetEmissiveHDR(true);
	}
	if (Source.bUseUnlitEmissive == true)
	{
		// HDRの最大値で8bit全体を正規化すると暗い色が失われる。表面色用ベイクだけ0..1へ制限する。
		auto* ClampEmissive = IConsoleManager::Get().FindConsoleVariable(TEXT("MaterialBaking.ForceDisableEmissiveScaling"));
		if (ClampEmissive == nullptr)
		{
			OutError = TEXT("The material baker cannot preserve Unlit surface color precision.");
			return false;
		}
		const auto Tag = FName(TEXT("MeshParticleizer.UnlitSurfaceBake"));
		ClampEmissive->Set(true, ECVF_SetByTemp, Tag);
		if (ClampEmissive->GetBool() == false)
		{
			ClampEmissive->Unset(ECVF_SetByTemp, Tag);
			OutError = TEXT("A console override disables Unlit bake clamping. Reset MaterialBaking.ForceDisableEmissiveScaling before preparation.");
			return false;
		}
		Baker.BakeMaterials(MaterialPointers, MeshPointers, Outputs);
		ClampEmissive->Unset(ECVF_SetByTemp, Tag);
	}
	else
	{
		Baker.BakeMaterials(MaterialPointers, MeshPointers, Outputs);
	}
	if (Outputs.Num() != SourceMaterials.Num())
	{
		OutError = TEXT("Material baking did not return every material slot.");
		return false;
	}
	for (auto Slot = 0; Slot < Outputs.Num(); ++Slot)
	{
		auto& Output = Outputs[Slot];
		const auto Property = SourceMaterials[Slot]->GetShadingModels().HasShadingModel(MSM_Unlit) ? MP_EmissiveColor : MP_BaseColor;
		if (Output.PropertyData.Contains(Property) == false || Output.PropertyData[Property].IsEmpty() ||
			Output.PropertyData.Contains(MP_OpacityMask) == false || Output.PropertyData[MP_OpacityMask].IsEmpty())
		{
			OutError = TEXT("Material baking returned empty surface color or opacity data.");
			return false;
		}

		// 表面のマゼンタを背景と誤認しない。スカラーのOpacity Maskで空白だけを識別してから縁を埋める。
		auto& ColorData = Output.PropertyData[Property];
		auto& MaskData = Output.PropertyData[MP_OpacityMask];
		auto& ColorSize = Output.PropertySizes[Property];
		auto& MaskSize = Output.PropertySizes[MP_OpacityMask];
		if (ColorData.Num() != MaskData.Num() || ColorSize != MaskSize)
		{
			OutError = TEXT("Surface color and coverage baking returned different atlas layouts.");
			return false;
		}
		if (Source.bBakeSourceEmission == true)
		{
			auto* HDR = Output.HDRPropertyData.Find(MP_EmissiveColor);
			if (HDR == nullptr || HDR->Num() != MaskData.Num() || Output.PropertySizes.FindRef(MP_EmissiveColor) != MaskSize)
			{
				OutError = TEXT("Original emission baking returned no HDR atlas or a mismatched layout.");
				return false;
			}

			// HDR値を8bitへ変換せず、元Maskで識別した空白を最寄りの有効画素で埋める。
			auto Filled = TBitArray<>(false, HDR->Num());
			auto Queue = TArray<int32>{};
			Queue.Reserve(HDR->Num());
			for (auto Pixel = 0; Pixel < HDR->Num(); ++Pixel)
			{
				if (MaskData[Pixel] == FColor::Magenta)
				{
					continue;
				}
				const auto Value = (*HDR)[Pixel].GetFloats();
				if (FMath::IsFinite(Value.R) == false || FMath::IsFinite(Value.G) == false || FMath::IsFinite(Value.B) == false ||
					Value.R < 0 || Value.G < 0 || Value.B < 0)
				{
					OutError = TEXT("Original emission must contain finite, non-negative HDR values representable by the material baker.");
					return false;
				}
				Filled[Pixel] = true;
				Queue.Add(Pixel);
			}
			if (Queue.IsEmpty())
			{
				OutError = TEXT("Original emission atlas contains no covered surface.");
				return false;
			}
			for (auto Head = 0; Head < Queue.Num(); ++Head)
			{
				const auto Pixel = Queue[Head];
				const auto X = Pixel % MaskSize.X;
				const auto Y = Pixel / MaskSize.X;
				const int32 Neighbors[] = {X > 0 ? Pixel - 1 : INDEX_NONE, X + 1 < MaskSize.X ? Pixel + 1 : INDEX_NONE,
					Y > 0 ? Pixel - MaskSize.X : INDEX_NONE, Y + 1 < MaskSize.Y ? Pixel + MaskSize.X : INDEX_NONE};
				for (const auto Neighbor : Neighbors)
				{
					if (Neighbor == INDEX_NONE || Filled[Neighbor] == true)
					{
						continue;
					}
					(*HDR)[Neighbor] = (*HDR)[Pixel];
					Filled[Neighbor] = true;
					Queue.Add(Neighbor);
				}
			}
		}
		const auto Empty = FColor(255, 0, 255, 0);
		for (auto Pixel = 0; Pixel < ColorData.Num(); ++Pixel)
		{
			if (MaskData[Pixel] == FColor::Magenta)
			{
				ColorData[Pixel] = Empty;
			}
			else
			{
				ColorData[Pixel].A = 255;
			}
		}
		if (Source.bBakeSourceEmission == true)
		{
			// HDRと色・被覆を同じ座標で読むため、任意の単色縮小は行わない。
			FMaterialBakingHelpers::PerformUVBorderSmear(ColorData, ColorSize.X, ColorSize.Y, -1, Empty);
			FMaterialBakingHelpers::PerformUVBorderSmear(MaskData, MaskSize.X, MaskSize.Y, -1, FColor::Magenta);
		}
		else
		{
			FMaterialBakingHelpers::PerformUVBorderSmearAndShrink(ColorData, ColorSize.X, ColorSize.Y, Empty);
			FMaterialBakingHelpers::PerformUVBorderSmearAndShrink(MaskData, MaskSize.X, MaskSize.Y, FColor::Magenta);
		}
	}

	constexpr auto Width = 512;
	const auto Height = FMath::DivideAndRoundUp(SampleCount, Width);
	auto PositionPixels = TArray<FLinearColor>{};
	auto NormalPixels = TArray<FLinearColor>{};
	auto ColorPixels = TArray<FLinearColor>{};
	auto EmissionPixels = TArray<FLinearColor>{};
	auto CoordinatePixels = TArray<FLinearColor>{};
	PositionPixels.SetNumZeroed(Width * Height);
	NormalPixels.SetNumZeroed(Width * Height);
	ColorPixels.SetNumZeroed(Width * Height);
	if (Source.bBakeSourceEmission == true)
	{
		EmissionPixels.SetNumZeroed(Width * Height);
	}
	CoordinatePixels.SetNumZeroed(Width * Height);
	auto Random = FRandomStream(Seed);
	auto Accepted = 0;
	for (auto Attempt = 0; Accepted < SampleCount && Attempt < SampleCount * 32; ++Attempt)
	{
		const auto TargetArea = Random.FRand() * TotalArea;
		auto Low = 0;
		auto High = Triangles.Num() - 1;
		while (Low < High)
		{
			const auto Mid = (Low + High) / 2;
			if (Triangles[Mid].CumulativeArea < TargetArea)
			{
				Low = Mid + 1;
			}
			else
			{
				High = Mid;
			}
		}
		const auto& Triangle = Triangles[Low];
		const auto Root = FMath::Sqrt(Random.FRand());
		const auto Bary = FVector3f(1 - Root, Root * (1 - Random.FRand()), 0);
		const auto Weights = FVector3f(Bary.X, Bary.Y, 1 - Bary.X - Bary.Y);
		const auto UV = Triangle.UVs[0] * Weights.X + Triangle.UVs[1] * Weights.Y + Triangle.UVs[2] * Weights.Z;
		const auto BakeUV = Triangle.BakeUVs[0] * Weights.X + Triangle.BakeUVs[1] * Weights.Y + Triangle.BakeUVs[2] * Weights.Z;
		const auto& Output = Outputs[Triangle.MaterialIndex];
		auto Sample = [&Output, BakeUV](EMaterialProperty Property) {
			const auto Size = Output.PropertySizes[Property];
			const auto X = FMath::Clamp(FMath::FloorToInt(BakeUV.X * Size.X), 0, Size.X - 1);
			const auto Y = FMath::Clamp(FMath::FloorToInt(BakeUV.Y * Size.Y), 0, Size.Y - 1);
			return Output.PropertyData[Property][Y * Size.X + X];
		};
		if (OutSamples.ColorSources[Triangle.MaterialIndex].bRequiresRuntimeCoverage == false &&
			SourceMaterials[Triangle.MaterialIndex]->GetBlendMode() == BLEND_Masked &&
			Sample(MP_OpacityMask).R / 255.0f < SourceMaterials[Triangle.MaterialIndex]->GetOpacityMaskClipValue())
		{
			continue;
		}

		const auto P = Triangle.Positions[0] * Weights.X + Triangle.Positions[1] * Weights.Y + Triangle.Positions[2] * Weights.Z;
		const auto N = (Triangle.Normals[0] * Weights.X + Triangle.Normals[1] * Weights.Y + Triangle.Normals[2] * Weights.Z).GetSafeNormal();
		const auto Property = SourceMaterials[Triangle.MaterialIndex]->GetShadingModels().HasShadingModel(MSM_Unlit) ? MP_EmissiveColor : MP_BaseColor;
		const auto Color = Sample(Property);
		PositionPixels[Accepted] = FLinearColor(P.X, P.Y, P.Z, Triangle.MaterialIndex);
		NormalPixels[Accepted] = FLinearColor(N.X, N.Y, N.Z, Triangle.TriangleId);
		auto LinearColor = Output.PropertyIsLinearColor.FindRef(Property) ? Color.ReinterpretAsLinear() : FLinearColor(Color);
		// Bakerの色空間と倍率に従って線形RGBへ戻す。表面色用の倍率は1。
		if (Property == MP_EmissiveColor)
		{
			LinearColor *= Output.EmissiveScale;
		}
		ColorPixels[Accepted] = LinearColor.GetClamped();
		ColorPixels[Accepted].A = 1;
		if (Source.bBakeSourceEmission == true)
		{
			const auto Size = Output.PropertySizes[MP_EmissiveColor];
			const auto X = FMath::Clamp(FMath::FloorToInt(BakeUV.X * Size.X), 0, Size.X - 1);
			const auto Y = FMath::Clamp(FMath::FloorToInt(BakeUV.Y * Size.Y), 0, Size.Y - 1);
			EmissionPixels[Accepted] = Output.HDRPropertyData[MP_EmissiveColor][Y * Size.X + X].GetFloats();
			EmissionPixels[Accepted].A = 1;
		}
		CoordinatePixels[Accepted] = FLinearColor(UV.X, UV.Y, Weights.X, Weights.Y);
		++Accepted;
	}
	if (Accepted != SampleCount)
	{
		OutError = TEXT("Too little opaque surface to produce the requested samples. Reduce samples or provide a coverage adapter.");
		return false;
	}

	OutSamples.Positions = MoveTemp(PositionPixels);
	OutSamples.Normals = MoveTemp(NormalPixels);
	OutSamples.Colors = MoveTemp(ColorPixels);
	OutSamples.Emission = MoveTemp(EmissionPixels);
	OutSamples.Coordinates = MoveTemp(CoordinatePixels);
	OutSamples.Materials = MoveTemp(SourceMaterials);
	OutSamples.SurfaceArea = TotalArea;
	OutSamples.Width = Width;
	OutSamples.Height = Height;
	return true;
}

FString UMeshParticleizerPreparationLibrary::InspectMaterialSupport(UObject* Mesh, bool bUseUnlitEmissive, bool bBakeSourceEmission)
{
	auto Materials = TArray<UMaterialInterface*>{};
	if (const auto* Static = Cast<UStaticMesh>(Mesh))
	{
		for (const auto& Slot : Static->GetStaticMaterials())
		{
			Materials.Add(Slot.MaterialInterface);
		}
	}
	else if (const auto* Skeletal = Cast<USkeletalMesh>(Mesh))
	{
		for (const auto& Slot : Skeletal->GetMaterials())
		{
			Materials.Add(Slot.MaterialInterface);
		}
	}
	else
	{
		return TEXT("Select a Static or Skeletal Mesh.");
	}

	auto Sources = TArray<FMeshParticleizerTextureColorSource>{};
	auto Error = FString{};
	if (MeshParticleizerPreparation::AnalyzeSurfaceMaterials(Materials, bUseUnlitEmissive, bBakeSourceEmission, Sources, Error) == false)
	{
		return TEXT("Material preparation is blocked. No surface data was generated.\n\n") + Error;
	}

	auto Report = FString::Printf(TEXT("All %d material slots support preparation. No surface data was generated."), Materials.Num());
	for (auto Slot = 0; Slot < Sources.Num(); ++Slot)
	{
		if (Sources[Slot].DependencyDiagnostics.IsEmpty() == false)
		{
			Report += FString::Printf(TEXT("\n\nSlot %d (%s): %s"), Slot, *GetNameSafe(Materials[Slot]), *Sources[Slot].DependencyDiagnostics);
		}
	}
	return Report;
}

bool UMeshParticleizerPreparationLibrary::UpgradePreparedDependencies(UMeshParticleizerPreparedMesh* Prepared, FString& OutError)
{
	using namespace MeshParticleizerPreparation;
	if (Prepared == nullptr || (Prepared->DataVersion < 2 || Prepared->DataVersion > UMeshParticleizerPreparedMesh::CurrentDataVersion))
	{
		OutError = TEXT("Only version 2 through current surface data can be upgraded. Prepare older data again.");
		return false;
	}

	// 全スロットを先に検証し、未対応の穴や古いベイクを生成材の更新で隠さない。
	auto ColorSources = TArray<FMeshParticleizerTextureColorSource>{};
	if (Prepared->SourceMaterials.Num() != Prepared->DissolveMaterials.Num())
	{
		OutError = TEXT("Prepared material layout is incomplete. Prepare this mesh again.");
		return false;
	}
	auto SourceMaterials = TArray<UMaterialInterface*>{};
	auto Errors = TArray<FString>{};
	for (auto Slot = 0; Slot < Prepared->SourceMaterials.Num(); ++Slot)
	{
		auto* Source = Prepared->SourceMaterials[Slot].LoadSynchronous();
		SourceMaterials.Add(Source);
		if (Source != nullptr && Source->GetShadingModels().HasShadingModel(MSM_Unlit) && Prepared->bUsesUnlitEmissive == false)
		{
			Errors.Add(FString::Printf(TEXT("Slot %d (%s): Existing Unlit data must be baked again with Use Unlit Emissive. Dependency upgrades preserve the old surface colors."), Slot, *GetNameSafe(Source)));
			continue;
		}
		Prepared->DissolveMaterials[Slot].LoadSynchronous();
		auto Error = FString{};
		if (Prepared->ValidateBakedMaterialParameters(Slot, Source, nullptr, Error) == false)
		{
			Errors.Add(FString::Printf(TEXT("Slot %d (%s): %s"), Slot, *GetNameSafe(Source), *Error));
		}
	}
	auto MaterialError = FString{};
	if (AnalyzeSurfaceMaterials(SourceMaterials, Prepared->bUsesUnlitEmissive, Prepared->SourceEmission.IsNull() == false, ColorSources, MaterialError) == false)
	{
		Errors.Add(MaterialError);
	}
	if (Errors.IsEmpty() == false)
	{
		OutError = FString::Join(Errors, TEXT("\n\n"));
		return false;
	}
	// 他スロット用の三角形Textureがあっても、新たに動的と判定した面の候補は復元できない。
	for (auto Slot = 0; Slot < ColorSources.Num(); ++Slot)
	{
		if (ColorSources[Slot].bRequiresRuntimeCoverage == true &&
			(Prepared->TextureColorSources.IsValidIndex(Slot) == false || Prepared->TextureColorSources[Slot].bRequiresRuntimeCoverage == false))
		{
			OutError = FString::Printf(TEXT("Slot %d now requires runtime Opacity Mask. Prepare the mesh again to restore candidates removed by the previous fixed mask."), Slot);
			return false;
		}
	}
	if (ColorSources.ContainsByPredicate([](const auto& Slot) {
			return Slot.bRequiresRuntimeCoverage;
		}) &&
		(Prepared->CoverageDataVersion != UMeshParticleizerPreparedMesh::CurrentCoverageDataVersion ||
		Prepared->CoverageTriangleCount <= 0 || Prepared->CoverageTriangles.IsNull()))
	{
		OutError = TEXT("Dynamic Opacity Mask requires a new full-surface candidate bake. Dependency upgrades cannot restore candidates removed by the old mask.");
		return false;
	}
	if (Prepared->DataVersion < 4 || Prepared->bSupportsSurfaceTint == false)
	{
		auto Masks = TMap<UMaterial*, UMaterialExpressionCustom*>{};
		const auto Folder = FPackageName::GetLongPackagePath(Prepared->GetOutermost()->GetName()) + TEXT("/");
		for (const auto& Reference : Prepared->DissolveMaterials)
		{
			auto* Instance = Reference.LoadSynchronous();
			auto* Material = Instance != nullptr ? Instance->GetMaterial() : nullptr;
			UMaterialExpressionCustom* Mask = nullptr;
			if (Material != nullptr && Material->GetOutermost()->GetName().StartsWith(Folder))
			{
				for (const auto& Expression : Material->GetExpressions())
				{
					auto* Custom = Cast<UMaterialExpressionCustom>(Expression);
					if (Custom != nullptr && Custom->Description == TEXT("Mesh Particleizer stable reference-space mask"))
					{
						Mask = Custom;
						break;
					}
				}
			}
			if (Mask == nullptr)
			{
				OutError = TEXT("The generated dissolve material is missing or externally replaced. Prepare this mesh again.");
				return false;
			}
			Masks.Add(Material, Mask);
		}
		for (const auto& Pair : Masks)
		{
			Pair.Key->Modify();
			if (Prepared->DataVersion < 4)
			{
				ConfigureProgressSpace(Pair.Key, Pair.Value);
			}
			if (ConfigureSurfaceTint(Pair.Key, OutError) == false)
			{
				return false;
			}
			if (CompileDissolveMaterial(Pair.Key, OutError) == false || SaveAsset(Pair.Key) == false)
			{
				return false;
			}
		}
		Prepared->Modify();
		Prepared->bSupportsSurfaceTint = true;
		Prepared->MarkPackageDirty();
	}
	if (Prepared->ParticleSystem.IsNull() == true)
	{
		Prepared->Modify();
		Prepared->ParticleSystem = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/MeshParticleizer/Core/NS_MeshParticleizer.NS_MeshParticleizer")));
		Prepared->MarkPackageDirty();
	}
	if (Prepared->GdfParticleSystem.IsNull() == true)
	{
		Prepared->GdfParticleSystem = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/MeshParticleizer/CoreGDF/NS_MeshParticleizer.NS_MeshParticleizer")));
		Prepared->MarkPackageDirty();
	}

	const auto Folder = FPackageName::GetLongPackagePath(Prepared->GetOutermost()->GetName()) + TEXT("/");
	for (auto Slot = 0; Slot < ColorSources.Num(); ++Slot)
	{
		auto& Binding = ColorSources[Slot];
		auto* Instance = Cast<UMaterialInstanceConstant>(Prepared->DissolveMaterials[Slot].Get());
		auto* Material = Instance != nullptr ? Instance->GetMaterial() : nullptr;
		if (Material == nullptr || Material->GetOutermost()->GetName().StartsWith(Folder) == false)
		{
			OutError = TEXT("The generated color material is missing or externally replaced.");
			return false;
		}
		Material->Modify();
		if (Binding.bRequiresRuntimeCoverage)
		{
			Material->SetUsageByFlag(MATUSAGE_NiagaraMeshParticles, true);
		}
		if (ConfigureTextureColor(Material, Binding, OutError) == false ||
			ConfigureColorProvider(Material, Prepared->Colors.LoadSynchronous(), OutError) == false ||
			ConfigureSourceEmission(Material, Prepared->Colors.Get(), OutError) == false ||
			CompileDissolveMaterial(Material, OutError) == false || SaveAsset(Material) == false)
		{
			return false;
		}
		if (Binding.bCompatible)
		{
			Instance->SetTextureParameterValueEditorOnly(Binding.RegisteredTextureParameter, Binding.Texture.Get());
		}
		if (SaveAsset(Instance) == false)
		{
			return false;
		}
	}
	Prepared->Modify();
	Prepared->TextureColorSources = MoveTemp(ColorSources);
	Prepared->bSupportsColorProvider = true;
	Prepared->bSupportsEmissionProvider = true;
	if (RefreshPreparedResourceManifest(Prepared, OutError) == false)
	{
		return false;
	}
	Prepared->DataVersion = UMeshParticleizerPreparedMesh::CurrentDataVersion;
	return true;
}

bool UMeshParticleizerPreparationLibrary::PrepareSurfaceProjection(UMeshParticleizerPreparedMesh* Prepared, FString& OutError)
{
	OutError.Reset();
	if (Prepared == nullptr || Prepared->DataVersion != UMeshParticleizerPreparedMesh::CurrentDataVersion)
	{
		OutError = TEXT("Select current skeletal preparation data.");
		return false;
	}
	auto Description = FMeshDescription{};
	auto Source = MeshParticleizerPreparation::FSurfaceSource{};
	if (MeshParticleizerPreparation::BuildSkeletalSource(Prepared->SkeletalMesh.LoadSynchronous(), Description, Source, OutError, Prepared->SamplingLOD) == false)
	{
		return false;
	}
	if (Source.Fingerprint != Prepared->SourceFingerprint || Source.BoneCount != Prepared->BoneCount)
	{
		OutError = TEXT("Source mesh changed. Regenerate the complete preparation instead of updating only projection data.");
		return false;
	}
	auto Projection = FMeshParticleizerSurfaceProjection{};
	if (Projection.Build(Source.SkinVertices, Description.Vertices().Num(), Source.SkinTriangles,
			Description.Triangles().Num(), Source.BoneCount, OutError) == false)
	{
		return false;
	}

	Prepared->Modify();
	Prepared->SurfaceProjection = MoveTemp(Projection);
	Prepared->MarkPackageDirty();
	return true;
}

bool UMeshParticleizerPreparationLibrary::RefreshPreparedResourceManifest(UMeshParticleizerPreparedMesh* Prepared, FString& OutError)
{
	OutError.Reset();
	if (Prepared == nullptr)
	{
		OutError = TEXT("Select prepared mesh data.");
		return false;
	}
	Prepared->ParticleSystem.LoadSynchronous();
	Prepared->GdfParticleSystem.LoadSynchronous();
	for (const auto& Reference : {Prepared->Positions, Prepared->Normals, Prepared->Colors})
	{
		Reference.LoadSynchronous();
	}
	if (Prepared->SourceEmission.IsNull() == false)
	{
		Prepared->SourceEmission.LoadSynchronous();
	}
	if (Prepared->NeedsRuntimeCoverage())
	{
		Prepared->CoverageTriangles.LoadSynchronous();
		Prepared->Coordinates.LoadSynchronous();
	}
	if (Prepared->BoneCount > 0)
	{
		for (const auto& Reference : {Prepared->Coordinates, Prepared->SkinVertices, Prepared->SkinTriangles})
		{
			Reference.LoadSynchronous();
		}
	}
	else if (Prepared->NeedsSurfaceColorCache())
	{
		Prepared->Coordinates.LoadSynchronous();
	}
	for (const auto& Reference : Prepared->SourceMaterials)
	{
		Reference.LoadSynchronous();
	}
	for (const auto& Reference : Prepared->DissolveMaterials)
	{
		Reference.LoadSynchronous();
	}

	// コンパイル中の代替テクスチャ容量を保存しない。対象のテクスチャだけを完成させる。
	Prepared->Modify();
	if (Prepared->CaptureResourceManifest(OutError) == false)
	{
		return false;
	}
	auto Textures = TArray<UTexture*>{};
	for (const auto& Resource : Prepared->Resources)
	{
		if (auto* Texture = Cast<UTexture>(Resource.Asset.Get()))
		{
			Textures.Add(Texture);
		}
	}
	FTextureCompilingManager::Get().FinishCompilation(Textures);
	if (Prepared->CaptureResourceManifest(OutError) == false)
	{
		return false;
	}
	Prepared->MarkPackageDirty();
	return true;
}
