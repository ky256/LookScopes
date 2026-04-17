# 待蒸馏信号

<!-- signal_count:0 | last_updated: -->

## [2026-04-17] 会话 f17704 — MetaAgent 基础设施迁移至 Cursor 及 Windows 兼容性修复
- 信号类型: correction
- 描述: AI 误将对话收集文件 .pending.md 加入 .gitignore，用户纠正指出该文件需跟随项目版本控制，AI 随即修正。
- 信号类型: failure
- 描述: 迁移后发现 .pending.md 为空，原因为：1) ide_compat.py 中 CodeBuddy 兜底逻辑在双 IDE 共存时优先级高于 Cursor 环境变量；2) Windows 下 Cursor 传入的 stdin JSON 包含 UTF-8 BOM 导致解析失败；3) hooks.json 使用 $CURSOR_PROJECT_DIR 变量在 Windows cmd 下无法展开。均已定位并修复。
- 信号类型: preference
- 描述: 用户表达了对 Git 操作的谨慎态度，选择在 IDE 中手动点击 Push 按钮，而非让 AI 自动执行 git push。
- 摘要上下文:
  - 评估并执行了将 MetaAgent 系统（会话记忆、经验积累、知识蒸馏、Memory MCP）迁移到 Cursor IDE 的任务，结论为复杂度极低（上游已提供完整 Cursor 支持）。
  - 完成迁移落地：复制基础设施、合并 MCP 配置、创建目录骨架及种子文件，并将 hooks.json 中的 python3 改为 python 以兼容 Windows。
  - 排查并修复对话收集系统为空的问题：发现并修正了 ide_compat.py 中 CodeBuddy 兜底逻辑覆盖 Cursor 路径的 Bug，以及 Windows 下 Cursor 传入 stdin JSON 带 UTF-8 BOM 导致解析失败的 Bug。
  - 将上游 MetaAgent 仓库的两个 Bug 修复提交到独立的 feature 分支 fix/cursor-windows-compat，并随后补充修复了上游 hooks.json 的 Windows 路径变量不展开问题。
  - 指导用户完成 Git 分支推送操作，并详细解释了基于 PR 的分支合并流程及其相较于直接 commit 到 main 的优势。
  - 决策: 迁移采用最小可用集优先策略：先装基础设施和 MCP，Rules 和 Skills 按需选装。
  - 决策: hooks.json 中将 python3 替换为 python，使用相对路径代替 $CURSOR_PROJECT_DIR，以兼容 Windows 环境。
  - 决策: .pending.md 对话收集内容必须纳入 Git 版本控制（从 .gitignore 中移除）。
  - 决策: 上游 Bug 修复采用独立 feature 分支（fix/cursor-windows-compat）开发，推荐通过 PR 合并回 main 而非直接 merge。
  - 决策: 暂不修改 meta-agent-identity.mdc，待确定多 subagent 协作模式后再启用。
- 详情: details/2026-04-17.md#L1-L891


## [2026-04-17] 会话 f17704 — MetaAgent迁移至LookScopes及跨机记忆提交策略优化
- 信号类型: pattern
- 描述: AI IDE 的 stop hook 持续向被 git 追踪的缓冲文件追加内容，会导致工作区永远处于 dirty 状态，这是持续写入与快照版本控制的结构性冲突
- 信号类型: preference
- 描述: 用户明确偏好跨机器记忆连续性，拒绝将 .pending.md 从 git 追踪中移除，认为丢失一次无价值的元操作记录换取跨机连续性是完全可接受的 trade-off
- 信号类型: failure
- 描述: 尝试使用 git reset/revert 机制回滚已推送的 .pending.md 记录失败，因为这些操作会导致本地与远程账本不一致或破坏跨机连续性，最终改用 git checkout HEAD 丢弃未提交的增量
- 摘要上下文:
  - 确认 GitHub PR 页面信息无误，合并 fix/cursor-windows-compat 分支至 main，修复了 Windows 下 BOM stdin、CodeBuddy 优先级和 shell 变量三个兼容性 Bug
  - 本地执行 git 清理操作，拉取最新 main 并删除已合并的本地 fix 分支
  - 确认端到端记忆系统（DetailCollector、归档、LLM摘要）在 LookScopes 中完整运行
  - 将 LookScopes 的 MetaAgent 基础设施和首次运行产生的记忆数据分两个 commit 提交
  - 讨论 .pending.md 因 hook 持续追加导致工作区永远 dirty 的问题，评估了 gitignore、skip-worktree 和 checkout 回退等方案
  - 最终确认采用 git checkout HEAD -- 回退方案：丢弃每次 commit/push 后 hook 产生的‘提交元操作’记录，以保持工作区干净且支持跨机器记忆连续
  - 决策: 确认 LookScopes 仓库为 private，包含用户路径的 memory 文档直接提交无需脱敏
  - 决策: 将 LookScopes 的变更拆分为基础设施(feat)和记忆数据(chore)两个语义清晰的 commit
  - 决策: 拒绝将 .pending.md 加入 .gitignore，以保证跨机器记忆连续性
  - 决策: 采用 git checkout HEAD -- Docs/memory/details/.pending.md 方案清理工作区，丢弃 commit 触发的元操作记录
- 详情: details/2026-04-17.md#L892-L1551

