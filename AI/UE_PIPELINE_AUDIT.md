# UE 推理管线诊断工单

> **文档用途**: 训练机对 UE 侧 AI 调色推理管线做的静态代码审查结论 + 待开发机验证的问题清单。
> **目标读者**: 开发机（UE 工程机）上的 AI 助理。
> **执行要求**: 按优先级逐项验证，将结论回填到本文件末尾的"回报区"。
> **前置状态**: Phase 1 FiveK 基线模型已训练完成，已通过 ONNX 接入 UE 实测运行；肉眼观察"颜色调得不准"，但训练侧 PSNR、Python/UE 一致性均未验证。

---

## 0. 任务说明

训练机读了 UE 侧以下 4 个文件做了静态审查，按"5 个常见色彩管线坑"逐条对照：

```
Source/LookScopes/Private/AIColorGrader.cpp
Source/LookScopes/Private/AIGradingViewExtension.cpp
Source/LookScopes/Public/AIColorGrader.h
Shaders/AIDownsample.usf
```

审查结论：5 个坑里 4 个已正确避开。但发现 **2 个高嫌疑根因** + **2 个低优先级问题**。本工单要求开发机对这 4 条做动态验证（跑 UE、看 log、必要时改代码实测），然后根据证据决定修复方案。

**验证原则**:
- 任何改动前，先用 log / 实测收集证据。不要凭推理改代码。
- 每一项都有明确的"判定标准"；按标准做判定，不要凭感觉。
- 不确定的项目，标记为 `待人工确认` 而不是强行下结论。

---

## 1. 问题清单（按优先级）

### P0 — `PreExposure` 除法方向可能是错的（高嫌疑）

**疑似症状**: 画面整体偏色、对比度/亮度不符合预期；明亮场景和昏暗场景表现不对称。

**代码位置**:

```91:92:Source/LookScopes/Private/AIGradingViewExtension.cpp
	const float EyeExp = View.GetLastEyeAdaptationExposure();
	const float PreExposure = EyeExp > 0.0f ? EyeExp : 1.0f;
```

```182:182:Source/LookScopes/Private/AIGradingViewExtension.cpp
	DispatchDown(DownRDG, AI_SIZE, AI_SIZE, 1.0f / PreExposure);
```

Shader 侧的使用（`Shaders/AIDownsample.usf` L42-47）：

```hlsl
if (ExposureScale > 0.0f)
{
    Color.rgb = Color.rgb * ExposureScale;  // ExposureScale = 1.0 / PreExposure
    Color.rgb = ACESFilm(Color.rgb);
    Color.rgb = LinearToSRGB(Color.rgb);
}
```

**疑点**: 代码订阅的是 `EPostProcessingPass::MotionBlur` 之后的回调拿 SceneColor。问题：**在 MotionBlur 之后的 SceneColor 里，`EyeAdaptationExposure` 到底乘进去了没有**？

- 如果**没有乘进去**（EyeAdaptation 在 Tonemap pass 里才乘），那 `Color * (1/EyeExp)` 就是在"把本不该除的东西除掉"，会把亮场景变暗、暗场景变亮 → 模型看到的亮度分布与实际观感**反向**，预测的 LUT 自然调反方向
- 如果**已经乘进去了**，那代码是对的，是在"撤销"以回到相机 linear 空间

UE 的 PreExposure / EyeAdaptation 机制在 5.x 的不同小版本上行为不完全一致，不能凭记忆判断，必须查证当前项目用的 UE 版本的行为。

**验证步骤**（按顺序做，任何一步能确认就停）:

1. **查证 UE 版本和 PreExposure 机制**:
   - 查 `LookScopes.uproject` 和 Engine 版本号
   - 用 ripgrep 查 UE 源码（不在本仓库、在引擎源码里）:
     - `rg -i "SceneColor.*PreExposure" Engine/Source/Runtime/Renderer/Private/PostProcess/` 或类似
     - 关注 `PostProcessTonemap.cpp`、`SceneTextureParameters.cpp`
   - 确认 MotionBlur pass 之后、Tonemap pass 之前的 SceneColor 是否已乘 PreExposure

2. **运行时 log 取证**（不改代码，只看数据）:
   - 启用 AI 调色，在不同光照场景（室内昏暗 / 室外明亮 / 夜景）切换
   - 捕获以下 log 行：
     ```
     LogAICapture: 捕获链 #N: tex=... view=...(fmt=...,preExp=X.XXX) → ... | 均值 R=.. G=.. B=.. | 范围 [min, max]
     ```
   - 重点关注**不同场景下 `preExp` 的值**，和对应的 R/G/B 均值
   - **判定信号**:
     - 如果明亮场景 `preExp > 1.0` 且输出均值被压到很低（例如 < 60）→ 除法把画面压暗了，方向错
     - 如果昏暗场景 `preExp < 1.0` 且输出均值被推到很高（例如 > 200）→ 除法把画面推亮了，方向错
     - 如果不同 `preExp` 下输出均值都稳定落在 90~180 → 除法方向对

