// Copyright KuoYu. All Rights Reserved.

#include "ScopeAnalyzer.h"

// ============================================================
// FHistogramResult
// ============================================================

void FHistogramResult::ComputeStatistics()
{
	if (HistogramBins.Num() != 256 || TotalPixels == 0)
	{
		bIsValid = false;
		return;
	}

	// 暗部: Bin [0, 85]   -> 亮度 [0, 0.33]
	// 中间: Bin [86, 170]  -> 亮度 [0.33, 0.67]
	// 亮部: Bin [171, 255] -> 亮度 [0.67, 1.0]
	uint32 ShadowCount = 0;
	uint32 MidtoneCount = 0;
	uint32 HighlightCount = 0;

	double WeightedSum = 0.0;
	MaxBinValue = 0;

	for (int32 i = 0; i < 256; ++i)
	{
		uint32 BinVal = HistogramBins[i];

		if (i <= 85)
		{
			ShadowCount += BinVal;
		}
		else if (i <= 170)
		{
			MidtoneCount += BinVal;
		}
		else
		{
			HighlightCount += BinVal;
		}

		double BinLuminance = (double)i / 255.0;
		WeightedSum += BinLuminance * (double)BinVal;

		MaxBinValue = FMath::Max(MaxBinValue, BinVal);
	}

	float InvTotal = 1.0f / (float)TotalPixels;
	ShadowRatio = (float)ShadowCount * InvTotal;
	MidtoneRatio = (float)MidtoneCount * InvTotal;
	HighlightRatio = (float)HighlightCount * InvTotal;
	AverageLuminance = (float)(WeightedSum / (double)TotalPixels);

	// 计算中位数亮度
	uint32 HalfTotal = TotalPixels / 2;
	uint32 Accumulated = 0;
	MedianLuminance = 0.0f;
	for (int32 i = 0; i < 256; ++i)
	{
		Accumulated += HistogramBins[i];
		if (Accumulated >= HalfTotal)
		{
			MedianLuminance = (float)i / 255.0f;
			break;
		}
	}

	bIsValid = true;
}

// ============================================================
// FHistogramAnalyzer
// ============================================================

TSharedPtr<FScopeAnalysisResultBase> FHistogramAnalyzer::Analyze(const FViewportCaptureResult& CaptureData)
{
	TSharedPtr<FHistogramResult> Result = MakeShared<FHistogramResult>();
	Result->HistogramBins.SetNumZeroed(256);
	Result->TotalPixels = CaptureData.GetTotalPixels();

	if (!CaptureData.bIsValid || CaptureData.Pixels.Num() == 0)
	{
		Result->bIsValid = false;
		return Result;
	}

	// ITU-R BT.709 亮度权重
	const float WeightR = 0.2126f;
	const float WeightG = 0.7152f;
	const float WeightB = 0.0722f;
	const float Inv255 = 1.0f / 255.0f;

	// 遍历所有像素，计算亮度并累加到对应 bin
	for (const FColor& Pixel : CaptureData.Pixels)
	{
		float R = (float)Pixel.R * Inv255;
		float G = (float)Pixel.G * Inv255;
		float B = (float)Pixel.B * Inv255;

		float Luminance = R * WeightR + G * WeightG + B * WeightB;

		int32 BinIndex = FMath::Clamp((int32)(Luminance * 255.0f), 0, 255);
		Result->HistogramBins[BinIndex]++;
	}

	Result->ComputeStatistics();

UE_LOG(LogTemp, Verbose, TEXT("LookScopes [Histogram]: 暗部:%.1f%% 中间调:%.1f%% 亮部:%.1f%% 平均亮度:%.3f"),
		Result->ShadowRatio * 100.0f,
		Result->MidtoneRatio * 100.0f,
		Result->HighlightRatio * 100.0f,
		Result->AverageLuminance);

	return Result;
}

// ============================================================
// FWaveformAnalyzer
// ============================================================

void FWaveformAnalyzer::SetOutputResolution(int32 InWidth, int32 InHeight)
{
	OutputWidth = FMath::Clamp(InWidth, 64, 2048);
	OutputHeight = FMath::Clamp(InHeight, 64, 1024);
}

TSharedPtr<FScopeAnalysisResultBase> FWaveformAnalyzer::Analyze(const FViewportCaptureResult& CaptureData)
{
	TSharedPtr<FWaveformResult> Result = MakeShared<FWaveformResult>();
	Result->DensityWidth = OutputWidth;
	Result->DensityHeight = OutputHeight;
	Result->SourceWidth = CaptureData.Width;
	Result->SourceHeight = CaptureData.Height;

	if (!CaptureData.bIsValid || CaptureData.Pixels.Num() == 0)
	{
		Result->bIsValid = false;
		return Result;
	}

	// 初始化密度图
	const int32 MapSize = OutputWidth * OutputHeight;
	Result->DensityMap.SetNumZeroed(MapSize);

	// ITU-R BT.709 亮度权重
	const float WeightR = 0.2126f;
	const float WeightG = 0.7152f;
	const float WeightB = 0.0722f;
	const float Inv255 = 1.0f / 255.0f;

	// 水平方向的映射比例：源图像列 → 密度图列
	const float XScale = (float)OutputWidth / (float)CaptureData.Width;

	// 遍历所有像素
	for (int32 PixelY = 0; PixelY < CaptureData.Height; ++PixelY)
	{
		for (int32 PixelX = 0; PixelX < CaptureData.Width; ++PixelX)
		{
			const FColor& Pixel = CaptureData.Pixels[PixelY * CaptureData.Width + PixelX];

			// 计算 BT.709 亮度
			float R = (float)Pixel.R * Inv255;
			float G = (float)Pixel.G * Inv255;
			float B = (float)Pixel.B * Inv255;
			float Luminance = R * WeightR + G * WeightG + B * WeightB;

			// 映射到密度图坐标
			int32 DensityX = FMath::Clamp((int32)(PixelX * XScale), 0, OutputWidth - 1);
			// 亮度 0 在底部（DensityHeight-1），亮度 1 在顶部（0）
			int32 DensityY = FMath::Clamp(OutputHeight - 1 - (int32)(Luminance * (OutputHeight - 1)), 0, OutputHeight - 1);

			Result->DensityMap[DensityY * OutputWidth + DensityX]++;
		}
	}

	// 计算最大密度值（用于归一化）
	uint32 MaxDensity = 0;
	for (int32 i = 0; i < MapSize; ++i)
	{
		MaxDensity = FMath::Max(MaxDensity, Result->DensityMap[i]);
	}
	Result->MaxDensity = MaxDensity;
	Result->bIsValid = true;

UE_LOG(LogTemp, Verbose, TEXT("LookScopes [Waveform]: 密度图 %dx%d, 最大密度 %d"),
		OutputWidth, OutputHeight, MaxDensity);

	return Result;
}
