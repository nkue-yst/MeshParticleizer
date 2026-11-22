#include "NiagaraDataInterfaceMeshParticleizerCoverage.h"
#include "MeshParticleizerCoverageGpu.h"
#include "MeshParticleizerGpuState.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Engine/Texture2D.h"
#include "FXRenderingUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"
#include "RenderGraphUtils.h"
#include "SceneView.h"
#include "TextureResource.h"

namespace MeshParticleizerGpuCoverage
{
	constexpr auto TemplatePath = TEXT("/Plugin/MeshParticleizer/Private/ParticleizerCoverage.ush");

	struct FSource
	{
		TRefCountPtr<FRDGPooledBuffer> Samples;
		TRefCountPtr<FRDGPooledBuffer> Bones;
		TRefCountPtr<FRDGPooledBuffer> Slots;
		TRefCountPtr<FRDGPooledBuffer> RasterHeads;
		TRefCountPtr<FRDGPooledBuffer> RasterLinks;
		FTextureRHIRef Coverage;
		TWeakPtr<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe> Allocation;
		uint32 Count = 0;
		uint32 BoneCount = 0;
		uint32 MaterialSlots = 0;
		uint64 BoneRevision = 0;
		bool bInitialized = false;
		bool bValid = false;
#if WITH_EDITOR
		// 診断では、最後に正常評価したView数と実際の確保量を同時に確認する。
		uint32 ViewCount = 0;
#endif

		int64 Bytes() const
		{
			return this->Samples->GetRHI()->GetSize() + (this->Bones.IsValid() ? this->Bones->GetRHI()->GetSize() : 0) +
				this->Slots->GetRHI()->GetSize() + this->RasterHeads->GetRHI()->GetSize() + this->RasterLinks->GetRHI()->GetSize() +
				static_cast<int64>(RHICalcTexturePlatformSize(this->Coverage->GetDesc(), 0).Size);
		}
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
			const auto Allocation = Source.IsValid() ? Source->Allocation.Pin() : nullptr;
			if (Allocation.IsValid() == false || Allocation->bActive.load() == false || this->bEvaluate == false)
			{
				return;
			}
			Source->bValid = false;
			const auto Views = Context.GetComputeDispatchInterface().GetSimulationSceneViews();
			if (Views.IsEmpty() || this->DynamicSlots.Num() != Source->MaterialSlots || this->MaterialProxies.Num() != Source->MaterialSlots ||
				this->BoneRows.Num() != Source->BoneCount * 3 || Source->Bytes() > MeshParticleizerCoverageGpuBytes(Source->Count, Source->BoneCount, Source->MaterialSlots))
			{
				UE_LOG(LogTemp, Warning, TEXT("Coverage inputs invalid: Views=%d Slots=%d/%u Materials=%d Bones=%d/%u Bytes=%lld/%lld"), Views.Num(), this->DynamicSlots.Num(), Source->MaterialSlots, this->MaterialProxies.Num(), this->BoneRows.Num(), Source->BoneCount * 3, Source->Bytes(), MeshParticleizerCoverageGpuBytes(Source->Count, Source->BoneCount, Source->MaterialSlots));
				Allocation->bCoverageFault.store(true);
				return;
			}

