// Copyright KuoYu. All Rights Reserved.

#include "SLookMatchPanel.h"
#include "SScopeTextureDisplay.h"
#include "ScopeSessionManager.h"
#include "GPUScopeRenderer.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/STextComboBox.h"
#include "LookScopesSubsystem.h"
#include "AIColorGrader.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "SLookMatchPanel"

// ============================================================
// 构造 / 析构
// ============================================================

void SLookMatchPanel::Construct(const FArguments& InArgs)
{
	SessionManagerWeak = InArgs._SessionManager;

	// 初始化分辨率预设
	auto AddPreset = [this](const FString& Label, FIntPoint Res)
	{
		ResolutionOptionStrings.Add(MakeShared<FString>(Label));
		ResolutionOptionValues.Add(Res);
	};
	AddPreset(TEXT("自动"), FIntPoint::ZeroValue);

	// 16:9
	AddPreset(TEXT("1280 x 720  (16:9)"),   FIntPoint(1280, 720));
	AddPreset(TEXT("1920 x 1080 (16:9)"),   FIntPoint(1920, 1080));
	AddPreset(TEXT("2560 x 1440 (16:9)"),   FIntPoint(2560, 1440));
	AddPreset(TEXT("3840 x 2160 (16:9)"),   FIntPoint(3840, 2160));

	// Cinematic
	AddPreset(TEXT("1920 x 804  (2.39:1)"), FIntPoint(1920, 804));
	AddPreset(TEXT("2560 x 1072 (2.39:1)"), FIntPoint(2560, 1072));
	AddPreset(TEXT("1920 x 818  (2.35:1)"), FIntPoint(1920, 818));
	AddPreset(TEXT("1920 x 1038 (1.85:1)"), FIntPoint(1920, 1038));
	AddPreset(TEXT("1920 x 960  (2:1)"),    FIntPoint(1920, 960));

	// 订阅 SessionManager 的分析完成委托
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid())
	{
		AnalysisCompleteDelegateHandle = SM->OnAnalysisComplete.AddRaw(
			this, &SLookMatchPanel::OnAnalysisComplete);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		// === 标题栏（可折叠） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					bMainContentVisible = !bMainContentVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredHeight(26.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return FText::FromString(bMainContentVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("PanelTitle", "视觉对标 & 示波器"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
							.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
						]
					]
				]
			]
		]

		// === 可折叠内容（工具栏 + 预览 + 示波器） ===
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bMainContentVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SVerticalBox)

				// 功能工具栏
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildToolbar()
				]

				// 分隔线
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SSeparator)
				]

				// 主体内容区（左右分栏）
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SHorizontalBox)

					// 左侧：预览区
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						BuildViewportPlaceholder()
					]

					// 分隔线
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SSeparator)
						.Orientation(Orient_Vertical)
					]

					// 右侧：示波器区
					+ SHorizontalBox::Slot()
					.FillWidth(0.67f)
					[
						BuildScopesArea()
					]
				]
			]
		]

		// === AI 调色区（含可折叠 section header） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			BuildAIGradingArea()
		]

		// === Custom Bloom 区（含可折叠 section header） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			BuildCustomBloomArea()
		]

		// === 底部画廊区（含可折叠 section header） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			BuildGalleryPlaceholder()
		]
	];

	// 如果 SessionManager 处于 GPU 模式，绑定 RenderTarget 到显示 Widget
	if (SM.IsValid() && SM->IsGPUMode())
	{
		BindGPURenderTargets();
	}
}

SLookMatchPanel::~SLookMatchPanel()
{
	// 注销委托
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid())
	{
		SM->OnAnalysisComplete.Remove(AnalysisCompleteDelegateHandle);

		if (SM->IsRealtime())
		{
			SM->StopRealtime();
		}
	}
}

// ============================================================
// 委托回调
// ============================================================

