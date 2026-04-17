---
name: computer-use
description: >-
  Guide AI to operate desktop GUI via system-agent MCP tools. Use when the user
  asks to click buttons, toggle settings, navigate UI, fill forms, or perform
  any screen interaction through the system-agent MCP server.
---

# Computer Use — system-agent 工具使用指南

通过 `user-system-agent` MCP 服务控制桌面 GUI。共 12 个工具，分感知和行动两层。

## 工具总览

| 层 | 工具 | 用途 |
|----|------|------|
| 感知 | `screenshot_parsed` | 截屏 + OmniParser 识别所有 UI 元素 |
| 感知 | `find_nearby` | 查找元素的空间邻居（同行/同列/半径） |
| 感知 | `verify_element` | 移动鼠标到元素 + 局部放大 + 光标标记（确认用） |
| 感知 | `crop_element` | 裁剪元素高清区域，可存模板 |
| 感知 | `find_image` | 模板匹配，返回精确屏幕坐标 |
| 行动 | `click_element` | 按元素编号点击 |
| 行动 | `mouse_move` | 移动鼠标到坐标 |
| 行动 | `mouse_click` | 在坐标点击 |
| 行动 | `mouse_drag` | 拖拽 |
| 行动 | `keyboard_type` | 输入文本（支持中文） |
| 行动 | `keyboard_press` | 按键/组合键 |
| 行动 | `scroll` | 滚动鼠标滚轮 |

## 核心决策流程

收到 GUI 操作任务后，按以下决策树执行：

```
1. 调用 screenshot_parsed() 获取带编号截图 + 元素列表
   │
2. 能否从元素列表中直接找到目标？
   │
   ├─ YES，且确信无误 → click_element(id)  ✅ 完成
   │
   ├─ YES，但不太确定是否选对了
   │   └→ verify_element(id)  查看局部放大+光标标记
   │      ├─ 确认正确 → click_element(id)  ✅ 完成
   │      └─ 不对 → 重新选择 id 或调整策略
   │
   ├─ 找到了文字标签但需要旁边的开关/按钮
   │   └→ find_nearby(id, mode="row")  找同行关联元素
   │      └→ click_element(neighbor_id)  ✅ 完成
   │
   ├─ 目标太小或有多个相似元素，需要像素级精度
   │   └→ crop_element(id, save_as="xxx")  裁剪高清模板
   │      └→ find_image("xxx.png")  模板匹配得到精确坐标
   │         └→ mouse_click(x, y)  ✅ 完成
   │
   └─ 没找到目标（可能在屏幕外）
       └→ scroll() 滚动  →  回到步骤 1
```

## 工作流详解

### 流程 A：直接点击（最常用）

适合目标在元素列表中有明确对应项的场景。

```
screenshot_parsed()  →  阅读元素列表  →  click_element(id)
```

**示例**：点击 "Save" 按钮
1. `screenshot_parsed()` → 找到 `[42] Save (button) @(800,600) 80x30`
2. `click_element(id=42)`

### 流程 A+：确认后点击（安全路径）

适合不确定是否选对元素、或操作不可逆的场景。不调用 OmniParser，延迟 < 1 秒。

```
screenshot_parsed()  →  verify_element(id)  →  确认  →  click_element(id)
```

**示例**：关闭某个 MCP 服务的开关（操作重要，需确认）
1. `screenshot_parsed()` → 找到 `[283] (toggle) @(1900,554)`
2. `verify_element(id=283)` → 看到局部放大图，光标确实在目标开关上
3. `click_element(id=283)`

### 流程 B：空间关联查找

适合目标（如开关）与文字标签在同一行但距离较远的场景。

```
screenshot_parsed()  →  找到文字标签 id
  →  find_nearby(id, mode="row")  →  click_element(开关 id)
```

**示例**：关闭 "Show localhost links" 旁边的开关
1. `screenshot_parsed()` → 找到 `[156] Show localhost links in browser (text)`
2. `find_nearby(id=156, mode="row")` → 返回同行元素，其中有 `[283] (toggle)`
3. `click_element(id=283)`

**find_nearby 三种模式**：
- `mode="radius"`（默认）：欧氏距离，适合紧凑 UI
- `mode="row"`：同行查找（Y 容差内），适合宽屏设置页
- `mode="col"`：同列查找（X 容差内），适合找同列的一组元素

### 流程 C：模板匹配精确点击

适合元素太小、或需要区分多个外观相同元素的场景。

```
screenshot_parsed()  →  crop_element(id, save_as="tpl")
  →  find_image("tpl.png")  →  mouse_click(x, y)
```

## 鼠标位置确认

两种方式验证鼠标位置：

1. **verify_element**（推荐）：输入元素 id，自动移动鼠标并返回局部标记图
2. **手动方式**：`mouse_move(x, y)` → `screenshot_parsed(cursor=True)` → 查看全屏准星

**注意**：模板匹配（find_image）时不要用 cursor=True，光标标记会干扰匹配。

## 常见场景速查

| 场景 | 推荐流程 |
|------|----------|
| 点击按钮/链接/标签 | A：screenshot_parsed → click_element |
| 不确定元素是否正确 | A+：screenshot_parsed → verify_element → click_element |
| 切换设置开关 | B：screenshot_parsed → find_nearby(mode="row") → click_element |
| 找到同列的多个相似元素 | B：find_nearby(mode="col") |
| 精确点击小目标 | C：crop_element → find_image → mouse_click |
| 输入文本到输入框 | A 点击输入框 → keyboard_type |
| 滚动查找不可见元素 | scroll → screenshot_parsed → 重新搜索 |
| 按快捷键 | keyboard_press("ctrl+s") |
| 确认鼠标位置 | verify_element(id) 或 mouse_move → screenshot_parsed(cursor=True) |
| 屏幕上有多个同名文字 | 限定 ROI：先定位父元素/标题，再在其区域内搜索；或 find_nearby 限定范围 |

## 关键原则

1. **先感知再行动**：每次操作前调用 screenshot_parsed 确保信息最新
2. **优先用编号点击**：click_element 比 mouse_click 更可靠，因为坐标由 OmniParser 计算
3. **不确定就确认**：用 verify_element 查看局部放大图，确认后再点击
4. **找不到就滚动**：目标不在元素列表中，很可能在屏幕外，先 scroll 再重新截屏
5. **空间关联用 find_nearby**：不要猜测坐标关系，让工具计算
6. **验证关键操作**：重要操作后再次 screenshot_parsed 确认结果
7. **同名元素消歧**：有同名或相似文本时，限定 ROI 或使用层级上下文（如"位于 X 下方"），禁止全局搜索