			auto& Graph = Context.GetGraphBuilder();
			FRDGTextureRef Inputs[6]{};
			for (auto Index = 0; Index < (Source->BoneCount > 0 ? 6 : 4); ++Index)
			{
				auto* Texture = this->Textures[Index].IsValid() ? this->Textures[Index]->GetReferencedTexture() : nullptr;
				if (Texture == nullptr)
				{
					UE_LOG(LogTemp, Warning, TEXT("Coverage texture unavailable: input %d"), Index);
					Allocation->bCoverageFault.store(true);
					return;
				}
				Inputs[Index] = Graph.RegisterExternalTexture(CreateRenderTarget(Texture, TEXT("MeshParticleizer.CoverageInput")));
			}
			auto* Samples = Graph.RegisterExternalBuffer(Source->Samples);
			auto* Slots = Graph.RegisterExternalBuffer(Source->Slots);
			auto* RasterHeads = Graph.RegisterExternalBuffer(Source->RasterHeads);
			auto* RasterLinks = Graph.RegisterExternalBuffer(Source->RasterLinks);
			auto* Coverage = Graph.RegisterExternalTexture(CreateRenderTarget(Source->Coverage, TEXT("MeshParticleizer.Coverage")));
			auto Geometry = FMeshParticleizerCoverageGeometry{};
			Geometry.Count = Source->Count;
			Geometry.BoneCount = Source->BoneCount;
			Geometry.TriangleCount = this->TriangleCount;
			Geometry.DynamicSlots = Slots;
			Geometry.MaterialSlots = Source->MaterialSlots;
			Geometry.LocalToWorld = this->Transform;
			Geometry.Positions = Inputs[0];
			Geometry.Normals = Inputs[1];
			Geometry.Coordinates = Inputs[2];
			Geometry.TriangleData = Inputs[3];
			Geometry.SkinVertices = Inputs[4];
			Geometry.SkinTriangles = Inputs[5];
			if (Source->bInitialized == false)
			{
				Graph.QueueBufferUpload(Slots, this->DynamicSlots.GetData(), this->DynamicSlots.Num() * sizeof(uint32));
			}
			if (Source->Bones.IsValid())
			{
				Geometry.Bones = Graph.RegisterExternalBuffer(Source->Bones);
				if (Source->bInitialized == false || Source->BoneRevision != this->BoneRevision)
				{
					Graph.QueueBufferUpload(Geometry.Bones, this->BoneRows.GetData(), this->BoneRows.Num() * sizeof(FVector4f));
					Source->BoneRevision = this->BoneRevision;
				}
			}
			auto bValid = AddMeshParticleizerCoverageClearPass(Graph, Source->Count, Source->MaterialSlots, Inputs[0], Slots, Coverage);
			if (bValid == false)
			{
				UE_LOG(LogTemp, Warning, TEXT("Coverage clear input rejected."));
			}
			for (const auto& View : Views)
			{
				if (bValid == false)
				{
					break;
				}
				bValid = AddMeshParticleizerCoverageGeometryPass(Graph, View, Geometry, Samples);
				if (bValid == false)
				{
					UE_LOG(LogTemp, Warning, TEXT("Coverage geometry input rejected."));
				}
				// Niagaraが使う実際の内部描画領域を渡す。出力解像度や他Viewの領域で補間しない。
				bValid = bValid && View.bIsViewInfo && AddMeshParticleizerCoverageRasterInputPass(Graph, View, UE::FXRenderingUtils::GetRawViewRectUnsafe(View), Geometry, Samples, RasterHeads, RasterLinks);
				if (bValid == false)
				{
					UE_LOG(LogTemp, Warning, TEXT("Coverage raster input rejected."));
				}
				for (auto Slot = 0; bValid && Slot < this->DynamicSlots.Num(); ++Slot)
				{
					if (this->DynamicSlots[Slot] == 0)
					{
						continue;
					}
					bValid = AddMeshParticleizerCoveragePass(Graph, View, this->MaterialProxies[Slot], Slot, Samples, Source->Count, Coverage);
					if (bValid == false)
					{
						UE_LOG(LogTemp, Warning, TEXT("Coverage shader unavailable: slot %d"), Slot);
					}
				}
			}
			Source->bInitialized = true;
			Source->bValid = bValid;
#if WITH_EDITOR
			Source->ViewCount = bValid == true ? Views.Num() : 0;
#endif
			if (bValid == false)
			{
				Allocation->bCoverageFault.store(true);
			}
		}

		TMap<FNiagaraSystemInstanceID, TSharedPtr<FSource, ESPMode::ThreadSafe>> Sources;
		FTextureReferenceRHIRef Textures[6];
		TArray<const FMaterialRenderProxy*> MaterialProxies;
		TArray<uint32> DynamicSlots;
		TArray<FVector4f> BoneRows;
		FTransform Transform = FTransform::Identity;
		uint64 BoneRevision = 0;
		uint32 TriangleCount = 0;
		bool bEvaluate = true;
		bool bReserved = false;
	};
}

