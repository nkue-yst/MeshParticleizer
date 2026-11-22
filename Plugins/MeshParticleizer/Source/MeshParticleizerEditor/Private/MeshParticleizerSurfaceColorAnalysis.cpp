#include "MeshParticleizerPreparationLibrary.h"
#include "Engine/Texture2D.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSetMaterialAttributes.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"

namespace MeshParticleizerColorAnalysis
{
	struct FAnalysis
	{
		UMaterialInterface* Source = nullptr;
		FMeshParticleizerTextureColorSource Result;
		TSet<const UMaterialExpression*> Visiting;
		int32 TextureReads = 0;
		int32 VisitedInputs = 0;
		const UMaterialExpression* TextureExpression = nullptr;

		bool Fail(const FString& Reason)
		{
			this->Result.Diagnostics = Reason;
			return false;
		}

		bool ReadTexture(UMaterialExpressionTextureSample* Sample)
		{
			// SubUVなどの派生ノードは、通常のUV0サンプルと同じ評価にはならない。
			if (Sample->GetClass() != UMaterialExpressionTextureSample::StaticClass() &&
				Sample->GetClass() != UMaterialExpressionTextureSampleParameter2D::StaticClass())
			{
				return this->Fail(TEXT("Use baking or a color provider: specialized texture sample nodes need a dedicated binding."));
			}

			const auto* Parameter = Cast<UMaterialExpressionTextureSampleParameter2D>(Sample);
			if (++this->TextureReads != 1 || (Parameter != nullptr && Parameter->ParameterName.IsNone()))
			{
				return this->Fail(TEXT("Use baking or a color provider: the color needs more than one texture sample or has no texture parameter."));
			}
			if (Sample->TextureObject.Expression != nullptr || Sample->MipValueMode != TMVM_None ||
				Sample->GatherMode != TGM_None || Sample->SamplerSource != SSM_FromTextureAsset ||
				(Sample->SamplerType != SAMPLERTYPE_Color && Sample->SamplerType != SAMPLERTYPE_LinearColor))
			{
				return this->Fail(TEXT("Use baking or a color provider: custom texture objects, mip controls, gathers and non-color/shared samplers need a dedicated binding."));
			}

			// パラメータの既定値ではなく、このMaterial Instanceが実際に参照する値を保存する。
			UTexture* Resolved = Sample->Texture;
			if (Parameter != nullptr)
			{
				this->Source->GetTextureParameterValue(FMaterialParameterInfo(Parameter->ParameterName), Resolved);
			}
			auto* Texture = Cast<UTexture2D>(Resolved);
			if (Texture == nullptr || Texture->VirtualTextureStreaming == true ||
				(Sample->SamplerType == SAMPLERTYPE_Color) != (Texture->SRGB != 0))
			{
				return this->Fail(TEXT("Use a matching regular color Texture2D. Virtual textures and mismatched sRGB samplers are not direct color bindings."));
			}
			this->Result.Texture = Texture;
			this->TextureExpression = Sample;
			this->Result.TextureParameter = Parameter != nullptr ? Parameter->ParameterName : NAME_None;
			this->Result.SampleExpressionGuid = Sample->MaterialExpressionGuid;
			this->Result.bRequiresTextureParameter = Parameter == nullptr;

			// 候補座標はUV0。反転解除や別UV、時間・ワールド依存の座標を固定倍率として扱わない。
			auto* UV = Cast<UMaterialExpressionTextureCoordinate>(Sample->Coordinates.Expression);
			if (Sample->Coordinates.Expression == nullptr)
			{
				if (Sample->ConstCoordinate != 0)
				{
					return this->Fail(TEXT("A direct texture color binding requires UV0."));
				}
			}
			else if (UV == nullptr || Sample->Coordinates.OutputIndex != 0 ||
				(Sample->Coordinates.Mask != 0 && (Sample->Coordinates.MaskR == 0 || Sample->Coordinates.MaskG == 0 || Sample->Coordinates.MaskB != 0 || Sample->Coordinates.MaskA != 0)) || UV->CoordinateIndex != 0 ||
				UV->UnMirrorU != 0 || UV->UnMirrorV != 0 || FMath::IsFinite(UV->UTiling) == false || FMath::IsFinite(UV->VTiling) == false)
			{
				return this->Fail(TEXT("Use baking or a color provider: texture coordinates are not a fixed scaling of UV0."));
			}
			else
			{
				this->Result.UVScale = FVector2D(UV->UTiling, UV->VTiling);
			}
			return true;
		}

