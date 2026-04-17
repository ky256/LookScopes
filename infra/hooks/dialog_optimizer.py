#!/usr/bin/env python3
"""对话质量优化器 — UserPromptSubmit Hook 脚本。

在用户每次发送消息时触发，调用廉价内部模型分析对话上下文，
通过 additionalContext 向主 AI 注入优化信息（skills 提醒、rules 强化、
历史摘要、意图分析、输入纠错）。

设计原理:
  - 用廉价算力（DeepSeek-V3.2 内部部署，免费无限）优化昂贵算力（Opus 4.6）
  - 每轮对话都重新评估，不依赖主 AI 的"记忆"
  - 纯标准库实现，不依赖 requests 等第三方包
  - 通过 additionalContext 注入，不修改用户原始输入

用法:
  由 .codebuddy/settings.json 中的 UserPromptSubmit hook 调用。
  stdin: CodeBuddy hook JSON (含 prompt, transcript_path, session_id 等)
  stdout: hook 响应 JSON (含 additionalContext)
"""

import glob
import json
import logging
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from logging.handlers import RotatingFileHandler
from pathlib import Path

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

HOOKS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = HOOKS_DIR.parent.parent

# AI API
CODEBUDDY_ENDPOINT = "https://copilot.tencent.com/v2/chat/completions"
OPTIMIZER_MODEL = "deepseek-v3-2-volc-ioa"
REQUEST_TIMEOUT = 30  # 优化器需要快，不能让用户等太久
MAX_TOKENS = 2048

# 对话历史截断
MAX_TRANSCRIPT_CHARS = 8000  # 最多读取对话历史的字符数
MAX_TRANSCRIPT_LINES = 100   # 最多读取对话历史的行数

# 日志
LOG_FILE = HOOKS_DIR / "dialog_optimizer.log"

# ---------------------------------------------------------------------------
# 日志配置
# ---------------------------------------------------------------------------


def setup_logging() -> logging.Logger:
    log = logging.getLogger("dialog_optimizer")
    log.setLevel(logging.DEBUG)
    if not log.handlers:
        HOOKS_DIR.mkdir(parents=True, exist_ok=True)
        handler = RotatingFileHandler(
            str(LOG_FILE), maxBytes=256 * 1024, backupCount=1, encoding="utf-8"
        )
        handler.setFormatter(
            logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
        )
        log.addHandler(handler)
    return log


logger = setup_logging()

# ---------------------------------------------------------------------------
# CodeBuddy 认证（复用 codebuddy_proxy.py 的逻辑）
# ---------------------------------------------------------------------------


def find_codebuddy_auth() -> dict | None:
    """从本地 CodeBuddy 认证文件读取 JWT。"""
    local_app = os.environ.get("LOCALAPPDATA", "")
    if not local_app and sys.platform == "darwin":
        local_app = os.path.expanduser("~/Library/Application Support")
    if not local_app:
        return None

    exact_path = os.path.join(
        local_app, "CodeBuddyExtension", "Data", "Public",
        "auth", "Tencent-Cloud.coding-copilot.info",
    )
    if os.path.isfile(exact_path):
        return _parse_auth_file(exact_path)

    pattern = os.path.join(
        local_app, "CodeBuddyExtension", "**",
        "Tencent-Cloud.coding-copilot.info",
    )
    for match in glob.glob(pattern, recursive=True):
        result = _parse_auth_file(match)
        if result:
            return result
    return None


def _parse_auth_file(path: str) -> dict | None:
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
    except (OSError, json.JSONDecodeError, TypeError):
        return None


