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

#ifndef __FTL_HYBRID_MAPPING__
#define __FTL_HYBRID_MAPPING__

#include <cinttypes>
#include <unordered_map>
#include <vector>

#include "ftl/abstract_ftl.hh"
#include "ftl/common/block.hh"
#include "ftl/ftl.hh"
#include "pal/pal.hh"

namespace SimpleSSD {

namespace FTL {

class HybridMapping : public AbstractFTL {
 private:
  PAL::PAL *pPAL;

  ConfigReader &conf;

  // LBN -> PBN
  std::unordered_map<uint32_t, uint32_t> LogBlockTable;
  // LBN -> map(Logical page offset, physical page offset)
  std::unordered_map<uint32_t, std::vector<uint32_t>>
     LogPageMappingTable;
  // LBN -> PBN
  std::unordered_map<uint32_t, uint32_t> DataBlockTable;
  std::unordered_map<uint32_t, Block> blocks;
  std::list<Block> freeBlocks;
  std::list<Block> freeBlocksforMerge;
  uint32_t nFreeBlocks;
  uint32_t nFreeBlocksforMerge;
  std::vector<uint32_t> lastFreeBlock;
  Bitset lastFreeBlockIOMap;
  uint32_t lastFreeBlockIndex;

  uint32_t nToReclaim;
  bool bRandomTweak;
  uint32_t bitsetSize;

  struct {
    uint64_t gcCount;
    uint64_t reclaimedBlocks;
    uint64_t validSuperPageCopies;
    uint64_t validPageCopies;
  } stat;

  float freeBlockRatio();
  uint32_t getLBN(uint64_t);
  uint32_t getLPNoffset(uint64_t);
  uint32_t getFreeBlock(uint32_t, bool = false);
  uint32_t getLastFreeBlock(Bitset &);
  void calculateTotalPages(uint64_t &, uint64_t &);

  bool sequential(uint32_t &, uint64_t &, bool = false);
  void merge(uint32_t &, uint64_t &, bool = false);

  void selectBlocksToReclaim(std::vector<uint32_t> &, uint64_t&);

  void readInternal(Request &, uint64_t &);
  void writeInternal(Request &, uint64_t &, bool = true);
  void trimInternal(Request &, uint64_t &);
  void eraseInternal(PAL::Request &, uint64_t &, bool, bool = false);

 public:
  HybridMapping(ConfigReader &, Parameter &, PAL::PAL *, DRAM::AbstractDRAM *);
  ~HybridMapping();

  bool initialize() override;

  void read(Request &, uint64_t &) override;
  void write(Request &, uint64_t &) override;
  void trim(Request &, uint64_t &) override;

  void format(LPNRange &, uint64_t &) override;

  Status *getStatus(uint64_t, uint64_t) override;

  void getStatList(std::vector<Stats> &, std::string) override;
  void getStatValues(std::vector<double> &) override;
  void resetStatValues() override;
};

}  // namespace FTL

}  // namespace SimpleSSD

#endif