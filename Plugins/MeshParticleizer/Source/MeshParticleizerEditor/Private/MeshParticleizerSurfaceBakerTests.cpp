#include "MeshParticleizerSurfaceBaker.h"
#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "IMaterialBakingModule.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionActorPositionWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionParticleSubUV.h"
#include "Materials/MaterialExpressionQualitySwitch.h"
#include "Materials/MaterialExpressionScreenPosition.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionTwoSidedSign.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace MeshParticleizerPreparation
{
	bool SaveAsset(UObject* Asset);
	bool ConfigureSurfaceTint(UMaterial* Material, FString& OutError);
	bool ConfigureColorProvider(UMaterial* Material, UTexture2D* DefaultTexture, FString& OutError);
	bool ConfigureTextureColor(UMaterial* Material, FMeshParticleizerTextureColorSource& Binding, FString& OutError);
}

namespace MeshParticleizerColorFixture
{
#if WITH_DEV_AUTOMATION_TESTS
	void TestTextureAnalysis(FAutomationTestBase& Test)
	{
		auto Material = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
		auto FirstTexture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(4, 4));
		auto SecondTexture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(8, 8));
		FirstTexture->SRGB = true;
		SecondTexture->SRGB = true;
		auto* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		Sample->ParameterName = TEXT("SurfaceTexture");
		Sample->Texture = FirstTexture.Get();
		Sample->SamplerType = SAMPLERTYPE_Color;
		auto* UV = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureCoordinate::StaticClass()));
		UV->UTiling = 2;
		UV->VTiling = -3;
		Sample->Coordinates.Connect(0, UV);
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVectorParameter::StaticClass()));
		Tint->ParameterName = TEXT("OriginalTint");
		Tint->DefaultValue = FLinearColor::White;
		auto* Scale = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionScalarParameter::StaticClass()));
		Scale->ParameterName = TEXT("OriginalScale");
		Scale->DefaultValue = 1;
		auto* Uniform = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMultiply::StaticClass()));
		Uniform->A.Connect(0, Tint);
		Uniform->B.Connect(0, Scale);
		auto* Color = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMultiply::StaticClass()));
		Color->A.Connect(0, Sample);
		Color->B.Connect(0, Uniform);
		Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Color);
		UMaterialEditingLibrary::RecompileMaterial(Material.Get());
		Material->GetMaterialResource(GMaxRHIShaderPlatform)->FinishCompilation();

		// 親から継承した値と子のTexture上書きを合成し、元のグラフを変更しない。
		auto Parent = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
		Parent->SetParentEditorOnly(Material.Get());
		Parent->SetVectorParameterValueEditorOnly(TEXT("OriginalTint"), FLinearColor(0.2f, 0.4f, 0.8f));
		Parent->SetScalarParameterValueEditorOnly(TEXT("OriginalScale"), 0.5f);
		auto Child = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
		Child->SetParentEditorOnly(Parent.Get());
		Child->SetTextureParameterValueEditorOnly(TEXT("SurfaceTexture"), SecondTexture.Get());
		const auto ExpressionCount = Material->GetExpressions().Num();
		const auto Result = UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get());
		Test.TestTrue(TEXT("Texture times inherited uniform factors is a direct color candidate"), Result.bCompatible);
		Test.TestEqual(TEXT("The texture binding uses the instance override"), Result.Texture.Get(), SecondTexture.Get());
		Test.TestEqual(TEXT("The binding preserves the original texture parameter"), Result.TextureParameter, FName(TEXT("SurfaceTexture")));
		Test.TestEqual(TEXT("UV0 tiling and negative scaling are preserved"), Result.UVScale, FVector2D(2, -3));
		Test.TestTrue(TEXT("Inherited scalar and vector factors are combined"), Result.ColorScale.Equals(FLinearColor(0.1f, 0.2f, 0.4f, 1)));
		Test.TestEqual(TEXT("Analysis does not change the source graph"), Material->GetExpressions().Num(), ExpressionCount);

		Material->SetShadingModel(MSM_Unlit);
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Color);
		Test.TestFalse(TEXT("Unlit color requires explicit selection"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		Test.TestTrue(TEXT("Selected Unlit Emissive uses the dynamic texture route"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get(), true).bCompatible);
		Material->SetShadingModel(MSM_DefaultLit);

		auto Generated = TStrongObjectPtr<UMaterial>(DuplicateObject<UMaterial>(Material.Get(), GetTransientPackage()));
		auto Registered = Result;
		auto RegistrationError = FString{};
		Test.TestTrue(TEXT("Generated color sample accepts a registered texture input"),
			MeshParticleizerPreparation::ConfigureTextureColor(Generated.Get(), Registered, RegistrationError));
		const auto RegisteredCount = Generated->GetExpressions().Num();
		Test.TestTrue(TEXT("Texture input upgrade is idempotent"),
			MeshParticleizerPreparation::ConfigureTextureColor(Generated.Get(), Registered, RegistrationError));
		Test.TestEqual(TEXT("Repeated texture upgrade adds no nodes"), Generated->GetExpressions().Num(), RegisteredCount);
		Test.TestEqual(TEXT("Texture registration leaves the source graph intact"), Material->GetExpressions().Num(), ExpressionCount);
		UMaterialEditingLibrary::RecompileMaterial(Generated.Get());
		Generated->GetMaterialResource(GMaxRHIShaderPlatform)->FinishCompilation();
		UTexture* RegisteredDefault = nullptr;
		Test.TestTrue(TEXT("Generated material exposes the registered texture"), Generated->GetTextureParameterValue(Registered.RegisteredTextureParameter, RegisteredDefault));
		Test.TestEqual(TEXT("Registered default uses the resolved instance texture"), RegisteredDefault, static_cast<UTexture*>(SecondTexture.Get()));
		UTexture* OriginalDefault = nullptr;
		Test.TestTrue(TEXT("Original texture parameter remains available for baked validation"), Generated->GetTextureParameterValue(TEXT("SurfaceTexture"), OriginalDefault));

		// 同じ元材でもUVやチャンネル、複数サンプルを変えたら単純な経路へ通さない。
		UV->CoordinateIndex = 1;
		Test.TestFalse(TEXT("Non-UV0 input is not treated as UV0"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		UV->CoordinateIndex = 0;
		Color->A.Connect(1, Sample);
		Test.TestFalse(TEXT("Texture channel remapping is not treated as RGB"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		Color->A.Connect(0, Sample);
		Color->B.Connect(0, Sample);
		Test.TestFalse(TEXT("Two texture reads are not collapsed into one"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		Color->B.Connect(0, Color);
		Test.TestFalse(TEXT("Cycles fail without recursion overflow"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		Color->B.Connect(0, Uniform);
		SecondTexture->VirtualTextureStreaming = true;
		Test.TestFalse(TEXT("Virtual Texture is not a direct Texture2D binding"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		SecondTexture->VirtualTextureStreaming = false;
		SecondTexture->SRGB = false;
		Test.TestFalse(TEXT("sRGB and sampler mismatch is rejected"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		SecondTexture->SRGB = true;
		Parent->SetScalarParameterValueEditorOnly(TEXT("OriginalScale"), std::numeric_limits<float>::infinity());
		Test.TestFalse(TEXT("Non-finite uniform factors are rejected"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Child.Get()).bCompatible);
		Parent->SetScalarParameterValueEditorOnly(TEXT("OriginalScale"), 0.5f);

		// 属性形式では色だけを抽出し、色Textureと穴が共有される場合はベイクへ残す。
		auto* Attributes = Cast<UMaterialExpressionMakeMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMakeMaterialAttributes::StaticClass()));
		Attributes->BaseColor.Connect(0, Color);
		auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionSetMaterialAttributes::StaticClass()));
		Set->Inputs[0].Connect(0, Attributes);
		Set->ConnectInputAttribute(MP_Roughness, Scale);
		Material->bUseMaterialAttributes = true;
		Material->BlendMode = BLEND_Masked;
		Material->GetExpressionInputForProperty(MP_MaterialAttributes)->Connect(0, Set);
		Test.TestTrue(TEXT("Independent material attributes preserve the color binding"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		Material->SetShadingModel(MSM_Unlit);
		Attributes->EmissiveColor.Connect(0, Color);
		Test.TestTrue(TEXT("Unlit attributes use the Emissive texture binding"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get(), true).bCompatible);
		Material->SetShadingModel(MSM_DefaultLit);
		Attributes->OpacityMask.Connect(4, Sample);
		Test.TestFalse(TEXT("Texture updates cannot silently change baked coverage"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		auto* Alias = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		Alias->ParameterName = Sample->ParameterName;
		Attributes->OpacityMask.Connect(4, Alias);
		Test.TestFalse(TEXT("Distinct nodes sharing the mask texture parameter are rejected"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		Attributes->OpacityMask.Expression = nullptr;
		auto* Vertex = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVertexColor::StaticClass());
		Color->B.Connect(0, Vertex);
		const auto Complex = UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get());
		Test.TestFalse(TEXT("Vertex-dependent factors remain on the baked/provider path"), Complex.bCompatible);
		Test.TestTrue(TEXT("Unsupported graph returns a reason and no partial binding"), Complex.Diagnostics.IsEmpty() == false && Complex.Texture.IsNull() && Complex.TextureParameter.IsNone());

		// 固定のTexture Sampleも検出し、元資産を書き換えず生成材に入力が必要なことを返す。
		auto* FixedSample = Cast<UMaterialExpressionTextureSample>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureSample::StaticClass()));
		FixedSample->Texture = FirstTexture.Get();
		FixedSample->SamplerType = SAMPLERTYPE_Color;
		Attributes->BaseColor.Connect(0, FixedSample);
		const auto Fixed = UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get());
		Test.TestTrue(TEXT("Fixed Texture2D samples can receive a generated binding"), Fixed.bCompatible && Fixed.bRequiresTextureParameter && Fixed.TextureParameter.IsNone());
		Test.TestEqual(TEXT("Fixed sample identity is retained"), Fixed.SampleExpressionGuid, FixedSample->MaterialExpressionGuid);
		Attributes->OpacityMask.Connect(4, FixedSample);
		Test.TestFalse(TEXT("A fixed sample shared with coverage is not dynamically replaceable"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		Attributes->OpacityMask.Expression = nullptr;
		auto* SubUV = Cast<UMaterialExpressionParticleSubUV>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionParticleSubUV::StaticClass()));
		SubUV->Texture = FirstTexture.Get();
		SubUV->SamplerType = SAMPLERTYPE_Color;
		Attributes->BaseColor.Connect(0, SubUV);
		Test.TestFalse(TEXT("Particle SubUV is not treated as a regular UV0 sample"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		Attributes->BaseColor.Connect(0, FixedSample);
		Material->GetEditorOnlyData()->FrontMaterial.Connect(0, Color);
		Test.TestFalse(TEXT("A connected Front Material cannot use the legacy color binding"), UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material.Get()).bCompatible);
		Material->GetEditorOnlyData()->FrontMaterial.Expression = nullptr;
		Test.AddInfo(TEXT("Texture color analysis: inherited values, UV scale, attributes, independent coverage and unsupported graphs verified."));
	}
	void TestMaterialDependencies(FAutomationTestBase& Test)
	{
		auto Material = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
		auto* Fixed = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionConstant3Vector::StaticClass()));
		Fixed->Constant = FLinearColor::White;
		auto* World = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionWorldPosition::StaticClass());
		auto* Actor = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionActorPositionWS::StaticClass());
		auto* Time = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTime::StaticClass());
		auto* CPD = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVectorParameter::StaticClass()));
		CPD->ParameterName = TEXT("PrimitiveColor");
		CPD->bUseCustomPrimitiveData = true;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Fixed);
		Material->PostEditChange();
		auto Result = FMeshParticleizerTextureColorSource{};
		auto Error = FString{};
		const auto Analyze = [&Material, &Result, &Error]() {
			return MeshParticleizerPreparation::AnalyzeSurfaceDependencies(Material.Get(), false, Result, Error);
		};

		Test.TestTrue(TEXT("Unused dynamic nodes leave a fixed color eligible"), Analyze() && Result.HasCurrentDependencyAnalysis() && Result.bRequiresColorProvider == false);
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Fixed);
		Test.TestTrue(TEXT("Fixed original emission can be baked independently"), MeshParticleizerPreparation::ValidateSourceEmissionDependencies(Material.Get(), Error));
		for (auto* Expression : {World, static_cast<UMaterialExpression*>(CPD), static_cast<UMaterialExpression*>(Time)})
		{
			Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Expression);
			Test.TestFalse(TEXT("Changing original emission requires a dedicated provider"), MeshParticleizerPreparation::ValidateSourceEmissionDependencies(Material.Get(), Error));
		}
		Material->GetEditorOnlyData()->EmissiveColor.Expression = nullptr;
		for (auto* Expression : {World, Actor, static_cast<UMaterialExpression*>(CPD)})
		{
			Material->GetEditorOnlyData()->BaseColor.Connect(0, Expression);
			Test.TestTrue(TEXT("World, Actor and CPD color require a provider"), Analyze() && Result.bRequiresColorProvider && Result.DependencyDiagnostics.IsEmpty() == false);
		}
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Time);
		Test.TestTrue(TEXT("Time color explicitly keeps snapshot semantics"), Analyze() && Result.bUsesFrozenInputs && Result.bRequiresColorProvider == false);

		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVectorParameter::StaticClass()));
		Tint->ParameterName = TEXT("ColorOnlyTint");
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Tint);
		Test.TestTrue(TEXT("A color-only vector is eligible for provider fallback"), Analyze() && Result.ProviderVectorParameters.Contains(Tint->ParameterName));
		Material->BlendMode = BLEND_Masked;
		Material->GetEditorOnlyData()->OpacityMask.Connect(1, Tint);
		Test.TestTrue(TEXT("A shared coverage parameter cannot fall back through color"), Analyze() && Result.ProviderVectorParameters.IsEmpty());
		Material->BlendMode = BLEND_Opaque;
		Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, Tint);
		Test.TestTrue(TEXT("A shared position parameter remains fixed"), Analyze() && Result.ProviderVectorParameters.IsEmpty());
		Material->GetEditorOnlyData()->WorldPositionOffset.Expression = nullptr;
		auto* Scale = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionScalarParameter::StaticClass()));
		Scale->ParameterName = TEXT("ColorOnlyScale");
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Scale);
		Test.TestTrue(TEXT("A color-only scalar is eligible for provider fallback"), Analyze() && Result.ProviderScalarParameters.Contains(Scale->ParameterName));
		Material->GetEditorOnlyData()->Roughness.Connect(0, Scale);
		Test.TestTrue(TEXT("A shared shading parameter remains fixed"), Analyze() && Result.ProviderScalarParameters.IsEmpty());
		Material->GetEditorOnlyData()->Roughness.Expression = nullptr;

		// 属性の上書きと未使用出力は、実際の色への依存とは区別する。
		auto* Attributes = Cast<UMaterialExpressionMakeMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMakeMaterialAttributes::StaticClass()));
		Attributes->BaseColor.Connect(0, World);
		Attributes->Roughness.Connect(0, Time);
		auto* Set = Cast<UMaterialExpressionSetMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionSetMaterialAttributes::StaticClass()));
		Set->Inputs[0].Connect(0, Attributes);
		Set->ConnectInputAttribute(MP_BaseColor, Fixed);
		Material->bUseMaterialAttributes = true;
		Material->GetEditorOnlyData()->MaterialAttributes.Connect(0, Set);
		Test.TestTrue(TEXT("Overwritten color attributes do not retain hidden world dependencies"), Analyze() && Result.bRequiresColorProvider == false && Result.bUsesFrozenInputs == false);
		Material->bUseMaterialAttributes = false;

		auto Function = TStrongObjectPtr<UMaterialFunction>(NewObject<UMaterialFunction>());
		auto* FunctionFixed = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function.Get(), UMaterialExpressionConstant3Vector::StaticClass());
		auto* FunctionWorld = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function.Get(), UMaterialExpressionWorldPosition::StaticClass());
		auto* FixedOutput = Cast<UMaterialExpressionFunctionOutput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function.Get(), UMaterialExpressionFunctionOutput::StaticClass()));
		FixedOutput->OutputName = TEXT("Fixed");
		FixedOutput->SortPriority = 0;
		FixedOutput->A.Connect(0, FunctionFixed);
		auto* DynamicOutput = Cast<UMaterialExpressionFunctionOutput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function.Get(), UMaterialExpressionFunctionOutput::StaticClass()));
		DynamicOutput->OutputName = TEXT("Dynamic");
		DynamicOutput->SortPriority = 1;
		DynamicOutput->A.Connect(0, FunctionWorld);
		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionMaterialFunctionCall::StaticClass()));
		Call->SetMaterialFunction(Function.Get());
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Call);
		Test.TestTrue(TEXT("Unused function output does not require a provider"), Analyze() && Result.bRequiresColorProvider == false);
		Material->GetEditorOnlyData()->BaseColor.Connect(1, Call);
		Test.TestTrue(TEXT("Selected dynamic function output requires a provider"), Analyze() && Result.bRequiresColorProvider);

		auto* Switch = Cast<UMaterialExpressionStaticSwitchParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionStaticSwitchParameter::StaticClass()));
		Switch->ParameterName = TEXT("UseWorldColor");
		Switch->DefaultValue = false;
		Switch->A.Connect(0, World);
		Switch->B.Connect(0, Fixed);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Switch);
		Material->PostEditChange();
		Test.TestTrue(TEXT("Inactive static branch does not require a provider"), Analyze() && Result.bRequiresColorProvider == false);
		auto Instance = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
		Instance->SetParentEditorOnly(Material.Get());
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance.Get(), Switch->ParameterName, true);
		Test.TestTrue(TEXT("Instance static override selects the dynamic branch"), MeshParticleizerPreparation::AnalyzeSurfaceDependencies(Instance.Get(), false, Result, Error) && Result.bRequiresColorProvider);

		// 色Providerは穴の変動を解決しない。Opaqueの未使用Maskは拒否しない。
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Fixed);
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, CPD);
		Test.TestTrue(TEXT("Unused opacity input on Opaque remains supported"), Analyze());
		Material->BlendMode = BLEND_Masked;
		Test.TestFalse(TEXT("Dynamic coverage requires an adapter"), Analyze());
		Test.TestTrue(TEXT("Coverage rejection identifies its input"), Error.Contains(TEXT("Opacity Mask")) && Error.Contains(TEXT("Custom Primitive Data")));
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, Time);
		Test.TestFalse(TEXT("Time-dependent coverage cannot use a fixed candidate surface"), Analyze());

		// 他の動的入力を伴わない未知の式も、固定Maskとして間引いてはいけない。
		auto* Screen = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionScreenPosition::StaticClass());
		auto* Depth = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionPixelDepth::StaticClass());
		auto* Facing = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTwoSidedSign::StaticClass());
		for (auto* Expression : {Screen, Depth, Facing})
		{
			Material->GetEditorOnlyData()->OpacityMask.Connect(0, Expression);
			Test.TestFalse(TEXT("Unclassified mask input is rejected without another dynamic input"), Analyze());
			Test.TestFalse(TEXT("Rejected unknown mask cannot publish current analysis"), Result.HasCurrentDependencyAnalysis());
			auto InputName = Expression->GetClass()->GetName();
			InputName.RemoveFromStart(TEXT("MaterialExpression"));
			Test.TestTrue(TEXT("Unknown mask diagnostic names the input"), Error.Contains(InputName));
		}

		auto* FixedVertexColor = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVertexColor::StaticClass());
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, FixedVertexColor);
		Test.TestTrue(TEXT("Baked vertex color mask remains supported"), Analyze() && Result.bRequiresRuntimeCoverage == false);
		Material->DitherOpacityMask = true;
		Test.TestFalse(TEXT("Runtime coverage cannot substitute unavailable vertex color"), Analyze());
		Material->DitherOpacityMask = false;
		Switch->A.Connect(0, Screen);
		Switch->B.Connect(0, Fixed);
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, Switch);
		Test.TestTrue(TEXT("Unknown input in an inactive static branch is ignored"), Analyze());

		// 親をMaskedへ変えた後に作り、古いOpaqueのキャッシュを検証へ持ち込まない。
		auto MaskInstance = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
		MaskInstance->SetParentEditorOnly(Material.Get());
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MaskInstance.Get(), Switch->ParameterName, true);
		Test.TestEqual(TEXT("Mask instance uses the current blend mode"), MaskInstance->GetBlendMode(), BLEND_Masked);
		Test.TestFalse(TEXT("Instance override selecting the unknown mask input is rejected"),
			MeshParticleizerPreparation::AnalyzeSurfaceDependencies(MaskInstance.Get(), false, Result, Error));

		auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionCustom::StaticClass()));
		Custom->Code = TEXT("return 1;");
		Custom->Inputs.Reset();
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, Custom);
		Test.TestFalse(TEXT("Unverified Custom HLSL coverage requires an adapter"), Analyze());
		Material->BlendMode = BLEND_Opaque;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Custom);
		Test.TestTrue(TEXT("Unverified Custom HLSL color requires a provider"), Analyze() && Result.bRequiresColorProvider);

		// 品質切替で変わる固定値も、一度だけのベイクでは追従できない。
		auto* Quality = Cast<UMaterialExpressionQualitySwitch>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionQualitySwitch::StaticClass()));
		Quality->Default.Connect(0, Fixed);
		Quality->Inputs[EMaterialQualityLevel::Low].Connect(0, Scale);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Scale);
		Material->GetEditorOnlyData()->Roughness.Connect(0, Quality);
		Test.TestTrue(TEXT("A parameter used by another quality's roughness is not color-only"), Analyze() && Result.ProviderScalarParameters.IsEmpty());
		Material->GetEditorOnlyData()->Roughness.Expression = nullptr;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Quality);
		Test.TestTrue(TEXT("Quality-dependent color requires a provider even when both branches are uniform"), Analyze() && Result.bRequiresColorProvider && Result.DependencyDiagnostics.Contains(TEXT("Material Quality Switch")));
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Quality);
		Test.TestFalse(TEXT("Quality-dependent emission is not a fixed HDR bake"), MeshParticleizerPreparation::ValidateSourceEmissionDependencies(Material.Get(), Error));
		Material->GetEditorOnlyData()->EmissiveColor.Expression = nullptr;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Fixed);
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, Quality);
		Material->BlendMode = BLEND_Masked;
		Test.TestTrue(TEXT("Quality-dependent coverage selects full-surface runtime evaluation"), Analyze() && Result.HasCurrentDependencyAnalysis() && Result.bRequiresRuntimeCoverage);
		Quality->Inputs[EMaterialQualityLevel::Low].Connect(0, Time);
		Test.TestFalse(TEXT("Unsupported input in another quality branch is rejected"), Analyze());
		Test.TestFalse(TEXT("Rejected analysis clears the previously successful version"), Result.HasCurrentDependencyAnalysis());
		Test.TestTrue(TEXT("All-quality coverage validation identifies Time"), Error.Contains(TEXT("Time")));
		Quality->Inputs[EMaterialQualityLevel::Low].Connect(0, Scale);

		auto* Reroute = Cast<UMaterialExpressionReroute>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionReroute::StaticClass()));
		Reroute->Input.Connect(0, Fixed);
		Quality->Inputs[EMaterialQualityLevel::Low].Connect(0, Reroute);
		Test.TestTrue(TEXT("Identical quality inputs through a reroute remain fixed"), Analyze());
		Quality->Inputs[EMaterialQualityLevel::Low].Connect(1, Fixed);
		Test.TestTrue(TEXT("A different channel on the same quality input selects runtime evaluation"), Analyze() && Result.bRequiresRuntimeCoverage);
		Quality->Inputs[EMaterialQualityLevel::Low].Expression = nullptr;
		Test.TestTrue(TEXT("An unconnected quality input uses the default mask"), Analyze());
		Quality->Inputs[EMaterialQualityLevel::Low].Connect(0, Scale);
		Switch->A.Connect(0, Quality);
		Switch->B.Connect(0, Fixed);
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, Switch);
		Test.TestTrue(TEXT("A quality switch in an inactive static branch is ignored"), Analyze());
		Material->DitherOpacityMask = true;
		Test.TestTrue(TEXT("Dither alone requires full-surface runtime coverage"), Analyze() && Result.bRequiresRuntimeCoverage);

		// 他の動的スロット用データが存在しても、新しく動的になった面の間引きは戻らない。
		auto OldCoverage = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
		OldCoverage->DataVersion = UMeshParticleizerPreparedMesh::CurrentDataVersion;
		OldCoverage->CoverageDataVersion = UMeshParticleizerPreparedMesh::CurrentCoverageDataVersion;
		OldCoverage->CoverageTriangles = UTexture2D::CreateTransient(1, 1);
		OldCoverage->SourceMaterials.Add(Material.Get());
		OldCoverage->DissolveMaterials.Add(Material.Get());
		OldCoverage->TextureColorSources.AddDefaulted();
		Test.TestFalse(TEXT("A newly dynamic slot requires rebaking despite existing coverage geometry"),
			UMeshParticleizerPreparationLibrary::UpgradePreparedDependencies(OldCoverage.Get(), Error));
		Test.TestTrue(TEXT("Rebake diagnostic identifies the newly dynamic slot"), Error.Contains(TEXT("Slot 0 now requires runtime Opacity Mask")));
		Test.TestFalse(TEXT("Rejected upgrade preserves fixed-mask metadata"), OldCoverage->TextureColorSources[0].bRequiresRuntimeCoverage);
		Material->DitherOpacityMask = false;
		Test.TestTrue(TEXT("Disabling dither clears runtime coverage for a fixed mask"), Analyze() && Result.bRequiresRuntimeCoverage == false);

		Attributes->BaseColor.Connect(0, Fixed);
		Attributes->OpacityMask.Connect(0, Quality);
		Material->bUseMaterialAttributes = true;
		Material->GetEditorOnlyData()->MaterialAttributes.Connect(0, Attributes);
		Test.TestTrue(TEXT("Quality-dependent mask attributes select runtime evaluation"), Analyze() && Result.bRequiresRuntimeCoverage);
		Attributes->OpacityMask.Connect(0, Screen);
		auto Sources = TArray<FMeshParticleizerTextureColorSource>{FMeshParticleizerTextureColorSource{}};
		auto Slots = TArray<UMaterialInterface*>{Material.Get(), nullptr, Material.Get()};
		Test.TestFalse(TEXT("All unsupported slots are diagnosed before preparation"), MeshParticleizerPreparation::AnalyzeSurfaceMaterials(Slots, false, false, Sources, Error));
		Test.TestTrue(TEXT("Material diagnostics name every failing slot"), Error.Contains(TEXT("Slot 0")) && Error.Contains(TEXT("Slot 1")) && Error.Contains(TEXT("Slot 2")) && Error.Contains(Material->GetName()));
		Test.TestTrue(TEXT("Attribute mask diagnostics preserve the unknown input"), Error.Contains(TEXT("ScreenPosition")));
		Test.TestTrue(TEXT("A failed material preflight publishes no partial bindings"), Sources.IsEmpty());
		Attributes->OpacityMask.Connect(0, Fixed);
		Slots.RemoveAt(1);
		Test.TestTrue(TEXT("A repaired material set clears stale diagnostics and returns every binding"),
			MeshParticleizerPreparation::AnalyzeSurfaceMaterials(Slots, false, false, Sources, Error) && Error.IsEmpty() && Sources.Num() == 2);

		Material->bUseMaterialAttributes = false;
		auto* Camera = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionCameraVectorWS::StaticClass());
		auto* VertexNormal = UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionVertexNormalWS::StaticClass());
		auto* Dot = Cast<UMaterialExpressionDotProduct>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionDotProduct::StaticClass()));
		Dot->A.Connect(0, Camera);
		Dot->B.Connect(0, VertexNormal);
		Material->GetEditorOnlyData()->OpacityMask.Connect(0, Dot);
		Test.TestTrue(TEXT("View and posed vertex normal select runtime coverage"), Analyze() && Result.bRequiresRuntimeCoverage);
		auto* CoverageUV = Cast<UMaterialExpressionTextureCoordinate>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureCoordinate::StaticClass()));
		CoverageUV->CoordinateIndex = 1;
		Dot->B.Connect(0, CoverageUV);
		Test.TestFalse(TEXT("Runtime coverage cannot silently substitute UV0 for UV1"), Analyze());
		auto CoverageTexture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(4, 4));
		auto* CoverageSample = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material.Get(), UMaterialExpressionTextureSampleParameter2D::StaticClass()));
		CoverageSample->Texture = CoverageTexture.Get();
		CoverageSample->ParameterName = TEXT("CoverageTexture");
		CoverageUV->CoordinateIndex = 0;
		CoverageSample->Coordinates.Connect(0, CoverageUV);
		Dot->B.Connect(0, CoverageSample);
		Test.TestTrue(TEXT("Runtime mask accepts direct UV0 texture sampling"), Analyze() && Result.bRequiresRuntimeCoverage);
		CoverageSample->Coordinates.Connect(0, Camera);
		Test.TestFalse(TEXT("Camera-driven texture coordinates cannot use UV0 derivatives"), Analyze());
		CoverageSample->Coordinates.Connect(0, CoverageUV);
		CoverageSample->MipValueMode = TMVM_MipBias;
		Test.TestTrue(TEXT("Direct UV0 supports material mip bias"), Analyze() && Result.bRequiresRuntimeCoverage);
		CoverageSample->MipValueMode = TMVM_Derivative;
		Test.TestFalse(TEXT("Custom derivatives require a coverage adapter"), Analyze());
		Dot->B.Connect(0, VertexNormal);
		Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, Fixed);
		Material->PostEditChange();
		Test.TestFalse(TEXT("WPO deformation is not approximated by bone-only coverage"), Analyze());
		Material->GetEditorOnlyData()->WorldPositionOffset.Expression = nullptr;
		Test.AddInfo(TEXT("Material dependencies: selected properties, function outputs, static overrides, quality switches, CPD, coverage and complete slot diagnostics validated."));
	}
