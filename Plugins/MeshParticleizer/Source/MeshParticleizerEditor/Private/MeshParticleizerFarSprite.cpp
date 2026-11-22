#include "MeshParticleizerFarSprite.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionParticlePositionWS.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionWorldPosition.h"

namespace
{
	template <typename T>
	T* Add(UMaterial* Material)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass()));
	}

	void Input(UMaterialExpressionCustom* Expression, FName Name, UMaterialExpression* Source, int32 Output = 0)
	{
		auto& Entry = Expression->Inputs.AddDefaulted_GetRef();
		Entry.InputName = Name;
		Entry.Input.Connect(Output, Source);
	}
}

void MeshParticleizerConfigureFarSpriteMaterial(UMaterial* Material, float Distance, bool bCircle)
{
	auto* Position = Add<UMaterialExpressionParticlePositionWS>(Material);
	auto* Camera = Add<UMaterialExpressionCameraPositionWS>(Material);
	auto* Delta = Add<UMaterialExpressionSubtract>(Material);
	Delta->A.Connect(0, Position);
	Delta->B.Connect(0, Camera);
	auto* Limit = Add<UMaterialExpressionScalarParameter>(Material);
	Limit->ParameterName = TEXT("MP_FarSpriteDistance");
	Limit->DefaultValue = Distance;
	auto* Gate = Add<UMaterialExpressionCustom>(Material);
	Gate->OutputType = CMOT_Float1;
	Gate->Inputs.Reset();
	Input(Gate, TEXT("Delta"), Delta);
	Input(Gate, TEXT("Distance"), Limit);

	// メッシュの上限は境界を含む。Spriteは境界の外側だけを担当する。
	Gate->Code = TEXT("return dot(Delta, Delta) > Distance * Distance ? 1.0 : 0.0;");
	Gate->Description = TEXT("Mesh Particleizer far Sprite gate");

	auto* UV = Add<UMaterialExpressionTextureCoordinate>(Material);
	auto* MaskInput = Material->GetExpressionInputForProperty(MP_OpacityMask);
	auto* Mask = Add<UMaterialExpressionCustom>(Material);
	Mask->OutputType = CMOT_Float1;
	Mask->Inputs.Reset();
	Input(Mask, TEXT("Gate"), Gate);
	Input(Mask, TEXT("Mask"), MaskInput->Expression, MaskInput->OutputIndex);
	Input(Mask, TEXT("UV"), UV);
	Mask->Code = bCircle ? TEXT("return Gate * Mask * (dot(UV - 0.5, UV - 0.5) < 0.24 ? 1.0 : 0.0);") : TEXT("return Gate * Mask;");
	MaskInput->Connect(0, Mask);

	// 近距離ではラスタ化を省く。既存の診断WPOも遠方側だけに適用する。
	auto* Vertex = Add<UMaterialExpressionWorldPosition>(Material);
	Vertex->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;
	auto* VertexOffset = Add<UMaterialExpressionSubtract>(Material);
	VertexOffset->A.Connect(0, Vertex);
	VertexOffset->B.Connect(0, Position);
	auto* WpoInput = Material->GetExpressionInputForProperty(MP_WorldPositionOffset);
	auto* Wpo = Add<UMaterialExpressionCustom>(Material);
	Wpo->OutputType = CMOT_Float3;
	Wpo->Inputs.Reset();
	Input(Wpo, TEXT("Gate"), Gate);
	Input(Wpo, TEXT("VertexOffset"), VertexOffset);
	if (WpoInput->Expression != nullptr)
	{
		Input(Wpo, TEXT("Offset"), WpoInput->Expression, WpoInput->OutputIndex);
	}
	else
	{
		Input(Wpo, TEXT("Offset"), Add<UMaterialExpressionConstant>(Material));
	}
	Wpo->Code = TEXT("return Gate > 0.0 ? Offset : -VertexOffset;");
	WpoInput->Connect(0, Wpo);
}
