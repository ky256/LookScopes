---
name: code-standards
description: >-
  MetaAgent 项目 Python 代码规范，基于 Google Python Style Guide。
  涵盖命名、类型注解、docstring、导入顺序、项目特定约定。
  编写或审查 Python 代码时自动应用。
---

# 代码规范

基于 Google Python Style Guide，附加项目特定约定。

## 命名

| 类型 | 风格 | 示例 |
|------|------|------|
| 模块/包 | snake_case | `agent_registry.py` |
| 类 | PascalCase | `TaskChainOrchestrator` |
| 函数/方法 | snake_case | `route_intent()` |
| 常量 | UPPER_SNAKE | `MAX_RETRY_COUNT` |
| 私有成员 | 前缀 `_` | `_parse_manifest()` |
| 类型变量 | PascalCase | `AgentName = str` |

## 类型注解

所有公开接口必须有类型注解。内部辅助函数推荐但不强制。

```python
# 公开接口 — 必须
async def dispatch(self, step: TaskStep) -> TaskReport:
    ...

# 内部方法 — 推荐
def _collect_upstream(self, step) -> list[str]:
    ...
```

使用 `from __future__ import annotations` 启用延迟求值。
优先用内置泛型（`list[str]`, `dict[str, Any]`）而非 `typing.List`。

## Docstring

使用 Google 风格 docstring。类和公开函数必须有。

```python
def discover(self, capability: str) -> list[AgentManifest]:
    """根据能力查找可用 Agent。

    Args:
        capability: 目标能力关键词，如 "fbx_export"。

    Returns:
        匹配的 Agent manifest 列表，按优先级排序。

    Raises:
        RegistryError: 注册中心不可用时抛出。
    """
```

## 导入顺序

```python
# 1. 标准库
import asyncio
from pathlib import Path

# 2. 第三方库
from fastapi import FastAPI
import httpx

# 3. 项目内部（绝对导入）
from meta.registry.agent_registry import AgentRegistry
from shared.common_utils import load_config
```

## 项目特定约定

### 异步优先
中控核心模块（router, orchestrator, api）使用 `async/await`。
Agent 内部工具如无必要可用同步。

### 配置管理
配置项从 `config/` 目录读取，不在代码中硬编码。
使用 Pydantic `BaseSettings` 或 `configparser` 管理。

### 错误处理
自定义异常继承自项目基类：

```python
class MetaAgentError(Exception):
    """项目异常基类。"""

class AgentOfflineError(MetaAgentError):
    """目标 Agent 不可达。"""

class TaskChainError(MetaAgentError):
    """任务链执行异常。"""
```

### 日志
使用 `logging` 标准库，不用 `print`。
每个模块顶部：`logger = logging.getLogger(__name__)`

### 路径处理
使用 `pathlib.Path`，不用字符串拼接。
