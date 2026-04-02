// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "ScopeAnalyzer.h"

class FScopeSessionManager;
class SScopeTextureDisplay;
class STextComboBox;

/**
 * SLookMatchPanel - Look Match & Scopes 主面板
 * 
 * 设计文档中的主界面，严格划分为四个功能区：
 * 
 * ┌─────────────────────────────────────────────────┐
 * │                  全局控制栏 (Toolbar)              │
 * ├──────────────────────┬──────────────────────────┤
 * │                      │   波形图 (Waveform)       │
 * │   划像视口区          │                          │
 * │   (Wipe Viewport)    ├──────────────────────────┤
 * │   [Phase 2 占位]     │   直方图 (Histogram)      │
 * │                      │                          │
 * ├──────────────────────┴──────────────────────────┤
 * │              参考画廊 (Gallery) [Phase 3 占位]     │
 * └─────────────────────────────────────────────────┘
 * 
 * 当前实现（垂直切片 Phase 1）：
 * - 左侧：视口占位区（灰色背景 + 提示文字）
 * - 右侧：波形图 + 直方图（SScopeTextureDisplay 纹理渲染）
 * - 顶部：工具栏（实时开关、单次分析、间隔设置）
 * - 底部：画廊占位区
 * 
 * 设计原则：
 * - 不持有任何分析器或捕获器
 * - 通过 SessionManager 的委托接收数据更新
 * - 子窗口通过 SSplitter 分隔，用户可拖拽调整比例
 */
class LOOKSCOPES_API SLookMatchPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLookMatchPanel) {}
		/** 外部传入的会话管理器（由 Subsystem 持有生命周期） */
		SLATE_ARGUMENT(TSharedPtr<FScopeSessionManager>, SessionManager)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SLookMatchPanel();

	/** 触发单次分析 */
	void TriggerAnalysis();

	/** 开启实时分析 */
	void StartRealtime();

	/** 停止实时分析 */
	void StopRealtime();

	/** 是否处于实时模式 */
	bool IsRealtime() const;

private:
	// --- 委托回调 ---

	/** SessionManager 分析完成回调 */
	void OnAnalysisComplete(FName AnalyzerName, TSharedPtr<FScopeAnalysisResultBase> Result);

	// --- GPU 绑定 ---

	/** 将 GPU RenderTarget 绑定到显示 Widget */
	void BindGPURenderTargets();

	// --- Slate 构建 ---

	/** 构建顶部工具栏 */
	TSharedRef<SWidget> BuildToolbar();

	/** 构建左侧视口占位区 */
	TSharedRef<SWidget> BuildViewportPlaceholder();

	/** 构建右侧示波器区域 */
	TSharedRef<SWidget> BuildScopesArea();

	/** 构建底部画廊占位区 */
	TSharedRef<SWidget> BuildGalleryPlaceholder();

	// --- UI 更新 ---

	/** 获取实时按钮文字 */
	FText GetRealtimeButtonText() const;

	/** 获取状态指示文字 */
	FText GetStatusText() const;

	/** 获取状态指示颜色 */
	FSlateColor GetStatusColor() const;

	// --- 数据 ---

	/** 会话管理器（弱引用，生命周期由 Subsystem 管理） */
	TWeakPtr<FScopeSessionManager> SessionManagerWeak;

	/** 委托句柄 */
	FDelegateHandle AnalysisCompleteDelegateHandle;

	// --- 子 Widget 引用 ---

	/** 波形图纹理显示 */
	TSharedPtr<SScopeTextureDisplay> WaveformDisplay;

	/** 直方图纹理显示 */
	TSharedPtr<SScopeTextureDisplay> HistogramDisplay;

	/** 输入图像预览显示 */
	TSharedPtr<SScopeTextureDisplay> InputPreviewDisplay;

	/** 状态文字 */
	TSharedPtr<STextBlock> StatusText;

	// --- 面板折叠状态 ---

	bool bPreviewVisible = true;
	bool bScopesVisible = true;
	bool bGalleryVisible = true;

	// --- 分辨率预设 ---

	TArray<TSharedPtr<FString>> ResolutionOptionStrings;
	TArray<FIntPoint> ResolutionOptionValues;
	TSharedPtr<STextComboBox> ResolutionComboBox;
};
