#!/usr/bin/env python3
"""元学习 L2/L3 知识蒸馏脚本。

三种运行模式:
- --mode incremental: sessionEnd hook 调用，执行蒸馏→评估→衰减三阶段
- --mode report: 手动或定期运行，生成周报
- --mode apply: 用户确认后手动运行，将已批准的 proposal 应用到目标文件

引擎优先级: CodeBuddy (glm-5.0) → Ollama (8B) → 静默跳过
零外部依赖，仅使用 Python 标准库。
"""

import argparse
import glob
import json
import logging
import msvcrt
import os
import re
import ssl
import sys
import threading
import time
import urllib.request
from datetime import datetime, timedelta
from logging.handlers import RotatingFileHandler
from pathlib import Path

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

DEFAULT_CODEBUDDY_MODEL = "glm-5.0"
DEFAULT_OLLAMA_MODEL = "qwen2.5:14b"
CODEBUDDY_ENDPOINT = "https://copilot.tencent.com/v2/chat/completions"
OLLAMA_ENDPOINT = "http://localhost:11434/api/generate"
CODEBUDDY_TIMEOUT = 60
OLLAMA_TIMEOUT = 120
SCRIPT_HARD_TIMEOUT = 180
TRANSCRIPT_TRUNCATE_CHARS = 50000
OLLAMA_TRUNCATE_CHARS = 8000

QUALITY_SCORE_THRESHOLD = 0.6
AGGREGATE_MIN_COUNT = 2
SINGLE_CONFIDENCE_THRESHOLD = 0.6
VITALITY_ARCHIVE_THRESHOLD = 20
GRACE_PERIOD_DAYS = 30
LOOKBACK_SESSIONS = 10
DECAY_RATES: dict[str, float] = {"T1": 1 / 30, "T2": 5 / 7, "T3": 10 / 3}
CIRCUIT_BREAKER_RULE_LIMIT = 20
CIRCUIT_BREAKER_PENDING_LIMIT = 10
CIRCUIT_BREAKER_RATE_MULTIPLIER = 1.5

# ---------------------------------------------------------------------------
# Prompt 模板
# ---------------------------------------------------------------------------

SINGLE_DISTILL_PROMPT = (
    "你是 MetaAgent 知识蒸馏引擎。根据以下经验事件，判断是否应该产生一条"
    "新规则、偏好、反模式或技能修补。\n\n"
    "事件:\n{event_json}\n\n"
    "现有规则（摘要）:\n{existing_rules_summary}\n\n"
    "target_file 路径约定：\n"
    "- 新规则/规则修改 → .cursor/rules/<kebab-name>.mdc\n"
    "- 偏好持久化 → .cursor/rules/meta-agent-identity.mdc\n"
    "- 反模式 → .cursor/skills/antipatterns/SKILL.md\n"
    "- Skill 修补 → .cursor/skills/<skill-name>/SKILL.md\n"
    "- Agent 修补 → .cursor/agents/<agent-name>.md\n"
    "禁止在项目根目录下创建 rules/ 等自造目录。\n\n"
    '请输出 JSON:\n{{\n'
    '  "should_create": true,\n'
    '  "type": "rule:create|rule:patch|preference:persist|antipattern:create'
    '|skill:patch|agent:patch|rule:deprecate",\n'
    '  "content": {{\n'
    '    "rule_text": "规则正文",\n'
    '    "target_file": "目标文件路径",\n'
    '    "rationale": "为什么需要这条规则"\n'
    "  }},\n"
    '  "confidence": 0.85,\n'
    '  "conflicts_with": ["可能冲突的已有规则ID"]\n'
    "}}\n"
    "如果不值得创建，should_create 为 false，其余字段可省略。"
)

AGGREGATE_DISTILL_PROMPT = (
    "你是 MetaAgent 知识蒸馏引擎。以下是同类型的多条经验事件，"
    "请从中提炼出通用的规则或模式。\n\n"
    "事件集合 (共 {count} 条):\n{events_json}\n\n"
    "现有规则:\n{existing_rules_summary}\n\n"
    "target_file 路径约定：\n"
    "- 新规则/规则修改 → .cursor/rules/<kebab-name>.mdc\n"
    "- 偏好持久化 → .cursor/rules/meta-agent-identity.mdc\n"
    "- 反模式 → .cursor/skills/antipatterns/SKILL.md\n"
    "- Skill 修补 → .cursor/skills/<skill-name>/SKILL.md\n"
    "- Agent 修补 → .cursor/agents/<agent-name>.md\n"
    "禁止在项目根目录下创建 rules/ 等自造目录。\n\n"
    '请输出 JSON:\n{{\n'
    '  "patterns_found": [\n'
    "    {{\n"
    '      "should_create": true,\n'
    '      "type": "rule:create|rule:patch|preference:persist|antipattern:create'
    '|skill:patch|agent:patch|rule:deprecate",\n'
    '      "content": {{ "rule_text": "...", "target_file": "...", '
    '"rationale": "..." }},\n'
    '      "confidence": 0.85,\n'
    '      "source_event_ids": ["evt-..."],\n'
    '      "conflicts_with": []\n'
    "    }}\n"
    "  ]\n"
    "}}"
)

CONFLICT_CHECK_PROMPT = (
    "你是 MetaAgent 规则冲突检测器。判断新提议的规则是否与已有规则冲突。\n\n"
    "新规则:\n{proposal_json}\n\n"
    "已有规则:\n{existing_rules}\n\n"
    '请输出 JSON:\n{{\n'
    '  "has_conflict": false,\n'
    '  "conflict_type": "direct_contradiction|partial_overlap|subsumes|null",\n'
    '  "conflicting_rules": ["规则文件路径"],\n'
    '  "resolution": "keep_new|keep_old|merge|manual_review",\n'
    '  "explanation": "..."\n'
    "}}"
)

QUALITY_ASSESS_PROMPT = (
    "你是 MetaAgent 知识质量评估员。评估以下 proposal 的质量。\n\n"
    "Proposal:\n{proposal_json}\n\n"
    "评估维度:\n"
    "1. 可操作性（规则是否足够具体以指导行为？）\n"
    "2. 可验证性（是否能通过观察确认规则被遵循？）\n"
    "3. 范围适当性（不过于宽泛也不过于狭窄？）\n"
    "4. 与项目哲学一致性\n\n"
    '请输出 JSON:\n{{\n'
    '  "overall_score": 0.78,\n'
    '  "actionability": 0.9,\n'
    '  "verifiability": 0.7,\n'
    '  "scope": 0.8,\n'
    '  "alignment": 0.75,\n'
    '  "suggestion": "改进建议或 null",\n'
    '  "verdict": "approve|improve|reject"\n'
    "}}"
)

logger: logging.Logger = logging.getLogger("distill_knowledge")


# ---------------------------------------------------------------------------
# 日志
# ---------------------------------------------------------------------------


