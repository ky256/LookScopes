// Copyright KuoYu. All Rights Reserved.

#include "AIGradingViewExtension.h"
#include "ScopeShaders.h"
#include "SceneView.h"
#include "FinalPostProcessSettings.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

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
	if (!RT) { bCaptureRequested.store(false); return; }

	FTextureRHIRef RTTexture = RT->GetRenderTargetTexture();
	if (!RTTexture.IsValid()) { bCaptureRequested.store(false); return; }

	FIntPoint SrcSize = RT->GetSizeXY();
	if (SrcSize.X <= 0 || SrcSize.Y <= 0) { bCaptureRequested.store(false); return; }

	constexpr int32 AI_SIZE = 256;
	const bool bNeedGPUDownsample = (SrcSize.X > AI_SIZE * 2 || SrcSize.Y > AI_SIZE * 2);

	if (bNeedGPUDownsample)
	{
		FRDGTextureRef SrcRDG = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(RTTexture, TEXT("AICaptureSrc")));

		FRDGTextureDesc DownDesc = FRDGTextureDesc::Create2D(
			FIntPoint(AI_SIZE, AI_SIZE),
			PF_R8G8B8A8,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef DownRDG = GraphBuilder.CreateTexture(DownDesc, TEXT("AIDownsample"));

		{
			FAIDownsampleCS::FParameters* PassParams = GraphBuilder.AllocParameters<FAIDownsampleCS::FParameters>();
			PassParams->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SrcRDG));
			PassParams->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
			PassParams->OutputTexture = GraphBuilder.CreateUAV(DownRDG);
			PassParams->OutputSize = FUintVector2(AI_SIZE, AI_SIZE);

			TShaderMapRef<FAIDownsampleCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const int32 Groups = FMath::DivideAndRoundUp(AI_SIZE, FAIDownsampleCS::ThreadGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("AIDownsample"),
				ComputeShader, PassParams, FIntVector(Groups, Groups, 1));
		}

		FAIReadbackParameters* ReadbackParams = GraphBuilder.AllocParameters<FAIReadbackParameters>();
		ReadbackParams->DownsampledTexture = DownRDG;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("AIReadback"),
			ReadbackParams,
			ERDGPassFlags::Readback | ERDGPassFlags::NeverCull,
			[this, DownRDG](FRHICommandListImmediate& RHICmdList)
			{
				TArray<FColor> Pixels;
				FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
				RHICmdList.ReadSurfaceData(
					DownRDG->GetRHI(),
					FIntRect(0, 0, AI_SIZE, AI_SIZE),
					Pixels,
					ReadFlags);

				if (Pixels.Num() > 0)
				{
					FScopeLock Lock(&CaptureLock);
					CapturedPixels = MoveTemp(Pixels);
					CapturedWidth = AI_SIZE;
					CapturedHeight = AI_SIZE;
					bCaptureComplete.store(true);
				}
				bCaptureRequested.store(false);
			});
	}
	else
	{
		TArray<FColor> Pixels;
		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		GraphBuilder.RHICmdList.ReadSurfaceData(
			RTTexture,
			FIntRect(0, 0, SrcSize.X, SrcSize.Y),
			Pixels,
			ReadFlags);

		if (Pixels.Num() > 0)
		{
			FScopeLock Lock(&CaptureLock);
			CapturedPixels = MoveTemp(Pixels);
			CapturedWidth = SrcSize.X;
			CapturedHeight = SrcSize.Y;
			bCaptureComplete.store(true);
		}
		bCaptureRequested.store(false);
	}
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
