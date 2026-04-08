// Copyright KuoYu. All Rights Reserved.

#include "LookScopesSubsystem.h"
#include "LookScopesCommands.h"
#include "SLookMatchPanel.h"
#include "ScopeSessionManager.h"
#include "ScopeAnalyzer.h"
#include "ViewportStreamer.h"
#include "AIColorGrader.h"
#include "CustomBloomViewExtension.h"
#include "SceneViewExtension.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "Slate/SceneViewport.h"
#include "LevelEditorViewport.h"

#define LOCTEXT_NAMESPACE "LookScopesSubsystem"

static FAutoConsoleCommand CmdCustomBloom(
	TEXT("LookScopes.CustomBloom"),
	TEXT("Toggle custom bloom (0=off, 1=on)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor) return;
		auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>();
		if (!Sub) return;
		const bool bEnable = Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : true;
		Sub->SetCustomBloomEnabled(bEnable);
		UE_LOG(LogTemp, Log, TEXT("Custom Bloom: %s"), bEnable ? TEXT("ON") : TEXT("OFF"));
	}));

static FAutoConsoleCommand CmdBloomSceneIntensity(
	TEXT("LookScopes.Bloom.SceneIntensity"),
	TEXT("Set scene bloom intensity (float)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetSceneBloomIntensity(FCString::Atof(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomSceneThreshold(
	TEXT("LookScopes.Bloom.SceneThreshold"),
	TEXT("Set scene bloom threshold (float, HDR units)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetSceneBloomThreshold(FCString::Atof(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomVFXIntensity(
	TEXT("LookScopes.Bloom.VFXIntensity"),
	TEXT("Set VFX bloom intensity (float)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetVFXBloomIntensity(FCString::Atof(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomVFXThreshold(
	TEXT("LookScopes.Bloom.VFXThreshold"),
	TEXT("Set VFX bloom threshold (float, HDR units)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetVFXBloomThreshold(FCString::Atof(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomLevels(
	TEXT("LookScopes.Bloom.Levels"),
	TEXT("Set bloom downsample levels (int, 3-6)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetBloomLevels(FCString::Atoi(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomScatter(
	TEXT("LookScopes.Bloom.Scatter"),
	TEXT("Set bloom scatter (float, 0-1). 0=tight glow, 1=wide halo"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetBloomScatter(FCString::Atof(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomMaxBrightness(
	TEXT("LookScopes.Bloom.MaxBrightness"),
	TEXT("Clamp bloom HDR peaks (float, 0=off). Prevents specular flicker"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor || Args.Num() == 0) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
			Sub->SetMaxBrightness(FCString::Atof(*Args[0]));
	}));

static FAutoConsoleCommand CmdBloomDebug(
	TEXT("LookScopes.Bloom.Debug"),
	TEXT("Debug bloom output (0=normal, 1=bloom only, 2=VFX bloom only)"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor) return;
		if (auto* Sub = GEditor->GetEditorSubsystem<ULookScopesSubsystem>())
		{
			const int32 Mode = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
			Sub->SetBloomDebugMode(Mode);
			UE_LOG(LogTemp, Log, TEXT("Bloom Debug Mode: %d"), Mode);
		}
	}));

// ============================================================
// 全局输入处理器 - 拦截快捷键
// ============================================================

class FLookScopesInputProcessor : public IInputProcessor
{
public:
	FLookScopesInputProcessor(TSharedPtr<FUICommandList> InCommandList)
		: CommandList(InCommandList)
	{
	}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		if (CommandList.IsValid())
		{
			return CommandList->ProcessCommandBindings(InKeyEvent.GetKey(), InKeyEvent.GetModifierKeys(), InKeyEvent.IsRepeat());
		}
		return false;
	}

virtual const TCHAR* GetDebugName() const override { return TEXT("LookScopesInputProcessor"); }

private:
	TSharedPtr<FUICommandList> CommandList;
};

const FName ULookScopesSubsystem::ScopeTabId(TEXT("LookScopesTab"));

// ============================================================
// UEditorSubsystem 接口
// ============================================================

ULookScopesSubsystem::~ULookScopesSubsystem() = default;

void ULookScopesSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	InitializeSessionManager();
	RegisterCommands();
	RegisterTabSpawner();
	RegisterMenus();

	BloomViewExtension = FSceneViewExtensions::NewExtension<FCustomBloomViewExtension>();
	LoadBloomConfig();
	LoadAIGradingConfig();
	LoadToolbarConfig();

UE_LOG(LogTemp, Log, TEXT("LookScopes: Subsystem 已初始化"));
}

void ULookScopesSubsystem::Deinitialize()
{
	// 注销输入处理器
	if (InputProcessor.IsValid())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
		InputProcessor.Reset();
	}

	UnregisterTabSpawner();

	if (CommandList.IsValid())
	{
		CommandList.Reset();
	}

	FLookScopesCommands::Unregister();

	// 关闭 AI 调色
	if (AIColorGrader.IsValid())
	{
		AIColorGrader->Shutdown();
		AIColorGrader.Reset();
	}

	// 清除视口固定尺寸 & 停止 NDI 推流
	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
	{
		static_cast<FSceneViewport*>(GCurrentLevelEditingViewportClient->Viewport)->SetFixedViewportSize(0, 0);
	}
	if (ViewportStreamer.IsValid())
	{
		ViewportStreamer->StopStreaming();
		ViewportStreamer.Reset();
	}

	// 释放 Bloom ViewExtension
	if (BloomViewExtension.IsValid())
	{
		BloomViewExtension->SetEnabled(false);
		BloomViewExtension.Reset();
	}

	// 销毁 SessionManager（会自动停止实时模式、清理分析器）
	SessionManager.Reset();

UE_LOG(LogTemp, Log, TEXT("LookScopes: Subsystem 已销毁"));

	Super::Deinitialize();
}

// ============================================================
// SessionManager 初始化
// ============================================================

void ULookScopesSubsystem::InitializeSessionManager()
{
	SessionManager = MakeShared<FScopeSessionManager>();

	// 注册分析器
	SessionManager->RegisterAnalyzer(MakeShared<FHistogramAnalyzer>());
	SessionManager->RegisterAnalyzer(MakeShared<FWaveformAnalyzer>());

UE_LOG(LogTemp, Log, TEXT("LookScopes: SessionManager 已初始化，已注册 %d 个分析器"),
		SessionManager->GetAnalyzers().Num());
}

// ============================================================
// 公开接口
// ============================================================

void ULookScopesSubsystem::OpenScopePanel()
{
	FGlobalTabmanager::Get()->TryInvokeTab(ScopeTabId);
}

void ULookScopesSubsystem::TriggerAnalysis()
{
	// 先确保面板已打开
	OpenScopePanel();

	// 通过 SessionManager 触发分析
	if (SessionManager.IsValid())
	{
		SessionManager->AnalyzeOnce();
	}
}

// ============================================================
// 内部实现
// ============================================================

void ULookScopesSubsystem::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		ScopeTabId,
		FOnSpawnTab::CreateUObject(this, &ULookScopesSubsystem::SpawnScopeTab))
		.SetDisplayName(LOCTEXT("TabTitle", "视觉对标"))
		.SetTooltipText(LOCTEXT("TabTooltip", "视口分析与视觉对标工具 - 波形图、直方图、Look Match"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));
}

