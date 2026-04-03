// Copyright KuoYu. All Rights Reserved.

#include "AIColorGrader.h"
#include "AIGradingViewExtension.h"
#include "NNE.h"
#include "NNERuntimeCPU.h"
#include "NNERuntimeGPU.h"
#include "NNEModelData.h"
#include "NNETypes.h"
#include "Engine/Texture2D.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "RHICommandList.h"

DEFINE_LOG_CATEGORY_STATIC(LogAIGrader, Log, All);

// ============================================================
// 构造 / 析构
// ============================================================

FAIColorGrader::FAIColorGrader()
{
	SmoothedLUT.SetNumZeroed(MODEL_LUT_FLOATS);
}

FAIColorGrader::~FAIColorGrader()
{
	Shutdown();
}

// ============================================================
// 初始化 / 关闭
// ============================================================

bool FAIColorGrader::Initialize(const FString& OnnxModelPath)
{
	if (bInitialized)
	{
		UE_LOG(LogAIGrader, Warning, TEXT("AIColorGrader 已初始化，先 Shutdown 再 Initialize"));
		return false;
	}

	// 1. 创建 2D strip LUT texture (256x16, B8G8R8A8 sRGB)
	if (!CreateLUTTexture())
	{
		UE_LOG(LogAIGrader, Error, TEXT("LUT 纹理创建失败"));
		return false;
	}
	WriteIdentityLUT();

	// 2. 注册 ViewExtension
	ViewExtension = FSceneViewExtensions::NewExtension<FAIGradingViewExtension>();
	ViewExtension->SetLUTTexture(OutputLUT);
	ViewExtension->SetIntensity(Intensity);

	// 3. 加载 ONNX 模型
	FString ModelPath = OnnxModelPath;
	if (ModelPath.IsEmpty())
	{
		ModelPath = FindDefaultModelPath();
	}

	if (!ModelPath.IsEmpty())
	{
		bModelLoaded = LoadONNXModel(ModelPath);
		if (bModelLoaded)
		{
			UE_LOG(LogAIGrader, Log, TEXT("ONNX 模型加载成功: %s"), *ModelPath);
		}
		else
		{
			UE_LOG(LogAIGrader, Warning, TEXT("ONNX 模型加载失败: %s"), *ModelPath);
		}
	}
	else
	{
		UE_LOG(LogAIGrader, Warning, TEXT("未找到 ONNX 模型文件"));
	}

	// 4. 注册 Tick
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FAIColorGrader::OnTick), 0.0f);

	bInitialized = true;
	UE_LOG(LogAIGrader, Log, TEXT("AIColorGrader 初始化完成 (model=%s)"),
		bModelLoaded ? TEXT("OK") : TEXT("N/A"));
	return true;
}

void FAIColorGrader::Shutdown()
{
	if (!bInitialized) return;

	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	if (ViewExtension.IsValid())
	{
		ViewExtension->SetEnabled(false);
		ViewExtension->SetLUTTexture(nullptr);
		ViewExtension.Reset();
	}

	NNEInstance.Reset();
	NNEModelGPU.Reset();
	NNEModelCPU.Reset();
	NNEModelData = nullptr;
	bUsingGPU = false;

	DestroyLUTTexture();

	bModelLoaded = false;
	bEnabled = false;
	bInitialized = false;
	bFirstLUT = true;
	TotalInferenceCount = 0;

	UE_LOG(LogAIGrader, Log, TEXT("AIColorGrader 已关闭"));
}

// ============================================================
// ONNX 模型加载
// ============================================================

FString FAIColorGrader::FindDefaultModelPath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LookScopes"));
	if (!Plugin.IsValid()) return FString();

	FString PluginDir = Plugin->GetBaseDir();
	FString Fp32 = PluginDir / TEXT("AI/models/lut_predictor_fp32.onnx");
	if (FPaths::FileExists(Fp32)) return Fp32;

	FString Fp16 = PluginDir / TEXT("AI/models/lut_predictor_fp16.onnx");
	if (FPaths::FileExists(Fp16)) return Fp16;

	return FString();
}

