#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceMeshParticleizerCollision.generated.h"

struct FMeshParticleizerDistanceField;

// GDF専用システムだけが使用する。平面衝突ではGDFの生成要求を発生させない。
UCLASS(EditInlineNew, Category = "Mesh Particleizer", meta = (DisplayName = "Mesh Particleizer Collision"))
class MESHPARTICLEIZERRUNTIME_API UNiagaraDataInterfaceMeshParticleizerCollision : public UNiagaraDataInterface
{
	GENERATED_BODY()

public:
	UNiagaraDataInterfaceMeshParticleizerCollision(const FObjectInitializer& Initializer);
	virtual void PostInitProperties() override;
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override
	{
		return Target == ENiagaraSimTarget::GPUComputeSim;
	}
	virtual bool RequiresGlobalDistanceField() const override
	{
		return true;
	}
	virtual bool Equals(const UNiagaraDataInterface* Other) const override;
	virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;
	virtual int32 PerInstanceDataSize() const override
	{
		return sizeof(uint32);
	}
	virtual bool InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance) override;
	virtual void DestroyPerInstanceData(void* Data, FNiagaraSystemInstance* Instance) override;
	virtual void BuildShaderParameters(FNiagaraShaderParametersBuilder& Builder) const override;
	virtual void SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const override;
	void Configure(const TSharedRef<FMeshParticleizerDistanceField, ESPMode::ThreadSafe>& State);

#if WITH_EDITORONLY_DATA
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const override;
	virtual void GetCommonHLSL(FString& OutHLSL) override;
	virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL) override;
	virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
		const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL) override;
	virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const override;
#endif

private:
	TSharedPtr<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> WorldState;
};
