#!/usr/bin/env python3
"""元学习经验采集脚本（IDE 无关）。

由 Hook 系统触发（Cursor/CodeBuddy 均支持）：
- --mode mark   : Stop hook（每次 AI 回复后），快速筛选并标记经验信号
- --mode extract: SessionEnd hook（会话结束时），LLM 精判并生成结构化经验

引擎优先级: CodeBuddy (glm-5.0) → Ollama (8B) → 静默跳过
零外部依赖，仅使用 Python 标准库。
"""

import argparse
import glob
import json
import logging
import os
import re
import ssl
import sys
import threading
import time
import urllib.request
from datetime import datetime
from logging.handlers import RotatingFileHandler
from pathlib import Path

if sys.platform == "win32":
    import msvcrt
else:
    import fcntl

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

DEFAULT_CODEBUDDY_MODEL = "deepseek-v3-2-volc-ioa"
DEFAULT_OLLAMA_MODEL = "qwen2.5:14b"
CODEBUDDY_ENDPOINT = "https://copilot.tencent.com/v2/chat/completions"
OLLAMA_ENDPOINT = "http://127.0.0.1:11434/api/generate"
CODEBUDDY_TIMEOUT = 60
OLLAMA_TIMEOUT = 120
SCRIPT_HARD_TIMEOUT = 180
TRANSCRIPT_TRUNCATE_CHARS = 50000
OLLAMA_TRUNCATE_CHARS = 8000

MAX_MARKERS_PER_SESSION = 20
MIN_MARKERS_FOR_EXTRACT = 2
CONTEXT_WINDOW_TURNS = 5
RECENT_TAIL_LINES = 20

DEFAULT_THRESHOLD = 1.0
HEIGHTENED_THRESHOLD = 0.5
SUPPRESSED_THRESHOLD = 2.0

MIN_CONFIDENCE = 0.6
DEDUP_JACCARD_THRESHOLD = 0.6

SHORT_MSG_CHARS = 30
LONG_MSG_CHARS = 500
CONTEXT_PREVIEW_CHARS = 200

# Stage 1 关键词模式
EXPLICIT_PATTERNS = [
    r"不对|不是|错了|不应该|别这样",
    r"应该[用是]|改[成为]|换成|不要用",
    r"为什么没|怎么又|之前说过|不是这个意思",
    r"再试|重新[做来]|还是不行",
]

TOOL_FAILURE_PATTERNS = [r"Error:|Exception:|failed|错误|失败"]

# 分类体系（13 种两级分类）
VALID_TYPES = frozenset({
    "correction/user", "correction/self",
    "failure/task", "failure/tool", "failure/process",
    "preference/style", "preference/workflow", "preference/tooling",
    "violation/rule", "violation/skill", "violation/arch",
    "insight/pattern", "insight/boundary",
})

EXTRACT_SYSTEM_PROMPT = (
    "你是一个 AI 对话经验分析助手。分析 AI IDE 的对话片段，识别用户纠正、"
    "工具失败、偏好表达、规则违反等经验事件。\n"
    "对每个候选片段输出严格的 JSON，不要输出其他内容。"
)

EXTRACT_USER_TEMPLATE = (
    "以下是 AI IDE 对话中标记的候选经验片段。请分析并提取结构化经验。\n\n"
    "## 候选片段\n{fragments}\n\n"
    "请对每个片段输出一个 JSON 对象，所有片段的结果放在一个 JSON 数组中。\n"
    "每个 JSON 对象包含以下字段：\n"
    "- is_correction (bool): 是否为用户纠正\n"
    "- confidence (float 0-1): 置信度\n"
    "- type (string): 两级分类，可选值: {valid_types}\n"
    "- trigger (string): 触发文本\n"
    "- observation (string): 观察到的问题\n"
    "- lesson (string): 经验教训\n"
    "- rule_candidate (string, 可选): 候选规则\n"
    "- target_file (string, 可选): 相关文件\n"
    "- action (string, 可选): reinforce|create|deprecate\n\n"
    "另外，输出一个 session_topics 字段（字符串数组），表示本会话涉及的主题标签。\n\n"
    "输出格式:\n"
    '```json\n{{\n  "events": [...],\n  "session_topics": ["topic1", "topic2"]\n}}\n```'
)

logger: logging.Logger = logging.getLogger("collect_experience")


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
    log_path = project_dir / "infra" / "hooks" / "collect_experience.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    handler = RotatingFileHandler(
        str(log_path),
        maxBytes=512 * 1024,
        backupCount=2,
        encoding="utf-8",
    )
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)s %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    )

    root = logging.getLogger("collect_experience")
    root.setLevel(logging.DEBUG)
    root.addHandler(handler)
    return root


