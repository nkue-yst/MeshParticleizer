#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreview.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerPreset.h"
#include "MeshParticleizeOperation.h"
#include "AdvancedPreviewScene.h"
#include "AssetEditorModeManager.h"
#include "Animation/AnimationAsset.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/PackageName.h"
#include "Materials/MaterialInterface.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "IStructureDetailsView.h"
#include "PropertyEditorModule.h"
#include "PropertyCustomizationHelpers.h"
#include "SEditorViewport.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshParticleizerPreview"

class FMeshParticleizerPreviewClient : public FEditorViewportClient
{
public:
	FMeshParticleizerPreviewClient(UMeshParticleizerPreview* InSession, const TSharedRef<SEditorViewport>& Widget)
		: FEditorViewportClient(nullptr, InSession->GetScene(), Widget), Session(InSession)
	{
		// このViewportはFWidgetのドラッグを扱う。選択Actor用ITFへ描画を横取りさせない。
		this->ModeTools->SetSupportsViewportITF(false);
		StaticCastSharedPtr<FAssetEditorModeManager>(this->ModeTools)->SetPreviewScene(InSession->GetScene());
		this->SetViewMode(VMI_Lit);
		this->SetRealtime(true);
		this->EngineShowFlags.SetGrid(false);
		this->SetViewLocation(FVector(-260, 340, 190));
		this->SetViewRotation(FRotator(-12, -55, 0));
	}

	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		this->Session->Advance(DeltaSeconds);
		this->EngineShowFlags.SetParticles(this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Order && this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Samples);
		this->EngineShowFlags.SetNiagara(this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Order && this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Samples);

		// 診断色が露出やBloomで白飛びしないよう、診断表示ではポスト処理を外す。
		this->EngineShowFlags.SetPostProcessing(this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Order &&
			this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::SurfaceColor &&
			this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Velocity &&
			this->Session->DisplayMode != EMeshParticleizerPreviewDisplay::Samples);
		if (this->Session->Mesh != nullptr && this->LastMesh != this->Session->Mesh)
		{
			this->LastMesh = this->Session->Mesh;
			this->FocusViewportOnBox(this->Session->Mesh->Bounds.GetBox().ExpandBy(30), true);
		}
	}

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
	{
		FEditorViewportClient::Draw(View, PDI);
		this->Session->DrawSamples(View, PDI);
	}

	virtual FLinearColor GetBackgroundColor() const override
	{
		return FLinearColor(0.035f, 0.045f, 0.06f);
	}

	virtual UE::Widget::EWidgetMode GetWidgetMode() const override
	{
		return this->Session->bEditingOrigin == true && this->Session->CanEditOrigin() == true ? UE::Widget::WM_Translate : UE::Widget::WM_None;
	}

	virtual FVector GetWidgetLocation() const override
	{
		return this->Session->GetOriginHandlePosition();
	}
	virtual FMatrix GetWidgetCoordSystem() const override
	{
		return FMatrix::Identity;
	}
	virtual ECoordSystem GetWidgetCoordSystemSpace() const override
	{
		return COORD_World;
	}

	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type Axis, FVector& Drag, FRotator& Rot, FVector& Scale) override
	{
		if (Axis == EAxisList::None || this->GetWidgetMode() != UE::Widget::WM_Translate)
		{
			return false;
		}
		auto Error = FString{};
		return this->Session->SetOriginHandlePosition(this->GetWidgetLocation() + Drag, Error);
	}

private:
	TStrongObjectPtr<UMeshParticleizerPreview> Session;
	TWeakObjectPtr<UMeshComponent> LastMesh;
};

class SMeshParticleizerPreviewViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SMeshParticleizerPreviewViewport)
	{
	}
		SLATE_ARGUMENT(UMeshParticleizerPreview*, Session)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		this->Session = Args._Session;
		SEditorViewport::Construct(SEditorViewport::FArguments());
	}

	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
	{
		return MakeShared<FMeshParticleizerPreviewClient>(this->Session.Get(), SharedThis(this));
	}

private:
	TWeakObjectPtr<UMeshParticleizerPreview> Session;
};