#endif

	UMaterial* MakeMaterial(bool bMasked, UObject* Outer = GetTransientPackage(), FName Name = NAME_None, EObjectFlags Flags = RF_Transient, bool bUnlit = false)
	{
		auto MaterialOwner = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>(Outer, Name, Flags));
		auto* Material = MaterialOwner.Get();
		Material->TwoSided = true;
		Material->BlendMode = bMasked ? BLEND_Masked : BLEND_Opaque;
		Material->bCanMaskedBeAssumedOpaque = false;
		auto* UV = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass());
		auto* Vertex = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexColor::StaticClass());
		auto* Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
		Multiply->B.Connect(0, Vertex);
		Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Multiply);

		if (bMasked == false)
		{
			auto* Blue = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass()));
			Blue->R = 0.18f;
			auto* Gradient = Cast<UMaterialExpressionAppendVector>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionAppendVector::StaticClass()));
			Gradient->A.Connect(0, UV);
			Gradient->B.Connect(0, Blue);
			Multiply->A.Connect(0, Gradient);
		}
		else
		{
			auto* Constant = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass()));
			Constant->Constant = FLinearColor(0.18f, 0.04f, 0.8f);
			Multiply->A.Connect(0, Constant);
			auto* U = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionComponentMask::StaticClass()));
			U->Input.Connect(0, UV);
			U->R = true;
			U->G = U->B = U->A = false;
			auto* One = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass()));
			One->R = 1;
			auto* Zero = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass());
			auto* Mask = Cast<UMaterialExpressionIf>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionIf::StaticClass()));
			Mask->A.Connect(0, U);
			Mask->ConstB = 0.5f;
			Mask->EqualsThreshold = 0;
			Mask->AGreaterThanB.Connect(0, One);
			Mask->AEqualsB.Connect(0, One);
			Mask->ALessThanB.Connect(0, Zero);
			Material->GetExpressionInputForProperty(MP_OpacityMask)->Connect(0, Mask);

			// 属性形式でも元の色と透明部分を評価する。同じ既存テストで両形式を扱う。
			auto* Attributes = Cast<UMaterialExpressionMakeMaterialAttributes>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMakeMaterialAttributes::StaticClass()));
			Attributes->BaseColor.Connect(0, Multiply);
			Attributes->OpacityMask.Connect(0, Mask);
			Material->bUseMaterialAttributes = true;
			Material->GetExpressionInputForProperty(MP_MaterialAttributes)->Connect(0, Attributes);
		}
		if (bUnlit == true)
		{
			Material->SetShadingModel(MSM_Unlit);
			auto* HDR = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
			HDR->A.Connect(0, Multiply);
			auto* Range = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass()));
			Range->Constant = FLinearColor(2, 2, 4000);
			HDR->B.Connect(0, Range);
			Material->GetEditorOnlyData()->EmissiveColor.Connect(0, HDR);
			if (bMasked == true)
			{
				auto* Attributes = Cast<UMaterialExpressionMakeMaterialAttributes>(Material->GetEditorOnlyData()->MaterialAttributes.Expression);
				Attributes->EmissiveColor.Connect(0, HDR);
			}
		}
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Material->GetMaterialResource(GMaxRHIShaderPlatform)->FinishCompilation();
		return Material;
	}

	UStaticMesh* MakeMesh(UMaterial* Opaque, UMaterial* Masked, UObject* Outer = GetTransientPackage(), FName Name = NAME_None, EObjectFlags Flags = RF_Transient)
	{
		auto MeshOwner = TStrongObjectPtr<UStaticMesh>(NewObject<UStaticMesh>(Outer, Name, Flags));
		auto* Mesh = MeshOwner.Get();
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Opaque, TEXT("Gradient"), TEXT("Gradient")));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Masked, TEXT("Masked"), TEXT("Masked")));
		Mesh->SetNumSourceModels(1);
		auto* Description = Mesh->CreateMeshDescription(0);
		auto Attributes = FStaticMeshAttributes(*Description);
		Attributes.Register();
		auto Positions = Attributes.GetVertexPositions();
		auto Normals = Attributes.GetVertexInstanceNormals();
		auto Tangents = Attributes.GetVertexInstanceTangents();
		auto Signs = Attributes.GetVertexInstanceBinormalSigns();
		auto Colors = Attributes.GetVertexInstanceColors();
		auto UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(1);
		auto Names = Attributes.GetPolygonGroupMaterialSlotNames();
		// PolygonGroup IDと実マテリアルスロットの番号が違うメッシュも扱う。
		Description->CreatePolygonGroupWithID(FPolygonGroupID(2));
		Description->CreatePolygonGroupWithID(FPolygonGroupID(5));
		Names[FPolygonGroupID(2)] = TEXT("Gradient");
		Names[FPolygonGroupID(5)] = TEXT("Masked");
		const FVector2f Corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
		const FVector4f Tints[] = {FVector4f(1, 0, 1, 1), FVector4f(0, 1, 1, 1), FVector4f(1, 1, 0, 1)};
		for (auto Panel = 0; Panel < 3; ++Panel)
		{
			for (auto Triangle = 0; Triangle < 2; ++Triangle)
			{
				auto Instances = TArray<FVertexInstanceID>{};
				for (auto Corner = 0; Corner < 3; ++Corner)
				{
					const auto UV = Corners[Triangle * 3 + Corner];
					const auto Vertex = Description->CreateVertex();
					Positions[Vertex] = FVector3f(Panel * 120 + UV.X * 100, 0, UV.Y * 100);
					const auto Instance = Description->CreateVertexInstance(Vertex);
					Normals[Instance] = FVector3f(0, -1, 0);
					Tangents[Instance] = FVector3f(1, 0, 0);
					Signs[Instance] = 1;
					Colors[Instance] = Tints[Panel];
					UVs.Set(Instance, 0, UV);
					Instances.Add(Instance);
				}
				Description->CreatePolygon(FPolygonGroupID(Panel == 2 ? 5 : 2), Instances);
			}
		}
		Mesh->CommitMeshDescription(0);
		Mesh->Build(false);
		return Mesh;
	}
}