void SLookMatchPanel::OnAnalysisComplete(FName AnalyzerName, TSharedPtr<FScopeAnalysisResultBase> Result)
{
	// GPU 模式：RenderTarget 已经被 Compute Shader 直接写入，只需触发重绘
	if (AnalyzerName == FName(TEXT("GPU_InputPreview")))
	{
		if (InputPreviewDisplay.IsValid())
		{
			TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
			if (SM.IsValid())
			{
				// 使用 CPU 模式显示 InputPreview：
				// 直接将捕获的像素数据写入 DisplayTexture（通过 BulkData + UpdateResource）
				// 这条路径已被 BMP 调试代码验证可靠
				// 
				// 之前尝试的 GPU 模式（SetRenderTarget/SetTexture2D）均无法显示，
				// 根本原因是 UTexture2D::CreateTransient 创建的纹理通过 RHIUpdateTexture2D 更新后，
				// Slate 渲染器的 FSlateResourceHandle 缓存机制无法正确追踪动态纹理内容变化
				const FViewportCaptureResult& CaptureData = SM->GetLastCaptureData();
				if (CaptureData.bIsValid && CaptureData.Pixels.Num() > 0)
				{
					InputPreviewDisplay->UpdateFromRawPixels(
						CaptureData.Pixels, CaptureData.Width, CaptureData.Height);
				}
			}
		}
		return;
	}

	if (AnalyzerName == FName(TEXT("GPU_Waveform")) && WaveformDisplay.IsValid())
	{
		WaveformDisplay->MarkGPUTextureUpdated();
		return;
	}

	if (AnalyzerName == FName(TEXT("GPU_Histogram")) && HistogramDisplay.IsValid())
	{
		HistogramDisplay->MarkGPUTextureUpdated();
		return;
	}

	// CPU 模式：接收分析结果并写入纹理
	if (!Result.IsValid())
	{
		return;
	}

	// 波形图结果 → 更新波形图纹理
	if (AnalyzerName == FName(TEXT("Waveform")) && WaveformDisplay.IsValid())
	{
		TSharedPtr<FWaveformResult> WaveResult = StaticCastSharedPtr<FWaveformResult>(Result);
		if (WaveResult.IsValid())
		{
			WaveformDisplay->UpdateFromWaveform(*WaveResult);
		}
	}

	// 直方图结果 → 更新直方图纹理
	if (AnalyzerName == FName(TEXT("Histogram")) && HistogramDisplay.IsValid())
	{
		TSharedPtr<FHistogramResult> HistResult = StaticCastSharedPtr<FHistogramResult>(Result);
		if (HistResult.IsValid())
		{
			HistogramDisplay->UpdateFromHistogram(*HistResult);
		}
	}
}

// ============================================================
// GPU 绑定
// ============================================================

void SLookMatchPanel::BindGPURenderTargets()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (!SM.IsValid() || !SM->IsGPUMode())
	{
		return;
	}

	FGPUScopeRenderer& Renderer = SM->GetGPURenderer();

	// 绑定波形图 RenderTarget
	if (WaveformDisplay.IsValid() && Renderer.GetWaveformRT())
	{
		WaveformDisplay->SetRenderTarget(Renderer.GetWaveformRT());
	}

	// 绑定直方图 RenderTarget
	if (HistogramDisplay.IsValid() && Renderer.GetHistogramRT())
	{
		HistogramDisplay->SetRenderTarget(Renderer.GetHistogramRT());
	}

	// InputPreview 不在此处绑定：InputTexture 在首次 Render 时才创建
	// 绑定逻辑在 OnAnalysisComplete("GPU_InputPreview") 中处理

	UE_LOG(LogTemp, Log, TEXT("SLookMatchPanel: GPU RenderTarget 已绑定"));
}

// ============================================================
// 公开接口
// ============================================================

void SLookMatchPanel::TriggerAnalysis()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid() && !SM->IsAnalyzing())
	{
		SM->AnalyzeOnce();
	}
}

void SLookMatchPanel::StartRealtime()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid() && !SM->IsRealtime())
	{
		SM->StartRealtime();
	}
}

void SLookMatchPanel::StopRealtime()
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	if (SM.IsValid())
	{
		SM->StopRealtime();
	}
}

bool SLookMatchPanel::IsRealtime() const
{
	TSharedPtr<FScopeSessionManager> SM = SessionManagerWeak.Pin();
	return SM.IsValid() && SM->IsRealtime();
}

