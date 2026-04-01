# UE 视口分析与 Look Match 插件设计开发白皮书

> **文档目标**：为 Unreal Engine 视口画面分析与视觉对标（Look Matching）插件提供 UI/UX 核心交互逻辑、模块定义及前端原型代码参考。
> **面向对象**：技术美术 (TA)、灯光师、工具链开发工程师。

---

## 🧩 First Principles Analysis (第一性原理分析)

在设计本插件的交互框架时，我们严格摒弃了"在 UI 中内置修图滤镜"的行业惯性思维，回归到专业渲染引擎的物理本质。

### 1. 拆解：隐含假设与逻辑公理

- **隐含假设**：为了让画面达到目标效果，分析工具应该提供一套类似达芬奇的"调色滑块"让用户在 UI 里预览效果。
- **物理/逻辑限制**：UE 的画面是由底层 PostProcessVolume (PPV) 和物理材质驱动的。在独立的 2D UI 面板中套用非原生的图像算法，不仅浪费性能，且得到的参数无法"无损落地"回引擎中。
- **逻辑公理**："视觉对标 (Look Matching)"的本质是一个 **"测量差距 → 修改引擎参数 → 验证重叠"** 的闭环控制系统。**工具的作用是测量，而不是修改。**

### 2. 验证：剔除偏见，保留事实

- **偏见**：把参考图和引擎画面并排放在一起，用肉眼对比就能调好。
- **事实 1**：人眼存在强烈的"同时对比效应 (Simultaneous Contrast)"，大脑会自动模糊并补偿两张并排图片的色彩差异，导致肉眼判断极度不可靠。
- **事实 2**：要实现 100% 的像素级物理重现，不能仅靠主观感官，必须依赖数学图谱（示波器）的绝对重合。

### 3. 重构：基于基本事实推导新方案

- **空间上的硬边界 (Wipe)**：废弃并排视图，强制引入鼠标可拖拽的"划像"功能。通过锐利的物理边界，强行打破大脑的视觉补偿，暴露像素级差异。
- **数据上的重叠图谱 (Dual-Trace)**：废弃单层示波器，引入"双轨示波技术"。将参考图冻结为底色（橙色），将实时视口作为顶层（白色）。用户的核心交互行为被降维成：**"在 UE 中调整参数，直到白色波形完全覆盖橙色波形"**。

---

## 核心视口模块定义 (The 6 Pillars)

插件整体划分为 6 个核心数据获取与可视化模块：

| # | 模块名称 | 功能说明 |
|---|---------|----------|
| 1 | **波形图 (Waveform)** | 监控曝光与对比度。横轴对应画面水平位置，纵轴对应亮度（IRE/Nits）。用于检测死黑与高光溢出。 |
| 2 | **RGB 分量图 (RGB Parade)** | 分离 R、G、B 通道波形，用于精准检测白平衡偏移与特定通道的饱和度溢出。 |
| 3 | **矢量示波器 (Vectorscope)** | 抛开亮度，专职监控色彩与饱和度。带有标准的肤色参考线 (Skin Tone Line)，确保数字人肤色处于物理正确区间。 |
| 4 | **伪色/热力图 (False Color)** | 将曝光数据空间化，直接叠印在模型上，哪里欠曝/过曝一目了然。 |
| 5 | **PBR 反照率校验 (Albedo Validation)** `[UE特供]` | 验证材质的 Base Color 是否符合物理守恒定律（如禁止出现纯黑 0.0 或纯白 1.0 的材质）。 |
| 6 | **划像匹配与参考画廊 (Wipe Match & Gallery)** | 基于双轨数据技术，支持导入原画或抓取历史帧作为目标，进行 1:1 的视觉逆向工程。 |

---

## UI 布局逻辑与空间锚点

主界面（约 1280x800）被严格划分为四个功能区，共同构成精密的工作流：

### 左侧：划像视口区 (空间锚点)