def setup_logging(project_dir: Path) -> logging.Logger:
    """配置带 RotatingFileHandler 的日志系统。

    Args:
        project_dir: 项目根目录路径。

    Returns:
        配置好的 Logger 实例。
    """
    log_path = project_dir / ".cursor" / "hooks" / "distill_knowledge.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    handler = RotatingFileHandler(
        str(log_path),
        maxBytes=512 * 1024,
        backupCount=2,
        encoding="utf-8",
    )
    handler.setFormatter(
        logging.Formatter(
            "%(asctime)s %(levelname)s %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        )
    )

    root = logging.getLogger("distill_knowledge")
    root.setLevel(logging.DEBUG)
    root.addHandler(handler)
    return root


# ---------------------------------------------------------------------------
# 文件锁（Windows msvcrt）
# ---------------------------------------------------------------------------


def acquire_lock(f) -> bool:  # noqa: ANN001
    """尝试对已打开的文件描述符获取非阻塞排他锁。

    Args:
        f: 已打开的文件对象（需有 fileno()）。

    Returns:
        True 表示获取成功，False 表示被其它进程持有。
    """
    try:
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        return True
    except (OSError, IOError):
        return False


def release_lock(f) -> None:  # noqa: ANN001
    """释放通过 acquire_lock 获取的排他锁。

    Args:
        f: 持有锁的文件对象。
    """
    try:
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
    except (OSError, IOError):
        pass


# ---------------------------------------------------------------------------
# Watchdog
# ---------------------------------------------------------------------------


def watchdog_timer(seconds: int) -> None:
    """后台守护线程，超时后强制终止进程。

    Args:
        seconds: 超时秒数。
    """
    time.sleep(seconds)
    logger.error("watchdog timeout after %ds — force exit", seconds)
    os._exit(1)


# ---------------------------------------------------------------------------
# stdin / session / 路径解析
# ---------------------------------------------------------------------------


def read_stdin() -> dict:
    """读取 Cursor 通过 stdin 传入的 JSON 负载。

    Returns:
        解析后的 dict，或空 dict。
    """
    try:
        raw = sys.stdin.buffer.read(65536)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))
    except Exception:
        return {}


def _resolve_session_id(stdin_data: dict) -> str:
    """从多个来源尝试提取 session ID。

    优先级:
      1. stdin JSON 的 session_id 字段
      2. CURSOR_SESSION_ID 环境变量
      3. CURSOR_TRANSCRIPT_PATH 路径中的 UUID
      4. fallback "unknown"
    """
    sid = stdin_data.get("session_id", "")
    if sid:
        return sid

    sid = os.environ.get("CURSOR_SESSION_ID", "")
    if sid:
        return sid

    transcript = os.environ.get("CURSOR_TRANSCRIPT_PATH", "")
    if transcript:
        p = Path(transcript)
        candidate = p.stem
        if len(candidate) >= 6 and candidate != p.parent.name:
            candidate = p.parent.name
        if len(candidate) >= 6:
            return candidate

    return "unknown"


def resolve_project_dir(cli_arg: str | None) -> Path:
    """确定项目根目录。

    优先级: CURSOR_PROJECT_DIR 环境变量 → CLI 参数 → 脚本文件位置推导。

    Args:
        cli_arg: 命令行传入的 --project-dir 值，可为 None。

    Returns:
        项目根目录的 Path。
    """
    env_val = os.environ.get("CURSOR_PROJECT_DIR")
    if env_val:
        return Path(env_val)
    if cli_arg:
        return Path(cli_arg)
    return Path(__file__).resolve().parent.parent.parent


# ---------------------------------------------------------------------------
# CodeBuddy / Ollama API
# ---------------------------------------------------------------------------


def find_codebuddy_auth() -> dict | None:
    """通过 glob 搜索 CodeBuddy 的 JWT 认证文件。

    Returns:
        包含 token / user_id / enterprise_id / department_info 的 dict，
        找不到或解析失败时返回 None。
    """
    local_app = os.environ.get("LOCALAPPDATA", "")
    if not local_app:
        return None

    exact_path = os.path.join(
        local_app,
        "CodeBuddyExtension",
        "Data",
        "Public",
        "auth",
        "Tencent-Cloud.coding-copilot.info",
    )
    if os.path.isfile(exact_path):
        result = _parse_auth_file(exact_path)
        if result:
            return result

    auth_pattern = os.path.join(
        local_app,
        "CodeBuddyExtension",
        "**",
        "Tencent-Cloud.coding-copilot.info",
    )
    matches = glob.glob(auth_pattern, recursive=True)
    for match in matches:
        result = _parse_auth_file(match)
        if result:
            return result

    return None


