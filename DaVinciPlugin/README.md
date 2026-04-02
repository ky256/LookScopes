# LookScopes DaVinci Bridge

UE 视口 → NDI → DaVinci Resolve 实时预览管线。

## 架构

```
UE 5.7 (LookScopes Plugin)
    → FViewportStreamer → NDIMediaOutput (MediaCapture)
    → NDI 网络流 ("UE_LookScopes")
        ↓
DaVinci Resolve (NDIReceiverPlugin.ofx)
    → Fusion 页面: OFX Generator 节点 → MediaOut
    → Color 页面: 实时调色
    → 渲染输出
```

## 前置条件

| 项目 | 说明 |
|------|------|
| NDI Runtime | UE 5.7 自带（NDIMedia 插件的 Binaries/ThirdParty），无需另外安装 |
| UE 插件 | Edit > Plugins 中启用 `NDI Media` 和 `Media IO Framework` |
| DaVinci Resolve | 免费版或 Studio 版均可 |

## UE 侧使用

### 通过面板 UI（推荐）

1. 打开 **Tools > Look Scopes 面板**（或 `Ctrl+Shift+L`）
2. 点击工具栏的 **NDI Stream** 按钮开始推流
3. 使用 **Res** 下拉框选择输出分辨率：
   - **Auto** — 跟随视口物理尺寸
   - **1280 x 720** — HD
   - **1920 x 1080** — Full HD
   - **2560 x 1440** — QHD
   - **3840 x 2160** — 4K UHD
4. 再次点击 **NDI Stop** 停止推流

> 选择固定分辨率后，视口渲染目标会锁定为该分辨率（通过 `SetFixedViewportSize`），
> 视口内的画面按比例缩放显示。选择 Auto 恢复跟随窗口大小。

### 通过代码 API

```cpp
auto* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>();

// 设置输出分辨率（可选，0x0 = 跟随视口）
Subsystem->SetStreamResolution(FIntPoint(1920, 1080));

// 开始推流
Subsystem->StartNDIStream(TEXT("UE_LookScopes"));

// 停止推流
Subsystem->StopNDIStream();
```

## DaVinci 侧：编译安装 OFX 插件

### 编译

```bash
cd DaVinciPlugin/NDIReceiver
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build
```

编译后 `NDIReceiverPlugin.ofx.bundle` 会安装到：
```
C:\Program Files\Common Files\OFX\Plugins\
```

> 安装时需关闭 DaVinci Resolve，否则会因文件占用失败。

### 在 DaVinci Resolve 中使用

#### 方式一：Fusion 页面（推荐）

1. 打开 DaVinci Resolve，切换到 **Fusion** 页面
2. 在节点编辑器空白区域右键 → **Add Tool > OpenFX > NDI Receiver (UE Bridge)**
3. 该节点作为 Generator（源），将其输出连接到 **MediaOut1**
4. 在右侧 Inspector 中确认 **NDI Source Name** 为 `UE_LookScopes`（需与 UE 侧一致）
5. 回到 UE 开始推流，Fusion 中即可看到实时画面

#### 方式二：Color 页面

1. 切换到 **Color** 页面
2. 在节点图中添加节点，选择 **OpenFX** 分类
3. 找到 **NDI Receiver (UE Bridge)** 拖入节点图
4. 配置 Source Name 后即可接收 UE 画面进行调色

## OFX 插件参数

| 参数 | 说明 |
|------|------|
| NDI Source Name | 要接收的 NDI 源名称，需与 UE 侧一致（默认 `UE_LookScopes`） |

## 常见问题

### 视口大小变化导致推流中断

ViewportStreamer 内置了 debounce 重连机制：当视口尺寸变化触发 `MediaCapture` 停止时，
会等待 1 秒（防止连续 resize 抖动），然后自动以新尺寸重启推流。日志中可看到：

```
LogViewportStreamer: [OnCaptureStateChanged] Capture stopped, starting 1.0s debounce timer
LogViewportStreamer: [TickDebounce] Debounce expired, restarting stream
```

### 选择分辨率后视口顶部工具栏变小

这是 UE 引擎的固有行为。`SLevelViewport` 使用 `SScaleBox` 包裹整棵视口子树
（包括 Perspective/Lit 工具栏），`SetFixedViewportSize` 激活 `ScaleToFit` 时
工具栏也跟随缩放。选择 **Auto** 可恢复正常 UI 大小。

### DaVinci 中看不到画面

1. 确认 UE 侧推流已启动（工具栏 NDI 指示灯为红色实心圆）
2. 确认 DaVinci 的 OFX 节点 Source Name 与 UE 侧一致
3. 检查 UE 日志是否有 `NDI 推流已启动` 消息
4. 两端需在同一局域网内（或同一台机器）