3. **对照实测**（如前两步仍有疑义）:
   - 临时把 L182 的 `1.0f / PreExposure` 改成 `1.0f`
   - 在同一场景下对比两种设置的最终画面截图
   - 如果改成 `1.0f` 后画面明显更接近预期 → 原除法是错的

**判定标准**:

| 证据 | 结论 |
|---|---|
| 步骤 1 查到 UE 源码明确未乘 PreExposure + 步骤 2 不同场景均值差异大 | **确认错**，改成 `1.0f` |
| 步骤 1 查到已乘 + 步骤 2 各场景均值稳定 | **正确**，保留 |
| 步骤 3 改成 1.0f 画面明显更好 | **改**，不管步骤 1 结论 |
| 证据矛盾或不清 | 标记 `待人工确认`，优先拉项目负责人看 |

**修复方案（如需改）**:
- 把 L182 的 `1.0f / PreExposure` 改成 `1.0f`
- 同时可以考虑不再调用 `GetLastEyeAdaptationExposure()`，简化为常量
- 改完后**必须重跑实测 1**，新数据回报

---

### P1 — 训练集 vs UE 运行时分布不一致（高嫌疑，修复最复杂）

**疑似症状**: 即使 P0 修好，画面可能仍偏色，因为模型训练时"见过的输入分布"和运行时"被喂的分布"不是同一种 sRGB。

**背景**:
- **训练集 FiveK**: 数码相机拍摄 + ISP 处理 + 专业摄影师调色后的 sRGB JPG/TIFF。典型特征：中间调偏暗、对比较高、饱和度自然。
- **UE 运行时**: Linear HDR scene color → ACES tonemap → LinearToSRGB gamma → 输出 8-bit sRGB。典型特征：ACES 的 S 曲线让**中间调偏亮、高光软压缩**，整体对比度比消费级照片低。

两种 sRGB 在**像素分布统计**上差距很大。小 CNN Classifier 对输入分布敏感，在 FiveK 上学到的决策规则（看到什么样的图就预测什么样的 LUT 权重）在 ACES 后的游戏画面上可能输出反向或过度的权重。

**相关代码**:

```176:182:Source/LookScopes/Private/AIGradingViewExtension.cpp
	FRDGTextureRef DownRDG = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(FIntPoint(AI_SIZE, AI_SIZE), PF_R8G8B8A8,
			FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("AIDown_Final"));

	DispatchDown(DownRDG, AI_SIZE, AI_SIZE, 1.0f / PreExposure);
```

Shader 管线（`AIDownsample.usf`）做的是 `linear → ACES → sRGB`。

**验证步骤**:

1. **导出 UE 侧实际喂给 ONNX 的 256×256 输入图**:
   - 在 `AIGradingViewExtension.cpp` 的 readback 回调（L194 附近）里，额外把 `Pixels` 用 `FImageUtils::CompressImageArray` 保存成 PNG
   - 保存位置：`<Project>/Saved/AIDebug/ue_input_<timestamp>.png`
   - 只需要保存一次（加个 `static bool bSaved = false;` guard），不要每帧保存
   - 选 2~3 张典型场景（明亮户外 / 昏暗室内 / 有天空的场景）各导出一张

2. **对比训练集样本**:
   - 从 FiveK 数据集（训练机上 `C:\Users\THINKSTATION\Desktop\data\fiveK\fiveK`，该数据需要向训练机索取，或本机自行下载）随机选 5~10 张 input 图
   - 把 UE 导出的图和 FiveK input 图**并排肉眼对比**
   - 关注:
     - 整体亮度水平（UE 图是不是明显更亮/更平？）
     - 对比度（UE 图是不是看起来更"雾"？）
     - 饱和度（ACES 后色彩是否偏淡？）

3. **定量对比直方图**（可选但强烈推荐）:
   - 写一个 `scripts/histogram_compare.py`（或让训练机写，开发机调用）:
     - 输入：UE 导出的 PNG 目录 + FiveK input PNG 目录
     - 输出：RGB 通道均值/方差 / 亮度直方图对比图
   - 关键指标：UE 图的**均值**和 FiveK input 均值是否差异 > 20（0-255 尺度）

**判定标准**:

| 证据 | 结论 |
|---|---|
| UE 图明显更亮且均值差 > 30 | 分布偏移显著，需要训练侧介入 |
| 肉眼难分辨，均值差 < 10 | 分布基本对齐，P1 不是瓶颈 |
| 有天空 / 高光区域明显 ACES S 曲线特征 | 分布偏移存在，但严重程度看场景 |

