#include "MeshParticleizerSurfaceBaker.h"
#include "MaterialCompiler.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionQualitySwitch.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Engine/Texture2D.h"
#include "MeshParticleizerPreparationLibrary.h"

namespace MeshParticleizerPreparation
{
	namespace
	{
		struct FDependencyUsage
		{
			TArray<FString> Dynamic;
			TArray<FString> Frozen;
			TArray<FName> Scalars;
			TArray<FName> Vectors;
			TArray<FString> UnsupportedCoverage;
			bool bUnclassifiedCoverageInput = false;
			bool bFailed = false;
		};

		// UEのコンパイル経路で、選択された関数出力・属性・静的分岐だけを追う。
		// Custom HLSLの暗黙の外部参照は解析できないため、固定色と判定しない。
		class FDependencyCompiler : public FProxyMaterialCompiler
		{
		public:
			FDependencyCompiler(FMaterialCompiler* InCompiler, FDependencyUsage& InUsage,
				EMaterialQualityLevel::Type InQuality = EMaterialQualityLevel::Num)
				: FProxyMaterialCompiler(InCompiler), Usage(InUsage), Quality(InQuality)
			{
			}

			virtual EMaterialQualityLevel::Type GetQualityLevel() override
			{
				return this->Quality != EMaterialQualityLevel::Num ? this->Quality : FProxyMaterialCompiler::GetQualityLevel();
			}

