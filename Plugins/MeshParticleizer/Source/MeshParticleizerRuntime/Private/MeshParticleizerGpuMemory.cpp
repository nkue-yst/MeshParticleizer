#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerCoverageGpu.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraDataInterfaceArrayFloat.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraMeshVertexFactory.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GenerateMips.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "MeshParticleizerSceneGpu.h"
#include "SceneDefinitions.h"

bool MeshParticleizerAssetGpuBytes(const UObject* Asset, int64& OutBytes, FString& OutError)
{
	OutError.Reset();
	if (const auto* Mesh = Cast<UStaticMesh>(Asset); Mesh != nullptr)
	{
		// NaniteとBLASは別の共有プールを使う。未計上の資源を0 bytesとして受理しない。
		if (Mesh->IsCompiling() == true || Mesh->GetRenderData() == nullptr || Mesh->GetRenderData()->LODResources.IsEmpty() ||
			Mesh->HasValidNaniteData() == true || Mesh->bSupportRayTracing == true)
		{
			OutError = TEXT("Particle mesh capacity requires compiled raster LODs with Nanite and Support Ray Tracing disabled.");
			return false;
		}

		auto Bytes = int64{0};
		auto IndexBytes = [](const FRawStaticIndexBuffer& Buffer) {
			// Cook後はCPU配列を解放しても、インデックス数と型のメタデータは残る。
			return int64{Buffer.GetNumIndices()} * (Buffer.Is32Bit() ? int64{sizeof(uint32)} : int64{sizeof(uint16)});
		};
		for (const auto& LOD : Mesh->GetRenderData()->LODResources)
		{
			const auto& Vertices = LOD.VertexBuffers;
			auto LODBytes = int64{Vertices.PositionVertexBuffer.GetNumVertices()} * Vertices.PositionVertexBuffer.GetStride() +
				Vertices.StaticMeshVertexBuffer.GetResourceSize() +
				int64{Vertices.ColorVertexBuffer.GetNumVertices()} * Vertices.ColorVertexBuffer.GetStride();
			LODBytes += IndexBytes(LOD.IndexBuffer) + IndexBytes(LOD.DepthOnlyIndexBuffer);
			if (LOD.AdditionalIndexBuffers != nullptr)
			{
				LODBytes += IndexBytes(LOD.AdditionalIndexBuffers->ReversedIndexBuffer) +
					IndexBytes(LOD.AdditionalIndexBuffers->ReversedDepthOnlyIndexBuffer) +
					IndexBytes(LOD.AdditionalIndexBuffers->WireframeIndexBuffer);
			}
			// 非常駐LODでは一部のメタデータが空になるため、保存済み全バッファ容量も下限にする。
			Bytes += FMath::Max<int64>(LODBytes, LOD.BuffersSize);
			if (Mesh->bSupportGpuUniformlyDistributedSampling == true)
			{
				for (const auto& Section : LOD.Sections)
				{
					Bytes += int64{Section.NumTriangles} * (sizeof(float) + sizeof(uint32));
				}
			}
		}
		if (Bytes <= 0)
		{
			OutError = TEXT("The particle mesh has no prepared raster buffers.");
			return false;
		}

		OutBytes = Bytes * 2;
		return true;
	}

	OutBytes = MeshParticleizerTextureGpuBytes(Cast<UTexture>(Asset));
	return true;
}

