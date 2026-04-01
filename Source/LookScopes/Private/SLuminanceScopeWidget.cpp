// Copyright KuoYu. All Rights Reserved.

#include "SLuminanceScopeWidget.h"
#include "ScopeSessionManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Images/SImage.h"
#include "Rendering/DrawElements.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "SLuminanceScopeWidget"

// ============================================================
// SHistogramDisplay 实现
// ============================================================

void SHistogramDisplay::Construct(const FArguments& InArgs)
{
	ResultPtr = InArgs._ResultPtr;
}

FVector2D SHistogramDisplay::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(400.0f, 200.0f);
}

int32 SHistogramDisplay::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	// 绘制深色背景
	const FLinearColor BackgroundColor(0.02f, 0.02f, 0.02f, 0.95f);
	FSlateDrawElement::MakeBox(
		OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, BackgroundColor
	);
	LayerId++;

	const float TotalWidth = AllottedGeometry.GetLocalSize().X - Padding * 2;
	const float TotalHeight = AllottedGeometry.GetLocalSize().Y - Padding * 2;

	// 绘制边框
	{
		TArray<FVector2D> BorderPoints;
		BorderPoints.Add(FVector2D(Padding, Padding));
		BorderPoints.Add(FVector2D(Padding + TotalWidth, Padding));
		BorderPoints.Add(FVector2D(Padding + TotalWidth, Padding + TotalHeight));
		BorderPoints.Add(FVector2D(Padding, Padding + TotalHeight));
		BorderPoints.Add(FVector2D(Padding, Padding));

		FSlateDrawElement::MakeLines(
			OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			BorderPoints, ESlateDrawEffect::None,
			FLinearColor(0.3f, 0.3f, 0.3f, 1.0f), true, 1.0f
		);
	}
	LayerId++;

	if (!ResultPtr || !ResultPtr->bIsValid || ResultPtr->MaxBinValue == 0)
	{
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 12);
		FSlateDrawElement::MakeText(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(
				FVector2D(TotalWidth, 20.0f),
				FSlateLayoutTransform(FVector2D(Padding, Padding + TotalHeight * 0.5f - 10.0f))
			),
			TEXT("点击「实时分析」或「单次分析」开始"),
			Font, ESlateDrawEffect::None,
			FLinearColor(0.5f, 0.5f, 0.5f, 1.0f)
		);
		return LayerId;
	}

	// 绘制直方图柱状条
	const float BarWidth = TotalWidth / 256.0f;
	const float StartX = Padding;
	const float BottomY = Padding + TotalHeight;
	const float InvMaxBin = 1.0f / (float)ResultPtr->MaxBinValue;

	for (int32 i = 0; i < 256; ++i)
	{
		float NormalizedHeight = (float)ResultPtr->HistogramBins[i] * InvMaxBin;
		float BarHeight = NormalizedHeight * TotalHeight;

		if (BarHeight < 0.5f) continue;

		FVector2D BarPos(StartX + i * BarWidth, BottomY - BarHeight);
		FVector2D BarSize(FMath::Max(BarWidth - 0.5f, 1.0f), BarHeight);

		FSlateDrawElement::MakeBox(
			OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(BarSize, FSlateLayoutTransform(BarPos)),
			FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None,
			GetBinColor(i)
		);
	}
	LayerId++;

	// 绘制暗部/中间调/亮部分界线
	const float Divider1X = StartX + (86.0f / 256.0f) * TotalWidth;
	const float Divider2X = StartX + (171.0f / 256.0f) * TotalWidth;

	TArray<FVector2D> DividerLine1;
	DividerLine1.Add(FVector2D(Divider1X, Padding));
	DividerLine1.Add(FVector2D(Divider1X, BottomY));

	TArray<FVector2D> DividerLine2;
	DividerLine2.Add(FVector2D(Divider2X, Padding));
	DividerLine2.Add(FVector2D(Divider2X, BottomY));

	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		DividerLine1, ESlateDrawEffect::None, FLinearColor(0.5f, 0.5f, 0.5f, 0.4f), true, 1.0f);
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
		DividerLine2, ESlateDrawEffect::None, FLinearColor(0.5f, 0.5f, 0.5f, 0.4f), true, 1.0f);

	return LayerId;
}

