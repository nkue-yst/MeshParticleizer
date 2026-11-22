#include "NiagaraDataInterfaceMeshParticleizerBudget.h"
#include "MeshParticleizerGpuState.h"
#include "MeshParticleizerProfiling.h"
#include "MeshParticleizerRestingGpu.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraGpuComputeDataManager.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraGpuReadbackManager.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

namespace MeshParticleizerGpuBudget
{
#if WITH_EDITOR
	UNiagaraDataInterfaceMeshParticleizerBudget::FDispatchObserver DispatchObserver;
#endif
	constexpr auto TemplatePath = TEXT("/Plugin/MeshParticleizer/Private/ParticleizerBudget.ush");

	struct FSource
	{
		TRefCountPtr<FRDGPooledBuffer> Records;
		TRefCountPtr<FRDGPooledBuffer> Scratch;
		TSharedPtr<FMeshParticleizerGpuState, ESPMode::ThreadSafe> World;
		TWeakPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> Allocation;
		uint32 Count = 0;
		uint32 Priority = 1;
		float StartTime = 0;
		bool bInitialized = false;
	};

	// Niagaraのシーン所有。イベントと管理クラスは同じDispatchの破棄で終了する。
	class FManager : public FNiagaraGpuComputeDataManager
	{
	public:
		explicit FManager(FNiagaraGpuComputeDispatchInterface* Owner)
			: FNiagaraGpuComputeDataManager(Owner)
		{
			Owner->GetOnPostRenderEvent().AddRaw(this, &FManager::PostRender);
		}

		static FName GetManagerName()
		{
			return TEXT("MeshParticleizerBudget");
		}

		void Register(FNiagaraSystemInstanceID Id, const TSharedRef<FSource, ESPMode::ThreadSafe>& Source)
		{
			// 同じ割当を別のSystem instanceへ引き継いでも、二重に集計しない。
			for (auto It = this->Sources.CreateIterator(); It; ++It)
			{
				if (It.Value().Pin() == Source)
				{
					It.RemoveCurrent();
				}
			}
			this->Sources.Add(Id, Source);
			this->World = Source->World;
		}

	private:
		void PostRender(FRDGBuilder& GraphBuilder)
		{
			CSV_SCOPED_TIMING_STAT(MeshParticleizer, BudgetPostRender);

			auto Buffers = TArray<FRDGBufferRef>{};
			auto Allocations = TArray<TWeakPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>>{};
			FRDGBufferRef Scratch = nullptr;
			auto Bytes = int64{0};
			for (auto It = this->Sources.CreateIterator(); It; ++It)
			{
				const auto Source = It.Value().Pin();
				if (Source.IsValid() == false)
				{
					It.RemoveCurrent();
					continue;
				}
				if (Source->bInitialized == false)
				{
					continue;
				}
				const auto Playback = Source->Allocation.Pin();
				if (Playback.IsValid() == false || Playback->bActive.load() == false)
				{
					continue;
				}

				Buffers.Add(GraphBuilder.RegisterExternalBuffer(Source->Records));
				Allocations.Add(Source->Allocation);
				if (Scratch == nullptr)
				{
					Scratch = GraphBuilder.RegisterExternalBuffer(Source->Scratch);
				}
				Bytes += Source->Records->GetRHI()->GetSize() + Source->Scratch->GetRHI()->GetSize();
			}
			const auto State = this->World.Pin();
			if (State.IsValid() == false)
			{
				return;
			}
			const auto PublicationSerial = ++this->Serial;
			const auto RenderFrame = GFrameNumberRenderThread;
			if (Buffers.IsEmpty() == true)
			{
				const uint32 Empty[5]{};
				State->Publish(PublicationSerial, RenderFrame, Empty, 0);
				return;
			}

			AddMeshParticleizerRestingPasses(GraphBuilder, Buffers, Scratch, State->RestingLimit.load());

			// 同時要求は一つだけ。全体の診断値と、再生に紐付く消滅数を同じGPUフェンスで受け取る。
			if (State->ReadbackPending.exchange(true) == false)
			{
				// UE 5.8の複数範囲APIはオフセットを0でコピーするため、集計領域を連続して読む。
				const auto ReadBytes = (MeshParticleizerGpuDeathOffset + Allocations.Num()) * sizeof(uint32);
				this->GetOwnerInterface()->GetGpuReadbackManager()->EnqueueReadback(GraphBuilder, Scratch, 0, ReadBytes,
					[State, PublicationSerial, RenderFrame, Bytes, Allocations = MoveTemp(Allocations)](TConstArrayView<TPair<void*, uint32>> Data) {
						CSV_SCOPED_TIMING_STAT(MeshParticleizer, PublishGpuCounts);

						const auto* Deaths = static_cast<const uint32*>(Data[0].Key) + MeshParticleizerGpuDeathOffset;
						for (auto Index = 0; Index < Allocations.Num(); ++Index)
						{
							if (const auto Allocation = Allocations[Index].Pin())
							{
								Allocation->PublishDeaths(Deaths[Index]);
							}
						}
						State->Publish(PublicationSerial, RenderFrame, static_cast<const uint32*>(Data[0].Key), Bytes);
						State->ReadbackPending.store(false);
					});
			}
		}