class SMeshParticleizerPreview : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMeshParticleizerPreview)
	{
	}
		SLATE_ARGUMENT(UMeshParticleizerPreview*, Session)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		this->Session.Reset(Args._Session);
		this->EditedOptions = MakeShared<FStructOnScope>(FMeshParticleizeOptions::StaticStruct());
		*this->GetEditedOptions() = this->Session->Options;
		auto DetailsArgs = FDetailsViewArgs{};
		DetailsArgs.bAllowSearch = true;
		DetailsArgs.bShowScrollBar = true;
		this->Details = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"))
																					 .CreateStructureDetailView(DetailsArgs, FStructureDetailsViewArgs{}, this->EditedOptions);
		this->Details->GetOnFinishedChangingPropertiesDelegate().AddSP(this, &SMeshParticleizerPreview::OptionsChanged);
		this->Session->OnOptionsChanged.AddSP(this, &SMeshParticleizerPreview::RefreshOptions);
		for (const auto* Label : {TEXT("Stationary"), TEXT("Move"), TEXT("Rotate")})
		{
			this->Movements.Add(MakeShared<FString>(Label));
		}
		this->Displays = {MakeShared<FString>(TEXT("Effect")), MakeShared<FString>(TEXT("Load")), MakeShared<FString>(TEXT("Order")), MakeShared<FString>(TEXT("Surface color")), MakeShared<FString>(TEXT("Velocity")), MakeShared<FString>(TEXT("Samples"))};
		this->SampleSlots = {MakeShared<FString>(TEXT("All slots"))};

		this->ChildSlot
			[SNew(SBorder).Padding(12)
					[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)[SNew(STextBlock).Text(LOCTEXT("Prepared", "Prepared mesh"))] + SHorizontalBox::Slot().FillWidth(1)[SNew(SObjectPropertyEntryBox).AllowedClass(UMeshParticleizerPreparedMesh::StaticClass()).ObjectPath_Lambda([this] {
																																																																																																	 return GetPathNameSafe(this->Session->Prepared);
																																																																																																 })
																																																																			 .OnObjectChanged_Lambda([this](const FAssetData& Asset) {
																																																																				 auto Error = FString{};
																																																																				 this->Session->SetPreparedData(Cast<UMeshParticleizerPreparedMesh>(Asset.GetAsset()), Error);
																																																																			 })] +
											  SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 6, 0)[SNew(STextBlock).Text(LOCTEXT("Display", "View"))] + SHorizontalBox::Slot().AutoWidth()[SAssignNew(this->DisplayCombo, SComboBox<TSharedPtr<FString>>).OptionsSource(&this->Displays).InitiallySelectedItem(this->Displays[0]).OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
																																																																																							  return SNew(STextBlock).Text(FText::FromString(*Item));
																																																																																						  })
																																														 .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type) {
																																															 auto Error = FString{};
																																															 this->Session->SetDisplayMode(static_cast<EMeshParticleizerPreviewDisplay>(this->Displays.IndexOfByKey(Item)), Error);
																																														 })[SNew(STextBlock).Text_Lambda([this] {
																																															 return FText::FromString(*this->Displays[int32(this->Session->DisplayMode)]);
																																														 })]]] +
						SVerticalBox::Slot().FillHeight(1)
							[SNew(SSplitter) + SSplitter::Slot().Value(0.72f)[SNew(SMeshParticleizerPreviewViewport).Session(this->Session.Get())] + SSplitter::Slot().Value(0.28f)[SNew(SWidgetSwitcher).WidgetIndex_Lambda([this] {
								return this->Session->DisplayMode == EMeshParticleizerPreviewDisplay::Load ? 1 : 0;
							}) + SWidgetSwitcher::Slot()[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 12)[SNew(SVerticalBox).Visibility_Lambda([this] {
								return this->Session->DisplayMode == EMeshParticleizerPreviewDisplay::Order ? EVisibility::Visible : EVisibility::Collapsed;
							}) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("OrderTitle", "Surface separation order")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))] +
																				  SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 8)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.015f, 0.12f, 0.8f)).Padding(6)[SNew(STextBlock).Text(LOCTEXT("EarlyOrder", "Early"))]] + SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.02f, 0.8f, 0.5f)).Padding(6)[SNew(STextBlock).Text(LOCTEXT("MiddleOrder", "Middle"))]] + SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(1, 0.12f, 0.015f)).Padding(6)[SNew(STextBlock).Text(LOCTEXT("LateOrder", "Late"))]]] + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("OrderHelp", "Restore to inspect the whole surface. During playback, colors show the remaining surface. Detached particles are hidden in this view."))]] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(STextBlock).Visibility_Lambda([this] {
																												 return this->Session->DisplayMode == EMeshParticleizerPreviewDisplay::SurfaceColor ? EVisibility::Visible : EVisibility::Collapsed;
																											 })
											 .AutoWrapText(true)
											 .Text(LOCTEXT("SurfaceColorHelp", "Inherited surface color on the mesh and detached particles. Lighting, emission and effect tint are excluded."))] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(SVerticalBox).Visibility_Lambda([this] {
										 return this->Session->DisplayMode == EMeshParticleizerPreviewDisplay::Velocity ? EVisibility::Visible : EVisibility::Collapsed;
									 }) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("VelocityTitle", "Particle velocity")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))] +
										 SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 8)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.015f, 0.12f, 0.8f)).Padding(6)[SNew(STextBlock).Text(LOCTEXT("SlowVelocity", "Slow"))]] + SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(0.02f, 0.8f, 0.5f)).Padding(6)[SNew(STextBlock).Text(LOCTEXT("MidVelocity", "Half"))]] + SHorizontalBox::Slot().FillWidth(1)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(1, 0.12f, 0.015f)).Padding(6)[SNew(STextBlock).Text(LOCTEXT("FastVelocity", "Scale+"))]]] + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("VelocityHelp", "Arrows show direction and projected speed. Gray dots are stopped. End-on motion uses a ring with a dot toward you, or a cross away."))] + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 6)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(LOCTEXT("VelocityScale", "Scale (cm/s)"))] + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(85)[SNew(SNumericEntryBox<float>).MinValue(1).MaxValue(100000).Value_Lambda([this] {
																																																																																																																																																																																																																																																																																																																																																		 return TOptional<float>(this->Session->VelocityMaxSpeed);
																																																																																																																																																																																																																																																																																																																																																	 })
																																																																																																																																																																																																																																																																																																																				   .OnValueCommitted_Lambda([this](float Value, ETextCommit::Type) {
																																																																																																																																																																																																																																																																																																																					   auto Error = FString{};
																																																																																																																																																																																																																																																																																																																					   this->Session->SetVelocityDisplay(Value, this->Session->VelocitySampleStride, Error);
																																																																																																																																																																																																																																																																																																																				   })]]] +
										 SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(LOCTEXT("VelocityStride", "Show about 1 in"))] + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(85)[SNew(SNumericEntryBox<int32>).MinValue(1).MaxValue(4096).Value_Lambda([this] {
																																																																																							   return TOptional<int32>(this->Session->VelocitySampleStride);
																																																																																						   })
																																																										   .OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type) {
																																																											   auto Error = FString{};
																																																											   this->Session->SetVelocityDisplay(this->Session->VelocityMaxSpeed, Value, Error);
																																																										   })]]]] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(SVerticalBox).Visibility_Lambda([this] {
										 return this->Session->DisplayMode == EMeshParticleizerPreviewDisplay::Samples ? EVisibility::Visible : EVisibility::Collapsed;
									 }) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("SamplesTitle", "Surface samples")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))] +
										 SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 6)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
											 const auto* Data = this->Session->Prepared.Get();
											 return Data != nullptr ? FText::Format(LOCTEXT("SampleDensity", "Sampling LOD {2} · {0} candidates\n{1} / m² of reference surface"),
																		  FText::AsNumber(Data->SampleCount), FText::AsNumber(Data->SurfaceArea > 0 ? FMath::RoundToInt(Data->SampleCount * 10000.0 / Data->SurfaceArea) : 0), FText::AsNumber(Data->SamplingLOD))
																	: FText::GetEmpty();
										 })] +
										 SVerticalBox::Slot().AutoHeight()[SAssignNew(this->SampleCombo, SComboBox<TSharedPtr<FString>>).OptionsSource(&this->SampleSlots).InitiallySelectedItem(this->SampleSlots[0]).OnGenerateWidget_Lambda([this](TSharedPtr<FString> Item) {
																																																						  const auto Slot = this->SampleSlots.IndexOfByKey(Item) - 1;
																																																						  return SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)[SNew(SBox).WidthOverride(12).HeightOverride(12)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(Slot < 0 ? FLinearColor::White : UMeshParticleizerPreview::GetSampleSlotColor(Slot))]] + SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(FText::FromString(*Item))];
																																																					  })
												 .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type Selection) {
													 if (Selection == ESelectInfo::Direct)
													 {
														 return;
													 }
													 auto Error = FString{};
													 this->Session->SetSampleDisplay(this->SampleSlots.IndexOfByKey(Item) - 1, this->Session->SampleDisplayStride, Error);
												 })[SNew(STextBlock).Text_Lambda([this] {
													 const auto Index = this->Session->SampleMaterialSlot + 1;
													 return FText::FromString(*this->SampleSlots[this->SampleSlots.IsValidIndex(Index) ? Index : 0]);
												 })]] +
										 SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 6)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(LOCTEXT("SampleStride", "Show 1 in"))] + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(85)[SNew(SNumericEntryBox<int32>).MinValue(1).MaxValue(4096).Value_Lambda([this] {
																																																																																										   return TOptional<int32>(this->Session->SampleDisplayStride);
																																																																																									   })
																																																													   .OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type) {
																																																														   auto Error = FString{};
																																																														   this->Session->SetSampleDisplay(this->Session->SampleMaterialSlot, Value, Error);
																																																													   })]]] +
										 SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this] {
											 return FText::Format(LOCTEXT("ShownSamples", "Showing {0} of {1} in the selection"),
												 FText::AsNumber(this->Session->GetDisplayedSampleCount()), FText::AsNumber(this->Session->GetPreparedSampleCount(this->Session->SampleMaterialSlot)));
										 })] +
										 SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("SampleHelp", "Colors identify material slots. Prepared candidates follow the display pose; runtime masks and budgets can reduce emitted particles."))]] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 6)[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(LOCTEXT("Preset", "Preset defaults"))] + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("ReapplyPreset", "Reapply")).IsEnabled_Lambda([this] {
																																																																																														return this->Session->Preset.IsNull() == false;
																																																																																													})
																																																													.ToolTipText(LOCTEXT("ReapplyPresetTip", "Restore the mesh and copy the preset defaults into playback settings."))
																																																													.OnClicked_Lambda([this] {
																																																														auto Error = FString{};
																																																														this->Session->ApplyPreset(this->Session->Preset.LoadSynchronous(), Error);
																																																														return FReply::Handled();
																																																													})] +
										 SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("SavePreset", "Save as...")).OnClicked(this, &SMeshParticleizerPreview::SavePreset).ToolTipText(LOCTEXT("SavePresetTip", "Save the current playback settings as a new preset asset."))]] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(SObjectPropertyEntryBox).AllowedClass(UMeshParticleizerPreset::StaticClass()).ObjectPath_Lambda([this] {
																																												   return this->Session->Preset.ToString();
																																											   })
											 .ToolTipText(LOCTEXT("PresetTip", "Selecting a preset restores the mesh and copies its defaults. Playback edits do not change the asset. Clearing the selection keeps the current settings."))
											 .OnObjectChanged_Lambda([this](const FAssetData& Asset) {
												 if (Asset.IsValid() == false)
												 {
													 this->Session->Preset.Reset();
													 return;
												 }
												 auto Error = FString{};
												 this->Session->ApplyPreset(Cast<UMeshParticleizerPreset>(Asset.GetAsset()), Error);
											 })] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(STextBlock).Text(LOCTEXT("Settings", "Playback settings")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))] + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(SHorizontalBox).Visibility_Lambda([this] {
										 return this->Session->CanEditOrigin() == true ? EVisibility::Visible : EVisibility::Collapsed;
									 }) + SHorizontalBox::Slot().FillWidth(1)[SNew(SCheckBox).IsChecked_Lambda([this] {
																								 return this->Session->bEditingOrigin == true ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
																							 })
												  .ToolTipText(LOCTEXT("OriginHelp", "Restore the mesh and drag a reference-space origin. Playback hides the handle."))
												  .OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
													  if (State == ECheckBoxState::Checked)
													  {
														  auto Error = FString{};
														  this->Session->EditOrigin(this->Session->OriginPointIndex, Error);
													  }
													  else
													  {
														  this->Session->StopEditingOrigin();
													  }
												  })[SNew(STextBlock).Text(LOCTEXT("OriginHandle", "Edit origin"))]] + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(80).Visibility_Lambda([this] {
										 return this->Session->Options.Origin == EMeshParticleizerOrigin::MultiplePoints ? EVisibility::Visible : EVisibility::Collapsed;
									 })[SNew(SNumericEntryBox<int32>).MinValue(1).MaxValue_Lambda([this] {
																					 return this->Session->Options.LocalOrigins.Num();
																				 }).Value_Lambda([this] {
																					   return this->Session->OriginPointIndex + 1;
																				   }).ToolTipText(LOCTEXT("OriginIndex", "Point number to move"))
																															   .OnValueCommitted_Lambda([this](int32 Value, ETextCommit::Type) {
																																   auto Error = FString{};
																																   this->Session->EditOrigin(Value - 1, Error);
																															   })]]] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 10)[SNew(SBox).Visibility_Lambda([this] {
										 return Cast<USkeletalMeshComponent>(this->Session->Mesh) != nullptr ? EVisibility::Visible : EVisibility::Collapsed;
									 })[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("Animation", "Animation"))] + SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)[SNew(SObjectPropertyEntryBox).AllowedClass(UAnimationAsset::StaticClass()).ObjectPath_Lambda([this] {
																																																																							   return this->Session->Animation.ToString();
																																																																						   })
																																								   .OnObjectChanged_Lambda([this](const FAssetData& Asset) {
																																									   auto Error = FString{};
																																									   this->Session->SetAnimation(Cast<UAnimationAsset>(Asset.GetAsset()), Error);
																																								   })]]] +
									 SVerticalBox::Slot().AutoHeight().Padding(10, 0, 0, 0)[SAssignNew(this->ProviderBox, SVerticalBox)] + SVerticalBox::Slot().FillHeight(1).Padding(10, 0, 0, 0)[this->Details->GetWidget().ToSharedRef()]] +
																																						 SWidgetSwitcher::Slot()[this->MakeLoadPanel()]]] +
						SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
							[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).ButtonStyle(FAppStyle::Get(), "PrimaryButton").Text(LOCTEXT("Replay", "Replay")).OnClicked_Lambda([this] {
								auto Error = FString{};
								if (this->Session->SetOptions(*this->GetEditedOptions(), Error))
								{
									this->Session->Replay(Error);
								}
								return FReply::Handled();
							})] +
								SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
									[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text_Lambda([this] {
																									return this->Session->bPaused ? LOCTEXT("Resume", "Resume") : LOCTEXT("Pause", "Pause");
																								})
											.OnClicked_Lambda([this] {
												this->Session->SetPaused(this->Session->bPaused == false);
												return FReply::Handled();
											})] +
								SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
									[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("Restore", "Restore")).OnClicked_Lambda([this] {
										this->Session->Restore();
										return FReply::Handled();
									})] +
								SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)[SNew(STextBlock).Text(LOCTEXT("Speed", "Speed"))] + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 16, 0)[SNew(SBox).WidthOverride(70)[SNew(SNumericEntryBox<float>).MinValue(0.1f).MaxValue(4).AllowSpin(true).Value_Lambda([this] {
																																																																																						return TOptional<float>(this->Session->PlaybackSpeed);
																																																																																					})
																																										 .OnValueChanged_Lambda([this](float Value) {
																																											 auto Error = FString{};
																																											 this->Session->SetPlaybackSpeed(Value, Error);
																																										 })]] +
								SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 16, 0)
									[SAssignNew(this->MovementCombo, SComboBox<TSharedPtr<FString>>).OptionsSource(&this->Movements).InitiallySelectedItem(this->Movements[0]).OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) {
																																												  return SNew(STextBlock).Text(FText::FromString(*Item));
																																											  })
											.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type Selection) {
												if (Selection == ESelectInfo::Direct)
												{
													return;
												}
												this->Session->SetMovement(static_cast<EMeshParticleizerPreviewMovement>(this->Movements.IndexOfByKey(Item)));
											})[SNew(STextBlock).Text_Lambda([this] {
												return FText::FromString(*this->Movements[int32(this->Session->GetMovement())]);
											})]] +
								SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
									[SNew(SCheckBox).IsChecked_Lambda([this] {
														return this->Session->bFloorVisible ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
													})
											.OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
												this->Session->SetFloorVisible(State == ECheckBoxState::Checked);
											})
												[SNew(STextBlock).Text(LOCTEXT("Floor", "Floor"))]]] +
						SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
							[SNew(SHorizontalBox) + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SSlider).MinValue(0).MaxValue(1).Value_Lambda([this] {
																																					   return this->bDragging ? this->SliderValue : this->Session->Elapsed / FMath::Max(this->Session->GetDuration(), 0.001f);
																																				   })
															.OnMouseCaptureBegin_Lambda([this] {
																this->bDragging = true;
															})
															.OnValueChanged_Lambda([this](float Value) {
																this->SliderValue = Value;
																if (this->bDragging == false)
																{
																	this->SeekSlider();
																}
															})
															.OnControllerCaptureBegin_Lambda([this] {
																this->bDragging = true;
															})
															.OnControllerCaptureEnd_Lambda([this] {
																this->SeekSlider();
															})
															.OnMouseCaptureEnd_Lambda([this] {
																this->SeekSlider();
															})] +
								SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
									[SNew(STextBlock).Text_Lambda([this] {
										return FText::FromString(FString::Printf(TEXT("%.2f / %.2f s"), this->Session->Elapsed, this->Session->GetDuration()));
									})]] +
						SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
								return FText::FromString(this->Session->GetStatus());
							})]]];
	}

	virtual ~SMeshParticleizerPreview() override
	{
		this->Details->GetOnFinishedChangingPropertiesDelegate().RemoveAll(this);
		this->Session->OnOptionsChanged.RemoveAll(this);
		this->Session->Close();
		this->ChildSlot.DetachWidget();
		this->Session->ReleaseScene();
	}

	virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override
	{
		SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);

		// 公開APIからの切替も選択行へ反映し、表示中のラベルとメニュー選択を揃える。
		const auto Display = this->Displays[int32(this->Session->DisplayMode)];
		if (this->DisplayCombo->GetSelectedItem() != Display)
		{
			this->DisplayCombo->SetSelectedItem(Display);
		}

		const auto Movement = this->Movements[int32(this->Session->GetMovement())];
		if (this->MovementCombo->GetSelectedItem() != Movement)
		{
			this->MovementCombo->SetSelectedItem(Movement);
		}

		const auto SlotIndex = this->Session->SampleMaterialSlot + 1;
		if (this->SampleSlots.IsValidIndex(SlotIndex) && this->SampleCombo->GetSelectedItem() != this->SampleSlots[SlotIndex])
		{
			this->SampleCombo->SetSelectedItem(this->SampleSlots[SlotIndex]);
		}

		// 一つの表示更新で同じ集計を使う。同期GPU読戻しや再生の再初期化は行わない。
		if (this->Session->DisplayMode == EMeshParticleizerPreviewDisplay::Load)
		{
			this->BudgetUsage = this->Session->GetBudgetUsage();
			this->CacheUsage = this->Session->GetAssetCacheUsage();
		}

		if (this->LastPrepared == this->Session->Prepared)
		{
			return;
		}
		this->LastPrepared = this->Session->Prepared;
		this->SampleSlots.SetNum(1);
		if (this->Session->Prepared != nullptr)
		{
			for (auto Slot = 0; Slot < this->Session->Prepared->SourceMaterials.Num(); ++Slot)
			{
				this->SampleSlots.Add(MakeShared<FString>(FString::Printf(TEXT("%d · %s"), Slot,
					*this->Session->Prepared->SourceMaterials[Slot].GetAssetName())));
			}
		}
		this->SampleCombo->RefreshOptions();
		this->ProviderBox->ClearChildren();
		if (this->Session->Prepared == nullptr)
		{
			return;
		}
		for (auto Slot = 0; Slot < this->Session->Prepared->TextureColorSources.Num(); ++Slot)
		{
			if (this->Session->Prepared->TextureColorSources[Slot].bRequiresColorProvider == false)
			{
				continue;
			}
			this->ProviderBox->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
				[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::Format(LOCTEXT("ColorSlot", "Color input · slot {0} (UV0)"), FText::AsNumber(Slot)))] + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)[SNew(SObjectPropertyEntryBox).AllowedClass(UMaterialInterface::StaticClass()).ToolTipText(LOCTEXT("ColorInputHelp", "Choose an Unlit material whose Emissive output contains the UV0 surface color. This explicit input replaces the unsupported color path.")).ObjectPath_Lambda([this, Slot] {
																																																																																																																													  const auto* Material = this->Session->ColorProviders.Find(Slot);
																																																																																																																													  return Material != nullptr ? Material->ToString() : FString{};
																																																																																																																												  })
																																																	 .OnObjectChanged_Lambda([this, Slot](const FAssetData& Asset) {
																																																		 auto Error = FString{};
																																																		 this->Session->SetColorProvider(Slot, Cast<UMaterialInterface>(Asset.GetAsset()), Error);
																																																	 })]];
		}
	}

