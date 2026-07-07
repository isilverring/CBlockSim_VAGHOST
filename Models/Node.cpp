#include "Node.h"
#include <algorithm>
#include "../Configs.h"

Node::Node()
{
    neighborNum = 0;
    hasMiningEvent = false;

    reorgCount = 0;
    reorgDepthSum = 0;
    maxReorgDepth = 0;
    isAttacker = false;

    auto genesis = make_shared<Block>();
    genesis->id = 0;
    genesis->prevBlock = -1;
    genesis->depth = 0;
    genesis->miner = -1;
    genesis->timestamp = 0.0;

    RememberBlock(genesis);
    mainchain.push_back(genesis);
    visibility_count[genesis->id] = 1;

    high = -1;
    low  = -1;
    txPool.set();
}

void Node::RememberBlock(shared_ptr<Block> block)
{
    if (!block) return;
    if (knownBlocks.find(block->id) != knownBlocks.end()) return;

    knownBlocks[block->id] = block;
    if (block->prevBlock >= 0) {
        childBlocks[block->prevBlock].push_back(block->id);
    }
    if (visibility_count.find(block->id) == visibility_count.end()) {
        visibility_count[block->id] = 0;
    }
}

void Node::AddBlock(shared_ptr<Block> block)
{
    RememberBlock(block);
    if (mainchain.empty() || mainchain.back()->id != block->id) {
        mainchain.push_back(block);
    }

    if (!block->uncles.empty()) {
        for (auto iter = block->uncles.begin(); iter != block->uncles.end(); ++iter) {
            includedUncles[*iter] = true;
        }
    }
}

void Node::RebuildUnclePool()
{
    unordered_set<int> inMain;
    for (auto &blk : mainchain) {
        inMain.insert(blk->id);
    }

    unclechain.clear();
    for (auto &kv : knownBlocks) {
        int bid = kv.first;
        if (bid == 0) continue;                 // ignore genesis
        if (inMain.count(bid)) continue;        // not stale if on preferred chain
        if (includedUncles.find(bid) != includedUncles.end()) continue;
        unclechain.push_back(kv.second);
    }

    sort(unclechain.begin(), unclechain.end(), [](const shared_ptr<Block> &a, const shared_ptr<Block> &b) {
        if (a->depth != b->depth) return a->depth > b->depth;
        if (a->timestamp != b->timestamp) return a->timestamp > b->timestamp;
        return a->id < b->id;
    });
}

void Node::AddUncles(shared_ptr<Block> block)
{
    int maxUncles = MAX_UNCLES;
    for (auto iter = unclechain.begin(); iter != unclechain.end() && maxUncles > 0; ++iter) {
        int baseDepth  = block->depth;
        int uncleDepth = (*iter)->depth;
        if (uncleDepth > baseDepth - MIN_UNCLE_GENERATION + 1 || uncleDepth < baseDepth - MAX_UNCLE_GENERATION + 1) continue;
        if (uncleDepth >= 0 && uncleDepth < (int)mainchain.size() && mainchain[uncleDepth]->id == (*iter)->id) continue;
        if (uncleDepth >= 0 && uncleDepth < (int)mainchain.size() && mainchain[uncleDepth]->prevBlock != (*iter)->prevBlock) continue;
        if (includedUncles.find((*iter)->id) != includedUncles.end()) continue;
        block->uncles.push_back((*iter)->id);
        includedUncles[(*iter)->id] = true;
        block->diff++;
        maxUncles--;
        iter = unclechain.erase(iter);
        --iter;
    }
}