def _build_headers(auth: dict) -> dict:
    return {
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


# ---------------------------------------------------------------------------
# Skills / Rules 扫描
# ---------------------------------------------------------------------------


def scan_skills() -> list[dict]:
    """扫描 .codebuddy/skills/ 目录，提取 skill 名称和描述。"""
    skills_dir = PROJECT_DIR / ".codebuddy" / "skills"
    results = []
    if not skills_dir.exists():
        return results

    for skill_dir in sorted(skills_dir.iterdir()):
        if not skill_dir.is_dir():
            continue
        skill_file = skill_dir / "SKILL.md"
        if not skill_file.exists():
            continue
        try:
            content = skill_file.read_text(encoding="utf-8")
            # 提取第一个 # 标题作为描述
            desc = ""
            for line in content.split("\n"):
                line = line.strip()
                if line.startswith("# "):
                    desc = line[2:].strip()
                    break
            results.append({
                "name": skill_dir.name,
                "description": desc or skill_dir.name,
            })
        except Exception:
            results.append({"name": skill_dir.name, "description": skill_dir.name})
    return results


def scan_rules() -> list[dict]:
    """扫描 .codebuddy/rules/ 目录，提取 rule 名称、描述和 alwaysApply。"""
    rules_dir = PROJECT_DIR / ".codebuddy" / "rules"
    results = []
    if not rules_dir.exists():
        return results

    for rule_dir in sorted(rules_dir.iterdir()):
        if not rule_dir.is_dir():
            continue
        rule_file = rule_dir / "RULE.mdc"
        if not rule_file.exists():
            continue
        try:
            content = rule_file.read_text(encoding="utf-8")
            desc = ""
            always_apply = False

            # 解析 frontmatter
            if content.startswith("---"):
                parts = content.split("---", 2)
                if len(parts) >= 3:
                    frontmatter = parts[1]
                    for line in frontmatter.split("\n"):
                        line = line.strip()
                        if line.startswith("description:"):
                            desc = line[len("description:"):].strip()
                        elif line.startswith("alwaysApply:"):
                            val = line[len("alwaysApply:"):].strip().lower()
                            always_apply = val == "true"

            results.append({
                "name": rule_dir.name,
                "description": desc or rule_dir.name,
                "alwaysApply": always_apply,
            })
        except Exception:
            results.append({
                "name": rule_dir.name,
                "description": rule_dir.name,
                "alwaysApply": False,
            })
    return results


# ---------------------------------------------------------------------------
# 对话历史读取
# ---------------------------------------------------------------------------


def read_transcript(transcript_path: str) -> str:
    """读取对话历史文件，截断到合理长度。"""
    if not transcript_path or not os.path.isfile(transcript_path):
        return ""

    try:
        with open(transcript_path, "r", encoding="utf-8") as f:
            lines = f.readlines()

        # 取最后 N 行（最近的对话更重要）
        recent_lines = lines[-MAX_TRANSCRIPT_LINES:]
        text = "".join(recent_lines)

        # 截断到最大字符数
        if len(text) > MAX_TRANSCRIPT_CHARS:
            text = text[-MAX_TRANSCRIPT_CHARS:]
            # 找到第一个完整行的开始
            newline_pos = text.find("\n")
            if newline_pos > 0:
                text = text[newline_pos + 1:]

        return text
    except Exception as exc:
        logger.warning("Failed to read transcript: %s", exc)
        return ""


# ---------------------------------------------------------------------------
# Prompt 构建
# ---------------------------------------------------------------------------


def load_spec() -> str:
    """加载 dialog_optimizer_spec.md 作为 system prompt。"""
    spec_path = HOOKS_DIR / "dialog_optimizer_spec.md"
    try:
        return spec_path.read_text(encoding="utf-8")
    except Exception:
        logger.error("Failed to load spec file: %s", spec_path)
        return "你是对话质量优化器，输出 JSON 格式的优化建议。"


def build_user_message(
    prompt: str,
    transcript: str,
    skills: list[dict],
    rules: list[dict],
) -> str:
    """构建发给优化器 AI 的 user message。"""
    parts = []

    # 对话历史
    if transcript:
        parts.append("## 对话历史（最近部分）\n")
        parts.append(transcript)
        parts.append("\n")

    # 用户当前消息
    parts.append("## 用户当前消息\n")
    parts.append(prompt)
    parts.append("\n")

    # Skills 列表
    if skills:
        parts.append("## 可用 Skills\n")
        for s in skills:
            parts.append(f"- **{s['name']}**: {s['description']}")
        parts.append("\n")

    # Rules 列表
    if rules:
        parts.append("## 可用 Rules\n")
        for r in rules:
            tag = "始终生效" if r.get("alwaysApply") else "按需激活"
            parts.append(f"- **{r['name']}** [{tag}]: {r['description']}")
        parts.append("\n")

    parts.append("请根据以上信息，输出 JSON 格式的优化建议。")
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# AI 调用（非流式，聚合响应）
# ---------------------------------------------------------------------------


def call_optimizer_ai(system_prompt: str, user_message: str) -> dict | None:
    """调用 CodeBuddy API（非流式聚合），返回解析后的 JSON。"""
    auth = find_codebuddy_auth()
    if not auth:
        logger.error("CodeBuddy auth not available")
        return None

    headers = _build_headers(auth)
    payload = {
        "model": OPTIMIZER_MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_message},
        ],
        "max_tokens": MAX_TOKENS,
        "stream": True,  # CodeBuddy 只支持流式
        "temperature": 0.3,  # 低温度，确保输出稳定
    }

    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        CODEBUDDY_ENDPOINT, data=body, headers=headers, method="POST",
    )

    try:
        ctx = ssl.create_default_context()
        resp = urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT, context=ctx)
    except urllib.error.HTTPError as exc:
        logger.error("API HTTP error %d: %s", exc.code, exc.reason)
        return None
    except Exception as exc:
        logger.error("API request failed: %s", exc)
        return None

    # 聚合 SSE 流
    raw = resp.read().decode("utf-8", errors="replace")
    content_parts: list[str] = []

    for line in raw.split("\n"):
        line = line.strip()
        if not line.startswith("data: ") or line == "data: [DONE]":
            continue
        try:
            chunk = json.loads(line[6:])
        except json.JSONDecodeError:
            continue
        for choice in chunk.get("choices", []):
            delta = choice.get("delta", {})
            if delta.get("content"):
                content_parts.append(delta["content"])

    full_text = "".join(content_parts).strip()
    if not full_text:
        logger.warning("Empty response from optimizer AI")
        return None

    # 尝试解析 JSON（可能被 markdown 代码块包裹）
    return _extract_json(full_text)


