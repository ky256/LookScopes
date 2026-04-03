// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class UTexture;

class FAIGradingViewExtension : public FSceneViewExtensionBase
{
public:
	FAIGradingViewExtension(const FAutoRegister& AutoRegister);

	// --- FSceneViewExtensionBase ---
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	// --- LUT 控制 (Game Thread) ---

	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }

	void SetIntensity(float InIntensity) { Intensity = FMath::Clamp(InIntensity, 0.0f, 1.0f); }
	float GetIntensity() const { return Intensity; }

	void SetLUTTexture(UTexture* InLUT) { CurrentLUT = InLUT; }

	// --- 异步帧捕获 (渲染线程内 ReadSurfaceData，零游戏线程阻塞) ---

	void RequestFrameCapture() { bCaptureRequested.store(true); }
	bool IsCaptureReady() const { return bCaptureComplete.load(); }
	bool HasPendingCapture() const { return bCaptureRequested.load() || bCaptureComplete.load(); }

	struct FCaptureResult
	{
		TArray<FColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		bool bIsValid = false;
	};
	FCaptureResult CollectCaptureResult();

private:
	bool bEnabled = false;
	float Intensity = 1.0f;
	UTexture* CurrentLUT = nullptr;

	// --- 帧捕获 (跨线程通信) ---
	std::atomic<bool> bCaptureRequested{false};
	std::atomic<bool> bCaptureComplete{false};
	FCriticalSection CaptureLock;
	TArray<FColor> CapturedPixels;
	int32 CapturedWidth = 0;
	int32 CapturedHeight = 0;
};
