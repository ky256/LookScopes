// Copyright KuoYu. All Rights Reserved.

#include "BloomRenderer.h"
#include "ScopeShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

void FBloomRenderer::Render(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef SceneColor,
	FRDGTextureRef TranslucentColor,
	const FIntRect& ViewRect,
	const FCustomBloomParams& Params)
{
	if (!SceneColor) return;

	const int32 Levels = FMath::Clamp(Params.BloomLevels, 3, MaxBloomLevels);
	const FIntPoint SceneExtent = SceneColor->Desc.Extent;

	// --- Scene Bloom Chain ---
	TArray<FRDGTextureRef> SceneMips;
	RunDownsampleChain(GraphBuilder, SceneColor, SceneExtent,
		Params.SceneBloomThreshold, Params.MaxBrightness, Levels, TEXT("SceneBloom"), SceneMips);

	FRDGTextureRef SceneBloom = RunUpsampleChain(GraphBuilder, SceneMips, Params.Scatter, TEXT("SceneBloomUp"));

	// --- VFX Bloom Chain (skip if no translucent) ---
	FRDGTextureRef VFXBloom = nullptr;
	if (TranslucentColor)
	{
		TArray<FRDGTextureRef> VFXMips;
		const FIntPoint TransExtent = TranslucentColor->Desc.Extent;
		RunDownsampleChain(GraphBuilder, TranslucentColor, TransExtent,
			Params.VFXBloomThreshold, Params.MaxBrightness, Levels, TEXT("VFXBloom"), VFXMips);
		VFXBloom = RunUpsampleChain(GraphBuilder, VFXMips, Params.Scatter, TEXT("VFXBloomUp"));
	}

	// If no VFX bloom, create a 1x1 black texture as placeholder
	if (!VFXBloom)
	{
		VFXBloom = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_FloatRGBA,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			TEXT("VFXBloom_Black"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VFXBloom), FLinearColor::Black);
	}

	// --- Composite: SceneColor + Bloom → TempOutput ---
	FRDGTextureRef CompositeOutput = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(SceneExtent, SceneColor->Desc.Format,
			FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("BloomCompositeOut"));

	TShaderMapRef<FBloomCompositeCS> CompositeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FBloomCompositeCS::FParameters* CompParams = GraphBuilder.AllocParameters<FBloomCompositeCS::FParameters>();
	CompParams->SceneColorTex = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SceneColor));
	CompParams->SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
	CompParams->SceneBloomTex = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(SceneBloom));
	CompParams->SceneBloomSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
	CompParams->VFXBloomTex = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(VFXBloom));
	CompParams->VFXBloomSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
	CompParams->OutputTexture = GraphBuilder.CreateUAV(CompositeOutput);
	CompParams->OutputSize = FUintVector2(SceneExtent.X, SceneExtent.Y);
	CompParams->SceneBloomIntensity = Params.SceneBloomIntensity;
	CompParams->VFXBloomIntensity = Params.VFXBloomIntensity;
	CompParams->BloomTint = FVector3f(Params.BloomTint.R, Params.BloomTint.G, Params.BloomTint.B);

	const int32 GX = FMath::DivideAndRoundUp(SceneExtent.X, ThreadGroupSize);
	const int32 GY = FMath::DivideAndRoundUp(SceneExtent.Y, ThreadGroupSize);
	FComputeShaderUtils::AddPass(GraphBuilder,
		RDG_EVENT_NAME("BloomComposite_%dx%d", SceneExtent.X, SceneExtent.Y),
		CompositeShader, CompParams, FIntVector(GX, GY, 1));

	// --- Copy result back to SceneColor ---
	AddCopyTexturePass(GraphBuilder, CompositeOutput, SceneColor,
		FIntPoint::ZeroValue, FIntPoint::ZeroValue, SceneExtent);
}

