#include "NiagaraDataInterfaceMeshParticleizerCollision.h"
#include "MeshParticleizerDistanceField.h"
#include "MeshParticleizerProfiling.h"
#include "GlobalDistanceFieldParameters.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraDistanceFieldHelper.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"

namespace MeshParticleizerCollision
{
	struct FProxy : public FNiagaraDataInterfaceProxy
	{
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override
		{
			return 0;
		}
		TMap<FNiagaraSystemInstanceID, TSharedPtr<FMeshParticleizerDistanceField, ESPMode::ThreadSafe>> Worlds;
	};
}

UNiagaraDataInterfaceMeshParticleizerCollision::UNiagaraDataInterfaceMeshParticleizerCollision(const FObjectInitializer& Initializer)
	: Super(Initializer)
{
	this->Proxy.Reset(new MeshParticleizerCollision::FProxy());
}

void UNiagaraDataInterfaceMeshParticleizerCollision::PostInitProperties()
{
	Super::PostInitProperties();
	if (this->HasAnyFlags(RF_ClassDefaultObject) == true)
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(this->GetClass()),
			ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter);
	}
}

void UNiagaraDataInterfaceMeshParticleizerCollision::Configure(const TSharedRef<FMeshParticleizerDistanceField, ESPMode::ThreadSafe>& State)
{
	this->WorldState = State;
}

bool UNiagaraDataInterfaceMeshParticleizerCollision::Equals(const UNiagaraDataInterface* Other) const
{
	return Super::Equals(Other) && this->WorldState == CastChecked<UNiagaraDataInterfaceMeshParticleizerCollision>(Other)->WorldState;
}

bool UNiagaraDataInterfaceMeshParticleizerCollision::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (Super::CopyToInternal(Destination) == false)
	{
		return false;
	}
	CastChecked<UNiagaraDataInterfaceMeshParticleizerCollision>(Destination)->WorldState = this->WorldState;
	return true;
}

bool UNiagaraDataInterfaceMeshParticleizerCollision::InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerCollision::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerGDFCreate)([RenderProxy, Id = Instance->GetId(), State = this->WorldState](FRHICommandListImmediate& RHICmdList) {
		RenderProxy->Worlds.Add(Id, State);
	});
	return this->WorldState.IsValid();
}

void UNiagaraDataInterfaceMeshParticleizerCollision::DestroyPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerCollision::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerGDFDestroy)([RenderProxy, Id = Instance->GetId()](FRHICommandListImmediate& RHICmdList) {
		RenderProxy->Worlds.Remove(Id);
	});
}

void UNiagaraDataInterfaceMeshParticleizerCollision::BuildShaderParameters(FNiagaraShaderParametersBuilder& Builder) const
{
	Builder.AddIncludedStruct<FGlobalDistanceFieldParameters2>();
}

void UNiagaraDataInterfaceMeshParticleizerCollision::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, BindCollisionParameters);

	const auto State = Context.GetProxy<MeshParticleizerCollision::FProxy>().Worlds.FindRef(Context.GetSystemInstanceID());
	const auto* Data = Context.GetComputeDispatchInterface().GetGlobalDistanceFieldData();
	const auto Bytes = FMeshParticleizerDistanceField::Measure(Data);
	const auto bCovered = State.IsValid() && Bytes > 0 && Bytes <= State->ReservedBytes.load() / 2;

	// 解像度変更などで予約を超えた資源をバインドしない。次のWorld更新で表示を復元して終了する。
	if (State.IsValid() == true && bCovered == false)
	{
		State->Fault.store(true);
	}
	FNiagaraDistanceFieldHelper::SetGlobalDistanceFieldParameters(bCovered ? Data : nullptr,
		*Context.GetParameterIncludedStruct<FGlobalDistanceFieldParameters2>());
}

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceMeshParticleizerCollision::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const
{
	auto& Signature = Functions.AddDefaulted_GetRef();
	Signature.Name = TEXT("QuerySurface");
	Signature.bMemberFunction = true;
	Signature.bRequiresContext = false;
	Signature.bSupportsCPU = false;
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(this->GetClass()), TEXT("Collision")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Distance")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Gradient")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid")));
}

void UNiagaraDataInterfaceMeshParticleizerCollision::GetCommonHLSL(FString& OutHLSL)
{
	OutHLSL += TEXT("#include \"/Plugin/FX/Niagara/Private/NiagaraDataInterfaceCollisionQuery.ush\"\n");
}

void UNiagaraDataInterfaceMeshParticleizerCollision::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL)
{
	OutHLSL += FString::Printf(TEXT("void QuerySurface_%s(float3 Position, out float Distance, out float3 Gradient, out bool Valid)\n"
		"{ NDICollisionQuery_QueryMeshDistanceFieldGPU(Position, GetEngineOwnerLWCTile(), Distance, Gradient, Valid); }\n"),
		*Info.DataInterfaceHLSLSymbol);
}

bool UNiagaraDataInterfaceMeshParticleizerCollision::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
	const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL)
{
	return Function.DefinitionName == TEXT("QuerySurface");
}

bool UNiagaraDataInterfaceMeshParticleizerCollision::AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const
{
	return Super::AppendCompileHash(Visitor) &&
		Visitor->UpdateShaderFile(TEXT("/Plugin/FX/Niagara/Private/NiagaraDataInterfaceCollisionQuery.ush")) &&
		Visitor->UpdateShaderParameters<FGlobalDistanceFieldParameters2>();
}
#endif
