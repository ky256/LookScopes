// Copyright KuoYu. All Rights Reserved.

#include "SScopeTextureDisplay.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "TextureResource.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "SScopeTextureDisplay"

// ============================================================
// 构造 / 析构
// ============================================================

void SScopeTextureDisplay::Construct(const FArguments& InArgs)
{
	TextureWidth = FMath::Max(InArgs._TextureWidth, 64);
	TextureHeight = FMath::Max(InArgs._TextureHeight, 64);

	InitializeTexture(TextureWidth, TextureHeight);

	// ===== 调试：从 BMP 文件加载测试纹理，验证捕获内容和显示链路 =====
	{
const FString BmpPath = FPaths::ProjectSavedDir() / TEXT("LookScopes");
		// 查找目录下最新的 BMP 文件
		TArray<FString> BmpFiles;
		IFileManager::Get().FindFiles(BmpFiles, *(BmpPath / TEXT("*.bmp")), true, false);

		if (BmpFiles.Num() > 0)
		{
			// 使用最后一个文件（最新的）
			const FString FullPath = BmpPath / BmpFiles.Last();
			TArray<uint8> FileData;
			if (FFileHelper::LoadFileToArray(FileData, *FullPath))
			{
				// 解析 BMP 文件头
				if (FileData.Num() > 54)
				{
					// BMP 文件头：偏移 18 = 宽度(4字节), 偏移 22 = 高度(4字节), 偏移 10 = 像素数据偏移(4字节)
					int32 BmpWidth = *(int32*)(FileData.GetData() + 18);
					int32 BmpHeight = *(int32*)(FileData.GetData() + 22);
					uint32 PixelOffset = *(uint32*)(FileData.GetData() + 10);

					UE_LOG(LogTemp, Warning, TEXT("SScopeTextureDisplay: 加载 BMP 文件: %s (%dx%d, 像素偏移=%d)"),
						*FullPath, BmpWidth, BmpHeight, PixelOffset);

					if (BmpWidth > 0 && BmpHeight > 0)
					{
						// 重建纹理以匹配 BMP 尺寸
						if (BmpWidth != TextureWidth || BmpHeight != TextureHeight)
						{
							if (DisplayTexture)
							{
								TextureBrush.SetResourceObject(nullptr);
								DisplayTexture->RemoveFromRoot();
								DisplayTexture = nullptr;
							}
							InitializeTexture(BmpWidth, BmpHeight);
						}

						const int32 NumPixels = TextureWidth * TextureHeight;
						PixelBuffer.SetNum(NumPixels);

						// BMP 像素数据是 BGR 格式，从底部到顶部存储
						// 每行可能有填充字节（对齐到 4 字节）
						const int32 RowStride = ((BmpWidth * 3 + 3) / 4) * 4;
						const uint8* PixelData = FileData.GetData() + PixelOffset;

						for (int32 y = 0; y < BmpHeight; ++y)
						{
							// BMP 是底部到顶部，翻转 Y
							const int32 SrcY = BmpHeight - 1 - y;
							const uint8* SrcRow = PixelData + SrcY * RowStride;

							for (int32 x = 0; x < BmpWidth; ++x)
							{
								uint8 B = SrcRow[x * 3 + 0];
								uint8 G = SrcRow[x * 3 + 1];
								uint8 R = SrcRow[x * 3 + 2];
								PixelBuffer[y * TextureWidth + x] = FColor(R, G, B, 255);
							}
						}

						bHasValidData = true;
						FlushPixelsToTexture();

						UE_LOG(LogTemp, Warning, TEXT("SScopeTextureDisplay: BMP 纹理已加载并显示 (%dx%d)"),
							TextureWidth, TextureHeight);
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SScopeTextureDisplay: 无法读取 BMP 文件: %s"), *FullPath);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SScopeTextureDisplay: 未找到 BMP 文件，目录: %s"), *BmpPath);
		}
	}
	// ===== 调试结束 =====
}

SScopeTextureDisplay::~SScopeTextureDisplay()
{
	// 清理画刷引用
	TextureBrush.SetResourceObject(nullptr);

	// GPU 模式不持有 RT 生命周期，只清引用
	BoundRenderTarget = nullptr;

	if (DisplayTexture && DisplayTexture->IsValidLowLevel())
	{
		DisplayTexture->RemoveFromRoot();
		DisplayTexture = nullptr;
	}
}

// ============================================================
// GPU 模式
// ============================================================

void SScopeTextureDisplay::SetRenderTarget(UTextureRenderTarget2D* InRT)
{
	if (!InRT)
	{
		// 切回 CPU 模式
		bGPUMode = false;
		BoundRenderTarget = nullptr;
		TextureBrush.SetResourceObject(DisplayTexture);
		return;
	}

	BoundRenderTarget = InRT;
	bGPUMode = true;

	// 更新内部尺寸用于宽高比计算
	TextureWidth = BoundRenderTarget->SizeX;
	TextureHeight = BoundRenderTarget->SizeY;

	// 将画刷指向 RenderTarget
	TextureBrush.SetResourceObject(BoundRenderTarget);
	TextureBrush.ImageSize = FVector2D(
		(float)BoundRenderTarget->SizeX,
		(float)BoundRenderTarget->SizeY);
	TextureBrush.DrawAs = ESlateBrushDrawType::Image;
	TextureBrush.Tiling = ESlateBrushTileType::NoTile;

	bHasValidData = true;

	UE_LOG(LogTemp, Log, TEXT("SScopeTextureDisplay: 切换到 GPU 模式 (RT: %dx%d)"),
		BoundRenderTarget->SizeX, BoundRenderTarget->SizeY);
}

void SScopeTextureDisplay::SetTexture2D(UTexture2D* InTexture)
{
	if (!InTexture)
	{
		// 切回 CPU 模式
		bGPUMode = false;
		BoundTexture2D = nullptr;
		TextureBrush.SetResourceObject(DisplayTexture);
		return;
	}

	BoundTexture2D = InTexture;
	BoundRenderTarget = nullptr; // 互斥：清除 RT 绑定
	bGPUMode = true;

	// 将画刷指向 UTexture2D
	TextureBrush.SetResourceObject(BoundTexture2D);
	TextureBrush.ImageSize = FVector2D(
		(float)BoundTexture2D->GetSizeX(),
		(float)BoundTexture2D->GetSizeY());
	TextureBrush.DrawAs = ESlateBrushDrawType::Image;
	TextureBrush.Tiling = ESlateBrushTileType::NoTile;

	// 更新内部尺寸用于宽高比计算
	TextureWidth = BoundTexture2D->GetSizeX();
	TextureHeight = BoundTexture2D->GetSizeY();

	bHasValidData = true;

	// 诊断日志：检查 RHI 资源状态
	FTextureResource* TexResource = BoundTexture2D->GetResource();
	UE_LOG(LogTemp, Log, TEXT("SScopeTextureDisplay: 切换到 Texture2D 模式 (%dx%d), Resource=%p, SRGB=%d, Format=PF_B8G8R8A8, CompressionSettings=%d"),
		TextureWidth, TextureHeight, TexResource,
		(int32)BoundTexture2D->SRGB, (int32)BoundTexture2D->CompressionSettings);
	if (TexResource)
	{
		FRHITexture* RHITex = TexResource->GetTexture2DRHI();
		UE_LOG(LogTemp, Log, TEXT("SScopeTextureDisplay: RHITexture=%p, ResourceSize=%dx%d"),
			RHITex, TexResource->GetSizeX(), TexResource->GetSizeY());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("SScopeTextureDisplay: Resource 为空！UpdateResource() 可能尚未完成"));
	}
}

