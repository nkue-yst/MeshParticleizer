#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MeshParticleizerAssetCache.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "TextureResource.h"
#include "Engine/World.h"
#include "MeshParticleizerSurfaceTexture.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "RenderingThread.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "StaticMeshCompiler.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"

namespace
{
	struct FCacheLifetimeTest
	{
		TStrongObjectPtr<UMeshParticleizerAssetCache> Cache{NewObject<UMeshParticleizerAssetCache>()};
		TStrongObjectPtr<UMeshParticleizerPreparedMesh> OwnerA{NewObject<UMeshParticleizerPreparedMesh>()};
		TStrongObjectPtr<UMeshParticleizerPreparedMesh> OwnerB{NewObject<UMeshParticleizerPreparedMesh>()};
		TStrongObjectPtr<UMeshParticleizerPreparedMesh> OwnerC{NewObject<UMeshParticleizerPreparedMesh>()};
		TStrongObjectPtr<UTexture2D> TextureA{UTexture2D::CreateTransient(256, 256)};
		TStrongObjectPtr<UTexture2D> TextureB{UTexture2D::CreateTransient(256, 256)};
		TStrongObjectPtr<UTexture2D> TextureC{UTexture2D::CreateTransient(256, 256)};
		FMeshParticleizerResource ResourceA;
		FMeshParticleizerResource ResourceB;
		FMeshParticleizerResource ResourceC;
		FAutomationTestBase* Test = nullptr;
		double Deadline = FPlatformTime::Seconds() + 30;
		int32 Stage = 0;
	};

	class FWaitForCacheRelease final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForCacheRelease(TSharedRef<FCacheLifetimeTest> InContext)
			: Context(MoveTemp(InContext))
		{
		}

		virtual bool Update() override
		{
			auto& State = *this->Context;
			State.Cache->Poll();
			if (FPlatformTime::Seconds() > State.Deadline)
			{
				State.Test->AddError(TEXT("Shared asset destruction or GPU release did not finish within 30 seconds."));
				return true;
			}
			if (State.Cache->GetUsage().RetiringAssets != 0)
			{
				return false;
			}
			if (State.Stage == 1)
			{
				State.Test->TestEqual(TEXT("Native destruction and GPU completion return all cache capacity"), State.Cache->GetUsage().ReservedBytes, int64{0});
				return true;
			}

			auto Error = FString{};
			State.Test->TestEqual(TEXT("LRU releases only the older texture"), State.Cache->GetUsage().ReservedBytes, State.ResourceA.GpuBytes);
			State.Test->TestTrue(TEXT("New registration fits after the release fence"), State.Cache->Acquire(State.OwnerC.Get(), {State.ResourceC}, Error));
			State.Test->TestTrue(TEXT("A live texture resolves without an async load"), State.Cache->IsReady(State.OwnerC.Get(), Error));
			State.Test->TestEqual(TEXT("Reusing freed capacity retains the newer cached texture"), State.Cache->GetUsage().ReservedBytes,
				State.ResourceA.GpuBytes + State.ResourceC.GpuBytes);

			// 利用者の破棄でも参照数を戻す。GCを起こすのは検証のみで、通常再生では強制しない。
			State.OwnerC.Reset();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			State.Cache->Poll();
			State.Test->TestEqual(TEXT("Dead weak owner does not pin assets forever"), State.Cache->GetUsage().PinnedBytes, int64{0});
			State.Cache->TrimUnused();
			State.Test->TestEqual(TEXT("External texture owners keep retirement charged"), State.Cache->GetUsage().RetiringBytes,
				State.ResourceA.GpuBytes + State.ResourceC.GpuBytes);
			State.Test->TestTrue(TEXT("Reacquiring a live retiring texture succeeds"), State.Cache->Acquire(State.OwnerA.Get(), {State.ResourceA}, Error));
			State.Test->TestEqual(TEXT("Reacquisition protects exactly that texture"), State.Cache->GetUsage().PinnedBytes, State.ResourceA.GpuBytes);
			State.Cache->Release(State.OwnerA.Get());
			State.Cache->TrimUnused();
			State.TextureA.Reset();
			State.TextureC.Reset();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			State.Stage = 1;
			return false;
		}

