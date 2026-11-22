#include "MeshParticleizerPreview.h"
#include "AdvancedPreviewScene.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerState.h"
#include "MeshParticleizeOperation.h"
#include "MeshParticleizerComponent.h"
#include "MeshParticleizerPreparedMesh.h"
#include "MeshParticleizerWorldSubsystem.h"
#include "MeshParticleizerSurfaceTexture.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

UMeshParticleizerPreview::UMeshParticleizerPreview() = default;
UMeshParticleizerPreview::~UMeshParticleizerPreview() = default;

bool UMeshParticleizerPreview::CaptureWindow(const FString& OutputPng, FString& OutError) const
{
	OutError.Reset();
	const auto PreviewWindow = this->Window.Pin();
	if (this->bClosed == true || PreviewWindow.IsValid() == false || FSlateApplication::IsInitialized() == false ||
		FPaths::GetExtension(OutputPng).Equals(TEXT("png"), ESearchCase::IgnoreCase) == false || FPaths::FileExists(OutputPng) == true)
	{
		OutError = TEXT("Choose an unused PNG path while the preview window is open.");
		return false;
	}

	// Slateが描いたウィンドウを読み、再生状態やカメラを変更せずに記録する。
	auto Pixels = TArray<FColor>{};
	auto Size = FIntVector::ZeroValue;
	if (FSlateApplication::Get().TakeScreenshot(PreviewWindow.ToSharedRef(), Pixels, Size) == false ||
		Size.X <= 0 || Size.Y <= 0 || Pixels.Num() != int64{Size.X} * Size.Y)
	{
		OutError = TEXT("The preview window has no completed image to capture.");
		return false;
	}
	auto Compressed = TArray64<uint8>{};
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, MakeArrayView(Pixels), Compressed);
	if (FFileHelper::SaveArrayToFile(Compressed, *OutputPng, &IFileManager::Get(), FILEWRITE_NoReplaceExisting) == false)
	{
		OutError = TEXT("Could not save the preview PNG. Check the destination folder and choose an unused file name.");
		return false;
	}
	return true;
}

FAdvancedPreviewScene* UMeshParticleizerPreview::GetScene()
{
	if (this->Scene == nullptr && this->bClosed == false)
	{
		// 通常の再生経路を使い、Texture常駐もRuntimeの登録へ任せる。
		this->Scene = MakeShared<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues()
				.SetEditor(false)
				.SetTransactional(false)
				.SetForceMipsResident(false));
		this->Scene->SetFloorOffset(0);
		this->Scene->SetFloorVisibility(this->bFloorVisible, true);
		this->Scene->SetEnvironmentVisibility(false, true);

		// シーク末尾の短い刻みもそのまま進め、表示時刻とRuntimeの時計を一致させる。
		this->Scene->GetWorld()->GetWorldSettings()->MinUndilatedFrameTime = 0;
		this->Scene->GetWorld()->BeginPlay();
		// GameModeを持たないプレビューでもActorのBeginPlayとEndPlayを対にする。
		this->Scene->GetWorld()->GetWorldSettings()->NotifyBeginPlay();
		auto Spawn = FActorSpawnParameters{};
		Spawn.ObjectFlags = RF_Transient;
		this->PauseOwner = this->Scene->GetWorld()->SpawnActor<APlayerState>(Spawn);
		this->SetPaused(this->bPaused);
		this->Scene->GetWorld()->GetOutermost()->SetDirtyFlag(false);
		// UEがPreviewSceneを解放する前に、通常再生の終了通知を完了させる。
		GEditor->OnEditorClose().AddUObject(this, &UMeshParticleizerPreview::Close);
	}
	return this->Scene.Get();
}

