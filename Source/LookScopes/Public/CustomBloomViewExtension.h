// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "BloomRenderer.h"

class FCustomBloomViewExtension : public FSceneViewExtensionBase
{
public:
	FCustomBloomViewExtension(const FAutoRegister& AutoRegister);

	// --- FSceneViewExtensionBase ---
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs) override;
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

	// --- Public API (Game Thread) ---
	void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
	bool IsEnabled() const { return bEnabled; }

	void SetBloomParams(const FCustomBloomParams& InParams) { BloomParams = InParams; }
	const FCustomBloomParams& GetBloomParams() const { return BloomParams; }

	void SetSceneBloomIntensity(float V) { BloomParams.SceneBloomIntensity = FMath::Max(V, 0.0f); }
	void SetSceneBloomThreshold(float V) { BloomParams.SceneBloomThreshold = FMath::Max(V, 0.0f); }
	void SetVFXBloomIntensity(float V) { BloomParams.VFXBloomIntensity = FMath::Max(V, 0.0f); }
	void SetVFXBloomThreshold(float V) { BloomParams.VFXBloomThreshold = FMath::Max(V, 0.0f); }
	void SetBloomLevels(int32 V) { BloomParams.BloomLevels = FMath::Clamp(V, 3, 6); }
	void SetBloomScatter(float V) { BloomParams.Scatter = FMath::Clamp(V, 0.0f, 1.0f); }
	void SetMaxBrightness(float V) { BloomParams.MaxBrightness = FMath::Max(V, 0.0f); }
	void SetBloomTint(const FLinearColor& C) { BloomParams.BloomTint = C; }

private:
	bool bEnabled = false;
	FCustomBloomParams BloomParams;
	FBloomRenderer BloomRenderer;
};
