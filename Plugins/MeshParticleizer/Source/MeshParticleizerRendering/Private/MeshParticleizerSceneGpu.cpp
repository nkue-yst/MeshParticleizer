#include "MeshParticleizerSceneGpu.h"
#include "SceneViewExtension.h"
#include "ScenePrivate.h"
#include "InstanceUniformShaderParameters.h"
#include "PrimitiveSceneShaderData.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ShadowRendering.h"
#include "MeshDrawCommands.h"
#include "InstanceCulling/InstanceCullingLoadBalancer.h"
#include "InstanceCulling/InstanceCullingMergedContext.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

namespace MeshParticleizerSceneGpu
{
	class FObserver final : public FSceneViewExtensionBase
	{
	public:
		FObserver(const FAutoRegister& AutoRegister, FSceneInterface* InScene,
			const TSharedRef<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe>& InState)
			: FSceneViewExtensionBase(AutoRegister), Scene(InScene), State(InState)
		{
		}

		virtual void SetupViewFamily(FSceneViewFamily& ViewFamily) override
		{
		}
		virtual void SetupView(FSceneViewFamily& ViewFamily, FSceneView& View) override
		{
		}
		virtual void BeginRenderViewFamily(FSceneViewFamily& ViewFamily) override
		{
		}

		virtual void PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& View) override
		{
			if (this->Scene == nullptr || View.Family == nullptr || View.Family->Scene != this->Scene ||
				View.bIsViewInfo == false || this->State.IsValid() == false)
			{
				return;
			}
			const auto* ViewInfo = static_cast<const FViewInfo*>(&View);

			// 描画setupはGraph実行前に完了する。Viewは実行完了まで有効で、ここから待機しない。
			GraphBuilder.AddPostExecuteCallback([ViewInfo, WeakState = this->State]() {
				const auto Shared = WeakState.Pin();
				if (Shared.IsValid() == false)
				{
					return;
				}
					auto Passes = TArray<FMeshParticleizerDrawGpuPass>{};
					auto bSupported = true;
					for (auto PassIndex = 0; PassIndex < EMeshPass::Num; ++PassIndex)
				{
						auto* Pass = ViewInfo->ParallelMeshDrawCommandPasses[PassIndex];
					if (Pass == nullptr || Pass->HasAnyDraw() == false)
					{
						continue;
					}
						const auto* Context = Pass->GetInstanceCullingContext();
					if (Context->bIsEnabled == false)
					{
						continue;
					}
						auto Item = FMeshParticleizerDrawGpuPass{};
						Item.Name = GetMeshPassName(static_cast<EMeshPass::Type>(PassIndex));
						// CPU setup済みでも、使われなかったpassには未実行の描画準備が残る。
						if (static_cast<bool>(Context->SyncPrerequisitesFunc) == true)
					{
							Passes.Add(MoveTemp(Item));
							continue;
					}
						if (Context->bUsesUniformBufferView == true)
					{
							bSupported = false;
							continue;
					}

						Item.bSubmitted = true;
						Item.Views = Context->ViewIds.Num();
						Item.Instances = Context->TotalInstances;
						Item.Draws = Context->IndirectArgs.Num();
						Item.InstanceIdBytes = Item.Instances * Item.Views * sizeof(uint32);
						Item.CommandBytes = int64{Context->IndirectArgs.Num()} * sizeof(FRHIDrawIndexedIndirectParameters) +
							int64{Context->DrawCommandDescs.Num()} * sizeof(FUintVector2) +
							int64{Context->InstanceIdOffsets.Num()} * sizeof(uint32) +
							int64{Context->PayloadData.Num()} * sizeof(FInstanceCullingContext::FPayloadData) +
							int64{Item.Views} * sizeof(uint32);

						// CPU uploadをqueueしたbufferはRDGの64KiB poolへ入る。空の配列も1要素分を持つ。
					auto AddPooled = [&Item](int64 Count, int64 Stride) {
							Item.PooledBufferBytes.Add(FMath::Max<int64>(1, Count) * Stride);
						};
						AddPooled(Context->IndirectArgs.Num(), sizeof(FRHIDrawIndexedIndirectParameters));
						AddPooled(Context->DrawCommandDescs.Num(), sizeof(FUintVector2));
						AddPooled(Context->InstanceIdOffsets.Num(), sizeof(uint32));
						AddPooled(Context->PayloadData.Num(), sizeof(FInstanceCullingContext::FPayloadData));
						AddPooled(Item.Views, sizeof(uint32));
						AddPooled(1, sizeof(FInstanceCullingMergedContext::FContextBatchInfoPacked));
						for (const auto* Balancer : Context->LoadBalancers)
					{
							// 未使用のmodeも同じslotへ最小容量を置き、画面状態でbufferの役割をずらさない。
							const auto Batches = Balancer != nullptr ? Balancer->GetBatches().Num() : 0;
							const auto Items = Balancer != nullptr ? Balancer->GetItems().Num() : 0;
							Item.LoadBalancerBytes += int64{Batches} * sizeof(FInstanceCullingLoadBalancerBase::FPackedBatch) +
								int64{Items} * sizeof(FInstanceCullingLoadBalancerBase::FPackedItem);
							AddPooled(Batches, sizeof(FInstanceCullingLoadBalancerBase::FPackedBatch));
							AddPooled(Items, sizeof(FInstanceCullingLoadBalancerBase::FPackedItem));
							AddPooled(Batches, sizeof(uint32));
					}
						// 順序保持の一時ID、block count・offset・draw index、drawごとのcompaction情報。
						Item.CompactionBytes = int64{Context->NumCompactionInstances} * sizeof(uint32) +
							int64{Context->CompactionBlockDataIndices.Num()} * sizeof(uint32) * 3 +
							int64{Context->DrawCommandCompactionData.Num()} * sizeof(FInstanceCullingContext::FCompactionData);
						// deferred経路はcompactionが0件でも最小bufferを作る。GPU専用IDは別枠とする。
						AddPooled(Context->DrawCommandCompactionData.Num(), sizeof(FInstanceCullingContext::FCompactionData));
						AddPooled(Context->NumCompactionInstances, sizeof(uint32));
						AddPooled(Context->CompactionBlockDataIndices.Num(), sizeof(uint32));
						AddPooled(Context->CompactionBlockDataIndices.Num(), sizeof(uint32));
						AddPooled(Context->CompactionBlockDataIndices.Num(), sizeof(uint32));
						Item.GpuWrittenBytes = FMath::Max<int64>(sizeof(uint32), Item.InstanceIdBytes);
						Passes.Add(MoveTemp(Item));
				}
					Shared->PublishDraws(GFrameNumberRenderThread, bSupported, MoveTemp(Passes));
				});
		}

		virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& ViewFamily) override
		{
			if (this->Scene == nullptr || ViewFamily.Scene != this->Scene || this->State.IsValid() == false)
			{
				return;
			}
			auto* RenderScene = this->Scene->GetRenderScene();
			if (RenderScene == nullptr)
			{
				return;
			}

			// 主Viewとshadowの共通入力はfamilyにつき1組。即時RDGは途中で再uploadするため別の寿命になる。
			auto* Renderer = static_cast<FSceneRendererBase*>(ViewFamily.GetSceneRenderer())->GetDeferredShadingSceneRenderer();
			const auto bSupportedViews = Renderer != nullptr && FRDGBuilder::IsImmediateMode() == false;
			GraphBuilder.AddPostExecuteCallback([Renderer, RenderScene, bSupportedViews, WeakState = this->State]() {
				const auto Shared = WeakState.Pin();
				if (Shared.IsValid() == false)
				{
					return;
				}
					auto Buffers = TArray<FMeshParticleizerDrawGpuPass>{};
					if (bSupportedViews == true)
				{
						// ViewDataはprimaryを確保後、shadow枠の追加時に既存primary分も追加する。
						// VSMはnon-Nanite経路を使わない場合も含め、shadow枠の上界を取る。
						auto ShadowViews = int64{0};
					auto CountShadows = [&ShadowViews](const auto& Shadows) {
							for (const auto* Shadow : Shadows)
						{
							if (Shadow != nullptr)
							{
								ShadowViews += Shadow->bOnePassPointLightShadow ? 6 : 1;
							}
						}
						};
						const auto& Sorted = Renderer->SortedShadowsForShadowDepthPass;
					for (const auto& Atlas : Sorted.ShadowMapAtlases)
					{
						CountShadows(Atlas.Shadows);
					}
					for (const auto& Atlas : Sorted.ShadowMapCubemaps)
					{
						CountShadows(Atlas.Shadows);
					}
						CountShadows(Sorted.PreshadowCache.Shadows);
					for (const auto& Atlas : Sorted.TranslucencyShadowMapAtlases)
					{
						CountShadows(Atlas.Shadows);
					}
						for (const auto* Shadow : Sorted.VirtualShadowMapShadows)
					{
							if (Shadow != nullptr && Shadow->bShouldRenderVSM == true)
						{
								ShadowViews += Shadow->bOnePassPointLightShadow ? 6 : 1;
						}
					}

						auto Item = FMeshParticleizerDrawGpuPass{};
						Item.Name = TEXT("SharedViewData");
						Item.bSubmitted = true;
						Item.Views = Renderer->AllViews.Num();
						Item.Instances = RenderScene->GPUScene.GetNumInstances();
						const auto PrimaryViews = FMath::Max<int64>(1, Item.Views);
						const auto CullingViews = ShadowViews > 0 ? PrimaryViews * 2 + ShadowViews : PrimaryViews;
						Item.PooledBufferBytes = {PrimaryViews * int64{sizeof(Nanite::FPackedView)}, CullingViews * int64{sizeof(Nanite::FPackedView)},
							FMath::Max<int64>(1, RenderScene->GetMaxPersistentViewId()) * int64{sizeof(uint32)}};
						Item.GpuWrittenBytes = FMath::Max<int64>(1, FMath::DivideAndRoundUp(Item.Instances, int64{32}) * PrimaryViews) * sizeof(uint32);
						Buffers.Add(MoveTemp(Item));
				}
					Shared->PublishDraws(GFrameNumberRenderThread, bSupportedViews, MoveTemp(Buffers), 0);
				});

			// RDGによるコミット更新の後に読む。SceneはこのGraphの実行完了まで有効。
			GraphBuilder.AddPostExecuteCallback([RenderScene, FamilyViews = ViewFamily.Views.Num(), WeakState = this->State]() {
				const auto Shared = WeakState.Pin();
				if (Shared.IsValid() == false)
				{
					return;
				}
					const auto& GPUScene = RenderScene->GPUScene;
					if (GPUScene.PrimitiveBuffer.IsValid() == false || GPUScene.InstanceSceneDataBuffer.IsValid() == false ||
					GPUScene.InstancePayloadDataBuffer.IsValid() == false)
				{
					return;
				}

					auto Snapshot = FMeshParticleizerSceneGpuSnapshot{};
					Snapshot.Frame = GFrameNumberRenderThread;
				const auto CommittedBytes = [](const FRDGPooledBuffer* Buffer) {
						// RDGは要求bytesを保持するが、RHIの予約資源はタイル単位でコミットする。
						return EnumHasAnyFlags(Buffer->Desc.Usage, EBufferUsageFlags::ReservedResource)
							? Align(Buffer->GetCommittedSize(), uint64{GRHIGlobals.ReservedResources.TileSizeInBytes})
							: uint64{Buffer->GetAlignedSize()};
					};
					Snapshot.PrimitiveBytes = CommittedBytes(GPUScene.PrimitiveBuffer);
					Snapshot.InstanceBytes = CommittedBytes(GPUScene.InstanceSceneDataBuffer);
					Snapshot.PayloadBytes = CommittedBytes(GPUScene.InstancePayloadDataBuffer);
					Snapshot.RetainedUploadBytes = int64{GPUScene.PrimitiveUploadBuffer.GetNumBytes()} +
						GPUScene.InstanceSceneUploadBuffer.GetNumBytes() + GPUScene.InstancePayloadUploadBuffer.GetNumBytes() +
						GPUScene.LightmapUploadBuffer.GetNumBytes();
					Snapshot.VirtualBytes = int64{GPUScene.PrimitiveBuffer->GetSize()} + GPUScene.InstanceSceneDataBuffer->GetSize() +
						GPUScene.InstancePayloadDataBuffer->GetSize();
					Snapshot.Primitives = GPUScene.GetNumPrimitives();
					Snapshot.Instances = GPUScene.GetNumInstances();
					Snapshot.PayloadFloat4s = GPUScene.InstancePayloadDataAllocator.GetMaxSize();
					Snapshot.PrimitiveStride = sizeof(FPrimitiveSceneShaderData::Data);
					Snapshot.InstanceStride = FInstanceSceneShaderData::GetEffectiveNumBytes();
					auto Payload = FMeshBatchDynamicPrimitiveData{};
					Payload.EnableInstanceDynamicData(true);
					Payload.SetNumInstanceCustomDataFloats(1);
					Snapshot.MeshPayloadStride = Payload.GetPayloadFloat4Stride();
					static const auto* TileSize = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUScene.InstanceDataTileSizeLog2"));
					Snapshot.TileSizeLog2 = GRHIGlobals.ReservedResources.Supported && TileSize != nullptr ? TileSize->GetInt() : -1;
					Snapshot.CommitAlignment = GRHIGlobals.ReservedResources.Supported ? GRHIGlobals.ReservedResources.TileSizeInBytes : 16;
					Shared->Publish(Snapshot, FamilyViews);
				});
		}

	private:
		FSceneInterface* Scene = nullptr;
		TWeakPtr<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe> State;
	};
}

