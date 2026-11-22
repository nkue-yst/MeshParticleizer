#include "MeshParticleizerPreparationLibrary.h"
#include "MeshParticleizerPreparedMesh.h"
#include "Engine/Texture2D.h"

TArray<FVector4> UMeshParticleizerPreparationLibrary::ReadPreparedSurfaceColors(
	UMeshParticleizerPreparedMesh* Prepared, const TArray<int32>& SampleIndices, bool bSourceEmission)
{
	if (Prepared == nullptr)
	{
		return {};
	}
	const auto SampleCount = Prepared->SampleCount;
	if (SampleIndices.ContainsByPredicate([SampleCount](int32 Index) {
			return Index < 0 || Index >= SampleCount;
		}))
	{
		return {};
	}

	auto* Positions = Prepared->Positions.LoadSynchronous();
	auto* Colors = bSourceEmission ? Prepared->SourceEmission.LoadSynchronous() : Prepared->Colors.LoadSynchronous();
	TArray64<uint8> PositionBytes, ColorBytes;
	if (Positions == nullptr || Colors == nullptr || Positions->Source.GetFormat() != TSF_RGBA32F || Colors->Source.GetFormat() != TSF_RGBA32F ||
		Positions->Source.GetMipData(PositionBytes, 0) == false || Colors->Source.GetMipData(ColorBytes, 0) == false ||
		PositionBytes.Num() / int64{sizeof(FLinearColor)} < SampleCount || ColorBytes.Num() / int64{sizeof(FLinearColor)} < SampleCount)
	{
		return {};
	}

	const auto* PositionData = reinterpret_cast<const FLinearColor*>(PositionBytes.GetData());
	const auto* ColorData = reinterpret_cast<const FLinearColor*>(ColorBytes.GetData());
	auto Result = TArray<FVector4>{};
	Result.Reserve(SampleIndices.Num());
	for (const auto Index : SampleIndices)
	{
		const auto& Color = ColorData[Index];
		Result.Add(FVector4(Color.R, Color.G, Color.B, PositionData[Index].A));
	}
	return Result;
}