UNiagaraDataInterfaceMeshParticleizerCoverage::UNiagaraDataInterfaceMeshParticleizerCoverage(const FObjectInitializer& Initializer)
	: Super(Initializer)
{
	this->Proxy.Reset(new MeshParticleizerGpuCoverage::FProxy());
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::PostInitProperties()
{
	Super::PostInitProperties();
	if (this->HasAnyFlags(RF_ClassDefaultObject) == true)
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(this->GetClass()), ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter);
	}
}

bool UNiagaraDataInterfaceMeshParticleizerCoverage::Equals(const UNiagaraDataInterface* Other) const
{
	if (Super::Equals(Other) == false)
	{
		return false;
	}
	const auto* Typed = CastChecked<UNiagaraDataInterfaceMeshParticleizerCoverage>(Other);
	return this->Prepared == Typed->Prepared && this->Materials == Typed->Materials && this->bReserved == Typed->bReserved &&
		this->SourceAllocation.Pin() == Typed->SourceAllocation.Pin() && this->Transform.Equals(Typed->Transform) &&
		this->BoneRows == Typed->BoneRows && this->bEvaluate == Typed->bEvaluate;
}

bool UNiagaraDataInterfaceMeshParticleizerCoverage::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (Super::CopyToInternal(Destination) == false)
	{
		return false;
	}
	auto* Typed = CastChecked<UNiagaraDataInterfaceMeshParticleizerCoverage>(Destination);
	Typed->Prepared = this->Prepared;
	Typed->Materials = this->Materials;
	Typed->bReserved = this->bReserved;
	Typed->SourceAllocation = this->SourceAllocation;
	Typed->Transform = this->Transform;
	Typed->BoneRows = this->BoneRows;
	Typed->BoneRevision = this->BoneRevision;
	Typed->bEvaluate = this->bEvaluate;
	return true;
}

