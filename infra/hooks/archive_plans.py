#!/usr/bin/env python3
"""Plan 文件归档兜底脚本（IDE 无关）。

扫描 Cursor 全局 plans 目录，将属于本项目的 plan 归档到 docs/plans/。
作为 SessionEnd hook 的兜底机制，补充 Rule 可能遗漏的归档。

用法:
    py -3 infra/hooks/archive_plans.py [--project-dir PATH] [--dry-run]
"""

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

METAAGENT_KEYWORDS = [
    "MetaAgent",
    "ARCHITECTURE.md",
    ".cursor/agents/",
    ".cursor/hooks/",
    "CURSOR_TRANSCRIPT_PATH",
    "中控",
    "Blueprint DNA",
    "docs/summaries",
    "docs/plans",
    "Subagent",
]

KEYWORD_THRESHOLD = 2


def resolve_project_dir(cli_arg: str | None) -> Path:
    for env_key in ("CODEBUDDY_PROJECT_DIR", "CLAUDE_PROJECT_DIR", "CURSOR_PROJECT_DIR"):
        env_val = os.environ.get(env_key)
        if env_val:
            return Path(env_val)
    if cli_arg:
        return Path(cli_arg)
    return Path(__file__).resolve().parent.parent.parent


def find_cursor_plans_dir() -> Path | None:
    """定位 Cursor 全局 plans 目录。"""
    local_app = os.environ.get("LOCALAPPDATA", "")
    if not local_app:
        home = Path.home()
        candidate = home / ".cursor" / "plans"
    else:
        candidate = Path(local_app).parent / ".cursor" / "plans"

    if candidate.is_dir():
        return candidate

    home_candidate = Path.home() / ".cursor" / "plans"
    if home_candidate.is_dir():
        return home_candidate

    return None


def parse_plan_name(content: str) -> str | None:
    """从 YAML frontmatter 提取 plan 的 name 字段。"""
    match = re.match(r"^---\s*\n(.*?)\n---", content, re.DOTALL)
    if not match:
        return None
    for line in match.group(1).splitlines():
        m = re.match(r"^name:\s*(.+)$", line.strip())
        if m:
            return m.group(1).strip()
    return None


def to_kebab_case(name: str) -> str:
    """将 plan name 转为 kebab-case 文件名。"""
    result = name.lower()
    result = re.sub(r"[^a-z0-9\s-]", "", result)
    result = re.sub(r"\s+", "-", result.strip())
    result = re.sub(r"-+", "-", result)
    return result


def is_metaagent_plan(content: str) -> bool:
    """启发式判断 plan 是否属于 MetaAgent 项目。"""
    hits = sum(1 for kw in METAAGENT_KEYWORDS if kw in content)
    return hits >= KEYWORD_THRESHOLD


def content_changed(src: Path, dst: Path) -> bool:
    """检查源文件和目标文件内容是否不同。"""
    if not dst.exists():
        return True
    try:
        return src.read_bytes() != dst.read_bytes()
    except OSError:
        return True


def archive_plans(project_dir: Path, dry_run: bool = False) -> list[str]:
    """扫描并归档属于 MetaAgent 的 plan 文件。返回归档的文件名列表。"""
    plans_dir = find_cursor_plans_dir()
    if not plans_dir:
        return []

    dest_dir = project_dir / "docs" / "plans"
    if not dry_run:
        dest_dir.mkdir(parents=True, exist_ok=True)

    archived = []

    for plan_file in sorted(plans_dir.glob("*.plan.md")):
        try:
            content = plan_file.read_text(encoding="utf-8")
        except OSError:
            continue

        if not is_metaagent_plan(content):
            continue

        name = parse_plan_name(content)
        if not name:
            continue

        kebab = to_kebab_case(name)
        if not kebab:
            continue

        dest_name = f"{kebab}.plan.md"
        dest_path = dest_dir / dest_name

        if not content_changed(plan_file, dest_path):
            continue

        if dry_run:
            print(f"[DRY-RUN] {plan_file.name} -> docs/plans/{dest_name}")
        else:
            shutil.copy2(plan_file, dest_path)
            print(f"Archived: {plan_file.name} -> docs/plans/{dest_name}")

        archived.append(dest_name)

    return archived


def main() -> None:
    parser = argparse.ArgumentParser(description="Archive MetaAgent plan files")
    parser.add_argument("--project-dir", help="Project root directory")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be archived")
    args = parser.parse_args()

    project_dir = resolve_project_dir(args.project_dir)
    archived = archive_plans(project_dir, dry_run=args.dry_run)

    if not archived:
        print("No new plans to archive.")
    else:
        print(f"Total: {len(archived)} plan(s) archived.")


if __name__ == "__main__":
    main()