bool FAIColorGrader::LoadONNXModel(const FString& ModelPath)
{
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *ModelPath))
	{
		UE_LOG(LogAIGrader, Error, TEXT("无法读取: %s"), *ModelPath);
		return false;
	}
	UE_LOG(LogAIGrader, Log, TEXT("模型 %.1f KB"), FileData.Num() / 1024.0f);

	NNEModelData = NewObject<UNNEModelData>();
	NNEModelData->Init(TEXT("onnx"), TConstArrayView64<uint8>(FileData.GetData(), FileData.Num()));

	// GPU (DirectML) first, CPU fallback
	TWeakInterfacePtr<INNERuntimeGPU> GPURuntime = UE::NNE::GetRuntime<INNERuntimeGPU>(TEXT("NNERuntimeORTDml"));
	if (GPURuntime.IsValid())
	{
		NNEModelGPU = GPURuntime->CreateModelGPU(NNEModelData);
		if (NNEModelGPU.IsValid())
		{
			NNEInstance = NNEModelGPU->CreateModelInstanceGPU();
			if (NNEInstance.IsValid())
			{
				bUsingGPU = true;
				UE_LOG(LogAIGrader, Log, TEXT("NNE 推理后端: GPU (DirectML)"));
				return true;
			}
		}
		UE_LOG(LogAIGrader, Warning, TEXT("GPU 推理初始化失败，回退到 CPU"));
	}

	TWeakInterfacePtr<INNERuntimeCPU> CPURuntime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
	if (!CPURuntime.IsValid())
	{
		UE_LOG(LogAIGrader, Error, TEXT("NNERuntimeORTCpu 不可用"));
		return false;
	}

	NNEModelCPU = CPURuntime->CreateModelCPU(NNEModelData);
	if (!NNEModelCPU.IsValid())
	{
		UE_LOG(LogAIGrader, Error, TEXT("CreateModelCPU 失败"));
		return false;
	}

	NNEInstance = NNEModelCPU->CreateModelInstanceCPU();
	if (!NNEInstance.IsValid())
	{
		UE_LOG(LogAIGrader, Error, TEXT("CreateModelInstanceCPU 失败"));
		return false;
	}

	bUsingGPU = false;
	UE_LOG(LogAIGrader, Log, TEXT("NNE 推理后端: CPU"));
	return true;
}

// ============================================================
// LUT Texture — 2D strip (256 x 16, B8G8R8A8 sRGB)
// ============================================================

bool FAIColorGrader::CreateLUTTexture()
{
	OutputLUT = UTexture2D::CreateTransient(STRIP_WIDTH, STRIP_HEIGHT, PF_B8G8R8A8, FName(TEXT("AI_GradingLUT")));
	if (!OutputLUT) return false;

	OutputLUT->SRGB = 0;
	OutputLUT->Filter = TF_Bilinear;
	OutputLUT->LODGroup = TEXTUREGROUP_ColorLookupTable;
	OutputLUT->AddressX = TA_Clamp;
	OutputLUT->AddressY = TA_Clamp;
	OutputLUT->NeverStream = true;

	// Write identity LUT data into mip before creating GPU resource
	FTexture2DMipMap& Mip = OutputLUT->GetPlatformData()->Mips[0];
	FColor* PixelData = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	if (PixelData)
	{
		const int32 D = OUTPUT_LUT_DIM;
		const float InvD = 1.0f / (D - 1);
		for (int32 b = 0; b < D; b++)
		{
			for (int32 g = 0; g < D; g++)
			{
				for (int32 r = 0; r < D; r++)
				{
					const int32 X = r + b * D;
					const int32 Y = g;
					PixelData[Y * STRIP_WIDTH + X] = FColor(
						FMath::RoundToInt32(r * InvD * 255.0f),
						FMath::RoundToInt32(g * InvD * 255.0f),
						FMath::RoundToInt32(b * InvD * 255.0f),
						255);
				}
			}
		}
	}
	Mip.BulkData.Unlock();

	OutputLUT->UpdateResource();
	OutputLUT->AddToRoot();

	UE_LOG(LogAIGrader, Log, TEXT("LUT 纹理已创建 (%dx%d, SRGB=0) 并上传 GPU"), STRIP_WIDTH, STRIP_HEIGHT);
	return true;
}

void FAIColorGrader::DestroyLUTTexture()
{
	if (OutputLUT)
	{
		FlushRenderingCommands();
		OutputLUT->RemoveFromRoot();
		OutputLUT = nullptr;
	}
}

