/*
 * Copyright (C) 2017 CAMELab
 *
 * This file is part of SimpleSSD.
 *
 * SimpleSSD is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SimpleSSD is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SimpleSSD.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ftl/page_mapping.hh"

#include <algorithm>
#include <limits>
#include <random>

#include "util/algorithm.hh"
#include "util/bitset.hh"

SimpleSSD::Event refreshEvent;

namespace SimpleSSD {

namespace FTL {

PageMapping::PageMapping(ConfigReader &c, Parameter &p, PAL::PAL *l,
                         DRAM::AbstractDRAM *d)
    : AbstractFTL(p, l, d),
      pPAL(l),
      conf(c),
      lastFreeBlock(param.pageCountToMaxPerf),
      lastFreeBlockIOMap(param.ioUnitInPage),
      bReclaimMore(false),
      lastRefreshed(0) {
          blocks.reserve(param.totalPhysicalBlocks);
  table.reserve(param.totalLogicalBlocks * param.pagesInBlock);
  refresh_table.reserve(param.totalPhysicalBlocks * 64);

  uint32_t initEraseCount = conf.readUint(CONFIG_FTL, FTL_INITIAL_ERASE_COUNT);

  for (uint32_t i = 0; i < param.totalPhysicalBlocks; i++) {
    //freeBlocks.emplace_back(Block(i, param.pagesInBlock, param.ioUnitInPage));
    freeBlocks.emplace_back(Block(i, param.pagesInBlock, param.ioUnitInPage, initEraseCount));
  }

  nFreeBlocks = param.totalPhysicalBlocks;

  status.totalLogicalPages = param.totalLogicalBlocks * param.pagesInBlock;

  // Allocate free blocks
  for (uint32_t i = 0; i < param.pageCountToMaxPerf; i++) {
    lastFreeBlock.at(i) = getFreeBlock(i);
  }

  lastFreeBlockIndex = 0;

  memset(&stat, 0, sizeof(stat));

  bRandomTweak = conf.readBoolean(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK);
  bitsetSize = bRandomTweak ? param.ioUnitInPage : 1;

  float tmp = conf.readFloat(CONFIG_FTL, FTL_TEMPERATURE);
  float Ea = 1.1;
  float epsilon = conf.readFloat(CONFIG_FTL, FTL_EPSILON);
  float alpha = conf.readFloat(CONFIG_FTL, FTL_ALPHA);
  float beta = conf.readFloat(CONFIG_FTL, FTL_BETA);
  float kTerm = conf.readFloat(CONFIG_FTL, FTL_KTERM);
  float mTerm = conf.readFloat(CONFIG_FTL, FTL_MTERM);
  float nTerm = conf.readFloat(CONFIG_FTL, FTL_NTERM);
  float sigma = conf.readFloat(CONFIG_FTL, FTL_ERROR_SIGMA);
  uint32_t seed = conf.readUint(CONFIG_FTL, FTL_RANDOM_SEED);


  errorModel = ErrorModeling(tmp, Ea, epsilon, alpha, beta,
                             kTerm, mTerm, nTerm, 
                             sigma, param.pageSize, seed);
}

PageMapping::~PageMapping() {
  refreshStatFile.close();
}

bool PageMapping::initialize() {
  uint64_t nPagesToWarmup;
  uint64_t nPagesToInvalidate;
  uint64_t nTotalLogicalPages;
  uint64_t maxPagesBeforeGC;
  uint64_t tick;
  uint64_t valid;
  uint64_t invalid;
  FILLING_MODE mode;

  Request req(param.ioUnitInPage);

  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization started");

  nTotalLogicalPages = param.totalLogicalBlocks * param.pagesInBlock;
  nPagesToWarmup =
      nTotalLogicalPages * conf.readFloat(CONFIG_FTL, FTL_FILL_RATIO);
  nPagesToInvalidate =
      nTotalLogicalPages * conf.readFloat(CONFIG_FTL, FTL_INVALID_PAGE_RATIO);
  mode = (FILLING_MODE)conf.readUint(CONFIG_FTL, FTL_FILLING_MODE);
  maxPagesBeforeGC =
      param.pagesInBlock *
      (param.totalPhysicalBlocks *
           (1 - conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO)) -
       param.pageCountToMaxPerf);  // # free blocks to maintain

  if (nPagesToWarmup + nPagesToInvalidate > maxPagesBeforeGC) {
    warn("ftl: Too high filling ratio. Adjusting invalidPageRatio.");
    nPagesToInvalidate = maxPagesBeforeGC - nPagesToWarmup;
  }

  debugprint(LOG_FTL_PAGE_MAPPING, "Total logical pages: %" PRIu64,
             nTotalLogicalPages);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "Total logical pages to fill: %" PRIu64 " (%.2f %%)",
             nPagesToWarmup, nPagesToWarmup * 100.f / nTotalLogicalPages);
  debugprint(LOG_FTL_PAGE_MAPPING,
             "Total invalidated pages to create: %" PRIu64 " (%.2f %%)",
             nPagesToInvalidate,
             nPagesToInvalidate * 100.f / nTotalLogicalPages);

  req.ioFlag.set();

  // Step 1. Filling
  if (mode == FILLING_MODE_0 || mode == FILLING_MODE_1) {
    // Sequential
    for (uint64_t i = 0; i < nPagesToWarmup; i++) {
      tick = 0;
      req.lpn = i;
      writeInternal(req, tick, false);
    }
  }
  else {
    // Random
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nTotalLogicalPages - 1);

    for (uint64_t i = 0; i < nPagesToWarmup; i++) {
      tick = 0;
      req.lpn = dist(gen);
      writeInternal(req, tick, false);
    }
  }

  // Step 2. Invalidating
  if (mode == FILLING_MODE_0) {
    // Sequential
    for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
      tick = 0;
      req.lpn = i;
      writeInternal(req, tick, false);
    }
  }
  else if (mode == FILLING_MODE_1) {
    // Random
    // We can successfully restrict range of LPN to create exact number of
    // invalid pages because we wrote in sequential mannor in step 1.
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nPagesToWarmup - 1);

    for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
      tick = 0;
      req.lpn = dist(gen);
      writeInternal(req, tick, false);
    }
  }
  else {
    // Random
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, nTotalLogicalPages - 1);

    for (uint64_t i = 0; i < nPagesToInvalidate; i++) {
      tick = 0;
      req.lpn = dist(gen);
      writeInternal(req, tick, false);
    }
  }

  //setup refresh
  refreshStatFile.open("/home/wooks/SimpleSSD-base/log/refresh_web_2_2hour_400s_30d_log.txt");
  uint64_t random_seed = conf.readUint(CONFIG_FTL, FTL_RANDOM_SEED) + 1231;
  uint32_t num_bf = conf.readUint(CONFIG_FTL, FTL_REFRESH_FILTER_NUM);
  uint32_t filter_size = conf.readUint(CONFIG_FTL, FTL_REFRESH_FILTER_SIZE);
  debugprint(LOG_FTL_PAGE_MAPPING, "Refresh setting start. The number of bloom filters: %u", num_bf);
  debugprint(LOG_FTL_PAGE_MAPPING, "Refresh threshold error count: %u", param.pageSize / 1000);
  //multi level bloom filter
  for (unsigned int i=0; i<=num_bf; i++){
    bloom_parameters parameters;

    parameters.projected_element_count = 10000;
    parameters.false_positive_probability = 1.0E-6; // 1 in 10^10
    parameters.random_seed = random_seed++;
    if (filter_size){
      parameters.maximum_size = filter_size;
      parameters.minimum_size = filter_size;
    }

    parameters.compute_optimal_parameters();
    refreshStatFile << parameters.maximum_number_of_hashes << ", " \
                    << parameters.maximum_size << ", " \
                    << parameters.minimum_number_of_hashes << ", " \
                    << parameters.minimum_size << ", " \
                    << parameters.optimal_parameters.number_of_hashes << ", " \
                    << parameters.optimal_parameters.table_size << ", " \
                    << parameters.false_positive_probability << ", " \
                    << parameters.random_seed << ", " \
                    << parameters.projected_element_count << "\n\n";
    if (i != 0){  // Bloom filter size == 0 for first calculated parameters. I don't know why
    auto newBloom = bloom_filter(parameters);
    newBloom.clear();
    bloomFilters.push_back(newBloom);
    //  debugprint(LOG_FTL_PAGE_MAPPING, "Bloom filter size: %u", bloomFilters[bloomFilters.size() - 1].size());
    }
    else{
      auto dummy = bloom_filter(parameters); // First Bloom filter is not accurate. I don't know why
    }
    
  }

  for (unsigned int i=0; i<num_bf; i++){
    debugprint(LOG_FTL_PAGE_MAPPING, "Bloom filter %u size: %u", i, bloomFilters[i].size());
    debugprint(LOG_FTL_PAGE_MAPPING, "bloom filter %u element count : %u", i, bloomFilters[i].element_count());
  }

  refresh_period = conf.readUint(CONFIG_FTL, FTL_REFRESH_PERIOD);
  // set up periodic refresh event
  if (conf.readUint(CONFIG_FTL, FTL_REFRESH_PERIOD) > 0) {
    refreshEvent = engine.allocateEvent([this](uint64_t tick) {
      refresh_event(tick);

      engine.scheduleEvent(
          refreshEvent,
          tick + conf.readUint(CONFIG_FTL, FTL_REFRESH_PERIOD) *
                     1000000000ULL);
    });
    engine.scheduleEvent(
        refreshEvent,
        conf.readUint(CONFIG_FTL, FTL_REFRESH_PERIOD) * 1000000000ULL);
  }

  stat.refreshCallCount = 1;
  bloomFilters[0].false_positive = 0;
  debugprint(LOG_FTL_PAGE_MAPPING, "Refresh setting done. The number of bloom filters: %u", bloomFilters.size());
  for (uint32_t target_bf=0; target_bf<bloomFilters.size(); target_bf++){
    refreshStatFile << "bloomfilter_stat" << target_bf << endl;
    refreshStatFile << "false_positive :" << bloomFilters[target_bf].false_positive << endl;
    refreshStatFile << "true_positive :" << bloomFilters[target_bf].true_positive << endl;
    refreshStatFile << "true_negative :" << bloomFilters[target_bf].true_negative << endl;
    refreshStatFile << "bloom filter size :" << bloomFilters[target_bf].table_size_ << endl;
    refreshStatFile << "bloom filter hash :" << bloomFilters[target_bf].salt_count_ << endl;
    refreshStatFile << "actual insertion :" << bloomFilters[target_bf].actual_insert << "\n\n";

  }
  refreshStatFile.flush();

  // Report
  calculateTotalPages(valid, invalid);
  debugprint(LOG_FTL_PAGE_MAPPING, "Filling finished. Page status:");
  debugprint(LOG_FTL_PAGE_MAPPING,
             "  Total valid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             valid, valid * 100.f / nTotalLogicalPages, nPagesToWarmup,
             (int64_t)(valid - nPagesToWarmup));
  debugprint(LOG_FTL_PAGE_MAPPING,
             "  Total invalid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             invalid, invalid * 100.f / nTotalLogicalPages, nPagesToInvalidate,
             (int64_t)(invalid - nPagesToInvalidate));
  debugprint(LOG_FTL_PAGE_MAPPING, "Initialization finished");

  return true;
}

void PageMapping::refresh_event(uint64_t tick){
  uint32_t num_block = param.totalPhysicalBlocks;
  uint32_t num_layer = 64;

  unsigned int target_bf = 0;
  uint64_t RC_copy = stat.refreshCallCount;
  //uint64_t tick_old = tick;

  debugprint(LOG_FTL_PAGE_MAPPING, "Refresh at %" PRIu64, tick);
  refreshStatFile << "Refresh at" << tick << "\n";

  while (target_bf<bloomFilters.size()-1){
    if ((RC_copy&1) == 0){
      target_bf++;
      RC_copy= RC_copy>>1;
    }
    else{
      break;
    }
  }
  debugprint(LOG_FTL_PAGE_MAPPING, "check bloom filter %u", target_bf);
  refreshStatFile << "Check bllom filter %u" << target_bf << "\n";
  
  uint32_t layerCheckCount = 0;
  for (uint32_t i=0; i<num_block;i++){
    for (uint32_t j=0; j<num_layer;j++){
      uint64_t item = ((uint64_t)i<<32) + j;
      auto refresh_entry = refresh_table.find(item);
      if (bloomFilters[target_bf].contains(item)){
        //refresh layer
        if (refresh_entry != refresh_table.end() && refresh_entry->second <=target_bf ){
          bloomFilters[target_bf].true_positive++;
        }else{
          bloomFilters[target_bf].false_positive++;
        }
        layerCheckCount ++;
        //debugprint(LOG_FTL_PAGE_MAPPING, "Refresh block %u, layer %u", i, j);
        refreshPage(i,j,tick);
      }
      else{
        bloomFilters[target_bf].true_negative++;
      }
    }
  }
  stat.refreshCallCount++;
  stat.layerCheckCount += layerCheckCount;
  debugprint(LOG_FTL_PAGE_MAPPING, "%u / %u layers checked", layerCheckCount, num_block * num_layer);
  refreshStatFile << layerCheckCount << " / " << num_block * num_layer << "layers checked\n";
  refreshStatFile << "bloomfilter_stat" << target_bf << endl;
  refreshStatFile << "false_positive :" << bloomFilters[target_bf].false_positive << endl;
  refreshStatFile << "true_positive :" << bloomFilters[target_bf].true_positive << endl;
  refreshStatFile << "true_negative :" << bloomFilters[target_bf].true_negative << endl;
  refreshStatFile << "actual insertion :" << bloomFilters[target_bf].actual_insert << "\n\n";
  refreshStatFile.flush();
  // stat.refreshTick += tick - tick_old;
}

// insert to bloom filter depending on its retention capability
void PageMapping::setRefreshPeriod(uint32_t block_id, uint32_t layer_id, uint64_t rtc){
  uint64_t item = ((uint64_t)block_id<<32) + layer_id;
  auto refresh_entry = refresh_table.find(item);
  if (refresh_entry == refresh_table.end()){
    refresh_table[item] = rtc;
    bloomFilters[rtc].actual_insert++;
  }
  else if (refresh_entry->second>rtc) {
    refresh_entry->second = rtc;
    bloomFilters[rtc].actual_insert++;
  }
  debugprint(LOG_FTL_PAGE_MAPPING, "rtc %u", rtc);
  bloomFilters[rtc].insert(item);
}


void PageMapping::read(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.ioFlag.count() > 0) {
    readInternal(req, tick);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "READ  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ);
}

void PageMapping::write(Request &req, uint64_t &tick) {
  uint64_t begin = tick;
  
  if (req.ioFlag.count() > 0) {
    writeInternal(req, tick);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "WRITE | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE);
}

void PageMapping::trim(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  trimInternal(req, tick);

  debugprint(LOG_FTL_PAGE_MAPPING,
             "TRIM  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
             ")",
             req.lpn, begin, tick, tick - begin);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM);
}

void PageMapping::format(LPNRange &range, uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<uint32_t> list;

  req.ioFlag.set();

  for (auto iter = table.begin(); iter != table.end();) {
    if (iter->first >= range.slpn && iter->first < range.slpn + range.nlp) {
      auto &mappingList = iter->second;

      // Do trim
      for (uint32_t idx = 0; idx < bitsetSize; idx++) {
        auto &mapping = mappingList.at(idx);
        auto block = blocks.find(mapping.first);

        if (block == blocks.end()) {
          panic("Block is not in use");
        }

        block->second.invalidate(mapping.second, idx);

        // Collect block indices
        list.push_back(mapping.first);
      }

      iter = table.erase(iter);
    }
    else {
      iter++;
    }
  }

  // Get blocks to erase
  std::sort(list.begin(), list.end());
  auto last = std::unique(list.begin(), list.end());
  list.erase(last, list.end());

  // Do GC only in specified blocks
  doGarbageCollection(list, tick);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::FORMAT);
}

Status *PageMapping::getStatus(uint64_t lpnBegin, uint64_t lpnEnd) {
  status.freePhysicalBlocks = nFreeBlocks;

  if (lpnBegin == 0 && lpnEnd >= status.totalLogicalPages) {
    status.mappedLogicalPages = table.size();
  }
  else {
    status.mappedLogicalPages = 0;

    for (uint64_t lpn = lpnBegin; lpn < lpnEnd; lpn++) {
      if (table.count(lpn) > 0) {
        status.mappedLogicalPages++;
      }
    }
  }

  return &status;
}

float PageMapping::freeBlockRatio() {
  return (float)nFreeBlocks / param.totalPhysicalBlocks;
}

uint32_t PageMapping::convertBlockIdx(uint32_t blockIdx) {
  return blockIdx % param.pageCountToMaxPerf;
}

uint32_t PageMapping::getFreeBlock(uint32_t idx) {
  uint32_t blockIndex = 0;

  if (idx >= param.pageCountToMaxPerf) {
    panic("Index out of range");
  }

  if (nFreeBlocks > 0) {
    // Search block which is blockIdx % param.pageCountToMaxPerf == idx
    auto iter = freeBlocks.begin();

    for (; iter != freeBlocks.end(); iter++) {
      blockIndex = iter->getBlockIndex();

      if (blockIndex % param.pageCountToMaxPerf == idx) {
        break;
      }
    }

    // Sanity check
    if (iter == freeBlocks.end()) {
      // Just use first one
      iter = freeBlocks.begin();
      blockIndex = iter->getBlockIndex();
    }

    // Insert found block to block list
    if (blocks.find(blockIndex) != blocks.end()) {
      panic("Corrupted");
    }

    blocks.emplace(blockIndex, std::move(*iter));

    // Update first write time
    blocks.find(blockIndex)->second.setLastWrittenTime(getTick());


    // Remove found block from free block list
    freeBlocks.erase(iter);
    nFreeBlocks--;
  }
  else {
    std::cout << "nFreeBlock" << nFreeBlocks << std::endl;
    panic("No free block left");
  }

  return blockIndex;
}

uint32_t PageMapping::getLastFreeBlock(Bitset &iomap) {
  if (!bRandomTweak || (lastFreeBlockIOMap & iomap).any()) {
    // Update lastFreeBlockIndex
    lastFreeBlockIndex++;

    if (lastFreeBlockIndex == param.pageCountToMaxPerf) {
      lastFreeBlockIndex = 0;
    }

    lastFreeBlockIOMap = iomap;
  }
  else {
    lastFreeBlockIOMap |= iomap;
  }

  auto freeBlock = blocks.find(lastFreeBlock.at(lastFreeBlockIndex));

  // Sanity check
  if (freeBlock == blocks.end()) {
    panic("Corrupted");
  }

  // If current free block is full, get next block
  if (freeBlock->second.getNextWritePageIndex() == param.pagesInBlock) {
    lastFreeBlock.at(lastFreeBlockIndex) = getFreeBlock(lastFreeBlockIndex);

    bReclaimMore = true;
  }

  return lastFreeBlock.at(lastFreeBlockIndex);
}

// calculate weight of each block regarding victim selection policy
void PageMapping::calculateVictimWeight(
    std::vector<std::pair<uint32_t, float>> &weight, const EVICT_POLICY policy,
    uint64_t tick) {
  float temp;

  weight.reserve(blocks.size());

  switch (policy) {
    case POLICY_GREEDY:
    case POLICY_RANDOM:
    case POLICY_DCHOICE:
      for (auto &iter : blocks) {
        if (iter.second.getNextWritePageIndex() != param.pagesInBlock) {
          continue;
        }

        weight.push_back({iter.first, iter.second.getValidPageCountRaw()});
      }

      break;
    case POLICY_COST_BENEFIT:
      for (auto &iter : blocks) {
        if (iter.second.getNextWritePageIndex() != param.pagesInBlock) {
          continue;
        }

        temp = (float)(iter.second.getValidPageCountRaw()) / param.pagesInBlock;

        weight.push_back(
            {iter.first,
             temp / ((1 - temp) * (tick - iter.second.getLastAccessedTime()))});
      }

      break;
    default:
      panic("Invalid evict policy");
  }
}

void PageMapping::selectVictimBlock(std::vector<uint32_t> &list,
                                    uint64_t &tick, std::vector<uint32_t> &exceptList) {
  static const GC_MODE mode = (GC_MODE)conf.readInt(CONFIG_FTL, FTL_GC_MODE);
  static const EVICT_POLICY policy =
      (EVICT_POLICY)conf.readInt(CONFIG_FTL, FTL_GC_EVICT_POLICY);
  static uint32_t dChoiceParam =
      conf.readUint(CONFIG_FTL, FTL_GC_D_CHOICE_PARAM);
  uint64_t nBlocks = conf.readUint(CONFIG_FTL, FTL_GC_RECLAIM_BLOCK);
  std::vector<std::pair<uint32_t, float>> weight;

  list.clear();

  // Calculate number of blocks to reclaim
  if (mode == GC_MODE_0) {
    // DO NOTHING
  }
  else if (mode == GC_MODE_1) {
    static const float t = conf.readFloat(CONFIG_FTL, FTL_GC_RECLAIM_THRESHOLD);

    nBlocks = param.totalPhysicalBlocks * t - nFreeBlocks;
  }
  else {
    panic("Invalid GC mode");
  }

  // reclaim one more if last free block fully used
  if (bReclaimMore) {
    nBlocks += param.pageCountToMaxPerf;

    bReclaimMore = false;
  }

  // Calculate weights of all blocks
  calculateVictimWeight(weight, policy, tick);

  if (policy == POLICY_RANDOM || policy == POLICY_DCHOICE) {
    uint64_t randomRange =
        policy == POLICY_RANDOM ? nBlocks : dChoiceParam * nBlocks;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(0, weight.size() - 1);
    std::vector<std::pair<uint32_t, float>> selected;

    while (selected.size() < randomRange) {
      uint64_t idx = dist(gen);

      auto findIter = std::find(exceptList.begin(), exceptList.end(), weight.at(idx).first);

      if (weight.at(idx).first < std::numeric_limits<uint32_t>::max() 
          && findIter == exceptList.end()) {
        selected.push_back(weight.at(idx));
        weight.at(idx).first = std::numeric_limits<uint32_t>::max();
      }
    }

    weight = std::move(selected);
  }

  // Sort weights
  std::sort(
      weight.begin(), weight.end(),
      [](std::pair<uint32_t, float> a, std::pair<uint32_t, float> b) -> bool {
        return a.second < b.second;
      });

  // Select victims from the blocks with the lowest weight
  nBlocks = MIN(nBlocks, weight.size());

  for (uint64_t i = 0; i < nBlocks; i++) {
    list.push_back(weight.at(i).first);
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::SELECT_VICTIM_BLOCK);
}

void PageMapping::doGarbageCollection(std::vector<uint32_t> &blocksToReclaim,
                                      uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> writeRequests;
  std::vector<PAL::Request> eraseRequests;
  std::vector<uint64_t> lpns;
  Bitset bit(param.ioUnitInPage);
  uint64_t beginAt;
  uint64_t readFinishedAt = tick;
  uint64_t writeFinishedAt = tick;
  uint64_t eraseFinishedAt = tick;

  if (blocksToReclaim.size() == 0) {
    return;
  }

  // For all blocks to reclaim, collecting request structure only
  for (auto &iter : blocksToReclaim) {
    auto block = blocks.find(iter);

    if (block == blocks.end()) {
      panic("Invalid block");
    }

    // Copy valid pages to free block
    for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++) {
      // Valid?
      if (block->second.getPageInfo(pageIndex, lpns, bit)) {
        if (!bRandomTweak) {
          bit.set();
        }

        // Retrive free block
        auto freeBlock = blocks.find(getLastFreeBlock(bit));

        // Issue Read
        req.blockIndex = block->first;
        req.pageIndex = pageIndex;
        req.ioFlag = bit;

        readRequests.push_back(req);

        // Update mapping table
        uint32_t newBlockIdx = freeBlock->first;

        for (uint32_t idx = 0; idx < bitsetSize; idx++) {
          if (bit.test(idx)) {
            // Invalidate
            block->second.invalidate(pageIndex, idx);

            auto mappingList = table.find(lpns.at(idx));

            if (mappingList == table.end()) {
              panic("Invalid mapping table entry");
            }

            pDRAM->read(&(*mappingList), 8 * param.ioUnitInPage, tick);

            auto &mapping = mappingList->second.at(idx);

            uint32_t newPageIdx = freeBlock->second.getNextWritePageIndex(idx);

            mapping.first = newBlockIdx;
            mapping.second = newPageIdx;

            freeBlock->second.write(newPageIdx, lpns.at(idx), idx, beginAt);

            // Issue Write
            req.blockIndex = newBlockIdx;
            req.pageIndex = newPageIdx;

            if (bRandomTweak) {
              req.ioFlag.reset();
              req.ioFlag.set(idx);
            }
            else {
              req.ioFlag.set();
            }

            writeRequests.push_back(req);

            stat.validPageCopies++;
          }
        }
        // TODO : this will updated for every write. The refresh should be done only for first write
        //freeBlock->second.setLastWrittenTime(tick);   
        stat.validSuperPageCopies++;
      }
    }

    // Erase block
    req.blockIndex = block->first;
    req.pageIndex = 0;
    req.ioFlag.set();

    eraseRequests.push_back(req);
  }

  // Do actual I/O here
  // This handles PAL2 limitation (SIGSEGV, infinite loop, or so-on)
  
  for (auto &iter : readRequests) {
    beginAt = tick;

    pPAL->read(iter, beginAt);

    readFinishedAt = MAX(readFinishedAt, beginAt);
  }

  for (auto &iter : writeRequests) {
    beginAt = readFinishedAt;

    pPAL->write(iter, beginAt);

    writeFinishedAt = MAX(writeFinishedAt, beginAt);
  }

  for (auto &iter : eraseRequests) {
    beginAt = readFinishedAt;

    eraseInternal(iter, beginAt);

    eraseFinishedAt = MAX(eraseFinishedAt, beginAt);
  }
  


  tick = MAX(writeFinishedAt, eraseFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}

void PageMapping::doRefresh(std::vector<uint32_t> &blocksToRefresh,
                                      uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> writeRequests;
  std::vector<PAL::Request> eraseRequests;
  std::vector<uint64_t> lpns;
  Bitset bit(param.ioUnitInPage);
  uint64_t beginAt;
  uint64_t readFinishedAt = tick;
  uint64_t writeFinishedAt = tick;
  uint64_t eraseFinishedAt = tick;

  std::vector<uint64_t> tempLpns;
  Bitset tempBit(param.ioUnitInPage);
  static float gcThreshold = conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO);

  if (blocksToRefresh.size() == 0) {
    return;
  }


  while (nFreeBlocks < blocksToRefresh.size() * 1.5) {
    
    debugprint(LOG_FTL_PAGE_MAPPING, "gcThreshold : %lf", gcThreshold);
    debugprint(LOG_FTL_PAGE_MAPPING, "freeBlockRatio : %lf", freeBlockRatio());
    debugprint(LOG_FTL_PAGE_MAPPING, "n free blocks : %u", nFreeBlocks);

    std::vector<uint32_t> list;
    uint64_t beginAt = tick;

    std::vector<uint32_t> dummy;

    selectVictimBlock(list, beginAt, dummy);

    // If the block would be garbage collected, it shouldn't be refeshed
    for (auto & gcIter : list) {
      //debugprint(LOG_FTL_PAGE_MAPPING, "Block %u will be garbage collected", gcIter);
      blocksToRefresh.erase(std::remove(blocksToRefresh.begin(), blocksToRefresh.end(), gcIter), blocksToRefresh.end());
    }
    

    debugprint(LOG_FTL_PAGE_MAPPING,
              "GC   | Refreshing | %u blocks will be reclaimed", list.size());

    doGarbageCollection(list, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
              "GC   | Done | %" PRIu64 " - %" PRIu64 " (%" PRIu64 ")", tick,
              beginAt, beginAt - tick);
    stat.gcCount++;
    stat.reclaimedBlocks += list.size();
    //debugprint(LOG_FTL_PAGE_MAPPING, "n free blocks after gc : %u", nFreeBlocks);

    //** Problem : refresh ??? block??? garbage collection??? ?????? erase ??? ??? ??????
  }
  
  //debugprint(LOG_FTL_PAGE_MAPPING, "start refreshing");
  // For all blocks to reclaim, collecting request structure only
  for (auto &iter : blocksToRefresh) {
    auto block = blocks.find(iter);

    if (block == blocks.end()) {
      printf("Cannot find block %u", iter);
      panic("Invalid block, refresh failed");
    }

    // Copy valid pages to free block
    for (uint32_t pageIndex = 0; pageIndex < param.pagesInBlock; pageIndex++) {
      //debugprint(LOG_FTL_PAGE_MAPPING, "Check valid");
      // Valid?
      if (block->second.getValidPageCount()) {
        block->second.getPageInfo(pageIndex, lpns, bit);
        if (!bRandomTweak) {
          bit.set();
        }

        //debugprint(LOG_FTL_PAGE_MAPPING, "Retrive free block");

        // Retrive free block
        auto freeBlock = blocks.find(getLastFreeBlock(bit));

        // Issue Read
        req.blockIndex = block->first;
        req.pageIndex = pageIndex;
        req.ioFlag = bit;

        readRequests.push_back(req);

        //debugprint(LOG_FTL_PAGE_MAPPING, "Update mapping table");
        // Update mapping table
        uint32_t newBlockIdx = freeBlock->first;

        for (uint32_t idx = 0; idx < bitsetSize; idx++) {
          if (bit.test(idx)) {    
            //debugprint(LOG_FTL_PAGE_MAPPING, "in the if statement");
            // Invalidate
            block->second.invalidate(pageIndex, idx); // ????????? out of range error
            //debugprint(LOG_FTL_PAGE_MAPPING, "Invalidated");


            auto mappingList = table.find(lpns.at(idx));
            //debugprint(LOG_FTL_PAGE_MAPPING, "Found mapping list");

            if (mappingList == table.end()) {
              panic("Invalid mapping table entry, refresh failed");
            }

            pDRAM->read(&(*mappingList), 8 * param.ioUnitInPage, tick);

            auto &mapping = mappingList->second.at(idx);
            //debugprint(LOG_FTL_PAGE_MAPPING, "Found mapping");

            uint32_t newPageIdx = freeBlock->second.getNextWritePageIndex(idx);

            mapping.first = newBlockIdx;
            mapping.second = newPageIdx;


            freeBlock->second.write(newPageIdx, lpns.at(idx), idx, beginAt);
            //debugprint(LOG_FTL_PAGE_MAPPING, "Written block");

            freeBlock->second.getPageInfo(newPageIdx, tempLpns, tempBit);
            //debugprint(LOG_FTL_PAGE_MAPPING, "got page info");
 
            // Issue Write
            req.blockIndex = newBlockIdx;
            req.pageIndex = newPageIdx;

            if (bRandomTweak) {
              req.ioFlag.reset();
              req.ioFlag.set(idx);
            }
            else {
              req.ioFlag.set();
            }

            writeRequests.push_back(req);

            stat.refreshPageCopies++;
          }
        }
        //debugprint(LOG_FTL_PAGE_MAPPING, "set last written time");
        //freeBlock->second.setLastWrittenTime(tick);

        stat.refreshSuperPageCopies++;
      }
    }
    // TODO: Should be garbage collected when there is not enough blocks
    // Or write should be performed by writeInternal
  }
  //debugprint(LOG_FTL_PAGE_MAPPING, "Do actual I/O");
  // Do actual I/O here
  // This handles PAL2 limitation (SIGSEGV, infinite loop, or so-on)
  for (auto &iter : readRequests) {
    beginAt = tick;

    pPAL->read(iter, beginAt);

    readFinishedAt = MAX(readFinishedAt, beginAt);
  }

  for (auto &iter : writeRequests) {
    beginAt = readFinishedAt;

    pPAL->write(iter, beginAt);

    writeFinishedAt = MAX(writeFinishedAt, beginAt);
  }

  tick = MAX(writeFinishedAt, eraseFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}

// calculate weight of each block regarding victim selection policy
void PageMapping::calculateRefreshWeight(
    std::vector<std::pair<uint32_t, float>> &weight, const REFRESH_POLICY policy,
    uint64_t tick) {

  static uint64_t refreshThreshold =
      conf.readUint(CONFIG_FTL, FTL_REFRESH_THRESHOLD);

  weight.reserve(blocks.size());

  switch (policy) {
    case POLICY_NONE:
      for (auto &iter : blocks) {
        if (tick - iter.second.getLastWrittenTime() < refreshThreshold) {
          continue;
        }
        // Refresh all blocks having data retention time exceeding thredhold
        weight.push_back({iter.first, iter.second.getValidPageCountRaw()});
      }

      break;
    default:
      panic("Invalid refresh policy");
  }
}

void PageMapping::selectRefreshVictim(std::vector<uint32_t> &list,
                                    uint64_t &tick) {
  static const REFRESH_POLICY policy =
      (REFRESH_POLICY)conf.readInt(CONFIG_FTL, FTL_REFRESH_POLICY);
  std::vector<std::pair<uint32_t, float>> weight;

  list.clear();

  // Calculate weights of all blocks
  calculateRefreshWeight(weight, policy, tick);

  for (uint64_t i = 0; i < weight.size(); i++) {
    list.push_back(weight.at(i).first);
  }

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::SELECT_VICTIM_BLOCK);
}

void PageMapping::refreshPage(uint32_t blockIndex, uint32_t layerNum,
                              uint64_t &tick) {
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readRequests;
  std::vector<PAL::Request> writeRequests;
  std::vector<PAL::Request> eraseRequests;
  std::vector<uint64_t> lpns;
  Bitset bit(param.ioUnitInPage);
  uint64_t beginAt = tick;
  uint64_t readFinishedAt = tick;
  uint64_t writeFinishedAt = tick;
  //uint64_t eraseFinishedAt = tick;

  std::vector<uint64_t> tempLpns;
  Bitset tempBit(param.ioUnitInPage);
  static float gcThreshold = conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO);

  if (freeBlockRatio() < gcThreshold) {

    std::vector<uint32_t> list;
    uint64_t beginAt = tick;

    std::vector<uint32_t> dummy;
    selectVictimBlock(list, beginAt, dummy);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | Refreshing | %u blocks will be reclaimed", list.size());

    doGarbageCollection(list, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | Done | %" PRIu64 " - %" PRIu64 " (%" PRIu64 ")", tick,
               beginAt, beginAt - tick);

    stat.gcCount++;
    stat.reclaimedBlocks += list.size();
  }
  
  //debugprint(LOG_FTL_PAGE_MAPPING, "start block %u layer %u refreshing", blockIndex, layerNum);
  // For all blocks to reclaim, collecting request structure only
  auto block = blocks.find(blockIndex);

  if (block == blocks.end()) {
    //printf("Cannot find block %u", blockIndex);
    //panic("Invalid block, refresh failed");
    // This can be happen because bloom filter can produce false negative
    return;
  }

  // Copy valid pages to free block
  // pageIndex is n*layerNum??
  for (uint32_t pageIndex = layerNum; pageIndex < param.pagesInBlock; pageIndex += 64) {
    //debugprint(LOG_FTL_PAGE_MAPPING, "Check valid");
    // Valid?
    if (block->second.getValidPageCount()) {
      block->second.getPageInfo(pageIndex, lpns, bit);
      if (!bRandomTweak) {
        bit.set();
      }

      //debugprint(LOG_FTL_PAGE_MAPPING, "Retrive free block");

      // Retrive free block
      auto freeBlock = blocks.find(getLastFreeBlock(bit));

      // Issue Read
      req.blockIndex = block->first;
      req.pageIndex = pageIndex;
      req.ioFlag = bit;

      readRequests.push_back(req);

      //debugprint(LOG_FTL_PAGE_MAPPING, "Update mapping table");
      // Update mapping table
      uint32_t newBlockIdx = freeBlock->first;

      for (uint32_t idx = 0; idx < bitsetSize; idx++) {
        if (bit.test(idx)) {    
          //debugprint(LOG_FTL_PAGE_MAPPING, "in the if statement");
          // Invalidate
          block->second.invalidate(pageIndex, idx); // ????????? out of range error
          //debugprint(LOG_FTL_PAGE_MAPPING, "Invalidated");


          auto mappingList = table.find(lpns.at(idx));
          //debugprint(LOG_FTL_PAGE_MAPPING, "Found mapping list");

          if (mappingList == table.end()) {
            //panic("Invalid mapping table entry, refresh failed");
            // This can be happen because bloom filter can produce false negative
            continue;
          }

          pDRAM->read(&(*mappingList), 8 * param.ioUnitInPage, tick);

          auto &mapping = mappingList->second.at(idx);
          //debugprint(LOG_FTL_PAGE_MAPPING, "Found mapping");

          uint32_t newPageIdx = freeBlock->second.getNextWritePageIndex(idx);

          mapping.first = newBlockIdx;
          mapping.second = newPageIdx;


          freeBlock->second.write(newPageIdx, lpns.at(idx), idx, beginAt);
          //debugprint(LOG_FTL_PAGE_MAPPING, "Written block");

          freeBlock->second.getPageInfo(newPageIdx, tempLpns, tempBit);
          //debugprint(LOG_FTL_PAGE_MAPPING, "got page info");

          // Issue Write
          req.blockIndex = newBlockIdx;
          req.pageIndex = newPageIdx;

          if (bRandomTweak) {
            req.ioFlag.reset();
            req.ioFlag.set(idx);
          }
          else {
            req.ioFlag.set();
          }

          writeRequests.push_back(req);

          stat.refreshPageCopies++;
        }
      }
      //debugprint(LOG_FTL_PAGE_MAPPING, "set last written time");
      //freeBlock->second.setLastWrittenTime(tick);

      stat.refreshSuperPageCopies++;
    }
  }
  // TODO: Should be garbage collected when there is not enough blocks
  // Or write should be performed by writeInternal

  //debugprint(LOG_FTL_PAGE_MAPPING, "Do actual I/O");
  // Do actual I/O here
  // This handles PAL2 limitation (SIGSEGV, infinite loop, or so-on)
  for (auto &iter : readRequests) {
    beginAt = tick;

    pPAL->read(iter, beginAt);

    readFinishedAt = MAX(readFinishedAt, beginAt);
  }

  for (auto &iter : writeRequests) {
    beginAt = readFinishedAt;

    pPAL->write(iter, beginAt);

    writeFinishedAt = MAX(writeFinishedAt, beginAt);
  }
  
  //debugprint(LOG_FTL_PAGE_MAPPING, "page refresh done. remaining free blocks: %u", nFreeBlocks);
  tick = MAX(writeFinishedAt, readFinishedAt);
  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::DO_GARBAGE_COLLECTION);
}


void PageMapping::readInternal(Request &req, uint64_t &tick) {
  PAL::Request palRequest(req);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  auto mappingList = table.find(req.lpn);

  if (mappingList != table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
    }

    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (mapping.first < param.totalPhysicalBlocks &&
            mapping.second < param.pagesInBlock) {
          palRequest.blockIndex = mapping.first;
          palRequest.pageIndex = mapping.second;

          if (bRandomTweak) {
            palRequest.ioFlag.reset();
            palRequest.ioFlag.set(idx);
          }
          else {
            palRequest.ioFlag.set();
          }

          auto block = blocks.find(palRequest.blockIndex);

          if (block == blocks.end()) {
            panic("Block is not in use");
          }

          beginAt = tick;

          block->second.read(palRequest.pageIndex, idx, beginAt);
          pPAL->read(palRequest, beginAt);

          /*
          uint64_t lastWritten = block->second.getLastWrittenTime();
          uint32_t eraseCount = block->second.getEraseCount();
          uint64_t curErrorCount = block->second.getMaxErrorCount();

          debugprint(LOG_FTL_PAGE_MAPPING, "Erase count %u", eraseCount);

          //TODO: Get layer number
          uint32_t layerNumber = mapping.second % 64;
          uint64_t newErrorCount = errorModel.getRandError(tick - lastWritten, eraseCount, layerNumber);

          debugprint(LOG_FTL_PAGE_MAPPING, "new rber: %f", errorModel.getRBER(tick - lastWritten, eraseCount, 0));
          debugprint(LOG_FTL_PAGE_MAPPING, "new randerror: %u", newErrorCount);


          block->second.setMaxErrorCount(max(curErrorCount, newErrorCount));
          */

          finishedAt = MAX(finishedAt, beginAt);
        }
      }
    }



    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::READ_INTERNAL);
  }
}

