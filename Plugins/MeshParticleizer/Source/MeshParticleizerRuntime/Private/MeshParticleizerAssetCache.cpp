#include "MeshParticleizerAssetCache.h"
#include "MeshParticleizerGpuMemory.h"
#include "MeshParticleizerReleaseFence.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/Texture.h"
#include "TextureResource.h"
#include "MeshParticleizerRenderAssetResidency.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

namespace
{
	bool IsMeshCompilationPending(const UObject* Asset)
	{
		const auto* Mesh = Cast<UStaticMesh>(Asset);
		return Mesh != nullptr && Mesh->IsCompiling() == true;
	}
}

bool UMeshParticleizerAssetCache::Acquire(UObject* Owner, TConstArrayView<FMeshParticleizerResource> Resources,
	FString& OutError, bool bDisplayOnly)
{
	OutError.Reset();
	this->Poll();
	if (IsValid(Owner) == false || Resources.IsEmpty())
	{
		OutError = TEXT("A resource owner and current prepared cache manifest are required.");
		return false;
	}
	auto Requested = TMap<FSoftObjectPath, int64>{};
	for (const auto& Resource : Resources)
	{
		if (bDisplayOnly == true && Resource.bDisplayResource == false)
		{
			continue;
		}
		const auto Path = Resource.Asset.ToSoftObjectPath();
		if (Path.IsNull() || Resource.GpuBytes < 0)
		{
			OutError = TEXT("Prepared resource metadata is invalid. Prepare the mesh again.");
			return false;
		}
		if (const auto* Bytes = Requested.Find(Path); Bytes != nullptr && *Bytes != Resource.GpuBytes)
		{
			OutError = TEXT("Prepared sources disagree on a shared asset's capacity. Prepare the affected meshes again.");
			return false;
		}
		Requested.Add(Path, Resource.GpuBytes);
	}

	auto AdditionalBytes = int64{0};
	auto AdditionalAssets = 0;
	for (const auto& Pair : Requested)
	{
		auto LoadedBytes = int64{0};
		// Editorの初回コンパイル中も保存済み容量を予約し、完成後の再検査までReadyにしない。
		const auto* Loaded = Pair.Key.ResolveObject();
		if (IsMeshCompilationPending(Loaded) == false && MeshParticleizerAssetGpuBytes(Loaded, LoadedBytes, OutError) == false)
		{
			return false;
		}
		if (LoadedBytes > Pair.Value)
		{
			OutError = TEXT("Loaded resource capacity exceeds its prepared manifest. Prepare this source again.");
			return false;
		}
		const auto* Entry = this->Entries.Find(Pair.Key);
		if (Entry != nullptr)
		{
			if (Entry->GpuBytes != Pair.Value)
			{
				OutError = TEXT("Shared resource metadata changed while cached. Release and prepare the affected meshes again.");
				return false;
			}
			if (Entry->bRetiring == true && Entry->RetiringAsset.IsValid() == false)
			{
				OutError = TEXT("A requested shared asset is waiting for GPU release. Retry preparation after release completes.");
				return false;
			}
		}
		else
		{
			if (Pair.Value > this->LimitBytes - AdditionalBytes)
			{
				OutError = TEXT("The required shared assets exceed the cache GPU memory limit.");
				return false;
			}
			AdditionalBytes += Pair.Value;
			++AdditionalAssets;
		}
	}

	if (const auto* Existing = this->Owners.Find(Owner))
	{
		if (Existing->Num() == Requested.Num() &&
			Existing->ContainsByPredicate([&Requested](const auto& Path) {
				return Requested.Contains(Path) == false;
			}) == false)
		{
			return true;
		}
		OutError = TEXT("Release the previous resource registration before replacing it.");
		return false;
	}

	// 使用中を保護し、古い未使用資産から参照を外す。GC待ちを即座に空きとは扱わない。
	auto Usage = this->GetUsage();
	auto ProjectedBytes = Usage.ReservedBytes;
	auto ProjectedAssets = this->Entries.Num() + AdditionalAssets;
	auto Idle = TArray<FSoftObjectPath>{};
	for (const auto& Pair : this->Entries)
	{
		if (Pair.Value.Users == 0 && Pair.Value.bLoading == false && Pair.Value.bRetiring == false && Requested.Contains(Pair.Key) == false)
		{
			Idle.Add(Pair.Key);
		}
	}
	Idle.Sort([&Entries = this->Entries](const auto& A, const auto& B) {
		return Entries[A].LastUse < Entries[B].LastUse;
	});
	for (const auto& Path : Idle)
	{
		if (AdditionalBytes <= this->LimitBytes - ProjectedBytes && ProjectedAssets <= AssetLimit)
		{
			break;
		}
		ProjectedBytes -= this->Entries[Path].GpuBytes;
		--ProjectedAssets;
		this->Evict(Path);
	}
	Usage = this->GetUsage();
	if (AdditionalBytes > this->LimitBytes - Usage.ReservedBytes || this->Entries.Num() + AdditionalAssets > AssetLimit)
	{
		OutError = FString::Printf(TEXT("Shared asset cache is full (%lld / %lld bytes, %lld waiting for GC/GPU release). New resources were not loaded."),
			Usage.ReservedBytes, this->LimitBytes, Usage.RetiringBytes);
		return false;
	}

	auto& Paths = this->Owners.Add(Owner);
	auto ToLoad = TArray<FSoftObjectPath>{};
	for (const auto& Pair : Requested)
	{
		auto& Entry = this->Entries.FindOrAdd(Pair.Key);
		Entry.GpuBytes = Pair.Value;
		Entry.LastUse = ++this->Serial;
		++Entry.Users;
		Paths.Add(Pair.Key);
		if (Entry.bRetiring == true)
		{
			Entry.Asset = Entry.RetiringAsset.Get();
			Entry.RetiringAsset.Reset();
			Entry.bRetiring = false;
		}
		if (Entry.Asset == nullptr && Entry.bLoading == false)
		{
			Entry.Asset = Pair.Key.ResolveObject();
			if (Entry.Asset == nullptr)
			{
				Entry.bLoading = true;
				ToLoad.Add(Pair.Key);
			}
		}
		if (IsMeshCompilationPending(Entry.Asset) == true && Entry.bLoading == false)
		{
			Entry.bLoading = true;
			ToLoad.AddUnique(Pair.Key);
		}
		if (auto* Mesh = Cast<UStaticMesh>(Entry.Asset); Mesh != nullptr && Entry.Residency.IsValid() == false)
		{
			Entry.Residency = FMeshParticleizerRenderAssetResidency::Acquire(Mesh);
		}
	}
	if (ToLoad.IsEmpty() == false)
	{
		// 読み込みはキャッシュが所有する。利用者が中止しても完了と回収を追跡する。
		auto& Batch = this->Loads.AddDefaulted_GetRef();
		Batch.Paths = MoveTemp(ToLoad);
		Batch.Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Batch.Paths,
			FStreamableDelegate::CreateUObject(this, &UMeshParticleizerAssetCache::CompleteLoads));
		this->CompleteLoads();
	}
	return true;
}