// ============================================================
// UI 构建：工具栏
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildToolbar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SHorizontalBox)

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
						SAssignNew(StatusText, STextBlock)
						.Text(this, &SLookMatchPanel::GetStatusText)
						.ColorAndOpacity(this, &SLookMatchPanel::GetStatusColor)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SLookMatchPanel::GetRealtimeButtonText)
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

			// NDI 分隔线
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f)
			[
				SNew(SSeparator)
				.Orientation(Orient_Vertical)
				.Thickness(1.0f)
			]

			// NDI 推流按钮
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(6.0f, 3.0f))
				.ToolTipText(LOCTEXT("NDIStreamTooltip", "开始/停止 NDI 推流到 DaVinci Resolve"))
				.OnClicked_Lambda([this]()
				{
					if (auto* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
					{
						if (Subsystem->IsNDIStreaming())
							Subsystem->StopNDIStream();
						else
							Subsystem->StartNDIStream();
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
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							if (auto* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								return Subsystem->IsNDIStreaming()
									? FText::FromString(TEXT("\x25CF"))
									: FText::FromString(TEXT("\x25CB"));
							return FText::FromString(TEXT("\x25CB"));
						})
						.ColorAndOpacity_Lambda([this]() -> FSlateColor
						{
							if (auto* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								return Subsystem->IsNDIStreaming()
									? FSlateColor(FLinearColor(0.9f, 0.3f, 0.2f))
									: FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
							return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() -> FText
						{
							if (auto* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								return Subsystem->IsNDIStreaming()
								? LOCTEXT("StopNDI", "NDI 停止")
								: LOCTEXT("StartNDI", "NDI 推流");
						return LOCTEXT("StartNDI", "NDI 推流");
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					]
				]
			]

			// 分辨率分隔线
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f)
			[
				SNew(SSeparator)
				.Orientation(Orient_Vertical)
				.Thickness(1.0f)
			]

			// 分辨率预设下拉框
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
					.Text(LOCTEXT("ResLabel", "分辨率:"))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride(130.0f)
					[
						SAssignNew(ResolutionComboBox, STextComboBox)
						.OptionsSource(&ResolutionOptionStrings)
						.InitiallySelectedItem(ResolutionOptionStrings[0])
						.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
						{
							if (!NewValue.IsValid()) return;
							int32 Idx = ResolutionOptionStrings.IndexOfByKey(NewValue);
							if (Idx != INDEX_NONE)
							{
								if (auto* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								{
									Subsystem->SetStreamResolution(ResolutionOptionValues[Idx]);
								}
							}
						})
					]
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
						.Value_Lambda([]() -> float
						{
							if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								return S->GetRealtimeInterval();
							return 0.2f;
						})
						.Delta(0.05f)
						.MinDesiredWidth(50.0f)
						.OnValueChanged_Lambda([](float NewValue)
						{
							if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								Sub->SetRealtimeInterval(NewValue);
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
			]

		];
}

// ============================================================
// UI 构建：左侧视口占位区
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildViewportPlaceholder()
{
	return SNew(SVerticalBox)

		// Section header: INPUT PREVIEW
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					bPreviewVisible = !bPreviewVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredHeight(26.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return FText::FromString(bPreviewVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("InputPreviewTitle", "输入预览"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
							.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("InputPreviewSource", "视口捕获"))
							.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							.ColorAndOpacity(FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f)))
						]
					]
				]
			]
		]

		// 输入图像预览纹理显示（可折叠）
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bPreviewVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Padding(4.0f)
			[
				SAssignNew(InputPreviewDisplay, SScopeTextureDisplay)
				.TextureWidth(256)
				.TextureHeight(256)
			]
		];
}

// ============================================================
// UI 构建：右侧示波器区域
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildScopesArea()
{
	return SNew(SVerticalBox)

		// Section header: SCOPES
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					bScopesVisible = !bScopesVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredHeight(26.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return FText::FromString(bScopesVisible ? TEXT("\x25BC") : TEXT("\x25C0"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ScopesHeader", "示波器"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
							.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
						]
					]
				]
			]
		]

		// Scopes 内容（可折叠）
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bScopesVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SVerticalBox)

				// 波形图
				+ SVerticalBox::Slot()
				.FillHeight(0.5f)
				.Padding(4.0f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.Padding(FMargin(8.0f, 3.0f))
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("WaveformTitle", "波形图 (亮度)"))
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
							]

							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SNew(SSpacer)
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("WaveformUnit", "IRE"))
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
							]
						]
					]

					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SAssignNew(WaveformDisplay, SScopeTextureDisplay)
						.TextureWidth(512)
						.TextureHeight(256)
					]
				]

				// 直方图
				+ SVerticalBox::Slot()
				.FillHeight(0.5f)
				.Padding(4.0f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.Padding(FMargin(8.0f, 3.0f))
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HistogramTitle", "直方图"))
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
							]

							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SNew(SSpacer)
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HistogramUnit", "亮度"))
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
							]
						]
					]

					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SAssignNew(HistogramDisplay, SScopeTextureDisplay)
						.TextureWidth(512)
						.TextureHeight(256)
					]
				]
			]
		];
}

