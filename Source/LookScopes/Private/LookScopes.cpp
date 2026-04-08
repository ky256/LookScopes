// Copyright KuoYu. All Rights Reserved.

#include "LookScopes.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FLookScopesModule"

void FLookScopesModule::StartupModule()
{
	RegisterShaderDirectory();
	UE_LOG(LogTemp, Log, TEXT("LookScopes: Module loaded"));
}

void FLookScopesModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("LookScopes: Module unloaded"));
}

void FLookScopesModule::RegisterShaderDirectory()
{
	FString PluginShaderDir = FPaths::Combine(
		IPluginManager::Get().FindPlugin(TEXT("LookScopes"))->GetBaseDir(),
		TEXT("Shaders")
	);

	AddShaderSourceDirectoryMapping(TEXT("/Plugin/LookScopes"), PluginShaderDir);

	UE_LOG(LogTemp, Log, TEXT("LookScopes: Shader directory registered -> %s"), *PluginShaderDir);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLookScopesModule, LookScopes)