void SScopeTextureDisplay::MarkGPUTextureUpdated()
{
	if (bGPUMode)
	{
		// 如果绑定的是 Texture2D，尺寸可能变化了（重建），需要更新画刷
		if (BoundTexture2D)
		{
			const int32 NewWidth = BoundTexture2D->GetSizeX();
			const int32 NewHeight = BoundTexture2D->GetSizeY();
			if (NewWidth != TextureWidth || NewHeight != TextureHeight)
			{
				TextureWidth = NewWidth;
				TextureHeight = NewHeight;
				TextureBrush.SetResourceObject(BoundTexture2D);
				TextureBrush.ImageSize = FVector2D((float)NewWidth, (float)NewHeight);
			}
		}

		bHasValidData = true;
		Invalidate(EInvalidateWidgetReason::Paint);
	}
}

// ============================================================
// 纹理管理
// ============================================================

void SScopeTextureDisplay::InitializeTexture(int32 InWidth, int32 InHeight, bool bSRGB)
{
	TextureWidth = InWidth;
	TextureHeight = InHeight;

	// 初始化像素缓冲区为黑色
	const int32 NumPixels = TextureWidth * TextureHeight;
	PixelBuffer.SetNumZeroed(NumPixels);

	// 创建 UTexture2D（动态纹理，可运行时更新）
	DisplayTexture = UTexture2D::CreateTransient(TextureWidth, TextureHeight, PF_B8G8R8A8);
	if (!DisplayTexture)
	{
		UE_LOG(LogTemp, Error, TEXT("SScopeTextureDisplay: 创建纹理失败"));
		return;
	}

	// 防止被 GC 回收
	DisplayTexture->AddToRoot();

	// 设置纹理属性
	// bSRGB=true：像素数据已经是 sRGB 编码（如 Viewport::ReadPixels），Slate 不会再做 Gamma 校正
	// bSRGB=false：像素数据是线性空间（如 CPU 绘制的波形图/直方图），适合精确控制颜色值
	DisplayTexture->Filter = bSRGB ? TF_Bilinear : TF_Nearest;
	DisplayTexture->SRGB = bSRGB;
	DisplayTexture->CompressionSettings = TC_VectorDisplacementmap; // 无压缩
	DisplayTexture->UpdateResource();

	// 配置 Slate 画刷
	TextureBrush.SetResourceObject(DisplayTexture);
	TextureBrush.ImageSize = FVector2D(TextureWidth, TextureHeight);
	TextureBrush.DrawAs = ESlateBrushDrawType::Image;
	TextureBrush.Tiling = ESlateBrushTileType::NoTile;

	// 初始推送黑色纹理
	FlushPixelsToTexture();
}

