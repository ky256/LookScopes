#!/usr/bin/env python3
"""Transcript 解析器（共享模块）。

统一封装 Cursor .jsonl 和 CodeBuddy index.json+messages/ 两种格式的 transcript 解析逻辑。
从 summarize_session.py 和 collect_experience.py 中提取消除重复。
"""

import json
import logging
from pathlib import Path

logger: logging.Logger = logging.getLogger("shared.transcript_parser")

# ---------------------------------------------------------------------------
# 常量（可被调用方覆盖的默认值）
# ---------------------------------------------------------------------------

RECENT_TAIL_LINES = 20
CONTEXT_WINDOW_TURNS = 5
CONTEXT_PREVIEW_CHARS = 200


# ---------------------------------------------------------------------------
# 内容提取
# ---------------------------------------------------------------------------


def extract_content(obj: dict) -> str:
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


# ---------------------------------------------------------------------------
# CodeBuddy 格式检测 & 加载
# ---------------------------------------------------------------------------


def load_codebuddy_messages(transcript_path: Path) -> list[dict] | None:
    """尝试从 CodeBuddy index.json + messages/ 目录加载消息列表。

    返回一个 dict 列表，每个 dict 格式同 JSONL 解析结果：
    [{"role": "user", "content": [...]}, ...]
    如果不是 CodeBuddy 格式，返回 None。
    """
    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            header = f.read(100).strip()

        if not (header.startswith('{') and '"messages"' in header):
            return None

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

            msg_file = messages_dir / f"{msg_id}.json"
            if not msg_file.exists():
                msg_file = messages_dir / f".{msg_id}_bak.json"
                if not msg_file.exists():
                    continue

            try:
                with open(msg_file, "r", encoding="utf-8") as f:
                    msg_data = json.load(f)

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


# ---------------------------------------------------------------------------
# 核心解析函数
# ---------------------------------------------------------------------------


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
    cb_msgs = load_codebuddy_messages(transcript_path)
    if cb_msgs is not None:
        return sum(1 for m in cb_msgs[since_line:] if isinstance(m, dict) and m.get("role") == "user")

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
                except (json.JSONDecodeError, TypeError):
                    continue
                if not isinstance(obj, dict):
                    continue
                if obj.get("role") == "user":
                    turns += 1
    except OSError:
        pass
    return turns


def parse_transcript(transcript_path: Path, since_line: int = 0) -> tuple[str, int]:
    """解析 transcript 文件，提取对话文本。

    自动检测 Cursor .jsonl 或 CodeBuddy index.json 格式。

    Args:
        transcript_path: transcript 文件路径。
        since_line: 从该行（0-based）之后开始解析；0 表示从头开始。

    Returns:
        (拼接后的对话文本, 文件总行数/消息数) 元组。
    """
    cb_msgs = load_codebuddy_messages(transcript_path)
    if cb_msgs is not None:
        parts = []
        for obj in cb_msgs[since_line:]:
            if not isinstance(obj, dict):
                continue
            role = obj.get("role", "unknown")
            content = extract_content(obj)
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
                content = extract_content(obj)
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
    cb_msgs = load_codebuddy_messages(transcript_path)
    if cb_msgs is not None:
        records: list[dict] = []
        for idx, obj in enumerate(cb_msgs):
            if not isinstance(obj, dict):
                continue
            role = obj.get("role", "unknown")
            content = extract_content(obj)
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
                content = extract_content(obj)
                records.append({
                    "line": idx,
                    "role": role,
                    "content": content,
                })
    except OSError as exc:
        logger.warning("parse_transcript_lines failed: %s", exc)
    return records


def get_tail_records(
    transcript_path: Path,
    n: int = RECENT_TAIL_LINES,
) -> list[dict]:
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
    preview_chars: int = CONTEXT_PREVIEW_CHARS,
) -> dict:
    """根据触发行号，提取前后各 window 轮的上下文窗口。

    Args:
        records: 完整的 transcript 记录列表。
        center_line: 触发事件所在的行号。
        window: 前后各取多少轮。
        preview_chars: 每条记录的内容预览字符数。

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
        text = f"[{records[i]['role']}]: {records[i]['content'][:preview_chars]}"
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
