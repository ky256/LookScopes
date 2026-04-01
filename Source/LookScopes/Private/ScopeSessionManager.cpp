// Copyright KuoYu. All Rights Reserved.

#include "ScopeSessionManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

// ============================================================
// 构造 / 析构
// ============================================================

FScopeSessionManager::FScopeSessionManager()
{
	ViewportCapture = MakeUnique<FViewportCapture>();

	// 默认初始化 GPU 渲染器
GPURenderer.Initialize(1024, 512, 1024, 512);
	bUseGPU = GPURenderer.IsInitialized();

	if (bUseGPU)
	{
UE_LOG(LogTemp, Log, TEXT("LookScopes: GPU 渲染模式已启用"));
	}
	else
	{
UE_LOG(LogTemp, Warning, TEXT("LookScopes: GPU 渲染初始化失败，回退到 CPU 模式"));
	}
}

FScopeSessionManager::~FScopeSessionManager()
{
	StopRealtime();
	GPURenderer.Release();
	Analyzers.Empty();
	LastResults.Empty();
}

// ============================================================
// GPU 模式切换
// ============================================================

void FScopeSessionManager::SetGPUMode(bool bEnable)
{
	if (bEnable && !GPURenderer.IsInitialized())
	{
GPURenderer.Initialize(1024, 512, 1024, 512);
	}

	bUseGPU = bEnable && GPURenderer.IsInitialized();
UE_LOG(LogTemp, Log, TEXT("LookScopes: %s"), bUseGPU ? TEXT("GPU 模式") : TEXT("CPU 模式"));
}

// ============================================================
// 分析器注册
// ============================================================

void FScopeSessionManager::RegisterAnalyzer(TSharedPtr<IScopeAnalyzer> Analyzer)
{
	if (!Analyzer.IsValid())
	{
		return;
	}

	FName Name = Analyzer->GetAnalyzerName();

	// 避免重复注册
	for (const auto& Existing : Analyzers)
	{
		if (Existing->GetAnalyzerName() == Name)
		{
UE_LOG(LogTemp, Warning, TEXT("LookScopes: 分析器 '%s' 已注册，跳过重复注册"), *Name.ToString());
			return;
		}
	}

	Analyzers.Add(Analyzer);
UE_LOG(LogTemp, Log, TEXT("LookScopes: 注册分析器 '%s'"), *Name.ToString());
}

void FScopeSessionManager::UnregisterAnalyzer(FName AnalyzerName)
{
	Analyzers.RemoveAll([AnalyzerName](const TSharedPtr<IScopeAnalyzer>& A)
	{
		return A.IsValid() && A->GetAnalyzerName() == AnalyzerName;
	});

	LastResults.Remove(AnalyzerName);
UE_LOG(LogTemp, Log, TEXT("LookScopes: 注销分析器 '%s'"), *AnalyzerName.ToString());
}

// ============================================================
// 单次分析
// ============================================================

void FScopeSessionManager::AnalyzeOnce()
{
	if (bIsAnalyzing)
	{
UE_LOG(LogTemp, Warning, TEXT("LookScopes: 上一次分析尚未完成，请稍后再试"));
		return;
	}

	ExecuteAnalysisPipeline();
}

// ============================================================
// 分析流水线
// ============================================================

