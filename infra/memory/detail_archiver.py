#!/usr/bin/env python3
"""详情归档器。

将临时详情文件的内容追加到 docs/memory/details/{date}.md，
记录起始/结束行号用于指针注入。
"""

import logging
from datetime import datetime
from pathlib import Path

logger: logging.Logger = logging.getLogger("memory.detail_archiver")


class DetailArchiver:
    """将临时详情文件归档到 details/ 目录。"""

    @staticmethod
    def archive(
        tmp_file: Path,
        details_dir: Path,
        session_id: str,
        date: str | None = None,
    ) -> tuple[int, int] | None:
        """将临时文件内容追加到 details/{date}.md。

        Args:
            tmp_file: 临时详情文件路径。
            details_dir: docs/memory/details/ 目录路径。
            session_id: 会话 ID（6 字符）。
            date: 日期字符串（YYYY-MM-DD），默认今天。

        Returns:
            (start_line, end_line) 在 details 文件中的行号范围，
            或 None（如果临时文件为空或不存在）。
        """
        if not tmp_file.exists():
            logger.debug("tmp_file not found: %s", tmp_file)
            return None

        try:
            content = tmp_file.read_text(encoding="utf-8")
        except OSError as exc:
            logger.error("Failed to read tmp_file %s: %s", tmp_file, exc)
            return None

        # 跳过 .pending.md 的头部标记行
        if content.startswith("<!-- pending_turns:"):
            newline_idx = content.find("\n")
            content = content[newline_idx + 1:] if newline_idx >= 0 else ""

        if not content.strip():
            logger.debug("tmp_file is empty: %s", tmp_file)
            return None

        if date is None:
            date = datetime.now().strftime("%Y-%m-%d")

        details_dir.mkdir(parents=True, exist_ok=True)
        details_path = details_dir / f"{date}.md"

        # 写入前的行数（1-based: 下一行就是 start_line）
        lines_before = DetailArchiver._count_lines(details_path)

        # 构建带元数据的内容块
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M")
        header = f"<!-- session: {session_id} | appended: {now_str} -->\n"

        try:
            with open(details_path, "a", encoding="utf-8") as f:
                # 如果文件非空，先加一个空行分隔
                if lines_before > 0:
                    f.write("\n")
                f.write(header)
                f.write(content)
                if not content.endswith("\n"):
                    f.write("\n")
        except OSError as exc:
            logger.error("Failed to append to details %s: %s", details_path, exc)
            return None

        # 写入后的行数
        lines_after = DetailArchiver._count_lines(details_path)
        # 1-based 行号：start = 写入前行数+1, end = 写入后行数
        start_line = lines_before + 1
        end_line = lines_after

        logger.info(
            "Archived details: %s L%d-L%d (session=%s)",
            details_path.name,
            start_line,
            end_line,
            session_id,
        )
        return (start_line, end_line)

    @staticmethod
    def _count_lines(path: Path) -> int:
        """统计文件行数。"""
        if not path.exists():
            return 0
        try:
            with open(path, "r", encoding="utf-8") as f:
                return sum(1 for _ in f)
        except OSError:
            return 0

    @staticmethod
    def cleanup_tmp(tmp_file: Path) -> None:
        """清理临时文件。"""
        try:
            if tmp_file.exists():
                tmp_file.unlink()
                logger.debug("Cleaned tmp file: %s", tmp_file)
        except OSError:
            pass
