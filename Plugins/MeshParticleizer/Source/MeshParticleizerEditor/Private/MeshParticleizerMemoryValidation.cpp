#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerSceneGpu.h"
#include "Engine/Engine.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Dom/JsonObject.h"
#include "NiagaraComponent.h"
#include "NiagaraComputeExecutionContext.h"
#include "NiagaraDataInterfaceArray.h"
#include "NiagaraDataInterfaceMeshParticleizerBudget.h"
#include "NiagaraDataInterfaceMeshParticleizerColor.h"
#include "NiagaraDataInterfaceMeshParticleizerCoverage.h"
#include "NiagaraDataSet.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"

FString UMeshParticleizerPreparationLibrary::InspectPreparedTextureReadiness(UMeshParticleizerPreparedMesh* Prepared)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("game_frame"), GFrameCounter);
	auto Rows = TArray<TSharedPtr<FJsonValue>>{};
	auto Unloaded = 0;
	if (Prepared != nullptr)
	{
		for (const auto& Entry : Prepared->Resources)
		{
			auto* Asset = Entry.Asset.Get();
			if (Asset == nullptr)
			{
				++Unloaded;
				continue;
			}
			const auto* Texture = Cast<UTexture>(Asset);
			if (Texture == nullptr)
			{
				continue;
			}

			// Render ThreadをFlushすると調査対象の初期化競合を隠すため、公開されたCPU状態だけ読む。
			const auto* Resource = Texture->GetResource();
			const auto& State = Texture->GetStreamableResourceState();
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset"), Texture->GetPathName());
			Row->SetBoolField(TEXT("initialized"), Resource != nullptr && Resource->IsInitialized() == true);
			Row->SetBoolField(TEXT("default_resource"), Texture->IsDefaultTexture());
			Row->SetBoolField(TEXT("pending_init_or_streaming"), Texture->HasPendingInitOrStreaming());
			Row->SetNumberField(TEXT("resource_width"), Resource != nullptr ? Resource->GetSizeX() : 0);
			Row->SetNumberField(TEXT("resident_mips"), State.NumResidentLODs);
			Row->SetNumberField(TEXT("max_mips"), State.MaxNumLODs);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	Result->SetNumberField(TEXT("unloaded_assets"), Unloaded);
	Result->SetArrayField(TEXT("textures"), Rows);
	auto Output = FString{};
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Output));
	return Output;
}

TArray<float> UMeshParticleizerPreparationLibrary::ReadRuntimeCoverage(UNiagaraComponent* Particles)
{
	auto Result = TArray<float>{};
	const auto Controller = Particles != nullptr ? Particles->GetSystemInstanceController() : nullptr;
	if (Controller.IsValid() == false)
	{
		return Result;
	}
	Controller->WaitForConcurrentTickAndFinalize();
	auto* Instance = Controller->GetSystemInstance_Unsafe();
	if (Instance == nullptr)
	{
		return Result;
	}

	auto Proxies = TSet<FNiagaraDataInterfaceProxy*>{};
	for (const auto& Emitter : Instance->GetEmitters())
	{
		auto* Context = Emitter->GetGPUContext();
		if (Context == nullptr)
		{
			continue;
		}
		for (auto* Interface : Context->GetDataInterfaces())
		{
			if (auto* Coverage = Cast<UNiagaraDataInterfaceMeshParticleizerCoverage>(Interface))
			{
				Proxies.Add(Coverage->GetProxy());
			}
		}
	}
	if (Proxies.Num() != 1)
	{
		return Result;
	}
	auto* Proxy = *Proxies.CreateConstIterator();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerReadCoverage)([Proxy, Id = Instance->GetId(), &Result](FRHICommandListImmediate& RHICmdList) {
		UNiagaraDataInterfaceMeshParticleizerCoverage::ReadProxyCoverage(RHICmdList, Proxy, Id, Result);
	});
	FlushRenderingCommands();
	return Result;
}

