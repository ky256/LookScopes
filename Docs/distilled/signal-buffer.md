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