float FAIColorGrader::SampleLUT33(int32 Channel, float R, float G, float B) const
{
	const int32 D = MODEL_LUT_DIM;
	const int32 D3 = D * D * D;

	auto Idx = [D](int32 c, int32 r, int32 g, int32 b) -> int32
	{
		return c * D * D * D + r * D * D + g * D + b;
	};

	const float MaxIdx = static_cast<float>(D - 1);
	R = FMath::Clamp(R, 0.0f, 1.0f) * MaxIdx;
	G = FMath::Clamp(G, 0.0f, 1.0f) * MaxIdx;
	B = FMath::Clamp(B, 0.0f, 1.0f) * MaxIdx;

	const int32 r0 = FMath::Min(static_cast<int32>(R), D - 2);
	const int32 g0 = FMath::Min(static_cast<int32>(G), D - 2);
	const int32 b0 = FMath::Min(static_cast<int32>(B), D - 2);
	const float fr = R - r0, fg = G - g0, fb = B - b0;

	// Trilinear interpolation
	const float c000 = SmoothedLUT[Idx(Channel, r0,   g0,   b0  )];
	const float c001 = SmoothedLUT[Idx(Channel, r0,   g0,   b0+1)];
	const float c010 = SmoothedLUT[Idx(Channel, r0,   g0+1, b0  )];
	const float c011 = SmoothedLUT[Idx(Channel, r0,   g0+1, b0+1)];
	const float c100 = SmoothedLUT[Idx(Channel, r0+1, g0,   b0  )];
	const float c101 = SmoothedLUT[Idx(Channel, r0+1, g0,   b0+1)];
	const float c110 = SmoothedLUT[Idx(Channel, r0+1, g0+1, b0  )];
	const float c111 = SmoothedLUT[Idx(Channel, r0+1, g0+1, b0+1)];

	const float c00 = c000 * (1 - fb) + c001 * fb;
	const float c01 = c010 * (1 - fb) + c011 * fb;
	const float c10 = c100 * (1 - fb) + c101 * fb;
	const float c11 = c110 * (1 - fb) + c111 * fb;

	const float c0 = c00 * (1 - fg) + c01 * fg;
	const float c1 = c10 * (1 - fg) + c11 * fg;

	return c0 * (1 - fr) + c1 * fr;
}

void FAIColorGrader::UpdateLUTTexture()
{
	if (!OutputLUT) return;

	FTextureResource* Resource = OutputLUT->GetResource();
	if (!Resource)
	{
		UE_LOG(LogAIGrader, Warning, TEXT("LUT Resource 为空，跳过纹理更新"));
		return;
	}

	const int32 D = OUTPUT_LUT_DIM;
	const int32 NumTexels = STRIP_WIDTH * STRIP_HEIGHT;
	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(NumTexels);

	const float InvD = 1.0f / (D - 1);

	for (int32 b = 0; b < D; b++)
	{
		for (int32 g = 0; g < D; g++)
		{
			for (int32 r = 0; r < D; r++)
			{
				const float rf = r * InvD;
				const float gf = g * InvD;
				const float bf = b * InvD;

				const float outR = FMath::Clamp(SampleLUT33(0, rf, gf, bf), 0.0f, 1.0f);
				const float outG = FMath::Clamp(SampleLUT33(1, rf, gf, bf), 0.0f, 1.0f);
				const float outB = FMath::Clamp(SampleLUT33(2, rf, gf, bf), 0.0f, 1.0f);

				const int32 X = r + b * D;
				const int32 Y = g;
				Pixels[Y * STRIP_WIDTH + X] = FColor(
					FMath::RoundToInt32(outR * 255.0f),
					FMath::RoundToInt32(outG * 255.0f),
					FMath::RoundToInt32(outB * 255.0f),
					255);
			}
		}
	}

	// RHI in-place update — no resource recreation, no stall
	ENQUEUE_RENDER_COMMAND(UpdateAIGradingLUT)(
		[Resource, Data = MoveTemp(Pixels)](FRHICommandListImmediate& RHICmdList)
		{
			if (!Resource || !Resource->TextureRHI) return;
			FUpdateTextureRegion2D Region(0, 0, 0, 0, 256, 16);
			RHICmdList.UpdateTexture2D(
				Resource->TextureRHI,
				0,
				Region,
				256 * sizeof(FColor),
				reinterpret_cast<const uint8*>(Data.GetData()));
		});
}

