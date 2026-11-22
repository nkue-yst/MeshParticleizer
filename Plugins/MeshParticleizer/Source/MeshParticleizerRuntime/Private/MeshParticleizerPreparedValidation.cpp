#include "MeshParticleizerPreparedMesh.h"

#if WITH_EDITOR
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/TextureCollection.h"
#include "Engine/Font.h"
#include "Interfaces/IPluginManager.h"
#include "MaterialCachedData.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/DataValidation.h"
#include "Misc/SecureHash.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UnrealType.h"

namespace MeshParticleizerPreparationValidation
{
	constexpr int32 FingerprintVersion = 2;

	FString Hash(const FString& Value)
	{
		return FMD5::HashAnsiString(*Value);
	}

	// 値の変更も拾う。GUIDだけではEditor APIが通知を省略した場合を判定できない。
	FString EditableValues(const UObject* Object)
	{
		auto Values = FString{};
		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			const auto* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Edit) == false || Property->HasAnyPropertyFlags(CPF_Transient) == true)
			{
				continue;
			}
			auto Value = FString{};
			Property->ExportText_InContainer(0, Value, Object, nullptr, const_cast<UObject*>(Object), PPF_None);
			Values += Property->GetName() + TEXT("=") + Value + TEXT("\n");
		}
		return Values;
	}

	void AddParameterValues(const UMaterialInterface* Material, TMap<FString, FString>& Out)
	{
		for (const auto& Key : FMeshParticleizerMaterialParameterKey::Gather(Material))
		{
			auto Parameter = FMaterialParameterMetadata{};
			if (Material->GetParameterValue(Key.Type, Key.Info, Parameter) == false)
			{
				continue;
			}
			const auto& Value = Parameter.Value;
			auto Text = FString{};
			switch (Key.Type)
			{
			case EMaterialParameterType::Scalar:
				Text = BytesToHex(reinterpret_cast<const uint8*>(Value.Float), sizeof(float));
				break;
			case EMaterialParameterType::Vector:
				Text = BytesToHex(reinterpret_cast<const uint8*>(Value.Float), sizeof(float) * 4);
				break;
			case EMaterialParameterType::DoubleVector:
				Text = BytesToHex(reinterpret_cast<const uint8*>(Value.Double), sizeof(double) * 4);
				break;
			case EMaterialParameterType::StaticSwitch:
				Text = Value.Bool[0] ? TEXT("1") : TEXT("0");
				break;
			case EMaterialParameterType::StaticComponentMask:
				Text = BytesToHex(reinterpret_cast<const uint8*>(Value.Bool), sizeof(bool) * 4);
				break;
			case EMaterialParameterType::Texture:
				Text = GetPathNameSafe(Value.Texture);
				break;
			case EMaterialParameterType::TextureCollection:
				Text = GetPathNameSafe(Value.TextureCollection);
				break;
			case EMaterialParameterType::ParameterCollection:
				Text = GetPathNameSafe(Value.ParameterCollection);
				break;
			case EMaterialParameterType::Font:
				Text = GetPathNameSafe(Value.Font.Value) + FString::FromInt(Value.Font.Page);
				break;
			default:
				Text = GetPathNameSafe(Value.AsTextureObject());
				break;
			}
			Out.Add(TEXT("Parameter: ") + Material->GetPathName() + TEXT(" / ") + FString::FromInt(static_cast<int32>(Key.Type)) + TEXT(":") + Key.Info.ToString(), Hash(Text));
		}
	}

	bool Build(const UMeshParticleizerPreparedMesh& Prepared, TMap<FString, FString>& Out, FString& OutError)
	{
		Out.Reset();
		OutError.Reset();

		// 生成形式だけでなく、配布プラグインの版が変わった場合も再準備を要求する。
		const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("MeshParticleizer"));
		if (Plugin.IsValid() == false)
		{
			OutError = TEXT("Mesh Particleizer plugin descriptor is unavailable.");
			return false;
		}
		const auto& Descriptor = Plugin->GetDescriptor();
		Out.Add(TEXT("Plugin version: MeshParticleizer"), Hash(FString::Printf(TEXT("%d:%s"), Descriptor.Version, *Descriptor.VersionName)));

		auto* Static = Prepared.SourceMesh.LoadSynchronous();
		auto* Skeletal = Prepared.SkeletalMesh.LoadSynchronous();
		if ((Static == nullptr) == (Skeletal == nullptr))
		{
			OutError = TEXT("Exactly one available source mesh is required.");
			return false;
		}
		auto Geometry = FString{};
		if (Static != nullptr)
		{
			const auto* Render = Static->GetRenderData();
			if (Render == nullptr || Render->LODResources.IsValidIndex(Prepared.SamplingLOD) == false)
			{
				OutError = TEXT("The Static Mesh sampling LOD is unavailable.");
				return false;
			}
			Geometry = UMeshParticleizerPreparedMesh::MakeSourceFingerprint(Render->DerivedDataKey, Prepared.SamplingLOD);
			for (const auto& Slot : Static->GetStaticMaterials())
			{
				Geometry += TEXT("\n") + Slot.MaterialSlotName.ToString() + TEXT(":") + GetPathNameSafe(Slot.MaterialInterface);
			}
			Out.Add(TEXT("Mesh / LOD / UV: ") + Static->GetPathName(), Hash(Geometry));
		}
		else
		{
			const auto* Render = Skeletal->GetResourceForRendering();
			if (Render == nullptr || Render->LODRenderData.IsValidIndex(Prepared.SamplingLOD) == false)
			{
				OutError = TEXT("The Skeletal Mesh sampling LOD is unavailable.");
				return false;
			}
			Geometry = UMeshParticleizerPreparedMesh::MakeSourceFingerprint(Render->DerivedDataKey, Prepared.SamplingLOD);
			for (const auto& Slot : Skeletal->GetMaterials())
			{
				Geometry += TEXT("\n") + Slot.MaterialSlotName.ToString() + TEXT(":") + GetPathNameSafe(Slot.MaterialInterface);
			}
			Out.Add(TEXT("Mesh / LOD / UV: ") + Skeletal->GetPathName(), Hash(Geometry));
			auto* Skeleton = Skeletal->GetSkeleton();
			if (Skeleton == nullptr)
			{
				OutError = TEXT("Source skeleton is unavailable.");
				return false;
			}
			Out.Add(TEXT("Skeleton: ") + Skeleton->GetPathName(), Hash(Skeleton->GetGuid().ToString() + Skeleton->GetVirtualBoneGuid().ToString()));
		}

		auto Visited = TSet<const UObject*>{};
		auto AddTexture = [&Out, &Visited](const UTexture* Texture) {
			if (Texture == nullptr || Visited.Contains(Texture) == true)
			{
				return;
			}
			Visited.Add(Texture);
			Out.Add(TEXT("Texture: ") + Texture->GetPathName(), Hash(Texture->Source.GetId().ToString() + Texture->GetLightingGuid().ToString() + EditableValues(Texture)));
		};
		auto AddCollection = [&Out, &Visited, &AddTexture](UTextureCollection* Collection) {
			if (Collection == nullptr || Visited.Contains(Collection) == true)
			{
				return;
			}
			Visited.Add(Collection);
			Out.Add(TEXT("Texture collection: ") + Collection->GetPathName(), Hash(EditableValues(Collection)));
			for (const auto& Texture : Collection->Textures)
			{
				AddTexture(Texture);
			}
		};
		for (auto Slot = 0; Slot < Prepared.SourceMaterials.Num(); ++Slot)
		{
			auto* Material = Prepared.SourceMaterials[Slot].LoadSynchronous();
			if (Material == nullptr)
			{
				OutError = FString::Printf(TEXT("Source material slot %d is unavailable."), Slot);
				return false;
			}
			Out.Add(FString::Printf(TEXT("Material slot %d"), Slot), Hash(Material->GetPathName()));
			while (Material != nullptr && Visited.Contains(Material) == false)
			{
				Visited.Add(Material);
				auto* Instance = Cast<UMaterialInstance>(Material);
				auto Description = Material->GetLightingGuid().ToString();
				if (Instance != nullptr)
				{
					// 上書き配列の有無ではなく実効値を比較し、同じ値へ戻した編集を許す。
					Description += GetPathNameSafe(Instance->Parent);
					FMaterialInstanceBasePropertyOverrides::StaticStruct()->ExportText(Description,
						&Instance->BasePropertyOverrides, nullptr, Instance, PPF_None, nullptr);
				}
				else
				{
					Description += EditableValues(Material);
				}
				AddParameterValues(Material, Out);
				if (const auto* Base = Cast<UMaterial>(Material))
				{
					Description += Base->StateId.ToString();
				}
				Out.Add(TEXT("Material: ") + Material->GetPathName(), Hash(Description));

				// 親材・関数・集合・Textureの内容を別々に保存し、変更箇所を一覧へ返す。
				auto Functions = TArray<UMaterialFunctionInterface*>{};
				Material->GetDependentFunctions(Functions);
				for (const auto* Function : Functions)
				{
					if (Function != nullptr)
					{
						Out.Add(TEXT("Material function: ") + Function->GetPathName(), Hash(Function->StateId.ToString() + EditableValues(Function)));
					}
				}
				for (const auto& Entry : Material->GetCachedExpressionData().ParameterCollectionInfos)
				{
					const auto* Collection = Entry.ParameterCollection.Get();
					if (Collection != nullptr)
					{
						Out.Add(TEXT("Parameter collection: ") + Collection->GetPathName(), Hash(Collection->StateId.ToString() + EditableValues(Collection)));
					}
				}
				auto Textures = TSet<const UTexture*>{};
				Material->GetReferencedTexturesAndOverrides(Textures);
				for (const auto* Texture : Textures)
				{
					AddTexture(Texture);
				}
				for (const auto& Collection : Material->GetReferencedTextureCollections())
				{
					AddCollection(Collection);
				}
				if (Instance != nullptr)
				{
					for (const auto& Value : Instance->TextureCollectionParameterValues)
					{
						AddCollection(Value.ParameterValue);
					}
				}
				Material = Instance != nullptr ? Instance->Parent.Get() : nullptr;
			}
		}
		return true;
	}
}

