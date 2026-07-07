#include "BlockPolicy.h"
#include <random>
#include <queue>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

namespace {
    // VA-GHOST parameters aligned with the paper notation.
    constexpr int    TAU_V  = 4;    // visibility threshold τ_v
    constexpr int    L_MAX  = 32;   // certificate size bound L_max
    constexpr double DELTA  = 0.5;  // piecewise threshold δ
    constexpr double ALPHA  = 0.5;  // low-visibility discount α
    constexpr double EPS    = 1e-12;

    double visibilityFactor(Node *node, int blockId)
    {
        auto it = node->visibility_count.find(blockId);
        int k = (it != node->visibility_count.end()) ? it->second : 0;
        return std::min(1.0, static_cast<double>(k) / static_cast<double>(TAU_V));
    }

    double visibilityDiscount(double nu)
    {
        return (nu < DELTA) ? (ALPHA * nu) : nu;
    }

    double descendantContribution(Node *node, int blockId)
    {
        auto it = node->knownBlocks.find(blockId);
        if (it == node->knownBlocks.end()) return 0.0;
        return visibilityDiscount(visibilityFactor(node, blockId));
    }

    // Recursive contribution of the subtree rooted at blockId when blockId is treated
    // as a DESCENDANT of some candidate root: g(nu_blockId) + discounted descendants.
    double subtreeContributionDfs(Node *node, int blockId, std::unordered_map<int, double> &memo)
    {
        auto memIt = memo.find(blockId);
        if (memIt != memo.end()) return memIt->second;

        double total = descendantContribution(node, blockId);
        auto it = node->childBlocks.find(blockId);
        if (it != node->childBlocks.end()) {
            for (int childId : it->second) {
                if (node->knownBlocks.find(childId) == node->knownBlocks.end()) continue;
                total += subtreeContributionDfs(node, childId, memo);
            }
        }
        memo[blockId] = total;
        return total;
    }

    // Exact VA-GHOST subtree weight from the paper:
    //   W_i^{VA}(b,t) = 1 + sum_{d in Desc_i(b,t)} g(nu_d^{(i)}(t))
    // The root block b itself contributes the constant 1 and is NOT visibility-discounted.
    double subtreeWeightAtRoot(Node *node, int rootId, std::unordered_map<int, double> &memo)
    {
        double total = 1.0;
        auto it = node->childBlocks.find(rootId);
        if (it != node->childBlocks.end()) {
            for (int childId : it->second) {
                if (node->knownBlocks.find(childId) == node->knownBlocks.end()) continue;
                total += subtreeContributionDfs(node, childId, memo);
            }
        }
        return total;
    }

    int chooseBestChild(Node *node, int parentId, std::unordered_map<int, double> &memo)
    {
        auto it = node->childBlocks.find(parentId);
        if (it == node->childBlocks.end() || it->second.empty()) return -1;

        int bestId = -1;
        double bestWeight = -1.0;
        int bestDepth = -1;
        double bestTs = INF;

        for (int childId : it->second) {
            auto blkIt = node->knownBlocks.find(childId);
            if (blkIt == node->knownBlocks.end()) continue;

            double w = subtreeWeightAtRoot(node, childId, memo);
            int depth = blkIt->second->depth;
            double ts = blkIt->second->timestamp;

            bool better = false;
            if (w > bestWeight + EPS) better = true;
            else if (std::fabs(w - bestWeight) <= EPS) {
                if (depth > bestDepth) better = true;
                else if (depth == bestDepth) {
                    if (ts < bestTs - EPS) better = true;
                    else if (std::fabs(ts - bestTs) <= EPS && childId < bestId) better = true;
                }
            }

            if (better) {
                bestId = childId;
                bestWeight = w;
                bestDepth = depth;
                bestTs = ts;
            }
        }
        return bestId;
    }

