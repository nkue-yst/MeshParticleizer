#include "NiagaraDataInterfaceMeshParticleizerColor.h"
#include "MeshParticleizerColorGpu.h"
#include "MeshParticleizerGpuState.h"
#include "Engine/Texture2D.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"
#include "RenderGraphUtils.h"
#include "TextureResource.h"

namespace MeshParticleizerGpuColor
{
	constexpr auto TemplatePath = TEXT("/Plugin/MeshParticleizer/Private/ParticleizerColor.ush");

	struct FBinding
	{
		FTextureReferenceRHIRef Texture;
		FSamplerStateRHIRef Sampler;
		FVector2f UVScale = FVector2f(1, 1);
		FVector2f UVOffset = FVector2f::ZeroVector;
		FVector3f ColorScale = FVector3f(1, 1, 1);
		uint64 Revision = 0;

		bool operator==(const FBinding& Other) const
		{
			return this->Texture == Other.Texture && this->Sampler == Other.Sampler &&
				this->UVOffset == Other.UVOffset && this->UVScale == Other.UVScale && this->ColorScale == Other.ColorScale && this->Revision == Other.Revision;
		}
	};

	struct FSource
	{
		TRefCountPtr<FRDGPooledBuffer> Colors;
		TArray<FBinding> Applied;
		uint32 Count = 0;
		bool bInitialized = false;
	};

	struct FProxy : public FNiagaraDataInterfaceProxy
	{
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override
		{
			return 0;
		}

		virtual void PreStage(const FNDIGpuComputePreStageContext& Context) override
		{
			const auto Source = this->Sources.FindRef(Context.GetSystemInstanceID());
			if (Source.IsValid() == false)
			{
				return;
			}

			auto& GraphBuilder = Context.GetGraphBuilder();
			auto* Buffer = GraphBuilder.RegisterExternalBuffer(Source->Colors);
			const auto bReset = Source->bInitialized == false || Source->Applied.Num() != this->Bindings.Num();
			if (bReset == true)
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, PF_R32_UINT), 0);
			}

			auto* PositionsRHI = this->Positions.IsValid() ? this->Positions->GetReferencedTexture() : nullptr;
			auto* CoordinatesRHI = this->Coordinates.IsValid() ? this->Coordinates->GetReferencedTexture() : nullptr;
			if (PositionsRHI != nullptr && CoordinatesRHI != nullptr)
			{
				for (auto Slot = 0; Slot < this->Bindings.Num(); ++Slot)
				{
					const auto& Binding = this->Bindings[Slot];
					if (bReset == false && Binding == Source->Applied[Slot])
					{
						continue;
					}
					if (bReset == true && Binding.Texture.IsValid() == false)
					{
						continue;
					}

					AddMeshParticleizerColorPass(GraphBuilder, Buffer, Source->Count, Slot, PositionsRHI, CoordinatesRHI,
						Binding.Texture.IsValid() ? Binding.Texture->GetReferencedTexture() : nullptr,
						Binding.Sampler, Binding.UVScale, Binding.UVOffset, Binding.ColorScale, this->bEmission == false);
				}
				Source->Applied = this->Bindings;
				Source->bInitialized = true;
			}
		}

		TMap<FNiagaraSystemInstanceID, TSharedPtr<FSource, ESPMode::ThreadSafe>> Sources;
		FTextureReferenceRHIRef Positions;
		FTextureReferenceRHIRef Coordinates;
		TArray<FBinding> Bindings;
		bool bEmission = false;
	};
}

UNiagaraDataInterfaceMeshParticleizerColor::UNiagaraDataInterfaceMeshParticleizerColor(const FObjectInitializer& Initializer)
	: Super(Initializer)
{
	this->Proxy.Reset(new MeshParticleizerGpuColor::FProxy());
}

void UNiagaraDataInterfaceMeshParticleizerColor::PostInitProperties()
{
	Super::PostInitProperties();
	if (this->HasAnyFlags(RF_ClassDefaultObject) == true)
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(this->GetClass()),
			ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter);
	}
}