void PageMapping::writeInternal(Request &req, uint64_t &tick, bool sendToPAL) {
  PAL::Request palRequest(req);
  std::unordered_map<uint32_t, Block>::iterator block;
  auto mappingList = table.find(req.lpn);
  uint64_t beginAt;
  uint64_t finishedAt = tick;
  bool readBeforeWrite = false;


  if (mappingList != table.end()) {
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      if (req.ioFlag.test(idx) || !bRandomTweak) {
        auto &mapping = mappingList->second.at(idx);

        if (mapping.first < param.totalPhysicalBlocks &&
            mapping.second < param.pagesInBlock) {
          block = blocks.find(mapping.first);

          // Invalidate current page
          block->second.invalidate(mapping.second, idx);
        }
      }
    }
  }
  else {
    // Create empty mapping
    auto ret = table.emplace(
        req.lpn,
        std::vector<std::pair<uint32_t, uint32_t>>(
            bitsetSize, {param.totalPhysicalBlocks, param.pagesInBlock}));

    if (!ret.second) {
      panic("Failed to insert new mapping");
    }

    mappingList = ret.first;
  }

  // Write data to free block
  block = blocks.find(getLastFreeBlock(req.ioFlag));

  if (block == blocks.end()) {
    panic("No such block");
  }

  if (sendToPAL) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
      pDRAM->write(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
      pDRAM->write(&(*mappingList), 8, tick);
    }
  }

  if (!bRandomTweak && !req.ioFlag.all()) {
    // We have to read old data
    readBeforeWrite = true;
  }

  for (uint32_t idx = 0; idx < bitsetSize; idx++) {
    if (req.ioFlag.test(idx) || !bRandomTweak) {
      uint32_t pageIndex = block->second.getNextWritePageIndex(idx);
      auto &mapping = mappingList->second.at(idx);

      beginAt = tick;

      block->second.write(pageIndex, req.lpn, idx, beginAt);

      // Read old data if needed (Only executed when bRandomTweak = false)
      // Maybe some other init procedures want to perform 'partial-write'
      // So check sendToPAL variable
      if (readBeforeWrite && sendToPAL) {
        palRequest.blockIndex = mapping.first;
        palRequest.pageIndex = mapping.second;

        // We don't need to read old data
        palRequest.ioFlag = req.ioFlag;
        palRequest.ioFlag.flip();

        pPAL->read(palRequest, beginAt);
      }

      // update mapping to table
      mapping.first = block->first;
      mapping.second = pageIndex;

      if (sendToPAL) {
        palRequest.blockIndex = block->first;
        palRequest.pageIndex = pageIndex;

        if (bRandomTweak) {
          palRequest.ioFlag.reset();
          palRequest.ioFlag.set(idx);
        }
        else {
          palRequest.ioFlag.set();
        }

        pPAL->write(palRequest, beginAt);
      }

      finishedAt = MAX(finishedAt, beginAt);

      if (sendToPAL){
        // Predict error
        uint32_t eraseCount = block->second.getEraseCount();
        uint32_t layerNumber = mapping.second % 64;
        
        //debugprint(LOG_FTL_PAGE_MAPPING, "P/E, layerNum: %u, %u", eraseCount, layerNumber);
        for (uint32_t i = bloomFilters.size(), j = (1<<(bloomFilters.size()-1)); i > 0 ; i--, j=j>>1){
          if (i == bloomFilters.size()) {
            setRefreshPeriod(block->first, layerNumber, i-1);
            continue;
          }
        
          float newRBER = errorModel.getRBER(refresh_period * 1000000000ULL * j, eraseCount, layerNumber);
          
          debugprint(LOG_FTL_PAGE_MAPPING, "%u period RBER: %f", i, newRBER);

          if (newRBER > 0.01){ // 10^-2 = ECC capability
            //debugprint(LOG_FTL_PAGE_MAPPING, "insert %u, %u, %u", block->first, layerNumber, i);
            setRefreshPeriod(block->first, layerNumber, i-1);
          }
        }
      }

      //TODO: Now error count can be used to put layer to bloom filter

    }
  }

  // TODO: Have to record for each page
  //block->second.setLastWrittenTime(tick);

  // Exclude CPU operation when initializing
  if (sendToPAL) {
    tick = finishedAt;
    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::WRITE_INTERNAL);
  }

  // GC if needed
  // I assumed that init procedure never invokes GC
  static float gcThreshold = conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO);

  if (freeBlockRatio() < gcThreshold) {
    if (!sendToPAL) {
      panic("ftl: GC triggered while in initialization");
    }

    std::vector<uint32_t> list;
    uint64_t beginAt = tick;

    std::vector<uint32_t> dummy;
    selectVictimBlock(list, beginAt, dummy);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | On-demand | %u blocks will be reclaimed", list.size());

    doGarbageCollection(list, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "GC   | Done | %" PRIu64 " - %" PRIu64 " (%" PRIu64 ")", tick,
               beginAt, beginAt - tick);

    stat.gcCount++;
    stat.reclaimedBlocks += list.size();
  }
  /*
  if (tick - lastRefreshed > 1000000000 && sendToPAL){  // check every 1ms

    std::vector<uint32_t> list;
    uint64_t beginAt = tick;

    selectRefreshVictim(list, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "REFRESH   | On-time | %u blocks will be refreshed", list.size());

    doRefresh(list, beginAt);

    debugprint(LOG_FTL_PAGE_MAPPING,
               "REFRESH   | Done | %" PRIu64 " - %" PRIu64 " (%" PRIu64 ")", tick,
               beginAt, beginAt - tick);

    if (list.size() > 0) {
      stat.refreshCount++;
      stat.refreshedBlocks += list.size();
    }
    lastRefreshed = tick;
  }
  */
}