int64 MeshParticleizerTextureGpuBytes(const UTexture* Texture)
{
	const auto* Target = Cast<UTextureRenderTarget2D>(Texture);
	if (Target == nullptr)
	{
		return Texture != nullptr ? Texture->CalcTextureMemorySizeEnum(TMC_AllMips) : 0;
	}
	if (Target->SizeX <= 0 || Target->SizeY <= 0 || Target->SizeX > GMaxTextureDimensions || Target->SizeY > GMaxTextureDimensions ||
		Target->GetFormat() == PF_Unknown || GDynamicRHI == nullptr)
	{
		return 0;
	}

	// FTextureRenderTarget2DResourceと同じ記述から、現在のRHIの確保単位で算出する。
	auto Flags = TexCreate_RenderTargetable | TexCreate_ShaderResource;
	if (Target->IsSRGB() == true)
	{
		Flags |= TexCreate_SRGB;
	}
	if (Target->bGPUSharedFlag == true)
	{
		Flags |= TexCreate_Shared;
	}
	if (Target->bNoFastClear == true)
	{
		Flags |= TexCreate_NoFastClear;
	}
	if (Target->bCanCreateUAV == true || Target->bSupportsUAV == true ||
		(Target->bAutoGenerateMips == true && FGenerateMips::WillFormatSupportCompute(Target->GetFormat()) == true))
	{
		Flags |= TexCreate_UAV;
	}
	const auto Mips = Target->bAutoGenerateMips ? FMath::FloorLog2(FMath::Max(Target->SizeX, Target->SizeY)) + 1 : 1;
	const auto Samples = GetNumFromRenderTargetSampleCount(Target->GetSampleCount());
	auto Desc = FRHITextureCreateDesc::Create2D(TEXT("MeshParticleizer.TextureReservation"), Target->SizeX, Target->SizeY, Target->GetFormat())
													.SetNumMips(Mips)
													.SetNumSamples(Samples)
													.SetFlags(Flags)
													.SetInitialState(ERHIAccess::SRVMask);
	auto Bytes = static_cast<int64>(RHICalcTexturePlatformSize(Desc).Size);
	if (Samples > 1)
	{
		Desc.SetNumSamples(1).SetFlags((Flags & ~TexCreate_RenderTargetable) | TexCreate_ResolveTargetable);
		Bytes += RHICalcTexturePlatformSize(Desc).Size;
	}
	else if (Target->bNeedsTwoCopies == true)
	{
		Bytes += RHICalcTexturePlatformSize(Desc).Size;
	}
	return Bytes;
}