			virtual int32 CallExpression(FMaterialExpressionKey Key, FMaterialCompiler* InCompiler) override
			{
				if (this->Quality != EMaterialQualityLevel::Num && Key.Expression != nullptr)
				{
					// 動的Maskには実際に供給できる入力だけを許可する。未知の式を定数扱いしない。
					static const TSet<FName> Supported = {TEXT("Add"), TEXT("Subtract"), TEXT("Multiply"), TEXT("Divide"), TEXT("Abs"), TEXT("Min"), TEXT("Max"), TEXT("Clamp"), TEXT("Saturate"), TEXT("OneMinus"), TEXT("Power"), TEXT("SquareRoot"), TEXT("Normalize"), TEXT("DotProduct"), TEXT("CrossProduct"), TEXT("AppendVector"), TEXT("ComponentMask"), TEXT("Floor"), TEXT("Ceil"), TEXT("Frac"), TEXT("Fmod"), TEXT("Round"), TEXT("Truncate"), TEXT("Sign"), TEXT("Logarithm2"), TEXT("Logarithm10"), TEXT("Sine"), TEXT("Cosine"), TEXT("Tangent"), TEXT("Arcsine"), TEXT("Arccosine"), TEXT("Arctangent"), TEXT("Arctangent2"), TEXT("If"), TEXT("LinearInterpolate"), TEXT("Step"), TEXT("SmoothStep"), TEXT("ConstantBiasScale"), TEXT("Constant"), TEXT("Constant2Vector"), TEXT("Constant3Vector"), TEXT("Constant4Vector"), TEXT("ScalarParameter"), TEXT("VectorParameter"), TEXT("TextureCoordinate"), TEXT("TextureSample"), TEXT("TextureSampleParameter2D"), TEXT("TextureObject"), TEXT("TextureObjectParameter"), TEXT("MaterialFunctionCall"), TEXT("FunctionInput"), TEXT("FunctionOutput"), TEXT("MakeMaterialAttributes"), TEXT("GetMaterialAttributes"), TEXT("SetMaterialAttributes"), TEXT("BreakMaterialAttributes"), TEXT("StaticSwitch"), TEXT("StaticSwitchParameter"), TEXT("StaticBool"), TEXT("StaticBoolParameter"), TEXT("StaticComponentMaskParameter"), TEXT("Reroute"), TEXT("NamedRerouteDeclaration"), TEXT("NamedRerouteUsage"), TEXT("QualitySwitch"), TEXT("CameraVectorWS"), TEXT("VertexNormalWS")};
					auto Type = Key.Expression->GetClass()->GetName();
					Type.RemoveFromStart(TEXT("MaterialExpression"));
					if (Supported.Contains(FName(Type)) == false)
					{
						this->Usage.UnsupportedCoverage.AddUnique(Type);

						// 頂点色は固定ベイクへ渡せる。その他の未知の式を固定入力と推定しない。
						this->Usage.bUnclassifiedCoverageInput |= Type != TEXT("VertexColor");
					}
					if (const auto* UV = Cast<UMaterialExpressionTextureCoordinate>(Key.Expression))
					{
						if (UV->CoordinateIndex != 0 || UV->UnMirrorU || UV->UnMirrorV)
						{
							this->Usage.UnsupportedCoverage.AddUnique(TEXT("UV channels other than UV0 or unmirroring"));
						}
					}
					if (const auto* Sample = Cast<UMaterialExpressionTextureSample>(Key.Expression))
					{
						// Camera/Normalの画面微分は供給していない。Textureの座標はUV0の直接入力に限定する。
						const auto Coordinates = Sample->Coordinates.GetTracedInput();
						if ((Coordinates.Expression != nullptr && Cast<UMaterialExpressionTextureCoordinate>(Coordinates.Expression) == nullptr) ||
							(Coordinates.Expression == nullptr && Sample->ConstCoordinate != 0) || Sample->MipValueMode == TMVM_Derivative)
						{
							this->Usage.UnsupportedCoverage.AddUnique(TEXT("computed texture coordinates or custom derivatives (coverage requires direct UV0)"));
						}
						const auto* Texture = Cast<UTexture2D>(Sample->Texture);
						if (Sample->Texture != nullptr && (Texture == nullptr || Texture->VirtualTextureStreaming))
						{
							this->Usage.UnsupportedCoverage.AddUnique(TEXT("non-2D or virtual texture"));
						}
					}
				}
				// 現在の品質だけをベイクしても、別品質の色や穴は再現できない。
				// 未使用の分岐はUEが呼び出さない。同じ接続への品質別入力は変更とみなさない。
				if (const auto* Switch = Cast<UMaterialExpressionQualitySwitch>(Key.Expression))
				{
					const auto Default = Switch->Default.GetTracedInput();
					for (const auto& Input : Switch->Inputs)
					{
						const auto Branch = Input.GetTracedInput();
						if (Branch.Expression != nullptr && (Branch.Expression != Default.Expression || Branch.OutputIndex != Default.OutputIndex || Branch.Mask != Default.Mask || Branch.MaskR != Default.MaskR || Branch.MaskG != Default.MaskG || Branch.MaskB != Default.MaskB || Branch.MaskA != Default.MaskA))
						{
							this->Usage.Dynamic.AddUnique(TEXT("Material Quality Switch"));
							break;
						}
					}
				}
				return FProxyMaterialCompiler::CallExpression(Key, InCompiler);
			}

			virtual int32 Error(const TCHAR* Text) override
			{
				this->Usage.bFailed = true;
				return FProxyMaterialCompiler::Error(Text);
			}

			virtual int32 NumericParameter(EMaterialParameterType Type, FName Name, const UE::Shader::FValue& Default) override
			{
				if (Type == EMaterialParameterType::Scalar)
				{
					this->Usage.Scalars.AddUnique(Name);
				}
				if (Type == EMaterialParameterType::Vector)
				{
					this->Usage.Vectors.AddUnique(Name);
				}
				return FProxyMaterialCompiler::NumericParameter(Type, Name, Default);
			}

			virtual int32 WorldPosition(EWorldPositionIncludedOffsets Offsets) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("World Position"));
				return FProxyMaterialCompiler::WorldPosition(Offsets);
			}