void SScopeTextureDisplay::FlushPixelsToTexture()
{
	if (!DisplayTexture || !DisplayTexture->GetPlatformData() ||
		DisplayTexture->GetPlatformData()->Mips.Num() == 0)
	{
		return;
	}

	// 锁定纹理 Mip 0，写入像素数据
	FTexture2DMipMap& Mip = DisplayTexture->GetPlatformData()->Mips[0];
	void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	if (TextureData)
	{
		const int32 DataSize = TextureWidth * TextureHeight * sizeof(FColor);
		FMemory::Memcpy(TextureData, PixelBuffer.GetData(), DataSize);
		Mip.BulkData.Unlock();
		DisplayTexture->UpdateResource();
	}

	bTextureDirty = false;
}

void SScopeTextureDisplay::ClearTexture()
{
	FMemory::Memzero(PixelBuffer.GetData(), PixelBuffer.Num() * sizeof(FColor));
	bHasValidData = false;
	FlushPixelsToTexture();
	Invalidate(EInvalidateWidgetReason::Paint);
}

// ============================================================
// 数据更新：波形图
// ============================================================

void SScopeTextureDisplay::UpdateFromWaveform(const FWaveformResult& Result)
{
	// GPU 模式下忽略 CPU 写入
	if (bGPUMode) return;

	if (!Result.bIsValid || Result.DensityMap.Num() == 0 || Result.MaxDensity == 0)
	{
		return;
	}

	// 确保纹理尺寸匹配
	if (Result.DensityWidth != TextureWidth || Result.DensityHeight != TextureHeight)
	{
		// 纹理尺寸不匹配，需要重建
		if (DisplayTexture)
		{
			TextureBrush.SetResourceObject(nullptr);
			DisplayTexture->RemoveFromRoot();
			DisplayTexture = nullptr;
		}
		InitializeTexture(Result.DensityWidth, Result.DensityHeight);
	}

	// 清空像素缓冲区
	FMemory::Memzero(PixelBuffer.GetData(), PixelBuffer.Num() * sizeof(FColor));

	// 绘制网格参考线
	DrawGridLines();

	// 将密度数据映射为荧光绿色散点
	// 使用对数映射增强低密度区域的可见性
	const float InvMaxDensity = 1.0f / (float)Result.MaxDensity;
	const float LogScale = 1.0f / FMath::Loge((float)Result.MaxDensity + 1.0f);

	for (int32 y = 0; y < Result.DensityHeight; ++y)
	{
		for (int32 x = 0; x < Result.DensityWidth; ++x)
		{
			const uint32 Density = Result.DensityMap[y * Result.DensityWidth + x];
			if (Density == 0)
			{
				continue;
			}

			// 对数映射：增强低密度区域的可见性
			float NormalizedDensity = FMath::Loge((float)Density + 1.0f) * LogScale;
			NormalizedDensity = FMath::Clamp(NormalizedDensity, 0.0f, 1.0f);

			// 荧光绿色调，密度越高越亮
			// 低密度：暗绿色，高密度：亮白绿色
			uint8 G = (uint8)(NormalizedDensity * 255.0f);
			uint8 R = (uint8)(NormalizedDensity * NormalizedDensity * 180.0f); // 高密度时偏白
			uint8 B = (uint8)(NormalizedDensity * NormalizedDensity * 100.0f);
			uint8 A = (uint8)(FMath::Max(NormalizedDensity * 255.0f, 20.0f)); // 最低透明度保证可见

			// 与网格线混合（additive）
			FColor& Pixel = PixelBuffer[y * TextureWidth + x];
			Pixel.R = FMath::Min(255, (int32)Pixel.R + (int32)R);
			Pixel.G = FMath::Min(255, (int32)Pixel.G + (int32)G);
			Pixel.B = FMath::Min(255, (int32)Pixel.B + (int32)B);
			Pixel.A = 255;
		}
	}

	bHasValidData = true;
	FlushPixelsToTexture();
	Invalidate(EInvalidateWidgetReason::Paint);
}

