// Copyright KuoYu. All Rights Reserved.

#include "ViewportCapture.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "Editor.h"

FViewportCaptureResult FViewportCapture::CaptureCurrentFrame()
{
	FViewportCaptureResult Result;

	// 获取活跃的编辑器视口
	FEditorViewportClient* ViewportClient = nullptr;

	if (GEditor && GEditor->GetActiveViewport())
	{
		if (GCurrentLevelEditingViewportClient)
		{
			ViewportClient = GCurrentLevelEditingViewportClient;
		}
	}

	if (!ViewportClient)
	{
UE_LOG(LogTemp, Error, TEXT("LookScopes: 无法获取活跃的编辑器视口"));
		return Result;
	}

	FViewport* Viewport = ViewportClient->Viewport;
	if (!Viewport)
	{
UE_LOG(LogTemp, Error, TEXT("LookScopes: 视口无效"));
		return Result;
	}

	int32 ViewportWidth = Viewport->GetSizeXY().X;
	int32 ViewportHeight = Viewport->GetSizeXY().Y;

	if (ViewportWidth <= 0 || ViewportHeight <= 0)
	{
UE_LOG(LogTemp, Error, TEXT("LookScopes: 视口尺寸无效 (%d x %d)"), ViewportWidth, ViewportHeight);
		return Result;
	}

	// ReadPixels 从 GPU 回读当前帧缓冲
	TArray<FColor> Pixels;
	bool bSuccess = Viewport->ReadPixels(Pixels);
	if (!bSuccess || Pixels.Num() == 0)
	{
UE_LOG(LogTemp, Error, TEXT("LookScopes: 读取视口像素失败"));
		return Result;
	}

UE_LOG(LogTemp, Verbose, TEXT("LookScopes: 捕获视口 %d x %d (%d 像素)"),
		ViewportWidth, ViewportHeight, Pixels.Num());

	Result.Pixels = MoveTemp(Pixels);
	Result.Width = ViewportWidth;
	Result.Height = ViewportHeight;
	Result.bIsValid = true;

	return Result;
}
