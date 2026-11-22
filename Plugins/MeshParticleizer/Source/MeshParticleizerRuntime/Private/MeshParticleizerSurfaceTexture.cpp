#include "MeshParticleizerSurfaceTexture.h"
#include "MeshParticleizerAssetCache.h"
#include "MeshParticleizerRenderAssetResidency.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "MeshParticleizerColorGpu.h"
#include "RenderGraphUtils.h"
#include "TextureResource.h"
#include <atomic>

struct FMeshParticleizerColorPublication
{
	std::atomic<bool> bReady{false};
	std::atomic<bool> bFailed{false};
	FString Failure;
};

bool UMeshParticleizerSurfaceTexture::Initialize(UMeshParticleizerWorldSubsystem* Manager, UTexture2D* InTexture, FString& OutError)
{
	OutError.Reset();
	if (Manager == nullptr || IsValid(InTexture) == false || InTexture->VirtualTextureStreaming == true ||
		InTexture->GetResource() == nullptr || InTexture->GetSizeX() <= 0 || InTexture->GetSizeY() <= 0)
	{
		OutError = TEXT("Register a loaded, initialized, non-virtual Texture2D before changing surface color.");
		return false;
	}

	// 開始時にはロードしない。全Mip分を既存キャッシュへ一度だけ予約してから常駐を要求する。
	this->ReservedBytes = InTexture->CalcTextureMemorySizeEnum(TMC_AllMips);
	auto Resource = FMeshParticleizerResource{};
	Resource.Asset = InTexture;
	Resource.GpuBytes = this->ReservedBytes;
	if (Manager->GetAssetCache()->Acquire(this, MakeArrayView(&Resource, 1), OutError) == false)
	{
		return false;
	}

	this->Texture = InTexture;
	this->Cache = Manager->GetAssetCache();
	this->Residency = FMeshParticleizerRenderAssetResidency::Acquire(InTexture);
	return true;
}

bool UMeshParticleizerSurfaceTexture::BelongsTo(const UMeshParticleizerWorldSubsystem* Manager) const
{
	return this->GetOuter() == Manager && Manager != nullptr && Manager->bShuttingDown == false && this->Cache.IsValid();
}

bool UMeshParticleizerSurfaceTexture::IsReady(FString& OutError) const
{
	OutError.Reset();
	if (this->BelongsTo(this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()) == false)
	{
		OutError = TEXT("The surface texture registration's world is no longer available.");
		return false;
	}

	if (this->IsColorProvider() == true)
	{
		return this->ValidateProvider(OutError) && this->Publication->bReady.load();
	}
	const auto* Asset = Cast<UTexture2D>(this->Texture);
	if (Asset == nullptr || this->Cache.IsValid() == false || this->Residency.IsValid() == false)
	{
		OutError = TEXT("The surface texture registration is no longer available.");
		return false;
	}
	if (this->Cache->IsReady(this, OutError) == false)
	{
		return false;
	}

	if (Asset->VirtualTextureStreaming == true || Asset->CalcTextureMemorySizeEnum(TMC_AllMips) > this->ReservedBytes ||
		Asset->bForceMiplevelsToBeResident == false)
	{
		OutError = TEXT("The registered texture's capacity or residency changed. Register it again before use.");
		return false;
	}

	// Optional mip不足もReadyとせず、色をサンプルする最高解像度が揃うまで待つ。
	const auto& State = Asset->GetStreamableResourceState();
	return Asset->HasPendingInitOrStreaming() == false && State.MaxNumLODs > 0 &&
		State.NumResidentLODs == State.MaxNumLODs;
}

bool UMeshParticleizerSurfaceTexture::InitializeProvider(UMeshParticleizerWorldSubsystem* Manager,
	UTextureRenderTarget2D* Input, FString& OutError, bool bInPreserveHDR)
{
	OutError.Reset();
	if (Manager == nullptr || IsValid(Input) == false || Input->GetResource() == nullptr ||
		MeshParticleizerTextureGpuBytes(Input) <= 0)
	{
		OutError = TEXT("Register a loaded, initialized 2D Render Target with a valid GPU capacity.");
		return false;
	}

	// 入力は外部所有。公開画像は線形の単一Mipに揃え、未公開の書換えを表示へ漏らさない。
	auto* Published = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	Published->SizeX = Input->SizeX;
	Published->SizeY = Input->SizeY;
	Published->OverrideFormat = PF_FloatRGBA;
	Published->bForceLinearGamma = true;
	Published->bSupportsUAV = true;
	Published->AddressX = TA_Wrap;
	Published->AddressY = TA_Wrap;
	Published->Filter = TF_Bilinear;
	auto Resources = TArray<FMeshParticleizerResource>{};
	for (auto* Target : {Input, Published})
	{
		auto& Resource = Resources.AddDefaulted_GetRef();
		Resource.Asset = Target;
		Resource.GpuBytes = MeshParticleizerTextureGpuBytes(Target);
	}
	if (Manager->GetAssetCache()->Acquire(this, Resources, OutError) == false)
	{
		return false;
	}

	// 公開画像の確保は予約成功後だけ行う。RHI初期化と初期Clearは命令順で公開より先に実行する。
	this->Texture = Published;
	this->ProviderInput = Input;
	this->Cache = Manager->GetAssetCache();
	this->ProviderSize = FIntPoint(Input->SizeX, Input->SizeY);
	this->ProviderFormat = Input->GetFormat();
	this->bProviderSRGB = Input->IsSRGB();
	this->bPreserveHDR = bInPreserveHDR;
	this->ProviderInputBytes = Resources[0].GpuBytes;
	this->ReservedBytes = Resources[1].GpuBytes;
	this->Publication = MakeShared<FMeshParticleizerColorPublication, ESPMode::ThreadSafe>();
	Published->InitCustomFormat(Input->SizeX, Input->SizeY, PF_FloatRGBA, true);
	Published->UpdateResourceImmediate(false);
	return this->PublishColor(OutError);
}

