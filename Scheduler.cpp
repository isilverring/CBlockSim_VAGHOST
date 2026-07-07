#include "Scheduler.h"
#include <random>
#include <map>
#include <iostream>
#include <queue>
#include <algorithm>
#include <cmath>
#include <string.h>
#include "Policy/BlockPolicy.h"

Scheduler::Scheduler()
{
    halfBPD = 0;
    ninetyBPD = 0;

    globalchain = -1;
    totalGenBlocks = 1;
    count = 0;

    if(!NETWORK) topoGenerator.ProduceCompleteTopo(topo);
    if(DATA) txGenerator.ProduceTXs(txPool);
    if(NETWORK) topoGenerator.ProduceSmallWorldTopo(MAX_DISTANCE, BETA, DENSITY, topo);
    topoGenerator.ProduceNodesByDistrib(nodePool, topo);

    // Genesis is public by definition.
    publicBlocks.insert(0);
}

void Scheduler::GenerateInitialEvents()
{
    for (int i = 0; i < NODES_NUM; i++) GenBlock(i, 0);
}

int Scheduler::PublicHeadDepth() const
{
    int best = 0;
    for (int bid : publicBlocks) {
        if (bid == 0) continue;
        auto it = blockPool.find(bid);
        if (it != blockPool.end()) best = std::max(best, it->second->depth);
    }
    return best;
}

int Scheduler::PrivateHeadDepth() const
{
    int best = 0;
    for (const auto &ab : attackBuffer) {
        if (!ab.propagated && ab.block) best = std::max(best, ab.block->depth);
    }
    return best;
}

void Scheduler::ReleaseBufferedAttackBlocks(double now)
{
    if (ATTACK_SCENARIO != 1) return;

    bool hasUnreleased = false;
    bool timeoutRelease = false;
    for (const auto &ab : attackBuffer) {
        if (!ab.propagated) {
            hasUnreleased = true;
            if (ab.releaseTime <= now) timeoutRelease = true;
        }
    }
    if (!hasUnreleased) return;

    const int publicDepth  = PublicHeadDepth();
    const int privateDepth = PrivateHeadDepth();
    const int privateLead  = privateDepth - publicDepth;

    // Release policy for the withheld coalition:
    //  (i) forced release after ATTACK_HOLD_TIME,
    //  (ii) strategic release once the private chain has a clear lead,
    //  (iii) defensive release if the public chain is about to catch up.
    const bool leadRelease = (privateLead >= ATTACK_RELEASE_LEAD);
    const bool riskRelease = (publicDepth >= privateDepth - 1);
    if (!(timeoutRelease || leadRelease || riskRelease)) return;

    std::vector<AttackBlock*> toRelease;
    for (auto &ab : attackBuffer) {
        if (!ab.propagated) toRelease.push_back(&ab);
    }

    std::sort(toRelease.begin(), toRelease.end(), [](const AttackBlock *a, const AttackBlock *b) {
        if (a->block->depth != b->block->depth) return a->block->depth < b->block->depth;
        if (a->block->timestamp != b->block->timestamp) return a->block->timestamp < b->block->timestamp;
        return a->block->id < b->block->id;
    });

    for (auto *ab : toRelease) {
        publicBlocks.insert(ab->block->id);
        // Released privately mined blocks are propagated at the RELEASE time, not the mining time.
        PropagateBlock(ab->block, now, /*honestOnly=*/true, /*attackersOnly=*/false);
        ab->propagated = true;
    }
}

void Scheduler::HandleEvent(multiset<Event>::iterator e)
{
    // Release withheld coalition blocks when their release policy triggers.
    ReleaseBufferedAttackBlocks(e->timestamp);

    if (e->type == GENERATE_BLOCK) GenerateBlock(e);
    else if (e->type == RECEIVE_BLOCK) ReceiveBlock(e);
}

void Scheduler::GenerateBlock(multiset<Event>::iterator e)
{
    if (e->block->prevBlock == nodePool[e->owner]->mainchain.back()->id) {
        if(DATA) {
            txnPolicy.PropagationTxn(e->timestamp, nodePool, txPool);
            txnPolicy.SelectTxn(e, nodePool, txPool);
        }
        if(FLAG == 1) nodePool[e->owner]->AddUncles(e->block);
        nodePool[e->owner]->AddBlock(e->block);
        bkPolicy.UpdateTransactionPool(e->owner, e->block, nodePool);

        bool isAttackNode = (ATTACK_SCENARIO == 1 && nodePool[e->owner]->isAttacker);

        if (isAttackNode) {
            // Withhold from honest nodes but share immediately inside the attacking coalition.
            AttackBlock ab;
            ab.block       = e->block;
            ab.releaseTime = e->timestamp + ATTACK_HOLD_TIME;
            ab.propagated  = false;
            attackBuffer.push_back(ab);

            PropagateBlock(e->block, e->timestamp, /*honestOnly=*/false, /*attackersOnly=*/true);
        } else {
            publicBlocks.insert(e->block->id);
            PropagateBlock(e->block, e->timestamp, /*honestOnly=*/false, /*attackersOnly=*/false);
        }

        nodePool[e->owner]->hasMiningEvent = false;
        GenBlock(e->owner, e->timestamp);
        totalGenBlocks++;
        if(e->timestamp > 0) blockPool[e->block->id] = e->block;
    }
}

