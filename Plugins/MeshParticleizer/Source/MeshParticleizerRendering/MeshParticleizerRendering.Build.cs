using UnrealBuildTool;
using System.IO;

public class MeshParticleizerRendering : ModuleRules
{
	public MeshParticleizerRendering(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "RenderCore", "RHI" });
		PrivateDependencyModuleNames.AddRange(new[] { "Projects", "Engine", "Renderer" });
		// GPU Sceneの物理コミット量は公開Scene APIにないため、このモジュールへ依存を限定する。
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));
	}
}
