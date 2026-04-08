// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "AIColorGrader.h"
#include "BloomRenderer.h"
#include "LookScopesSubsystem.generated.h"

class SLookMatchPanel;
class FScopeSessionManager;
class FViewportStreamer;
class FCustomBloomViewExtension;

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
	virtual ~ULookScopesSubsystem();

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

	/** 推流分辨率（0,0 表示跟随视口） */
	void SetStreamResolution(FIntPoint InRes);
	FIntPoint GetStreamResolution() const { return StreamResolution; }

	/** 获取推流器 */
	TSharedPtr<FViewportStreamer> GetViewportStreamer() const { return ViewportStreamer; }

	/** AI 调色控制 */
	void EnableAIGrading(const FString& OnnxModelPath = FString());
	void DisableAIGrading();
	bool IsAIGradingEnabled() const;
	void SetAIGradingIntensity(float Intensity);
	void SetAIGradingInterval(float Seconds);
	void SetAIGradingTransitionTime(float Seconds);
	void TriggerAIInferOnce();
	FAIColorGrader* GetAIColorGrader() const;

	/** 自定义 Bloom 控制 */
	void SetCustomBloomEnabled(bool bEnabled);
	bool IsCustomBloomEnabled() const;
	FCustomBloomParams GetBloomParams() const;
	void SetSceneBloomIntensity(float V);
	void SetSceneBloomThreshold(float V);
	void SetVFXBloomIntensity(float V);
	void SetVFXBloomThreshold(float V);
	void SetBloomLevels(int32 V);
	void SetBloomScatter(float V);
	void SetMaxBrightness(float V);
	void SetBloomDebugMode(int32 Mode);
	void SetTemporalWeight(float V);

	void SaveBloomConfig() const;
	void LoadBloomConfig();

	void SaveAIGradingConfig() const;
	void LoadAIGradingConfig();

	/** AI 调色参数缓存（grader 未创建时 UI 读这里） */
	float GetCachedAIIntensity() const { return CachedAIIntensity; }
	float GetCachedAIInterval() const { return CachedAIInterval; }
	float GetCachedAITransition() const { return CachedAITransition; }

	/** 工具栏参数持久化 */
	void SetRealtimeInterval(float Seconds);
	float GetRealtimeInterval() const { return CachedRealtimeInterval; }
	void SaveToolbarConfig() const;
	void LoadToolbarConfig();

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

	/** 推流输出分辨率（0,0=跟随视口） */
	FIntPoint StreamResolution = FIntPoint::ZeroValue;

	/** 全局输入处理器（用于快捷键拦截） */
	TSharedPtr<class IInputProcessor> InputProcessor;

	/** AI 调色器 */
	TUniquePtr<FAIColorGrader> AIColorGrader;

	/** AI 调色参数缓存 */
	float CachedAIIntensity = 1.0f;
	float CachedAIInterval = 0.1f;
	float CachedAITransition = 0.3f;

	/** 工具栏参数缓存 */
	float CachedRealtimeInterval = 0.2f;

	/** 自定义 Bloom ViewExtension（独立于 AI 调色） */
	TSharedPtr<FCustomBloomViewExtension, ESPMode::ThreadSafe> BloomViewExtension;

	/** Tab ID */
	static const FName ScopeTabId;
};
