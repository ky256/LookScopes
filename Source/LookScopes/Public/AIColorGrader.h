// Copyright KuoYu. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ViewportCapture.h"

class FAIGradingViewExtension;
class UTexture2D;
class UNNEModelData;

namespace UE::NNE
{
	class IModelCPU;
	class IModelInstanceCPU;
	class IModelGPU;
	class IModelInstanceGPU;
	class IModelInstanceRunSync;
}

/**
 * FAIColorGrader - AI 推理管理器
 *
 * 完整管线:
 *   视口捕获 → 预处理(NCHW) → NNE ONNX 推理 → EMA 平滑 → UVolumeTexture → ViewExtension 注入
 *
 * 生命周期由 ULookScopesSubsystem 管理。
 */
class LOOKSCOPES_API FAIColorGrader
{
public:
	static constexpr int32 MODEL_LUT_DIM = 33;
	static constexpr int32 MODEL_LUT_FLOATS = 3 * MODEL_LUT_DIM * MODEL_LUT_DIM * MODEL_LUT_DIM;
	static constexpr int32 OUTPUT_LUT_DIM = 16;
	static constexpr int32 STRIP_WIDTH = OUTPUT_LUT_DIM * OUTPUT_LUT_DIM;
	static constexpr int32 STRIP_HEIGHT = OUTPUT_LUT_DIM;

	FAIColorGrader();
	~FAIColorGrader();

	/**
	 * 初始化: 加载 ONNX 模型 + 创建 VolumeTexture + 注册 ViewExtension
	 * @param OnnxModelPath 磁盘上 .onnx 文件路径 (为空则自动搜索插件目录)
	 * @return 是否成功初始化
	 */
	bool Initialize(const FString& OnnxModelPath = FString());

	/** 关闭并释放所有资源 */
	void Shutdown();

	// --- 启停控制 ---

	void SetEnabled(bool bInEnabled);
	bool IsEnabled() const { return bEnabled; }

	// --- 参数调节 ---

	/** 推理间隔 (秒, 默认 0.1s = 100ms) */
	void SetInferenceInterval(float InSeconds);
	float GetInferenceInterval() const { return InferenceInterval; }

	/** 调色强度 [0, 1] */
	void SetIntensity(float InIntensity);
	float GetIntensity() const { return Intensity; }

	/** EMA 平滑系数 [0, 1]，越大越跟手，越小越平滑 */
	void SetSmoothingFactor(float Alpha);
	float GetSmoothingFactor() const { return SmoothingAlpha; }

	// --- 手动触发 ---

	/** 捕获当前帧并推理一次（忽略定时器） */
	void InferOnce();

	// --- 状态查询 ---

	bool IsModelLoaded() const { return bModelLoaded; }
	float GetLastInferenceTimeMs() const { return LastInferenceTimeMs; }
	int32 GetTotalInferenceCount() const { return TotalInferenceCount; }

	/** 获取 ViewExtension */
	TSharedPtr<FAIGradingViewExtension> GetViewExtension() const { return ViewExtension; }

	// 不可拷贝
	FAIColorGrader(const FAIColorGrader&) = delete;
	FAIColorGrader& operator=(const FAIColorGrader&) = delete;

private:
	// --- NNE ---
	bool LoadONNXModel(const FString& ModelPath);
	FString FindDefaultModelPath() const;

	TObjectPtr<UNNEModelData> NNEModelData;
	TSharedPtr<UE::NNE::IModelGPU> NNEModelGPU;
	TSharedPtr<UE::NNE::IModelCPU> NNEModelCPU;
	TSharedPtr<UE::NNE::IModelInstanceRunSync> NNEInstance;
	bool bUsingGPU = false;

	// --- LUT Texture (2D strip: 256x16, PF_B8G8R8A8) ---
	bool CreateLUTTexture();
	void UpdateLUTTexture();
	void DestroyLUTTexture();
	float SampleLUT33(int32 Channel, float R, float G, float B) const;

	UTexture2D* OutputLUT = nullptr;

	// --- ViewExtension ---
	TSharedPtr<FAIGradingViewExtension> ViewExtension;

	// --- 推理管线 ---
	void ProcessCapturedFrame(const FViewportCaptureResult& Capture);
	TArray<float> PreprocessFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height);
	bool RunNNEInference(const TArray<float>& InputTensor, int32 Height, int32 Width, TArray<float>& OutLUT);
	void ApplyAndUpdateLUT(const TArray<float>& RawLUT);
	void WriteIdentityLUT();

	// --- Tick ---
	bool OnTick(float DeltaTime);
	FTSTicker::FDelegateHandle TickHandle;

	// --- 帧捕获 ---
	FViewportCapture FrameCapture;

	// --- LUT 数据 (CPU) ---
	TArray<float> SmoothedLUT;
	bool bFirstLUT = true;

	// --- 状态 ---
	bool bEnabled = false;
	bool bModelLoaded = false;
	bool bInitialized = false;
	float InferenceInterval = 0.1f;
	float TimeSinceLastInference = 0.0f;
	float LastInferenceTimeMs = 0.0f;
	float Intensity = 1.0f;
	float SmoothingAlpha = 0.15f;
	int32 TotalInferenceCount = 0;
};