def _extract_json(text: str) -> dict | None:
    """从 AI 输出中提取 JSON，兼容 markdown 代码块包裹。"""
    # 直接尝试
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass

    # 去掉 markdown 代码块
    import re
    match = re.search(r"```(?:json)?\s*\n?(.*?)\n?\s*```", text, re.DOTALL)
    if match:
        try:
            return json.loads(match.group(1))
        except json.JSONDecodeError:
            pass

    # 找第一个 { 到最后一个 }
    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1 and end > start:
        try:
            return json.loads(text[start:end + 1])
        except json.JSONDecodeError:
            pass

    logger.warning("Failed to extract JSON from AI output: %s", text[:200])
    return None


# ---------------------------------------------------------------------------
# 构建 additionalContext
# ---------------------------------------------------------------------------


def format_additional_context(result: dict) -> str:
    """将优化器输出格式化为 additionalContext 字符串。

    当所有字段都为空时返回空字符串，避免无意义注入。
    """
    sections: list[str] = []

    # 意图分析
    intent = result.get("intent_analysis", "")
    if intent:
        sections.append(f"**用户意图**: {intent}")

    # 结构化需求
    structured = result.get("structured_request", "")
    if structured:
        sections.append(f"**结构化需求**: {structured}")

    # 上下文补全
    context = result.get("optimized_context", "")
    if context:
        sections.append(f"**上下文补全**: {context}")

    # 历史摘要
    history = result.get("history_summary", "")
    if history:
        sections.append(f"**相关历史**: {history}")

    # 输入纠错
    corrections = result.get("input_corrections", [])
    if corrections:
        lines = ["**输入纠错**:"]
        for c in corrections:
            lines.append(f"- 「{c.get('original', '')}」→「{c.get('corrected', '')}」({c.get('reason', '')})")
        sections.append("\n".join(lines))

    # Skills 激活
    skills = result.get("active_skills", [])
    if skills:
        lines = ["**建议激活 Skills**:"]
        for s in skills:
            lines.append(f"- **{s.get('name', '')}**: {s.get('reason', '')}")
        sections.append("\n".join(lines))

    # Rules 强化
    rules = result.get("active_rules", [])
    if rules:
        lines = ["**强化提醒 Rules**:"]
        for r in rules:
            lines.append(f"- **{r.get('name', '')}**: {r.get('reason', '')}")
        sections.append("\n".join(lines))

    # 无实质内容时返回空字符串
    if not sections:
        return ""

    return "## 🔍 对话质量优化器注入\n\n" + "\n\n".join(sections)


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------