void FScopeSessionManager::ExecuteAnalysisPipeline()
{
	bIsAnalyzing = true;

	// 第一步：统一捕获一帧
	FViewportCaptureResult CaptureData = ViewportCapture->CaptureCurrentFrame();

	if (!CaptureData.bIsValid)
	{
UE_LOG(LogTemp, Error, TEXT("LookScopes: 视口捕获失败，分析中止"));
		bIsAnalyzing = false;
		return;
	}

	// ===== 调试：将捕获的像素数据保存为 BMP 文件到磁盘 =====
	{
		static int32 CaptureCount = 0;
		if (CaptureCount < 3) // 只保存前3次，避免磁盘写满
		{
FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LookScopes"));
			IFileManager::Get().MakeDirectory(*SaveDir, true);

			FString SavePath = FPaths::Combine(
				SaveDir,
				FString::Printf(TEXT("Capture_%d_%dx%d"), CaptureCount, CaptureData.Width, CaptureData.Height)
			);

			FFileHelper::CreateBitmap(
				*SavePath,
				CaptureData.Width,
				CaptureData.Height,
				CaptureData.Pixels.GetData()
			);

UE_LOG(LogTemp, Warning, TEXT("LookScopes: 捕获数据已保存到 %s.bmp (%d x %d, %d 像素)"),
				*SavePath, CaptureData.Width, CaptureData.Height, CaptureData.Pixels.Num());

			CaptureCount++;
		}
	}
	// ===== 调试结束 =====

	if (bUseGPU && GPURenderer.IsInitialized())
	{
		// 缓存捕获数据（供 InputPreview CPU 模式显示使用）
		LastCaptureData = CaptureData;

		// GPU 路径：Compute Shader 直接分析+渲染
		GPURenderer.Render(CaptureData);

		// 广播 GPU 渲染完成通知（UI 层通过此委托刷新显示）
		OnAnalysisComplete.Broadcast(FName(TEXT("GPU_InputPreview")), nullptr);
		OnAnalysisComplete.Broadcast(FName(TEXT("GPU_Waveform")), nullptr);
		OnAnalysisComplete.Broadcast(FName(TEXT("GPU_Histogram")), nullptr);
	}
	else
	{
		// CPU 路径（Fallback）：将同一帧数据分发给所有已注册的分析器
		for (const auto& Analyzer : Analyzers)
		{
			if (!Analyzer.IsValid())
			{
				continue;
			}

			TSharedPtr<FScopeAnalysisResultBase> Result = Analyzer->Analyze(CaptureData);

			FName AnalyzerName = Analyzer->GetAnalyzerName();

			// 缓存结果
			LastResults.Add(AnalyzerName, Result);

			// 广播通知所有订阅者
			OnAnalysisComplete.Broadcast(AnalyzerName, Result);
		}
	}

	bIsAnalyzing = false;
}

// ============================================================
// 结果缓存
// ============================================================

TSharedPtr<FScopeAnalysisResultBase> FScopeSessionManager::GetLastResult(FName AnalyzerName) const
{
	const TSharedPtr<FScopeAnalysisResultBase>* Found = LastResults.Find(AnalyzerName);
	return Found ? *Found : nullptr;
}

// ============================================================
// 实时模式
// ============================================================

void FScopeSessionManager::StartRealtime(float IntervalSeconds)
{
	if (bIsRealtime)
	{
		SetRealtimeInterval(IntervalSeconds);
		return;
	}

	RealtimeInterval = FMath::Max(IntervalSeconds, 0.05f);
	bIsRealtime = true;

	RealtimeTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FScopeSessionManager::OnRealtimeTick),
		RealtimeInterval
	);

UE_LOG(LogTemp, Log, TEXT("LookScopes: 实时分析已开启 (间隔: %.2f秒)"), RealtimeInterval);
}

void FScopeSessionManager::StopRealtime()
{
	if (!bIsRealtime)
	{
		return;
	}

	bIsRealtime = false;

	if (RealtimeTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RealtimeTickerHandle);
		RealtimeTickerHandle.Reset();
	}

UE_LOG(LogTemp, Log, TEXT("LookScopes: 实时分析已停止"));
}

void FScopeSessionManager::SetRealtimeInterval(float IntervalSeconds)
{
	RealtimeInterval = FMath::Max(IntervalSeconds, 0.05f);

	if (bIsRealtime)
	{
		if (RealtimeTickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(RealtimeTickerHandle);
		}

		RealtimeTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FScopeSessionManager::OnRealtimeTick),
			RealtimeInterval
		);
	}
}

bool FScopeSessionManager::OnRealtimeTick(float DeltaTime)
{
	if (!bIsRealtime)
	{
		return false;
	}

	if (bIsAnalyzing)
	{
		return true; // 上一次还没完成，跳过
	}

	ExecuteAnalysisPipeline();

	return true;
}
