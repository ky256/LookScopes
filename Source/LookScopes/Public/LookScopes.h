// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FLookScopesModule : public IModuleInterface
{
public:
	/** IModuleInterface 实现 */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	/** 注册 Shader 目录映射 */
	void RegisterShaderDirectory();
};