	private:
		TSharedRef<FCacheLifetimeTest> Context;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerAssetCacheTest, "MeshParticleizer.Runtime.AssetCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerAssetCacheTest::RunTest(const FString& Parameters)
{
	// UObjectの読込だけでGPU準備済みとしない。初期化前も容量を保持して、完了後だけ開始を許す。
	auto ReadinessCache = TStrongObjectPtr<UMeshParticleizerAssetCache>(NewObject<UMeshParticleizerAssetCache>());
	auto ReadinessOwner = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	auto PendingTexture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(32, 32));
	auto PendingResource = FMeshParticleizerResource{};
	PendingResource.Asset = PendingTexture.Get();
	PendingResource.GpuBytes = PendingTexture->CalcTextureMemorySizeEnum(TMC_AllMips);
	auto ReadinessError = FString{};
	this->TestTrue(TEXT("Texture capacity is reserved before GPU initialization"),
		ReadinessCache->Acquire(ReadinessOwner.Get(), {PendingResource}, ReadinessError));
	this->TestFalse(TEXT("Loaded texture without a render resource is not ready"), ReadinessCache->IsReady(ReadinessOwner.Get(), ReadinessError));
	this->TestTrue(TEXT("Pending initialization reports waiting, not failure"), ReadinessError.IsEmpty());
	this->TestEqual(TEXT("Waiting texture keeps its capacity reserved"), ReadinessCache->GetUsage().PinnedBytes, PendingResource.GpuBytes);
	PendingTexture->UpdateResource();
	FlushRenderingCommands();
	this->TestTrue(TEXT("Texture becomes ready after GPU initialization"), ReadinessCache->IsReady(ReadinessOwner.Get(), ReadinessError));
	PendingTexture->ReleaseResource();
	this->TestFalse(TEXT("Released render resource is no longer ready"), ReadinessCache->IsReady(ReadinessOwner.Get(), ReadinessError));
	ReadinessCache->Release(ReadinessOwner.Get());

	// 粒子形状は全LODと切替中の旧面を予約し、共有する別ワールドの常駐を解除しない。
	auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (this->TestNotNull(TEXT("Mesh fixture source is available"), Cube) == false)
	{
		return false;
	}
	auto ParticleMesh = TStrongObjectPtr<UStaticMesh>(DuplicateObject<UStaticMesh>(Cube, GetTransientPackage()));
	ParticleMesh->bSupportRayTracing = false;
	ParticleMesh->NaniteSettings.bEnabled = false;
	ParticleMesh->bForceMiplevelsToBeResident = false;
	ParticleMesh->Build(false);
	FStaticMeshCompilingManager::Get().FinishCompilation({ParticleMesh.Get()});
	FlushRenderingCommands();
	auto MeshBytes = int64{0};
	if (this->TestTrue(TEXT("Raster mesh capacity can be prepared"), MeshParticleizerAssetGpuBytes(ParticleMesh.Get(), MeshBytes, ReadinessError)) == false)
	{
		this->AddError(ReadinessError);
		return false;
	}

	auto ResidentBytes = int64{0};
	auto AddBuffer = [&ResidentBytes](const FBufferRHIRef& Buffer) {
		if (Buffer.IsValid() == true)
		{
			ResidentBytes += Buffer->GetSize();
		}
	};
	for (const auto& LOD : ParticleMesh->GetRenderData()->LODResources)
	{
		AddBuffer(LOD.VertexBuffers.PositionVertexBuffer.VertexBufferRHI);
		AddBuffer(LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.VertexBufferRHI);
		AddBuffer(LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.VertexBufferRHI);
		AddBuffer(LOD.VertexBuffers.ColorVertexBuffer.VertexBufferRHI);
		AddBuffer(LOD.IndexBuffer.IndexBufferRHI);
		AddBuffer(LOD.DepthOnlyIndexBuffer.IndexBufferRHI);
		if (LOD.AdditionalIndexBuffers != nullptr)
		{
			AddBuffer(LOD.AdditionalIndexBuffers->ReversedIndexBuffer.IndexBufferRHI);
			AddBuffer(LOD.AdditionalIndexBuffers->ReversedDepthOnlyIndexBuffer.IndexBufferRHI);
			AddBuffer(LOD.AdditionalIndexBuffers->WireframeIndexBuffer.IndexBufferRHI);
		}
		AddBuffer(LOD.AreaWeightedSectionSamplersBuffer.GetBufferRHI());
	}
	this->TestTrue(TEXT("Mesh reservation includes resident RHI buffers and a streaming overlap"), ResidentBytes > 0 && MeshBytes >= ResidentBytes * 2);
	this->AddInfo(FString::Printf(TEXT("Particle mesh raster buffers %lld B; reservation %lld B."), ResidentBytes, MeshBytes));