// ============================================================
// 数据更新：直方图
// ============================================================

void SScopeTextureDisplay::UpdateFromHistogram(const FHistogramResult& Result)
{
	// GPU 模式下忽略 CPU 写入
	if (bGPUMode) return;

	if (!Result.bIsValid || Result.MaxBinValue == 0)
	{
		return;
	}

	// 清空像素缓冲区
	FMemory::Memzero(PixelBuffer.GetData(), PixelBuffer.Num() * sizeof(FColor));

	// 绘制网格参考线
	DrawGridLines();

	// 将 256-bin 直方图绘制为柱状条
	const float BinWidth = (float)TextureWidth / 256.0f;
	const float InvMaxBin = 1.0f / (float)Result.MaxBinValue;

	for (int32 i = 0; i < 256; ++i)
	{
		float NormalizedHeight = (float)Result.HistogramBins[i] * InvMaxBin;
		int32 BarHeight = FMath::CeilToInt(NormalizedHeight * TextureHeight);

		if (BarHeight <= 0) continue;

		int32 StartX = FMath::FloorToInt(i * BinWidth);
		int32 EndX = FMath::Min(FMath::FloorToInt((i + 1) * BinWidth), TextureWidth);

		// 颜色：暗部蓝、中间调绿、亮部橙
		FColor BarColor;
		if (i <= 85)
		{
			float T = (float)i / 85.0f;
			BarColor = FColor(
				(uint8)(25 + T * 25),
				(uint8)(50 + T * 50),
				(uint8)(150 + T * 50),
				220);
		}
		else if (i <= 170)
		{
			float T = (float)(i - 86) / 84.0f;
			BarColor = FColor(
				(uint8)(25 + T * 50),
				(uint8)(130 + T * 75),
				(uint8)(50 + T * 25),
				220);
		}
		else
		{
			float T = (float)(i - 171) / 84.0f;
			BarColor = FColor(
				(uint8)(200 + T * 55),
				(uint8)(150 + T * 50),
				(uint8)(25),
				220);
		}

		// 从底部向上绘制柱状条
		for (int32 y = TextureHeight - 1; y >= TextureHeight - BarHeight && y >= 0; --y)
		{
			for (int32 x = StartX; x < EndX; ++x)
			{
				PixelBuffer[y * TextureWidth + x] = BarColor;
			}
		}
	}

	bHasValidData = true;
	FlushPixelsToTexture();
	Invalidate(EInvalidateWidgetReason::Paint);
}