void ULookScopesSubsystem::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ScopeTabId);
}

void ULookScopesSubsystem::RegisterMenus()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		return;
	}

	// 在 Tools 菜单下添加入口
	UToolMenu* ToolsMenu = ToolMenus->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (ToolsMenu)
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection("LookScopes");
		Section.Label = LOCTEXT("MenuSectionLabel", "视觉对标");

		Section.AddMenuEntry(
			"OpenLookScopes",
			LOCTEXT("MenuEntryLabel", "视觉对标面板"),
			LOCTEXT("MenuEntryTooltip", "打开视觉对标面板 (Ctrl+Shift+L)"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateUObject(this, &ULookScopesSubsystem::OpenScopePanel))
		);
	}
}

void ULookScopesSubsystem::RegisterCommands()
{
	FLookScopesCommands::Register();

	CommandList = MakeShared<FUICommandList>();

	const FLookScopesCommands& Commands = FLookScopesCommands::Get();

	// F8: 触发分析
	CommandList->MapAction(
		Commands.AnalyzeFrame,
		FExecuteAction::CreateUObject(this, &ULookScopesSubsystem::TriggerAnalysis)
	);

	// Ctrl+Shift+L: 打开面板
	CommandList->MapAction(
		Commands.TogglePanel,
		FExecuteAction::CreateUObject(this, &ULookScopesSubsystem::OpenScopePanel)
	);

	// 绑定到全局输入处理器
	InputProcessor = MakeShared<FLookScopesInputProcessor>(CommandList);
	FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor, 0);
}

