// Copyright KuoYu. All Rights Reserved.

#include "GPUScopeRenderer.h"
#include "ScopeShaders.h"
#include "LuminanceHistogramShader.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"
#include "PixelShaderUtils.h"
#include "ScreenPass.h"

// ============================================================
// 构造 / 析构
// ============================================================

FGPUScopeRenderer::FGPUScopeRenderer()
{
}

FGPUScopeRenderer::~FGPUScopeRenderer()
{
	Release();
}

// ============================================================
// 初始化 / 释放
// ============================================================

void FGPUScopeRenderer::Initialize(int32 InWaveformWidth, int32 InWaveformHeight,
                                    int32 InHistogramWidth, int32 InHistogramHeight)
{
	if (bInitialized)
	{
		Release();
	}

	WaveformWidth = FMath::Clamp(InWaveformWidth, 64, 2048);
	WaveformHeight = FMath::Clamp(InWaveformHeight, 64, 1024);
	HistogramWidth = FMath::Clamp(InHistogramWidth, 64, 2048);
	HistogramHeight = FMath::Clamp(InHistogramHeight, 64, 1024);

	// 创建输出 RenderTarget
	WaveformRT = CreateOutputRT(WaveformWidth, WaveformHeight, FName(TEXT("WaveformRT")));
	HistogramRT = CreateOutputRT(HistogramWidth, HistogramHeight, FName(TEXT("HistogramRT")));

	// InputPreviewRT 在首次捕获时根据实际尺寸创建，这里不预创建

	bInitialized = (WaveformRT != nullptr && HistogramRT != nullptr);

	if (bInitialized)
	{
		UE_LOG(LogTemp, Log, TEXT("GPUScopeRenderer: 初始化成功 (Waveform: %dx%d, Histogram: %dx%d)"),
			WaveformWidth, WaveformHeight, HistogramWidth, HistogramHeight);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("GPUScopeRenderer: 初始化失败"));
	}
}

void FGPUScopeRenderer::Release()
{
	if (InputTexture && InputTexture->IsValidLowLevel())
	{
		InputTexture->RemoveFromRoot();
		InputTexture = nullptr;
	}

	if (WaveformRT && WaveformRT->IsValidLowLevel())
	{
		WaveformRT->RemoveFromRoot();
		WaveformRT = nullptr;
	}

	if (HistogramRT && HistogramRT->IsValidLowLevel())
	{
		HistogramRT->RemoveFromRoot();
		HistogramRT = nullptr;
	}

	if (InputPreviewRT && InputPreviewRT->IsValidLowLevel())
	{
		InputPreviewRT->RemoveFromRoot();
		InputPreviewRT = nullptr;
	}

	bInitialized = false;
	bHasPendingData = false;

	UE_LOG(LogTemp, Log, TEXT("GPUScopeRenderer: 资源已释放"));
}

UTextureRenderTarget2D* FGPUScopeRenderer::CreateOutputRT(int32 Width, int32 Height, const FName& Name)
{
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>();
	if (!RT)
	{
		return nullptr;
	}

	RT->AddToRoot(); // 防止 GC
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->ClearColor = FLinearColor::Black;
	RT->bAutoGenerateMips = false;
	RT->bCanCreateUAV = true; // 关键：允许创建 UAV，Compute Shader 需要写入
	RT->InitAutoFormat(Width, Height);
	RT->UpdateResourceImmediate(true);

	return RT;
}

// ============================================================
// 像素上传
// ============================================================