bool FMeshParticleizerDrawGpuSnapshot::AddMeshDemand(int64 InstancesPerView, int64 SectionsPerView,
	TConstArrayView<FName> MeshPasses, FString& OutError)
{
	OutError.Reset();
	if (this->Frame == 0 || this->Views <= 0 || this->bSupportedLayout == false || InstancesPerView < 0 ||
		SectionsPerView < 0 || SectionsPerView > InstancesPerView || InstancesPerView > MAX_INSTANCE_ID / this->Views ||
		(MeshPasses.IsEmpty() == false && SectionsPerView == 0) ||
		(MeshPasses.IsEmpty() == true && SectionsPerView != 0))
	{
		OutError = TEXT("Mesh draw prediction requires a supported rendered snapshot and bounded per-view instance/section counts.");
		return false;
	}
	if (InstancesPerView == 0)
	{
		return true;
	}

	auto Names = TSet<FName>{};
	for (const auto Name : MeshPasses)
	{
		if (Name.IsNone() == true || Name == TEXT("SharedViewData") || Names.Contains(Name) == true)
		{
			OutError = TEXT("Mesh draw prediction requires unique mesh-pass names.");
			return false;
		}
		Names.Add(Name);
	}
	auto Predicted = this->Passes;
	const int64 Minimums[] = {sizeof(FRHIDrawIndexedIndirectParameters), sizeof(FUintVector2), sizeof(uint32),
		sizeof(FInstanceCullingContext::FPayloadData), sizeof(uint32), sizeof(FInstanceCullingMergedContext::FContextBatchInfoPacked),
		sizeof(FInstanceCullingLoadBalancerBase::FPackedBatch), sizeof(FInstanceCullingLoadBalancerBase::FPackedItem), sizeof(uint32),
		sizeof(FInstanceCullingLoadBalancerBase::FPackedBatch), sizeof(FInstanceCullingLoadBalancerBase::FPackedItem), sizeof(uint32),
		sizeof(FInstanceCullingContext::FCompactionData), sizeof(uint32), sizeof(uint32), sizeof(uint32), sizeof(uint32)};
	// 64件境界と各sectionの切れ目を含める。個々の粒子をCPUで列挙しない。
	const auto Groups = FMath::DivideAndRoundUp(InstancesPerView, int64{FInstanceCullingLoadBalancerBase::ThreadGroupSize}) + SectionsPerView;
	const int64 Additions[] = {SectionsPerView * Minimums[0], SectionsPerView * Minimums[1], SectionsPerView * Minimums[2],
		0, 0, 0, Groups * Minimums[6], Groups * Minimums[7], Groups * Minimums[8], 0, 0, 0, 0, 0, 0, 0, 0};
	static_assert(EBatchProcessingMode::Generic == EBatchProcessingMode{0});
	auto AddToPass = [&](FMeshParticleizerDrawGpuPass& Pass) {
		if (Pass.bSubmitted == false)
		{
			Pass = FMeshParticleizerDrawGpuPass{Pass.Name};
			Pass.bSubmitted = true;
			Pass.Views = 1;
			Pass.PooledBufferBytes.Append(Minimums, UE_ARRAY_COUNT(Minimums));
			Pass.GpuWrittenBytes = sizeof(uint32);
		}
		if (Pass.Views <= 0 || Pass.Views > this->Views || Pass.PooledBufferBytes.Num() != UE_ARRAY_COUNT(Minimums) ||
			Pass.Instances < 0 || Pass.Instances > MAX_INSTANCE_ID - InstancesPerView || Pass.Draws < 0 ||
			Pass.Draws > MAX_uint32 / Minimums[0] - SectionsPerView)
		{
			return false;
		}
		for (auto Index = 0; Index < UE_ARRAY_COUNT(Minimums); ++Index)
		{
			if (Pass.PooledBufferBytes[Index] < 0 || Pass.PooledBufferBytes[Index] > MAX_uint32 - Additions[Index])
			{
				return false;
			}
			Pass.PooledBufferBytes[Index] += Additions[Index];
		}
		const auto IdBytes = InstancesPerView * Pass.Views * int64{sizeof(uint32)};
		if (Pass.GpuWrittenBytes < 0 || Pass.GpuWrittenBytes > MAX_uint32 - IdBytes)
		{
			return false;
		}
		Pass.GpuWrittenBytes += IdBytes;
		Pass.Instances += InstancesPerView;
		Pass.Draws += SectionsPerView;
		Pass.InstanceIdBytes += IdBytes;
		Pass.CommandBytes += Additions[0] + Additions[1] + Additions[2];
		Pass.LoadBalancerBytes += Additions[6] + Additions[7];
		return true;
	};

	// passがまだ存在しないViewにも最小bufferから確保する。既存passは加算し、別枠に分けない。
	for (const auto Name : MeshPasses)
	{
		auto Count = 0;
		for (auto& Pass : Predicted)
		{
			if (Pass.Name != Name)
			{
				continue;
			}
			if (AddToPass(Pass) == false)
			{
				OutError = TEXT("Observed mesh-pass layout or projected buffer size is unsupported.");
				return false;
			}
			++Count;
		}
		while (Count++ < this->Views)
		{
			auto& Pass = Predicted.AddDefaulted_GetRef();
			Pass.Name = Name;
			if (AddToPass(Pass) == false)
			{
				return false;
			}
		}
	}

	// 動的primitiveは主Viewごとに別登録される。family共通の変形maskにもその合計を反映する。
	auto SharedViews = 0;
	for (auto& Pass : Predicted)
	{
		if (Pass.Name != TEXT("SharedViewData"))
		{
			continue;
		}
		const auto AdditionalInstances = InstancesPerView * this->Views;
		if (Pass.bSubmitted == false || Pass.Views <= 0 || Pass.Views > this->Views || Pass.Instances < 0 ||
			Pass.Instances > MAX_INSTANCE_ID - AdditionalInstances || Pass.PooledBufferBytes.Num() != 3)
		{
			OutError = TEXT("Shared view data cannot accommodate the projected dynamic instances.");
			return false;
		}
		Pass.Instances += AdditionalInstances;
		Pass.GpuWrittenBytes = FMath::Max(Pass.GpuWrittenBytes,
			FMath::DivideAndRoundUp(Pass.Instances, int64{32}) * Pass.Views * int64{sizeof(uint32)});
		++SharedViews;
	}
	if (SharedViews == 0)
	{
		OutError = TEXT("Mesh draw prediction is waiting for shared view data.");
		return false;
	}
	this->Passes = MoveTemp(Predicted);
	return true;
}