void Scheduler::ReceiveBlock(multiset<Event>::iterator e)
{
    bkPolicy.FinalizeBlock(e, nodePool, evEngine);
}

void Scheduler::GenBlock(int miner, double currentTime)
{
    bkPolicy.GenBlock(miner, currentTime, nodePool, evEngine);
}

void Scheduler::PropagateBlock(shared_ptr<Block> block, double sendTime, bool honestOnly, bool attackersOnly)
{
    static std::mt19937 gen(2026);
    std::exponential_distribution<double> GenTime(1.0 / AVER_DELAY);
    double delay;
    double delays[NODES_NUM];
    if(NETWORK) bkPolicy.PropagateBlock(block, topo, nodePool, delays);

    std::vector<double> bpd;
    for (int i = 0; i < (int)nodePool.size(); i++) {
        if (block->miner == nodePool[i]->id) continue;
        if (honestOnly && nodePool[i]->isAttacker) continue;
        if (attackersOnly && !nodePool[i]->isAttacker) continue;

        Event e;
        e.block = block;
        e.owner = nodePool[i]->id;
        if(!NETWORK) delay = GenTime(gen);
        else delay = delays[i];

        if (!attackersOnly) bpd.push_back(delay);
        e.timestamp = sendTime + delay;
        e.type = RECEIVE_BLOCK;
        evEngine.AddEvent(e);
    }

    // Only public propagation contributes to the observed BPD metrics.
    if (!attackersOnly && !bpd.empty()) {
        sort(bpd.begin(), bpd.end());
        if ((bpd.size()) % 2 == 0) {
            halfBPD += (bpd[bpd.size()/2 - 1] + bpd[bpd.size()/2]) / 2.0;
        } else {
            halfBPD += bpd[bpd.size()/2];
        }
        ninetyBPD += bpd[(int)(bpd.size() * 0.9)];
        count++;
    }
}

void Scheduler::ResolveFork()
{
    if(FINALIZE_POLICY == 0) {
        map<int, int> counter;
        int maxDepth = 0;
        for (int i = 0; i < NODES_NUM; i++) {
            if (nodePool[i]->mainchain.back()->depth > maxDepth) maxDepth = nodePool[i]->mainchain.back()->depth;
        }
        for (int i = 0; i < NODES_NUM; i++) {
            if (nodePool[i]->mainchain.back()->depth == maxDepth) {
                if (counter.find(nodePool[i]->mainchain.back()->miner) == counter.end()) counter[nodePool[i]->mainchain.back()->miner] = 1;
                else counter[nodePool[i]->mainchain.back()->miner]++;
            }
        }
        int num = 0, nid = 0;
        for (auto iter = counter.begin(); iter != counter.end(); iter++) {
            if (iter->second > num) {
                num = iter->second;
                nid = iter->first;
            }
        }
        for (int i = 0; i < NODES_NUM; i++) {
            if (nodePool[i]->mainchain.back()->depth == maxDepth && nodePool[i]->mainchain.back()->miner == nid) {
                globalchain = i;
                break;
            }
        }
    } else if(FINALIZE_POLICY == 1) {
        map<int, int> counter;
        int maxDiff = 0;
        for (int i = 0; i < NODES_NUM; i++) {
            if (nodePool[i]->mainchain.back()->diff > maxDiff) maxDiff = nodePool[i]->mainchain.back()->diff;
        }
        for (int i = 0; i < NODES_NUM; i++) {
            if (nodePool[i]->mainchain.back()->diff == maxDiff) {
                if (counter.find(nodePool[i]->mainchain.back()->miner) == counter.end()) counter[nodePool[i]->mainchain.back()->miner] = 1;
                else counter[nodePool[i]->mainchain.back()->miner]++;
            }
        }
        int num = 0, nid = 0;
        for (auto iter = counter.begin(); iter != counter.end(); iter++) {
            if (iter->second > num) {
                num = iter->second;
                nid = iter->first;
            }
        }
        for (int i = 0; i < NODES_NUM; i++) {
            if (nodePool[i]->mainchain.back()->diff == maxDiff && nodePool[i]->mainchain.back()->miner == nid) {
                globalchain = i;
                break;
            }
        }
    } else if(FINALIZE_POLICY == 2) {
        map<int, int> headVotes;
        map<int, double> headScore;
        map<int, int> headDepth;

        for (int i = 0; i < NODES_NUM; i++) {
            int headId = bkPolicy.SelectVAGhostHead(nodePool[i].get());
            double score = bkPolicy.ComputeVAGhostWeight(nodePool[i].get());
            int depth = 0;
            auto blkIt = nodePool[i]->knownBlocks.find(headId);
            if (blkIt != nodePool[i]->knownBlocks.end()) depth = blkIt->second->depth;
            headVotes[headId]++;
            if (headScore.find(headId) == headScore.end() || score > headScore[headId]) headScore[headId] = score;
            if (headDepth.find(headId) == headDepth.end() || depth > headDepth[headId]) headDepth[headId] = depth;
        }

        int bestHead = -1;
        int bestVotes = -1;
        double bestScore = -1.0;
        int bestDepth = -1;
        for (auto &kv : headVotes) {
            int headId = kv.first;
            int votes = kv.second;
            double score = headScore[headId];
            int depth = headDepth[headId];
            bool better = false;
            if (votes > bestVotes) better = true;
            else if (votes == bestVotes && score > bestScore) better = true;
            else if (votes == bestVotes && std::fabs(score - bestScore) <= 1e-12 && depth > bestDepth) better = true;
            else if (votes == bestVotes && std::fabs(score - bestScore) <= 1e-12 && depth == bestDepth && headId < bestHead) better = true;
            if (better) {
                bestHead = headId;
                bestVotes = votes;
                bestScore = score;
                bestDepth = depth;
            }
        }

        globalchain = 0;
        for (int i = 0; i < NODES_NUM; i++) {
            if (!nodePool[i]->mainchain.empty() && nodePool[i]->mainchain.back()->id == bestHead) {
                globalchain = i;
                break;
            }
        }
    }
}