void FGPUScopeRenderer::UploadPixelsToTexture(const FViewportCaptureResult& CaptureData)
{
	const bool bNeedRecreate = !InputTexture
		|| InputWidth != CaptureData.Width
		|| InputHeight != CaptureData.Height;

	if (bNeedRecreate)
	{
		// 尺寸变化或首次创建：重建纹理
		if (InputTexture)
		{
			InputTexture->RemoveFromRoot();
			InputTexture = nullptr;
		}

		InputTexture = UTexture2D::CreateTransient(CaptureData.Width, CaptureData.Height, PF_B8G8R8A8);
		if (!InputTexture)
		{
			UE_LOG(LogTemp, Error, TEXT("GPUScopeRenderer: 创建输入纹理失败"));
			return;
		}
		InputTexture->AddToRoot();
		InputTexture->Filter = TF_Bilinear;
		InputTexture->SRGB = false;
		InputTexture->CompressionSettings = TC_VectorDisplacementmap;

		// 首次创建：通过 BulkData 写入初始像素，然后 UpdateResource 初始化 RHI 资源
		if (InputTexture->GetPlatformData() && InputTexture->GetPlatformData()->Mips.Num() > 0)
		{
			FTexture2DMipMap& Mip = InputTexture->GetPlatformData()->Mips[0];
			void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
			if (TextureData)
			{
				const int32 DataSize = CaptureData.Width * CaptureData.Height * sizeof(FColor);
				FMemory::Memcpy(TextureData, CaptureData.Pixels.GetData(), DataSize);
				Mip.BulkData.Unlock();
			}
		}
		InputTexture->UpdateResource();

		// 关键：同步等待 RHI 资源创建完成
		// UpdateResource() 是异步的，如果不等待，Slate 绑定时 TextureRHI 可能为空
		// 这只在纹理首次创建或尺寸变化时发生，性能影响可忽略
		FlushRenderingCommands();

		InputWidth = CaptureData.Width;
		InputHeight = CaptureData.Height;

		// 同步创建/重建 InputPreviewRT（尺寸跟随输入）
		// 使用与 Waveform/Histogram 完全一致的 CreateOutputRT 方式（RTF_RGBA8 + bCanCreateUAV=true）
		// 之前使用 RTF_RGBA16f 导致 Slate 渲染器无法正确显示 HDR 格式的 RenderTarget
		if (InputPreviewRT)
		{
			InputPreviewRT->RemoveFromRoot();
			InputPreviewRT = nullptr;
		}
		InputPreviewRT = CreateOutputRT(InputWidth, InputHeight, FName(TEXT("InputPreviewRT")));

		UE_LOG(LogTemp, Log, TEXT("GPUScopeRenderer: 输入纹理已创建 (%dx%d)"),
			InputWidth, InputHeight);
	}
	else
	{
		// 尺寸不变：使用 RHIUpdateTexture2D 直接更新像素，不重建 RHI 资源
		// 这样 Slate 画刷的资源引用保持有效
		InputWidth = CaptureData.Width;
		InputHeight = CaptureData.Height;

		FTextureResource* Resource = InputTexture->GetResource();
		if (!Resource)
		{
			return;
		}

		// 拷贝像素数据到堆上，供渲染线程使用
		const int32 DataSize = CaptureData.Width * CaptureData.Height * sizeof(FColor);
		TArray<uint8> PixelCopy;
		PixelCopy.SetNumUninitialized(DataSize);
		FMemory::Memcpy(PixelCopy.GetData(), CaptureData.Pixels.GetData(), DataSize);

		const int32 Width = CaptureData.Width;
		const int32 Height = CaptureData.Height;

		ENQUEUE_RENDER_COMMAND(UpdateInputTexture)(
			[Resource, PixelData = MoveTemp(PixelCopy), Width, Height]
			(FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* RHITexture = Resource->GetTexture2DRHI();
			if (!RHITexture)
			{
				return;
			}

			const uint32 Stride = Width * sizeof(FColor);
			FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);

			RHICmdList.UpdateTexture2D(
				RHITexture,
				0,       // Mip index
				Region,
				Stride,
				PixelData.GetData()
			);
		});
	}
}

// ============================================================
// 主渲染入口
// ============================================================

