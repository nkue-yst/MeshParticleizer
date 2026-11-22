using UnrealBuildTool;

public class MeshParticleizerEditor : ModuleRules
{
	public MeshParticleizerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "MeshParticleizerRuntime" });
		PrivateDependencyModuleNames.Add("MeshParticleizerRendering");
		PrivateDependencyModuleNames.Add("AdvancedPreviewScene");
		PrivateDependencyModuleNames.AddRange(new[] { "BlueprintGraph", "KismetCompiler" });
		PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "AssetRegistry", "AssetTools", "MeshDescription", "StaticMeshDescription", "MaterialBaking", "MaterialEditor", "RenderCore", "RHI", "Niagara", "NiagaraEditor", "NiagaraShader", "Projects", "Slate", "SlateCore", "InputCore", "ToolMenus", "ContentBrowser", "PropertyEditor", "Json" });
	}
}
