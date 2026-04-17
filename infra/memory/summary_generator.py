#!/usr/bin/env python3
"""摘要生成器。

调用 LLM 生成/更新结构化日度摘要，输出包含话题分段和轮次范围的 JSON。
使用新的分层存储格式，摘要写入 docs/memory/daily/{date}.md。
"""

import json
import logging
from datetime import datetime

from infra.shared.llm_client import (
    DEFAULT_CODEBUDDY_MODEL,
    DEFAULT_OLLAMA_MODEL,
    OLLAMA_TRUNCATE_CHARS,
    call_codebuddy,
    call_ollama,
    extract_json_from_response,
    truncate_text,
)

logger: logging.Logger = logging.getLogger("memory.summary_generator")

# ---------------------------------------------------------------------------
# Prompt 模板 — 新格式（输出 JSON 而非 Markdown）
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = (
    "你是一个开发会话摘要助手。根据 AI IDE 的对话记录，生成结构化的会话摘要。\n"
    "规则：\n"
    "- 忽略工具调用细节、搜索结果、文件内容等噪音，聚焦于讨论和决策\n"
    "- 将对话按话题切分，每个话题标注涉及的轮次范围\n"
    "- 使用中文\n"
    "- 严格输出 JSON 格式\n"
    "- 额外判断本轮对话是否包含值得蒸馏的经验信号（用户纠正AI行为、工具/任务失败并找到原因、"
    "用户表达新偏好、出现重复性问题或反模式），在 experience_signals 字段中输出。"
    "如果没有经验信号，输出空数组"
)

FIRST_TIME_TEMPLATE = (
    "以下是 AI IDE 的对话记录。请生成结构化摘要。\n\n"
    "## 对话内容\n{conversation}\n\n"
    "请输出以下 JSON 格式：\n"
    "```json\n"
    '{{\n'
    '  "sessions": [{{\n'
    '    "id": "{session_id}",\n'
    '    "title": "用一句话概括主题",\n'
    '    "importance": "high|medium|low",\n'
    '    "turns": {turns},\n'
    '    "engine": "{engine}/{model}",\n'
    '    "summary": ["要点1", "要点2"],\n'
    '    "decisions": ["决策1", "决策2"],\n'
    '    "preferences": ["偏好1"],\n'
    '    "topics": [\n'
    '      {{"name": "话题名", "turn_range": [1, 10]}},\n'
    '      {{"name": "话题名", "turn_range": [11, 20]}}\n'
    '    ],\n'
    '    "experience_signals": [\n'
    '      {{"type": "correction|failure|preference|pattern", "brief": "简要描述"}}\n'
    '    ]\n'
    '  }}]\n'
    '}}\n'
    "```"
)

INCREMENTAL_TEMPLATE = (
    "以下是现有摘要和新增的对话内容。请更新摘要，保留已有信息，整合新内容。\n"
    "如果新对话修改了之前的决策，更新对应条目。\n\n"
    "## 现有摘要\n```json\n{existing_summary}\n```\n\n"
    "## 新增对话（第 {start_line} 行之后）\n{new_conversation}\n\n"
    "请输出更新后的完整 JSON（同格式）。"
)

# 兼容旧格式的校验
REQUIRED_HEADINGS_LEGACY = ["## 讨论要点", "## 做出的决策"]


