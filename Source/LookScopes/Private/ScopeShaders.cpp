// Copyright KuoYu. All Rights Reserved.

#include "ScopeShaders.h"

// ============================================================
// 波形图 Shader 注册
// ============================================================

IMPLEMENT_GLOBAL_SHADER(
	FWaveformAccumulateCS,
	"/Plugin/LookScopes/WaveformScope.usf",
	"AccumulateCS",
	SF_Compute
);

IMPLEMENT_GLOBAL_SHADER(
	FWaveformVisualizeCS,
	"/Plugin/LookScopes/WaveformScope.usf",
	"VisualizeCS",
	SF_Compute
);

// ============================================================
// AI 降采样 Shader 注册
// ============================================================

IMPLEMENT_GLOBAL_SHADER(
	FAIDownsampleCS,
	"/Plugin/LookScopes/AIDownsample.usf",
	"MainCS",
	SF_Compute
);

// ============================================================
// 直方图可视化 Shader 注册
// ============================================================

IMPLEMENT_GLOBAL_SHADER(
	FHistogramVisualizerCS,
	"/Plugin/LookScopes/HistogramVisualizer.usf",
	"MainCS",
	SF_Compute
);

// ============================================================
// Buffer 最大值归约 Shader 注册
// ============================================================

IMPLEMENT_GLOBAL_SHADER(
	FBufferMaxReduceCS,
	"/Plugin/LookScopes/BufferMaxReduce.usf",
	"MainCS",
	SF_Compute
);
