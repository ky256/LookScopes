// Copyright KuoYu. All Rights Reserved.

#include "ViewportStreamer.h"
#include "MediaOutput.h"
#include "MediaCapture.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogViewportStreamer, Log, All);

static constexpr float DEBOUNCE_SECONDS = 1.0f;

FViewportStreamer::FViewportStreamer() {}

FViewportStreamer::~FViewportStreamer()
{
	StopStreaming();
}

UClass* FViewportStreamer::FindNDIMediaOutputClass() const
{
	UClass* C = FindObject<UClass>(nullptr, TEXT("/Script/NDIMedia.NDIMediaOutput"));
	if (!C) C = LoadClass<UMediaOutput>(nullptr, TEXT("/Script/NDIMedia.NDIMediaOutput"));
	return C;
}

bool FViewportStreamer::StartStreaming(const FString& SourceName, int32 Width, int32 Height)
{
	UE_LOG(LogViewportStreamer, Log, TEXT("[StartStreaming] called, SourceName=%s, bIsStreaming=%d"), *SourceName, bIsStreaming);

	SavedWidth = Width;
	SavedHeight = Height;

	if (bIsStreaming)
	{
		UE_LOG(LogViewportStreamer, Warning, TEXT("[StartStreaming] Already streaming, skipping"));
		return true;
	}

	UClass* NDIOutputClass = FindNDIMediaOutputClass();
	if (!NDIOutputClass)
	{
		UE_LOG(LogViewportStreamer, Error, TEXT("[StartStreaming] NDIMediaOutput class not found"));
		return false;
	}

	MediaOutput = NewObject<UMediaOutput>(GetTransientPackage(), NDIOutputClass);
	if (!MediaOutput)
	{
		UE_LOG(LogViewportStreamer, Error, TEXT("[StartStreaming] NewObject<NDIMediaOutput> failed"));
		return false;
	}

	if (FProperty* Prop = NDIOutputClass->FindPropertyByName(TEXT("SourceName")))
		if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
			StrProp->SetPropertyValue_InContainer(MediaOutput, SourceName);

	if (Width > 0 && Height > 0)
	{
		if (FProperty* P = NDIOutputClass->FindPropertyByName(TEXT("bOverrideDesiredSize")))
			if (FBoolProperty* BP = CastField<FBoolProperty>(P))
				BP->SetPropertyValue_InContainer(MediaOutput, true);
		if (FProperty* P = NDIOutputClass->FindPropertyByName(TEXT("DesiredSize")))
			if (FStructProperty* SP = CastField<FStructProperty>(P))
				if (FIntPoint* Ptr = SP->ContainerPtrToValuePtr<FIntPoint>(MediaOutput))
					*Ptr = FIntPoint(Width, Height);
	}

	MediaCapture = MediaOutput->CreateMediaCapture();
	if (!MediaCapture)
	{
		UE_LOG(LogViewportStreamer, Error, TEXT("[StartStreaming] CreateMediaCapture failed"));
		MediaOutput = nullptr;
		return false;
	}

	UE_LOG(LogViewportStreamer, Log, TEXT("[StartStreaming] Subscribing to OnStateChangedNative"));
	StateChangedHandle = MediaCapture->OnStateChangedNative.AddRaw(
		this, &FViewportStreamer::OnCaptureStateChanged);

	FMediaCaptureOptions Opts;
	Opts.bAutoRestartOnSourceSizeChange = true;
	Opts.OverrunAction = EMediaCaptureOverrunAction::Skip;
	if (Width > 0 && Height > 0)
	{
		Opts.ResizeMethod = EMediaCaptureResizeMethod::ResizeInRenderPass;
	}

	UE_LOG(LogViewportStreamer, Log, TEXT("[StartStreaming] Calling CaptureActiveSceneViewport..."));
	if (!MediaCapture->CaptureActiveSceneViewport(Opts))
	{
		UE_LOG(LogViewportStreamer, Error, TEXT("[StartStreaming] CaptureActiveSceneViewport failed"));
		MediaCapture->OnStateChangedNative.Remove(StateChangedHandle);
		MediaCapture = nullptr;
		MediaOutput = nullptr;
		return false;
	}

	bIsStreaming = true;
	bWantsRestart = false;
	CurrentSourceName = SourceName;

	UE_LOG(LogViewportStreamer, Log, TEXT("[StartStreaming] SUCCESS, source=\"%s\""), *SourceName);
	return true;
}

