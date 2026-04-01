// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ViewportCapture.h"

class UTextureRenderTarget2D;

/**
 * FGPUScopeRenderer - GPU 示波器渲染器
 * 
 * 核心职责：
 * 1. 将视口捕获的像素数据上传为 GPU 纹理
 * 2. 通过 RDG 调度 Compute Shader 执行分析+可视化
 * 3. 管理输出 RenderTarget，供 Slate 直接显示
 * 
 * 数据流：
 *   FViewportCaptureResult (CPU 像素)
 *     → Upload 为 UTexture2D (GPU SRV)
 *     → Compute Shader Pass 1: 分析（密度累加 / 直方图统计）
 *     → Compute Shader Pass 2: 可视化（写入 RWTexture2D）
 *     → UTextureRenderTarget2D → FSlateBrush → Slate 显示
 * 
 * 设计原则：
 * - 替代 SScopeTextureDisplay 中的 CPU 写纹理逻辑
 * - 所有 GPU 操作在渲染线程执行
 * - 输出纹理可直接被 Slate 引用，无需 GPU→CPU 回读
 */
class LOOKSCOPES_API FGPUScopeRenderer
{
public:
	FGPUScopeRenderer();
	~FGPUScopeRenderer();

	/**
	 * 初始化 GPU 资源
	 * 
	 * @param InWaveformWidth  波形图输出宽度
	 * @param InWaveformHeight 波形图输出高度
	 * @param InHistogramWidth 直方图输出宽度
	 * @param InHistogramHeight 直方图输出高度
	 */
	void Initialize(int32 InWaveformWidth = 1024, int32 InWaveformHeight = 512,
	                int32 InHistogramWidth = 1024, int32 InHistogramHeight = 512);

	/** 释放 GPU 资源 */
	void Release();

	/** 是否已初始化 */
	bool IsInitialized() const { return bInitialized; }

	/**
	 * 执行完整的 GPU 分析+渲染流水线
	 * 
	 * @param CaptureData 视口捕获结果（CPU 像素数据）
	 */
	void Render(const FViewportCaptureResult& CaptureData);

	/** 获取波形图输出纹理（供 Slate 显示） */
	UTextureRenderTarget2D* GetWaveformRT() const { return WaveformRT; }

	/** 获取直方图输出纹理（供 Slate 显示） */
	UTextureRenderTarget2D* GetHistogramRT() const { return HistogramRT; }

	/** 获取输入纹理（供 Compute Shader 读取） */
	UTexture2D* GetInputTexture() const { return InputTexture; }

	/** 获取输入预览 RenderTarget（供 Slate 显示原始捕获画面） */
	UTextureRenderTarget2D* GetInputPreviewRT() const { return InputPreviewRT; }

	// 不可拷贝
	FGPUScopeRenderer(const FGPUScopeRenderer&) = delete;
	FGPUScopeRenderer& operator=(const FGPUScopeRenderer&) = delete;

private:
	/** 创建输出 RenderTarget */
	UTextureRenderTarget2D* CreateOutputRT(int32 Width, int32 Height, const FName& Name);

	/** 将 CPU 像素数据上传为 GPU 纹理 */
	void UploadPixelsToTexture(const FViewportCaptureResult& CaptureData);

	/** 在渲染线程执行 RDG Compute Shader 调度 */
	void DispatchComputeShaders_RenderThread(FRHICommandListImmediate& RHICmdList);

	// --- GPU 资源 ---

	/** 输入纹理（CPU 像素上传到此） */
	UTexture2D* InputTexture = nullptr;

	/** 波形图输出 RenderTarget */
	UTextureRenderTarget2D* WaveformRT = nullptr;

	/** 直方图输出 RenderTarget */
	UTextureRenderTarget2D* HistogramRT = nullptr;

	/** 输入预览 RenderTarget（从 InputTexture 拷贝，供 Slate 显示） */
	UTextureRenderTarget2D* InputPreviewRT = nullptr;

	// --- 尺寸参数 ---

	int32 WaveformWidth = 512;
	int32 WaveformHeight = 256;
	int32 HistogramWidth = 512;
	int32 HistogramHeight = 256;

	/** 输入纹理尺寸（上一帧） */
	int32 InputWidth = 0;
	int32 InputHeight = 0;

	/** 是否已初始化 */
	bool bInitialized = false;

	/** 是否有待渲染的数据 */
	bool bHasPendingData = false;
};