bool UMeshParticleizerSurfaceTexture::ValidateProvider(FString& OutError) const
{
	OutError.Reset();
	const auto* Output = Cast<UTextureRenderTarget2D>(this->Texture);
	const auto* Input = this->ProviderInput.Get();
	if (this->BelongsTo(this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()) == false || Input == nullptr || Output == nullptr ||
		this->Publication.IsValid() == false)
	{
		OutError = TEXT("The Color Provider registration or its GPU publication is no longer valid.");
		return false;
	}
	if (this->Publication->bFailed.load() == true)
	{
		OutError = this->Publication->Failure;
		return false;
	}
	if (FIntPoint(Input->SizeX, Input->SizeY) != this->ProviderSize || Input->GetFormat() != this->ProviderFormat ||
		Input->IsSRGB() != this->bProviderSRGB || MeshParticleizerTextureGpuBytes(Input) != this->ProviderInputBytes ||
		FIntPoint(Output->SizeX, Output->SizeY) != this->ProviderSize || Output->GetFormat() != PF_FloatRGBA || Output->IsSRGB() == true ||
		Output->bAutoGenerateMips == true || Output->bSupportsUAV == false || Output->AddressX != TA_Wrap || Output->AddressY != TA_Wrap ||
		Output->Filter != TF_Bilinear || MeshParticleizerTextureGpuBytes(Output) != this->ReservedBytes ||
		Input->GetResource() == nullptr || Output->GetResource() == nullptr)
	{
		OutError = TEXT("The Color Provider layout changed. Release it and register the new layout before use.");
		return false;
	}
	return true;
}

bool UMeshParticleizerSurfaceTexture::PublishColor(FString& OutError)
{
	if (this->ValidateProvider(OutError) == false)
	{
		return false;
	}
	this->GetTypedOuter<UMeshParticleizerWorldSubsystem>()->PendingColorProviders.AddUnique(this);
	return true;
}

void UMeshParticleizerSurfaceTexture::PublishPending()
{
	auto Error = FString{};
	if (this->ValidateProvider(Error) == false)
	{
		return;
	}
	const auto* Input = this->ProviderInput->GetResource();
	const auto* Output = this->Texture->GetResource();
	const auto State = this->Publication;
	const auto Size = this->ProviderSize;
	const auto Format = this->ProviderFormat;
	const auto InputBytes = this->ProviderInputBytes;
	const auto OutputBytes = this->ReservedBytes;
	const auto bHDR = this->bPreserveHDR;

	// Resourceの初期化・解放と同じ命令列で参照する。TextureReferenceの更新はRHI Threadで
	// 遅延実行されるため、初回は代替画像を読む恐れがある。初期化済みResourceの実体を使う。
	ENQUEUE_RENDER_COMMAND(MeshParticleizerPublishColor)([Input, Output, State, Size, Format, InputBytes, OutputBytes, bHDR](FRHICommandListImmediate& RHICmdList) {
			auto* Source = Input->TextureRHI.GetReference();
			auto* Destination = Output->TextureRHI.GetReference();
			if (Source == nullptr || Destination == nullptr || Source->GetDesc().Extent != Size || Destination->GetDesc().Extent != Size ||
				Source->GetFormat() != Format || Destination->GetFormat() != PF_FloatRGBA || Source->GetDesc().NumSamples != 1 ||
				RHIComputeMemorySize(Source) > InputBytes || RHIComputeMemorySize(Destination) > OutputBytes)
		{
				State->Failure = FString::Printf(TEXT("Color Provider RHI layout or capacity differs from its reservation: input %dx%d format %d samples %d, %u/%lld B; output %dx%d format %d, %u/%lld B; expected %dx%d format %d."),
					Source != nullptr ? Source->GetDesc().Extent.X : 0, Source != nullptr ? Source->GetDesc().Extent.Y : 0,
					Source != nullptr ? int32(Source->GetFormat()) : -1, Source != nullptr ? int32(Source->GetDesc().NumSamples) : 0,
					Source != nullptr ? RHIComputeMemorySize(Source) : 0, InputBytes,
					Destination != nullptr ? Destination->GetDesc().Extent.X : 0, Destination != nullptr ? Destination->GetDesc().Extent.Y : 0,
					Destination != nullptr ? int32(Destination->GetFormat()) : -1, Destination != nullptr ? RHIComputeMemorySize(Destination) : 0,
					OutputBytes, Size.X, Size.Y, int32(Format));
				State->bFailed.store(true);
				return;
		}
			auto GraphBuilder = FRDGBuilder(RHICmdList);
			AddMeshParticleizerColorPublishPass(GraphBuilder, Source, Destination, bHDR);
			GraphBuilder.Execute();
			State->bReady.store(true);
		});
	++this->Revision;
}

void UMeshParticleizerSurfaceTexture::BeginDestroy()
{
	if (auto* OwnerCache = this->Cache.Get())
	{
		OwnerCache->Release(this);
	}
	this->Residency.Reset();
	Super::BeginDestroy();
}
