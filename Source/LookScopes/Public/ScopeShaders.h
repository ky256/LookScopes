// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"

// ============================================================
// 波形图 Compute Shader - Pass 1: 密度累加
// ============================================================

/**
 * FWaveformAccumulateCS - 波形图密度累加 Compute Shader
 * 
 * 读取输入纹理，计算 BT.709 亮度，原子累加到密度 Buffer。
 * 每个线程组处理一列输出。
 */
class FWaveformAccumulateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FWaveformAccumulateCS);
	SHADER_USE_PARAMETER_STRUCT(FWaveformAccumulateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, DensityBuffer)
		SHADER_PARAMETER(FUintVector2, InputSize)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(FVector3f, LuminanceWeights)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 GroupSize = 256;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ACCUMULATE_GROUP_SIZE"), GroupSize);
	}
};

// ============================================================
// 波形图 Compute Shader - Pass 2: 可视化渲染
// ============================================================

/**
 * FWaveformVisualizeCS - 波形图可视化 Compute Shader
 * 
 * 读取密度 Buffer，渲染为荧光绿色调的输出纹理。
 */
class FWaveformVisualizeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FWaveformVisualizeCS);
	SHADER_USE_PARAMETER_STRUCT(FWaveformVisualizeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, DensityBufferSRV)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(uint32, TotalInputPixels)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSizeX = 16;
	static constexpr int32 ThreadGroupSizeY = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VISUALIZE_GROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("VISUALIZE_GROUP_SIZE_Y"), ThreadGroupSizeY);
	}
};

// ============================================================
// GPU Buffer 最大值归约 Compute Shader
// ============================================================

/**
 * FBufferMaxReduceCS - GPU 并行归约求最大值
 * 
 * 对 Buffer<uint> 执行并行归约，输出最大值到 1 元素 Buffer。
 * 替代 CPU 侧的启发式估算，确保密度/直方图归一化精确。
 */
class FBufferMaxReduceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBufferMaxReduceCS);
	SHADER_USE_PARAMETER_STRUCT(FBufferMaxReduceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InputBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, MaxValueOutput)
		SHADER_PARAMETER(uint32, BufferLength)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 GroupSize = 256;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("REDUCE_GROUP_SIZE"), GroupSize);
	}
};

// ============================================================
// AI 降采样 Compute Shader
// ============================================================

class FAIDownsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAIDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FAIDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(float, ExposureScale)
		SHADER_PARAMETER(FVector2f, UVOffset)
		SHADER_PARAMETER(FVector2f, UVScale)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ============================================================
// AI 降采样 Readback 参数（声明 RDG 纹理依赖）
// ============================================================

BEGIN_SHADER_PARAMETER_STRUCT(FAIReadbackParameters, )
	RDG_TEXTURE_ACCESS(DownsampledTexture, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

// ============================================================
// 直方图可视化 Compute Shader
// ============================================================

/**
 * FHistogramVisualizerCS - 直方图可视化 Compute Shader
 * 
 * 读取 256-bin 直方图 Buffer，渲染为带颜色渐变的柱状图纹理。
 */
class FHistogramVisualizerCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHistogramVisualizerCS);
	SHADER_USE_PARAMETER_STRUCT(FHistogramVisualizerCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HistogramBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MaxBinBuffer)
		SHADER_PARAMETER(uint32, TotalInputPixels)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSizeX = 16;
	static constexpr int32 ThreadGroupSizeY = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSizeY);
		OutEnvironment.SetDefine(TEXT("HISTOGRAM_BIN_COUNT"), 256);
	}
};

// ============================================================
// Bloom Downsample Compute Shader (Dual Kawase)
// ============================================================

class FBloomDownsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBloomDownsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FBloomDownsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(FVector2f, TexelSize)
		SHADER_PARAMETER(float, Threshold)
		SHADER_PARAMETER(float, MaxBrightness)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ============================================================
// Bloom Upsample Compute Shader (Dual Kawase)
// ============================================================

class FBloomUpsampleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBloomUpsampleCS);
	SHADER_USE_PARAMETER_STRUCT(FBloomUpsampleCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, BlendTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, BlendSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(FVector2f, TexelSize)
		SHADER_PARAMETER(float, BlendWeight)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ============================================================
// Bloom Temporal Blend Compute Shader
// ============================================================

class FBloomTemporalBlendCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBloomTemporalBlendCS);
	SHADER_USE_PARAMETER_STRUCT(FBloomTemporalBlendCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, CurrentTex)
		SHADER_PARAMETER_SAMPLER(SamplerState, CurrentSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, HistoryTex)
		SHADER_PARAMETER_SAMPLER(SamplerState, HistorySampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(float, BlendWeight)
		SHADER_PARAMETER(FVector2f, TexelSize)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

// ============================================================
// Bloom Composite Compute Shader
// ============================================================

class FBloomCompositeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBloomCompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FBloomCompositeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneColorTex)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneBloomTex)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneBloomSampler)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, VFXBloomTex)
		SHADER_PARAMETER_SAMPLER(SamplerState, VFXBloomSampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FUintVector2, OutputSize)
		SHADER_PARAMETER(float, SceneBloomIntensity)
		SHADER_PARAMETER(float, VFXBloomIntensity)
		SHADER_PARAMETER(FVector3f, BloomTint)
		SHADER_PARAMETER(FVector2f, BloomUVScale)
		SHADER_PARAMETER(FVector2f, VFXBloomUVScale)
		SHADER_PARAMETER(uint32, DebugMode)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};