UStaticMesh* UMeshParticleizerPreparationLibrary::CreateColorValidationMesh(const FString& Destination, FString& OutError)
{
	OutError.Reset();
	if (Destination.StartsWith(TEXT("/Game/")) == false || FPackageName::IsValidLongPackageName(Destination) == false)
	{
		OutError = TEXT("Use a new /Game/ folder for the color validation assets.");
		return nullptr;
	}
	const auto MeshPath = Destination / TEXT("SM_ColorValidation");
	const auto OpaquePath = Destination / TEXT("M_ColorGradient");
	const auto MaskedPath = Destination / TEXT("M_ColorMasked");
	for (const auto& Path : {MeshPath, OpaquePath, MaskedPath})
	{
		if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
		{
			OutError = TEXT("Validation assets already exist. Use their mesh or choose a new folder.");
			return nullptr;
		}
	}

	auto* Opaque = MeshParticleizerColorFixture::MakeMaterial(false, CreatePackage(*OpaquePath), TEXT("M_ColorGradient"), RF_Public | RF_Standalone);
	auto* Masked = MeshParticleizerColorFixture::MakeMaterial(true, CreatePackage(*MaskedPath), TEXT("M_ColorMasked"), RF_Public | RF_Standalone);
	auto* Mesh = MeshParticleizerColorFixture::MakeMesh(Opaque, Masked, CreatePackage(*MeshPath), TEXT("SM_ColorValidation"), RF_Public | RF_Standalone);
	for (auto* Asset : TArray<UObject*>{Opaque, Masked, Mesh})
	{
		if (MeshParticleizerPreparation::SaveAsset(Asset) == false)
		{
			OutError = FString::Printf(TEXT("Failed to save %s."), *Asset->GetPathName());
			return nullptr;
		}
	}
	return Mesh;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerSurfaceColorTest, "MeshParticleizer.Editor.SurfaceColor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerSurfaceColorTest::RunTest(const FString& Parameters)
{
	MeshParticleizerColorFixture::TestTextureAnalysis(*this);
	MeshParticleizerColorFixture::TestMaterialDependencies(*this);
	for (const auto bUnlit : {false, true})
	{
		// UV共有、頂点色、複数スロット、透明部分、線形色を同じ生成経路で確認する。
		auto Opaque = TStrongObjectPtr<UMaterial>(MeshParticleizerColorFixture::MakeMaterial(false, GetTransientPackage(), NAME_None, RF_Transient, bUnlit));
		auto Masked = TStrongObjectPtr<UMaterial>(MeshParticleizerColorFixture::MakeMaterial(true, GetTransientPackage(), NAME_None, RF_Transient, bUnlit));
		if (bUnlit == false)
		{
			for (auto* Material : {Opaque.Get(), Masked.Get()})
			{
				auto* Emission = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant3Vector::StaticClass()));
				Emission->Constant = FLinearColor(4, 0.25f, 2);
				if (Material->bUseMaterialAttributes == true)
				{
					CastChecked<UMaterialExpressionMakeMaterialAttributes>(Material->GetEditorOnlyData()->MaterialAttributes.Expression)->EmissiveColor.Connect(0, Emission);
				}
				else
				{
					Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Emission);
				}
				UMaterialEditingLibrary::RecompileMaterial(Material);
				Material->GetMaterialResource(GMaxRHIShaderPlatform)->FinishCompilation();
			}
		}
		auto Mesh = TStrongObjectPtr<UStaticMesh>(MeshParticleizerColorFixture::MakeMesh(Opaque.Get(), Masked.Get()));
		auto Samples = MeshParticleizerPreparation::FSurfaceSamples{};
		auto Error = FString{};
		if (bUnlit == true)
		{
			this->TestFalse(TEXT("Unselected Unlit does not silently bake black Base Color"),
				MeshParticleizerPreparation::BakeSurfaceSamples(Mesh.Get(), 6000, 1337, Samples, Error));
			this->TestTrue(TEXT("Unlit rejection explains the required selection"), Error.Contains(TEXT("Use Unlit Emissive")));
			Error.Reset();
		}
		auto* ClampEmissive = IConsoleManager::Get().FindConsoleVariable(TEXT("MaterialBaking.ForceDisableEmissiveScaling"));
		const auto PreviousClamp = ClampEmissive->GetBool();
		const auto PreviousFlags = ClampEmissive->GetFlags() & ECVF_SetByMask;
		if (MeshParticleizerPreparation::BakeSurfaceSamples(Mesh.Get(), 6000, 1337, Samples, Error, bUnlit) == false)
		{
			this->AddError(Error);
			return false;
		}
		this->TestEqual(TEXT("Unlit bake restores the shared baker setting"), ClampEmissive->GetBool(), PreviousClamp);
		this->TestEqual(TEXT("Unlit bake preserves console setting priority"), ClampEmissive->GetFlags() & ECVF_SetByMask, PreviousFlags);

		auto Errors = TArray<float>{};
		auto Counts = TArray<int32>{0, 0, 0};
		auto WrongCoverage = 0;
		for (auto Index = 0; Index < 6000; ++Index)
		{
			const auto Panel = FMath::Clamp(FMath::FloorToInt(Samples.Positions[Index].R / 120), 0, 2);
			const auto UV = Samples.Coordinates[Index];
			auto Expected = Panel == 0 ? FLinearColor(UV.R, 0, 0.18f) : (Panel == 1 ? FLinearColor(0, UV.G, 0.18f) : FLinearColor(0.18f, 0.04f, 0));
			if (bUnlit == true)
			{
				Expected = (Expected * FLinearColor(2, 2, 4000)).GetClamped();
			}
			const auto Difference = Samples.Colors[Index] - Expected;
			Errors.Add(FMath::Max3(FMath::Abs(Difference.R), FMath::Abs(Difference.G), FMath::Abs(Difference.B)));
			++Counts[Panel];
			if (Panel == 2 && UV.R < 0.495f)
			{
				++WrongCoverage;
			}
		}
		Errors.Sort();
		const auto P95 = Errors[FMath::FloorToInt((Errors.Num() - 1) * 0.95)];
		this->AddInfo(FString::Printf(TEXT("%s linear RGB error p95 %.6f; panel samples %d/%d/%d; clipped samples %d."), bUnlit ? TEXT("Unlit Emissive") : TEXT("Base Color"), P95, Counts[0], Counts[1], Counts[2], WrongCoverage));
		UE_LOG(LogTemp, Display, TEXT("MeshParticleizer SurfaceColor: RGB p95 %.6f; samples %d/%d/%d; clipped %d."), P95, Counts[0], Counts[1], Counts[2], WrongCoverage);
		this->TestTrue(TEXT("Base Color at each surface point is accurate within 0.01 linear RGB"), P95 < 0.01f);
		this->TestEqual(TEXT("Masked holes generate no particles"), WrongCoverage, 0);
		for (auto Panel = 0; Panel < 3; ++Panel)
		{
			this->TestTrue(FString::Printf(TEXT("Panel %d has surface samples"), Panel), Counts[Panel] > 500);
		}

		{
			// 共有ベイク設定の両状態を復元し、明るい発光と暗い表面色を同時に保持する。
			auto& Baker = FModuleManager::LoadModuleChecked<IMaterialBakingModule>(TEXT("MaterialBaking"));
			auto bPreviousHDR = false;
			if (MeshParticleizerPreparation::ReadEmissionBakeState(Baker, bPreviousHDR, Error) == false)
			{
				this->AddError(Error);
				return false;
			}
			ON_SCOPE_EXIT
			{
				Baker.SetEmissiveHDR(bPreviousHDR);
			};
			for (const auto bInitiallyHDR : {false, true})
			{
				Baker.SetEmissiveHDR(bInitiallyHDR);
				auto HDR = MeshParticleizerPreparation::FSurfaceSamples{};
				if (MeshParticleizerPreparation::BakeSurfaceSamples(Mesh.Get(), 6000, 1337, HDR, Error, bUnlit, true) == false)
				{
					this->AddError(Error);
					return false;
				}
				auto bRestoredHDR = !bInitiallyHDR;
				this->TestTrue(TEXT("HDR output state remains readable"), MeshParticleizerPreparation::ReadEmissionBakeState(Baker, bRestoredHDR, Error));
				this->TestEqual(TEXT("HDR preparation restores the previous shared output setting"), bRestoredHDR, bInitiallyHDR);
				this->TestTrue(TEXT("Original emission keeps the same surface positions and UVs"), HDR.Positions == Samples.Positions && HDR.Coordinates == Samples.Coordinates);
				auto RelativeErrors = TArray<float>{};
				auto ColorErrors = TArray<float>{};
				auto PeakEmission = 0.f;
				for (auto Index = 0; Index < 6000; ++Index)
				{
					const auto Panel = FMath::Clamp(FMath::FloorToInt(HDR.Positions[Index].R / 120), 0, 2);
					const auto UV = HDR.Coordinates[Index];
					const auto Base = Panel == 0 ? FLinearColor(UV.R, 0, 0.18f) : (Panel == 1 ? FLinearColor(0, UV.G, 0.18f) : FLinearColor(0.18f, 0.04f, 0));
					const auto Expected = bUnlit ? Base * FLinearColor(2, 2, 4000) : FLinearColor(4, 0.25f, 2);
					const auto Difference = HDR.Emission[Index] - Expected;
					RelativeErrors.Add(FMath::Max3(FMath::Abs(Difference.R) / FMath::Max(Expected.R, 1.f),
						FMath::Abs(Difference.G) / FMath::Max(Expected.G, 1.f), FMath::Abs(Difference.B) / FMath::Max(Expected.B, 1.f)));
					const auto ColorDifference = HDR.Colors[Index] - (bUnlit ? Expected.GetClamped() : Base);
					ColorErrors.Add(FMath::Max3(FMath::Abs(ColorDifference.R), FMath::Abs(ColorDifference.G), FMath::Abs(ColorDifference.B)));
					PeakEmission = FMath::Max(PeakEmission, HDR.Emission[Index].B);
				}
				RelativeErrors.Sort();
				ColorErrors.Sort();
				const auto HDRP95 = RelativeErrors[FMath::FloorToInt((RelativeErrors.Num() - 1) * 0.95)];
				const auto ColorP95 = ColorErrors[FMath::FloorToInt((ColorErrors.Num() - 1) * 0.95)];
				this->TestTrue(TEXT("HDR emission retains bright values and half-float precision"), HDRP95 < 0.01f && PeakEmission > (bUnlit ? 700.f : 1.f));
				this->TestTrue(TEXT("HDR output does not reduce dark surface color precision"), ColorP95 < 0.01f);
				this->AddInfo(FString::Printf(TEXT("%s HDR initial=%d restored=%d relative RGB p95 %.6f; surface p95 %.6f; peak B %.3f."), bUnlit ? TEXT("Unlit") : TEXT("Lit"), bInitiallyHDR, bRestoredHDR, HDRP95, ColorP95, PeakEmission));
			}
		}

		// 通常入力とMaterial Attributesの両方で、同じTintを掛けても色・穴・UVの対応を保つ。
		const FLinearColor Tints[] = {FLinearColor(0.25f, 0.5f, 0.75f), FLinearColor(0.75f, 0.25f, 0.5f)};
		const TArray<UMaterial*> Materials = {Opaque.Get(), Masked.Get()};
		auto ProviderDefault = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(8, 8, PF_FloatRGBA));
		auto& DefaultMip = ProviderDefault->GetPlatformData()->Mips[0];
		auto* DefaultPixels = static_cast<FFloat16Color*>(DefaultMip.BulkData.Lock(LOCK_READ_WRITE));
		for (auto Index = 0; Index < 64; ++Index)
		{
			DefaultPixels[Index] = FFloat16Color(FLinearColor::White);
		}
		DefaultMip.BulkData.Unlock();
		ProviderDefault->SRGB = false;
		ProviderDefault->UpdateResource();
		for (auto Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			auto* Material = Materials[Slot];
			if (this->TestTrue(TEXT("Generated material accepts surface tint input"), MeshParticleizerPreparation::ConfigureSurfaceTint(Material, Error)) == false)
			{
				return false;
			}
			const auto Expressions = Material->GetExpressions().Num();
			this->TestTrue(TEXT("Surface tint upgrade is repeatable"), MeshParticleizerPreparation::ConfigureSurfaceTint(Material, Error));
			this->TestEqual(TEXT("Repeated upgrade adds no second tint"), Material->GetExpressions().Num(), Expressions);
			this->TestTrue(TEXT("Provider preserves both ordinary and Material Attributes graphs"),
				MeshParticleizerPreparation::ConfigureColorProvider(Material, ProviderDefault.Get(), Error));
			const auto ProviderExpressions = Material->GetExpressions().Num();
			this->TestTrue(TEXT("Provider upgrade is repeatable"), MeshParticleizerPreparation::ConfigureColorProvider(Material, ProviderDefault.Get(), Error));
			this->TestEqual(TEXT("Repeated upgrade adds no second provider graph"), Material->GetExpressions().Num(), ProviderExpressions);
			for (const auto& Expression : Material->GetExpressions())
			{
				auto* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression);
				if (Parameter != nullptr && Parameter->ParameterName == TEXT("MP_SurfaceTint"))
				{
					Parameter->DefaultValue = Tints[Slot];
				}
			}
			UMaterialEditingLibrary::RecompileMaterial(Material);
			Material->GetMaterialResource(GMaxRHIShaderPlatform)->FinishCompilation();
		}
		auto Tinted = MeshParticleizerPreparation::FSurfaceSamples{};
		if (MeshParticleizerPreparation::BakeSurfaceSamples(Mesh.Get(), 6000, 1337, Tinted, Error, bUnlit) == false)
		{
			this->AddError(Error);
			return false;
		}
		this->TestTrue(TEXT("Surface tint keeps coverage and sample positions"), Samples.Positions == Tinted.Positions);
		auto TintError = 0.0f;
		auto WorstTintIndex = 0;
		for (auto Index = 0; Index < 6000; ++Index)
		{
			const auto Expected = Samples.Colors[Index] * Tints[static_cast<int32>(Samples.Positions[Index].A)];
			const auto Difference = Tinted.Colors[Index] - Expected;
			const auto DifferenceMax = FMath::Max3(FMath::Abs(Difference.R), FMath::Abs(Difference.G), FMath::Abs(Difference.B));
			if (DifferenceMax > TintError)
			{
				TintError = DifferenceMax;
				WorstTintIndex = Index;
			}
		}
		this->AddInfo(FString::Printf(TEXT("Registered surface tint maximum RGB difference %.6f."), TintError));
		if (TintError >= 0.01f)
		{
			this->AddInfo(FString::Printf(TEXT("Worst tint sample %d slot %.0f: source %s, tinted %s."), WorstTintIndex,
				Samples.Positions[WorstTintIndex].A, *Samples.Colors[WorstTintIndex].ToString(), *Tinted.Colors[WorstTintIndex].ToString()));
		}
		this->TestTrue(TEXT("Generated surface tint matches the particle color formula"), TintError < 0.01f);

		// 公開画像を有効にした場合もBase Colorだけを差し替え、穴とMaterial Attributesを保つ。
		for (auto* Material : Materials)
		{
			for (const auto& Expression : Material->GetExpressions())
			{
				auto* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression);
				if (Parameter != nullptr && Parameter->ParameterName == TEXT("MP_UseColorProvider"))
				{
					Parameter->DefaultValue = 1;
				}
			}
			UMaterialEditingLibrary::RecompileMaterial(Material);
			Material->GetMaterialResource(GMaxRHIShaderPlatform)->FinishCompilation();
		}
		auto Published = MeshParticleizerPreparation::FSurfaceSamples{};
		if (MeshParticleizerPreparation::BakeSurfaceSamples(Mesh.Get(), 6000, 1337, Published, Error, bUnlit) == false)
		{
			this->AddError(Error);
			return false;
		}
		this->TestTrue(TEXT("Provider retains masked coverage and the same surface coordinates"), Samples.Positions == Published.Positions && Samples.Coordinates == Published.Coordinates);
		auto ProviderError = 0.f;
		for (auto Index = 0; Index < 6000; ++Index)
		{
			const auto Difference = Published.Colors[Index] - Tints[static_cast<int32>(Samples.Positions[Index].A)];
			ProviderError = FMath::Max(ProviderError, FMath::Max3(FMath::Abs(Difference.R), FMath::Abs(Difference.G), FMath::Abs(Difference.B)));
		}
		this->AddInfo(FString::Printf(TEXT("Published provider material maximum RGB difference %.6f."), ProviderError));
		this->TestTrue(TEXT("Provider overrides Base Color before surface tint"), ProviderError < 0.01f);
	}
	return true;
}
#endif