**修复方向**（不在本工单执行，只做判定，训练侧决定）:

方向 A — **训练侧做 ACES 预处理**（推荐）:
- 在 `train.py` 的 dataloader 里，对 input 图先做 `sRGB → linear → ACES → sRGB` 往返，模拟 UE 的预处理
- 这样模型在"ACES 处理过的分布"上训练，运行时分布对齐

方向 B — **UE 侧改用简单 sRGB 编码**:
- 去掉 `AIDownsample.usf` 里的 `ACESFilm()` 调用
- 直接 `LinearToSRGB(Color * exposure)` 出去
- 简单但会让"游戏 HDR → sRGB"的色彩更生硬

方向 C — **用 UE 渲染的数据做训练**:
- 从 UE 导出 1000+ 张不同场景的 pre-tonemap sRGB 图
- 用 LUT 批量造 (原图, 调色图) 配对
- 训练出的模型天然在 UE 分布上对齐

开发机把方向选择**回报给训练机**，由训练机决定，不要自行在训练数据上动手。

---

### P2 — LUT 分辨率从 33 降到 16 有精度损失（低优先级）

**疑似症状**: 在天空、皮肤、灰阶渐变区域可能出现细微色带（banding），但不是颜色偏色的主因。

**代码位置**:

```33:37:Source/LookScopes/Public/AIColorGrader.h
	static constexpr int32 MODEL_LUT_DIM = 33;
	static constexpr int32 MODEL_LUT_FLOATS = 3 * MODEL_LUT_DIM * MODEL_LUT_DIM * MODEL_LUT_DIM;
	static constexpr int32 OUTPUT_LUT_DIM = 16;
	static constexpr int32 STRIP_WIDTH = OUTPUT_LUT_DIM * OUTPUT_LUT_DIM;
	static constexpr int32 STRIP_HEIGHT = OUTPUT_LUT_DIM;
```

模型输出 33 网格 LUT，UE 输出 strip 是 16 网格，通过 `SampleLUT33` 做 33→16 三线性重采样（`AIColorGrader.cpp` L267-307）。

UE 的 `FPostProcessSettings::PushLUT` 默认接受 256×16 unwrapped LUT（16 网格）。理论上可以通过 `UTexture3D` + 自定义 shader 走更高精度，但工程复杂度高。

**验证步骤**:

1. 开 AI 调色后，切到有**大片单色渐变**的场景（傍晚天空、人脸特写）
2. 截图放大 400% 目视检查是否有 banding
3. 对比关闭 AI 调色的同场景截图，banding 是否由 AI 调色引入

**判定标准**:
- 肉眼看不到 banding → 不是问题，搁置
- 肉眼可见 banding 但仅在极端场景 → 搁置，等 P0/P1 修完再评估
- 常见场景就有 banding → 需要升级到 32 网格 `UTexture3D`（另起工单）

**修复**（仅当判定为问题时做，本工单不做）:
- 方案 A: 把 `OUTPUT_LUT_DIM` 改成 32，改用 `PF_FloatRGBA` 512×32 strip
- 方案 B: 改用 `UVolumeTexture`，自行写注入 shader 替代 `PushLUT`
- 方案 B 破坏"使用 UE 原生 LUT 注入管线"的简单性，需要权衡

---

### P3 — CPU 下采样用最近邻（最低优先级）

**疑似症状**: 仅在调用 `InferOnce()` 这个手动触发路径时生效，日常 Tick 走 GPU 不受影响。

**代码位置**:

```520:528:Source/LookScopes/Private/AIColorGrader.cpp
	for (int32 y = 0; y < DstH; y++)
	{
		const int32 sy = y * SrcH / DstH;
		const int32 SrcRow = sy * SrcW;
		for (int32 x = 0; x < DstW; x++)
		{
			Dst[y * DstW + x] = Src[SrcRow + x * SrcW / DstW];
		}
	}
```

这是**最近邻**采样（nearest-neighbor），不是双线性。训练时数据预处理用的是 PIL 的 BILINEAR（详见训练机 `train.py`）。如果 `InferOnce()` 是调试入口，两种路径输出的 LUT 会有可测量差异。

**验证步骤**:

1. 确认 `InferOnce()` 是否还在被调用（搜 `.cpp` 里的所有调用点）
2. 如果只是 UI 调试按钮触发，无日常影响 → 搁置
3. 如果是某个常用路径 → 改成双线性（简单几行代码）

