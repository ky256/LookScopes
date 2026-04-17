#!/usr/bin/env python3
"""MetaAgent Memory MCP Server — 记忆查询服务。

零外部依赖，纯 Python 标准库实现 MCP 协议（JSON-RPC over stdin/stdout）。
提供 drill_detail / read_daily / list_days 三个工具，供 AI 按需钻取历史对话。

用法：
  作为 MCP server 由 IDE 自动启动，无需手动运行。
"""

import json
import os
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# 路径
# ---------------------------------------------------------------------------

PROJECT_DIR = Path(__file__).resolve().parent.parent.parent
MEMORY_DIR = PROJECT_DIR / "docs" / "memory"


# ---------------------------------------------------------------------------
# 工具实现
# ---------------------------------------------------------------------------


def drill_detail(pointer: str) -> str:
    """按指针读取详情片段。

    Args:
        pointer: 格式如 "details/2026-04-11.md#L100-L200"

    Returns:
        指定行范围的文本内容。
    """
    try:
        # 解析 pointer: "details/2026-04-11.md#L100-L200"
        if "#L" not in pointer:
            return f"错误：指针格式无效，期望 'details/xxx.md#L起始-L结束'，收到 '{pointer}'"

        file_part, fragment = pointer.split("#", 1)
        file_path = MEMORY_DIR / file_part

        if not file_path.exists():
            return f"错误：文件不存在 {file_path}"

        # 解析行号 "L100-L200"
        parts = fragment.replace("L", "").split("-")
        if len(parts) == 2:
            start_line = int(parts[0])
            end_line = int(parts[1])
        elif len(parts) == 1:
            start_line = int(parts[0])
            end_line = start_line
        else:
            return f"错误：行号格式无效 '{fragment}'"

        # 读取指定行范围（1-based）
        lines = []
        with open(file_path, "r", encoding="utf-8") as f:
            for line_num, line in enumerate(f, start=1):
                if line_num < start_line:
                    continue
                if line_num > end_line:
                    break
                lines.append(line.rstrip("\n"))

        if not lines:
            return f"L{start_line}-L{end_line} 范围内无内容"

        return "\n".join(lines)

    except (ValueError, IndexError) as exc:
        return f"错误：解析指针失败 — {exc}"


def read_daily(date: str = "") -> str:
    """读取指定日期的摘要文件。

    Args:
        date: 日期字符串（YYYY-MM-DD），为空时读取最新的。

    Returns:
        摘要文件全文。
    """
    daily_dir = MEMORY_DIR / "daily"
    if not daily_dir.exists():
        return "错误：daily/ 目录不存在"

    if date:
        target = daily_dir / f"{date}.md"
        if not target.exists():
            return f"错误：{date} 的摘要不存在"
        return target.read_text(encoding="utf-8")

    # 读取最新的
    files = sorted(daily_dir.glob("*.md"), reverse=True)
    if not files:
        return "暂无摘要文件"
    return files[0].read_text(encoding="utf-8")


def list_days() -> str:
    """列出所有可用的日期及文件大小。

    Returns:
        可用日期列表。
    """
    daily_dir = MEMORY_DIR / "daily"
    details_dir = MEMORY_DIR / "details"

    result = []
    if daily_dir.exists():
        for f in sorted(daily_dir.glob("*.md")):
            size_kb = f.stat().st_size / 1024
            result.append(f"[摘要] {f.stem} ({size_kb:.1f}KB)")

    if details_dir.exists():
        for f in sorted(details_dir.glob("*.md")):
            if f.name.startswith("."):
                continue
            size_kb = f.stat().st_size / 1024
            result.append(f"[详情] {f.stem} ({size_kb:.1f}KB)")

    # pending 状态
    pending = details_dir / ".pending.md" if details_dir.exists() else None
    if pending and pending.exists():
        first_line = ""
        try:
            with open(pending, "r", encoding="utf-8") as pf:
                first_line = pf.readline().strip()
        except OSError:
            pass
        if "pending_turns:" in first_line:
            turns = first_line.split(":")[1].rstrip(" ->").strip()
            result.append(f"[暂存] {turns} 轮未归档")

    return "\n".join(result) if result else "暂无记忆数据"