# ---------------------------------------------------------------------------
# 文件锁（跨平台）
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
        if sys.platform == "win32":
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
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
        if sys.platform == "win32":
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
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
# stdin / session 解析
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
      1. stdin JSON 的 session_id 字段（CodeBuddy / Claude Code 方式）
      2. CURSOR_SESSION_ID 环境变量
      3. stdin transcript_path 或 CURSOR_TRANSCRIPT_PATH 路径中的 UUID
      4. fallback "unknown"
    """
    sid = stdin_data.get("session_id", "")
    if sid:
        return sid

    sid = os.environ.get("CURSOR_SESSION_ID", "")
    if sid:
        return sid

    transcript = stdin_data.get("transcript_path", "") or os.environ.get("CURSOR_TRANSCRIPT_PATH", "")
    if transcript:
        p = Path(transcript)
        candidate = p.stem
        if len(candidate) >= 6 and candidate != p.parent.name:
            candidate = p.parent.name
        if len(candidate) >= 6:
            return candidate

    return "unknown"


# ---------------------------------------------------------------------------
# 路径解析
# ---------------------------------------------------------------------------


def resolve_project_dir(cli_arg: str | None) -> Path:
    """确定项目根目录。

    优先级: CODEBUDDY_PROJECT_DIR → CLAUDE_PROJECT_DIR → CURSOR_PROJECT_DIR
            → CLI 参数 → 脚本文件位置推导（infra/hooks/ → 项目根）。

    Args:
        cli_arg: 命令行传入的 --project-dir 值，可为 None。

    Returns:
        项目根目录的 Path。
    """
    for env_key in ("CODEBUDDY_PROJECT_DIR", "CLAUDE_PROJECT_DIR", "CURSOR_PROJECT_DIR"):
        env_val = os.environ.get(env_key)
        if env_val:
            return Path(env_val)
    if cli_arg:
        return Path(cli_arg)
    return Path(__file__).resolve().parent.parent.parent


def _get_codebuddy_data_base() -> Path | None:
    """获取 CodeBuddy 数据存储根目录（跨平台）。

    Windows: %LOCALAPPDATA%/CodeBuddyExtension/Data
    macOS:   ~/Library/Application Support/CodeBuddyExtension/Data

    Returns:
        数据目录 Path，不存在时返回 None。
    """
    if sys.platform == "win32":
        local_app = os.environ.get("LOCALAPPDATA", "")
        if local_app:
            base = Path(local_app) / "CodeBuddyExtension" / "Data"
            if base.is_dir():
                return base
    else:
        base = Path.home() / "Library" / "Application Support" / "CodeBuddyExtension" / "Data"
        if base.is_dir():
            return base
    return None


def _iter_codebuddy_history_dirs(base: Path):
    """遍历所有 CodeBuddy history 目录。

    Yields:
        (history_dir, project_dir) 元组。
    """
    for machine_dir in base.iterdir():
        if not machine_dir.is_dir() or machine_dir.name in ("default", "Public"):
            continue
        cb_ide = machine_dir / "CodeBuddyIDE" / machine_dir.name / "history"
        if not cb_ide.is_dir():
            continue
        for project_dir in cb_ide.iterdir():
            if project_dir.is_dir():
                yield cb_ide, project_dir


def _discover_codebuddy_transcript(session_id: str) -> Path | None:
    """通过 session_id 在 CodeBuddy 存储目录中搜索 transcript 文件。

    跨平台支持 Windows (%LOCALAPPDATA%) 和 macOS (~Library/Application Support)。

    Args:
        session_id: 完整的 session UUID。

    Returns:
        transcript index.json 的 Path，或 None。
    """
    base = _get_codebuddy_data_base()
    if base is None:
        return None
    for _, project_dir in _iter_codebuddy_history_dirs(base):
        candidate = project_dir / session_id / "index.json"
        if candidate.exists():
            return candidate
    return None


def _discover_latest_codebuddy_transcript() -> Path | None:
    """在没有 session_id 时，通过最近修改时间发现最新的 transcript。

    用于 CodeBuddy stop hook 不传 session_id 的情况（兜底策略）。

    Returns:
        最新 session 的 index.json Path，或 None。
    """
    base = _get_codebuddy_data_base()
    if base is None:
        return None

    latest_path: Path | None = None
    latest_mtime: float = 0

    for _, project_dir in _iter_codebuddy_history_dirs(base):
        for session_dir in project_dir.iterdir():
            if not session_dir.is_dir():
                continue
            candidate = session_dir / "index.json"
            if not candidate.exists():
                continue
            try:
                mtime = candidate.stat().st_mtime
                if mtime > latest_mtime:
                    latest_mtime = mtime
                    latest_path = candidate
            except OSError:
                continue

    if latest_path:
        logger.debug("latest transcript discovered: %s (mtime=%s)", latest_path, latest_mtime)
    return latest_path


def resolve_transcript_path(cli_arg: str | None, stdin_data: dict | None = None) -> Path | None:
    """确定 transcript 文件路径。

    优先级: stdin transcript_path → CodeBuddy session_id 发现 → CodeBuddy 最新 session 兜底
            → CURSOR_TRANSCRIPT_PATH 环境变量 → CLI 参数。

    Args:
        cli_arg: 命令行传入的 --transcript 值，可为 None。
        stdin_data: 从 stdin 读取的 JSON 数据，可为 None。

    Returns:
        transcript 文件的 Path，或 None。
    """
    # 1. stdin 传入的 transcript_path（优先最高）
    if stdin_data and stdin_data.get("transcript_path"):
        p = Path(stdin_data["transcript_path"])
        if p.exists():
            return p

    # 2. CodeBuddy: 通过 session_id 自动发现 transcript
    if stdin_data and stdin_data.get("session_id"):
        p = _discover_codebuddy_transcript(stdin_data["session_id"])
        if p is not None:
            return p

    # 3. CodeBuddy 兜底: stop hook 不传 session_id 时，用最新 session
    p = _discover_latest_codebuddy_transcript()
    if p is not None:
        return p

    # 4. Cursor 环境变量 / CLI 参数
    env_val = os.environ.get("CURSOR_TRANSCRIPT_PATH")
    candidate = env_val if env_val else cli_arg
    if not candidate:
        return None
    p = Path(candidate)
    if not p.exists():
        return None
    return p


# ---------------------------------------------------------------------------
# Transcript 解析
# ---------------------------------------------------------------------------


def _extract_content(obj: dict) -> str:
    """从 transcript JSON 对象中提取文本内容。

    支持多种 Cursor transcript 格式:
    - {"role": "user", "message": {"content": [{"type": "text", "text": "..."}]}}
    - {"role": "user", "content": "..."}
    - {"role": "user", "content": [{"text": "..."}]}

    Args:
        obj: 单行 JSON 解析后的 dict。

    Returns:
        提取出的纯文本内容。
    """
    message = obj.get("message")
    if isinstance(message, dict) and message:
        content = message.get("content", "")
    else:
        content = obj.get("content", obj.get("text", ""))

    if isinstance(content, list):
        text_parts = []
        for item in content:
            if isinstance(item, dict):
                text_parts.append(item.get("text", ""))
            elif isinstance(item, str):
                text_parts.append(item)
        return " ".join(p for p in text_parts if p)

    if isinstance(content, str):
        return content

    return ""


def _load_codebuddy_messages(transcript_path: Path) -> list[dict] | None:
    """尝试从 CodeBuddy index.json + messages/ 目录加载消息列表。

    返回一个 dict 列表，每个 dict 格式同 JSONL 解析结果：
    [{"role": "user", "content": [...]}, ...]
    如果不是 CodeBuddy 格式，返回 None。
    """
    try:
        # 读取文件前几个字符检测格式
        with open(transcript_path, "r", encoding="utf-8") as f:
            header = f.read(100).strip()

        # 检测是否为 CodeBuddy 格式（JSON 对象且包含 messages 键）
        if not (header.startswith('{') and '"messages"' in header):
            return None

        # 解析 index.json
        with open(transcript_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        messages_meta = data.get("messages", [])
        if not isinstance(messages_meta, list):
            return None

        result: list[dict] = []
        messages_dir = transcript_path.parent / "messages"

        for meta in messages_meta:
            if not isinstance(meta, dict):
                continue
            msg_id = meta.get("id", "")
            if not msg_id:
                continue

            # 尝试读取消息文件
            msg_file = messages_dir / f"{msg_id}.json"
            if not msg_file.exists():
                # 尝试备份文件
                msg_file = messages_dir / f".{msg_id}_bak.json"
                if not msg_file.exists():
                    continue

            try:
                with open(msg_file, "r", encoding="utf-8") as f:
                    msg_data = json.load(f)

                # message 字段是 JSON 字符串，需要二次解析
                message_str = msg_data.get("message", "")
                if isinstance(message_str, str) and message_str:
                    msg_obj = json.loads(message_str)
                    if isinstance(msg_obj, dict):
                        result.append(msg_obj)
            except (OSError, json.JSONDecodeError, TypeError):
                continue

        return result if result else None

    except (OSError, json.JSONDecodeError, TypeError):
        return None


def parse_transcript(transcript_path: Path, since_line: int = 0) -> tuple[str, int]:
    """解析 .jsonl transcript 文件，提取对话文本。

    Args:
        transcript_path: transcript 文件路径。
        since_line: 从该行（0-based）之后开始解析。

    Returns:
        (拼接后的对话文本, 文件总行数) 元组。
    """
    # CodeBuddy format auto-detection
    cb_msgs = _load_codebuddy_messages(transcript_path)
    if cb_msgs is not None:
        parts = []
        for obj in cb_msgs[since_line:]:
            if not isinstance(obj, dict):
                continue
            role = obj.get("role", "unknown")
            content = _extract_content(obj)
            if content:
                parts.append(f"[{role}]: {content}")
        return "\n".join(parts), len(cb_msgs)

    parts: list[str] = []
    total_lines = 0
    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            for idx, raw_line in enumerate(f):
                total_lines = idx + 1
                if idx < since_line:
                    continue
                raw_line = raw_line.strip()
                if not raw_line:
                    continue
                try:
                    obj = json.loads(raw_line)
                except (json.JSONDecodeError, TypeError):
                    continue
                if not isinstance(obj, dict):
                    continue
                role = obj.get("role", "unknown")
                content = _extract_content(obj)
                if content:
                    parts.append(f"[{role}]: {content}")
    except OSError as exc:
        logger.warning("parse_transcript failed: %s", exc)
    return "\n".join(parts), total_lines


def parse_transcript_lines(transcript_path: Path) -> list[dict]:
    """逐行解析 transcript，返回带行号和角色的结构化记录列表。

    Args:
        transcript_path: transcript 文件路径。

    Returns:
        每条记录包含 line / role / content 字段的 dict 列表。
    """
    # CodeBuddy format auto-detection
    cb_msgs = _load_codebuddy_messages(transcript_path)
    if cb_msgs is not None:
        records: list[dict] = []
        for idx, obj in enumerate(cb_msgs):
            if not isinstance(obj, dict):
                continue
            role = obj.get("role", "unknown")
            content = _extract_content(obj)
            records.append({
                "line": idx,
                "role": role,
                "content": content,
            })
        return records

    records: list[dict] = []
    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            for idx, raw_line in enumerate(f):
                raw_line = raw_line.strip()
                if not raw_line:
                    continue
                try:
                    obj = json.loads(raw_line)
                except (json.JSONDecodeError, TypeError):
                    continue
                if not isinstance(obj, dict):
                    continue
                role = obj.get("role", "unknown")
                content = _extract_content(obj)
                records.append({
                    "line": idx,
                    "role": role,
                    "content": content,
                })
    except OSError as exc:
        logger.warning("parse_transcript_lines failed: %s", exc)
    return records


def get_tail_records(transcript_path: Path, n: int = RECENT_TAIL_LINES) -> list[dict]:
    """获取 transcript 末尾的最近 n 条记录，用于 mark 模式快速扫描。

    Args:
        transcript_path: transcript 文件路径。
        n: 尾部保留条数。

    Returns:
        最近 n 条结构化记录。
    """
    records = parse_transcript_lines(transcript_path)
    return records[-n:] if records else []


def get_context_window(
    records: list[dict],
    center_line: int,
    window: int = CONTEXT_WINDOW_TURNS,
) -> dict:
    """根据触发行号，提取前后各 window 轮的上下文窗口。

    Args:
        records: 完整的 transcript 记录列表。
        center_line: 触发事件所在的行号。
        window: 前后各取多少轮。

    Returns:
        包含 before / trigger_turn / after 的 dict。
    """
    center_idx = None
    for i, rec in enumerate(records):
        if rec["line"] >= center_line:
            center_idx = i
            break
    if center_idx is None:
        center_idx = len(records) - 1

    start = max(0, center_idx - window)
    end = min(len(records), center_idx + window + 1)

    before_parts = []
    trigger_part = ""
    after_parts = []

    for i in range(start, end):
        text = f"[{records[i]['role']}]: {records[i]['content'][:CONTEXT_PREVIEW_CHARS]}"
        if i < center_idx:
            before_parts.append(text)
        elif i == center_idx:
            trigger_part = text
        else:
            after_parts.append(text)

    return {
        "before": "\n".join(before_parts),
        "trigger_turn": trigger_part,
        "after": "\n".join(after_parts),
    }


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


# ---------------------------------------------------------------------------
# CodeBuddy / Ollama API
# ---------------------------------------------------------------------------


def find_codebuddy_auth() -> dict | None:
    """通过 glob 搜索 CodeBuddy 的 JWT 认证文件。

    Returns:
        包含 token / user_id / enterprise_id / department_info 的 dict，
        找不到或解析失败时返回 None。
    """
    # Windows: %LOCALAPPDATA%\CodeBuddyExtension\...
    # macOS:   ~/Library/Application Support/CodeBuddyExtension\...
    local_app = os.environ.get("LOCALAPPDATA", "")
    if not local_app and sys.platform == "darwin":
        local_app = os.path.expanduser(
            "~/Library/Application Support"
        )

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

    req = urllib.request.Request(CODEBUDDY_ENDPOINT, data=body, headers=headers, method="POST")

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
        {
            "model": model,
            "prompt": prompt_text,
            "stream": False,
        }
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
# L3 反馈信号消费
# ---------------------------------------------------------------------------


def load_l3_hints(project_dir: Path) -> dict:
    """读取 L3 蒸馏层提供的阈值调优提示。

    当 docs/distilled/metrics/l1-hints.json 存在时：
    - heightened_categories: 降低 Stage 1 过滤阈值（更容易标记）
    - suppressed_categories: 提高阈值（降低灵敏度）

    Args:
        project_dir: 项目根目录。

    Returns:
        包含 heightened_categories 和 suppressed_categories 的 dict。
    """
    hints_path = project_dir / "docs" / "distilled" / "metrics" / "l1-hints.json"
    default: dict = {"heightened_categories": [], "suppressed_categories": []}
    try:
        if not hints_path.exists():
            return default
        with open(hints_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return default
        return {
            "heightened_categories": data.get("heightened_categories", []),
            "suppressed_categories": data.get("suppressed_categories", []),
        }
    except (OSError, json.JSONDecodeError, TypeError):
        return default


def get_category_threshold(marker_type: str, l3_hints: dict) -> float:
    """根据 marker 类型的顶级分类和 L3 提示计算阈值。

    Args:
        marker_type: marker 类型字符串（如 "correction/user"）。
        l3_hints: L3 提示 dict。

    Returns:
        该分类对应的阈值（关键词命中数需 >= 此值才标记）。
    """
    category = marker_type.split("/")[0] if "/" in marker_type else marker_type
    if category in l3_hints.get("heightened_categories", []):
        return HEIGHTENED_THRESHOLD
    if category in l3_hints.get("suppressed_categories", []):
        return SUPPRESSED_THRESHOLD
    return DEFAULT_THRESHOLD


# ---------------------------------------------------------------------------
# Stage 1: 快速筛选器
# ---------------------------------------------------------------------------


def detect_explicit_correction(text: str) -> list[str]:
    """检测用户显式纠正关键词。

    Args:
        text: 用户消息文本。

    Returns:
        匹配到的关键词列表。
    """
    matched: list[str] = []
    for pattern in EXPLICIT_PATTERNS:
        found = re.findall(pattern, text)
        if found:
            matched.extend(found)
    return matched


def detect_tool_failure(text: str) -> list[str]:
    """检测工具失败信号关键词。

    Args:
        text: 消息文本。

    Returns:
        匹配到的失败关键词列表。
    """
    matched: list[str] = []
    for pattern in TOOL_FAILURE_PATTERNS:
        found = re.findall(pattern, text)
        if found:
            matched.extend(found)
    return matched


def detect_conversation_structure(recent_records: list[dict]) -> list[dict]:
    """检测对话结构异常信号。

    两类信号：
    - 用户连续两条消息（AI 回复被忽略/打断）
    - 用户极短消息紧接 AI 长回复（否定信号）

    Args:
        recent_records: 最近的 transcript 记录列表。

    Returns:
        检测到的结构信号列表。
    """
    signals: list[dict] = []
    if len(recent_records) < 2:
        return signals

    for i in range(len(recent_records) - 1):
        curr = recent_records[i]
        nxt = recent_records[i + 1]

        if curr["role"] == "user" and nxt["role"] == "user":
            signals.append({
                "type": "consecutive_user",
                "line": nxt["line"],
                "detail": "用户连续两条消息",
            })

        if (curr["role"] == "assistant"
                and nxt["role"] == "user"
                and len(curr["content"]) > LONG_MSG_CHARS
                and len(nxt["content"]) < SHORT_MSG_CHARS):
            signals.append({
                "type": "short_after_long",
                "line": nxt["line"],
                "detail": "用户极短消息紧接 AI 长回复",
            })

    return signals


def run_stage1_filter(recent_records: list[dict], l3_hints: dict) -> list[dict]:
    """Stage 1 快速筛选：扫描最近消息，生成 marker 列表。

    三类信号源：显式纠正关键词、对话结构异常、工具失败。

    Args:
        recent_records: 最近的 transcript 记录。
        l3_hints: L3 反馈提示。

    Returns:
        符合条件的 marker 列表。
    """
    markers: list[dict] = []
    now_ts = datetime.now().isoformat(timespec="seconds")

    for rec in recent_records:
        if rec["role"] != "user":
            continue
        keywords = detect_explicit_correction(rec["content"])
        if keywords:
            threshold = get_category_threshold("correction", l3_hints)
            if len(keywords) >= threshold:
                markers.append({
                    "line": rec["line"],
                    "type": "explicit_correction",
                    "keywords": keywords[:5],
                    "ts": now_ts,
                })

    structure_signals = detect_conversation_structure(recent_records)
    for sig in structure_signals:
        threshold = get_category_threshold("correction", l3_hints)
        if 1.0 >= threshold:
            markers.append({
                "line": sig["line"],
                "type": f"structure_{sig['type']}",
                "keywords": [sig["detail"]],
                "ts": now_ts,
            })

    for rec in recent_records:
        keywords = detect_tool_failure(rec["content"])
        if keywords:
            threshold = get_category_threshold("failure", l3_hints)
            if len(keywords) >= threshold:
                markers.append({
                    "line": rec["line"],
                    "type": "tool_failure",
                    "keywords": keywords[:5],
                    "ts": now_ts,
                })

    return markers


# ---------------------------------------------------------------------------
# Marker 文件管理
# ---------------------------------------------------------------------------


def get_marker_path(hooks_dir: Path, session_id: str) -> Path:
    """获取 marker JSONL 文件路径。"""
    return hooks_dir / f".experience_markers_{session_id}.jsonl"


def get_state_path(hooks_dir: Path, session_id: str) -> Path:
    """获取节流状态文件路径。"""
    return hooks_dir / f".correction_state_{session_id}.json"


def read_markers(marker_path: Path) -> list[dict]:
    """读取 marker JSONL 文件中的所有条目。

    Args:
        marker_path: marker 文件路径。

    Returns:
        所有 marker 的 dict 列表。
    """
    markers: list[dict] = []
    if not marker_path.exists():
        return markers
    try:
        with open(marker_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    markers.append(json.loads(line))
                except (json.JSONDecodeError, TypeError):
                    continue
    except OSError:
        pass
    return markers


def append_markers(marker_path: Path, new_markers: list[dict]) -> int:
    """追加 markers 到 JSONL 文件。

    Args:
        marker_path: marker 文件路径。
        new_markers: 待追加的 marker 列表。

    Returns:
        实际写入的条目数量。
    """
    if not new_markers:
        return 0
    try:
        marker_path.parent.mkdir(parents=True, exist_ok=True)
        with open(marker_path, "a", encoding="utf-8") as f:
            for m in new_markers:
                f.write(json.dumps(m, ensure_ascii=False) + "\n")
        return len(new_markers)
    except OSError as exc:
        logger.warning("append_markers failed: %s", exc)
        return 0


def cleanup_markers(marker_path: Path) -> None:
    """清理 marker 临时文件。"""
    try:
        if marker_path.exists():
            marker_path.unlink()
            logger.debug("Cleaned marker file: %s", marker_path)
    except OSError:
        pass


# ---------------------------------------------------------------------------
# 节流状态管理
# ---------------------------------------------------------------------------


def read_correction_state(state_path: Path) -> dict:
    """读取节流状态 JSON。

    Args:
        state_path: 状态文件路径。

    Returns:
        状态 dict，缺失或损坏时返回默认值。
    """
    default: dict = {"marker_count": 0, "type_counts": {}}
    if not state_path.exists():
        return default
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return default
        return data
    except (OSError, json.JSONDecodeError):
        return default


def write_correction_state(state_path: Path, state: dict) -> None:
    """通过临时文件 + os.replace 原子写入节流状态。

    Args:
        state_path: 状态文件路径。
        state: 待持久化的状态 dict。
    """
    state_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = state_path.with_suffix(".tmp")
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=2)
        os.replace(str(tmp_path), str(state_path))
    except OSError as exc:
        logger.error("write_correction_state failed: %s", exc)
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


def deduplicate_markers(existing: list[dict], new: list[dict]) -> list[dict]:
    """L2 去重：同 session 中同类型+同行号的 marker 合并。

    Args:
        existing: 已有的 marker 列表。
        new: 新增的 marker 列表。

    Returns:
        去重后的新增 marker 列表。
    """
    existing_keys: set[str] = set()
    for m in existing:
        key = f"{m.get('type', '')}_{m.get('line', 0)}"
        existing_keys.add(key)

    unique: list[dict] = []
    for m in new:
        key = f"{m.get('type', '')}_{m.get('line', 0)}"
        if key not in existing_keys:
            unique.append(m)
            existing_keys.add(key)
    return unique


# ---------------------------------------------------------------------------
# Stage 2: LLM 精判
# ---------------------------------------------------------------------------


def build_extract_prompt(fragments: list[str]) -> list[dict]:
    """构建 Stage 2 经验提取的 LLM messages。

    Args:
        fragments: 候选上下文片段列表。

    Returns:
        OpenAI 兼容的 messages 列表。
    """
    fragments_text = "\n\n---\n\n".join(
        f"### 片段 {i + 1}\n{frag}" for i, frag in enumerate(fragments)
    )
    user_content = EXTRACT_USER_TEMPLATE.format(
        fragments=fragments_text,
        valid_types=", ".join(sorted(VALID_TYPES)),
    )
    return [
        {"role": "system", "content": EXTRACT_SYSTEM_PROMPT},
        {"role": "user", "content": user_content},
    ]


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


def jaccard_similarity(s1: str, s2: str) -> float:
    """计算两个字符串的 Jaccard 相似度（基于分词集合）。

    Args:
        s1: 第一个字符串。
        s2: 第二个字符串。

    Returns:
        0.0 ~ 1.0 之间的相似度值。
    """
    words1 = set(s1.lower().split())
    words2 = set(s2.lower().split())
    if not words1 and not words2:
        return 1.0
    if not words1 or not words2:
        return 0.0
    intersection = words1 & words2
    union = words1 | words2
    return len(intersection) / len(union)


def deduplicate_events(new_events: list[dict], learnings_dir: Path) -> list[dict]:
    """与 docs/learnings/ 下所有已有经验的 lesson 字段做 Jaccard 去重。

    Args:
        new_events: 待去重的新事件列表。
        learnings_dir: 经验记录目录。

    Returns:
        去重后的事件列表。
    """
    existing_lessons: list[str] = []
    if learnings_dir.exists():
        try:
            for file_path in learnings_dir.glob("*.json"):
                with open(file_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                for evt in data.get("events", []):
                    lesson = evt.get("lesson", "")
                    if lesson:
                        existing_lessons.append(lesson)
        except (OSError, json.JSONDecodeError, TypeError):
            pass

    unique: list[dict] = []
    for evt in new_events:
        lesson = evt.get("lesson", "")
        if not lesson:
            unique.append(evt)
            continue
        is_dup = False
        for existing in existing_lessons:
            if jaccard_similarity(lesson, existing) > DEDUP_JACCARD_THRESHOLD:
                is_dup = True
                logger.debug("Dedup: '%s' similar to existing", lesson[:50])
                break
        if not is_dup:
            unique.append(evt)
            existing_lessons.append(lesson)
    return unique


def build_experience_record(
    session_id: str,
    events: list[dict],
    session_topics: list[str],
    project_dir: Path,
) -> dict:
    """将 LLM 提取的事件组装为最终经验记录 JSON。

    Args:
        session_id: 6 字符会话 ID。
        events: 经过去重的事件列表（来自 LLM 提取结果）。
        session_topics: 会话主题标签列表。
        project_dir: 项目根目录。

    Returns:
        符合数据模型规范的经验记录 dict。
    """
    today = datetime.now().strftime("%Y-%m-%d")
    today_compact = today.replace("-", "")
    session_6 = session_id[:6]
    summary_ref = f"docs/memory/daily/{today}.md"

    formatted_events: list[dict] = []
    for i, evt in enumerate(events):
        evt_id = f"evt-{today_compact}-{session_6}-{i + 1:03d}"
        confidence = 0.0
        try:
            confidence = float(evt.get("confidence", 0))
        except (ValueError, TypeError):
            pass
        severity = "high" if confidence > 0.8 else "medium"

        evt_type = evt.get("type", "insight/pattern")
        if evt_type not in VALID_TYPES:
            evt_type = "insight/pattern"

        formatted_evt: dict = {
            "id": evt_id,
            "timestamp": evt.get("ts", datetime.now().isoformat(timespec="seconds")),
            "type": evt_type,
            "severity": severity,
            "trigger": evt.get("trigger", ""),
            "observation": evt.get("observation", ""),
            "lesson": evt.get("lesson", ""),
            "context": evt.get("context", {}),
            "_distill_status": "pending",
            "distillation": {},
        }

        if evt.get("rule_candidate") or evt.get("action"):
            formatted_evt["distillation"] = {
                "rule_candidate": evt.get("rule_candidate", ""),
                "target_file": evt.get("target_file", ""),
                "confidence": confidence,
                "action": evt.get("action", ""),
            }

        formatted_events.append(formatted_evt)

    return {
        "session_id": session_6,
        "date": today,
        "summary_ref": summary_ref,
        "session_topics": session_topics,
        "event_count": len(formatted_events),
        "events": formatted_events,
    }


def write_experience(output_path: Path, record: dict) -> None:
    """通过临时文件 + os.replace 原子写入经验 JSON。

    Args:
        output_path: 目标 JSON 文件路径。
        record: 经验记录 dict。
    """
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = output_path.with_suffix(".tmp")
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(record, f, ensure_ascii=False, indent=2)
        os.replace(str(tmp_path), str(output_path))
        logger.info("Experience written: %s", output_path)
    except OSError as exc:
        logger.error("write_experience failed: %s", exc)
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# run_mark: stop hook 快速标记模式
# ---------------------------------------------------------------------------


def run_mark(
    project_dir: Path,
    transcript_path: Path,
    session_id: str,
    l3_hints: dict,
) -> None:
    """mark 模式主流程：快速筛选信号并追加 marker。

    零 LLM 调用，目标延迟 < 200ms。

    Args:
        project_dir: 项目根目录。
        transcript_path: transcript 文件路径。
        session_id: 6 字符会话 ID。
        l3_hints: L3 反馈提示。
    """
    hooks_dir = project_dir / "infra" / "hooks"
    marker_path = get_marker_path(hooks_dir, session_id)
    state_path = get_state_path(hooks_dir, session_id)

    state = read_correction_state(state_path)
    if state.get("marker_count", 0) >= MAX_MARKERS_PER_SESSION:
        logger.debug("Marker cap reached (%d), skipping", MAX_MARKERS_PER_SESSION)
        return

    recent = get_tail_records(transcript_path, n=RECENT_TAIL_LINES)
    if not recent:
        logger.debug("No recent records found")
        return

    new_markers = run_stage1_filter(recent, l3_hints)
    if not new_markers:
        logger.debug("Stage 1: no signals detected")
        return

    existing_markers = read_markers(marker_path)
    unique_markers = deduplicate_markers(existing_markers, new_markers)
    if not unique_markers:
        logger.debug("All markers deduplicated")
        return

    remaining = MAX_MARKERS_PER_SESSION - state.get("marker_count", 0)
    unique_markers = unique_markers[:remaining]

    written = append_markers(marker_path, unique_markers)

    new_count = state.get("marker_count", 0) + written
    type_counts = state.get("type_counts", {})
    for m in unique_markers:
        mt = m.get("type", "unknown")
        type_counts[mt] = type_counts.get(mt, 0) + 1

    write_correction_state(state_path, {
        "marker_count": new_count,
        "type_counts": type_counts,
        "last_mark_ts": datetime.now().isoformat(timespec="seconds"),
    })

    logger.info("mark: wrote %d markers (total %d)", written, new_count)


# ---------------------------------------------------------------------------
# run_extract: sessionEnd LLM 精判模式
# ---------------------------------------------------------------------------


def run_extract(
    project_dir: Path,
    transcript_path: Path,
    session_id: str,
    l3_hints: dict,
) -> None:
    """extract 模式主流程：读取 markers → LLM 精判 → 生成结构化经验。

    Args:
        project_dir: 项目根目录。
        transcript_path: transcript 文件路径。
        session_id: 6 字符会话 ID。
        l3_hints: L3 反馈提示。
    """
    hooks_dir = project_dir / "infra" / "hooks"
    marker_path = get_marker_path(hooks_dir, session_id)
    state_path = get_state_path(hooks_dir, session_id)

    markers = read_markers(marker_path)
    if len(markers) < MIN_MARKERS_FOR_EXTRACT:
        logger.info(
            "extract: only %d markers (need >= %d), skipping",
            len(markers), MIN_MARKERS_FOR_EXTRACT,
        )
        cleanup_markers(marker_path)
        return

    logger.info("extract: processing %d markers", len(markers))

    all_records = parse_transcript_lines(transcript_path)
    if not all_records:
        logger.warning("extract: empty transcript")
        cleanup_markers(marker_path)
        return

    fragments: list[str] = []
    for marker in markers:
        window = get_context_window(all_records, marker["line"])
        fragment_text = (
            f"[Marker type: {marker.get('type', 'unknown')}, "
            f"keywords: {marker.get('keywords', [])}, "
            f"line: {marker.get('line', 0)}]\n"
            f"--- Before ---\n{window['before']}\n"
            f"--- Trigger ---\n{window['trigger_turn']}\n"
            f"--- After ---\n{window['after']}"
        )
        fragments.append(fragment_text)

    total_text = "\n\n".join(fragments)
    if len(total_text) > TRANSCRIPT_TRUNCATE_CHARS:
        total_text = truncate_text(total_text, TRANSCRIPT_TRUNCATE_CHARS)
        fragments = [total_text]

    messages = build_extract_prompt(fragments)

    result = call_codebuddy(messages, DEFAULT_CODEBUDDY_MODEL)
    if result is None:
        result = call_ollama(messages, DEFAULT_OLLAMA_MODEL)

    if result is None:
        logger.warning("extract: all LLM engines failed")
        cleanup_markers(marker_path)
        return

    parsed = extract_json_from_response(result)
    if parsed is None:
        logger.warning("extract: failed to parse LLM JSON response")
        cleanup_markers(marker_path)
        return

    raw_events = parsed.get("events", [])
    session_topics = parsed.get("session_topics", [])

    if not isinstance(raw_events, list):
        logger.warning("extract: events is not a list")
        cleanup_markers(marker_path)
        return
    if not isinstance(session_topics, list):
        session_topics = []

    filtered: list[dict] = []
    for evt in raw_events:
        if not isinstance(evt, dict):
            continue
        try:
            confidence = float(evt.get("confidence", 0))
        except (ValueError, TypeError):
            confidence = 0.0
        if confidence < MIN_CONFIDENCE:
            logger.debug("extract: dropping low-confidence event (%.2f)", confidence)
            continue
        evt_type = evt.get("type", "")
        if evt_type not in VALID_TYPES:
            evt["type"] = "insight/pattern"
        filtered.append(evt)

    if not filtered:
        logger.info("extract: no events passed confidence filter")
        cleanup_markers(marker_path)
        return

    for i, evt in enumerate(filtered):
        if i < len(markers):
            marker = markers[i]
            window = get_context_window(all_records, marker["line"])
            evt["context"] = {
                "task": evt.get("observation", ""),
                "what_ai_did": "",
                "what_should_have_done": "",
                "conversation_window": window,
            }
            evt["ts"] = marker.get("ts", datetime.now().isoformat(timespec="seconds"))

    # 新路径：docs/memory/learnings/（按日合并）
    learnings_dir = project_dir / "docs" / "memory" / "learnings"
    # 兼容：同时检查旧路径的已有经验用于去重
    old_learnings_dir = project_dir / "docs" / "learnings"
    unique_events = deduplicate_events(filtered, learnings_dir)
    if old_learnings_dir.exists():
        unique_events = deduplicate_events(unique_events, old_learnings_dir)

    if not unique_events:
        logger.info("extract: all events deduplicated")
        cleanup_markers(marker_path)
        return

    record = build_experience_record(session_id, unique_events, session_topics, project_dir)

    today = datetime.now().strftime("%Y-%m-%d")
    output_path = learnings_dir / f"{today}.json"

    # 按日合并：如果今天已有文件，合并 events
    if output_path.exists():
        try:
            existing = json.loads(output_path.read_text(encoding="utf-8"))
            existing_events = existing.get("events", [])
            existing_topics = existing.get("session_topics", [])
            record["events"] = existing_events + record["events"]
            record["event_count"] = len(record["events"])
            record["session_topics"] = list(set(existing_topics + record.get("session_topics", [])))
        except (OSError, json.JSONDecodeError):
            pass

    write_experience(output_path, record)

    cleanup_markers(marker_path)
    try:
        if state_path.exists():
            state_path.unlink()
    except OSError:
        pass

    logger.info("extract: wrote %d events to %s", len(unique_events), output_path)


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def main() -> None:
    """脚本入口：解析参数，启动 watchdog，按模式分发执行。"""
    parser = argparse.ArgumentParser(description="Cursor hook experience collector")
    parser.add_argument("--mode", choices=["mark", "extract"], required=True)
    parser.add_argument("--transcript", default=None, help="手动指定 transcript 路径")
    parser.add_argument("--project-dir", default=None, help="手动指定项目根目录")
    args = parser.parse_args()

    wd = threading.Thread(
        target=watchdog_timer, args=(SCRIPT_HARD_TIMEOUT,), daemon=True
    )
    wd.start()

    stdin_data = read_stdin()
    session_id_full = _resolve_session_id(stdin_data)
    session_id = re.sub(r"[^a-zA-Z0-9_-]", "", session_id_full[:6]) or "unknwn"

    project_dir = resolve_project_dir(args.project_dir)

    global logger
    logger = setup_logging(project_dir)
    logger.info(
        "=== collect_experience start === mode=%s session=%s", args.mode, session_id
    )

    transcript_path = resolve_transcript_path(args.transcript, stdin_data)
    if transcript_path is None:
        logger.info("No transcript found, exiting")
        sys.exit(0)

    hooks_dir = project_dir / "infra" / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    lock_path = hooks_dir / f".experience_{args.mode}_{session_id}.lock"
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
        l3_hints = load_l3_hints(project_dir)
        if l3_hints["heightened_categories"] or l3_hints["suppressed_categories"]:
            logger.info(
                "L3 hints: heightened=%s, suppressed=%s",
                l3_hints["heightened_categories"],
                l3_hints["suppressed_categories"],
            )

        if args.mode == "mark":
            run_mark(project_dir, transcript_path, session_id, l3_hints)
        else:
            run_extract(project_dir, transcript_path, session_id, l3_hints)
    finally:
        if lock_file:
            release_lock(lock_file)
            lock_file.close()

    logger.info("=== collect_experience done === mode=%s", args.mode)


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