// ============================================================
// 数据更新：原始像素（InputPreview 用）
// ============================================================

void SScopeTextureDisplay::UpdateFromRawPixels(const TArray<FColor>& Pixels, int32 Width, int32 Height)
{
	// GPU 模式下忽略 CPU 写入
	if (bGPUMode) return;

	if (Pixels.Num() == 0 || Width <= 0 || Height <= 0)
	{
		return;
	}

	// 确保纹理尺寸匹配（InputPreview 使用 sRGB 纹理）
	if (Width != TextureWidth || Height != TextureHeight)
	{
		if (DisplayTexture)
		{
			TextureBrush.SetResourceObject(nullptr);
			DisplayTexture->RemoveFromRoot();
			DisplayTexture = nullptr;
		}
		InitializeTexture(Width, Height, /*bSRGB=*/true);
	}
	// 首次调用时纹理可能是非 sRGB 的（Construct 中创建），需要重建
	else if (DisplayTexture && !DisplayTexture->SRGB)
	{
		TextureBrush.SetResourceObject(nullptr);
		DisplayTexture->RemoveFromRoot();
		DisplayTexture = nullptr;
		InitializeTexture(Width, Height, /*bSRGB=*/true);
	}

	// 直接拷贝像素数据
	const int32 NumPixels = Width * Height;
	PixelBuffer.SetNum(NumPixels);
	FMemory::Memcpy(PixelBuffer.GetData(), Pixels.GetData(), NumPixels * sizeof(FColor));

	// 关键修复：Viewport::ReadPixels() 返回的 Alpha 通道通常为 0，
	// 而 Slate 渲染器使用 Alpha 混合，Alpha=0 会导致纹理完全透明不可见。
	// 强制将所有像素的 Alpha 设为 255（完全不透明）。
	for (int32 i = 0; i < NumPixels; ++i)
	{
		PixelBuffer[i].A = 255;
	}

	bHasValidData = true;
	FlushPixelsToTexture();
	Invalidate(EInvalidateWidgetReason::Paint);
}

// ============================================================
// 网格参考线
// ============================================================

void SScopeTextureDisplay::DrawGridLines()
{
	const FColor GridColor(40, 40, 40, 255);

	// 水平参考线：25%, 50%, 75%
	const int32 Lines[] = { TextureHeight / 4, TextureHeight / 2, TextureHeight * 3 / 4 };
	for (int32 LineY : Lines)
	{
		if (LineY >= 0 && LineY < TextureHeight)
		{
			for (int32 x = 0; x < TextureWidth; ++x)
			{
				PixelBuffer[LineY * TextureWidth + x] = GridColor;
			}
		}
	}

	// 垂直参考线：25%, 50%, 75%
	const int32 VLines[] = { TextureWidth / 4, TextureWidth / 2, TextureWidth * 3 / 4 };
	for (int32 LineX : VLines)
	{
		if (LineX >= 0 && LineX < TextureWidth)
		{
			for (int32 y = 0; y < TextureHeight; ++y)
			{
				PixelBuffer[y * TextureWidth + LineX] = GridColor;
			}
		}
	}
}

// ============================================================
// SLeafWidget 接口
// ============================================================

FVector2D SScopeTextureDisplay::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D((float)TextureWidth, (float)TextureHeight);
}