void FGPUScopeRenderer::Render(const FViewportCaptureResult& CaptureData)
{
	if (!bInitialized)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUScopeRenderer: 未初始化，跳过渲染"));
		return;
	}

	if (!CaptureData.bIsValid || CaptureData.Pixels.Num() == 0)
	{
		return;
	}

	// 第一步：上传像素到 GPU 纹理
	UploadPixelsToTexture(CaptureData);

	if (!InputTexture || !InputTexture->GetResource())
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUScopeRenderer::Render - InputTexture 或 Resource 为空"));
		return;
	}

	bHasPendingData = true;

	// 第二步：在渲染线程调度 Compute Shader
	// 捕获所有需要的参数（避免在 lambda 中引用 this 的成员被修改）
	FTextureResource* InputResource = InputTexture->GetResource();
	FTextureRenderTargetResource* WaveformResource = WaveformRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* HistogramResource = HistogramRT->GameThread_GetRenderTargetResource();
	UE_LOG(LogTemp, Verbose, TEXT("GPUScopeRenderer::Render - InputResource:%p, WaveformResource:%p, HistogramResource:%p"),
		InputResource, WaveformResource, HistogramResource);

	FTextureRenderTargetResource* InputPreviewResource = InputPreviewRT ? InputPreviewRT->GameThread_GetRenderTargetResource() : nullptr;

	if (!InputResource || !WaveformResource || !HistogramResource)
	{
		UE_LOG(LogTemp, Warning, TEXT("GPUScopeRenderer::Render - 某个 Resource 为空，跳过"));
		return;
	}

	const int32 CapturedInputWidth = InputWidth;
	const int32 CapturedInputHeight = InputHeight;
	const int32 CapturedWaveformWidth = WaveformWidth;
	const int32 CapturedWaveformHeight = WaveformHeight;
	const int32 CapturedHistogramWidth = HistogramWidth;
	const int32 CapturedHistogramHeight = HistogramHeight;

	ENQUEUE_RENDER_COMMAND(GPUScopeRenderer_Dispatch)(
		[InputResource, WaveformResource, HistogramResource, InputPreviewResource,
		 CapturedInputWidth, CapturedInputHeight,
		 CapturedWaveformWidth, CapturedWaveformHeight,
		 CapturedHistogramWidth, CapturedHistogramHeight]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// --- 注册输入纹理为 RDG 资源 ---
		FRHITexture* InputRHI = InputResource->GetTexture2DRHI();
		if (!InputRHI)
		{
			UE_LOG(LogTemp, Warning, TEXT("GPUScopeRenderer: InputRHI 为空 (RHI资源未就绪)，跳过本帧"));
			return;
		}

		UE_LOG(LogTemp, Log, TEXT("GPUScopeRenderer: RDG 开始 - InputRHI: %dx%d"),
			InputRHI->GetDesc().Extent.X, InputRHI->GetDesc().Extent.Y);

		FRDGTextureRef InputRDG = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(InputRHI, TEXT("ScopeInputTexture")));

		// --- 注册波形图输出纹理 ---
		FRHITexture* WaveformRHI = WaveformResource->GetRenderTargetTexture();
		if (!WaveformRHI)
		{
			return;
		}

		FRDGTextureRef WaveformOutputRDG = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(WaveformRHI, TEXT("WaveformOutput")));

		// --- 注册直方图输出纹理 ---
		FRHITexture* HistogramRHI = HistogramResource->GetRenderTargetTexture();
		if (!HistogramRHI)
		{
			return;
		}

		FRDGTextureRef HistogramOutputRDG = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(HistogramRHI, TEXT("HistogramOutput")));

		// ============================================================
		// 波形图 Pass 1: 密度累加
		// ============================================================
		const int32 WaveformBufferSize = CapturedWaveformWidth * CapturedWaveformHeight;

		FRDGBufferRef WaveformDensityBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), WaveformBufferSize),
			TEXT("WaveformDensityBuffer"));

		// 清零密度 Buffer
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(WaveformDensityBuffer, PF_R32_UINT), 0u);

		{
			FWaveformAccumulateCS::FParameters* PassParams = GraphBuilder.AllocParameters<FWaveformAccumulateCS::FParameters>();
			PassParams->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(InputRDG));
			PassParams->InputSampler = TStaticSamplerState<SF_Point>::GetRHI();
			PassParams->DensityBuffer = GraphBuilder.CreateUAV(WaveformDensityBuffer, PF_R32_UINT);
			PassParams->InputSize = FUintVector2(CapturedInputWidth, CapturedInputHeight);
			PassParams->OutputSize = FUintVector2(CapturedWaveformWidth, CapturedWaveformHeight);
			PassParams->LuminanceWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);

			TShaderMapRef<FWaveformAccumulateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			// 每列一个线程组
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("WaveformAccumulate"),
				ComputeShader,
				PassParams,
				FIntVector(CapturedWaveformWidth, 1, 1)
			);
		}

		// ============================================================
		// 波形图 Pass 2: 可视化（使用 kdenlive gain 算法）
		// ============================================================
		{
			FWaveformVisualizeCS::FParameters* PassParams = GraphBuilder.AllocParameters<FWaveformVisualizeCS::FParameters>();
			PassParams->DensityBufferSRV = GraphBuilder.CreateSRV(WaveformDensityBuffer, PF_R32_UINT);
			PassParams->OutputTexture = GraphBuilder.CreateUAV(WaveformOutputRDG);
			PassParams->OutputSize = FUintVector2(CapturedWaveformWidth, CapturedWaveformHeight);
			PassParams->TotalInputPixels = (uint32)(CapturedInputWidth * CapturedInputHeight);

			TShaderMapRef<FWaveformVisualizeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			const int32 GroupsX = FMath::DivideAndRoundUp(CapturedWaveformWidth, FWaveformVisualizeCS::ThreadGroupSizeX);
			const int32 GroupsY = FMath::DivideAndRoundUp(CapturedWaveformHeight, FWaveformVisualizeCS::ThreadGroupSizeY);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("WaveformVisualize"),
				ComputeShader,
				PassParams,
				FIntVector(GroupsX, GroupsY, 1)
			);
		}

		// ============================================================
		// 直方图 Pass 1: 统计（复用已有的 FLuminanceHistogramCS）
		// ============================================================
		FRDGBufferRef HistogramBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 256),
			TEXT("HistogramBuffer"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HistogramBuffer, PF_R32_UINT), 0u);

		{
			FLuminanceHistogramCS::FParameters* PassParams = GraphBuilder.AllocParameters<FLuminanceHistogramCS::FParameters>();
			PassParams->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(InputRDG));
			PassParams->InputSampler = TStaticSamplerState<SF_Point>::GetRHI();
			PassParams->HistogramOutput = GraphBuilder.CreateUAV(HistogramBuffer, PF_R32_UINT);
			PassParams->TextureSize = FUintVector2(CapturedInputWidth, CapturedInputHeight);
			PassParams->LuminanceWeights = FVector3f(0.2126f, 0.7152f, 0.0722f);

			TShaderMapRef<FLuminanceHistogramCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			const int32 GroupsX = FMath::DivideAndRoundUp(CapturedInputWidth, FLuminanceHistogramCS::ThreadGroupSize);
			const int32 GroupsY = FMath::DivideAndRoundUp(CapturedInputHeight, FLuminanceHistogramCS::ThreadGroupSize);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("HistogramAccumulate"),
				ComputeShader,
				PassParams,
				FIntVector(GroupsX, GroupsY, 1)
			);
		}

		// ============================================================
		// 直方图 Pass 1.5: GPU 归约求 MaxBinValue
		// ============================================================
		FRDGBufferRef HistogramMaxBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
			TEXT("HistogramMaxBin"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HistogramMaxBuffer, PF_R32_UINT), 0u);

		{
			FBufferMaxReduceCS::FParameters* PassParams = GraphBuilder.AllocParameters<FBufferMaxReduceCS::FParameters>();
			PassParams->InputBuffer = GraphBuilder.CreateSRV(HistogramBuffer, PF_R32_UINT);
			PassParams->MaxValueOutput = GraphBuilder.CreateUAV(HistogramMaxBuffer, PF_R32_UINT);
			PassParams->BufferLength = 256u;

			TShaderMapRef<FBufferMaxReduceCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("HistogramMaxReduce"),
				ComputeShader,
				PassParams,
				FIntVector(1, 1, 1)
			);
		}

		// ============================================================
		// 直方图 Pass 2: 可视化（使用 GPU 归约的精确 MaxBinValue）
		// ============================================================
		{
			FHistogramVisualizerCS::FParameters* PassParams = GraphBuilder.AllocParameters<FHistogramVisualizerCS::FParameters>();
			PassParams->HistogramBuffer = GraphBuilder.CreateSRV(HistogramBuffer, PF_R32_UINT);
			PassParams->OutputTexture = GraphBuilder.CreateUAV(HistogramOutputRDG);
			PassParams->OutputSize = FUintVector2(CapturedHistogramWidth, CapturedHistogramHeight);
			PassParams->MaxBinBuffer = GraphBuilder.CreateSRV(HistogramMaxBuffer, PF_R32_UINT);
			PassParams->TotalInputPixels = (uint32)(CapturedInputWidth * CapturedInputHeight);

			TShaderMapRef<FHistogramVisualizerCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			const int32 GroupsX = FMath::DivideAndRoundUp(CapturedHistogramWidth, FHistogramVisualizerCS::ThreadGroupSizeX);
			const int32 GroupsY = FMath::DivideAndRoundUp(CapturedHistogramHeight, FHistogramVisualizerCS::ThreadGroupSizeY);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("HistogramVisualize"),
				ComputeShader,
				PassParams,
				FIntVector(GroupsX, GroupsY, 1)
			);
		}

		// ============================================================
		// InputPreview Pass: 将输入纹理拷贝到 InputPreviewRT
		// 在 RDG 图内执行，保证与 Waveform/Histogram 同一帧完成
		// ============================================================
		if (InputPreviewResource)
		{
			FRHITexture* InputPreviewRHI = InputPreviewResource->GetRenderTargetTexture();
			if (InputPreviewRHI)
			{
				FRDGTextureRef InputPreviewRDG = GraphBuilder.RegisterExternalTexture(
					CreateRenderTarget(InputPreviewRHI, TEXT("InputPreviewOutput")));

				// 使用 AddCopyTexturePass 将输入纹理拷贝到 InputPreviewRT
				// InputPreviewRT 现在使用 RTF_RGBA8（与 Waveform/Histogram 一致，Slate 能正确渲染）
				// 之前使用 RTF_RGBA16f 导致 Slate 无法正确显示 HDR 格式纹理
				AddCopyTexturePass(GraphBuilder, InputRDG, InputPreviewRDG,
					FRHICopyTextureInfo());

				UE_LOG(LogTemp, Log, TEXT("GPUScopeRenderer: InputPreview AddCopyTexturePass 已添加 (%dx%d)"),
					CapturedInputWidth, CapturedInputHeight);
			}
		}

		// 执行所有 RDG Pass（波形图 + 直方图 + InputPreview）
		GraphBuilder.Execute();
	});
}
