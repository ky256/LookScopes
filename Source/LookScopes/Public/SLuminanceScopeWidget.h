// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "ScopeAnalyzer.h"

class FScopeSessionManager;

/**
 * SHistogramDisplay - 直方图自定义绘制 Widget
 * 
 * 轻量级 LeafWidget，仅负责绘制直方图柱状条和分界线。
 */
class SHistogramDisplay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SHistogramDisplay) {}
		SLATE_ARGUMENT(const FHistogramResult*, ResultPtr)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** 更新数据指针 */
	void SetResultPtr(const FHistogramResult* InPtr) { ResultPtr = InPtr; }

private:
	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

	/** 获取 bin 对应的颜色 */
	FLinearColor GetBinColor(int32 BinIndex) const;

	const FHistogramResult* ResultPtr = nullptr;

	static constexpr float Padding = 8.0f;
};

/**
 * SLuminanceScopeWidget - 亮度分析可视化面板
 * 
 * 显示内容：
 * 1. 工具栏（实时开关 + 单次分析按钮 + 状态指示）
 * 2. 直方图（256 bin 柱状图，暗部/中间调/亮部分色显示）
 * 3. 明暗占比数值（百分比）
 * 4. 平均亮度、中位数亮度
 * 
 * 设计原则：
 * - 不持有任何分析器或捕获器
 * - 通过外部传入的 SessionManager 进行操作
 * - 订阅 SessionManager 的委托接收数据更新
 */
class SLuminanceScopeWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLuminanceScopeWidget) {}
		/** 外部传入的会话管理器（由 Subsystem 持有生命周期） */
		SLATE_ARGUMENT(TSharedPtr<FScopeSessionManager>, SessionManager)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SLuminanceScopeWidget();

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

	// --- Slate 构建 ---
	
	/** 构建工具栏 */
	TSharedRef<SWidget> BuildToolbar();

	/** 构建统计信息区域 */
	TSharedRef<SWidget> BuildStatsArea();

	// --- UI 更新 ---

	/** 刷新统计文字 */
	void RefreshStatsText();

	/** 获取实时按钮文字 */
	FText GetRealtimeButtonText() const;

	/** 获取状态指示文字 */
	FText GetStatusText() const;

	/** 获取状态指示颜色 */
	FSlateColor GetStatusColor() const;

	// --- 数据 ---
	FHistogramResult CurrentResult;

	/** 会话管理器（弱引用，生命周期由 Subsystem 管理） */
	TWeakPtr<FScopeSessionManager> SessionManagerWeak;

	/** 委托句柄（用于注销） */
	FDelegateHandle AnalysisCompleteDelegateHandle;

	// --- UI 引用 ---
	TSharedPtr<STextBlock> ShadowText;
	TSharedPtr<STextBlock> MidtoneText;
	TSharedPtr<STextBlock> HighlightText;
	TSharedPtr<STextBlock> DetailText;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SHistogramDisplay> HistogramDisplay;

	// --- UI 布局常量 ---
	static constexpr float HistogramHeight = 200.0f;
	static constexpr float Padding = 8.0f;
};
