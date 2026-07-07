#pragma once
#include "Block.h"
#include "Event.h"
#include <set>
#include <bitset>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

class Node
{
public:
    int id;
    int location;
    int neighborNum;
    double hashPower;
    double stake;
    double stakeTime;
    double balance;
    int high, low;
    bitset<TX_NUM> txPool;
    vector<shared_ptr<Block>> mainchain;
    vector<shared_ptr<Block>> unclechain;
    map<int, bool> includedUncles;
    bool hasMiningEvent;
    multiset<Event>::iterator miningEvent;

    // Node-local DAG view: all known blocks plus parent->children adjacency.
    unordered_map<int, shared_ptr<Block>> knownBlocks;
    unordered_map<int, vector<int>> childBlocks;

    // Visibility counters observed by this node.
    unordered_map<int,int> visibility_count;  // block_id -> k_b

    // Reorg statistics.
    bool isAttacker = false;
    int  reorgCount = 0;
    int  reorgDepthSum = 0;
    int  maxReorgDepth = 0;

    Node();

    // Remember a block in the local DAG view.
    void RememberBlock(shared_ptr<Block> block);

    // Add a block to the current preferred chain.
    void AddBlock(shared_ptr<Block> block);

    // Add uncle candidates to a newly generated block.
    void AddUncles(shared_ptr<Block> block);

    // Recompute uncle candidates from local DAG minus current preferred chain.
    void RebuildUnclePool();
};
