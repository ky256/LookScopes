---
name: session summary via Cursor hooks
overview: 创建会话摘要机制：Cursor 原生 stop hook 定期触发 + sessionEnd 兜底，通过 Python 脚本调用 CodeBuddy API（国内免费模型）生成增量摘要，存入仓库。Fallback 到本地 Ollama。
todos:
  - id: write-adr
    content: "写 ADR-001: 会话摘要方案选择 — 先记录决策再实现"
    status: completed
  - id: create-script-and-hooks
    content: 创建 .cursor/hooks/summarize_session.py + .cursor/hooks.json — 脚本 + hook 配置一起完成
    status: completed
  - id: validate-hooks
    content: 冒烟测试第一步 — 验证 Cursor hooks 触发机制（不含 LLM 调用）
    status: completed
  - id: validate-e2e
    content: 冒烟测试第二步 — 端到端验证 LLM 调用 + 摘要生成
    status: completed
  - id: update-commit-conventions
    content: 更新 commit-conventions Skill — 说明自动摘要机制
    status: completed
  - id: archive-plan
    content: 将本 Plan 归档到 docs/plans/session-summary.plan.md
    status: completed
isProject: false
---

# 会话摘要机制：Cursor Hooks + CodeBuddy

## 方案总结

- **触发机制**: Cursor 原生 hooks，双层配合
  - `stop` hook（主力）: 每次 AI 回复后触发，脚本内部节流
  - `sessionEnd` hook（锦上添花）: 对话结束时生成最终摘要，但不依赖其可靠性
- **更新策略**: 旧摘要 + 新增内容 → LLM 合并更新（非全量重生成）
- **摘要引擎（主）**: CodeBuddy API — glm-5.0（已验证可用，国内免费无限，注意小写）
- **摘要引擎（备）**: 本地 Ollama — 离线时 fallback
- **失败策略**: 静默跳过，不阻塞任何工作流，下次重试
- **存储位置**: `docs/summaries/YYYY-MM-DD-{session_id_6char}.md`，随项目进仓库
- **关键优势**: 环境变量 `CURSOR_TRANSCRIPT_PATH` 直接提供 transcript 路径

> **范围约束**: 本计划只做摘要自动化。文档体系重构（devlog 与摘要共存、五层体系、Plan 归档流程）
> 作为独立 Plan 后续推进，待摘要机制验证稳定后再做。

## 数据流

```
Cursor 自动记录               Hooks 定期提炼              仓库持久化
┌───────────────────┐     ┌───────────────────┐     ┌─────────────────┐
│ .jsonl transcript │     │ stop hook（主力）   │     │ docs/summaries/ │
│ (完整原始对话)     │ ──→ │ 每轮触发,节流执行   │ ──→ │ 结构化摘要文件   │
│ Cursor 自动写入    │     │                   │     │ 跟着仓库走       │
│                   │     │ sessionEnd（兜底） │     │                 │
│                   │ ──→ │ 对话结束时,不强依赖 │ ──→ │                 │
└───────────────────┘     └───────────────────┘     └─────────────────┘
     已有，不用动              我们要建的               持久化到 git
```

## 实现约束（审核修复项）

以下约束来自两轮 subagent 审核，编码时**必须遵守**:

1. **原子写入用 `os.replace()` 而非 `os.rename()`** — Windows 上 `os.rename()` 目标存在时抛 `FileExistsError`，`os.replace()` 保证原子覆盖（Python 3.3+）
2. **状态文件按会话隔离，存 `.cursor/hooks/` 下** — `.cursor/hooks/.summary_state_{session_id_6char}.json`，与日志文件同目录集中管理；启动时清理 7 天以上旧 state 文件
3. **HTTP 请求必须设超时** — CodeBuddy 60s / Ollama 120s / 脚本整体 180s 硬超时（threading watchdog）
4. **所有文件 I/O 显式 `encoding='utf-8'`** — Windows 默认 GBK，中文必乱码
5. **stdin 读取加超时保护** — 用 `sys.stdin.buffer.read(65536)` 限制最大读取量 + try/except，防 Cursor 未关闭 pipe 时无限阻塞
6. **JWT 路径用 glob 发现** — `glob.glob()` 搜索 `%LOCALAPPDATA%/CodeBuddyExtension/**/Tencent-Cloud.coding-copilot.info`，处理版本号目录；401 响应时日志记录 "JWT expired" 并 fallback
7. **环境变量缺失时 graceful fallback** — `CURSOR_PROJECT_DIR` 缺失则用 `Path(__file__).resolve().parent.parent.parent` 推导；`CURSOR_TRANSCRIPT_PATH` 缺失或文件不存在或为空 → exit 0
8. **目录自动创建** — `os.makedirs("docs/summaries", exist_ok=True)`
9. **模型上下文参数化** — glm-5.0 官方 200K tokens / Ollama 8B 按实际 context_length；prompt 总长超 80% 时压缩旧摘要；支持 `SUMMARY_MAX_CONTEXT` 环境变量覆盖

