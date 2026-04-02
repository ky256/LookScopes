# LookScopes DaVinci Bridge

UE 视口 → NDI → DaVinci Resolve 实时预览管线。

## 架构

```
UE 5.7 (LookScopes ViewportStreamer)
    → NDIMediaOutput (MediaCapture)
    → NDI 网络流 ("UE_LookScopes")
        ↓
DaVinci Resolve (NDIReceiverPlugin.ofx)
    → OFX Generator 节点
    → Color 页面调色
    → 渲染输出
```

## 前置条件

1. **NDI Runtime** - UE 5.7 自带（位于 NDIMedia 插件的 Binaries/ThirdParty），无需另外安装
2. **UE 插件** - 在 Edit > Plugins 中启用 `NDI Media` 和 `Media IO Framework`
3. **项目设置** - Project Settings > Rendering > Frame Buffer Pixel Format = RGBA8

## UE 侧使用

在 LookScopes Subsystem 中调用：

```cpp
// 开始推流
ULookScopesSubsystem* Subsystem = GEditor->GetEditorSubsystem<ULookScopesSubsystem>();
Subsystem->StartNDIStream(TEXT("UE_LookScopes"));

// 停止推流
Subsystem->StopNDIStream();
```

或通过面板 UI 的 Stream 按钮控制。

## DaVinci 侧编译安装

```bash
cd DaVinciPlugin/NDIReceiver
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build
```

编译后 `NDIReceiverPlugin.ofx.bundle` 会安装到 `C:\Program Files\Common Files\OFX\Plugins\`。
重启 DaVinci Resolve 后，在 Color 页面的 OFX 列表中找到 "NDI Receiver (UE Bridge)"。

## 参数

| 参数 | 说明 |
|------|------|
| NDI Source Name | 要接收的 NDI 源名称，需与 UE 侧一致（默认 `UE_LookScopes`） |