def read_stdin_safe() -> dict:
    """从 stdin 安全读取 JSON。"""
    try:
        if sys.stdin.isatty():
            return {}
        raw = sys.stdin.read()
        if not raw.strip():
            return {}
        return json.loads(raw)
    except Exception as exc:
        logger.warning("stdin read/parse failed: %s", exc)
        return {}


def main() -> None:
    start_time = time.time()

    # 读取 hook 输入
    stdin_data = read_stdin_safe()
    prompt = stdin_data.get("prompt", "")
    transcript_path = stdin_data.get("transcript_path", "")
    session_id = stdin_data.get("session_id", "")

    logger.info(
        "=== Dialog Optimizer start: session=%s prompt_len=%d ===",
        session_id[:8] if session_id else "?",
        len(prompt),
    )

    # 如果没有用户消息，直接放行
    if not prompt.strip():
        logger.info("Empty prompt, passthrough")
        json.dump({"continue": True}, sys.stdout)
        return

    # 极短消息（"好"、"继续"、"是"等）也走优化，因为需要上下文关联
    # 但如果连对话历史都没有，就没必要优化了
    transcript = read_transcript(transcript_path)
    if not transcript and len(prompt) < 10:
        logger.info("Short prompt without history, passthrough")
        json.dump({"continue": True}, sys.stdout)
        return

    # 扫描 skills 和 rules
    skills = scan_skills()
    rules = scan_rules()
    logger.info("Scanned %d skills, %d rules", len(skills), len(rules))

    # 构建 prompt
    system_prompt = load_spec()
    user_message = build_user_message(prompt, transcript, skills, rules)

    # 调用优化器 AI
    result = call_optimizer_ai(system_prompt, user_message)

    elapsed = time.time() - start_time

    if result is None:
        logger.warning("Optimizer AI returned no result (%.1fs), passthrough", elapsed)
        json.dump({"continue": True}, sys.stdout)
        return

    # 格式化注入内容
    additional_context = format_additional_context(result)
    logger.info(
        "Optimization done (%.1fs): skills=%s rules=%s corrections=%d",
        elapsed,
        [s.get("name") for s in result.get("active_skills", [])],
        [r.get("name") for r in result.get("active_rules", [])],
        len(result.get("input_corrections", [])),
    )

    # 无实质内容时走 passthrough
    if not additional_context:
        logger.info("No actionable optimization, passthrough")
        json.dump({"continue": True}, sys.stdout)
        return

    # 输出 hook 响应
    output = {
        "continue": True,
        "hookSpecificOutput": {
            "hookEventName": "UserPromptSubmit",
            "additionalContext": additional_context,
        },
    }
    json.dump(output, sys.stdout, ensure_ascii=False)


if __name__ == "__main__":
    main()
