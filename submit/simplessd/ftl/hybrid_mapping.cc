#include "ftl/hybrid_mapping.hh"

#include <algorithm>
#include <limits>
#include <random>

#include "util/algorithm.hh"
#include "util/bitset.hh"

namespace SimpleSSD {

namespace FTL {

const uint32_t HYBRID_LOG_BLOCK_LIMIT = 16384;

HybridMapping::HybridMapping(ConfigReader &c, Parameter &p, PAL::PAL *l,
                         DRAM::AbstractDRAM *d)
    : AbstractFTL(p, l, d),
      pPAL(l),
      conf(c) {
  blocks.reserve(param.totalPhysicalBlocks);
  LogBlockTable.reserve(HYBRID_LOG_BLOCK_LIMIT);
  LogPageMappingTable.reserve(HYBRID_LOG_BLOCK_LIMIT);
  DataBlockTable.reserve(param.totalLogicalBlocks);

  for (uint32_t i = 0; i < param.totalPhysicalBlocks; i++) {
    freeBlocks.emplace_back(Block(i, param.pagesInBlock, param.ioUnitInPage));
  }

  nFreeBlocks = param.totalPhysicalBlocks;

  status.totalLogicalPages = param.totalLogicalBlocks * param.pagesInBlock;

  memset(&stat, 0, sizeof(stat));

  bRandomTweak = conf.readUint(CONFIG_FTL, FTL_USE_RANDOM_IO_TWEAK);
  bitsetSize = bRandomTweak ? param.ioUnitInPage : 1;
}

HybridMapping::~HybridMapping() {}

bool HybridMapping::initialize() {
  uint64_t nPagesToWarmup;
  uint64_t nPagesToInvalidate;
  uint64_t nTotalLogicalPages;
  uint64_t maxPagesBeforeGC;
  uint64_t tick;
  uint64_t valid = 0;
  uint64_t invalid = 0;
  FILLING_MODE mode;

  Request req(param.ioUnitInPage);

  debugprint(LOG_FTL_HYBRID_MAPPING, "Initialization started");

  nTotalLogicalPages = param.totalLogicalBlocks * param.pagesInBlock;
  nPagesToWarmup =
      nTotalLogicalPages * conf.readFloat(CONFIG_FTL, FTL_FILL_RATIO);
  nPagesToInvalidate =
      nTotalLogicalPages * conf.readFloat(CONFIG_FTL, FTL_INVALID_PAGE_RATIO);
  mode = (FILLING_MODE)conf.readUint(CONFIG_FTL, FTL_FILLING_MODE);
  maxPagesBeforeGC =
      param.pagesInBlock *
      (param.totalLogicalBlocks *
           (1 - conf.readFloat(CONFIG_FTL, FTL_GC_THRESHOLD_RATIO)) -
       param.pageCountToMaxPerf);  // # free blocks to maintain

  if (nPagesToWarmup + nPagesToInvalidate > maxPagesBeforeGC) {
    warn("ftl: Too high filling ratio. Adjusting invalidPageRatio.");
    nPagesToInvalidate = maxPagesBeforeGC - nPagesToWarmup;
  }

  debugprint(LOG_FTL_HYBRID_MAPPING, "Total logical pages: %" PRIu64,
             nTotalLogicalPages);
  debugprint(LOG_FTL_HYBRID_MAPPING,
             "Total logical pages to fill: %" PRIu64 " (%.2f %%)",
             nPagesToWarmup, nPagesToWarmup * 100.f / nTotalLogicalPages);
  debugprint(LOG_FTL_HYBRID_MAPPING,
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

  // Report
  calculateTotalPages(valid, invalid);
  debugprint(LOG_FTL_HYBRID_MAPPING, "Filling finished. Page status:");
  debugprint(LOG_FTL_HYBRID_MAPPING,
             "  Total valid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             valid, valid * 100.f / nTotalLogicalPages, nPagesToWarmup,
             (int64_t)(valid - nPagesToWarmup));
  debugprint(LOG_FTL_HYBRID_MAPPING,
             "  Total invalid physical pages: %" PRIu64
             " (%.2f %%, target: %" PRIu64 ", error: %" PRId64 ")",
             invalid, invalid * 100.f / nTotalLogicalPages, nPagesToInvalidate,
             (int64_t)(invalid - nPagesToInvalidate));
  debugprint(LOG_FTL_HYBRID_MAPPING, "Initialization finished");

  return true;
}

void HybridMapping::read(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.ioFlag.count() > 0) {
    readInternal(req, tick);

    debugprint(LOG_FTL_HYBRID_MAPPING,
               "READ  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::READ);
}

void HybridMapping::write(Request &req, uint64_t &tick) {
  static uint64_t count = 0;
  count ++;
  uint64_t begin = tick;
  volatile static uint64_t ticknew = 0;
  volatile static uint64_t tickold = 0;
  (void) tickold;
  if (req.ioFlag.count() > 0) {
    writeInternal(req, tick);
    tickold = ticknew;
    ticknew = tick - begin;
    debugprint(LOG_FTL_HYBRID_MAPPING,
               "WRITE | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::WRITE);
}

void HybridMapping::trim(Request &req, uint64_t &tick) {
  uint64_t begin = tick;

  if (req.ioFlag.count() > 0) {
    trimInternal(req, tick);

    debugprint(LOG_FTL_HYBRID_MAPPING,
               "TRIM  | LPN %" PRIu64 " | %" PRIu64 " - %" PRIu64 " (%" PRIu64
               ")",
               req.lpn, begin, tick, tick - begin);
  }
  else {
    warn("FTL got empty request");
  }

  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::TRIM);
}

void HybridMapping::format(LPNRange &range, uint64_t &tick) {
  uint64_t begin = range.nlp;
  uint64_t end = tick;
  begin = end;
  end = begin;
  // No need to implement spcific logic for format function

  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::FORMAT);
}

Status *HybridMapping::getStatus(uint64_t lpnBegin, uint64_t lpnEnd){
  status.freePhysicalBlocks = nFreeBlocks;
  status.mappedLogicalPages = 0;
  uint32_t slbn = getLBN(lpnBegin);
  uint32_t elbn = getLBN(lpnEnd - 1);
  uint32_t slpnOffset = getLPNoffset(lpnBegin);
  uint32_t elpnOffset = getLPNoffset(lpnEnd - 1);
  for(auto iter = DataBlockTable.begin(); iter != DataBlockTable.end(); iter++) {
    auto block = blocks.find(iter->second);
    if(block == blocks.end()) {
      panic("Block not found in data block table");
    }
    uint32_t start = 0, end = param.pagesInBlock;
    if(block->first == slbn && block->first == elbn) {
      start = slpnOffset;
      end = elpnOffset;
    }
    else if(block->first == slbn) {
      start = slpnOffset;
    }
    else if(block->first == elbn) {
      end = elpnOffset;
    }
    status.mappedLogicalPages += block->second.getValidPageCount(start, end);
  }
  for(auto iter = LogPageMappingTable.begin(); iter != LogPageMappingTable.end(); iter++) {
    uint32_t start = 0, end = param.pagesInBlock;
    if(iter->first == slbn && iter->first == elbn) {
      start = slpnOffset;
      end = elpnOffset;
    }
    else if(iter->first == slbn) {
      start = slpnOffset;
    }
    else if(iter->first == elbn) {
      end = elpnOffset;
    }
    for(uint32_t i = start; i < end; i++) {
      if(iter->second.at(i) != 0xFFFFFFFF) {
        status.mappedLogicalPages++;
      }
    }
  }
  return &status;
}

float HybridMapping::freeBlockRatio() {
  return (float)nFreeBlocks / param.totalLogicalBlocks;
}

uint32_t HybridMapping::getLBN(uint64_t lpn) {
  return lpn / param.pagesInBlock;
}

uint32_t HybridMapping::getLPNoffset(uint64_t lpn) {
  return lpn % param.pagesInBlock;
}

void HybridMapping::calculateTotalPages(uint64_t &valid, uint64_t &invalid){
  valid = 0;
  invalid = 0;

  for(auto &iter : blocks) {
    valid += iter.second.getValidPageCount();
    invalid += iter.second.getDirtyPageCount();
  }
}

void HybridMapping::readInternal(Request &req, uint64_t &tick){
  PAL::Request palRequest(req);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  uint32_t LogicBlockNumber = getLBN(req.lpn);
  uint32_t PageOffsetInBlock = getLPNoffset(req.lpn);

  auto logBlockPBN = LogBlockTable.find(LogicBlockNumber);

  auto dataBlockPBN = DataBlockTable.find(LogicBlockNumber);

  uint32_t reqReadblock = 0;
  uint32_t reqReadPage = 0;

  uint32_t logPageOffset = 0xFFFFFFFF;
  if(logBlockPBN != LogBlockTable.end()) { // log block exist

    auto logPageMapping = LogPageMappingTable.find(LogicBlockNumber); // log page table

    logPageOffset = logPageMapping->second.at(PageOffsetInBlock);

    pDRAM->read(&(*logBlockPBN), 8, tick);
    pDRAM->read(&(*logPageMapping), 8, tick);
  }

  bool PageInDataBlockValid = false;

  if(dataBlockPBN != DataBlockTable.end()) { // data block exist
    pDRAM->read(&(*dataBlockPBN), 8, tick);

    auto block = blocks.find(dataBlockPBN->second);

    if(block == blocks.end()) {
      panic("Block not in use");
    }

    std::vector<uint64_t> lpns;
    Bitset map(param.ioUnitInPage);

    if(block->second.getPageInfo(PageOffsetInBlock, lpns, map)) { // valid page
      PageInDataBlockValid = true;
    }
  }

  if(logBlockPBN != LogBlockTable.end() && logPageOffset != 0xFFFFFFFF){ // In log block
    reqReadblock = logBlockPBN->second;
    reqReadPage = logPageOffset;
  }
  else if(((logBlockPBN != LogBlockTable.end() && logPageOffset == 0xFFFFFFFF) ||
           logBlockPBN == LogBlockTable.end()) &&
           PageInDataBlockValid){ // Not in log block, and data block page is valid
    reqReadblock = dataBlockPBN->second;
    reqReadPage = PageOffsetInBlock;
  }
  else {
    // panic("Read request to invalid page");
    return;
  }

  for(uint32_t idx = 0; idx < bitsetSize; idx++) {
    if(req.ioFlag.test(idx) || !bRandomTweak) {
      if(reqReadblock < param.totalLogicalBlocks &&
         reqReadPage < param.pagesInBlock) {
        
        palRequest.blockIndex = reqReadblock;
        palRequest.pageIndex = reqReadPage;
        
        if(bRandomTweak){
          palRequest.ioFlag.reset();
          palRequest.ioFlag.set(idx);
        }
        else {
          palRequest.ioFlag.set();
        }

        auto block = blocks.find(palRequest.blockIndex);

        if(block == blocks.end()) {
          panic("Block not found");
        }

        beginAt = tick;

        block->second.read(palRequest.pageIndex, idx, beginAt);
        pPAL->read(palRequest, beginAt);

        finishedAt = MAX(finishedAt, beginAt);
      }
    }
  }
  tick = finishedAt;
  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::READ_INTERNAL);
}


void HybridMapping::writeInternal(Request &req, uint64_t &tick, bool sendToPAL){
  PAL::Request palRequest(req);
  uint64_t beginAt;
  uint64_t finishedAt = tick;

  uint32_t LogicBlockNumber = getLBN(req.lpn);
  uint32_t PageOffsetInBlock = getLPNoffset(req.lpn);

  auto logBlockPBN = LogBlockTable.find(LogicBlockNumber);
  auto logPageMapping = LogPageMappingTable.find(LogicBlockNumber);
  auto dataBlockPBN = DataBlockTable.find(LogicBlockNumber);

  if(sendToPAL) {
    pDRAM->read(&(*logBlockPBN), 8, tick);
    pDRAM->read(&(*dataBlockPBN), 8, tick);
  }

  if(logBlockPBN == LogBlockTable.end()) {
    uint32_t newLogBlock = getFreeBlock(tick, sendToPAL);
    
    auto ret1 = LogBlockTable.emplace(LogicBlockNumber, newLogBlock);
    auto ret2 = LogPageMappingTable.emplace(
      LogicBlockNumber, std::vector<uint32_t>(param.pagesInBlock, 0xFFFFFFFF));

    if(!ret1.second || !ret2.second) {
      panic("Failed to insert log block");
    }

    logBlockPBN = ret1.first;
    logPageMapping = ret2.first;
  }

  auto block = blocks.find(logBlockPBN->second);
  if(block == blocks.end()) {
    panic("Block not found");
  }

  if(logPageMapping->second.at(PageOffsetInBlock) != 0xFFFFFFFF) {
    block->second.invalidate(logPageMapping->second.at(PageOffsetInBlock), 0);
  }

  // if(!bRandomTweak && !req.ioFlag.all()){
  //   readBeforeWrite = true;
  // }

  if(block->second.getNextWritePageIndex() == param.pagesInBlock) {
    if(!sequential(LogicBlockNumber, tick, sendToPAL)) {
      merge(LogicBlockNumber, tick, sendToPAL);
    }

    uint32_t newLogBlock = getFreeBlock(tick, sendToPAL);

    auto ret1 = LogBlockTable.emplace(LogicBlockNumber, newLogBlock);
    auto ret2 = LogPageMappingTable.emplace(
      LogicBlockNumber, std::vector<uint32_t>(param.pagesInBlock, 0xFFFFFFFF));
    
    if(!ret1.second || !ret2.second) {
      panic("Failed to insert log block");
    }

    logBlockPBN = ret1.first;
    logPageMapping = ret2.first;

    block = blocks.find(logBlockPBN->second);
    if(block == blocks.end()) {
      panic("Block not found");
    }
  }

  if(dataBlockPBN != DataBlockTable.end()) {
    auto block = blocks.find(dataBlockPBN->second);

    if(block == blocks.end()) {
      panic("Block not found");
    }

    block->second.invalidate(PageOffsetInBlock, 0);
  }

  uint32_t pageIndex = block->second.getNextWritePageIndex();

  block->second.write(pageIndex, req.lpn, 0, tick);

  logPageMapping->second.at(PageOffsetInBlock) = pageIndex;


  if(sendToPAL){
    pDRAM->write(&(*logPageMapping), 8, tick);

    palRequest.blockIndex = logBlockPBN->second;
    palRequest.pageIndex = pageIndex;

    if(bRandomTweak){
      palRequest.ioFlag.reset();
      palRequest.ioFlag.set();
    }
    else {
      palRequest.ioFlag.set();
    }

    beginAt = tick;
    pPAL->write(palRequest, beginAt);
    finishedAt = MAX(finishedAt, beginAt);

    tick = finishedAt;
    // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::WRITE_INTERNAL);
  }

  // TODO:Do GC if needed
  
}

uint32_t HybridMapping::getFreeBlock(uint64_t & tick, bool sendToPAL, bool forMerge){
  uint32_t nLogBlock = LogBlockTable.size();
  // Check if too many log blocks:

  if(!forMerge && nLogBlock >= HYBRID_LOG_BLOCK_LIMIT){
    // Use random function to select victim block
    uint32_t victim = random() % LogBlockTable.size() - 1;
    victim %= LogBlockTable.size();
    auto iter = LogBlockTable.begin();
    for(uint32_t idx = 0; idx < victim; idx ++){
      iter ++;
    }
    auto LBN = iter->first;
    if(!sequential(LBN, tick, sendToPAL)){
      merge(LBN, tick, sendToPAL);
    }
  }

  uint32_t blockIndex = 0;


  if(nFreeBlocks > 0) {
    auto iter = freeBlocks.begin();

    blockIndex = iter->getBlockIndex();

    if(blocks.find(blockIndex) != blocks.end()) {
      panic("Block already in use");
    }

    blocks.emplace(blockIndex, std::move(*iter));

    freeBlocks.erase(iter);
    nFreeBlocks--;
  }
  else {
    panic("No free block");
  }

  if(nFreeBlocks <= 512){
    auto iter = LogBlockTable.begin();
    auto LBN = iter->first;
    if(!sequential(LBN, tick, sendToPAL)){
      merge(LBN, tick, sendToPAL);
    }
  }

  return blockIndex;
}


void HybridMapping::merge(uint32_t &LBN, uint64_t &tick, bool sendToPAL){
  PAL::Request req(param.ioUnitInPage);
  std::vector<PAL::Request> readReqs;
  std::vector<PAL::Request> writeReqs;
  std::vector<PAL::Request> eraseReqs;
  std::vector<uint64_t> lpns;
  Bitset bit(param.ioUnitInPage);
  uint64_t beginAt;
  uint64_t readFinishedAt = tick;
  uint64_t writeFinishedAt = tick;
  uint64_t eraseFinishedAt = tick;

  auto logBlockPBN = LogBlockTable.find(LBN);
  auto dataBlockPBN = DataBlockTable.find(LBN);
  auto logPageMapping = LogPageMappingTable.find(LBN);

  if (logBlockPBN == LogBlockTable.end()) {
    panic("Invalid merge request");
  }

  if(sendToPAL){
    pDRAM->read(&(*logBlockPBN), 8, tick);
  }

  auto logBlock = blocks.find(logBlockPBN->second);

  std::unordered_map<uint32_t, SimpleSSD::FTL::Block>::iterator dataBlock = blocks.end();
  
  if(dataBlockPBN != DataBlockTable.end()){
    dataBlock = blocks.find(dataBlockPBN->second);

    if(sendToPAL){
      pDRAM->read(&(*dataBlockPBN), 8, tick);
    }
  }

  uint32_t freeBlockforM = getFreeBlock(tick, sendToPAL, true); // get free block for merge

  if(LogBlockTable.find(LBN) == LogBlockTable.end()) return;

  for(uint32_t idx = 0; idx < param.pagesInBlock; idx++) {
    // get physical page offset in log block
    auto physicalPageOffset = logPageMapping->second.at(idx);

    if(sendToPAL){
      pDRAM->read(&physicalPageOffset, 8, tick);
    }
    // If target page is in log block, copy it to free block
    if(physicalPageOffset != 0xFFFFFFFF) {
      if(!bRandomTweak) {
        bit.set();
      }
      // Read target page
      req.blockIndex = logBlockPBN->second;
      req.pageIndex = physicalPageOffset;
      req.ioFlag = bit;

      readReqs.push_back(req);

      if(sendToPAL){
        logBlock->second.read(physicalPageOffset, 0, tick);
      }

      logBlock->second.invalidate(physicalPageOffset, 0);

    }
    else if(dataBlock != blocks.end() &&
            dataBlock->second.getPageInfo(idx, lpns, bit)) { // If target page is in data block
      // Issue Read
      if(!bRandomTweak) {
        bit.set();
      }

      req.blockIndex = dataBlockPBN->second;
      req.pageIndex = idx;
      req.ioFlag = bit;

      readReqs.push_back(req);

      if(sendToPAL){
        dataBlock->second.read(idx, 0, tick);
      }
      dataBlock->second.invalidate(idx, 0);

    }
    else{
      // Target page is empty
      continue;
    }

    auto freeBlock = blocks.find(freeBlockforM);
    uint64_t lpn = LBN * param.pagesInBlock + idx;

    if(sendToPAL){
      freeBlock->second.write(idx, lpn, 0, tick);
    }

    // Issue Write
    req.blockIndex = freeBlockforM;
    req.pageIndex = idx;

    if(!bRandomTweak) {
      req.ioFlag.set();
    }

    writeReqs.push_back(req);
  }


  if(dataBlock != blocks.end()) {
    // dataBlock->second.erase(); // erased in eraseInternal

    req.blockIndex = dataBlockPBN->second;
    req.pageIndex = 0;
    req.ioFlag.set();
    eraseReqs.push_back(req);
  }

  // logBlock->second.erase(); // So as above

  req.blockIndex = logBlockPBN->second;
  req.pageIndex = 0;
  req.ioFlag.set();

  eraseReqs.push_back(req);

  // Do I/O here
  if(sendToPAL){
    for(auto &req : readReqs) {
      beginAt = tick;

      pPAL->read(req, beginAt);

      readFinishedAt = MAX(readFinishedAt, beginAt);
    }

    for(auto &req : writeReqs) {
      beginAt = tick;

      pPAL->write(req, beginAt);

      writeFinishedAt = MAX(writeFinishedAt, beginAt);
    }
  }
  // Either init or not, eraseInternal will be called
  for(auto &req : eraseReqs) {
    beginAt = tick;

    eraseInternal(req, beginAt, sendToPAL);

    eraseFinishedAt = MAX(eraseFinishedAt, beginAt);
  }

  tick = MAX(readFinishedAt, MAX(writeFinishedAt, eraseFinishedAt));

  // Update tables

  DataBlockTable[LBN] = freeBlockforM;
  LogBlockTable.erase(logBlockPBN);
  LogPageMappingTable.erase(logPageMapping);

  if(sendToPAL){
    pDRAM->write(&(*dataBlockPBN), 8, tick);
  }

  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::MERGE);
};


bool HybridMapping::sequential(uint32_t &LBN, uint64_t &tick, bool sendToPAL){
  bool ret = true;

  auto logPageMapping = LogPageMappingTable.find(LBN);

  for(uint32_t idx = 0; idx < param.pagesInBlock; idx++) {
    if(logPageMapping->second.at(idx) != idx) {
      ret = false;
      break;
    }

    if(sendToPAL){
      pDRAM->read(&logPageMapping, 8, tick);
    }
  }

  if(ret) { // switch
    auto logBlockPBN = LogBlockTable.find(LBN);
    auto dataBlockPBN = DataBlockTable.find(LBN);
    
    if(sendToPAL){
      pDRAM->read(&(*logBlockPBN), 8, tick);
      pDRAM->read(&(*dataBlockPBN), 8, tick);
    }

    if(dataBlockPBN == DataBlockTable.end()) {
      DataBlockTable.emplace(LBN, logBlockPBN->second);
    }
    else{
      PAL::Request req(param.ioUnitInPage);
      
      req.blockIndex = dataBlockPBN->second;
      req.pageIndex = 0;
      req.ioFlag.set();

      eraseInternal(req, tick, sendToPAL);
      DataBlockTable[LBN] = logBlockPBN->second;
    }

    if(sendToPAL){
      pDRAM->write(&(*dataBlockPBN), 8, tick);
    }

    LogBlockTable.erase(logBlockPBN);
    LogPageMappingTable.erase(logPageMapping);
  }

  return ret;
}

void HybridMapping::eraseInternal(PAL::Request &req, uint64_t &tick, bool sendToPAL){
  static uint64_t threshold = 
      conf.readUint(CONFIG_FTL, FTL_BAD_BLOCK_THRESHOLD);
  auto block = blocks.find(req.blockIndex);

  if(block == blocks.end()) {
    panic("Block not found");
  }

  if(block->second.getValidPageCount() != 0){
    panic("There are valid pages in victim block");
  }

  block->second.erase();

  if(sendToPAL){
    pPAL->erase(req, tick);
  }

  uint32_t erasedCount = block->second.getEraseCount();

  if(erasedCount < threshold){
    auto iter = freeBlocks.end();
    auto begin = freeBlocks.begin();

    while(true){
      iter --;

      if(iter->getEraseCount() <= erasedCount){
        iter ++;
        break;
      }

      if(iter == begin){
        break;
      }
    }

    freeBlocks.emplace(iter, std::move(block->second));
    nFreeBlocks++;
  }

  blocks.erase(block);

  // tick += applyLatency(CPU::FTL__HYBRID_MAPPING, CPU::ERASE_INTERNAL);
}


void HybridMapping::trimInternal(Request &, uint64_t &){};
void HybridMapping::getStatList(std::vector<Stats> &, std::string){};
void HybridMapping::getStatValues(std::vector<double> &){};
void HybridMapping::resetStatValues(){};

}  // namespace FTL

}  // namespace SimpleSSD
