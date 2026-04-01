// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FViewportCaptureResult - 视口捕获结果
 * 
 * 纯数据结构，持有一帧的原始像素数据。
 * 所有分析模块共享同一份捕获结果，避免重复采样。
 */
struct FViewportCaptureResult
{
	/** 原始像素数据 (sRGB, 8bit per channel) */
	TArray<FColor> Pixels;

	/** 捕获的宽度 */
	int32 Width = 0;

	/** 捕获的高度 */
	int32 Height = 0;

	/** 捕获是否成功 */
	bool bIsValid = false;

	/** 总像素数（便捷访问） */
	int32 GetTotalPixels() const { return Width * Height; }
};

/**
 * FViewportCapture - 视口捕获服务
 * 
 * 职责单一：从编辑器活跃视口读取当前帧像素。
 * 
 * 设计原则：
 * - 不持有任何分析逻辑
 * - 不管理实时模式或定时器
 * - 每次调用 CaptureCurrentFrame() 都是无状态的同步操作
 * - 未来可扩展为 GPU 路径（SceneCapture2D → RenderTarget → GPU 直读）
 */
class LOOKSCOPES_API FViewportCapture
{
public:
	FViewportCapture() = default;
	~FViewportCapture() = default;

	/**
	 * 捕获当前编辑器活跃视口的一帧像素
	 * 
	 * @return 捕获结果，调用方检查 bIsValid 判断是否成功
	 */
	FViewportCaptureResult CaptureCurrentFrame();

	// 不可拷贝
	FViewportCapture(const FViewportCapture&) = delete;
	FViewportCapture& operator=(const FViewportCapture&) = delete;
};