	auto MeshCacheA = TStrongObjectPtr<UMeshParticleizerAssetCache>(NewObject<UMeshParticleizerAssetCache>());
	auto MeshCacheB = TStrongObjectPtr<UMeshParticleizerAssetCache>(NewObject<UMeshParticleizerAssetCache>());
	auto MeshOwnerB = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	auto MeshResource = FMeshParticleizerResource{};
	MeshResource.Asset = ParticleMesh.Get();
	MeshResource.GpuBytes = MeshBytes;
	MeshCacheA->SetLimit(MeshBytes - 1, ReadinessError);
	this->TestFalse(TEXT("Mesh capacity is rejected before forcing residency"), MeshCacheA->Acquire(ReadinessOwner.Get(), {MeshResource}, ReadinessError));
	this->TestFalse(TEXT("Rejected mesh leaves its residency unchanged"), bool(ParticleMesh->bForceMiplevelsToBeResident));
	this->TestEqual(TEXT("Rejected mesh reserves no partial capacity"), MeshCacheA->GetUsage().ReservedBytes, int64{0});
	MeshCacheA->SetLimit(MeshBytes, ReadinessError);
	this->TestTrue(TEXT("Mesh capacity can be shared within a cache"), MeshCacheA->Acquire(ReadinessOwner.Get(), {MeshResource, MeshResource}, ReadinessError));
	this->TestTrue(TEXT("Initialized raster mesh is ready"), MeshCacheA->IsReady(ReadinessOwner.Get(), ReadinessError));
	this->TestEqual(TEXT("Repeated mesh references reserve once"), MeshCacheA->GetUsage().PinnedBytes, MeshBytes);
	this->TestTrue(TEXT("Another cache acquires the same mesh"), MeshCacheB->Acquire(MeshOwnerB.Get(), {MeshResource}, ReadinessError));
	MeshCacheA->Release(ReadinessOwner.Get());
	this->TestTrue(TEXT("Other cache keeps mesh LODs resident"), bool(ParticleMesh->bForceMiplevelsToBeResident));

	const auto OldBufferSize = ParticleMesh->GetRenderData()->LODResources[0].BuffersSize;
	ParticleMesh->GetRenderData()->LODResources[0].BuffersSize += MeshBytes;
	this->TestFalse(TEXT("Mesh growth cannot reuse the old reservation"), MeshCacheB->IsReady(MeshOwnerB.Get(), ReadinessError));
	ParticleMesh->GetRenderData()->LODResources[0].BuffersSize = OldBufferSize;
	ParticleMesh->bForceMiplevelsToBeResident = false;
	this->TestFalse(TEXT("External mesh residency changes are detected"), MeshCacheB->IsReady(MeshOwnerB.Get(), ReadinessError));
	MeshCacheB->Release(MeshOwnerB.Get());
	this->TestFalse(TEXT("External residency change survives release"), bool(ParticleMesh->bForceMiplevelsToBeResident));
	this->TestTrue(TEXT("Released mesh can be acquired again"), MeshCacheA->Acquire(ReadinessOwner.Get(), {MeshResource}, ReadinessError));
	MeshCacheA->Release(ReadinessOwner.Get());
	this->TestFalse(TEXT("Final mesh release restores its original residency"), bool(ParticleMesh->bForceMiplevelsToBeResident));
	MeshCacheA->TrimUnused();
	this->TestEqual(TEXT("Externally held mesh remains charged while retiring"), MeshCacheA->GetUsage().RetiringBytes, MeshBytes);

