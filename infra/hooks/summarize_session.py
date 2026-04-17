#!/usr/bin/env python3
"""会话摘要入口脚本（薄调度层）。

由 Hook 系统触发（Cursor stop/sessionEnd 或 CodeBuddy Stop/SessionEnd），
在 AI 回复后或对话结束时自动收集详情 + 生成/更新会话摘要。

职责：接收 stdin → 解析上下文 → 调度 memory/ 子系统模块 → 返回。
不含业务逻辑，所有实现委派给 infra/memory/ 和 infra/shared/。

新存储路径：docs/memory/daily/{date}.md (摘要) + docs/memory/details/{date}.md (详情)
"""

import argparse
import json
import os
import re
import sys
from datetime import datetime, timedelta
from pathlib import Path

# 确保 infra/ 在 sys.path 中（支持从 hooks/ 目录直接执行）
_INFRA_DIR = Path(__file__).resolve().parent.parent
if str(_INFRA_DIR.parent) not in sys.path:
    sys.path.insert(0, str(_INFRA_DIR.parent))

from infra.shared.ide_compat import (
    read_stdin,
    resolve_project_dir,
    resolve_session_id,
    resolve_transcript_path,
    sanitize_session_id,
    setup_logging,
    start_watchdog,
)
from infra.shared.state import acquire_lock, read_state, release_lock, write_state, cleanup_old_states
from infra.shared.transcript_parser import count_lines, count_turns_since, parse_transcript
from infra.shared.llm_client import truncate_text, TRANSCRIPT_TRUNCATE_CHARS, OLLAMA_TRUNCATE_CHARS
from infra.memory.detail_collector import DetailCollector
from infra.memory.detail_archiver import DetailArchiver
from infra.memory.summary_generator import SummaryGenerator
from infra.memory.pointer_injector import PointerInjector

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

SCRIPT_HARD_TIMEOUT = 180
THROTTLE_TURNS = 10
DEFAULT_MAX_CONTEXT = 200000
STATE_DEFAULT = {"last_collected_line": 0, "last_summary_turn": 0, "last_summary_time": None, "engine": None}

import logging
logger: logging.Logger = logging.getLogger("summarize_session")


# ---------------------------------------------------------------------------
# .pending.md 轮次标记
# ---------------------------------------------------------------------------

PENDING_TURNS_PREFIX = "<!-- pending_turns:"


def _read_pending_turns(pending_file: Path) -> int:
    """从 .pending.md 头部读取累积 user 轮次数。

    格式：文件第一行为 <!-- pending_turns:N -->
    兜底：头部标记缺失时扫描 [user]: 行数。
    """
    if not pending_file.exists():
        return 0
    try:
        with open(pending_file, "r", encoding="utf-8") as f:
            first_line = f.readline().strip()
            if first_line.startswith(PENDING_TURNS_PREFIX):
                num_str = first_line[len(PENDING_TURNS_PREFIX):].rstrip(" ->").strip()
                return int(num_str)
            # 头部标记缺失，扫描实际 user 行数
            f.seek(0)
            return sum(1 for line in f if line.startswith("[user]:"))
    except (OSError, ValueError):
        pass
    return 0





def _write_pending_turns(pending_file: Path, turns: int, clear_content: bool = False) -> None:
    """更新 .pending.md 头部的累积轮次标记。

    Args:
        pending_file: .pending.md 路径。
        turns: 新的累积轮次数。
        clear_content: True 时清空正文内容（归档后调用）。
    """
    header = f"{PENDING_TURNS_PREFIX}{turns} -->\n"
    try:
        if clear_content:
            with open(pending_file, "w", encoding="utf-8") as f:
                f.write(header)
        else:
            # 读取现有内容，替换或插入头部
            content = ""
            if pending_file.exists():
                with open(pending_file, "r", encoding="utf-8") as f:
                    content = f.read()
            # 移除旧的 header
            if content.startswith(PENDING_TURNS_PREFIX):
                newline_idx = content.find("\n")
                content = content[newline_idx + 1:] if newline_idx >= 0 else ""
            with open(pending_file, "w", encoding="utf-8") as f:
                f.write(header + content)
    except OSError:
        pass


