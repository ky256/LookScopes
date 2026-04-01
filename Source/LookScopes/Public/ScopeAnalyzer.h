// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ViewportCapture.h"

/**
 * FScopeAnalysisResultBase - 分析结果基类
 * 
 * 所有分析模块的结果都继承此基类，
 * 便于 SessionManager 统一管理和分发。
 */
struct FScopeAnalysisResultBase
{
	/** 分析是否有效 */
	bool bIsValid = false;

	virtual ~FScopeAnalysisResultBase() = default;
};

/**
 * FHistogramResult - 直方图分析结果
 */
struct LOOKSCOPES_API FHistogramResult : public FScopeAnalysisResultBase
{
	/** 256 个 bin 的直方图原始数据 */
	TArray<uint32> HistogramBins;

	/** 总像素数 */
	uint32 TotalPixels = 0;

	/** 暗部占比 (Bin 0~85, 亮度 0~0.33) */
	float ShadowRatio = 0.0f;

	/** 中间调占比 (Bin 86~170, 亮度 0.33~0.67) */
	float MidtoneRatio = 0.0f;

	/** 亮部占比 (Bin 171~255, 亮度 0.67~1.0) */
	float HighlightRatio = 0.0f;

	/** 平均亮度 */
	float AverageLuminance = 0.0f;

	/** 中位数亮度 */
	float MedianLuminance = 0.0f;

	/** 最大 bin 的值（用于直方图归一化显示） */
	uint32 MaxBinValue = 0;

	/** 从直方图原始数据计算所有统计值 */
	void ComputeStatistics();
};

/**
 * IScopeAnalyzer - 分析模块抽象接口
 * 
 * 所有分析模块（直方图、波形图、矢量示波器等）都实现此接口。
 * 
 * 设计原则：
 * - 输入：统一的 FViewportCaptureResult（一帧像素）
 * - 输出：各自的 FScopeAnalysisResultBase 派生结构
 * - 无状态：每次 Analyze() 都是独立的纯计算
 * - 可并行：多个 Analyzer 可以对同一帧数据并行执行
 */
class IScopeAnalyzer
{
public:
	virtual ~IScopeAnalyzer() = default;

	/** 获取分析器名称（用于日志和 UI 标识） */
	virtual FName GetAnalyzerName() const = 0;

	/**
	 * 对一帧捕获数据执行分析
	 * 
	 * @param CaptureData 视口捕获结果（只读引用）
	 * @return 分析结果（具体类型由子类决定）
	 */
	virtual TSharedPtr<FScopeAnalysisResultBase> Analyze(const FViewportCaptureResult& CaptureData) = 0;
};

/**
 * FHistogramAnalyzer - 直方图分析器
 * 
 * 职责：对输入像素计算 256-bin 亮度直方图及统计数据。
 * 当前使用 CPU 路径，未来可切换为 Compute Shader (FLuminanceHistogramCS)。
 */
class LOOKSCOPES_API FHistogramAnalyzer : public IScopeAnalyzer
{
public:
	virtual FName GetAnalyzerName() const override { return FName(TEXT("Histogram")); }

	virtual TSharedPtr<FScopeAnalysisResultBase> Analyze(const FViewportCaptureResult& CaptureData) override;
};

/**
 * FWaveformResult - 波形图分析结果
 * 
 * 波形图的本质：横轴对应画面水平位置（列），纵轴对应亮度值（0~1）。
 * 每一列的每个亮度级别记录有多少像素落在该位置，形成散点密度图。
 * 
 * 数据格式：一张 Width x Height 的密度图，每个像素值代表该位置的散点密度。
 */
struct LOOKSCOPES_API FWaveformResult : public FScopeAnalysisResultBase
{
	/** 
	 * 密度图数据：DensityMap[y * DensityWidth + x] = 该位置的像素累计数
	 * x 对应画面水平位置（归一化到 DensityWidth 列）
	 * y 对应亮度级别（0=最暗 在底部，DensityHeight-1=最亮 在顶部）
	 */
	TArray<uint32> DensityMap;

	/** 密度图宽度（水平分辨率，对应画面列数的降采样） */
	int32 DensityWidth = 0;

	/** 密度图高度（亮度分辨率，通常 256 级） */
	int32 DensityHeight = 0;

	/** 密度图中的最大值（用于归一化显示） */
	uint32 MaxDensity = 0;

	/** 源图像宽度 */
	int32 SourceWidth = 0;

	/** 源图像高度 */
	int32 SourceHeight = 0;
};

/**
 * FWaveformAnalyzer - 波形图分析器
 * 
 * 职责：对输入像素计算波形图密度数据。
 * 横轴 = 画面水平位置，纵轴 = 亮度（BT.709）。
 * 输出一张密度图，供 UI 层渲染为散点荧光效果。
 */
class LOOKSCOPES_API FWaveformAnalyzer : public IScopeAnalyzer
{
public:
	virtual FName GetAnalyzerName() const override { return FName(TEXT("Waveform")); }

	virtual TSharedPtr<FScopeAnalysisResultBase> Analyze(const FViewportCaptureResult& CaptureData) override;

	/** 设置密度图输出分辨率 */
	void SetOutputResolution(int32 InWidth, int32 InHeight);

private:
	/** 密度图输出宽度，默认 512（水平方向降采样） */
	int32 OutputWidth = 512;

	/** 密度图输出高度，默认 256（亮度级别数） */
	int32 OutputHeight = 256;
};