bool UMeshParticleizerPreview::SetPreparedData(UMeshParticleizerPreparedMesh* Data, FString& OutError)
{
	if (this->bClosed == true || Data == nullptr || Data->ValidateResourceManifest(OutError) == false || Data->ValidatePreparation(OutError) == false)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Select current prepared mesh data.");
		}
		this->Error = OutError;
		return false;
	}
	auto* Static = Data->SourceMesh.LoadSynchronous();
	auto* Skeletal = Data->SkeletalMesh.LoadSynchronous();
	if (Static == nullptr && Skeletal == nullptr)
	{
		this->Error = OutError = TEXT("The prepared source mesh is unavailable.");
		return false;
	}

	this->Restore();
	this->ColorProviders.Reset();
	this->ClearSamples();
	this->SampleMaterialSlot = -1;
	this->ClearDisplayOverlay();
	this->OrderMaterials.Reset();
	this->SurfaceColorMaterials.Reset();
	this->ParticleColorMaterials.Reset();
	this->ParticleVelocityMaterials.Reset();
	this->FarParticleColorMaterials.Reset();
	this->FarParticleVelocityMaterials.Reset();
	this->BoundColorProviders.Reset();
	this->LoadedProviderMaterials.Reset();
	if (this->Subject != nullptr)
	{
		this->Subject->Destroy();
	}
	auto* World = this->GetScene()->GetWorld();
	auto Spawn = FActorSpawnParameters{};
	Spawn.ObjectFlags = RF_Transient;
	this->Subject = World->SpawnActor<AActor>(Spawn);
	if (Skeletal != nullptr)
	{
		auto* Component = NewObject<USkeletalMeshComponent>(this->Subject, NAME_None, RF_Transient);
		Component->SetSkeletalMesh(Skeletal);

		// 準備データと同じLODで、再生前から表面と候補を比較する。
		Component->SetForcedLOD(1);
		Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		this->Mesh = Component;
		this->BaseLocation = FVector(0, 0, -Skeletal->GetBounds().GetBox().Min.Z);
	}
	else
	{
		auto* Component = NewObject<UStaticMeshComponent>(this->Subject, NAME_None, RF_Transient);
		Component->SetStaticMesh(Static);
		Component->SetForcedLodModel(1);
		this->Mesh = Component;
		this->BaseLocation = FVector(0, 0, -Static->GetBoundingBox().Min.Z);
	}
	this->Subject->SetRootComponent(this->Mesh);
	this->Subject->AddInstanceComponent(this->Mesh);
	this->Mesh->SetMobility(EComponentMobility::Movable);
	this->Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	this->Mesh->RegisterComponent();
	this->Player = NewObject<UMeshParticleizerComponent>(this->Subject, NAME_None, RF_Transient);
	this->Subject->AddInstanceComponent(this->Player);
	this->Player->RegisterComponent();
	this->Prepared = Data;
	this->Animation.Reset();
	this->ResetPose();
	if (this->Player->Configure(this->Mesh, Data, OutError) == false)
	{
		this->Error = OutError;
		return false;
	}
	this->Error.Reset();
	OutError.Reset();
	World->GetOutermost()->SetDirtyFlag(false);
	return this->UpdateDisplay(OutError);
}

bool UMeshParticleizerPreview::SetOptions(const FMeshParticleizeOptions& Settings, FString& OutError)
{
	if (Settings.Validate(OutError) == false)
	{
		this->Error = OutError;
		return false;
	}
	this->Restore();
	this->Options = Settings;
	if (this->Options.Origin != EMeshParticleizerOrigin::MultiplePoints ||
		this->Options.LocalOrigins.IsValidIndex(this->OriginPointIndex) == false)
	{
		this->OriginPointIndex = 0;
	}
	if (this->CanEditOrigin() == false)
	{
		this->bEditingOrigin = false;
	}
	this->Error.Reset();
	OutError.Reset();
	this->OnOptionsChanged.Broadcast();
	return this->UpdateDisplay(OutError);
}

bool UMeshParticleizerPreview::SetAnimation(UAnimationAsset* Asset, FString& OutError)
{
	auto* Skeletal = Cast<USkeletalMeshComponent>(this->Mesh);
	if (Skeletal == nullptr || (Asset != nullptr && (Asset->GetSkeleton() == nullptr || Skeletal->GetSkeletalMeshAsset()->GetSkeleton()->IsCompatibleForEditor(Asset->GetSkeleton()) == false)))
	{
		this->Error = OutError = TEXT("Choose an animation compatible with this skeletal mesh.");
		return false;
	}
	this->Restore();
	this->Animation = Asset;
	this->ResetPose();
	this->Error.Reset();
	OutError.Reset();
	return true;
}

bool UMeshParticleizerPreview::SetPlaybackSpeed(float Speed, FString& OutError)
{
	if (FMath::IsFinite(Speed) == false || Speed < 0.1f || Speed > 4)
	{
		OutError = TEXT("Playback speed must be between 0.1 and 4.");
		return false;
	}
	this->PlaybackSpeed = Speed;
	OutError.Reset();
	return true;
}

