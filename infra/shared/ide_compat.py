#!/usr/bin/env python3
"""IDE 兼容层（共享模块）。

封装所有 IDE 差异（Cursor/CodeBuddy/Claude Code 的路径、认证、stdin 格式），
向上层提供统一接口。从三个 hook 脚本中提取消除重复。
"""

import json
import logging
import os
import re
import sys
import threading
import time
from pathlib import Path

logger: logging.Logger = logging.getLogger("shared.ide_compat")

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

SCRIPT_HARD_TIMEOUT = 180


# ---------------------------------------------------------------------------
# stdin 读取
# ---------------------------------------------------------------------------


def read_stdin() -> dict:
    """读取 Hook 系统通过 stdin 传入的 JSON 负载。

    限制最多读取 64KB，解析失败返回空 dict。
    使用 utf-8-sig 解码以兼容 Cursor 在 Windows 下发送的 UTF-8 BOM 前缀。

    Returns:
        解析后的 dict，或空 dict。
    """
    try:
        raw = sys.stdin.buffer.read(65536)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8-sig"))
    except Exception:
        return {}


# ---------------------------------------------------------------------------
# Session ID 解析
# ---------------------------------------------------------------------------


def resolve_session_id(stdin_data: dict) -> str:
    """从多个来源尝试提取 session ID。

    优先级:
      1. stdin JSON 的 session_id 字段（CodeBuddy / Claude Code 方式）
      2. CURSOR_SESSION_ID 环境变量
      3. stdin transcript_path 或 CURSOR_TRANSCRIPT_PATH 路径中的 UUID
      4. fallback "unknown"

    Args:
        stdin_data: 从 stdin 读取的 JSON dict。

    Returns:
        session ID 字符串。
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


def sanitize_session_id(session_id_full: str, max_len: int = 6) -> str:
    """将原始 session ID 清理为安全的文件名片段。

    Args:
        session_id_full: 原始 session ID。
        max_len: 截断长度。

    Returns:
        清理后的 session ID（仅保留字母数字下划线连字符）。
    """
    return re.sub(r"[^a-zA-Z0-9_-]", "", session_id_full[:max_len]) or "unknwn"


# ---------------------------------------------------------------------------
# 项目目录解析
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


# ---------------------------------------------------------------------------
# CodeBuddy 数据目录
# ---------------------------------------------------------------------------


def get_codebuddy_data_base() -> Path | None:
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


def iter_codebuddy_history_dirs(base: Path):
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


def discover_codebuddy_transcript(session_id: str) -> Path | None:
    """通过 session_id 在 CodeBuddy 存储目录中搜索 transcript 文件。

    Args:
        session_id: 完整的 session UUID。

    Returns:
        transcript index.json 的 Path，或 None。
    """
    base = get_codebuddy_data_base()
    if base is None:
        return None
    for _, project_dir in iter_codebuddy_history_dirs(base):
        candidate = project_dir / session_id / "index.json"
        if candidate.exists():
            return candidate
    return None


def discover_latest_codebuddy_transcript() -> Path | None:
    """在没有 session_id 时，通过最近修改时间发现最新的 transcript。

    Returns:
        最新 session 的 index.json Path，或 None。
    """
    base = get_codebuddy_data_base()
    if base is None:
        return None

    latest_path: Path | None = None
    latest_mtime: float = 0

    for _, project_dir in iter_codebuddy_history_dirs(base):
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


# ---------------------------------------------------------------------------
# Transcript 路径解析
# ---------------------------------------------------------------------------


def resolve_transcript_path(cli_arg: str | None, stdin_data: dict | None = None) -> Path | None:
    """确定 transcript 文件路径。

    优先级（显式来源优先于发现式兜底）：
      1. stdin transcript_path（Cursor / CodeBuddy 均可能提供）
      2. CURSOR_TRANSCRIPT_PATH 环境变量（Cursor 原生）
      3. CLI --transcript 参数
      4. CodeBuddy session_id → 目录发现（仅当 stdin.session_id 存在）
      5. CodeBuddy 最新 session 兜底（仅当运行在 CodeBuddy 环境中）

    Note:
        将 CodeBuddy "最新 session" 兜底降到最低优先级，并限制为仅在
        CODEBUDDY_PROJECT_DIR 存在时启用，避免 Cursor 环境下错误
        匹配到本机残留的 CodeBuddy 历史 session（导致读取错误的 transcript）。

    Args:
        cli_arg: 命令行传入的 --transcript 值，可为 None。
        stdin_data: 从 stdin 读取的 JSON 数据，可为 None。

    Returns:
        transcript 文件的 Path，或 None。
    """
    if stdin_data and stdin_data.get("transcript_path"):
        p = Path(stdin_data["transcript_path"])
        logger.debug("transcript from stdin: %s exists=%s", p, p.exists())
        if p.exists():
            return p

    env_val = os.environ.get("CURSOR_TRANSCRIPT_PATH")
    if env_val:
        p = Path(env_val)
        logger.debug("transcript from CURSOR_TRANSCRIPT_PATH: %s exists=%s", p, p.exists())
        if p.exists():
            return p

    if cli_arg:
        p = Path(cli_arg)
        if p.exists():
            return p

    if stdin_data and stdin_data.get("session_id"):
        p = discover_codebuddy_transcript(stdin_data["session_id"])
        if p is not None:
            logger.debug("transcript discovered via session_id: %s", p)
            return p

    is_codebuddy_env = bool(os.environ.get("CODEBUDDY_PROJECT_DIR"))
    if is_codebuddy_env:
        p = discover_latest_codebuddy_transcript()
        if p is not None:
            logger.debug("transcript fallback to latest CodeBuddy session: %s", p)
            return p

    return None


# ---------------------------------------------------------------------------
# IDE 检测
# ---------------------------------------------------------------------------


def detect_ide_prefix(project_dir: Path) -> str:
    """检测当前 IDE 环境，返回配置目录前缀。

    检测逻辑:
    1. CODEBUDDY_PROJECT_DIR 环境变量存在 → ".codebuddy"
    2. .codebuddy/settings.json 存在 → ".codebuddy"
    3. 其他情况 → ".cursor" (默认/Cursor 兼容)

    Returns:
        ".codebuddy" 或 ".cursor"
    """
    if os.environ.get("CODEBUDDY_PROJECT_DIR"):
        return ".codebuddy"
    if (project_dir / ".codebuddy" / "settings.json").exists():
        return ".codebuddy"
    return ".cursor"


def get_target_file_convention(ide_prefix: str) -> str:
    """根据 IDE 前缀生成 target_file 路径约定文本。

    Args:
        ide_prefix: ".codebuddy" 或 ".cursor"

    Returns:
        格式化的路径约定字符串，用于注入 LLM prompt。
    """
    if ide_prefix == ".codebuddy":
        return (
            "target_file 路径约定：\n"
            "- 新规则/规则修改 → .codebuddy/rules/<kebab-name>/RULE.mdc\n"
            "- 偏好持久化 → .codebuddy/rules/meta-agent-identity/RULE.mdc\n"
            "- 反模式 → .codebuddy/skills/antipatterns/SKILL.md\n"
            "- Skill 修补 → .codebuddy/skills/<skill-name>/SKILL.md\n"
            "禁止在项目根目录下创建 rules/ 等自造目录。\n"
        )
    return (
        "target_file 路径约定：\n"
        "- 新规则/规则修改 → .cursor/rules/<kebab-name>.mdc\n"
        "- 偏好持久化 → .cursor/rules/meta-agent-identity.mdc\n"
        "- 反模式 → .cursor/skills/antipatterns/SKILL.md\n"
        "- Skill 修补 → .cursor/skills/<skill-name>/SKILL.md\n"
        "- Agent 修补 → .cursor/agents/<agent-name>.md\n"
        "禁止在项目根目录下创建 rules/ 等自造目录。\n"
    )


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


def start_watchdog(seconds: int = SCRIPT_HARD_TIMEOUT) -> threading.Thread:
    """启动看门狗守护线程。

    Args:
        seconds: 超时秒数。

    Returns:
        已启动的守护线程。
    """
    wd = threading.Thread(target=watchdog_timer, args=(seconds,), daemon=True)
    wd.start()
    return wd


# ---------------------------------------------------------------------------
# 日志配置工具
# ---------------------------------------------------------------------------


def setup_logging(project_dir: Path, script_name: str) -> logging.Logger:
    """配置带 RotatingFileHandler 的日志系统。

    Args:
        project_dir: 项目根目录路径。
        script_name: 脚本名称（用于日志文件名和 logger 名称）。

    Returns:
        配置好的 Logger 实例。
    """
    from logging.handlers import RotatingFileHandler

    log_path = project_dir / "infra" / "hooks" / f"{script_name}.log"
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

    root = logging.getLogger(script_name)
    root.setLevel(logging.DEBUG)
    root.addHandler(handler)
    return root