		bool ReadColor(const FExpressionInput& Input, float Unconnected = 1)
		{
			auto* Expression = Input.Expression;
			if (Expression == nullptr)
			{
				this->Result.ColorScale *= Unconnected;
				return true;
			}
			if (this->Visiting.Contains(Expression) || this->Visiting.Num() >= 64 || ++this->VisitedInputs > 256)
			{
				return this->Fail(TEXT("The color graph contains a cycle or exceeds the supported analysis depth."));
			}
			if (Input.OutputIndex != 0 || (Input.Mask != 0 && (Input.MaskR == 0 || Input.MaskG == 0 || Input.MaskB == 0 || Input.MaskA != 0)))
			{
				return this->Fail(TEXT("Use baking or a color provider: color channel remapping needs a dedicated binding."));
			}
			this->Visiting.Add(Expression);
			const auto bResult = this->ReadExpression(Expression);
			this->Visiting.Remove(Expression);
			return bResult;
		}

		bool ReadExpression(UMaterialExpression* Expression)
		{
			if (auto* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				return this->ReadColor(Multiply->A, Multiply->ConstA) && this->ReadColor(Multiply->B, Multiply->ConstB);
			}
			if (auto* Reroute = Cast<UMaterialExpressionReroute>(Expression))
			{
				return this->ReadColor(Reroute->Input);
			}
			if (auto* Texture = Cast<UMaterialExpressionTextureSample>(Expression))
			{
				return this->ReadTexture(Texture);
			}
			if (const auto* Constant = Cast<UMaterialExpressionConstant>(Expression))
			{
				this->Result.ColorScale *= Constant->R;
				return true;
			}
			if (const auto* Constant = Cast<UMaterialExpressionConstant3Vector>(Expression))
			{
				this->Result.ColorScale *= Constant->Constant;
				return true;
			}
			if (const auto* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				if (Parameter->bUseCustomPrimitiveData == true)
				{
					return this->Fail(TEXT("Custom Primitive Data requires a registered Color Provider."));
				}
				auto Value = FLinearColor{};
				if (this->Source->GetVectorParameterValue(FMaterialParameterInfo(Parameter->ParameterName), Value) == false)
				{
					return this->Fail(TEXT("The source vector parameter could not be resolved."));
				}
				this->Result.ColorScale *= Value;
				return true;
			}
			if (const auto* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				if (Parameter->bUseCustomPrimitiveData == true)
				{
					return this->Fail(TEXT("Custom Primitive Data requires a registered Color Provider."));
				}
				auto Value = 0.0f;
				if (this->Source->GetScalarParameterValue(FMaterialParameterInfo(Parameter->ParameterName), Value) == false)
				{
					return this->Fail(TEXT("The source scalar parameter could not be resolved."));
				}
				this->Result.ColorScale *= Value;
				return true;
			}
			return this->Fail(FString::Printf(TEXT("Use baking or a color provider: %s is not a texture multiplied by uniform color factors."), *Expression->GetClass()->GetName()));
		}
	};

	// 属性の未接続を成功と区別する。複雑な属性ブレンドは色だけを推測して取り出さない。
	bool ReadAttribute(const FExpressionInput& Input, EMaterialProperty Property, FExpressionInput& Out, TSet<const UMaterialExpression*>& Visited)
	{
		auto* Expression = Input.Expression;
		if (Expression == nullptr)
		{
			Out = FExpressionInput{};
			return true;
		}
		if (Input.OutputIndex != 0 || Visited.Contains(Expression) || Visited.Num() >= 64)
		{
			return false;
		}
		Visited.Add(Expression);
		if (const auto* Reroute = Cast<UMaterialExpressionReroute>(Expression))
		{
			return ReadAttribute(Reroute->Input, Property, Out, Visited);
		}
		if (const auto* Attributes = Cast<UMaterialExpressionMakeMaterialAttributes>(Expression))
		{
			Out = Property == MP_EmissiveColor ? Attributes->EmissiveColor : (Property == MP_BaseColor ? Attributes->BaseColor : Attributes->OpacityMask);
			return true;
		}
		if (const auto* Attributes = Cast<UMaterialExpressionSetMaterialAttributes>(Expression))
		{
			const auto Index = Attributes->AttributeSetTypes.IndexOfByKey(FMaterialAttributeDefinitionMap::GetID(Property));
			const auto InputIndex = Index == INDEX_NONE ? 0 : Index + 1;
			if (Attributes->Inputs.IsValidIndex(InputIndex) == false)
			{
				return false;
			}
			if (Index != INDEX_NONE)
			{
				Out = Attributes->Inputs[InputIndex];
				return true;
			}
			return ReadAttribute(Attributes->Inputs[0], Property, Out, Visited);
		}
		return false;
	}

