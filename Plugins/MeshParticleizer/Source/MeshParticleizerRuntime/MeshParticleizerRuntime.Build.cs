using UnrealBuildTool;

public class MeshParticleizerRuntime : ModuleRules
{
	public MeshParticleizerRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "Niagara" });
		PrivateDependencyModuleNames.AddRange(new[] { "RenderCore", "RHI", "Renderer", "NiagaraShader", "NiagaraVertexFactories", "MeshParticleizerRendering" });
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("Projects");
		}
	}
}
