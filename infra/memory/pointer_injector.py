#!/usr/bin/env python3
"""指针注入器。

将 LLM 生成的结构化摘要中的话题轮次范围映射为 details/ 文件的行号范围，
注入详情索引表，输出最终的日度摘要 Markdown。

核心原则：
- LLM 负责语义：话题切分、轮次范围标注
- 程序负责位置：行号映射、文件路径、指针注入
- 指针指向自控文件：details/ 是项目仓库内的文件，不依赖 IDE 目录
"""

import logging
from datetime import datetime

logger: logging.Logger = logging.getLogger("memory.pointer_injector")


class PointerInjector:
    """将摘要 JSON 转换为带指针的 Markdown。"""

    @staticmethod
    def inject(
        summary_data: dict,
        line_range: tuple[int, int] | None,
        turn_index: dict[int, int],
        date: str | None = None,
    ) -> str:
        """将结构化摘要注入详情指针后输出完整 Markdown（含日期标题）。

        Args:
            summary_data: LLM 生成的结构化摘要 dict（含 sessions 列表）。
            line_range: 本次归档在 details 文件中的 (start_line, end_line)。
            turn_index: {轮次号 → .pending.md 文件行号} 映射。
            date: 日期字符串，默认今天。

        Returns:
            完整的日度摘要 Markdown 文本。
        """
        if date is None:
            date = datetime.now().strftime("%Y-%m-%d")

        header = f"# {date}\n\n"
        body = PointerInjector._render_sessions(summary_data, line_range, turn_index, date)
        return header + body

    @staticmethod
    def inject_section(
        summary_data: dict,
        line_range: tuple[int, int] | None,
        turn_index: dict[int, int],
        date: str | None = None,
    ) -> str:
        """生成单轮摘要块（不含日期标题），用于追加到已有 daily 文件。

        Args:
            summary_data: LLM 生成的结构化摘要 dict（含 sessions 列表）。
            line_range: 本次归档在 details 文件中的 (start_line, end_line)。
            turn_index: {轮次号 → .pending.md 文件行号} 映射。
            date: 日期字符串，默认今天。

        Returns:
            单轮摘要 Markdown 文本。
        """
        if date is None:
            date = datetime.now().strftime("%Y-%m-%d")

        return PointerInjector._render_sessions(summary_data, line_range, turn_index, date)

    @staticmethod
    def _render_sessions(
        summary_data: dict,
        line_range: tuple[int, int] | None,
        turn_index: dict[int, int],
        date: str,
    ) -> str:
        """渲染所有 session 块的 Markdown。"""
        lines: list[str] = []

        sessions = summary_data.get("sessions", [])
        for session in sessions:
            session_id = session.get("id", "unknown")
            title = session.get("title", "未命名会话")
            importance = session.get("importance", "medium")
            turns = session.get("turns", 0)
            engine = session.get("engine", "unknown")

            lines.append(f"## 会话 {session_id} — {title}")
            lines.append(f"- 重要度: {importance} | 轮次: {turns} | 引擎: {engine}\n")

            # 摘要
            summary_items = session.get("summary", [])
            if isinstance(summary_items, str):
                summary_items = [summary_items]
            if summary_items:
                lines.append("### 摘要")
                for item in summary_items:
                    lines.append(f"- {item}")
                lines.append("")

            # 决策
            decisions = session.get("decisions", [])
            if isinstance(decisions, str):
                decisions = [decisions]
            if decisions:
                lines.append("### 决策")
                for item in decisions:
                    lines.append(f"- {item}")
                lines.append("")

            # 用户偏好
            preferences = session.get("preferences", [])
            if isinstance(preferences, str):
                preferences = [preferences]
            if preferences:
                lines.append("### 用户偏好")
                for item in preferences:
                    lines.append(f"- {item}")
                lines.append("")

            # 详情索引表
            topics = session.get("topics", [])
            if topics and line_range is not None:
                lines.append("### 详情索引")
                lines.append("| 话题 | 详情位置 | 轮次范围 |")
                lines.append("|------|---------|---------|")

                # 先计算所有话题的 start 行号
                topic_starts: list[int] = []
                for topic in topics:
                    tr = topic.get("turn_range", [])
                    if len(tr) == 2:
                        offset = line_range[0]
                        start = PointerInjector._map_turn_to_line(
                            tr[0], turn_index, 0
                        ) + offset
                        start = max(line_range[0], min(start, line_range[1]))
                        topic_starts.append(start)
                    else:
                        topic_starts.append(line_range[0])

                for i, topic in enumerate(topics):
                    topic_name = topic.get("name", "")
                    turn_range_val = topic.get("turn_range", [])

                    detail_start = topic_starts[i]
                    # end = 下一个话题 start - 1，最后一个话题 end = line_range[1]
                    if i + 1 < len(topic_starts):
                        detail_end = topic_starts[i + 1] - 1
                    else:
                        detail_end = line_range[1]
                    detail_end = max(detail_start, detail_end)

                    if len(turn_range_val) == 2:
                        turns_str = f"{turn_range_val[0]}-{turn_range_val[1]}"
                    else:
                        turns_str = "全部"

                    ref = f"details/{date}.md#L{detail_start}-L{detail_end}"
                    lines.append(f"| {topic_name} | {ref} | {turns_str} |")

                lines.append("")

            lines.append("---\n")

        return "\n".join(lines)

    @staticmethod
    def _map_turn_to_line(
        turn: int,
        turn_index: dict[int, int],
        fallback_line: int,
    ) -> int:
        """将轮次号映射为 details 文件行号。

        Args:
            turn: 轮次号。
            turn_index: 轮次→行号映射表。
            fallback_line: 无法映射时的兜底行号。

        Returns:
            对应的行号。
        """
        if turn in turn_index:
            return turn_index[turn]

        # 找最接近的轮次
        if not turn_index:
            return fallback_line

        closest = min(turn_index.keys(), key=lambda t: abs(t - turn))
        return turn_index[closest]

    @staticmethod
    def inject_fallback(
        raw_text: str,
        session_id: str,
        date: str | None = None,
    ) -> str:
        """当 LLM 输出非结构化文本时的兜底处理。

        直接将原始文本作为摘要内容输出，不注入指针。

        Args:
            raw_text: LLM 原始输出文本。
            session_id: 会话 ID。
            date: 日期字符串。

        Returns:
            Markdown 格式的摘要文本。
        """
        if date is None:
            date = datetime.now().strftime("%Y-%m-%d")

        return (
            f"# {date}\n\n"
            f"## 会话 {session_id}\n\n"
            f"{raw_text}\n\n"
            "---\n"
        )
