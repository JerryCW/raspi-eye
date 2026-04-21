#!/usr/bin/env python3
"""核实并修正 species.yaml 中的 taxon_id。

从 iNaturalist API 按学名搜索真实 taxon_id，与 species.yaml 中的值对比，
同时直接验证 yaml 中的 taxon_id 是否存在（防止搜索结果误匹配）。
打印差异报告，可选自动更新 YAML 文件。

用法：
    # 仅检查，不修改
    python model/collection/verify_taxon_ids.py --config model/config/species.yaml

    # 检查并自动修正
    python model/collection/verify_taxon_ids.py --config model/config/species.yaml --fix

    # 指定并发数（默认 5）
    python model/collection/verify_taxon_ids.py --config model/config/species.yaml --workers 10
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
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
            wait = 2 ** attempt
            time.sleep(wait)
    raise RuntimeError("不应到达此处")


def verify_taxon_exists(taxon_id: int) -> bool:
    """直接调 /v1/taxa/{id} 验证 taxon_id 是否存在。

    区分三种情况：
    - 200 + results 非空 → 存在
    - 200 + results 为空 → 不存在（API 正常返回但无数据）
    - 网络错误/限流 → 视为存在（宁可漏报，不误报）
    """
    try:
        resp = _request_with_backoff(f"{INAT_API_BASE}/taxa/{taxon_id}", params={})
        results = resp.json().get("results", [])
        return len(results) > 0
    except requests.exceptions.RequestException:
        # 网络错误或限流，不能确定是否存在，保守返回 True 避免误报
        return True


def search_taxon_id(scientific_name: str) -> tuple[int | None, str]:
    """按学名搜索 iNaturalist taxon_id。返回 (taxon_id, matched_name)。"""
    resp = _request_with_backoff(
        f"{INAT_API_BASE}/taxa",
        params={"q": scientific_name, "rank": "species", "per_page": 5},
    )
    results = resp.json().get("results", [])
    for r in results:
        if r.get("name", "").lower() == scientific_name.lower():
            return r["id"], r["name"]
    if results:
        return results[0]["id"], results[0].get("name", "")
    return None, ""


def count_observations(taxon_id: int) -> int:
    """查询某 taxon_id 的 research grade 观察数量。"""
    resp = _request_with_backoff(
        f"{INAT_API_BASE}/observations",
        params={"taxon_id": taxon_id, "quality_grade": "research", "per_page": 1},
    )
    return resp.json().get("total_results", 0)


@dataclass
class VerifyResult:
    """单个物种的验证结果。"""
    name: str
    yaml_id: int
    real_id: int | None
    matched_name: str
    obs_count: int
    yaml_id_exists: bool
    status: str  # " ✓ ", " ✗ ", "???"
    note: str


def verify_one_species(sp: dict) -> VerifyResult:
    """验证单个物种（线程安全，无共享状态）。"""
    name = sp["scientific_name"]
    yaml_id = sp["taxon_id"]

    yaml_id_exists = verify_taxon_exists(yaml_id)
    real_id, matched_name = search_taxon_id(name)

    if real_id is None:
        return VerifyResult(
            name=name, yaml_id=yaml_id, real_id=None, matched_name="",
            obs_count=0, yaml_id_exists=yaml_id_exists, status="???", note="",
        )

    obs = count_observations(real_id)

    if not yaml_id_exists:
        status = " ✗ "
        note = f"  ⚠️ taxon_id {yaml_id} 不存在!"
    elif real_id == yaml_id:
        status = " ✓ "
        note = ""
    else:
        status = " ✗ "
        note = ""

    if not note and matched_name and matched_name.lower() != name.lower():
        note = f"  (iNat: {matched_name})"

    return VerifyResult(
        name=name, yaml_id=yaml_id, real_id=real_id, matched_name=matched_name,
        obs_count=obs, yaml_id_exists=yaml_id_exists, status=status, note=note,
    )


def main():
    parser = argparse.ArgumentParser(description="核实并修正 species.yaml 中的 taxon_id")
    parser.add_argument("--config", required=True, help="species.yaml 路径")
    parser.add_argument("--fix", action="store_true", help="自动修正错误的 taxon_id")
    parser.add_argument("--workers", type=int, default=5,
                        help="并发线程数（默认 5，iNaturalist API 限流友好）")
    args = parser.parse_args()

    config_path = Path(args.config)
    if not config_path.is_file():
        print(f"错误: 文件不存在 {config_path}", file=sys.stderr)
        sys.exit(1)

    with open(config_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    species_list = cfg.get("species", [])
    total = len(species_list)
    print(f"共 {total} 个物种，{args.workers} 线程并发验证\n")

    # 并发验证所有物种
    results: list[VerifyResult] = [None] * total  # type: ignore
    done_count = 0
    start_time = time.time()

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        future_to_idx = {
            executor.submit(verify_one_species, sp): i
            for i, sp in enumerate(species_list)
        }
        for future in as_completed(future_to_idx):
            idx = future_to_idx[future]
            try:
                results[idx] = future.result()
            except Exception as e:
                sp = species_list[idx]
                results[idx] = VerifyResult(
                    name=sp["scientific_name"], yaml_id=sp["taxon_id"],
                    real_id=None, matched_name="", obs_count=0,
                    yaml_id_exists=False, status="???",
                    note=f"  ⚠️ {e.__class__.__name__}: {e}",
                )
            done_count += 1
            sys.stdout.write(f"\r  验证进度: {done_count}/{total}")
            sys.stdout.flush()

    elapsed = time.time() - start_time
    print(f"\r  验证完成: {total} 个物种，耗时 {elapsed:.1f}s\n")

    # 按原始顺序打印结果
    print(f"{'状态':<4} {'学名':<40} {'yaml_id':>10} {'real_id':>10} {'观察数':>8}")
    print("-" * 80)

    fixes = {}
    errors = 0

    for r in results:
        if r.status != " ✓ ":
            errors += 1
            if r.real_id is not None and r.real_id != r.yaml_id:
                fixes[r.yaml_id] = r.real_id

        real_id_str = str(r.real_id) if r.real_id is not None else "???"
        print(f"{r.status} {r.name:<40} {r.yaml_id:>10} {real_id_str:>10} {r.obs_count:>8}{r.note}")

    print("-" * 80)
    print(f"正确: {total - errors}  错误: {errors}")

    if not fixes:
        if errors == 0:
            print("\n所有 taxon_id 正确，无需修改。")
        return

    if not args.fix:
        print(f"\n发现 {len(fixes)} 个错误的 taxon_id。使用 --fix 自动修正。")
        return

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