// ============================================================
// UI 构建：AI 调色区域
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildAIGradingArea()
{
	return SNew(SVerticalBox)

		// Category header
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					bAIGradingVisible = !bAIGradingVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredHeight(26.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return FText::FromString(bAIGradingVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("AIGradingHeader", "AI 自动调色"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
							.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([]() -> FText
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								{
									if (FAIColorGrader* G = Sub->GetAIColorGrader())
									{
										if (G->IsEnabled() && G->IsModelLoaded())
										{
											return FText::Format(
												LOCTEXT("AIStatusRunning", "推理中 · {0} ms"),
												FText::AsNumber(FMath::RoundToInt(G->GetLastInferenceTimeMs())));
										}
										if (G->IsModelLoaded())
										{
											return LOCTEXT("AIStatusReady", "就绪");
										}
									}
								}
								return LOCTEXT("AIStatusOff", "未加载");
							})
							.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							.ColorAndOpacity_Lambda([]() -> FSlateColor
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								{
									if (Sub->IsAIGradingEnabled())
										return FSlateColor(FLinearColor(0.2f, 0.9f, 0.2f));
								}
								return FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f));
							})
						]
					]
				]
			]
		]

		// Content (collapsible)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bAIGradingVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.GridLine"))
				.Padding(FMargin(16.0f, 4.0f, 0.0f, 4.0f))
				[
					SNew(SVerticalBox)

					// 启用开关
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(80.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AIEnableLabel", "启用 AI 调色"))
								.ToolTipText(LOCTEXT("AIEnableTip", "开启/关闭 AI 自动调色，使用 ONNX 模型实时推理生成 LUT"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([]()
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									return Sub->IsAIGradingEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
								return ECheckBoxState::Unchecked;
							})
							.OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								{
									if (NewState == ECheckBoxState::Checked)
										Sub->EnableAIGrading();
									else
										Sub->DisableAIGrading();
								}
							})
							.Padding(0.0f)
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SSpacer)
						]

						// 单次推理按钮
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.ContentPadding(FMargin(6.0f, 3.0f))
							.ToolTipText(LOCTEXT("InferOnceTooltip", "捕获当前帧，推理一次"))
							.IsEnabled_Lambda([]()
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								{
									if (FAIColorGrader* G = Sub->GetAIColorGrader())
										return G->IsModelLoaded();
								}
								return false;
							})
							.OnClicked_Lambda([]()
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->TriggerAIInferOnce();
								return FReply::Handled();
							})
							[
								SNew(STextBlock)
								.Text(LOCTEXT("InferOnce", "单次推理"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]
					]

					// 分隔线
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f)
					[
						SNew(SSeparator)
					]

					// 强度
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(80.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AIIntensity", "强度"))
								.ToolTipText(LOCTEXT("AIIntensityTip", "AI 调色 LUT 的混合强度，0=不应用，1=完全应用"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(125.0f)
							[
								SNew(SNumericEntryBox<float>)
								.Value_Lambda([]() -> TOptional<float>
								{
									if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										return S->GetCachedAIIntensity();
									return 1.0f;
								})
								.OnValueChanged_Lambda([](float Val)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetAIGradingIntensity(Val);
								})
								.OnValueCommitted_Lambda([](float Val, ETextCommit::Type)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetAIGradingIntensity(Val);
								})
								.AllowSpin(true)
								.Delta(0.05f)
								.MinValue(0.0f)
								.MaxValue(1.0f)
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							]
						]
					]

					// 推理间隔
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(80.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AIInterval", "间隔(ms)"))
								.ToolTipText(LOCTEXT("AIIntervalTip", "AI 推理间隔（毫秒），值越小推理越频繁，GPU 负载越高"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(125.0f)
							[
								SNew(SNumericEntryBox<float>)
								.Value_Lambda([]() -> TOptional<float>
								{
									if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										return S->GetCachedAIInterval() * 1000.0f;
									return 100.0f;
								})
								.OnValueChanged_Lambda([](float Val)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetAIGradingInterval(Val / 1000.0f);
								})
								.OnValueCommitted_Lambda([](float Val, ETextCommit::Type)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetAIGradingInterval(Val / 1000.0f);
								})
								.AllowSpin(true)
								.Delta(10.0f)
								.MinValue(50.0f)
								.MaxValue(2000.0f)
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							]
						]
					]

					// 过渡时间
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(80.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AITransition", "过渡"))
								.ToolTipText(LOCTEXT("AITransitionTip", "LUT 切换的过渡时间（秒），值越大过渡越平滑"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(125.0f)
							[
								SNew(SNumericEntryBox<float>)
								.Value_Lambda([]() -> TOptional<float>
								{
									if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										return S->GetCachedAITransition();
									return 0.3f;
								})
								.OnValueChanged_Lambda([](float Val)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetAIGradingTransitionTime(Val);
								})
								.OnValueCommitted_Lambda([](float Val, ETextCommit::Type)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetAIGradingTransitionTime(Val);
								})
								.AllowSpin(true)
								.Delta(0.05f)
								.MinValue(0.05f)
								.MaxValue(3.0f)
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							]
						]
					]

					// 推理次数统计
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([]() -> FText
						{
							if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
							{
								if (FAIColorGrader* G = Sub->GetAIColorGrader())
								{
									return FText::Format(
										LOCTEXT("AIStats", "推理: {0} 次  |  最近耗时: {1} ms"),
										FText::AsNumber(G->GetTotalInferenceCount()),
										FText::AsNumber(FMath::RoundToInt(G->GetLastInferenceTimeMs())));
								}
							}
							return LOCTEXT("AIStatsNone", "—");
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
					]
				]
			]
		];
}