    void updateVisibilityForReceivedBlock(multiset<Event>::iterator e, vector<unique_ptr<Node>> &nodePool)
    {
        auto &node = nodePool[e->owner];
        auto blk = e->block;

        // Seeing the block itself counts as one observation.
        node->visibility_count[blk->id]++;

        // Only attest to blocks already recognized in the local DAG, as specified in the paper.
        for (int refId : blk->visibility_cert) {
            if (node->knownBlocks.find(refId) != node->knownBlocks.end()) {
                node->visibility_count[refId]++;
            }
        }
    }

    void buildVisibilityCertificate(int miner,
                                    vector<unique_ptr<Node>> &nodePool,
                                    shared_ptr<Block> &newBlock)
    {
        auto &node = nodePool[miner];
        vector<shared_ptr<Block>> candidates;
        candidates.reserve(node->knownBlocks.size());

        for (auto &kv : node->knownBlocks) {
            auto blk = kv.second;
            if (blk->id == newBlock->id) continue;
            if (blk->id == 0) continue; // omit genesis from certificates
            auto it = node->visibility_count.find(blk->id);
            int k = (it != node->visibility_count.end()) ? it->second : 0;
            if (k >= TAU_V) candidates.push_back(blk);
        }

        // Deterministic public reduction rule: decreasing depth, then lexicographic block id.
        std::sort(candidates.begin(), candidates.end(), [](const shared_ptr<Block> &a, const shared_ptr<Block> &b) {
            if (a->depth != b->depth) return a->depth > b->depth;
            return a->id < b->id;
        });

        newBlock->visibility_cert.clear();
        for (auto &blk : candidates) {
            newBlock->visibility_cert.push_back(blk->id);
            if ((int)newBlock->visibility_cert.size() >= L_MAX) break;
        }
    }

    int commonPrefixLength(const vector<shared_ptr<Block>> &a, const vector<shared_ptr<Block>> &b)
    {
        int i = 0;
        while (i < (int)a.size() && i < (int)b.size() && a[i]->id == b[i]->id) ++i;
        return i;
    }
} // namespace

BlockPolicy::BlockPolicy()
{
    nextBKID = 1;
}

vector<shared_ptr<Block>> BlockPolicy::ComputeVAGhostPreferredChain(Node* node)
{
    vector<shared_ptr<Block>> chain;
    auto itGenesis = node->knownBlocks.find(0);
    if (itGenesis == node->knownBlocks.end()) return chain;

    chain.push_back(itGenesis->second);
    int current = 0;
    unordered_map<int, double> memo;
    unordered_set<int> seen;
    seen.insert(0);

    while (true) {
        int bestChild = chooseBestChild(node, current, memo);
        if (bestChild < 0) break;
        if (seen.count(bestChild)) break;
        auto blkIt = node->knownBlocks.find(bestChild);
        if (blkIt == node->knownBlocks.end()) break;
        chain.push_back(blkIt->second);
        seen.insert(bestChild);
        current = bestChild;
    }

    return chain;
}

int BlockPolicy::SelectVAGhostHead(Node* node)
{
    auto chain = ComputeVAGhostPreferredChain(node);
    if (chain.empty()) return 0;
    return chain.back()->id;
}

double BlockPolicy::ComputeVAGhostWeight(Node* node)
{
    // Aggregate the branch scores encountered during the greedy VA-GHOST descent.
    // This is used only as a simulator-side tie-break when multiple nodes prefer
    // different heads with equal vote counts in ResolveFork().
    double total = 0.0;
    int current = 0;
    std::unordered_map<int, double> memo;
    std::unordered_set<int> seen;
    seen.insert(0);

    while (true) {
        int bestChild = chooseBestChild(node, current, memo);
        if (bestChild < 0) break;
        if (seen.count(bestChild)) break;
        total += subtreeWeightAtRoot(node, bestChild, memo);
        seen.insert(bestChild);
        current = bestChild;
    }
    return total;
}

