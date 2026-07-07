#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

RUNS="${RUNS:-3}"
SCENARIOS="${SCENARIOS:-A,B,C,D}"
NO_ANALYZE="${NO_ANALYZE:-0}"
BUILD_DIR="${BUILD_DIR:-build_research}"

CONFIG_H="$ROOT/Configs.h"
CONFIG_CPP="$ROOT/Configs.cpp"
TOPO_CPP="$ROOT/Factories/TopologyFactory.cpp"
RES_DIR="$ROOT/res"

mkdir -p "$RES_DIR"

cp "$CONFIG_H" "$CONFIG_H.bak.runall"
cp "$CONFIG_CPP" "$CONFIG_CPP.bak.runall"
cp "$TOPO_CPP" "$TOPO_CPP.bak.runall"
restore_originals() {
  cp "$CONFIG_H.bak.runall" "$CONFIG_H" || true
  cp "$CONFIG_CPP.bak.runall" "$CONFIG_CPP" || true
  cp "$TOPO_CPP.bak.runall" "$TOPO_CPP" || true
}
trap restore_originals EXIT

apply_config() {
  local scenario="$1"
  local policy_id="$2"
  python3 - "$scenario" "$policy_id" "$CONFIG_H" "$CONFIG_CPP" "$TOPO_CPP" <<'PY'
import sys, re, textwrap
from pathlib import Path

scenario = sys.argv[1]
policy_id = sys.argv[2]
config_h = Path(sys.argv[3])
config_cpp = Path(sys.argv[4])
topo_cpp = Path(sys.argv[5])

h = config_h.read_text(encoding='utf-8')
cpp = config_cpp.read_text(encoding='utf-8')
topo = topo_cpp.read_text(encoding='utf-8')

# policy selector
h = re.sub(r'constexpr auto FINALIZE_POLICY\s*=\s*\d+;',
           f'constexpr auto FINALIZE_POLICY = {policy_id};', h, count=1)

if scenario == 'A':
    attack_s, adv_frac, hold = 0, '0.00', '0.0'
    region_line = 'const double REGIONS_DISTRIBUTION[REGIONS_NUM] = { 0.476, 0.222, 0, 0.297, 0.005, 0 };'
    hash_mode = 'gaussian'
elif scenario == 'B':
    attack_s, adv_frac, hold = 0, '0.00', '0.0'
    region_line = 'const double REGIONS_DISTRIBUTION[REGIONS_NUM] = { 0.20, 0.10, 0.0, 0.60, 0.10, 0.0 };'
    hash_mode = 'equal'
elif scenario == 'C':
    attack_s, adv_frac, hold = 1, '0.10', '5.0'
    region_line = 'const double REGIONS_DISTRIBUTION[REGIONS_NUM] = { 0.476, 0.222, 0, 0.297, 0.005, 0 };'
    hash_mode = 'gaussian'
elif scenario == 'D':
    attack_s, adv_frac, hold = 1, '0.30', '10.0'
    region_line = 'const double REGIONS_DISTRIBUTION[REGIONS_NUM] = { 0.476, 0.222, 0, 0.297, 0.005, 0 };'
    hash_mode = 'gaussian'
else:
    raise SystemExit(f'Unknown scenario: {scenario}')

h = re.sub(r'constexpr auto ATTACK_SCENARIO\s*=\s*\d+;',
           f'constexpr auto ATTACK_SCENARIO      = {attack_s};', h, count=1)
h = re.sub(r'constexpr auto ADVERSARY_FRACTION\s*=\s*[0-9.]+;',
           f'constexpr auto ADVERSARY_FRACTION   = {adv_frac};', h, count=1)
h = re.sub(r'constexpr auto ATTACK_HOLD_TIME\s*=\s*[0-9.]+;',
           f'constexpr auto ATTACK_HOLD_TIME     = {hold};', h, count=1)

cpp = re.sub(r'const double REGIONS_DISTRIBUTION\[REGIONS_NUM\]\s*=\s*\{[^;]+\};', region_line, cpp, count=1)

pat = re.compile(r'(\n\s*// Hash power assignment .*?\n)(.*?)(\n\s*// Stake falls in Gaussian distribution)', re.S)
if hash_mode == 'gaussian':
    block = textwrap.dedent('''
        // Hash power assignment (deterministic seed for reproducible experiments).
        std::mt19937 gen(HASH_ASSIGN_SEED);
        std::normal_distribution<double> dis(5.0, 1.0);

        for (int i = 0; i < NODES_NUM; i++) {
            double hp = std::max(0.1, dis(gen));
            nodePool[i]->hashPower = hp;
            totalHash += hp;
        }

        double tmp = 0.0;
        for (int i = 0; i < NODES_NUM - 1; i++) {
            nodePool[i]->hashPower /= totalHash;
            tmp += nodePool[i]->hashPower;
        }
        nodePool[NODES_NUM - 1]->hashPower = 1.0 - tmp;

        // ^ Scenario B: equal hash power for all nodes
        //  for (int i = 0; i < NODES_NUM; ++i) {
        //      nodePool[i]->hashPower = 1.0 / static_cast<double>(NODES_NUM);
        //  }
        //  totalHash = 1.0;
    ''').rstrip()
else:
    block = textwrap.dedent('''
        // Hash power assignment (deterministic seed for reproducible experiments).
        std::mt19937 gen(HASH_ASSIGN_SEED);
        std::normal_distribution<double> dis(5.0, 1.0);

        for (int i = 0; i < NODES_NUM; ++i) {
            nodePool[i]->hashPower = 1.0 / static_cast<double>(NODES_NUM);
        }
        totalHash = 1.0;
    ''').rstrip()

def repl(m):
    return m.group(1) + block + m.group(3)

topo_new, n = pat.subn(repl, topo, count=1)
if n != 1:
    raise SystemExit('Failed to patch TopologyFactory.cpp hash-power block')
topo = topo_new

config_h.write_text(h, encoding='utf-8')
config_cpp.write_text(cpp, encoding='utf-8')
topo_cpp.write_text(topo, encoding='utf-8')
PY
}