	bool HasIndependentMask(const FExpressionInput& Input, const UMaterialExpression* ColorSample, FName TextureParameter,
		TSet<const UMaterialExpression*>& Visited, int32& InputCount)
	{
		auto* Expression = Input.Expression;
		if (Expression == nullptr)
		{
			return true;
		}
		if (Visited.Contains(Expression) || Visited.Num() >= 64 || ++InputCount > 256)
		{
			return false;
		}
		Visited.Add(Expression);
		if (Expression == ColorSample || (TextureParameter.IsNone() == false && Expression->HasAParameterName() && Expression->GetParameterName() == TextureParameter))
		{
			return false;
		}
		// 関数内部の同名パラメータやCustom HLSLを、入力ピンだけから無関係と判定しない。
		if (Expression->IsA<UMaterialExpressionMaterialFunctionCall>() || Expression->IsA<UMaterialExpressionCustom>())
		{
			return false;
		}
		for (auto It = FExpressionInputIterator(Expression); It; ++It)
		{
			if (HasIndependentMask(*It.Input, ColorSample, TextureParameter, Visited, InputCount) == false)
			{
				return false;
			}
		}
		Visited.Remove(Expression);
		return true;
	}
}

FMeshParticleizerTextureColorSource UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(UMaterialInterface* Source, bool bUseUnlitEmissive)
{
	using namespace MeshParticleizerColorAnalysis;
	auto Analysis = FAnalysis{};
	Analysis.Source = Source;
	auto* Material = Source != nullptr ? Source->GetMaterial() : nullptr;
	if (Material == nullptr || Material->MaterialDomain != MD_Surface ||
		(Source->GetBlendMode() != BLEND_Opaque && Source->GetBlendMode() != BLEND_Masked) ||
		(Source->GetShadingModels().HasShadingModel(MSM_Unlit) &&
			(bUseUnlitEmissive == false || Source->GetShadingModels().HasOnlyShadingModel(MSM_Unlit) == false)))
	{
		Analysis.Fail(TEXT("Direct color analysis requires an opaque or masked surface. Unlit requires explicit Use Unlit Emissive selection."));
		return Analysis.Result;
	}

	// Front Material接続時の色を、使われていない従来のBase Colorから推測しない。
	if (Material->GetEditorOnlyData()->FrontMaterial.Expression != nullptr)
	{
		Analysis.Fail(TEXT("A Substrate Front Material requires baking or a dedicated color provider."));
		return Analysis.Result;
	}

	auto Color = FExpressionInput{};
	auto Mask = FExpressionInput{};
	const auto Property = Source->GetShadingModels().HasShadingModel(MSM_Unlit) ? MP_EmissiveColor : MP_BaseColor;
	if (Material->bUseMaterialAttributes == true)
	{
		auto Visited = TSet<const UMaterialExpression*>{};
		const auto& Attributes = Material->GetEditorOnlyData()->MaterialAttributes;
		if (ReadAttribute(Attributes, Property, Color, Visited) == false)
		{
			Analysis.Fail(TEXT("The surface color attribute requires baking or a color provider."));
			return Analysis.Result;
		}
		Visited.Reset();
		if (ReadAttribute(Attributes, MP_OpacityMask, Mask, Visited) == false)
		{
			Analysis.Fail(TEXT("The opacity attribute cannot be proven independent of the color texture."));
			return Analysis.Result;
		}
	}
	else
	{
		const auto& Base = Property == MP_EmissiveColor ? Material->GetEditorOnlyData()->EmissiveColor : Material->GetEditorOnlyData()->BaseColor;
		if (Base.UseConstant == false)
		{
			Color = Base;
		}
		const auto& Opacity = Material->GetEditorOnlyData()->OpacityMask;
		if (Opacity.UseConstant == false)
		{
			Mask = Opacity;
		}
	}

	auto Visited = TSet<const UMaterialExpression*>{};
	auto MaskInputCount = 0;
	if (Analysis.ReadColor(Color) && Analysis.TextureReads == 1 &&
		FMath::IsFinite(Analysis.Result.ColorScale.R) && FMath::IsFinite(Analysis.Result.ColorScale.G) && FMath::IsFinite(Analysis.Result.ColorScale.B) &&
		(Source->GetBlendMode() != BLEND_Masked || HasIndependentMask(Mask, Analysis.TextureExpression, Analysis.Result.TextureParameter, Visited, MaskInputCount)))
	{
		Analysis.Result.bCompatible = true;
		Analysis.Result.ColorScale.A = 1;
		Analysis.Result.Diagnostics = TEXT("Texture And Tint candidate: one color Texture2D sample, fixed UV0 scale and uniform color factors. Playback remains baked until a dynamic binding is registered.");
	}
	else
	{
		if (Analysis.Result.Diagnostics.IsEmpty())
		{
			Analysis.Fail(TEXT("No independent finite Texture And Tint binding was found. Keep baking or use a color provider."));
		}
		const auto Reason = Analysis.Result.Diagnostics;
		Analysis.Result = FMeshParticleizerTextureColorSource{};
		Analysis.Result.Diagnostics = Reason;
	}
	return Analysis.Result;
}
