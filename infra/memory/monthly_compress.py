#!/usr/bin/env python3
"""月度压缩器。

读取当月所有 daily/{month}-*.md 文件，调用 LLM 压缩为月度摘要，
写入 monthly/{month}.md。daily/ 文件保留不删除（供钻取）。
"""

import logging
from datetime import datetime
from pathlib import Path

from infra.shared.llm_client import call_llm, truncate_text, TRANSCRIPT_TRUNCATE_CHARS

logger: logging.Logger = logging.getLogger("memory.monthly_compress")

COMPRESS_SYSTEM_PROMPT = (
    "你是 MetaAgent 月度回顾助手。根据一个月内的所有日度摘要，"
    "生成精炼的月度总结。\n"
    "要求：\n"
    "- 提炼核心成果和关键决策\n"
    "- 统计会话次数、主要话题\n"
    "- 标注重要的架构变更\n"
    "- 使用中文\n"
    "- 输出 Markdown 格式"
)

COMPRESS_USER_TEMPLATE = (
    "以下是 {month} 月的所有日度摘要。请压缩为月度总结。\n\n"
    "{daily_summaries}\n\n"
    "请按以下格式输出：\n\n"
    "# {month} 月度总结\n\n"
    "## 核心成果\n- ...\n\n"
    "## 关键决策\n- ...\n\n"
    "## 统计\n- 会话次数: N\n- 主要话题: ...\n\n"
    "## 架构变更\n- ...\n"
)


def compress_month(
    memory_dir: Path,
    month: str | None = None,
) -> bool:
    """将指定月份的日度摘要压缩为月度摘要。

    Args:
        memory_dir: docs/memory/ 目录路径。
        month: 月份字符串（YYYY-MM），默认上个月。

    Returns:
        True 表示压缩成功。
    """
    if month is None:
        now = datetime.now()
        if now.month == 1:
            month = f"{now.year - 1}-12"
        else:
            month = f"{now.year}-{now.month - 1:02d}"

    daily_dir = memory_dir / "daily"
    monthly_dir = memory_dir / "monthly"

    if not daily_dir.exists():
        logger.info("No daily directory found")
        return False

    # 收集当月所有日度摘要
    daily_files = sorted(daily_dir.glob(f"{month}-*.md"))
    if not daily_files:
        logger.info("No daily summaries found for %s", month)
        return False

    # 读取所有日度摘要
    daily_contents: list[str] = []
    for f in daily_files:
        try:
            content = f.read_text(encoding="utf-8")
            daily_contents.append(f"--- {f.stem} ---\n{content}")
        except OSError:
            continue

    if not daily_contents:
        logger.info("All daily files empty for %s", month)
        return False

    combined = "\n\n".join(daily_contents)
    combined = truncate_text(combined, TRANSCRIPT_TRUNCATE_CHARS)

    user_prompt = COMPRESS_USER_TEMPLATE.format(
        month=month,
        daily_summaries=combined,
    )

    result = call_llm(COMPRESS_SYSTEM_PROMPT, user_prompt)
    if result is None:
        logger.warning("LLM compression failed for %s", month)
        return False

    # 写入月度摘要
    monthly_dir.mkdir(parents=True, exist_ok=True)
    monthly_path = monthly_dir / f"{month}.md"

    try:
        with open(monthly_path, "w", encoding="utf-8") as f:
            f.write(result)
        logger.info("Monthly summary written: %s (%d daily files)", monthly_path, len(daily_files))
        return True
    except OSError as exc:
        logger.error("Failed to write monthly summary: %s", exc)
        return False
