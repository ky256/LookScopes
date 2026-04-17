---
name: Meta-Learning Plans Fix
overview: 基于 Architect 和 Verifier 的全量审查结果，修复四份元学习规划文档中的 4 个 BLOCKER、7 个 DEFECT，补全缺失的跨层接口定义和 todo 项，使文档达到可实施状态。
todos:
  - id: b1-preference-antipattern
    content: "BLOCKER: 更新 L2 文档，偏好/反模式载体以 L3 决策为准，删除 docs/identity/ 引用"
    status: completed
  - id: b2-distill-status
    content: "BLOCKER: L1 数据模型新增 _distill_status 字段，更新 l1-format-change todo 描述"
    status: completed
  - id: b3-l1-hints
    content: "BLOCKER: L1 文档新增 l1-hints.json 消费机制描述 + 新增 todo"
    status: completed
  - id: b4-arch-update
    content: "BLOCKER: 规划 ARCHITECTURE.md 元学习章节更新（登记 todo）"
    status: completed
  - id: d1-yaml-to-json
    content: "DEFECT: L1 文档全面替换 YAML → JSON（overview/示例/路径/mermaid）"
    status: completed
  - id: d2-foundation-sync
    content: "DEFECT: Foundation 文档标注 docs/corrections/ 已改为 docs/learnings/"
    status: completed
  - id: d3-metrics-unify
    content: "DEFECT: 统一度量文件到 docs/distilled/metrics/，更新 L2 文档路径"
    status: completed
  - id: d4-reflection-boundary
    content: "DEFECT: 增加 Agent 级 vs 系统级反思边界说明"
    status: completed
  - id: d5-session-topics
    content: "DEFECT: L1 数据模型新增 session_topics 字段"
    status: completed
  - id: d6-llm-estimate
    content: "DEFECT: L2 文档补充 Phase 2/3 的 LLM 消耗估算"
    status: completed
  - id: d7-missing-todos
    content: "DEFECT: 补全缺失 todo（l1-hints-consume/l1-session-topics/l2-doc-sync/e2e-test）"
    status: completed
isProject: false
---

# 元学习规划文档修复计划

基于 Architect（架构合规审查）和 Verifier（完整性与一致性验证）的交叉审查，四份文档存在 **4 个 BLOCKER、7+ 个 DEFECT**，需在实施前全部修复。

## BLOCKER 修复（4 项，实施前必须完成）

### B1. 偏好/反模式存储位置矛盾（L2 vs L3）

**问题**：L2 定义 `preference:persist` → `docs/identity/preferences.json`，L3 改为 → `meta-agent-identity.mdc` 偏好段落。反模式同理，L2 → `docs/identity/antipatterns.json`，L3 → `.cursor/skills/antipatterns/SKILL.md`。

**决策建议**：以 L3（最后定稿）为准，因为更贴合 Cursor 原生注入通道。

**修改**：

- [knowledge_distillation_layer_ee64dc6d.plan.md](c:\Users\THINKSTATION.cursor\plans\knowledge_distillation_layer_ee64dc6d.plan.md) section 二 产物类型表（line 87-95）：更新 `preference:persist` 目标载体为 `meta-agent-identity.mdc`，`antipattern:create` 目标载体为 `.cursor/skills/antipatterns/SKILL.md`
- 同文件 section 十 目录结构（line 320-342）：删除 `docs/identity/` 整个子树
- 同文件 section 十二 与三层记忆映射 mermaid 图：更新 `prefs` 和 `anti` 节点指向新位置

### B2. L1 数据模型缺少 `_distill_status` 字段

**问题**：L2 Step 1 读取 `_distill_status != "distilled"`，Step 6 写入 `"distilled"`，但 L1 数据模型无此字段。

**修改**：

- [experience_collection_layer_56740b02.plan.md](c:\Users\THINKSTATION.cursor\plans\experience_collection_layer_56740b02.plan.md) section 二 数据模型中，在顶级字段增加 `"_distill_status": "pending"`
- 同文件 todo `l1-format-change` 描述修改为包含此字段新增

### B3. L1 缺少 l1-hints 消费机制（闭环断裂）

**问题**：L3 Phase 2 输出 `l1-hints.json`（heightened/suppressed categories），但 L1 文档完全未提及消费此文件。

**修改**：