FLinearColor SHistogramDisplay::GetBinColor(int32 BinIndex) const
{
	if (BinIndex <= 85)
	{
		float T = (float)BinIndex / 85.0f;
		return FLinearColor(0.1f + T * 0.1f, 0.2f + T * 0.2f, 0.6f + T * 0.2f, 0.85f);
	}
	else if (BinIndex <= 170)
	{
		float T = (float)(BinIndex - 86) / 84.0f;
		return FLinearColor(0.1f + T * 0.2f, 0.5f + T * 0.3f, 0.2f + T * 0.1f, 0.85f);
	}
	else
	{
		float T = (float)(BinIndex - 171) / 84.0f;
		return FLinearColor(0.8f + T * 0.2f, 0.6f + T * 0.2f, 0.1f, 0.85f);
	}
}

// ============================================================
// SLuminanceScopeWidget 构造 / 析构
// ============================================================

void SLuminanceScopeWidget::Construct(const FArguments& InArgs)
{
	SessionManagerWeak = InArgs._SessionManager;

	// 初始化空结果
	CurrentResult.HistogramBins.SetNumZeroed(256);
	CurrentResult.bIsValid = false;

	// 订阅 SessionManager 的分析完成委托
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid())
	{
		AnalysisCompleteDelegateHandle = SM->OnAnalysisComplete.AddRaw(
			this, &SLuminanceScopeWidget::OnAnalysisComplete);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// === 工具栏 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			BuildToolbar()
		]

		// === 分隔线 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]

		// === 直方图区域 ===
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f)
		[
			SNew(SBox)
			.MinDesiredHeight(HistogramHeight)
			[
				SAssignNew(HistogramDisplay, SHistogramDisplay)
				.ResultPtr(&CurrentResult)
			]
		]

		// === 分隔线 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]

		// === 统计信息区域 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			BuildStatsArea()
		]
	];
}

SLuminanceScopeWidget::~SLuminanceScopeWidget()
{
	// 注销委托
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid())
	{
		SM->OnAnalysisComplete.Remove(AnalysisCompleteDelegateHandle);

		// 确保停止实时模式
		if (SM->IsRealtime())
		{
			SM->StopRealtime();
		}
	}
}

// ============================================================
// 委托回调
// ============================================================

void SLuminanceScopeWidget::OnAnalysisComplete(FName AnalyzerName, TSharedPtr<FScopeAnalysisResultBase> Result)
{
	// 只处理直方图分析器的结果
	if (AnalyzerName != FName(TEXT("Histogram")))
	{
		return;
	}

	TSharedPtr<FHistogramResult> HistResult = StaticCastSharedPtr<FHistogramResult>(Result);
	if (HistResult.IsValid())
	{
		CurrentResult = *HistResult;
		RefreshStatsText();
		Invalidate(EInvalidateWidgetReason::Paint);
	}
}

// ============================================================
// 公开接口
// ============================================================

void SLuminanceScopeWidget::TriggerAnalysis()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid() && !SM->IsAnalyzing())
	{
		SM->AnalyzeOnce();
	}
}

void SLuminanceScopeWidget::StartRealtime()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid() && !SM->IsRealtime())
	{
		SM->StartRealtime();
	}
}

void SLuminanceScopeWidget::StopRealtime()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid())
	{
		SM->StopRealtime();
	}
}

bool SLuminanceScopeWidget::IsRealtime() const
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	return SM.IsValid() && SM->IsRealtime();
}

// ============================================================
// UI 构建
// ============================================================