void BlockPolicy::GenBlock(int miner, double currentTime, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    if(PROPOSAL_POLICY == 0) LoadPoW(miner, currentTime, nodePool, evEngine);
    if(PROPOSAL_POLICY == 1) LoadPoS(miner, currentTime, nodePool, evEngine);
}

void BlockPolicy::LoadPoW(int miner, double currentTime, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    static std::vector<std::mt19937> gens;
    if (gens.empty()) {
        gens.reserve(NODES_NUM);
        for (int i = 0; i < NODES_NUM; ++i) gens.emplace_back(HASH_ASSIGN_SEED + 1000 + i);
    }
    exponential_distribution<double> GenTime(nodePool[miner]->hashPower / BLOCK_INTERVAL);

    Event e;
    if (nodePool[miner]->hashPower > 0) {
        e.owner     = miner;
        e.type      = GENERATE_BLOCK;
        e.timestamp = currentTime + GenTime(gens[miner]);

        if (e.timestamp <= SIM_TIME) {
            e.block            = make_shared<Block>();
            e.block->id        = nextBKID++;
            e.block->depth     = nodePool[miner]->mainchain.back()->depth + 1;
            e.block->diff      = nodePool[miner]->mainchain.back()->diff + 1;
            e.block->miner     = miner;
            e.block->prevBlock = nodePool[miner]->mainchain.back()->id;
            e.block->size      = BLOCK_SIZE;
            e.block->timestamp = e.timestamp;

            if (FINALIZE_POLICY == 2) {
                buildVisibilityCertificate(miner, nodePool, e.block);
            }

            // Self-attestation/observation for the newly mined block.
            nodePool[miner]->visibility_count[e.block->id]++;

            nodePool[miner]->miningEvent    = evEngine.AddEvent(e);
            nodePool[miner]->hasMiningEvent = true;
        }
    }
}

void BlockPolicy::LoadPoS(int miner, double currentTime, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    static std::vector<std::mt19937> gens;
    if (gens.empty()) {
        gens.reserve(NODES_NUM);
        for (int i = 0; i < NODES_NUM; ++i) gens.emplace_back(HASH_ASSIGN_SEED + 2000 + i);
    }
    double totalStake = 0;
    for(int i=0; i<nodePool.size(); i++) totalStake += nodePool[i]->stake * (currentTime-nodePool[i]->stakeTime);
    exponential_distribution<double> GenTime(nodePool[miner]->stake*(currentTime-nodePool[miner]->stakeTime) / totalStake / BLOCK_INTERVAL);
    Event e;
    if (nodePool[miner]->hashPower > 0) {
        e.owner = miner;
        e.type = GENERATE_BLOCK;
        e.timestamp = currentTime + GenTime(gens[miner]);
        if (e.timestamp <= SIM_TIME) {
            e.block = make_shared<Block>();
            e.block->id = nextBKID++;
            e.block->depth = nodePool[miner]->mainchain.back()->depth + 1;
            e.block->diff = nodePool[miner]->mainchain.back()->diff + 1;
            e.block->miner = miner;
            e.block->prevBlock = nodePool[miner]->mainchain.back()->id;
            e.block->size = BLOCK_SIZE;
            e.block->timestamp = e.timestamp;

            if (FINALIZE_POLICY == 2) {
                buildVisibilityCertificate(miner, nodePool, e.block);
            }

            nodePool[miner]->visibility_count[e.block->id]++;
            nodePool[miner]->miningEvent = evEngine.AddEvent(e);
            nodePool[miner]->hasMiningEvent = true;
            nodePool[miner]->stakeTime = currentTime;
        }
    }
}

void BlockPolicy::PropagateBlock(shared_ptr<Block> block, double topo[NODES_NUM][NODES_NUM], vector<unique_ptr<Node>> &nodePool, double delays[NODES_NUM])
{
    if(PROPAGATION_POLICY == 0) LoadBitcoinPropagation(block, topo, nodePool, delays);
    if(PROPAGATION_POLICY == 1) LoadEthereumPropagation(block, topo, nodePool, delays);
}

