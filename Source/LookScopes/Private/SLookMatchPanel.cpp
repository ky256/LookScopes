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
	AddPreset(TEXT("1280 x 720"), FIntPoint(1280, 720));
	AddPreset(TEXT("1920 x 1080"), FIntPoint(1920, 1080));
	AddPreset(TEXT("2560 x 1440"), FIntPoint(2560, 1440));
	AddPreset(TEXT("3840 x 2160"), FIntPoint(3840, 2160));

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

		// === 标题栏 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(FMargin(12.0f, 5.0f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PanelTitle", "视觉对标 & 示波器"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
			]
		]

		// === 功能工具栏 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildToolbar()
		]

		// === 分隔线 ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]

		// === 主体内容区（左右分栏） ===
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SHorizontalBox)

			// 左侧：预览区（含可折叠 section header）
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

			// 右侧：示波器区（含可折叠 section header）
			+ SHorizontalBox::Slot()
			.FillWidth(0.67f)
			[
				BuildScopesArea()
			]
		]

		// === AI 调色区（含可折叠 section header） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildAIGradingArea()
		]

		// === Custom Bloom 区（含可折叠 section header） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildCustomBloomArea()
		]

		// === 底部画廊区（含可折叠 section header） ===
		+ SVerticalBox::Slot()
		.AutoHeight()
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
			]

		];
}

// ============================================================
// UI 构建：左侧视口占位区
// ============================================================