bool UMeshParticleizerPreparedMesh::CapturePreparationDependencies(FString& OutError)
{
	auto Current = TMap<FString, FString>{};
	if (MeshParticleizerPreparationValidation::Build(*this, Current, OutError) == false)
	{
		return false;
	}
	this->PreparationDependencies = MoveTemp(Current);
	this->DependencyFingerprintVersion = MeshParticleizerPreparationValidation::FingerprintVersion;
	return true;
}

void UMeshParticleizerPreparedMesh::GetPreparationIssues(TArray<FString>& OutIssues) const
{
	OutIssues.Reset();
	if (this->DataVersion != CurrentDataVersion)
	{
		OutIssues.Add(TEXT("Generator version changed. Prepare this mesh again."));
	}
	if (this->DependencyFingerprintVersion != MeshParticleizerPreparationValidation::FingerprintVersion || this->PreparationDependencies.IsEmpty() == true)
	{
		OutIssues.Add(TEXT("Preparation has no current dependency fingerprints. Prepare into a new folder; refreshing the resource manifest cannot verify old baked data."));
		return;
	}

	auto Current = TMap<FString, FString>{};
	auto Error = FString{};
	if (MeshParticleizerPreparationValidation::Build(*this, Current, Error) == false)
	{
		OutIssues.Add(Error);
		return;
	}
	for (const auto& Entry : this->PreparationDependencies)
	{
		const auto* Value = Current.Find(Entry.Key);
		if (Value == nullptr || *Value != Entry.Value)
		{
			OutIssues.Add(TEXT("Changed or removed: ") + Entry.Key + TEXT(". Prepare this mesh again."));
		}
	}
	for (const auto& Entry : Current)
	{
		if (this->PreparationDependencies.Contains(Entry.Key) == false)
		{
			OutIssues.Add(TEXT("New dependency: ") + Entry.Key + TEXT(". Prepare this mesh again."));
		}
	}
	OutIssues.Sort();
}