	ParticleMesh->bSupportRayTracing = true;
	this->TestFalse(TEXT("Unaccounted ray tracing geometry is rejected"), MeshCacheB->Acquire(MeshOwnerB.Get(), {MeshResource}, ReadinessError));
	ParticleMesh->bSupportRayTracing = false;
	MeshCacheA->Shutdown();
	MeshCacheB->Shutdown();

	// 生成元のSystemは編集せず、Rendererに追加したメッシュの依存漏れを検出する。
	auto* Core = LoadObject<UNiagaraSystem>(nullptr, TEXT("/MeshParticleizer/Core/NS_MeshParticleizer.NS_MeshParticleizer"));
	if (this->TestNotNull(TEXT("Generated system is available for dependency test"), Core) == false)
	{
		return false;
	}
	auto ManifestSystem = TStrongObjectPtr<UNiagaraSystem>(DuplicateObject<UNiagaraSystem>(Core, GetTransientPackage()));
	auto VersionedEmitter = ManifestSystem->GetEmitterHandles()[0].GetInstance();
	auto* MeshRenderer = NewObject<UNiagaraMeshRendererProperties>(VersionedEmitter.Emitter);
	MeshRenderer->Meshes.AddDefaulted_GetRef().Mesh = ParticleMesh.Get();
	VersionedEmitter.Emitter->AddRenderer(MeshRenderer, VersionedEmitter.Version);
	auto Manifest = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	Manifest->ParticleSystem = ManifestSystem.Get();
	Manifest->GdfParticleSystem = ManifestSystem.Get();
	Manifest->Positions = PendingTexture.Get();
	Manifest->Normals = PendingTexture.Get();
	Manifest->Colors = PendingTexture.Get();
	this->TestTrue(TEXT("Manifest captures a renderer mesh once"), Manifest->CaptureResourceManifest(ReadinessError));
	this->TestTrue(TEXT("Captured renderer mesh passes dependency validation"), Manifest->ValidateResourceManifest(ReadinessError));
	const auto* MeshEntry = Manifest->Resources.FindByPredicate([&ParticleMesh](const auto& Entry) {
		return Entry.Asset.Get() == ParticleMesh.Get();
	});
	this->TestTrue(TEXT("Renderer mesh manifest contains its full reservation"), MeshEntry != nullptr && MeshEntry->GpuBytes == MeshBytes);
	Manifest->Resources.RemoveAll([&ParticleMesh](const auto& Entry) {
		return Entry.Asset.Get() == ParticleMesh.Get();
	});
	this->TestFalse(TEXT("Removing renderer mesh metadata rejects playback"), Manifest->ValidateResourceManifest(ReadinessError));

