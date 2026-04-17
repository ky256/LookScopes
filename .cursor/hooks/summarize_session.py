#!/usr/bin/env python3
"""Cursor hook 会话摘要脚本。

由 .cursor/hooks.json 配置触发，在 Cursor AI 回复后（stop hook）
或对话结束时（sessionEnd hook）自动生成/更新会话摘要。

引擎优先级: CodeBuddy (glm-5.0) → Ollama (8B) → 静默跳过
"""

import argparse
import glob
import json
import logging
import msvcrt
import re
import os
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
FAST_CHECK_MINUTES = 3
THROTTLE_MINUTES = 15
THROTTLE_TURNS = 10
DEFAULT_MAX_CONTEXT = 200000
TRANSCRIPT_TRUNCATE_CHARS = 50000
OLLAMA_TRUNCATE_CHARS = 8000

REQUIRED_HEADINGS = ["## 讨论要点", "## 做出的决策"]

SYSTEM_PROMPT = (
    "你是一个开发会话摘要助手。根据 Cursor IDE 的对话记录，生成结构化的会话摘要。\n"
    "规则:\n"
    "- 忽略工具调用细节、搜索结果、文件内容等噪音，聚焦于讨论和决策\n"
    '- "讨论要点"只列关键议题，不复述对话\n'
    '- "做出的决策"必须明确记录选择了什么、放弃了什么\n'
    '- "用户偏好"提取用户表达的风格/流程/工具偏好\n'
    "- 使用中文"
)

FIRST_TIME_USER_TEMPLATE = (
    "以下是 Cursor IDE 的对话记录。请生成结构化摘要。\n\n"
    "## 对话内容\n{conversation}\n\n"
    "请按以下格式输出:\n\n"
    "# 会话摘要: [用一句话概括主题]\n\n"
    "- 日期: {date}\n"
    "- Session: {session_id}\n"
    "- 轮次: 约 {turns} 轮对话（1 轮 = 1 对 user+assistant 消息）\n"
    "- 引擎: {engine}/{model}\n"
    "- 最后更新: {timestamp}\n\n"
    "## 讨论要点\n- ...\n\n"
    "## 做出的决策\n- ...\n\n"
    "## 发现的用户偏好\n- ...\n\n"
    "## 下一步行动\n- ...\n"
)

INCREMENTAL_USER_TEMPLATE = (
    "以下是现有摘要和新增的对话内容。请更新摘要，保留已有信息，整合新内容。\n"
    '如果新对话修改了之前的决策，更新对应条目并标注"[已更新]"。\n\n'
    "## 现有摘要\n{existing_summary}\n\n"
    "## 新增对话（第 {start_line} 行之后）\n{new_conversation}\n\n"
    "请输出更新后的完整摘要。\n"
)

logger: logging.Logger = logging.getLogger("summarize_session")


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
    log_path = project_dir / ".cursor" / "hooks" / "summarize.log"
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

    root = logging.getLogger("summarize_session")
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
# stdin 读取
# ---------------------------------------------------------------------------


