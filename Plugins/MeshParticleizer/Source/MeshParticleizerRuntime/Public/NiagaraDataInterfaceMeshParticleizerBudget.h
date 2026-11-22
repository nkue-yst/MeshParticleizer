#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "ShaderParameterStruct.h"
#include "NiagaraDataInterfaceMeshParticleizerBudget.generated.h"

struct FMeshParticleizerGpuState;
struct FMeshParticleizerGpuAllocation;

// 生成済みの固定候補を追跡する。通常のNiagara編集では容量を直接設定しない。
UCLASS(EditInlineNew, Category = "Mesh Particleizer", meta = (DisplayName = "Mesh Particleizer Budget"))
class MESHPARTICLEIZERRUNTIME_API UNiagaraDataInterfaceMeshParticleizerBudget : public UNiagaraDataInterface
{
	GENERATED_BODY()

public:
	UNiagaraDataInterfaceMeshParticleizerBudget(const FObjectInitializer& ObjectInitializer);
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

	void Configure(int32 Count, int32 Priority, double StartTime,
		const TSharedRef<FMeshParticleizerGpuState, ESPMode::ThreadSafe>& State,
		const TSharedRef<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>& Allocation);

#if WITH_EDITOR
	// Editor診断だけが使う。登録・解除をRender Threadへ順序付け、通常再生の入力は変更しない。
	using FDispatchObserver = TFunction<void(const FNiagaraDataInterfaceSetShaderParametersContext&, const uint8*)>;
	static void SetEditorDispatchObserver(FDispatchObserver Observer);

	// 同期するEditor検証専用。通常再生で粒子バッファを読み戻さない。
	static int64 GetProxyBufferBytes(FNiagaraDataInterfaceProxy* RenderProxy, FNiagaraSystemInstanceID InstanceId, uint64* OutIdentity = nullptr);
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
		SHADER_PARAMETER(uint32, Priority)
		SHADER_PARAMETER(float, StartTime)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, Records)
	END_SHADER_PARAMETER_STRUCT()

private:
	int32 CandidateCount = 0;
	int32 SourcePriority = 1;
	float WorldStartTime = 0;
	TSharedPtr<FMeshParticleizerGpuState, ESPMode::ThreadSafe> WorldState;
	TWeakPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> SourceAllocation;
};