void UMeshParticleizerAssetCache::CompleteLoads()
{
	for (auto Index = this->Loads.Num() - 1; Index >= 0; --Index)
	{
		auto& Batch = this->Loads[Index];
		if (Batch.Handle.IsValid() && Batch.Handle->HasLoadCompleted() == false)
		{
			continue;
		}
		if (Batch.Paths.ContainsByPredicate([](const auto& Path) {
				return IsMeshCompilationPending(Path.ResolveObject());
			}) == true)
		{
			continue;
		}
		for (const auto& Path : Batch.Paths)
		{
			auto& Entry = this->Entries[Path];
			Entry.Asset = Path.ResolveObject();
			Entry.bLoading = false;
			Entry.Error.Reset();
			if (Entry.Asset == nullptr)
			{
				Entry.Error = TEXT("A shared resource could not be loaded: ") + Path.ToString();
			}
			else
			{
				auto LoadedBytes = int64{0};
				if (MeshParticleizerAssetGpuBytes(Entry.Asset, LoadedBytes, Entry.Error) == true && LoadedBytes > Entry.GpuBytes)
				{
					Entry.GpuBytes = LoadedBytes;
					Entry.Error = TEXT("Resource capacity exceeds the prepared manifest. Prepare this source again: ") + Path.ToString();
				}
				if (auto* Mesh = Cast<UStaticMesh>(Entry.Asset); Mesh != nullptr && Entry.Error.IsEmpty() == true && Entry.Users > 0)
				{
					Entry.Residency = FMeshParticleizerRenderAssetResidency::Acquire(Mesh);
				}
			}
			if (Entry.Users == 0 && Entry.Error.IsEmpty() == false)
			{
				this->Evict(Path);
			}
		}
		this->Loads.RemoveAtSwap(Index);
	}
}