bool UNiagaraDataInterfaceMeshParticleizerColor::Equals(const UNiagaraDataInterface* Other) const
{
	if (Super::Equals(Other) == false)
	{
		return false;
	}
	const auto* Typed = CastChecked<UNiagaraDataInterfaceMeshParticleizerColor>(Other);
	if (this->Bindings.Num() != Typed->Bindings.Num())
	{
		return false;
	}
	for (auto Slot = 0; Slot < this->Bindings.Num(); ++Slot)
	{
		const auto& A = this->Bindings[Slot];
		const auto& B = Typed->Bindings[Slot];
		if (A.Texture != B.Texture || A.UVOffset != B.UVOffset || A.UVScale != B.UVScale || A.ColorScale != B.ColorScale || A.Revision != B.Revision)
		{
			return false;
		}
	}
	return this->Positions == Typed->Positions && this->Coordinates == Typed->Coordinates &&
		this->bReserved == Typed->bReserved && this->bEmission == Typed->bEmission && this->SourceAllocation.Pin() == Typed->SourceAllocation.Pin() &&
		this->GetClass() == Typed->GetClass();
}

bool UNiagaraDataInterfaceMeshParticleizerColor::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (Super::CopyToInternal(Destination) == false)
	{
		return false;
	}
	auto* Typed = CastChecked<UNiagaraDataInterfaceMeshParticleizerColor>(Destination);
	Typed->Positions = this->Positions;
	Typed->Coordinates = this->Coordinates;
	Typed->Bindings = this->Bindings;
	Typed->bReserved = this->bReserved;
	Typed->bEmission = this->bEmission;
	Typed->SourceAllocation = this->SourceAllocation;
	return true;
}

void UNiagaraDataInterfaceMeshParticleizerColor::Configure(UTexture2D* InPositions, UTexture2D* InCoordinates,
	bool bInReserved, const TSharedRef<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>& Allocation, bool bInEmission)
{
	this->Positions = InPositions;
	this->Coordinates = InCoordinates;
	this->bReserved = bInReserved;
	this->bEmission = bInEmission;
	const auto Previous = this->SourceAllocation.Pin();
	this->SourceAllocation = Allocation;
	if (Previous.IsValid() && Previous != Allocation)
	{
		// プールの終了Fence後に呼ぶ。前の色を初期化しつつ確保した容量を再利用する。
		ENQUEUE_RENDER_COMMAND(MeshParticleizerColorReuse)([Previous, Allocation, Channel = int32(bInEmission)](FRHICommandListImmediate& RHICmdList) {
			Allocation->ColorSources[Channel] = MoveTemp(Previous->ColorSources[Channel]);
			if (Allocation->ColorSources[Channel].IsValid())
			{
				Allocation->ColorSources[Channel]->bInitialized = false;
			}
			});
	}
	this->PushInputs();
}

void UNiagaraDataInterfaceMeshParticleizerColor::SetBindings(TConstArrayView<FMeshParticleizerGpuColorBinding> InBindings)
{
	this->Bindings = InBindings;
	this->PushInputs();
}

void UNiagaraDataInterfaceMeshParticleizerColor::PushInputs()
{
	const auto PositionsRHI = this->Positions != nullptr ? this->Positions->TextureReference.TextureReferenceRHI : FTextureReferenceRHIRef{};
	const auto CoordinatesRHI = this->Coordinates != nullptr ? this->Coordinates->TextureReference.TextureReferenceRHI : FTextureReferenceRHIRef{};
	auto RenderBindings = TArray<MeshParticleizerGpuColor::FBinding>{};
	for (const auto& Binding : this->Bindings)
	{
		auto& Render = RenderBindings.AddDefaulted_GetRef();
		if (Binding.Texture != nullptr)
		{
			Render.Texture = Binding.Texture->TextureReference.TextureReferenceRHI;
			if (const auto* Resource = Binding.Texture->GetResource())
			{
				Render.Sampler = Resource->SamplerStateRHI;
			}
		}
		Render.UVScale = FVector2f(Binding.UVScale);
		Render.UVOffset = FVector2f(Binding.UVOffset);
		Render.ColorScale = FVector3f(Binding.ColorScale.R, Binding.ColorScale.G, Binding.ColorScale.B);
		Render.Revision = Binding.Revision;
	}

	// Render ThreadへはRHI参照と値だけを渡す。UObjectの生参照はCaptureしない。
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuColor::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerColorInputs)([RenderProxy, PositionsRHI, CoordinatesRHI,
															bHDR = this->bEmission, RenderBindings = MoveTemp(RenderBindings)](FRHICommandListImmediate& RHICmdList) mutable {
			RenderProxy->Positions = PositionsRHI;
			RenderProxy->Coordinates = CoordinatesRHI;
			RenderProxy->Bindings = MoveTemp(RenderBindings);
			RenderProxy->bEmission = bHDR;
		});
}

