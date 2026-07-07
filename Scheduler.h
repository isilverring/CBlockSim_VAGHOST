#pragma once
#include "Models/Event.h"
#include "Models/Node.h"
#include "Configs.h"
#include "Factories/TransactionFactory.h"
#include "Factories/TopologyFactory.h"
#include "Policy/TransactionPolicy.h"
#include "Policy/BlockPolicy.h"
#include "EventEngine.h"
#include "Statistics.h"
#include <set>
#include <unordered_set>

using namespace std;

class Scheduler
{
public:

    double halfBPD;
    double ninetyBPD;
    int count;

    int totalGenBlocks;
    int globalchain;
    Statistics st;
    TransactionFactory txGenerator;
    TopologyFactory topoGenerator;
    TransactionPolicy txnPolicy;
    BlockPolicy bkPolicy;
    EventEngine evEngine;

    double topo[NODES_NUM][NODES_NUM];
    vector<shared_ptr<Transaction>> txPool;
    vector<unique_ptr<Node>> nodePool;
    map<int, shared_ptr<Block>> blockPool;

    // attack buffer for withheld blocks
    struct AttackBlock {
        shared_ptr<Block> block;
        double releaseTime;
        bool propagated;
    };
    vector<AttackBlock> attackBuffer;
    unordered_set<int> publicBlocks;

    Scheduler();

    void GenerateInitialEvents();
    void HandleEvent(multiset<Event>::iterator e);

    void GenerateBlock(multiset<Event>::iterator e);
    void ReceiveBlock(multiset<Event>::iterator e);

    void GenBlock(int miner, double currentTime);
    void PropagateBlock(shared_ptr<Block> block, double sendTime, bool honestOnly = false, bool attackersOnly = false);
    void ReleaseBufferedAttackBlocks(double now);
    int PublicHeadDepth() const;
    int PrivateHeadDepth() const;
    void ResolveFork();
    void DistributeRewards();
    double ShowStatistics(int rd, double t);
};
