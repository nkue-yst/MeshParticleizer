#include "MeshParticleizerPreview.h"
#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerPreset.h"
#include "MeshParticleizerParticleStyle.h"
#include "MeshParticleizerComponent.h"
#include "MeshParticleizeAsyncAction.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CustomEvent.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"
#include "Curves/CurveFloat.h"
#include "Engine/Texture2D.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "AdvancedPreviewScene.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerPreviewLifecycleTest, "MeshParticleizer.Editor.PreviewLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerPreviewLifecycleTest::RunTest(const FString& Parameters)
{
	// ウィンドウとEditorの双方からCloseされても、Worldの終了を一度だけ行う。
	auto Session = TStrongObjectPtr<UMeshParticleizerPreview>(NewObject<UMeshParticleizerPreview>());
	auto* World = Session->GetScene()->GetWorld();
	this->TestTrue(TEXT("Preview world begins play"), World->HasBegunPlay());
	this->TestNotNull(TEXT("Preview world owns runtime budgets"), World->GetSubsystem<UMeshParticleizerWorldSubsystem>());
	Session->Advance(1.0f / 60);
	this->TestEqual(TEXT("Paused preview time remains zero"), World->GetTimeSeconds(), 0.0);
	this->TestFalse(TEXT("Preview does not dirty a map"), World->GetOutermost()->IsDirty());

	// シークの最後の端数をWorld標準の最小刻みへ拡大しない。
	Session->SetPaused(false);
	const auto BeforeTinyStep = World->GetTimeSeconds();
	Session->Advance(0.00001f);
	this->TestTrue(TEXT("A short final seek step keeps the requested world time"),
		FMath::Abs(World->GetTimeSeconds() - BeforeTinyStep - 0.00001) < 0.00000001);
	Session->SetPaused(true);

	// プリセットは検証したコピーを渡す。個別の上書きや不正入力で共有設定を壊さない。
	auto Preset = TStrongObjectPtr<UMeshParticleizerPreset>(NewObject<UMeshParticleizerPreset>());
	auto PresetOptions = FMeshParticleizeOptions{};
	PresetOptions.ConversionDuration = 2;
	PresetOptions.ParticleLifetime = 6;
	PresetOptions.Origin = EMeshParticleizerOrigin::MultiplePoints;
	PresetOptions.LocalOrigins = {FVector(1, 2, 3), FVector(4, 5, 6)};
	auto PresetError = FString{};
	this->TestTrue(TEXT("Preset accepts reusable options"), Preset->SetOptions(PresetOptions, PresetError));
	this->TestTrue(TEXT("Preview applies preset defaults"), Session->ApplyPreset(Preset.Get(), PresetError));
	auto Player = TStrongObjectPtr<UMeshParticleizerComponent>(NewObject<UMeshParticleizerComponent>());
	this->TestTrue(TEXT("Runtime applies the same preset defaults"), Player->ApplyPreset(Preset.Get(), PresetError));
	this->TestTrue(TEXT("Preview and runtime settings match"), FMeshParticleizeOptions::StaticStruct()->CompareScriptStruct(&Session->Options, &Player->Options, 0));
	Player->Options.LocalOrigins[0] = FVector(7, 8, 9);
	this->TestEqual(TEXT("Runtime edits do not modify the preset"), Preset->Options.LocalOrigins[0], FVector(1, 2, 3));
	this->TestEqual(TEXT("Runtime edits do not modify another consumer"), Session->Options.LocalOrigins[0], FVector(1, 2, 3));

	Preset->Options.ParticleLifetime = -1;
	this->TestFalse(TEXT("Invalid stored preset is rejected before restoring playback"), Session->ApplyPreset(Preset.Get(), PresetError));
	this->TestEqual(TEXT("Rejected preset keeps playback settings"), Session->Options.ParticleLifetime, 6.0f);
	this->TestFalse(TEXT("Runtime rejects invalid stored preset"), Player->ApplyPreset(Preset.Get(), PresetError));
	this->TestEqual(TEXT("Rejected runtime preset preserves settings"), Player->Options.ParticleLifetime, 6.0f);
	auto Copy = FMeshParticleizeOptions{};
	this->TestFalse(TEXT("Invalid preset does not publish options"), Preset->CopyOptions(Copy, PresetError));
	this->TestEqual(TEXT("Rejected copy preserves the output"), Copy.ParticleLifetime, 3.0f);
	this->TestTrue(TEXT("Preset can be corrected"), Preset->SetOptions(PresetOptions, PresetError));

	auto HitMesh = TStrongObjectPtr<UStaticMeshComponent>(NewObject<UStaticMeshComponent>());
	PresetOptions.HitOrigin.SourceMesh = HitMesh.Get();
	this->TestFalse(TEXT("Preset cannot retain a world-specific hit"), Preset->SetOptions(PresetOptions, PresetError));
	PresetOptions.HitOrigin.SourceMesh.Reset();
	auto UnsavedCurve = TStrongObjectPtr<UCurveFloat>(NewObject<UCurveFloat>());
	PresetOptions.ProgressCurve = UnsavedCurve.Get();
	this->TestFalse(TEXT("Unsaved progress curve is not a reusable asset reference"), Preset->SetOptions(PresetOptions, PresetError));
	this->TestNull(TEXT("A rejected curve leaves the preset unchanged"), Preset->Options.ProgressCurve.Get());
	PresetOptions.ProgressCurve.Reset();
	auto UnsavedStyle = TStrongObjectPtr<UMeshParticleizerParticleStyle>(NewObject<UMeshParticleizerParticleStyle>());
	PresetOptions.ParticleStyle = UnsavedStyle.Get();
	this->TestFalse(TEXT("Unsaved particle style cannot enter reusable playback settings"), Preset->SetOptions(PresetOptions, PresetError));
	this->TestNull(TEXT("A rejected style preserves the previous preset"), Preset->Options.ParticleStyle.Get());
	this->TestNull(TEXT("Preview does not save into plugin or engine content"), Session->SavePreset(TEXT("/MeshParticleizer/RejectedPreset"), PresetError));
	this->TestTrue(TEXT("Reset after preset checks"), Session->SetOptions(FMeshParticleizeOptions{}, PresetError));

	// 実際のAsyncノードで診断ピン、時間の既定値、詳細起点の折畳みを検証する。
	auto Blueprint = TStrongObjectPtr<UBlueprint>(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
		GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("ParticleizerBasicApi")), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
	auto* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("BasicApi"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(), Graph);
	for (const auto Name : {TEXT("StartParticleize"), TEXT("ParticleizeMeshSimple"), TEXT("PrepareForParticleize")})
	{
		const auto bPreparation = FString(Name) == TEXT("PrepareForParticleize");
		auto* ProxyClass = bPreparation ? UMeshParticleizePrepareAsyncAction::StaticClass() : UMeshParticleizeAsyncAction::StaticClass();
		auto* Node = NewObject<UK2Node_AsyncAction>(Graph);
		Node->InitializeProxyFromFunction(ProxyClass->FindFunctionByName(Name));
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		this->TestNotNull(TEXT("Async node exposes failure execution"), Node->FindPin(TEXT("Failed"), EGPD_Output));
		this->TestNotNull(TEXT("Async node exposes diagnostic data"), Node->FindPin(bPreparation ? TEXT("Diagnostics") : TEXT("Message"), EGPD_Output));
		if (bPreparation == false)
		{
			this->TestNotNull(TEXT("Async node exposes the finish reason"), Node->FindPin(TEXT("Reason"), EGPD_Output));
			this->TestNotNull(TEXT("Async node exposes its operation"), Node->FindPin(TEXT("Operation"), EGPD_Output));
			const auto* Duration = Node->FindPin(TEXT("ConversionDuration"), EGPD_Input);
			const auto* Lifetime = Node->FindPin(TEXT("ParticleLifetime"), EGPD_Input);
			const auto* OriginPin = Node->FindPin(TEXT("Origin"), EGPD_Input);
			this->TestTrue(TEXT("Timing defaults keep preset values"), Duration != nullptr && Lifetime != nullptr && FCString::Atof(*Duration->DefaultValue) == -1.f && FCString::Atof(*Lifetime->DefaultValue) == -1.f);
			this->TestTrue(TEXT("Detailed origin is collapsed and accepts a default input"), OriginPin != nullptr && OriginPin->bAdvancedView && OriginPin->bDefaultValueIsIgnored == false);
		}

		auto* Event = NewObject<UK2Node_CustomEvent>(Graph);
		Event->CustomFunctionName = *FString::Printf(TEXT("Test_%s"), Name);
		Graph->AddNode(Event, false, false);
		Event->CreateNewGuid();
		Event->AllocateDefaultPins();
		this->TestTrue(TEXT("Async factory connects to an event"), Graph->GetSchema()->TryCreateConnection(Event->FindPin(UEdGraphSchema_K2::PN_Then), Node->FindPin(UEdGraphSchema_K2::PN_Execute)));
	}
	auto CompileLog = FCompilerResultsLog{};
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::None, &CompileLog);
	this->TestEqual(TEXT("Basic async graphs compile"), CompileLog.NumErrors, 0);
	this->TestNotNull(TEXT("Basic async graphs produce an executable class"), Blueprint->GeneratedClass.Get());

	// 表示材を準備できない場合は元の表示を保ち、UIから失敗の理由を確認できる。
	auto DisplayError = FString{};
	this->TestFalse(TEXT("Order view rejects missing prepared data"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::Order, DisplayError));
	this->TestEqual(TEXT("Rejected display preserves the previous mode"), Session->DisplayMode, EMeshParticleizerPreviewDisplay::Effect);
	this->TestTrue(TEXT("Display failure reaches the visible status"), DisplayError.IsEmpty() == false && Session->GetStatus() == DisplayError);

	this->TestFalse(TEXT("Surface color rejects missing prepared data"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::SurfaceColor, DisplayError));
	this->TestEqual(TEXT("Rejected surface color preserves the previous mode"), Session->DisplayMode, EMeshParticleizerPreviewDisplay::Effect);

	this->TestFalse(TEXT("Velocity rejects missing prepared data"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::Velocity, DisplayError));
	this->TestEqual(TEXT("Rejected velocity preserves the previous mode"), Session->DisplayMode, EMeshParticleizerPreviewDisplay::Effect);
	this->TestTrue(TEXT("Velocity display settings accepted"), Session->SetVelocityDisplay(250, 128, DisplayError));
	this->TestFalse(TEXT("Non-finite velocity scale rejected"), Session->SetVelocityDisplay(std::numeric_limits<float>::quiet_NaN(), 64, DisplayError));
	this->TestFalse(TEXT("Zero sample stride rejected"), Session->SetVelocityDisplay(100, 0, DisplayError));
	this->TestEqual(TEXT("Invalid input preserves velocity scale"), Session->VelocityMaxSpeed, 250.0f);
	this->TestEqual(TEXT("Invalid input preserves sample stride"), Session->VelocitySampleStride, 128);

	this->TestFalse(TEXT("Samples reject missing prepared data"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::Samples, DisplayError));
	this->TestEqual(TEXT("Rejected samples preserve the previous mode"), Session->DisplayMode, EMeshParticleizerPreviewDisplay::Effect);
	this->TestTrue(TEXT("Sample display stride accepted"), Session->SetSampleDisplay(-1, 20, DisplayError));
	this->TestFalse(TEXT("Unavailable material slot rejected"), Session->SetSampleDisplay(0, 5, DisplayError));
	this->TestFalse(TEXT("Zero sample stride rejected"), Session->SetSampleDisplay(-1, 0, DisplayError));
	this->TestEqual(TEXT("Invalid sample settings preserve the stride"), Session->SampleDisplayStride, 20);
	this->TestEqual(TEXT("Invalid sample settings preserve all slots"), Session->SampleMaterialSlot, -1);

	// 候補IDとslotの対応、座標変換、フィルタを小さな準備Textureで検証する。
	auto Data = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	auto Texture = TStrongObjectPtr<UTexture2D>(NewObject<UTexture2D>());
	Data->SampleCount = 2;
	Data->SourceMaterials.SetNum(2);
	Data->Positions = Texture.Get();
	const FLinearColor Invalid[] = {FLinearColor(1, 2, 3, 0), FLinearColor(4, 5, 6, 3)};
	Texture->Source.Init(2, 1, 1, 1, TSF_RGBA32F, reinterpret_cast<const uint8*>(Invalid));
	Session->Prepared = Data.Get();
	Session->Mesh = NewObject<UStaticMeshComponent>(Session.Get());
	Session->Mesh->SetWorldLocation(FVector(100, 200, 300));
	this->TestFalse(TEXT("Invalid stored slot rejects the complete sample display"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::Samples, DisplayError));
	this->TestEqual(TEXT("Invalid stored data publishes no partial points"), Session->GetSamplePoints().Num(), 0);

	const FLinearColor Valid[] = {FLinearColor(1, 2, 3, 0), FLinearColor(4, 5, 6, 1)};
	Texture->Source.Init(2, 1, 1, 1, TSF_RGBA32F, reinterpret_cast<const uint8*>(Valid));
	this->TestTrue(TEXT("All samples selected"), Session->SetSampleDisplay(-1, 1, DisplayError));
	this->TestTrue(TEXT("Valid prepared samples accepted"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::Samples, DisplayError));
	this->TestEqual(TEXT("All sample positions published"), Session->GetSamplePoints().Num(), 2);
	this->TestEqual(TEXT("Per-slot count preserves prepared mapping"), Session->GetPreparedSampleCount(1), 1);
	this->TestTrue(TEXT("Second slot selected"), Session->SetSampleDisplay(1, 1, DisplayError));
	const auto Points = Session->GetSamplePoints();
	if (this->TestEqual(TEXT("Filter publishes one point"), Points.Num(), 1))
	{
		this->TestEqual(TEXT("Filtering preserves sample ID"), Points[0].SampleId, 1);
		this->TestEqual(TEXT("Sample position includes source transform"), Points[0].WorldPosition, FVector(104, 205, 306));
	}
	this->TestTrue(TEXT("Returning to normal view clears diagnostic points"), Session->SetDisplayMode(EMeshParticleizerPreviewDisplay::Effect, DisplayError));
	this->TestEqual(TEXT("Normal view has no diagnostic points"), Session->GetSamplePoints().Num(), 0);

	Session->Close();
	this->TestFalse(TEXT("Closing preview ends play before scene cleanup"), World->HasBegunPlay());
	this->TestNull(TEXT("A retained closed session releases its preview scene"), Session->GetScene());
	Session->Close();
	Session->Restore();
	Session->Advance(1.0f / 60);
	this->TestEqual(TEXT("Repeated close stays closed"), Session->GetStatus(), FString(TEXT("Closed")));
	this->TestEqual(TEXT("Closed preview has no reservation"), Session->GetBudgetUsage().ReservedSimulationBytes, int64(0));
	this->TestEqual(TEXT("Closed preview has no pinned assets"), Session->GetAssetCacheUsage().PinnedBytes, int64(0));

	// 実際のViewportのドラッグ経路で座標変換と選択中の起点だけの変更を確認する。
	auto HandleSession = TStrongObjectPtr<UMeshParticleizerPreview>(UMeshParticleizerPreparationLibrary::OpenPreviewWindow(nullptr));
	auto* Mesh = NewObject<UStaticMeshComponent>(HandleSession.Get(), NAME_None, RF_Transient);
	HandleSession->Mesh = Mesh;
	const auto Transform = FTransform(FRotator(0, 90, 0), FVector(30, -40, 50), FVector(2, 1, 0.5));
	Mesh->SetWorldTransform(Transform);
	auto Settings = FMeshParticleizeOptions{};
	Settings.Origin = EMeshParticleizerOrigin::Point;
	Settings.LocalOrigin = FVector(10, 20, 30);
	auto Error = FString{};
	this->TestTrue(TEXT("Point options accepted"), HandleSession->SetOptions(Settings, Error));
	this->TestTrue(TEXT("Point handle enabled"), HandleSession->EditOrigin(0, Error));
	auto* Client = static_cast<FEditorViewportClient*>(nullptr);
	for (auto* Candidate : GEditor->GetAllViewportClients())
	{
		if (Candidate->GetWorld() == HandleSession->GetScene()->GetWorld())
		{
			Client = Candidate;
			break;
		}
	}
	if (this->TestNotNull(TEXT("Real preview viewport is present"), Client))
	{
		this->TestEqual(TEXT("Origin uses translate widget"), Client->GetWidgetMode(), UE::Widget::WM_Translate);
		this->TestTrue(TEXT("Widget position includes source transform"), Client->GetWidgetLocation().Equals(Transform.TransformPosition(Settings.LocalOrigin)));
		auto Drag = FVector(0, 20, 0);
		auto Rotation = FRotator::ZeroRotator;
		auto Scale = FVector::ZeroVector;
		this->TestTrue(TEXT("Viewport drag updates point"), Client->InputWidgetDelta(Client->Viewport, EAxisList::Y, Drag, Rotation, Scale));
		this->TestTrue(TEXT("World drag converts to reference coordinates"), HandleSession->Options.LocalOrigin.Equals(FVector(20, 20, 30)));

		Settings.Origin = EMeshParticleizerOrigin::MultiplePoints;
		Settings.LocalOrigins = {FVector(1, 2, 3), FVector(10, 20, 30)};
		this->TestTrue(TEXT("Multiple point options accepted"), HandleSession->SetOptions(Settings, Error));
		this->TestTrue(TEXT("Second origin selected"), HandleSession->EditOrigin(1, Error));
		this->TestTrue(TEXT("Viewport drag updates selected point"), Client->InputWidgetDelta(Client->Viewport, EAxisList::Y, Drag, Rotation, Scale));
		this->TestEqual(TEXT("First origin remains unchanged"), HandleSession->Options.LocalOrigins[0], Settings.LocalOrigins[0]);
		this->TestTrue(TEXT("Second origin moved"), HandleSession->Options.LocalOrigins[1].Equals(FVector(20, 20, 30)));
		this->TestFalse(TEXT("Invalid origin index rejected"), HandleSession->EditOrigin(2, Error));
		this->TestFalse(TEXT("Non-finite position rejected"), HandleSession->SetOriginHandlePosition(FVector(std::numeric_limits<double>::quiet_NaN(), 0, 0), Error));
		this->TestTrue(TEXT("Invalid input preserves selected position"), HandleSession->Options.LocalOrigins[1].Equals(FVector(20, 20, 30)));
		HandleSession->StopEditingOrigin();
		this->TestEqual(TEXT("Widget hidden outside origin editing"), Client->GetWidgetMode(), UE::Widget::WM_None);
		this->TestFalse(TEXT("Hidden widget cannot change origin"), Client->InputWidgetDelta(Client->Viewport, EAxisList::Y, Drag, Rotation, Scale));
	}
	HandleSession->Close();
	return true;
}
#endif
