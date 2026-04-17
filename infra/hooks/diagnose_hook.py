#!/usr/bin/env python3
"""诊断脚本：把 Cursor 传给 hook 的 stdin / env 落盘，用于定位 transcript 问题。

一次性工具，定位完 transcript 传递机制后应移除或禁用。
"""

import json
import os
import sys
from datetime import datetime
from pathlib import Path


def main() -> None:
    project_dir = Path(
        os.environ.get("CURSOR_PROJECT_DIR")
        or os.environ.get("CODEBUDDY_PROJECT_DIR")
        or Path(__file__).resolve().parent.parent.parent
    )
    out_dir = project_dir / "infra" / "hooks"
    out_dir.mkdir(parents=True, exist_ok=True)

    try:
        raw = sys.stdin.buffer.read(65536)
        stdin_text = raw.decode("utf-8", errors="replace") if raw else ""
        try:
            stdin_json = json.loads(stdin_text) if stdin_text.strip() else None
        except Exception:
            stdin_json = None
    except Exception as e:
        stdin_text = f"<stdin read error: {e}>"
        stdin_json = None

    relevant_env = {
        k: v for k, v in os.environ.items()
        if k.startswith(("CURSOR_", "CLAUDE_", "CODEBUDDY_"))
    }

    record = {
        "ts": datetime.now().isoformat(),
        "argv": sys.argv,
        "stdin_raw": stdin_text,
        "stdin_parsed": stdin_json,
        "env": relevant_env,
        "cwd": os.getcwd(),
    }

    out_file = out_dir / "diagnose_hook.jsonl"
    with open(out_file, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
