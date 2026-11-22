#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/DataValidation.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

FString UMeshParticleizerPreparationLibrary::InspectPreparedData(UMeshParticleizerPreparedMesh* Prepared)
{
	if (Prepared == nullptr)
	{
		return TEXT("Select prepared mesh data.");
	}
	auto Context = FDataValidationContext{};
	const auto Result = Prepared->IsDataValid(Context);
	auto Warnings = TArray<FText>{};
	auto Errors = TArray<FText>{};
	Context.SplitIssues(Warnings, Errors);
	auto Lines = TArray<FString>{Prepared->GetPathName()};
	Lines.Add(Result == EDataValidationResult::Valid ? TEXT("Current dependencies. Runtime asset references are available.")
													 : TEXT("Needs attention before playback or Cook:"));
	for (const auto& Error : Errors)
	{
		Lines.Add(TEXT("- ") + Error.ToString());
	}
	return FString::Join(Lines, TEXT("\n"));
}

void UMeshParticleizerPreparationLibrary::OpenPreparedValidationWindow(const TArray<UMeshParticleizerPreparedMesh*>& Prepared)
{
	auto Reports = TArray<FString>{};
	for (auto* Asset : Prepared)
	{
		Reports.Add(InspectPreparedData(Asset));
	}
	auto Window = SNew(SWindow).Title(NSLOCTEXT("MeshParticleizer", "PreparationCheck", "Mesh Particleizer — Preparation Check")).ClientSize(FVector2D(850, 580))[SNew(SBorder).Padding(24)[SNew(SScrollBox) + SScrollBox::Slot()[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(FString::Join(Reports, TEXT("\n\n"))))]]];
	FSlateApplication::Get().AddWindow(Window);
}
