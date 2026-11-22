#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeLock.h"

namespace MeshParticleizerGpuBudget
{
	struct FSource;
}
namespace MeshParticleizerGpuColor
{
	struct FSource;
}
namespace MeshParticleizerGpuCoverage
{
	struct FSource;
}

// 一回のソース再生だけに属する。古い読み戻しが次の再生の予約を返すことを防ぐ。
struct FMeshParticleizerGpuAllocation
{
	explicit FMeshParticleizerGpuAllocation(int32 Count)
		: CandidateCount(Count)
	{
	}

	const int32 CandidateCount;
	std::atomic<int32> DeadParticles{0};
	std::atomic<bool> bActive{true};

	// 描画スレッドだけが操作する。同じ再生のDI再初期化でも死亡記録を維持する。
	TSharedPtr<MeshParticleizerGpuBudget::FSource, ESPMode::ThreadSafe> RenderSource;
	TSharedPtr<MeshParticleizerGpuColor::FSource, ESPMode::ThreadSafe> ColorSources[2];
	TSharedPtr<MeshParticleizerGpuCoverage::FSource, ESPMode::ThreadSafe> CoverageSource;
	std::atomic<bool> bCoverageFault{false};

	void PublishDeaths(uint32 Count)
	{
		if (Count <= static_cast<uint32>(this->CandidateCount) && Count > static_cast<uint32>(this->DeadParticles.load()))
		{
			this->DeadParticles.store(static_cast<int32>(Count));
		}
	}
};

// UObjectを描画スレッドへ渡さず、少量の非同期集計だけを共有する。
struct FMeshParticleizerGpuState
{
	std::atomic<int32> RestingLimit{50000};
	std::atomic<bool> ReadbackPending{false};
	FCriticalSection Mutex;
	uint64 PublishedSerial = 0;
	uint32 RenderFrame = 0;
	uint32 Counts[5]{};
	int64 BufferBytes = 0;

	void Publish(uint64 Serial, uint32 InRenderFrame, const uint32* InCounts, int64 InBufferBytes)
	{
		const auto Lock = FScopeLock(&this->Mutex);
		if (Serial <= this->PublishedSerial)
		{
			return;
		}
		this->PublishedSerial = Serial;
		this->RenderFrame = InRenderFrame;
		FMemory::Memcpy(this->Counts, InCounts, sizeof(this->Counts));
		this->BufferBytes = InBufferBytes;
	}
};
