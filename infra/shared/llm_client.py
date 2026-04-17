#!/usr/bin/env python3
"""LLM 调用客户端（共享模块）。

封装 CodeBuddy API 和 Ollama API 的调用逻辑，消除三个 hook 脚本中的重复代码。
引擎优先级: CodeBuddy → Ollama → None（静默跳过）。
零外部依赖，仅使用 Python 标准库。
"""

import glob
import json
import logging
import os
import re
import ssl
import sys
import urllib.request
from pathlib import Path

# ---------------------------------------------------------------------------
# 常量（各脚本可通过参数覆盖默认模型）
# ---------------------------------------------------------------------------

DEFAULT_CODEBUDDY_MODEL = "glm-5.1"
DEFAULT_OLLAMA_MODEL = "qwen2.5:14b"
CODEBUDDY_ENDPOINT = "https://copilot.tencent.com/v2/chat/completions"
OLLAMA_ENDPOINT = "http://127.0.0.1:11434/api/generate"
CODEBUDDY_TIMEOUT = 60
OLLAMA_TIMEOUT = 120
TRANSCRIPT_TRUNCATE_CHARS = 50000
OLLAMA_TRUNCATE_CHARS = 8000

logger: logging.Logger = logging.getLogger("shared.llm_client")


# ---------------------------------------------------------------------------
# 文本工具
# ---------------------------------------------------------------------------


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


# ---------------------------------------------------------------------------
# CodeBuddy 认证
# ---------------------------------------------------------------------------


def find_codebuddy_auth() -> dict | None:
    """通过 glob 搜索 CodeBuddy 的 JWT 认证文件。

    Returns:
        包含 token / user_id / enterprise_id / department_info 的 dict，
        找不到或解析失败时返回 None。
    """
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


# ---------------------------------------------------------------------------
# SSE 解析
# ---------------------------------------------------------------------------


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


# ---------------------------------------------------------------------------
# API 调用
# ---------------------------------------------------------------------------


def call_codebuddy(messages: list[dict], model: str) -> str | None:
    """调用 CodeBuddy API 生成内容。

    Args:
        messages: OpenAI 兼容的 messages 列表。
        model: 模型名称（如 deepseek-v3-2-volc-ioa）。

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


def call_llm(
    system_prompt: str,
    user_prompt: str,
    codebuddy_model: str = DEFAULT_CODEBUDDY_MODEL,
    ollama_model: str = DEFAULT_OLLAMA_MODEL,
) -> str | None:
    """调用 LLM 生成内容，CodeBuddy 优先，Ollama 兜底。

    Args:
        system_prompt: 系统提示。
        user_prompt: 用户提示。
        codebuddy_model: CodeBuddy 使用的模型名称。
        ollama_model: Ollama 使用的模型名称。

    Returns:
        LLM 响应文本，失败时返回 None。
    """
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]
    result = call_codebuddy(messages, codebuddy_model)
    if result is None:
        result = call_ollama(messages, ollama_model)
    return result