bool UMeshParticleizerPreparedMesh::ValidatePreparation(FString& OutError) const
{
	auto Issues = TArray<FString>{};
	this->GetPreparationIssues(Issues);
	OutError = FString::Join(Issues, TEXT("\n"));
	return Issues.IsEmpty();
}

EDataValidationResult UMeshParticleizerPreparedMesh::IsDataValid(FDataValidationContext& Context) const
{
	const auto Base = Super::IsDataValid(Context);
	auto Issues = TArray<FString>{};
	this->GetPreparationIssues(Issues);
	auto Error = FString{};
	if (this->ValidateResourceManifest(Error) == false)
	{
		Issues.Add(Error);
	}
	for (const auto& Resource : this->Resources)
	{
		auto* Asset = Resource.Asset.LoadSynchronous();
		if (Asset == nullptr || Asset->IsAsset() == false || Asset->IsEditorOnly() == true)
		{
			Issues.Add(TEXT("Cook requires a saved runtime asset: ") + Resource.Asset.ToSoftObjectPath().ToString());
		}
	}
	for (const auto& Issue : Issues)
	{
		Context.AddError(FText::FromString(Issue));
	}
	return Issues.IsEmpty() == false || Base == EDataValidationResult::Invalid ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}

void UMeshParticleizerPreparedMesh::PreSave(FObjectPreSaveContext SaveContext)
{
	Super::PreSave(SaveContext);
	if (SaveContext.IsCooking() == true)
	{
		// 古い対応表をパッケージへ無言で渡さず、Cookの失敗理由に資産名を残す。
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
