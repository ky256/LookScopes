// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

struct FCustomBloomParams
{
	float SceneBloomIntensity = 0.8f;
	float SceneBloomThreshold = 1.0f;
	float VFXBloomIntensity = 1.0f;
	float VFXBloomThreshold = 0.2f;
	int32 BloomLevels = 6;
	float Scatter = 0.4f;
	float MaxBrightness = 10.0f;
	FLinearColor BloomTint = FLinearColor::White;
	int32 DebugMode = 0; // 0=normal, 1=bloom only, 2=VFX bloom
	float TemporalWeight = 0.9f; // 0=no smoothing, higher=more stable (max ~0.95)
};

class FBloomRenderer
{
public:
	void Render(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef SceneColor,
		FRDGTextureRef TranslucentColor,
		const FIntRect& ViewRect,
		const FCustomBloomParams& Params,
		TRefCountPtr<IPooledRenderTarget>& InOutSceneBloomHistory);

private:
	static constexpr int32 MaxBloomLevels = 6;
	static constexpr int32 ThreadGroupSize = 16;

	FRDGTextureRef RunDownsampleChain(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef SourceTexture,
		const FIntPoint& SourceExtent,
		float Threshold,
		float MaxBrightness,
		int32 Levels,
		const TCHAR* DebugPrefix,
		TArray<FRDGTextureRef>& OutMips);

	FRDGTextureRef RunUpsampleChain(
		FRDGBuilder& GraphBuilder,
		const TArray<FRDGTextureRef>& Mips,
		float Scatter,
		const TCHAR* DebugPrefix);

	FRDGTextureRef RunTemporalBlend(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef Current,
		FRDGTextureRef History,
		float Weight);
};
