#include "MeshParticleizerPreparationLibrary.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "UnrealClient.h"

bool UMeshParticleizerPreparationLibrary::StartValidationPlay(FString& OutError)
{
	OutError.Reset();
	if (GEditor == nullptr || GEditor->IsPlaySessionInProgress() == true)
	{
		OutError = TEXT("Start validation from an editor with no active or queued play session.");
		return false;
	}

	auto Request = FRequestPlaySessionParams{};
	Request.WorldType = EPlaySessionWorldType::PlayInEditor;
	Request.bAllowOnlineSubsystem = false;
	GEditor->RequestPlaySession(Request);
	return true;
}

bool UMeshParticleizerPreparationLibrary::RequestValidationScreenshot(UObject* WorldContextObject,
	const FString& OutputPng, FString& OutError)
{
	OutError.Reset();
	auto* World = WorldContextObject != nullptr ? WorldContextObject->GetWorld() : nullptr;
	auto* Viewport = World != nullptr ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr || Viewport->Viewport == nullptr || FScreenshotRequest::IsScreenshotRequested() == true ||
		FPaths::GetExtension(OutputPng).Equals(TEXT("png"), ESearchCase::IgnoreCase) == false ||
		FPaths::IsRelative(OutputPng) == true || IFileManager::Get().FileExists(*OutputPng) == true)
	{
		OutError = TEXT("Choose an unused absolute PNG path with a running game viewport and no pending screenshot.");
		return false;
	}

	if (IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPng), true) == false)
	{
		OutError = TEXT("Could not create the screenshot directory.");
		return false;
	}
	FScreenshotRequest::RequestScreenshot(OutputPng, false, false);
	return true;
}
