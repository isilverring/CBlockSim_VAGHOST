import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import os
import glob

# -------------------------------------------------------
# CONFIG: scenarios A, B, C, D with glob patterns
# -------------------------------------------------------

SCENARIOS = {
    "A_baseline": {
        "LCR": {
            "output_pattern": "output_A_LCR_run*.csv",
            "nodes_pattern":  "node_stats_A_LCR_run*.csv",
        },
        "GHOST": {
            "output_pattern": "output_A_GHOST_run*.csv",
            "nodes_pattern":  "node_stats_A_GHOST_run*.csv",
        },
        "VA-GHOST": {
            "output_pattern": "output_A_VAGHOST_run*.csv",
            "nodes_pattern":  "node_stats_A_VAGHOST_run*.csv",
        },
    },
    "B_fairness_hetero": {
        "LCR": {
            "output_pattern": "output_B_LCR_run*.csv",
            "nodes_pattern":  "node_stats_B_LCR_run*.csv",
        },
        "GHOST": {
            "output_pattern": "output_B_GHOST_run*.csv",
            "nodes_pattern":  "node_stats_B_GHOST_run*.csv",
        },
        "VA-GHOST": {
            "output_pattern": "output_B_VAGHOST_run*.csv",
            "nodes_pattern":  "node_stats_B_VAGHOST_run*.csv",
        },
    },
    "C_attack_moderate": {
        "LCR": {
            "output_pattern": "output_C_LCR_run*.csv",
            "nodes_pattern":  "node_stats_C_LCR_run*.csv",
        },
        "GHOST": {
            "output_pattern": "output_C_GHOST_run*.csv",
            "nodes_pattern":  "node_stats_C_GHOST_run*.csv",
        },
        "VA-GHOST": {
            "output_pattern": "output_C_VAGHOST_run*.csv",
            "nodes_pattern":  "node_stats_C_VAGHOST_run*.csv",
        },
    },
    "D_attack_strong": {
        "LCR": {
            "output_pattern": "output_D_LCR_run*.csv",
            "nodes_pattern":  "node_stats_D_LCR_run*.csv",
        },
        "GHOST": {
            "output_pattern": "output_D_GHOST_run*.csv",
            "nodes_pattern":  "node_stats_D_GHOST_run*.csv",
        },
        "VA-GHOST": {
            "output_pattern": "output_D_VAGHOST_run*.csv",
            "nodes_pattern":  "node_stats_D_VAGHOST_run*.csv",
        },
    }
}


# -------------------------------------------------------
# Helper functions
# -------------------------------------------------------

def summarize_output(out: pd.DataFrame):
    """
    Return a dict of key metrics averaged over rows.

    Requires columns:
      - "Stale/Uncle Rate"
      - "Block Time"
      - "50% Block Propagation Delay"
      - "90% Block Propagation Delay"
      - "Avg Reorg Depth"
      - "Reorg Count"
      - "Max Reorg Depth"
    """
    return {
        "avg_stale_or_uncle_rate": out["Stale/Uncle Rate"].mean(),
        "avg_block_time":          out["Block Time"].mean(),
        "avg_50p_BPD":             out["50% Block Propagation Delay"].mean(),
        "avg_90p_BPD":             out["90% Block Propagation Delay"].mean(),
        "avg_reorg_depth":         out["Avg Reorg Depth"].mean(),
        "avg_reorg_count":         out["Reorg Count"].mean(),
        "max_reorg_depth":         out["Max Reorg Depth"].max(),
    }


def fairness_stats_single(nodes: pd.DataFrame):
    """
    Fairness stats for ONE node_stats file.
    """
    hp  = nodes["hashPower"].to_numpy(dtype=float)
    bal = nodes["balance"].to_numpy(dtype=float)

    is_att = nodes["isAttacker"].to_numpy(dtype=int) if "isAttacker" in nodes.columns \
             else np.zeros_like(hp, dtype=int)

    # Global fairness
    hp_sum = hp.sum()
    bal_sum = bal.sum()

    hp_share  = hp / hp_sum if hp_sum > 0 else np.zeros_like(hp)
    bal_share = bal / bal_sum if bal_sum > 0 else np.zeros_like(bal)

    if np.all(hp_share == 0) or np.all(bal_share == 0):
        corr = np.nan
    else:
        corr = np.corrcoef(hp_share, bal_share)[0, 1]

    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.where(hp_share > 0, bal_share / hp_share, np.nan)

    # Gini on balances
    sorted_bal = np.sort(bal)
    n = len(sorted_bal)
    if n == 0 or sorted_bal.sum() == 0:
        gini = 0.0
    else:
        index = np.arange(1, n + 1)
        gini = (2 * np.sum(index * sorted_bal) / (n * sorted_bal.sum())) - (n + 1) / n

    def group_stats(mask):
        if mask.sum() == 0:
            return {"ratio_mean": np.nan, "ratio_std": np.nan, "total_reward_share": np.nan}

        hp_g  = hp[mask]
        bal_g = bal[mask]

        hp_share_g  = hp_g / hp_sum if hp_sum > 0 else np.zeros_like(hp_g)
        bal_share_g = bal_g / bal_sum if bal_sum > 0 else np.zeros_like(bal_g)

        with np.errstate(divide="ignore", invalid="ignore"):
            r = np.where(hp_share_g > 0, bal_share_g / hp_share_g, np.nan)

        return {
            "ratio_mean": np.nanmean(r),
            "ratio_std":  np.nanstd(r),
            # total reward share of this group normalized by its hash share
            "total_reward_share": bal_share_g.sum() / hp_share_g.sum()
                                  if hp_share_g.sum() > 0 else np.nan
        }

    attacker = group_stats(is_att == 1)
    honest   = group_stats(is_att == 0)

    return {
        "reward_hash_corr":              corr,
        "reward_hash_ratio_mean":        np.nanmean(ratio),
        "reward_hash_ratio_std":         np.nanstd(ratio),
        "gini_rewards":                  gini,
        "attacker_ratio_mean":           attacker["ratio_mean"],
        "attacker_ratio_std":            attacker["ratio_std"],
        "attacker_total_reward_vs_hash": attacker["total_reward_share"],
        "honest_total_reward_vs_hash":   honest["total_reward_share"],
    }