void UMeshParticleizerPreview::ResetPose()
{
	this->SceneTime = 0;
	if (this->Subject != nullptr)
	{
		this->Subject->SetActorTransform(FTransform(this->BaseLocation));
	}
	if (auto* Skeletal = Cast<USkeletalMeshComponent>(this->Mesh))
	{
		Skeletal->SetAnimation(this->Animation.LoadSynchronous());
		Skeletal->SetPosition(0, false);
		Skeletal->SetPlayRate(1);
		Skeletal->Play(true);
		// Pause中は次のアニメーションTickが来ないため、時刻と表示姿勢をここで揃える。
		Skeletal->TickAnimation(0, false);
		Skeletal->RefreshBoneTransforms();
	}
}

bool UMeshParticleizerPreview::SetColorProvider(int32 MaterialSlot, UMaterialInterface* Material, FString& OutError)
{
	if (this->bClosed == true || this->Prepared == nullptr || this->Prepared->DissolveMaterials.IsValidIndex(MaterialSlot) == false)
	{
		this->Error = OutError = TEXT("Select a material slot of the prepared mesh.");
		return false;
	}
	this->Restore();
	this->BoundColorProviders.Remove(MaterialSlot);
	this->LoadedProviderMaterials.Remove(MaterialSlot);
	if (Material != nullptr)
	{
		this->ColorProviders.Add(MaterialSlot, Material);
	}
	else
	{
		this->ColorProviders.Remove(MaterialSlot);
	}
	this->Error.Reset();
	OutError.Reset();
	return true;
}

bool UMeshParticleizerPreview::PrepareColorProviders(FString& OutError)
{
	auto* World = this->Scene->GetWorld();
	auto* Manager = World->GetSubsystem<UMeshParticleizerWorldSubsystem>();
	for (const auto& Entry : this->ColorProviders)
	{
		if (this->BoundColorProviders.Contains(Entry.Key) == false)
		{
			auto* Material = Entry.Value.LoadSynchronous();
			if (Material == nullptr)
			{
				OutError = TEXT("A color input material is unavailable.");
				return false;
			}
			this->LoadedProviderMaterials.Add(Entry.Key, Material);
			auto* Input = UKismetRenderingLibrary::CreateRenderTarget2D(World, 1024, 1024, RTF_RGBA16f);
			UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Input, Material);
			auto* Provider = Manager->RegisterColorProvider(Input, OutError);
			if (Provider == nullptr)
			{
				return false;
			}
			this->BoundColorProviders.Add(Entry.Key, Provider);
		}
		auto* Provider = this->BoundColorProviders[Entry.Key].Get();
		if (Provider->IsReady(OutError) == false)
		{
			return false;
		}
		auto Parameters = FMeshParticleizerSurfaceColorParameters{};
		Parameters.Texture = Provider;
		if (this->Player->SetSurfaceColorParameters(0, Entry.Key, Parameters, OutError) == false)
		{
			return false;
		}
	}
	return true;
}

void UMeshParticleizerPreview::Restore()
{
	if (this->bClosed == true)
	{
		return;
	}
	this->bStartPending = false;
	this->SeekTarget = -1;
	this->ClearParticleDisplay();
	this->ClearDisplayOverlay();
	if (this->Player != nullptr)
	{
		// プレビューの復元・再シークでは、変換済みの残粒子も消して次の再生へ持ち越さない。
		if (auto* Previous = this->Player->GetLastOperation(); Previous != nullptr)
		{
			Previous->Cancel(true);
		}
		this->Player->RestoreMesh();
	}
	this->Operation = nullptr;
	this->Elapsed = 0;
	this->SetPaused(true);
	this->ResetPose();
}

bool UMeshParticleizerPreview::Replay(FString& OutError)
{
	if (this->bClosed == true || this->Player == nullptr)
	{
		this->Error = OutError = TEXT("Select prepared mesh data before playback.");
		return false;
	}
	this->Restore();
	this->StopEditingOrigin();
	auto Effective = this->Options;

	// 床の表示と衝突を揃える。元の再生設定やレベル内の床は変更しない。
	if (this->bFloorVisible == false)
	{
		Effective.Ground = EMeshParticleizerGround::Disabled;
	}
	else if (Effective.Ground == EMeshParticleizerGround::Automatic)
	{
		Effective.Ground = EMeshParticleizerGround::ExplicitHeight;
		Effective.GroundHeight = 0;
	}
	if (this->Player->SetOptions(Effective, OutError) == false)
	{
		this->Error = OutError;
		return false;
	}
	this->Player->PrepareForParticleize();

	// 読込に何フレームかかっても、演出の0秒は同じ姿勢から始める。
	if (auto* Skeletal = Cast<USkeletalMeshComponent>(this->Mesh))
	{
		Skeletal->SetPlayRate(0);
	}
	this->bStartPending = true;
	this->PreparationFrames = 0;
	this->SetPaused(false);
	this->Error.Reset();
	OutError.Reset();
	return true;
}

