// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FRHIGPUTextureReadback;

struct FViewportCaptureResult
{
	TArray<FColor> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	bool bIsValid = false;
	int32 GetTotalPixels() const { return Width * Height; }
};

/**
 * FViewportCapture - 视口捕获服务
 *
 * 支持两种模式:
 *   同步: CaptureCurrentFrame() — FlushRenderingCommands，会阻塞游戏线程
 *   异步: RequestAsyncCapture() + IsAsyncReady() + CollectAsyncResult()
 *         不阻塞，数据延迟 1 帧
 */
class LOOKSCOPES_API FViewportCapture
{
public:
	FViewportCapture() = default;
	~FViewportCapture();

	/** 同步捕获（会 FlushRenderingCommands，慢但立即返回数据） */
	FViewportCaptureResult CaptureCurrentFrame();

	/** 异步捕获 — 发起请求（不阻塞，渲染线程完成后数据可用） */
	void RequestAsyncCapture();

	/** 上一次异步捕获是否就绪 */
	bool IsAsyncReady() const;

	/** 是否有正在进行的异步捕获 */
	bool HasPendingCapture() const { return PendingReadback != nullptr; }

	/** 收集异步捕获结果（调用后清空状态，可再次 Request） */
	FViewportCaptureResult CollectAsyncResult();

	FViewportCapture(const FViewportCapture&) = delete;
	FViewportCapture& operator=(const FViewportCapture&) = delete;

private:
	FRHIGPUTextureReadback* PendingReadback = nullptr;
	int32 PendingWidth = 0;
	int32 PendingHeight = 0;
};
