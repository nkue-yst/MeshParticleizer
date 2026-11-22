#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MeshParticleizerPreparation"

class SMeshParticleizerPreparation : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMeshParticleizerPreparation)
	{
	}
		SLATE_ARGUMENT(UObject*, Mesh)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		this->SetMesh(FAssetData(InArgs._Mesh));
		this->ChildSlot
			[SNew(SBorder).Padding(24)
					[SNew(SVerticalBox) + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)[SNew(STextBlock).Text(LOCTEXT("Intro", "Prepare mesh")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 18))] + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)[SNew(STextBlock).Text(LOCTEXT("Mesh", "Static or Skeletal Mesh"))] + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)[SNew(SObjectPropertyEntryBox).AllowedClass(UObject::StaticClass()).ObjectPath(this, &SMeshParticleizerPreparation::GetMeshPath).OnShouldFilterAsset(FOnShouldFilterAsset::CreateStatic(&SMeshParticleizerPreparation::FilterAsset)).OnObjectChanged(this, &SMeshParticleizerPreparation::SetMesh)] + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)[SNew(STextBlock).Text(LOCTEXT("Destination", "Output folder"))] + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)[SNew(SEditableTextBox).Text(this, &SMeshParticleizerPreparation::GetDestination).OnTextChanged(this, &SMeshParticleizerPreparation::SetDestination)] + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)[SNew(STextBlock).Text(LOCTEXT("SamplingLOD", "Sampling LOD"))] + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 0, 0, 18)[SNew(SBox).WidthOverride(180)[SNew(SNumericEntryBox<int32>).MinValue(0).MaxValue_Lambda([this] {
																																																																																																																																																																																																																																																																																																																														return TOptional<int32>(this->MaxSamplingLOD);
																																																																																																																																																																																																																																																																																																																													})
																																																																																																																																																																																																																																																																																										  .AllowSpin(true)
																																																																																																																																																																																																																																																																																										  .Value_Lambda([this] {
																																																																																																																																																																																																																																																																																											  return TOptional<int32>(this->SamplingLOD);
																																																																																																																																																																																																																																																																																										  })
																																																																																																																																																																																																																																																																																										  .OnValueChanged(this, &SMeshParticleizerPreparation::SetSamplingLOD)
																																																																																																																																																																																																																																																																																										  .ToolTipText(LOCTEXT("SamplingLODHelp", "Surface samples and hit projection use this fixed LOD. Skeletal playback keeps the complete LOD 0 bone pose."))]] +
						SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
							[SNew(STextBlock).Text(LOCTEXT("Samples", "Surface samples"))] +
						SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 0, 0, 18)[SNew(SBox).WidthOverride(180)[SNew(SNumericEntryBox<int32>).MinValue(1).MaxValue(250000).AllowSpin(true).Value(this, &SMeshParticleizerPreparation::GetSampleCount).OnValueChanged(this, &SMeshParticleizerPreparation::SetSampleCount)]] + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)[SNew(SCheckBox).IsChecked_Lambda([this] {
																																																																																																						 return this->bUseUnlitEmissive ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
																																																																																																					 })
																																																																																					   .OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
																																																																																						   this->bUseUnlitEmissive = State == ECheckBoxState::Checked;
																																																																																						   this->Status = FText::GetEmpty();
																																																																																					   })
																																																																																					   .ToolTipText(LOCTEXT("UnlitHelp", "For Unlit slots, use Emissive RGB clamped to 0..1 as surface color. Original HDR emission is a separate feature."))[SNew(STextBlock).Text(LOCTEXT("UnlitColor", "Use Unlit Emissive as surface color"))]] +
						SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)
							[SNew(SCheckBox).IsChecked_Lambda([this] {
												return this->bBakeSourceEmission ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
											})
									.OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
										this->bBakeSourceEmission = State == ECheckBoxState::Checked;
										this->Status = FText::GetEmpty();
									})
									.ToolTipText(LOCTEXT("EmissionHelp", "Bake fixed original Emissive values into a separate HDR texture. Enable Inherit source emission in playback options to use it."))
										[SNew(STextBlock).Text(LOCTEXT("SourceEmission", "Bake original emission (HDR)"))]] +
						SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 18)
							[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("Scope", "Creates surface data and dissolve materials for conventional Opaque and Masked materials in the output folder."))] +
						SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0, 0, 0, 18)[SNew(SHorizontalBox) + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).ButtonStyle(FAppStyle::Get(), "PrimaryButton").Text(LOCTEXT("Prepare", "Prepare mesh")).IsEnabled(this, &SMeshParticleizerPreparation::CanPrepare).OnClicked(this, &SMeshParticleizerPreparation::Prepare)] + SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("Inspect", "Check materials")).IsEnabled(this, &SMeshParticleizerPreparation::CanPrepare).OnClicked(this, &SMeshParticleizerPreparation::Inspect)] + SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)[SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").Text(LOCTEXT("Preview", "Preview")).Visibility_Lambda([this] {
																																																																																																																																																																																																										  return this->Prepared.IsValid() ? EVisibility::Visible : EVisibility::Collapsed;
																																																																																																																																																																																																									  })
																																																																																																																																																																					   .OnClicked_Lambda([this] {
																																																																																																																																																																						   UMeshParticleizerPreparationLibrary::OpenPreviewWindow(this->Prepared.Get());
																																																																																																																																																																						   return FReply::Handled();
																																																																																																																																																																					   })]] +
						SVerticalBox::Slot().FillHeight(1)
							[SNew(SScrollBox) + SScrollBox::Slot()[SNew(STextBlock).AutoWrapText(true).Text(this, &SMeshParticleizerPreparation::GetStatus)]]]];
	}

