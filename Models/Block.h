#pragma once
#include "Transaction.h"
#include <vector>
#include <bitset>
#include "../Configs.h"


using namespace std;

class Block
{
public:
	int id;
	int depth;
	int diff;
	int prevBlock;
	int miner;
	double timestamp;
	double size;
	int low, high;
	bitset<TX_NUM> txs;
	vector<int> uncles;
	
	//  visibility certificate = list of block IDs this miner attests to
    std::vector<int> visibility_cert;   // or std::vector<BlockID>

    
    double visibility_factor;  // in [0,1], can be recomputed from counters
	int gasLimit;

	Block();
};