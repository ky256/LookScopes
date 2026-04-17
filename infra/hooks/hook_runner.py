#!/usr/bin/env python3
"""跨平台 Hook Runner — 所有 CodeBuddy/Cursor hook 的统一入口。

由 .codebuddy/settings.json 中的 hook 配置直接调用。
自动从 stdin 读取 hook JSON，根据 --event 和 --stage 参数调度对应脚本。

设计原理:
  - CodeBuddy 仅支持 stop 事件，不支持 SessionEnd
  - stop 事件通过 loop_count 区分：0 = 首次停止 = throttled，>0 = 后续
  - 当 status == "completed" 且 loop_count == 0 时额外触发 "final" 阶段脚本
  - 所有路径使用 Path(__file__) 推导，不依赖环境变量

兼容:
  - macOS: python3 / bash
  - Windows: py -3 / cmd / powershell / git bash
  - Cursor: stop + sessionEnd 事件
  - CodeBuddy: stop 事件（合并 throttled + final 逻辑）

用法:
  python hook_runner.py --event stop --stage throttled
  python hook_runner.py --event stop --stage final
  python hook_runner.py --event stop --stage auto  (根据 stdin JSON 自动判断)
"""

import argparse
import json
import logging
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

HOOKS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = HOOKS_DIR.parent.parent

# 各阶段要执行的脚本及参数
# throttled: 每次 Stop 轻量执行（摘要节流 + 信号积累）
# final: SessionEnd 或强制触发时执行（强制 flush 摘要 + 信号积累）
# 注意：蒸馏不再由 hook 自动触发，改为用户主动执行
STAGE_SCRIPTS: dict[str, list[list[str]]] = {
    "throttled": [
        ["summarize_session.py", "--mode", "throttled"],
        ["archive_plans.py"],
    ],
    "final": [
        ["summarize_session.py", "--mode", "final"],
        ["archive_plans.py"],
    ],
}

# 各脚本的超时（秒）
SCRIPT_TIMEOUTS: dict[str, int] = {
    "summarize_session.py": 60,
    "collect_experience.py": 60,
    "archive_plans.py": 30,
    "distill_knowledge.py": 180,
}

HARD_TIMEOUT = 300  # 整体硬超时

LOG_DIR = HOOKS_DIR
LOG_FILE = LOG_DIR / "hook_runner.log"

# ---------------------------------------------------------------------------
# 日志
# ---------------------------------------------------------------------------


def setup_logging() -> logging.Logger:
    """配置日志，输出到文件。"""
    log = logging.getLogger("hook_runner")
    log.setLevel(logging.DEBUG)
    if not log.handlers:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        from logging.handlers import RotatingFileHandler

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
# 工具函数
# ---------------------------------------------------------------------------


def find_python() -> str:
    """找到可用的 Python 解释器路径。

    优先级: sys.executable → python3 → py -3 → python
    """
    # 优先使用当前解释器
    if sys.executable:
        return sys.executable

    # 按平台尝试
    candidates = (
        ["py", "-3"] if sys.platform == "win32" else ["python3"]
    ) + ["python"]

    for cmd in candidates:
        if isinstance(cmd, list):
            try:
                subprocess.run(
                    cmd + ["--version"],
                    capture_output=True,
                    timeout=5,
                )
                return " ".join(cmd)
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
        else:
            try:
                subprocess.run(
                    [cmd, "--version"],
                    capture_output=True,
                    timeout=5,
                )
                return cmd
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue

    # 兜底
    return sys.executable or "python"


def read_stdin_safe() -> dict:
    """从 stdin 安全读取 JSON，超时或失败返回空 dict。"""
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


def determine_stage(stdin_data: dict) -> str:
    """根据 stdin JSON 自动判断应执行的阶段。

    兼容两种 IDE 环境:
      - Cursor: {"status": "completed", "loop_count": 0}
      - CodeBuddy: {"hook_event_name": "Stop", "stop_hook_active": false, ...}

    策略:
      - Cursor: status=="completed" 且 loop_count==0 → final
      - CodeBuddy SessionEnd 事件 → final
      - 其他 → throttled（由 summarize_session.py 内部节流控制是否调 LLM）
    """
    # Cursor 格式: 有 status + loop_count
    if "status" in stdin_data and "loop_count" in stdin_data:
        loop_count = stdin_data.get("loop_count", 0)
        status = stdin_data.get("status", "")
        if loop_count == 0 and status == "completed":
            return "final"
        return "throttled"

    # CodeBuddy 格式: 有 hook_event_name
    event_name = stdin_data.get("hook_event_name", "")
    if event_name == "SessionEnd":
        return "final"

    # CodeBuddy Stop 事件 → throttled（摘要脚本内部按轮次节流）
    return "throttled"


