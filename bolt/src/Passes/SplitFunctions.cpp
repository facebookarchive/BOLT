//===--- SplitFunctions.cpp - pass for splitting function code ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "BinaryFunction.h"
#include "ParallelUtilities.h"
#include "SplitFunctions.h"
#include "llvm/Support/Options.h"

#include <numeric>
#include <vector>

#define DEBUG_TYPE "bolt-opts"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltOptCategory;

extern cl::opt<bool> SplitEH;

static cl::opt<bool>
AggressiveSplitting("split-all-cold",
  cl::desc("outline as many cold basic blocks as possible"),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

static cl::opt<unsigned>
SplitAlignThreshold("split-align-threshold",
  cl::desc("when deciding to split a function, apply this alignment "
           "while doing the size comparison (see -split-threshold). "
           "Default value: 2."),
  cl::init(2),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltOptCategory));

static cl::opt<SplitFunctions::SplittingType>
SplitFunctions("split-functions",
  cl::desc("split functions into hot and cold regions"),
  cl::init(SplitFunctions::ST_NONE),
  cl::values(clEnumValN(SplitFunctions::ST_NONE, "0",
                        "do not split any function"),
             clEnumValN(SplitFunctions::ST_LARGE, "1",
                        "in non-relocation mode only split functions too large "
                        "to fit into original code space"),
             clEnumValN(SplitFunctions::ST_LARGE, "2",
                        "same as 1 (backwards compatibility)"),
             clEnumValN(SplitFunctions::ST_ALL, "3",
                        "split all functions")),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

static cl::opt<unsigned>
SplitThreshold("split-threshold",
  cl::desc("split function only if its main size is reduced by more than "
           "given amount of bytes. Default value: 0, i.e. split iff the "
           "size is reduced. Note that on some architectures the size can "
           "increase after splitting."),
  cl::init(0),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltOptCategory));

void syncOptions(BinaryContext &BC) {
  if (!BC.HasRelocations && opts::SplitFunctions == SplitFunctions::ST_LARGE)
    opts::SplitFunctions = SplitFunctions::ST_ALL;
}

} // namespace opts

namespace llvm {
namespace bolt {

void SplitFunctions::runOnFunctions(BinaryContext &BC) {
  opts::syncOptions(BC);

  if (opts::SplitFunctions == SplitFunctions::ST_NONE)
    return;

  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    splitFunction(BF);
  };

  ParallelUtilities::PredicateTy SkipFunc = [&](const BinaryFunction &BF) {
    return !shouldOptimize(BF);
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_BB_LINEAR, WorkFun, SkipFunc,
      "SplitFunctions");
}

void SplitFunctions::splitFunction(BinaryFunction &BF) const {
  if (!BF.size())
    return;

  if (!BF.hasValidProfile())
    return;

  bool AllCold = true;
  for (auto *BB : BF.layout()) {
    auto ExecCount = BB->getExecutionCount();
    if (ExecCount == BinaryBasicBlock::COUNT_NO_PROFILE)
      return;
    if (ExecCount != 0)
      AllCold = false;
  }

  if (AllCold)
    return;

  auto PreSplitLayout = BF.getLayout();

  auto &BC = BF.getBinaryContext();
  size_t OriginalHotSize;
  size_t HotSize;
  size_t ColdSize;
  if (BC.isX86()) {
    std::tie(OriginalHotSize, ColdSize) = BC.calculateEmittedSize(BF);
    DEBUG(dbgs() << "Estimated size for function " << BF << " pre-split is <0x"
                 << Twine::utohexstr(OriginalHotSize) << ", 0x"
                 << Twine::utohexstr(ColdSize) << ">\n");
  }

  if (opts::SplitFunctions == SplitFunctions::ST_LARGE && !BC.HasRelocations) {
    // Split only if the function wouldn't fit.
    if (OriginalHotSize <= BF.getMaxSize())
      return;
  }

  // Never outline the first basic block.
  BF.layout_front()->setCanOutline(false);
  for (auto *BB : BF.layout()) {
    if (!BB->canOutline())
      continue;
    if (BB->getExecutionCount() != 0) {
      BB->setCanOutline(false);
      continue;
    }
    // Do not split extra entry points in aarch64. They can be referred by
    // using ADRs and when this happens, these blocks cannot be placed far
    // away due to the limited range in ADR instruction.
    if (BC.isAArch64() && BB->isEntryPoint()) {
      BB->setCanOutline(false);
      continue;
    }
    if (BF.hasEHRanges() && !opts::SplitEH) {
      // We cannot move landing pads (or rather entry points for landing
      // pads).
      if (BB->isLandingPad()) {
        BB->setCanOutline(false);
        continue;
      }
      // We cannot move a block that can throw since exception-handling
      // runtime cannot deal with split functions. However, if we can guarantee
      // that the block never throws, it is safe to move the block to
      // decrease the size of the function.
      for (auto &Instr : *BB) {
        if (BF.getBinaryContext().MIB->isInvoke(Instr)) {
          BB->setCanOutline(false);
          break;
        }
      }
    }
  }

  if (opts::AggressiveSplitting) {
    // All blocks with 0 count that we can move go to the end of the function.
    // Even if they were natural to cluster formation and were seen in-between
    // hot basic blocks.
    std::stable_sort(BF.layout_begin(), BF.layout_end(),
        [&] (BinaryBasicBlock *A, BinaryBasicBlock *B) {
          return A->canOutline() < B->canOutline();
        });
  } else if (BF.hasEHRanges() && !opts::SplitEH) {
    // Typically functions with exception handling have landing pads at the end.
    // We cannot move beginning of landing pads, but we can move 0-count blocks
    // comprising landing pads to the end and thus facilitate splitting.
    auto FirstLP = BF.layout_begin();
    while ((*FirstLP)->isLandingPad())
      ++FirstLP;

    std::stable_sort(FirstLP, BF.layout_end(),
        [&] (BinaryBasicBlock *A, BinaryBasicBlock *B) {
          return A->canOutline() < B->canOutline();
        });
  }

  // Separate hot from cold starting from the bottom.
  for (auto I = BF.layout_rbegin(), E = BF.layout_rend();
       I != E; ++I) {
    BinaryBasicBlock *BB = *I;
    if (!BB->canOutline())
      break;
    BB->setIsCold(true);
  }

  // Check the new size to see if it's worth splitting the function.
  if (BC.isX86() && BF.isSplit()) {
    std::tie(HotSize, ColdSize) = BC.calculateEmittedSize(BF);
    DEBUG(dbgs() << "Estimated size for function " << BF << " post-split is <0x"
                 << Twine::utohexstr(HotSize) << ", 0x"
                 << Twine::utohexstr(ColdSize) << ">\n");
    if (alignTo(OriginalHotSize, opts::SplitAlignThreshold) <=
        alignTo(HotSize, opts::SplitAlignThreshold) + opts::SplitThreshold) {
      DEBUG(dbgs() << "Reversing splitting of function " << BF << ":\n  0x"
                   << Twine::utohexstr(HotSize) << ", 0x"
                   << Twine::utohexstr(ColdSize) << " -> 0x"
                   << Twine::utohexstr(OriginalHotSize) << '\n');

      BF.updateBasicBlockLayout(PreSplitLayout);
      for (auto &BB : BF) {
        BB.setIsCold(false);
      }
    }
  }
}

} // namespace bolt
} // namespace llvm