// ============================================================
// UI 构建：Custom Bloom 设置区域
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildCustomBloomArea()
{
	auto MakeParamRow = [](const FText& Label, const FText& Tooltip, float MaxVal, float Step,
		TFunction<void(float)> OnChanged, TFunction<float()> GetValue) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			.ToolTipText(Tooltip)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(80.0f)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(125.0f)
				[
					SNew(SNumericEntryBox<float>)
					.Value_Lambda([GetValue]() -> TOptional<float> { return GetValue(); })
					.OnValueChanged_Lambda([OnChanged](float Val) { OnChanged(Val); })
					.OnValueCommitted_Lambda([OnChanged](float Val, ETextCommit::Type) { OnChanged(Val); })
					.AllowSpin(true)
					.Delta(Step)
					.MinValue(0.0f)
					.MaxValue(MaxVal)
					.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
				]
			];
	};

	return SNew(SVerticalBox)

		// Category header
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					bBloomVisible = !bBloomVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredHeight(26.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return FText::FromString(bBloomVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("BloomHeader", "Custom Bloom"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
							.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
						]
					]
				]
			]
		]

		// Content (collapsible)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bBloomVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.GridLine"))
				.Padding(FMargin(16.0f, 4.0f, 0.0f, 4.0f))
				[
					SNew(SVerticalBox)

					// Enable toggle
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(80.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BloomEnableLabel", "启用 Bloom"))
								.ToolTipText(LOCTEXT("BloomEnableTip", "开启/关闭自定义双层 Bloom（场景 + VFX 半透明层独立控制）"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([]()
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									return Sub->IsCustomBloomEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
								return ECheckBoxState::Unchecked;
							})
							.OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetCustomBloomEnabled(NewState == ECheckBoxState::Checked);
							})
							.Padding(0.0f)
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f)
					[
						SNew(SSeparator)
					]

					// Scene Bloom Intensity
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeParamRow(
							LOCTEXT("SceneIntensity", "Scene 强度"),
							LOCTEXT("SceneIntensityTip", "场景 Bloom 的整体强度倍数，值越大辉光越明显"),
							2.0f, 0.05f,
							[](float Val) { if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>()) S->SetSceneBloomIntensity(Val); },
							[]() -> float { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().SceneBloomIntensity : 0.8f; })
					]

					// Scene Bloom Threshold
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeParamRow(
							LOCTEXT("SceneThreshold", "Scene 阈值"),
							LOCTEXT("SceneThresholdTip", "场景 HDR 亮度阈值，只有超过此亮度的像素才会产生 Bloom"),
							10.0f, 0.05f,
							[](float Val) { if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>()) S->SetSceneBloomThreshold(Val); },
							[]() -> float { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().SceneBloomThreshold : 1.0f; })
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f)
					[
						SNew(SSeparator)
					]

					// VFX Bloom Intensity
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeParamRow(
							LOCTEXT("VFXIntensity", "VFX 强度"),
							LOCTEXT("VFXIntensityTip", "半透明 VFX 层 Bloom 的强度倍数"),
							3.0f, 0.05f,
							[](float Val) { if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>()) S->SetVFXBloomIntensity(Val); },
							[]() -> float { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().VFXBloomIntensity : 1.0f; })
					]

					// VFX Bloom Threshold
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeParamRow(
							LOCTEXT("VFXThreshold", "VFX 阈值"),
							LOCTEXT("VFXThresholdTip", "半透明 VFX 层的 HDR 亮度阈值"),
							2.0f, 0.05f,
							[](float Val) { if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>()) S->SetVFXBloomThreshold(Val); },
							[]() -> float { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().VFXBloomThreshold : 0.2f; })
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f)
					[
						SNew(SSeparator)
					]

					// Bloom Levels
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						.ToolTipText(LOCTEXT("BloomLevelsTip", "Bloom 降采样层数 (3~6)，层数越多辉光范围越大，性能消耗越高"))

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox)
							.WidthOverride(80.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BloomLevels", "Levels"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(125.0f)
							[
								SNew(SNumericEntryBox<int32>)
								.Value_Lambda([]() -> TOptional<int32> { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().BloomLevels : 6; })
								.OnValueChanged_Lambda([](int32 Val)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetBloomLevels(FMath::Clamp(Val, 3, 6));
								})
								.OnValueCommitted_Lambda([](int32 Val, ETextCommit::Type)
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										Sub->SetBloomLevels(FMath::Clamp(Val, 3, 6));
								})
								.AllowSpin(true)
								.MinValue(3)
								.MaxValue(6)
								.Delta(1)
								.Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
							]
						]
					]

					// Scatter
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeParamRow(
							LOCTEXT("BloomScatter", "Scatter"),
							LOCTEXT("BloomScatterTip", "Bloom 扩散程度，0=集中，1=分散，值越大辉光越向外扩散"),
							1.0f, 0.05f,
							[](float Val) { if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>()) S->SetBloomScatter(Val); },
							[]() -> float { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().Scatter : 0.4f; })
					]

					// Max Brightness (anti-flicker)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeParamRow(
							LOCTEXT("MaxBrightness", "Max亮度"),
							LOCTEXT("MaxBrightnessTip", "HDR 亮度上限钳制，用于抑制高光闪烁，值越低抗闪烁越强"),
							50.0f, 0.5f,
							[](float Val) { if (auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>()) S->SetMaxBrightness(Val); },
							[]() -> float { auto* S = GEditor->GetEditorSubsystem<ULookScopesSubsystem>(); return S ? S->GetBloomParams().MaxBrightness : 10.0f; })
					]
				]
			]
		];
}