TSharedRef<SWidget> SLuminanceScopeWidget::BuildToolbar()
{
	return SNew(SHorizontalBox)

		// 实时分析 开关按钮
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(6.0f, 3.0f))
			.OnClicked_Lambda([this]()
			{
				if (IsRealtime())
				{
					StopRealtime();
				}
				else
				{
					StartRealtime();
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					// 状态圆点指示器
					SAssignNew(StatusText, STextBlock)
					.Text(this, &SLuminanceScopeWidget::GetStatusText)
					.ColorAndOpacity(this, &SLuminanceScopeWidget::GetStatusColor)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &SLuminanceScopeWidget::GetRealtimeButtonText)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				]
			]
		]

		// 单次分析按钮
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(2.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(6.0f, 3.0f))
			.ToolTipText(LOCTEXT("AnalyzeOnceTooltip", "捕获当前帧并分析一次 (F8)"))
			.IsEnabled_Lambda([this]() { return !IsRealtime(); })
			.OnClicked_Lambda([this]()
			{
				TriggerAnalysis();
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AnalyzeOnce", "📸 单次分析"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			]
		]

		// 弹性空间
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SSpacer)
		]

		// 刷新间隔设置
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("IntervalLabel", "间隔:"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(60.0f)
				[
					SNew(SSpinBox<float>)
					.MinValue(0.05f)
					.MaxValue(2.0f)
					.Value(0.2f)
					.Delta(0.05f)
					.MinDesiredWidth(50.0f)
					.OnValueChanged_Lambda([this](float NewValue)
					{
						TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
						if (SM.IsValid())
						{
							SM->SetRealtimeInterval(NewValue);
						}
					})
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("IntervalUnit", "秒"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
			]
		];
}

TSharedRef<SWidget> SLuminanceScopeWidget::BuildStatsArea()
{
	return SNew(SVerticalBox)

		// 第一行：暗部 / 中间调 / 亮部
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)

			// 暗部
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ShadowLabel", "暗部 (Shadows)"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.5f, 0.9f)))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(ShadowText, STextBlock)
					.Text(LOCTEXT("ShadowDefault", "--"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.6f, 1.0f)))
				]
			]

			// 中间调
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("MidtoneLabel", "中间调 (Midtones)"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.8f, 0.3f)))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(MidtoneText, STextBlock)
					.Text(LOCTEXT("MidtoneDefault", "--"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.9f, 0.4f)))
				]
			]

			// 亮部
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("HighlightLabel", "亮部 (Highlights)"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.8f, 0.2f)))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(HighlightText, STextBlock)
					.Text(LOCTEXT("HighlightDefault", "--"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
					.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.9f, 0.3f)))
				]
			]
		]

		// 第二行：详细信息
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SAssignNew(DetailText, STextBlock)
			.Text(LOCTEXT("DetailDefault", "等待分析..."))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		];
}

// ============================================================
// UI 更新
// ============================================================

void SLuminanceScopeWidget::RefreshStatsText()
{
	if (!CurrentResult.bIsValid)
	{
		return;
	}

	if (ShadowText.IsValid())
	{
		ShadowText->SetText(FText::FromString(
			FString::Printf(TEXT("%.1f%%"), CurrentResult.ShadowRatio * 100.0f)));
	}

	if (MidtoneText.IsValid())
	{
		MidtoneText->SetText(FText::FromString(
			FString::Printf(TEXT("%.1f%%"), CurrentResult.MidtoneRatio * 100.0f)));
	}

	if (HighlightText.IsValid())
	{
		HighlightText->SetText(FText::FromString(
			FString::Printf(TEXT("%.1f%%"), CurrentResult.HighlightRatio * 100.0f)));
	}

	if (DetailText.IsValid())
	{
		FString Info = FString::Printf(
			TEXT("平均亮度: %.3f  |  中位数: %.3f  |  像素: %s"),
			CurrentResult.AverageLuminance,
			CurrentResult.MedianLuminance,
			*FString::FormatAsNumber(CurrentResult.TotalPixels));
		DetailText->SetText(FText::FromString(Info));
	}
}

FText SLuminanceScopeWidget::GetRealtimeButtonText() const
{
	if (IsRealtime())
	{
		return LOCTEXT("StopRealtime", "⏹ 停止实时");
	}
	return LOCTEXT("StartRealtime", "▶ 实时分析");
}

FText SLuminanceScopeWidget::GetStatusText() const
{
	if (IsRealtime())
	{
		return FText::FromString(TEXT("●"));
	}
	return FText::FromString(TEXT("○"));
}

FSlateColor SLuminanceScopeWidget::GetStatusColor() const
{
	if (IsRealtime())
	{
		return FSlateColor(FLinearColor(0.2f, 0.9f, 0.2f)); // 绿色 = 运行中
	}
	return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)); // 灰色 = 停止
}

#undef LOCTEXT_NAMESPACE