			virtual int32 ActorWorldPosition(EPositionOrigin Origin) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Actor Position"));
				return FProxyMaterialCompiler::ActorWorldPosition(Origin);
			}

			virtual int32 ObjectWorldPosition(EPositionOrigin Origin) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Object Position"));
				return FProxyMaterialCompiler::ObjectWorldPosition(Origin);
			}

			virtual int32 CustomPrimitiveData(int32 Index, EMaterialValueType Type) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Custom Primitive Data"));
				return FProxyMaterialCompiler::CustomPrimitiveData(Index, Type);
			}

			virtual int32 AccessCollectionParameter(UMaterialParameterCollection* Collection, int32 Index, int32 Component) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Material Parameter Collection"));
				return FProxyMaterialCompiler::AccessCollectionParameter(Collection, Index, Component);
			}

			virtual int32 PerInstanceRandom() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Per Instance Random"));
				return FProxyMaterialCompiler::PerInstanceRandom();
			}

			virtual int32 PerInstanceCustomData(int32 Index, int32 Default) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Per Instance Custom Data"));
				return FProxyMaterialCompiler::PerInstanceCustomData(Index, Default);
			}

			virtual int32 PerInstanceCustomData3Vector(int32 Index, int32 Default) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Per Instance Custom Data"));
				return FProxyMaterialCompiler::PerInstanceCustomData3Vector(Index, Default);
			}

			virtual int32 ObjectOrientation() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Object Orientation"));
				return FProxyMaterialCompiler::ObjectOrientation();
			}

			virtual int32 ObjectBounds() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Object Bounds"));
				return FProxyMaterialCompiler::ObjectBounds();
			}

			virtual int32 ObjectRadius() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Object Radius"));
				return FProxyMaterialCompiler::ObjectRadius();
			}

			virtual int32 VertexNormal() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Vertex Normal"));
				return FProxyMaterialCompiler::VertexNormal();
			}

			virtual int32 PixelNormalWS() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Pixel Normal"));
				return FProxyMaterialCompiler::PixelNormalWS();
			}

			virtual int32 CustomExpression(UMaterialExpressionCustom* Expression, int32 Output, TArray<int32>& Inputs) override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Custom HLSL (external dependencies unverified)"));
				return FProxyMaterialCompiler::CustomExpression(Expression, Output, Inputs);
			}

			virtual int32 GameTime(bool bPeriodic, float Period) override
			{
				this->Usage.Frozen.AddUnique(TEXT("Time"));
				return FProxyMaterialCompiler::GameTime(bPeriodic, Period);
			}

			virtual int32 RealTime(bool bPeriodic, float Period) override
			{
				this->Usage.Frozen.AddUnique(TEXT("Real Time"));
				return FProxyMaterialCompiler::RealTime(bPeriodic, Period);
			}

			virtual int32 ViewProperty(EMaterialExposedViewProperty Property, bool bInverse) override
			{
				this->Usage.Frozen.AddUnique(TEXT("View / Camera"));
				return FProxyMaterialCompiler::ViewProperty(Property, bInverse);
			}

			virtual int32 CameraVector() override
			{
				this->Usage.Frozen.AddUnique(TEXT("Camera Vector"));
				return FProxyMaterialCompiler::CameraVector();
			}

			virtual int32 ReflectionVector() override
			{
				this->Usage.Frozen.AddUnique(TEXT("Reflection Vector"));
				return FProxyMaterialCompiler::ReflectionVector();
			}

			virtual int32 ReflectionAboutCustomWorldNormal(int32 Normal, int32 Normalize) override
			{
				this->Usage.Frozen.AddUnique(TEXT("Reflection Vector"));
				return this->Compiler->ReflectionAboutCustomWorldNormal(Normal, Normalize);
			}

			// UEのProxyに委譲がない入力。粒子固有値は固定表面の入力として扱わない。
			virtual int32 ParticleRelativeTime() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Time"));
				return this->Compiler->ParticleRelativeTime();
			}
			virtual int32 ParticleMotionBlurFade() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Motion"));
				return this->Compiler->ParticleMotionBlurFade();
			}
			virtual int32 ParticleRandom() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Random"));
				return this->Compiler->ParticleRandom();
			}
			virtual int32 ParticleDirection() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Direction"));
				return this->Compiler->ParticleDirection();
			}
			virtual int32 ParticleSpeed() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Speed"));
				return this->Compiler->ParticleSpeed();
			}
			virtual int32 ParticleSize() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Size"));
				return this->Compiler->ParticleSize();
			}
			virtual int32 ParticleSpriteRotation() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("Particle Rotation"));
				return this->Compiler->ParticleSpriteRotation();
			}

			virtual int32 VertexInterpolator(uint32 Index) override
			{
				// プロパティ単位の解析には頂点補間の事前パスがない。未解析を成功にしない。
				return this->Error(TEXT("Vertex Interpolator requires a material adapter for dependency analysis."));
			}

			virtual int32 MaterialBakingWorldPosition() override
			{
				this->Usage.Dynamic.AddUnique(TEXT("World Position"));
				return this->Compiler->MaterialBakingWorldPosition();
			}

		private:
			FDependencyUsage& Usage;
			EMaterialQualityLevel::Type Quality;
		};

		FDependencyUsage AnalyzeProperty(UMaterialInterface* Source, EMaterialProperty Property, bool bCoverage = false)
		{
			auto Usage = FDependencyUsage{};
			const auto WeakSource = TWeakObjectPtr<UMaterialInterface>(Source);
			// Maskの候補を全品質で共有するため、現在非選択の品質分岐も検証する。
			for (auto Quality = 0; Quality < (bCoverage ? int32(EMaterialQualityLevel::Num) : 1); ++Quality)
			{
				auto Result = FMaterialAnalysisResult{};
				Source->AnalyzeMaterialCompilationInCallback([WeakSource, Property, &Usage, bCoverage, Quality](FMaterialCompiler* Compiler) {
					auto Proxy = FDependencyCompiler(Compiler, Usage, bCoverage ? static_cast<EMaterialQualityLevel::Type>(Quality) : EMaterialQualityLevel::Num);
					Proxy.SetMaterialProperty(Property, SF_NumFrequencies, false);
					if (WeakSource.IsValid() == false || WeakSource->CompileProperty(&Proxy, Property) == INDEX_NONE)
					{
						Usage.bFailed = true;
					}
				},
					Result);
			}
			return Usage;
		}
	}

	bool ValidateSourceEmissionDependencies(UMaterialInterface* Material, FString& OutError)
	{
		const auto Usage = AnalyzeProperty(Material, MP_EmissiveColor);
		if (Usage.bFailed == true || Usage.Dynamic.IsEmpty() == false || Usage.Frozen.IsEmpty() == false)
		{
			OutError = TEXT("Original emission baking requires a fixed Emissive input. Dynamic emission needs an emission provider: ") +
				FString::Join(Usage.Dynamic, TEXT(", ")) + TEXT(" ") + FString::Join(Usage.Frozen, TEXT(", "));
			return false;
		}
		return true;
	}

	bool AnalyzeSurfaceDependencies(UMaterialInterface* Material, bool bUseUnlitEmissive,
		FMeshParticleizerTextureColorSource& OutSource, FString& OutError)
	{
		OutSource.bDependenciesAnalyzed = false;
		OutSource.DependencyAnalysisVersion = 0;
		OutSource.bRequiresColorProvider = false;
		OutSource.bUsesFrozenInputs = false;
		OutSource.bRequiresRuntimeCoverage = false;
		OutSource.DependencyDiagnostics.Reset();
		OutSource.ProviderScalarParameters.Reset();
		OutSource.ProviderVectorParameters.Reset();
		OutError.Reset();
		if (Material != nullptr && Material->GetShadingModels().HasShadingModel(MSM_Unlit) &&
			(bUseUnlitEmissive == false || Material->GetShadingModels().HasOnlyShadingModel(MSM_Unlit) == false))
		{
			OutError = TEXT("Enable Use Unlit Emissive to bake Emissive RGB as 0..1 surface color. Mixed shading models require a material adapter.");
			return false;
		}
		if (Material == nullptr || Material->GetMaterial() == nullptr ||
			Material->GetMaterial()->GetEditorOnlyData()->FrontMaterial.Expression != nullptr ||
			(Material->GetBlendMode() != BLEND_Opaque && Material->GetBlendMode() != BLEND_Masked))
		{
			OutError = TEXT("Dependency analysis requires a supported surface material and explicit Unlit selection.");
			return false;
		}

		// 色は登録Providerで置き換えられる。変化する穴は元の材質を再生中に評価する。
		const auto ColorProperty = Material->GetShadingModels().HasShadingModel(MSM_Unlit) ? MP_EmissiveColor : MP_BaseColor;
		const auto Color = AnalyzeProperty(Material, ColorProperty);
		const auto Mask = Material->GetBlendMode() == BLEND_Masked ? AnalyzeProperty(Material, MP_OpacityMask, true) : FDependencyUsage{};
		if (Color.bFailed || Mask.bFailed)
		{
			OutError = TEXT("Material dependency compilation failed. Repair the material before preparation.");
			return false;
		}
		const auto bDitheredMask = Material->GetBlendMode() == BLEND_Masked && Material->GetMaterial()->DitherOpacityMask;
		if (bDitheredMask == true || Mask.bUnclassifiedCoverageInput == true ||
			Mask.Dynamic.IsEmpty() == false || Mask.Frozen.IsEmpty() == false)
		{
			auto Unsupported = Mask.UnsupportedCoverage;
			for (const auto& Input : Mask.Dynamic)
			{
				if (Input != TEXT("Vertex Normal") && Input != TEXT("Material Quality Switch"))
				{
					Unsupported.AddUnique(Input);
				}
			}
			for (const auto& Input : Mask.Frozen)
			{
				if (Input != TEXT("Camera Vector"))
				{
					Unsupported.AddUnique(Input);
				}
			}
			if (Material->GetMaterial()->HasVertexPositionOffsetConnected())
			{
				Unsupported.AddUnique(TEXT("World Position Offset deformation"));
			}
			auto Textures = TArray<UTexture*>{};
			Material->GetUsedTextures(Textures);
			for (const auto* Texture : Textures)
			{
				const auto* Texture2D = Cast<UTexture2D>(Texture);
				if (Texture2D == nullptr || Texture2D->VirtualTextureStreaming)
				{
					Unsupported.AddUnique(TEXT("non-2D or virtual texture after material overrides"));
				}
			}
			if (Unsupported.IsEmpty() == false)
			{
				OutError = TEXT("Opacity Mask requires a coverage adapter: ") + FString::Join(Unsupported, TEXT(", "));
				return false;
			}
			OutSource.bRequiresRuntimeCoverage = true;
		}

		// Providerが置き換える色以外にも使う値は、ベイク時のまま固定する。
		// 同名のレイヤー値も保守的に除外し、Custom出力など未解析の経路を許可しない。
		auto Scalars = Color.Scalars;
		auto Vectors = Color.Vectors;
		auto CustomOutputs = TArray<UMaterialExpressionCustomOutput*>{};
		Material->GetMaterial()->GetAllExpressionsInMaterialAndFunctionsOfType(CustomOutputs);
		auto bFallbackAnalyzed = CustomOutputs.IsEmpty();
		for (auto Index = 0; bFallbackAnalyzed && (Scalars.IsEmpty() == false || Vectors.IsEmpty() == false) && Index < MP_MaterialAttributes; ++Index)
		{
			const auto Property = static_cast<EMaterialProperty>(Index);
			if (Property == ColorProperty || Property == MP_WorldDisplacement_DEPRECATED ||
				Property == MP_TessellationMultiplier_DEPRECATED || Property == MP_FrontMaterial || Material->IsPropertyActive(Property) == false)
			{
				continue;
			}
			const auto Other = Property == MP_OpacityMask ? Mask : AnalyzeProperty(Material, Property);
			bFallbackAnalyzed = Other.bFailed == false && Other.Dynamic.Contains(TEXT("Custom HLSL (external dependencies unverified)")) == false && Other.Dynamic.Contains(TEXT("Material Quality Switch")) == false;
			for (const auto Name : Other.Scalars)
			{
				Scalars.Remove(Name);
			}
			for (const auto Name : Other.Vectors)
			{
				Vectors.Remove(Name);
			}
		}
		if (bFallbackAnalyzed == true)
		{
			OutSource.ProviderScalarParameters = MoveTemp(Scalars);
			OutSource.ProviderVectorParameters = MoveTemp(Vectors);
		}

		OutSource.bDependenciesAnalyzed = true;
		OutSource.DependencyAnalysisVersion = FMeshParticleizerTextureColorSource::CurrentDependencyAnalysisVersion;
		OutSource.bRequiresColorProvider = Color.Dynamic.IsEmpty() == false;
		OutSource.bUsesFrozenInputs = Color.Frozen.IsEmpty() == false;
		if (OutSource.bRequiresColorProvider == true)
		{
			OutSource.bCompatible = false;
			OutSource.DependencyDiagnostics = TEXT("Register a Color Provider before starting: ") + FString::Join(Color.Dynamic, TEXT(", "));
		}
		if (OutSource.bUsesFrozenInputs == true)
		{
			OutSource.DependencyDiagnostics += TEXT(" Baked snapshot inputs: ") + FString::Join(Color.Frozen, TEXT(", "));
		}
		if (OutSource.bRequiresRuntimeCoverage)
		{
			OutSource.DependencyDiagnostics += TEXT(" Runtime Opacity Mask uses the current view, vertex normal, material quality and enabled dither. A full-surface candidate bake is required.");
		}
		return true;
	}

	bool AnalyzeSurfaceMaterials(TConstArrayView<UMaterialInterface*> Materials, bool bUseUnlitEmissive, bool bBakeSourceEmission,
		TArray<FMeshParticleizerTextureColorSource>& OutSources, FString& OutError)
	{
		OutSources.Reset();
		OutError.Reset();
		if (Materials.IsEmpty())
		{
			OutError = TEXT("The mesh has no material slots.");
			return false;
		}

		// 最初の失敗だけで止めず、全スロットを診断してから生成の可否を返す。
		auto Sources = TArray<FMeshParticleizerTextureColorSource>{};
		auto Errors = TArray<FString>{};
		for (auto Slot = 0; Slot < Materials.Num(); ++Slot)
		{
			auto* Material = Materials[Slot];
			auto Source = UMeshParticleizerPreparationLibrary::AnalyzeTextureColorSource(Material, bUseUnlitEmissive);
			auto Error = FString{};
			if (AnalyzeSurfaceDependencies(Material, bUseUnlitEmissive, Source, Error) == false)
			{
				Errors.Add(FString::Printf(TEXT("Slot %d (%s): %s"), Slot, *GetNameSafe(Material), *Error));
			}
			else if (bBakeSourceEmission == true && ValidateSourceEmissionDependencies(Material, Error) == false)
			{
				Errors.Add(FString::Printf(TEXT("Slot %d (%s): %s"), Slot, *GetNameSafe(Material), *Error));
			}
			Sources.Add(MoveTemp(Source));
		}
		if (Errors.IsEmpty() == false)
		{
			OutError = FString::Join(Errors, TEXT("\n\n"));
			return false;
		}

		OutSources = MoveTemp(Sources);
		return true;
	}
}