bool UNiagaraDataInterfaceMeshParticleizerCoverage::Configure(UMeshParticleizerPreparedMesh* InPrepared,
	TConstArrayView<TObjectPtr<UMaterialInstanceDynamic>> InMaterials, bool bInReserved,
	const TSharedRef<FMeshParticleizerGpuAllocation, ESPMode::ThreadSafe>& Allocation, FString& OutError)
{
	if (bInReserved && (InPrepared == nullptr || InPrepared->CoverageDataVersion != UMeshParticleizerPreparedMesh::CurrentCoverageDataVersion || InPrepared->CoverageTriangleCount <= 0 || InPrepared->CoverageTriangles.IsValid() == false || InPrepared->Coordinates.IsValid() == false || InMaterials.Num() != InPrepared->TextureColorSources.Num() || InMaterials.IsEmpty() || InMaterials.ContainsByPredicate([](const auto& Material) {
			return Material == nullptr;
		})))
	{
		OutError = TEXT("Runtime coverage requires complete prepared geometry and every display material.");
		return false;
	}
	this->Prepared = InPrepared;
	this->Materials = InMaterials;
	this->bReserved = bInReserved;
	this->bEvaluate = true;
	this->BoneRows.Reset();
	++this->BoneRevision;
	const auto Previous = this->SourceAllocation.Pin();
	this->SourceAllocation = Allocation;
	if (bInReserved && Previous.IsValid() && Previous != Allocation)
	{
		// 再利用前のGPU完了をSimulationが待つ。確保容量を残し、入力の対応だけ初期化する。
		ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageReuse)([Previous, Allocation](FRHICommandListImmediate& RHICmdList) {
			Allocation->CoverageSource = MoveTemp(Previous->CoverageSource);
			if (Allocation->CoverageSource.IsValid())
			{
				Allocation->CoverageSource->bInitialized = false;
				Allocation->CoverageSource->bValid = false;
				Allocation->CoverageSource->Allocation = Allocation;
			}
		});
	}
	this->PushInputs();
	return true;
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::PushInputs()
{
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuCoverage::FProxy>();
	if (this->bReserved == false || this->Prepared == nullptr)
	{
		ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageDisable)([RenderProxy](FRHICommandListImmediate& RHICmdList) {
			RenderProxy->bReserved = false;
		});
		return;
	}
	auto References = TArray<FTextureReferenceRHIRef>{};
	const UTexture2D* Textures[] = {this->Prepared->Positions.Get(), this->Prepared->Normals.Get(), this->Prepared->Coordinates.Get(),
		this->Prepared->CoverageTriangles.Get(), this->Prepared->SkinVertices.Get(), this->Prepared->SkinTriangles.Get()};
	for (const auto* Texture : Textures)
	{
		References.Add(Texture != nullptr ? Texture->TextureReference.TextureReferenceRHI : FTextureReferenceRHIRef{});
	}
	auto Proxies = TArray<const FMaterialRenderProxy*>{};
	auto Slots = TArray<uint32>{};
	for (auto Slot = 0; Slot < this->Materials.Num(); ++Slot)
	{
		Proxies.Add(this->Materials[Slot]->GetRenderProxy());
		Slots.Add(this->Prepared->TextureColorSources[Slot].bRequiresRuntimeCoverage ? 1 : 0);
	}
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageInputs)([RenderProxy, References = MoveTemp(References),
															   Proxies = MoveTemp(Proxies), Slots = MoveTemp(Slots), Transform = this->Transform,
															   Bones = this->BoneRows, Revision = this->BoneRevision, TriangleCount = uint32(this->Prepared->CoverageTriangleCount),
															   bEvaluate = this->bEvaluate](FRHICommandListImmediate& RHICmdList) mutable {
		for (auto Index = 0; Index < 6; ++Index)
		{
			RenderProxy->Textures[Index] = References[Index];
		}
		RenderProxy->MaterialProxies = MoveTemp(Proxies);
		RenderProxy->DynamicSlots = MoveTemp(Slots);
		RenderProxy->Transform = Transform;
		RenderProxy->BoneRows = MoveTemp(Bones);
		RenderProxy->BoneRevision = Revision;
		RenderProxy->TriangleCount = TriangleCount;
		RenderProxy->bEvaluate = bEvaluate;
		RenderProxy->bReserved = true;
	});
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::SetTransform(const FTransform& InTransform)
{
	if (this->bReserved == false)
	{
		return;
	}
	if (InTransform.ContainsNaN() || InTransform.GetScale3D().GetAbsMin() <= SMALL_NUMBER)
	{
		if (const auto Allocation = this->SourceAllocation.Pin())
		{
			Allocation->bCoverageFault.store(true);
		}
		return;
	}
	this->Transform = InTransform;
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuCoverage::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageTransform)([RenderProxy, InTransform](FRHICommandListImmediate& RHICmdList) {
		RenderProxy->Transform = InTransform;
	});
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::SetBones(TConstArrayView<FVector4f> Rows)
{
	if (this->bReserved == false)
	{
		return;
	}
	if (this->Prepared == nullptr || Rows.Num() != this->Prepared->BoneCount * 3 ||
		Rows.ContainsByPredicate([](const FVector4f& Row) {
			return Row.ContainsNaN();
		}))
	{
		if (const auto Allocation = this->SourceAllocation.Pin())
		{
			Allocation->bCoverageFault.store(true);
		}
		return;
	}
	this->BoneRows = Rows;
	++this->BoneRevision;
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuCoverage::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageBones)([RenderProxy, Values = this->BoneRows, Revision = this->BoneRevision](FRHICommandListImmediate& RHICmdList) mutable {
		RenderProxy->BoneRows = MoveTemp(Values);
		RenderProxy->BoneRevision = Revision;
	});
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::StopEvaluating()
{
	if (this->bReserved == false || this->bEvaluate == false)
	{
		return;
	}
	this->bEvaluate = false;
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuCoverage::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageStop)([RenderProxy](FRHICommandListImmediate& RHICmdList) {
		RenderProxy->bEvaluate = false;
	});
}

