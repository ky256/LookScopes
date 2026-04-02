// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "LookScopesSubsystem.generated.h"

class SLookMatchPanel;
class FScopeSessionManager;
class FViewportStreamer;

/**
 * ULookScopesSubsystem - 编辑器子系统
 * 
 * 职责：
 * 1. 持有并管理 FScopeSessionManager 的生命周期
 * 2. 注册 Tab Spawner（面板窗口）
 * 3. 注册快捷键绑定（F8 触发分析）
 * 4. 注册工具菜单入口
 * 5. 管理 Widget 生命周期
 */
UCLASS()
class LOOKSCOPES_API ULookScopesSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	// --- UEditorSubsystem 接口 ---
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** 打开/聚焦亮度分析面板 */
	void OpenScopePanel();

	/** 触发一次分析（可从快捷键调用） */
	void TriggerAnalysis();

	/** 获取会话管理器 */
	TSharedPtr<FScopeSessionManager> GetSessionManager() const { return SessionManager; }

	/** NDI 推流控制 */
	void StartNDIStream(const FString& SourceName = TEXT("UE_LookScopes"));
	void StopNDIStream();
	bool IsNDIStreaming() const;

	/** 获取推流器 */
	TSharedPtr<FViewportStreamer> GetViewportStreamer() const { return ViewportStreamer; }

private:
	/** 注册 Tab Spawner */
	void RegisterTabSpawner();
	void UnregisterTabSpawner();

	/** 注册工具菜单 */
	void RegisterMenus();

	/** 注册快捷键 */
	void RegisterCommands();

	/** 初始化 SessionManager 并注册默认分析器 */
	void InitializeSessionManager();

	/** Tab 生成回调 */
	TSharedRef<SDockTab> SpawnScopeTab(const FSpawnTabArgs& Args);

	/** 会话管理器（核心，持有捕获器和所有分析器） */
	TSharedPtr<FScopeSessionManager> SessionManager;

	/** 面板 Widget 弱引用 */
	TWeakPtr<SLookMatchPanel> PanelWidgetWeak;

	/** 命令列表 */
	TSharedPtr<FUICommandList> CommandList;

	/** NDI 视口推流器 */
	TSharedPtr<FViewportStreamer> ViewportStreamer;

	/** 全局输入处理器（用于快捷键拦截） */
	TSharedPtr<class IInputProcessor> InputProcessor;

	/** Tab ID */
	static const FName ScopeTabId;
};
