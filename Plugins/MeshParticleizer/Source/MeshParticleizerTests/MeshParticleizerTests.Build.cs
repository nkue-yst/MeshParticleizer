using UnrealBuildTool;

public class MeshParticleizerTests : ModuleRules
{
	public MeshParticleizerTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RenderCore", "MeshParticleizerRuntime", "Niagara" });
		PrivateDependencyModuleNames.Add("MeshParticleizerRendering");
	}
}
