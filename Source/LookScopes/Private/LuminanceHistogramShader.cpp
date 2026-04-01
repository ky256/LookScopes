// Copyright KuoYu. All Rights Reserved.

#include "LuminanceHistogramShader.h"

// 注册 Global Shader
// Shader 路径使用插件注册的虚拟目录映射
IMPLEMENT_GLOBAL_SHADER(
	FLuminanceHistogramCS,
	"/Plugin/LookScopes/LuminanceHistogram.usf",
	"MainCS",
	SF_Compute
);
