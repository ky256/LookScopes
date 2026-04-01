// Copyright KuoYu. All Rights Reserved.

#include "LookScopes.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FLookScopesModule"

void FLookScopesModule::StartupModule()
{
	RegisterShaderDirectory();

UE_LOG(LogTemp, Log, TEXT("LookScopes: 模块已加载"));
}

void FLookScopesModule::ShutdownModule()
{
UE_LOG(LogTemp, Log, TEXT("LookScopes: 模块已卸载"));
}

void FLookScopesModule::RegisterShaderDirectory()
{
	FString PluginShaderDir = FPaths::Combine(
IPluginManager::Get().FindPlugin(TEXT("LookScopes"))->GetBaseDir(),
		TEXT("Shaders")
	);

AddShaderSourceDirectoryMapping(TEXT("/Plugin/LookScopes"), PluginShaderDir);

UE_LOG(LogTemp, Log, TEXT("LookScopes: Shader 目录已注册 -> %s"), *PluginShaderDir);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLookScopesModule, LookScopes)