FString UMeshParticleizerPreparationLibrary::InspectSimulationMemory(UNiagaraComponent* Particles)
{
	auto Controller = Particles != nullptr ? Particles->GetSystemInstanceController() : nullptr;
	if (Controller.IsValid() == false)
	{
		return TEXT("A live Niagara controller is required.");
	}
	Controller->WaitForConcurrentTickAndFinalize();
	auto* Instance = Controller->GetSystemInstance_Unsafe();
	if (Instance == nullptr)
	{
		return TEXT("The system instance is unavailable.");
	}

	auto Contexts = TArray<FNiagaraComputeExecutionContext*>{};
	auto Arrays = TSet<INDIArrayProxyBase*>{};
	auto Budgets = TSet<FNiagaraDataInterfaceProxy*>{};
	auto Colors = TSet<FNiagaraDataInterfaceProxy*>{};
	auto Emissions = TSet<FNiagaraDataInterfaceProxy*>{};
	auto Coverages = TSet<FNiagaraDataInterfaceProxy*>{};
	for (const auto& Emitter : Instance->GetEmitters())
	{
		auto* Context = Emitter->GetGPUContext();
		if (Context == nullptr)
		{
			continue;
		}
		Contexts.Add(Context);
		for (auto* Interface : Context->GetDataInterfaces())
		{
			if (auto* Budget = Cast<UNiagaraDataInterfaceMeshParticleizerBudget>(Interface))
			{
				Budgets.Add(Budget->GetProxy());
			}
			if (auto* Coverage = Cast<UNiagaraDataInterfaceMeshParticleizerCoverage>(Interface))
			{
				Coverages.Add(Coverage->GetProxy());
			}
			if (auto* Color = Cast<UNiagaraDataInterfaceMeshParticleizerColor>(Interface))
			{
				(Color->IsEmission() ? Emissions : Colors).Add(Color->GetProxy());
			}
			if (auto* Array = Cast<UNiagaraDataInterfaceArray>(Interface))
			{
				if (Array->IsUsedWithGPUScript() == true)
				{
					Arrays.Add(static_cast<INDIArrayProxyBase*>(Array->GetProxy()));
				}
			}
		}
	}
	if (Contexts.IsEmpty())
	{
		return TEXT("A running GPU emitter is required.");
	}

	// この関数内で同期完了するため、UObjectを描画コマンドへ渡す必要はない。
	auto DataBytes = int64{0};
	auto ArrayBytes = int64{0};
	auto BudgetBytes = int64{0};
	auto BudgetIdentity = uint64{0};
	auto ColorBytes = int64{0};
	auto ColorIdentity = uint64{0};
	auto EmissionBytes = int64{0};
	auto EmissionIdentity = uint64{0};
	auto CoverageBytes = int64{0};
	auto CoverageIdentity = uint64{0};
	auto CoverageIndexBytes = int64{0};
	auto CoverageHeadsIdentity = uint64{0};
	auto CoverageLinksIdentity = uint64{0};
	auto CoverageViewCount = uint32{0};
	auto BufferIdentities = FString{};
	const auto ID = Instance->GetId();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerInspectMemory)(
		[Contexts, Arrays, Budgets, Colors, Emissions, Coverages, ID, &DataBytes, &ArrayBytes, &BudgetBytes, &BudgetIdentity,
			&ColorBytes, &ColorIdentity, &EmissionBytes, &EmissionIdentity, &CoverageBytes, &CoverageIdentity, &CoverageIndexBytes, &CoverageHeadsIdentity, &CoverageLinksIdentity, &CoverageViewCount, &BufferIdentities](FRHICommandListImmediate& RHICmdList) {
			auto Buffers = TSet<FRHIBuffer*>{};
			for (auto* Context : Contexts)
			{
				DataBytes += Context->MainDataSet->GetSizeBytes();

				// 更新用2面に加え、現在の描画面も含める。面の入替えを再確保と混同しない。
				for (auto* Data : {Context->DataBuffers_RT[0], Context->DataBuffers_RT[1], Context->MainDataSet->GetCurrentData()})
				{
					if (Data == nullptr)
					{
						continue;
					}
					for (auto* Buffer : {Data->GetGPUBufferFloat().Buffer.GetReference(), Data->GetGPUBufferInt().Buffer.GetReference(), Data->GetGPUBufferHalf().Buffer.GetReference()})
					{
						if (Buffer != nullptr)
						{
							Buffers.Add(Buffer);
						}
					}
				}
			}
			for (auto* Buffer : Buffers)
			{
				if (BufferIdentities.IsEmpty() == false)
				{
					BufferIdentities += TEXT(",");
				}
				BufferIdentities += FString::Printf(TEXT("\"%p\""), Buffer);
			}
			for (auto* Array : Arrays)
			{
				auto Parameters = INDIArrayProxyBase::FShaderParameters{};
				Array->SetShaderParameters(&Parameters, ID);
				if (Parameters.ArrayReadBuffer != nullptr)
				{
					ArrayBytes += Parameters.ArrayReadBuffer->GetBuffer()->GetSize();
				}
				else if (Parameters.ArrayRWBuffer != nullptr)
				{
					ArrayBytes += Parameters.ArrayRWBuffer->GetBuffer()->GetSize();
				}
			}
			for (auto* Budget : Budgets)
			{
				BudgetBytes += UNiagaraDataInterfaceMeshParticleizerBudget::GetProxyBufferBytes(Budget, ID, &BudgetIdentity);
			}
			for (auto* Color : Colors)
			{
				ColorBytes += UNiagaraDataInterfaceMeshParticleizerColor::GetProxyBufferBytes(Color, ID, &ColorIdentity);
			}
			for (auto* Emission : Emissions)
			{
				EmissionBytes += UNiagaraDataInterfaceMeshParticleizerColor::GetProxyBufferBytes(Emission, ID, &EmissionIdentity);
			}
			for (auto* Coverage : Coverages)
			{
				auto ViewCount = uint32{0};
				CoverageBytes += UNiagaraDataInterfaceMeshParticleizerCoverage::GetProxyBufferBytes(Coverage, ID, &CoverageIdentity, &ViewCount,
					&CoverageIndexBytes, &CoverageHeadsIdentity, &CoverageLinksIdentity);
				CoverageViewCount = FMath::Max(CoverageViewCount, ViewCount);
			}
		});
	FlushRenderingCommands();
	return FString::Printf(TEXT("{\"particle_buffer_bytes\":%lld,\"array_buffer_bytes\":%lld,\"budget_buffer_bytes\":%lld,\"color_buffer_bytes\":%lld,\"emission_buffer_bytes\":%lld,\"coverage_buffer_bytes\":%lld,\"coverage_index_bytes\":%lld,\"coverage_heads_buffer\":\"%llu\",\"coverage_links_buffer\":\"%llu\",\"coverage_view_count\":%u,\"total_bytes\":%lld,\"array_count\":%d,\"memory_tracking\":%s,\"particle_buffers\":[%s],\"budget_buffer\":\"%llu\",\"color_buffer\":\"%llu\",\"emission_buffer\":\"%llu\",\"coverage_buffer\":\"%llu\"}"),
		DataBytes, ArrayBytes, BudgetBytes, ColorBytes, EmissionBytes, CoverageBytes, CoverageIndexBytes, CoverageHeadsIdentity, CoverageLinksIdentity, CoverageViewCount, DataBytes + ArrayBytes + BudgetBytes + ColorBytes + EmissionBytes + CoverageBytes,
		Arrays.Num(), NIAGARA_MEMORY_TRACKING ? TEXT("true") : TEXT("false"), *BufferIdentities, BudgetIdentity, ColorIdentity, EmissionIdentity, CoverageIdentity);
}

