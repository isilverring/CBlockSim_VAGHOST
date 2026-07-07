#include "TopologyFactory.h"
#include <random>
#include <queue>
#include <vector>
#include <iostream>
#include <algorithm>

// Returns the connection probability between two nodes.
double getPij(double dens, double prop, int maxDis, int i, int j)
{
    int dis = abs(i - j);
    int theta = 0;
    if (dens - (double)std::min(dis, NODES_NUM - dis) / (double)maxDis >= 0) theta = 1;
    return prop * dens + (1 - prop) * theta;
}

void TopologyFactory::ProduceCompleteTopo(double topo[NODES_NUM][NODES_NUM])
{
    for (int i = 0; i < NODES_NUM; i++) {
        for (int j = 0; j < NODES_NUM; j++) {
            if (i == j) topo[i][j] = 0;
            else {
                topo[i][j] = 1;
            }
        }
    }
}

// Ref: "Simple, distance-dependent formulation of the Watts-Strogatz model for directed and undirected small-world networks"
// Parameters:
// maxDis : Dmax in paper; max distance between any two nodes
// prop: Beta in paper
// dens : p0 in paper
// topo : the adjacency matrix of a Watts - Strogatz graph
void TopologyFactory::ProduceSmallWorldTopo(int maxDis, double prop, double dens, double topo[NODES_NUM][NODES_NUM])
{
    double randNum;
    for (int i = 0; i < NODES_NUM; i++) {
        for (int j = 0; j < NODES_NUM; j++) {
            if (i == j) topo[i][j] = INF;
            else if (i > j) topo[i][j] = topo[j][i];
            else {
                randNum = rand() % 100 / (double)100;
                if (randNum < getPij(dens, prop, maxDis, i, j)) topo[i][j] = 1.0;
                else {
                    topo[i][j] = INF;
                }
            }
        }
    }
}

void TopologyFactory::ProduceNodesByDistrib(std::vector<unique_ptr<Node>>& nodePool, double topo[NODES_NUM][NODES_NUM])
{
    totalHash = 0.0;

    int numsArr[REGIONS_NUM];
    unsigned int total = 0, id = 0;
    for (int i = 0; i < REGIONS_NUM - 1; i++) {
        numsArr[i] = (int)NODES_NUM * REGIONS_DISTRIBUTION[i];
        total += numsArr[i];
    }
    numsArr[REGIONS_NUM - 1] = NODES_NUM - total;

    for (int i = 0; i < REGIONS_NUM; i++) {
        for (int j = 0; j < numsArr[i]; j++) {
            auto node = make_unique<Node>();
            node->id = id;
            node->location = i;
            nodePool.push_back(move(node));
            id++;
        }
    }

    for (int i = 0; i < NODES_NUM; i++) {
        for (int j = 0; j < NODES_NUM; j++) {
            if (topo[i][j] != INF) {
                nodePool[i]->neighborNum++;
                topo[i][j] = LATENCIES[nodePool[i]->location][nodePool[j]->location];
            }
        }
    }

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


    // Stake falls in Gaussian distribution
    for (int i = 0; i < NODES_NUM; i++) {
        nodePool[i]->stake = std::max(0.1, dis(gen));
        nodePool[i]->stakeTime = 0;
    }

    // Adversaries are selected by TARGET HASH-POWER SHARE, not by raw node count.
    for (int i = 0; i < NODES_NUM; ++i) {
        nodePool[i]->isAttacker = false;
    }

    std::vector<int> ids;
    ids.reserve(NODES_NUM);
    for (int i = 0; i < NODES_NUM; ++i) ids.push_back(i);
    std::mt19937 attackerGen(ATTACKER_SELECT_SEED);
    std::shuffle(ids.begin(), ids.end(), attackerGen);

    double advHash = 0.0;
    for (int idx : ids) {
        if (advHash + 1e-12 >= ADVERSARY_FRACTION) break;
        nodePool[idx]->isAttacker = true;
        advHash += nodePool[idx]->hashPower;
    }
}