---
name: code-review
description: >-
  MetaAgent 项目代码审查流程。通过并行 subagent 实现多维度自动审查：
  linter检查、架构合规、逻辑审查、失败模式匹配。适用于完成重要编码后、
  提交前、用户要求审查代码时触发。
---

# 代码审查

## 何时触发

- 用户说"审查一下" / "review" / "检查代码"
- 完成一个完整功能模块的编码后（用户确认需要审查时）
- 提交前用户要求全面检查

## 审查流程

### 1. 确定审查范围

先明确本次审查涉及哪些文件。优先使用 git diff，无 git 时由用户指定或根据本次会话编辑记录确定。

### 2. 并行派发 subagent 审查

同时启动最多 4 个 subagent，各负责一个审查维度。**必须在同一条消息中发起所有 Task 调用以实现并行。**

> **关于 model 参数：** Cursor subagent 只支持 `model: "fast"`（轻量快速）或不指定（继承当前对话模型）。
> 不能指定具体 LLM 供应商（Claude / GPT 等），供应商由 Cursor 全局设置决定。
> 策略：自动化检查和模式匹配用 fast 省成本，逻辑审查继承父级以获得最强推理。

#### Agent A: 自动化检查

```
subagent_type: shell
model: fast
prompt 要点:
- 对变更文件运行 linter（ruff check 或 flake8）
- 运行 type checker（mypy --strict 目标文件）
- 检查 import 排序（isort --check-only --diff）
- 汇总：通过/失败 + 具体错误列表
```

#### Agent B: 架构合规

```
subagent_type: explore
model: fast
prompt 要点:
- 读取 docs/ARCHITECTURE.md 第十一节（项目结构）
- 读取变更文件列表
- 检查：
  a) 新文件是否放在正确的模块目录下
  b) 是否存在跨层直接引用（如 ui/ 直接 import orchestrator/ 内部实现）
  c) 公开接口是否有 docstring（Google 风格）
  d) 配置值是否硬编码（应在 config/ 中管理）
- 汇总：合规 / 发现N个问题 + 具体列表
```

#### Agent C: 逻辑与质量审查

```
subagent_type: generalPurpose
model: （不指定，继承父级模型，获得最强推理能力）
prompt 要点:
- 逐文件审查变更代码
- 检查维度：
  a) 逻辑正确性：边界条件、空值处理、异常路径
  b) 异步安全：await 遗漏、并发竞争
  c) 错误处理：异常是否被吞掉、是否用了项目异常基类
  d) 命名与可读性：是否表意清晰
  e) 潜在性能问题：不必要的循环、重复计算
- 严重度分级：
  🔴 Critical — 必须修复，会导致运行时错误或数据丢失
  🟡 Warning  — 建议修复，可能导致问题或降低可维护性
  🟢 Info     — 可选改进，代码风格或优化建议
- 汇总：按严重度排序的发现列表
```

#### Agent D: 失败模式匹配

```
subagent_type: explore
model: fast
prompt 要点:
- 读取 docs/failures/（如存在）中的已知失败模式
- 对照变更代码检查是否命中已知模式
- 同时检查常见 Python 陷阱：
  a) 可变默认参数（def f(x=[])）
  b) 在循环中 await 单个请求（应批量）
  c) 裸 except 捕获所有异常
  d) 路径拼接用字符串而非 pathlib
  e) 日志用 print 而非 logging
- 如 docs/failures/ 不存在则跳过已知模式检查，只做通用陷阱检查
- 汇总：匹配到的模式列表 + 建议修复方式
```

### 3. 汇总审查报告

所有 subagent 返回后，主 Agent 汇总为统一报告：

```markdown
## 代码审查报告

### 概览
- 审查文件: N 个
- 发现问题: X 个（🔴 a / 🟡 b / 🟢 c）
- 自动化检查: ✅ 通过 / ❌ 未通过
- 架构合规: ✅ 合规 / ⚠️ N个问题

### 🔴 Critical
1. [文件:行号] 问题描述 — 建议修复方式

### 🟡 Warning
1. [文件:行号] 问题描述 — 建议修复方式

### 🟢 Info
1. [文件:行号] 问题描述 — 建议修复方式

### 自动化检查详情
（linter / type check / import 排序的具体输出）
```

### 4. 处理发现

- **🔴 Critical**: 立即修复，修复后重新运行对应检查
- **🟡 Warning**: 与用户讨论是否修复
- **🟢 Info**: 列出供参考，用户决定

### 5. 记录失败模式（可选）

如果本次审查发现了新的、有价值的失败模式，追加到 `docs/failures/` 中：

```markdown
# docs/failures/NNN-简短标题.md

- 日期: YYYY-MM-DD
- 发现于: 哪个模块/文件
- 模式: 一句话描述
- 症状: 会导致什么问题
- 修复: 正确做法
- 示例:
  错误: ...
  正确: ...
```

## 轻量审查模式

对于小改动（<50行 / 单文件修改），不必启动全部 4 个 subagent。只运行：
- Agent A（自动化检查）+ Agent C（逻辑审查）

判断标准：改动行数少且集中在单文件时用轻量模式，其余用完整模式。

## 前置条件

审查前确保项目中已安装必要工具。如未安装，Agent A 跳过对应检查并在报告中注明：

```
ruff    — linter（推荐）
mypy    — type checker
isort   — import 排序检查
```
