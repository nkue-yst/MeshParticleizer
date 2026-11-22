#include "MeshParticleizerPreparationLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_Niagara.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionParticleColor.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "MaterialShared.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraConstants.h"
#include "NiagaraDataInterfaceTexture.h"
#include "NiagaraDataInterfaceMeshParticleizerBudget.h"
#include "NiagaraDataInterfaceMeshParticleizerColor.h"
#include "NiagaraDataInterfaceMeshParticleizerCoverage.h"
#include "NiagaraDataInterfaceMeshParticleizerCollision.h"
#include "NiagaraDataInterfaceArrayFloat.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "NiagaraScriptSource.h"
#include "NiagaraShared.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerFarSprite.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "StaticMeshCompiler.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace MeshParticleizerNiagara
{
	struct FBinding
	{
		FName Pin;
		FNiagaraVariable Variable;
	};

	TArray<FBinding> MeshInputs()
	{
		return {{TEXT("InMeshOrientation"), SYS_PARAM_PARTICLES_MESH_ORIENTATION},
			{TEXT("MeshSizeScale"), FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.MeshSizeScale"))},
			{TEXT("AlignMeshToSurface"), FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.AlignMeshToSurface"))},
			{TEXT("MeshAngularVelocity"), FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("User.MeshAngularVelocity"))}};
	}

	TArray<FBinding> MeshOutputs()
	{
		return {{TEXT("OutMeshScale"), SYS_PARAM_PARTICLES_SCALE}, {TEXT("OutMeshOrientation"), SYS_PARAM_PARTICLES_MESH_ORIENTATION}};
	}

	FString MeshShader(const FString& Code, bool bMesh)
	{
		return FString::Printf(TEXT("#define MP_MESH_PARTICLES %d\n"), bMesh ? 1 : 0) + Code + TEXT("\n#undef MP_MESH_PARTICLES\n");
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, FName Name = NAME_None)
	{
		for (auto* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && (Name.IsNone() || Pin->PinName == Name))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	template <typename T>
	T* AddNode(UNiagaraGraph* Graph)
	{
		auto* Node = NewObject<T>(Graph);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		return Node;
	}

	UEdGraphNode* AddMapNode(UNiagaraGraph* Graph, const TCHAR* ClassPath)
	{
		auto* Node = NewObject<UEdGraphNode>(Graph, FindObject<UClass>(nullptr, ClassPath));
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		return Node;
	}

	bool Save(UObject* Asset)
	{
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		auto Args = FSavePackageArgs{};
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const auto Filename = FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args);
	}

	void ConfigureSourceEmission(UMaterial* Material)
	{
		const auto Marker = FString(TEXT("Mesh Particleizer original emission"));
		for (const auto& Expression : Material->GetExpressions())
		{
			if (Expression->Desc == Marker)
			{
				return;
			}
		}

		// 元発光は表面色・Tint・追加発光強度から独立して加算する。
		auto* Source = Cast<UMaterialExpressionDynamicParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionDynamicParameter::StaticClass()));
		Source->ParameterIndex = 1;
		Source->Desc = Marker;
		auto* Sum = Cast<UMaterialExpressionAdd>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionAdd::StaticClass()));
		auto* Emission = Material->GetExpressionInputForProperty(MP_EmissiveColor);
		Sum->A = *Emission;
		Sum->B.Connect(4, Source);
		Emission->Connect(0, Sum);
	}

	UNiagaraScript* CreateModule(const FString& Path, const FString& Code,
		const TArray<FBinding>& Inputs, const TArray<FBinding>& Outputs, ENiagaraScriptUsage Usage)
	{
		auto* Script = NewObject<UNiagaraScript>(CreatePackage(*Path), *FPackageName::GetLongPackageAssetName(Path), RF_Public | RF_Standalone);
		Script->SetUsage(ENiagaraScriptUsage::Module);
		Script->CheckVersionDataAvailable();
		Script->GetLatestScriptData()->ModuleUsageBitmask = 1 << static_cast<int32>(Usage);
		auto* Source = NewObject<UNiagaraScriptSource>(Script);
		auto* Graph = NewObject<UNiagaraGraph>(Source);
		Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
		Source->NodeGraph = Graph;
		Script->SetLatestSource(Source);

		// Parameter Mapの入力、読取り、計算、書込みを明示的に接続する。
		auto* Input = AddNode<UNiagaraNodeInput>(Graph);
		Input->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		Input->Usage = ENiagaraInputNodeUsage::Parameter;
		static_cast<UEdGraphNode*>(Input)->AllocateDefaultPins();
		auto* Get = AddMapNode(Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
		auto* Set = AddMapNode(Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
		FindPin(Input, EGPD_Output)->MakeLinkTo(FindPin(Get, EGPD_Input));
		FindPin(Input, EGPD_Output)->MakeLinkTo(FindPin(Set, EGPD_Input));

		auto* Custom = AddNode<UNiagaraNodeCustomHlsl>(Graph);
		Custom->Signature.Name = *FPackageName::GetLongPackageAssetName(Path);
		for (const auto& Binding : Inputs)
		{
			Custom->Signature.Inputs.Emplace(Binding.Variable.GetType(), Binding.Pin);
		}
		for (const auto& Binding : Outputs)
		{
			Custom->Signature.Outputs.Emplace(Binding.Variable.GetType(), Binding.Pin);
		}
		// CustomHlslのsetterは別モジュールへ公開されていないため、保存プロパティを設定する。
		FindFProperty<FStrProperty>(Custom->GetClass(), TEXT("CustomHlsl"))->SetPropertyValue_InContainer(Custom, Code);
		auto* IncludeProperty = FindFProperty<FArrayProperty>(Custom->GetClass(), TEXT("VirtualIncludeFilePaths"));
		auto* Includes = IncludeProperty->ContainerPtrToValuePtr<TArray<FString>>(Custom);
		Includes->Add(TEXT("/Plugin/MeshParticleizer/Private/ParticleizerCommon.ush"));
		static_cast<UEdGraphNode*>(Custom)->AllocateDefaultPins();
		for (const auto& Binding : Inputs)
		{
			auto* Pin = Get->CreatePin(EGPD_Output, UEdGraphSchema_Niagara::TypeDefinitionToPinType(Binding.Variable.GetType()), Binding.Variable.GetName());
			Pin->MakeLinkTo(FindPin(Custom, EGPD_Input, Binding.Pin));
		}
		for (const auto& Binding : Outputs)
		{
			auto* Pin = Set->CreatePin(EGPD_Input, UEdGraphSchema_Niagara::TypeDefinitionToPinType(Binding.Variable.GetType()), Binding.Variable.GetName());
			FindPin(Custom, EGPD_Output, Binding.Pin)->MakeLinkTo(Pin);
		}

		auto* Output = AddNode<UNiagaraNodeOutput>(Graph);
		Output->SetUsage(ENiagaraScriptUsage::Module);
		Output->Outputs.Emplace(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("OutputMap"));
		static_cast<UEdGraphNode*>(Output)->AllocateDefaultPins();
		FindPin(Set, EGPD_Output)->MakeLinkTo(FindPin(Output, EGPD_Input));
		static_cast<UEdGraph*>(Graph)->NotifyGraphChanged();
		return Script;
	}

	bool ApplyCollisionCode(FString& Code, bool bGdf, FString& OutError)
	{
		if (bGdf == false)
		{
			return true;
		}
		auto CollisionCode = FString{};
		const auto Path = IPluginManager::Get().FindPlugin(TEXT("MeshParticleizer"))->GetBaseDir() / TEXT("Shaders/Private/ParticleizerGdfCollision.ush");
		const auto Begin = Code.Find(TEXT("        // MP_COLLISION_BEGIN"));
		const auto End = Code.Find(TEXT("        // MP_COLLISION_END"));
		if (Begin == INDEX_NONE || End <= Begin || FFileHelper::LoadFileToString(CollisionCode, *Path) == false)
		{
			OutError = TEXT("Generated GDF collision shader source is missing or incompatible.");
			return false;
		}
		Code = Code.Left(Begin) + CollisionCode + TEXT("\n") + Code.Mid(End);
		return true;
	}

	void ConfigureParticleMaterialBinding(UNiagaraSystem* System)
	{
		// 通常材を既定に残し、各Componentだけで診断表示へ切り替えられるようにする。
		const auto Parameter = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.ParticleMaterial"));
		System->GetExposedParameters().AddParameter(Parameter);
		const auto FarParameter = FNiagaraVariable(FNiagaraTypeDefinition::GetUMaterialDef(), TEXT("User.FarParticleMaterial"));
		const auto Version = FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.FarSpriteVersion"));
		const auto bFar = System->GetExposedParameters().IndexOf(Version) != INDEX_NONE &&
			System->GetExposedParameters().GetParameterValue<int32>(Version) == 1;
		if (bFar == true)
		{
			System->GetExposedParameters().AddParameter(FarParameter);
		}
		System->GetExposedParameters().SetParameterValue(1,
			FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.PreviewDataVersion")), true);
		for (const auto& Handle : System->GetEmitterHandles())
		{
			auto* Data = Handle.GetInstance().GetEmitterData();
			if (Data == nullptr)
			{
				continue;
			}
			for (auto* Renderer : Data->GetRenderers())
			{
				if (auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					Sprite->MaterialUserParamBinding.Parameter = bFar ? FarParameter : Parameter;
				}
				else if (auto* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					for (auto& Override : Mesh->OverrideMaterials)
					{
						Override.UserParamBinding.Parameter = Parameter;
					}
				}
			}
		}
	}

	bool ConfigurePoolLifecycle(UNiagaraSystem* System, const FString& Folder, FString& OutError)
	{
		const auto Int = FNiagaraTypeDefinition::GetIntDef();
		const auto Count = FNiagaraVariable(Int, TEXT("Emitter.InitialBurstCount"));
		const auto Path = Folder / TEXT("NM_InitialBurst");
		auto* InitialBurst = LoadObject<UNiagaraScript>(nullptr, *(Path + TEXT(".NM_InitialBurst")), nullptr, LOAD_NoWarn);
		if (InitialBurst == nullptr)
		{
			InitialBurst = CreateModule(Path, TEXT("OutCount = LoopCount == 0 ? Count : 0;"), {{TEXT("Count"), FNiagaraVariable(Int, TEXT("User.Count"))}, {TEXT("LoopCount"), FNiagaraVariable(Int, TEXT("Emitter.LoopCount"))}}, {{TEXT("OutCount"), Count}}, ENiagaraScriptUsage::EmitterUpdateScript);
			if (Save(InitialBurst) == false)
			{
				OutError = TEXT("Could not save the managed burst module.");
				return false;
			}
		}

		for (auto& Handle : System->GetEmitterHandles())
		{
			auto* Emitter = Handle.GetInstance().GetEmitterData();
			auto* Source = Emitter != nullptr ? Cast<UNiagaraScriptSource>(Emitter->GraphSource) : nullptr;
			auto* Graph = Source != nullptr ? Source->NodeGraph.Get() : nullptr;
			if (Graph == nullptr)
			{
				OutError = TEXT("A generated emitter graph is required for pooling.");
				return false;
			}
			auto Nodes = TArray<UNiagaraNodeFunctionCall*>{};
			Graph->GetNodesOfClass(Nodes);
			UNiagaraNodeFunctionCall* State = nullptr;
			UNiagaraNodeFunctionCall* Burst = nullptr;
			auto bHasInitialBurst = false;
			for (auto* Node : Nodes)
			{
				if (Node->FunctionScript == nullptr)
				{
					continue;
				}
				const auto ScriptName = Node->FunctionScript->GetName();
				if (ScriptName == TEXT("EmitterState"))
				{
					State = Node;
				}
				if (ScriptName == TEXT("SpawnBurst_Instantaneous"))
				{
					Burst = Node;
				}
				bHasInitialBurst |= Node->FunctionScript == InitialBurst;
			}
			auto* Loop = State != nullptr ? State->FindPin(TEXT("Loop Behavior"), EGPD_Input) : nullptr;
			auto* Enum = Loop != nullptr ? Cast<UEnum>(Loop->PinType.PinSubCategoryObject.Get()) : nullptr;
			if (Enum == nullptr || Burst == nullptr)
			{
				OutError = TEXT("Generated emitter lifecycle modules were changed.");
				return false;
			}

			// GPU容量の終了はOperationが管理する。ループしても最初の一回しか発生させない。
			Loop->DefaultValue = Enum->GetNameStringByValue(0); // Infinite
			State->PinDefaultValueChanged(Loop);
			if (bHasInitialBurst == false)
			{
				FNiagaraStackGraphUtilities::AddScriptModuleToStack(InitialBurst,
					*Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::EmitterUpdateScript), 1);
			}
			const auto HandleName = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				FNiagaraParameterHandle::CreateModuleParameterHandle(TEXT("Spawn Count")), Burst);
			auto& Override = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Burst, HandleName, Int, FGuid(), FGuid());
			if (Override.LinkedTo.Num() == 1 && Override.LinkedTo[0]->PinName == TEXT("User.Count") && Override.LinkedTo[0]->LinkedTo.Num() == 1)
			{
				// 生成した旧リンクだけを外す。同じ移行を繰り返しても孤立ノードを増やさない。
				auto* PreviousRead = Override.LinkedTo[0]->GetOwningNode();
				const auto* PreviousValue = Override.LinkedTo[0];
				if (PreviousRead->GetClass()->GetName() != TEXT("NiagaraNodeParameterMapGet") || PreviousRead->Pins.ContainsByPredicate([PreviousValue](const auto* Pin) {
						return Pin != PreviousValue && Pin->Direction == EGPD_Output && Pin->LinkedTo.IsEmpty() == false;
					}))
				{
					OutError = TEXT("The generated burst reader was edited. Generate a new system before using pooling.");
					return false;
				}
				PreviousRead->BreakAllNodeLinks();
				Graph->RemoveNode(PreviousRead);
			}
			if (Override.LinkedTo.IsEmpty() == true)
			{
				FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Override, Count, {});
			}
			else if (Override.LinkedTo.Num() != 1 || Override.LinkedTo[0]->PinName != Count.GetName())
			{
				OutError = TEXT("The generated burst input was edited. Generate a new system before using pooling.");
				return false;
			}
			static_cast<UEdGraph*>(Graph)->NotifyGraphChanged();
		}
		System->GetExposedParameters().SetParameterValue(1, FNiagaraVariable(Int, TEXT("User.ManagedLifetime")), true);
		return true;
	}
}