bool FMeshParticleizerGpuMemory::Build(UNiagaraSystem* System, int32 BoneCount, int32 CurveVectorCount, int32 MaterialSlots,
	FMeshParticleizerGpuMemory& OutMemory, FString& OutError)
{
	if (System == nullptr || BoneCount < 0 || CurveVectorCount < 0 || MaterialSlots < 1 ||
		System->GetEmitterHandles().Num() != 1 || System->GetEmitterCompiledData().Num() != 1)
	{
		OutError = TEXT("Memory accounting requires one compiled Mesh Particleizer GPU emitter.");
		return false;
	}
	const auto& Handle = System->GetEmitterHandles()[0];
	const auto* Emitter = Handle.GetInstance().GetEmitterData();
	if (Emitter == nullptr || Handle.GetIsEnabled() == false || Emitter->SimTarget != ENiagaraSimTarget::GPUComputeSim ||
		Emitter->AllocationMode != EParticleAllocationMode::ManualEstimate || Emitter->PreAllocationCount < 0)
	{
		OutError = TEXT("The GPU emitter must use Manual Estimate allocation. Upgrade or regenerate the particle system.");
		return false;
	}
	if (System->HasFixedTickDelta() == true || Emitter->GetSimulationStages().IsEmpty() == false)
	{
		OutError = TEXT("Shared GPU memory accounting requires the generated variable-tick emitter without additional simulation stages.");
		return false;
	}

	auto* ChunkSize = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.NiagaraGPUDataBufferChunkSize"));
	const auto Alignment = ChunkSize != nullptr ? ChunkSize->GetInt() : 4096;
	if (Alignment <= 0 || FMath::IsPowerOfTwo(Alignment) == false)
	{
		OutError = TEXT("Niagara GPU buffer chunk size must be a positive power of two.");
		return false;
	}
	const auto& Layout = System->GetEmitterCompiledData()[0]->DataSetCompiledData;

	// 生成済みSprite／ラスタメッシュを扱う。未計上のsort・追加LOD等を黙って通さない。
	const auto FarVersion = FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.FarSpriteVersion"));
	const auto bFarSprite = System->GetExposedParameters().IndexOf(FarVersion) != INDEX_NONE;
	if (bFarSprite == true && System->GetExposedParameters().GetParameterValue<int32>(FarVersion) != 1)
	{
		OutError = TEXT("Unsupported far Sprite renderer version.");
		return false;
	}
	auto RendererCount = 0;
	auto MeshSections = 0;
	auto SpriteCount = 0;
	auto MeshDistance = 0.f;
	auto SpriteDistance = 0.f;
	for (const auto* Renderer : Emitter->GetRenderers())
	{
		if (Renderer == nullptr || Renderer->GetIsEnabled() == false)
		{
			continue;
		}
		if (auto* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer))
		{
			auto* Mesh = MeshRenderer->Meshes.Num() == 1 ? MeshRenderer->Meshes[0].Mesh.Get() : nullptr;
			auto AssetBytes = int64{0};
			if (Mesh == nullptr)
			{
				OutError = TEXT("Mesh particle capacity requires exactly one valid static mesh.");
				return false;
			}
			if (MeshParticleizerAssetGpuBytes(Mesh, AssetBytes, OutError) == false)
			{
				return false;
			}
			const auto& Entry = MeshRenderer->Meshes[0];
			const auto* Material = MeshRenderer->OverrideMaterials.Num() == 1 ? MeshRenderer->OverrideMaterials[0].ExplicitMat.Get() : nullptr;
			const auto bParticleVisibility = Layout.Variables.Contains(MeshRenderer->RendererVisibilityTagBinding.GetDataSetBindableVariable());
			const auto bParticleMeshIndex = Layout.Variables.Contains(MeshRenderer->MeshIndexBinding.GetDataSetBindableVariable());

			// 追加LOD・sort・影は別の描画要求を生むため、生成時と同じprofileだけを受理する。
			if (MeshRenderer->SourceMode != ENiagaraRendererSourceDataMode::Particles || MeshRenderer->SortMode != ENiagaraSortMode::None ||
				MeshRenderer->bCastShadows == true || MeshRenderer->bEnableMeshFlipbook == true ||
				MeshRenderer->bEnableFrustumCulling == true || MeshRenderer->bEnableCameraDistanceCulling != bFarSprite ||
				bParticleVisibility == true || bParticleMeshIndex == true || MeshRenderer->bOverrideMaterials == false || Material == nullptr ||
				IsTranslucentBlendMode(Material->GetBlendMode()) == true || MeshRenderer->MeshesBinding.ResolvedParameter.IsValid() == true ||
				Entry.MeshParameterBinding.ResolvedParameter.IsValid() == true || Entry.LODMode != ENiagaraMeshLODMode::LODLevel || Entry.LODLevel != 0 ||
				Mesh->GetRenderData()->LODResources.Num() != 1 || Mesh->GetRenderData()->LODResources[0].Sections.Num() != 1 ||
				FNiagaraMeshVertexFactory::IsGPUSceneEnabled(GMaxRHIShaderPlatform, GMaxRHIFeatureLevel) == false)
			{
				OutError = TEXT("Mesh particle capacity requires one fixed raster LOD/section, GPU Scene, opaque or masked override material, and generated settings without sorting, additional culling or shadows.");
				return false;
			}
			if (bFarSprite == true && (MeshRenderer->MinCameraDistance != 0.f || FMath::IsFinite(MeshRenderer->MaxCameraDistance) == false || MeshRenderer->MaxCameraDistance <= 0.f || Entry.PivotOffsetSpace != ENiagaraMeshPivotOffsetSpace::Mesh || Entry.PivotOffset.Equals(-Mesh->GetBounds().Origin) == false))
			{
				OutError = TEXT("Far Sprite switching requires a finite mesh distance and a centered particle pivot.");
				return false;
			}
			MeshDistance = MeshRenderer->MaxCameraDistance;
			++MeshSections;
			++RendererCount;
			continue;
		}
		const auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer);
		if (Sprite == nullptr || Sprite->SourceMode != ENiagaraRendererSourceDataMode::Particles || Sprite->Material == nullptr)
		{
			OutError = TEXT("GPU memory accounting requires the generated particle Sprite renderer and its material.");
			return false;
		}
		const auto bNeedsSort = Sprite->SortMode != ENiagaraSortMode::None &&
			(Sprite->bSortOnlyWhenTranslucent == false || IsTranslucentBlendMode(Sprite->Material->GetBlendMode()));
		const auto bParticleVisibility = Sprite->RendererVisibilityTagBinding.CanBindToHostParameterMap() == false &&
			Layout.Variables.Contains(Sprite->RendererVisibilityTagBinding.GetDataSetBindableVariable());
		if (bNeedsSort == true || Sprite->bEnableCameraDistanceCulling == true || bParticleVisibility == true ||
			Sprite->GetCutoutData().IsEmpty() == false)
		{
			OutError = TEXT("Sprite sorting, per-particle culling and cutout geometry are not covered by the current GPU memory model. Use the generated renderer settings.");
			return false;
		}
		if (bFarSprite == true && (Sprite->bCastShadows == true || IsTranslucentBlendMode(Sprite->Material->GetBlendMode()) == true || Sprite->MaterialUserParamBinding.Parameter.GetName() != TEXT("User.FarParticleMaterial") || Sprite->Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("MP_FarSpriteDistance")), SpriteDistance) == false || FMath::IsFinite(SpriteDistance) == false || SpriteDistance <= 0.f))
		{
			OutError = TEXT("Far Sprite capacity requires the generated masked material and its distance binding, without shadows.");
			return false;
		}
		++SpriteCount;
		++RendererCount;
	}
	// 二つのRendererは同じ粒子bufferを読む。GPU Sceneはメッシュ1組、間接描画はSprite1組。
	// 共有予算の既存の1 Simulation当たり2描画枠が、通常ViewとStereoの両方を覆う。
	if (RendererCount != (bFarSprite ? 2 : 1) ||
		(bFarSprite == true && (MeshSections != 1 || SpriteCount != 1 || MeshDistance != SpriteDistance)))
	{
		OutError = TEXT("GPU memory accounting requires one generated renderer, or one matching near mesh and far Sprite pair.");
		return false;
	}

	auto Memory = FMeshParticleizerGpuMemory{};
	Memory.MeshSections = MeshSections;
	Memory.FloatComponents = Layout.TotalFloatComponents;
	Memory.IntComponents = Layout.TotalInt32Components;
	Memory.HalfComponents = Layout.TotalHalfComponents;
	Memory.AllocationAlignment = Alignment;
	Memory.MinimumInstances = Emitter->PreAllocationCount;
	Memory.bPersistentIDs = Layout.bRequiresPersistentIDs;
	Memory.CoverageBoneCount = BoneCount;
	Memory.CoverageMaterialSlots = MaterialSlots;
	// 2姿勢×3行、カーブ、スロット別の色。各配列の末尾にはNiagaraの既定値が1要素入る。
	const FName Names[] = {TEXT("User.Bones"), TEXT("User.PreviousBones"), TEXT("User.ProgressCurve"), TEXT("User.SurfaceTints")};
	const int64 Counts[] = {int64{BoneCount} * 3, int64{BoneCount} * 3, CurveVectorCount, MaterialSlots};
	for (auto Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
	{
		const auto Variable = FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat4::StaticClass()), Names[Index]);
		const auto* Array = Cast<UNiagaraDataInterfaceArrayFloat4>(System->GetExposedParameters().GetDataInterface(Variable));
		if (Array == nullptr || Array->MaxElements < 0)
		{
			OutError = TEXT("A required GPU history, curve or surface color array is missing or has an invalid capacity.");
			return false;
		}
		const auto Elements = Counts[Index] > 0 ? Counts[Index] : int64{Array->GetArrayReference().Num()};
		Memory.ArrayBytes += (FMath::Max<int64>(Elements, Array->MaxElements) + 1) * sizeof(FVector4f);
	}
	OutMemory = Memory;
	return true;
}

