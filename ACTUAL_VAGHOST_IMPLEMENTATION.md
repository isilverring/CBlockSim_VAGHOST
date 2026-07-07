# Actual VA-GHOST implementation notes

This package upgrades the earlier approximation into a substantially more faithful **VA-GHOST research prototype**.

## Core changes relative to the earlier approximation

### 1. Exact subtree root treatment
The earlier approximation visibility-discounted the candidate child block itself. That does **not** match the paper's definition:

\[
W_i^{\mathrm{VA}}(b,t) = 1 + \sum_{d \in \mathcal{D}_i(b,t)} g(\nu_d^{(i)}(t)).
\]

In this package, the candidate root contributes the constant `1`, while only **descendants** are visibility-discounted.

### 2. Node-local DAG and greedy VA-GHOST descent
Each node maintains its own local DAG:

- `knownBlocks`: all blocks known to the node
- `childBlocks`: parent → children adjacency
- `visibility_count`: local attestation counts per block

The preferred chain is computed by greedy descent from genesis using visibility-weighted subtree scores.

### 3. Certificate semantics aligned with the paper
When a node mines a block under `FINALIZE_POLICY == 2`, it builds a bounded visibility certificate from blocks in its local view whose counters satisfy:

\[
k_b^{(i)} \ge \tau_v.
\]

The reduction rule is **deterministic**:

- sort by decreasing depth
- tie-break by block id
- keep the first `L_max` block identifiers

On receive, a node increments counters only for certificate references it already recognizes in its local DAG.

### 4. Clean visibility-weighted contribution
The VA-GHOST subtree score no longer incorporates Ethereum uncle-count into the fork-choice weight. Uncle handling remains in the simulator for reward/statistics purposes, but the VA-GHOST fork-choice metric itself is now a **clean visibility-weighted subtree rule**.

### 5. Adversaries selected by hash-power share
The earlier code marked adversaries by **node count**. This package selects the attacker coalition by accumulating nodes until the configured target adversarial **hash-power share** is reached.

### 6. Coalition-style withholding and release
Attackers can share privately mined blocks within the attacking coalition while withholding them from honest nodes. Release occurs when one of the following triggers fires:

- maximum hold time reached
- private lead reaches `ATTACK_RELEASE_LEAD`
- public chain is about to catch up

Released blocks are propagated at **release time**, not retroactively at mining time.

## Remaining research-model limitations

This is still a simulator-side research model, so some choices remain heuristic and should be documented in any paper:

- the attacker strategy is a stylized withhold-and-release coalition, not a fully optimal adaptive adversary
- global end-of-simulation chain selection still uses a simulator-side majority/tie-break rule across node-local preferred heads
- certificate design is deterministic and simple; other publicly verifiable reduction rules could also be studied