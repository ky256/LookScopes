// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ViewportCapture.h"
#include "ScopeAnalyzer.h"
#include "GPUScopeRenderer.h"
#include "Containers/Ticker.h"

/**
 * 分析完成时的多播委托
 * 
 * @param AnalyzerName 产出结果的分析器名称
 * @param Result       分析结果（具体类型需 StaticCastSharedPtr 转换）
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnScopeAnalysisComplete, FName /*AnalyzerName*/, TSharedPtr<FScopeAnalysisResultBase> /*Result*/);

/**
 * FScopeSessionManager - 分析会话管理器
 * 
 * 核心编排层，统一管理：
 * 1. 视口捕获（FViewportCapture）
 * 2. GPU 渲染器（FGPUScopeRenderer）—— Compute Shader 分析+可视化
 * 3. CPU 分析模块注册与调度（IScopeAnalyzer 集合，作为 fallback）
 * 4. 实时模式（Ticker 定时驱动）
 * 5. 结果分发（多播委托通知所有订阅者）
 * 
 * 设计原则：
 * - 一帧捕获，多模块共享：捕获一次像素，分发给所有已注册的分析器
 * - UI 层只订阅委托，不直接持有分析器
 * - Subsystem 持有 SessionManager，管理其生命周期
 */
class LOOKSCOPES_API FScopeSessionManager
{
public:
	FScopeSessionManager();
	~FScopeSessionManager();

	// --- 分析器注册 ---

	/** 注册一个分析模块 */
	void RegisterAnalyzer(TSharedPtr<IScopeAnalyzer> Analyzer);

	/** 注销一个分析模块 */
	void UnregisterAnalyzer(FName AnalyzerName);

	/** 获取已注册的分析器列表 */
	const TArray<TSharedPtr<IScopeAnalyzer>>& GetAnalyzers() const { return Analyzers; }

	// --- 单次分析 ---

	/** 触发一次完整分析流程：捕获 → 所有分析器执行 → 委托通知 */
	void AnalyzeOnce();

	// --- 实时模式 ---

	/** 开启实时分析模式 */
	void StartRealtime(float IntervalSeconds = 0.2f);

	/** 停止实时分析模式 */
	void StopRealtime();

	/** 是否处于实时分析模式 */
	bool IsRealtime() const { return bIsRealtime; }

	/** 设置实时分析间隔（秒） */
	void SetRealtimeInterval(float IntervalSeconds);

	/** 获取实时分析间隔 */
	float GetRealtimeInterval() const { return RealtimeInterval; }

	/** 是否正在分析中 */
	bool IsAnalyzing() const { return bIsAnalyzing; }

	// --- 委托 ---

	/** 分析完成委托（每个分析器完成时都会广播一次） */
	FOnScopeAnalysisComplete OnAnalysisComplete;

	// --- 结果缓存 ---

	/** 获取指定分析器的最近一次结果 */
	TSharedPtr<FScopeAnalysisResultBase> GetLastResult(FName AnalyzerName) const;

	// --- GPU 渲染器 ---

	/** 获取 GPU 渲染器（供 UI 层绑定 RenderTarget） */
	FGPUScopeRenderer& GetGPURenderer() { return GPURenderer; }
	const FGPUScopeRenderer& GetGPURenderer() const { return GPURenderer; }

	/** 是否使用 GPU 渲染模式 */
	bool IsGPUMode() const { return bUseGPU; }

	/** 设置是否使用 GPU 渲染模式 */
	void SetGPUMode(bool bEnable);

	/** 获取最后一次捕获的像素数据（用于 InputPreview CPU 模式显示） */
	const FViewportCaptureResult& GetLastCaptureData() const { return LastCaptureData; }

	// 不可拷贝
	FScopeSessionManager(const FScopeSessionManager&) = delete;
	FScopeSessionManager& operator=(const FScopeSessionManager&) = delete;

private:
	/** 执行完整的分析流水线 */
	void ExecuteAnalysisPipeline();

	/** 实时模式 Tick 回调 */
	bool OnRealtimeTick(float DeltaTime);

	/** 视口捕获服务 */
	TUniquePtr<FViewportCapture> ViewportCapture;

	/** GPU 渲染器 */
	FGPUScopeRenderer GPURenderer;

	/** 是否使用 GPU 模式 */
	bool bUseGPU = true;

	/** 最后一次捕获的数据（用于 InputPreview CPU 模式显示） */
	FViewportCaptureResult LastCaptureData;

	/** 已注册的分析器列表（CPU fallback） */
	TArray<TSharedPtr<IScopeAnalyzer>> Analyzers;

	/** 各分析器的最近结果缓存 (AnalyzerName -> Result) */
	TMap<FName, TSharedPtr<FScopeAnalysisResultBase>> LastResults;

	/** 是否正在分析 */
	bool bIsAnalyzing = false;

	/** 是否处于实时模式 */
	bool bIsRealtime = false;

	/** 实时分析间隔（秒） */
	float RealtimeInterval = 0.2f;

	/** 实时模式 Ticker 句柄 */
	FTSTicker::FDelegateHandle RealtimeTickerHandle;
};
