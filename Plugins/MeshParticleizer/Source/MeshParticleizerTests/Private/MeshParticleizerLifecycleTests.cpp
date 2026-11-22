#include "MeshParticleizerTestReceiver.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "../../MeshParticleizerRuntime/Private/MeshParticleizerDistanceField.h"
#include "MeshParticleizerSceneGpu.h"
#include "MeshParticleizerComponent.h"
#include "MeshParticleizeAsyncAction.h"
#include "MeshParticleizerSurfaceTextureAsyncAction.h"
#include "MeshParticleizerPreset.h"
#include "MeshParticleizerOriginLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Curves/CurveFloat.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "NiagaraSystem.h"
#include "NiagaraComponent.h"
#include "StaticMeshResources.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "RenderingThread.h"
#include "TimerManager.h"
#include "TextureResource.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include <limits>

void UMeshParticleizerTestReceiver::Converted(UMeshParticleizeOperation* Operation)
{
	this->Events.Add(TEXT("Converted"));
	if (this->DestroyOnConverted.IsValid() == true)
	{
		this->DestroyOnConverted->Destroy();
	}
}

void UMeshParticleizerTestReceiver::Bind(UMeshParticleizeOperation* Operation)
{
	Operation->OnStarted.AddDynamic(this, &UMeshParticleizerTestReceiver::Started);
	Operation->OnMeshConverted.AddDynamic(this, &UMeshParticleizerTestReceiver::Converted);
	Operation->OnCancelled.AddDynamic(this, &UMeshParticleizerTestReceiver::Cancelled);
	Operation->OnFinished.AddDynamic(this, &UMeshParticleizerTestReceiver::Finished);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	// Timerは実フレームで進める。公開待ち・中止・期限・ワールド終了を同じ入口で確認する。
	class FValidateSurfaceTextureAsync final : public IAutomationLatentCommand
	{
	public:
		explicit FValidateSurfaceTextureAsync(FAutomationTestBase* InTest)
			: Test(InTest)
		{
		}

		virtual ~FValidateSurfaceTextureAsync() override
		{
			if (this->World != nullptr)
			{
				this->World->DestroyWorld(false);
				GEngine->DestroyWorldContext(this->World);
			}
		}

		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() > this->Deadline)
			{
				this->Test->AddError(TEXT("Surface texture async validation did not complete."));
				return true;
			}
			if (this->Stage == 0)
			{
				this->World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("MeshParticleizerSurfaceAsync"));
				GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(this->World);
				auto* Manager = this->World->GetSubsystem<UMeshParticleizerWorldSubsystem>();
				this->Receiver.Reset(NewObject<UMeshParticleizerTestReceiver>());
				this->Input.Reset(NewObject<UTextureRenderTarget2D>());
				this->Input->ClearColor = FLinearColor(4, 0.5f, 0.25f, 1);
				this->Input->InitCustomFormat(16, 16, PF_FloatRGBA, true);
				this->Input->UpdateResourceImmediate(true);
				auto Resident = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(16, 16));
				Resident->UpdateResource();
				FlushRenderingCommands();

				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterSurfaceTexture(nullptr, Resident.Get()));
				this->Action->Activate();
				this->Action->Activate();
				this->Test->TestEqual(TEXT("Missing async world fails once"), this->Receiver->Events.Num(), 1);
				this->Test->TestTrue(TEXT("Missing async world has a diagnostic"), this->Receiver->FailureMessage.Contains(TEXT("world")));
				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterSurfaceTexture(this->World, Resident.Get()));
				this->Action->Activate();
				this->Action->Activate();
				this->Test->TestEqual(TEXT("Resident texture delivers one Ready"), this->Receiver->Events, TArray<FName>{TEXT("SurfaceReady")});
				this->Test->TestNotNull(TEXT("Ready transfers a live registration"), this->Receiver->SurfaceRegistration.Get());

				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(this->World, nullptr));
				this->Action->Activate();
				this->Test->TestEqual(TEXT("Null provider input fails"), this->Receiver->Events, TArray<FName>{TEXT("SurfaceFailed")});
				this->Test->TestNull(TEXT("Failed never returns a registration"), this->Receiver->SurfaceRegistration.Get());
				auto Error = FString{};
				const auto Limit = Manager->GetAssetCacheUsage().LimitBytes;
				const auto Reserved = Manager->GetAssetCacheUsage().ReservedBytes;
				Manager->SetAssetCacheLimit(Reserved, Error);
				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(this->World, this->Input.Get()));
				this->Action->Activate();
				this->Test->TestEqual(TEXT("Async capacity rejection fails once"), this->Receiver->Events, TArray<FName>{TEXT("SurfaceFailed")});
				this->Test->TestEqual(TEXT("Async rejection creates no extra reservation"), Manager->GetAssetCacheUsage().ReservedBytes, Reserved);
				Manager->SetAssetCacheLimit(Limit, Error);

				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(this->World, this->Input.Get(), true));
				this->Action->Activate();
				this->Test->TestTrue(TEXT("Async provider waits for initial publication"), this->Receiver->Events.IsEmpty());
				Manager->Tick(0);
				this->Stage = 1;
				return false;
			}

			this->World->TimeSeconds += 0.1;
			this->World->GetTimerManager().Tick(0.1f);
			auto* Manager = this->World->GetSubsystem<UMeshParticleizerWorldSubsystem>();
			if (this->Stage == 1)
			{
				if (this->Receiver->Events.IsEmpty() == true)
				{
					return false;
				}
				this->Test->TestEqual(TEXT("Publication delivers one Ready"), this->Receiver->Events, TArray<FName>{TEXT("SurfaceReady")});
				auto* Registration = this->Receiver->SurfaceRegistration.Get();
				if (this->Test->TestNotNull(TEXT("Published registration reaches the receiver"), Registration) == false)
				{
					return true;
				}
				this->Test->TestTrue(TEXT("Async provider keeps the requested HDR mode"), Registration->IsHDRProvider());
				auto Pixels = TArray<FLinearColor>{};
				Cast<UTextureRenderTarget2D>(Registration->GetTexture())->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax));
				this->Test->TestTrue(TEXT("Ready follows the HDR GPU copy"), Pixels.IsEmpty() == false && Pixels[0].Equals(this->Input->ClearColor, 0.001f));

				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(this->World, this->Input.Get()));
				this->Action->Activate();
				this->Action->Cancel();
				this->Action->Activate();
				Manager->Tick(0);
				this->Stage = 2;
				return false;
			}
			if (this->Stage == 2)
			{
				this->Test->TestTrue(TEXT("Cancelled wait never publishes a delegate"), this->Receiver->Events.IsEmpty());
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
				this->Test->TestEqual(TEXT("Completed and cancelled actions release their cache users"), Manager->GetAssetCacheUsage().PinnedBytes, int64{0});
				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(this->World, this->Input.Get()));
				this->Action->Activate();
				this->World->TimeSeconds += 31;
				this->Stage = 3;
				return false;
			}
			if (this->Stage == 3)
			{
				if (this->Receiver->Events.IsEmpty() == true)
				{
					return false;
				}
				this->Test->TestEqual(TEXT("Unpublished input reaches one timeout failure"), this->Receiver->Events, TArray<FName>{TEXT("SurfaceFailed")});
				this->Test->TestTrue(TEXT("Timeout explains the wait"), this->Receiver->FailureMessage.Contains(TEXT("timed out")));
				Manager->Tick(0);

				// 初期化済みワールドを終了し、保持中の通常Textureも同じ寿命診断を返す。
				auto Resident = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(16, 16));
				Resident->UpdateResource();
				FlushRenderingCommands();
				auto Error = FString{};
				auto Retained = TStrongObjectPtr<UMeshParticleizerSurfaceTexture>(Manager->RegisterSurfaceTexture(Resident.Get(), Error));
				if (this->Test->TestNotNull(TEXT("Retained texture registration exists"), Retained.Get()) == false)
				{
					return true;
				}
				this->Test->TestTrue(TEXT("Retained texture is ready before cleanup"), Retained->IsReady(Error));
				this->Bind(UMeshParticleizerSurfaceTextureAsyncAction::RegisterColorProvider(this->World, this->Input.Get()));
				this->Action->Activate();
				this->World->DestroyWorld(false);
				GEngine->DestroyWorldContext(this->World);
				this->World = nullptr;
				this->Test->TestFalse(TEXT("Retained texture is not ready after world cleanup"), Retained->IsReady(Error));
				this->Test->TestTrue(TEXT("Expired registration explains its world lifetime"), Error.Contains(TEXT("world")));
				this->Action->Activate();
				this->Test->TestTrue(TEXT("World cleanup cancels pending callbacks"), this->Receiver->Events.IsEmpty());
				return true;
			}
			return false;
		}

	private:
		void Bind(UMeshParticleizerSurfaceTextureAsyncAction* Request)
		{
			this->Receiver->Events.Reset();
			this->Receiver->FailureMessage.Reset();
			this->Receiver->SurfaceRegistration = nullptr;
			this->Action.Reset(Request);
			Request->Ready.AddDynamic(this->Receiver.Get(), &UMeshParticleizerTestReceiver::SurfaceReady);
			Request->Failed.AddDynamic(this->Receiver.Get(), &UMeshParticleizerTestReceiver::SurfaceFailed);
		}

		FAutomationTestBase* Test = nullptr;
		UWorld* World = nullptr;
		TStrongObjectPtr<UTextureRenderTarget2D> Input;
		TStrongObjectPtr<UMeshParticleizerSurfaceTextureAsyncAction> Action;
		TStrongObjectPtr<UMeshParticleizerTestReceiver> Receiver;
		double Deadline = FPlatformTime::Seconds() + 30;
		int32 Stage = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerLifecycleTest, "MeshParticleizer.Runtime.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerLifecycleTest::RunTest(const FString& Parameters)
{
	// 通知、所有権、復元を実際のUObjectで検証する。描画の受入は別の実機試験で行う。
	auto* System = LoadObject<UNiagaraSystem>(nullptr, TEXT("/MeshParticleizer/Core/NS_MeshParticleizer.NS_MeshParticleizer"));
	if (this->TestNotNull(TEXT("Plugin GPU system exists"), System) == false)
	{
		return false;
	}
	System->WaitForCompilationComplete(true, false);
	if (this->TestTrue(TEXT("GPU system is valid"), System->IsValid()) == false)
	{
		return false;
	}

	auto* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("MeshParticleizerLifecycle"));
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
	};
	auto* Manager = World->GetSubsystem<UMeshParticleizerWorldSubsystem>();

	// GDF設定の不足を観測待ちと区別し、既存予約を二重計上しない容量下限を確認する。
	{
		auto* Updates = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AOUpdateGlobalDistanceField"));
		const auto OriginalUpdates = Updates->GetInt();
		ON_SCOPE_EXIT
		{
			Updates->SetWithCurrentPriority(OriginalUpdates);
		};
		Updates->SetWithCurrentPriority(0);
		auto Usage = Manager->GetBudgetUsage();

		// 開発プロジェクトはGenerate Mesh Distance FieldsとGDFを有効にしている。
		this->TestEqual(TEXT("Disabled updates have a specific status"), Usage.GdfStatus, EMeshParticleizerGdfStatus::UpdatesDisabled);
		this->TestTrue(TEXT("Disabled updates identify the configuration to change"), Usage.GdfDiagnostics.Contains(TEXT("r.AOUpdateGlobalDistanceField")));
		this->TestEqual(TEXT("Unavailable GDF does not publish a capacity estimate"), Usage.GdfMinimumMemoryLimitBytes, int64{0});
		Updates->SetWithCurrentPriority(1);
		this->TestEqual(TEXT("Reading usage does not start observation"), Manager->GetBudgetUsage().GdfStatus, EMeshParticleizerGdfStatus::NotObserved);

		const auto Field = Manager->GetDistanceField();
		this->TestEqual(TEXT("An unrendered observation remains pending"), Manager->GetBudgetUsage().GdfStatus, EMeshParticleizerGdfStatus::WaitingForRender);
		Field->Frame.store(1);
		this->TestEqual(TEXT("A rendered view without textures is unavailable"), Manager->GetBudgetUsage().GdfStatus, EMeshParticleizerGdfStatus::Unavailable);
		Field->TextureBytes.store(70ll * 1024 * 1024);
		Usage = Manager->GetBudgetUsage();
		this->TestEqual(TEXT("GDF query textures allow readiness"), Usage.GdfStatus, EMeshParticleizerGdfStatus::Ready);
		this->TestEqual(TEXT("GDF lower bound includes the retiring generation"), Usage.GdfMinimumMemoryLimitBytes, 140ll * 1024 * 1024);
		this->TestTrue(TEXT("GDF estimate explains additional capacity"), Usage.GdfDiagnostics.Contains(TEXT("Additional particle and rendering buffers")));
		this->TestEqual(TEXT("Reading readiness reserves no memory"), Usage.ReservedSimulationBytes, int64{0});
		Field->TextureBytes.store(0);
		Field->Frame.store(0);
	}

	auto* Actor = World->SpawnActor<AStaticMeshActor>();
	auto* Mesh = Actor->GetStaticMeshComponent();
	Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	auto* Original = Mesh->GetMaterial(0);

	auto Data = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	auto Texture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(1, 1));
	Data->SourceMesh = Mesh->GetStaticMesh();
	Data->DataVersion = UMeshParticleizerPreparedMesh::CurrentDataVersion;
	Data->ParticleSystem = System;
	Data->GdfParticleSystem = System;
	Data->SourceFingerprint = Mesh->GetStaticMesh()->GetRenderData()->DerivedDataKey;
	Data->SampleCount = 1;
	Data->SourceMaterials.Add(Original);
	Data->DissolveMaterials.Add(Original);
	Data->TextureColorSources.AddDefaulted_GetRef().bDependenciesAnalyzed = true;
	Data->TextureColorSources[0].DependencyAnalysisVersion = FMeshParticleizerTextureColorSource::CurrentDependencyAnalysisVersion;
	Data->Positions = Texture.Get();
	Data->Normals = Texture.Get();
	Data->Colors = Texture.Get();
	auto Settings = FMeshParticleizeOptions{};
	Settings.ConversionDuration = 0;
	Settings.ParticleLifetime = 0;
	Settings.MaxParticles = 0;
	Settings.Ground = EMeshParticleizerGround::Disabled;
	auto Error = FString{};
	if (this->TestTrue(TEXT("Fixture captures shared resource metadata"), Data->CaptureResourceManifest(Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	if (Data->CapturePreparationDependencies(Error) == false)
	{
		this->AddError(Error);
		return false;
	}

	// 専用プレビューのWorldにも管理を生成し、Configure時の履歴登録を成立させる。
	{
		auto* PreviewWorld = UWorld::CreateWorld(EWorldType::GamePreview, false, TEXT("MeshParticleizerPreviewLifecycle"));
		GEngine->CreateNewWorldContext(EWorldType::GamePreview).SetCurrentWorld(PreviewWorld);
		ON_SCOPE_EXIT
		{
			PreviewWorld->EndPlay(EEndPlayReason::Quit);
			PreviewWorld->DestroyWorld(false);
			GEngine->DestroyWorldContext(PreviewWorld);
		};
		PreviewWorld->InitializeActorsForPlay(FURL{});
		PreviewWorld->BeginPlay();
		PreviewWorld->GetWorldSettings()->NotifyBeginPlay();
		auto* PreviewManager = PreviewWorld->GetSubsystem<UMeshParticleizerWorldSubsystem>();
		if (this->TestNotNull(TEXT("Game preview owns its runtime manager"), PreviewManager) == false)
		{
			return false;
		}
		this->TestNotEqual(TEXT("Preview budgets are isolated from the game world"), PreviewManager, Manager);
		auto* PreviewActor = PreviewWorld->SpawnActor<AStaticMeshActor>();
		auto* PreviewMesh = PreviewActor->GetStaticMeshComponent();
		PreviewMesh->SetMobility(EComponentMobility::Movable);
		PreviewMesh->SetStaticMesh(Mesh->GetStaticMesh());
		auto* PreviewPlayer = NewObject<UMeshParticleizerComponent>(PreviewActor);
		PreviewActor->AddInstanceComponent(PreviewPlayer);
		PreviewPlayer->RegisterComponent();
		this->TestTrue(TEXT("Preview configuration registers source tracking"), PreviewPlayer->Configure(PreviewMesh, Data.Get(), Error));
		this->TestNotNull(TEXT("Preview source history is available"), PreviewManager->FindSourceHistory(PreviewMesh));
		PreviewPlayer->DestroyComponent();
		this->TestNull(TEXT("Closing preview tracking unregisters its history"), PreviewManager->FindSourceHistory(PreviewMesh));
	}

	// 旧解析結果で品質依存の穴を見逃さず、再解析前は元の表示を保持する。
	Data->TextureColorSources[0].DependencyAnalysisVersion = 0;
	this->TestNull(TEXT("Legacy material analysis requires an explicit upgrade"), Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	this->TestTrue(TEXT("Legacy analysis rejection explains recovery"), Error.Contains(TEXT("outdated dependency analysis")) && Error.Contains(TEXT("Upgrade")));
	this->TestEqual(TEXT("Legacy analysis rejection preserves the original material"), Mesh->GetMaterial(0), Original);
	Data->TextureColorSources[0].DependencyAnalysisVersion = FMeshParticleizerTextureColorSource::CurrentDependencyAnalysisVersion;

	// 元発光Textureも同じ共有資産契約へ含め、未準備の継承要求は予約前に拒否する。
	auto EmissionTexture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(512, 1, PF_A32B32G32R32F));
	EmissionTexture->SRGB = false;
	Data->SourceEmission = EmissionTexture.Get();
	this->TestFalse(TEXT("Original emission requires its own resource manifest entry"), Data->ValidateResourceManifest(Error));
	if (Data->CaptureResourceManifest(Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Original emission is charged as a simulation texture"), Data->Resources.ContainsByPredicate([&EmissionTexture](const auto& Resource) {
		return Resource.Asset.Get() == EmissionTexture.Get() && Resource.GpuBytes > 0 && Resource.bDisplayResource == false;
	}));
	// 現在のシェーダーが使わなくても、親材質に残るテクスチャ参照を見落とさない。
	auto UnusedTexture = TStrongObjectPtr<UTexture2D>(UTexture2D::CreateTransient(1, 1));
	auto ParentMaterial = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	auto ChildMaterial = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	ParentMaterial->SetParentEditorOnly(Original);
	ChildMaterial->SetParentEditorOnly(ParentMaterial.Get());
	auto Override = FTextureParameterValue{};
	Override.ParameterInfo = FMaterialParameterInfo(TEXT("CacheReferenceFixture"));
	Override.ParameterValue = UnusedTexture.Get();
	ParentMaterial->TextureParameterValues.Add(Override);
	Data->DissolveMaterials[0] = ChildMaterial.Get();
	if (this->TestTrue(TEXT("Manifest captures material references without a matching shader parameter"), Data->CaptureResourceManifest(Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Parent override texture remains a protected display dependency"), Data->Resources.ContainsByPredicate([&UnusedTexture](const auto& Resource) {
		return Resource.Asset.Get() == UnusedTexture.Get() && Resource.bDisplayResource == true;
	}));
	Data->DissolveMaterials[0] = Original;
	if (Data->CaptureResourceManifest(Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto Receiver = TStrongObjectPtr<UMeshParticleizerTestReceiver>(NewObject<UMeshParticleizerTestReceiver>());

	// 同期準備の成功条件として、テスト用の位置・色・発光TextureもGPU初期化を済ませる。
	Texture->UpdateResource();
	EmissionTexture->UpdateResource();
	FlushRenderingCommands();

	// 共有資産は開始前に一度だけ計上し、最後の利用者が離れるまで保護する。
	const auto CacheLimit = Manager->GetAssetCacheUsage().LimitBytes;
	this->TestTrue(TEXT("Empty cache accepts a zero limit"), Manager->SetAssetCacheLimit(0, Error));
	this->TestNull(TEXT("Cache capacity rejects before changing source materials"), Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	this->TestEqual(TEXT("Cache rejection preserves source material"), Mesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Rejected registration reserves no assets"), Manager->GetAssetCacheUsage().CachedAssets, 0);
	this->TestFalse(TEXT("Negative cache limit is rejected"), Manager->SetAssetCacheLimit(-1, Error));
	Manager->SetAssetCacheLimit(CacheLimit, Error);
	auto ExpectedBytes = int64{0};
	auto DisplayBytes = int64{0};
	for (const auto& Resource : Data->Resources)
	{
		ExpectedBytes += Resource.GpuBytes;
		if (Resource.bDisplayResource == true)
		{
			DisplayBytes += Resource.GpuBytes;
		}
	}
	const auto ValidManifest = Data->Resources;
	Data->Resources.RemoveAll([&Texture](const auto& Resource) {
		return Resource.Asset.Get() == Texture.Get();
	});
	this->TestNull(TEXT("Missing resource metadata rejects preparation"), Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	Data->Resources = ValidManifest;
	for (auto& Resource : Data->Resources)
	{
		if (Resource.Asset.Get() == Texture.Get())
		{
			Resource.GpuBytes = 0;
		}
	}
	this->TestNull(TEXT("Understated texture capacity rejects preparation"), Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	this->TestEqual(TEXT("Invalid manifests create no cache entries"), Manager->GetAssetCacheUsage().CachedAssets, 0);
	Data->Resources = ValidManifest;
	auto* CacheActor = World->SpawnActor<AStaticMeshActor>();
	auto* CacheMesh = CacheActor->GetStaticMeshComponent();
	CacheMesh->SetStaticMesh(Mesh->GetStaticMesh());
	auto CacheFirst = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	auto CacheSecond = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleize(CacheMesh, Data.Get(), Error));
	if (this->TestNotNull(TEXT("First cache owner prepares"), CacheFirst.Get()) == false ||
			this->TestNotNull(TEXT("Second cache owner prepares"), CacheSecond.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("Shared assets are counted once across owners and material slots"), Manager->GetAssetCacheUsage().ReservedBytes, ExpectedBytes);
	this->TestEqual(TEXT("Preparing pins the entire shared manifest"), Manager->GetAssetCacheUsage().PinnedBytes, ExpectedBytes);
	this->TestFalse(TEXT("Cache limit cannot drop below protected assets"), Manager->SetAssetCacheLimit(ExpectedBytes - 1, Error));
	CacheFirst->RestoreMesh();
	Manager->TrimUnusedAssets();
	this->TestEqual(TEXT("One owner protects resources after another restores"), Manager->GetAssetCacheUsage().PinnedBytes, ExpectedBytes);
	this->TestEqual(TEXT("Trim does not retire pinned assets"), Manager->GetAssetCacheUsage().RetiringBytes, int64{0});
	CacheSecond->RestoreMesh();
	this->TestEqual(TEXT("Restoring all prepared owners removes their pins"), Manager->GetAssetCacheUsage().PinnedBytes, int64{0});
	Manager->TrimUnusedAssets();
	this->TestEqual(TEXT("Evicted texture capacity remains charged until native destruction"), Manager->GetAssetCacheUsage().RetiringBytes, ExpectedBytes);
	CacheFirst.Reset(Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	if (this->TestNotNull(TEXT("A still-live retiring resource can be reacquired"), CacheFirst.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("Reacquisition does not double the reservation"), Manager->GetAssetCacheUsage().ReservedBytes, ExpectedBytes);
	this->TestEqual(TEXT("Reacquisition cancels retirement"), Manager->GetAssetCacheUsage().RetiringBytes, int64{0});
	CacheFirst->RestoreMesh();
	CacheActor->Destroy();

	Data->SourceEmission.Reset();
	if (Data->CaptureResourceManifest(Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto EmissionSettings = Settings;
	EmissionSettings.bInheritSourceEmission = true;

	const auto BeforeEmission = Manager->GetBudgetUsage().ReservedSimulationBytes;
	this->TestNull(TEXT("Missing HDR preparation rejects original emission playback"), Manager->StartParticleize(Mesh, Data.Get(), EmissionSettings, Error));
	this->TestTrue(TEXT("Missing HDR preparation has an actionable error"), Error.Contains(TEXT("HDR-prepared")));
	this->TestEqual(TEXT("Rejected HDR playback makes no GPU reservation"), Manager->GetBudgetUsage().ReservedSimulationBytes, BeforeEmission);
	this->TestEqual(TEXT("Rejected HDR playback restores the source material"), Mesh->GetMaterial(0), Original);

	// 時間は変えず、対象上限と要求側の品質を開始時に適用する。寿命0なのでGPUを作らない。
	Data->SampleCount = 262144;
	auto QualitySettings = Settings;
	QualitySettings.ConversionDuration = 2;
	QualitySettings.MaxParticles = 300000;
	QualitySettings.Quality = EMeshParticleizerQuality::High;
	const EMeshParticleizerQuality Qualities[] = {EMeshParticleizerQuality::Fast, EMeshParticleizerQuality::Balanced, EMeshParticleizerQuality::High};
	const int32 ExpectedCounts[] = {10000, 100000, 250000};
	for (auto Index = 0; Index < UE_ARRAY_COUNT(Qualities); ++Index)
	{
		this->TestTrue(TEXT("Idle world accepts quality setting"), Manager->SetQuality(Qualities[Index], Error));
		auto* QualityOperation = Manager->StartParticleize(Mesh, Data.Get(), QualitySettings, Error);
		if (this->TestNotNull(TEXT("Quality-limited request starts"), QualityOperation) == false)
		{
			this->AddError(Error);
			return false;
		}
		const auto Result = QualityOperation->GetResult();
		this->TestTrue(TEXT("Quality reduction is available as structured data"), Result.bSucceeded && Result.bReducedQuality);
		this->TestEqual(TEXT("Requested count is preserved"), Result.RequestedParticles, 300000);
		this->TestEqual(TEXT("Target count follows the world tier"), Result.GrantedParticles, ExpectedCounts[Index]);
		this->TestEqual(TEXT("Result exposes the effective quality"), Result.AppliedQuality, Qualities[Index]);
		if (Qualities[Index] != EMeshParticleizerQuality::Fast)
		{
			this->TestFalse(TEXT("Per-target reservations prevent unsafe downgrade"), Manager->SetQuality(EMeshParticleizerQuality::Fast, Error));
			this->TestEqual(TEXT("Rejected switch preserves world quality"), Manager->GetBudgetUsage().Quality, Qualities[Index]);
		}
		const auto QualityStartTime = World->TimeSeconds;
		World->TimeSeconds += 1;
		Manager->Tick(1);
		this->TestNearlyEqual(TEXT("Quality reduction preserves conversion duration"), QualityOperation->GetProgress(), 0.5f, 0.000001f);
		QualityOperation->Cancel(true);
		World->TimeSeconds = QualityStartTime;
	}
	QualitySettings.Quality = EMeshParticleizerQuality::Fast;
	auto* FastRequest = Manager->StartParticleize(Mesh, Data.Get(), QualitySettings, Error);
	if (this->TestNotNull(TEXT("Fast request starts inside High world"), FastRequest) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("Request quality cannot consume a higher target budget"), FastRequest->GetGrantedParticleCount(), 10000);
	this->TestEqual(TEXT("Request quality never changes world quality"), Manager->GetBudgetUsage().Quality, EMeshParticleizerQuality::High);
	FastRequest->Cancel(true);
	QualitySettings.Quality = static_cast<EMeshParticleizerQuality>(255);
	this->TestNull(TEXT("Invalid quality rejects before changing materials"), Manager->StartParticleize(Mesh, Data.Get(), QualitySettings, Error));
	this->TestEqual(TEXT("Rejected quality leaves source materials intact"), Mesh->GetMaterial(0), Original);
	Manager->SetQuality(EMeshParticleizerQuality::Balanced, Error);
	Data->SampleCount = 1;

	// 不正なカーブは材質変更より先に拒否し、準備した曲線はアセットの後編集から切り離す。
	auto CurveAsset = TStrongObjectPtr<UCurveFloat>(NewObject<UCurveFloat>());
	CurveAsset->FloatCurve.SetKeys({FRichCurveKey(0, 0), FRichCurveKey(0.5f, 0.75f), FRichCurveKey(0.75f, 0.5f), FRichCurveKey(1, 1)});
	auto CurvedSettings = Settings;
	CurvedSettings.ProgressCurve = CurveAsset.Get();
	CurvedSettings.ConversionDuration = 2;
	auto Target = FMeshParticleizerTarget{};
	Target.Mesh = Mesh;
	Target.PreparedData = Data.Get();

	// 複数起点は最短距離を共通の範囲で正規化する。範囲の角だけでは中央の最大値を見落とす。
	auto PointSettings = Settings;
	this->TestFalse(TEXT("Empty point list rejects atomically"), UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings, {}, Error));
	this->TestEqual(TEXT("Rejected point setter preserves mode"), PointSettings.Origin, Settings.Origin);
	this->TestTrue(TEXT("Two point origins validate"), UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings, {FVector(-1, 0, 0), FVector(1, 0, 0)}, Error));
	const auto ValidPoints = PointSettings.LocalOrigins;
	this->TestFalse(TEXT("Fifth origin is rejected"), UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings, {FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector}, Error));
	this->TestTrue(TEXT("Rejected point setter preserves all points"), PointSettings.LocalOrigins == ValidPoints);
	this->TestFalse(TEXT("Nonfinite origin is rejected"), UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings, {FVector(std::numeric_limits<double>::quiet_NaN(), 0, 0)}, Error));
	auto PointSpace = FMeshParticleizerProgressSpace{};
	PointSpace.Bounds = FBox(FVector(-1), FVector(1));
	auto PointResults = TArray<FMeshParticleizerResolvedOrigin>{};
	if (FMeshParticleizerResolvedOrigin::Resolve({Target}, PointSettings, {PointSpace}, PointResults, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestNearlyEqual(TEXT("Nearest-distance maximum includes Voronoi face vertices"), PointResults[0].ReferenceDistance, FMath::Sqrt(3.0f), 0.000001f);
	this->TestTrue(TEXT("Resolved points preserve reference coordinates"), PointResults[0].ReferencePoints == ValidPoints);
	UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings, {FVector(3, 0, 0)}, Error);
	if (FMeshParticleizerResolvedOrigin::Resolve({Target}, PointSettings, {PointSpace}, PointResults, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestNearlyEqual(TEXT("One origin agrees with single-point bounds distance"), PointResults[0].ReferenceDistance, FMath::Sqrt(18.0f), 0.000001f);
	UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings, {FVector(3, 0, 0), FVector(3, 0, 0)}, Error);
	if (FMeshParticleizerResolvedOrigin::Resolve({Target}, PointSettings, {PointSpace}, PointResults, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestNearlyEqual(TEXT("Coincident origins preserve the same field"), PointResults[0].ReferenceDistance, FMath::Sqrt(18.0f), 0.000001f);
	PointSpace.Bounds = FBox(FVector(-1, -1, 0), FVector(1, 1, 0));
	UMeshParticleizerOriginLibrary::SetPointOrigins(PointSettings,
		{FVector(-1, -1, 0), FVector(-1, 1, 0), FVector(1, -1, 0), FVector(1, 1, 0)}, Error);
	if (FMeshParticleizerResolvedOrigin::Resolve({Target}, PointSettings, {PointSpace}, PointResults, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestNearlyEqual(TEXT("Flat bounds include interior farthest point"), PointResults[0].ReferenceDistance, FMath::Sqrt(2.0f), 0.000001f);
	const auto ForwardDistance = PointResults[0].ReferenceDistance;
	Swap(PointSettings.LocalOrigins[0], PointSettings.LocalOrigins[3]);
	if (FMeshParticleizerResolvedOrigin::Resolve({Target}, PointSettings, {PointSpace}, PointResults, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestNearlyEqual(TEXT("Point order does not change common radius"), PointResults[0].ReferenceDistance, ForwardDistance, 0.000001f);

	this->TestNull(TEXT("Reversing curve fails preparation"), Manager->PrepareParticleizeTargets({Target}, Error, &CurvedSettings));
	this->TestEqual(TEXT("Rejected curve leaves material untouched"), Mesh->GetMaterial(0), Original);
	CurveAsset->FloatCurve.SetKeys({FRichCurveKey(0, 0), FRichCurveKey(0.5f, 0.25f), FRichCurveKey(1, 1)});
	auto CurveReady = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleizeTargets({Target}, Error, &CurvedSettings));
	if (this->TestNotNull(TEXT("Valid curve prepares"), CurveReady.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto InvalidCurveAsset = TStrongObjectPtr<UCurveFloat>(NewObject<UCurveFloat>());
	auto RejectedSettings = CurvedSettings;
	RejectedSettings.ProgressCurve = InvalidCurveAsset.Get();
	this->TestNull(TEXT("Invalid replacement curve rejects start without losing preparation"),
		Manager->StartPreparedParticleize(CurveReady.Get(), RejectedSettings, Error));
	CurveAsset->FloatCurve.SetKeys({FRichCurveKey(0, 0.2f), FRichCurveKey(1, 0.8f)});
	this->TestEqual(TEXT("Start uses prepared curve snapshot"), Manager->StartPreparedParticleize(CurveReady.Get(), CurvedSettings, Error), CurveReady.Get());
	const auto SavedWorldTime = World->TimeSeconds;
	World->TimeSeconds += 1;
	Manager->Tick(1);
	this->TestNearlyEqual(TEXT("Operation progress follows prepared curve"), CurveReady->GetProgress(), 0.25f, 0.000001f);
	CurveReady->Cancel(true);
	World->TimeSeconds = SavedWorldTime;
	this->TestEqual(TEXT("Curved operation restores material"), Mesh->GetMaterial(0), Original);

	// 準備済みの材質は開始で作り直さず、未開始の解除でも元の参照へ戻す。
	auto Ready = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	if (this->TestNotNull(TEXT("Material preparation accepted"), Ready.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto* ReadyMaterial = Mesh->GetMaterial(0);
	this->TestTrue(TEXT("Preparation owns a new dynamic material"), ReadyMaterial != Original && Cast<UMaterialInstanceDynamic>(ReadyMaterial) != nullptr);
	this->TestTrue(TEXT("Preparation preserves source visibility"), Mesh->IsVisible());
	this->TestEqual(TEXT("Preparation reserves no particles"), Ready->GetGrantedParticleCount(), 0);
	auto* PreparedStart = Manager->StartPreparedParticleize(Ready.Get(), Settings, Error);
	this->TestEqual(TEXT("Start transfers the prepared operation"), PreparedStart, Ready.Get());
	this->TestEqual(TEXT("Start reuses the prepared material"), Mesh->GetMaterial(0), ReadyMaterial);
	Ready->Cancel(true);
	this->TestEqual(TEXT("Prepared cancellation restores original material"), Mesh->GetMaterial(0), Original);

	Ready.Reset(Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	if (this->TestNotNull(TEXT("Unstarted preparation accepted"), Ready.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	Ready->RestoreMesh();
	this->TestEqual(TEXT("Restore before start restores original material"), Mesh->GetMaterial(0), Original);
	this->TestNull(TEXT("Restored preparation cannot start"), Manager->StartPreparedParticleize(Ready.Get(), Settings, Error));

	Ready.Reset(Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	if (this->TestNotNull(TEXT("External edit preparation accepted"), Ready.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto ExternalParent = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	ExternalParent->SetParentEditorOnly(Original);
	auto ExternalMaterial = TStrongObjectPtr<UMaterialInstanceDynamic>(UMaterialInstanceDynamic::Create(ExternalParent.Get(), Mesh));
	Mesh->SetMaterial(0, ExternalMaterial.Get());
	this->TestNull(TEXT("Changed material rejects prepared start"), Manager->StartPreparedParticleize(Ready.Get(), Settings, Error));
	Ready->RestoreMesh();
	this->TestEqual(TEXT("Restore preserves externally assigned material"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(ExternalMaterial.Get()));
	this->TestNull(TEXT("Mismatched material rejects preparation"), Manager->PrepareParticleize(Mesh, Data.Get(), Error));
	this->TestEqual(TEXT("Rejected preparation leaves external material intact"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(ExternalMaterial.Get()));
	Mesh->SetMaterial(0, Original);

	auto* Immediate = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Immediate operation accepted"), Immediate) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(Immediate);
	this->TestEqual(TEXT("No event before caller can subscribe"), Receiver->Events.Num(), 0);
	Manager->Tick(0);
	this->TestTrue(TEXT("Zero durations preserve event order"), Receiver->Events == TArray<FName>{TEXT("Started"), TEXT("Converted"), TEXT("Finished")});
	this->TestEqual(TEXT("Immediate operation finishes"), Immediate->GetState(), EMeshParticleizeState::Finished);
	this->TestEqual(TEXT("Finished material keeps its display resources pinned"), Manager->GetAssetCacheUsage().PinnedBytes, DisplayBytes);
	Manager->Tick(0);
	this->TestEqual(TEXT("Notifications occur once"), Receiver->Events.Num(), 3);
	Immediate->RestoreMesh();
	this->TestEqual(TEXT("Exact material reference restored"), Mesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Restoring a finished operation releases display pins"), Manager->GetAssetCacheUsage().PinnedBytes, int64{0});

	// 準備済み資産の通知を同フレームへ寄せ、最短経路でも購読と所有権を保つ。
	{
		auto* DelegateDelay = IConsoleManager::Get().FindConsoleVariable(TEXT("s.StreamableDelegateDelayFrames"));
		const auto OldDelay = DelegateDelay->GetInt();
		const auto OldFlags = DelegateDelay->GetFlags();
		const auto OldTime = World->TimeSeconds;
		ON_SCOPE_EXIT
		{
			DelegateDelay->Set(OldDelay, ECVF_SetByConsole);
			DelegateDelay->SetFlags(OldFlags);
			World->TimeSeconds = OldTime;
		};
		DelegateDelay->Set(0, ECVF_SetByConsole);
		auto Player = TStrongObjectPtr<UMeshParticleizerComponent>(NewObject<UMeshParticleizerComponent>(Actor));
		Player->RegisterComponent();
		ON_SCOPE_EXIT
		{
			Player->RestoreMesh();
			Player->DestroyComponent();
		};

		// Provider設定前に準備を完了できる。入力不足は開始時に割当前の段階で拒否する。
		this->TestTrue(TEXT("HDR component can configure before provider binding"), Player->Configure(Mesh, Data.Get(), Error));
		this->TestTrue(TEXT("HDR options are valid before provider binding"), Player->SetOptions(EmissionSettings, Error));
		Player->PrepareForParticleize();
		FlushAsyncLoading();
		this->TestEqual(TEXT("Component reaches ready before HDR registration"), Player->GetPreparationState(), EMeshParticleizerPreparationState::Ready);
		this->TestNull(TEXT("Prepared component still rejects missing HDR input"), Player->StartParticleize(Error));
		this->TestTrue(TEXT("Missing prepared HDR input has an actionable diagnostic"), Error.Contains(TEXT("HDR-prepared")));
		Player->RestoreMesh();

		auto Preset = TStrongObjectPtr<UMeshParticleizerPreset>(NewObject<UMeshParticleizerPreset>());
		auto Defaults = Settings;
		Defaults.ConversionDuration = 4;
		Defaults.ParticleLifetime = 5;
		this->TestTrue(TEXT("Basic API preset validates"), Preset->SetOptions(Defaults, Error));
		auto Events = TStrongObjectPtr<UMeshParticleizerTestReceiver>(NewObject<UMeshParticleizerTestReceiver>());
		auto Prepare = TStrongObjectPtr<UMeshParticleizePrepareAsyncAction>(UMeshParticleizePrepareAsyncAction::PrepareForParticleize(
			Player.Get(), Mesh, Data.Get(), Preset.Get()));
		Prepare->Prepared.AddDynamic(Events.Get(), &UMeshParticleizerTestReceiver::Prepared);
		Prepare->Failed.AddDynamic(Events.Get(), &UMeshParticleizerTestReceiver::PreparationFailed);
		Preset->Options.ConversionDuration = 99;
		Prepare->Activate();
		FlushAsyncLoading();
		if (this->TestEqual(TEXT("Basic async preparation reaches ready"), Player->GetPreparationState(), EMeshParticleizerPreparationState::Ready) == false)
		{
			this->AddError(Player->GetPreparationDiagnostics());
			return false;
		}
		this->TestEqual(TEXT("Preparation snapshots the requested preset"), Player->Options.ConversionDuration, 4.f);
		this->TestTrue(TEXT("Prepared notification occurs once"), Events->Events == TArray<FName>{TEXT("Prepared")});
		Prepare->Activate();
		this->TestEqual(TEXT("Repeated activation does not repeat preparation"), Events->Events.Num(), 1);

		// 登録の失敗では、準備済みの表示・登録・通知を巻き戻さない。
		auto RegisteredTarget = FMeshParticleizerTarget{};
		RegisteredTarget.Mesh = Mesh;
		RegisteredTarget.PreparedData = Data.Get();
		auto* PreparedMaterial = Mesh->GetMaterial(0);
		this->TestFalse(TEXT("Duplicate configuration is rejected while ready"), Player->ConfigureTargets({RegisteredTarget, RegisteredTarget}, Error));
		this->TestTrue(TEXT("Duplicate diagnostic identifies the array entry and mesh"),
			Error.Contains(TEXT("Source 1")) && Error.Contains(Mesh->GetName()) && Error.Contains(TEXT("already registered")));
		RegisteredTarget.Importance = std::numeric_limits<float>::quiet_NaN();
		this->TestFalse(TEXT("Non-finite importance is rejected while ready"), Player->ConfigureTargets({RegisteredTarget}, Error));
		this->TestTrue(TEXT("Importance diagnostic identifies the source and reason"), Error.Contains(TEXT("Source 0")) && Error.Contains(TEXT("Importance")));
		RegisteredTarget.Importance = 1;
		RegisteredTarget.PreparedData.Reset();
		this->TestFalse(TEXT("Missing prepared data is rejected while ready"), Player->ConfigureTargets({RegisteredTarget}, Error));
		this->TestTrue(TEXT("Missing data diagnostic identifies the source and reason"), Error.Contains(TEXT("Source 0")) && Error.Contains(TEXT("prepared data")));
		this->TestEqual(TEXT("Rejected configuration keeps ready state"), Player->GetPreparationState(), EMeshParticleizerPreparationState::Ready);
		this->TestEqual(TEXT("Rejected configuration preserves prepared asset"), Player->PreparedData.Get(), Data.Get());
		this->TestEqual(TEXT("Rejected configuration preserves prepared material"), Mesh->GetMaterial(0), PreparedMaterial);
		this->TestEqual(TEXT("Rejected configuration emits no preparation event"), Events->Events.Num(), 1);

		// 二つの時間は個別に上書きでき、起点は要求時点のコピーを使う。
		auto Origin = FMeshParticleizerOriginOverride{};
		Origin.bOverride = true;
		Origin.Mode = EMeshParticleizerOrigin::Point;
		Origin.LocalOrigin = FVector(1, 2, 3);
		auto Start = TStrongObjectPtr<UMeshParticleizeAsyncAction>(UMeshParticleizeAsyncAction::StartParticleize(
			Player.Get(), 0, 0, nullptr, Origin));
		Start->Started.AddDynamic(Events.Get(), &UMeshParticleizerTestReceiver::AsyncStarted);
		Start->MeshConverted.AddDynamic(Events.Get(), &UMeshParticleizerTestReceiver::AsyncConverted);
		Start->Finished.AddDynamic(Events.Get(), &UMeshParticleizerTestReceiver::AsyncFinished);
		Start->Failed.AddDynamic(Events.Get(), &UMeshParticleizerTestReceiver::Failed);
		Origin.LocalOrigin = FVector(99, 99, 99);
		Start->Activate();
		FlushAsyncLoading();
		auto* Started = Player->GetLastOperation();
		if (this->TestNotNull(TEXT("Registered component returns its operation"), Started) == false)
		{
			this->AddError(Events->FailureMessage);
			return false;
		}
		this->TestEqual(TEXT("A consumed preparation is not advertised as ready"), Player->GetPreparationState(), EMeshParticleizerPreparationState::Unprepared);
		this->TestEqual(TEXT("Start does not notify before subscriptions"), Events->Events.Num(), 1);
		Start->Activate();
		Manager->Tick(0);
		this->TestTrue(TEXT("Zero times keep all async events in order"), Events->Events == TArray<FName>{TEXT("Prepared"), TEXT("Started"), TEXT("Converted"), TEXT("Finished")});
		this->TestEqual(TEXT("Origin input is copied"), Started->GetResolvedOrigin().ReferencePosition, FVector(1, 2, 3));
		this->TestTrue(TEXT("Async action does not destroy the caller's component"), Player->IsRegistered());
		Player->RestoreMesh();
		this->TestEqual(TEXT("Caller can restore the original material"), Mesh->GetMaterial(0), Original);

		auto Running = TStrongObjectPtr<UMeshParticleizeAsyncAction>(UMeshParticleizeAsyncAction::StartParticleize(
			Player.Get(), 4, 5, nullptr, FMeshParticleizerOriginOverride{}));
		Running->Activate();
		FlushAsyncLoading();
		auto* Active = Player->GetLastOperation();
		if (this->TestTrue(TEXT("Caller component can replay after restore"), Active != nullptr && Active != Started) == false)
		{
			return false;
		}
		auto DuplicateEvents = TStrongObjectPtr<UMeshParticleizerTestReceiver>(NewObject<UMeshParticleizerTestReceiver>());
		auto DuplicateStart = TStrongObjectPtr<UMeshParticleizeAsyncAction>(UMeshParticleizeAsyncAction::StartParticleize(
			Player.Get(), 99, 99, nullptr, FMeshParticleizerOriginOverride{}));
		DuplicateStart->Failed.AddDynamic(DuplicateEvents.Get(), &UMeshParticleizerTestReceiver::Failed);
		DuplicateStart->Activate();
		this->TestEqual(TEXT("Duplicate async start returns the active handle"), DuplicateEvents->FailedOperation.Get(), Active);
		this->TestEqual(TEXT("Duplicate start preserves registered settings"), Player->Options.ConversionDuration, 4.f);

		auto State = EMeshParticleizeState::Failed;
		auto Progress = 0.f;
		auto Remaining = 0.f;
		Active->GetParticleizeStatus(State, Progress, Remaining);
		this->TestEqual(TEXT("Status includes both logical durations"), Remaining, 9.f);
		World->TimeSeconds += 2;
		Manager->Tick(0);
		Active->GetParticleizeStatus(State, Progress, Remaining);
		this->TestEqual(TEXT("Status uses world logical time"), Remaining, 7.f);
		this->TestEqual(TEXT("Status shares the operation progress"), Progress, Active->GetProgress());
		Active->Cancel(true);
		Active->GetParticleizeStatus(State, Progress, Remaining);
		this->TestEqual(TEXT("Cancelled status has no remaining lifetime"), Remaining, 0.f);

		// 同じ設定でも次の準備を作り直す。消費済みReadyの短絡でGDF待機を飛ばさない。
		auto ReplayEvents = TStrongObjectPtr<UMeshParticleizerTestReceiver>(NewObject<UMeshParticleizerTestReceiver>());
		auto ReplayPrepare = TStrongObjectPtr<UMeshParticleizePrepareAsyncAction>(UMeshParticleizePrepareAsyncAction::PrepareForParticleize(Player.Get()));
		ReplayPrepare->Prepared.AddDynamic(ReplayEvents.Get(), &UMeshParticleizerTestReceiver::Prepared);
		ReplayPrepare->Activate();
		FlushAsyncLoading();
		this->TestEqual(TEXT("Unchanged settings prepare a new operation"), Player->GetPreparationState(), EMeshParticleizerPreparationState::Ready);
		this->TestTrue(TEXT("Repeated preparation notifies once"), ReplayEvents->Events == TArray<FName>{TEXT("Prepared")});
		auto* ReplayOperation = Player->StartParticleize(Error);
		if (this->TestNotNull(TEXT("Reprepared operation starts"), ReplayOperation) == false)
		{
			this->AddError(Error);
			return false;
		}
		this->TestNotEqual(TEXT("Replay owns a fresh preparation"), ReplayOperation, Active);
		this->TestEqual(TEXT("Replay also consumes preparation"), Player->GetPreparationState(), EMeshParticleizerPreparationState::Unprepared);
		ReplayOperation->Cancel(true);
		Player->RestoreMesh();

		// 不正な時間・起点・Presetでは、既存の対象と設定を変更しない。
		for (const auto InvalidTime : {-0.5f, std::numeric_limits<float>::quiet_NaN()})
		{
			DuplicateEvents->Events.Reset();
			auto Rejected = TStrongObjectPtr<UMeshParticleizeAsyncAction>(UMeshParticleizeAsyncAction::StartParticleize(
				Player.Get(), InvalidTime, -1, nullptr, FMeshParticleizerOriginOverride{}));
			Rejected->Failed.AddDynamic(DuplicateEvents.Get(), &UMeshParticleizerTestReceiver::Failed);
			Rejected->Activate();
			this->TestTrue(TEXT("Invalid time fails once before state mutation"), DuplicateEvents->Events == TArray<FName>{TEXT("Failed")});
			this->TestEqual(TEXT("Rejected time preserves settings"), Player->Options.ConversionDuration, 4.f);
		}
		Origin.Mode = EMeshParticleizerOrigin::MultiplePoints;
		Origin.LocalOrigins.Init(FVector::ZeroVector, 5);
		auto InvalidOrigin = TStrongObjectPtr<UMeshParticleizeAsyncAction>(UMeshParticleizeAsyncAction::StartParticleize(Player.Get(), -1, -1, nullptr, Origin));
		DuplicateEvents->Events.Reset();
		InvalidOrigin->Failed.AddDynamic(DuplicateEvents.Get(), &UMeshParticleizerTestReceiver::Failed);
		InvalidOrigin->Activate();
		this->TestTrue(TEXT("Origin input uses the existing four-point validation"), DuplicateEvents->Events == TArray<FName>{TEXT("Failed")});
		this->TestEqual(TEXT("Rejected origin preserves the component origin"), Player->Options.Origin, EMeshParticleizerOrigin::Point);
		Preset->Options.ParticleLifetime = -2;
		auto InvalidPreset = TStrongObjectPtr<UMeshParticleizePrepareAsyncAction>(UMeshParticleizePrepareAsyncAction::PrepareForParticleize(Player.Get(), Mesh, Data.Get(), Preset.Get()));
		DuplicateEvents->Events.Reset();
		InvalidPreset->Failed.AddDynamic(DuplicateEvents.Get(), &UMeshParticleizerTestReceiver::PreparationFailed);
		InvalidPreset->Activate();
		this->TestTrue(TEXT("Invalid preset fails before reconfiguring"), DuplicateEvents->Events == TArray<FName>{TEXT("PreparationFailed")});
		this->TestEqual(TEXT("Invalid preset preserves the component lifetime"), Player->Options.ParticleLifetime, 5.f);
	}

	Settings.ConversionDuration = 10;
	Settings.ParticleLifetime = 1;
	Receiver->Events.Reset();
	auto* Cancellable = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Cancellable operation accepted"), Cancellable) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(Cancellable);
	auto* Duplicate = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	this->TestEqual(TEXT("Duplicate returns existing handle"), Duplicate, Cancellable);
	this->TestFalse(TEXT("Duplicate explains rejection"), Error.IsEmpty());
	Cancellable->Cancel(true);
	Cancellable->Cancel(true);
	this->TestTrue(TEXT("Cancel and finish each occur once"), Receiver->Events == TArray<FName>{TEXT("Cancelled"), TEXT("Finished")});
	this->TestEqual(TEXT("Cancellation reason retained"), Receiver->Reason, EMeshParticleizeFinishReason::Cancelled);
	this->TestEqual(TEXT("Cancellation restores material"), Mesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Cancellation restores shadow caching"), Mesh->ShadowCacheInvalidationBehavior, EShadowCacheInvalidationBehavior::Auto);

	// 保持計測はMIDの所有権を変えず、通常の検査と復元を使う。
	{
		auto* Retain = IConsoleManager::Get().FindConsoleVariable(TEXT("mp.Profile.RetainSource"));
		if (this->TestNotNull(TEXT("Development profiling control exists"), Retain) == false)
		{
			return false;
		}
		const auto OriginalRetain = Retain->GetInt();
		const auto OriginalFlags = Retain->GetFlags();
		const auto OriginalTime = World->TimeSeconds;
		ON_SCOPE_EXIT
		{
			Retain->Set(OriginalRetain, ECVF_SetByConsole);
			Retain->SetFlags(OriginalFlags);
			World->TimeSeconds = OriginalTime;
		};
		Retain->Set(1, ECVF_SetByConsole);
		auto* Retained = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
		if (this->TestNotNull(TEXT("Runtime-owned retained operation starts"), Retained) == false)
		{
			this->AddError(Error);
			return false;
		}
		auto* RetainedMaterial = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(0));
		Retain->Set(0, ECVF_SetByConsole);
		World->TimeSeconds += 2;
		Manager->Tick(0);
		this->TestEqual(TEXT("Retained source passes input validation"), Retained->GetState(), EMeshParticleizeState::Converting);
		this->TestTrue(TEXT("Operation progress advances while display is retained"), Retained->GetProgress() > 0);
		this->TestEqual(TEXT("Retained operation keeps the owned MID"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(RetainedMaterial));
		this->TestEqual(TEXT("Profiling setting remains latched"), RetainedMaterial->K2_GetScalarParameterValue(TEXT("MP_Progress")), 0.f);
		RetainedMaterial->SetScalarParameterValue(TEXT("MP_Progress"), 0.75f);
		Manager->Tick(0);
		this->TestEqual(TEXT("Profiling cannot bypass external input validation"), Retained->GetState(), EMeshParticleizeState::Draining);
		Retained->Cancel(true);
		this->TestEqual(TEXT("Retained restore returns original material"), Mesh->GetMaterial(0), Original);
	}

	// 固定表示は元Actorの移動に追従せず、解除すると元の表示設定を返す。
	Settings.bFreezeAtStart = true;
	Mesh->SetCastHiddenShadow(true);
	auto* Frozen = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Frozen source accepted"), Frozen) == false)
	{
		this->AddError(Error);
		return false;
	}
	UStaticMeshComponent* FrozenMesh = nullptr;
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		if (It->GetOuter() == World && It->IsRegistered() == true)
		{
			FrozenMesh = *It;
			break;
		}
	}
	if (this->TestNotNull(TEXT("World owns the frozen display"), FrozenMesh) == false)
	{
		Frozen->Cancel(true);
		return false;
	}
	this->TestFalse(TEXT("Live source display is hidden during freeze"), Mesh->GetVisibleFlag());
	this->TestFalse(TEXT("Live hidden shadow is disabled during freeze"), Mesh->bCastHiddenShadow);
	const auto FrozenTransform = FrozenMesh->GetComponentTransform();
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetWorldLocation(FVector(200, 0, 0));
	Manager->Tick(0);
	this->TestTrue(TEXT("Frozen display ignores source movement"), FrozenMesh->GetComponentTransform().Equals(FrozenTransform));
	Frozen->Cancel(true);
	this->TestFalse(TEXT("Cancel destroys frozen display"), FrozenMesh->IsRegistered());
	this->TestTrue(TEXT("Cancel restores source visibility"), Mesh->GetVisibleFlag());
	this->TestTrue(TEXT("Cancel restores original hidden shadow"), Mesh->bCastHiddenShadow);
	this->TestEqual(TEXT("Frozen cancellation restores material"), Mesh->GetMaterial(0), Original);
	Mesh->SetCastHiddenShadow(false);
	Settings.bFreezeAtStart = false;

	Settings.ConversionDuration = 0;
	auto* Reusable = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Reusable operation accepted"), Reusable) == false)
	{
		this->AddError(Error);
		return false;
	}
	Manager->Tick(0);
	Reusable->RestoreMesh();
	auto* Reused = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	this->TestTrue(TEXT("Converted source can start a new operation while old particles live"), Reused != nullptr && Reused != Reusable);
	if (Reused != nullptr)
	{
		Reused->Cancel(true);
	}
	Reusable->Cancel(true);

	// 再生中のメッシュ差し替えは元の対応データを使い続けず、分離済み粒子だけを完了させる。
	Settings.ConversionDuration = 10;
	Settings.ParticleLifetime = 0.1f;
	Settings.bFreezeAtStart = true;
	Receiver->Events.Reset();
	auto* ChangedSource = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Source change operation accepted"), ChangedSource) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(ChangedSource);
	auto* OriginalMesh = Mesh->GetStaticMesh().Get();
	Mesh->SetStaticMesh(nullptr);
	Manager->Tick(0);
	this->TestEqual(TEXT("Changed source drains without further detachment"), ChangedSource->GetState(), EMeshParticleizeState::Draining);
	this->TestEqual(TEXT("Changed source restores owned shadow policy"), Mesh->ShadowCacheInvalidationBehavior, EShadowCacheInvalidationBehavior::Auto);
	this->TestTrue(TEXT("Changed source releases frozen visibility override"), Mesh->GetVisibleFlag());
	World->Tick(LEVELTICK_All, 0.2f);
	Manager->Tick(0);
	this->TestEqual(TEXT("Changed source has a distinct finish reason"), Receiver->Reason, EMeshParticleizeFinishReason::SourceChanged);
	this->TestFalse(TEXT("Changed source does not report complete conversion"), Receiver->Events.Contains(TEXT("Converted")));
	Mesh->SetStaticMesh(OriginalMesh);

	// 外部から置き換えた材質を上書きせず、固定表示も同じ停止経路で解放する。
	auto Replacement = TStrongObjectPtr<UMaterialInstanceDynamic>(UMaterialInstanceDynamic::Create(Original, GetTransientPackage()));
	auto* ChangedMaterial = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Material change operation accepted"), ChangedMaterial) == false)
	{
		this->AddError(Error);
		return false;
	}
	Mesh->SetMaterial(0, Replacement.Get());
	Manager->Tick(0);
	this->TestEqual(TEXT("External material replacement stops new detachment"), ChangedMaterial->GetState(), EMeshParticleizeState::Draining);
	this->TestEqual(TEXT("External replacement remains assigned"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Replacement.Get()));
	this->TestTrue(TEXT("Material change releases frozen visibility"), Mesh->GetVisibleFlag());
	this->TestTrue(TEXT("Material replacement diagnostic names the slot"), ChangedMaterial->GetDiagnostics().Contains(TEXT("Material slot 0")));
	ChangedMaterial->Cancel(true);
	Mesh->SetMaterial(0, Original);

	auto* ChangedFrozenMaterial = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Frozen material change operation accepted"), ChangedFrozenMaterial) == false)
	{
		this->AddError(Error);
		return false;
	}
	UStaticMeshComponent* ChangedFrozenMesh = nullptr;
	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		if (It->GetOuter() == World && It->IsRegistered() == true)
		{
			ChangedFrozenMesh = *It;
			break;
		}
	}
	if (this->TestNotNull(TEXT("Changed frozen display exists"), ChangedFrozenMesh) == false)
	{
		ChangedFrozenMaterial->Cancel(true);
		return false;
	}
	ChangedFrozenMesh->SetMaterial(0, Replacement.Get());
	Manager->Tick(0);
	this->TestEqual(TEXT("Frozen display material change stops detachment"), ChangedFrozenMaterial->GetState(), EMeshParticleizeState::Draining);
	this->TestFalse(TEXT("Changed frozen display is released"), ChangedFrozenMesh->IsRegistered());
	this->TestEqual(TEXT("Frozen material change restores owned source material"), Mesh->GetMaterial(0), Original);
	ChangedFrozenMaterial->Cancel(true);

	// 姿勢を保持していても、元Actorの早期破棄後は未分離部分を描き続けない。
	auto* TemporaryActor = World->SpawnActor<AStaticMeshActor>();
	auto* TemporaryMesh = TemporaryActor->GetStaticMeshComponent();
	TemporaryMesh->SetStaticMesh(OriginalMesh);
	Receiver->Events.Reset();
	auto* LostSource = Manager->StartParticleize(TemporaryMesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Frozen source loss operation accepted"), LostSource) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(LostSource);
	TemporaryActor->Destroy();
	Manager->Tick(0);
	this->TestEqual(TEXT("Destroyed frozen source stops new detachment"), LostSource->GetState(), EMeshParticleizeState::Draining);
	World->Tick(LEVELTICK_All, 0.2f);
	Manager->Tick(0);
	this->TestEqual(TEXT("Destroyed frozen source reports source loss"), Receiver->Reason, EMeshParticleizeFinishReason::SourceLost);
	this->TestFalse(TEXT("Destroyed frozen source never reports conversion"), Receiver->Events.Contains(TEXT("Converted")));

	Settings.ConversionDuration = 0;
	Settings.ParticleLifetime = 0.1f;
	Settings.bFreezeAtStart = false;
	Receiver->Events.Reset();
	Receiver->DestroyOnConverted = Actor;
	auto* Independent = Manager->StartParticleize(Mesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Independent operation accepted"), Independent) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(Independent);
	Manager->Tick(0);
	this->TestTrue(TEXT("Source destroyed by converted callback"), Actor->IsActorBeingDestroyed());
	this->TestEqual(TEXT("Operation survives source destruction"), Independent->GetState(), EMeshParticleizeState::Converted);
	World->Tick(LEVELTICK_All, 0.2f);
	Manager->Tick(0);
	this->TestEqual(TEXT("World-owned operation finishes"), Independent->GetState(), EMeshParticleizeState::Finished);
	this->TestEqual(TEXT("Destroy after conversion is normal completion"), Receiver->Reason, EMeshParticleizeFinishReason::Completed);

	// 二つの部品が本体を共有しても、片方の終了では本体の更新設定を戻さない。
	auto* ModularActor = World->SpawnActor<AActor>();
	auto* Leader = NewObject<USkeletalMeshComponent>(ModularActor);
	auto* FirstPart = NewObject<USkeletalMeshComponent>(ModularActor);
	auto* SecondPart = NewObject<USkeletalMeshComponent>(ModularActor);
	auto* SkeletalCube = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (this->TestNotNull(TEXT("Engine skeletal fixture exists"), SkeletalCube) == false)
	{
		return false;
	}
	Leader->SetSkeletalMesh(SkeletalCube);
	FirstPart->SetSkeletalMesh(SkeletalCube);
	SecondPart->SetSkeletalMesh(SkeletalCube);
	Leader->RegisterComponent();
	FirstPart->RegisterComponent();
	SecondPart->RegisterComponent();
	Leader->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	FirstPart->SetLeaderPoseComponent(Leader);
	SecondPart->SetLeaderPoseComponent(Leader);
	Manager->AcquireSourceHistory(FirstPart);
	Manager->AcquireSourceHistory(SecondPart);
	Manager->AcquireFullPose(FirstPart);
	Manager->AcquireFullPose(SecondPart);
	this->TestEqual(TEXT("Follower preparation keeps leader LOD0"), Leader->GetForcedLOD(), 1);
	this->TestEqual(TEXT("Follower preparation evaluates offscreen leader"), Leader->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	Manager->ReleaseFullPose(FirstPart);
	Manager->ReleaseSourceHistory(FirstPart);
	this->TestEqual(TEXT("Another follower retains leader policy"), Leader->GetForcedLOD(), 1);
	SecondPart->DestroyComponent();
	Manager->Tick(0);
	this->TestEqual(TEXT("Destroyed last follower restores leader LOD"), Leader->GetForcedLOD(), 0);
	this->TestEqual(TEXT("Destroyed last follower restores leader visibility policy"), Leader->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered);

	Manager->AcquireSourceHistory(FirstPart);
	Manager->AcquireFullPose(FirstPart);
	Leader->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	Manager->ReleaseFullPose(FirstPart);
	Manager->ReleaseSourceHistory(FirstPart);
	this->TestEqual(TEXT("Release preserves externally changed leader policy"), Leader->VisibilityBasedAnimTickOption,
		EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered);

	// 明示登録した部品だけを一つの予算・時刻・通知で扱う。
	auto* FirstActor = World->SpawnActor<AStaticMeshActor>();
	auto* SecondActor = World->SpawnActor<AStaticMeshActor>();
	auto* OtherActor = World->SpawnActor<AStaticMeshActor>();
	auto* FirstMesh = FirstActor->GetStaticMeshComponent();
	auto* SecondMesh = SecondActor->GetStaticMeshComponent();
	auto* OtherMesh = OtherActor->GetStaticMeshComponent();
	for (auto* Part : {FirstMesh, SecondMesh, OtherMesh})
	{
		Part->SetStaticMesh(OriginalMesh);
	}
	Data->SampleCount = 100;
	Data->SurfaceArea = 100;
	Data->SurfaceAreaVectors = {FVector(0, 0, 100)};
	auto Targets = TArray<FMeshParticleizerTarget>{};
	auto FirstTarget = FMeshParticleizerTarget{};
	FirstTarget.Mesh = FirstMesh;
	FirstTarget.PreparedData = Data.Get();
	auto SecondTarget = FirstTarget;
	SecondTarget.Mesh = SecondMesh;
	SecondTarget.Importance = 3;
	Targets.Append({FirstTarget, SecondTarget});

	// 回転と非一様スケールを含む別部品も、先頭の参照空間へ一度だけ合わせる。
	Data->BoundsMin = FVector(-50);
	Data->BoundsMax = FVector(50);
	FirstMesh->SetMobility(EComponentMobility::Movable);
	SecondMesh->SetMobility(EComponentMobility::Movable);
	FirstMesh->SetWorldTransform(FTransform(FRotator(0, 90, 0), FVector(100, 0, 0), FVector(2, 1, 1)));
	SecondMesh->SetWorldTransform(FTransform(FRotator(20, 10, 30), FVector(230, 10, 60), FVector(1, 2, 0.5)));
	auto Spaces = TArray<FMeshParticleizerProgressSpace>{};
	if (this->TestTrue(TEXT("Shared reference space resolves"), FMeshParticleizerProgressSpace::Resolve(Targets, true, Spaces, Error)) == false)
	{
		return false;
	}
	const auto Point = FVector(13, -17, 9);
	const auto Expected = FirstMesh->GetComponentTransform().InverseTransformPosition(SecondMesh->GetComponentTransform().TransformPosition(Point));
	this->TestTrue(TEXT("Shared transform retains rotation and nonuniform scale"), FVector(Spaces[1].SourceToGroup.TransformPosition(Point)).Equals(Expected, 0.00001));
	this->TestTrue(TEXT("All parts use common bounds"), Spaces[0].Bounds.Equals(Spaces[1].Bounds, 0.00001));
	this->TestTrue(TEXT("Common bounds contain other part"), Spaces[0].Bounds.IsInsideOrOn(Expected));

	auto GroupPointSettings = Settings;
	UMeshParticleizerOriginLibrary::SetPointOrigins(GroupPointSettings, {FVector(-20, 0, 0), FVector(20, 0, 0)}, Error);
	auto GroupPoints = TArray<FMeshParticleizerResolvedOrigin>{};
	if (FMeshParticleizerResolvedOrigin::Resolve(Targets, GroupPointSettings, Spaces, GroupPoints, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("All parts use the same reference origins"), GroupPoints[0].ReferencePoints == GroupPoints[1].ReferencePoints);
	this->TestEqual(TEXT("All parts use the same distance scale"), GroupPoints[0].ReferenceDistance, GroupPoints[1].ReferenceDistance);
	const auto StoredPoints = GroupPoints[0].ReferencePoints;
	GroupPointSettings.LocalOrigins[0] = FVector(100);
	this->TestTrue(TEXT("Resolved origins are independent of later input edits"), GroupPoints[0].ReferencePoints == StoredPoints);

	// ヒット時に保存したローカル位置を、準備待ち後の共通参照座標へ合わせる。
	auto OriginSettings = FMeshParticleizeOptions{};
	OriginSettings.Origin = EMeshParticleizerOrigin::Hit;
	auto Hit = FHitResult(SecondActor, SecondMesh, SecondMesh->GetComponentTransform().TransformPosition(Point), FVector::UpVector);
	// Component直接のTraceと同様、Blockingフラグなしでも命中点を受け取る。
	Hit.bBlockingHit = false;
	OriginSettings.HitOrigin = UMeshParticleizerOriginLibrary::CaptureHitOrigin(Hit);
	this->TestEqual(TEXT("Static hit reports its accuracy"), OriginSettings.HitOrigin.Accuracy, EMeshParticleizerOriginAccuracy::StaticHit);
	this->TestTrue(TEXT("Static hit removes nonuniform scale and rotation"), OriginSettings.HitOrigin.ReferencePosition.Equals(Point, 0.00001));
	Hit.bStartPenetrating = true;
	this->TestEqual(TEXT("Initial overlap does not invent a hit surface point"), UMeshParticleizerOriginLibrary::CaptureHitOrigin(Hit).Accuracy, EMeshParticleizerOriginAccuracy::Unavailable);
	Hit.bStartPenetrating = false;
	SecondMesh->SetWorldTransform(FTransform(FRotator(40, -15, 20), FVector(80, 70, 30), FVector(0.5, 3, 1)));
	if (FMeshParticleizerProgressSpace::Resolve(Targets, true, Spaces, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto Origins = TArray<FMeshParticleizerResolvedOrigin>{};
	if (this->TestTrue(TEXT("Captured part hit resolves for whole group"), FMeshParticleizerResolvedOrigin::Resolve(Targets, OriginSettings, Spaces, Origins, Error)) == false)
	{
		return false;
	}
	this->TestTrue(TEXT("Captured local point survives movement before start"), Origins[0].ReferencePosition.Equals(FVector(Spaces[1].SourceToGroup.TransformPosition(Point)), 0.00001));
	this->TestEqual(TEXT("Shared result identifies the hit part"), Origins[0].OriginMesh.Get(), static_cast<UMeshComponent*>(SecondMesh));
	this->TestTrue(TEXT("Shared result is identical on all parts"), Origins[0].ReferencePosition.Equals(Origins[1].ReferencePosition));
	SecondMesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	if (FMeshParticleizerResolvedOrigin::Resolve(Targets, OriginSettings, Spaces, Origins, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Changed hit mesh uses explicit fallback"), Origins[0].bUsedFallback);
	this->TestEqual(TEXT("No valid fallback bone selects center"), Origins[0].Mode, EMeshParticleizerOrigin::CenterOut);
	SecondMesh->SetStaticMesh(OriginalMesh);

	// ワールド方向はベクトルの逆変換ではなく、進行平面の法線として固定する。
	OriginSettings.Origin = EMeshParticleizerOrigin::Direction;
	OriginSettings.bDirectionInWorldSpace = true;
	OriginSettings.Direction = FVector(1, 2, 3).GetSafeNormal();
	if (FMeshParticleizerResolvedOrigin::Resolve(Targets, OriginSettings, Spaces, Origins, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	const auto Tangent = FVector::CrossProduct(OriginSettings.Direction, FVector::UpVector);
	const auto LocalTangent = FirstMesh->GetComponentTransform().InverseTransformVector(Tangent);
	this->TestTrue(TEXT("World direction preserves a world-space progress plane under nonuniform scale"), FMath::Abs(FVector::DotProduct(LocalTangent, Origins[0].ReferenceDirection)) < 0.00001);
	this->TestTrue(TEXT("World direction preserves progression sign"), FVector::DotProduct(FirstMesh->GetComponentTransform().InverseTransformVector(OriginSettings.Direction), Origins[0].ReferenceDirection) > 0);

	this->TestTrue(TEXT("Independent reference spaces resolve"), FMeshParticleizerProgressSpace::Resolve(Targets, false, Spaces, Error));
	this->TestTrue(TEXT("Independent parts keep original coordinates"), Spaces[1].SourceToGroup.Equals(FMatrix::Identity));
	this->TestTrue(TEXT("Independent bounds stay local"), Spaces[1].Bounds.Equals(FBox(Data->BoundsMin, Data->BoundsMax), 0.00001));

	// 骨の取付けは現在姿勢ではなく参照姿勢へ戻す。親を動かしても順序は同じになる。
	auto* Pose = NewObject<UPoseableMeshComponent>(ModularActor);
	Pose->SetSkinnedAssetAndUpdate(SkeletalCube);
	Pose->RegisterComponent();
	const auto BoneName = Pose->GetBoneName(0);
	SecondMesh->AttachToComponent(Pose, FAttachmentTransformRules::KeepRelativeTransform, BoneName);
	SecondMesh->SetRelativeTransform(FTransform(FRotator(10, 25, 0), FVector(30, 0, 10)));
	auto PoseTarget = FirstTarget;
	PoseTarget.Mesh = Pose;
	const auto PoseTargets = TArray<FMeshParticleizerTarget>{PoseTarget, SecondTarget};
	this->TestTrue(TEXT("Bone-attached reference resolves"), FMeshParticleizerProgressSpace::Resolve(PoseTargets, true, Spaces, Error));
	const auto AttachedReference = Spaces[1].SourceToGroup;
	Pose->SetBoneTransformByName(BoneName, FTransform(FRotator(0, 70, 0), FVector(0, 35, 20)), EBoneSpaces::ComponentSpace);
	Pose->RefreshBoneTransforms();
	SecondMesh->UpdateComponentToWorld();
	this->TestTrue(TEXT("Animated attachment resolves"), FMeshParticleizerProgressSpace::Resolve(PoseTargets, true, Spaces, Error));
	this->TestTrue(TEXT("Attachment reference is invariant under bone animation"), Spaces[1].SourceToGroup.Equals(AttachedReference, 0.0001));

	// 現在姿勢の骨から参照姿勢へ戻すため、Componentの逆変換だけでは得られない位置になる。
	Hit = FHitResult(ModularActor, Pose, Pose->GetBoneTransform(0).TransformPosition(Point), FVector::UpVector);
	Hit.bBlockingHit = true;
	Hit.BoneName = BoneName;
	OriginSettings.Origin = EMeshParticleizerOrigin::Hit;
	OriginSettings.HitOrigin = UMeshParticleizerOriginLibrary::CaptureHitOrigin(Hit);
	const auto ReferenceBone = FMatrix(SkeletalCube->GetRefBasesInvMatrix()[0]).Inverse();
	const auto ReferenceHit = FVector(ReferenceBone.TransformPosition(Point));
	this->TestEqual(TEXT("Skeletal hit explicitly reports bone approximation"), OriginSettings.HitOrigin.Accuracy, EMeshParticleizerOriginAccuracy::BoneApproximation);
	this->TestTrue(TEXT("Bone-local hit maps to reference pose"), OriginSettings.HitOrigin.ReferencePosition.Equals(ReferenceHit, 0.0001));
	this->TestFalse(TEXT("Bone mapping is not component inverse alone"), Pose->GetComponentTransform().InverseTransformPosition(Hit.ImpactPoint).Equals(ReferenceHit, 0.01));
	Pose->SetBoneTransformByName(BoneName, FTransform(FRotator(25, -30, 10), FVector(45, -25, 10)), EBoneSpaces::ComponentSpace);
	Pose->RefreshBoneTransforms();
	if (FMeshParticleizerResolvedOrigin::Resolve(PoseTargets, OriginSettings, Spaces, Origins, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Hit snapshot does not use a later bone pose"), Origins[0].ReferencePosition.Equals(ReferenceHit, 0.0001));
	Hit.BoneName = TEXT("MissingBone");
	OriginSettings.HitOrigin = UMeshParticleizerOriginLibrary::CaptureHitOrigin(Hit);
	OriginSettings.FallbackBone = BoneName;
	if (FMeshParticleizerResolvedOrigin::Resolve(PoseTargets, OriginSettings, Spaces, Origins, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Missing hit bone reports fallback"), Origins[0].bUsedFallback);
	this->TestEqual(TEXT("Fallback reports actual bone"), Origins[0].BoneOrSocket, BoneName);
	this->TestEqual(TEXT("Fallback reports bone accuracy"), Origins[0].Accuracy, EMeshParticleizerOriginAccuracy::BoneOrSocket);
	this->TestTrue(TEXT("Fallback uses reference bone position"), Origins[0].ReferencePosition.Equals(ReferenceBone.GetOrigin(), 0.0001));

	// 公開APIの対応可否も検証する。LODごとの空Morph入れ物は通常メッシュにも存在する。
	auto ProjectionData = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	ProjectionData->SkeletalMesh = SkeletalCube;
	ProjectionData->DataVersion = UMeshParticleizerPreparedMesh::CurrentDataVersion;
	ProjectionData->SourceFingerprint = SkeletalCube->GetResourceForRendering()->DerivedDataKey;
	ProjectionData->BoneCount = SkeletalCube->GetRefSkeleton().GetRawBoneNum();
	auto ProjectionVertices = TArray<FLinearColor>{};
	ProjectionVertices.SetNumZeroed(3 * UMeshParticleizerPreparedMesh::SkinVertexStride);
	ProjectionVertices[UMeshParticleizerPreparedMesh::SkinVertexStride] = FLinearColor(10, 0, 0, 1);
	ProjectionVertices[2 * UMeshParticleizerPreparedMesh::SkinVertexStride] = FLinearColor(0, 10, 0, 1);
	for (auto Vertex = 0; Vertex < 3; ++Vertex)
	{
		ProjectionVertices[Vertex * UMeshParticleizerPreparedMesh::SkinVertexStride + 3].R = 1;
	}
	const auto ProjectionTriangles = TArray<FLinearColor>{FLinearColor(0, 1, 2, 0)};
	if (this->TestTrue(TEXT("Register a small projection fixture"), ProjectionData->SurfaceProjection.Build(ProjectionVertices, 3, ProjectionTriangles, 1, ProjectionData->BoneCount, Error)) == false)
	{
		return false;
	}
	auto ProjectionMatrices = TArray<FMatrix44f>{};
	Pose->GetCurrentRefToLocalMatrices(ProjectionMatrices, 0);
	const auto ProjectionReference = FVector(2, 3, 0);
	const auto ProjectionWorld = Pose->GetComponentTransform().TransformPosition(
		FVector(FMatrix(ProjectionMatrices[0]).TransformPosition(ProjectionReference)));
	auto ProjectionHit = FHitResult(ModularActor, Pose, ProjectionWorld, FVector::UpVector);
	ProjectionHit.bBlockingHit = true;
	auto ProjectionMaterial = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
	auto OriginalPoseMaterials = TArray<TObjectPtr<UMaterialInterface>>{};
	for (auto Slot = 0; Slot < Pose->GetNumMaterials(); ++Slot)
	{
		OriginalPoseMaterials.Add(Pose->GetMaterial(Slot));
		Pose->SetMaterial(Slot, ProjectionMaterial.Get());
	}
	Pose->ExternalMorphSets.SetNum(1);
	const auto SurfaceHit = UMeshParticleizerOriginLibrary::CaptureSurfaceHitOrigin(ProjectionHit, ProjectionData.Get(), 0.1);
	this->AddInfo(FString::Printf(TEXT("Surface API fixture: %s"), *SurfaceHit.Diagnostics));
	this->TestEqual(TEXT("Empty per-LOD morph containers allow standard surface projection"), SurfaceHit.Accuracy, EMeshParticleizerOriginAccuracy::SurfaceProjection);
	this->TestTrue(TEXT("Public surface capture maps the posed triangle to reference space"), SurfaceHit.ReferencePosition.Equals(ProjectionReference, 0.0001));

	// 複数登録では先頭のデータではなく、命中部品のデータを使う。
	auto HitPlayer = TStrongObjectPtr<UMeshParticleizerComponent>(NewObject<UMeshParticleizerComponent>(ModularActor));
	auto ProjectionTarget = PoseTarget;
	ProjectionTarget.PreparedData = ProjectionData.Get();
	if (this->TestTrue(TEXT("Register mixed static and skeletal hit targets"), HitPlayer->ConfigureTargets({SecondTarget, ProjectionTarget}, Error)) == false)
	{
		return false;
	}
	const auto RegisteredHit = HitPlayer->CaptureRegisteredHitOrigin(ProjectionHit);
	this->TestEqual(TEXT("Registered hit selects the matching prepared mesh"), RegisteredHit.Accuracy, EMeshParticleizerOriginAccuracy::SurfaceProjection);
	this->TestTrue(TEXT("Registered hit preserves the projected position"), RegisteredHit.ReferencePosition.Equals(SurfaceHit.ReferencePosition, 0.0001));
	ProjectionHit.BoneName = BoneName;
	this->TestEqual(TEXT("Bone approximation is an explicit choice"), HitPlayer->CaptureRegisteredHitOrigin(ProjectionHit, false).Accuracy, EMeshParticleizerOriginAccuracy::BoneApproximation);
	this->TestEqual(TEXT("Unregistered mesh hit is rejected"), HitPlayer->CaptureRegisteredHitOrigin(FHitResult(FirstActor, FirstMesh, Point, FVector::UpVector)).Accuracy, EMeshParticleizerOriginAccuracy::Unavailable);
	this->TestEqual(TEXT("Registered static hit needs no skeletal projection"), HitPlayer->CaptureRegisteredHitOrigin(FHitResult(SecondActor, SecondMesh, Point, FVector::UpVector)).Accuracy, EMeshParticleizerOriginAccuracy::StaticHit);
	this->TestEqual(TEXT("Projection distance validation is retained"), HitPlayer->CaptureRegisteredHitOrigin(ProjectionHit, true, -1).Accuracy, EMeshParticleizerOriginAccuracy::Unavailable);
	ProjectionTarget.PreparedData = TSoftObjectPtr<UMeshParticleizerPreparedMesh>(FSoftObjectPath(TEXT("/Game/MissingHitData.MissingHitData")));
	this->TestTrue(TEXT("Unloaded hit data can be configured"), HitPlayer->ConfigureTargets({ProjectionTarget}, Error));
	const auto MissingHit = HitPlayer->CaptureRegisteredHitOrigin(ProjectionHit);
	this->TestEqual(TEXT("Missing prepared data does not silently become bone approximation"), MissingHit.Accuracy, EMeshParticleizerOriginAccuracy::Unavailable);
	this->TestFalse(TEXT("Missing prepared data explains the failure"), MissingHit.Diagnostics.IsEmpty());
	this->TestEqual(TEXT("Capture does not start preparation or synchronously load assets"), HitPlayer->GetPreparationState(), EMeshParticleizerPreparationState::Unprepared);
	this->TestEqual(TEXT("Explicit bone approximation needs no prepared asset"), HitPlayer->CaptureRegisteredHitOrigin(ProjectionHit, false).Accuracy, EMeshParticleizerOriginAccuracy::BoneApproximation);

	Pose->MorphTargetWeights.Add(0.5f);
	this->TestEqual(TEXT("Active morph cannot claim a standard skinned surface"),
		UMeshParticleizerOriginLibrary::CaptureSurfaceHitOrigin(ProjectionHit, ProjectionData.Get()).Accuracy, EMeshParticleizerOriginAccuracy::Unavailable);
	Pose->MorphTargetWeights.Reset();
	Pose->ExternalMorphSets[0].Add(0, nullptr);
	this->TestEqual(TEXT("Registered external morph data requires a different projection path"),
		UMeshParticleizerOriginLibrary::CaptureSurfaceHitOrigin(ProjectionHit, ProjectionData.Get()).Accuracy, EMeshParticleizerOriginAccuracy::Unavailable);
	Pose->ExternalMorphSets[0].Reset();
	for (auto Slot = 0; Slot < OriginalPoseMaterials.Num(); ++Slot)
	{
		Pose->SetMaterial(Slot, OriginalPoseMaterials[Slot]);
	}

	// テスト用Socketは一時複製だけに追加し、エンジンの元アセットを変更しない。
	auto SocketAsset = TStrongObjectPtr<USkeletalMesh>(DuplicateObject<USkeletalMesh>(SkeletalCube, GetTransientPackage()));
	auto* Socket = NewObject<USkeletalMeshSocket>(SocketAsset.Get());
	Socket->SocketName = TEXT("ParticleOrigin");
	Socket->BoneName = BoneName;
	Socket->RelativeLocation = Point;
	SocketAsset->GetMeshOnlySocketList().Add(Socket);
	SocketAsset->RebuildSocketMap();
	Pose->SetSkinnedAssetAndUpdate(SocketAsset.Get());
	Pose->AddSocketOverride(TEXT("OriginAlias"), Socket->SocketName);
	OriginSettings.Origin = EMeshParticleizerOrigin::BoneOrSocket;
	OriginSettings.OriginBoneOrSocket = TEXT("OriginAlias");
	if (FMeshParticleizerResolvedOrigin::Resolve(PoseTargets, OriginSettings, Spaces, Origins, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestFalse(TEXT("Socket override resolves without fallback"), Origins[0].bUsedFallback);
	this->TestTrue(TEXT("Socket local offset is included in reference origin"), Origins[0].ReferencePosition.Equals(ReferenceHit, 0.0001));
	Pose->RemoveAllSocketOverrides();
	Pose->SetSkinnedAssetAndUpdate(SkeletalCube);
	OriginSettings.OriginBoneOrSocket = TEXT("MissingSocket");
	OriginSettings.FallbackBone = NAME_None;
	if (FMeshParticleizerResolvedOrigin::Resolve(PoseTargets, OriginSettings, Spaces, Origins, Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Missing named origin reports center fallback"), Origins[0].bUsedFallback && Origins[0].ReferencePosition.Equals(Spaces[0].Bounds.GetCenter()));

	SecondMesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	SecondMesh->SetWorldScale3D(FVector::ZeroVector);
	this->TestNull(TEXT("Degenerate shared frame rejects the complete start"), Manager->StartParticleizeTargets(Targets, Settings, Error));
	this->TestEqual(TEXT("Rejected shared frame does not modify source materials"), FirstMesh->GetMaterial(0), Original);
	FirstMesh->SetWorldTransform(FTransform::Identity);
	SecondMesh->SetWorldTransform(FTransform::Identity);

	// 自分のOwnerの補助Colliderと、別Actorの登録部品を床として拾わない。
	FirstMesh->SetWorldLocation(FVector(10000, 0, 300));
	SecondMesh->SetWorldLocation(FVector(10000, 0, 150));
	for (auto* Part : {FirstMesh, SecondMesh})
	{
		Part->SetCollisionProfileName(TEXT("BlockAll"));
	}
	auto* Auxiliary = NewObject<UBoxComponent>(FirstActor);
	Auxiliary->SetBoxExtent(FVector(60, 60, 10));
	Auxiliary->SetWorldLocation(FVector(10000, 0, 230));
	Auxiliary->SetCollisionProfileName(TEXT("BlockAll"));
	Auxiliary->RegisterComponent();
	auto* FloorActor = World->SpawnActor<AStaticMeshActor>();
	auto* FloorMesh = FloorActor->GetStaticMeshComponent();
	FloorMesh->SetMobility(EComponentMobility::Movable);
	FloorMesh->SetStaticMesh(OriginalMesh);
	FloorMesh->SetWorldTransform(FTransform(FQuat::Identity, FVector(10000, 0, -50), FVector(10, 10, 1)));
	FloorMesh->SetCollisionProfileName(TEXT("BlockAll"));
	auto GroundSettings = FMeshParticleizeOptions{};
	GroundSettings.ConversionDuration = 10;
	GroundSettings.ParticleLifetime = 0;
	GroundSettings.MaxParticles = 0;
	GroundSettings.Motion = EMeshParticleizerMotion::Settle;
	auto CheckGround = [this, WeakWorld = TWeakObjectPtr<UWorld>(World)](int32 ExpectedCount, bool bExpectedEnabled, float ExpectedHeight) {
		auto Count = 0;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			if (It->GetWorld() != WeakWorld.Get() || It->IsRegistered() == false)
			{
				continue;
			}
			auto bFound = false;
			if (It->GetVariableInt(TEXT("User.Motion"), bFound) != static_cast<int32>(EMeshParticleizerMotion::Settle) || bFound == false)
			{
				continue;
			}
			const auto Enabled = It->GetVariableFloat(TEXT("User.GroundEnabled"), bFound);
			this->TestTrue(TEXT("Contact enable flag is present"), bFound);
			this->TestEqual(TEXT("Ground detection chooses a usable plane or disables collision"), Enabled, bExpectedEnabled ? 1.0f : 0.0f);
			const auto Height = It->GetVariableFloat(TEXT("User.GroundHeight"), bFound);
			this->TestTrue(TEXT("Contact height is present"), bFound);
			this->TestNearlyEqual(TEXT("Only an accepted plane reaches the GPU"), Height, ExpectedHeight, 0.001f);
			++Count;
		}
		this->TestEqual(TEXT("Every registered source receives the ground result"), Count, ExpectedCount);
	};
	auto* GroundControl = Manager->StartParticleize(FirstMesh, Data.Get(), GroundSettings, Error);
	if (this->TestNotNull(TEXT("Single source ground control starts"), GroundControl) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(1, true, 200);
	this->TestEqual(TEXT("Automatic floor has a structured result"), GroundControl->GetResolvedGround().Status, EMeshParticleizerGroundStatus::Detected);
	this->TestNearlyEqual(TEXT("Public floor height matches the GPU"), GroundControl->GetResolvedGround().Height, 200.f, 0.001f);
	GroundControl->Cancel(true);
	this->TestTrue(TEXT("Cancelled operation retains the accepted floor"), GroundControl->GetResolvedGround().bEnabled);

	auto* GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("Grouped ground control starts"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, true, 0);
	this->TestEqual(TEXT("Second source exposes its own floor result"), GroundGroup->GetResolvedGround(1).Status, EMeshParticleizerGroundStatus::Detected);
	this->TestEqual(TEXT("Invalid source index is not a detected floor"), GroundGroup->GetResolvedGround(2).Status, EMeshParticleizerGroundStatus::NotEvaluated);
	GroundGroup->Cancel(true);

	// 重力を扱えない旧システムでは、灰を無重力のまま正常再生したことにしない。
	const auto DriftVersion = FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("User.DriftGravityVersion"));
	auto& SystemParameters = System->GetExposedParameters();
	const auto SavedDriftVersion = SystemParameters.GetParameterValue<int32>(DriftVersion);
	auto DriftSettings = GroundSettings;
	DriftSettings.Motion = EMeshParticleizerMotion::Drift;
	DriftSettings.DriftGravityScale = 0.1f;
	DriftSettings.MaxParticles = 1;
	DriftSettings.ParticleLifetime = 1;
	SystemParameters.SetParameterValue(0, DriftVersion, true);
	auto* UnsupportedDrift = Manager->StartParticleize(FirstMesh, Data.Get(), DriftSettings, Error);
	SystemParameters.SetParameterValue(SavedDriftVersion, DriftVersion);
	this->TestNull(TEXT("Old particle system rejects drift gravity"), UnsupportedDrift);
	this->TestTrue(TEXT("Unsupported drift identifies the upgrade"), Error.Contains(TEXT("Drift gravity")));
	this->TestEqual(TEXT("Rejected drift leaves source material intact"), FirstMesh->GetMaterial(0), Original);
	if (UnsupportedDrift != nullptr)
	{
		UnsupportedDrift->Cancel(true);
	}

	// 各隣接サンプルの差が許容内でも、中央から累積する段差を水平床にしない。
	auto Steps = TArray<TWeakObjectPtr<AStaticMeshActor>>{};
	const FVector2D Offsets[] = {{-40, -40}, {-40, 40}, {40, -40}, {40, 40}};
	for (auto Index = 0; Index < 4; ++Index)
	{
		auto* StepActor = World->SpawnActor<AStaticMeshActor>();
		auto* StepMesh = StepActor->GetStaticMeshComponent();
		StepMesh->SetMobility(EComponentMobility::Movable);
		StepMesh->SetStaticMesh(OriginalMesh);
		StepMesh->SetWorldTransform(FTransform(FQuat::Identity,
			FVector(10000 + Offsets[Index].X, Offsets[Index].Y, (Index + 1) * 1.5 - 5), FVector(0.2, 0.2, 0.1)));
		StepMesh->SetCollisionProfileName(TEXT("BlockAll"));
		Steps.Add(StepActor);
	}
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("Stepped terrain permits a diagnosed free fall"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, false, 0);
	this->TestEqual(TEXT("Stepped terrain reports inconsistent heights"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::UnevenHeights);
	GroundGroup->Cancel(true);
	for (const auto& StepActor : Steps)
	{
		StepActor->Destroy();
	}

	FloorMesh->SetWorldRotation(FRotator(20, 0, 0));
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("Unsupported slope still permits free fall"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, false, 0);
	this->TestEqual(TEXT("Slope is distinguished from a missing floor"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::UnsupportedSlope);
	GroundGroup->Cancel(true);

	// 中央だけ床がある場合も、開始点が床の中にある場合も平面を作らない。
	FloorMesh->SetWorldTransform(FTransform(FQuat::Identity, FVector(10000, 0, 70), FVector(0.4, 0.4, 0.1)));
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("A partial floor permits free fall"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, false, 0);
	this->TestEqual(TEXT("A missing corner rejects the center height"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::NoHit);
	this->TestEqual(TEXT("A rejected public height is cleared"), GroundGroup->GetResolvedGround().Height, 0.f);
	GroundGroup->Cancel(true);

	FloorMesh->SetWorldTransform(FTransform(FQuat::Identity, FVector(10000, 0, 300), FVector(10, 10, 1)));
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("An embedded probe permits free fall"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, false, 0);
	this->TestEqual(TEXT("An embedded probe does not create a plane at the source center"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::InitialOverlap);
	this->TestEqual(TEXT("Other source keeps its own missing-floor reason"), GroundGroup->GetResolvedGround(1).Status, EMeshParticleizerGroundStatus::NoHit);
	GroundGroup->Cancel(true);

	FloorActor->Destroy();
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("Missing floor still permits free fall"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, false, 0);
	this->TestTrue(TEXT("Missing floor is reported without an invented origin plane"), GroundGroup->GetDiagnostics().Contains(TEXT("No collision plane")));
	this->TestEqual(TEXT("Missing floor has a structured reason"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::NoHit);
	GroundGroup->Cancel(true);

	GroundSettings.Ground = EMeshParticleizerGround::ExplicitHeight;
	GroundSettings.GroundHeight = 75;
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("An explicit height works without a traced floor"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, true, 75);
	this->TestEqual(TEXT("Explicit height is distinguished from detection"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::ExplicitHeight);
	GroundGroup->Cancel(true);

	GroundSettings.Ground = EMeshParticleizerGround::Disabled;
	GroundGroup = Manager->StartParticleizeTargets(Targets, GroundSettings, Error);
	if (this->TestNotNull(TEXT("Disabled floor still permits playback"), GroundGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	CheckGround(2, false, 0);
	this->TestEqual(TEXT("Disabled floor is not a detection failure"), GroundGroup->GetResolvedGround().Status, EMeshParticleizerGroundStatus::Disabled);
	GroundGroup->Cancel(true);
	Auxiliary->DestroyComponent();
	FirstMesh->SetWorldTransform(FTransform::Identity);
	SecondMesh->SetWorldTransform(FTransform::Identity);

	this->TestNull(TEXT("Duplicate group source rejected"), Manager->PrepareParticleizeTargets({FirstTarget, FirstTarget}, Error));
	this->TestEqual(TEXT("Duplicate rejection leaves materials intact"), FirstMesh->GetMaterial(0), Original);
	SecondMesh->SetMaterial(0, ExternalMaterial.Get());
	this->TestNull(TEXT("One unsupported part rejects the entire preparation"), Manager->PrepareParticleizeTargets(Targets, Error));
	this->TestEqual(TEXT("Late validation failure does not modify earlier part"), FirstMesh->GetMaterial(0), Original);
	SecondMesh->SetMaterial(0, Original);

	Settings.ConversionDuration = 0;
	Settings.ParticleLifetime = 0;
	Settings.MaxParticles = 100;
	Settings.Origin = EMeshParticleizerOrigin::Hit;
	Settings.HitOrigin = UMeshParticleizerOriginLibrary::CaptureHitOrigin(FHitResult{});

	// 準備待ち後の開始時スケールを使い、確定した結果は再生中と終了後に保持する。
	auto ScaledGroup = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleizeTargets(Targets, Error));
	if (this->TestNotNull(TEXT("Scaled group prepares"), ScaledGroup.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestFalse(TEXT("Preparation alone does not resolve allocation"), ScaledGroup->GetSourceAllocation().bResolved);
	FirstMesh->SetWorldScale3D(FVector(2, 1, 1));
	if (this->TestNotNull(TEXT("Scaled group starts"), Manager->StartPreparedParticleize(ScaledGroup.Get(), Settings, Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("Start-time area changes first source allocation"), ScaledGroup->GetGrantedParticlesForSource(FirstMesh), 40);
	this->TestEqual(TEXT("Importance and scaled area share one budget"), ScaledGroup->GetGrantedParticlesForSource(SecondMesh), 60);
	const auto AreaSnapshot = ScaledGroup->GetSourceAllocation();
	this->TestTrue(TEXT("Successful start resolves allocation"), AreaSnapshot.bResolved);
	this->TestEqual(TEXT("Snapshot includes transformed reference area"), AreaSnapshot.ScaledArea, 200.0);
	this->TestEqual(TEXT("Snapshot retains prepared candidate limit"), AreaSnapshot.CandidateLimit, 100);
	FirstMesh->SetWorldScale3D(FVector(4, 1, 1));
	this->TestEqual(TEXT("Later scaling does not repartition an operation"), ScaledGroup->GetSourceAllocation().ScaledArea, AreaSnapshot.ScaledArea);
	ScaledGroup->Cancel(true);
	this->TestEqual(TEXT("Cancelled operation retains its allocation"), ScaledGroup->GetSourceAllocation().GrantedParticles, 40);
	this->TestFalse(TEXT("Invalid allocation index is unresolved"), ScaledGroup->GetSourceAllocation(-1).bResolved);
	Data->SurfaceAreaVectors.Reset();
	this->TestNull(TEXT("Legacy nonuniform group is rejected"), Manager->StartParticleizeTargets(Targets, Settings, Error));
	this->TestTrue(TEXT("Legacy rejection identifies source and reprepare"), Error.Contains(TEXT("Source 0")) && Error.Contains(TEXT("Prepare this source again")));
	this->TestEqual(TEXT("Rejected scale preserves source material"), FirstMesh->GetMaterial(0), Original);
	Data->SurfaceAreaVectors = {FVector(0, 0, 100)};
	FirstMesh->SetWorldTransform(FTransform::Identity);

	Receiver->Events.Reset();
	Receiver->DestroyOnConverted.Reset();
	auto Group = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleizeTargets(Targets, Error));
	if (this->TestNotNull(TEXT("Group preparation accepted"), Group.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto* StartedGroup = Manager->StartPreparedParticleize(Group.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Prepared group starts"), StartedGroup) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("Group exposes one handle"), StartedGroup, Group.Get());
	this->TestEqual(TEXT("Group source count is explicit"), Group->GetSourceCount(), 2);
	this->TestTrue(TEXT("Operation exposes the actual fallback after start"), Group->GetResolvedOrigin().bUsedFallback);
	this->TestTrue(TEXT("All group parts expose the same fallback origin"), Group->GetResolvedOrigin(1).ReferencePosition.Equals(Group->GetResolvedOrigin().ReferencePosition));
	this->TestEqual(TEXT("Group grants one shared budget"), Group->GetGrantedParticleCount(), 100);
	this->TestEqual(TEXT("First part receives weighted budget"), Group->GetGrantedParticlesForSource(FirstMesh), 25);
	this->TestEqual(TEXT("Second part receives weighted budget"), Group->GetGrantedParticlesForSource(SecondMesh), 75);
	this->TestEqual(TEXT("Unregistered mesh is untouched"), OtherMesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Individual duplicate returns existing group"), Manager->StartParticleize(SecondMesh, Data.Get(), Settings, Error), Group.Get());

	// 一部だけ重なる要求へ既存ハンドルを返すと、呼出側が別のグループを操作してしまう。
	auto OtherTarget = FirstTarget;
	OtherTarget.Mesh = OtherMesh;
	const auto BeforeConflict = Manager->GetBudgetUsage();
	const auto FirstStateBeforeConflict = Group->GetState();
	auto* FirstGroupMaterial = FirstMesh->GetMaterial(0);
	this->TestNull(TEXT("Partial overlap rejects instead of returning an unrelated group"),
		Manager->StartParticleizeTargets({FirstTarget, OtherTarget}, Settings, Error));
	this->TestFalse(TEXT("Partial overlap explains the rejection"), Error.IsEmpty());
	this->TestNull(TEXT("Partial overlap does not depend on target order"),
		Manager->StartParticleizeTargets({OtherTarget, SecondTarget}, Settings, Error));
	this->TestEqual(TEXT("Conflict preserves the running group material"), FirstMesh->GetMaterial(0), FirstGroupMaterial);
	this->TestEqual(TEXT("Conflict leaves the new target untouched"), OtherMesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Conflict reserves no new sources"), Manager->GetBudgetUsage().ReservedSources, BeforeConflict.ReservedSources);
	this->TestEqual(TEXT("Reordered duplicate returns the same complete group"),
		Manager->StartParticleizeTargets({SecondTarget, FirstTarget}, Settings, Error), Group.Get());

	// 不正な入力を、既存再生に含まれるという理由で二重開始へ読み替えない。
	this->TestNull(TEXT("Duplicate entries are rejected even when already active"),
		Manager->StartParticleizeTargets({FirstTarget, FirstTarget}, Settings, Error));
	auto InvalidTarget = FirstTarget;
	InvalidTarget.Importance = 0;
	this->TestNull(TEXT("Invalid importance is rejected before duplicate lookup"),
		Manager->StartParticleizeTargets({InvalidTarget}, Settings, Error));
	InvalidTarget = FirstTarget;
	InvalidTarget.PreparedData.Reset();
	this->TestNull(TEXT("Missing prepared reference is rejected before duplicate lookup"),
		Manager->StartParticleizeTargets({InvalidTarget}, Settings, Error));

	auto* OtherOperation = Manager->StartParticleizeTargets({OtherTarget}, Settings, Error);
	if (this->TestNotNull(TEXT("Independent group can start"), OtherOperation) == false)
	{
		this->AddError(Error);
		return false;
	}
	const auto OtherStateBeforeConflict = OtherOperation->GetState();
	this->TestNull(TEXT("A request spanning two active operations is rejected"),
		Manager->StartParticleizeTargets({FirstTarget, OtherTarget}, Settings, Error));
	this->TestEqual(TEXT("First operation keeps its state after conflict"), Group->GetState(), FirstStateBeforeConflict);
	this->TestEqual(TEXT("Second operation keeps its state after conflict"), OtherOperation->GetState(), OtherStateBeforeConflict);
	OtherOperation->Cancel(true);

	Receiver->Bind(Group.Get());
	Manager->Tick(0);
	this->TestTrue(TEXT("Zero-time group fires each event only once"), Receiver->Events == TArray<FName>{TEXT("Started"), TEXT("Converted"), TEXT("Finished")});
	Group->RestoreMesh();
	this->TestEqual(TEXT("Group restore returns first material"), FirstMesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Group restore returns second material"), SecondMesh->GetMaterial(0), Original);

	Group.Reset(Manager->PrepareParticleizeTargets(Targets, Error));
	if (this->TestNotNull(TEXT("Group can prepare again"), Group.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	SecondMesh->SetMaterial(0, ExternalMaterial.Get());
	this->TestNull(TEXT("Changed prepared part rejects complete start"), Manager->StartPreparedParticleize(Group.Get(), Settings, Error));
	Group->RestoreMesh();
	this->TestEqual(TEXT("Failed group can restore unaffected part"), FirstMesh->GetMaterial(0), Original);
	this->TestEqual(TEXT("Group restore preserves externally changed part"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ExternalMaterial.Get()));
	SecondMesh->SetMaterial(0, Original);

	Settings.ConversionDuration = 10;
	Settings.ParticleLifetime = 0.1f;
	Settings.MaxParticles = 0;
	Receiver->Events.Reset();
	Group.Reset(Manager->StartParticleizeTargets(Targets, Settings, Error));
	if (this->TestNotNull(TEXT("Long group accepted"), Group.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(Group.Get());
	FirstActor->Destroy();
	Manager->Tick(0);
	this->TestEqual(TEXT("One lost part drains whole operation"), Group->GetState(), EMeshParticleizeState::Draining);
	this->TestEqual(TEXT("Remaining part restores material on group failure"), SecondMesh->GetMaterial(0), Original);
	World->Tick(LEVELTICK_All, 0.2f);
	Manager->Tick(0);
	this->TestEqual(TEXT("Group reports source loss once"), Receiver->Reason, EMeshParticleizeFinishReason::SourceLost);
	this->TestTrue(TEXT("Resolved origin is retained after source loss and resource release"), Group->GetResolvedOrigin().bUsedFallback);
	this->TestTrue(TEXT("Failed group never reports successful conversion"), Receiver->Events == TArray<FName>{TEXT("Started"), TEXT("Finished")});

	// 終了通知後もGPU完了までは予約が残る。粒子を動かさない経路には待機を作らない。
	this->TestEqual(TEXT("Zero-particle and zero-lifetime operations release without a GPU wait"), Manager->GetBudgetUsage().ReleasingOperations, 0);
	const auto BeforeRelease = Manager->GetBudgetUsage();
	this->TestEqual(TEXT("Zero-particle and zero-lifetime paths create no shared GPU reservation"), BeforeRelease.SharedGpuReservationBytes, int64{0});
	Settings.MaxParticles = 1;
	Settings.ParticleLifetime = 1;
	this->TestFalse(TEXT("Negative memory limit rejects"), Manager->SetSimulationMemoryLimit(-1, Error));
	this->TestTrue(TEXT("Zero memory limit is accepted while idle"), Manager->SetSimulationMemoryLimit(0, Error));
	auto* DissolveOnly = Manager->StartParticleize(SecondMesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Memory exhaustion still permits dissolve-only"), DissolveOnly) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestEqual(TEXT("No late GPU allocation after memory reduction"), DissolveOnly->GetGrantedParticleCount(), 0);
	this->TestEqual(TEXT("Dissolve-only reserves zero bytes"), DissolveOnly->GetSimulationReservationBytes(), int64{0});
	this->TestTrue(TEXT("Memory reduction is diagnosed"), DissolveOnly->GetDiagnostics().Contains(TEXT("memory limit")));
	DissolveOnly->Cancel(true);
	this->TestTrue(TEXT("Memory budget can be restored"), Manager->SetSimulationMemoryLimit(256ll * 1024 * 1024, Error));

	// このfixtureは描画しないため、未観測時の拒否を確かめてから容量モデルの入力を与える。
	this->TestNull(TEXT("GPU start waits for a rendered snapshot"), Manager->StartParticleize(SecondMesh, Data.Get(), Settings, Error));
	this->TestTrue(TEXT("Unobserved rendering is diagnosed"), Error.Contains(TEXT("Rendering preparation is pending")));
	auto Scene = FMeshParticleizerSceneGpuSnapshot{};
	Scene.Frame = 1;
	Scene.PrimitiveStride = 672;
	Scene.InstanceStride = 48;
	Scene.MeshPayloadStride = 3;
	auto ViewBuffers = FMeshParticleizerDrawGpuPass{};
	ViewBuffers.Name = TEXT("SharedViewData");
	ViewBuffers.bSubmitted = true;
	ViewBuffers.Views = 1;
	ViewBuffers.PooledBufferBytes = {1056, 1056, 4};
	ViewBuffers.GpuWrittenBytes = 4;
	Manager->GetSceneGpu()->PublishDraws(Scene.Frame, true, {ViewBuffers});
	Manager->GetSceneGpu()->Publish(Scene, 1);
	Receiver->Events.Reset();
	auto* GpuOperation = Manager->StartParticleize(SecondMesh, Data.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("GPU release fixture starts"), GpuOperation) == false)
	{
		this->AddError(Error);
		return false;
	}
	Receiver->Bind(GpuOperation);
	GpuOperation->Cancel(true);
	const auto PendingRelease = Manager->GetBudgetUsage();
	this->TestTrue(TEXT("GPU simulation capacity is reserved"), GpuOperation->GetSimulationReservationBytes() > 0);
	this->TestEqual(TEXT("GPU memory remains reserved until completion"), PendingRelease.ReleasingSimulationBytes, GpuOperation->GetSimulationReservationBytes());
	this->TestEqual(TEXT("Pending GPU memory is part of total reservation"), PendingRelease.ReservedSimulationBytes,
		BeforeRelease.ReservedSimulationBytes + GpuOperation->GetSimulationReservationBytes() + PendingRelease.SharedGpuReservationBytes);
	this->TestTrue(TEXT("First GPU start reserves the world shared capacity"), PendingRelease.SharedGpuReservationBytes > 0);
	this->TestFalse(TEXT("A lower limit cannot discard pending GPU bytes"), Manager->SetSimulationMemoryLimit(0, Error));
	this->TestEqual(TEXT("Cancelled GPU allocation remains reserved"), PendingRelease.ReservedParticles, BeforeRelease.ReservedParticles + 1);
	this->TestEqual(TEXT("Cancelled source slot remains reserved"), PendingRelease.ReservedSources, BeforeRelease.ReservedSources + 1);
	this->TestEqual(TEXT("GPU wait exposes its particle allocation"), PendingRelease.ReleasingParticles, 1);
	this->TestEqual(TEXT("GPU wait exposes its source allocation"), PendingRelease.ReleasingSources, 1);
	this->TestTrue(TEXT("GPU release can progress while paused"), Manager->IsTickableWhenPaused());
	this->TestTrue(TEXT("Cancellation events do not wait for GPU completion"), Receiver->Events == TArray<FName>{TEXT("Cancelled"), TEXT("Finished")});
	GpuOperation->Cancel(true);
	this->TestEqual(TEXT("Repeated cancellation does not duplicate pending release"), Manager->GetBudgetUsage().ReleasingOperations, 1);
	this->TestEqual(TEXT("GPU cancellation restores immediately"), SecondMesh->GetMaterial(0), Original);

	// 同じ材質参照のまま値だけ変わった場合も、古いベイク色で開始しない。
	auto ColorMaster = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
	auto* SurfaceTint = NewObject<UMaterialExpressionVectorParameter>(ColorMaster.Get());
	SurfaceTint->ParameterName = TEXT("SurfaceTint");
	SurfaceTint->DefaultValue = FLinearColor::White;
	ColorMaster->GetExpressionCollection().AddExpression(SurfaceTint);
	ColorMaster->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, SurfaceTint);
	auto* SurfaceScalar = NewObject<UMaterialExpressionScalarParameter>(ColorMaster.Get());
	SurfaceScalar->ParameterName = TEXT("SurfaceScalar");
	SurfaceScalar->DefaultValue = 0.5f;
	ColorMaster->GetExpressionCollection().AddExpression(SurfaceScalar);
	ColorMaster->GetExpressionInputForProperty(MP_Roughness)->Connect(0, SurfaceScalar);
	auto* SurfaceTexture = NewObject<UMaterialExpressionTextureSampleParameter2D>(ColorMaster.Get());
	SurfaceTexture->ParameterName = TEXT("SurfaceTexture");
	SurfaceTexture->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	ColorMaster->GetExpressionCollection().AddExpression(SurfaceTexture);
	ColorMaster->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, SurfaceTexture);
	ColorMaster->PostEditChange();

	auto ColorParent = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	auto ColorOriginal = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	auto ColorBaked = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	ColorParent->SetParentEditorOnly(ColorMaster.Get());
	ColorOriginal->SetParentEditorOnly(ColorParent.Get());
	auto ColorBakedMaster = TStrongObjectPtr<UMaterial>(DuplicateObject<UMaterial>(ColorMaster.Get(), GetTransientPackage()));
	auto* RegisteredTint = NewObject<UMaterialExpressionVectorParameter>(ColorBakedMaster.Get());
	RegisteredTint->ParameterName = TEXT("MP_SurfaceTint");
	RegisteredTint->DefaultValue = FLinearColor::White;
	ColorBakedMaster->GetExpressionCollection().AddExpression(RegisteredTint);
	auto* RegisteredMultiply = NewObject<UMaterialExpressionMultiply>(ColorBakedMaster.Get());
	RegisteredMultiply->A = *ColorBakedMaster->GetExpressionInputForProperty(MP_BaseColor);
	RegisteredMultiply->B.Connect(0, RegisteredTint);
	ColorBakedMaster->GetExpressionCollection().AddExpression(RegisteredMultiply);
	ColorBakedMaster->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, RegisteredMultiply);
	ColorBakedMaster->PostEditChange();
	ColorBaked->SetParentEditorOnly(ColorBakedMaster.Get());
	ColorBaked->CopyMaterialUniformParametersEditorOnly(ColorOriginal.Get(), true);
	auto ColorData = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(DuplicateObject<UMeshParticleizerPreparedMesh>(Data.Get(), GetTransientPackage()));
	ColorData->SourceMaterials[0] = ColorOriginal.Get();
	ColorData->DissolveMaterials[0] = ColorBaked.Get();
	ColorData->bSupportsSurfaceTint = true;
	if (this->TestTrue(TEXT("Color fixture records resources"), ColorData->CaptureResourceManifest(Error)) == false)
	{
		this->AddError(Error);
		return false;
	}
	if (ColorData->CapturePreparationDependencies(Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	SecondMesh->SetMaterial(0, ColorOriginal.Get());
	this->TestTrue(TEXT("Copied fixed parameters match"), ColorData->ValidateBakedMaterialParameters(0, ColorOriginal.Get(), nullptr, Error));

	ColorParent->SetScalarParameterValueEditorOnly(TEXT("SurfaceScalar"), 0.25f);
	this->TestNull(TEXT("Inherited scalar change rejects preparation"), Manager->PrepareParticleize(SecondMesh, ColorData.Get(), Error));
	this->TestTrue(TEXT("Parameter rejection names the changed value"), Error.Contains(TEXT("SurfaceScalar")));
	this->TestEqual(TEXT("Parameter rejection preserves the source material"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ColorOriginal.Get()));
	ColorParent->SetScalarParameterValueEditorOnly(TEXT("SurfaceScalar"), 0.5f);
	ColorOriginal->SetTextureParameterValueEditorOnly(TEXT("SurfaceTexture"), UnusedTexture.Get());
	this->TestFalse(TEXT("Texture substitution rejects stale baked color"), ColorData->ValidateBakedMaterialParameters(0, ColorOriginal.Get(), nullptr, Error));
	ColorOriginal->SetTextureParameterValueEditorOnly(TEXT("SurfaceTexture"), SurfaceTexture->Texture);

	auto ColorReady = TStrongObjectPtr<UMeshParticleizeOperation>(Manager->PrepareParticleize(SecondMesh, ColorData.Get(), Error));
	if (this->TestNotNull(TEXT("Restored fixed parameters can prepare"), ColorReady.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto SurfaceParameters = FMeshParticleizerSurfaceColorParameters{};
	SurfaceParameters.Tint = FLinearColor(0.5f, 1, 0.25f);
	this->TestFalse(TEXT("Invalid surface source is rejected"), ColorReady->SetSurfaceColorParameters(1, 0, SurfaceParameters, Error));
	this->TestFalse(TEXT("Invalid surface material slot is rejected"), ColorReady->SetSurfaceColorParameters(0, -1, SurfaceParameters, Error));
	this->TestTrue(TEXT("Registered tint can be set while ready"), ColorReady->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	auto AppliedTint = Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0))->K2_GetVectorParameterValue(TEXT("MP_SurfaceTint"));
	this->TestEqual(TEXT("Registered source tint reaches the display"), AppliedTint, SurfaceParameters.Tint);
	SurfaceParameters.Tint.R = std::numeric_limits<float>::quiet_NaN();
	this->TestFalse(TEXT("Non-finite surface tint is rejected"), ColorReady->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	this->TestEqual(TEXT("Rejected surface tint preserves the display"), Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0))->K2_GetVectorParameterValue(TEXT("MP_SurfaceTint")), AppliedTint);
	ColorOriginal->SetVectorParameterValueEditorOnly(TEXT("SurfaceTint"), FLinearColor::Red);
	this->TestNull(TEXT("Source tint changed after preparation rejects start"), Manager->StartPreparedParticleize(ColorReady.Get(), Settings, Error));
	this->TestEqual(TEXT("Rejected color start reserves no particles"), ColorReady->GetGrantedParticleCount(), 0);
	ColorReady->RestoreMesh();
	this->TestEqual(TEXT("Rejected color start restores the original reference"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ColorOriginal.Get()));
	this->TestTrue(TEXT("Rejected color start leaves the mesh visible"), SecondMesh->GetVisibleFlag());

	ColorOriginal->SetVectorParameterValueEditorOnly(TEXT("SurfaceTint"), FLinearColor::White);

	// 正規更新の前に直接変更を検出し、開始や次フレームの書き込みで食い違いを隠さない。
	ColorReady.Reset(Manager->PrepareParticleize(SecondMesh, ColorData.Get(), Error));
	if (this->TestNotNull(TEXT("Managed-input fixture prepares"), ColorReady.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0))->SetScalarParameterValue(TEXT("MP_Progress"), 0.5f);
	this->TestNull(TEXT("Managed progress edit rejects start"), Manager->StartPreparedParticleize(ColorReady.Get(), Settings, Error));
	this->TestTrue(TEXT("Managed progress rejection names its input"), Error.Contains(TEXT("MP_Progress")));
	this->TestEqual(TEXT("Rejected managed edit reserves no particles"), ColorReady->GetGrantedParticleCount(), 0);
	ColorReady->RestoreMesh();

	Settings.ConversionDuration = 10;
	auto* ManagedParameters = Manager->StartParticleize(SecondMesh, ColorData.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Managed-input operation starts"), ManagedParameters) == false)
	{
		this->AddError(Error);
		return false;
	}
	SurfaceParameters.Tint = FLinearColor(0.25f, 0.5f, 1.f);
	this->TestTrue(TEXT("First deferred input accepted"), ManagedParameters->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	SurfaceParameters.Tint = FLinearColor(0.75f, 0.25f, 1.f);
	this->TestTrue(TEXT("Second deferred input still compares applied state"), ManagedParameters->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	Manager->Tick(0);
	this->TestEqual(TEXT("Deferred managed updates remain converting"), ManagedParameters->GetState(), EMeshParticleizeState::Converting);
	auto* ManagedDisplay = Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0));
	this->TestEqual(TEXT("Last deferred value reaches display"), ManagedDisplay->K2_GetVectorParameterValue(TEXT("MP_SurfaceTint")), SurfaceParameters.Tint);
	ManagedDisplay->SetVectorParameterValue(TEXT("MP_Origin"), FLinearColor(123, 456, 789, 0));
	this->TestFalse(TEXT("API update cannot conceal an external origin edit"), ManagedParameters->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	this->TestTrue(TEXT("External origin edit is diagnosed"), Error.Contains(TEXT("MP_Origin")));
	Manager->Tick(0);
	this->TestEqual(TEXT("External origin edit drains"), ManagedParameters->GetState(), EMeshParticleizeState::Draining);
	this->TestEqual(TEXT("Managed-input failure restores source"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ColorOriginal.Get()));
	ManagedParameters->Cancel(true);

	// 実行中も継承した固定値と表示用MIDを照合し、正規の遅延更新はそのまま通す。
	Settings.ConversionDuration = 10;
	Settings.bFreezeAtStart = false;
	auto* LiveParameters = Manager->StartParticleize(SecondMesh, ColorData.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Runtime parameter operation accepted"), LiveParameters) == false)
	{
		this->AddError(Error);
		return false;
	}
	SurfaceParameters.Tint = FLinearColor(0.25f, 0.5f, 1.f);
	this->TestTrue(TEXT("Registered tint can change while converting"), LiveParameters->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	Manager->Tick(0);
	this->TestEqual(TEXT("Registered deferred tint does not trigger material guard"), LiveParameters->GetState(), EMeshParticleizeState::Converting);
	ColorParent->SetScalarParameterValueEditorOnly(TEXT("SurfaceScalar"), 0.25f);
	Manager->Tick(0);
	this->TestEqual(TEXT("Inherited parameter change stops detachment"), LiveParameters->GetState(), EMeshParticleizeState::Draining);
	this->TestTrue(TEXT("Runtime diagnostic names inherited parameter"), LiveParameters->GetDiagnostics().Contains(TEXT("SurfaceScalar")));
	this->TestEqual(TEXT("Runtime parameter change restores original reference"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ColorOriginal.Get()));
	LiveParameters->Cancel(true);
	ColorParent->SetScalarParameterValueEditorOnly(TEXT("SurfaceScalar"), 0.5f);

	LiveParameters = Manager->StartParticleize(SecondMesh, ColorData.Get(), Settings, Error);
	if (this->TestNotNull(TEXT("Runtime display parameter operation accepted"), LiveParameters) == false)
	{
		this->AddError(Error);
		return false;
	}
	Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0))->SetTextureParameterValue(TEXT("SurfaceTexture"), UnusedTexture.Get());
	Manager->Tick(0);
	this->TestEqual(TEXT("Display texture substitution stops detachment"), LiveParameters->GetState(), EMeshParticleizeState::Draining);
	this->TestTrue(TEXT("Runtime diagnostic names changed display texture"), LiveParameters->GetDiagnostics().Contains(TEXT("SurfaceTexture")));
	LiveParameters->Cancel(true);

	ColorData->TextureColorSources[0].bRequiresColorProvider = true;
	ColorData->TextureColorSources[0].DependencyDiagnostics = TEXT("Register a Color Provider: Custom Primitive Data.");
	ColorReady.Reset(Manager->PrepareParticleize(SecondMesh, ColorData.Get(), Error));
	if (this->TestNotNull(TEXT("Dynamic color can prepare before provider registration"), ColorReady.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestNull(TEXT("Missing required provider rejects start"), Manager->StartPreparedParticleize(ColorReady.Get(), Settings, Error));
	this->TestTrue(TEXT("Missing provider diagnostic names its dependency"), Error.Contains(TEXT("Custom Primitive Data")));
	this->TestEqual(TEXT("Missing provider reserves no particles"), ColorReady->GetGrantedParticleCount(), 0);
	SurfaceParameters.Tint = FLinearColor::White;
	this->TestFalse(TEXT("Required provider cannot be cleared with a tint-only update"), ColorReady->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	ColorReady->RestoreMesh();
	this->TestEqual(TEXT("Missing provider preserves the original source"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ColorOriginal.Get()));
	ColorData->TextureColorSources[0].bRequiresColorProvider = false;
	ColorReady.Reset(Manager->PrepareParticleize(SecondMesh, ColorData.Get(), Error));
	if (this->TestNotNull(TEXT("Color fixture can prepare again"), ColorReady.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	auto* ColorDisplay = Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0));
	if (this->TestNotNull(TEXT("Prepared display is a dynamic material"), ColorDisplay) == false)
	{
		return false;
	}
	this->TestEqual(TEXT("New preparation resets the registered tint"), ColorDisplay->K2_GetVectorParameterValue(TEXT("MP_SurfaceTint")), FLinearColor::White);
	ColorDisplay->SetScalarParameterValue(TEXT("MP_Progress"), -1);
	this->TestTrue(TEXT("Dissolve control is independent of fixed color"), ColorData->ValidateBakedMaterialParameters(0, ColorOriginal.Get(), ColorDisplay, Error));
	ColorDisplay->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor::Blue);
	this->TestNull(TEXT("Externally edited display tint rejects start"), Manager->StartPreparedParticleize(ColorReady.Get(), Settings, Error));
	ColorReady->RestoreMesh();

	// 色専用の値だけはProviderへ委譲できる。準備時のMID参照と変更値を復元する。
	ColorData->bSupportsColorProvider = true;
	ColorData->Coordinates = Texture.Get();
	ColorData->TextureColorSources[0].ProviderVectorParameters = {TEXT("SurfaceTint")};
	auto ChangedMID = TStrongObjectPtr<UMaterialInstanceDynamic>(UMaterialInstanceDynamic::Create(ColorOriginal.Get(), GetTransientPackage()));
	ChangedMID->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor::Red);
	this->TestFalse(TEXT("Changed MID still requires a dynamic color route"), ColorData->ValidateBakedMaterialParameters(0, ChangedMID.Get(), nullptr, Error));
	this->TestTrue(TEXT("Color-only mismatch accepts a provider route"), ColorData->ValidateBakedMaterialParameters(0, ChangedMID.Get(), nullptr, Error, true));
	ChangedMID->SetScalarParameterValue(TEXT("SurfaceScalar"), 0.2f);
	this->TestFalse(TEXT("Provider cannot conceal a non-color mismatch"), ColorData->ValidateBakedMaterialParameters(0, ChangedMID.Get(), nullptr, Error, true));
	ChangedMID->SetScalarParameterValue(TEXT("SurfaceScalar"), 0.5f);
	ChangedMID->SetTextureParameterValue(TEXT("SurfaceTexture"), UnusedTexture.Get());
	this->TestFalse(TEXT("Provider cannot bypass texture resource validation"), ColorData->ValidateBakedMaterialParameters(0, ChangedMID.Get(), nullptr, Error, true));
	ChangedMID->SetTextureParameterValue(TEXT("SurfaceTexture"), SurfaceTexture->Texture);
	ChangedMID->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor(std::numeric_limits<float>::quiet_NaN(), 0, 0, 1));
	this->TestFalse(TEXT("Provider fallback rejects non-finite source values"), ColorData->ValidateBakedMaterialParameters(0, ChangedMID.Get(), nullptr, Error, true));
	ChangedMID->SetVectorParameterValue(TEXT("SurfaceTint"), FLinearColor::Red);
	SecondMesh->SetMaterial(0, ChangedMID.Get());
	ColorReady.Reset(Manager->PrepareParticleize(SecondMesh, ColorData.Get(), Error));
	if (this->TestNotNull(TEXT("Color-only changed MID can prepare before registration"), ColorReady.Get()) == false)
	{
		this->AddError(Error);
		return false;
	}
	ColorDisplay = Cast<UMaterialInstanceDynamic>(SecondMesh->GetMaterial(0));
	this->TestEqual(TEXT("Preparation preserves the changed color before registration"), ColorDisplay->K2_GetVectorParameterValue(TEXT("SurfaceTint")), FLinearColor::Red);
	this->TestNull(TEXT("Changed MID without provider rejects start"), Manager->StartPreparedParticleize(ColorReady.Get(), Settings, Error));
	this->TestTrue(TEXT("Fallback diagnostic requests a provider"), Error.Contains(TEXT("Color Provider")));
	this->TestEqual(TEXT("Rejected MID fallback reserves no particles"), ColorReady->GetGrantedParticleCount(), 0);
	this->TestFalse(TEXT("Tint-only registration cannot hide the mismatch"), ColorReady->SetSurfaceColorParameters(0, 0, SurfaceParameters, Error));
	ColorReady->RestoreMesh();
	this->TestEqual(TEXT("Fallback rejection restores the original MID reference"), SecondMesh->GetMaterial(0), static_cast<UMaterialInterface*>(ChangedMID.Get()));
	this->TestEqual(TEXT("Fallback restoration preserves the user's value"), ChangedMID->K2_GetVectorParameterValue(TEXT("SurfaceTint")), FLinearColor::Red);
	SecondMesh->SetMaterial(0, Original);
	ADD_LATENT_AUTOMATION_COMMAND(FValidateSurfaceTextureAsync(this));
	return true;
}
#endif
