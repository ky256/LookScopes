// Copyright KuoYu. All Rights Reserved.

#include "LookScopesCommands.h"

#define LOCTEXT_NAMESPACE "LookScopesCommands"

void FLookScopesCommands::RegisterCommands()
{
	UI_COMMAND(
		AnalyzeFrame,
		"分析当前帧",
		"捕获当前编辑器视口并分析亮度分布",
		EUserInterfaceActionType::Button,
		FInputChord(EKeys::F8)
	);

	UI_COMMAND(
		TogglePanel,
		"亮度分析面板",
		"打开或关闭亮度分析面板",
		EUserInterfaceActionType::ToggleButton,
		FInputChord(EModifierKey::Control | EModifierKey::Shift, EKeys::L)
	);
}

#undef LOCTEXT_NAMESPACE
