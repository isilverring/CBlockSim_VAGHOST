# CBlockSim + VA-GHOST Research Prototype

This package contains a research-oriented extension of CBlockSim with a substantially tighter **VA-GHOST** implementation than the earlier chain-score approximation.

## What is implemented

The simulator now includes:

- **node-local block-DAG state** at each node (`knownBlocks`, `childBlocks`)
- **visibility counters** updated from received blocks and embedded certificates
- **bounded visibility certificates** constructed deterministically from the miner's local view
- **visibility-weighted subtree scoring** consistent with the paper's definition
- **greedy VA-GHOST head selection** recomputed on block receipt
- **adversarial withholding coalition support** with attacker selection by **hash-power share** rather than node count
- **delayed/strategic release** of privately mined blocks

## Important modeling note

This is a **research prototype**, not a production blockchain client. It is designed to study the behavior of visibility-aware fork choice under controlled network and attack settings.

## Build

```bash
mkdir -p build_research
cd build_research
cmake ..
make -j"$(nproc)"
```

Or simply:

```bash
bash run_research.sh
```

## Batch experiments

To run the standard A/B/C/D scenario suite:

```bash
bash run_all_scenarios.sh
```

Useful options:

```bash
RUNS=5 bash run_all_scenarios.sh
SCENARIOS=A,C bash run_all_scenarios.sh
NO_ANALYZE=1 bash run_all_scenarios.sh
```

## Results directory

The `res/` directory intentionally contains only analysis scripts in this package. Historical CSV/PNG artifacts from earlier approximations were removed to avoid accidental reuse. Re-run the experiments after this implementation change before using any results in a manuscript.

## Files of interest

- `Policy/BlockPolicy.cpp` / `.h` — VA-GHOST fork-choice logic
- `Models/Node.cpp` / `.h` — node-local DAG state and visibility counters
- `Factories/TopologyFactory.cpp` — hash-power assignment and attacker selection
- `Scheduler.cpp` / `.h` — simulation loop and withholding/release logic
- `ACTUAL_VAGHOST_IMPLEMENTATION.md` — implementation notes and model choices

## Reference

The base simulator originates from CBlockSim. This package extends it for visibility-aware fork-choice experiments.

Abdulwahab Almusailem, Othman Alenezi, and Ameer Mohammed, **“Visibility-Aware GHOST: Mitigating Visibility Asymmetry in Subtree-Based Proof-of-Work Consensus,”** in *Applied Cryptography and Network Security — ACNS 2026*, Springer LNCS, 2026.
