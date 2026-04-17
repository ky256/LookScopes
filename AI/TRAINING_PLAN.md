# AI 实时调色模型 — 训练规划

> 状态: **规划中**
> 训练硬件: RTX 4080 (16GB VRAM)
> 部署目标: UE 5.x 实时推理 (NNE / ONNX Runtime)

---

## 1. 目标

训练一个轻量模型，在游戏运行时**逐帧**分析画面并输出自适应 3D LUT，
应用到后处理链，实现实时自适应调色。

### 约束
- 推理延迟 < 2ms / 帧（不影响帧率）
- 帧间调色变化平滑（无闪烁）
- 模型体积 < 1MB（可打包进游戏）

---

## 2. 架构设计

### 2.1 核心架构：基础 LUT 融合（借鉴 Image-Adaptive-3DLUT）

不直接预测调色参数，而是**学习多个基础 LUT + 预测融合权重**：

```
输入图片 ──→ 下采样 (256×256) ──→ 权重预测网络 ──→ 权重 (w1, w2, w3)
  │                                                      │
  │         ┌── Basis LUT 1 (可学习) ──┐                 │
  │         ├── Basis LUT 2 (可学习) ──┤                 │
  │         ├── Basis LUT 3 (可学习) ──┤                 │
  │         └──────────────────────────┘                 │
  │                      ↓                                │
  │           w1×LUT1 + w2×LUT2 + w3×LUT3 = 自适应 LUT  ←┘
  │                      ↓
  └────────→ 三线性插值查表 ──→ 调色后的图片
```

**为什么这样设计**：
- 3 个 33×33×33 的基础 LUT 有 ~32 万可学习参数，色彩表达能力远超 11 个固定参数
- 权重预测网络只在低分辨率上跑（256×256），极快
- 三线性插值查表与分辨率无关 → **480p 训练，4K 推理无性能损失**
- 已有论文验证：< 60 万参数，4K 推理 < 2ms (Titan RTX)

### 2.2 为什么需要时序信息

逐帧独立预测会导致：
- 相邻帧参数跳变 → 画面闪烁
- 场景切换时反应过度（硬切 vs 渐变）
- 动态场景中调色不稳定（角色走进阴影区域）

NVIDIA DLSS 4/5 的做法：
- 输入包含当前帧 + motion vectors
- Transformer 架构做帧间关联
- 端到端训练保证时序一致性（deterministic & temporally stable）

### 2.3 权重预测网络的演进路线

```
阶段 1: 原版 CNN（先跑通）
┌──────────────┐     ┌──────────────┐     ┌────────┐
│ 下采样图片    │────→│ 小 CNN       │────→│ 3 权重  │
│ (256×256×3)  │     │ (原版架构)   │     │ softmax│
└──────────────┘     └──────────────┘     └────────┘
  直接复用 Image-Adaptive-3DLUT 原版代码
  目的: 验证训练管线跑通

阶段 2: CNN + GRU（加入时序）
┌──────────────┐     ┌──────────────┐     ┌─────────┐     ┌────────┐
│ 下采样图片    │────→│ CNN 特征提取  │────→│ GRU     │────→│ 3 权重  │
│ (256×256×3)  │     │ → 特征向量    │     │ hidden  │     │ softmax│
└──────────────┘     └──────────────┘     │ state 64│     └────────┘
                                          └─────────┘
                                            ↑    │
                                            └────┘ 传递到下一帧
  CNN 提取当前帧特征，GRU 带入前帧记忆
  实现帧间平滑过渡

阶段 3: CNN + GRU + 风格条件（加入风格选择）
┌──────────────┐     ┌──────────────┐
│ 下采样图片    │────→│ CNN 特征提取  │──┐
└──────────────┘     └──────────────┘  │
                                       ├──→ GRU → FC → 3 权重
┌──────────────┐     ┌──────────────┐  │
│ 风格选择      │────→│ 风格嵌入向量  │──┘
│ (电影/胶片/..)│     │ (16 维)      │
└──────────────┘     └──────────────┘
  玩家可选择调色风格，模型输出对应风格的 LUT
```

