"""iNaturalist API 数据采集 + taxonomy 获取模块。

提供 DataCollector 类，从 iNaturalist 公开 API 批量采集指定物种的
research grade 成鸟观察照片，并获取物种分类层级信息生成 taxonomy.json。
"""

import json
import logging
import os
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

import requests

from model.src.config import DatasetConfig, SpeciesEntry

logger = logging.getLogger(__name__)

INAT_API_BASE = "https://api.inaturalist.org"


@dataclass
class CollectStats:
    """单物种采集统计。"""

    species: str = ""
    target: int = 0
    downloaded: int = 0
    skipped: int = 0
    failed: int = 0


def _retry_request(func, max_retries: int = 3, base_delay: float = 2.0):
    """指数退避重试包装器。"""
    for attempt in range(max_retries):
        try:
            return func()
        except requests.RequestException as e:
            if attempt == max_retries - 1:
                raise
            delay = base_delay * (2 ** attempt)
            logger.warning(
                "请求失败，%.1fs 后重试 (%d/%d): %s",
                delay, attempt + 1, max_retries, e,
            )
            time.sleep(delay)


class DataCollector:
    """iNaturalist 数据采集器。"""

    def __init__(self, config: DatasetConfig, output_dir: str = "model/data/raw"):
        self.config = config
        self.output_dir = Path(output_dir)
        self._last_request_time: float = 0.0

    def _rate_limit(self) -> None:
        """确保两次 API 请求间隔 ≥ rate_limit 秒。"""
        elapsed = time.time() - self._last_request_time
        wait = self.config.global_config.rate_limit - elapsed
        if wait > 0:
            time.sleep(wait)
        self._last_request_time = time.time()

    def _api_get(self, path: str, params: dict | None = None) -> dict:
        """带速率限制和重试的 API GET 请求。"""
        url = f"{INAT_API_BASE}{path}"

        def do_request():
            self._rate_limit()
            resp = requests.get(url, params=params, timeout=30)
            resp.raise_for_status()
            return resp.json()

        return _retry_request(do_request)

    def _fetch_observations_page(
        self, taxon_id: int, page: int
    ) -> dict:
        """获取单页观察记录。"""
        params = {
            "taxon_id": taxon_id,
            "quality_grade": "research",
            "photos": "true",
            "per_page": 200,
            "page": page,
            "order": "desc",
            "order_by": "votes",
            "term_id": 1,
            "term_value_id": 2,
        }
        return self._api_get("/v1/observations", params)

    @staticmethod
    def _extract_photo_urls(observations: list[dict]) -> list[tuple[int, int, str]]:
        """从观察记录中提取照片信息。

        返回 (observation_id, photo_id, original_url) 列表。
        """
        results = []
        for obs in observations:
            obs_id = obs.get("id")
            photos = obs.get("photos", [])
            if not photos or obs_id is None:
                continue
            photo = photos[0]
            photo_id = photo.get("id")
            photo_url = photo.get("url", "")
            if not photo_url or photo_id is None:
                continue
            # 替换 square 为 original 尺寸
            original_url = photo_url.replace("square", "original")
            results.append((obs_id, photo_id, original_url))
        return results

    @staticmethod
    def _download_single(url: str, dest_path: str) -> bool:
        """下载单张图片到指定路径。成功返回 True，失败返回 False。"""
        try:
            resp = requests.get(url, timeout=60)
            resp.raise_for_status()
            with open(dest_path, "wb") as f:
                f.write(resp.content)
            return True
        except Exception as e:
            logger.error("下载失败 %s: %s", url, e)
            return False

    def collect_species(self, entry: SpeciesEntry) -> CollectStats:
        """采集单个物种的图片。

        流程：
        1. 分页调用 /v1/observations API
        2. 提取每个 observation 的第一张照片 URL，替换为 original 尺寸
        3. 多线程并发下载图片到 raw/{scientific_name}/
        4. 支持断点续传（跳过已存在文件）
        """
        stats = CollectStats(species=entry.scientific_name, target=entry.max_images)
        species_dir = self.output_dir / entry.scientific_name
        species_dir.mkdir(parents=True, exist_ok=True)

        # 收集已存在的文件名（断点续传）
        existing_files = {f.name for f in species_dir.iterdir() if f.is_file()}

        # 收集所有待下载的照片信息
        download_tasks: list[tuple[str, str]] = []  # (url, dest_path)
        page = 1

        while len(download_tasks) + stats.skipped < entry.max_images:
            try:
                data = self._fetch_observations_page(entry.taxon_id, page)
            except requests.RequestException as e:
                logger.error(
                    "获取 %s 第 %d 页失败，跳过: %s",
                    entry.scientific_name, page, e,
                )
                break

            results = data.get("results", [])
            if not results:
                break

            photos = self._extract_photo_urls(results)
            for obs_id, photo_id, url in photos:
                if len(download_tasks) + stats.skipped >= entry.max_images:
                    break
                filename = f"{obs_id}_{photo_id}.jpg"
                dest = str(species_dir / filename)
                if filename in existing_files:
                    stats.skipped += 1
                    continue
                download_tasks.append((url, dest))

            # 检查是否还有更多页
            total_results = data.get("total_results", 0)
            if page * 200 >= total_results:
                break
            page += 1

        # 多线程并发下载
        max_workers = self.config.global_config.download_threads
        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = {
                executor.submit(self._download_single, url, dest): (url, dest)
                for url, dest in download_tasks
            }
            for future in as_completed(futures):
                if future.result():
                    stats.downloaded += 1
                else:
                    stats.failed += 1

        # 打印统计
        print(
            f"[{entry.scientific_name}] "
            f"目标={stats.target} "
            f"下载={stats.downloaded} "
            f"跳过={stats.skipped} "
            f"失败={stats.failed}"
        )

        if stats.downloaded + stats.skipped < entry.max_images:
            print(
                f"提示: {entry.scientific_name} 可用图片不足 "
                f"(获取 {stats.downloaded + stats.skipped}/{entry.max_images})"
            )

        return stats

    def fetch_taxonomy(self, entry: SpeciesEntry) -> dict:
        """调用 /v1/taxa/{taxon_id} 获取物种分类层级。

        返回包含 family、order、common_name_en 等字段的字典。
        缺少中文名时用英文名填充。
        """
        try:
            data = self._api_get(f"/v1/taxa/{entry.taxon_id}")
        except requests.RequestException as e:
            logger.error("获取 %s taxonomy 失败: %s", entry.scientific_name, e)
            return {
                "taxon_id": entry.taxon_id,
                "scientific_name": entry.scientific_name,
                "common_name_cn": entry.common_name_cn or entry.scientific_name,
                "common_name_en": entry.scientific_name,
                "family": "",
                "family_cn": "",
                "order": "",
                "order_cn": "",
            }

        results = data.get("results", [])
        if not results:
            return {
                "taxon_id": entry.taxon_id,
                "scientific_name": entry.scientific_name,
                "common_name_cn": entry.common_name_cn or entry.scientific_name,
                "common_name_en": entry.scientific_name,
                "family": "",
                "family_cn": "",
                "order": "",
                "order_cn": "",
            }

        taxon = results[0]
        ancestors = taxon.get("ancestors", [])

        # 从 ancestors 中提取 family 和 order
        family = ""
        family_cn = ""
        order = ""
        order_cn = ""

        for ancestor in ancestors:
            rank = ancestor.get("rank", "")
            if rank == "family":
                family = ancestor.get("name", "")
                family_cn = ancestor.get("preferred_common_name", "")
            elif rank == "order":
                order = ancestor.get("name", "")
                order_cn = ancestor.get("preferred_common_name", "")

        # 英文名从 taxon 本身获取
        common_name_en = taxon.get("preferred_common_name", "")
        if not common_name_en:
            common_name_en = taxon.get("english_common_name", entry.scientific_name)

        # 中文名：优先用配置中的，其次 API 返回，最后用英文名填充
        common_name_cn = entry.common_name_cn
        if not common_name_cn:
            common_name_cn = common_name_en
            logger.warning(
                "%s 缺少中文名，使用英文名填充: %s",
                entry.scientific_name, common_name_en,
            )

        # 缺少中文科名/目名时用英文名填充
        if not family_cn:
            family_cn = family
        if not order_cn:
            order_cn = order

        return {
            "taxon_id": entry.taxon_id,
            "scientific_name": entry.scientific_name,
            "common_name_cn": common_name_cn,
            "common_name_en": common_name_en,
            "family": family,
            "family_cn": family_cn,
            "order": order,
            "order_cn": order_cn,
        }

    def collect_all(
        self, species_filter: str | None = None
    ) -> list[CollectStats]:
        """采集所有物种（或指定物种），返回统计列表。"""
        results: list[CollectStats] = []
        for entry in self.config.species:
            if species_filter and entry.scientific_name != species_filter:
                continue
            stats = self.collect_species(entry)
            results.append(stats)
        return results

    def build_taxonomy(self) -> dict:
        """构建完整 taxonomy.json 结构（species 段 + label_to_species 段）。

        class_label 从 0 开始，按 config 中物种顺序编号。
        """
        species_map: dict[str, dict] = {}
        label_to_species: dict[str, str] = {}

        for idx, entry in enumerate(self.config.species):
            tax_info = self.fetch_taxonomy(entry)
            tax_info["class_label"] = idx
            species_map[entry.scientific_name] = tax_info
            label_to_species[str(idx)] = entry.scientific_name

        return {
            "species": species_map,
            "label_to_species": label_to_species,
        }

    def save_taxonomy(
        self, output_path: str = "model/data/taxonomy.json"
    ) -> dict:
        """保存 taxonomy 到 JSON 文件。返回生成的 taxonomy 字典。"""
        taxonomy = self.build_taxonomy()
        out = Path(output_path)
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            json.dump(taxonomy, f, ensure_ascii=False, indent=2)
        print(f"Taxonomy 已保存到 {output_path} ({len(taxonomy['species'])} 个物种)")
        return taxonomy