// ============================================================
// NDI 推流控制
// ============================================================

void ULookScopesSubsystem::StartNDIStream(const FString& SourceName)
{
	if (!ViewportStreamer.IsValid())
	{
		ViewportStreamer = MakeShared<FViewportStreamer>();
	}

	if (!ViewportStreamer->IsStreaming())
	{
		ViewportStreamer->StartStreaming(SourceName, StreamResolution.X, StreamResolution.Y);
	}
}

void ULookScopesSubsystem::SetStreamResolution(FIntPoint InRes)
{
	StreamResolution = InRes;

	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
	{
		FSceneViewport* SceneVP = static_cast<FSceneViewport*>(GCurrentLevelEditingViewportClient->Viewport);
		if (InRes.X > 0 && InRes.Y > 0)
		{
			SceneVP->SetFixedViewportSize(InRes.X, InRes.Y);
		}
		else
		{
			SceneVP->SetFixedViewportSize(0, 0);
		}
	}

	if (IsNDIStreaming())
	{
		FString Name = ViewportStreamer->GetSourceName();
		ViewportStreamer->StopStreaming();
		ViewportStreamer->StartStreaming(Name, StreamResolution.X, StreamResolution.Y);
	}

	SaveToolbarConfig();
}

void ULookScopesSubsystem::StopNDIStream()
{
	if (ViewportStreamer.IsValid())
	{
		ViewportStreamer->StopStreaming();
	}
}

bool ULookScopesSubsystem::IsNDIStreaming() const
{
	return ViewportStreamer.IsValid() && ViewportStreamer->IsStreaming();
}

// ============================================================
// AI 调色控制
// ============================================================

void ULookScopesSubsystem::EnableAIGrading(const FString& OnnxModelPath)
{
	if (!AIColorGrader.IsValid())
	{
		AIColorGrader = MakeUnique<FAIColorGrader>();
		if (!AIColorGrader->Initialize(OnnxModelPath))
		{
			UE_LOG(LogTemp, Error, TEXT("LookScopes: AI 调色器初始化失败"));
		}
		AIColorGrader->SetIntensity(CachedAIIntensity);
		AIColorGrader->SetInferenceInterval(CachedAIInterval);
		AIColorGrader->SetTransitionTime(CachedAITransition);
	}

	AIColorGrader->SetEnabled(true);
}

void ULookScopesSubsystem::DisableAIGrading()
{
	if (AIColorGrader.IsValid())
	{
		AIColorGrader->SetEnabled(false);
	}
}

bool ULookScopesSubsystem::IsAIGradingEnabled() const
{
	return AIColorGrader.IsValid() && AIColorGrader->IsEnabled();
}

void ULookScopesSubsystem::SetAIGradingIntensity(float InIntensity)
{
	CachedAIIntensity = InIntensity;
	if (AIColorGrader.IsValid())
		AIColorGrader->SetIntensity(InIntensity);
	SaveAIGradingConfig();
}

void ULookScopesSubsystem::SetAIGradingInterval(float Seconds)
{
	CachedAIInterval = Seconds;
	if (AIColorGrader.IsValid())
		AIColorGrader->SetInferenceInterval(Seconds);
	SaveAIGradingConfig();
}

void ULookScopesSubsystem::SetAIGradingTransitionTime(float Seconds)
{
	CachedAITransition = Seconds;
	if (AIColorGrader.IsValid())
		AIColorGrader->SetTransitionTime(Seconds);
	SaveAIGradingConfig();
}

void ULookScopesSubsystem::TriggerAIInferOnce()
{
	if (AIColorGrader.IsValid())
	{
		AIColorGrader->InferOnce();
	}
}

FAIColorGrader* ULookScopesSubsystem::GetAIColorGrader() const
{
	return AIColorGrader.Get();
}

// ============================================================
// Bloom Config Persistence
// ============================================================

static const TCHAR* BloomConfigSection = TEXT("/Script/LookScopes.BloomSettings");

