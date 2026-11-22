#include "MeshParticleizerPreparedMesh.h"
#include "Engine/StaticMesh.h"
#include "MeshParticleizerSurfaceBaker.h"
#include "StaticMeshAttributes.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshParticleizerPreparedDependenciesTest, "MeshParticleizer.Editor.PreparedDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshParticleizerPreparedDependenciesTest::RunTest(const FString& Parameters)
{
	// 元資産は変更せず、複製したメッシュと一時材質の編集で判定を確認する。
	auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (this->TestNotNull(TEXT("Source fixture exists"), Cube) == false)
	{
		return false;
	}
	auto Mesh = TStrongObjectPtr<UStaticMesh>(DuplicateObject<UStaticMesh>(Cube, GetTransientPackage()));
	auto Material = TStrongObjectPtr<UMaterial>(NewObject<UMaterial>());
	auto Instance = TStrongObjectPtr<UMaterialInstanceConstant>(NewObject<UMaterialInstanceConstant>());
	auto Texture = TStrongObjectPtr<UTexture2D>(NewObject<UTexture2D>());
	const uint8 Pixels[] = {255, 64, 32, 255};
	Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, Pixels);
	auto* Scalar = NewObject<UMaterialExpressionScalarParameter>(Material.Get());
	Scalar->ParameterName = TEXT("PreparedScalar");
	Scalar->DefaultValue = 0.5f;
	Material->GetExpressionCollection().AddExpression(Scalar);
	Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Scalar);
	auto* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material.Get());
	Sample->ParameterName = TEXT("PreparedTexture");
	Sample->Texture = Texture.Get();
	Material->GetExpressionCollection().AddExpression(Sample);
	Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Sample);
	Material->PostEditChange();
	Instance->SetParentEditorOnly(Material.Get());
	Mesh->GetStaticMaterials()[0].MaterialInterface = Instance.Get();

	// 別形状のLODを作り、番号だけを保存してLOD0を読む回帰を検出する。
	Mesh->AddSourceModel();
	auto* LowDescription = Mesh->CreateMeshDescription(1);
	*LowDescription = *Mesh->GetMeshDescription(0);
	auto LowPositions = FStaticMeshAttributes(*LowDescription).GetVertexPositions();
	for (const auto Vertex : LowDescription->Vertices().GetElementIDs())
	{
		LowPositions[Vertex] *= 0.25f;
	}
	Mesh->CommitMeshDescription(1);
	Mesh->Build(false);
	auto HighDescription = FMeshDescription{};
	auto LowBuiltDescription = FMeshDescription{};
	auto High = MeshParticleizerPreparation::FSurfaceSource{};
	auto Low = MeshParticleizerPreparation::FSurfaceSource{};
	auto LODError = FString{};
	if (MeshParticleizerPreparation::BuildStaticSource(Mesh.Get(), HighDescription, High, LODError, 0) == false ||
		MeshParticleizerPreparation::BuildStaticSource(Mesh.Get(), LowBuiltDescription, Low, LODError, 1) == false)
	{
		this->AddError(LODError);
		return false;
	}
	this->TestEqual(TEXT("The selected LOD is retained"), Low.SamplingLOD, 1);
	this->TestTrue(TEXT("LOD identities differ"), High.Fingerprint != Low.Fingerprint);
	this->TestTrue(TEXT("Enabled sections are retained"), Low.EnabledSections.Num() > 0);
	const auto HighVertex = FStaticMeshConstAttributes(HighDescription).GetVertexPositions()[FVertexID(0)];
	const auto LowVertex = FStaticMeshConstAttributes(LowBuiltDescription).GetVertexPositions()[FVertexID(0)];
	this->TestTrue(TEXT("Selected built geometry differs from LOD0"), LowVertex.Equals(HighVertex * 0.25f, 0.001f));
	this->TestFalse(TEXT("An unavailable LOD is rejected"), MeshParticleizerPreparation::BuildStaticSource(Mesh.Get(), LowBuiltDescription, Low, LODError, 99));
	this->TestFalse(TEXT("A negative LOD is rejected"), MeshParticleizerPreparation::BuildStaticSource(Mesh.Get(), LowBuiltDescription, Low, LODError, -1));

	auto Prepared = TStrongObjectPtr<UMeshParticleizerPreparedMesh>(NewObject<UMeshParticleizerPreparedMesh>());
	Prepared->SourceMesh = Mesh.Get();
	Prepared->SourceMaterials.Add(Instance.Get());
	Prepared->DataVersion = UMeshParticleizerPreparedMesh::CurrentDataVersion;
	auto Error = FString{};
	this->TestFalse(TEXT("Legacy data is not silently certified"), Prepared->ValidatePreparation(Error));
	this->TestTrue(TEXT("Legacy rejection requests a new bake"), Error.Contains(TEXT("new folder")));
	if (Prepared->CapturePreparationDependencies(Error) == false)
	{
		this->AddError(Error);
		return false;
	}
	this->TestTrue(TEXT("Captured dependencies are current"), Prepared->ValidatePreparation(Error));
	const auto OriginalFingerprint = Prepared->PreparationDependencies;
	Prepared->SamplingLOD = 1;
	this->TestFalse(TEXT("Changing the sampling LOD requires a new bake"), Prepared->ValidatePreparation(Error));
	Prepared->SamplingLOD = 0;
	this->TestTrue(TEXT("Restoring the sampled LOD preserves the baseline"), Prepared->ValidatePreparation(Error));

	// 過去版の生成記録は拒否し、検査で現在版の記録へ付け替えない。
	const auto PluginKey = FString(TEXT("Plugin version: MeshParticleizer"));
	this->TestTrue(TEXT("Preparation records the distribution plugin version"), OriginalFingerprint.Contains(PluginKey));
	Prepared->PreparationDependencies.Add(PluginKey, TEXT("Previous distribution version"));
	this->TestFalse(TEXT("A different plugin release requires preparation"), Prepared->ValidatePreparation(Error));
	this->TestTrue(TEXT("The plugin version is named in the diagnostic"), Error.Contains(PluginKey));
	this->TestEqual(TEXT("Validation does not certify another release"), Prepared->PreparationDependencies.FindRef(PluginKey), FString(TEXT("Previous distribution version")));
	Prepared->PreparationDependencies = OriginalFingerprint;
	this->TestTrue(TEXT("Matching release remains valid"), Prepared->ValidatePreparation(Error));

	Instance->SetScalarParameterValueEditorOnly(TEXT("PreparedScalar"), 0.25f);
	this->TestFalse(TEXT("Parameter changes are detected without PostEditChange"), Prepared->ValidatePreparation(Error));
	this->TestTrue(TEXT("Changed parameter is named"), Error.Contains(TEXT("PreparedScalar")));
	Instance->SetScalarParameterValueEditorOnly(TEXT("PreparedScalar"), 0.5f);
	this->TestTrue(TEXT("Restored effective values are current"), Prepared->ValidatePreparation(Error));
	this->TestTrue(TEXT("Inspection never rewrites the baseline"), Prepared->PreparationDependencies.OrderIndependentCompareEqual(OriginalFingerprint));

	const auto GraphId = Material->StateId;
	Material->StateId = FGuid::NewGuid();
	this->TestFalse(TEXT("Parent graph change invalidates instances"), Prepared->ValidatePreparation(Error));
	Material->StateId = GraphId;
	this->TestTrue(TEXT("Restored graph is current"), Prepared->ValidatePreparation(Error));
	const auto SlotName = Mesh->GetStaticMaterials()[0].MaterialSlotName;
	Mesh->GetStaticMaterials()[0].MaterialSlotName = TEXT("ChangedSlot");
	this->TestFalse(TEXT("Slot layout changes invalidate the preparation"), Prepared->ValidatePreparation(Error));
	Mesh->GetStaticMaterials()[0].MaterialSlotName = SlotName;

	const uint8 Reimported[] = {32, 64, 255, 255};
	Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, Reimported);
	this->TestFalse(TEXT("Reimported pixels invalidate an unchanged texture path"), Prepared->ValidatePreparation(Error));
	this->TestTrue(TEXT("Texture content change is named"), Error.Contains(TEXT("Texture:")));
	this->TestTrue(TEXT("Checks preserve the original generation snapshot"), Prepared->PreparationDependencies.OrderIndependentCompareEqual(OriginalFingerprint));
	return true;
}
#endif
