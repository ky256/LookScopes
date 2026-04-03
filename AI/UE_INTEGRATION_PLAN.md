# AI 实时调色 — UE 引擎集成开发规划

> 状态: **规划中**
> 前置: 训练侧（RTX 4080 机器）正在用 Image-Adaptive-3DLUT 训练
> 本文档: 开发机上 UE 插件侧的集成方案

---

## 1. 目标

在 LookScopes 插件中集成 AI 推理能力，实现：
- 实时分析视口画面 → AI 预测调色 LUT → 自动应用到场景后处理
- 不需要 PostProcessVolume Actor（不污染场景）
- 不需要自定义渲染管线（复用 UE 原生后处理链）
- 与现有 NDI → DaVinci 工作流兼容

---

## 2. 核心架构

### 2.1 数据流全景

```
UE 渲染管线（每帧）:

  场景渲染 → 曝光 → 色调映射 → 美术 PostProcess
                                       │
                 ┌─────────────────────┤
                 │                     ↓
         GPU 缩放拷贝          AI LUT 注入 (ViewExtension)
         → 256×256 RT                  │
                 │                     ↓
                 │              最终画面 → NDI → DaVinci
                 ↓
         (异步, 按时间间隔触发, 默认 100ms)
         GPU ReadBack → CPU
                 │
                 ↓
         NNE ONNX 推理 (~1-2ms)
         → 直接输出融合后的 LUT [3, 33, 33, 33]
                 │
                 ↓
         EMA 平滑 → 写入 UVolumeTexture (33³)
         → 下一帧 ViewExtension 注入
```

### 2.2 关键设计决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 推理方式 | UE NNE (内置) | 无需外部插件，CPU 推理对小模型够快 |
| LUT 注入 | FSceneViewExtension | 不污染场景，不和美术 PPV 冲突 |
| 输入捕获 | 管线中间截取 + GPU 降采样 | 不重新渲染，避免反馈回路 |
| 驱动频率 | 固定时间间隔 (默认 100ms, 可配置) | 帧率无关，不同机器体验一致 |
| 时序平滑 | EMA (指数移动平均) | 权重缓慢变化，无闪烁 |

---

## 3. 新增模块设计

### 3.1 文件清单

```
Source/LookScopes/
├── Public/
│   ├── AIColorGrader.h           # AI 推理管理器 (含 LUT→VolumeTexture 写入)
│   └── AIGradingViewExtension.h  # 渲染管线钩子
├── Private/
│   ├── AIColorGrader.cpp
│   └── AIGradingViewExtension.cpp
```

> 注意: 原计划的 `ColorLUTProcessor` 不再需要。
> ONNX 模型已将 classifier + 基础 LUT 融合打包为一体，
> 直接输出最终 LUT `[3, 33, 33, 33]`，引擎侧只需写入 VolumeTexture。

### 3.2 FAIGradingViewExtension — 渲染管线钩子

```
职责:
1. 在色调映射之后、AI LUT 之前，截取画面降采样到 256×256 RT
2. 将 AI 生成的 LUT 纹理注入到 FSceneView::FinalPostProcessSettings
3. 判断 View 类型: 主视口注入 LUT，SceneCapture 跳过

生命周期: 随 ULookScopesSubsystem 创建/销毁
```

**关键接口**:

```cpp
// 已在 UE 5.7 引擎头文件中验证全部接口签名 (2026-04-03)
//
// SceneViewExtension.h:
//   SetupView(FSceneViewFamily&, FSceneView&)                    ✅ 存在
//   SubscribeToPostProcessingPass(Pass, View, Delegates, bool)   ✅ 存在 (5.5+ 新签名含 View)
//   EPostProcessingPass::Tonemap                                 ✅ 存在
//   FPostProcessingPassDelegate 签名:
//     FScreenPassTexture(FRDGBuilder&, const FSceneView&, const FPostProcessMaterialInputs&)  ✅
//
// Scene.h:
//   FPostProcessSettings::ColorGradingLUT      → TObjectPtr<UTexture>  ✅
//   FPostProcessSettings::ColorGradingIntensity → float                ✅
//
// SceneView.h:
//   FSceneView::bIsSceneCapture                                  ✅ 存在
//
// VolumeTexture.h:
//   UVolumeTexture::CreateTransient(SizeX, SizeY, SizeZ, Format) ✅ 存在
//
// NNE (NNERuntimeCPU.h / NNERuntimeRunSync.h / NNEModelData.h / NNE.h):
//   UE::NNE::GetRuntime<INNERuntimeCPU>(Name)                   ✅ 返回 TWeakInterfacePtr
//   INNERuntimeCPU::CreateModelCPU(UNNEModelData*)               ✅ 返回 TSharedPtr<IModelCPU>
//   IModelCPU::CreateModelInstanceCPU()                          ✅ 返回 TSharedPtr<IModelInstanceCPU>
//   IModelInstanceRunSync::RunSync(InInputs, InOutputs)          ✅ 使用 FTensorBindingCPU{void*, uint64}
//   UNNEModelData                                                ✅ UCLASS, BlueprintType

class FAIGradingViewExtension : public FSceneViewExtensionBase
{
public:
    FAIGradingViewExtension(const FAutoRegister& AutoRegister);

    // --- FSceneViewExtensionBase 接口 (签名已验证 UE 5.7) ---

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

    // UE 5.5+ 新签名 (含 const FSceneView&)，旧版已 deprecated
    virtual void SubscribeToPostProcessingPass(
        EPostProcessingPass Pass,
        const FSceneView& InView,
        FPostProcessingPassDelegateArray& InOutPassCallbacks,
        bool bIsPassEnabled) override;

    // --- 外部控制 ---

    void SetEnabled(bool bInEnabled);
    void SetIntensity(float InIntensity);
    void SetLUTTexture(UTexture* InLUT); // Scene.h 中 ColorGradingLUT 类型为 TObjectPtr<UTexture>

    // 获取截取的 256×256 预 LUT 画面 (供 AI 推理读取)
    UTextureRenderTarget2D* GetPreLUT_RT() const;

private:
    bool bEnabled = false;
    float Intensity = 1.0f;
    UTexture* LUTTexture = nullptr; // UVolumeTexture 继承自 UTexture，可向上转型
    UTextureRenderTarget2D* PreLUT_RT = nullptr; // 256×256

    // 渲染线程: 委托签名 → FScreenPassTexture(FRDGBuilder&, const FSceneView&, const FPostProcessMaterialInputs&)
    FScreenPassTexture CapturePreLUT_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessMaterialInputs& Inputs);
};
```

### ~~3.3 FColorLUTProcessor~~ (已取消)

> ONNX 模型直接输出融合后的 LUT `[3, 33, 33, 33]`，
> 不再需要引擎侧存储基础 LUT 或做融合计算。
> LUT → VolumeTexture 的写入逻辑合并到 FAIColorGrader 中。

### 3.3 FAIColorGrader — AI 推理管理器

```
职责:
1. 加载 ONNX 模型 (通过 UE NNE)
2. 定时从 ViewExtension 读取预 LUT 画面 → GPU ReadBack → CPU
3. NNE 推理 → 直接输出融合后的 3D LUT [3, 33, 33, 33]
4. 将 LUT 数据写入 UVolumeTexture (供 ViewExtension 注入)
5. EMA 时序平滑 (对 LUT 数据做逐元素平滑)

生命周期: 随 ULookScopesSubsystem 创建/销毁
驱动方式: FTSTicker 定时器 (可配置间隔，默认 100ms)
```

**实际 ONNX 模型规格 (已验证)**:

```
模型文件:
  lut_predictor_fp32.onnx  (2.4 MB, dtype=float32)
  lut_predictor_fp16.onnx  (1.2 MB, dtype=float16)

输入:
  name: "image"
  shape: [1, 3, height, width]   ← 动态尺寸! 模型内部自动缩放到 256×256
  dtype: float32 (fp32版) / float16 (fp16版)
  值域: [0, 1], NCHW 格式

输出:
  name: "lut_3d"
  shape: [3, 33, 33, 33]         ← 直接是融合后的最终 LUT!
  dtype: float32 / float16
  含义: [output_channel, R_in, G_in, B_in]
  值域: 可能 < 0 或 > 1 (训练学习的偏移)

注意:
  - 模型已将 classifier(权重预测) + 3个基础LUT融合 打包为一体
  - 引擎侧不需要存储基础 LUT，不需要做融合计算
  - 只需要: 输入图片 → ONNX 推理 → 拿到 LUT → 写入 VolumeTexture
  - Opset 17, IR version 8
```