void ULookScopesSubsystem::SaveBloomConfig() const
{
	if (!BloomViewExtension.IsValid()) return;

	const FCustomBloomParams& P = BloomViewExtension->GetBloomParams();
	const bool bOn = BloomViewExtension->IsEnabled();

	GConfig->SetBool(BloomConfigSection, TEXT("bEnabled"), bOn, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("SceneBloomIntensity"), P.SceneBloomIntensity, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("SceneBloomThreshold"), P.SceneBloomThreshold, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("VFXBloomIntensity"), P.VFXBloomIntensity, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("VFXBloomThreshold"), P.VFXBloomThreshold, GEditorPerProjectIni);
	GConfig->SetInt(BloomConfigSection, TEXT("BloomLevels"), P.BloomLevels, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("Scatter"), P.Scatter, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("MaxBrightness"), P.MaxBrightness, GEditorPerProjectIni);
	GConfig->SetFloat(BloomConfigSection, TEXT("TemporalWeight"), P.TemporalWeight, GEditorPerProjectIni);

	GConfig->Flush(false, GEditorPerProjectIni);
}

void ULookScopesSubsystem::LoadBloomConfig()
{
	if (!BloomViewExtension.IsValid()) return;

	bool bEnabled = false;
	if (GConfig->GetBool(BloomConfigSection, TEXT("bEnabled"), bEnabled, GEditorPerProjectIni))
	{
		BloomViewExtension->SetEnabled(bEnabled);
	}

	FCustomBloomParams P = BloomViewExtension->GetBloomParams();
	bool bHasAny = false;

	float F; int32 I;
	if (GConfig->GetFloat(BloomConfigSection, TEXT("SceneBloomIntensity"), F, GEditorPerProjectIni)) { P.SceneBloomIntensity = F; bHasAny = true; }
	if (GConfig->GetFloat(BloomConfigSection, TEXT("SceneBloomThreshold"), F, GEditorPerProjectIni)) { P.SceneBloomThreshold = F; bHasAny = true; }
	if (GConfig->GetFloat(BloomConfigSection, TEXT("VFXBloomIntensity"), F, GEditorPerProjectIni))   { P.VFXBloomIntensity = F; bHasAny = true; }
	if (GConfig->GetFloat(BloomConfigSection, TEXT("VFXBloomThreshold"), F, GEditorPerProjectIni))   { P.VFXBloomThreshold = F; bHasAny = true; }
	if (GConfig->GetInt(BloomConfigSection, TEXT("BloomLevels"), I, GEditorPerProjectIni))            { P.BloomLevels = I; bHasAny = true; }
	if (GConfig->GetFloat(BloomConfigSection, TEXT("Scatter"), F, GEditorPerProjectIni))              { P.Scatter = F; bHasAny = true; }
	if (GConfig->GetFloat(BloomConfigSection, TEXT("MaxBrightness"), F, GEditorPerProjectIni))        { P.MaxBrightness = F; bHasAny = true; }
	if (GConfig->GetFloat(BloomConfigSection, TEXT("TemporalWeight"), F, GEditorPerProjectIni))       { P.TemporalWeight = F; bHasAny = true; }

	if (bHasAny)
	{
		BloomViewExtension->SetBloomParams(P);
	}

	UE_LOG(LogTemp, Log, TEXT("LookScopes: Bloom config loaded (enabled=%d)"), bEnabled ? 1 : 0);
}

// ============================================================
// AI Grading Config Persistence
// ============================================================

static const TCHAR* AIGradingConfigSection = TEXT("/Script/LookScopes.AIGradingSettings");

void ULookScopesSubsystem::SaveAIGradingConfig() const
{
	GConfig->SetFloat(AIGradingConfigSection, TEXT("Intensity"), CachedAIIntensity, GEditorPerProjectIni);
	GConfig->SetFloat(AIGradingConfigSection, TEXT("InferenceInterval"), CachedAIInterval, GEditorPerProjectIni);
	GConfig->SetFloat(AIGradingConfigSection, TEXT("TransitionTime"), CachedAITransition, GEditorPerProjectIni);

	GConfig->Flush(false, GEditorPerProjectIni);
}