TSharedRef<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe> FMeshParticleizerSceneGpu::Observe(FSceneInterface* Scene)
{
	auto State = MakeShared<FMeshParticleizerSceneGpu, ESPMode::ThreadSafe>();
	State->Observer = FSceneViewExtensions::NewExtension<MeshParticleizerSceneGpu::FObserver>(Scene, State);
	return State;
}

FMeshParticleizerSceneGpuSnapshot FMeshParticleizerSceneGpu::Read(FMeshParticleizerDrawGpuSnapshot* OutDraws) const
{
	const auto Lock = FScopeLock(&this->Mutex);
	if (OutDraws != nullptr)
	{
		*OutDraws = this->DrawSnapshot;
	}
	return this->Snapshot;
}

void FMeshParticleizerSceneGpu::Publish(const FMeshParticleizerSceneGpuSnapshot& Value, int32 FamilyViews)
{
	const auto Lock = FScopeLock(&this->Mutex);
	if (Value.Frame == 0 || FamilyViews <= 0)
	{
		return;
	}

	// UEはfamily情報を先に通知する。複数familyの主View数を同じframeで合算する。
	if (this->PendingSnapshot.Frame != Value.Frame)
	{
		this->ExpectedViews = 0;
	}
	this->PendingSnapshot = Value;
	this->ExpectedViews += FamilyViews;
	this->PublishIfComplete();
}