void PageMapping::trimInternal(Request &req, uint64_t &tick) {
  auto mappingList = table.find(req.lpn);

  if (mappingList != table.end()) {
    if (bRandomTweak) {
      pDRAM->read(&(*mappingList), 8 * req.ioFlag.count(), tick);
    }
    else {
      pDRAM->read(&(*mappingList), 8, tick);
    }

    // Do trim
    for (uint32_t idx = 0; idx < bitsetSize; idx++) {
      auto &mapping = mappingList->second.at(idx);
      auto block = blocks.find(mapping.first);

      if (block == blocks.end()) {
        panic("Block is not in use");
      }

      block->second.invalidate(mapping.second, idx);
    }

    // Remove mapping
    table.erase(mappingList);

    tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::TRIM_INTERNAL);
  }
}

void PageMapping::eraseInternal(PAL::Request &req, uint64_t &tick) {
  static uint64_t threshold =
      conf.readUint(CONFIG_FTL, FTL_BAD_BLOCK_THRESHOLD);
  auto block = blocks.find(req.blockIndex);

  // Sanity checks
  if (block == blocks.end()) {
    panic("No such block");
  }

  if (block->second.getValidPageCount() != 0) {
    panic("There are valid pages in victim block");
  }

  // Erase block
  block->second.erase();

  pPAL->erase(req, tick);

  // Check erase count
  uint32_t erasedCount = block->second.getEraseCount();

  if (erasedCount < threshold) {
    // Reverse search
    auto iter = freeBlocks.end();

    while (true) {
      iter--;

      if (iter->getEraseCount() <= erasedCount) {
        // emplace: insert before pos
        iter++;

        break;
      }

      if (iter == freeBlocks.begin()) {
        break;
      }
    }

    // Insert block to free block list
    freeBlocks.emplace(iter, std::move(block->second));
    nFreeBlocks++;
  }

  // Remove block from block list
  blocks.erase(block);

  tick += applyLatency(CPU::FTL__PAGE_MAPPING, CPU::ERASE_INTERNAL);
}