int64 FMeshParticleizerGpuMemory::Estimate(int32 ParticleCount) const
{
	if (ParticleCount <= 0)
	{
		return 0;
	}
	// GPUの余剰1要素、スレッド群、各属性バッファのチャンク単位を順に反映する。
	const auto Instances = int64{FMath::Max(ParticleCount, this->MinimumInstances)} + 1;
	const auto Padded = Align(Instances, int64{NiagaraComputeMaxThreadGroupSize});
	const auto Alignment = int64{this->AllocationAlignment};
	const auto DataBytes = Align(Padded * this->FloatComponents * sizeof(float), Alignment) +
		Align(Padded * this->IntComponents * sizeof(int32), Alignment) +
		Align(Padded * this->HalfComponents * sizeof(FFloat16), Alignment);
	// 描画中の1面と、複数更新時に使う2面を予約する。永続IDは3面とFree ID表を含む。
	const auto IDBytes = this->bPersistentIDs ? Align(Instances, int64{1024}) * sizeof(int32) * 4 : 0;

	// 状態は候補ごとにuint4。選別・集計・1件の読み戻し用に64KiBを含め、終了Fenceまで保持する。
	const auto BudgetBytes = int64{ParticleCount} * sizeof(uint32) * 4 + 64 * 1024;
	const auto ColorBytes = this->bSurfaceColorCache ? int64{ParticleCount} * sizeof(FVector4f) : 0;
	const auto EmissionBytes = this->bSourceEmissionCache ? int64{ParticleCount} * sizeof(FVector4f) : 0;
	const auto CoverageBytes = this->bSurfaceCoverage ? MeshParticleizerCoverageGpuBytes(ParticleCount, this->CoverageBoneCount, this->CoverageMaterialSlots) : 0;
	return DataBytes * 3 + IDBytes + this->ArrayBytes + BudgetBytes + ColorBytes + EmissionBytes + CoverageBytes;
}