bool UMeshParticleizerPreview::Seek(float Seconds, FString& OutError)
{
	if (FMath::IsFinite(Seconds) == false || Seconds < 0 || Seconds > this->GetDuration())
	{
		OutError = TEXT("Select a time within the effect duration.");
		return false;
	}
	if (Seconds == 0)
	{
		this->Restore();
		OutError.Reset();
		return true;
	}
	if (this->Replay(OutError) == false)
	{
		return false;
	}
	// 履歴・衝突を飛ばさず、描画フレームごとに最初から進めて指定時刻で停止する。
	this->SeekTarget = Seconds;
	return true;
}

void UMeshParticleizerPreview::SetPaused(bool bPause)
{
	this->bPaused = bPause;
	if (bPause == true)
	{
		this->SeekTarget = -1;
	}
	if (this->Scene != nullptr && this->Scene->GetWorld() != nullptr)
	{
		this->Scene->GetWorld()->GetWorldSettings()->SetPauserPlayerState(bPause ? this->PauseOwner.Get() : nullptr);
	}
}

void UMeshParticleizerPreview::SetFloorVisible(bool bVisible)
{
	this->Restore();
	this->bFloorVisible = bVisible;
	if (this->Scene != nullptr)
	{
		this->Scene->SetFloorVisibility(bVisible, true);
	}
}

void UMeshParticleizerPreview::SetMovement(EMeshParticleizerPreviewMovement Movement)
{
	if (Movement > EMeshParticleizerPreviewMovement::Rotate)
	{
		return;
	}
	this->Restore();
	this->SourceMovement = Movement;
}

bool UMeshParticleizerPreview::SetDisplayMode(EMeshParticleizerPreviewDisplay Mode, FString& OutError)
{
	if (this->bClosed == true || Mode > EMeshParticleizerPreviewDisplay::Samples)
	{
		OutError = TEXT("Choose a display mode in an open preview.");
		return false;
	}
	if ((Mode == EMeshParticleizerPreviewDisplay::Order || Mode == EMeshParticleizerPreviewDisplay::SurfaceColor || Mode == EMeshParticleizerPreviewDisplay::Velocity) &&
		this->PrepareDisplayMaterials(Mode, OutError) == false)
	{
		this->Error = OutError;
		return false;
	}
	if (Mode == EMeshParticleizerPreviewDisplay::Samples && this->PrepareSamples(OutError) == false)
	{
		this->Error = OutError;
		return false;
	}
	const auto PreviousMode = this->DisplayMode;
	this->DisplayMode = Mode;
	if (this->UpdateDisplay(OutError) == false)
	{
		this->DisplayMode = PreviousMode;
		this->ClearDisplayOverlay();
		this->ClearParticleDisplay();
		this->Error = OutError;
		return false;
	}
	OutError.Reset();
	return true;
}

bool UMeshParticleizerPreview::CanEditOrigin() const
{
	return this->bClosed == false && this->Mesh != nullptr &&
		(this->Options.Origin == EMeshParticleizerOrigin::Point ||
		(this->Options.Origin == EMeshParticleizerOrigin::MultiplePoints && this->Options.LocalOrigins.Num() > 0));
}

bool UMeshParticleizerPreview::EditOrigin(int32 PointIndex, FString& OutError)
{
	if (this->CanEditOrigin() == false || (this->Options.Origin == EMeshParticleizerOrigin::Point ? PointIndex != 0 : this->Options.LocalOrigins.IsValidIndex(PointIndex) == false))
	{
		this->Error = OutError = TEXT("Select Point or a valid Multiple Points origin before editing its handle.");
		return false;
	}
	this->Restore();
	this->OriginPointIndex = PointIndex;
	this->bEditingOrigin = true;
	this->Error.Reset();
	OutError.Reset();
	return true;
}

