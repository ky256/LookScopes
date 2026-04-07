// Copyright KuoYu. All Rights Reserved.

#include "AIGradingViewExtension.h"
#include "ScopeShaders.h"
#include "SceneView.h"
#include "FinalPostProcessSettings.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"

DEFINE_LOG_CATEGORY_STATIC(LogAICapture, Verbose, All);

FAIGradingViewExtension::FAIGradingViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

// ============================================================
// LUT 注入 — 始终生效，不跳过任何帧
// ============================================================

void FAIGradingViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (!bEnabled || !CurrentLUT) return;
	if (InView.bIsSceneCapture) return;

	// Micro-jitter the weight when LUT data has been updated.
	// This forces CombineLUTs to re-compute instead of serving stale cache.
	const uint32 CurrentCounter = LUTUpdateCounter.load();
	float Jitter = 0.0f;
	if (CurrentCounter != LastAppliedLUTCounter)
	{
		Jitter = (CurrentCounter & 1) ? 1.0e-6f : 2.0e-6f;
		LastAppliedLUTCounter = CurrentCounter;
	}

	const float W = FMath::Clamp(Intensity - Jitter, 0.0f, 1.0f);
	InView.FinalPostProcessSettings.ContributingLUTs.Reset();
	InView.FinalPostProcessSettings.PushLUT(nullptr, 1.0f - W);
	InView.FinalPostProcessSettings.PushLUT(CurrentLUT, W);
}

bool FAIGradingViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return bEnabled || bCaptureRequested.load();
}

// ============================================================
// 订阅 MotionBlur pass — 在 CombineLUTs + Tonemap 之前捕获
// ============================================================

void FAIGradingViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	if (Pass == EPostProcessingPass::MotionBlur
		&& bCaptureRequested.load()
		&& !InView.bIsSceneCapture)
	{
		InOutPassCallbacks.Add(
			FAfterPassCallbackDelegate::CreateRaw(
				this, &FAIGradingViewExtension::OnPreTonemapCapture_RenderThread));
	}
}

// ============================================================
// Pre-tonemap capture: GPU downsample + ACES tonemap + readback
// ============================================================

FScreenPassTexture FAIGradingViewExtension::OnPreTonemapCapture_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
		GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!SceneColor.IsValid())
	{
		UE_LOG(LogAICapture, Warning, TEXT("Pre-tonemap SceneColor invalid"));
		bCaptureRequested.store(false);
		return SceneColor;
	}

	constexpr int32 AI_SIZE = 256;

	// GPU downsample HDR scene → 256x256 sRGB (ACES + gamma in shader)
	FRDGTextureDesc DownDesc = FRDGTextureDesc::Create2D(
		FIntPoint(AI_SIZE, AI_SIZE),
		PF_R8G8B8A8,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef DownRDG = GraphBuilder.CreateTexture(DownDesc, TEXT("AIDownsample"));

	{
		FAIDownsampleCS::FParameters* PassParams = GraphBuilder.AllocParameters<FAIDownsampleCS::FParameters>();
		PassParams->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SceneColor.Texture));
		PassParams->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		PassParams->OutputTexture = GraphBuilder.CreateUAV(DownRDG);
		PassParams->OutputSize = FUintVector2(AI_SIZE, AI_SIZE);
		PassParams->ExposureScale = 1.0f;

		TShaderMapRef<FAIDownsampleCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const int32 Groups = FMath::DivideAndRoundUp(AI_SIZE, FAIDownsampleCS::ThreadGroupSize);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("AIDownsample_PreTonemap"),
			ComputeShader, PassParams, FIntVector(Groups, Groups, 1));
	}

	// Readback pass
	FAIReadbackParameters* ReadbackParams = GraphBuilder.AllocParameters<FAIReadbackParameters>();
	ReadbackParams->DownsampledTexture = DownRDG;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("AIReadback_PreTonemap"),
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

	UE_LOG(LogAICapture, Verbose, TEXT("Pre-tonemap capture dispatched (%dx%d → %dx%d)"),
		SceneColor.Texture->Desc.Extent.X, SceneColor.Texture->Desc.Extent.Y, AI_SIZE, AI_SIZE);

	return SceneColor;
}

// ============================================================
// PostRenderViewFamily — 保留为空（捕获已移至 pre-tonemap）
// ============================================================

void FAIGradingViewExtension::PostRenderViewFamily_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneViewFamily& InViewFamily)
{
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