int32 SScopeTextureDisplay::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();

	// 绘制深色背景（填满整个区域）
	const FLinearColor BackgroundColor(0.02f, 0.02f, 0.02f, 0.95f);
	FSlateDrawElement::MakeBox(
		OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, BackgroundColor
	);
	LayerId++;

	// 绘制纹理（保持宽高比，居中 letterbox/pillarbox）
	if (bHasValidData && (DisplayTexture || bGPUMode))
	{
	// 诊断日志：检查画刷资源状态（每个实例独立计数）
		{
			if (DiagLogCounter < 10) // 每个实例输出前10次
			{
				DiagLogCounter++;
				UObject* BrushResource = TextureBrush.GetResourceObject();
				FName BrushResName = TextureBrush.GetResourceName();
				UE_LOG(LogTemp, Warning, TEXT("SScopeTextureDisplay::OnPaint[%p] #%d - GPUMode=%d, ResourceObj=%p, ResourceName='%s', DrawAs=%d, HasUObject=%d"),
					this, DiagLogCounter, bGPUMode ? 1 : 0,
					BrushResource,
					*BrushResName.ToString(),
					(int32)TextureBrush.DrawAs,
					TextureBrush.HasUObject() ? 1 : 0);

				if (BrushResource)
				{
					UTexture* BrushTex = Cast<UTexture>(BrushResource);
					if (BrushTex)
					{
						FTextureResource* TexRes = BrushTex->GetResource();
						UE_LOG(LogTemp, Warning, TEXT("  BrushTex=%s, Resource=%p, HasRHI=%d"),
							*BrushTex->GetName(), TexRes,
							TexRes ? (TexRes->TextureRHI.IsValid() ? 1 : 0) : -1);
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("  BrushResource 为空！"));
				}

				// 检查 ResourceHandle 是否有效
				const FSlateResourceHandle& Handle = TextureBrush.GetRenderingResource();
				UE_LOG(LogTemp, Warning, TEXT("  ResourceHandle.IsValid=%d, Proxy=%p"),
					Handle.IsValid() ? 1 : 0,
					Handle.GetResourceProxy());
			}
		}
		// 计算保持宽高比的绘制区域
		const float TexAspect = (float)TextureWidth / FMath::Max((float)TextureHeight, 1.0f);
		const float WidgetAspect = LocalSize.X / FMath::Max(LocalSize.Y, 1.0f);

		FVector2D DrawSize;
		FVector2D DrawOffset;

		if (WidgetAspect > TexAspect)
		{
			// Widget 更宽 → pillarbox（左右留黑边）
			DrawSize.Y = LocalSize.Y;
			DrawSize.X = LocalSize.Y * TexAspect;
			DrawOffset.X = (LocalSize.X - DrawSize.X) * 0.5f;
			DrawOffset.Y = 0.0f;
		}
		else
		{
			// Widget 更高 → letterbox（上下留黑边）
			DrawSize.X = LocalSize.X;
			DrawSize.Y = LocalSize.X / TexAspect;
			DrawOffset.X = 0.0f;
			DrawOffset.Y = (LocalSize.Y - DrawSize.Y) * 0.5f;
		}

		FSlateDrawElement::MakeBox(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(DrawSize, FSlateLayoutTransform(DrawOffset)),
			&TextureBrush, ESlateDrawEffect::None, FLinearColor::White
		);
	}
	else
	{
		// 无数据时显示提示文字
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 11);
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(
				FVector2D(LocalSize.X, 20.0f),
				FSlateLayoutTransform(FVector2D(0.0f, LocalSize.Y * 0.5f - 10.0f))
			),
			LOCTEXT("NoData", "等待分析数据..."),
			Font, ESlateDrawEffect::None,
			FLinearColor(0.4f, 0.4f, 0.4f, 1.0f)
		);
	}
	LayerId++;

	// 绘制边框
	{
		TArray<FVector2D> BorderPoints;
		BorderPoints.Add(FVector2D(0, 0));
		BorderPoints.Add(FVector2D(LocalSize.X, 0));
		BorderPoints.Add(FVector2D(LocalSize.X, LocalSize.Y));
		BorderPoints.Add(FVector2D(0, LocalSize.Y));
		BorderPoints.Add(FVector2D(0, 0));

		FSlateDrawElement::MakeLines(
			OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			BorderPoints, ESlateDrawEffect::None,
			FLinearColor(0.15f, 0.15f, 0.15f, 0.8f), true, 1.0f
		);
	}

	return LayerId;
}

#undef LOCTEXT_NAMESPACE