		TMap<FNiagaraSystemInstanceID, TWeakPtr<FSource, ESPMode::ThreadSafe>> Sources;
		TWeakPtr<FMeshParticleizerGpuState, ESPMode::ThreadSafe> World;
		uint64 Serial = 0;
	};

	struct FProxy : public FNiagaraDataInterfaceProxy
	{
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override
		{
			return 0;
		}

		virtual void PreStage(const FNDIGpuComputePreStageContext& Context) override
		{
			CSV_SCOPED_TIMING_STAT(MeshParticleizer, InitializeBudgetRecords);

			const auto Source = this->Sources.FindRef(Context.GetSystemInstanceID());
			if (Source.IsValid() == false || Source->bInitialized == true)
			{
				return;
			}

			auto& GraphBuilder = Context.GetGraphBuilder();

			// 初回のGPU更新がまだ触れていない候補も、未分離の予約として保持する。
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(Source->Records), PF_R32_UINT), 1);
			Source->bInitialized = true;
		}

		TMap<FNiagaraSystemInstanceID, TSharedPtr<FSource, ESPMode::ThreadSafe>> Sources;
	};

	TRefCountPtr<FRDGPooledBuffer> CreateBuffer(FRHICommandListImmediate& RHICmdList, uint32 Count, const TCHAR* Name)
	{
		auto Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Count);
		Desc.Usage |= BUF_SourceCopy;
		const auto CreateDesc = FRHIBufferCreateDesc::CreateVertex(Name, Desc.GetSize())
									.AddUsage(Desc.Usage)
									.SetInitialState(ERHIAccess::UAVCompute);
		// 共有RDGプールへ返さず、ソースの解放Fenceで容量を実際に手放す。
		return new FRDGPooledBuffer(RHICmdList, RHICmdList.CreateBuffer(CreateDesc), Desc, Count, Name);
	}
}

UNiagaraDataInterfaceMeshParticleizerBudget::UNiagaraDataInterfaceMeshParticleizerBudget(const FObjectInitializer& Initializer)
	: Super(Initializer)
{
	this->Proxy.Reset(new MeshParticleizerGpuBudget::FProxy());
}

void UNiagaraDataInterfaceMeshParticleizerBudget::PostInitProperties()
{
	Super::PostInitProperties();
	if (this->HasAnyFlags(RF_ClassDefaultObject) == true)
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(this->GetClass()),
			ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter);
	}
}