bool UNiagaraDataInterfaceMeshParticleizerColor::InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	if (this->bReserved == false)
	{
		return true;
	}
	const auto Allocation = this->SourceAllocation.Pin();
	if (Allocation.IsValid() == false || Allocation->CandidateCount <= 0 || this->Positions == nullptr || this->Coordinates == nullptr)
	{
		return false;
	}
	this->PushInputs();

	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuColor::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerColorCreate)([RenderProxy, Id = Instance->GetId(), Allocation, Channel = int32(this->bEmission)](FRHICommandListImmediate& RHICmdList) {
			if (Allocation->ColorSources[Channel].IsValid() == false)
		{
				auto Source = MakeShared<MeshParticleizerGpuColor::FSource, ESPMode::ThreadSafe>();
				Source->Count = Allocation->CandidateCount;
				const auto Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Source->Count);
				const auto* Name = Channel == 0 ? TEXT("MeshParticleizer.SurfaceColors") : TEXT("MeshParticleizer.SourceEmission");
				const auto CreateDesc = FRHIBufferCreateDesc::CreateVertex(Name, Desc.GetSize())
										.AddUsage(Desc.Usage)
										.SetInitialState(ERHIAccess::UAVCompute);
				// RDGの共有プールへ残さず、再生の解放Fenceと同じ寿命で返却する。
				Source->Colors = new FRDGPooledBuffer(RHICmdList, RHICmdList.CreateBuffer(CreateDesc), Desc, Source->Count, Name);
				Allocation->ColorSources[Channel] = Source;
		}
			RenderProxy->Sources.Add(Id, Allocation->ColorSources[Channel]);
		});
	return true;
}

void UNiagaraDataInterfaceMeshParticleizerColor::DestroyPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuColor::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerColorDestroy)([RenderProxy, Id = Instance->GetId()](FRHICommandListImmediate& RHICmdList) {
		RenderProxy->Sources.Remove(Id);
	});
}

void UNiagaraDataInterfaceMeshParticleizerColor::BuildShaderParameters(FNiagaraShaderParametersBuilder& Builder) const
{
	Builder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceMeshParticleizerColor::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	const auto Source = Context.GetProxy<MeshParticleizerGpuColor::FProxy>().Sources.FindRef(Context.GetSystemInstanceID());
	auto* Parameters = Context.GetParameterNestedStruct<FShaderParameters>();
	Parameters->Count = Source.IsValid() ? Source->Count : 0;
	Parameters->Colors = Source.IsValid()
		? Context.GetGraphBuilder().CreateSRV(Context.GetGraphBuilder().RegisterExternalBuffer(Source->Colors), PF_A32B32G32R32F)
		: Context.GetComputeDispatchInterface().GetEmptyBufferSRV(Context.GetGraphBuilder(), PF_A32B32G32R32F);
}

#if WITH_EDITOR
int64 UNiagaraDataInterfaceMeshParticleizerColor::GetProxyBufferBytes(FNiagaraDataInterfaceProxy* Proxy, FNiagaraSystemInstanceID Id, uint64* OutIdentity)
{
	const auto Source = static_cast<MeshParticleizerGpuColor::FProxy*>(Proxy)->Sources.FindRef(Id);
	if (OutIdentity != nullptr)
	{
		*OutIdentity = Source.IsValid() ? reinterpret_cast<uint64>(Source->Colors->GetRHI()) : 0;
	}
	return Source.IsValid() ? Source->Colors->GetRHI()->GetSize() : 0;
}
#endif

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceMeshParticleizerColor::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const
{
	auto& Signature = Functions.AddDefaulted_GetRef();
	Signature.Name = TEXT("GetColor");
	Signature.bMemberFunction = true;
	Signature.bRequiresContext = false;
	Signature.bSupportsCPU = false;
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(this->GetClass()), TEXT("SurfaceColor")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Baked")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Color")));
}

void UNiagaraDataInterfaceMeshParticleizerColor::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL)
{
	this->AppendTemplateHLSL(OutHLSL, MeshParticleizerGpuColor::TemplatePath, {{TEXT("ParameterName"), Info.DataInterfaceHLSLSymbol}});
}

bool UNiagaraDataInterfaceMeshParticleizerColor::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
	const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL)
{
	return Function.DefinitionName == TEXT("GetColor");
}

bool UNiagaraDataInterfaceMeshParticleizerColor::AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const
{
	return Super::AppendCompileHash(Visitor) && Visitor->UpdateShaderFile(MeshParticleizerGpuColor::TemplatePath) &&
		Visitor->UpdateShaderParameters<FShaderParameters>();
}
#endif
