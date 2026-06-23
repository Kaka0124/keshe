#!/usr/bin/env python3
"""Local batch testing script for GPU scheduling."""
import argparse
import subprocess
import sys
import time
from pathlib import Path


def run_case(binary: Path, input_file: Path) -> tuple[int, float, str]:
    """Run a single test case. Returns (exit_code, elapsed_sec, stderr)."""
    with open(input_file) as f:
        input_data = f.read()

    start = time.perf_counter()
    try:
        proc = subprocess.run(
            [str(binary)],
            input=input_data,
            capture_output=True,
            text=True,
            timeout=65,
        )
        elapsed = time.perf_counter() - start
        return proc.returncode, elapsed, proc.stderr
    except subprocess.TimeoutExpired:
        elapsed = time.perf_counter() - start
        return -1, elapsed, "TIMEOUT"
    except Exception as e:
        elapsed = time.perf_counter() - start
        return -2, elapsed, str(e)


def check_output(output: str, expected_job_count: int) -> list[str]:
    """Quick check of output format. Returns list of errors."""
    errors = []
    lines = [l.strip() for l in output.strip().split("\n") if l.strip()]

    if len(lines) != expected_job_count:
        errors.append(f"Line count {len(lines)} != expected {expected_job_count}")

    seen_ids = set()
    for line in lines:
        parts = line.split()
        if len(parts) != 5:
            errors.append(f"Invalid format: {line[:50]}")
            continue
        try:
            job_id = int(parts[0])
            if job_id in seen_ids:
                errors.append(f"Duplicate job_id: {job_id}")
            seen_ids.add(job_id)
        except ValueError:
            errors.append(f"Non-integer job_id: {parts[0]}")

    if len(seen_ids) != expected_job_count:
        missing = set(range(1, expected_job_count + 1)) - seen_ids
        if missing:
            errors.append(f"Missing jobs: {sorted(missing)[:10]}...")

    return errors


def main():
    parser = argparse.ArgumentParser(description="Batch test GPU scheduler")
    parser.add_argument("--binary", default="./main", help="Path to compiled binary")
    parser.add_argument("--input", default="./课程设计相关材料/数据集/", help="Input directory")
    parser.add_argument("--limit", type=int, default=0, help="Max cases to run (0=all)")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    if not binary.exists():
        print(f"ERROR: Binary not found: {binary}")
        sys.exit(1)

    input_dir = Path(args.input).resolve()
    case_files = sorted(input_dir.glob("case*.in"))
    if args.limit > 0:
        case_files = case_files[: args.limit]

    print(f"Testing {len(case_files)} cases with {binary}")
    print(f"{'='*60}")

    passed = 0
    failed = 0
    total_time = 0.0

    for case_file in case_files:
        case_name = case_file.stem
        exit_code, elapsed, stderr = run_case(binary, case_file)

        # Parse job count from input file
        with open(case_file) as f:
            first_line = f.readline()
            parts = first_line.split()
            expected_jobs = int(parts[1]) if len(parts) >= 2 else 0

        status = "PASS"
        detail = ""

        if exit_code != 0:
            status = "FAIL"
            detail = f"exit={exit_code}"
            failed += 1
        elif elapsed > 60:
            status = "FAIL"
            detail = f"time={elapsed:.1f}s > 60s"
            failed += 1
        else:
            passed += 1

        total_time += elapsed

        marker = "✓" if status == "PASS" else "✗"
        if args.verbose or status != "PASS":
            print(f"  {marker} {case_name}: {elapsed:.2f}s {detail}")

    print(f"{'='*60}")
    print(f"Results: {passed} passed, {failed} failed, "
          f"total time {total_time:.1f}s, "
          f"avg {total_time/len(case_files):.2f}s/case")


if __name__ == "__main__":
    main()