void Scheduler::DistributeRewards()
{
    for(auto iter=nodePool[globalchain]->mainchain.begin()+1; iter != nodePool[globalchain]->mainchain.end(); iter++) {
        nodePool[(*iter)->miner]->balance += BLOCK_REWARD;
        if(DATA) {
            for(int i=(*iter)->low; i<=(*iter)->high; i++) {
                if((*iter)->txs.test(i)) nodePool[(*iter)->miner]->balance += txPool[i]->fee;
            }
            if(FLAG == 1) {
                nodePool[(*iter)->miner]->balance += (*iter)->uncles.size() * UNCLE_REWARD;
                for(int i=0; i<(*iter)->uncles.size(); i++) {
                    int uncleDepth = blockPool[(*iter)->uncles[i]]->depth;
                    int blockDepth = (*iter)->depth;
                    nodePool[blockPool[(*iter)->uncles[i]]->miner]->balance += ((uncleDepth - blockDepth + 8) * BLOCK_REWARD / 8);
                }
            }
        }
    }

    std::ofstream out("node_stats.csv", std::ios::out);
    out << "node_id,location,hashPower,balance,isAttacker\n";
    for (int i = 0; i < NODES_NUM; ++i) {
        const auto &node = nodePool[i];
        out << node->id        << ","
            << node->location  << ","
            << node->hashPower << ","
            << node->balance   << ","
            << (node->isAttacker ? 1 : 0)
            << "\n";
    }
}

double Scheduler::ShowStatistics(int rd, double t)
{
    double res;
    int mainBlocks = nodePool[globalchain]->mainchain.size();
    int uncleBlocks = 0;
    double averBlockSize = 0;

    for(auto iter = nodePool[globalchain]->mainchain.begin();
             iter != nodePool[globalchain]->mainchain.end();
             iter++) {
        uncleBlocks   += (*iter)->uncles.size();
        averBlockSize += (*iter)->size;
    }

    int staleBlocks  = totalGenBlocks - mainBlocks;
    double uncleRate = (double)uncleBlocks / (mainBlocks + uncleBlocks);
    double staleRate = (double)staleBlocks / totalGenBlocks;
    averBlockSize   /= mainBlocks;

    int totalReorgs        = 0;
    int totalReorgDepthSum = 0;
    int globalMaxReorgDepth = 0;

    for (int i = 0; i < NODES_NUM; ++i) {
        totalReorgs        += nodePool[i]->reorgCount;
        totalReorgDepthSum += nodePool[i]->reorgDepthSum;
        if (nodePool[i]->maxReorgDepth > globalMaxReorgDepth)
            globalMaxReorgDepth = nodePool[i]->maxReorgDepth;
    }

    double avgReorgDepth = 0.0;
    if (totalReorgs > 0) avgReorgDepth = (double)totalReorgDepthSum / (double)totalReorgs;

    if(FLAG == 0) {
        res = staleRate;
        st.write(rd, t, (double)SIM_TIME / totalGenBlocks, averBlockSize, mainBlocks, staleBlocks,
                 halfBPD / std::max(1, count), ninetyBPD / std::max(1, count), staleRate,
                 avgReorgDepth, globalMaxReorgDepth, totalReorgs);
    } else {
        res = uncleRate;
        st.write(rd, t, (double)SIM_TIME / totalGenBlocks, averBlockSize, mainBlocks, uncleBlocks,
                 halfBPD / std::max(1, count), ninetyBPD / std::max(1, count), uncleRate,
                 avgReorgDepth, globalMaxReorgDepth, totalReorgs);
    }

    return res;
}
