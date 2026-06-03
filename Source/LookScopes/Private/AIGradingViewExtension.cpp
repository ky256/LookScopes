// Copyright KuoYu. All Rights Reserved.

#include "AIGradingViewExtension.h"
#include "ScopeShaders.h"
#include "SceneView.h"
#include "FinalPostProcessSettings.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ScreenPass.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "ImageUtils.h"
#include "Async/Async.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogAICapture, Verbose, All);

namespace
{
	static TAutoConsoleVariable<int32> CVarDumpAIInput(
		TEXT("r.LookScopes.AI.DumpInput"),
		0,
		TEXT("Set to 1 to dump the next AI model input (256x256 RGBA8) as PNG under Saved/AIDebug/."),
		ECVF_Cheat);

	// One-shot 锁：防止 CAS 成功 → AsyncTask 清零 CVar 之间的窗口期内，
	// 后续帧 readback 再次读到 CVar=1 导致多次落盘。
	static std::atomic<bool> GAIDumpConsumed{false};
}

FAIGradingViewExtension::FAIGradingViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

// ============================================================
// LUT 注入 — 始终生效，不跳过任何帧
// ============================================================

void FAIGradingViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (InView.bIsSceneCapture) return;
	if (!bEnabled || !CurrentLUT) return;

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
	TShaderMapRef<FAIDownsampleCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// FSceneView 只暴露 GetLastEyeAdaptationExposure() 这一个 exposure 取值 API；
	// UE 内部 uniform buffer 的 View.PreExposure 以此为基础通过 UpdatePreExposure()
	// 衍生，有一帧滞后但无更好的 public 替代。除以该值用于撤销 BasePass 乘入的
	// pre-exposure，回到近似相机 linear 空间，给 AI 模型输入。
	const float EyeExp = View.GetLastEyeAdaptationExposure();
	const float PreExposure = EyeExp > 0.0f ? EyeExp : 1.0f;

	// Compute ViewRect UV range — only sample the actual viewport, not the full render target
	const FIntRect VR = View.UnscaledViewRect;
	const float TexW = static_cast<float>(SceneColor.Texture->Desc.Extent.X);
	const float TexH = static_cast<float>(SceneColor.Texture->Desc.Extent.Y);
	const FVector2f ViewUVOffset(VR.Min.X / TexW, VR.Min.Y / TexH);
	const FVector2f ViewUVScale(VR.Width() / TexW, VR.Height() / TexH);
	const FVector2f FullUVOffset(0.0f, 0.0f);
	const FVector2f FullUVScale(1.0f, 1.0f);

	// Use viewport dimensions (not texture dimensions) for the downsample chain
	FRDGTextureRef CurrentTexture = SceneColor.Texture;
	int32 CurW = VR.Width();
	int32 CurH = VR.Height();
	int32 StepIdx = 0;
	bool bReadingSceneColor = true;

	FString ChainLog = FString::Printf(TEXT("tex=%dx%d view=%dx%d(fmt=%d,preExp=%.3f)"),
		static_cast<int32>(TexW), static_cast<int32>(TexH),
		CurW, CurH, static_cast<int32>(SceneColor.Texture->Desc.Format), PreExposure);

	// Helper lambda to dispatch a downsample pass
	auto DispatchDown = [&](FRDGTextureRef DstTex, int32 DstW, int32 DstH, float Exposure)
	{
		FAIDownsampleCS::FParameters* P = GraphBuilder.AllocParameters<FAIDownsampleCS::FParameters>();
		P->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(CurrentTexture));
		P->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		P->OutputTexture = GraphBuilder.CreateUAV(DstTex);
		P->OutputSize = FUintVector2(DstW, DstH);
		P->ExposureScale = Exposure;
		P->UVOffset = bReadingSceneColor ? ViewUVOffset : FullUVOffset;
		P->UVScale = bReadingSceneColor ? ViewUVScale : FullUVScale;

		const int32 GX = FMath::DivideAndRoundUp(DstW, FAIDownsampleCS::ThreadGroupSize);
		const int32 GY = FMath::DivideAndRoundUp(DstH, FAIDownsampleCS::ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("AIDown_%dx%d", DstW, DstH),
			ComputeShader, P, FIntVector(GX, GY, 1));
	};

	// Multi-pass 2x downsample chain (HDR passthrough)
	while (CurW > AI_SIZE * 2 || CurH > AI_SIZE * 2)
	{
		const int32 NextW = CurW / 2;
		const int32 NextH = CurH / 2;
		if (NextW < AI_SIZE || NextH < AI_SIZE) break;
		CurW = NextW;
		CurH = NextH;

		FRDGTextureRef IntermRDG = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(CurW, CurH), PF_FloatRGBA,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			*FString::Printf(TEXT("AIDown_Step%d"), StepIdx));

		DispatchDown(IntermRDG, CurW, CurH, 0.0f);
		ChainLog += FString::Printf(TEXT(" → %dx%d"), CurW, CurH);

		CurrentTexture = IntermRDG;
		bReadingSceneColor = false;
		StepIdx++;
	}

	// Clamp pass if one dimension still > 2x target
	if (CurW > AI_SIZE * 2 || CurH > AI_SIZE * 2)
	{
		const int32 ClampW = FMath::Min(CurW, AI_SIZE * 2);
		const int32 ClampH = FMath::Min(CurH, AI_SIZE * 2);

		FRDGTextureRef ClampRDG = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(ClampW, ClampH), PF_FloatRGBA,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			*FString::Printf(TEXT("AIDown_Clamp%d"), StepIdx));

		DispatchDown(ClampRDG, ClampW, ClampH, 0.0f);
		ChainLog += FString::Printf(TEXT(" → %dx%d(clamp)"), ClampW, ClampH);

		CurrentTexture = ClampRDG;
		bReadingSceneColor = false;
		CurW = ClampW;
		CurH = ClampH;
		StepIdx++;
	}

	// Final pass: resize to 256x256 with ACES tonemap + sRGB gamma
	FRDGTextureRef DownRDG = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(AI_SIZE, AI_SIZE), PF_R8G8B8A8,
			FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("AIDown_Final"));

	DispatchDown(DownRDG, AI_SIZE, AI_SIZE, 1.0f / PreExposure);

	// Readback pass
	FAIReadbackParameters* ReadbackParams = GraphBuilder.AllocParameters<FAIReadbackParameters>();
	ReadbackParams->DownsampledTexture = DownRDG;

	ChainLog += FString::Printf(TEXT(" → %dx%d(ACES+sRGB)"), AI_SIZE, AI_SIZE);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("AIReadback_PreTonemap"),
		ReadbackParams,
		ERDGPassFlags::Readback | ERDGPassFlags::NeverCull,
		[this, DownRDG, Chain = MoveTemp(ChainLog)](FRHICommandListImmediate& RHICmdList)
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
				static int32 CaptureLogCounter = 0;
				CaptureLogCounter++;
				const bool bDetailLog = (CaptureLogCounter <= 1) || (CaptureLogCounter % 100 == 0);

				if (bDetailLog)
				{
					uint64 SumR = 0, SumG = 0, SumB = 0;
					uint8 MinV = 255, MaxV = 0;
					for (const FColor& Px : Pixels)
					{
						SumR += Px.R; SumG += Px.G; SumB += Px.B;
						const uint8 Lum = FMath::Max3(Px.R, Px.G, Px.B);
						MinV = FMath::Min(MinV, Lum);
						MaxV = FMath::Max(MaxV, Lum);
					}
					const int32 N = Pixels.Num();
					UE_LOG(LogAICapture, Log, TEXT("捕获链 #%d: %s | 均值 R=%.1f G=%.1f B=%.1f | 范围 [%d, %d]"),
						CaptureLogCounter, *Chain,
						SumR / (double)N, SumG / (double)N, SumB / (double)N,
						MinV, MaxV);
				}

				// 一次性 PNG dump：CVar 置 1 则保存本次 readback 结果并异步归零，
				// 避免持续写盘。IConsoleVariable::Set() 仅允许 game thread 调用，
				// 故通过 AsyncTask 派发到 game thread 完成清零。
				// CAS 抢占确保即使 CVar 清零尚未落地，后续帧的 readback 也不会重复触发。
				if (CVarDumpAIInput.GetValueOnRenderThread() >= 1)
				{
					bool Expected = false;
					if (GAIDumpConsumed.compare_exchange_strong(Expected, true))
					{
						const FString DumpDir = FPaths::ProjectSavedDir() / TEXT("AIDebug");
						IFileManager::Get().MakeDirectory(*DumpDir, /*Tree=*/ true);

						const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
						const FString DumpPath = DumpDir / FString::Printf(TEXT("ue_input_%s.png"), *Timestamp);

						// 使用 PNGCompressImageArray 而非已弃用的 CompressImageArray；
						// 新 API 形参为 TArrayView64<const FColor> / TArray64<uint8>。
						// FFileHelper::SaveArrayToFile 有 const TArray64<uint8>& 重载，直接传入即可。
						TArray64<uint8> CompressedPng;
						const TArrayView64<const FColor> PixelsView(Pixels.GetData(), Pixels.Num());
						FImageUtils::PNGCompressImageArray(AI_SIZE, AI_SIZE, PixelsView, CompressedPng);

						const bool bSaved = (CompressedPng.Num() > 0)
							&& FFileHelper::SaveArrayToFile(CompressedPng, *DumpPath);

						if (bSaved)
						{
							const FString AbsPath = FPaths::ConvertRelativePathToFull(DumpPath);
							UE_LOG(LogAICapture, Log, TEXT("已保存 AI 输入样本: %s"), *AbsPath);
						}
						else
						{
							UE_LOG(LogAICapture, Warning,
								TEXT("AI 输入样本保存失败: %s (compressed=%lld bytes)"),
								*DumpPath, static_cast<int64>(CompressedPng.Num()));
						}

						// 归零 CVar 必须在 game thread 上做；按名字查而不捕获
						// CVarDumpAIInput 指针，减少 lambda 对 static 符号的隐式耦合。
						AsyncTask(ENamedThreads::GameThread, []()
						{
							if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
									TEXT("r.LookScopes.AI.DumpInput")))
							{
								CVar->Set(TEXT("0"), ECVF_SetByConsole);
							}
							GAIDumpConsumed.store(false);
						});
					}
					// CAS 失败：上一次 dump 尚未完成清零流程，本帧跳过，避免重复落盘。
				}

				FScopeLock Lock(&CaptureLock);
				CapturedPixels = MoveTemp(Pixels);
				CapturedWidth = AI_SIZE;
				CapturedHeight = AI_SIZE;
				bCaptureComplete.store(true);
			}
			bCaptureRequested.store(false);
		});

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
