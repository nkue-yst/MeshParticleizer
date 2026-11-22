#include "MeshParticleizerDistanceField.h"
#include "DynamicRHI.h"
#include "GlobalDistanceFieldParameters.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraGpuComputeDataManager.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"

namespace MeshParticleizerDistanceField
{
	// Dispatchと同じ寿命。World側の参照がなくなれば公開を止める。
	class FObserver : public FNiagaraGpuComputeDataManager
	{
	public:
		explicit FObserver(FNiagaraGpuComputeDispatchInterface* Owner)
			: FNiagaraGpuComputeDataManager(Owner)
		{
			Owner->GetOnPostRenderEvent().AddRaw(this, &FObserver::PostRender);
		}

		// DispatchはイベントをDataManagerより先に破棄する。ここで破棄済みDelegateへ触れない。
		static FName GetManagerName()
		{
			return TEXT("MeshParticleizerDistanceField");
		}
		TWeakPtr<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> State;

	private:
		void PostRender(FRDGBuilder& GraphBuilder)
		{
			const auto Shared = this->State.Pin();
			if (Shared.IsValid() == false)
			{
				return;
			}
			const auto* Data = this->GetOwnerInterface()->GetGlobalDistanceFieldData();
			Shared->TextureBytes.store(FMeshParticleizerDistanceField::Measure(Data));
			Shared->Clipmaps.store(Data != nullptr ? Data->NumGlobalSDFClipmaps : 0);
			Shared->VoxelSizeCm.store(Data != nullptr && Data->GlobalDFResolution > 0
					? 2 * Data->TranslatedCenterAndExtent[0].W / Data->GlobalDFResolution
					: 0);
			Shared->Frame.store(GFrameNumberRenderThread);
		}
	};
}

TSharedRef<FMeshParticleizerDistanceField, ESPMode::ThreadSafe> FMeshParticleizerDistanceField::Observe(FSceneInterface* Scene)
{
	auto State = MakeShared<FMeshParticleizerDistanceField, ESPMode::ThreadSafe>();
	ENQUEUE_RENDER_COMMAND(MeshParticleizerObserveGDF)([Scene, State](FRHICommandListImmediate& RHICmdList) {
			if (auto* Dispatch = FNiagaraGpuComputeDispatchInterface::Get(Scene))
		{
				Dispatch->GetOrCreateDataManager<MeshParticleizerDistanceField::FObserver>().State = State;
		}
		});
	return State;
}

int64 FMeshParticleizerDistanceField::Measure(const FGlobalDistanceFieldParameterData* Data)
{
	if (Data == nullptr || Data->NumGlobalSDFClipmaps <= 0 || Data->MaxDFAOConeDistance <= 0 ||
		Data->PageAtlasTexture == nullptr || Data->PageTableTexture == nullptr)
	{
		return 0;
	}

	// Niagaraの問い合わせに実際にバインドする4資源を重複なく数える。Lightingの構築用資源は含めない。
	FRHITexture* Textures[] = {Data->PageAtlasTexture, Data->CoverageAtlasTexture, Data->PageTableTexture, Data->MipTexture};
	auto Bytes = int64{0};
	for (auto Index = 0; Index < UE_ARRAY_COUNT(Textures); ++Index)
	{
		auto* Texture = Textures[Index];
		if (Texture == nullptr)
		{
			continue;
		}
		auto bDuplicate = false;
		for (auto Earlier = 0; Earlier < Index; ++Earlier)
		{
			bDuplicate |= Textures[Earlier] == Texture;
		}
		if (bDuplicate == false)
		{
			Bytes += RHICalcTexturePlatformSize(Texture->GetDesc(), 0).Size;
		}
	}
	return Bytes;
}

bool FMeshParticleizerDistanceField::IsEnabled()
{
	return GetSettingsStatus() == EMeshParticleizerGdfStatus::Ready;
}

EMeshParticleizerGdfStatus FMeshParticleizerDistanceField::GetSettingsStatus()
{
	// Engine所有のCVar参照は固定し、毎フレームは変更可能な値だけ読む。
	static const IConsoleVariable* Variables[] = {
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.GenerateMeshDistanceFields")),
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.AOGlobalDistanceField")),
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.AOUpdateGlobalDistanceField")),
					IConsoleManager::Get().FindConsoleVariable(TEXT("r.AOGlobalDistanceFieldClipmapUpdatesPerFrame"))};
	constexpr EMeshParticleizerGdfStatus Reasons[] = {
		EMeshParticleizerGdfStatus::GenerateMeshDistanceFieldsDisabled,
		EMeshParticleizerGdfStatus::GlobalDistanceFieldDisabled,
		EMeshParticleizerGdfStatus::UpdatesDisabled,
		EMeshParticleizerGdfStatus::ClipmapUpdatesDisabled};
	for (auto Index = 0; Index < UE_ARRAY_COUNT(Variables); ++Index)
	{
		if (Variables[Index] == nullptr || Variables[Index]->GetInt() <= 0)
		{
			return Reasons[Index];
		}
	}
	return EMeshParticleizerGdfStatus::Ready;
}

EMeshParticleizerGdfStatus FMeshParticleizerDistanceField::GetStatus(const FMeshParticleizerDistanceField* Field)
{
	const auto Settings = GetSettingsStatus();
	if (Settings != EMeshParticleizerGdfStatus::Ready)
	{
		return Settings;
	}
	if (Field == nullptr)
	{
		return EMeshParticleizerGdfStatus::NotObserved;
	}
	if (Field->Frame.load() == 0)
	{
		return EMeshParticleizerGdfStatus::WaitingForRender;
	}
	return Field->TextureBytes.load() > 0 ? EMeshParticleizerGdfStatus::Ready : EMeshParticleizerGdfStatus::Unavailable;
}

FString FMeshParticleizerDistanceField::DescribeStatus(EMeshParticleizerGdfStatus Status)
{
	switch (Status)
	{
	case EMeshParticleizerGdfStatus::GenerateMeshDistanceFieldsDisabled:
		return TEXT("Enable Generate Mesh Distance Fields (r.GenerateMeshDistanceFields) in Project Settings, restart the editor and rebuild mesh distance fields.");
	case EMeshParticleizerGdfStatus::GlobalDistanceFieldDisabled:
		return TEXT("Enable r.AOGlobalDistanceField in the rendering configuration.");
	case EMeshParticleizerGdfStatus::UpdatesDisabled:
		return TEXT("Enable r.AOUpdateGlobalDistanceField; collision requires an updating field.");
	case EMeshParticleizerGdfStatus::ClipmapUpdatesDisabled:
		return TEXT("Set r.AOGlobalDistanceFieldClipmapUpdatesPerFrame above zero.");
	case EMeshParticleizerGdfStatus::NotObserved:
		return TEXT("GDF has not been observed. Prepare For Particleize with Global Distance Field or Auto collision in a rendering game world.");
	case EMeshParticleizerGdfStatus::WaitingForRender:
		return TEXT("Waiting for the first GDF rendering observation. Keep a rendering view active during preparation.");
	case EMeshParticleizerGdfStatus::Unavailable:
		return TEXT("No usable GDF query textures were observed. Check the rendering view, distance-field meshes and distance-field AO or Lumen settings.");
	default:
		return TEXT("GDF query textures are available. Source exclusion and simulation memory reservation are checked during preparation and start.");
	}
}