void FAIColorGrader::WriteIdentityLUT()
{
	const int32 D = MODEL_LUT_DIM;
	const float Inv = 1.0f / (D - 1);

	for (int32 r = 0; r < D; r++)
		for (int32 g = 0; g < D; g++)
			for (int32 b = 0; b < D; b++)
			{
				SmoothedLUT[0 * D*D*D + r * D*D + g * D + b] = r * Inv;
				SmoothedLUT[1 * D*D*D + r * D*D + g * D + b] = g * Inv;
				SmoothedLUT[2 * D*D*D + r * D*D + g * D + b] = b * Inv;
			}
	bFirstLUT = true;
	UpdateLUTTexture();
}

// ============================================================
// 控制接口
// ============================================================

void FAIColorGrader::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (ViewExtension.IsValid())
	{
		ViewExtension->SetEnabled(bInEnabled);
	}

	UE_LOG(LogAIGrader, Log, TEXT("AI 调色 %s (model=%s, resource=%s)"),
		bInEnabled ? TEXT("启用") : TEXT("禁用"),
		bModelLoaded ? TEXT("OK") : TEXT("N/A"),
		(OutputLUT && OutputLUT->GetResource()) ? TEXT("OK") : TEXT("N/A"));

	if (bInEnabled && bModelLoaded)
	{
		InferOnce();
		TimeSinceLastInference = 0.0f;
	}
}

void FAIColorGrader::SetInferenceInterval(float InSeconds)
{
	InferenceInterval = FMath::Max(0.05f, InSeconds);
}

void FAIColorGrader::SetIntensity(float InIntensity)
{
	Intensity = FMath::Clamp(InIntensity, 0.0f, 1.0f);
	if (ViewExtension.IsValid())
	{
		ViewExtension->SetIntensity(Intensity);
	}
}

void FAIColorGrader::SetSmoothingFactor(float Alpha)
{
	SmoothingAlpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
}

// ============================================================
// 推理管线
// ============================================================

TArray<float> FAIColorGrader::PreprocessFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	const int32 ChannelSize = Height * Width;
	TArray<float> Tensor;
	Tensor.SetNumUninitialized(1 * 3 * ChannelSize);

	const float Inv = 1.0f / 255.0f;
	for (int32 i = 0; i < ChannelSize; i++)
	{
		const FColor& Px = Pixels[i];
		Tensor[0 * ChannelSize + i] = Px.R * Inv;
		Tensor[1 * ChannelSize + i] = Px.G * Inv;
		Tensor[2 * ChannelSize + i] = Px.B * Inv;
	}
	return Tensor;
}

bool FAIColorGrader::RunNNEInference(const TArray<float>& InputTensor, int32 Height, int32 Width, TArray<float>& OutLUT)
{
	if (!NNEInstance.IsValid()) return false;

	TArray<uint32> Dims = { 1, 3, static_cast<uint32>(Height), static_cast<uint32>(Width) };
	TArray<UE::NNE::FTensorShape> InputShapes;
	InputShapes.Add(UE::NNE::FTensorShape::Make(Dims));

	if (NNEInstance->SetInputTensorShapes(InputShapes) != UE::NNE::EResultStatus::Ok)
	{
		UE_LOG(LogAIGrader, Error, TEXT("SetInputTensorShapes 失败"));
		return false;
	}

	OutLUT.SetNumUninitialized(MODEL_LUT_FLOATS);

	UE::NNE::FTensorBindingCPU InBind;
	InBind.Data = const_cast<float*>(InputTensor.GetData());
	InBind.SizeInBytes = InputTensor.Num() * sizeof(float);

	UE::NNE::FTensorBindingCPU OutBind;
	OutBind.Data = OutLUT.GetData();
	OutBind.SizeInBytes = OutLUT.Num() * sizeof(float);

	if (NNEInstance->RunSync(MakeArrayView(&InBind, 1), MakeArrayView(&OutBind, 1)) != UE::NNE::EResultStatus::Ok)
	{
		UE_LOG(LogAIGrader, Error, TEXT("RunSync 失败"));
		return false;
	}

	return true;
}

void FAIColorGrader::ApplyAndUpdateLUT(const TArray<float>& RawLUT)
{
	if (bFirstLUT)
	{
		FMemory::Memcpy(SmoothedLUT.GetData(), RawLUT.GetData(), MODEL_LUT_FLOATS * sizeof(float));
		bFirstLUT = false;
	}
	else
	{
		const float OneMinusAlpha = 1.0f - SmoothingAlpha;
		for (int32 i = 0; i < MODEL_LUT_FLOATS; i++)
		{
			SmoothedLUT[i] = OneMinusAlpha * SmoothedLUT[i] + SmoothingAlpha * RawLUT[i];
		}
	}

	UpdateLUTTexture();
}

