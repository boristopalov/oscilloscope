#!/usr/bin/env python3
"""Summarize exported host observations without claiming Live validation."""
import argparse
import collections
import csv
import math


def summarize(rows):
    groups = collections.defaultdict(list)
    for row in rows:
        if row.get("schema") != "1":
            raise ValueError("Unsupported or missing probe CSV schema")
        groups[(row["process_id"], row["registry_token"], row["source_id"], row["generation"])].append(row)
    lines = []
    registries = {(key[0], key[1]) for key in groups}
    lines.append(f"Registries observed: {len(registries)}; source/session groups: {len(groups)}")
    for key, source_rows in sorted(groups.items()):
        pid, token, source_id, generation = key
        events = collections.Counter(row["event"] for row in source_rows)
        lines.append(f"\nProcess {pid}, registry {token}, Tap {source_id}, session {generation}")
        lines.append(f"Rows: {len(source_rows)}; events: {dict(sorted(events.items()))}")
        lines.append(f"Cumulative dropped: {max(int(row['dropped_total']) for row in source_rows)}; "
                     f"session dropped: {max(int(row['dropped_session']) for row in source_rows)}; "
                     f"session evicted: {max(int(row['evicted_total']) for row in source_rows)}")
        # Select contiguous sample positions, not the provisional PPQ classification.
        # This preserves meaningful PPQ rounding residuals even if they exceed the initial threshold.
        residuals = []
        for row in source_rows:
            if row["playing"] != "1" or row["looping"] != "0" or row["event"] in {"tempo_changed", "capture_gap"}:
                continue
            if row["sample_error"] and row["ppq_error_samples"] and abs(float(row["sample_error"])) <= 1:
                value = float(row["ppq_error_samples"])
                if math.isfinite(value):
                    residuals.append(value)
        if residuals:
            lines.append(f"PPQ residual for contiguous sample blocks ({len(residuals)} rows): "
                         f"min {min(residuals):.12g}, max {max(residuals):.12g}, "
                         f"max magnitude {max(map(abs, residuals)):.12g} samples")
        else:
            lines.append("No comparable contiguous timing rows; no tolerance can be inferred.")
        positions = []
        for row in source_rows:
            if row["sample_position"] and int(row["peak_offset_l"]) >= 0:
                positions.append(int(row["sample_position"]) + int(row["peak_offset_l"]))
        lines.append("L block-peak positions (first 20, useful ONLY with isolated impulses): "
                     + ", ".join(map(str, positions[:20])))
    lines.append("\nCapture summaries are observations. Confirm experiment labels, host version, routing, "
                 "and audio compensation before adopting a tolerance or display offset.")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+", help="CSV files exported from the diagnostic editor")
    args = parser.parse_args()
    rows = []
    for path in args.csv:
        with open(path, newline="", encoding="utf-8") as stream:
            rows.extend(csv.DictReader(stream))
    print(summarize(rows))


if __name__ == "__main__":
    main()