	// 同じTextureを複数ワールドで登録しても、片方の終了でMip固定を解除しない。
	auto WorldA = TStrongObjectPtr<UWorld>(NewObject<UWorld>());
	auto WorldB = TStrongObjectPtr<UWorld>(NewObject<UWorld>());
	auto ManagerA = TStrongObjectPtr<UMeshParticleizerWorldSubsystem>(NewObject<UMeshParticleizerWorldSubsystem>(WorldA.Get()));
	auto ManagerB = TStrongObjectPtr<UMeshParticleizerWorldSubsystem>(NewObject<UMeshParticleizerWorldSubsystem>(WorldB.Get()));
	auto Texture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(32, 32));
	Texture->UpdateResource();
	FlushRenderingCommands();
	const auto TextureBytes = int64{Texture->CalcTextureMemorySizeEnum(TMC_AllMips)};
	auto RegistrationError = FString{};
	ManagerA->SetAssetCacheLimit(TextureBytes - 1, RegistrationError);
	this->TestNull(TEXT("Texture registration rejects insufficient cache capacity"), ManagerA->RegisterSurfaceTexture(Texture.Get(), RegistrationError));
	this->TestFalse(TEXT("Rejected texture registration does not force mips"), bool(Texture->bForceMiplevelsToBeResident));
	ManagerA->SetAssetCacheLimit(TextureBytes, RegistrationError);
	auto LeaseA = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(ManagerA->RegisterSurfaceTexture(Texture.Get(), RegistrationError));
	auto LeaseA2 = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(ManagerA->RegisterSurfaceTexture(Texture.Get(), RegistrationError));
	auto LeaseB = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(ManagerB->RegisterSurfaceTexture(Texture.Get(), RegistrationError));
	if (this->TestNotNull(TEXT("Loaded surface texture registration succeeds"), LeaseA.Get()) == false || LeaseA2.IsValid() == false || LeaseB.IsValid() == false)
	{
		return false;
	}
	this->TestTrue(TEXT("Resident texture registration is ready"), LeaseA->IsReady(RegistrationError));
	this->TestFalse(TEXT("Registrations cannot cross worlds"), LeaseA->BelongsTo(ManagerB.Get()));
	this->TestEqual(TEXT("Repeated texture registration reserves capacity once"), ManagerA->GetAssetCacheUsage().PinnedBytes, TextureBytes);
	LeaseA.Reset();
	LeaseA2.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	this->TestEqual(TEXT("Collected registration returns cache users"), ManagerA->GetAssetCacheUsage().PinnedBytes, int64{0});
	this->TestTrue(TEXT("Other world still holds the mip lease"), bool(Texture->bForceMiplevelsToBeResident));
	LeaseB.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	this->TestFalse(TEXT("Last registration restores the original mip flag"), bool(Texture->bForceMiplevelsToBeResident));

	// UEの共有ノイズも既存キャッシュへ一度だけ計上し、待機中もワールドが常駐を保持する。
	GEngine->LoadBlueNoiseTexture(false);
	auto* Noise = GEngine->BlueNoiseScalarTexture.Get();
	if (this->TestNotNull(TEXT("UE scalar blue noise is loaded"), Noise) == false)
	{
		return false;
	}
	const auto NoiseBytes = int64{Noise->CalcTextureMemorySizeEnum(TMC_AllMips)};
	const auto bNoiseForced = bool(Noise->bForceMiplevelsToBeResident);
	auto NoiseManager = TStrongObjectPtr<UMeshParticleizerWorldSubsystem>(NewObject<UMeshParticleizerWorldSubsystem>(WorldA.Get()));
	auto bNoiseReady = false;
	NoiseManager->SetAssetCacheLimit(NoiseBytes - 1, RegistrationError);
	this->TestFalse(TEXT("Shared noise rejects insufficient capacity"), NoiseManager->PrepareCoverageNoise(bNoiseReady, RegistrationError));
	this->TestEqual(TEXT("Rejected noise does not reserve partial capacity"), NoiseManager->GetAssetCacheUsage().ReservedBytes, int64{0});
	this->TestEqual(TEXT("Rejected noise does not change residency"), bool(Noise->bForceMiplevelsToBeResident), bNoiseForced);
	NoiseManager->SetAssetCacheLimit(NoiseBytes, RegistrationError);
	this->TestTrue(TEXT("Shared noise becomes ready"), NoiseManager->PrepareCoverageNoise(bNoiseReady, RegistrationError) && bNoiseReady);
	this->TestTrue(TEXT("Repeated preparation reuses shared noise"), NoiseManager->PrepareCoverageNoise(bNoiseReady, RegistrationError) && bNoiseReady);
	NoiseManager->TrimUnusedAssets();
	this->TestEqual(TEXT("Shared noise remains pinned once after trim"), NoiseManager->GetAssetCacheUsage().PinnedBytes, NoiseBytes);
	NoiseManager.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	this->TestEqual(TEXT("World ownership ending restores the noise mip flag"), bool(Noise->bForceMiplevelsToBeResident), bNoiseForced);

	// Render Targetは入力と公開画像を別々に計上し、公開していない入力変更を見せない。
	auto ProviderManager = TStrongObjectPtr<UMeshParticleizerWorldSubsystem>(NewObject<UMeshParticleizerWorldSubsystem>(WorldA.Get()));
	auto Input = TStrongObjectPtr<UTextureRenderTarget2D>(NewObject<UTextureRenderTarget2D>());
	Input->bAutoGenerateMips = true;
	Input->bNeedsTwoCopies = true;
	Input->ClearColor = FLinearColor(0.2f, 0.4f, 0.6f, 1);
	Input->InitCustomFormat(256, 256, PF_FloatRGBA, true);
	Input->UpdateResourceImmediate(true);
	FlushRenderingCommands();
	const auto InputBytes = MeshParticleizerTextureGpuBytes(Input.Get());
	this->TestTrue(TEXT("Render Target accounting includes mip chain and second surface"),
		InputBytes > int64{Input->CalcTextureMemorySizeEnum(TMC_AllMips)} * 2);
	ProviderManager->SetAssetCacheLimit(InputBytes, RegistrationError);
	this->TestNull(TEXT("Provider rejects a limit that only fits its external input"), ProviderManager->RegisterColorProvider(Input.Get(), RegistrationError));
	this->TestEqual(TEXT("Rejected provider reserves no partial capacity"), ProviderManager->GetAssetCacheUsage().ReservedBytes, int64{0});
	ProviderManager->SetAssetCacheLimit(InputBytes * 4, RegistrationError);
	auto Provider = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(ProviderManager->RegisterColorProvider(Input.Get(), RegistrationError));
	if (this->TestNotNull(TEXT("Provider registration reserves both images"), Provider.Get()) == false)
	{
		return false;
	}
	this->TestFalse(TEXT("First publication is asynchronous"), Provider->IsReady(RegistrationError));
	ProviderManager->Tick(0);
	FlushRenderingCommands();
	if (this->TestTrue(TEXT("GPU publication becomes ready"), Provider->IsReady(RegistrationError)) == false)
	{
		this->AddError(RegistrationError);
		return false;
	}
	auto* Published = Cast<UTextureRenderTarget2D>(Provider->GetTexture());
	const auto OutputBytes = MeshParticleizerTextureGpuBytes(Published);
	this->AddInfo(FString::Printf(TEXT("Provider GPU reservation: mip-chain/two-surface input %lld B, linear publication %lld B."), InputBytes, OutputBytes));
	this->TestEqual(TEXT("Cache includes external input and published image"), ProviderManager->GetAssetCacheUsage().PinnedBytes, InputBytes + OutputBytes);
	auto Pixels = TArray<FLinearColor>{};
	Published->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
	this->TestTrue(TEXT("Publication preserves linear RGB"), Pixels.Num() > 0 && Pixels[0].Equals(Input->ClearColor, 0.001f));
	UKismetRenderingLibrary::ClearRenderTarget2D(WorldA.Get(), Input.Get(), FLinearColor::Red);

	// 二面入力は描画面を解決してから公開する。Clear単独では参照面へコピーされない。
	Input->UpdateResourceImmediate(false);
	FlushRenderingCommands();
	Published->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
	this->TestTrue(TEXT("Unpublished input changes do not affect display"), Pixels.Num() > 0 && Pixels[0].Equals(Input->ClearColor, 0.001f));
	this->TestTrue(TEXT("Provider accepts explicit republication"), Provider->PublishColor(RegistrationError));
	ProviderManager->Tick(0);
	FlushRenderingCommands();
	Published->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
	this->TestTrue(TEXT("Explicit publication updates the display image"), Pixels.Num() > 0 && Pixels[0].Equals(FLinearColor::Red, 0.001f));
	Input->SizeX = 512;
	this->TestFalse(TEXT("A resized input cannot be published under its old reservation"), Provider->PublishColor(RegistrationError));
	Input->SizeX = 256;

	// 同じ入力を色とHDRの両モードで公開しても入力容量は重複せず、出力の範囲だけを変える。
	auto HDRProvider = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(ProviderManager->RegisterColorProvider(Input.Get(), RegistrationError, true));
	if (this->TestNotNull(TEXT("HDR registration shares the external input"), HDRProvider.Get()) == false)
	{
		return false;
	}
	this->TestEqual(TEXT("Two publications reserve one input and two outputs"), ProviderManager->GetAssetCacheUsage().PinnedBytes, InputBytes + OutputBytes * 2);
	UKismetRenderingLibrary::ClearRenderTarget2D(WorldA.Get(), Input.Get(), FLinearColor(4, 0.5f, -2, 1));
	Input->UpdateResourceImmediate(false);
	this->TestTrue(TEXT("Color republication accepts the shared HDR input"), Provider->PublishColor(RegistrationError));
	ProviderManager->Tick(0);
	FlushRenderingCommands();
	const auto ReadFlags = FReadSurfaceDataFlags(RCM_MinMax);
	Published->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, ReadFlags);
	this->TestTrue(TEXT("Default color publication still clamps to one"), Pixels.Num() > 0 && Pixels[0].Equals(FLinearColor(1, 0.5f, 0, 1), 0.001f));
	auto* HDRPublished = Cast<UTextureRenderTarget2D>(HDRProvider->GetTexture());
	HDRPublished->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, ReadFlags);
	this->TestTrue(TEXT("HDR publication preserves emission above one and clamps negatives"), Pixels.Num() > 0 && Pixels[0].Equals(FLinearColor(4, 0.5f, 0, 1), 0.001f));
	HDRProvider.Reset();
	auto PublishedWeak = TWeakObjectPtr<UTextureRenderTarget2D>(Published);
	Provider.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	this->TestEqual(TEXT("Published image does not retain its registration through an Outer cycle"), ProviderManager->GetAssetCacheUsage().PinnedBytes, int64{0});
	ProviderManager->TrimUnusedAssets();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	FlushRenderingCommands();
	this->TestFalse(TEXT("Unreferenced published image can be collected"), PublishedWeak.IsValid());

	auto State = MakeShared<FCacheLifetimeTest>();
	State->Test = this;
	auto Initialize = [](UTexture2D* Texture, FMeshParticleizerResource& Resource) {
		Texture->UpdateResource();
		Resource.Asset = Texture;
		Resource.GpuBytes = Texture->CalcTextureMemorySizeEnum(TMC_AllMips);
	};
	Initialize(State->TextureA.Get(), State->ResourceA);
	Initialize(State->TextureB.Get(), State->ResourceB);
	Initialize(State->TextureC.Get(), State->ResourceC);
	auto Error = FString{};
	State->Cache->SetLimit(State->ResourceA.GpuBytes + State->ResourceB.GpuBytes, Error);
	this->TestTrue(TEXT("First texture registers"), State->Cache->Acquire(State->OwnerA.Get(), {State->ResourceA}, Error));
	this->TestTrue(TEXT("Second texture registers"), State->Cache->Acquire(State->OwnerB.Get(), {State->ResourceB}, Error));
	State->Cache->Release(State->OwnerB.Get());
	State->Cache->Release(State->OwnerA.Get());

	// 直近まで使っていたAを残し、Bを退避する。GC前の空きを新規Cへ渡さない。
	this->TestFalse(TEXT("Eviction cannot immediately fund a new GPU asset"), State->Cache->Acquire(State->OwnerC.Get(), {State->ResourceC}, Error));
	this->TestEqual(TEXT("Exactly one old asset enters retirement"), State->Cache->GetUsage().RetiringAssets, 1);
	this->TestEqual(TEXT("Rejected request pins no resources"), State->Cache->GetUsage().PinnedBytes, int64{0});
	this->TestFalse(TEXT("Rejected request creates no partial owner registration"), State->Cache->IsReady(State->OwnerC.Get(), Error));
	this->TestEqual(TEXT("Pending destruction retains the old reservation"), State->Cache->GetUsage().ReservedBytes,
		State->ResourceA.GpuBytes + State->ResourceB.GpuBytes);
	State->TextureB.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForCacheRelease(State));
	return true;
}

#endif