static void DownsamplePixels(const TArray<FColor>& Src, int32 SrcW, int32 SrcH,
	TArray<FColor>& Dst, int32 DstW, int32 DstH)
{
	Dst.SetNumUninitialized(DstW * DstH);
	for (int32 y = 0; y < DstH; y++)
	{
		const int32 sy = y * SrcH / DstH;
		const int32 SrcRow = sy * SrcW;
		for (int32 x = 0; x < DstW; x++)
		{
			Dst[y * DstW + x] = Src[SrcRow + x * SrcW / DstW];
		}
	}
}

void FAIColorGrader::ProcessCapturedFrame(const FViewportCaptureResult& Capture)
{
	double T0 = FPlatformTime::Seconds();

	constexpr int32 AI_INPUT_SIZE = 256;
	TArray<FColor> SmallPixels;
	if (Capture.Width > AI_INPUT_SIZE * 2 || Capture.Height > AI_INPUT_SIZE * 2)
	{
		DownsamplePixels(Capture.Pixels, Capture.Width, Capture.Height,
			SmallPixels, AI_INPUT_SIZE, AI_INPUT_SIZE);
	}
	else
	{
		SmallPixels = Capture.Pixels;
	}

	const int32 InputW = (SmallPixels.Num() == AI_INPUT_SIZE * AI_INPUT_SIZE) ? AI_INPUT_SIZE : Capture.Width;
	const int32 InputH = (SmallPixels.Num() == AI_INPUT_SIZE * AI_INPUT_SIZE) ? AI_INPUT_SIZE : Capture.Height;

	double T1 = FPlatformTime::Seconds();

	TArray<float> InputTensor = PreprocessFrame(SmallPixels, InputW, InputH);

	double T2 = FPlatformTime::Seconds();

	TArray<float> RawLUT;
	if (!RunNNEInference(InputTensor, InputH, InputW, RawLUT))
	{
		return;
	}

	double T3 = FPlatformTime::Seconds();

	ApplyAndUpdateLUT(RawLUT);

	double T4 = FPlatformTime::Seconds();

	LastInferenceTimeMs = static_cast<float>((T4 - T0) * 1000.0);
	TotalInferenceCount++;

	UE_LOG(LogAIGrader, Log, TEXT("#%d 总%.1fms [%s] | 降采样%.1f 预处理%.1f 推理%.1f 应用%.1f | src=%dx%d"),
		TotalInferenceCount,
		LastInferenceTimeMs,
		bUsingGPU ? TEXT("GPU") : TEXT("CPU"),
		(T1 - T0) * 1000.0,
		(T2 - T1) * 1000.0,
		(T3 - T2) * 1000.0,
		(T4 - T3) * 1000.0,
		Capture.Width, Capture.Height);
}

void FAIColorGrader::InferOnce()
{
	if (!bModelLoaded || !bInitialized)
	{
		UE_LOG(LogAIGrader, Warning, TEXT("模型未加载"));
		return;
	}

	FViewportCaptureResult Capture = FrameCapture.CaptureCurrentFrame();
	if (!Capture.bIsValid)
	{
		UE_LOG(LogAIGrader, Warning, TEXT("视口捕获失败"));
		return;
	}

	ProcessCapturedFrame(Capture);
}

// ============================================================
// Tick — 异步流水线: ViewExtension 在渲染线程回读 → 游戏线程收集 + 推理
// ============================================================

bool FAIColorGrader::OnTick(float DeltaTime)
{
	if (!bEnabled || !bModelLoaded) return true;
	if (!ViewExtension.IsValid()) return true;

	if (ViewExtension->IsCaptureReady())
	{
		auto CaptureResult = ViewExtension->CollectCaptureResult();
		if (CaptureResult.bIsValid)
		{
			FViewportCaptureResult Capture;
			Capture.Pixels = MoveTemp(CaptureResult.Pixels);
			Capture.Width = CaptureResult.Width;
			Capture.Height = CaptureResult.Height;
			Capture.bIsValid = true;
			ProcessCapturedFrame(Capture);
		}
	}

	TimeSinceLastInference += DeltaTime;
	if (TimeSinceLastInference >= InferenceInterval && !ViewExtension->HasPendingCapture())
	{
		TimeSinceLastInference = 0.0f;
		ViewExtension->RequestFrameCapture();
	}

	return true;
}