# ---------------------------------------------------------------------------
# 工具注册表
# ---------------------------------------------------------------------------

TOOLS = {
    "drill_detail": {
        "description": "按指针读取详情片段。输入格式: details/2026-04-11.md#L100-L200",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pointer": {
                    "type": "string",
                    "description": "详情指针，格式如 details/2026-04-11.md#L100-L200"
                }
            },
            "required": ["pointer"]
        },
        "handler": lambda args: drill_detail(args["pointer"]),
    },
    "read_daily": {
        "description": "读取指定日期的摘要。不传 date 则读取最新的。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "date": {
                    "type": "string",
                    "description": "日期（YYYY-MM-DD），可选"
                }
            },
            "required": []
        },
        "handler": lambda args: read_daily(args.get("date", "")),
    },
    "list_days": {
        "description": "列出所有可用的记忆日期和文件状态。",
        "inputSchema": {
            "type": "object",
            "properties": {},
            "required": []
        },
        "handler": lambda args: list_days(),
    },
}


# ---------------------------------------------------------------------------
# MCP 协议实现（JSON-RPC over stdin/stdout）
# ---------------------------------------------------------------------------


def send_response(id: int | str | None, result: dict) -> None:
    """发送 JSON-RPC 响应。"""
    msg = {"jsonrpc": "2.0", "id": id, "result": result}
    raw = json.dumps(msg, ensure_ascii=False)
    sys.stdout.write(raw + "\n")
    sys.stdout.flush()


def send_error(id: int | str | None, code: int, message: str) -> None:
    """发送 JSON-RPC 错误响应。"""
    msg = {"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}}
    raw = json.dumps(msg, ensure_ascii=False)
    sys.stdout.write(raw + "\n")
    sys.stdout.flush()


def handle_request(req: dict) -> None:
    """处理单个 JSON-RPC 请求。"""
    method = req.get("method", "")
    id = req.get("id")
    params = req.get("params", {})

    if method == "initialize":
        send_response(id, {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {
                "name": "memory-mcp",
                "version": "1.0.0",
            },
        })

    elif method == "notifications/initialized":
        pass  # 通知，无需响应

    elif method == "tools/list":
        tools_list = []
        for name, tool in TOOLS.items():
            tools_list.append({
                "name": name,
                "description": tool["description"],
                "inputSchema": tool["inputSchema"],
            })
        send_response(id, {"tools": tools_list})

    elif method == "tools/call":
        tool_name = params.get("name", "")
        arguments = params.get("arguments", {})

        if tool_name not in TOOLS:
            send_error(id, -32601, f"Unknown tool: {tool_name}")
            return

        try:
            result_text = TOOLS[tool_name]["handler"](arguments)
            send_response(id, {
                "content": [{"type": "text", "text": result_text}],
            })
        except Exception as exc:
            send_response(id, {
                "content": [{"type": "text", "text": f"工具执行失败: {exc}"}],
                "isError": True,
            })

    elif method == "ping":
        send_response(id, {})

    else:
        if id is not None:
            send_error(id, -32601, f"Method not found: {method}")


def main() -> None:
    """主循环：读取 stdin JSON-RPC 请求，处理并响应。"""
    # Windows 下强制 stdin/stdout 使用 UTF-8
    if sys.platform == "win32":
        sys.stdin = open(sys.stdin.fileno(), "r", encoding="utf-8", errors="replace")
        sys.stdout = open(sys.stdout.fileno(), "w", encoding="utf-8", errors="replace")

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            handle_request(req)
        except json.JSONDecodeError:
            send_error(None, -32700, "Parse error")
        except Exception as exc:
            send_error(None, -32603, f"Internal error: {exc}")


if __name__ == "__main__":
    main()