def _build_turn_index_from_details(
    details_path: Path,
    line_range: tuple[int, int] | None,
) -> dict[int, int]:
    """扫描 details 文件的归档范围，构建轮次→行号映射。

    在 line_range 范围内扫描 [user]: 开头的行，按出现顺序编号为轮次 1, 2, 3...
    返回的行号是 .pending.md 内的相对行号（从 0 开始），
    PointerInjector 会加上 line_range[0] 偏移转为绝对行号。

    Args:
        details_path: details/{date}.md 文件路径。
        line_range: 本次归档的 (start_line, end_line)，1-based。

    Returns:
        {轮次号 → 相对行号} 映射。
    """
    if line_range is None or not details_path.exists():
        return {}

    start, end = line_range
    turn_index: dict[int, int] = {}
    user_turn = 0

    try:
        with open(details_path, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, start=1):  # 1-based
                if line_num < start:
                    continue
                if line_num > end:
                    break
                if line.startswith("[user]:"):
                    user_turn += 1
                    # 存储相对于 start 的偏移（PointerInjector 会加上 start）
                    turn_index[user_turn] = line_num - start
    except OSError:
        pass

    logger.debug("Built turn_index: %d turns in L%d-L%d", len(turn_index), start, end)
    return turn_index



# ---------------------------------------------------------------------------
# 经验信号 buffer
# ---------------------------------------------------------------------------

SIGNAL_BUFFER_HEADER_RE = r"<!-- signal_count:(\d+) \| last_updated: (.*?) -->"
SIGNAL_NOTIFY_THRESHOLD = 3


def _read_signal_count(buffer_path: Path) -> int:
    """从 signal-buffer.md 头部读取信号计数。"""
    if not buffer_path.exists():
        return 0
    try:
        with open(buffer_path, "r", encoding="utf-8") as f:
            for line in f:
                m = re.match(SIGNAL_BUFFER_HEADER_RE, line.strip())
                if m:
                    return int(m.group(1))
    except (OSError, ValueError):
        pass
    return 0


def _write_signal_buffer_header(buffer_path: Path, count: int) -> None:
    """更新 signal-buffer.md 的头部元数据行。"""
    now_str = datetime.now().strftime("%Y-%m-%dT%H:%M")
    new_header = f"<!-- signal_count:{count} | last_updated: {now_str} -->"
    try:
        content = ""
        if buffer_path.exists():
            content = buffer_path.read_text(encoding="utf-8")
        # 替换或插入 header
        if re.match(SIGNAL_BUFFER_HEADER_RE, content.split("\n")[1] if "\n" in content else ""):
            lines = content.split("\n")
            lines[1] = new_header
            content = "\n".join(lines)
        elif "<!-- signal_count:" in content:
            content = re.sub(SIGNAL_BUFFER_HEADER_RE, new_header, content)
        else:
            # 头部不存在，在 # 标题后插入
            if content.startswith("# "):
                first_nl = content.find("\n")
                content = content[:first_nl + 1] + "\n" + new_header + "\n" + content[first_nl + 1:]
            else:
                content = new_header + "\n" + content
        buffer_path.write_text(content, encoding="utf-8")
    except OSError:
        pass


def _append_signals_to_buffer(
    project_dir: Path,
    summary_data: dict,
    summary_md: str,
    session_id: str,
    date: str,
    line_range: tuple[int, int] | None,
) -> None:
    """从摘要结果中提取经验信号，追加到 signal-buffer.md。

    Args:
        project_dir: 项目根目录。
        summary_data: LLM 生成的结构化摘要 dict。
        summary_md: 渲染后的摘要 Markdown 文本。
        session_id: 会话 ID。
        date: 日期字符串。
        line_range: 本次归档在 details 文件中的行号范围。
    """
    # 从所有 session 中收集 experience_signals
    all_signals: list[dict] = []
    for session in summary_data.get("sessions", []):
        signals = session.get("experience_signals", [])
        if isinstance(signals, list):
            all_signals.extend(signals)

    if not all_signals:
        logger.debug("No experience signals in this summary")
        return

    buffer_path = project_dir / "docs" / "distilled" / "signal-buffer.md"
    buffer_path.parent.mkdir(parents=True, exist_ok=True)

    # 构建详情指针
    detail_ref = ""
    if line_range is not None:
        detail_ref = f"details/{date}.md#L{line_range[0]}-L{line_range[1]}"

    # 获取 session title
    session_title = ""
    for session in summary_data.get("sessions", []):
        session_title = session.get("title", "")
        if session_title:
            break

    # 构建摘要文本块（仅摘要部分，不含详情索引）
    summary_lines: list[str] = []
    for session in summary_data.get("sessions", []):
        for item in session.get("summary", []):
            summary_lines.append(f"  - {item}")
        for item in session.get("decisions", []):
            summary_lines.append(f"  - 决策: {item}")

    # 构建 buffer 条目
    entry_lines: list[str] = [
        f"\n## [{date}] 会话 {session_id} — {session_title}",
    ]
    for sig in all_signals:
        sig_type = sig.get("type", "unknown")
        sig_brief = sig.get("brief", "")
        entry_lines.append(f"- 信号类型: {sig_type}")
        entry_lines.append(f"- 描述: {sig_brief}")
    if summary_lines:
        entry_lines.append("- 摘要上下文:")
        entry_lines.extend(summary_lines)
    if detail_ref:
        entry_lines.append(f"- 详情: {detail_ref}")
    entry_lines.append("")

    # 追加到 buffer
    try:
        with open(buffer_path, "a", encoding="utf-8") as f:
            f.write("\n".join(entry_lines) + "\n")
    except OSError as exc:
        logger.error("Failed to append to signal buffer: %s", exc)
        return

    # 更新计数
    old_count = _read_signal_count(buffer_path)
    new_count = old_count + len(all_signals)
    _write_signal_buffer_header(buffer_path, new_count)

    logger.info(
        "Appended %d signals to buffer (total: %d, threshold: %d)",
        len(all_signals), new_count, SIGNAL_NOTIFY_THRESHOLD,
    )


