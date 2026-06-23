#!/bin/bash
# Batch evaluation: runs main on all cases and prints raw metrics
# Usage: ./tools/batch_eval.sh [data_dir]

DATA_DIR="${1:-课程设计相关材料/数据集}"
BINARY="${2:-./main}"
EVAL_TOOL="${3:-./tools/evaluate}"

echo "Case        M     N   E_wait         E_memory(GB)  E_finish  Legal"
echo "===================================================================="

total_cases=0
legal_cases=0
total_wait=0
total_mem=0
total_finish=0

for infile in "$DATA_DIR"/case*.in; do
    case_name=$(basename "$infile" .in)
    outfile="/tmp/eval_${case_name}.out"

    # Run scheduler
    timeout 65 "$BINARY" < "$infile" > "$outfile" 2>/dev/null

    if [ $? -ne 0 ]; then
        printf "%-10s  CRASH/TIMEOUT\n" "$case_name"
        continue
    fi

    # Evaluate
    result=$("$EVAL_TOOL" "$infile" "$outfile" 2>/dev/null)
    legal=$(echo "$result" | grep "^PASS\|^FAIL" | head -1)
    wait_score=$(echo "$result" | grep "E_wait" | head -1 | awk '{print $3}')
    mem_score=$(echo "$result" | grep "E_memory" | head -1 | awk '{print $3}')
    finish_score=$(echo "$result" | grep "E_finish" | head -1 | awk '{print $3}')

    # Get M,N from input
    mn=$(head -1 "$infile")
    M=$(echo "$mn" | awk '{print $1}')
    N=$(echo "$mn" | awk '{print $2}')

    if echo "$legal" | grep -q "PASS"; then
        leg="OK"
        legal_cases=$((legal_cases + 1))
    else
        leg="FAIL"
    fi

    printf "%-10s %4s %5s %14s %13s %9s %s\n" \
        "$case_name" "$M" "$N" "$wait_score" "$mem_score" "$finish_score" "$leg"

    total_cases=$((total_cases + 1))
done

echo "===================================================================="
echo "Total: $total_cases cases, $legal_cases legal"
