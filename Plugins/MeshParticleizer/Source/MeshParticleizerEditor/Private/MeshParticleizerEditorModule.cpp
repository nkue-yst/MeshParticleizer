#include "Modules/ModuleManager.h"
#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "ContentBrowserMenuContexts.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "MeshParticleizerEditor"

class FMeshParticleizerEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMeshParticleizerEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		// 診断のRender Threadコールバックも、Editorモジュールが消える前に回収する。
		UMeshParticleizerPreparationLibrary::EndSpawnDiagnostics();

		// メニューと起動コールバックをモジュール破棄より先に解除する。
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

private:
	static void PrepareSelection(const FToolMenuContext& Context)
	{
		if (const auto* Selection = UContentBrowserAssetContextMenuContext::FindContextWithAssets(Context))
		{
			for (const auto& Asset : Selection->SelectedAssets)
			{
				auto* Mesh = Asset.GetAsset();
				if (auto* Prepared = Cast<UMeshParticleizerPreparedMesh>(Mesh))
				{
					UMeshParticleizerPreparationLibrary::OpenPreviewWindow(Prepared);
					return;
				}
				if (Cast<UStaticMesh>(Mesh) != nullptr || Cast<USkeletalMesh>(Mesh) != nullptr)
				{
					UMeshParticleizerPreparationLibrary::OpenPreparationWindow(Mesh);
					return;
				}
			}
		}
	}

	static void OpenPreparation()
	{
		UMeshParticleizerPreparationLibrary::OpenPreparationWindow(nullptr);
	}

	static void CheckPreparedSelection(const FToolMenuContext& Context)
	{
		auto Prepared = TArray<UMeshParticleizerPreparedMesh*>{};
		if (const auto* Selection = UContentBrowserAssetContextMenuContext::FindContextWithAssets(Context))
		{
			for (const auto& Asset : Selection->SelectedAssets)
			{
				if (auto* Data = Cast<UMeshParticleizerPreparedMesh>(Asset.GetAsset()))
				{
					Prepared.Add(Data);
				}
			}
		}
		UMeshParticleizerPreparationLibrary::OpenPreparedValidationWindow(Prepared);
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		for (const auto* MenuName : {TEXT("ContentBrowser.AssetContextMenu.StaticMesh"), TEXT("ContentBrowser.AssetContextMenu.SkeletalMesh")})
		{
			auto* ContextMenu = UToolMenus::Get()->ExtendMenu(MenuName);
			auto& Section = ContextMenu->FindOrAddSection(TEXT("MeshParticleizer"), LOCTEXT("Section", "Mesh Particleizer"));
			Section.AddMenuEntry(TEXT("MeshParticleizer.Prepare"), LOCTEXT("Prepare", "Prepare for Particleize..."),
				LOCTEXT("PrepareTooltip", "Generate surface colors and dissolve materials for the selected mesh."), FSlateIcon(),
				FToolMenuExecuteAction::CreateStatic(&FMeshParticleizerEditorModule::PrepareSelection));
		}

		auto* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		auto& PreviewSection = UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.MeshParticleizerPreparedMesh"))
																 ->FindOrAddSection(TEXT("MeshParticleizer"), LOCTEXT("PreviewSection", "Mesh Particleizer"));
		PreviewSection.AddMenuEntry(TEXT("MeshParticleizer.Preview"), LOCTEXT("Preview", "Preview Particleize..."),
			LOCTEXT("PreviewTip", "Try prepared data and playback settings in a separate preview scene."), FSlateIcon(),
			FToolMenuExecuteAction::CreateStatic(&FMeshParticleizerEditorModule::PrepareSelection));
		PreviewSection.AddMenuEntry(TEXT("MeshParticleizer.CheckPreparation"), LOCTEXT("CheckPreparation", "Check preparation..."),
			LOCTEXT("CheckPreparationTip", "List changed dependencies and unavailable runtime assets for the selected prepared meshes."), FSlateIcon(),
			FToolMenuExecuteAction::CreateStatic(&FMeshParticleizerEditorModule::CheckPreparedSelection));

		ToolsMenu->FindOrAddSection(TEXT("MeshParticleizer"), LOCTEXT("PreviewToolsSection", "Mesh Particleizer"))
										.AddMenuEntry(TEXT("MeshParticleizer.OpenPreview"), LOCTEXT("OpenPreview", "Preview Mesh Particleizer..."),
											LOCTEXT("OpenPreviewTip", "Choose prepared data to preview particleization."), FSlateIcon(),
											FUIAction(FExecuteAction::CreateLambda([] {
												UMeshParticleizerPreparationLibrary::OpenPreviewWindow();
											})));
		ToolsMenu->FindOrAddSection(TEXT("MeshParticleizer"), LOCTEXT("ToolsSection", "Mesh Particleizer"))
										.AddMenuEntry(TEXT("MeshParticleizer.OpenPreparation"), LOCTEXT("OpenPreparation", "Prepare Mesh Particleizer Data..."),
											LOCTEXT("OpenTooltip", "Select a Static or Skeletal Mesh and create its particleization data."), FSlateIcon(),
											FUIAction(FExecuteAction::CreateStatic(&FMeshParticleizerEditorModule::OpenPreparation)));
	}
};

IMPLEMENT_MODULE(FMeshParticleizerEditorModule, MeshParticleizerEditor)
#undef LOCTEXT_NAMESPACE