FMeshParticleizerSharedGpuSettings FMeshParticleizerSharedGpuSettings::Read()
{
	auto Result = FMeshParticleizerSharedGpuSettings{};
	auto ReadInt = [](const TCHAR* Name, int32 Default) {
		const auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable != nullptr ? Variable->GetInt() : Default;
	};
	auto ReadFloat = [](const TCHAR* Name, float Default) {
		const auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable != nullptr ? Variable->GetFloat() : Default;
	};
	Result.MinimumCounters = ReadInt(TEXT("Niagara.MinGPUInstanceCount"), Result.MinimumCounters);
	Result.CounterIncrement = ReadInt(TEXT("Niagara.GPUCountManager.AllocateIncrement"), Result.CounterIncrement);
	Result.CounterSlack = ReadFloat(TEXT("Niagara.GPUCountBufferSlack"), Result.CounterSlack);
	Result.MinimumIndirectArgs = ReadInt(TEXT("fx.Niagara.IndirectArgsPool.MinSize"), Result.MinimumIndirectArgs);
	Result.IndirectGrowth = ReadFloat(TEXT("fx.Niagara.IndirectArgsPool.BlockSizeFactor"), Result.IndirectGrowth);
	Result.MaxPendingTicks = ReadInt(TEXT("fx.Niagara.Batcher.TickFlush.MaxPendingTicks"), Result.MaxPendingTicks);
	return Result;
}

bool FMeshParticleizerSharedGpuMemory::Grow(int32 Simulations, const FMeshParticleizerSharedGpuSettings& Settings, FString& OutError)
{
	OutError.Reset();
	if (Simulations == 0)
	{
		return true;
	}
	if (Simulations < 0 || Settings.MinimumCounters <= 0 || Settings.CounterIncrement <= 0 ||
		FMath::IsFinite(Settings.CounterSlack) == false || Settings.CounterSlack < 1 || Settings.MinimumIndirectArgs <= 0 ||
		FMath::IsFinite(Settings.IndirectGrowth) == false || Settings.IndirectGrowth < 1 || Settings.MaxPendingTicks <= 0)
	{
		OutError = TEXT("Niagara shared GPU allocation settings must have positive capacities and finite growth factors of at least one.");
		return false;
	}

	// 3つの粒子面と、滞留した各更新のSpawn/Updateを見込む。解放待ちソースもSimulationsに含む。
	const auto RequiredCounters = int64{Simulations} * (3 + int64{Settings.MaxPendingTicks} * 2) + Settings.CounterIncrement;
	const auto RecommendedCounters = FMath::Max<double>(Settings.MinimumCounters, RequiredCounters * double{Settings.CounterSlack});

	// 同じソースの通常描画とInstanced Stereoは別のキー。Viewごとの同じキーはNiagaraが共有する。
	const auto Draws = int64{Simulations} * 2;
	const auto RecommendedIndirect = FMath::Max<double>(Settings.MinimumIndirectArgs, Draws * double{Settings.IndirectGrowth});
	if (FMath::IsFinite(RecommendedCounters) == false || RecommendedCounters > MAX_uint32 / sizeof(uint32) ||
		FMath::IsFinite(RecommendedIndirect) == false || RecommendedIndirect > MAX_uint32 / (5 * sizeof(uint32)))
	{
		OutError = TEXT("Niagara shared GPU allocation settings exceed the supported buffer size.");
		return false;
	}

	// 増設前のプールは要求数未満、最後の1面は成長係数分を確保する。設定変更でも過去の最大値を保つ。
	auto Candidate = *this;
	Candidate.CounterCapacity = FMath::Max(Candidate.CounterCapacity, FMath::CeilToInt64(RecommendedCounters));
	Candidate.IndirectCapacity = FMath::Max(Candidate.IndirectCapacity, FMath::CeilToInt64(RecommendedIndirect));
	Candidate.IndirectPoolBytes = FMath::Max(Candidate.IndirectPoolBytes, (Candidate.IndirectCapacity + Draws) * 5 * int64{sizeof(uint32)});

	// 間接描画生成とカウンタ消去の一時入力。3フレーム分を含める。
	Candidate.TaskBytes = FMath::Max(Candidate.TaskBytes, Align((Draws * 5 + RequiredCounters) * int64{sizeof(uint32)} * 3, int64{256}));
	*this = Candidate;
	return true;
}