void UMeshParticleizerAssetCache::Release(UObject* Owner)
{
	auto Paths = TArray<FSoftObjectPath>{};
	if (this->Owners.RemoveAndCopyValue(Owner, Paths) == false)
	{
		return;
	}
	for (const auto& Path : Paths)
	{
		if (auto* Entry = this->Entries.Find(Path))
		{
			Entry->Users = FMath::Max(0, Entry->Users - 1);
			Entry->LastUse = ++this->Serial;
			if (Entry->Users == 0)
			{
				Entry->Residency.Reset();
			}
			if (Entry->Users == 0 && Entry->bLoading == false && Entry->Error.IsEmpty() == false)
			{
				this->Evict(Path);
			}
		}
	}
}

bool UMeshParticleizerAssetCache::IsReady(const UObject* Owner, FString& OutError) const
{
	OutError.Reset();
	const auto* Paths = this->Owners.Find(Owner);
	if (Paths == nullptr)
	{
		OutError = TEXT("Shared resources are not registered.");
		return false;
	}
	auto bReady = true;
	for (const auto& Path : *Paths)
	{
		const auto& Entry = this->Entries[Path];
		if (Entry.Error.IsEmpty() == false)
		{
			OutError = Entry.Error;
			return false;
		}
		bReady &= Entry.bLoading == false && Entry.Asset != nullptr;

		// UObjectの読込完了だけでは、初回Spawnが参照するTextureはまだ使えない。
		if (const auto* Texture = Cast<UTexture>(Entry.Asset))
		{
			const auto* Resource = Texture->GetResource();
			bReady &= Resource != nullptr && Resource->IsInitialized() == true;
#if WITH_EDITOR
			bReady &= Texture->IsDefaultTexture() == false;
#endif
		}
		if (const auto* Mesh = Cast<UStaticMesh>(Entry.Asset); Mesh != nullptr)
		{
			if (IsMeshCompilationPending(Mesh) == true)
			{
				bReady = false;
				continue;
			}
			auto CurrentBytes = int64{0};
			if (MeshParticleizerAssetGpuBytes(Mesh, CurrentBytes, OutError) == false)
			{
				return false;
			}
			if (CurrentBytes > Entry.GpuBytes || Entry.Residency.IsValid() == false || Mesh->bForceMiplevelsToBeResident == false)
			{
				OutError = TEXT("The particle mesh capacity or residency changed. Prepare this source again.");
				return false;
			}
			const auto& State = Mesh->GetStreamableResourceState();
			bReady &= Mesh->GetRenderData()->IsInitialized() == true && Mesh->HasPendingInitOrStreaming() == false &&
				State.MaxNumLODs > 0 && State.NumResidentLODs == State.MaxNumLODs;
		}
	}
	return bReady;
}

