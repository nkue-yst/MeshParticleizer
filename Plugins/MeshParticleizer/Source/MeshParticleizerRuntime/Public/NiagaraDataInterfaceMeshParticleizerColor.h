#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "ShaderParameterStruct.h"
#include "NiagaraDataInterfaceMeshParticleizerColor.generated.h"

class UTexture2D;
class UTexture;
struct FMeshParticleizerGpuAllocation;

USTRUCT()
struct FMeshParticleizerGpuColorBinding
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UTexture> Texture;

	UPROPERTY(Transient)
	FVector2D UVScale = FVector2D(1, 1);

	UPROPERTY(Transient)
	FLinearColor ColorScale = FLinearColor::White;

	UPROPERTY(Transient)
	FVector2D UVOffset = FVector2D::ZeroVector;

	uint64 Revision = 0;
};

// 更新時だけGPUでサンプル色を作る。分離した粒子はこの入力を読み直さない。
UCLASS(EditInlineNew, Category = "Mesh Particleizer", meta = (DisplayName = "Mesh Particleizer Surface Color"))
class MESHPARTICLEIZERRUNTIME_API UNiagaraDataInterfaceMeshParticleizerColor : public UNiagaraDataInterface
{
	GENERATED_BODY()

public:
	UNiagaraDataInterfaceMeshParticleizerColor(const FObjectInitializer& Initializer);
	virtual void PostInitProperties() override;
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override
	{
		return Target == ENiagaraSimTarget::GPUComputeSim;
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

	void Configure(UTexture2D* Positions, UTexture2D* Coordinates, bool bReserved,
		const TSharedRef<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>& Allocation, bool bEmission = false);
	void SetBindings(TConstArrayView<FMeshParticleizerGpuColorBinding> Bindings);

#if WITH_EDITOR
	bool IsEmission() const
	{
		return this->bEmission;
	}
	static int64 GetProxyBufferBytes(FNiagaraDataInterfaceProxy* Proxy, FNiagaraSystemInstanceID Id, uint64* OutIdentity = nullptr);
#endif

#if WITH_EDITORONLY_DATA
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const override;
	virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL) override;
	virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
		const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL) override;
	virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const override;
#endif

	BEGIN_SHADER_PARAMETER_STRUCT(FShaderParameters, )
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, Colors)
	END_SHADER_PARAMETER_STRUCT()

private:
	void PushInputs();

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Positions;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Coordinates;

	UPROPERTY(Transient)
	TArray<FMeshParticleizerGpuColorBinding> Bindings;

	bool bReserved = false;
	bool bEmission = false;
	TWeakPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> SourceAllocation;
};
