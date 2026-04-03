// Copyright KuoYu. All Rights Reserved.

#include "AIGradingViewExtension.h"
#include "SceneView.h"
#include "FinalPostProcessSettings.h"
#include "RenderGraphBuilder.h"

FAIGradingViewExtension::FAIGradingViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

// ============================================================
// LUT 注入
// ============================================================

void FAIGradingViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (!bEnabled || !CurrentLUT) return;
	if (InView.bIsSceneCapture) return;

	InView.FinalPostProcessSettings.ContributingLUTs.Reset();
	InView.FinalPostProcessSettings.PushLUT(nullptr, 1.0f - Intensity);
	InView.FinalPostProcessSettings.PushLUT(CurrentLUT, Intensity);
}

bool FAIGradingViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return bEnabled || bCaptureRequested.load();
}

// ============================================================
// 渲染线程：渲染完成后直接读取 RenderTarget 像素
// ============================================================

void FAIGradingViewExtension::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
	if (!bCaptureRequested.load()) return;

	const FRenderTarget* RT = InViewFamily.RenderTarget;
	if (!RT) return;

	FTextureRHIRef RTTexture = RT->GetRenderTargetTexture();
	if (!RTTexture.IsValid()) return;

	FIntPoint Size = RT->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0) return;

	TArray<FColor> Pixels;
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
	GraphBuilder.RHICmdList.ReadSurfaceData(
		RTTexture,
		FIntRect(0, 0, Size.X, Size.Y),
		Pixels,
		ReadFlags);

	if (Pixels.Num() > 0)
	{
		FScopeLock Lock(&CaptureLock);
		CapturedPixels = MoveTemp(Pixels);
		CapturedWidth = Size.X;
		CapturedHeight = Size.Y;
		bCaptureComplete.store(true);
	}

	bCaptureRequested.store(false);
}

// ============================================================
// 游戏线程：收集捕获结果
// ============================================================

FAIGradingViewExtension::FCaptureResult FAIGradingViewExtension::CollectCaptureResult()
{
	FCaptureResult Result;
	if (!bCaptureComplete.load()) return Result;

	FScopeLock Lock(&CaptureLock);
	Result.Pixels = MoveTemp(CapturedPixels);
	Result.Width = CapturedWidth;
	Result.Height = CapturedHeight;
	Result.bIsValid = Result.Pixels.Num() > 0;
	bCaptureComplete.store(false);

	return Result;
}