UNiagaraSystem* UMeshParticleizerPreparationLibrary::CreateParticleSystem(const FString& Destination, FString& OutError, bool bGlobalDistanceField, UStaticMesh* ParticleMesh)
{
	using namespace MeshParticleizerNiagara;
	OutError.Reset();
	if (Destination.StartsWith(TEXT("/MeshParticleizer/")) == false || FPackageName::IsValidLongPackageName(Destination) == false)
	{
		OutError = TEXT("Choose a new /MeshParticleizer/ content folder.");
		return nullptr;
	}
	const auto SystemPath = Destination / TEXT("NS_MeshParticleizer");
	if (FPackageName::DoesPackageExist(SystemPath) || FindObject<UPackage>(nullptr, *SystemPath) != nullptr)
	{
		OutError = FString::Printf(TEXT("System output already exists: %s"), *SystemPath);
		return nullptr;
	}
	const auto bMesh = ParticleMesh != nullptr;
	auto MeshBytes = int64{0};
	if (bMesh == true)
	{
		// 読込直後の非同期ビルドは生成時に完了させ、再生へ持ち込まない。
		FStaticMeshCompilingManager::Get().FinishCompilation({ParticleMesh});
		if (MeshParticleizerAssetGpuBytes(ParticleMesh, MeshBytes, OutError) == false)
		{
			return nullptr;
		}
		if (ParticleMesh->GetRenderData()->LODResources.Num() != 1 || ParticleMesh->GetRenderData()->LODResources[0].Sections.Num() != 1 ||
			FMath::IsFinite(ParticleMesh->GetBounds().SphereRadius) == false || ParticleMesh->GetBounds().SphereRadius <= UE_SMALL_NUMBER)
		{
			OutError = TEXT("Choose a particle mesh with one raster LOD/section and nonzero bounds.");
			return nullptr;
		}
	}
	const auto ShaderDirectory = IPluginManager::Get().FindPlugin(TEXT("MeshParticleizer"))->GetBaseDir() / TEXT("Shaders/Private");
	auto SpawnCode = FString{};
	auto UpdateCode = FString{};
	if (FFileHelper::LoadFileToString(SpawnCode, *(ShaderDirectory / TEXT("ParticleizerSpawn.ush"))) == false ||
		FFileHelper::LoadFileToString(UpdateCode, *(ShaderDirectory / TEXT("ParticleizerUpdate.ush"))) == false)
	{
		OutError = TEXT("Particleizer shader sources are missing.");
		return nullptr;
	}

	if (ApplyCollisionCode(UpdateCode, bGlobalDistanceField, OutError) == false)
	{
		return nullptr;
	}
	SpawnCode = MeshShader(SpawnCode, bMesh);
	UpdateCode = MeshShader(UpdateCode, bMesh);

	const auto Float = FNiagaraTypeDefinition::GetFloatDef();
	const auto Int = FNiagaraTypeDefinition::GetIntDef();
	const auto Vec3 = FNiagaraTypeDefinition::GetVec3Def();
	const auto Vec4 = FNiagaraTypeDefinition::GetVec4Def();
	const auto Texture = FNiagaraTypeDefinition(UNiagaraDataInterfaceTexture::StaticClass());
	const auto BoneArray = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat4::StaticClass());
	auto User = [](const TCHAR* Name, const FNiagaraTypeDefinition& Type) {
		return FBinding{FName(Name), FNiagaraVariable(Type, FName(FString(TEXT("User.")) + Name))};
	};
	auto SpawnInputs = TArray<FBinding>{User(TEXT("PositionData"), Texture), User(TEXT("BoundsMin"), Vec3), User(TEXT("BoundsMax"), Vec3), User(TEXT("Direction"), Vec3), User(TEXT("Origin"), Vec3), User(TEXT("Mode"), Int), User(TEXT("Seed"), Float), User(TEXT("ProgressCurve"), BoneArray), User(TEXT("CurveSegmentCount"), Int)};
	for (const auto* Name : {TEXT("Origin1"), TEXT("Origin2"), TEXT("Origin3")})
	{
		SpawnInputs.Add(User(Name, Vec3));
	}
	SpawnInputs.Add(User(TEXT("OriginCount"), Int));
	SpawnInputs.Add(User(TEXT("OriginDistance"), Float));
	for (const auto* Name : {TEXT("ProgressX"), TEXT("ProgressY"), TEXT("ProgressZ")})
	{
		SpawnInputs.Add(User(Name, Vec4));
	}
	auto Outputs = TArray<FBinding>{
		{TEXT("OutPosition"), SYS_PARAM_PARTICLES_POSITION}, {TEXT("OutVelocity"), SYS_PARAM_PARTICLES_VELOCITY}, {TEXT("OutColor"), SYS_PARAM_PARTICLES_COLOR}, {TEXT("OutSize"), SYS_PARAM_PARTICLES_SPRITE_SIZE}, {TEXT("OutAlive"), SYS_PARAM_INSTANCE_ALIVE}, {TEXT("OutDetached"), FNiagaraVariable(Float, TEXT("Particles.Detached"))}, {TEXT("OutRestTime"), FNiagaraVariable(Float, TEXT("Particles.RestTime"))}, {TEXT("OutSleeping"), FNiagaraVariable(Float, TEXT("Particles.Sleeping"))}, {TEXT("OutSourceColor"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM}, {TEXT("OutSourceEmission"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM_1}};
	if (bMesh == true)
	{
		Outputs.Append(MeshOutputs());
	}
	auto SpawnOutputs = Outputs;
	SpawnOutputs.Add({TEXT("OutOrder"), FNiagaraVariable(Float, TEXT("Particles.Order"))});
	SpawnOutputs.Add({TEXT("OutDetachFraction"), FNiagaraVariable(Float, TEXT("Particles.DetachFraction"))});
	SpawnOutputs.Add({TEXT("OutIndex"), FNiagaraVariable(Int, TEXT("Particles.SampleIndex"))});
	auto* Spawn = CreateModule(Destination / TEXT("NM_SurfaceSpawn"), SpawnCode, SpawnInputs, SpawnOutputs, ENiagaraScriptUsage::ParticleSpawnScript);
	auto UpdateInputs = TArray<FBinding>{User(TEXT("PositionData"), Texture), User(TEXT("NormalData"), Texture), User(TEXT("ColorData"), Texture)};
	for (const auto* Name : {TEXT("CoordinateData"), TEXT("SkinVertices"), TEXT("SkinTriangles"), TEXT("EmissionData")})
	{
		UpdateInputs.Add(User(Name, Texture));
	}
	UpdateInputs.Add(User(TEXT("Bones"), BoneArray));
	UpdateInputs.Add(User(TEXT("PreviousBones"), BoneArray));
	UpdateInputs.Add(User(TEXT("SurfaceTints"), BoneArray));
	UpdateInputs.Add(User(TEXT("SurfaceColor"), FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass())));
	UpdateInputs.Add(User(TEXT("SurfaceEmission"), FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass())));
	UpdateInputs.Add(User(TEXT("SurfaceCoverage"), FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCoverage::StaticClass())));
	UpdateInputs.Add(User(TEXT("Budget"), FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerBudget::StaticClass())));
	if (bGlobalDistanceField)
	{
		UpdateInputs.Add(User(TEXT("Collision"), FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCollision::StaticClass())));
	}
	for (const auto* Name : {TEXT("Elapsed"), TEXT("PreviousElapsed"), TEXT("Duration"), TEXT("Lifetime"), TEXT("SourceAvailable"), TEXT("Skinned"), TEXT("BoneHistoryValid"), TEXT("BoneDeltaSeconds"), TEXT("BonePreviousElapsed"), TEXT("HistoryValid"), TEXT("InheritVelocity"), TEXT("ParticleSize"), TEXT("GroundEnabled"), TEXT("GroundHeight"), TEXT("Emissive"), TEXT("InheritSourceEmission"), TEXT("HasBakedEmission"), TEXT("FadeDuration"), TEXT("PoseDeltaSeconds"), TEXT("PosePreviousElapsed"), TEXT("NormalSpeed"), TEXT("ScatterSpeed"), TEXT("RandomSpeed"), TEXT("NoiseAcceleration"), TEXT("Drag"), TEXT("Restitution"), TEXT("Friction"), TEXT("MaxInitialSpeed"), TEXT("CollisionRadiusScale"), TEXT("SleepSpeed"), TEXT("SleepDuration")})
	{
		UpdateInputs.Add(User(Name, Float));
	}
	for (const auto* Name : {TEXT("TransformX"), TEXT("TransformY"), TEXT("TransformZ"), TEXT("PreviousX"), TEXT("PreviousY"), TEXT("PreviousZ")})
	{
		UpdateInputs.Add(User(Name, Vec4));
	}
	UpdateInputs.Add(User(TEXT("Tint"), FNiagaraTypeDefinition::GetColorDef()));
	UpdateInputs.Add(User(TEXT("Motion"), Int));
	UpdateInputs.Add(User(TEXT("Disappear"), Int));
	for (const auto* Name : {TEXT("NormalX"), TEXT("NormalY"), TEXT("NormalZ"), TEXT("Gravity"), TEXT("Wind"), TEXT("FallbackVelocity"), TEXT("FallbackAngularVelocity"), TEXT("FallbackCenter")})
	{
		UpdateInputs.Add(User(Name, Vec3));
	}
	UpdateInputs.Append({{TEXT("InPosition"), SYS_PARAM_PARTICLES_POSITION}, {TEXT("InVelocity"), SYS_PARAM_PARTICLES_VELOCITY}, {TEXT("InColor"), SYS_PARAM_PARTICLES_COLOR}, {TEXT("InDetached"), FNiagaraVariable(Float, TEXT("Particles.Detached"))}, {TEXT("Order"), FNiagaraVariable(Float, TEXT("Particles.Order"))}, {TEXT("Index"), FNiagaraVariable(Int, TEXT("Particles.SampleIndex"))}, {TEXT("DetachFraction"), FNiagaraVariable(Float, TEXT("Particles.DetachFraction"))}, {TEXT("InSleeping"), FNiagaraVariable(Float, TEXT("Particles.Sleeping"))}, {TEXT("InRestTime"), FNiagaraVariable(Float, TEXT("Particles.RestTime"))}, {TEXT("InSourceColor"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM}, {TEXT("InSourceEmission"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM_1}});
	if (bMesh == true)
	{
		UpdateInputs.Append(MeshInputs());
	}
	auto* Update = CreateModule(Destination / TEXT("NM_SurfaceUpdate"), UpdateCode, UpdateInputs, Outputs, ENiagaraScriptUsage::ParticleUpdateScript);

	// 通常の照明を受ける粒子材。元色と追加発光を独立して扱う。
	const auto MaterialPath = Destination / TEXT("M_SurfaceParticle");
	auto* Material = NewObject<UMaterial>(CreatePackage(*MaterialPath), TEXT("M_SurfaceParticle"), RF_Public | RF_Standalone);
	Material->SetShadingModel(MSM_DefaultLit);
	Material->BlendMode = BLEND_Masked;
	Material->bCanMaskedBeAssumedOpaque = false;
	Material->SetUsageByFlag(bMesh ? MATUSAGE_NiagaraMeshParticles : MATUSAGE_NiagaraSprites, true);
	Material->TwoSided = true;
	auto* Color = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionParticleColor::StaticClass());
	Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Color);
	auto* SourceColor = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionDynamicParameter::StaticClass());
	auto* Emission = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
	Emission->A.Connect(4, SourceColor);
	Emission->B.Connect(3, SourceColor);
	Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Emission);
	ConfigureSourceEmission(Material);

	// 円形の被覆とTemporal AAのディザで、半透明の積層負荷を避ける。
	auto* UV = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass());
	auto* Circle = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass()));
	Circle->OutputType = CMOT_Float1;
	Circle->Inputs.Reset();
	auto& UVInput = Circle->Inputs.AddDefaulted_GetRef();
	UVInput.InputName = TEXT("UV");
	UVInput.Input.Connect(0, UV);
	Circle->Code = TEXT("return dot(UV - 0.5, UV - 0.5) < 0.24 ? 1 : 0;");
	auto* Coverage = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass()));
	Coverage->A.Connect(0, Circle);
	Coverage->B.Connect(4, Color);
	auto* Dither = Cast<UMaterialExpressionMaterialFunctionCall>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass()));
	Dither->SetMaterialFunction(LoadObject<UMaterialFunction>(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA.DitherTemporalAA")));
	if (bMesh == true)
	{
		Dither->FunctionInputs[0].Input.Connect(4, Color);
	}
	else
	{
		Dither->FunctionInputs[0].Input.Connect(0, Coverage);
	}
	Material->GetExpressionInputForProperty(MP_OpacityMask)->Connect(0, Dither);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	if (auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		Resource->FinishCompilation();
		if (Resource->GetCompileErrors().IsEmpty() == false)
		{
			OutError = FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
			return nullptr;
		}
	}

	// 同じSimulationへ遠距離Spriteを追加する。GPU sort/cull配列は使わない。
	constexpr float FarDistance = 1000.f;
	auto* FarMaterial = static_cast<UMaterial*>(nullptr);
	if (bMesh == true)
	{
		const auto FarPath = Destination / TEXT("M_FarSurfaceParticle");
		FarMaterial = DuplicateObject<UMaterial>(Material, CreatePackage(*FarPath), TEXT("M_FarSurfaceParticle"));
		FarMaterial->SetFlags(RF_Public | RF_Standalone);
		FarMaterial->SetUsageByFlag(MATUSAGE_NiagaraSprites, true);
		MeshParticleizerConfigureFarSpriteMaterial(FarMaterial, FarDistance, true);
		UMaterialEditingLibrary::RecompileMaterial(FarMaterial);
		if (auto* Resource = FarMaterial->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			Resource->FinishCompilation();
			if (Resource->GetCompileErrors().IsEmpty() == false)
			{
				OutError = FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
				return nullptr;
			}
		}
	}

	auto* System = NewObject<UNiagaraSystem>(CreatePackage(*SystemPath), TEXT("NS_MeshParticleizer"), RF_Public | RF_Standalone);
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);
	auto& Parameters = System->GetExposedParameters();
	auto AllInputs = SpawnInputs;
	AllInputs.Append(UpdateInputs);
	for (const auto& Binding : AllInputs)
	{
		if (Binding.Variable.GetName().ToString().StartsWith(TEXT("User.")))
		{
			Parameters.AddParameter(Binding.Variable);
		}
	}
	if (bMesh == true)
	{
		Parameters.SetParameterValue(1, FNiagaraVariable(Int, TEXT("User.MeshRendererVersion")), true);
		Parameters.SetParameterValue(1, FNiagaraVariable(Int, TEXT("User.FarSpriteVersion")), true);
		Parameters.SetParameterValue(static_cast<float>(0.5 / ParticleMesh->GetBounds().SphereRadius), FNiagaraVariable(Float, TEXT("User.MeshSizeScale")));
		Parameters.SetParameterValue(1.f, FNiagaraVariable(Float, TEXT("User.AlignMeshToSurface")));
	}
	Parameters.SetParameterValue(100000, FNiagaraVariable(Int, TEXT("User.Count")), true);
	Parameters.SetParameterValue(1, FNiagaraVariable(Int, TEXT("User.DriftGravityVersion")), true);
	Parameters.SetParameterValue(1.5f, FNiagaraVariable(Float, TEXT("User.Duration")));
	Parameters.SetParameterValue(3.0f, FNiagaraVariable(Float, TEXT("User.Lifetime")));
	Parameters.SetParameterValue(0.7f, FNiagaraVariable(Float, TEXT("User.ParticleSize")));
	Parameters.SetParameterValue(0.15f, FNiagaraVariable(Float, TEXT("User.Restitution")));
	Parameters.SetParameterValue(0.6f, FNiagaraVariable(Float, TEXT("User.Friction")));
	Parameters.SetParameterValue(0.5f, FNiagaraVariable(Float, TEXT("User.CollisionRadiusScale")));
	Parameters.SetParameterValue(5.0f, FNiagaraVariable(Float, TEXT("User.SleepSpeed")));
	Parameters.SetParameterValue(0.15f, FNiagaraVariable(Float, TEXT("User.SleepDuration")));
	Parameters.SetParameterValue(FLinearColor::White, FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), TEXT("User.Tint")));
	Parameters.AddParameter(FNiagaraVariable(Float, TEXT("User.TotalDuration")));
	Parameters.SetParameterValue(4.5f, FNiagaraVariable(Float, TEXT("User.TotalDuration")));
	Parameters.SetParameterValue(FVector4f(1, 0, 0, 0), FNiagaraVariable(Vec4, TEXT("User.ProgressX")));
	Parameters.SetParameterValue(FVector4f(0, 1, 0, 0), FNiagaraVariable(Vec4, TEXT("User.ProgressY")));
	Parameters.SetParameterValue(FVector4f(0, 0, 1, 0), FNiagaraVariable(Vec4, TEXT("User.ProgressZ")));

	auto* Emitter = NewObject<UNiagaraEmitter>(GetTransientPackage(), TEXT("Surface"));
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	Emitter->SetUniqueEmitterName(TEXT("Surface"));
	auto* Data = Emitter->GetLatestEmitterData();
	Data->SimTarget = ENiagaraSimTarget::GPUComputeSim;
	Data->bLocalSpace = false;
	Data->bDeterminism = true;

	// 過去の別再生から容量を継承せず、今回の単発Spawn数で予約・確保する。
	Data->AllocationMode = EParticleAllocationMode::ManualEstimate;
	Data->PreAllocationCount = 0;
	Data->InterpolatedSpawnMode = ENiagaraInterpolatedSpawnMode::NoInterpolation;
	Data->SpawnScriptProps.Script->SetUsage(ENiagaraScriptUsage::ParticleSpawnScript);
	Data->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	Data->FixedBounds = FBox(FVector(-10000), FVector(10000));
	if (bMesh == true)
	{
		auto* Renderer = NewObject<UNiagaraMeshRendererProperties>(Emitter);
		Renderer->Meshes.SetNum(1);
		auto& Entry = Renderer->Meshes[0];
		Entry.Mesh = ParticleMesh;
		Entry.PivotOffset = -ParticleMesh->GetBounds().Origin;
		Entry.LODMode = ENiagaraMeshLODMode::LODLevel;
		Entry.LODLevel = 0;
		Renderer->bOverrideMaterials = true;
		Renderer->OverrideMaterials.AddDefaulted_GetRef().ExplicitMat = Material;
		Renderer->SortMode = ENiagaraSortMode::None;
		Renderer->bEnableFrustumCulling = false;
		Renderer->bEnableCameraDistanceCulling = true;
		Renderer->MinCameraDistance = 0.f;
		Renderer->MaxCameraDistance = FarDistance;
		Renderer->bCastShadows = false;
		Emitter->AddRenderer(Renderer, Data->Version.VersionGuid);

		auto* FarRenderer = NewObject<UNiagaraSpriteRendererProperties>(Emitter);
		FarRenderer->Material = FarMaterial;
		FarRenderer->SortMode = ENiagaraSortMode::None;
		FarRenderer->bEnableCameraDistanceCulling = false;
		FarRenderer->bCastShadows = false;
		Emitter->AddRenderer(FarRenderer, Data->Version.VersionGuid);
	}
	else
	{
		auto* Renderer = NewObject<UNiagaraSpriteRendererProperties>(Emitter);
		Renderer->Material = Material;
		Emitter->AddRenderer(Renderer, Data->Version.VersionGuid);
	}
	auto* Graph = Cast<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph.Get();
	auto* EmitterUpdate = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::EmitterUpdateScript);
	auto AddModule = [](const TCHAR* Path, UNiagaraNodeOutput* Output) {
		return FNiagaraStackGraphUtilities::AddScriptModuleToStack(LoadObject<UNiagaraScript>(nullptr, Path), *Output);
	};
	auto* State = AddModule(TEXT("/Niagara/Modules/Emitter/EmitterState.EmitterState"), EmitterUpdate);

	// 演出全体で一度だけ発生する。既定の無限ループでは候補が5秒ごとに重複する。
	for (const auto* SwitchName : {TEXT("Life Cycle Mode"), TEXT("Loop Behavior")})
	{
		auto* Pin = State->FindPin(SwitchName, EGPD_Input);
		auto* Enum = Pin != nullptr ? Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()) : nullptr;
		if (Enum == nullptr)
		{
			OutError = FString::Printf(TEXT("Emitter State is missing required switch %s."), SwitchName);
			return nullptr;
		}
		Pin->DefaultValue = Enum->GetNameStringByValue(1); // Self / Once
		State->PinDefaultValueChanged(Pin);
	}
	auto DurationHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle::CreateModuleParameterHandle(TEXT("Loop Duration")), State);
	auto& DurationOverride = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*State, DurationHandle, Float, FGuid(), FGuid());
	FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(DurationOverride, FNiagaraVariable(Float, TEXT("User.TotalDuration")), {});

	auto* Burst = AddModule(TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), EmitterUpdate);
	auto Handle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(FNiagaraParameterHandle::CreateModuleParameterHandle(TEXT("Spawn Count")), Burst);
	auto& Override = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*Burst, Handle, Int, FGuid(), FGuid());
	FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(Override, FNiagaraVariable(Int, TEXT("User.Count")), {});
	FNiagaraStackGraphUtilities::AddScriptModuleToStack(Spawn, *Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSpawnScript));
	FNiagaraStackGraphUtilities::AddScriptModuleToStack(Update, *Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleUpdateScript));

	// 標準の追加経路で複製し、SystemのSpawn/UpdateグラフへEmitterを接続する。
	FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Emitter, Data->Version.VersionGuid);
	System->SetFixedBounds(Data->FixedBounds);
	ConfigureParticleMaterialBinding(System);
	if (ConfigurePoolLifecycle(System, Destination, OutError) == false)
	{
		return nullptr;
	}
	System->RequestCompile(false);
	System->WaitForCompilationComplete(true, false);
	if (System->IsReadyToRun() == false || System->IsValid() == false)
	{
		OutError = TEXT("GPU system compilation failed. Inspect Niagara compiler diagnostics.");
		return nullptr;
	}
	if (FarMaterial != nullptr && Save(FarMaterial) == false)
	{
		OutError = TEXT("Failed to save the far Sprite material.");
		return nullptr;
	}
	for (auto* Asset : TArray<UObject*>{Spawn, Update, Material, System})
	{
		if (Save(Asset) == false)
		{
			OutError = FString::Printf(TEXT("Failed to save %s."), *Asset->GetPathName());
			return nullptr;
		}
	}
	return System;
}