def read_stdin() -> dict:
    """读取 Cursor 通过 stdin 传入的 JSON 负载。

    限制最多读取 64KB，解析失败返回空 dict。

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
      3. CURSOR_TRANSCRIPT_PATH 路径中的 UUID（父目录名或文件名）
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
        # 路径形如 .../agent-transcripts/<uuid>/<uuid>.jsonl
        candidate = p.stem  # uuid (without .jsonl)
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


def resolve_transcript_path(cli_arg: str | None) -> Path | None:
    """确定 transcript 文件路径。

    优先级: CURSOR_TRANSCRIPT_PATH 环境变量 → CLI 参数。
    文件不存在时返回 None。

    Args:
        cli_arg: 命令行传入的 --transcript 值，可为 None。

    Returns:
        transcript 文件的 Path，或 None。
    """
    env_val = os.environ.get("CURSOR_TRANSCRIPT_PATH")
    candidate = env_val if env_val else cli_arg
    if not candidate:
        return None
    p = Path(candidate)
    if not p.exists():
        return None
    return p


# ---------------------------------------------------------------------------
# 节流
# ---------------------------------------------------------------------------


def fast_throttle_check(state_file: Path) -> bool:
    """基于 state 文件 mtime 的快速节流检查。

    如果 state 文件最后修改不到 FAST_CHECK_MINUTES 分钟，返回 True（应跳过）。

    Args:
        state_file: 状态文件路径。

    Returns:
        True 表示应跳过本次执行。
    """
    if not state_file.exists():
        return False
    try:
        mtime = datetime.fromtimestamp(state_file.stat().st_mtime)
        return datetime.now() - mtime < timedelta(minutes=FAST_CHECK_MINUTES)
    except OSError:
        return False


def full_throttle_check(state: dict, new_turns: int) -> bool:
    """完整节流检查：距上次摘要不足阈值轮次且不足阈值时间则跳过。

    Args:
        state: 上次的状态 dict（含 last_summary_time）。
        new_turns: 自上次摘要以来的新增 user 消息数（实际轮次）。

    Returns:
        True 表示应跳过本次执行。
    """
    last_time_str = state.get("last_summary_time")
    if not last_time_str:
        return False
    try:
        last_time = datetime.fromisoformat(last_time_str)
    except (ValueError, TypeError):
        return False
    elapsed = datetime.now() - last_time
    if elapsed < timedelta(minutes=THROTTLE_MINUTES) and new_turns < THROTTLE_TURNS:
        return True
    return False


def count_lines(path: Path) -> int:
    """快速统计文件总行数（不解析内容）。

    Args:
        path: 文件路径。

    Returns:
        行数，文件不存在返回 0。
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0


def count_turns_since(transcript_path: Path, since_line: int) -> int:
    """统计 transcript 中指定行之后的 user 消息数（粗估轮次）。

    Args:
        transcript_path: transcript 文件路径。
        since_line: 起始行号（0-based），从该行之后开始计数。

    Returns:
        user 消息数量。
    """
    turns = 0
    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            for idx, line in enumerate(f):
                if idx < since_line:
                    continue
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                    if obj.get("role") == "user":
                        turns += 1
                except (json.JSONDecodeError, TypeError):
                    pass
    except OSError:
        pass
    return turns


# ---------------------------------------------------------------------------
# Transcript 解析
# ---------------------------------------------------------------------------


def parse_transcript(transcript_path: Path, since_line: int = 0) -> tuple[str, int]:
    """解析 .jsonl transcript 文件，提取对话文本。

    Args:
        transcript_path: transcript 文件路径。
        since_line: 从该行（0-based）之后开始解析；0 表示从头开始。

    Returns:
        (拼接后的对话文本, 文件总行数) 元组。
    """
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
                role = obj.get("role", "unknown")
                content = _extract_content(obj)
                if content:
                    parts.append(f"[{role}]: {content}")
    except OSError as exc:
        logger.warning("parse_transcript failed: %s", exc)
    return "\n".join(parts), total_lines


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
    message = obj.get("message") or {}
    if isinstance(message, dict):
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
# Prompt 构建
# ---------------------------------------------------------------------------


def build_prompt(
    existing_summary: str | None,
    new_conversation: str,
    start_line: int,
    session_id: str = "",
    turns: int = 0,
    engine: str = "",
    model: str = "",
) -> list[dict]:
    """构建发送给 LLM 的 messages 列表。

    Args:
        existing_summary: 已有摘要文本，首次为 None。
        new_conversation: 本次新增的对话文本。
        start_line: 对话起始行号。
        session_id: 会话 ID 前 6 字符。
        turns: 估计轮次。
        engine: 使用的引擎名称。
        model: 使用的模型名称。

    Returns:
        OpenAI 兼容的 messages 列表。
    """
    now = datetime.now()
    messages: list[dict] = [{"role": "system", "content": SYSTEM_PROMPT}]

    if existing_summary:
        user_content = INCREMENTAL_USER_TEMPLATE.format(
            existing_summary=existing_summary,
            start_line=start_line,
            new_conversation=new_conversation,
        )
    else:
        user_content = FIRST_TIME_USER_TEMPLATE.format(
            conversation=new_conversation,
            date=now.strftime("%Y-%m-%d"),
            session_id=session_id,
            turns=turns,
            engine=engine,
            model=model,
            timestamp=now.strftime("%Y-%m-%d %H:%M"),
        )

    messages.append({"role": "user", "content": user_content})
    return messages