FVector UMeshParticleizerPreview::GetOriginHandlePosition() const
{
	if (this->CanEditOrigin() == false)
	{
		return FVector::ZeroVector;
	}
	const auto Local = this->Options.Origin == EMeshParticleizerOrigin::MultiplePoints ? this->Options.LocalOrigins[this->OriginPointIndex] : this->Options.LocalOrigin;
	return this->Mesh->GetComponentTransform().TransformPosition(Local);
}

bool UMeshParticleizerPreview::SetOriginHandlePosition(FVector WorldPosition, FString& OutError)
{
	if (this->bEditingOrigin == false || this->CanEditOrigin() == false || WorldPosition.ContainsNaN() == true)
	{
		this->Error = OutError = TEXT("Enable the origin handle and provide a finite position.");
		return false;
	}
	// 表示上のワールド座標を通常再生の参照座標へ戻し、他の再生設定と同じ検証を通す。
	const auto& Transform = this->Mesh->GetComponentTransform();
	if (Transform.GetScale3D().GetAbsMin() <= UE_SMALL_NUMBER)
	{
		this->Error = OutError = TEXT("The preview transform must have non-zero scale.");
		return false;
	}
	auto Settings = this->Options;
	const auto Local = Transform.InverseTransformPosition(WorldPosition);
	if (Settings.Origin == EMeshParticleizerOrigin::MultiplePoints)
	{
		Settings.LocalOrigins[this->OriginPointIndex] = Local;
	}
	else
	{
		Settings.LocalOrigin = Local;
	}
	return this->SetOptions(Settings, OutError);
}

void UMeshParticleizerPreview::Advance(float DeltaSeconds)
{
	if (this->Scene == nullptr || this->bClosed == true || this->bAdvancing == true || IsGarbageCollecting() == true ||
		FMath::IsFinite(DeltaSeconds) == false || DeltaSeconds <= 0)
	{
		return;
	}
	const auto Guard = TGuardValue<bool>(this->bAdvancing, true);
	ON_SCOPE_EXIT
	{
		auto DisplayError = FString{};
		if (this->UpdateDisplay(DisplayError) == false)
		{
			this->Error = DisplayError;
		}
	};

	// 表示を止めてもFenceとキャッシュの解放待ちは回収する。再生時刻は進めない。
	if (this->bPaused == true)
	{
		this->Scene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
		return;
	}
	auto Step = this->SeekTarget >= 0 ? 1.0f / 60 : FMath::Min(DeltaSeconds, 1.0f / 30) * this->PlaybackSpeed;
	if (this->Operation != nullptr && this->SeekTarget >= 0)
	{
		Step = FMath::Min(Step, this->SeekTarget - this->Elapsed);
	}
	if (this->bStartPending == false)
	{
		this->SceneTime += Step;
	}
	if (this->Subject != nullptr && this->bStartPending == false)
	{
		auto Location = this->BaseLocation;
		auto Rotation = FRotator::ZeroRotator;
		if (this->SourceMovement == EMeshParticleizerPreviewMovement::Translate)
		{
			Location.X += 60 * FMath::Sin(this->SceneTime);
		}
		if (this->SourceMovement == EMeshParticleizerPreviewMovement::Rotate)
		{
			Rotation.Yaw = this->SceneTime * 45;
		}
		this->Subject->SetActorLocationAndRotation(Location, Rotation);
	}
	this->Scene->GetWorld()->Tick(LEVELTICK_All, Step);

	if (this->bStartPending == true)
	{
		++this->PreparationFrames;
		if (this->Player->GetPreparationState() == EMeshParticleizerPreparationState::Failed)
		{
			this->Error = this->Player->GetPreparationDiagnostics();
			this->bStartPending = false;
			this->SetPaused(true);
		}
		else if (this->Player->GetPreparationState() == EMeshParticleizerPreparationState::Ready && this->PreparationFrames >= 2)
		{
			if (this->PrepareColorProviders(this->Error) == false)
			{
				if (this->Error.IsEmpty() == false)
				{
					this->bStartPending = false;
					this->SetPaused(true);
				}
				return;
			}
			this->Operation = this->Player->StartParticleize(this->Error);
			this->bStartPending = false;
			if (this->Operation == nullptr)
			{
				this->SetPaused(true);
			}
			else if (auto* Skeletal = Cast<USkeletalMeshComponent>(this->Mesh))
			{
				Skeletal->SetPlayRate(1);
			}
		}
		return;
	}
	if (this->Operation != nullptr)
	{
		this->Elapsed = FMath::Min(this->Elapsed + Step, this->GetDuration());
		if (this->SeekTarget >= 0 && this->Elapsed >= this->SeekTarget)
		{
			this->SeekTarget = -1;
			this->SetPaused(true);
		}
	}
}