bool UMeshParticleizerPreparationLibrary::UpgradeParticleSystem(UNiagaraSystem* System, FString& OutError)
{
	using namespace MeshParticleizerNiagara;
	OutError.Reset();
	if (System == nullptr || System->GetOutermost()->GetName().StartsWith(TEXT("/MeshParticleizer/")) == false)
	{
		OutError = TEXT("Select a generated Mesh Particleizer system in the plugin content.");
		return false;
	}
	const auto Folder = FPackageName::GetLongPackagePath(System->GetOutermost()->GetName());
	auto* Spawn = LoadObject<UNiagaraScript>(nullptr, *(Folder / TEXT("NM_SurfaceSpawn.NM_SurfaceSpawn")));
	auto* Update = LoadObject<UNiagaraScript>(nullptr, *(Folder / TEXT("NM_SurfaceUpdate.NM_SurfaceUpdate")));
	const auto bGdf = System->GetExposedParameters().IndexOf(FNiagaraVariable(
																 FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCollision::StaticClass()), TEXT("User.Collision"))) != INDEX_NONE;
	const auto bMesh = System->GetExposedParameters().IndexOf(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.MeshRendererVersion"))) != INDEX_NONE;
	auto UpgradeModule = [&OutError, bGdf, bMesh](UNiagaraScript* Script, const TCHAR* ShaderName,
							 const TArray<FBinding>& Inputs, const TArray<FBinding>& Outputs) {
		auto* Source = Script != nullptr ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
		auto* Graph = Source != nullptr ? Source->NodeGraph.Get() : nullptr;
		auto Nodes = TArray<UNiagaraNodeCustomHlsl*>{};
		if (Graph != nullptr)
		{
			Graph->GetNodesOfClass(Nodes);
		}
		auto* Custom = Nodes.Num() == 1 ? Nodes[0] : nullptr;
		auto* ReadPin = Custom != nullptr ? FindPin(Custom, EGPD_Input, TEXT("PositionData")) : nullptr;
		auto* WritePin = Custom != nullptr ? FindPin(Custom, EGPD_Output, TEXT("OutPosition")) : nullptr;
		if (ReadPin == nullptr || ReadPin->LinkedTo.Num() != 1 || WritePin == nullptr || WritePin->LinkedTo.Num() != 1)
		{
			OutError = TEXT("A generated module was replaced or edited. Use a newly generated system.");
			return false;
		}
		auto Code = FString{};
		const auto ShaderPath = IPluginManager::Get().FindPlugin(TEXT("MeshParticleizer"))->GetBaseDir() / TEXT("Shaders/Private") / ShaderName;
		if (FFileHelper::LoadFileToString(Code, *ShaderPath) == false)
		{
			OutError = TEXT("Particleizer shader source is missing.");
			return false;
		}

		if (FCString::Strcmp(ShaderName, TEXT("ParticleizerUpdate.ush")) == 0 && ApplyCollisionCode(Code, bGdf, OutError) == false)
		{
			return false;
		}

		Code = MeshShader(Code, bMesh);
		auto* Get = ReadPin->LinkedTo[0]->GetOwningNode();
		auto* Set = WritePin->LinkedTo[0]->GetOwningNode();
		Script->Modify();
		for (const auto& Binding : Inputs)
		{
			if (FindPin(Custom, EGPD_Input, Binding.Pin) == nullptr)
			{
				Custom->Signature.Inputs.Emplace(Binding.Variable.GetType(), Binding.Pin);
			}
		}
		for (const auto& Binding : Outputs)
		{
			if (FindPin(Custom, EGPD_Output, Binding.Pin) == nullptr)
			{
				Custom->Signature.Outputs.Emplace(Binding.Variable.GetType(), Binding.Pin);
			}
		}
		// UE標準の同期で既存の接続を保ち、入力順と末尾の追加用ピンを揃える。
		static_cast<UNiagaraNode*>(Custom)->RefreshFromExternalChanges();
		for (const auto& Binding : Inputs)
		{
			auto* Read = FindPin(Get, EGPD_Output, Binding.Variable.GetName());
			if (Read == nullptr)
			{
				Read = Get->CreatePin(EGPD_Output, UEdGraphSchema_Niagara::TypeDefinitionToPinType(Binding.Variable.GetType()), Binding.Variable.GetName());
			}
			auto* Input = FindPin(Custom, EGPD_Input, Binding.Pin);
			if (Input->LinkedTo.Contains(Read) == false)
			{
				Read->MakeLinkTo(Input);
			}
		}
		for (const auto& Binding : Outputs)
		{
			auto* Write = FindPin(Set, EGPD_Input, Binding.Variable.GetName());
			if (Write == nullptr)
			{
				Write = Set->CreatePin(EGPD_Input, UEdGraphSchema_Niagara::TypeDefinitionToPinType(Binding.Variable.GetType()), Binding.Variable.GetName());
			}
			auto* Output = FindPin(Custom, EGPD_Output, Binding.Pin);
			if (Output->LinkedTo.Contains(Write) == false)
			{
				Output->MakeLinkTo(Write);
			}
		}
		FindFProperty<FStrProperty>(Custom->GetClass(), TEXT("CustomHlsl"))->SetPropertyValue_InContainer(Custom, Code);
		static_cast<UEdGraph*>(Graph)->NotifyGraphChanged();
		Script->MarkScriptAndSourceDesynchronized(TEXT("Mesh Particleizer generated playback update"), Script->GetExposedVersion().VersionGuid);
		return true;
	};

	System->Modify();
	for (auto& Handle : System->GetEmitterHandles())
	{
		if (auto* Emitter = Handle.GetInstance().GetEmitterData())
		{
			Emitter->AllocationMode = EParticleAllocationMode::ManualEstimate;
			Emitter->PreAllocationCount = 0;
		}
	}
	const auto Vec4 = FNiagaraTypeDefinition::GetVec4Def();
	const auto Float = FNiagaraTypeDefinition::GetFloatDef();
	const auto Int = FNiagaraTypeDefinition::GetIntDef();
	const auto CurveArray = FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat4::StaticClass());
	auto Inputs = TArray<FBinding>{
		{TEXT("ProgressCurve"), FNiagaraVariable(CurveArray, TEXT("User.ProgressCurve"))},
		{TEXT("CurveSegmentCount"), FNiagaraVariable(Int, TEXT("User.CurveSegmentCount"))}};
	for (const auto* Name : {TEXT("Origin1"), TEXT("Origin2"), TEXT("Origin3")})
	{
		Inputs.Add({FName(Name), FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), FName(FString(TEXT("User.")) + Name))});
	}
	Inputs.Add({TEXT("OriginCount"), FNiagaraVariable(Int, TEXT("User.OriginCount"))});
	Inputs.Add({TEXT("OriginDistance"), FNiagaraVariable(Float, TEXT("User.OriginDistance"))});
	const FName Names[] = {TEXT("ProgressX"), TEXT("ProgressY"), TEXT("ProgressZ")};
	for (const auto Name : Names)
	{
		Inputs.Add({Name, FNiagaraVariable(Vec4, FName(FString(TEXT("User.")) + Name.ToString()))});
	}
	const auto DetachVariable = FNiagaraVariable(Float, TEXT("Particles.DetachFraction"));
	const auto SleepVariable = FNiagaraVariable(Float, TEXT("Particles.Sleeping"));
	auto UpdateInputs = TArray<FBinding>{{TEXT("DetachFraction"), DetachVariable}, {TEXT("InSleeping"), SleepVariable}};
	UpdateInputs.Add({TEXT("InSourceEmission"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM_1});
	UpdateInputs.Add({TEXT("EmissionData"), FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceTexture::StaticClass()), TEXT("User.EmissionData"))});
	UpdateInputs.Add({TEXT("InheritSourceEmission"), FNiagaraVariable(Float, TEXT("User.InheritSourceEmission"))});
	UpdateInputs.Add({TEXT("HasBakedEmission"), FNiagaraVariable(Float, TEXT("User.HasBakedEmission"))});
	UpdateInputs.Add({TEXT("SurfaceEmission"), FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceEmission"))});
	UpdateInputs.Add({TEXT("SurfaceTints"), FNiagaraVariable(CurveArray, TEXT("User.SurfaceTints"))});
	UpdateInputs.Add({TEXT("SurfaceColor"), FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerColor::StaticClass()), TEXT("User.SurfaceColor"))});
	UpdateInputs.Add({TEXT("SurfaceCoverage"), FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerCoverage::StaticClass()), TEXT("User.SurfaceCoverage"))});
	UpdateInputs.Add({TEXT("Budget"), FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceMeshParticleizerBudget::StaticClass()), TEXT("User.Budget"))});
	for (const auto* Name : {TEXT("CollisionRadiusScale"), TEXT("SleepSpeed"), TEXT("SleepDuration")})
	{
		UpdateInputs.Add({FName(Name), FNiagaraVariable(Float, FName(FString(TEXT("User.")) + Name))});
	}
	auto SpawnOutputs = TArray<FBinding>{{TEXT("OutDetachFraction"), DetachVariable}, {TEXT("OutSleeping"), SleepVariable}, {TEXT("OutSourceEmission"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM_1}};
	auto UpdateOutputs = TArray<FBinding>{{TEXT("OutSleeping"), SleepVariable}, {TEXT("OutSourceEmission"), SYS_PARAM_PARTICLES_DYNAMIC_MATERIAL_PARAM_1}};
	if (bMesh == true)
	{
		UpdateInputs.Append(MeshInputs());
		SpawnOutputs.Append(MeshOutputs());
		UpdateOutputs.Append(MeshOutputs());
	}
	if (UpgradeModule(Spawn, TEXT("ParticleizerSpawn.ush"), Inputs, SpawnOutputs) == false ||
			UpgradeModule(Update, TEXT("ParticleizerUpdate.ush"), UpdateInputs, UpdateOutputs) == false)
	{
		return false;
	}
	for (const auto& Binding : Inputs)
	{
		System->GetExposedParameters().AddParameter(Binding.Variable);
	}
	for (const auto& Binding : UpdateInputs)
	{
		if (Binding.Variable.GetName().ToString().StartsWith(TEXT("User.")))
		{
			System->GetExposedParameters().AddParameter(Binding.Variable);
		}
	}
	auto& Parameters = System->GetExposedParameters();
	Parameters.SetParameterValue(1, FNiagaraVariable(Int, TEXT("User.DriftGravityVersion")), true);
	Parameters.SetParameterValue(0.5f, FNiagaraVariable(Float, TEXT("User.CollisionRadiusScale")));
	Parameters.SetParameterValue(5.0f, FNiagaraVariable(Float, TEXT("User.SleepSpeed")));
	Parameters.SetParameterValue(0.15f, FNiagaraVariable(Float, TEXT("User.SleepDuration")));
	for (auto Axis = 0; Axis < 3; ++Axis)
	{
		const auto UserName = FName(FString(TEXT("User.")) + Names[Axis].ToString());
		System->GetExposedParameters().SetParameterValue(
			FVector4f(Axis == 0 ? 1 : 0, Axis == 1 ? 1 : 0, Axis == 2 ? 1 : 0, 0), FNiagaraVariable(Vec4, UserName), true);
	}
	ConfigureParticleMaterialBinding(System);
	if (ConfigurePoolLifecycle(System, Folder, OutError) == false)
	{
		return false;
	}

	auto* Material = LoadObject<UMaterial>(nullptr, *(Folder / TEXT("M_SurfaceParticle.M_SurfaceParticle")));
	if (Material == nullptr)
	{
		OutError = TEXT("Generated particle material is missing.");
		return false;
	}
	ConfigureSourceEmission(Material);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	if (auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		Resource->FinishCompilation();
		if (Resource->GetCompileErrors().IsEmpty() == false)
		{
			OutError = FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
			return false;
		}
	}
	System->RequestCompile(true);
	System->WaitForCompilationComplete(true, false);
	if (System->IsReadyToRun() == false || System->IsValid() == false)
	{
		OutError = TEXT("Upgraded GPU system compilation failed. Inspect Niagara compiler diagnostics.");
		return false;
	}
	if (Save(Spawn) == false || Save(Update) == false || Save(Material) == false || Save(System) == false)
	{
		OutError = TEXT("Could not save the upgraded GPU system.");
		return false;
	}
	return true;
}