### 2.4 Loss 函数设计

```python
L_total = L_content + λ_tv * L_tv + λ_mn * L_mono + λ_smooth * L_smooth

L_content = MSE(predicted_image, target_image)            # 图像重建
L_tv      = TV_regularization(fused_LUT)                  # LUT 平滑，防色带
L_mono    = ReLU_monotonicity(fused_LUT)                  # LUT 单调性，防反转
L_smooth  = MSE(weights_t, weights_{t-1})                 # 时序平滑 (阶段2+)
```

推荐超参（来自论文消融实验）：
- λ_tv = 0.0001（太大会限制 LUT 灵活性）
- λ_mn = 10（单调性是自然约束，可以大）
- λ_smooth = 0.01（时序平滑，需实验调节）

---

## 3. 训练数据策略

### 3.1 数据本质

训练数据 = 大量 **(原图, 调好色的目标图)** 配对。
模型学习：看到一张原图 → 预测出一张 LUT → 使原图变成目标图的效果。

### 3.2 数据来源（三条路线并行）

#### 路线 A：FiveK 数据集（现成，零成本，跑通管线用）

```
来源: MIT-Adobe FiveK 数据集
内容: 5000 张 RAW 照片，5 位专业摄影师分别修图
格式: (原始照片, 专业修图结果) 配对
用途: 先跑通训练管线，验证代码正确
下载: https://data.csail.mit.edu/graphics/fivek/
备注: 3DLUT 仓库提供了 480p 版本，直接可用
```

#### 路线 A+：PST50 电影级参考配对（高质量，补强 Phase 2）

```
来源: Hugging Face — zrgong/PST50
链接: https://huggingface.co/datasets/zrgong/PST50
内容: 50 组 photorealistic style transfer 配对 (原图, 电影风格调色图)
配套论文: SA-LUT (https://huggingface.co/papers/2506.13465)
格式: (原图, 调好色的图) — 与 FiveK 格式完全一致
用途: Phase 2 与 FiveK + LUT 批量数据 ConcatDataset 并用
价值: LUT 批量数据是线性变换生成，无法表达"同一色在不同上下文的差异化处理";
      PST50 是真实调色师的配对，隐含上下文条件分布，显著提升 Classifier 泛化
集成: 新增 PST50Dataset 类 (~50 行)，main() 里 ConcatDataset 一行拼接,
      网络/loss/训练循环完全不改
陷阱: 部分配对 input/target 尺寸可能不一致，接入前必须 assert 或 resize 对齐
      再做 RandomCrop (FiveKDataset 当前实现要求两图尺寸相同)

注意: 只用它的数据，不用它的模型 —— SA-LUT 的 4D 空间自适应 LUT 架构
      与我们的 [3,33,33,33] 标准 LUT + UE 原生 ColorGradingLUT 注入管线不兼容
```

#### 路线 B：LUT 批量造数据（量大，覆盖多种风格）

```
步骤:
1. 网上下载 200~500 个免费 .cube LUT 文件
   - GitHub 搜 "free cinema LUT .cube"
   - 胶片模拟 LUT (Kodak / Fuji / ACES)
   - 社区分享的调色预设
2. 准备 1000~2000 张图片（游戏截图 + 普通照片）
3. 脚本: 对每张图 × 每个 LUT → 生成调色后的图
4. 得到 20~100 万对 (原图, 风格图) 训练数据

工作量: 收集 LUT 半天 + 写脚本 1 小时 + 批处理跑几小时
优点: 数据量大，风格丰富
```

#### 路线 C：达芬奇手动调色（最高质量，个性化微调）

```
步骤:
1. 在 UE 截取不同场景的游戏画面 (200~500 张)
2. 导入达芬奇，手动调色（或用 MCP AI 辅助调色）
3. 导出调色后的图
4. 形成 (原始截图, 调色截图) 配对

工作量: 取决于你愿意花多少时间调色
优点: 模型学到的就是你的审美偏好
用途: 训练后期做微调
```