class SummaryGenerator:
    """LLM 驱动的摘要生成器。"""

    def __init__(
        self,
        codebuddy_model: str = DEFAULT_CODEBUDDY_MODEL,
        ollama_model: str = DEFAULT_OLLAMA_MODEL,
        engine_choice: str = "auto",
    ) -> None:
        self.codebuddy_model = codebuddy_model
        self.ollama_model = ollama_model
        self.engine_choice = engine_choice
        self.actual_engine: str = "unknown"
        self.actual_model: str = codebuddy_model

    def generate(
        self,
        existing_summary_json: str | None,
        new_conversation: str,
        start_line: int = 0,
        session_id: str = "",
        turns: int = 0,
        max_context: int = 200000,
    ) -> dict | None:
        """生成或更新结构化摘要。

        Args:
            existing_summary_json: 已有摘要的 JSON 字符串，首次为 None。
            new_conversation: 本次新增的对话文本。
            start_line: 对话起始行号。
            session_id: 会话 ID。
            turns: 估计轮次。
            max_context: 最大上下文字符数。

        Returns:
            结构化摘要 dict（含 sessions 列表），失败返回 None。
        """
        messages = self._build_messages(
            existing_summary_json, new_conversation, start_line,
            session_id, turns, max_context,
        )

        result_text = self._call_llm(messages)
        if result_text is None:
            return None

        # 尝试解析为 JSON
        parsed = extract_json_from_response(result_text)
        if parsed is not None:
            # 标准格式：{sessions: [...]}
            if "sessions" in parsed:
                return parsed
            # LLM 返回了单个 session 对象（有 title/summary 等字段）
            if "title" in parsed or "summary" in parsed:
                # 补全缺失字段
                parsed.setdefault("id", session_id)
                parsed.setdefault("turns", turns)
                parsed.setdefault("engine", f"{self.actual_engine}/{self.actual_model}")
                parsed.setdefault("importance", "medium")
                parsed.setdefault("summary", [])
                parsed.setdefault("decisions", [])
                parsed.setdefault("preferences", [])
                parsed.setdefault("topics", [])
                parsed.setdefault("experience_signals", [])
                return {"sessions": [parsed]}

        # 兜底：LLM 返回的不是可解析 JSON
        logger.warning("LLM output is not structured JSON, attempting fallback")
        return {
            "sessions": [{
                "id": session_id,
                "title": "会话摘要",
                "importance": "medium",
                "turns": turns,
                "engine": f"{self.actual_engine}/{self.actual_model}",
                "summary": [result_text[:500]],
                "decisions": [],
                "preferences": [],
                "topics": [],
                "experience_signals": [],
            }],
            "_raw_text": result_text,
        }

    def _build_messages(
        self,
        existing_summary_json: str | None,
        new_conversation: str,
        start_line: int,
        session_id: str,
        turns: int,
        max_context: int,
    ) -> list[dict]:
        """构建 LLM messages。"""
        messages: list[dict] = [{"role": "system", "content": SYSTEM_PROMPT}]

        if existing_summary_json:
            user_content = INCREMENTAL_TEMPLATE.format(
                existing_summary=existing_summary_json,
                start_line=start_line,
                new_conversation=new_conversation,
            )
        else:
            user_content = FIRST_TIME_TEMPLATE.format(
                conversation=new_conversation,
                session_id=session_id,
                turns=turns,
                engine="CodeBuddy",
                model=self.codebuddy_model,
            )

        # 估算 token 量并压缩
        total_chars = len(SYSTEM_PROMPT) + len(user_content)
        estimated_tokens = total_chars // 2
        if estimated_tokens > int(max_context * 0.8) and existing_summary_json:
            existing_summary_json = truncate_text(existing_summary_json, max_context // 4)
            user_content = INCREMENTAL_TEMPLATE.format(
                existing_summary=existing_summary_json,
                start_line=start_line,
                new_conversation=new_conversation,
            )

        messages.append({"role": "user", "content": user_content})
        return messages

    def _call_llm(self, messages: list[dict]) -> str | None:
        """调用 LLM 引擎。"""
        result: str | None = None

        if self.engine_choice in ("auto", "codebuddy"):
            result = call_codebuddy(messages, self.codebuddy_model)
            if result:
                self.actual_engine = "CodeBuddy"
                self.actual_model = self.codebuddy_model

        if result is None and self.engine_choice in ("auto", "ollama"):
            # 为 Ollama 截断对话
            ollama_messages = []
            for m in messages:
                ollama_messages.append({
                    "role": m["role"],
                    "content": truncate_text(m["content"], OLLAMA_TRUNCATE_CHARS),
                })
            result = call_ollama(ollama_messages, self.ollama_model)
            if result:
                self.actual_engine = "Ollama"
                self.actual_model = self.ollama_model

        if result is None:
            logger.warning("All LLM engines failed")

        return result