TSharedRef<SWidget> SLookMatchPanel::BuildViewportPlaceholder()
{
	return SNew(SVerticalBox)

		// Section header: INPUT PREVIEW（可点击折叠）
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 5.0f))
				.OnClicked_Lambda([this]()
				{
					bPreviewVisible = !bPreviewVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("InputPreviewTitle", "输入预览"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("InputPreviewSource", "视口捕获"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f)))
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
						.Text_Lambda([this]() -> FText
						{
							return FText::FromString(bPreviewVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
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

		// Section header: SCOPES（可点击折叠）
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 5.0f))
				.OnClicked_Lambda([this]()
				{
					bScopesVisible = !bScopesVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ScopesHeader", "示波器"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
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
						.Text_Lambda([this]() -> FText
						{
							return FText::FromString(bScopesVisible ? TEXT("\x25BC") : TEXT("\x25C0"));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
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
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
						.Padding(FMargin(8.0f, 3.0f))
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("WaveformTitle", "波形图 (亮度)"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
							]

							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SNew(SSpacer)
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("WaveformUnit", "IRE"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
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
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
						.Padding(FMargin(8.0f, 3.0f))
						[
							SNew(SHorizontalBox)

							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HistogramTitle", "直方图"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
							]

							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SNew(SSpacer)
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HistogramUnit", "亮度"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
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

		// Section header
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 5.0f))
				.OnClicked_Lambda([this]()
				{
					bAIGradingVisible = !bAIGradingVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AIGradingHeader", "AI 自动调色"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
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
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
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
						.Text_Lambda([this]() -> FText
						{
							return FText::FromString(bAIGradingVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
					]
				]
			]
		]

		// 内容区（可折叠）
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
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(FMargin(12.0f, 6.0f))
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
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.ContentPadding(FMargin(6.0f, 3.0f))
							.OnClicked_Lambda([]()
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								{
									if (Sub->IsAIGradingEnabled())
										Sub->DisableAIGrading();
									else
										Sub->EnableAIGrading();
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
									.Text_Lambda([]() -> FText
									{
										if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
											return Sub->IsAIGradingEnabled()
												? FText::FromString(TEXT("\x25A0"))
												: FText::FromString(TEXT("\x25A1"));
										return FText::FromString(TEXT("\x25A1"));
									})
									.ColorAndOpacity_Lambda([]() -> FSlateColor
									{
										if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
											return Sub->IsAIGradingEnabled()
												? FSlateColor(FLinearColor(0.2f, 0.8f, 0.4f))
												: FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
										return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
									})
									.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
								]

								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text_Lambda([]() -> FText
									{
										if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
											return Sub->IsAIGradingEnabled()
												? LOCTEXT("AIDisable", "停止 AI 调色")
												: LOCTEXT("AIEnable", "启用 AI 调色");
										return LOCTEXT("AIEnable", "启用 AI 调色");
									})
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
								]
							]
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

					// 强度滑条
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
							.WidthOverride(60.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AIIntensity", "强度"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Value(1.0f)
							.Delta(0.05f)
							.OnValueChanged_Lambda([](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetAIGradingIntensity(Val);
							})
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
							.WidthOverride(60.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AIInterval", "间隔(ms)"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinValue(50.0f)
							.MaxValue(2000.0f)
							.Value(100.0f)
							.Delta(50.0f)
							.OnValueChanged_Lambda([](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetAIGradingInterval(Val / 1000.0f);
							})
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
							.WidthOverride(60.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("AITransition", "过渡"))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
							]
						]

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.05f)
							.MaxValue(3.0f)
							.Value(0.3f)
							.Delta(0.05f)
							.ToolTipText(LOCTEXT("AITransitionTip", "过渡时间 (秒)，越大越平滑"))
							.OnValueChanged_Lambda([](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetAIGradingTransitionTime(Val);
							})
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
	auto MakeSliderRow = [](const FText& Label, float Min, float Max, float Default, float Step,
		TFunction<void(float)> OnChanged, TFunction<float()> GetValue = nullptr) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)

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
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SSpinBox<float>)
				.MinValue(Min)
				.MaxValue(Max)
				.Value(Default)
				.Delta(Step)
				.OnValueChanged_Lambda([OnChanged](float Val) { OnChanged(Val); })
			];
	};

	return SNew(SVerticalBox)

		// Section header
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 5.0f))
				.OnClicked_Lambda([this]()
				{
					bBloomVisible = !bBloomVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("BloomHeader", "Custom Bloom"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
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
								return Sub->IsCustomBloomEnabled()
									? LOCTEXT("BloomStatusOn", "ON")
									: LOCTEXT("BloomStatusOff", "OFF");
							return LOCTEXT("BloomStatusOff", "OFF");
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity_Lambda([]() -> FSlateColor
						{
							if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								return Sub->IsCustomBloomEnabled()
									? FSlateColor(FLinearColor(0.2f, 0.9f, 0.2f))
									: FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f));
							return FSlateColor(FLinearColor(0.35f, 0.35f, 0.35f));
						})
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
						.Text_Lambda([this]() -> FText
						{
							return FText::FromString(bBloomVisible ? TEXT("\x25BC") : TEXT("\x25B6"));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
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
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(FMargin(12.0f, 6.0f))
				[
					SNew(SVerticalBox)

					// Enable toggle
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(6.0f, 3.0f))
						.OnClicked_Lambda([]()
						{
							if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
								Sub->SetCustomBloomEnabled(!Sub->IsCustomBloomEnabled());
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
								.Text_Lambda([]() -> FText
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										return Sub->IsCustomBloomEnabled()
											? FText::FromString(TEXT("\x25A0"))
											: FText::FromString(TEXT("\x25A1"));
									return FText::FromString(TEXT("\x25A1"));
								})
								.ColorAndOpacity_Lambda([]() -> FSlateColor
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										return Sub->IsCustomBloomEnabled()
											? FSlateColor(FLinearColor(0.2f, 0.8f, 0.4f))
											: FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
									return FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
								})
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
							]

							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text_Lambda([]() -> FText
								{
									if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
										return Sub->IsCustomBloomEnabled()
											? LOCTEXT("BloomDisable", "关闭 Custom Bloom")
											: LOCTEXT("BloomEnable", "启用 Custom Bloom");
									return LOCTEXT("BloomEnable", "启用 Custom Bloom");
								})
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
							]
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
						MakeSliderRow(
							LOCTEXT("SceneIntensity", "Scene 强度"),
							0.0f, 5.0f, 0.8f, 0.1f,
							[](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetSceneBloomIntensity(Val);
							})
					]

					// Scene Bloom Threshold
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeSliderRow(
							LOCTEXT("SceneThreshold", "Scene 阈值"),
							0.0f, 5.0f, 1.0f, 0.1f,
							[](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetSceneBloomThreshold(Val);
							})
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
						MakeSliderRow(
							LOCTEXT("VFXIntensity", "VFX 强度"),
							0.0f, 5.0f, 1.0f, 0.1f,
							[](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetVFXBloomIntensity(Val);
							})
					]

					// VFX Bloom Threshold
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeSliderRow(
							LOCTEXT("VFXThreshold", "VFX 阈值"),
							0.0f, 5.0f, 0.2f, 0.05f,
							[](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetVFXBloomThreshold(Val);
							})
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
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(SSpinBox<int32>)
							.MinValue(3)
							.MaxValue(6)
							.Value(6)
							.Delta(1)
							.OnValueChanged_Lambda([](int32 Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetBloomLevels(Val);
							})
						]
					]

					// Scatter
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeSliderRow(
							LOCTEXT("BloomScatter", "Scatter"),
							0.0f, 1.0f, 0.4f, 0.05f,
							[](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetBloomScatter(Val);
							})
					]

					// Max Brightness (anti-flicker)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[
						MakeSliderRow(
							LOCTEXT("MaxBrightness", "Max亮度"),
							0.0f, 50.0f, 10.0f, 1.0f,
							[](float Val)
							{
								if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
									Sub->SetMaxBrightness(Val);
							})
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

		// Section header: GALLERY（可点击折叠）
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
			.Padding(0)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(8.0f, 4.0f))
				.OnClicked_Lambda([this]()
				{
					bGalleryVisible = !bGalleryVisible;
					Invalidate(EInvalidateWidgetReason::Layout);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("GalleryHeader", "参考画廊"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.55f, 0.55f)))
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
						.Text_Lambda([this]() -> FText
						{
							return FText::FromString(bGalleryVisible ? TEXT("\x25BC") : TEXT("\x25B2"));
						})
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f)))
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
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(FMargin(12.0f, 4.0f))
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
