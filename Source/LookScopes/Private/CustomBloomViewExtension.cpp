// Copyright KuoYu. All Rights Reserved.

#include "CustomBloomViewExtension.h"
#include "SceneView.h"
#include "PostProcess/PostProcessInputs.h"
#include "TranslucentPassResource.h"
#include "RenderGraphBuilder.h"

FCustomBloomViewExtension::FCustomBloomViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FCustomBloomViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	if (!bEnabled) return;
	if (InView.bIsSceneCapture) return;

	InView.FinalPostProcessSettings.BloomIntensity = 0.0f;
	InViewFamily.EngineShowFlags.SetBloom(false);
}

bool FCustomBloomViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return bEnabled;
}

void FCustomBloomViewExtension::PrePostProcessPass_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& InView,
	const FPostProcessingInputs& Inputs)
{
	if (!bEnabled) return;
	if (InView.bIsSceneCapture) return;

	FRDGTextureRef SceneColor = (*Inputs.SceneTextures)->SceneColorTexture;
	if (!SceneColor) return;

	const auto& AfterDOF = Inputs.TranslucencyViewResourcesMap.Get(ETranslucencyPass::TPT_TranslucencyAfterDOF);
	FRDGTextureRef TranslucentColor = AfterDOF.GetColorForRead(GraphBuilder);

	BloomRenderer.Render(GraphBuilder, SceneColor, TranslucentColor,
		InView.UnscaledViewRect, BloomParams);
}