bool UNiagaraDataInterfaceMeshParticleizerCoverage::InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	if (this->bReserved == false)
	{
		return true;
	}
	const auto Allocation = this->SourceAllocation.Pin();
	if (Allocation.IsValid() == false || Allocation->CandidateCount <= 0 || this->Prepared == nullptr)
	{
		return false;
	}
	this->PushInputs();
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuCoverage::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageCreate)([RenderProxy, Id = Instance->GetId(), Allocation,
															   BoneCount = uint32(this->Prepared->BoneCount), MaterialSlots = uint32(this->Materials.Num())](FRHICommandListImmediate& RHICmdList) {
		if (Allocation->CoverageSource.IsValid() == false)
		{
			auto Source = MakeShared<MeshParticleizerGpuCoverage::FSource, ESPMode::ThreadSafe>();
			Source->Count = Allocation->CandidateCount;
			Source->BoneCount = BoneCount;
			Source->MaterialSlots = MaterialSlots;
			Source->Allocation = Allocation;
			auto Buffer = [&RHICmdList](const TCHAR* Name, uint32 ElementSize, uint32 Count) {
				const auto Desc = FRDGBufferDesc::CreateBufferDesc(ElementSize, Count);
				const auto CreateDesc = FRHIBufferCreateDesc::CreateVertex(Name, Desc.GetSize()).AddUsage(Desc.Usage).SetInitialState(ERHIAccess::UAVCompute);
				return TRefCountPtr<FRDGPooledBuffer>(new FRDGPooledBuffer(RHICmdList, RHICmdList.CreateBuffer(CreateDesc), Desc, Count, Name));
			};
			Source->Samples = Buffer(TEXT("MeshParticleizer.CoverageSamples"), sizeof(FVector4f), Source->Count * 4);
			Source->Slots = Buffer(TEXT("MeshParticleizer.CoverageSlots"), sizeof(uint32), MaterialSlots);
			Source->RasterHeads = Buffer(TEXT("MeshParticleizer.CoverageRasterHeads"), sizeof(uint32), MeshParticleizerCoverageBucketCount(Source->Count));
			Source->RasterLinks = Buffer(TEXT("MeshParticleizer.CoverageRasterLinks"), sizeof(FUintVector4), Source->Count);
			if (BoneCount > 0)
			{
				Source->Bones = Buffer(TEXT("MeshParticleizer.CoverageBones"), sizeof(FVector4f), BoneCount * 3);
			}
			Source->Coverage = RHICmdList.CreateTexture(FRHITextureCreateDesc::Create2D(TEXT("MeshParticleizer.Coverage"),
																							MeshParticleizerCoverageWidth, FMath::DivideAndRoundUp(Source->Count, MeshParticleizerCoverageWidth), PF_R32_FLOAT)
																							.SetFlags(TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV)
																							.SetInitialState(ERHIAccess::UAVCompute));
			Allocation->CoverageSource = Source;
		}
		const auto Source = Allocation->CoverageSource;
		if (Source->Count != Allocation->CandidateCount || Source->BoneCount != BoneCount || Source->MaterialSlots != MaterialSlots)
		{
			Allocation->bCoverageFault.store(true);
			return;
		}
		RenderProxy->Sources.Add(Id, Source);
	});
	return true;
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::DestroyPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	auto* RenderProxy = this->GetProxyAs<MeshParticleizerGpuCoverage::FProxy>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerCoverageDestroy)([RenderProxy, Id = Instance->GetId()](FRHICommandListImmediate& RHICmdList) {
		RenderProxy->Sources.Remove(Id);
	});
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::BuildShaderParameters(FNiagaraShaderParametersBuilder& Builder) const
{
	Builder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	const auto Source = Context.GetProxy<MeshParticleizerGpuCoverage::FProxy>().Sources.FindRef(Context.GetSystemInstanceID());
	auto* Parameters = Context.GetParameterNestedStruct<FShaderParameters>();
	Parameters->Enabled = Context.GetProxy<MeshParticleizerGpuCoverage::FProxy>().bReserved ? 1 : 0;
	Parameters->Count = Source.IsValid() && Source->bValid ? Source->Count : 0;
	Parameters->Coverage = Source.IsValid() ? Context.GetGraphBuilder().RegisterExternalTexture(CreateRenderTarget(Source->Coverage, TEXT("MeshParticleizer.CoverageRead")))
											: Context.GetComputeDispatchInterface().GetBlackTexture(Context.GetGraphBuilder(), ETextureDimension::Texture2D);
}