### 3.3 数据使用计划

```
Phase 1: FiveK 数据集 (纯净基准)                 ← 验证代码，对照论文 PSNR
Phase 2: FiveK + PST50 + LUT 批量造数据 (并用)    ← 正式训练，覆盖多种风格
Phase 3: 达芬奇手动调色                          ← 微调，注入个人审美
Phase 4: 部署后收集游戏实际数据                   ← 持续优化
```

Phase 2 数据组合策略:
- FiveK       : 5000 对      — 专业修图基线
- PST50       : 50 组        — 电影级高质量配对 (高权重采样)
- LUT 批量    : 10~20 万对   — 广度覆盖，教模型"LUT 空间长什么样"

风险控制: 若加入 PST50 后 FiveK 验证集 PSNR 下降超过 0.5 dB，
         说明风格分布冲突过大，退回纯 LUT 批量方案，PST50 留给 Phase 3 微调。

LUT 数据资源待收集:
- [ ] GitHub 免费 LUT 集合
- [ ] 胶片模拟 LUT (Kodak / Fuji / ACES)
- [ ] 社区分享的调色预设
- [ ] 游戏截图素材库

---

## 4. 具体工作计划

### 4.1 环境搭建（4080 训练机）

```
Python 3.10+
PyTorch 2.x + CUDA 12
numpy, Pillow, tqdm, tensorboard
Image-Adaptive-3DLUT 代码 (git clone)
```

### 4.2 需要写的代码

| 文件 | 功能 | 基于 | 工作量 |
|------|------|------|--------|
| `generate_pairs.py` | 路线 B: 用 LUT 批量造训练数据 | 新写 | 小 |
| `dataset.py` | 数据加载、下采样、转 tensor | 改原版 | 小 |
| `model.py` | 网络定义 (阶段1 复用原版) | 原版 | 无 |
| `model_gru.py` | 阶段2: CNN+GRU 时序版 | 基于原版改造 | 中 |
| `train.py` | 训练循环、loss、保存 | 改原版 | 小 |
| `train_seq.py` | 阶段2: 序列训练循环 | 新写 | 中 |
| `export_onnx.py` | 导出 ONNX 格式 | 新写 | 小 |
| `evaluate.py` | 评估：PSNR / SSIM / 视觉对比 | 改原版 | 小 |

### 4.3 执行步骤

```
Step 1: 跑通原版（1~2 天）
  ├── Clone Image-Adaptive-3DLUT 仓库
  ├── 下载 FiveK 480p 数据集
  ├── 安装依赖，编译三线性插值 CUDA 算子
  ├── 跑原版训练 (200 epochs ≈ 2~3h on 4080)
  └── 验证结果：看生成的增强图是否正常

Step 2: 用 LUT 造数据 + 训练（2~3 天）
  ├── 收集免费 .cube LUT 文件 (200~500 个)
  ├── 准备图片素材 (游戏截图 + 通用图片)
  ├── 写 generate_pairs.py 批量造数据
  ├── 训练 (50~100 epochs ≈ 6~12h on 4080)
  └── 评估不同风格的效果

Step 3: 加入时序 GRU（3~5 天）
  ├── 写 model_gru.py: CNN backbone + GRU
  ├── 写 train_seq.py: 序列训练循环
  ├── 生成时序训练数据（视频序列 or 模拟场景变化）
  ├── 训练 + 调参
  └── 验证: 帧间是否平滑无闪烁

Step 4: 导出 & 部署到 UE（2~3 天）
  ├── PyTorch → ONNX 导出
  ├── UE 侧用 NNE 或 OnnxRuntime-UE 加载
  ├── 每帧: 下采样画面 → 推理 → 融合 LUT → PostProcess
  └── 集成到 LookScopes 插件

Step 5: 达芬奇微调 + 风格条件（可选，1~2 天）
  ├── 用达芬奇调一批游戏截图
  ├── 小 learning rate 微调模型
  ├── 加入风格嵌入向量（多风格支持）
  └── UE 侧暴露风格选择 UI
```

