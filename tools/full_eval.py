#!/usr/bin/env python3 -u
"""Full 100-case evaluation with raw metrics. Output flushed per case."""
import subprocess, os, sys, time

DATA = '课程设计相关材料/数据集'
BINARY = './main'
EVAL = './tools/evaluate'
CASES = sorted(f for f in os.listdir(DATA) if f.endswith('.in'))

results = {'简单': [], '中等': [], '困难': [], '极端': []}

def difficulty(n):
    if n <= 20: return '简单'
    if n <= 60: return '中等'
    if n <= 90: return '困难'
    return '极端'

print(f"{'Case':<12} {'M':>4} {'N':>6} {'E_wait':>18} {'E_memory':>12} {'E_finish':>10} {'Time':>7} {'OK':>5}")
print('-' * 95)
sys.stdout.flush()

t0_total = time.perf_counter()

for infile_name in CASES:
    cn = int(infile_name.replace('case','').replace('.in',''))
    diff = difficulty(cn)
    infile = f'{DATA}/{infile_name}'
    outfile = f'/tmp/ev_{infile_name}.out'

    with open(infile) as f:
        parts = f.read().split()
        M, N = int(parts[0]), int(parts[1])

    # Generate schedule
    t0 = time.perf_counter()
    r = subprocess.run([BINARY], stdin=open(infile), capture_output=True, timeout=70)
    t1 = time.perf_counter()

    if r.returncode != 0:
        print(f"{infile_name:<12} {'?':>4} {'?':>6} {'CRASH':>18} {'-':>12} {'-':>10} {t1-t0:>6.1f}s FAIL")
        sys.stdout.flush()
        results[diff].append({'wait': float('inf'), 'mem': float('inf'), 'finish': float('inf'), 'legal': False})
        continue

    with open(outfile, 'wb') as f:
        f.write(r.stdout)

    # Evaluate
    r2 = subprocess.run([EVAL, infile, outfile], capture_output=True, text=True, timeout=10)
    output = r2.stdout
    legal = 'PASS' in output

    wait_s = mem_s = fin_s = 0.0
    for line in output.split('\n'):
        if 'E_wait' in line and '=' in line:
            try: wait_s = float(line.strip().split()[-2])
            except: pass
        if 'E_memory' in line and '=' in line:
            try: mem_s = float(line.strip().split()[-2])
            except: pass
        if 'E_finish' in line and '=' in line:
            try: fin_s = float(line.strip().split()[-1])
            except: pass

    results[diff].append({'wait': wait_s, 'mem': mem_s, 'finish': fin_s, 'legal': legal})

    print(f"{infile_name:<12} {M:>4} {N:>6} {wait_s:>18,.1f} {mem_s:>11,.1f} {fin_s:>10,.0f} {t1-t0:>6.1f}s {'OK' if legal else 'FAIL':>5}")
    sys.stdout.flush()

t_total = time.perf_counter() - t0_total

# Summary
print()
print('=' * 95)
for diff in ['简单', '中等', '困难', '极端']:
    items = results[diff]
    if not items: continue
    legal = sum(1 for x in items if x['legal'])
    waits = [x['wait'] for x in items if x['legal']]
    mems  = [x['mem']  for x in items if x['legal']]
    fins  = [x['finish'] for x in items if x['legal']]
    if waits:
        print(f"{diff:>4} ({len(items):>2}): legal={legal:>3}  avg_wait={sum(waits)/len(waits):>16,.1f}  avg_mem={sum(mems)/len(mems):>10,.1f}  avg_finish={sum(fins)/len(fins):>10,.0f}")
    else:
        print(f"{diff:>4} ({len(items):>2}): legal={legal:>3}  NO LEGAL RESULTS")

print(f"\nTotal: {t_total/60:.1f} min")