void BlockPolicy::LoadBitcoinPropagation(shared_ptr<Block> block, double topo[NODES_NUM][NODES_NUM], vector<unique_ptr<Node>> &nodePool, double delays[NODES_NUM])
{
    srand(time(0));
    vector<int> s;
    priority_queue<pair<double, int>, vector<pair<double, int>>, greater<pair<double, int>>> u;
    int v = block->miner;
    s.push_back(v);
    for (int i = 0; i < NODES_NUM; i++) {
        if (i == v) delays[i] = 0;
        else if (topo[v][i] != INF) {
            int loc_from = nodePool[v]->location, loc_to = nodePool[i]->location;
            double bandwidth;
            if (loc_from == loc_to) bandwidth = min(UPLOAD_BANDWIDTHS[REGIONS_NUM], DOWNLOAD_BANDWIDTHS[REGIONS_NUM]);
            else bandwidth = min(UPLOAD_BANDWIDTHS[loc_from], DOWNLOAD_BANDWIDTHS[loc_to]);
            delays[i] = 3*topo[v][i] + BLOCK_VALIDATION * block->size;
            u.push(make_pair(delays[i], i));
        }
        else delays[i] = INF;
    }
    while (s.size() != NODES_NUM) {
        double dis = u.top().first;
        int update_nid = u.top().second;
        u.pop();
        s.push_back(update_nid);
        for (int i = 0; i < NODES_NUM; i++) {
            if (topo[update_nid][i] != INF) {
                double new_dis = dis + 3*topo[update_nid][i] + BLOCK_VALIDATION*block->size;
                if (delays[i] > new_dis) {
                    delays[i] = new_dis;
                    u.push(make_pair(new_dis, i));
                }
            }
        }
    }
    random_device rd;
    mt19937 gen(rd());
    for(int i=0; i<NODES_NUM; i++) {
        exponential_distribution<double> GenTime(1.0 / delays[i]);
        delays[i] = GenTime(gen);
    }
}

void BlockPolicy::LoadEthereumPropagation(shared_ptr<Block> block, double topo[NODES_NUM][NODES_NUM], vector<unique_ptr<Node>> &nodePool, double delays[NODES_NUM])
{
    srand(time(0));
    vector<int> s;
    priority_queue<pair<double, int>, vector<pair<double, int>>, greater<pair<double, int>>> u;
    int v = block->miner;
    s.push_back(v);
    for (int i = 0; i < NODES_NUM; i++) {
        if (i == v) delays[i] = 0;
        else if (topo[v][i] != INF) {
            int loc_from = nodePool[v]->location, loc_to = nodePool[i]->location;
            double bandwidth;
            if (loc_from == loc_to) bandwidth = min(UPLOAD_BANDWIDTHS[REGIONS_NUM], DOWNLOAD_BANDWIDTHS[REGIONS_NUM]);
            else bandwidth = min(UPLOAD_BANDWIDTHS[loc_from], DOWNLOAD_BANDWIDTHS[loc_to]);
            if(rand() % nodePool[v]->neighborNum + 1 <= (int)sqrt(nodePool[v]->neighborNum)) delays[i] = topo[v][i] + block->size / bandwidth + BLOCK_VALIDATION * block->size;
            else delays[i] = 5 * topo[v][i] + block->size / bandwidth + 0.5 + BLOCK_VALIDATION * block->size;
            u.push(make_pair(delays[i], i));
        }
        else delays[i] = INF;
    }
    while (s.size() != NODES_NUM) {
        double dis = u.top().first;
        int update_nid = u.top().second;
        u.pop();
        s.push_back(update_nid);
        for (int i = 0; i < NODES_NUM; i++) {
            if (topo[update_nid][i] != INF) {
                int loc_from = nodePool[update_nid]->location, loc_to = nodePool[i]->location;
                double bandwidth;
                if (loc_from == loc_to) bandwidth = min(UPLOAD_BANDWIDTHS[REGIONS_NUM], DOWNLOAD_BANDWIDTHS[REGIONS_NUM]);
                else bandwidth = min(UPLOAD_BANDWIDTHS[loc_from], DOWNLOAD_BANDWIDTHS[loc_to]);
                double new_dis;
                if (rand() % nodePool[update_nid]->neighborNum + 1 <= (int)sqrt(nodePool[update_nid]->neighborNum)) {
                    new_dis = dis + topo[update_nid][i] + block->size / bandwidth + BLOCK_VALIDATION * block->size;
                } else {
                    new_dis = dis + 5 * topo[update_nid][i] + block->size / bandwidth + BLOCK_VALIDATION * block->size + 0.5;
                }
                if (delays[i] > new_dis) {
                    delays[i] = new_dis;
                    u.push(make_pair(new_dis, i));
                }
            }
        }
    }
    random_device rd;
    mt19937 gen(rd());
    for(int i=0; i<NODES_NUM; i++) {
        exponential_distribution<double> GenTime(1.0 / delays[i]);
        delays[i] = GenTime(gen);
    }
}