void ULookScopesSubsystem::LoadAIGradingConfig()
{
	float F;
	if (GConfig->GetFloat(AIGradingConfigSection, TEXT("Intensity"), F, GEditorPerProjectIni))
		CachedAIIntensity = F;
	if (GConfig->GetFloat(AIGradingConfigSection, TEXT("InferenceInterval"), F, GEditorPerProjectIni))
		CachedAIInterval = F;
	if (GConfig->GetFloat(AIGradingConfigSection, TEXT("TransitionTime"), F, GEditorPerProjectIni))
		CachedAITransition = F;

	if (AIColorGrader.IsValid())
	{
		AIColorGrader->SetIntensity(CachedAIIntensity);
		AIColorGrader->SetInferenceInterval(CachedAIInterval);
		AIColorGrader->SetTransitionTime(CachedAITransition);
	}
}

// ============================================================
// Toolbar Config Persistence
// ============================================================

static const TCHAR* ToolbarConfigSection = TEXT("/Script/LookScopes.ToolbarSettings");

void ULookScopesSubsystem::SetRealtimeInterval(float Seconds)
{
	CachedRealtimeInterval = FMath::Max(Seconds, 0.05f);
	if (SessionManager.IsValid())
		SessionManager->SetRealtimeInterval(CachedRealtimeInterval);
	SaveToolbarConfig();
}

void ULookScopesSubsystem::SaveToolbarConfig() const
{
	GConfig->SetFloat(ToolbarConfigSection, TEXT("RealtimeInterval"), CachedRealtimeInterval, GEditorPerProjectIni);
	GConfig->SetInt(ToolbarConfigSection, TEXT("ResolutionX"), StreamResolution.X, GEditorPerProjectIni);
	GConfig->SetInt(ToolbarConfigSection, TEXT("ResolutionY"), StreamResolution.Y, GEditorPerProjectIni);

	GConfig->Flush(false, GEditorPerProjectIni);
}

void ULookScopesSubsystem::LoadToolbarConfig()
{
	float F; int32 IX, IY;
	if (GConfig->GetFloat(ToolbarConfigSection, TEXT("RealtimeInterval"), F, GEditorPerProjectIni))
		CachedRealtimeInterval = FMath::Max(F, 0.05f);
	if (GConfig->GetInt(ToolbarConfigSection, TEXT("ResolutionX"), IX, GEditorPerProjectIni) &&
		GConfig->GetInt(ToolbarConfigSection, TEXT("ResolutionY"), IY, GEditorPerProjectIni))
		StreamResolution = FIntPoint(IX, IY);
}

// ============================================================
// Bloom Setters
// ============================================================

void ULookScopesSubsystem::SetCustomBloomEnabled(bool bEnabled)
{
	if (BloomViewExtension.IsValid())
	{
		BloomViewExtension->SetEnabled(bEnabled);
		SaveBloomConfig();
	}
}

bool ULookScopesSubsystem::IsCustomBloomEnabled() const
{
	return BloomViewExtension.IsValid() && BloomViewExtension->IsEnabled();
}

FCustomBloomParams ULookScopesSubsystem::GetBloomParams() const
{
	if (BloomViewExtension.IsValid()) return BloomViewExtension->GetBloomParams();
	return FCustomBloomParams();
}

void ULookScopesSubsystem::SetSceneBloomIntensity(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetSceneBloomIntensity(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetSceneBloomThreshold(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetSceneBloomThreshold(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetVFXBloomIntensity(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetVFXBloomIntensity(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetVFXBloomThreshold(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetVFXBloomThreshold(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetBloomLevels(int32 V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetBloomLevels(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetBloomScatter(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetBloomScatter(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetMaxBrightness(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetMaxBrightness(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetTemporalWeight(float V)
{
	if (BloomViewExtension.IsValid()) { BloomViewExtension->SetTemporalWeight(V); SaveBloomConfig(); }
}

void ULookScopesSubsystem::SetBloomDebugMode(int32 Mode)
{
	if (BloomViewExtension.IsValid())
	{
		FCustomBloomParams P = BloomViewExtension->GetBloomParams();
		P.DebugMode = FMath::Clamp(Mode, 0, 2);
		BloomViewExtension->SetBloomParams(P);
	}
}

// ============================================================
// Tab
// ============================================================

TSharedRef<SDockTab> ULookScopesSubsystem::SpawnScopeTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SLookMatchPanel> PanelWidget = SNew(SLookMatchPanel)
		.SessionManager(SessionManager);

	PanelWidgetWeak = PanelWidget;

	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.Label(LOCTEXT("TabLabel", "视觉对标 & 示波器"))
		[
			PanelWidget
		];
}

#undef LOCTEXT_NAMESPACE