void UMeshParticleizerAssetCache::PruneOwners()
{
	for (auto It = this->Owners.CreateIterator(); It; ++It)
	{
		if (It.Key().IsValid() == true)
		{
			continue;
		}
		for (const auto& Path : It.Value())
		{
			if (auto* Entry = this->Entries.Find(Path))
			{
				Entry->Users = FMath::Max(0, Entry->Users - 1);
				Entry->LastUse = ++this->Serial;
				if (Entry->Users == 0)
				{
					Entry->Residency.Reset();
				}
				if (Entry->Users == 0 && Entry->bLoading == false && Entry->Error.IsEmpty() == false)
				{
					this->Evict(Path);
				}
			}
		}
		It.RemoveCurrent();
	}
}

void UMeshParticleizerAssetCache::Evict(const FSoftObjectPath& Path)
{
	auto& Entry = this->Entries[Path];
	if (Entry.GpuBytes == 0 || Entry.Asset == nullptr)
	{
		this->Entries.Remove(Path);
		return;
	}
	Entry.RetiringAsset = Entry.Asset;
	Entry.Asset = nullptr;
	Entry.bRetiring = true;
}

void UMeshParticleizerAssetCache::TrimUnused()
{
	this->Poll();
	auto Paths = TArray<FSoftObjectPath>{};
	for (const auto& Pair : this->Entries)
	{
		if (Pair.Value.Users == 0 && Pair.Value.bLoading == false && Pair.Value.bRetiring == false)
		{
			Paths.Add(Pair.Key);
		}
	}
	for (const auto& Path : Paths)
	{
		this->Evict(Path);
	}
}

void UMeshParticleizerAssetCache::Poll()
{
	// UObject読込後もコンパイル待ちのbatchを、ワールドの通常更新で完了させる。
	this->CompleteLoads();
	this->PruneOwners();
	for (auto It = this->Entries.CreateIterator(); It; ++It)
	{
		auto& Entry = It.Value();
		// GarbageフラグだけではBeginDestroyが済んだとは限らない。実際のUObject破棄まで待つ。
		if (Entry.bRetiring == false || Entry.RetiringAsset.GetEvenIfUnreachable() != nullptr)
		{
			continue;
		}
		if (Entry.Fence.IsValid() == false)
		{
			Entry.Fence = MakeShared<FMeshParticleizerReleaseFence>();
		}
		if (Entry.Fence->Poll() == true)
		{
			It.RemoveCurrent();
		}
	}
}

bool UMeshParticleizerAssetCache::HasPendingReleases() const
{
	for (const auto& Pair : this->Entries)
	{
		if (Pair.Value.bRetiring == true)
		{
			return true;
		}
	}
	return false;
}

FMeshParticleizerAssetCacheUsage UMeshParticleizerAssetCache::GetUsage() const
{
	auto Usage = FMeshParticleizerAssetCacheUsage{};
	Usage.LimitBytes = this->LimitBytes;
	for (const auto& Pair : this->Entries)
	{
		const auto& Entry = Pair.Value;
		Usage.ReservedBytes += Entry.GpuBytes;
		if (Entry.Users > 0)
		{
			Usage.PinnedBytes += Entry.GpuBytes;
		}
		if (Entry.bLoading == true)
		{
			Usage.LoadingBytes += Entry.GpuBytes;
		}
		if (Entry.bRetiring == true)
		{
			Usage.RetiringBytes += Entry.GpuBytes;
			++Usage.RetiringAssets;
		}
		else
		{
			++Usage.CachedAssets;
		}
	}
	return Usage;
}

bool UMeshParticleizerAssetCache::SetLimit(int64 Bytes, FString& OutError)
{
	OutError.Reset();
	this->Poll();
	if (Bytes < 0 || Bytes < this->GetUsage().ReservedBytes)
	{
		OutError = TEXT("Cache limit must include cached, loading and pending release resources. Trim unused assets first.");
		return false;
	}
	this->LimitBytes = Bytes;
	return true;
}

void UMeshParticleizerAssetCache::Shutdown()
{
	this->Loads.Reset();
	this->Owners.Reset();
	this->Entries.Reset();
}