**关键接口**:

```cpp
class FAIColorGrader
{
public:
    static constexpr int32 LUT_DIM = 33;
    static constexpr int32 LUT_TOTAL_FLOATS = 3 * 33 * 33 * 33; // 107,811

    FAIColorGrader();
    ~FAIColorGrader();

    // 初始化: 加载 ONNX 模型 + 创建 VolumeTexture
    bool Initialize(UNNEModelData* ModelData);

    // 关闭并释放资源
    void Shutdown();

    // 启用/禁用
    void SetEnabled(bool bInEnabled);
    bool IsEnabled() const;

    // 设置推理间隔 (秒, 默认 0.1s = 100ms)
    void SetInferenceInterval(float InSeconds);

    // 设置调色强度 [0, 1]
    void SetIntensity(float InIntensity);

    // 设置 EMA 平滑系数 [0, 1]，越小越平滑
    void SetSmoothingFactor(float Alpha);

    // 获取 ViewExtension (供 Subsystem 注册到渲染器)
    TSharedPtr<FAIGradingViewExtension> GetViewExtension() const;

    // 获取输出 VolumeTexture (供 ViewExtension 注入 PostProcess)
    UVolumeTexture* GetOutputLUT() const;

    // 状态信息
    float GetLastInferenceTimeMs() const;
    bool IsModelLoaded() const;

private:
    // NNE 推理 (签名已验证 UE 5.7 NNERuntimeCPU.h / NNERuntimeRunSync.h)
    // GetRuntime 返回 TWeakInterfacePtr<INNERuntimeCPU>
    // CreateModelCPU 返回 TSharedPtr<IModelCPU>
    // CreateModelInstanceCPU 返回 TSharedPtr<IModelInstanceCPU>
    // RunSync 使用 TConstArrayView<FTensorBindingCPU>{void* Data, uint64 SizeInBytes}
    TSharedPtr<UE::NNE::IModelCPU> NNEModel;
    TSharedPtr<UE::NNE::IModelInstanceCPU> NNEInstance;

    // 输出 Volume Texture (GPU)
    // 创建: UVolumeTexture::CreateTransient(33, 33, 33, PF_FloatRGBA)  ✅ 已验证
    UVolumeTexture* OutputLUT = nullptr;

    // 当前 LUT 数据 (CPU, EMA 平滑后)
    TArray<float> SmoothedLUT; // 大小 = LUT_TOTAL_FLOATS

    // 渲染钩子
    TSharedPtr<FAIGradingViewExtension> ViewExtension;

    // 推理 Tick
    bool OnInferenceTick(float DeltaTime);
    FTSTicker::FDelegateHandle TickHandle;

    // 异步 ReadBack
    void RequestReadBack();
    void OnReadBackComplete(const TArray<FColor>& Pixels, int32 Width, int32 Height);

    // 预处理: FColor[H×W] → float[1×3×H×W] (NHWC→NCHW, /255)
    TArray<float> PreprocessFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height);

    // 推理: float[1×3×H×W] → float[3×33×33×33] (最终 LUT)
    TArray<float> RunInference(const TArray<float>& InputData, int32 Height, int32 Width);

    // EMA 平滑 LUT 并写入 VolumeTexture
    void SmoothAndUpdateLUT(const TArray<float>& RawLUT);

    // LUT 格式转换: [3,33,33,33] (channel-first) → RGBA 体积纹理数据
    void WriteLUTToVolumeTexture(const TArray<float>& LUTData);

    // 状态
    bool bEnabled = false;
    bool bModelLoaded = false;
    float InferenceIntervalSeconds = 0.1f; // 100ms, 帧率无关
    float TimeSinceLastInference = 0.0f;
    float LastInferenceTimeMs = 0.0f;
    float Intensity = 1.0f;
    float SmoothingAlpha = 0.15f; // EMA: 0=不变化, 1=无平滑
};
```
```

---

## 4. 集成到现有架构

### 4.1 ULookScopesSubsystem 扩展

```cpp
// LookScopesSubsystem.h 新增:

