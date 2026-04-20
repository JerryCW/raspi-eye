"""iNaturalist 鸟类数据采集与清洗 CLI 入口。

用法：
    python model/prepare_dataset.py --config model/config/species.yaml
    python model/prepare_dataset.py --config model/config/species.yaml --skip-download
    python model/prepare_dataset.py --config model/config/species.yaml --species "Passer montanus"
"""

import argparse
import sys
import time
from pathlib import Path

# 将代码目录加入 sys.path，使 src.* 可导入（兼容本地和 SageMaker 环境）
sys.path.insert(0, str(Path(__file__).resolve().parent))

from src.cleaner import DataCleaner
from src.collector import DataCollector
from src.config import load_config


def main():
    parser = argparse.ArgumentParser(description="iNaturalist 鸟类数据采集与清洗")
    parser.add_argument("--config", required=True, help="物种配置文件路径")
    parser.add_argument(
        "--skip-download", action="store_true", help="跳过下载，直接清洗"
    )
    parser.add_argument(
        "--species", type=str, help="指定单个物种（scientific_name）用于调试"
    )
    parser.add_argument(
        "--force-clean", action="store_true", help="强制重新清洗（忽略已有输出）"
    )
    args = parser.parse_args()

    # 1. 加载配置
    try:
        config = load_config(args.config)
    except FileNotFoundError as e:
        print(f"错误: {e}", file=sys.stderr)
        sys.exit(1)
    except ValueError as e:
        print(f"错误: {e}", file=sys.stderr)
        sys.exit(1)

    # 按 --species 过滤物种列表
    if args.species:
        matched = [s for s in config.species if s.scientific_name == args.species]
        if not matched:
            print(
                f"错误: 未找到物种 '{args.species}'，请检查 --species 参数",
                file=sys.stderr,
            )
            sys.exit(1)
        config.species = matched

    print(f"配置加载完成: {len(config.species)} 个物种")
    start_time = time.time()

    # 2. 采集（除非 --skip-download）
    collect_stats_map = {}
    if not args.skip_download:
        print("\n=== 阶段 1: 数据采集 ===")
        collector = DataCollector(config)
        collect_results = collector.collect_all(
            species_filter=args.species
        )
        for cs in collect_results:
            collect_stats_map[cs.species] = cs
    else:
        print("\n=== 跳过数据采集（--skip-download）===")

    # 3. 清洗（去重 → 质量过滤 → resize）
    print("\n=== 阶段 2: 数据清洗 ===")
    cleaner = DataCleaner(config)

    if args.species:
        # 单物种模式：只清洗指定物种
        clean_results = [cleaner.clean_species(args.species, force=args.force_clean)]
    else:
        clean_results = cleaner.clean_all(force=args.force_clean)

    # 4. 生成 taxonomy.json
    print("\n=== 阶段 3: 生成 taxonomy.json ===")
    collector_for_tax = DataCollector(config)
    collector_for_tax.save_taxonomy()

    # 5. 打印统计报告
    elapsed = time.time() - start_time
    print("\n" + "=" * 70)
    print("统计报告")
    print("=" * 70)
    print(
        f"{'物种':<35} {'采集':>6} {'去重后':>6} {'过滤后':>6} {'最终':>6}"
    )
    print("-" * 70)

    total_collected = 0
    total_dedup = 0
    total_filtered = 0
    total_final = 0

    for cs in clean_results:
        species = cs.species
        # 采集数 = 下载 + 跳过（断点续传）
        cstats = collect_stats_map.get(species)
        collected = (cstats.downloaded + cstats.skipped) if cstats else cs.input_count

        print(
            f"{species:<35} {collected:>6} {cs.after_dedup:>6} "
            f"{cs.after_filter:>6} {cs.output_count:>6}"
        )

        total_collected += collected
        total_dedup += cs.after_dedup
        total_filtered += cs.after_filter
        total_final += cs.output_count

        if cs.output_count == 0:
            print(f"  错误: {species} 最终图片数为 0")
        elif cs.output_count < 30:
            print(f"  警告: {species} 清洗后图片数 {cs.output_count} < 30")

    print("-" * 70)
    print(
        f"{'合计':<35} {total_collected:>6} {total_dedup:>6} "
        f"{total_filtered:>6} {total_final:>6}"
    )
    print(f"\n总耗时: {elapsed:.1f} 秒")
    print("=" * 70)


if __name__ == "__main__":
    main()