void UNiagaraDataInterfaceMeshParticleizerBudget::Configure(int32 Count, int32 Priority, double StartTime,
	const TSharedRef<FMeshParticleizerGpuState, ESPMode::ThreadSafe>& State,
	const TSharedRef<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>& InAllocation)
{
	if (Count < 0 || Count > 1000000 || Count != InAllocation->CandidateCount || Priority < 0 || Priority > 2 || FMath::IsFinite(StartTime) == false)
	{
		return;
	}
	this->CandidateCount = Count;
	this->SourcePriority = Priority;
	this->WorldStartTime = FMath::Max(0.0, StartTime);
	this->WorldState = State;
	const auto Previous = this->SourceAllocation.Pin();
	this->SourceAllocation = InAllocation;
	if (Previous.IsValid() == true && Previous != InAllocation)
	{
		// 前の再生のFence完了後に呼ぶ。読み戻しの宛先と優先度だけを新しい再生へ替える。
		ENQUEUE_RENDER_COMMAND(MeshParticleizerBudgetReuse)([Previous, InAllocation, Priority, StartTime](FRHICommandListImmediate& RHICmdList) {
				InAllocation->RenderSource = MoveTemp(Previous->RenderSource);
				if (const auto Source = InAllocation->RenderSource)
			{
					Source->Allocation = InAllocation;
					Source->Priority = Priority;
					Source->StartTime = StartTime;
					Source->bInitialized = false;
			}
			});
	}
}

bool UNiagaraDataInterfaceMeshParticleizerBudget::Equals(const UNiagaraDataInterface* Other) const
{
	if (Super::Equals(Other) == false)
	{
		return false;
	}
	const auto* Typed = CastChecked<UNiagaraDataInterfaceMeshParticleizerBudget>(Other);
	return this->CandidateCount == Typed->CandidateCount && this->SourcePriority == Typed->SourcePriority &&
		this->WorldStartTime == Typed->WorldStartTime && this->WorldState == Typed->WorldState &&
		this->SourceAllocation.Pin() == Typed->SourceAllocation.Pin();
}

bool UNiagaraDataInterfaceMeshParticleizerBudget::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (Super::CopyToInternal(Destination) == false)
	{
		return false;
	}
	auto* Typed = CastChecked<UNiagaraDataInterfaceMeshParticleizerBudget>(Destination);
	Typed->CandidateCount = this->CandidateCount;
	Typed->SourcePriority = this->SourcePriority;
	Typed->WorldStartTime = this->WorldStartTime;
	Typed->WorldState = this->WorldState;
	Typed->SourceAllocation = this->SourceAllocation;
	return true;
}

bool UNiagaraDataInterfaceMeshParticleizerBudget::InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	if (this->CandidateCount == 0 || this->WorldState.IsValid() == false)
	{
		return true;
	}
	auto* ComputeInterface = Instance->GetComputeDispatchInterface();
	const auto Allocation = this->SourceAllocation.Pin();
	if (ComputeInterface == nullptr || Allocation.IsValid() == false)
	{
		return false;
	}
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuBudget::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerBudgetCreate)([RenderProxy, Id = Instance->GetId(), Allocation, ComputeInterface,
															 State = this->WorldState, Priority = this->SourcePriority, StartTime = this->WorldStartTime](FRHICommandListImmediate& RHICmdList) {
			if (Allocation->RenderSource.IsValid() == false)
		{
				auto Source = MakeShared<MeshParticleizerGpuBudget::FSource, ESPMode::ThreadSafe>();
				Source->Count = Allocation->CandidateCount;
				Source->Priority = Priority;
				Source->StartTime = StartTime;
				Source->World = State;
				Source->Allocation = Allocation;
				Source->Records = MeshParticleizerGpuBudget::CreateBuffer(RHICmdList, Source->Count * 4, TEXT("MeshParticleizer.Records"));
				Source->Scratch = MeshParticleizerGpuBudget::CreateBuffer(RHICmdList, MeshParticleizerGpuScratchCount, TEXT("MeshParticleizer.Counters"));
				Allocation->RenderSource = Source;
		}
			const auto Source = Allocation->RenderSource.ToSharedRef();
			RenderProxy->Sources.Add(Id, Source);
			ComputeInterface->GetOrCreateDataManager<MeshParticleizerGpuBudget::FManager>().Register(Id, Source);
		});
	return true;
}

void UNiagaraDataInterfaceMeshParticleizerBudget::DestroyPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuBudget::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerBudgetDestroy)([RenderProxy, Id = Instance->GetId()](FRHICommandListImmediate& RHICmdList) {
			RenderProxy->Sources.Remove(Id);
		});
}