class FAIColorGrader;

UCLASS()
class ULookScopesSubsystem : public UEditorSubsystem
{
    // ... 现有代码 ...

    /** AI 调色控制 */
    void EnableAIGrading(UNNEModelData* ModelData);
    void DisableAIGrading();
    bool IsAIGradingEnabled() const;
    void SetAIGradingIntensity(float Intensity);

    FAIColorGrader* GetAIColorGrader() const;

private:
    // ... 现有成员 ...

    /** AI 调色器 */
    TUniquePtr<FAIColorGrader> AIColorGrader;
};
```

### 4.2 SLookMatchPanel UI 扩展

在现有可折叠面板架构下新增一个区域：

```
┌─────────────────────────────────────┐
│ ▼ 视口预览                          │ ← 已有
│ ▼ 示波器                            │ ← 已有
│ ▼ NDI 推流                          │ ← 已有
│                                     │
│ ▼ AI 自动调色                        │ ← 新增
│ ┌─────────────────────────────────┐ │
│ │ 启用  [■]                       │ │
│ │ 模型  [ai_grade.onnx    ] [选择]│ │
│ │ 强度  [████████░░]  0.80        │ │
│ │ 间隔  [████░░░░░░]  100 ms      │ │
│ │ 平滑  [████░░░░░░]  0.15        │ │
│ │ ─────────────────────────────── │ │
│ │ 状态: ● 推理中                   │ │
│ │ 耗时: 1.2 ms                    │ │
│ │ 权重: [0.58, 0.31, 0.11]       │ │
│ └─────────────────────────────────┘ │
└─────────────────────────────────────┘
```

---

## 5. 依赖配置

### 5.1 插件依赖 (LookScopes.uplugin)

```json
{
    "Name": "NNE",
    "Enabled": true
},
{
    "Name": "NNERuntimeORTCpu",
    "Enabled": true
}
```

### 5.2 模块依赖 (LookScopes.Build.cs)

```csharp
PrivateDependencyModuleNames.AddRange(new string[]
{
    // ... 现有依赖 ...
    "NNE"                    // NNE 推理框架
});
```

### 5.3 资产文件

```
Content/AIGrading/                        # UE Content 目录
└── lut_predictor_fp32.onnx              # 拖入后自动变为 UNNEModelData 资产
    (或 lut_predictor_fp16.onnx 更小)