private:
	static bool FilterAsset(const FAssetData& Asset)
	{
		return Asset.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName() &&
			Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName();
	}

	void SetMesh(const FAssetData& Asset)
	{
		this->MeshPath = Asset.ToSoftObjectPath();
		this->Prepared.Reset();
		this->SamplingLOD = 0;
		this->MaxSamplingLOD = 0;
		this->Status = FText::GetEmpty();
		if (Asset.IsValid() == false)
		{
			return;
		}

		auto* Mesh = Asset.GetAsset();
		if (const auto* Static = Cast<UStaticMesh>(Mesh))
		{
			this->MaxSamplingLOD = FMath::Max(0, Static->GetNumLODs() - 1);
		}
		else if (const auto* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			this->MaxSamplingLOD = FMath::Max(0, Skeletal->GetLODNum() - 1);
		}

		const auto BaseFolder = FString(TEXT("/Game/MeshParticleizer/Prepared/")) + Asset.AssetName.ToString();
		this->Destination = BaseFolder;
		for (auto Suffix = 2; FPackageName::DoesPackageExist(this->Destination / TEXT("DA_Prepared")) ||
			FindPackage(nullptr, *(this->Destination / TEXT("DA_Prepared"))) != nullptr; ++Suffix)
		{
			this->Destination = BaseFolder + FString::Printf(TEXT("_%d"), Suffix);
		}
	}

	FString GetMeshPath() const
	{
		return this->MeshPath.ToString();
	}
	FText GetDestination() const
	{
		return FText::FromString(this->Destination);
	}
	FText GetStatus() const
	{
		return this->Status;
	}
	TOptional<int32> GetSampleCount() const
	{
		return this->SampleCount;
	}
	bool CanPrepare() const
	{
		return this->MeshPath.IsNull() == false && this->bPreparing == false;
	}
	void SetDestination(const FText& Value)
	{
		this->Destination = Value.ToString();
		this->Status = FText::GetEmpty();
	}
	void SetSampleCount(int32 Value)
	{
		this->SampleCount = Value;
		this->Status = FText::GetEmpty();
	}

	void SetSamplingLOD(int32 Value)
	{
		if (Value < 0 || Value > this->MaxSamplingLOD)
		{
			return;
		}
		this->SamplingLOD = Value;
		this->Prepared.Reset();
		this->Status = FText::GetEmpty();
	}

	FReply Inspect()
	{
		if (this->CanPrepare() == false)
		{
			return FReply::Handled();
		}
		this->bPreparing = true;
		this->Status = FText::FromString(UMeshParticleizerPreparationLibrary::InspectMaterialSupport(
			this->MeshPath.TryLoad(), this->bUseUnlitEmissive, this->bBakeSourceEmission));
		this->bPreparing = false;
		return FReply::Handled();
	}

	FReply Prepare()
	{
		if (this->CanPrepare() == false)
		{
			return FReply::Handled();
		}
		this->bPreparing = true;
		auto Task = FScopedSlowTask(1, LOCTEXT("Working", "Preparing surface colors and dissolve materials..."));
		Task.MakeDialog(false);
		auto Error = FString{};
		auto* Mesh = this->MeshPath.TryLoad();
		auto* Data = Cast<USkeletalMesh>(Mesh) != nullptr ? UMeshParticleizerPreparationLibrary::PrepareSkeletalMesh(Cast<USkeletalMesh>(Mesh), this->Destination, this->SampleCount, 1337, Error, this->bUseUnlitEmissive, this->bBakeSourceEmission, this->SamplingLOD) : UMeshParticleizerPreparationLibrary::PrepareStaticMesh(Cast<UStaticMesh>(Mesh), this->Destination, this->SampleCount, 1337, Error, this->bUseUnlitEmissive, this->bBakeSourceEmission, this->SamplingLOD);
		this->bPreparing = false;
		if (Data == nullptr)
		{
			this->Status = FText::FromString(Error);
			return FReply::Handled();
		}

		this->Prepared = Data;
		this->Status = FText::Format(LOCTEXT("Complete", "Prepared {0} surface samples from LOD {1} ({2} sections). Preview the effect, then assign DA_Prepared to a Mesh Particleizer Component."), FText::AsNumber(Data->SampleCount), FText::AsNumber(Data->SamplingLOD), FText::AsNumber(Data->EnabledSections.Num()));
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<FAssetData>{FAssetData(Data)});
		Task.EnterProgressFrame(1);
		return FReply::Handled();
	}

	FSoftObjectPath MeshPath;
	TWeakObjectPtr<UMeshParticleizerPreparedMesh> Prepared;
	FString Destination = TEXT("/Game/MeshParticleizer/Prepared");
	int32 SampleCount = 100000;
	int32 SamplingLOD = 0;
	int32 MaxSamplingLOD = 0;
	FText Status;
	bool bPreparing = false;
	bool bUseUnlitEmissive = false;
	bool bBakeSourceEmission = false;
};

void UMeshParticleizerPreparationLibrary::OpenPreparationWindow(UObject* Mesh)
{
	auto Window = SNew(SWindow).Title(LOCTEXT("WindowTitle", "Mesh Particleizer — Prepare Mesh")).ClientSize(FVector2D(640, 760)).SupportsMaximize(false).SupportsMinimize(false)[SNew(SMeshParticleizerPreparation).Mesh(Mesh)];
	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE
