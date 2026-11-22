#include "MeshParticleizerMaterialInputs.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceDynamic.h"

void FMeshParticleizerMaterialInputs::SetScalar(UMaterialInstanceDynamic* Material, FName Name, float Value)
{
	Material->SetScalarParameterValue(Name, Value);
	this->Scalars.Add(Name, Value);
}

void FMeshParticleizerMaterialInputs::SetVector(UMaterialInstanceDynamic* Material, FName Name, const FLinearColor& Value)
{
	Material->SetVectorParameterValue(Name, Value);
	this->Vectors.Add(Name, Value);
}

void FMeshParticleizerMaterialInputs::SetTexture(UMaterialInstanceDynamic* Material, FName Name, UTexture* Value)
{
	Material->SetTextureParameterValue(Name, Value);
	this->Textures.Add(Name, Value);
}

bool FMeshParticleizerMaterialInputs::Validate(const UMaterialInstanceDynamic* Material, int32 Slot, FString& OutError) const
{
	auto Reject = [&OutError, Slot](FName Name) {
		OutError = FString::Printf(TEXT("Material slot %d managed input '%s' changed externally. Use Mesh Particleizer APIs."), Slot, *Name.ToString());
		return false;
	};
	if (Material == nullptr)
	{
		return Reject(NAME_None);
	}

	// Getterは継承値も解決するため、外部のClearParameterValuesによる上書き解除も検出する。
	for (const auto& Entry : this->Scalars)
	{
		auto Current = 0.f;
		if (Material->GetScalarParameterValue(Entry.Key, Current) == false || Current != Entry.Value)
		{
			return Reject(Entry.Key);
		}
	}
	for (const auto& Entry : this->Vectors)
	{
		auto Current = FLinearColor{};
		if (Material->GetVectorParameterValue(Entry.Key, Current) == false || Current != Entry.Value)
		{
			return Reject(Entry.Key);
		}
	}
	for (const auto& Entry : this->Textures)
	{
		UTexture* Current = nullptr;
		if (Entry.Value.IsStale() || Material->GetTextureParameterValue(Entry.Key, Current) == false || Current != Entry.Value.Get())
		{
			return Reject(Entry.Key);
		}
	}
	return true;
}