void BlockPolicy::FinalizeBlock(multiset<Event>::iterator e, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    // Every node remembers every block it receives in its local DAG view.
    nodePool[e->owner]->RememberBlock(e->block);

    // VA-GHOST visibility state is updated irrespective of the selected finalization policy.
    updateVisibilityForReceivedBlock(e, nodePool);

    if(FINALIZE_POLICY == 0) LoadLongestRule(e, nodePool, evEngine);
    if(FINALIZE_POLICY == 1) LoadGHOSTRule(e, nodePool, evEngine);
    if(FINALIZE_POLICY == 2) LoadVAGHOSTRule(e, nodePool, evEngine);
}

void BlockPolicy::LoadLongestRule(multiset<Event>::iterator e, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    if (e->block->prevBlock == nodePool[e->owner]->mainchain.back()->id) {
        nodePool[e->owner]->AddBlock(e->block);
        UpdateTransactionPool(e->owner, e->block, nodePool);
        if (nodePool[e->owner]->hasMiningEvent) {
            evEngine.RemoveEvent(nodePool[e->owner]->miningEvent);
            nodePool[e->owner]->hasMiningEvent = false;
        }
        GenBlock(e->owner, e->timestamp, nodePool, evEngine);
        if(FLAG == 1) {
            for (auto iter = e->block->uncles.begin(); iter != e->block->uncles.end(); ++iter) {
                nodePool[e->owner]->includedUncles[*iter] = true;
            }
        }
    }
    else if(e->block->depth > nodePool[e->owner]->mainchain.back()->depth) {
        int i, j;
        for (i = nodePool[e->owner]->mainchain.size()-1; i >= 0; --i) {
            if (nodePool[e->owner]->mainchain[i]->id == nodePool[e->block->miner]->mainchain[i]->id) break;
        }
        {
            auto &node = nodePool[e->owner];
            int oldLen     = node->mainchain.size();
            int reorgDepth = oldLen - 1 - i;
            if (reorgDepth < 0) reorgDepth = 0;
            node->reorgCount += 1;
            node->reorgDepthSum += reorgDepth;
            if (reorgDepth > node->maxReorgDepth) node->maxReorgDepth = reorgDepth;
        }
        for (j = i+1; j < nodePool[e->owner]->mainchain.size(); ++j) {
            if(FLAG == 1) {
                nodePool[e->owner]->unclechain.push_back(nodePool[e->owner]->mainchain[j]);
                for (auto iter = nodePool[e->owner]->mainchain[j]->uncles.begin(); iter != nodePool[e->owner]->mainchain[j]->uncles.end(); ++iter) {
                    auto uIter = nodePool[e->owner]->includedUncles.find(*iter);
                    if (uIter != nodePool[e->owner]->includedUncles.end()) nodePool[e->owner]->includedUncles.erase(uIter);
                }
            }
            ReleaseTransactions(e->owner, nodePool[e->owner]->mainchain[j], nodePool);
            UpdateTransactionPool(e->owner, nodePool[e->block->miner]->mainchain[j], nodePool);
            nodePool[e->owner]->mainchain[j] = nodePool[e->block->miner]->mainchain[j];
        }
        for(j = nodePool[e->owner]->mainchain.size(); j < nodePool[e->block->miner]->mainchain.size(); ++j) {
            nodePool[e->owner]->AddBlock(nodePool[e->block->miner]->mainchain[j]);
            UpdateTransactionPool(e->owner, nodePool[e->block->miner]->mainchain[j], nodePool);
            if (!nodePool[e->block->miner]->mainchain[j]->uncles.empty()) {
                for (auto iter = nodePool[e->block->miner]->mainchain[j]->uncles.begin(); iter != nodePool[e->block->miner]->mainchain[j]->uncles.end(); ++iter) {
                    nodePool[e->owner]->includedUncles[*iter] = true;
                }
            }
        }
        if (nodePool[e->owner]->hasMiningEvent) {
            evEngine.RemoveEvent(nodePool[e->owner]->miningEvent);
            nodePool[e->owner]->hasMiningEvent = false;
        }
        GenBlock(e->owner, e->timestamp, nodePool, evEngine);
    } else {
        if(FLAG == 1) nodePool[e->owner]->unclechain.push_back(e->block);
    }
}