void UNiagaraDataInterfaceMeshParticleizerBudget::BuildShaderParameters(FNiagaraShaderParametersBuilder& Builder) const
{
	Builder.AddNestedStruct<FShaderParameters>();
}

#if WITH_EDITOR
void UNiagaraDataInterfaceMeshParticleizerBudget::SetEditorDispatchObserver(FDispatchObserver Observer)
{
	ENQUEUE_RENDER_COMMAND(MeshParticleizerSetDispatchObserver)(
		[Observer = MoveTemp(Observer)](FRHICommandListImmediate&) mutable {
			MeshParticleizerGpuBudget::DispatchObserver = MoveTemp(Observer);
		});
}

int64 UNiagaraDataInterfaceMeshParticleizerBudget::GetProxyBufferBytes(FNiagaraDataInterfaceProxy* RenderProxy,
	FNiagaraSystemInstanceID InstanceId, uint64* OutIdentity)
{
	const auto Source = static_cast<MeshParticleizerGpuBudget::FProxy*>(RenderProxy)->Sources.FindRef(InstanceId);
	if (OutIdentity != nullptr)
	{
		*OutIdentity = Source.IsValid() == true ? reinterpret_cast<uint64>(Source->Records->GetRHI()) : 0;
	}
	return Source.IsValid() == true ? Source->Records->GetRHI()->GetSize() + Source->Scratch->GetRHI()->GetSize() : 0;
}
#endif

void UNiagaraDataInterfaceMeshParticleizerBudget::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	CSV_SCOPED_TIMING_STAT(MeshParticleizer, BindBudgetParameters);

	const auto Source = Context.GetProxy<MeshParticleizerGpuBudget::FProxy>().Sources.FindRef(Context.GetSystemInstanceID());
	auto* Parameters = Context.GetParameterNestedStruct<FShaderParameters>();
	Parameters->Count = Source.IsValid() == true ? Source->Count : 0;
	Parameters->Priority = Source.IsValid() == true ? Source->Priority : 1;
	Parameters->StartTime = Source.IsValid() == true ? Source->StartTime : 0;
	Parameters->Records = Source.IsValid() == true
		? Context.GetGraphBuilder().CreateUAV(Context.GetGraphBuilder().RegisterExternalBuffer(Source->Records), PF_R32_UINT)
		: Context.GetComputeDispatchInterface().GetEmptyBufferUAV(Context.GetGraphBuilder(), PF_R32_UINT);
#if WITH_EDITOR
	if (static_cast<bool>(MeshParticleizerGpuBudget::DispatchObserver) == true)
	{
		MeshParticleizerGpuBudget::DispatchObserver(Context, reinterpret_cast<const uint8*>(Parameters));
	}
#endif
}

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceMeshParticleizerBudget::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const
{
	auto& Signature = Functions.AddDefaulted_GetRef();
	Signature.Name = TEXT("UpdateState");
	Signature.bMemberFunction = true;
	Signature.bRequiresContext = false;
	Signature.bRequiresExecPin = true;
	Signature.bSupportsCPU = false;
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(this->GetClass()), TEXT("Budget")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Alive")));
	for (const auto* Name : {TEXT("Detached"), TEXT("Sleeping"), TEXT("Elapsed")})
	{
		Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Name));
	}
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Fade")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("StillAlive")));
}

void UNiagaraDataInterfaceMeshParticleizerBudget::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL)
{
	this->AppendTemplateHLSL(OutHLSL, MeshParticleizerGpuBudget::TemplatePath,
		{{TEXT("ParameterName"), Info.DataInterfaceHLSLSymbol}});
}

bool UNiagaraDataInterfaceMeshParticleizerBudget::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
	const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL)
{
	return Function.DefinitionName == TEXT("UpdateState");
}

bool UNiagaraDataInterfaceMeshParticleizerBudget::AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const
{
	return Super::AppendCompileHash(Visitor) && Visitor->UpdateShaderFile(MeshParticleizerGpuBudget::TemplatePath) &&
		Visitor->UpdateShaderParameters<FShaderParameters>();
}
#endif
