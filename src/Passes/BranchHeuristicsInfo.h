//===---- BranchHeuristicsInfo.h - Predict Branches Based on Heuristics ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
// This class is a helper to find which heuristic is applicable to a two-way
// conditional branch based on the analize of its successors. The heuristics
// implemented here are the one descibred in Ball and Larus' paper [1], and its
// taken probabilities were the fixed values extracted from the paper of Wu and
// Larus [2] as follows:
//
//   1) Loop Branch Heuristic (88%)
//   2) Pointer Heuristic     (60%)
//   3) Call Heuristic        (78%)
//   4) Opcode Heuristic      (84%)
//   5) Loop Exit Heuristic   (80%)
//   6) Return Heuristic      (72%)
//   7) Store Heuristic       (55%)
//   8) Loop Header Heuristic (75%)
//   9) Guard Heuristic       (62%)
//
// References:
//
// [1] Thomas Ball and James R. Larus. 1993. Branch prediction for free. In
// Proceedings of the ACM SIGPLAN 1993 conference on Programming language
// design and implementation (PLDI ’93). Association for Computing Machinery,
// New York, NY, USA, 300–313. DOI:https://doi.org/10.1145/155090.155119
//
// [2] Youfeng Wu and James R. Larus. 1994. Static branch frequency and
// program profile analysis. In Proceedings of the 27th annual international
// symposium on Microarchitecture (MICRO 27). Association for Computing
// Machinery, New York, NY, USA, 1–11. DOI:https://doi.org/10.1145/192724.192725
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_BRANCHHEURISTICSINFO_H_
#define LLVM_TOOLS_LLVM_BOLT_PASSES_BRANCHHEURISTICSINFO_H_

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "BinaryLoop.h"
#include "DominatorAnalysis.h"
#include "Passes/BinaryPasses.h"
#include "Passes/StaticBranchInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace bolt {

/// A Prediction is a pair of successors basic blocks of a two-way conditional
/// branch. The first element of this pair is the taken basic block and the
/// second the not taken basic block.
typedef std::pair<BinaryBasicBlock *, BinaryBasicBlock *> PredictionInfo;

/// The order here follows the same order described in Ball and Larus' paper.
enum BranchHeuristics {
  LOOP_BRANCH_HEURISTIC = 0,
  POINTER_HEURISTIC,
  CALL_HEURISTIC,
  OPCODE_HEURISTIC,
  LOOP_EXIT_HEURISTIC,
  RETURN_HEURISTIC,
  STORE_HEURISTIC,
  LOOP_HEADER_HEURISTIC,
  GUARD_HEURISTIC
};

struct BranchProbabilities {
  const char *HeuristicName;
  enum BranchHeuristics Heuristic;
  const float TakenProbability;
  const float NotTakenProbability;
};

static constexpr unsigned NumBranchHeuristics = 9;

static constexpr struct BranchProbabilities BranchProbs[NumBranchHeuristics] = {
    {"Loop Branch Heuristic", LOOP_BRANCH_HEURISTIC, 0.88, 0.12},
    {"Pointer Heuristic", POINTER_HEURISTIC, 0.60, 0.40},
    {"Call Heuristic", CALL_HEURISTIC, 0.78, 0.22},
    {"Opcode Heuristic", OPCODE_HEURISTIC, 0.84, 0.16},
    {"Loop Exit Heuristic", LOOP_EXIT_HEURISTIC, 0.80, 0.20},
    {"Return Heuristic", RETURN_HEURISTIC, 0.72, 0.28},
    {"Store Heuristic", STORE_HEURISTIC, 0.55, 0.45},
    {"Loop Header Heuristic", LOOP_HEADER_HEURISTIC, 0.75, 0.25},
    {"Guard Heuristic", GUARD_HEURISTIC, 0.62, 0.38},
};

class BranchHeuristicsInfo {

private:
  using Edge = StaticBranchInfo::Edge;

  std::unique_ptr<StaticBranchInfo> BPI = std::make_unique<StaticBranchInfo>();

  /// Empty prediction info that indicates the heuristics applies to none
  /// of the basic block's successors being analized.
  const PredictionInfo NONE;

  /// loopBranchHeuristic - Predict as taken an edge back to a loop's
  /// head. Predict as not taken an edge exiting a loop.
  PredictionInfo loopBranchHeuristic(BinaryBasicBlock *BB) const;

  /// pointerHeuristic - Predict that a comparison of a pointer against
  /// null or of two pointers will fail.
  PredictionInfo pointerHeuristic(BinaryBasicBlock *BB) const;

  /// callHeuristic - Predict a successor that contains a call and does
  /// not post-dominate will not be taken.
  PredictionInfo callHeuristic(BinaryBasicBlock *BB,
                               DominatorAnalysis<true> &PDA) const;

  /// opcodeHeuristic - Predict that a comparison of an integer for less
  /// than zero, less than or equal to zero, or equal to a constant, will
  /// fail.
  PredictionInfo opcodeHeuristic(BinaryBasicBlock *BB) const;

  /// loopExitHeuristic - Predict that a comparison in a loop in which no
  /// successor is a loop head will not exit the loop.
  PredictionInfo loopExitHeuristic(BinaryBasicBlock *BB) const;

  /// returnHeuristic - Predict a successor that contains a return will
  /// not be taken.
  PredictionInfo returnHeuristic(BinaryBasicBlock *BB) const;

  /// storeHeuristic - Predict a successor that contains a store
  /// instruction and does not post-dominate will not be taken.
  PredictionInfo storeHeuristic(BinaryBasicBlock *BB,
                                DominatorAnalysis<true> &PDA) const;

  /// loopHeaderHeuristic - Predict a successor that is a loop header or
  /// a loop pre-header and does not post-dominate will be taken.
  PredictionInfo loopHeaderHeuristic(BinaryBasicBlock *BB,
                                     DominatorAnalysis<true> &PDA) const;

  // guardHeuristic - Predict that a comparison in which a register is
  /// an operand, the register is used before being defined in a successor
  /// block, and the successor block does not post-dominate will reach the
  /// successor block.
  PredictionInfo guardHeuristic(BinaryBasicBlock *BB) const;

public:
  /// getApplicableHeuristic - Checks which heuristic applies to the branch
  /// based on the premisse that exacly one successor of a branch can be
  /// identified as taken.
  PredictionInfo getApplicableHeuristic(BranchHeuristics BH,
                                        BinaryBasicBlock *BB,
                                        DominatorAnalysis<true> &PDA) const;

  inline static unsigned getNumHeuristics() { return NumBranchHeuristics; }

  inline static StringRef getHeuristicName(enum BranchHeuristics BH) {
    return BranchProbs[BH].HeuristicName;
  }

  inline static enum BranchHeuristics getHeuristic(unsigned IdHeuristic) {
    return BranchProbs[IdHeuristic].Heuristic;
  }

  inline static float getTakenProbability(enum BranchHeuristics BH) {
    return BranchProbs[BH].TakenProbability;
  }

  inline static float getNotTakenProbability(enum BranchHeuristics BH) {
    return BranchProbs[BH].NotTakenProbability;
  }
};

} // namespace bolt
} // namespace llvm

#endif /* LLVM_TOOLS_LLVM_BOLT_PASSES_BRANCHHEURISTICSINFO_H_ */