## 实现细节

### 1. Cursor Hook 配置 `.cursor/hooks.json`

```json
{
  "version": 1,
  "hooks": {
    "stop": [
      {
        "command": "py -3 .cursor/hooks/summarize_session.py --mode throttled"
      }
    ],
    "sessionEnd": [
      {
        "command": "py -3 .cursor/hooks/summarize_session.py --mode final"
      }
    ]
  }
}
```

- 使用 `py -3` 而非 `python`，避免 Windows 上指向错误环境
- 配置在项目 `.cursor/` 目录下，跟着仓库走
- Cursor 自动监测并热加载

### 2. Python 摘要脚本 `.cursor/hooks/summarize_session.py`

#### 依赖策略

**纯标准库实现，零第三方依赖。** 具体:

- HTTP 请求: `urllib.request`（所有请求显式 `timeout` 参数）
- SSE 解析: 手动按行读取（详见 SSE 解析规格）
- JSON: `json` 标准库
- 文件锁: `msvcrt`（Windows）防多窗口并发
- 文件 I/O: 所有 `open()` 显式 `encoding='utf-8'`
- 路径发现: `glob.glob()` 用于 JWT 文件查找
- 日志: `logging` + `RotatingFileHandler`（标准库，自动轮转）

#### 输入来源

- **stdin**: Cursor 传入的 JSON（含 session_id, status 等）
  - 读取方式: `sys.stdin.buffer.read(65536)` 限制最大量 + try/except
  - 超时保护: 如果 stdin 为空或读取异常，graceful exit 0
- **环境变量**:
  - `CURSOR_TRANSCRIPT_PATH` — transcript 文件路径（缺失则 exit 0）
  - `CURSOR_PROJECT_DIR` — 项目根目录（缺失则从脚本路径推导: `Path(__file__).resolve().parent.parent.parent`）
- **调试 CLI 参数**:
  - `--transcript PATH` — 手动指定 transcript 文件路径
  - `--project-dir PATH` — 手动指定项目根目录

#### 启动快速路径

为减少每次 stop hook 的开销，脚本在 import 之后立即执行**快速节流检查**:

1. 构建当前会话的 state 文件路径: `.cursor/hooks/.summary_state_{session_id_6char}.json`
2. 检查 state 文件的 `mtime`（文件系统级，不解析 JSON）
3. `mtime` < **3 分钟**前 → 立即 `sys.exit(0)`（只过滤高频重复调用）
4. 通过快速检查后（>=3 分钟），才进入完整逻辑（读 JSON state、读 transcript、检查轮次等）

> **设计说明**: 快速路径阈值（3 分钟）远小于完整节流阈值（15 分钟），
> 确保高密度对话（3-15 分钟内 >=10 轮）仍能通过轮次条件触发摘要更新。

#### 两种运行模式

`**--mode throttled`**（stop hook 调用，主力）:

1. 读 stdin 获取 session_id
2. 用 `CURSOR_PROJECT_DIR` 构建绝对路径，读 `.cursor/hooks/.summary_state_{session_id_6char}.json`
3. 节流检查:
  - 距上次摘要 < 10 轮（1 轮 = transcript 中 1 对 user+assistant 消息）且 < 15 分钟 → exit 0（跳过）
  - 否则 → 继续
4. 读 `CURSOR_TRANSCRIPT_PATH` 指向的 .jsonl
5. 验证 `last_summary_line` 是否超过实际行数（超过则重置为 0，日志告警）
6. 解析: 提取 user/assistant 文本，过滤工具调用和搜索结果噪音
7. 截断策略: transcript 超过 50K 字符时，保留最近部分 + 开头概述
8. 增量更新:
  - 已有摘要 → LLM prompt: "现有摘要 + 第 N 行之后的新增对话 → 生成更新版摘要"
  - 无摘要 → LLM prompt: "全部内容 → 生成首份摘要"
  - Prompt 总长度估算: 如果现有摘要 + 新内容超过模型上下文 80%，压缩现有摘要只保留每节最近 N 条
9. 写入 `docs/summaries/YYYY-MM-DD-{session_id_6char}.md`（原子写入: 先写 `.tmp` 再 `os.replace()`）
10. 更新 `.cursor/hooks/.summary_state_{session_id_6char}.json`（同样 `os.replace()` 原子写入）
11. 写日志（`RotatingFileHandler`，最大 1MB，保留 2 个备份）
12. 清理 7 天以上旧 `.cursor/hooks/.summary_state_*.json` 文件