bool FMeshParticleizerSharedGpuMemory::GrowScene(const FMeshParticleizerSceneGpuSnapshot& Scene,
	int64 AdditionalPrimitives, int64 AdditionalInstances, int64 AdditionalPayloadFloat4s, FString& OutError, int32 Views)
{
	OutError.Reset();
	// Engineと同じ確保単位を使い、未観測・不正な設定では予約を更新しない。
	if (Scene.Frame == 0 || Views <= 0 || Scene.PrimitiveStride == 0 || Scene.InstanceStride == 0 ||
		Scene.CommitAlignment == 0 || FMath::IsPowerOfTwo(Scene.CommitAlignment) == false ||
		Scene.TileSizeLog2 < -1 || Scene.TileSizeLog2 > 24 ||
		Scene.Primitives < 0 || Scene.Instances < 0 || Scene.PayloadFloat4s < 0 ||
		AdditionalPrimitives < 0 || AdditionalInstances < 0 || AdditionalPayloadFloat4s < 0 ||
		Scene.Primitives > MAX_int32 || Scene.Instances > MAX_INSTANCE_ID || Scene.PayloadFloat4s > MAX_int32 ||
		AdditionalPrimitives > MAX_int32 || AdditionalInstances > MAX_INSTANCE_ID || AdditionalPayloadFloat4s > MAX_int32 ||
		Scene.PrimitiveBytes < 0 || Scene.InstanceBytes < 0 || Scene.PayloadBytes < 0 ||
		Scene.RetainedUploadBytes < 0 || Scene.RetainedUploadBytes > MAX_uint32)
	{
		OutError = TEXT("GPU Scene capacity requires a valid rendered snapshot and nonnegative bounded demands.");
		return false;
	}
	const auto Primitives = FMath::Max<int64>(256, Scene.Primitives + AdditionalPrimitives);
	const auto Instances = FMath::Max<int64>(256, Scene.Instances + AdditionalInstances);
	const auto Payload = FMath::Max<int64>(256, Scene.PayloadFloat4s + AdditionalPayloadFloat4s);
	if (Primitives > MAX_int32 || Instances > MAX_INSTANCE_ID || Payload > MAX_int32 ||
		AdditionalPrimitives > MAX_uint32 / Scene.PrimitiveStride)
	{
		OutError = TEXT("GPU Scene demands exceed the engine's addressable capacity.");
		return false;
	}

	const auto RoundPowerOfTwo = [](int64 Value) {
		return static_cast<int64>(FMath::RoundUpToPowerOfTwo64(static_cast<uint64>(Value)));
	};
	const auto PrimitiveBytes = Align(RoundPowerOfTwo(Primitives) * Scene.PrimitiveStride, int64{Scene.CommitAlignment});
	const auto PayloadBytes = Align(RoundPowerOfTwo(Payload) * int64{sizeof(FVector4f)}, int64{Scene.CommitAlignment});

	// 同じGraphの再uploadは別の対を確保する。static更新1組と各主View、全2冪サイズの保持を覆う。
	// instance／payloadをGPUで書くNiagara経路のprimitive転送。CPU instance転送の予測には使わない。
	const auto UploadElements = Scene.Primitives + AdditionalPrimitives;
	if (UploadElements > (int64{1} << 27) || UploadElements * Scene.PrimitiveStride > (int64{1} << 31))
	{
		OutError = TEXT("Primitive uploads exceed the engine's scatter upload capacity.");
		return false;
	}
	const auto UploadPoolPerCall = UploadElements > 0 ? (RoundPowerOfTwo(UploadElements * Scene.PrimitiveStride) + RoundPowerOfTwo(UploadElements * sizeof(uint32))) * 2 : 0;
	const auto UploadCalls = int64{Views} + 1;
	if (UploadPoolPerCall > (MAX_uint32 - Scene.RetainedUploadBytes) / UploadCalls)
	{
		OutError = TEXT("Retained primitive upload pools exceed the supported reservation size.");
		return false;
	}
	const auto UploadBytes = Scene.RetainedUploadBytes + UploadPoolPerCall * UploadCalls;
	auto InstanceBytes = RoundPowerOfTwo(Instances) * Scene.InstanceStride;
	if (Scene.TileSizeLog2 >= 0)
	{
		const auto TileSize = int64{1} << Scene.TileSizeLog2;
		InstanceBytes = Align(Align(Instances, TileSize) * Scene.InstanceStride, int64{16 * 1024 * 1024});
	}
	InstanceBytes = Align(InstanceBytes, int64{Scene.CommitAlignment});
	if (PrimitiveBytes > MAX_uint32 || InstanceBytes > MAX_uint32 || PayloadBytes > MAX_uint32 ||
		Scene.PrimitiveBytes > MAX_uint32 || Scene.InstanceBytes > MAX_uint32 || Scene.PayloadBytes > MAX_uint32 ||
		Scene.RetainedUploadBytes > MAX_uint32 || UploadBytes > MAX_uint32)
	{
		OutError = TEXT("GPU Scene buffer growth exceeds the supported RHI buffer size.");
		return false;
	}

	// 元のSceneが持つ余裕と過去の最大値も含める。仮想予約の2GiBを物理容量へ加算しない。
	this->ScenePrimitiveBytes = FMath::Max3(this->ScenePrimitiveBytes, Scene.PrimitiveBytes, PrimitiveBytes);
	this->SceneInstanceBytes = FMath::Max3(this->SceneInstanceBytes, Scene.InstanceBytes, InstanceBytes);
	this->ScenePayloadBytes = FMath::Max3(this->ScenePayloadBytes, Scene.PayloadBytes, PayloadBytes);
	this->SceneUploadBytes = FMath::Max(this->SceneUploadBytes, UploadBytes);
	return true;
}