def watchdog_timer(timeout: int) -> None:
    """硬超时看门狗，到时强制退出。"""
    time.sleep(timeout)
    logger.error("HARD TIMEOUT (%ds) reached, forcing exit", timeout)
    os._exit(1)


# ---------------------------------------------------------------------------
# 执行
# ---------------------------------------------------------------------------


def run_stage(stage: str, stdin_data: dict) -> dict:
    """执行指定阶段的所有脚本。

    Args:
        stage: "throttled" 或 "final"
        stdin_data: 从 hook stdin 读取的 JSON dict

    Returns:
        执行结果摘要 dict
    """
    scripts = STAGE_SCRIPTS.get(stage, [])
    if not scripts:
        logger.warning("Unknown stage: %s", stage)
        return {"error": f"unknown stage: {stage}"}

    python_cmd = find_python()
    stdin_bytes = json.dumps(stdin_data, ensure_ascii=False).encode("utf-8")
    results = {}

    logger.info("=== Stage [%s] start, %d scripts ===", stage, len(scripts))

    for script_args in scripts:
        script_name = script_args[0]
        script_path = HOOKS_DIR / script_name
        timeout = SCRIPT_TIMEOUTS.get(script_name, 60)

        if not script_path.exists():
            logger.warning("Script not found: %s", script_path)
            results[script_name] = "not_found"
            continue

        # 构建命令
        if " " in python_cmd:
            cmd = python_cmd.split() + [str(script_path)] + script_args[1:]
        else:
            cmd = [python_cmd, str(script_path)] + script_args[1:]

        logger.info("Running: %s (timeout=%ds)", " ".join(cmd), timeout)

        try:
            proc = subprocess.run(
                cmd,
                input=stdin_bytes,
                capture_output=True,
                timeout=timeout,
                cwd=str(PROJECT_DIR),
                env={**os.environ, "CODEBUDDY_PROJECT_DIR": str(PROJECT_DIR)},
            )
            if proc.returncode == 0:
                logger.info("%s completed OK", script_name)
                results[script_name] = "ok"
            else:
                stderr = proc.stderr.decode("utf-8", errors="replace")[:500]
                logger.warning(
                    "%s exited %d: %s", script_name, proc.returncode, stderr
                )
                results[script_name] = f"exit_{proc.returncode}"
        except subprocess.TimeoutExpired:
            logger.error("%s TIMEOUT after %ds", script_name, timeout)
            results[script_name] = "timeout"
        except FileNotFoundError as exc:
            logger.error("%s not found: %s", script_name, exc)
            results[script_name] = "exec_error"
        except Exception as exc:
            logger.error("%s unexpected error: %s", script_name, exc)
            results[script_name] = f"error: {exc}"

    logger.info("=== Stage [%s] done: %s ===", stage, results)
    return results


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(description="MetaAgent Hook Runner")
    parser.add_argument(
        "--event",
        default="stop",
        choices=["stop", "sessionEnd"],
        help="Hook event type",
    )
    parser.add_argument(
        "--stage",
        default="auto",
        choices=["auto", "throttled", "final"],
        help="Execution stage (auto = determine from stdin)",
    )
    args = parser.parse_args()

    # 看门狗
    wd = threading.Thread(target=watchdog_timer, args=(HARD_TIMEOUT,), daemon=True)
    wd.start()

    # 读取 stdin
    stdin_data = read_stdin_safe()
    logger.info(
        "hook_runner start: event=%s stage=%s stdin_keys=%s",
        args.event,
        args.stage,
        list(stdin_data.keys()),
    )

    # 确定阶段
    if args.stage == "auto":
        if args.event == "sessionEnd":
            # Cursor sessionEnd → 直接 final
            stage = "final"
        else:
            stage = determine_stage(stdin_data)
    else:
        stage = args.stage

    logger.info("Resolved stage: %s", stage)

    # 执行
    results = run_stage(stage, stdin_data)

    # stop hook 可以返回 followup_message 让 agent 继续
    # 我们不需要这个功能，静默退出
    logger.info("hook_runner done: %s", results)


if __name__ == "__main__":
    main()