float PageMapping::calculateWearLeveling() {
  uint64_t totalEraseCnt = 0;
  uint64_t sumOfSquaredEraseCnt = 0;
  uint64_t numOfBlocks = param.totalLogicalBlocks;
  uint64_t eraseCnt;

  for (auto &iter : blocks) {
    eraseCnt = iter.second.getEraseCount();
    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  // freeBlocks is sorted
  // Calculate from backward, stop when eraseCnt is zero
  for (auto riter = freeBlocks.rbegin(); riter != freeBlocks.rend(); riter++) {
    eraseCnt = riter->getEraseCount();

    if (eraseCnt == 0) {
      break;
    }

    totalEraseCnt += eraseCnt;
    sumOfSquaredEraseCnt += eraseCnt * eraseCnt;
  }

  if (sumOfSquaredEraseCnt == 0) {
    return -1;  // no meaning of wear-leveling
  }

  return (float)totalEraseCnt * totalEraseCnt /
         (numOfBlocks * sumOfSquaredEraseCnt);
}

void PageMapping::calculateTotalPages(uint64_t &valid, uint64_t &invalid) {
  valid = 0;
  invalid = 0;

  for (auto &iter : blocks) {
    valid += iter.second.getValidPageCount();
    invalid += iter.second.getDirtyPageCount();
  }
}

float PageMapping::calculateAverageError(){
  uint64_t totalError = 0;
  float validBlockCount = 0;

  for (auto &iter : blocks) {
    totalError = totalError + iter.second.getMaxErrorCount();  
    validBlockCount = validBlockCount + 1;
  }

  float averageError = totalError / validBlockCount;

  return averageError;
}

void PageMapping::getStatList(std::vector<Stats> &list, std::string prefix) {
  Stats temp;

  temp.name = prefix + "page_mapping.gc.count";
  temp.desc = "Total GC count";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.gc.reclaimed_blocks";
  temp.desc = "Total reclaimed blocks in GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.gc.superpage_copies";
  temp.desc = "Total copied valid superpages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.gc.page_copies";
  temp.desc = "Total copied valid pages during GC";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.count";
  temp.desc = "Total Refresh count";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.refreshed_blocks";
  temp.desc = "Total blocks been refreshed";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.superpage_copies";
  temp.desc = "Total copied valid superpages during Refresh";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.page_copies";
  temp.desc = "Total copied valid pages during Refresh";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.call_count";
  temp.desc = "The number of refresh call";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.layer_check_count";
  temp.desc = "The number of total layer check";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.refresh.error_counts";
  temp.desc = "The average number of errors";
  list.push_back(temp);

  // For the exact definition, see following paper:
  // Li, Yongkun, Patrick PC Lee, and John Lui.
  // "Stochastic modeling of large-scale solid-state storage systems: analysis,
  // design tradeoffs and optimization." ACM SIGMETRICS (2013)
  temp.name = prefix + "page_mapping.wear_leveling";
  temp.desc = "Wear-leveling factor";
  list.push_back(temp);

  temp.name = prefix + "page_mapping.freeBlock_counts";
  temp.desc = "The number of free blocks left";
  list.push_back(temp);

  
  if (bloomFilters.size()){
    for(uint32_t i=0; i<bloomFilters.size(); i++){
      temp.name = prefix + "page_mapping.bloomFilter";
      temp.desc = "The number elements of bf-";
      list.push_back(temp);
    }
  }
  
}

void PageMapping::getStatValues(std::vector<double> &values) {
  values.push_back(stat.gcCount);
  values.push_back(stat.reclaimedBlocks);
  values.push_back(stat.validSuperPageCopies);
  values.push_back(stat.validPageCopies);
 
  values.push_back(stat.refreshCount);
  values.push_back(stat.refreshedBlocks);
  values.push_back(stat.refreshSuperPageCopies);
  values.push_back(stat.refreshPageCopies);
  values.push_back(stat.refreshCallCount);
  values.push_back(stat.layerCheckCount);

  values.push_back(calculateAverageError());
  values.push_back(calculateWearLeveling());

  values.push_back(nFreeBlocks);
  
  if (bloomFilters.size()){
    for(uint32_t i=0; i<bloomFilters.size(); i++){
      values.push_back(bloomFilters[i].element_count());
    }
  }
  
  

}

void PageMapping::resetStatValues() {
  memset(&stat, 0, sizeof(stat));
}

}  // namespace FTL

}  // namespace SimpleSSD