// ============================================================
// UI 构建：底部画廊占位区
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildGalleryPlaceholder()
{
	return SNew(SVerticalBox)

		// Section header: GALLERY
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("DetailsView.CategoryTop"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 0.0f))
				.OnClicked_Lambda([this]()
				{
					bGalleryVisible = !bGalleryVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SBox)
					.MinDesiredHeight(26.0f)
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() -> FText
							{
								return FText::FromString(bGalleryVisible ? TEXT("\x25BC") : TEXT("\x25B2"));
							})
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity(FSlateColor(FLinearColor::White))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("GalleryHeader", "参考画廊"))
							.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
							.TextStyle(FAppStyle::Get(), "DetailsView.CategoryTextStyle")
						]
					]
				]
			]
		]

		// Gallery 内容（可折叠）
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([this]()
			{
				return bGalleryVisible ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.HeightOverride(56.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("DetailsView.GridLine"))
				.Padding(FMargin(16.0f, 4.0f, 0.0f, 4.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(80.0f)
						.HeightOverride(40.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(FLinearColor(0.15f, 0.15f, 0.15f, 1.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("GalleryItem1", "Ref_01"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(80.0f)
						.HeightOverride(40.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor(FLinearColor(0.12f, 0.12f, 0.12f, 1.0f))
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("GalleryItem2", "Grab_001"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SSpacer)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("GalleryHint", "参考画廊 — 第三阶段"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f)))
					]
				]
			]
		];
}

// ============================================================
// UI 更新
// ============================================================

FText SLookMatchPanel::GetRealtimeButtonText() const
{
	if (IsRealtime())
	{
		return LOCTEXT("StopRealtime", "⏹ 停止实时");
	}
	return LOCTEXT("StartRealtime", "▶ 实时分析");
}

FText SLookMatchPanel::GetStatusText() const
{
	if (IsRealtime())
	{
		return FText::FromString(TEXT("●"));
	}
	return FText::FromString(TEXT("○"));
}

FSlateColor SLookMatchPanel::GetStatusColor() const
{
	if (IsRealtime())
	{
		return FSlateColor(FLinearColor(0.2f, 0.9f, 0.2f)); // 绿色 = 运行中
	}
	return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)); // 灰色 = 停止
}

#undef LOCTEXT_NAMESPACE
