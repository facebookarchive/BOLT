//===--- Passes/BlockEdgeFrequency.h - Block and Edge Frequencies ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file describe the interface to the pass that calculates binary basic
// blocks and flow edge counts for a function as described in Wu and
// Larus' paper [1].
//
// Note: In this file we are following the notation described in Wu and Larus'
// paper [1] for branch probability and branch or block frequency. 
// In this notation a branch probability is an estimate of the likelihood of 
// a branch being taken, and its value is a range between zero and one. A block 
// or branch frequency is a measure of how often a basic block or a branch is 
// executed or taken based in one call of the function containg the branch. 
// Since BOLT works with absoltute counts we multiply the computed frequency of 
// a given basic block or branch by the SCALING_FACTOR constant.
//
//
// References:
//
// [1] Youfeng Wu and James R. Larus. 1994. Static branch frequency and
// program profile analysis. In Proceedings of the 27th annual international
// symposium on Microarchitecture (MICRO 27). Association for Computing
// Machinery, New York, NY, USA, 1–11. DOI:https://doi.org/10.1145/192724.192725
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_BLOCKEDGEFREQUENCY_H_
#define LLVM_TOOLS_LLVM_BOLT_PASSES_BLOCKEDGEFREQUENCY_H_

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "DominatorAnalysis.h"
#include "Passes/BinaryPasses.h"
#include "Passes/StaticBranchInfo.h"
#include "Passes/StaticBranchProbabilities.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace llvm {
namespace bolt {

class BlockEdgeFrequency : public BinaryFunctionPass {

private:
  static constexpr double SCALING_FACTOR = 10000.0;
  static constexpr double EPSILON = 0.01 * SCALING_FACTOR;
  static constexpr double LOOSEBOUND = 0.2 * SCALING_FACTOR;

  using Edge = StaticBranchInfo::Edge;

  std::unique_ptr<StaticBranchInfo> BSI;
  std::unique_ptr<StaticBranchProbabilities> SBP;

  using BasicBlockOffset = std::pair<uint64_t, BinaryBasicBlock *>;
  struct CompareBasicBlockOffsets {
    bool operator()(const BasicBlockOffset &A,
                    const BasicBlockOffset &B) const {
      return A.first < B.first;
    }
  };

  /// Holds edges counts assuming entry BB count is one.
  std::map<Edge, uint64_t> CFGEdgeFrequencies;

  /// Holds BB counts assuming entry BB count is one.
  DenseMap<const BinaryBasicBlock *, uint64_t> BlockFrequencies;

  /// Holds all basic blocks reachable from head.
  DenseSet<const BinaryBasicBlock *> ReachableBBs;

  /// Holds all visited loops.
  DenseSet<const BinaryLoop *> VisitedLoops;

  /// isVisited - Checks if the basic block BB is marked as visited
  // by checking if it is not in the reachable set.
  bool isVisited(const BinaryBasicBlock *BB) const;

  /// setVisited - Marks the basic block BB as visited removing it
  /// from the reachable set.
  void setVisited(const BinaryBasicBlock *BB);

  /// tagReachableBlocks - Mark all blocks reachable from head block as not
  /// visited.
  void tagReachableBlocks(BinaryBasicBlock *Head);

  /// propagateLoopFrequencies - Propagates intraprocedural (or local) basic
  /// block and flow edge frequencies in a loop enclosure of a function.
  /// Processes the inner-most loop first and uses the cyclic probabilities
  /// of the inner loops to compute frequencies for the outer loops.
  void propagateLoopFrequencies(BinaryLoop *Loop);

  /// propagateFrequencies - Calculates intraprocedural (or local) basic
  /// block and flow edge frequencies by propagating branch probabilities over
  /// a given function's CFG.
  void propagateFrequencies(BinaryBasicBlock *BB, BinaryBasicBlock *Head);

  /// checkPrecision - Checks if the computed local function frequency is
  /// within the defined bound.
  /// Keep in mind that the algorithm implemented in this pass does not handle
  /// irreducible CFGs, which may lead to incorrect frequencies for these cases.
  /// Detect this by checking the exit BB frequency to tolerate some deviation
  /// in favor of having a practical algorithm.
  bool checkPrecision(BinaryFunction &Function) const;

  /// updateLocalCallFrequencies - Updates intraprocedural call frequencies.
  void updateLocalCallFrequencies(BinaryFunction &Function);

  /// getCFGEdgeFrequency - Get updated local flow edge frequency calculated
  /// using the algorithm described in the Wu Larus paper based on
  /// Ramamoothy's equations.
  uint64_t getCFGEdgeFrequency(Edge &CFGEdge) const;

  /// getCFGEdgeFrequency - Get updated local flow edge frequency calculated
  /// using the algorithm described in the Wu Larus paper based on
  /// Ramamoothy's equations.
  uint64_t getCFGEdgeFrequency(BinaryBasicBlock &SrcBB,
                               BinaryBasicBlock &DstBB);

  /// computeFrequencies - Computes intraprocedural block and intraprocedural
  /// flow edge frequencies with the frequencies based on the local block and
  /// edge frequency algorithm described in page 5 of the Wu Larus paper.
  bool computeFrequencies(BinaryFunction &Function);

  void dumpProfileData(BinaryFunction &Function, raw_ostream &Printer);

  uint64_t getBBExecutionCount(BinaryBasicBlock &BB);

public:
  explicit BlockEdgeFrequency(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {
    BSI = std::make_unique<StaticBranchInfo>();
    SBP = std::make_unique<StaticBranchProbabilities>();
  }

  /// getLocalEdgeFrequency - Get updated local flow edge frequency from the
  /// CFG.
  double getLocalEdgeFrequency(BinaryBasicBlock *SrcBB,
                               BinaryBasicBlock *DstBB);

  /// getLocalBlockFrequency - Get updated local block frequency from the CFG.
  double getLocalBlockFrequency(BinaryBasicBlock *BB);

  /// computeBlockEdgeFrequencies - Computes the local block and local
  /// flow edge frequencies for a given function.
  void computeBlockEdgeFrequencies(BinaryFunction &Function);

  /// updateCallFrequency - Updated the global call frequency.
  void updateCallFrequency(BinaryContext &BC, MCInst &Inst,
                           StringRef CallAnnotation, double CallFreq,
                           uint64_t TakenFreqEdge);

  /// getCallFrequency - Get updated call frequency.
  void getCallFrequency(BinaryContext &BC, MCInst &Inst,
                        StringRef CallAnnotation, uint64_t &TakenFreqEdge);

  /// clear - Cleans up all the content from the data structures used.
  void clear();

  const char *getName() const override { return "local-frequency-inference"; }

  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif /* LLVM_TOOLS_LLVM_BOLT_PASSES_BLOCKEDGEFREQUENCY_H_ */