# ---------------------------------------------------------------------------
# CodeBuddy API
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
    """调用 CodeBuddy API 生成摘要。

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
    """调用本地 Ollama API 生成摘要。

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
# 摘要校验 & 写入
# ---------------------------------------------------------------------------


def validate_summary_format(text: str) -> bool:
    """检查 LLM 输出是否包含必要的 Markdown 标题。

    Args:
        text: LLM 返回的摘要文本。

    Returns:
        True 表示格式合规。
    """
    return all(heading in text for heading in REQUIRED_HEADINGS)


def write_summary(summary_path: Path, content: str) -> None:
    """通过临时文件 + os.replace 原子写入摘要。

    Args:
        summary_path: 摘要目标文件路径。
        content: 摘要内容。
    """
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = summary_path.with_suffix(".tmp")
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(str(tmp_path), str(summary_path))
        logger.info("Summary written: %s", summary_path)
    except OSError as exc:
        logger.error("write_summary failed: %s", exc)
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 状态管理
# ---------------------------------------------------------------------------


def read_state(state_path: Path) -> dict:
    """读取状态 JSON 文件。

    Args:
        state_path: 状态文件路径。

    Returns:
        解析后的状态 dict，缺失或损坏时返回默认值。
    """
    default = {"last_summary_line": 0, "last_summary_time": None, "engine": None}
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