- [experience_collection_layer_56740b02.plan.md](c:\Users\THINKSTATION.cursor\plans\experience_collection_layer_56740b02.plan.md) 在 section 二 "Stage 1 快速筛选器" 之前新增一段："启动时读取 `docs/distilled/metrics/l1-hints.json`（如存在），按 heightened categories 降低 Stage 1 过滤阈值（增加灵敏度），按 suppressed categories 提高阈值"
- 新增 todo：`l1-hints-consume`

### B4. ARCHITECTURE.md 缺少元学习章节

**问题**：元学习三层是对系统架构的重大扩展，但 ARCHITECTURE.md 无任何对应内容。

**修改**：新增 todo 规划 ARCHITECTURE.md 更新（不在本次文档修复范围，但需明确登记）：

- 新增"纵向自举 / Meta-Learning Engine"章节（与 section 六 Bootstrap Engine 对等）
- 明确 Blueprint DNA `reflection/` 是 Agent 级反思，与系统级元学习的边界
- 更新 Identity Core 引用元学习产物
- 更新 Unified Memory 映射

## DEFECT 修复（7 项）

### D1. L1 格式 YAML → JSON 未同步

- [experience_collection_layer_56740b02.plan.md](c:\Users\THINKSTATION.cursor\plans\experience_collection_layer_56740b02.plan.md)：
  - overview 中 "YAML" → "JSON"
  - section 二 标题和示例从 YAML 改为 JSON
  - 文件路径 `.yaml` → `.json`
  - mermaid 图中 `.yaml` → `.json`
  - todo `yaml-output` 改名/描述为 JSON

### D2. Foundation `docs/corrections/` 过期

- [meta-learning_foundation_3a310216.plan.md](c:\Users\THINKSTATION.cursor\plans\meta-learning_foundation_3a310216.plan.md) line 69：加注 "（后续 L1 详细规划中改为 `docs/learnings/`）"

### D3. 度量文件位置统一

**决策建议**：

- L2 的聚合统计 `metrics.json` → 合并到 `docs/distilled/metrics/` 目录下
- L3 的信号文件 `rule-signals.json`, `l1-hints.json`, `system-health.json` → 已在 `docs/distilled/metrics/`
- 周报 `_reports/` → 移到 `docs/distilled/reports/`

**修改**：更新 L2 section 八 和 section 十 的路径

### D4. reflection/ 与元学习边界定义

在 L3 或 Foundation 文档中增加一段说明：

- Agent 级反思（Blueprint `reflection/`）：单 Agent 域内自学习
- 系统级元学习（三层架构）：跨 Agent / 中控层面自学习
- 两者独立运行，不互相干预

### D5. L1 缺少 `session_topics` 字段

- L1 数据模型增加 `"session_topics": ["delegation", "coding"]` 字段
- 新增 todo 或扩展现有 todo

### D6. L2+L3 LLM 消耗估算不完整

- L2 section 六 LLM 消耗估算表补充 Phase 2（规则评估）和 Phase 3（衰减检查）的开销
- Phase 2 可能需要 1-3 次 LLM 调用（分析经验与规则的关联）
- Phase 3 是零 LLM（纯数学计算）

### D7. 缺失 todo 补全

新增以下 todo：

- `l1-hints-consume`：L1 实现读取 l1-hints.json 调整灵敏度
- `l1-session-topics`：L1 数据模型新增 session_topics 字段
- `l2-doc-sync`：L2 文档同步 L3 决策（偏好/反模式载体、目录结构）
- `arch-update`：ARCHITECTURE.md 新增元学习章节
- `e2e-test`：端到端集成测试（mock transcript → L1 → L2 → 验证 proposal）

## SUGGESTION 记录（不阻塞实施，纳入 v2 考虑）

- **v1 复杂度裁剪**：13 种二级分类可精简为 4 种一级；7 种产物可精简为 3 种核心
- **并发安全**：多窗口同时触发 sessionEnd 的竞争问题
- **回滚机制**：已应用 T1 规则发现有害时的快速回滚
- **多 Agent 经验采集**：当前仅采集中控会话，专业 Agent 内部经验 Phase 2 扩展
- **文件系统队列脆弱性**：幂等性、排序保证（用时间戳文件名缓解）
- **闭环收敛性**：安全熔断是经验性保障，形式化保证留待 v2
- **离线批准 CLI**：`distill_knowledge.py --mode apply --id dist-xxx`

