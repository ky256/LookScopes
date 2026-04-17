---
name: commit-conventions
description: >-
  MetaAgent 项目 Git 提交规范。强制中文、标题+正文结构、模块前缀。
  创建 commit 时必须遵循此规范生成提交信息。
---

# 提交规范

## 语言要求

**必须使用中文。** 标题、正文、补充说明均使用中文书写。

## 格式

```
[模块] 动作描述（一句话概括）

变更内容:
- 具体改动 1
- 具体改动 2

原因/背景:（为什么做这个变更）
简要说明动机或上下文。
```

### 规则

- **标题行**: 必填，`[模块] + 动作描述`，不超过 50 字
- **空行**: 标题与正文之间必须空一行
- **正文**: 必填，至少包含"变更内容"，列举具体改动
- **原因/背景**: 推荐填写，尤其是非显而易见的变更

## 模块前缀

| 前缀 | 适用场景 |
|------|---------|
| `[初始化]` | 项目初始提交、重大基础设施搭建 |
| `[中控]` | meta/ 下的中控 Agent 代码 |
| `[蓝图]` | blueprint/ 下的通用模板 |
| `[注册]` | registry 相关 |
| `[编排]` | orchestrator / task chain 相关 |
| `[路由]` | intent router 相关 |
| `[记忆]` | memory 相关 |
| `[自举]` | bootstrap engine 相关 |
| `[身份]` | identity core 相关 |
| `[接口]` | api / 通信协议相关 |
| `[前端]` | ui / 前端相关 |
| `[文档]` | docs/ 下的文档变更 |
| `[配置]` | config/ 下的配置变更 |
| `[工程]` | 依赖、CI、项目结构等基础设施 |
| `[修复]` | 跨模块 bug 修复 |
| `[Agent:名称]` | 特定 Agent，如 `[Agent:Houdini]` |

## 动作词

优先使用：新增、实现、修复、重构、优化、更新、移除、调整

## 准确性要求

- 描述必须准确反映实际变更内容，严禁臆造或猜测
- 先阅读变更文件再撰写提交信息，不可凭假设描述
- 文件用途描述应从文件自身内容获取，而非推测

## 示例

### 常规提交

```
[中控] 实现意图路由的 capability 匹配逻辑

变更内容:
- 新增 match_capability() 函数，支持模糊匹配和优先级排序
- 更新 IntentRouter 类，接入匹配逻辑
- 新增单元测试 test_match_capability.py

原因/背景:
中控需要根据用户意图自动选择最合适的 Agent，之前是硬编码映射。
```

### 修复提交

```
[编排] 修复任务链中上游交付物路径传递错误

变更内容:
- 修正 TaskChain.pass_artifact() 中路径拼接使用 os.path.join 的问题
- 改为 pathlib.Path，兼容 Windows

原因/背景:
Windows 环境下路径分隔符导致下游 Agent 找不到交付物文件。
```

### 大型/初始化提交

```
[初始化] MetaAgent 项目基础架构与会话摘要系统

项目文档:
- docs/ARCHITECTURE.md: 数字分身多Agent自举系统架构文档
- docs/adr/001: 会话摘要方案架构决策记录
- docs/plans/: 已归档的实施计划

会话摘要系统:
- .cursor/hooks/summarize_session.py: 自动摘要脚本（纯标准库）
- .cursor/hooks.json: stop + sessionEnd 双 hook 配置

AI Agent Skills:
- code-review / code-standards / commit-conventions / dev-workflow / step-by-step-design
```

## 提交前检查点（强制）

执行 `git commit` 之前，必须向用户确认是否已完成代码审查。使用以下问句：

> 本次变更是否已经过审查（code-review Skill 或手动审查）？

- 用户确认"已审查"或"跳过" → 继续提交
- 用户未回应或要求审查 → 先按 code-review Skill 执行审查，审查通过后再提交
- 仅文档变更、会话摘要更新等非代码改动可跳过审查

## 原则

- 一次提交做一件事，粒度适中
- 描述"做了什么、为什么做"而非"改了哪个文件"
- 涉及多模块时用最主要的模块作前缀
- 破坏性变更在正文中标注 `⚠️ 破坏性变更`

## 技术提示（PowerShell 环境）

PowerShell 不支持 heredoc 语法。多行提交信息使用多个 `-m` 参数：

```powershell
git commit -m "[模块] 标题" -m "变更内容:" -m "- 改动1" -m "- 改动2" -m "原因/背景:" -m "说明文字"
```

每个 `-m` 之间 Git 会自动插入空行。

## 自动会话摘要

本项目配置了 Cursor hooks（`.cursor/hooks.json`），在 AI 对话过程中自动生成会话摘要：

- **触发机制**：Cursor `stop` hook（每次 AI 回复后）+ `sessionEnd` hook（对话结束时）
- **摘要存储**：`docs/summaries/YYYY-MM-DD-{session_id}.md`
- **无需手动操作**：摘要由 `.cursor/hooks/summarize_session.py` 自动生成

### 提交摘要文件

- `git add` 时检查 `docs/summaries/` 下是否有新增或更新的摘要文件
- 如有，一并 stage 并提交（使用 `[文档]` 前缀）
- 示例：`[文档] 更新会话摘要`