void BlockPolicy::LoadGHOSTRule(multiset<Event>::iterator e, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    if (e->block->prevBlock == nodePool[e->owner]->mainchain.back()->id) {
        nodePool[e->owner]->AddBlock(e->block);
        UpdateTransactionPool(e->owner, e->block, nodePool);
        if (nodePool[e->owner]->hasMiningEvent) {
            evEngine.RemoveEvent(nodePool[e->owner]->miningEvent);
            nodePool[e->owner]->hasMiningEvent = false;
        }
        GenBlock(e->owner, e->timestamp, nodePool, evEngine);
    }
    else {
        if (nodePool[e->owner]->mainchain.back()->diff >= e->block->diff) {
            nodePool[e->owner]->unclechain.push_back(e->block);
        }
        else {
            int i, j;
            for (i = nodePool[e->owner]->mainchain.size()-1; i >= 0; --i) {
                if (i < nodePool[e->block->miner]->mainchain.size()) {
                    if (nodePool[e->owner]->mainchain[i]->id == nodePool[e->block->miner]->mainchain[i]->id) break;
                }
            }
            {
                auto &node = nodePool[e->owner];
                int oldLen     = node->mainchain.size();
                int reorgDepth = oldLen - 1 - i;
                if (reorgDepth < 0) reorgDepth = 0;
                node->reorgCount += 1;
                node->reorgDepthSum += reorgDepth;
                if (reorgDepth > node->maxReorgDepth) node->maxReorgDepth = reorgDepth;
            }
            for (j = i+1; j < nodePool[e->owner]->mainchain.size(); ++j) {
                nodePool[e->owner]->unclechain.push_back(nodePool[e->owner]->mainchain[j]);
                for (auto iter = nodePool[e->owner]->mainchain[j]->uncles.begin(); iter != nodePool[e->owner]->mainchain[j]->uncles.end(); ++iter) {
                    auto uIter = nodePool[e->owner]->includedUncles.find(*iter);
                    if (uIter != nodePool[e->owner]->includedUncles.end()) nodePool[e->owner]->includedUncles.erase(uIter);
                }
            }
            for (j = i+1; j < nodePool[e->block->miner]->mainchain.size(); ++j) {
                if (j < nodePool[e->owner]->mainchain.size()) {
                    UpdateTransactionPool(e->owner, nodePool[e->block->miner]->mainchain[j], nodePool);
                    nodePool[e->owner]->mainchain[j] = nodePool[e->block->miner]->mainchain[j];
                    if (!nodePool[e->block->miner]->mainchain[j]->uncles.empty()) {
                        for (auto iter = nodePool[e->block->miner]->mainchain[j]->uncles.begin(); iter != nodePool[e->block->miner]->mainchain[j]->uncles.end(); ++iter) {
                            nodePool[e->owner]->includedUncles[*iter] = true;
                        }
                    }
                }
                else {
                    UpdateTransactionPool(e->owner, nodePool[e->block->miner]->mainchain[j], nodePool);
                    nodePool[e->owner]->AddBlock(nodePool[e->block->miner]->mainchain[j]);
                    if (!nodePool[e->block->miner]->mainchain[j]->uncles.empty()) {
                        for (auto iter = nodePool[e->block->miner]->mainchain[j]->uncles.begin(); iter != nodePool[e->block->miner]->mainchain[j]->uncles.end(); ++iter) {
                            nodePool[e->owner]->includedUncles[*iter] = true;
                        }
                    }
                }
            }
            if (nodePool[e->owner]->hasMiningEvent) {
                evEngine.RemoveEvent(nodePool[e->owner]->miningEvent);
                nodePool[e->owner]->hasMiningEvent = false;
            }
            GenBlock(e->owner, e->timestamp, nodePool, evEngine);
        }
    }
}

