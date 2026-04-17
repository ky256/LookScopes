#!/usr/bin/env python3
"""状态管理与文件锁（共享模块）。

封装跨平台文件锁、JSON 状态文件读写、原子写入等基础设施。
从 summarize_session.py、collect_experience.py、distill_knowledge.py 中提取消除重复。
"""

import glob
import json
import logging
import os
import sys
import time
from pathlib import Path

if sys.platform == "win32":
    import msvcrt
else:
    import fcntl

logger: logging.Logger = logging.getLogger("shared.state")


# ---------------------------------------------------------------------------
# 文件锁（跨平台）
# ---------------------------------------------------------------------------


def acquire_lock(f) -> bool:  # noqa: ANN001
    """尝试对已打开的文件描述符获取非阻塞排他锁。

    Args:
        f: 已打开的文件对象（需有 fileno()）。

    Returns:
        True 表示获取成功，False 表示被其它进程持有。
    """
    try:
        f.seek(0)
        if sys.platform == "win32":
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except (OSError, IOError):
        return False


def release_lock(f) -> None:  # noqa: ANN001
    """释放通过 acquire_lock 获取的排他锁。

    Args:
        f: 持有锁的文件对象。
    """
    try:
        f.seek(0)
        if sys.platform == "win32":
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    except (OSError, IOError):
        pass


# ---------------------------------------------------------------------------
# JSON 状态文件读写
# ---------------------------------------------------------------------------


def read_state(state_path: Path, default: dict | None = None) -> dict:
    """读取状态 JSON 文件。

    Args:
        state_path: 状态文件路径。
        default: 缺失或损坏时返回的默认值。None 时使用空 dict。

    Returns:
        解析后的状态 dict。
    """
    if default is None:
        default = {}
    if not state_path.exists():
        return dict(default)
    try:
        with open(state_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if not isinstance(data, dict):
            return dict(default)
        return data
    except (OSError, json.JSONDecodeError):
        return dict(default)


def write_state(state_path: Path, state: dict) -> None:
    """通过临时文件 + os.replace 原子写入状态。

    Args:
        state_path: 状态文件路径。
        state: 待持久化的状态 dict。
    """
    state_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = state_path.with_suffix(".tmp")
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=2)
        os.replace(str(tmp_path), str(state_path))
    except OSError as exc:
        logger.error("write_state failed: %s", exc)
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass


def cleanup_old_states(hooks_dir: Path, pattern: str = ".summary_state_*.json", max_age_days: int = 7) -> None:
    """清理超过指定天数的旧状态文件。

    Args:
        hooks_dir: 状态文件所在目录。
        pattern: glob 匹配模式。
        max_age_days: 最大保留天数。
    """
    cutoff = time.time() - max_age_days * 86400
    full_pattern = str(hooks_dir / pattern)
    for path_str in glob.glob(full_pattern):
        try:
            if os.path.getmtime(path_str) < cutoff:
                os.remove(path_str)
                logger.debug("Cleaned old state: %s", path_str)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 原子写入工具
# ---------------------------------------------------------------------------


def atomic_write_json(path: Path, data: dict | list) -> bool:
    """通过临时文件 + os.replace 原子写入 JSON。

    Args:
        path: 目标文件路径。
        data: 待序列化的数据。

    Returns:
        True 表示写入成功。
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        os.replace(str(tmp), str(path))
        return True
    except OSError as exc:
        logger.error("atomic_write_json failed for %s: %s", path, exc)
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        return False


def atomic_write_text(path: Path, content: str) -> bool:
    """通过临时文件 + os.replace 原子写入文本文件。

    Args:
        path: 目标文件路径。
        content: 文本内容。

    Returns:
        True 表示写入成功。
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(content)
        os.replace(str(tmp), str(path))
        return True
    except OSError as exc:
        logger.error("atomic_write_text failed for %s: %s", path, exc)
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        return False


def read_json_safe(path: Path) -> dict | None:
    """安全读取 JSON 文件，失败时返回 None。

    Args:
        path: JSON 文件路径。

    Returns:
        解析后的 dict 或 None。
    """
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
        return None
    except (OSError, json.JSONDecodeError, TypeError):
        return None