# ---------------------------------------------------------------------------
# 核心流程
# ---------------------------------------------------------------------------


def handle_stop(
    project_dir: Path,
    transcript_path: Path,
    session_id: str,
    state: dict,
    state_file: Path,
    force: bool = False,
    engine_choice: str = "auto",
    model_override: str | None = None,
) -> None:
    """stop hook 处理：详情增量收集 + 节流判断 + 摘要生成。"""
    memory_dir = project_dir / "docs" / "memory"
    details_dir = memory_dir / "details"
    today = datetime.now().strftime("%Y-%m-%d")

    # 1. 详情增量收集（每次都做）→ 持久暂存文件（可 git 同步）
    pending_file = details_dir / ".pending.md"
    details_dir.mkdir(parents=True, exist_ok=True)
    collector = DetailCollector(transcript_path)
    last_collected = state.get("last_collected_line", 0)
    turn_index = collector.collect(last_collected, pending_file)

    # 2. 读取 .pending.md 头部的累积轮次，加上本次新增
    accumulated_turns = _read_pending_turns(pending_file) + collector.new_turns
    _write_pending_turns(pending_file, accumulated_turns)

    # 3. 纯轮次节流（基于累积轮次）
    if not force and accumulated_turns < THROTTLE_TURNS:
        state["last_collected_line"] = collector.current_line
        write_state(state_file, state)
        logger.debug("Throttled: %d accumulated turns (need >= %d)", accumulated_turns, THROTTLE_TURNS)
        return

    # 4. 读取 .pending.md 全部暂存内容（归档前），作为 LLM 摘要的输入
    try:
        pending_content = pending_file.read_text(encoding="utf-8")
        # 跳过头部 pending_turns 标记行
        if pending_content.startswith(PENDING_TURNS_PREFIX):
            newline_idx = pending_content.find("\n")
            pending_content = pending_content[newline_idx + 1:] if newline_idx >= 0 else ""
    except OSError:
        pending_content = ""

    if not pending_content.strip():
        logger.info("No pending content to summarize")
        state["last_collected_line"] = collector.current_line
        state["last_summary_time"] = datetime.now().isoformat()
        write_state(state_file, state)
        return

    # 5. 归档详情：.pending.md → details/{date}.md，然后清空并归零
    line_range = DetailArchiver.archive(pending_file, details_dir, session_id, today)
    _write_pending_turns(pending_file, 0, clear_content=True)

    # 6. 从归档后的 details 文件中扫描 [user]: 行号，构建精确的 turn_index
    turn_index = _build_turn_index_from_details(details_dir / f"{today}.md", line_range)

    # 用暂存内容（而非从 transcript 重读）作为摘要输入
    new_conversation = pending_content

    # 读取已有日度摘要（仅当内容可能是有效 JSON 时才传给 LLM）
    daily_dir = memory_dir / "daily"
    daily_path = daily_dir / f"{today}.md"
    existing_summary_json: str | None = None
    if daily_path.exists():
        try:
            raw = daily_path.read_text(encoding="utf-8")
            # 只有包含 JSON 结构标记的才作为增量更新的输入
            if '"sessions"' in raw or '"title"' in raw or '"summary"' in raw:
                existing_summary_json = raw
            else:
                logger.info("Existing daily file is not structured JSON, will regenerate")
        except OSError:
            pass

    # 截断对话
    new_conversation = truncate_text(new_conversation, TRANSCRIPT_TRUNCATE_CHARS)

    # 5. LLM 生成摘要
    codebuddy_model = model_override or "glm-5.1"
    ollama_model = model_override or "qwen2.5:14b"
    generator = SummaryGenerator(codebuddy_model, ollama_model, engine_choice)
    summary_data = generator.generate(
        existing_summary_json=existing_summary_json,
        new_conversation=new_conversation,
        start_line=last_collected,
        session_id=session_id,
        turns=collector.new_turns,
    )

    if summary_data is None:
        logger.warning("Summary generation failed")
        state["last_summary_time"] = datetime.now().isoformat()
        write_state(state_file, state)
        return

    # 6. 程序注入指针（不含日期标题，只生成本轮摘要块）
    if line_range is not None:
        final_md = PointerInjector.inject_section(summary_data, line_range, turn_index, today)
    else:
        raw_text = summary_data.get("_raw_text", "")
        if raw_text:
            final_md = PointerInjector.inject_fallback(raw_text, session_id, today)
        else:
            final_md = PointerInjector.inject_section(summary_data, None, turn_index, today)

    # 7. 追加写入 daily/{date}.md（保留已有摘要）
    daily_dir.mkdir(parents=True, exist_ok=True)
    try:
        # 文件不存在或为空时写日期标题
        if not daily_path.exists() or daily_path.stat().st_size == 0:
            with open(daily_path, "w", encoding="utf-8") as f:
                f.write(f"# {today}\n\n")
        with open(daily_path, "a", encoding="utf-8") as f:
            f.write(final_md)
        logger.info("Daily summary appended: %s", daily_path)
    except OSError as exc:
        logger.error("Failed to write daily summary: %s", exc)

    # 8. 提取经验信号并追加到 signal-buffer.md
    _append_signals_to_buffer(
        project_dir, summary_data, final_md, session_id, today, line_range,
    )

    # 9. 更新状态
    state.update({
        "last_collected_line": collector.current_line,
        "last_summary_turn": collector.new_turns,
        "last_summary_time": datetime.now().isoformat(),
        "engine": generator.actual_engine,
        "model": generator.actual_model,
        "session_id": session_id,
    })
    write_state(state_file, state)

    # 9. 清理旧状态文件
    cleanup_old_states(project_dir / "infra" / "hooks")

    logger.info("=== summarize done === engine=%s/%s", generator.actual_engine, generator.actual_model)