build_project() {
  mkdir -p "$BUILD_DIR"
  pushd "$BUILD_DIR" >/dev/null
  # Always reconfigure; avoids stale CMake cache/source-dir errors.
  cmake ..
  make -j"$(nproc)"
  popd >/dev/null
}

run_one() {
  local scenario="$1"
  local policy_name="$2"
  local run_no="$3"
  local exe="$ROOT/$BUILD_DIR/CBlockSim"

  rm -f "$ROOT/output.csv" "$ROOT/node_stats.csv"
  "$exe"

  if [ ! -f "$ROOT/output.csv" ] || [ ! -f "$ROOT/node_stats.csv" ]; then
    echo "[ERROR] Missing output.csv or node_stats.csv after run." >&2
    exit 1
  fi

  local run_tag
  run_tag=$(printf 'run%02d' "$run_no")
  mv "$ROOT/output.csv"     "$RES_DIR/output_${scenario}_${policy_name}_${run_tag}.csv"
  mv "$ROOT/node_stats.csv" "$RES_DIR/node_stats_${scenario}_${policy_name}_${run_tag}.csv"
}

policy_name_from_id() {
  case "$1" in
    0) echo "LCR" ;;
    1) echo "GHOST" ;;
    2) echo "VAGHOST" ;;
    *) echo "UNKNOWN" ;;
  esac
}

echo "[INFO] ROOT=$ROOT"
echo "[INFO] SCENARIOS=$SCENARIOS"
echo "[INFO] RUNS=$RUNS"

IFS=',' read -r -a scenarios_arr <<< "$SCENARIOS"
for scen in "${scenarios_arr[@]}"; do
  scen="${scen// /}"
  case "$scen" in A|B|C|D) ;; *) echo "[ERROR] Invalid scenario: $scen"; exit 1;; esac

  for policy_id in 0 1 2; do
    policy_name="$(policy_name_from_id "$policy_id")"
    echo "[INFO] Scenario=$scen Policy=$policy_name"
    apply_config "$scen" "$policy_id"
    build_project

    for ((r=1; r<=RUNS; r++)); do
      echo "[INFO]   run $r/$RUNS"
      run_one "$scen" "$policy_name" "$r"
    done
  done
done

if [ "$NO_ANALYZE" != "1" ] && [ -f "$RES_DIR/analyze_results1.py" ]; then
  echo "[INFO] Running aggregate analysis..."
  pushd "$RES_DIR" >/dev/null
  python3 analyze_results1.py || true
  popd >/dev/null
fi

echo "[DONE] Outputs are in: $RES_DIR"
echo "[DONE] Originals restored in source tree."