private:
	FReply SavePreset()
	{
		auto Dialog = FSaveAssetDialogConfig{};
		Dialog.DialogTitleOverride = LOCTEXT("SavePresetTitle", "Save Particleizer Preset As");
		Dialog.DefaultPath = TEXT("/Game/MeshParticleizer/Presets");
		Dialog.DefaultAssetName = TEXT("DA_ParticleizerPreset");
		Dialog.AssetClassNames.Add(UMeshParticleizerPreset::StaticClass()->GetClassPathName());
		Dialog.WindowOverride = FSlateApplication::Get().FindWidgetWindow(AsShared());
		const auto ObjectPath = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().CreateModalSaveAssetDialog(Dialog);
		if (ObjectPath.IsEmpty() == false)
		{
			auto Error = FString{};
			this->Session->SavePreset(FPackageName::ObjectPathToPackageName(ObjectPath), Error);
		}
		return FReply::Handled();
	}

	TSharedRef<SWidget> MakeLoadPanel()
	{
		return SNew(SScrollBox) + SScrollBox::Slot().Padding(14, 0, 0, 12)[SNew(STextBlock).Text(LOCTEXT("LoadTitle", "Preview load")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))] + SScrollBox::Slot().Padding(14, 0, 0, 16)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
				const auto& Usage = this->BudgetUsage;
				return FText::FromString(FString::Printf(TEXT("%s\nReserved  %s / %s particles\nSources  %d / %d  (releasing %d)\nPrepared samples  %s"),
					*StaticEnum<EMeshParticleizerQuality>()->GetNameStringByValue(int64(Usage.Quality)),
					*FText::AsNumber(Usage.ReservedParticles).ToString(), *FText::AsNumber(Usage.ParticleLimit).ToString(),
					Usage.ReservedSources, Usage.SourceLimit, Usage.ReleasingSources,
					*FText::AsNumber(this->Session->Prepared != nullptr ? this->Session->Prepared->SampleCount : 0).ToString()));
		})] +
			SScrollBox::Slot().Padding(14, 0, 0, 4)
				[SNew(STextBlock).Text(LOCTEXT("GpuParticles", "GPU particles")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))] +
			SScrollBox::Slot().Padding(14, 0, 0, 16)
				[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
					const auto& Usage = this->BudgetUsage;
					if (Usage.bGpuCountsAvailable == false)
					{
						return LOCTEXT("WaitingGpuCounts", "Waiting for GPU counts");
					}
				return FText::FromString(FString::Printf(TEXT("Live  %s\nNot yet detached  %s\nResting  %s  /  Budget fade  %s\nDelayed readback · render frame %lld"),
					*FText::AsNumber(Usage.GpuLiveParticles).ToString(), *FText::AsNumber(Usage.GpuUnseparatedParticles).ToString(),
					*FText::AsNumber(Usage.GpuRestingParticles).ToString(), *FText::AsNumber(Usage.GpuBudgetFadingParticles).ToString(),
					Usage.GpuCountsRenderFrame));
				})] +
			SScrollBox::Slot().Padding(14, 0, 0, 4)
				[SNew(STextBlock).Text(LOCTEXT("SimulationBudget", "Simulation budget")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))] +
			SScrollBox::Slot().Padding(14, 0, 0, 16)
				[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
				const auto& Usage = this->BudgetUsage;
				return FText::FromString(FString::Printf(TEXT("Reserved  %.2f / %.0f MiB\nIncludes shared  %.2f MiB\n  GDF share  %.2f MiB\nIncludes pooled  %.2f MiB (%d)\nIncludes releasing  %.2f MiB"),
					Usage.ReservedSimulationBytes / 1048576.0, Usage.SimulationMemoryLimitBytes / 1048576.0,
					Usage.SharedGpuReservationBytes / 1048576.0, Usage.GdfReservationBytes / 1048576.0,
					Usage.PooledSimulationBytes / 1048576.0, Usage.PooledSimulations, Usage.ReleasingSimulationBytes / 1048576.0));
				})] +
			SScrollBox::Slot().Padding(14, 0, 0, 4)
				[SNew(STextBlock).Text(LOCTEXT("AssetBudget", "Assets & textures")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))] +
			SScrollBox::Slot().Padding(14, 0, 0, 16)
				[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
				const auto& Cache = this->CacheUsage;
				return FText::FromString(FString::Printf(TEXT("Reserved  %.2f / %.0f MiB\nIn use  %.2f MiB\nLoading  %.2f MiB\nReleasing  %.2f MiB\nIncludes surface data, textures and providers.\nReservations are not measured VRAM usage."),
					Cache.ReservedBytes / 1048576.0, Cache.LimitBytes / 1048576.0, Cache.PinnedBytes / 1048576.0,
					Cache.LoadingBytes / 1048576.0, Cache.RetiringBytes / 1048576.0));
				})] +
			SScrollBox::Slot().Padding(14, 0, 0, 4)
				[SNew(STextBlock).Text(LOCTEXT("Rendering", "Rendering")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))] +
			SScrollBox::Slot().Padding(14, 0, 0, 16)
				[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("RenderingPath", "GPU particles + source mesh"))] +
			SScrollBox::Slot().Padding(14, 0, 0, 4)
				[SNew(STextBlock).Text(LOCTEXT("PlaybackDiagnostics", "Playback diagnostics")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))] +
			SScrollBox::Slot().Padding(14, 0, 0, 12)
				[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
				return FText::FromString(this->Session->Operation != nullptr ? this->Session->Operation->GetDiagnostics() : this->Session->GetStatus());
			})];
	}

	FMeshParticleizeOptions* GetEditedOptions() const
	{
		return reinterpret_cast<FMeshParticleizeOptions*>(this->EditedOptions->GetStructMemory());
	}
	void OptionsChanged(const FPropertyChangedEvent&)
	{
		auto Error = FString{};
		this->Session->SetOptions(*this->GetEditedOptions(), Error);
	}
	void RefreshOptions()
	{
		// ハンドルや公開APIで変更した値を、次のReplayが古いDetails値で上書きしないようにする。
		if (FMeshParticleizeOptions::StaticStruct()->CompareScriptStruct(this->GetEditedOptions(), &this->Session->Options, 0) == false)
		{
			*this->GetEditedOptions() = this->Session->Options;
			this->Details->GetDetailsView()->ForceRefresh();
		}
	}
	void SeekSlider()
	{
		this->bDragging = false;
		auto Error = FString{};
		this->Session->Seek(this->SliderValue * this->Session->GetDuration(), Error);
	}

	TStrongObjectPtr<UMeshParticleizerPreview> Session;
	TSharedPtr<FStructOnScope> EditedOptions;
	TSharedPtr<IStructureDetailsView> Details;
	TSharedPtr<SVerticalBox> ProviderBox;
	TWeakObjectPtr<UMeshParticleizerPreparedMesh> LastPrepared;
	TArray<TSharedPtr<FString>> Movements;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> MovementCombo;
	TArray<TSharedPtr<FString>> SampleSlots;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> SampleCombo;
	TArray<TSharedPtr<FString>> Displays;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> DisplayCombo;
	FMeshParticleizerBudgetUsage BudgetUsage;
	FMeshParticleizerAssetCacheUsage CacheUsage;
	bool bDragging = false;
	float SliderValue = 0;
};

UMeshParticleizerPreview* UMeshParticleizerPreparationLibrary::OpenPreviewWindow(UMeshParticleizerPreparedMesh* Prepared)
{
	auto Session = TStrongObjectPtr<UMeshParticleizerPreview>(NewObject<UMeshParticleizerPreview>());
	Session->GetScene();
	if (Prepared != nullptr)
	{
		auto Error = FString{};
		Session->SetPreparedData(Prepared, Error);
	}
	auto Window = SNew(SWindow).Title(LOCTEXT("Title", "Mesh Particleizer — Preview")).ClientSize(FVector2D(1280, 800))[SNew(SMeshParticleizerPreview).Session(Session.Get())];
	Session->SetWindow(Window);
	FSlateApplication::Get().AddWindow(Window);
	return Session.Get();
}

#undef LOCTEXT_NAMESPACE
