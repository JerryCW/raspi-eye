#!/usr/bin/env python3
"""核实并修正 species.yaml 中的 taxon_id。

从 iNaturalist API 按学名搜索真实 taxon_id，与 species.yaml 中的值对比，
打印差异报告，可选自动更新 YAML 文件。

用法：
    # 仅检查，不修改
    python model/src/verify_taxon_ids.py --config model/config/species.yaml

    # 检查并自动修正
    python model/src/verify_taxon_ids.py --config model/config/species.yaml --fix
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

import requests
import yaml

INAT_API_BASE = "https://api.inaturalist.org/v1"


def _request_with_backoff(url: str, params: dict, max_retries: int = 5) -> requests.Response:
    """带 exponential backoff 的 HTTP GET 请求。"""
    for attempt in range(max_retries):
        try:
            resp = requests.get(url, params=params, timeout=30)
            resp.raise_for_status()
            return resp
        except (requests.exceptions.RequestException, requests.exceptions.ConnectionError) as e:
            if attempt == max_retries - 1:
                raise
            wait = 2 ** attempt  # 1, 2, 4, 8, 16 秒
            print(f"  ⚠️ 请求失败 ({e.__class__.__name__})，{wait}s 后重试...")
            time.sleep(wait)
    raise RuntimeError("不应到达此处")


def search_taxon_id(scientific_name: str) -> tuple[int | None, str]:
    """按学名搜索 iNaturalist taxon_id。

    返回 (taxon_id, matched_name)。找不到返回 (None, "")。
    """
    resp = _request_with_backoff(
        f"{INAT_API_BASE}/taxa",
        params={"q": scientific_name, "rank": "species", "per_page": 5},
    )
    resp.raise_for_status()
    results = resp.json().get("results", [])

    # 精确匹配学名
    for r in results:
        if r.get("name", "").lower() == scientific_name.lower():
            return r["id"], r["name"]

    # 回退：返回第一个结果
    if results:
        return results[0]["id"], results[0].get("name", "")

    return None, ""


def count_observations(taxon_id: int) -> int:
    """查询某 taxon_id 的 research grade 观察数量。"""
    resp = _request_with_backoff(
        f"{INAT_API_BASE}/observations",
        params={"taxon_id": taxon_id, "quality_grade": "research", "per_page": 1},
    )
    resp.raise_for_status()
    return resp.json().get("total_results", 0)


def main():
    parser = argparse.ArgumentParser(description="核实并修正 species.yaml 中的 taxon_id")
    parser.add_argument("--config", required=True, help="species.yaml 路径")
    parser.add_argument("--fix", action="store_true", help="自动修正错误的 taxon_id")
    args = parser.parse_args()

    config_path = Path(args.config)
    if not config_path.is_file():
        print(f"错误: 文件不存在 {config_path}", file=sys.stderr)
        sys.exit(1)

    with open(config_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    species_list = cfg.get("species", [])
    print(f"共 {len(species_list)} 个物种\n")
    print(f"{'状态':<4} {'学名':<35} {'yaml_id':>10} {'real_id':>10} {'观察数':>8}")
    print("-" * 75)

    fixes = {}  # old_id -> new_id
    errors = 0

    for sp in species_list:
        name = sp["scientific_name"]
        yaml_id = sp["taxon_id"]

        real_id, matched_name = search_taxon_id(name)
        time.sleep(0.3)

        if real_id is None:
            obs = 0
            status = "???"
            errors += 1
        else:
            obs = count_observations(real_id)
            time.sleep(0.3)

            if real_id == yaml_id:
                status = " ✓ "
            else:
                status = " ✗ "
                fixes[yaml_id] = real_id
                errors += 1

        note = ""
        if matched_name and matched_name.lower() != name.lower():
            note = f"  (iNat: {matched_name})"

        print(f"{status} {name:<35} {yaml_id:>10} {real_id or '???':>10} {obs:>8}{note}")

    print("-" * 75)
    print(f"正确: {len(species_list) - errors}  错误: {errors}")

    if not fixes:
        print("\n所有 taxon_id 正确，无需修改。")
        return

    if not args.fix:
        print(f"\n发现 {len(fixes)} 个错误的 taxon_id。使用 --fix 自动修正。")
        return

    # 用正则替换 YAML 中的 taxon_id（保留注释和格式）
    raw_text = config_path.read_text(encoding="utf-8")
    fixed_count = 0
    for old_id, new_id in fixes.items():
        pattern = rf"(taxon_id:\s*){old_id}\b"
        new_text = re.sub(pattern, rf"\g<1>{new_id}", raw_text)
        if new_text != raw_text:
            raw_text = new_text
            fixed_count += 1

    config_path.write_text(raw_text, encoding="utf-8")
    print(f"\n已修正 {fixed_count} 个 taxon_id，保存到 {config_path}")


if __name__ == "__main__":
    main()