FString UMeshParticleizerPreparationLibrary::InspectParticleSystem(UNiagaraSystem* System)
{
	if (System == nullptr)
	{
		return TEXT("System is unavailable.");
	}

	auto Result = FString::Printf(TEXT("System ready=%d valid=%d\n"), System->IsReadyToRun(), System->IsValid());
	for (const auto& Handle : System->GetEmitterHandles())
	{
		auto* Data = Handle.GetInstance().GetEmitterData();
		if (Data == nullptr || Data->GetGPUComputeScript() == nullptr)
		{
			continue;
		}

		auto* Script = Data->GetGPUComputeScript();
		Result += FString::Printf(TEXT("%s enabled=%d valid=%d pending=%d GPU compiled=%d\n"),
			*Handle.GetName().ToString(), Handle.GetIsEnabled(), Data->IsValid(),
			Script->IsScriptCompilationPending(true), Script->DidScriptCompilationSucceed(true));
		if (const auto* Resource = Script->GetRenderThreadScript())
		{
			for (const auto& Error : Resource->GetCompileErrors())
			{
				Result += Error + TEXT("\n");
			}
		}

		const auto Filename = FPaths::ProjectSavedDir() / TEXT("MeshParticleizer") / Handle.GetName().ToString() + TEXT(".hlsl");
		if (FFileHelper::SaveStringToFile(Script->GetVMExecutableData().LastHlslTranslationGPU, *Filename))
		{
			Result += TEXT("HLSL: ") + Filename + TEXT("\n");
		}
	}
	return Result;
}