void FMeshParticleizerSceneGpu::PublishDraws(uint64 Frame, bool bSupportedLayout, TArray<FMeshParticleizerDrawGpuPass>&& Passes, int32 RenderedViews)
{
	const auto Lock = FScopeLock(&this->Mutex);
	if (this->PendingDrawSnapshot.Frame != Frame)
	{
		this->PendingDrawSnapshot = FMeshParticleizerDrawGpuSnapshot{};
		this->PendingDrawSnapshot.Frame = Frame;
	}
	this->PendingDrawSnapshot.Views += RenderedViews;
	this->PendingDrawSnapshot.bSupportedLayout &= bSupportedLayout;
	this->PendingDrawSnapshot.Passes.Append(MoveTemp(Passes));

	// family共通bufferだけの追加では公開せず、必要な主Viewが揃うまで待つ。
	if (RenderedViews > 0)
	{
		this->PublishIfComplete();
	}
}

void FMeshParticleizerSceneGpu::PublishIfComplete()
{
	if (this->PendingSnapshot.Frame == 0 || this->PendingSnapshot.Frame != this->PendingDrawSnapshot.Frame ||
		this->ExpectedViews <= 0 || this->PendingDrawSnapshot.Views != this->ExpectedViews)
	{
		return;
	}

	// 収集中は前回の完成した組を公開し続け、Sceneと描画費用を同時に切り替える。
	this->Snapshot = this->PendingSnapshot;
	this->DrawSnapshot = this->PendingDrawSnapshot;
}