void FViewportStreamer::StopStreaming()
{
	UE_LOG(LogViewportStreamer, Log, TEXT("[StopStreaming] called, bIsStreaming=%d"), bIsStreaming);

	// Always cancel pending debounce
	if (DebounceTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DebounceTickHandle);
		DebounceTickHandle.Reset();
	}
	bWantsRestart = false;

	if (!bIsStreaming) return;
	bIsStreaming = false;

	if (MediaCapture)
	{
		UE_LOG(LogViewportStreamer, Log, TEXT("[StopStreaming] Removing delegate & stopping capture"));
		MediaCapture->OnStateChangedNative.Remove(StateChangedHandle);
		MediaCapture->StopCapture(false);
		MediaCapture = nullptr;
	}
	MediaOutput = nullptr;
	CurrentSourceName.Empty();

	UE_LOG(LogViewportStreamer, Log, TEXT("[StopStreaming] Done"));
}

bool FViewportStreamer::IsStreaming() const
{
	return bIsStreaming && MediaCapture != nullptr;
}

void FViewportStreamer::OnCaptureStateChanged()
{
	if (!MediaCapture) return;

	EMediaCaptureState State = MediaCapture->GetState();
	UE_LOG(LogViewportStreamer, Log, TEXT("[OnCaptureStateChanged] State=%d, bIsStreaming=%d, bWantsRestart=%d"),
		(int)State, bIsStreaming, bWantsRestart);

	if (State != EMediaCaptureState::Stopped && State != EMediaCaptureState::Error) return;
	if (!bIsStreaming) return;

	// Debounce: reset timer on every stop event (viewport may still be resizing)
	DebounceTimer = DEBOUNCE_SECONDS;
	
	if (!bWantsRestart)
	{
		UE_LOG(LogViewportStreamer, Warning,
			TEXT("[OnCaptureStateChanged] Capture stopped, starting %.1fs debounce timer"), DEBOUNCE_SECONDS);
		bWantsRestart = true;

		DebounceTickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FViewportStreamer::TickDebounce), 0.1f);
	}
	else
	{
		UE_LOG(LogViewportStreamer, Log,
			TEXT("[OnCaptureStateChanged] Debounce timer reset (viewport still resizing)"));
	}
}

bool FViewportStreamer::TickDebounce(float DeltaTime)
{
	DebounceTimer -= DeltaTime;

	if (DebounceTimer > 0.0f) return true; // keep ticking

	UE_LOG(LogViewportStreamer, Log, TEXT("[TickDebounce] Debounce expired, restarting stream"));

	FString SavedName = CurrentSourceName;
	DebounceTickHandle.Reset();
	bWantsRestart = false;

	// Full stop — unsubscribes delegate, clears state
	bIsStreaming = true; // ensure StopStreaming actually runs
	if (MediaCapture)
	{
		MediaCapture->OnStateChangedNative.Remove(StateChangedHandle);
		MediaCapture->StopCapture(false);
		MediaCapture = nullptr;
	}
	MediaOutput = nullptr;
	bIsStreaming = false;

	UE_LOG(LogViewportStreamer, Log, TEXT("[TickDebounce] Old capture cleaned up, calling StartStreaming (res=%dx%d)"),
		SavedWidth, SavedHeight);
	StartStreaming(SavedName, SavedWidth, SavedHeight);

	return false; // stop ticking
}

void FViewportStreamer::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(MediaOutput);
	Collector.AddReferencedObject(MediaCapture);
}