FString UMeshParticleizerPreview::GetStatus() const
{
	if (this->bClosed == true)
	{
		return TEXT("Closed");
	}
	if (this->Error.IsEmpty() == false)
	{
		return this->Error;
	}
	if (this->Prepared == nullptr)
	{
		return TEXT("Select prepared mesh data.");
	}
	if (this->bStartPending == true)
	{
		return TEXT("Preparing playback...");
	}
	if (this->SeekTarget >= 0)
	{
		return FString::Printf(TEXT("Seeking %.2f / %.2f s"), this->Elapsed, this->SeekTarget);
	}
	if (this->Operation == nullptr)
	{
		return TEXT("Ready");
	}
	// 密度調整の有無は通常表示にも残し、詳しい理由は負荷表示から確認できるようにする。
	return FString::Printf(TEXT("%s | %.2f / %.2f s | %s%s"),
		*StaticEnum<EMeshParticleizeState>()->GetNameStringByValue(int64(this->Operation->GetState())),
		this->Elapsed, this->GetDuration(), this->bPaused == true ? TEXT("Paused") : TEXT("Playing"), this->Operation->GetResult().bReducedQuality == true ? TEXT(" | Reduced quality (see Load)") : TEXT(""));
}

FMeshParticleizerBudgetUsage UMeshParticleizerPreview::GetBudgetUsage() const
{
	auto* World = this->Scene != nullptr ? this->Scene->GetWorld() : nullptr;
	return this->bClosed == false && World != nullptr ? World->GetSubsystem<UMeshParticleizerWorldSubsystem>()->GetBudgetUsage() : FMeshParticleizerBudgetUsage{};
}

void UMeshParticleizerPreview::Close()
{
	this->ClearParticleDisplay();
	if (this->bClosed == true)
	{
		return;
	}
	this->ClearSamples();
	if (GEditor != nullptr)
	{
		GEditor->OnEditorClose().RemoveAll(this);
	}
	this->ClearDisplayOverlay();
	this->OrderMaterials.Reset();
	this->SurfaceColorMaterials.Reset();
	this->ParticleColorMaterials.Reset();
	this->ParticleVelocityMaterials.Reset();
	this->FarParticleColorMaterials.Reset();
	this->FarParticleVelocityMaterials.Reset();

	auto* World = this->Scene != nullptr ? this->Scene->GetWorld() : nullptr;
	if (World != nullptr)
	{
		this->Restore();
		if (this->Subject != nullptr)
		{
			this->Subject->Destroy();
		}
		World->EndPlay(EEndPlayReason::Quit);
		World->GetOutermost()->SetDirtyFlag(false);
	}
	// 強制終了ではUE側が既にWorldを解放している場合がある。残った参照だけ外す。
	this->Operation = nullptr;
	this->Subject = nullptr;
	this->Mesh = nullptr;
	this->Player = nullptr;
	this->PauseOwner = nullptr;
	this->ColorProviders.Reset();
	this->BoundColorProviders.Reset();
	this->LoadedProviderMaterials.Reset();
	this->Prepared = nullptr;
	this->Animation.Reset();
	this->bClosed = true;
	if (auto WindowToClose = this->Window.Pin())
	{
		WindowToClose->RequestDestroyWindow();
	}
	else
	{
		this->ReleaseScene();
	}
}

void UMeshParticleizerPreview::ReleaseScene()
{
	// ViewportClientを破棄してから呼ぶ。閉じたセッションをPython等が保持してもWorldを残さない。
	this->Scene.Reset();
	this->Window.Reset();
}

FMeshParticleizerAssetCacheUsage UMeshParticleizerPreview::GetAssetCacheUsage() const
{
	auto* World = this->Scene != nullptr ? this->Scene->GetWorld() : nullptr;
	return this->bClosed == false && World != nullptr ? World->GetSubsystem<UMeshParticleizerWorldSubsystem>()->GetAssetCacheUsage() : FMeshParticleizerAssetCacheUsage{};
}

void UMeshParticleizerPreview::BeginDestroy()
{
	this->Close();
	this->Scene.Reset();
	Super::BeginDestroy();
}
