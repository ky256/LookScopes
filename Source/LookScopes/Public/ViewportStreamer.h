// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class UMediaOutput;
class UMediaCapture;

class LOOKSCOPES_API FViewportStreamer : public FGCObject
{
public:
	FViewportStreamer();
	~FViewportStreamer();

	bool StartStreaming(const FString& SourceName = TEXT("UE_LookScopes"), int32 Width = 0, int32 Height = 0);
	void StopStreaming();
	bool IsStreaming() const;
	FString GetSourceName() const { return CurrentSourceName; }

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FViewportStreamer"); }

	FViewportStreamer(const FViewportStreamer&) = delete;
	FViewportStreamer& operator=(const FViewportStreamer&) = delete;

private:
	UClass* FindNDIMediaOutputClass() const;
	void OnCaptureStateChanged();
	bool TickDebounce(float DeltaTime);

	TObjectPtr<UMediaOutput> MediaOutput;
	TObjectPtr<UMediaCapture> MediaCapture;
	FDelegateHandle StateChangedHandle;
	FTSTicker::FDelegateHandle DebounceTickHandle;

	FString CurrentSourceName;
	int32 SavedWidth = 0;
	int32 SavedHeight = 0;
	bool bIsStreaming = false;
	bool bWantsRestart = false;
	float DebounceTimer = 0.0f;
};