### 4.4 训练时间预估（RTX 4080）

| 阶段 | 数据量 | batch_size | epochs | 预计时间 |
|------|--------|-----------|--------|---------|
| Step 1: FiveK 原版 | 5000 对 (480p) | 16 | 200 | 2~3h |
| Step 2: LUT 造的数据 | 10~20 万对 | 32 | 50~100 | 6~12h |
| Step 3: GRU 序列 | 时序序列 | 8 | 100~200 | 12~24h |
| Step 5: 达芬奇微调 | 几百对 | 8 | 10~20 | 几分钟 |

---

## 5. 部署架构（UE 侧）

```
每帧 / 每 N 帧:

  Viewport ──→ 下采样 (256×256) ──→ ONNX 推理 ──→ 3 个权重
                                                      │
  3 个基础 LUT (随模型一起打包)  ←────── 加权融合 ←────┘
                    │
                    ↓
            自适应 3D LUT
                    │
                    ↓
          PostProcessVolume.ColorGradingLUT ──→ 最终画面
```

关键实现细节:
- 下采样用 GPU (RenderTarget → ReadPixels 或 Compute Shader)
- 基础 LUT 存为常量数组，随 ONNX 模型打包
- 三线性插值查表用 GPU Compute Shader 或 UE 原生 LUT 管线
- 推理频率可降为每 2~4 帧一次以节省性能

