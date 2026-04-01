// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "ScopeAnalyzer.h"

class UTexture2D;
class UTextureRenderTarget2D;

/**
 * SScopeTextureDisplay - 通用示波器纹理显示 Widget
 * 
 * 核心职责：显示一张纹理（来自 CPU 或 GPU）
 * 
 * 两种模式：
 * 1. GPU 模式（推荐）：通过 SetRenderTarget() 绑定 GPU Compute Shader 的输出 RT
 *    - 零拷贝：GPU 直写 RenderTarget → FSlateBrush 直接引用
 *    - 高性能：无 CPU↔GPU 数据传输
 * 
 * 2. CPU 模式（Fallback）：通过 UpdateFromWaveform/Histogram 写入像素缓冲区
 *    - 兼容性好：不依赖 Compute Shader
 *    - 用于调试或 GPU 不可用时的降级
 * 
 * 设计原则：
 * - 不关心数据来源，只负责"显示一张纹理"
 * - GPU 模式优先，CPU 模式作为 fallback
 */
class LOOKSCOPES_API SScopeTextureDisplay : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SScopeTextureDisplay)
		: _TextureWidth(512)
		, _TextureHeight(256)
	{}
		/** 纹理宽度 */
		SLATE_ARGUMENT(int32, TextureWidth)
		/** 纹理高度 */
		SLATE_ARGUMENT(int32, TextureHeight)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SScopeTextureDisplay();

	// --- GPU 模式 ---

	/**
	 * 绑定 GPU RenderTarget 作为显示源
	 * 调用后进入 GPU 模式，CPU 写入方法将被忽略
	 * 
	 * @param InRT GPU Compute Shader 的输出 RenderTarget
	 */
	void SetRenderTarget(UTextureRenderTarget2D* InRT);

	/**
	 * 绑定 UTexture2D 作为显示源（用于输入预览等场景）
	 * 调用后进入 GPU 模式，FSlateBrush 直接引用该纹理
	 * 
	 * @param InTexture 要显示的 UTexture2D
	 */
	void SetTexture2D(UTexture2D* InTexture);

	/** 通知纹理内容已更新（GPU 模式下调用，触发重绘） */
	void MarkGPUTextureUpdated();

	// --- CPU 模式（Fallback） ---

	/** 用波形图密度数据更新纹理 */
	void UpdateFromWaveform(const FWaveformResult& Result);

/** 用直方图数据更新纹理 */
	void UpdateFromHistogram(const FHistogramResult& Result);

	/** 用原始像素数据更新纹理（CPU 模式，用于 InputPreview） */
	void UpdateFromRawPixels(const TArray<FColor>& Pixels, int32 Width, int32 Height);

	/** 清空纹理为黑色 */
	void ClearTexture();

	/** 是否处于 GPU 模式 */
	bool IsGPUMode() const { return bGPUMode; }

private:
	// --- SLeafWidget 接口 ---

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

	// --- CPU 模式纹理管理 ---

	void InitializeTexture(int32 InWidth, int32 InHeight, bool bSRGB = false);
	void FlushPixelsToTexture();
	void DrawGridLines();

	// --- 数据 ---

	/** CPU 模式：像素缓冲区 */
	TArray<FColor> PixelBuffer;

	/** CPU 模式：GPU 纹理 */
	UTexture2D* DisplayTexture = nullptr;

	/** GPU 模式：RenderTarget 引用（不持有生命周期） */
	UTextureRenderTarget2D* BoundRenderTarget = nullptr;

	/** GPU 模式：UTexture2D 引用（用于输入预览，不持有生命周期） */
	UTexture2D* BoundTexture2D = nullptr;

	/** Slate 画刷（引用 DisplayTexture 或 RenderTarget） */
	FSlateBrush TextureBrush;

	/** 纹理尺寸 */
	int32 TextureWidth = 512;
	int32 TextureHeight = 256;

	/** 是否有有效数据 */
	bool bHasValidData = false;

	/** 是否处于 GPU 模式 */
	bool bGPUMode = false;

	/** 纹理是否需要更新（CPU 模式） */
	mutable bool bTextureDirty = false;

	/** 诊断日志计数器（每个实例独立） */
	mutable int32 DiagLogCounter = 0;
};
