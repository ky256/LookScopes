// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FLookScopesModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterShaderDirectory();
};