FString UMeshParticleizerPreparationLibrary::InspectSharedGpuMemory(UObject* WorldContextObject, int32 AdditionalMeshInstances, int32 AdditionalMeshSections)
{
	auto* World = GEngine != nullptr ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (World == nullptr)
	{
		return TEXT("A live world is required.");
	}
	auto* Dispatch = FNiagaraGpuComputeDispatchInterface::Get(World);
	if (Dispatch == nullptr)
	{
		return TEXT("The world's Niagara GPU dispatcher is unavailable.");
	}

	// 公開APIから得られるワールド固有の値だけを読む。GPU粒子の読戻しや確保は行わない。
	auto CountBytes = int64{0};
	ENQUEUE_RENDER_COMMAND(MeshParticleizerInspectSharedMemory)(
		[Dispatch, &CountBytes](FRHICommandListImmediate& RHICmdList) {
			CountBytes = Dispatch->GetGPUInstanceCounterManager().GetInstanceCountBuffer().NumBytes;
		});
	FlushRenderingCommands();

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("world_instance_count_buffer_bytes"), CountBytes);
	Result->SetNumberField(TEXT("world_instance_count_capacity"), CountBytes / sizeof(uint32));
	if (auto* Manager = World->GetSubsystem<UMeshParticleizerWorldSubsystem>(); Manager != nullptr)
	{
		auto Draws = FMeshParticleizerDrawGpuSnapshot{};
		const auto Scene = Manager->GetSceneGpu()->Read(&Draws);
		auto SceneResult = MakeShared<FJsonObject>();
		SceneResult->SetBoolField(TEXT("ready"), Scene.Frame != 0);
		SceneResult->SetNumberField(TEXT("render_frame"), Scene.Frame);
		SceneResult->SetNumberField(TEXT("primitive_bytes"), Scene.PrimitiveBytes);
		SceneResult->SetNumberField(TEXT("instance_bytes"), Scene.InstanceBytes);
		SceneResult->SetNumberField(TEXT("payload_bytes"), Scene.PayloadBytes);
		SceneResult->SetNumberField(TEXT("retained_upload_bytes"), Scene.RetainedUploadBytes);
		SceneResult->SetNumberField(TEXT("committed_bytes"), Scene.GetCommittedBytes());
		SceneResult->SetNumberField(TEXT("virtual_bytes"), Scene.VirtualBytes);
		SceneResult->SetNumberField(TEXT("primitives"), Scene.Primitives);
		SceneResult->SetNumberField(TEXT("instances"), Scene.Instances);
		SceneResult->SetNumberField(TEXT("payload_float4s"), Scene.PayloadFloat4s);
		SceneResult->SetNumberField(TEXT("instance_stride"), Scene.InstanceStride);
		SceneResult->SetNumberField(TEXT("primitive_stride"), Scene.PrimitiveStride);
		SceneResult->SetNumberField(TEXT("mesh_payload_stride"), Scene.MeshPayloadStride);
		SceneResult->SetNumberField(TEXT("tile_size_log2"), Scene.TileSizeLog2);
		SceneResult->SetNumberField(TEXT("commit_alignment"), Scene.CommitAlignment);

		auto Projection = FMeshParticleizerSharedGpuMemory{};
		auto Error = FString{};
		const auto bValid = Projection.GrowScene(Scene, AdditionalMeshSections, AdditionalMeshInstances,
			int64{AdditionalMeshInstances} * Scene.MeshPayloadStride, Error, Draws.Views);
		SceneResult->SetBoolField(TEXT("projection_valid"), bValid);
		SceneResult->SetStringField(TEXT("projection_error"), Error);
		SceneResult->SetNumberField(TEXT("projected_primitive_bytes"), Projection.ScenePrimitiveBytes);
		SceneResult->SetNumberField(TEXT("projected_instance_bytes"), Projection.SceneInstanceBytes);
		SceneResult->SetNumberField(TEXT("projected_payload_bytes"), Projection.ScenePayloadBytes);
		SceneResult->SetNumberField(TEXT("projected_upload_bytes"), Projection.SceneUploadBytes);
		SceneResult->SetNumberField(TEXT("projected_reservation_bytes"), Projection.GetSceneReservationBytes());
		SceneResult->SetStringField(TEXT("scope"), TEXT("World GPU Scene primitive/instance/payload plus retained uploads and power-of-two primitive upload pools for the static update and primary views, with one retiring generation; excludes predicted CPU instance/payload uploads and upload allocator arenas."));
		Result->SetObjectField(TEXT("gpu_scene"), SceneResult);

		auto DrawResult = MakeShared<FJsonObject>();
		DrawResult->SetBoolField(TEXT("ready"), Draws.Frame != 0 && Draws.Frame == Scene.Frame);
		DrawResult->SetNumberField(TEXT("render_frame"), Draws.Frame);
		DrawResult->SetNumberField(TEXT("views"), Draws.Views);
		DrawResult->SetBoolField(TEXT("supported_layout"), Draws.bSupportedLayout);
		DrawResult->SetStringField(TEXT("scope"), TEXT("Main-view mesh-pass inputs and per-family SharedViewData (primary/culling views, persistent ID map, deformation mask) before RDG pool allocation. SharedViewData includes a conservative shadow-view capacity. Not physical VRAM."));
		auto DrawProjection = FMeshParticleizerSharedGpuMemory{};
		auto DrawError = FString{};
		DrawResult->SetBoolField(TEXT("pool_projection_valid"), DrawProjection.GrowDraw(Draws, DrawError));
		DrawResult->SetStringField(TEXT("pool_projection_error"), DrawError);
		DrawResult->SetNumberField(TEXT("pool_capacity_bound_bytes"), DrawProjection.DrawPoolBytes);
		DrawResult->SetNumberField(TEXT("pool_reservation_bytes"), DrawProjection.GetDrawReservationBytes());
		DrawResult->SetStringField(TEXT("pool_scope"), TEXT("Main-view buffers allow deferred merging/splitting by buffer role; shared-view buffers are separate. 64 KiB pages, up to 32 retained frames and one retiring RHI generation. GPU-only outputs use the non-transient pool bound. Deferred shading and non-immediate RDG only. Excludes shadow draw passes, upload heaps and uniforms. Not physical VRAM."));
		auto MakePassRows = [](const FMeshParticleizerDrawGpuSnapshot& Input) {
			auto PassRows = TArray<TSharedPtr<FJsonValue>>{};
			for (const auto& Pass : Input.Passes)
			{
				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("pass"), Pass.Name.ToString());
				Row->SetBoolField(TEXT("submitted"), Pass.bSubmitted);
				Row->SetNumberField(TEXT("views"), Pass.Views);
				Row->SetNumberField(TEXT("instances"), Pass.Instances);
				Row->SetNumberField(TEXT("draws"), Pass.Draws);
				Row->SetNumberField(TEXT("instance_id_bytes"), Pass.InstanceIdBytes);
				Row->SetNumberField(TEXT("command_bytes"), Pass.CommandBytes);
				Row->SetNumberField(TEXT("load_balancer_bytes"), Pass.LoadBalancerBytes);
				Row->SetNumberField(TEXT("compaction_bytes"), Pass.CompactionBytes);
				auto PoolBuffers = TArray<TSharedPtr<FJsonValue>>{};
				for (const auto Bytes : Pass.PooledBufferBytes)
				{
					PoolBuffers.Add(MakeShared<FJsonValueNumber>(Bytes));
				}
				Row->SetArrayField(TEXT("pooled_buffer_requests"), PoolBuffers);
				Row->SetNumberField(TEXT("gpu_written_bytes"), Pass.GpuWrittenBytes);
				PassRows.Add(MakeShared<FJsonValueObject>(Row));
			}
			return PassRows;
		};
		DrawResult->SetArrayField(TEXT("passes"), MakePassRows(Draws));

		// 通常の無影・不透明メッシュが使う主passを、生成前の入力へ重ねて予測する。
		auto PredictedDraws = Draws;
		auto PredictedMemory = FMeshParticleizerSharedGpuMemory{};
		auto PredictionError = FString{};
		const FName MeshPasses[] = {TEXT("DepthPass"), TEXT("BasePass"), TEXT("Velocity")};
		const auto bPredicted = PredictedDraws.AddMeshDemand(AdditionalMeshInstances, AdditionalMeshSections, MeshPasses, PredictionError) &&
			PredictedMemory.GrowDraw(PredictedDraws, PredictionError);
		auto PredictedResult = MakeShared<FJsonObject>();
		PredictedResult->SetBoolField(TEXT("valid"), bPredicted);
		PredictedResult->SetStringField(TEXT("error"), PredictionError);
		PredictedResult->SetNumberField(TEXT("instances_per_view"), AdditionalMeshInstances);
		PredictedResult->SetNumberField(TEXT("sections_per_view"), AdditionalMeshSections);
		PredictedResult->SetNumberField(TEXT("pool_capacity_bound_bytes"), PredictedMemory.DrawPoolBytes);
		PredictedResult->SetNumberField(TEXT("pool_reservation_bytes"), PredictedMemory.GetDrawReservationBytes());
		PredictedResult->SetArrayField(TEXT("passes"), MakePassRows(PredictedDraws));
		PredictedResult->SetStringField(TEXT("scope"), TEXT("Prospective unsorted raster mesh input for DepthPass, BasePass and Velocity, including absent passes/views and shared deformation mask; diagnostic only, not runtime mesh admission."));
		DrawResult->SetObjectField(TEXT("mesh_projection"), PredictedResult);
		Result->SetObjectField(TEXT("draw_inputs"), DrawResult);
	}
	Result->SetStringField(TEXT("resource_scope"), TEXT("process_including_other_worlds_and_pending_deletes"));
	Result->SetStringField(TEXT("resource_size"), TEXT("RHI-reported allocation bytes, not per-simulation ownership"));
	Result->SetStringField(TEXT("resource_coverage"), TEXT("Named tracked RHI resources; readback staging and uniform/upload allocator storage may be absent"));

	// RHI一覧はプロセス全体。粒子別のtotal_bytesへ足すと共有分を重複計上するため、別表にする。
	auto Resources = TArray<TSharedPtr<FRHIResourceStats>>{};
	RHIGetTrackedResourceStats(Resources);
	Result->SetBoolField(TEXT("resource_tracking_available"), Resources.IsEmpty() == false);
	auto Rows = TArray<TSharedPtr<FJsonValue>>{};
	auto TotalBytes = uint64{0};
	for (const auto& Resource : Resources)
	{
		const auto Name = Resource->Name.ToString();
		if (Name.Contains(TEXT("Niagara")) == false && Name.Contains(TEXT("MeshParticleizer")) == false && Name.Contains(TEXT("GPUSort")) == false && Name.Contains(TEXT("ParticleSort")) == false && Name.Contains(TEXT("DistanceField")) == false && Name.Contains(TEXT("GPUScene")) == false && Name.Contains(TEXT("InstanceCulling")) == false)
		{
			continue;
		}

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Name);
		Row->SetStringField(TEXT("owner"), Resource->OwnerName.ToString());
		Row->SetStringField(TEXT("type"), Resource->Type);
		Row->SetNumberField(TEXT("bytes"), Resource->SizeInBytes);
		Row->SetBoolField(TEXT("pending_delete"), Resource->bMarkedForDelete);
		Row->SetBoolField(TEXT("transient"), Resource->bTransient);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
		TotalBytes += Resource->SizeInBytes;
	}
	Result->SetArrayField(TEXT("resources"), Rows);
	Result->SetNumberField(TEXT("process_filtered_allocation_bytes"), TotalBytes);

	auto Settings = MakeShared<FJsonObject>();
	const TCHAR* Names[] = {TEXT("Niagara.MinGPUInstanceCount"), TEXT("Niagara.GPUCountBufferSlack"), TEXT("Niagara.GPUCountManager.AllocateIncrement"), TEXT("Niagara.MinCulledGPUInstanceCount"), TEXT("fx.Niagara.IndirectArgsPool.MinSize"), TEXT("fx.Niagara.IndirectArgsPool.BlockSizeFactor"), TEXT("fx.Niagara.IndirectArgsPool.AllowShrinking"), TEXT("fx.Niagara.IndirectArgsPool.LowWaterFrames"), TEXT("fx.NiagaraGPUDataBufferChunkSize"), TEXT("fx.Niagara.Batcher.TickFlush.MaxPendingTicks")};
	for (const auto* Name : Names)
	{
		if (const auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Settings->SetNumberField(Name, Variable->GetFloat());
		}
	}
	Result->SetObjectField(TEXT("allocation_settings"), Settings);
	auto Output = FString{};
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Output));
	return Output;
}