UE 已有的基础设施:
- `UNeuralNetwork` (NNE plugin) 或 [microsoft/OnnxRuntime-UnrealEngine](https://github.com/microsoft/OnnxRuntime-UnrealEngine)
- `FPostProcessSettings::ColorGradingLUT`
- GPU Compute Shader 做直方图统计

---

## 6. NVIDIA 参考架构笔记

### DLSS 4 (2025)
- 首次在图形渲染中使用 Transformer 架构
- Multi Frame Generation: 1 帧渲染 → 生成最多 3 额外帧
- 关键: 用 motion vectors 做帧间关联

### DLSS 5 (2026)
- 输入: 2D 帧 + motion vectors
- 输出: 光照/材质增强的像素
- 生成式模型, 但保持 deterministic & temporally stable
- 在 2D 空间操作（不需要 3D 信息），效率高
- 可提供艺术家控制: 强度、调色、遮罩

### 对我们的启发
1. **时序一致性是核心** — 不是额外的后处理，而是端到端训练的一部分
2. **motion vectors 是免费的时序信号** — UE 已经计算了，直接拿来用
3. **2D 空间操作足够** — 调色不需要 3D 理解
4. **提供艺术家控制接口** — 强度滑杆、风格选择等

---

## 7. 开源项目参考

### 7.1 核心参考：神经网络 → 3D LUT 预测

#### Image-Adaptive-3DLUT ⭐⭐⭐（最推荐）
- **仓库**: [HuiZeng/Image-Adaptive-3DLUT](https://github.com/HuiZeng/Image-Adaptive-3DLUT)
- **论文**: TPAMI 2022 — *Learning Image-adaptive 3D Lookup Tables for High Performance Photo Enhancement in Real-time*
- **架构**: 学习多个基础 3D LUT + 小 CNN 预测权重融合
- **性能**: < 60 万参数，4K 推理 < 2ms (Titan RTX)
- **亮点**: 可在 480p 训练直接应用 4K，无性能下降
- **对我们的价值**: 架构直接可用 — 把 CNN 替换成 GRU 即可加入时序能力；
  基础 LUT 融合思路比直接预测 11 个参数更有表达力

#### AdaInt（自适应间隔 3D LUT）
- **仓库**: [ImCharlesY/AdaInt](https://github.com/ImCharlesY/AdaInt)
- **论文**: CVPR 2022 — *Learning Adaptive Intervals for 3D Lookup Tables on Real-time Image Enhancement*
- **架构**: 非均匀采样间隔的 3D LUT，提升 LUT 表达能力
- **亮点**: 包含可微分 AiLUT-Transform 算子，C++/CUDA 实现
- **协议**: Apache 2.0
- **对我们的价值**: 改进版 LUT 预测，可微分算子可直接集成到训练管线

#### NILUT（Neural Implicit LUT）
- **仓库**: [mv-lab/nilut](https://github.com/mv-lab/nilut)
- **论文**: AAAI 2024 — *NILUT: Conditional Neural Implicit 3D Lookup Tables for Image Enhancement*
- **架构**: 神经隐式表示做连续色彩变换
- **亮点**: 支持风格混合，模型更紧凑
- **协议**: MIT
- **对我们的价值**: 隐式 LUT 不需要固定分辨率网格，可插值任意精度

#### LUTwithBGrid（双边网格 + LUT）
- **仓库**: [WontaeaeKim/LUTwithBGrid](https://github.com/WontaeaeKim/LUTwithBGrid)
- **论文**: ECCV 2024 — *Image-adaptive 3D LUTs with Bilateral Grids*
- **架构**: 双边网格 + 3D LUT，空间感知增强
- **亮点**: 参数更少，推理更快，SOTA 性能
- **对我们的价值**: 支持局部调色（画面不同区域施加不同 LUT），进阶功能参考

#### VideoColorGrading（视频 LUT 扩散模型）
- **仓库**: [seunghyuns98/VideoColorGrading](https://github.com/seunghyuns98/VideoColorGrading)
- **论文**: ICCV 2025 — *Video Color Grading via Look-Up Table Generation*
- **架构**: 扩散模型生成 LUT，支持参考图驱动
- **协议**: Apache 2.0，有训练代码 + 预训练模型
- **对我们的价值**: 参考图驱动风格迁移的思路；扩散模型本身太重不适合实时，
  但其数据处理管线和训练策略可借鉴

#### 3D-LUT Film Emulation
- **仓库**: [ns144/3D-LUT](https://github.com/ns144/3D-LUT)
- **架构**: 多种 CNN 架构（GAN / CycleGAN / StarGAN）预测 LUT
- **亮点**: 支持无配对数据训练（CycleGAN），胶片仿真
- **对我们的价值**: 无配对数据训练方案参考 — 只需要"好看的图"就能训练

### 7.2 时序一致性参考

#### ColorMNet
- **仓库**: [yyang181/colormnet](https://github.com/yyang181/colormnet)
- **论文**: ArXiv 2024 — 基于记忆的时空特征传播
- **亮点**: 有完整训练代码 (2024.9) + Gradio demo + HuggingFace
- **对我们的价值**: 时序特征传播机制参考

#### TCVC（时序一致视频着色）
- **仓库**: [lyh-18/TCVC-Temporally-Consistent-Video-Colorization](https://github.com/lyh-18/TCVC-Temporally-Consistent-Video-Colorization)
- **架构**: 双向特征传播 + 自正则化
- **亮点**: **不需要 ground-truth 彩色视频训练**
- **对我们的价值**: 自监督时序一致性方案，减少标注数据需求

### 7.3 轻量模型 & 引擎部署

#### CSRNet（条件序列调制网络）
- **仓库**: [hejingwenhejingwen/CSRNet](https://github.com/hejingwenhejingwen/CSRNet)
- **架构**: 极轻量全局图像调色网络
- **性能**: 参数量仅 HDRNet 的 1/13，White-Box 的 1/250
- **对我们的价值**: 极致轻量化参考，证明小模型也能做好调色

#### OnnxRuntime-UnrealEngine（微软官方）
- **仓库**: [microsoft/OnnxRuntime-UnrealEngine](https://github.com/microsoft/OnnxRuntime-UnrealEngine)
- **内容**: UE 插件，直接在引擎内跑 ONNX 模型
- **亮点**: 包含 5 个预训练风格迁移模型 demo
- **对我们的价值**: 部署管线直接参考，省去自己集成 ONNX Runtime 的工作

### 7.4 其他工具

#### color-matcher（传统色彩匹配）
- **仓库**: [hahnec/color-matcher](https://github.com/hahnec/color-matcher) (636 stars)
- **方法**: Reinhard / MKL / 直方图匹配等经典算法
- **对我们的价值**: Phase 1 规则引擎的算法参考

#### Agentic Color Grader（AI Agent 调色）
- **仓库**: [perbhat/agentic-color-grader](https://github.com/perbhat/agentic-color-grader)
- **架构**: TypeScript，自然语言驱动 ffmpeg 调色
- **对我们的价值**: Agent 化调色工作流参考（和我们的 MCP 方案类似）

### 7.5 推荐研究路线

```
Phase 1 — 跑通管线
  ├── 精读 Image-Adaptive-3DLUT 论文和代码
  ├── 理解基础 LUT 融合架构
  └── 用其代码在 FiveK 数据集上复现结果

Phase 2 — 加入时序
  ├── 参考 ColorMNet 的时序传播机制
  ├── 将 CNN backbone 替换为 GRU
  └── 用合成序列数据训练

Phase 3 — 轻量化部署
  ├── 参考 CSRNet 的极致压缩策略
  ├── 导出 ONNX
  └── 用 OnnxRuntime-UE 插件加载到引擎

Phase 4 — 风格扩展
  ├── 参考 NILUT 的隐式表示做风格混合
  ├── 参考 3D-LUT 的 CycleGAN 做无配对训练
  └── 收集 LUT 集合训练风格条件模型
```

---

## 8. 已确定 & 待确定项

### 已确定
- [x] 核心架构: 基础 LUT 融合 (借鉴 Image-Adaptive-3DLUT)
- [x] 起步方案: 先用原版 CNN 跑通，再加 GRU 时序
- [x] 训练框架: PyTorch 2.x + CUDA 12
- [x] 训练硬件: RTX 4080 (16GB VRAM)
- [x] 训练数据: FiveK 验证管线 → LUT 批量造数据 → 达芬奇微调

### 待确定
- [ ] 确定 UE 侧推理方式: NNE vs 微软 OnnxRuntime-UE
- [ ] 确定基础 LUT 数量: 3 个 (论文默认) vs 5 个
- [ ] 确定 LUT 分辨率: 33×33×33 (论文默认) vs 17 或 65
- [ ] 确定下采样分辨率: 256×256 vs 128×128
- [ ] 游戏截图素材收集方式和数量
- [ ] 免费 .cube LUT 收集（目标 200~500 个）
- [ ] 训练机 Python/PyTorch 环境搭建
- [ ] 是否引入 CycleGAN 做无配对风格训练（进阶）
- [ ] 风格条件注入方式（Phase 3 决策）:
  - 方案 A: 风格 ID 嵌入向量 (玩家下拉选"电影/胶片/赛博")
  - 方案 B: 参考图特征提取 (玩家拖一张参考图进来，模型自动匹配) —— 参考
    SA-LUT (https://huggingface.co/papers/2506.13465) 的参考图条件分支设计,
    天然对齐 LookScopes 第 6 支柱"参考画廊"的现有 UI 流
- [ ] 局部调色方案选型（进阶）:
  - LUTwithBGrid: 双边网格 + 3D LUT (轻量，Compute Shader 可实现，
    仍输出标准 [3,33,33,33] LUT，兼容 UE 原生注入管线)
  - SA-LUT: 4D 空间自适应 LUT (表达力强，但需完全替换 UE 原生 LUT 注入,
    并破坏示波器可验证性 — 违反 DesignDoc 第一性原理"工具用于测量")
  - 决策点: 先跑全局 3D LUT 到 Phase 2 结束，看真实游戏画面上
    是否出现"天空和皮肤需要差异化处理"的需求，再决定是否引入局部调色
