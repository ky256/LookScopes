// Copyright KuoYu. All Rights Reserved.

#include "ViewportCapture.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "Editor.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"

FViewportCapture::~FViewportCapture()
{
	if (PendingReadback)
	{
		if (!PendingReadback->IsReady())
		{
			FlushRenderingCommands();
		}
		delete PendingReadback;
		PendingReadback = nullptr;
	}
}

// ============================================================
// 同步捕获（旧路径，阻塞）
// ============================================================

FViewportCaptureResult FViewportCapture::CaptureCurrentFrame()
{
	FViewportCaptureResult Result;

	FEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetActiveViewport() && GCurrentLevelEditingViewportClient)
	{
		ViewportClient = GCurrentLevelEditingViewportClient;
	}
	if (!ViewportClient) return Result;

	FViewport* Viewport = ViewportClient->Viewport;
	if (!Viewport) return Result;

	const int32 W = Viewport->GetSizeXY().X;
	const int32 H = Viewport->GetSizeXY().Y;
	if (W <= 0 || H <= 0) return Result;

	TArray<FColor> Pixels;
	if (!Viewport->ReadPixels(Pixels) || Pixels.Num() == 0) return Result;

	Result.Pixels = MoveTemp(Pixels);
	Result.Width = W;
	Result.Height = H;
	Result.bIsValid = true;
	return Result;
}

// ============================================================
// 异步捕获（新路径，不阻塞）
// ============================================================

void FViewportCapture::RequestAsyncCapture()
{
	if (PendingReadback) return;

	FEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetActiveViewport() && GCurrentLevelEditingViewportClient)
	{
		ViewportClient = GCurrentLevelEditingViewportClient;
	}
	if (!ViewportClient || !ViewportClient->Viewport) return;

	FViewport* Viewport = ViewportClient->Viewport;
	PendingWidth = Viewport->GetSizeXY().X;
	PendingHeight = Viewport->GetSizeXY().Y;
	if (PendingWidth <= 0 || PendingHeight <= 0) return;

	FViewportRHIRef ViewportRHI = Viewport->GetViewportRHI();
	if (!ViewportRHI.IsValid()) return;

	PendingReadback = new FRHIGPUTextureReadback(TEXT("AIGraderCapture"));
	FRHIGPUTextureReadback* Readback = PendingReadback;

	ENQUEUE_RENDER_COMMAND(AIGraderAsyncCapture)(
		[Readback, ViewportRHI](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef BackBuffer = RHIGetViewportBackBuffer(ViewportRHI);
			if (BackBuffer.IsValid())
			{
				Readback->EnqueueCopy(RHICmdList, BackBuffer);
			}
		});
}

bool FViewportCapture::IsAsyncReady() const
{
	return PendingReadback && PendingReadback->IsReady();
}

FViewportCaptureResult FViewportCapture::CollectAsyncResult()
{
	FViewportCaptureResult Result;
	if (!PendingReadback || !PendingReadback->IsReady()) return Result;

	int32 RowPitchInPixels = 0;
	void* RawData = PendingReadback->Lock(RowPitchInPixels);
	if (RawData && RowPitchInPixels > 0)
	{
		Result.Width = PendingWidth;
		Result.Height = PendingHeight;
		Result.Pixels.SetNumUninitialized(PendingWidth * PendingHeight);

		const FColor* SrcRows = static_cast<const FColor*>(RawData);
		for (int32 y = 0; y < PendingHeight; y++)
		{
			FMemory::Memcpy(
				&Result.Pixels[y * PendingWidth],
				&SrcRows[y * RowPitchInPixels],
				PendingWidth * sizeof(FColor));
		}
		Result.bIsValid = true;
	}

	PendingReadback->Unlock();
	delete PendingReadback;
	PendingReadback = nullptr;

	return Result;
}