`**--mode final`**（sessionEnd hook 调用，兜底）:

1. 跳过节流检查
2. 基于现有摘要 + 自上次以来的新增内容，生成最终版
3. 其余逻辑同上

#### 超时与进程保护

- **HTTP 请求超时**: `urllib.request.urlopen(req, timeout=60)` (CodeBuddy) / `timeout=120` (Ollama)
- **SSE 流读取超时**: 逐行读取时检查 elapsed time，超过 90s 中断
- **脚本整体硬超时**: threading watchdog，180s 后 `os._exit(1)` 强制退出，防僵尸进程

#### 节流状态文件 `.cursor/hooks/.summary_state_{session_id_6char}.json`

```json
{
  "session_id": "c14b191a",
  "last_summary_line": 42,
  "last_summary_time": "2026-03-13T15:30:00",
  "turns_since_last": 3,
  "summary_file": "docs/summaries/2026-03-13-c14b19.md"
}
```

- 存储位置: `.cursor/hooks/` 目录下，与日志文件同目录集中管理
- 按会话隔离: 每个 session 一个文件，避免多窗口互相覆盖
- 加入 `.gitignore`: `.cursor/hooks/.summary_state_*`
- 使用 `CURSOR_PROJECT_DIR` 构建绝对路径读写
- 原子写入: `os.replace()` 保证 Windows 兼容
- 文件锁: `msvcrt.locking()` 防止同会话并发
  - 获取时机: step 4（读 transcript）之前
  - 释放时机: step 10（更新 state）之后
  - 快速路径在获取锁之前就退出（不写任何文件，无需锁）
- 自动清理: 脚本启动时删除 mtime > 7 天的旧 state 文件

#### 引擎选择与调用

**CodeBuddy（主引擎）:**

- Endpoint: `POST https://copilot.tencent.com/v2/chat/completions`
- Auth JWT 路径发现: `glob.glob(os.path.join(LOCALAPPDATA, "CodeBuddyExtension", "**", "Tencent-Cloud.coding-copilot.info"), recursive=True)`
  - 找不到 → 日志提示 "CodeBuddy 未安装或未登录" → fallback Ollama
- 必须 `stream: true`，用标准库手动解析 SSE
- HTTP 401 响应 → 日志记录 "JWT expired, falling back to Ollama"
- 模型: `glm-5.0`（已验证可用，注意必须小写，`GLM-5.0` 会 400 报错）
- 模型名可配置: 环境变量 `SUMMARY_MODEL` 覆盖默认值
- 上下文窗口: 200K tokens（官方规格），80% 阈值 = 160K tokens
- 请求超时: 60s

**Ollama（fallback）:**

- Endpoint: `POST http://localhost:11434/api/generate`
- 模型: 可用的 8B 级别
- 上下文窗口: 按实际模型 context_length（通常 8K-32K），输入截断至 8K 字符
- 请求超时: 120s（本地推理较慢）

上下文窗口大小支持 `SUMMARY_MAX_CONTEXT` 环境变量覆盖（单位: tokens）。

**优先级:** CodeBuddy 可达 → Ollama 可达 → 静默跳过（写日志警告）

#### SSE 解析规格

CodeBuddy 返回 Server-Sent Events 格式，解析逻辑:

```
逐行读取响应 body（encoding='utf-8'）
  → 空行: 跳过（事件分隔符）
  → 不以 "data: " 开头: 跳过（如 event:、id: 等字段）
  → "data: [DONE]": 结束读取
  → "data: {json}": 
      JSON parse → 提取 .choices[0].delta.content
      拼接到结果字符串
  → JSON parse 失败: 跳过该行，日志告警
```

多行 `data:` 字段按 SSE 规范拼接（换行连接）。

#### 摘要 Prompt 设计

**System prompt:**

```
你是一个开发会话摘要助手。根据 Cursor IDE 的对话记录，生成结构化的会话摘要。
规则:
- 忽略工具调用细节、搜索结果、文件内容等噪音，聚焦于讨论和决策
- "讨论要点"只列关键议题，不复述对话
- "做出的决策"必须明确记录选择了什么、放弃了什么
- "用户偏好"提取用户表达的风格/流程/工具偏好
- 使用中文
```

**增量更新 prompt:**

```
以下是现有摘要和新增的对话内容。请更新摘要，保留已有信息，整合新内容。
如果新对话修改了之前的决策，更新对应条目并标注"[已更新]"。

## 现有摘要
{existing_summary}

## 新增对话（第 {start_line} 行之后）
{new_conversation}

请输出更新后的完整摘要。
```

#### LLM 输出格式验证

生成摘要后进行基本格式校验:

- 检查是否包含 `## 讨论要点`、`## 做出的决策` 等必要标题
- 校验失败 → 保留上一版摘要不覆盖，日志记录 "LLM output format invalid"
- 首次生成（无历史摘要）且校验失败 → 仍写入但日志告警

#### 摘要输出格式

```markdown
# 会话摘要: [主题]

- 日期: YYYY-MM-DD
- Session: {session_id_6char}
- 轮次: 约 N 轮对话（1 轮 = 1 对 user+assistant 消息）
- 引擎: CodeBuddy/glm-5.0
- 最后更新: YYYY-MM-DD HH:MM

## 讨论要点
- ...

## 做出的决策
- ...

## 发现的用户偏好
- ...

## 下一步行动
- ...
```

#### 脚本参数

- `--mode`: `throttled`（节流）/ `final`（最终）
- `--engine`: `auto`(默认) / `codebuddy` / `ollama`
- `--model`: 覆盖默认模型名
- `--force`: 跳过节流，强制生成
- `--dry-run`: 执行所有逻辑但不调用 LLM、不写文件，打印 prompt + 状态信息
- `--transcript PATH`: 手动指定 transcript 路径（调试用）
- `--project-dir PATH`: 手动指定项目根目录（调试用）

### 3. 更新 [commit-conventions Skill](.cursor/skills/commit-conventions/SKILL.md)

- 说明摘要由 Cursor hooks 自动生成
- commit 前确认 `docs/summaries/` 下有变更时 stage 它

### 4. 项目结构变更

新增:

- `.cursor/hooks.json` — Cursor hook 配置（进仓库）
- `.cursor/hooks/summarize_session.py` — 摘要脚本（进仓库）
- `.cursor/hooks/summarize.log*` — 运行日志 + 轮转备份（加入 .gitignore，RotatingFileHandler 自动轮转 1MB x 2）
- `.cursor/hooks/.summary_state_*.json` — 按会话隔离的节流状态（加入 .gitignore，7 天自动清理）
- `docs/summaries/` — 摘要存储目录（脚本自动创建）

### 5. 与未来 Unified Memory 的兼容性

摘要中的结构化字段为未来中控 Agent 的 Unified Memory 预留了迁入路径:

- "做出的决策" → semantic memory（跨 Agent 元知识）
- "用户偏好" → identity/preferences（身份核心）
- "下一步行动" → task chain memory（任务链记忆）

当前只做 Markdown 文件存储，未来迁入数据库时按字段提取即可。

## 风险清单


| 风险                     | 影响             | 缓解措施                                                      |
| ---------------------- | -------------- | --------------------------------------------------------- |
| Windows hooks 不触发      | 功能完全失效         | 冒烟测试分两步验证（TODO #4/#5），失败则 fallback 到 Skill 驱动             |
| CodeBuddy 网络不可达        | 主引擎不可用         | 自动 fallback Ollama，都不行则静默跳过                               |
| iOA 拦截 Python HTTPS    | CodeBuddy 调用失败 | 确保 Python 进程白名单                                           |
| 长会话 token 超限           | 摘要生成失败或截断      | transcript 截断策略（50K/8K），prompt 总长超 80% 上下文时压缩旧摘要          |
| 多窗口并发                  | 状态文件覆盖         | 状态文件按 session 隔离（`.cursor/hooks/`下）+ 文件锁 + `os.replace()` |
| Cursor hooks API 变更    | 脚本失效           | 脚本加容错，未知输入时 graceful skip                                 |
| transcript .jsonl 格式变更 | 解析失败           | 解析器加 try/except，未知格式 skip 不 crash                         |
| 摘要脚本自身 crash/挂起        | 僵尸进程或静默失败      | 180s 硬超时 watchdog + 日志文件 + RotatingFileHandler            |
| `py -3` 命令不存在          | 脚本无法运行         | 冒烟测试验证，文档说明 Python 安装要求                                   |
| session_id 短码碰撞        | 文件名冲突          | 使用 6 位 + 日期前缀，碰撞概率极低                                      |
| JWT token 过期           | CodeBuddy 401  | 捕获 401 日志记录，fallback Ollama                               |
| JWT 文件路径变化             | 找不到认证文件        | glob 递归搜索，找不到时 fallback                                   |
| Windows 文件编码           | 中文乱码           | 所有 `open()` 显式 `encoding='utf-8'`                         |
| stdin pipe 未关闭         | 脚本阻塞           | `buffer.read(65536)` 限制 + try/except                      |
| LLM 输出格式偏差             | 增量更新累积退化       | 输出格式校验，失败时保留旧版                                            |


## 范围外（后续独立 Plan）

以下内容有意不包含在本计划中，待摘要机制验证稳定后独立推进:

- 文档体系重构（五层体系定义）
- devlog 与摘要的共存方案
- Plan 归档流程
- dev-workflow Skill 更新