```

ONNX 文件拖入 Content 后，UE 自动创建 `UNNEModelData` 资产。
不需要额外的基础 LUT 文件 — 模型内部已包含。

---

## 6. 开发步骤

### Phase 1: 基础骨架（1.5 天）

| 步骤 | 内容 | 产出 |
|------|------|------|
| 1.1 | 配置 NNE 插件依赖 + Build.cs | 编译通过 |
| 1.2 | 实现 `FAIGradingViewExtension` 骨架 | 能注入固定颜色 LUT 验证管线 |
| 1.3 | 创建 UVolumeTexture (33³) + 手写一个测试 LUT | VolumeTexture 写入正常 |
| 1.4 | 测试: 注入 VolumeTexture 到视口 PostProcess | 视口画面变色 = 成功 |

### Phase 2: AI 推理集成（2 天）

| 步骤 | 内容 | 产出 |
|------|------|------|
| 2.1 | 实现 ViewExtension 管线中间截取 (Tonemap 后) | GPU RT 有画面 |
| 2.2 | 实现 GPU ReadBack 异步回读 | CPU 拿到像素数据 |
| 2.3 | 实现 `FAIColorGrader` NNE 加载 + 推理 | 输出 LUT [3,33,33,33] |
| 2.4 | LUT 格式转换 [3,33,33,33] → RGBA VolumeTexture | 格式正确 |
| 2.5 | 串联: 截取 → 推理 → 写 VolumeTexture → 注入 | 全链路跑通 |

### Phase 3: 时序平滑 + UI（1 天）

| 步骤 | 内容 | 产出 |
|------|------|------|
| 3.1 | EMA 对 LUT 数据逐元素平滑 | 无闪烁 |
| 3.2 | SLookMatchPanel 新增 AI 调色区域 | UI 控制 |
| 3.3 | Subsystem 生命周期管理 | 启用/禁用干净 |

### Phase 4: 测试验证（0.5 天）

| 步骤 | 内容 | 产出 |
|------|------|------|
| 4.1 | 用 lut_predictor_fp32.onnx 测试 | AI 调色生效 |
| 4.2 | NDI 推流验证（DaVinci 看到调色效果） | 端到端 |
| 4.3 | 性能测试: 推理耗时、帧率影响 | 确认 < 2ms |

**总计: ~5 天** (比原计划少 1 天 — 模型直接输出 LUT 省去了融合模块)

---

## 7. 接口约定（训练侧 ↔ 引擎侧）— 已确认

### 训练产出文件 (已提交 commit a6b4883)

| 文件 | 大小 | 说明 |
|------|------|------|
| `AI/models/lut_predictor_fp32.onnx` | 2.4 MB | ONNX 模型 (float32) |
| `AI/models/lut_predictor_fp16.onnx` | 1.2 MB | ONNX 模型 (float16, 推荐部署用) |
| `AI/models/basis_lut_0.cube` | ~1 MB | 基础 LUT 0 (仅供参考/调试，模型内已包含) |
| `AI/models/basis_lut_1.cube` | ~1 MB | 基础 LUT 1 (同上) |
| `AI/models/basis_lut_2.cube` | ~1 MB | 基础 LUT 2 (同上) |
| `AI/models_pytorch2.py` | - | 模型定义 (Classifier + TrilinearInterpolation + LUT) |
| `AI/train.py` | - | FiveK 训练脚本 |

### ONNX 模型接口 (已验证)

```
输入:
  name: "image"
  shape: [1, 3, height, width]   # NCHW, 动态尺寸
  dtype: float32 (fp32) / float16 (fp16)
  值域: [0, 1]
  色彩空间: sRGB (归一化到 0~1)
  注意: 模型内部有 nn.Upsample(size=(256,256)) 自动缩放

输出:
  name: "lut_3d"
  shape: [3, 33, 33, 33]         # [output_channel, R_in, G_in, B_in]
  dtype: float32 / float16
  含义: 融合后的最终 3D LUT (classifier预测权重 × 3基础LUT 已在模型内完成)
  值域: 不限于 [0,1]，可能有负值 (训练学习的偏移)

Opset: 17
IR version: 8
```

### 重要实现细节

1. **权重未做 softmax** — 训练用 L2 正则约束权重大小，不是 softmax 归一化
2. **LUT 数据格式为 channel-first** `[C, R, G, B]` — 写入 VolumeTexture 时需转为引擎期望的格式
3. **LUT 值可以超出 [0,1]** — 应用后需 clamp
4. **输入接受任意分辨率** — 模型自动缩放到 256×256，降采样可在 GPU 侧做也可直接送原始尺寸
5. **.cube 文件仅供调试** — 实际推理不需要加载基础 LUT，全部在 ONNX 内完成

---

## 8. 与现有系统的关系

```
ULookScopesSubsystem
├── FScopeSessionManager          # 已有: 示波器分析
│   ├── FViewportCapture          # 已有: CPU 像素捕获
│   ├── FGPUScopeRenderer         # 已有: GPU 示波器渲染
│   └── IScopeAnalyzer[]          # 已有: CPU 分析模块
├── FViewportStreamer              # 已有: NDI 推流
└── FAIColorGrader                # 新增: AI 调色
    ├── FAIGradingViewExtension   # 新增: 渲染钩子 (截取+注入)
    └── FColorLUTProcessor        # 新增: LUT 融合
```

AI 调色模块与示波器分析模块**完全独立**，互不干扰。
示波器读的是最终画面（包含 AI 调色后的效果），可以用来验证 AI 效果。

---

## 9. 后续扩展（不在本期范围）

- [ ] GPU 推理 (NNERuntimeORTDml) — 进一步降低延迟
- [ ] GRU 时序模型 — 替换 CNN，加入 hidden state 管理
- [ ] 风格选择 UI — 多风格条件输入
- [ ] Runtime 支持 — 从 Editor-only 扩展到游戏运行时
- [ ] Compute Shader LUT 融合 — 从 CPU 融合迁移到 GPU