int64 FMeshParticleizerSharedGpuMemory::GetSceneReservationBytes() const
{
	// 再確保で並存する旧世代1組を含め、個々の粒子終了では共有容量を返さない。
	return (this->ScenePrimitiveBytes + this->SceneInstanceBytes + this->ScenePayloadBytes + this->SceneUploadBytes) * 2;
}

bool FMeshParticleizerSharedGpuMemory::GrowDraw(const FMeshParticleizerDrawGpuSnapshot& Draws, FString& OutError)
{
	OutError.Reset();
	if (Draws.Frame == 0 || Draws.Views <= 0 || Draws.bSupportedLayout == false)
	{
		OutError = TEXT("Draw pool capacity requires a rendered non-uniform-buffer-view snapshot.");
		return false;
	}

	// UE5.8のpoolは64KiB単位。未使用30frameを超えて退役するため、現frame込み32サイズを見込む。
	// 同じサイズは再利用されるので、最大値を32倍せず、異なるサイズの大きい順の合計を使う。
	constexpr int64 Page = 64 * 1024;
	constexpr int64 RetainedSizes = 32;
	auto Peaks = this->DrawBufferHighWater;
	auto Counts = this->DrawBufferCounts;
	auto Offsets = TMap<FName, int32>{};
	for (const auto& Pass : Draws.Passes)
	{
		if (Pass.bSubmitted == false)
		{
			continue;
		}
		if (Pass.GpuWrittenBytes <= 0 || Pass.GpuWrittenBytes > MAX_uint32 || Pass.PooledBufferBytes.IsEmpty())
		{
			OutError = TEXT("Submitted draw pass has no bounded buffer footprint.");
			return false;
		}
		const auto BufferCount = Pass.PooledBufferBytes.Num() + 1;
		auto& PreviousCount = Counts.FindOrAdd(Pass.Name);
		if (PreviousCount != 0 && PreviousCount != BufferCount)
		{
			OutError = TEXT("Draw pass buffer layout changed while its pool reservation is retained.");
			return false;
		}
		PreviousCount = BufferCount;

		// 同じpassのbuffer順を固定し、複数View分を並べる。減ったView・passの過去容量も保持する。
		auto& Offset = Offsets.FindOrAdd(Pass.Name);
		auto& PassPeaks = Peaks.FindOrAdd(Pass.Name);
		const auto Required = Offset + Pass.PooledBufferBytes.Num() + 1;
		if (PassPeaks.Num() < Required)
		{
			PassPeaks.SetNumZeroed(Required);
		}
		for (auto Index = 0; Index <= Pass.PooledBufferBytes.Num(); ++Index)
		{
			const auto Bytes = Index < Pass.PooledBufferBytes.Num() ? Pass.PooledBufferBytes[Index] : Pass.GpuWrittenBytes;
			if (Bytes <= 0 || Bytes > MAX_uint32)
			{
				OutError = TEXT("Draw pool buffer demand exceeds the supported RHI buffer size.");
				return false;
			}
			PassPeaks[Offset] = FMath::Max(PassPeaks[Offset], Bytes);
			++Offset;
		}
	}

	// deferred統合で同じ役割のbufferが合流する。独立したpassのサイズクラスを足すだけでは不足する。
	struct FPoolDemand
	{
		int64 Pages = 0;
		int64 Buffers = 0;
	};
	auto MeshPools = TArray<FPoolDemand>{};
	auto ViewPools = TArray<FPoolDemand>{};
	for (const auto& Pair : Peaks)
	{
		const auto Count = Counts.FindRef(Pair.Key);
		if (Count <= 0 || Pair.Value.Num() % Count != 0)
		{
			OutError = TEXT("Retained draw pool has an inconsistent buffer layout.");
			return false;
		}
		auto& Pools = Pair.Key == TEXT("SharedViewData") ? ViewPools : MeshPools;
		if (Pools.Num() < Count)
		{
			Pools.SetNum(Count);
		}
		for (auto Index = 0; Index < Pair.Value.Num(); ++Index)
		{
			auto& Pool = Pools[Index % Count];
			Pool.Pages += FMath::DivideAndRoundUp(Pair.Value[Index], Page);
			++Pool.Buffers;
		}
	}

	auto PoolBytes = int64{0};
	for (const auto* Pools : {&MeshPools, &ViewPools})
	{
		for (const auto& Pool : *Pools)
		{
			if (Pool.Pages > MAX_int64 / (RetainedSizes * Page * 2))
			{
				OutError = TEXT("Retained draw pool capacity exceeds the supported reservation size.");
				return false;
			}
			auto Retained = int64{0};
			if (Pool.Buffers == 1)
			{
				const auto Count = FMath::Min(Pool.Pages, RetainedSizes);
				Retained = (Pool.Pages + Pool.Pages - Count + 1) * Count / 2;
			}
			else
			{
				// 総量S pageを分割する際、k pageの同時buffer数はmin(分割数, S/k)以下。
				// 全サイズの合計と32frame分の総量の小さい方で、統合・分割の切替も覆う。
				const auto FrameBound = Pool.Pages * RetainedSizes;
				for (auto Layer = int64{1}; Layer <= Pool.Buffers && Layer <= Pool.Pages; ++Layer)
				{
					const auto Size = Pool.Pages / Layer;
					if (Size > (FrameBound - Retained) * 2 / (Size + 1))
					{
						Retained = FrameBound;
						break;
					}
					Retained += Size * (Size + 1) / 2;
				}
			}
			const auto Bytes = Retained * Page;
			if (Bytes > MAX_int64 / 2 - PoolBytes)
			{
				OutError = TEXT("Retained draw pool capacity exceeds the supported reservation size.");
				return false;
			}
			PoolBytes += Bytes;
		}
	}

	// 消失で入力が縮んでも過去のpool容量を返さない。全passの検証後に原子的に更新する。
	this->DrawPoolBytes = FMath::Max(this->DrawPoolBytes, PoolBytes);
	this->DrawBufferHighWater = MoveTemp(Peaks);
	this->DrawBufferCounts = MoveTemp(Counts);
	return true;
}

int64 FMeshParticleizerSharedGpuMemory::GetDrawReservationBytes() const
{
	// pool退役後のRHI解放待ち1組を含む。GPU出力も非Transient時のpool上界で見込む。
	return this->DrawPoolBytes * 2;
}

int64 FMeshParticleizerSharedGpuMemory::GetReservationBytes() const
{
	// カウンタ本体、複数View用コピー、読み戻し領域。拡張時の旧容量もそれぞれ1面見込む。
	return this->CounterCapacity * sizeof(uint32) * 6 + this->IndirectPoolBytes + this->TaskBytes + this->DistanceFieldBytes +
		this->GetSceneReservationBytes() + this->GetDrawReservationBytes();
}
