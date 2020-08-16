//===-- Passes/StaticBranchProbabilities.h - Infered Branch Probabilities -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file describe the interface to the pass that infer branch probabilities
// based on the heuristics and the technique described in Wu and
// Larus paper [1]. Besides it is also a helper for parsing the branch
// probabilities file that was built using a machine learning model.
//
// References:
//
// [1] Youfeng Wu and James R. Larus. 1994. Static branch frequency and
// program profile analysis. In Proceedings of the 27th annual international
// symposium on Microarchitecture (MICRO 27). Association for Computing
// Machinery, New York, NY, USA, 1–11. DOI:https://doi.org/10.1145/192724.192725
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_STATICBRANCHPROBABILITIES_H_
#define LLVM_TOOLS_LLVM_BOLT_PASSES_STATICBRANCHPROBABILITIES_H_

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "DominatorAnalysis.h"
#include "Passes/BinaryPasses.h"
#include "Passes/BranchHeuristicsInfo.h"
#include "Passes/StaticBranchInfo.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace bolt {

class StaticBranchProbabilities {

public:
  /// Choose which heuristic should be used to generate the BB counts
  enum HeuristicType : char {
    /// H_ALWAYS_TAKEN - all the edges that goes to a taken BB will have
    /// weight equals one.
    H_ALWAYS_TAKEN = 0, /// no reordering
    /// H_NEVER_TAKEN - all the edges that goes to a fallthrough BB will have
    /// weight equals one.
    H_NEVER_TAKEN,
    /// H_WEAKLY_TAKEN - all the edges that goes to a taken BB will have
    /// weight equals 0.2 and to a fallthrough equals 0.8.
    H_WEAKLY_TAKEN,
    /// H_WEAKLY_NOT_TAKEN - all the edges that goes to a taken BB will have
    /// weight equals 0.8 and to a fallthrough equals 0.2.
    H_WEAKLY_NOT_TAKEN,
    /// H_UNBIASED - all the edges have an equal likelihood of being taken
    /// (0.5).
    H_UNBIASED,
    /// H_WU_LARUS is an implementation based in Wu Larus' paper about
    /// static branch prediction.
    H_WU_LARUS,
  };

private:
  static constexpr double DIVISOR = 100000000000.0;

  using Edge = StaticBranchInfo::Edge;

  std::unique_ptr<StaticBranchInfo> BSI;
  std::unique_ptr<BranchHeuristicsInfo> BHI;

  using BasicBlockOffset = std::pair<uint64_t, BinaryBasicBlock *>;
  struct CompareBasicBlockOffsets {
    bool operator()(const BasicBlockOffset &A,
                    const BasicBlockOffset &B) const {
      return A.first < B.first;
    }
  };

  /// Holds probabilities propagated to the back edges.
  std::map<Edge, double> CFGBackEdgeProbabilities;

  /// Holds probabilities computed based on the input profile.
  std::map<Edge, double> CFGEdgeProbabilities;

public:
  explicit StaticBranchProbabilities() {
    BSI = std::make_unique<StaticBranchInfo>();
    BHI = std::make_unique<BranchHeuristicsInfo>();
  }

  void setCFGBackEdgeProbability(Edge &CFGEdge, double Prob);

  /// getCFGBackEdgeProbability - Get updated back edge probability, if not
  /// found it uses the edge probability gathered from the input profile
  /// or from branch prediction.
  double getCFGBackEdgeProbability(Edge &CFGEdge);

  double getCFGBackEdgeProbability(BinaryBasicBlock &SrcBB,
                                   BinaryBasicBlock &DstBB);

  /// getCFGEdgeProbability - Get CFG edge probability gathered from the
  /// input profile or from branch prediction.
  ///
  /// If an edge does not have a probability associated it returns 0.5 for
  /// conditional branches and 1.0 for unconditional branches.
  double getCFGEdgeProbability(Edge &CFGEdge, BinaryFunction *Function);

  double getCFGEdgeProbability(Edge &CFGEdge);

  /// getCFGEdgeProbability - Get CFG edge probability gathered from the
  /// input profile or from branch prediction.
  double getCFGEdgeProbability(BinaryBasicBlock &SrcBB,
                               BinaryBasicBlock &DstBB);

  /// inferLocalEdgeProbabilities - Calculates the local flow edge
  /// probabilities of a given function using the Wu Larus algorithm
  /// described on page 5.
  void inferLocalCFGEdgeProbabilities(BinaryFunction &Function);

  /// computeProbabilities - Calculates the probability of a branch being taken
  /// based on trivial predictors that guess that a branch is always taken
  /// (100% taken), never taken (0% taken), weakly taken (20% taken),
  /// weakly not taken (80% taken) or unbiased (50% taken). It also updates the
  /// value of a branch being taken based on predicted probabilities
  /// by a ML model.
  void computeProbabilities(BinaryFunction &Function);

  /// computeHeuristicBasedProbabilities - Combines the outcomes of the
  /// heuristics that applies to the branch using Dempster-Shafer theory of
  /// evidence as described at pages 3-5 of Wu and Larus' paper.
  void computeHeuristicBasedProbabilities(BinaryFunction &Function);

  void updateWeights(double &ProbTaken, double &ProbNotTaken);

  /// clear - Cleans up all the content from the data structures used.
  void clear();

  /// parseProbabilitiesFile - Coordinate reading and parsing of infered
  /// probabilities file.
  ///
  ///
  /// The probabilities file has .pdata extension and contains two-way branch
  /// probabilities that has those values multiplied by a 100000000000. BOLT
  /// will use those values to infer frequencies using the block frequency pass
  /// combined or not with the correction applied by the function call frequency
  /// pass.
  ///
  /// File format syntax:
  /// Function <function_name> <function_start_offset>
  /// EDGE     <source_BB_offset> <target_BB_offset> <probability>
  /// END
  ///
  /// Function - represents the begining of a function
  /// EDGE - represents an edge of a conditional branch in the current
  /// function's
  ///        CFG
  /// END - represents the end of the current function
  ///
  /// <function_name> - name in the binary of a given function.
  ///
  /// <function_start_offset> - start address of a given function.
  ///
  /// <source_BB_offset> - entry hex offset of the source BB.
  ///
  /// <target_BB_offset> - entry hex offset of the target BB.
  ///
  /// <probability> - taken probability info multiplied by 100000000000.
  ///
  ///
  /// Example:
  /// Function Checktree 4006f0
  /// EDGE 2b 67 32210953346
  /// EDGE 34 53 0
  /// EDGE 2b 34 67789046653
  /// EDGE 34 3e 100000000000
  /// EDGE 12 2b 0
  /// EDGE 12 1d 100000000000
  /// EDGE 58 67 65956607495
  /// EDGE 0 12 49300000000
  /// EDGE 58 34 34043392504
  /// EDGE 0 58 50700000000
  /// END
  void parseProbabilitiesFile(std::unique_ptr<MemoryBuffer> MemBuf,
                              BinaryContext &BC);
};

} // namespace bolt
} // namespace llvm

#endif /* LLVM_TOOLS_LLVM_BOLT_PASSES_STATICBRANCHPROBABILITIES_H_ */