void BlockPolicy::LoadVAGHOSTRule(multiset<Event>::iterator e, vector<unique_ptr<Node>> &nodePool, EventEngine &evEngine)
{
    auto &owner = nodePool[e->owner];

    // Recompute the preferred chain from the owner's full local DAG.
    vector<shared_ptr<Block>> oldChain = owner->mainchain;
    vector<shared_ptr<Block>> newChain = ComputeVAGhostPreferredChain(owner.get());
    if (newChain.empty()) return;

    int common = commonPrefixLength(oldChain, newChain);
    bool chainChanged = (common != (int)oldChain.size()) || (common != (int)newChain.size());
    if (!chainChanged) {
        owner->RebuildUnclePool();
        return;
    }

    int reorgDepth = static_cast<int>(oldChain.size()) - common;
    if (reorgDepth > 0) {
        owner->reorgCount += 1;
        owner->reorgDepthSum += reorgDepth;
        if (reorgDepth > owner->maxReorgDepth) owner->maxReorgDepth = reorgDepth;
    }

    // Release transactions from blocks that are no longer on the preferred chain.
    for (int j = common; j < (int)oldChain.size(); ++j) {
        ReleaseTransactions(e->owner, oldChain[j], nodePool);
    }

    owner->mainchain.resize(common);

    // Adopt the new preferred suffix and update the transaction pool.
    for (int j = common; j < (int)newChain.size(); ++j) {
        owner->mainchain.push_back(newChain[j]);
        UpdateTransactionPool(e->owner, newChain[j], nodePool);
        for (auto uncleId : newChain[j]->uncles) {
            owner->includedUncles[uncleId] = true;
        }
    }

    owner->RebuildUnclePool();

    if (owner->hasMiningEvent) {
        evEngine.RemoveEvent(owner->miningEvent);
        owner->hasMiningEvent = false;
    }
    GenBlock(e->owner, e->timestamp, nodePool, evEngine);
}

void BlockPolicy::UpdateTransactionPool(int nodeID, shared_ptr<Block> block, vector<unique_ptr<Node>> &nodePool)
{
    if(DATA) nodePool[nodeID]->txPool &= (~block->txs);
}

void BlockPolicy::ReleaseTransactions(int nodeID, shared_ptr<Block> block, vector<unique_ptr<Node>> &nodePool)
{
    if(DATA && block->low <= block->high) {
        nodePool[nodeID]->txPool |= block->txs;
        nodePool[nodeID]->low = min(nodePool[nodeID]->low, block->low);
        nodePool[nodeID]->high = max(nodePool[nodeID]->high, block->high);
    }
}
