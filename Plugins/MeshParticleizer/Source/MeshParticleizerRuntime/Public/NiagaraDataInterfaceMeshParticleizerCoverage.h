#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "ShaderParameterStruct.h"
#include "NiagaraDataInterfaceMeshParticleizerCoverage.generated.h"

class UMeshParticleizerPreparedMesh;
class UMaterialInstanceDynamic;
struct FMeshParticleizerGpuAllocation;

UCLASS(EditInlineNew, Category = "Mesh Particleizer", meta = (DisplayName = "Mesh Particleizer Surface Coverage"))
class MESHPARTICLEIZERRUNTIME_API UNiagaraDataInterfaceMeshParticleizerCoverage : public UNiagaraDataInterface
{
	GENERATED_BODY()

public:
	UNiagaraDataInterfaceMeshParticleizerCoverage(const FObjectInitializer& Initializer);
	virtual void PostInitProperties() override;
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override
	{
		return Target == ENiagaraSimTarget::GPUComputeSim;
	}
	virtual bool RequiresEarlyViewData() const override
	{
		return this->bReserved;
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

	bool Configure(UMeshParticleizerPreparedMesh* InPrepared, TConstArrayView<TObjectPtr<UMaterialInstanceDynamic>> InMaterials,
		bool bInReserved, const TSharedRef<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>& Allocation, FString& OutError);
	void SetTransform(const FTransform& InTransform);
	void SetBones(TConstArrayView<FVector4f> Rows);
	void StopEvaluating();

#if WITH_EDITOR
	static int64 GetProxyBufferBytes(FNiagaraDataInterfaceProxy* Proxy, FNiagaraSystemInstanceID Id,
		uint64* OutIdentity = nullptr, uint32* OutViewCount = nullptr, int64* OutIndexBytes = nullptr,
		uint64* OutHeadsIdentity = nullptr, uint64* OutLinksIdentity = nullptr);
	// 描画スレッドの同期診断だけで使用する。通常の再生経路からは読み戻さない。
	static void ReadProxyCoverage(FRHICommandListImmediate& RHICmdList, FNiagaraDataInterfaceProxy* Proxy,
		FNiagaraSystemInstanceID Id, TArray<float>& OutCoverage);
#endif

#if WITH_EDITORONLY_DATA
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const override;
	virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL) override;
	virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
		const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL) override;
	virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const override;
#endif

	BEGIN_SHADER_PARAMETER_STRUCT(FShaderParameters, )
		SHADER_PARAMETER(uint32, Enabled)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, Coverage)
	END_SHADER_PARAMETER_STRUCT()

private:
	void PushInputs();

	UPROPERTY(Transient)
	TObjectPtr<UMeshParticleizerPreparedMesh> Prepared;

	// 表示MIDを共有する。停止・破棄後はSimulationもGPU完了まで保持する。
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	bool bReserved = false;
	bool bEvaluate = true;
	FTransform Transform = FTransform::Identity;
	TArray<FVector4f> BoneRows;
	uint64 BoneRevision = 0;
	TWeakPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> SourceAllocation;
};