def handle_final(
    project_dir: Path,
    transcript_path: Path,
    session_id: str,
    state: dict,
    state_file: Path,
    engine_choice: str = "auto",
    model_override: str | None = None,
) -> None:
    """sessionEnd 收尾：强制 flush + 最终摘要。"""
    handle_stop(
        project_dir, transcript_path, session_id, state, state_file,
        force=True, engine_choice=engine_choice, model_override=model_override,
    )


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------


def main() -> None:
    """脚本入口：解析参数，启动 watchdog，按模式分发执行。"""
    parser = argparse.ArgumentParser(description="Session summarizer (memory hierarchy)")
    parser.add_argument("--mode", choices=["throttled", "final"], required=True)
    parser.add_argument("--engine", choices=["auto", "codebuddy", "ollama"], default="auto")
    parser.add_argument("--model", default=None, help="Override model name")
    parser.add_argument("--force", action="store_true", help="Skip throttle checks")
    parser.add_argument("--transcript", default=None, help="Manual transcript path")
    parser.add_argument("--project-dir", default=None, help="Manual project dir")
    args = parser.parse_args()

    # 1. Watchdog
    start_watchdog(SCRIPT_HARD_TIMEOUT)

    # 2. stdin → session_id
    stdin_data = read_stdin()
    session_id_full = resolve_session_id(stdin_data)
    session_id = sanitize_session_id(session_id_full)

    # 3. project_dir
    project_dir = resolve_project_dir(args.project_dir)

    # 4. logging
    global logger
    logger = setup_logging(project_dir, "summarize_session")
    logger.info("=== summarize_session start === mode=%s session=%s", args.mode, session_id)

    # 5. transcript path
    transcript_path = resolve_transcript_path(args.transcript, stdin_data)
    if transcript_path is None:
        logger.info("No transcript found, exiting")
        sys.exit(0)

    # 6. state file
    hooks_dir = project_dir / "infra" / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    state_file = hooks_dir / f".summary_state_{session_id}.json"

    # 7. 读取 state
    state = read_state(state_file, default=STATE_DEFAULT)

    # 9. 文件锁
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
        if args.mode == "throttled":
            handle_stop(
                project_dir, transcript_path, session_id, state, state_file,
                force=args.force, engine_choice=args.engine, model_override=args.model,
            )
        else:
            handle_final(
                project_dir, transcript_path, session_id, state, state_file,
                engine_choice=args.engine, model_override=args.model,
            )
    finally:
        if lock_file:
            release_lock(lock_file)
            lock_file.close()


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
