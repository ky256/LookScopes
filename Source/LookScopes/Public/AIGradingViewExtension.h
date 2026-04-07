// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

class UTexture;
struct FPostProcessMaterialInputs;

class FAIGradingViewExtension : public FSceneViewExtensionBase
{
public:
	FAIGradingViewExtension(const FAutoRegister& AutoRegister);

	// --- FSceneViewExtensionBase ---
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs) override;
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	// --- LUT 控制 (Game Thread) ---

	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }

	void SetCustomBloomEnabled(bool bIn) { bCustomBloomEnabled = bIn; }
	bool IsCustomBloomEnabled() const { return bCustomBloomEnabled; }

	void SetIntensity(float InIntensity) { Intensity = FMath::Clamp(InIntensity, 0.0f, 1.0f); }
	float GetIntensity() const { return Intensity; }

	void SetLUTTexture(UTexture* InLUT) { CurrentLUT = InLUT; }

	/** Called after LUT texture data is updated to invalidate CombineLUTs cache */
	void MarkLUTDirty() { LUTUpdateCounter.fetch_add(1); }

	// --- 异步帧捕获 (在 tone mapping 之前捕获干净画面) ---

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
	bool bCustomBloomEnabled = false;
	float Intensity = 1.0f;
	UTexture* CurrentLUT = nullptr;

	// Pre-tonemap capture callback (registered via SubscribeToPostProcessingPass)
	FScreenPassTexture OnPreTonemapCapture_RenderThread(
		FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

	// LUT cache invalidation — micro-jitters weight to bust CombineLUTs cache
	std::atomic<uint32> LUTUpdateCounter{0};
	uint32 LastAppliedLUTCounter = 0;

	// --- 帧捕获 (跨线程通信) ---
	std::atomic<bool> bCaptureRequested{false};
	std::atomic<bool> bCaptureComplete{false};
	FCriticalSection CaptureLock;
	TArray<FColor> CapturedPixels;
	int32 CapturedWidth = 0;
	int32 CapturedHeight = 0;
};
