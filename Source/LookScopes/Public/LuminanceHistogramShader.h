// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"

/**
 * 亮度直方图 Compute Shader 声明
 * 
 * 在 GPU 上统计输入纹理的亮度分布，输出 256 个 bin 的直方图。
 * 每个 bin 对应 [i/256, (i+1)/256) 的亮度区间。
 */
class FLuminanceHistogramCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLuminanceHistogramCS);
	SHADER_USE_PARAMETER_STRUCT(FLuminanceHistogramCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, HistogramOutput)
		SHADER_PARAMETER(FUintVector2, TextureSize)
		SHADER_PARAMETER(FVector3f, LuminanceWeights)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr int32 ThreadGroupSize = 16;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_Y"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("HISTOGRAM_BIN_COUNT"), 256);
	}
};