FRDGTextureRef FBloomRenderer::RunDownsampleChain(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef SourceTexture,
	const FIntPoint& SourceExtent,
	float Threshold,
	float MaxBrightness,
	int32 Levels,
	const TCHAR* DebugPrefix,
	TArray<FRDGTextureRef>& OutMips)
{
	OutMips.Reset();

	TShaderMapRef<FBloomDownsampleCS> DownShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FRDGTextureRef Current = SourceTexture;
	FIntPoint CurrentSize = SourceExtent;

	for (int32 i = 0; i < Levels; ++i)
	{
		const FIntPoint NextSize(
			FMath::Max(CurrentSize.X / 2, 1),
			FMath::Max(CurrentSize.Y / 2, 1));

		FRDGTextureRef MipTex = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(NextSize, PF_FloatRGBA,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			*FString::Printf(TEXT("%s_Down%d"), DebugPrefix, i));

		FBloomDownsampleCS::FParameters* P = GraphBuilder.AllocParameters<FBloomDownsampleCS::FParameters>();
		P->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(Current));
		P->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		P->OutputTexture = GraphBuilder.CreateUAV(MipTex);
		P->OutputSize = FUintVector2(NextSize.X, NextSize.Y);
		P->TexelSize = FVector2f(1.0f / CurrentSize.X, 1.0f / CurrentSize.Y);
		P->Threshold = (i == 0) ? Threshold : 0.0f;
		P->MaxBrightness = (i == 0) ? MaxBrightness : 0.0f;

		const int32 GX = FMath::DivideAndRoundUp(NextSize.X, ThreadGroupSize);
		const int32 GY = FMath::DivideAndRoundUp(NextSize.Y, ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("%s_Down%d_%dx%d", DebugPrefix, i, NextSize.X, NextSize.Y),
			DownShader, P, FIntVector(GX, GY, 1));

		OutMips.Add(MipTex);
		Current = MipTex;
		CurrentSize = NextSize;
	}

	return Current;
}

FRDGTextureRef FBloomRenderer::RunUpsampleChain(
	FRDGBuilder& GraphBuilder,
	const TArray<FRDGTextureRef>& Mips,
	float Scatter,
	const TCHAR* DebugPrefix)
{
	if (Mips.Num() == 0) return nullptr;
	if (Mips.Num() == 1) return Mips[0];

	TShaderMapRef<FBloomUpsampleCS> UpShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	const int32 NumUpsampleSteps = Mips.Num() - 1;
	const float ClampedScatter = FMath::Clamp(Scatter, 0.0f, 1.0f);

	FRDGTextureRef Current = Mips.Last();

	for (int32 i = Mips.Num() - 2; i >= 0; --i)
	{
		// Level index: 0 = finest (closest to full res), NumUpsampleSteps-1 = widest
		const int32 LevelFromFine = i;
		const float T = (NumUpsampleSteps > 1)
			? static_cast<float>(LevelFromFine) / static_cast<float>(NumUpsampleSteps - 1)
			: 0.0f;

		// Per-level weight: fine levels contribute more, wide levels less
		// Scatter shifts weight toward wider levels
		const float BaseWeight = FMath::Lerp(1.0f, 0.1f, T);
		const float ScatterBoost = ClampedScatter * T * 0.6f;
		const float W = FMath::Max(BaseWeight + ScatterBoost, 0.05f);

		FRDGTextureRef BlendTarget = Mips[i];
		const FIntPoint OutSize = BlendTarget->Desc.Extent;
		const FIntPoint InputSize = Current->Desc.Extent;

		FRDGTextureRef UpTex = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(OutSize, PF_FloatRGBA,
				FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
			*FString::Printf(TEXT("%s_%d"), DebugPrefix, i));

		FBloomUpsampleCS::FParameters* P = GraphBuilder.AllocParameters<FBloomUpsampleCS::FParameters>();
		P->InputTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(Current));
		P->InputSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		P->BlendTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(BlendTarget));
		P->BlendSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
		P->OutputTexture = GraphBuilder.CreateUAV(UpTex);
		P->OutputSize = FUintVector2(OutSize.X, OutSize.Y);
		P->TexelSize = FVector2f(1.0f / InputSize.X, 1.0f / InputSize.Y);
		P->BlendWeight = W;

		const int32 GX = FMath::DivideAndRoundUp(OutSize.X, ThreadGroupSize);
		const int32 GY = FMath::DivideAndRoundUp(OutSize.Y, ThreadGroupSize);
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("%s_%d_%dx%d", DebugPrefix, i, OutSize.X, OutSize.Y),
			UpShader, P, FIntVector(GX, GY, 1));

		Current = UpTex;
	}

	return Current;
}
