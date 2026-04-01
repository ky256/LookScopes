// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Commands/UICommandInfo.h"

/**
 * FLookScopesCommands - 快捷键命令定义
 */
class FLookScopesCommands : public TCommands<FLookScopesCommands>
{
public:
	FLookScopesCommands()
		: TCommands<FLookScopesCommands>(
			TEXT("LookScopes"),
			NSLOCTEXT("Contexts", "LookScopes", "Look Scopes"),
			NAME_None,
			FAppStyle::GetAppStyleSetName())
	{
	}

	virtual void RegisterCommands() override;

	/** 触发亮度分析 */
	TSharedPtr<FUICommandInfo> AnalyzeFrame;

	/** 打开/关闭面板 */
	TSharedPtr<FUICommandInfo> TogglePanel;
};
