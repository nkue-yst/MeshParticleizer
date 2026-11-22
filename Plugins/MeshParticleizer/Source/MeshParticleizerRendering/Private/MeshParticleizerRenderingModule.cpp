#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FMeshParticleizerRenderingModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("MeshParticleizer"));
		if (Plugin.IsValid())
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/MeshParticleizer"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
		}
	}
};

IMPLEMENT_MODULE(FMeshParticleizerRenderingModule, MeshParticleizerRendering)