def write_state(state_path: Path, state: dict) -> None:
    """通过临时文件 + os.replace 原子写入状态。

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
        logger.error("write_state failed: %s", exc)
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


def cleanup_old_states(hooks_dir: Path, max_age_days: int = 7) -> None:
    """清理超过指定天数的旧状态文件。

    Args:
        hooks_dir: .cursor/hooks/ 目录路径。
        max_age_days: 最大保留天数。
    """
    cutoff = time.time() - max_age_days * 86400
    pattern = str(hooks_dir / ".summary_state_*.json")
    for path_str in glob.glob(pattern):
        try:
            if os.path.getmtime(path_str) < cutoff:
                os.remove(path_str)
                logger.debug("Cleaned old state: %s", path_str)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def main() -> None:
    """脚本入口：解析参数，启动 watchdog，按模式分发执行。"""
    parser = argparse.ArgumentParser(description="Cursor hook session summarizer")
    parser.add_argument("--mode", choices=["throttled", "final"], required=True)
    parser.add_argument(
        "--engine", choices=["auto", "codebuddy", "ollama"], default="auto"
    )
    parser.add_argument("--model", default=None, help="Override model name")
    parser.add_argument("--force", action="store_true", help="Skip throttle checks")
    parser.add_argument(
        "--dry-run", action="store_true", help="Print prompt without calling LLM"
    )
    parser.add_argument(
        "--transcript", default=None, help="Manual transcript path for debugging"
    )
    parser.add_argument(
        "--project-dir", default=None, help="Manual project dir for debugging"
    )
    args = parser.parse_args()

    # 1. Watchdog
    wd = threading.Thread(
        target=watchdog_timer, args=(SCRIPT_HARD_TIMEOUT,), daemon=True
    )
    wd.start()

    # 2. stdin → session_id
    stdin_data = read_stdin()
    session_id_full = _resolve_session_id(stdin_data)
    session_id = re.sub(r"[^a-zA-Z0-9_-]", "", session_id_full[:6]) or "unknwn"

    # 3. project_dir
    project_dir = resolve_project_dir(args.project_dir)

    # 4. logging
    global logger
    logger = setup_logging(project_dir)
    logger.debug("stdin_data keys=%s", list(stdin_data.keys()))
    logger.info(
        "=== summarize_session start === mode=%s session=%s", args.mode, session_id
    )

    # 5. transcript path
    transcript_path = resolve_transcript_path(args.transcript)
    if transcript_path is None:
        logger.info("No transcript found, exiting")
        sys.exit(0)

    # 6. state file
    hooks_dir = project_dir / ".cursor" / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    state_file = hooks_dir / f".summary_state_{session_id}.json"

    is_throttled = args.mode == "throttled" and not args.force

    # 7. 快速路径
    if is_throttled and fast_throttle_check(state_file):
        logger.debug("Fast throttle: skipped (state mtime too recent)")
        sys.exit(0)

    # 8. 读取 state
    state = read_state(state_file)

    # 9. 统计新增轮次 + 总行数
    last_line = state.get("last_summary_line", 0)
    total_lines = count_lines(transcript_path)

    if last_line > total_lines:
        logger.warning(
            "last_summary_line (%d) > actual lines (%d), resetting to 0",
            last_line, total_lines,
        )
        state["last_summary_line"] = 0
        last_line = 0

    turns = count_turns_since(transcript_path, last_line)

    # 10. 完整节流（基于实际轮次，非原始行数）
    if is_throttled and full_throttle_check(state, turns):
        logger.debug(
            "Full throttle: skipped (turns=%d, total_lines=%d)", turns, total_lines
        )
        sys.exit(0)

    # 11. 文件锁（用独立 .lock 文件，避免锁住 state 文件导致 os.replace 失败）
    lock_path = state_file.with_suffix(".lock")
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
        _run_summary(args, project_dir, transcript_path, state, state_file, session_id, turns, total_lines)
    finally:
        if lock_file:
            release_lock(lock_file)
            lock_file.close()


def _run_summary(
    args: argparse.Namespace,
    project_dir: Path,
    transcript_path: Path,
    state: dict,
    state_file: Path,
    session_id: str,
    turns: int,
    total_lines: int,
) -> None:
    """获取锁后执行实际摘要生成流程。

    Args:
        args: CLI 参数命名空间。
        project_dir: 项目根目录。
        transcript_path: transcript 文件路径。
        state: 当前状态 dict。
        state_file: 状态文件路径。
        session_id: 6 字符会话 ID。
        turns: 新增轮次估计。
        total_lines: transcript 文件总行数。
    """
    last_line = state.get("last_summary_line", 0)

    # 12. 解析 transcript（只解析 last_line 之后的新增内容）
    new_conversation, actual_total = parse_transcript(transcript_path, since_line=last_line)
    if actual_total > total_lines:
        total_lines = actual_total
    if not new_conversation.strip():
        logger.info("No new conversation since line %d", last_line)
        write_state(state_file, {
            **state,
            "last_summary_time": datetime.now().isoformat(),
            "last_summary_line": total_lines,
        })
        return

    # 13. 读取已有摘要
    summary_dir = project_dir / "docs" / "summaries"
    summary_dir.mkdir(parents=True, exist_ok=True)
    today = datetime.now().strftime("%Y-%m-%d")
    summary_path = summary_dir / f"{today}-{session_id}.md"

    existing_summary: str | None = None
    if summary_path.exists():
        try:
            with open(summary_path, "r", encoding="utf-8") as f:
                existing_summary = f.read()
        except OSError:
            existing_summary = None

    # 确定引擎和模型
    engine_choice = args.engine
    model_override = args.model or os.environ.get("SUMMARY_MODEL")
    try:
        max_context = int(os.environ.get("SUMMARY_MAX_CONTEXT", str(DEFAULT_MAX_CONTEXT)))
    except (ValueError, TypeError):
        logger.warning("Invalid SUMMARY_MAX_CONTEXT, using default %d", DEFAULT_MAX_CONTEXT)
        max_context = DEFAULT_MAX_CONTEXT

    # 截断对话文本
    truncate_limit = TRANSCRIPT_TRUNCATE_CHARS
    if engine_choice == "ollama":
        truncate_limit = OLLAMA_TRUNCATE_CHARS
    new_conversation = truncate_text(new_conversation, truncate_limit)

    # 14. 构建 prompt
    codebuddy_model = model_override or DEFAULT_CODEBUDDY_MODEL
    ollama_model = model_override or DEFAULT_OLLAMA_MODEL
    actual_engine = "unknown"
    actual_model = codebuddy_model

    messages = build_prompt(
        existing_summary=existing_summary,
        new_conversation=new_conversation,
        start_line=last_line,
        session_id=session_id,
        turns=turns,
        engine="CodeBuddy",
        model=codebuddy_model,
    )

    # 15. 估算 token 量，压缩旧摘要
    total_chars = sum(len(m["content"]) for m in messages)
    estimated_tokens = total_chars // 2
    if estimated_tokens > int(max_context * 0.8) and existing_summary:
        logger.info(
            "Prompt too long (%d est tokens), compressing old summary", estimated_tokens
        )
        existing_summary = truncate_text(existing_summary, max_context // 4)
        messages = build_prompt(
            existing_summary=existing_summary,
            new_conversation=new_conversation,
            start_line=last_line,
            session_id=session_id,
            turns=turns,
            engine="CodeBuddy",
            model=codebuddy_model,
        )

    # dry-run 模式
    if args.dry_run:
        logger.info("Dry-run mode, printing prompt to stderr")
        for m in messages:
            try:
                sys.stderr.buffer.write(
                    f"--- {m['role']} ---\n{m['content']}\n\n".encode("utf-8", errors="replace")
                )
            except OSError:
                logger.debug("stderr write failed, skipping dry-run output")
        return

    # 16. 引擎选择
    result: str | None = None

    if engine_choice in ("auto", "codebuddy"):
        result = call_codebuddy(messages, codebuddy_model)
        if result:
            actual_engine = "CodeBuddy"
            actual_model = codebuddy_model

    if result is None and engine_choice in ("auto", "ollama"):
        messages_ollama = build_prompt(
            existing_summary=existing_summary,
            new_conversation=truncate_text(new_conversation, OLLAMA_TRUNCATE_CHARS),
            start_line=last_line,
            session_id=session_id,
            turns=turns,
            engine="Ollama",
            model=ollama_model,
        )
        result = call_ollama(messages_ollama, ollama_model)
        if result:
            actual_engine = "Ollama"
            actual_model = ollama_model

    if result is None:
        logger.warning("All engines failed, no summary generated")
        write_state(state_file, {**state, "last_summary_time": datetime.now().isoformat()})
        return

    # 17. 校验
    if not validate_summary_format(result):
        if existing_summary:
            logger.warning("LLM output format invalid, keeping old summary")
            return
        logger.warning("LLM output format invalid (first time), writing anyway")

    # 更新元数据中的引擎信息
    result = _patch_engine_meta(result, actual_engine, actual_model)

    # 18. 写入摘要
    write_summary(summary_path, result)

    # 19. 更新状态
    new_state = {
        "last_summary_line": total_lines,
        "last_summary_time": datetime.now().isoformat(),
        "engine": actual_engine,
        "model": actual_model,
        "session_id": session_id,
    }
    write_state(state_file, new_state)

    # 20. 清理旧 state
    hooks_dir = state_file.parent
    cleanup_old_states(hooks_dir)

    logger.info("=== summarize_session done === engine=%s/%s", actual_engine, actual_model)


def _patch_engine_meta(text: str, engine: str, model: str) -> str:
    """如果摘要中包含占位引擎信息，替换为实际使用的引擎/模型。

    Args:
        text: 原始摘要文本。
        engine: 实际使用的引擎名。
        model: 实际使用的模型名。

    Returns:
        替换后的文本。
    """
    text = re.sub(r"- 引擎[：:]\s*\S+/\S+", f"- 引擎: {engine}/{model}", text, count=1)
    return text


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
