#!/usr/bin/env python3
"""详情增量收集器。

每次 stop hook 触发时，从 transcript 读取自上次收集以来的增量内容，
格式化为结构化文本（标注轮次、角色），追加到临时文件。

零 LLM 调用，耗时 < 100ms，只做数据搬运。
"""

import json
import logging
from datetime import datetime
from pathlib import Path

from infra.shared.transcript_parser import (
    extract_content,
    load_codebuddy_messages,
)

logger: logging.Logger = logging.getLogger("memory.detail_collector")


class DetailCollector:
    """从 transcript 增量收集对话详情。

    Attributes:
        transcript_path: transcript 文件路径。
        new_turns: 本次收集到的新增 user 消息数。
        current_line: 收集后的当前行号/消息索引。
        turn_index: 轮次 → 临时文件行号的映射。
    """

    def __init__(self, transcript_path: Path) -> None:
        self.transcript_path = transcript_path
        self.new_turns: int = 0
        self.current_line: int = 0
        self.turn_index: dict[int, int] = {}

    def collect(
        self,
        last_collected_line: int,
        tmp_file: Path,
    ) -> dict[int, int]:
        """从上次收集位置开始，增量提取对话并追加到临时文件。

        Args:
            last_collected_line: 上次收集到的行号/消息索引。
            tmp_file: 临时详情文件路径。

        Returns:
            turn_index: {轮次号 → 临时文件中的起始行号} 映射。
        """
        tmp_file.parent.mkdir(parents=True, exist_ok=True)

        # 获取临时文件当前行数作为起始偏移
        file_line_offset = self._count_existing_lines(tmp_file)

        # 解析增量内容
        records = self._parse_incremental(last_collected_line)
        if not records:
            logger.debug("No new records since line %d", last_collected_line)
            self.current_line = last_collected_line
            return self.turn_index

        # 追加到临时文件
        user_turn = 0
        lines_written = 0
        try:
            with open(tmp_file, "a", encoding="utf-8") as f:
                for rec in records:
                    role = rec["role"]
                    content = rec["content"]
                    line_idx = rec["line"]

                    if role == "user":
                        user_turn += 1
                        self.new_turns += 1
                        # 记录轮次起始行号
                        self.turn_index[user_turn + self._count_existing_turns(last_collected_line)] = (
                            file_line_offset + lines_written
                        )

                    formatted = f"[{role}]: {content}\n"
                    f.write(formatted)
                    lines_written += 1
                    self.current_line = line_idx + 1

        except OSError as exc:
            logger.error("Failed to write to tmp_file %s: %s", tmp_file, exc)
            return self.turn_index

        logger.info(
            "Collected %d records (%d user turns) from line %d to %d",
            len(records),
            self.new_turns,
            last_collected_line,
            self.current_line,
        )
        return self.turn_index

    def _parse_incremental(self, since_line: int) -> list[dict]:
        """从指定位置开始解析 transcript 增量内容。

        Returns:
            结构化记录列表 [{line, role, content}, ...]
        """
        cb_msgs = load_codebuddy_messages(self.transcript_path)
        if cb_msgs is not None:
            records = []
            for idx, obj in enumerate(cb_msgs):
                if idx < since_line:
                    continue
                if not isinstance(obj, dict):
                    continue
                role = obj.get("role", "unknown")
                content = extract_content(obj)
                if content:
                    records.append({"line": idx, "role": role, "content": content})
            return records

        records = []
        try:
            with open(self.transcript_path, "r", encoding="utf-8") as f:
                for idx, raw_line in enumerate(f):
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
                        records.append({"line": idx, "role": role, "content": content})
        except OSError as exc:
            logger.warning("parse incremental failed: %s", exc)
        return records

    def _count_existing_lines(self, path: Path) -> int:
        """统计文件现有行数。"""
        if not path.exists():
            return 0
        try:
            with open(path, "r", encoding="utf-8") as f:
                return sum(1 for _ in f)
        except OSError:
            return 0

    def _count_existing_turns(self, last_line: int) -> int:
        """粗估已有的 user 轮次数（用于轮次编号连续性）。"""
        cb_msgs = load_codebuddy_messages(self.transcript_path)
        if cb_msgs is not None:
            return sum(
                1 for m in cb_msgs[:last_line]
                if isinstance(m, dict) and m.get("role") == "user"
            )

        turns = 0
        try:
            with open(self.transcript_path, "r", encoding="utf-8") as f:
                for idx, line in enumerate(f):
                    if idx >= last_line:
                        break
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except (json.JSONDecodeError, TypeError):
                        continue
                    if isinstance(obj, dict) and obj.get("role") == "user":
                        turns += 1
        except OSError:
            pass
        return turns