#if WITH_EDITOR
int64 UNiagaraDataInterfaceMeshParticleizerCoverage::GetProxyBufferBytes(FNiagaraDataInterfaceProxy* Proxy,
	FNiagaraSystemInstanceID Id, uint64* OutIdentity, uint32* OutViewCount,
	int64* OutIndexBytes, uint64* OutHeadsIdentity, uint64* OutLinksIdentity)
{
	const auto Source = static_cast<MeshParticleizerGpuCoverage::FProxy*>(Proxy)->Sources.FindRef(Id);
	if (OutViewCount != nullptr)
	{
		*OutViewCount = Source.IsValid() && Source->bValid == true ? Source->ViewCount : 0;
	}
	if (OutIdentity != nullptr)
	{
		*OutIdentity = Source.IsValid() ? reinterpret_cast<uint64>(Source->Samples->GetRHI()) : 0;
	}
	if (OutIndexBytes != nullptr)
	{
		*OutIndexBytes = Source.IsValid() ? Source->RasterHeads->GetRHI()->GetSize() + Source->RasterLinks->GetRHI()->GetSize() : 0;
	}
	if (OutHeadsIdentity != nullptr)
	{
		*OutHeadsIdentity = Source.IsValid() ? reinterpret_cast<uint64>(Source->RasterHeads->GetRHI()) : 0;
	}
	if (OutLinksIdentity != nullptr)
	{
		*OutLinksIdentity = Source.IsValid() ? reinterpret_cast<uint64>(Source->RasterLinks->GetRHI()) : 0;
	}
	return Source.IsValid() ? Source->Bytes() : 0;
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::ReadProxyCoverage(FRHICommandListImmediate& RHICmdList,
	FNiagaraDataInterfaceProxy* Proxy, FNiagaraSystemInstanceID Id, TArray<float>& OutCoverage)
{
	const auto Source = static_cast<MeshParticleizerGpuCoverage::FProxy*>(Proxy)->Sources.FindRef(Id);
	OutCoverage.Reset();
	if (Source.IsValid() == false || Source->bValid == false)
	{
		return;
	}

	auto Pixels = TArray<FLinearColor>{};
	auto Flags = FReadSurfaceDataFlags(RCM_MinMax);
	Flags.SetLinearToGamma(false);
	RHICmdList.ReadSurfaceData(Source->Coverage, FIntRect(0, 0, MeshParticleizerCoverageWidth, FMath::DivideAndRoundUp(Source->Count, MeshParticleizerCoverageWidth)), Pixels, Flags);
	if (uint32(Pixels.Num()) < Source->Count)
	{
		return;
	}
	OutCoverage.Reserve(Source->Count);
	for (auto Index = 0u; Index < Source->Count; ++Index)
	{
		OutCoverage.Add(Pixels[Index].R);
	}
}
#endif

#if WITH_EDITORONLY_DATA
void UNiagaraDataInterfaceMeshParticleizerCoverage::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& Functions) const
{
	auto& Signature = Functions.AddDefaulted_GetRef();
	Signature.Name = TEXT("GetCoverage");
	Signature.bMemberFunction = true;
	Signature.bRequiresContext = false;
	Signature.bSupportsCPU = false;
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(this->GetClass()), TEXT("SurfaceCoverage")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Covered")));
}

void UNiagaraDataInterfaceMeshParticleizerCoverage::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info, FString& OutHLSL)
{
	this->AppendTemplateHLSL(OutHLSL, MeshParticleizerGpuCoverage::TemplatePath, {{TEXT("ParameterName"), Info.DataInterfaceHLSLSymbol}});
}

bool UNiagaraDataInterfaceMeshParticleizerCoverage::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& Info,
	const FNiagaraDataInterfaceGeneratedFunction& Function, int FunctionIndex, FString& OutHLSL)
{
	return Function.DefinitionName == TEXT("GetCoverage");
}

bool UNiagaraDataInterfaceMeshParticleizerCoverage::AppendCompileHash(FNiagaraCompileHashVisitor* Visitor) const
{
	return Super::AppendCompileHash(Visitor) && Visitor->UpdateShaderFile(MeshParticleizerGpuCoverage::TemplatePath) && Visitor->UpdateShaderParameters<FShaderParameters>();
}
#endif