- 占据最大面积，建立环境光照与材质的上下文关系。
- 通过 Wipe 滑块暴露当前视口（Live）与目标（Reference）的空间差异。

### 右侧：双轨示波区 (数学锚点)

- 将感性的画面差异转化为绝对的坐标数据。
- **核心动作**：通过观察此区域，将"瞎调"变为有目的的"拼图游戏"（对齐双轨波形）。

### 底部：参考画廊区 (时间锚点)

- 管理多套 Target（如清晨、正午、夜景）或外部概念图。
- 支持一键加载为 Reference 冻结层。

### 顶部：全局控制栏

- 放置 Grab Still（抓取当前帧）、模式切换（单轨/双轨）等高频布尔开关。

---

## 阶段性原型源码 (HTML/JS/CSS)

> 本代码为高保真交互原型，完美还原了"暗色毛玻璃"风格、Wipe 划像交互以及 Canvas 双轨荧光粒子渲染逻辑。
> 开发时可参考其中的 DOM 结构与数学重绘逻辑。

```html
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>UE Reference Match & Scopes</title>
    <script src="https://unpkg.com/lucide@latest"></script>
    <style>
        :root {
            --bg-base: #111111;
            --panel-bg: rgba(30, 30, 32, 0.8);
            --border-color: rgba(255, 255, 255, 0.1);
            --text-main: #F5F5F7;
            --text-sub: #8E8E93;
            --color-ref: #FF9F0A; /* 橙色：代表参考图 Reference */
            --color-live: #FFFFFF; /* 白色：代表实时画面 Live */
            --color-accent: #0A84FF;
        }

        * { margin: 0; padding: 0; box-sizing: border-box; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }

        body {
            background-color: #000;
            color: var(--text-main);
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
            padding: 20px;
        }

        /* 整体应用窗口 */
        .app-container {
            width: 1280px;
            height: 800px;
            background: var(--bg-base);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            display: flex;
            flex-direction: column;
            overflow: hidden;
            box-shadow: 0 30px 60px rgba(0,0,0,0.6);
        }

        /* 顶部工具栏 */
        .toolbar {
            height: 48px;
            border-bottom: 1px solid var(--border-color);
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 0 16px;
            background: rgba(255,255,255,0.02);
        }

        .tool-group { display: flex; align-items: center; gap: 12px; }
        .app-title { font-weight: 600; font-size: 13px; letter-spacing: 1px; margin-right: 16px; }

        .btn {
            background: transparent;
            border: 1px solid var(--border-color);
            color: var(--text-main);
            padding: 6px 12px;
            border-radius: 6px;
            font-size: 12px;
            cursor: pointer;
            display: flex;
            align-items: center;
            gap: 6px;
            transition: 0.2s;
        }
        .btn:hover { background: rgba(255,255,255,0.05); }
        .btn.active { background: rgba(10, 132, 255, 0.15); border-color: var(--color-accent); color: var(--color-accent); }

        /* 主体内容区 (左右分栏) */
        .main-content {
            flex: 1;
            display: flex;
            height: calc(100% - 48px - 100px);
        }

        /* 左侧：划像视口区 */
        .viewport-section {
            flex: 1;
            border-right: 1px solid var(--border-color);
            position: relative;
            background: #1a1a1a;
            display: flex;
            flex-direction: column;
        }

        .viewport-header {
            height: 32px;
            background: rgba(0,0,0,0.3);
            display: flex;
            align-items: center;
            padding: 0 12px;
            font-size: 11px;
            justify-content: space-between;
            border-bottom: 1px solid var(--border-color);
        }

        .legend { display: flex; gap: 16px; font-weight: 500; }
        .legend-ref { color: var(--color-ref); display: flex; align-items: center; gap: 4px; }
        .legend-live { color: var(--color-live); display: flex; align-items: center; gap: 4px; }
        .dot { width: 8px; height: 8px; border-radius: 50%; }

        /* 划像组件核心逻辑 */
        .wipe-container {
            flex: 1;
            position: relative;
            overflow: hidden;
            cursor: ew-resize;
        }

        .image-layer {
            position: absolute;
            top: 0; left: 0;
            width: 100%; height: 100%;
            background-size: cover;
            background-position: center;
        }

        /* 模拟参考图 (偏暖、高对比度) */
        .img-reference {
            background-image: linear-gradient(45deg, #2a0800 0%, #8c2a00 50%, #ff8c42 100%);
        }

        /* 模拟当前UE视口 (偏冷、未调色) */
        .img-live {
            background-image: linear-gradient(45deg, #00122a 0%, #004c8c 50%, #42aaff 100%);
            clip-path: polygon(50% 0, 100% 0, 100% 100%, 50% 100%);
        }

        /* 划像分割线 */
        .wipe-slider {
            position: absolute;
            top: 0; bottom: 0;
            left: 50%;
            width: 2px;
            background: #fff;
            transform: translateX(-50%);
            box-shadow: 0 0 10px rgba(0,0,0,0.8);
            pointer-events: none;
            z-index: 10;
        }
        .wipe-handle {
            position: absolute;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            width: 32px; height: 32px;
            background: rgba(0,0,0,0.6);
            border: 2px solid #fff;
            border-radius: 50%;
            display: flex;
            align-items: center;
            justify-content: center;
            backdrop-filter: blur(4px);
        }
        .wipe-handle::before, .wipe-handle::after {
            content: ''; width: 0; height: 0; border-style: solid;
        }
        .wipe-handle::before { border-width: 5px 6px 5px 0; border-color: transparent #fff transparent transparent; margin-right: 4px; }
        .wipe-handle::after { border-width: 5px 0 5px 6px; border-color: transparent transparent transparent #fff; }

        /* 右侧：示波器数据区 */
        .scopes-section {
            width: 400px;
            background: var(--bg-base);
            display: flex;
            flex-direction: column;
            padding: 16px;
            gap: 16px;
        }

        .scope-card {
            flex: 1;
            background: var(--panel-bg);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            display: flex;
            flex-direction: column;
            position: relative;
        }

        .scope-header {
            font-size: 11px;
            color: var(--text-sub);
            padding: 8px 12px;
            border-bottom: 1px solid rgba(255,255,255,0.05);
            text-transform: uppercase;
            letter-spacing: 0.5px;
            display: flex; justify-content: space-between;
        }

        .scope-canvas-wrapper {
            flex: 1;
            position: relative;
            padding: 8px;
        }
        canvas { width: 100%; height: 100%; display: block; }

        .graticule {
            position: absolute; font-family: monospace; font-size: 10px; color: var(--text-sub); opacity: 0.5;
        }

        /* 底部：画廊区 */
        .gallery-section {
            height: 100px;
            border-top: 1px solid var(--border-color);
            background: #0f0f0f;
            display: flex;
            align-items: center;
            padding: 0 16px;
            gap: 12px;
            overflow-x: auto;
        }

        .gallery-item {
            width: 120px;
            height: 68px;
            background: #222;
            border: 1px solid var(--border-color);
            border-radius: 4px;
            cursor: pointer;
            position: relative;
            background-size: cover;
            transition: 0.2s;
        }
        .gallery-item:hover { border-color: var(--text-sub); }
        .gallery-item.active { border: 2px solid var(--color-ref); }
        .gallery-label {
            position: absolute; bottom: 0; left: 0; width: 100%;
            background: rgba(0,0,0,0.7); font-size: 10px; padding: 2px 4px;
        }
    </style>
</head>
<body>

    <div class="app-container">
        <!-- 顶部工具栏 -->
        <div class="toolbar">
            <div class="tool-group">
                <span class="app-title">LOOK MATCH & SCOPES</span>
                <button class="btn active"><i data-lucide="split-square-horizontal" size="14"></i> Wipe Mode</button>
                <button class="btn"><i data-lucide="images" size="14"></i> Load Reference</button>
            </div>
            <div class="tool-group">
                <button class="btn"><i data-lucide="camera" size="14"></i> Grab Still</button>
                <button class="btn"><i data-lucide="settings" size="14"></i></button>
            </div>
        </div>

        <!-- 主体区域 -->
        <div class="main-content">
            <!-- 左侧：划像对比视口 -->
            <div class="viewport-section">
                <div class="viewport-header">
                    <span>Viewport: Scene Camera</span>
                    <div class="legend">
                        <div class="legend-ref"><div class="dot" style="background: var(--color-ref)"></div> Reference (Target)</div>
                        <div class="legend-live"><div class="dot" style="background: var(--color-live)"></div> Live UE Viewport</div>
                    </div>
                </div>

                <div class="wipe-container" id="wipeContainer">
                    <div class="image-layer img-reference"></div>
                    <div class="image-layer img-live" id="liveImage"></div>
                    <div class="wipe-slider" id="wipeSlider">
                        <div class="wipe-handle"></div>
                    </div>
                </div>
            </div>

            <!-- 右侧：双轨示波器 -->
            <div class="scopes-section">
                <div class="scope-card">
                    <div class="scope-header">
                        <span>Dual-Trace Waveform (Luma)</span>
                        <span>IRE</span>
                    </div>
                    <div class="scope-canvas-wrapper">
                        <canvas id="cvsWaveform"></canvas>
                        <span class="graticule" style="top:8px; left:8px;">100</span>
                        <span class="graticule" style="bottom:8px; left:8px;">0</span>
                    </div>
                </div>

                <div class="scope-card">
                    <div class="scope-header">
                        <span>Dual-Trace Vectorscope</span>
                        <span>Sat</span>
                    </div>
                    <div class="scope-canvas-wrapper">
                        <canvas id="cvsVectorscope"></canvas>
                    </div>
                </div>
            </div>
        </div>

        <!-- 底部：参考帧画廊 -->
        <div class="gallery-section">
            <div class="gallery-item active" style="background-image: linear-gradient(45deg, #2a0800, #ff8c42);">
                <div class="gallery-label">Ref_Concept_01</div>
            </div>
            <div class="gallery-item" style="background-image: linear-gradient(to right, #111, #555);">
                <div class="gallery-label">Grab_001 (Dark)</div>
            </div>
            <div class="gallery-item" style="background-image: linear-gradient(to top, #2b580c, #f1fa3c);">
                <div class="gallery-label">Target_Daylight</div>
            </div>
            <div style="flex:1"></div>
            <span style="font-size: 11px; color: var(--text-sub);">Select a still to set as Reference</span>
        </div>
    </div>

    <script>
        lucide.createIcons();

        /* ================= 1. 划像交互逻辑 ================= */
        const container = document.getElementById('wipeContainer');
        const slider = document.getElementById('wipeSlider');
        const liveImg = document.getElementById('liveImage');
        let isDragging = false;

        function updateWipe(clientX) {
            const rect = container.getBoundingClientRect();
            let x = Math.max(0, Math.min(clientX - rect.left, rect.width));
            let percentage = (x / rect.width) * 100;

            slider.style.left = `${percentage}%`;
            liveImg.style.clipPath = `polygon(${percentage}% 0, 100% 0, 100% 100%, ${percentage}% 100%)`;
        }

        container.addEventListener('mousedown', (e) => {
            isDragging = true;
            updateWipe(e.clientX);
        });

        window.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            updateWipe(e.clientX);
        });

        window.addEventListener('mouseup', () => {
            isDragging = false;
        });

        /* ================= 2. 双轨数据示波器渲染 ================= */
        const colors = {
            ref: 'rgba(255, 159, 10, 0.08)',
            live: 'rgba(255, 255, 255, 0.08)',
            grid: 'rgba(255, 255, 255, 0.1)'
        };

        function setupCanvas(id) {
            const cvs = document.getElementById(id);
            const ctx = cvs.getContext('2d');
            const rect = cvs.parentElement.getBoundingClientRect();
            const dpr = window.devicePixelRatio || 1;
            cvs.width = rect.width * dpr;
            cvs.height = rect.height * dpr;
            ctx.scale(dpr, dpr);
            return { ctx, w: rect.width, h: rect.height };
        }

        function drawGrid(ctx, w, h) {
            ctx.strokeStyle = colors.grid;
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, h/2); ctx.lineTo(w, h/2);
            ctx.moveTo(0, h/4); ctx.lineTo(w, h/4);
            ctx.moveTo(0, h*0.75); ctx.lineTo(w, h*0.75);
            ctx.stroke();
        }

        function renderDualWaveform() {
            const { ctx, w, h } = setupCanvas('cvsWaveform');
            drawGrid(ctx, w, h);

            ctx.globalCompositeOperation = 'screen';
            const points = 8000;

            // 1. Reference (橙色)
            ctx.fillStyle = colors.ref;
            for(let i=0; i<points; i++) {
                let x = Math.random() * w;
                let nx = x / w;
                let baseY = Math.sin(nx * Math.PI) * 0.4 + 0.5;
                let noise = (Math.random() - 0.5) * 0.3;
                let y = (baseY + noise) * h;
                y = h - Math.max(10, Math.min(h-10, y));
                ctx.fillRect(x, y, 1.5, 1.5);
            }

            // 2. Live (白色)
            ctx.fillStyle = colors.live;
            for(let i=0; i<points; i++) {
                let x = Math.random() * w;
                let nx = x / w;
                let baseY = Math.sin(nx * Math.PI) * 0.2 + 0.6;
                let noise = (Math.random() - 0.5) * 0.2;
                let y = (baseY + noise) * h;
                y = h - Math.max(10, Math.min(h-10, y));
                ctx.fillRect(x, y, 1.5, 1.5);
            }
            ctx.globalCompositeOperation = 'source-over';
        }

        function renderDualVectorscope() {
            const { ctx, w, h } = setupCanvas('cvsVectorscope');
            const cx = w/2, cy = h/2, r = Math.min(w,h)/2 * 0.8;

            ctx.strokeStyle = colors.grid;
            ctx.beginPath();
            ctx.arc(cx, cy, r, 0, Math.PI*2);
            ctx.moveTo(cx-r, cy); ctx.lineTo(cx+r, cy);
            ctx.moveTo(cx, cy-r); ctx.lineTo(cx, cy+r);
            ctx.stroke();

            ctx.globalCompositeOperation = 'screen';
            const points = 5000;

            // 1. Reference (橙色)
            ctx.fillStyle = colors.ref;
            for(let i=0; i<points; i++) {
                let angle = (Math.random() * 1.5) + 3;
                let dist = Math.random() * r * 0.7;
                let px = cx + Math.cos(angle) * dist + (Math.random()-0.5)*10;
                let py = cy + Math.sin(angle) * dist + (Math.random()-0.5)*10;
                ctx.fillRect(px, py, 1.5, 1.5);
            }

            // 2. Live (白色)
            ctx.fillStyle = colors.live;
            for(let i=0; i<points; i++) {
                let angle = (Math.random() * 1.5) + 0;
                let dist = Math.random() * r * 0.5;
                let px = cx + Math.cos(angle) * dist + (Math.random()-0.5)*10;
                let py = cy + Math.sin(angle) * dist + (Math.random()-0.5)*10;
                ctx.fillRect(px, py, 1.5, 1.5);
            }
            ctx.globalCompositeOperation = 'source-over';
        }

        window.onload = () => {
            renderDualWaveform();
            renderDualVectorscope();
            updateWipe(container.getBoundingClientRect().left + container.offsetWidth / 2);
        };
    </script>
</body>
</html>
```