def _parse_auth_file(path: str) -> dict | None:
    """解析单个 CodeBuddy 认证 JSON 文件。

    Args:
        path: 认证文件绝对路径。

    Returns:
        解析结果 dict 或 None。
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        account = data.get("account", {})
        auth = data.get("auth", {})
        token = auth.get("accessToken")
        uid = account.get("uid")
        if not token or not uid:
            return None
        return {
            "token": token,
            "user_id": uid,
            "enterprise_id": account.get("enterpriseId", ""),
            "department_info": account.get("departmentFullName", ""),
        }
    except (OSError, json.JSONDecodeError, TypeError) as exc:
        logger.debug("_parse_auth_file(%s) failed: %s", path, exc)
        return None


def parse_sse_response(response) -> str:  # noqa: ANN001
    """解析 SSE（Server-Sent Events）流式响应，拼接文本内容。

    Args:
        response: urllib 返回的 HTTPResponse 对象。

    Returns:
        拼接后的完整响应文本。
    """
    chunks: list[str] = []
    for raw_line in response:
        if isinstance(raw_line, bytes):
            line = raw_line.decode("utf-8", errors="replace").rstrip("\n\r")
        else:
            line = raw_line.rstrip("\n\r")
        if not line:
            continue
        if not line.startswith("data: "):
            continue
        payload = line[6:]
        if payload.strip() == "[DONE]":
            break
        try:
            obj = json.loads(payload)
            delta = obj.get("choices", [{}])[0].get("delta", {})
            content = delta.get("content", "")
            if content:
                chunks.append(content)
        except (json.JSONDecodeError, IndexError, TypeError):
            logger.debug("SSE parse skip: %s", payload[:120])
    return "".join(chunks)


def call_codebuddy(messages: list[dict], model: str) -> str | None:
    """调用 CodeBuddy API 生成内容。

    Args:
        messages: OpenAI 兼容的 messages 列表。
        model: 模型名称（如 glm-5.0）。

    Returns:
        生成的文本，失败时返回 None。
    """
    auth = find_codebuddy_auth()
    if not auth:
        logger.info("CodeBuddy auth not found, skipping")
        return None

    headers = {
        "Authorization": f"Bearer {auth['token']}",
        "X-User-Id": auth["user_id"],
        "X-Enterprise-Id": auth["enterprise_id"],
        "X-Tenant-Id": auth["enterprise_id"],
        "X-Domain": "tencent.sso.copilot.tencent.com",
        "X-Department-Info": auth["department_info"],
        "X-Product": "SaaS",
        "X-Requested-With": "XMLHttpRequest",
        "X-Agent-Intent": "craft",
        "Content-Type": "application/json",
    }

    body = json.dumps(
        {
            "model": model,
            "messages": messages,
            "max_tokens": 4096,
            "stream": True,
        }
    ).encode("utf-8")

    req = urllib.request.Request(
        CODEBUDDY_ENDPOINT, data=body, headers=headers, method="POST"
    )
    ctx = ssl.create_default_context()
    try:
        resp = urllib.request.urlopen(req, timeout=CODEBUDDY_TIMEOUT, context=ctx)
        result = parse_sse_response(resp)
        if result:
            logger.info("CodeBuddy returned %d chars", len(result))
            return result
        logger.warning("CodeBuddy returned empty response")
        return None
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            logger.warning("JWT expired (401), falling back")
        else:
            logger.warning("CodeBuddy HTTP %d: %s", exc.code, exc.reason)
        return None
    except Exception as exc:
        logger.warning("CodeBuddy request failed: %s", exc)
        return None


def call_ollama(messages: list[dict], model: str) -> str | None:
    """调用本地 Ollama API 生成内容。

    Args:
        messages: OpenAI 兼容的 messages 列表。
        model: Ollama 模型名称。

    Returns:
        生成的文本，失败时返回 None。
    """
    prompt_text = "\n".join(
        f"<|{m['role']}|>\n{m['content']}" for m in messages
    )
    prompt_text = truncate_text(prompt_text, OLLAMA_TRUNCATE_CHARS)

    body = json.dumps(
        {"model": model, "prompt": prompt_text, "stream": False}
    ).encode("utf-8")

    req = urllib.request.Request(
        OLLAMA_ENDPOINT,
        data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        resp = urllib.request.urlopen(req, timeout=OLLAMA_TIMEOUT)
        result = json.loads(resp.read().decode("utf-8"))
        text = result.get("response", "")
        if text:
            logger.info("Ollama returned %d chars", len(text))
            return text
        logger.warning("Ollama returned empty response")
        return None
    except Exception as exc:
        logger.warning("Ollama request failed: %s", exc)
        return None


# ---------------------------------------------------------------------------
# JSON / 文本工具
# ---------------------------------------------------------------------------


def extract_json_from_response(text: str) -> dict | None:
    """从 LLM 响应中提取 JSON 对象（兼容 ```json 包裹格式）。

    Args:
        text: LLM 原始响应文本。

    Returns:
        解析出的 dict，或 None。
    """
    json_match = re.search(r"```json\s*\n?(.*?)\n?\s*```", text, re.DOTALL)
    if json_match:
        try:
            return json.loads(json_match.group(1))
        except json.JSONDecodeError:
            pass

    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass

    brace_match = re.search(r"\{.*\}", text, re.DOTALL)
    if brace_match:
        try:
            return json.loads(brace_match.group(0))
        except json.JSONDecodeError:
            pass

    return None


def truncate_text(text: str, max_chars: int) -> str:
    """截断文本到指定字符数，保留尾部（最新内容优先）。

    Args:
        text: 原始文本。
        max_chars: 最大字符数。

    Returns:
        截断后的文本。
    """
    if len(text) <= max_chars:
        return text
    return "...(earlier content truncated)...\n" + text[-max_chars:]


def atomic_write_json(path: Path, data: dict | list) -> bool:
    """通过临时文件 + os.replace 原子写入 JSON。

    Args:
        path: 目标文件路径。
        data: 待序列化的数据。

    Returns:
        True 表示写入成功。
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        os.replace(str(tmp), str(path))
        return True
    except OSError as exc:
        logger.error("atomic_write_json failed for %s: %s", path, exc)
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        return False


def atomic_write_text(path: Path, content: str) -> bool:
    """通过临时文件 + os.replace 原子写入文本文件。

    Args:
        path: 目标文件路径。
        content: 文本内容。

    Returns:
        True 表示写入成功。
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(str(tmp), str(path))
        return True
    except OSError as exc:
        logger.error("atomic_write_text failed for %s: %s", path, exc)
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        return False


def read_json_safe(path: Path) -> dict | None:
    """安全读取 JSON 文件，失败时返回 None。

    Args:
        path: JSON 文件路径。

    Returns:
        解析后的 dict 或 None。
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
        return None
    except (OSError, json.JSONDecodeError, TypeError):
        return None


def ensure_distilled_dirs(project_dir: Path) -> Path:
    """确保 docs/distilled/ 下所有子目录存在。

    Args:
        project_dir: 项目根目录。

    Returns:
        distilled 根目录路径。
    """
    base = project_dir / "docs" / "distilled"
    for sub in ("proposals", "applied", "rejected", "archived",
                "retired", "metrics", "reports"):
        (base / sub).mkdir(parents=True, exist_ok=True)
    return base


# ---------------------------------------------------------------------------
# 规则摘要 & Proposal ID 生成
# ---------------------------------------------------------------------------


def load_existing_rules_summary(project_dir: Path) -> str:
    """扫描已有规则文件，生成摘要文本供 LLM 参考。

    Args:
        project_dir: 项目根目录。

    Returns:
        规则摘要字符串。
    """
    summaries: list[str] = []
    rules_dir = project_dir / ".cursor" / "rules"
    if rules_dir.exists():
        for mdc_file in sorted(rules_dir.glob("*.mdc")):
            try:
                text = mdc_file.read_text(encoding="utf-8")
                first_content_line = _extract_first_content_line(text)
                summaries.append(f"- {mdc_file.stem}: {first_content_line}")
            except OSError:
                pass

    applied_dir = project_dir / "docs" / "distilled" / "applied"
    if applied_dir.exists():
        for json_file in sorted(applied_dir.glob("*.json")):
            try:
                data = json.loads(json_file.read_text(encoding="utf-8"))
                rule_text = data.get("content", {}).get("rule_text", "")[:100]
                summaries.append(f"- {json_file.stem} (applied): {rule_text}")
            except (OSError, json.JSONDecodeError):
                pass

    return "\n".join(summaries) if summaries else "（暂无已有规则）"


def _extract_first_content_line(text: str) -> str:
    """从 .mdc 文件中提取首行非 frontmatter 内容。

    Args:
        text: 文件全文。

    Returns:
        首行有效内容（截断到 100 字符）。
    """
    in_frontmatter = False
    for line in text.strip().split("\n"):
        stripped = line.strip()
        if stripped == "---":
            in_frontmatter = not in_frontmatter
            continue
        if not in_frontmatter and stripped:
            return stripped[:100]
    return ""


def next_proposal_id(proposals_dir: Path) -> str:
    """生成下一个 proposal ID（格式: prop-YYYYMMDD-NNN）。

    Args:
        proposals_dir: proposals 目录路径。

    Returns:
        新的 proposal ID 字符串。
    """
    today = datetime.now().strftime("%Y%m%d")
    prefix = f"prop-{today}-"
    max_num = 0
    if proposals_dir.exists():
        for p in proposals_dir.glob(f"{prefix}*.json"):
            try:
                num = int(p.stem.split("-")[-1])
                max_num = max(max_num, num)
            except (ValueError, IndexError):
                pass
    return f"{prefix}{max_num + 1:03d}"


# ---------------------------------------------------------------------------
# LLM 调用封装
# ---------------------------------------------------------------------------


def call_llm(system_prompt: str, user_prompt: str) -> str | None:
    """调用 LLM 生成内容，CodeBuddy 优先，Ollama 兜底。

    Args:
        system_prompt: 系统提示。
        user_prompt: 用户提示。

    Returns:
        LLM 响应文本，失败时返回 None。
    """
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]
    result = call_codebuddy(messages, DEFAULT_CODEBUDDY_MODEL)
    if result is None:
        result = call_ollama(messages, DEFAULT_OLLAMA_MODEL)
    return result


# ---------------------------------------------------------------------------
# Phase 1 Step 1: Ingest（摄入）
# ---------------------------------------------------------------------------


def ingest_pending_events(
    learnings_dir: Path,
) -> tuple[list[dict], dict[str, Path]]:
    """扫描 learnings 目录，提取所有 pending 状态的经验事件。

    Args:
        learnings_dir: docs/learnings 目录路径。

    Returns:
        (pending 事件列表, 事件ID→来源文件路径映射)。
    """
    events: list[dict] = []
    source_map: dict[str, Path] = {}

    if not learnings_dir.exists():
        return events, source_map

    for json_file in sorted(learnings_dir.glob("*.json")):
        try:
            data = json.loads(json_file.read_text(encoding="utf-8"))
            for evt in data.get("events", []):
                if evt.get("_distill_status") in ("pending", "deferred"):
                    events.append(evt)
                    evt_id = evt.get("id", "")
                    if evt_id:
                        source_map[evt_id] = json_file
        except (OSError, json.JSONDecodeError, TypeError):
            continue

    logger.info("ingest: found %d pending events from %d files",
                len(events), len(set(source_map.values())))
    return events, source_map


# ---------------------------------------------------------------------------
# Phase 1 Step 2: Route（分流）
# ---------------------------------------------------------------------------


def route_events(events: list[dict]) -> dict:
    """按顶级类型分桶，决定蒸馏路径。

    Args:
        events: 所有 pending 事件列表。

    Returns:
        {"single": [event, ...], "aggregate": {type: [events, ...]}}
    """
    buckets: dict[str, list[dict]] = {}
    for evt in events:
        top_type = evt.get("type", "unknown").split("/")[0]
        buckets.setdefault(top_type, []).append(evt)

    result: dict = {"single": [], "aggregate": {}}
    for top_type, group in buckets.items():
        if len(group) >= AGGREGATE_MIN_COUNT:
            result["aggregate"][top_type] = group
        elif len(group) == 1:
            conf = _get_event_confidence(group[0])
            if conf >= SINGLE_CONFIDENCE_THRESHOLD:
                result["single"].append(group[0])

    single_count = len(result["single"])
    agg_count = sum(len(v) for v in result["aggregate"].values())
    logger.info("route: %d single, %d aggregate (%d groups)",
                single_count, agg_count, len(result["aggregate"]))
    return result


def _get_event_confidence(evt: dict) -> float:
    """提取事件的置信度数值。"""
    try:
        return float(evt.get("distillation", {}).get("confidence", 0))
    except (ValueError, TypeError):
        return 0.0


# ---------------------------------------------------------------------------
# Phase 1 Step 3: LLM Distill（蒸馏）
# ---------------------------------------------------------------------------


def distill_single(evt: dict, rules_summary: str) -> dict | None:
    """对单条事件执行 LLM 蒸馏。

    Args:
        evt: 经验事件 dict。
        rules_summary: 已有规则摘要文本。

    Returns:
        蒸馏结果 dict（含 should_create 字段），失败返回 None。
    """
    event_json = json.dumps(evt, ensure_ascii=False, indent=2)
    prompt = SINGLE_DISTILL_PROMPT.format(
        event_json=event_json,
        existing_rules_summary=rules_summary,
    )
    raw = call_llm("你是 MetaAgent 知识蒸馏引擎。", prompt)
    if raw is None:
        return None
    return extract_json_from_response(raw)


def distill_aggregate(
    events: list[dict], rules_summary: str
) -> list[dict]:
    """对同类型多条事件执行聚合 LLM 蒸馏。

    Args:
        events: 同类型的事件列表。
        rules_summary: 已有规则摘要文本。

    Returns:
        蒸馏结果列表（每个含 should_create 字段）。
    """
    events_json = json.dumps(events, ensure_ascii=False, indent=2)
    prompt = AGGREGATE_DISTILL_PROMPT.format(
        count=len(events),
        events_json=events_json,
        existing_rules_summary=rules_summary,
    )
    raw = call_llm("你是 MetaAgent 知识蒸馏引擎。", prompt)
    if raw is None:
        return []

    parsed = extract_json_from_response(raw)
    if parsed is None:
        return []

    patterns = parsed.get("patterns_found", [])
    if not isinstance(patterns, list):
        return []
    return patterns


# ---------------------------------------------------------------------------
# Phase 1 Step 4: Conflict Check（冲突检测）
# ---------------------------------------------------------------------------


def check_conflict(proposal: dict, rules_summary: str) -> dict:
    """对单个 proposal 执行冲突检测。

    Args:
        proposal: 候选 proposal 的 content 部分。
        rules_summary: 已有规则摘要。

    Returns:
        冲突检测结果 dict，LLM 失败时返回无冲突默认值。
    """
    proposal_json = json.dumps(proposal, ensure_ascii=False, indent=2)
    prompt = CONFLICT_CHECK_PROMPT.format(
        proposal_json=proposal_json,
        existing_rules=rules_summary,
    )
    raw = call_llm("你是 MetaAgent 规则冲突检测器。", prompt)
    default = {"has_conflict": False, "conflict_type": None,
               "conflicting_rules": [], "resolution": None,
               "explanation": ""}
    if raw is None:
        return default

    parsed = extract_json_from_response(raw)
    return parsed if parsed else default


# ---------------------------------------------------------------------------
# Phase 1 Step 5: Quality Assess（质量评估）
# ---------------------------------------------------------------------------


def assess_quality(proposal: dict) -> dict:
    """对单个 proposal 执行质量评估。

    Args:
        proposal: 候选 proposal dict。

    Returns:
        质量评估结果 dict，LLM 失败时返回低分默认值。
    """
    proposal_json = json.dumps(proposal, ensure_ascii=False, indent=2)
    prompt = QUALITY_ASSESS_PROMPT.format(proposal_json=proposal_json)
    raw = call_llm("你是 MetaAgent 知识质量评估员。", prompt)
    default = {"overall_score": 0.0, "actionability": 0.0,
               "verifiability": 0.0, "scope": 0.0, "alignment": 0.0,
               "suggestion": None, "verdict": "reject"}
    if raw is None:
        return default

    parsed = extract_json_from_response(raw)
    return parsed if parsed else default


# ---------------------------------------------------------------------------
# Phase 1 Step 6: Output（输出）
# ---------------------------------------------------------------------------


def build_proposal(
    proposal_id: str,
    distill_result: dict,
    source_event_ids: list[str],
    quality: dict,
    conflict: dict,
) -> dict:
    """将蒸馏+评估结果组装为完整的 proposal JSON。

    Args:
        proposal_id: 唯一 ID。
        distill_result: LLM 蒸馏结果。
        source_event_ids: 来源事件 ID 列表。
        quality: 质量评估结果。
        conflict: 冲突检测结果。

    Returns:
        完整的 proposal dict。
    """
    now = datetime.now().isoformat(timespec="seconds")
    needs_review = (
        conflict.get("has_conflict") is True
        and conflict.get("resolution") == "manual_review"
    )
    conflict_status = "needs_review" if needs_review else "pass"
    status = "pending_review" if not needs_review else "conflict_review"

    return {
        "id": proposal_id,
        "created": now,
        "type": distill_result.get("type", "rule:create"),
        "trust_level": "T3",
        "source_events": source_event_ids,
        "content": distill_result.get("content", {}),
        "quality": {
            "confidence": _safe_float(distill_result.get("confidence", 0)),
            "overall_score": _safe_float(quality.get("overall_score", 0)),
            "actionability": _safe_float(quality.get("actionability", 0)),
            "verifiability": _safe_float(quality.get("verifiability", 0)),
            "scope": _safe_float(quality.get("scope", 0)),
            "alignment": _safe_float(quality.get("alignment", 0)),
            "conflict_check": conflict_status,
            "suggestion": quality.get("suggestion"),
        },
        "status": status,
        "vitality": 100,
        "applied_at": None,
        "decay_started": None,
    }


def _safe_float(val: object) -> float:
    """安全转换为 float，无法转换时返回 0.0。"""
    try:
        return round(float(val), 4)  # type: ignore[arg-type]
    except (ValueError, TypeError):
        return 0.0


def update_distill_status(
    event_ids: list[str],
    source_map: dict[str, Path],
    new_status: str,
) -> None:
    """批量更新 learnings 文件中事件的 _distill_status。

    Args:
        event_ids: 需要更新的事件 ID 列表。
        source_map: 事件 ID → learnings 文件路径映射。
        new_status: 新的蒸馏状态字符串。
    """
    files_to_update: dict[Path, list[str]] = {}
    for eid in event_ids:
        fp = source_map.get(eid)
        if fp:
            files_to_update.setdefault(fp, []).append(eid)

    for fp, ids_in_file in files_to_update.items():
        try:
            data = json.loads(fp.read_text(encoding="utf-8"))
            id_set = set(ids_in_file)
            changed = False
            for evt in data.get("events", []):
                if evt.get("id") in id_set:
                    evt["_distill_status"] = new_status
                    changed = True
            if changed:
                atomic_write_json(fp, data)
                logger.debug("Updated distill status in %s for %d events",
                             fp.name, len(ids_in_file))
        except (OSError, json.JSONDecodeError) as exc:
            logger.warning("update_distill_status failed for %s: %s",
                           fp, exc)


def update_metrics(
    metrics_path: Path,
    new_proposals: int,
    distilled_events: int,
) -> None:
    """更新综合度量文件 metrics.json。

    Args:
        metrics_path: metrics.json 路径。
        new_proposals: 本次新增的 proposal 数量。
        distilled_events: 本次蒸馏处理的事件数量。
    """
    data = read_json_safe(metrics_path) or {
        "total_distilled_events": 0,
        "total_proposals_created": 0,
        "last_distillation": None,
    }
    data["total_distilled_events"] = (
        data.get("total_distilled_events", 0) + distilled_events
    )
    data["total_proposals_created"] = (
        data.get("total_proposals_created", 0) + new_proposals
    )
    data["last_distillation"] = datetime.now().isoformat(timespec="seconds")
    atomic_write_json(metrics_path, data)


# ---------------------------------------------------------------------------
# Phase 1 编排: run_distillation
# ---------------------------------------------------------------------------


def run_distillation(project_dir: Path) -> int:
    """执行完整的 Phase 1 六步蒸馏流水线。

    Args:
        project_dir: 项目根目录。

    Returns:
        生成的 proposal 数量。
    """
    distilled_dir = ensure_distilled_dirs(project_dir)
    learnings_dir = project_dir / "docs" / "learnings"
    proposals_dir = distilled_dir / "proposals"
    metrics_path = distilled_dir / "metrics" / "metrics.json"

    events, source_map = ingest_pending_events(learnings_dir)
    if not events:
        logger.info("distillation: no pending events")
        return 0

    all_ingested_ids = [e.get("id", "") for e in events if e.get("id")]

    routed = route_events(events)
    rules_summary = load_existing_rules_summary(project_dir)

    proposals_created = 0
    processed_event_ids: list[str] = []

    proposals_created += _distill_singles(
        routed["single"], rules_summary, proposals_dir, processed_event_ids
    )
    proposals_created += _distill_aggregates(
        routed["aggregate"], rules_summary, proposals_dir, processed_event_ids
    )

    if processed_event_ids:
        update_distill_status(processed_event_ids, source_map, "distilled")

    deferred_ids = [
        eid for eid in all_ingested_ids
        if eid not in processed_event_ids
    ]
    if deferred_ids:
        update_distill_status(deferred_ids, source_map, "deferred")
        logger.info("distillation: deferred %d unrouted events", len(deferred_ids))

    update_metrics(metrics_path, proposals_created, len(processed_event_ids))
    logger.info("distillation: created %d proposals from %d events",
                proposals_created, len(processed_event_ids))
    return proposals_created


def _distill_singles(
    singles: list[dict],
    rules_summary: str,
    proposals_dir: Path,
    processed_ids: list[str],
) -> int:
    """蒸馏所有单条事件并生成 proposal。

    Returns:
        生成的 proposal 数量。
    """
    count = 0
    for evt in singles:
        try:
            result = distill_single(evt, rules_summary)
            if result is None or not result.get("should_create"):
                processed_ids.append(evt.get("id", ""))
                continue

            prop = _evaluate_and_write(
                result, [evt.get("id", "")], rules_summary, proposals_dir
            )
            if prop:
                count += 1
            processed_ids.append(evt.get("id", ""))
        except Exception as exc:
            logger.warning("distill single failed for %s: %s",
                           evt.get("id", ""), exc)
    return count


def _distill_aggregates(
    aggregates: dict[str, list[dict]],
    rules_summary: str,
    proposals_dir: Path,
    processed_ids: list[str],
) -> int:
    """蒸馏所有聚合分组并生成 proposal。

    Returns:
        生成的 proposal 数量。
    """
    count = 0
    for top_type, group in aggregates.items():
        try:
            patterns = distill_aggregate(group, rules_summary)
            group_ids = [e.get("id", "") for e in group]

            for pattern in patterns:
                if not pattern.get("should_create"):
                    continue
                source_ids = pattern.get("source_event_ids", group_ids)
                prop = _evaluate_and_write(
                    pattern, source_ids, rules_summary, proposals_dir
                )
                if prop:
                    count += 1

            processed_ids.extend(group_ids)
        except Exception as exc:
            logger.warning("distill aggregate failed for %s: %s",
                           top_type, exc)
    return count


def _evaluate_and_write(
    distill_result: dict,
    source_event_ids: list[str],
    rules_summary: str,
    proposals_dir: Path,
) -> dict | None:
    """对蒸馏结果执行冲突检测+质量评估，通过后写入 proposal。

    Returns:
        写入的 proposal dict，未通过则返回 None。
    """
    conflict = check_conflict(distill_result.get("content", {}), rules_summary)
    quality = assess_quality(distill_result)

    score = _safe_float(quality.get("overall_score", 0))
    verdict = quality.get("verdict", "reject")
    if score < QUALITY_SCORE_THRESHOLD or verdict == "reject":
        logger.info("proposal rejected: score=%.2f verdict=%s", score, verdict)
        return None

    prop_id = next_proposal_id(proposals_dir)
    proposal = build_proposal(
        prop_id, distill_result, source_event_ids, quality, conflict
    )
    prop_path = proposals_dir / f"{prop_id}.json"
    atomic_write_json(prop_path, proposal)
    logger.info("proposal created: %s (score=%.2f)", prop_id, score)
    return proposal


# ---------------------------------------------------------------------------
# Phase 2: Rule Evaluation（评估）
# ---------------------------------------------------------------------------


def run_evaluation(project_dir: Path) -> dict:
    """执行 Phase 2 规则效果评估。零 LLM 消耗，纯计算。

    Args:
        project_dir: 项目根目录。

    Returns:
        rule-signals 数据 dict。
    """
    distilled_dir = project_dir / "docs" / "distilled"
    metrics_dir = distilled_dir / "metrics"
    metrics_dir.mkdir(parents=True, exist_ok=True)

    applied_rules = _load_applied_rules(distilled_dir / "applied")
    if not applied_rules:
        logger.info("evaluation: no applied rules")
        return {}

    corrections = _load_recent_corrections(
        project_dir / "docs" / "learnings"
    )
    signals = _compute_rule_signals(applied_rules, corrections)
    _write_rule_signals(metrics_dir, signals)
    _write_l1_hints(metrics_dir, corrections)
    health = _build_system_health(distilled_dir, applied_rules, project_dir)
    health["circuit_breaker"] = check_circuit_breaker(health)
    atomic_write_json(metrics_dir / "system-health.json", health)

    logger.info("evaluation: %d rules assessed", len(applied_rules))
    return {"signals": signals}


def _load_applied_rules(applied_dir: Path) -> dict[str, dict]:
    """加载所有已应用的规则。"""
    rules: dict[str, dict] = {}
    if not applied_dir.exists():
        return rules
    for f in sorted(applied_dir.glob("*.json")):
        data = read_json_safe(f)
        if data:
            rules[data.get("id", f.stem)] = data
    return rules


def _load_recent_corrections(learnings_dir: Path) -> list[dict]:
    """加载最近 N 个会话的 correction/violation 事件。"""
    corrections: list[dict] = []
    if not learnings_dir.exists():
        return corrections

    files = sorted(learnings_dir.glob("*.json"))[-LOOKBACK_SESSIONS:]
    for f in files:
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
            for evt in data.get("events", []):
                evt_type = evt.get("type", "")
                if evt_type.startswith(("correction/", "violation/")):
                    corrections.append(evt)
        except (OSError, json.JSONDecodeError):
            pass
    return corrections


def _compute_rule_signals(
    applied_rules: dict[str, dict], corrections: list[dict]
) -> dict[str, dict]:
    """根据 correction 事件计算每条已应用规则的效果信号。"""
    signals: dict[str, dict] = {}
    for rule_id, rule_data in applied_rules.items():
        target = rule_data.get("content", {}).get("target_file", "")
        violations = _find_violations_for_rule(rule_id, target, corrections)

        if violations:
            last_ts = violations[-1].get("timestamp", "")
            last_date = last_ts[:10] if len(last_ts) >= 10 else None
            signals[rule_id] = {
                "effectiveness": "ineffective",
                "evidence_count": len(violations),
                "last_violation": last_date,
                "vitality_delta": -10,
            }
        else:
            signals[rule_id] = {
                "effectiveness": "effective",
                "evidence_count": 0,
                "last_violation": None,
                "vitality_delta": 5,
            }
    return signals


def _find_violations_for_rule(
    rule_id: str, target_file: str, corrections: list[dict]
) -> list[dict]:
    """查找与指定规则相关的 correction 事件。"""
    matches: list[dict] = []
    for corr in corrections:
        corr_target = corr.get("distillation", {}).get("target_file", "")
        corr_text = (corr.get("lesson", "") + " "
                     + corr.get("observation", ""))
        if ((target_file and corr_target == target_file)
                or rule_id in corr_text):
            matches.append(corr)
    return matches


def _write_rule_signals(metrics_dir: Path, signals: dict[str, dict]) -> None:
    """写入 rule-signals.json。"""
    data = {
        "updated": datetime.now().isoformat(timespec="seconds"),
        "signals": signals,
    }
    atomic_write_json(metrics_dir / "rule-signals.json", data)


def _write_l1_hints(metrics_dir: Path, corrections: list[dict]) -> None:
    """根据 correction 分布生成 l1-hints.json。"""
    problem_categories: set[str] = set()
    for corr in corrections:
        cat = corr.get("type", "").split("/")[0]
        if cat:
            problem_categories.add(cat)

    all_categories = {"correction", "failure", "preference",
                      "violation", "insight"}
    suppressed = sorted(all_categories - problem_categories)

    data = {
        "updated": datetime.now().isoformat(timespec="seconds"),
        "heightened_categories": sorted(problem_categories),
        "suppressed_categories": suppressed,
    }
    atomic_write_json(metrics_dir / "l1-hints.json", data)


def _build_system_health(
    distilled_dir: Path,
    applied_rules: dict[str, dict],
    project_dir: Path,
) -> dict:
    """构建系统健康度数据。"""
    proposals_pending = 0
    prop_dir = distilled_dir / "proposals"
    if prop_dir.exists():
        proposals_pending = len(list(prop_dir.glob("*.json")))

    rates = _compute_correction_rates(project_dir / "docs" / "learnings")
    trend = _determine_trend(rates["rate_7d"], rates["rate_30d"])

    vitalities = [r.get("vitality", 100) for r in applied_rules.values()]
    avg_v = sum(vitalities) / max(len(vitalities), 1)

    return {
        "updated": datetime.now().isoformat(timespec="seconds"),
        "total_rules_applied": len(applied_rules),
        "total_proposals_pending": proposals_pending,
        "correction_rate_7d": round(rates["rate_7d"], 4),
        "correction_rate_30d": round(rates["rate_30d"], 4),
        "trend": trend,
        "circuit_breaker": "normal",
        "vitality_avg": round(avg_v, 1),
    }


def _compute_correction_rates(learnings_dir: Path) -> dict:
    """统计 7 天和 30 天内的纠正率。"""
    all_7d = 0
    corr_7d = 0
    all_30d = 0
    corr_30d = 0
    now = datetime.now()

    if learnings_dir.exists():
        for f in learnings_dir.glob("*.json"):
            try:
                data = json.loads(f.read_text(encoding="utf-8"))
                for evt in data.get("events", []):
                    age = _event_age_days(evt, now)
                    if age is None:
                        continue
                    is_corr = evt.get("type", "").startswith(
                        ("correction/", "violation/")
                    )
                    if age <= 30:
                        all_30d += 1
                        if is_corr:
                            corr_30d += 1
                    if age <= 7:
                        all_7d += 1
                        if is_corr:
                            corr_7d += 1
            except (OSError, json.JSONDecodeError):
                pass

    return {
        "rate_7d": corr_7d / max(all_7d, 1),
        "rate_30d": corr_30d / max(all_30d, 1),
    }


def _event_age_days(evt: dict, now: datetime) -> int | None:
    """计算事件距今天数。"""
    try:
        ts = datetime.fromisoformat(evt.get("timestamp", ""))
        return (now - ts).days
    except (ValueError, TypeError):
        return None


def _determine_trend(rate_7d: float, rate_30d: float) -> str:
    """根据短期和长期纠正率判断趋势。"""
    if rate_7d < rate_30d * 0.8:
        return "improving"
    if rate_7d > rate_30d * 1.2:
        return "degrading"
    return "stable"


# ---------------------------------------------------------------------------
# Phase 3: Decay Check（衰减）
# ---------------------------------------------------------------------------


def calculate_vitality(
    proposal: dict, days_since_applied: int, signals: dict
) -> float:
    """根据信任等级、时间和反馈信号计算 vitality 值。

    Args:
        proposal: proposal dict（含 trust_level, vitality）。
        days_since_applied: 自应用以来的天数。
        signals: rule-signals 中该规则对应的信号 dict。

    Returns:
        计算后的 vitality 值（0-100）。
    """
    trust = proposal.get("trust_level", "T3")
    if trust == "T0":
        return 100.0

    base = float(proposal.get("vitality", 100))
    decay_rate = DECAY_RATES.get(trust, DECAY_RATES["T3"])

    if days_since_applied <= GRACE_PERIOD_DAYS:
        decay = 0.0
    else:
        decay = decay_rate * (days_since_applied - GRACE_PERIOD_DAYS)

    signal = signals.get(proposal.get("id", ""), {})
    if signal.get("effectiveness") == "effective":
        decay -= abs(signal.get("vitality_delta", 0))
    elif signal.get("effectiveness") == "ineffective":
        decay += abs(signal.get("vitality_delta", 0))

    return max(0.0, min(100.0, base - decay))


def run_decay(project_dir: Path) -> int:
    """执行 Phase 3 衰减检查。零 LLM 消耗。

    Args:
        project_dir: 项目根目录。

    Returns:
        归档的规则数量。
    """
    distilled_dir = project_dir / "docs" / "distilled"
    applied_dir = distilled_dir / "applied"
    archived_dir = distilled_dir / "archived"
    metrics_dir = distilled_dir / "metrics"

    signals_data = read_json_safe(metrics_dir / "rule-signals.json") or {}
    signals = signals_data.get("signals", {})
    now = datetime.now()
    archived_count = 0

    if not applied_dir.exists():
        return 0

    for f in list(applied_dir.glob("*.json")):
        data = read_json_safe(f)
        if not data:
            continue

        days = _days_since_applied(data, now)
        new_vitality = calculate_vitality(data, days, signals)
        data["vitality"] = round(new_vitality, 1)

        if new_vitality < VITALITY_ARCHIVE_THRESHOLD:
            data["archived_at"] = now.isoformat(timespec="seconds")
            data["archive_reason"] = (
                f"vitality={new_vitality:.1f} < {VITALITY_ARCHIVE_THRESHOLD}"
            )
            archived_dir.mkdir(parents=True, exist_ok=True)
            atomic_write_json(archived_dir / f.name, data)
            try:
                f.unlink()
            except OSError:
                pass
            archived_count += 1
            logger.info("archived rule %s (vitality=%.1f)",
                        data.get("id", f.stem), new_vitality)
        else:
            atomic_write_json(f, data)

    logger.info("decay: updated vitality, archived %d rules", archived_count)
    return archived_count


def _days_since_applied(proposal: dict, now: datetime) -> int:
    """计算 proposal 自应用以来的天数。"""
    applied_str = proposal.get("applied_at", "")
    if not applied_str:
        created_str = proposal.get("created", "")
        applied_str = created_str
    try:
        applied_dt = datetime.fromisoformat(applied_str)
        return max(0, (now - applied_dt).days)
    except (ValueError, TypeError):
        return 0


# ---------------------------------------------------------------------------
# 安全熔断
# ---------------------------------------------------------------------------


def check_circuit_breaker(health: dict) -> str:
    """根据系统健康度判断熔断器状态。

    Args:
        health: system-health 数据 dict。

    Returns:
        "triggered" / "warning" / "normal"。
    """
    total_applied = health.get("total_rules_applied", 0)
    rate_7d = health.get("correction_rate_7d", 0)
    rate_30d = health.get("correction_rate_30d", 0)
    pending = health.get("total_proposals_pending", 0)

    if (total_applied > CIRCUIT_BREAKER_RULE_LIMIT
            and rate_30d > 0
            and rate_7d > rate_30d * CIRCUIT_BREAKER_RATE_MULTIPLIER):
        return "triggered"
    if pending > CIRCUIT_BREAKER_PENDING_LIMIT:
        return "warning"
    return "normal"


# ---------------------------------------------------------------------------
# --mode report: 周报生成
# ---------------------------------------------------------------------------


def run_report(project_dir: Path) -> None:
    """生成元学习周报。

    Args:
        project_dir: 项目根目录。
    """
    distilled_dir = ensure_distilled_dirs(project_dir)
    now = datetime.now()
    year, week, _ = now.isocalendar()
    report_name = f"{year}-W{week:02d}.md"
    report_path = distilled_dir / "reports" / report_name

    stats = _gather_weekly_stats(distilled_dir, project_dir, now)
    health = read_json_safe(distilled_dir / "metrics" / "system-health.json")
    top_rules = _get_top_rules(distilled_dir / "applied", 5)
    attention = _get_attention_items(distilled_dir)

    content = _format_report(
        year, week, stats, health or {}, top_rules, attention
    )
    atomic_write_text(report_path, content)
    logger.info("report generated: %s", report_path)


def _gather_weekly_stats(
    distilled_dir: Path, project_dir: Path, now: datetime
) -> dict:
    """汇总本周蒸馏统计。"""
    week_start = now - timedelta(days=now.weekday())
    week_start = week_start.replace(hour=0, minute=0, second=0, microsecond=0)

    new_experiences = 0
    learnings_dir = project_dir / "docs" / "learnings"
    if learnings_dir.exists():
        for f in learnings_dir.glob("*.json"):
            try:
                data = json.loads(f.read_text(encoding="utf-8"))
                for evt in data.get("events", []):
                    ts = evt.get("timestamp", "")
                    try:
                        if datetime.fromisoformat(ts) >= week_start:
                            new_experiences += 1
                    except (ValueError, TypeError):
                        pass
            except (OSError, json.JSONDecodeError):
                pass

    new_proposals = _count_files_since(
        distilled_dir / "proposals", week_start
    )
    applied = _count_files_since(distilled_dir / "applied", week_start)
    rejected = _count_files_since(distilled_dir / "rejected", week_start)
    archived = _count_files_since(distilled_dir / "archived", week_start)

    return {
        "new_experiences": new_experiences,
        "new_proposals": new_proposals,
        "applied": applied,
        "rejected": rejected,
        "archived": archived,
    }


def _count_files_since(directory: Path, since: datetime) -> int:
    """统计目录中指定时间之后创建的 JSON 文件数。"""
    count = 0
    if not directory.exists():
        return count
    for f in directory.glob("*.json"):
        data = read_json_safe(f)
        if not data:
            continue
        created = data.get("created", data.get("applied_at", ""))
        try:
            if datetime.fromisoformat(created) >= since:
                count += 1
        except (ValueError, TypeError):
            pass
    return count


def _get_top_rules(applied_dir: Path, limit: int) -> list[dict]:
    """获取 vitality 最高的 N 条已应用规则。"""
    rules: list[dict] = []
    if not applied_dir.exists():
        return rules
    for f in applied_dir.glob("*.json"):
        data = read_json_safe(f)
        if data:
            rules.append(data)
    rules.sort(key=lambda r: r.get("vitality", 0), reverse=True)
    return rules[:limit]


def _get_attention_items(distilled_dir: Path) -> list[str]:
    """收集需要关注的项目。"""
    items: list[str] = []
    applied_dir = distilled_dir / "applied"
    if applied_dir.exists():
        for f in applied_dir.glob("*.json"):
            data = read_json_safe(f)
            if data and data.get("vitality", 100) < 40:
                items.append(
                    f"vitality 偏低: {data.get('id', f.stem)} "
                    f"(vitality={data.get('vitality', 0)})"
                )

    proposals_dir = distilled_dir / "proposals"
    if proposals_dir.exists():
        now = datetime.now()
        for f in proposals_dir.glob("*.json"):
            data = read_json_safe(f)
            if not data:
                continue
            created = data.get("created", "")
            try:
                age = (now - datetime.fromisoformat(created)).days
                if age > 7:
                    items.append(
                        f"pending 超期: {data.get('id', f.stem)} "
                        f"({age} 天未处理)"
                    )
            except (ValueError, TypeError):
                pass

    return items


def _format_report(
    year: int,
    week: int,
    stats: dict,
    health: dict,
    top_rules: list[dict],
    attention: list[str],
) -> str:
    """将统计数据格式化为 Markdown 周报。"""
    lines: list[str] = [
        f"# 元学习周报 {year}-W{week:02d}\n",
        "## 本周统计",
        f"- 新增经验: {stats['new_experiences']} 条",
        f"- 新增 proposal: {stats['new_proposals']} 条",
        f"- 已应用规则: {stats['applied']} 条",
        f"- 被拒 proposal: {stats['rejected']} 条",
        f"- 归档规则: {stats['archived']} 条\n",
        "## 规则效果 Top 5",
        "| 规则 | 效果 | vitality |",
        "|------|------|----------|",
    ]

    for rule in top_rules:
        rid = rule.get("id", "?")
        v = rule.get("vitality", 0)
        eff = "effective" if v >= 50 else "degrading"
        lines.append(f"| {rid} | {eff} | {v} |")
    if not top_rules:
        lines.append("| （暂无） | - | - |")

    lines.append("\n## 需要关注")
    if attention:
        for item in attention:
            lines.append(f"- {item}")
    else:
        lines.append("- （无需关注的项目）")

    trend = health.get("trend", "unknown")
    cb_status = health.get("circuit_breaker", "unknown")
    avg_v = health.get("vitality_avg", 0)
    lines.extend([
        "\n## 系统健康度",
        f"- 纠正率趋势: {trend}",
        f"- 熔断器状态: {cb_status}",
        f"- 平均 vitality: {avg_v}",
        "",
    ])

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# --mode apply: 手动应用
# ---------------------------------------------------------------------------


def run_apply(project_dir: Path) -> None:
    """将已批准的 proposal 应用到目标文件。

    Args:
        project_dir: 项目根目录。
    """
    distilled_dir = ensure_distilled_dirs(project_dir)
    proposals_dir = distilled_dir / "proposals"
    applied_dir = distilled_dir / "applied"

    if not proposals_dir.exists():
        logger.info("apply: no proposals directory")
        return

    applied_count = 0
    for prop_file in sorted(proposals_dir.glob("*.json")):
        data = read_json_safe(prop_file)
        if not data or data.get("status") != "approved":
            continue

        success = _apply_single_proposal(data, project_dir)
        if success:
            data["status"] = "applied"
            data["applied_at"] = datetime.now().isoformat(timespec="seconds")
            atomic_write_json(applied_dir / prop_file.name, data)
            try:
                prop_file.unlink()
            except OSError:
                pass
            applied_count += 1
            logger.info("applied: %s → %s",
                        data.get("id", ""),
                        data.get("content", {}).get("target_file", ""))

    logger.info("apply: applied %d proposals", applied_count)


def _apply_single_proposal(proposal: dict, project_dir: Path) -> bool:
    """将单个 proposal 应用到目标文件。

    Returns:
        True 表示应用成功。
    """
    prop_type = proposal.get("type", "")
    content = proposal.get("content", {})
    rule_text = content.get("rule_text", "")
    target_file = content.get("target_file", "")

    if not rule_text or not target_file:
        logger.warning("apply: missing rule_text or target_file for %s",
                       proposal.get("id", ""))
        return False

    target_path = project_dir / target_file
    try:
        if prop_type == "rule:create":
            return _apply_rule_create(target_path, content)
        if prop_type == "rule:patch":
            return _apply_rule_patch(target_path, rule_text)
        if prop_type == "preference:persist":
            identity = project_dir / ".cursor" / "rules" / "meta-agent-identity.mdc"
            return _apply_rule_patch(identity, rule_text)
        if prop_type == "antipattern:create":
            skill = (project_dir / ".cursor" / "skills"
                     / "antipatterns" / "SKILL.md")
            skill.parent.mkdir(parents=True, exist_ok=True)
            return _apply_rule_patch(skill, rule_text)
        if prop_type == "rule:deprecate":
            return _apply_rule_deprecate(target_path, content)
        if prop_type in ("skill:patch", "agent:patch"):
            if not target_path.exists():
                logger.warning("apply: target file not found: %s", target_file)
                return False
            try:
                existing = target_path.read_text(encoding="utf-8")
                if rule_text in existing:
                    logger.info("apply: content already exists in %s", target_file)
                    return True
                new_content = existing.rstrip() + "\n\n" + rule_text + "\n"
                atomic_write_text(target_path, new_content)
                logger.info("apply: patched %s with %s", target_file, prop_type)
                return True
            except OSError as exc:
                logger.warning("apply: failed to patch %s: %s", target_file, exc)
                return False
        if target_path.exists():
            logger.info("apply: unknown type %s, fallback to rule:patch for %s",
                        prop_type, target_file)
            return _apply_rule_patch(target_path, rule_text)
        logger.info("apply: unknown type %s, fallback to rule:create for %s",
                     prop_type, target_file)
        return _apply_rule_create(target_path, content)
    except Exception as exc:
        logger.error("apply failed for %s: %s",
                     proposal.get("id", ""), exc)
        return False


def _apply_rule_create(target_path: Path, content: dict) -> bool:
    """创建新的 .mdc 规则文件。"""
    rationale = content.get("rationale", "")
    rule_text = content.get("rule_text", "")
    frontmatter = f"---\ndescription: {rationale}\n---\n\n"
    return atomic_write_text(target_path, frontmatter + rule_text + "\n")


def _apply_rule_patch(target_path: Path, rule_text: str) -> bool:
    """追加规则文本到已有文件。"""
    existing = ""
    if target_path.exists():
        try:
            existing = target_path.read_text(encoding="utf-8")
        except OSError:
            pass
    return atomic_write_text(
        target_path, existing.rstrip("\n") + "\n\n" + rule_text + "\n"
    )


def _apply_rule_deprecate(target_path: Path, content: dict) -> bool:
    """在目标文件头部标记 deprecated。"""
    if not target_path.exists():
        logger.warning("deprecate: target not found: %s", target_path)
        return False
    try:
        existing = target_path.read_text(encoding="utf-8")
    except OSError:
        return False
    rationale = content.get("rationale", "")
    header = f"<!-- DEPRECATED: {rationale} -->\n\n"
    return atomic_write_text(target_path, header + existing)


# ---------------------------------------------------------------------------
# 编排: run_incremental
# ---------------------------------------------------------------------------


def run_incremental(project_dir: Path) -> None:
    """增量模式主流程: Phase 1(蒸馏) → Phase 2(评估) → Phase 3(衰减)。

    Args:
        project_dir: 项目根目录。
    """
    distilled_dir = ensure_distilled_dirs(project_dir)
    health_path = distilled_dir / "metrics" / "system-health.json"
    health = read_json_safe(health_path) or {}
    cb_status = health.get("circuit_breaker", "normal")

    if cb_status == "triggered":
        logger.warning("circuit breaker TRIGGERED — skipping Phase 1 distillation")
    else:
        if cb_status == "warning":
            logger.warning("circuit breaker WARNING — proceeding with caution")
        run_distillation(project_dir)

    run_evaluation(project_dir)
    run_decay(project_dir)

    logger.info("incremental: all phases complete")


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def main() -> None:
    """脚本入口：解析参数，启动 watchdog，按模式分派执行。"""
    parser = argparse.ArgumentParser(
        description="MetaAgent L2/L3 knowledge distillation"
    )
    parser.add_argument(
        "--mode",
        choices=["incremental", "report", "apply"],
        required=True,
    )
    parser.add_argument(
        "--project-dir", default=None, help="手动指定项目根目录"
    )
    args = parser.parse_args()

    wd = threading.Thread(
        target=watchdog_timer, args=(SCRIPT_HARD_TIMEOUT,), daemon=True
    )
    wd.start()

    stdin_data = read_stdin()
    project_dir = resolve_project_dir(args.project_dir)

    global logger  # noqa: PLW0603
    logger = setup_logging(project_dir)
    logger.info("=== distill_knowledge start === mode=%s", args.mode)

    hooks_dir = project_dir / ".cursor" / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    lock_path = hooks_dir / f".distill_{args.mode}.lock"
    lock_file = None
    try:
        lock_file = open(lock_path, "a+", encoding="utf-8")
        if not acquire_lock(lock_file):
            logger.info("Another instance holds lock, exiting")
            lock_file.close()
            sys.exit(0)
    except OSError as exc:
        logger.warning("Lock open failed: %s", exc)
        sys.exit(0)

    try:
        if args.mode == "incremental":
            run_incremental(project_dir)
        elif args.mode == "report":
            run_report(project_dir)
        elif args.mode == "apply":
            run_apply(project_dir)
    finally:
        if lock_file:
            release_lock(lock_file)
            lock_file.close()

    logger.info("=== distill_knowledge done === mode=%s", args.mode)


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as exc:
        try:
            logger.error("Unhandled exception: %s", exc, exc_info=True)
        except Exception:
            pass
        sys.exit(0)