def aggregate_fairness_over_runs(node_files):
    """
    Given a list of node_stats CSVs for the same scenario+policy,
    compute fairness_stats for each and then average the metrics.
    """
    stats_list = []
    for f in node_files:
        nodes = pd.read_csv(f)
        stats = fairness_stats_single(nodes)
        stats_list.append(stats)

    # Average each key across runs
    keys = stats_list[0].keys()
    agg = {}
    for k in keys:
        vals = np.array([s[k] for s in stats_list], dtype=float)
        agg[k] = np.nanmean(vals)
    return agg


# -------------------------------------------------------
# Scenario analysis
# -------------------------------------------------------

def analyze_scenario(scen_name: str, scen_cfg: dict):
    print(f"\n==============================")
    print(f"Scenario: {scen_name}")
    print(f"==============================")

    summaries = {}
    fairness  = {}

    for policy, cfg in scen_cfg.items():
        out_files  = sorted(glob.glob(cfg["output_pattern"]))
        node_files = sorted(glob.glob(cfg["nodes_pattern"]))

        if not out_files:
            print(f"[WARN] No output files for {scen_name} / {policy}: {cfg['output_pattern']}")
            continue
        if not node_files:
            print(f"[WARN] No node_stats files for {scen_name} / {policy}: {cfg['nodes_pattern']}")
            continue

        # Concatenate all output_*_run*.csv
        outs = [pd.read_csv(f) for f in out_files]
        out_all = pd.concat(outs, ignore_index=True)

        summaries[policy] = summarize_output(out_all)
        fairness[policy]  = aggregate_fairness_over_runs(node_files)

    if not summaries:
        print(f"[WARN] No valid results for scenario {scen_name}.")
        return

    # ---- Print metrics to console ----
    for policy in summaries:
        print(f"\n--- {policy} ---")
        for k, v in summaries[policy].items():
            print(f"{k}: {v:.6f}")
        for k, v in fairness[policy].items():
            print(f"{k}: {v:.6f}")

    # ---- Save a summary CSV per scenario (like the old script) ----
    rows = []
    for policy in summaries:
        row = {"policy": policy}
        row.update(summaries[policy])
        row.update(fairness[policy])
        rows.append(row)

    df_summary = pd.DataFrame(rows)
    summary_path = f"summary_{scen_name}.csv"
    df_summary.to_csv(summary_path, index=False)
    print(f"\n[Scenario {scen_name}] Summary saved to {summary_path}")

    # ---- Plots ----
    policies = list(summaries.keys())
    stale_rates    = [summaries[p]["avg_stale_or_uncle_rate"] for p in policies]
    reorg_depths   = [summaries[p]["avg_reorg_depth"] for p in policies]
    fairness_ratio = [fairness[p]["reward_hash_ratio_mean"] for p in policies]

    # 1) Stale/uncle rate
    plt.figure()
    plt.bar(policies, stale_rates)
    plt.ylabel("Average stale/uncle rate")
    plt.title(f"Stale/uncle rate comparison ({scen_name})")
    plt.tight_layout()
    plt.savefig(f"stale_rate_comparison_{scen_name}.png")

    # 2) Average reorg depth
    plt.figure()
    plt.bar(policies, reorg_depths)
    plt.ylabel("Average reorg depth")
    plt.title(f"Reorg depth comparison ({scen_name})")
    plt.tight_layout()
    plt.savefig(f"reorg_depth_comparison_{scen_name}.png")

    # 3) Reward/hash ratio
    plt.figure()
    plt.bar(policies, fairness_ratio)
    plt.axhline(1.0, linestyle="--")  # ideal fairness
    plt.ylabel("E[reward share / hash share]")
    plt.title(f"Fairness comparison ({scen_name})")
    plt.tight_layout()
    plt.savefig(f"fairness_ratio_comparison_{scen_name}.png")

    print(f"\n[Scenario {scen_name}] Plots saved:")
    print(f"  stale_rate_comparison_{scen_name}.png")
    print(f"  reorg_depth_comparison_{scen_name}.png")
    print(f"  fairness_ratio_comparison_{scen_name}.png")


def main():
    for scen_name, scen_cfg in SCENARIOS.items():
        analyze_scenario(scen_name, scen_cfg)


if __name__ == "__main__":
    main()