**修复**（如需）:
```cpp
// 简单双线性
for (int32 y = 0; y < DstH; y++)
{
    const float sy = (y + 0.5f) * SrcH / DstH - 0.5f;
    const int32 y0 = FMath::Clamp((int32)FMath::FloorToFloat(sy), 0, SrcH - 1);
    const int32 y1 = FMath::Min(y0 + 1, SrcH - 1);
    const float fy = sy - y0;
    for (int32 x = 0; x < DstW; x++)
    {
        const float sx = (x + 0.5f) * SrcW / DstW - 0.5f;
        const int32 x0 = FMath::Clamp((int32)FMath::FloorToFloat(sx), 0, SrcW - 1);
        const int32 x1 = FMath::Min(x0 + 1, SrcW - 1);
        const float fx = sx - x0;
        const FColor c00 = Src[y0 * SrcW + x0];
        const FColor c01 = Src[y0 * SrcW + x1];
        const FColor c10 = Src[y1 * SrcW + x0];
        const FColor c11 = Src[y1 * SrcW + x1];
        // 按通道线性插值，自行实现
        // ...
    }
}
```

---

## 2. 额外建议做的两项静态核对（不修代码，只确认）

### 2.1 确认 ONNX 模型输入 / 输出约定

用 Python + `onnx` 库检查当前 `AI/models/lut_predictor_fp32.onnx`:

```python
import onnx
m = onnx.load("AI/models/lut_predictor_fp32.onnx")
for inp in m.graph.input:
    print("IN:", inp.name, [d.dim_value for d in inp.type.tensor_type.shape.dim])
for out in m.graph.output:
    print("OUT:", out.name, [d.dim_value for d in out.type.tensor_type.shape.dim])
```

核对:
- 输入 shape 是否为 `[1, 3, 256, 256]` （即 NCHW，与 `RunNNEInference` L467 的 `Dims = {1, 3, H, W}` 一致）
- 输出 shape 是否为 `[3, 33, 33, 33]`（与 `MODEL_LUT_FLOATS` 一致）
- 输入**值域**约定（预期 0~1 float，与 `PreprocessFrame` L452 的 `Inv = 1.0f/255.0f` 一致）

**如果任何一条不匹配 → 是严重 bug，立即标记 P0**。

### 2.2 确认 `PushLUT` 在当前 UE 版本的输入色彩空间

在当前项目用的 UE 版本里，`FPostProcessSettings::PushLUT` 期望的 LUT 输入空间是什么？从 UE 5.2 起，`CombineLUTs.usf` 里 LUT 的输入是 log-encoded Working Color Space，不是直接 sRGB。具体行为因版本和项目 Working Color Space 设置而异。

核对步骤:
1. 查 `LookScopes.uproject` 的 Engine 版本
2. 在引擎源码查 `Engine/Shaders/Private/PostProcessCombineLUTs.usf`
3. 确认 LUT 采样坐标是否经过 `LinToLog()` 之类的变换
4. 如果是：训练时 LUT 是 `sRGB_in → sRGB_out`，UE 却按 `log_in → display_out` 用 → **又是一个分布偏移，和 P1 叠加**

**判定**:
- 确认 UE 版本 ≥ 5.2 且 `CombineLUTs` 用 log 编码 → 在回报区标记 `P1+` 追加分支，训练侧需要调整 LUT 学习的输入编码
- 如果 `CombineLUTs` 仍按 sRGB 采样 → 没问题

---

## 3. 执行顺序

```
Step 1: 静态核对 2.1（2 分钟）—— 排除 ONNX 约定 bug
Step 2: 静态核对 2.2（15 分钟）—— 排除 PushLUT 色彩空间 bug
Step 3: P0 验证（30~60 分钟）
         └─ 分步骤 1 → 2 → 3，能早停就早停
Step 4: 如果 P0 改了代码，重新跑一次 log 实测，观察画面是否改善
Step 5: P1 验证（30 分钟）—— 主要是导图 + 肉眼对比
Step 6: P2、P3 —— 只需简短判断是否是当前痛点，不要深入
Step 7: 回报
```

**时间预算**: 全程 2~3 小时。超过 3 小时还没结论的项目，标 `待人工确认` 跳过。

---

## 4. 回报格式

把结果追加到本文件末尾的**回报区**（section 5），不要开新文件。训练机会拉取更新后的本文件读取结论。

每一项按下面的模板填：

```markdown
### <P0/P1/P2/P3/静态核对 X> 结论

- **判定**: <确认是问题 / 不是问题 / 待人工确认>
- **关键证据**:
  - <指标 1: 具体数值或截图路径>
  - <指标 2: ...>
- **已做的改动**: <列出改了哪些文件哪些行；如果没改写"无">
- **需要训练机配合的事**: <如果需要训练机介入，具体说；否则写"无">
- **残留风险**: <改完之后还没解决的问题>
```

---

## 5. 回报区（开发机填写）

<!-- 开发机在下方追加结论 -->

### 静态核对 2.1 结论

待填。

### 静态核对 2.2 结论

待填。

### P0 结论

待填。

### P1 结论

待填。

### P2 结论

待填。

### P3 结论

待填。

### 总结与下一步建议

待填。
