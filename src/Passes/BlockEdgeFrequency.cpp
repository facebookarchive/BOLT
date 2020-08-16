//===---- Passes/BlockEdgeFrequency.cpp - Block and Edge Frequencies -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "Passes/BlockEdgeFrequency.h"
#include "Passes/DataflowInfoManager.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Error.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Options.h"
#include <string>
#include <cfloat>

#undef DEBUG_TYPE
#define DEBUG_TYPE "bolt-block-edge-counts"

using namespace llvm;
using namespace object;
using namespace bolt;

namespace opts {

extern cl::OptionCategory InferenceCategory;

extern cl::opt<bool> MLBased;

extern cl::opt<bolt::StaticBranchProbabilities::HeuristicType> HeuristicBased;

cl::opt<std::string> ProbFilename("prob-file", cl::desc("<data file>"),
                                  cl::Optional, cl::cat(InferenceCategory));
} // namespace opts

namespace llvm {

namespace bolt {

bool BlockEdgeFrequency::isVisited(const BinaryBasicBlock *BB) const {
  return !(ReachableBBs.count(BB));
}

void BlockEdgeFrequency::setVisited(const BinaryBasicBlock *BB) {
  ReachableBBs.erase(BB);
}

uint64_t BlockEdgeFrequency::getCFGEdgeFrequency(BinaryBasicBlock &SrcBB,
                                                 BinaryBasicBlock &DstBB) {

  int64_t Frequency = static_cast<int64_t>(SrcBB.getBranchInfo(DstBB).Count);

  if ((Frequency == INT64_MAX || Frequency < 0)) {
    SrcBB.setSuccessorBranchInfo(DstBB, 0, 0);
    return 0;
  }

  return Frequency;
}

uint64_t BlockEdgeFrequency::getBBExecutionCount(BinaryBasicBlock &BB) {
  int64_t Count = static_cast<int64_t>(BB.getKnownExecutionCount());

  if (Count == INT64_MAX || Count < 0) {
    BB.setExecutionCount(0);
    return 0;
  }

  return Count;
}

void BlockEdgeFrequency::updateCallFrequency(BinaryContext &BC, MCInst &Inst,
                                             StringRef CallAnnotation,
                                             double CallFreq,
                                             uint64_t TakenFreqEdge) {

  if (!BC.MIB->hasAnnotation(Inst, CallAnnotation)) {
    BC.MIB->addAnnotation<uint64_t>(Inst, CallAnnotation, TakenFreqEdge);
  } else {
    if (auto CountAnnt =
            BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, CallAnnotation)) {
      double Count = ((*CountAnnt) / SCALING_FACTOR) * CallFreq;
      (*CountAnnt) = round(Count);
    }
  }
}

void BlockEdgeFrequency::getCallFrequency(BinaryContext &BC, MCInst &Inst,
                                          StringRef CallAnnotation,
                                          uint64_t &TakenFreqEdge) {

  if (!BC.MIB->hasAnnotation(Inst, CallAnnotation)) {
    BC.MIB->addAnnotation<uint64_t>(Inst, CallAnnotation, TakenFreqEdge);
  } else {
    if (auto CountAnnt =
            BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, CallAnnotation)) {
      TakenFreqEdge = (*CountAnnt);
    }
  }
}

void BlockEdgeFrequency::tagReachableBlocks(BinaryBasicBlock *Head) {
  if (!Head)
    return;

  ReachableBBs.clear();

  SmallVector<BinaryBasicBlock *, 16> BBStack;
  BBStack.push_back(Head);

  // Traversing all children in depth-first fashion and
  // marking them as not visited.
  while (!BBStack.empty()) {
    BinaryBasicBlock *BB = BBStack.pop_back_val();
    if (!(ReachableBBs.insert(BB).second)) {
      continue;
    }
    // Adding the new successors into the call stack.
    for (BinaryBasicBlock *SuccBB : BB->successors()) {
      BBStack.push_back(SuccBB);
    }
  }
}

void BlockEdgeFrequency::propagateLoopFrequencies(BinaryLoop *Loop) {
  // Check if the loop has been visited.
  if (VisitedLoops.count(Loop))
    return;

  // Mark the current loop as visited.
  VisitedLoops.insert(Loop);

  // Process the inner-most loop first and use the cyclic probabilities
  // of the inner loops to compute frequencies for the outer loops.
  for (BinaryLoop *InnerLoop : *Loop)
    propagateLoopFrequencies(InnerLoop);

  // Get the loop header
  BinaryBasicBlock *LoopHeader = Loop->getHeader();

  // Mark all blocks reachable from the loop head as not visited.
  tagReachableBlocks(LoopHeader);

  // Propagate frequencies from the loop head.
  propagateFrequencies(LoopHeader, LoopHeader);
}

void BlockEdgeFrequency::propagateFrequencies(BinaryBasicBlock *BB,
                                              BinaryBasicBlock *Head) {

  DEBUG(dbgs() << "===============================>>>> "
               << Head->getFunction()->getPrintName() << "\n");
  DEBUG(dbgs() << "===== Current Basic block " << BB->getName() << " -  "
               << " Head " << Head->getName() << " =======\n");

  // Checks if the basic block BB has been visited.
  if (isVisited(BB))
    return;

  /// 1. Finds the block frequency of BB

  // If the basic block BB is a loop head then assumes it executes only once.
  BB->setExecutionCount(SCALING_FACTOR);

  // If BB is not the loop head, computes the basic block frequency by
  // adding all the in edges (the edges that go to this basic block).
  // If there is a back edge adds all the in edges and offsets it by the
  // cyclic probability value.
  if (BB != Head) {

    // Checks if each BB's predecessor is reachable from the head (if it is
    // marked as not visited) and if the edge departing from each BB's
    // predecessor was previously processed.
    for (BinaryBasicBlock *PredBB : BB->predecessors()) {
      Edge CFGEdge = std::make_pair(PredBB->getLabel(), BB->getLabel());
      if (!isVisited(PredBB) && !BSI->isBackEdge(CFGEdge)) {
        // There is an unprocessed predecessor edge.
        return;
      }
    }

    // Holds the sum of the incoming edges frequencies for this block.
    BB->setExecutionCount(0);

    // Holds the cyclic probability of BB. The cyclic probability of BB
    // is the probability along all the paths that control goes from BB to BB.
    double CyclicProbability = 0.0;

    // Updates the block frequency of BB or computes the cyclic probability of
    // BB if the edge that goes from the predecessor to BB is a back edge.
    for (BinaryBasicBlock *PredBB : BB->predecessors()) {
      Edge CFGEdge = std::make_pair(PredBB->getLabel(), BB->getLabel());
      if (BSI->isBackEdge(CFGEdge) && BSI->isLoopHeader(BB)) {
        CyclicProbability += SBP->getCFGBackEdgeProbability(*PredBB, *BB);
        CyclicProbability =
            (CyclicProbability == DBL_MAX || CyclicProbability < 0.0)
                ? 0.0
                : CyclicProbability;
      } else {
        int64_t BBExecCount =
            getBBExecutionCount(*BB) + getCFGEdgeFrequency(*PredBB, *BB);

        BBExecCount =
            (BBExecCount == INT64_MAX || BBExecCount < 0) ? 0 : BBExecCount;

        BB->setExecutionCount(BBExecCount);
      }
    }

    DEBUG(dbgs() << "CURRENT BLOCK FREQUENCY:\n BlockFrequencies[ "
                 << BB->getName() << " ] = " << getBBExecutionCount(*BB)
                 << "\n CyclicProbability " << CyclicProbability << "\n");

    // For a loop that terminates, the cyclic probability is less than one.
    // If a loop seems not to terminate the cyclic probability is higher than
    // one. Since the algorithm does not work as supposed to if the probability
    // is higher than one, we need to set it to the maximum value offset by
    // the  constant EPSILON.
    double UpperBound = SCALING_FACTOR - (EPSILON);
    if (CyclicProbability > UpperBound) {
      CyclicProbability = UpperBound;
    }

    int64_t BBExecCount = static_cast<int64_t>(
        round((getBBExecutionCount(*BB) / (SCALING_FACTOR - CyclicProbability)) *
              SCALING_FACTOR));

    BBExecCount =
        (BBExecCount == INT64_MAX || BBExecCount < 0) ? 0 : BBExecCount;

    BB->setExecutionCount(BBExecCount);

    DEBUG(dbgs() << "UPDATED BLOCK FREQUENCY\n BlockFrequencies[ "
                 << BB->getName() << " ] = " << BB->getKnownExecutionCount()
                 << "\n CyclicProbability " << CyclicProbability << "\n");
  }

  // Mark the basic block BB as visited.
  setVisited(BB);

  /// 2. Calculate the frequencies of BB's out edges
  for (BinaryBasicBlock *SuccBB : BB->successors()) {
    Edge CFGEdge = std::make_pair(BB->getLabel(), SuccBB->getLabel());
    double EdgeProb = SBP->getCFGEdgeProbability(*BB, *SuccBB);
    double EdgeFreq = EdgeProb * getBBExecutionCount(*BB);
    EdgeFreq = (EdgeFreq == DBL_MAX || EdgeFreq < 0.0) ? 0.0 : EdgeFreq;

    DEBUG(dbgs() << "CURRENT EDGE FREQ INFO:\n " << BB->getName() << " -> "
                 << SuccBB->getName() << " : "
                 << getCFGEdgeFrequency(*BB, *SuccBB) << "\n");

    BB->setSuccessorBranchInfo(*SuccBB, static_cast<int64_t>(round(EdgeFreq)),
                               0);

    DEBUG(dbgs() << "UPDATED EDGE FREQ INFO:\n " << BB->getName() << " -> "
                 << SuccBB->getName() << " : "
                 << getCFGEdgeFrequency(*BB, *SuccBB) << "\n");

    // Update back edge probability, in case the current successor is equal
    // to the head so it can be used by outer loops to calculate cyclic
    // probabilities of inner loops.
    if (SuccBB == Head) {
      DEBUG(dbgs() << "CURRENT BACK EDGE PROB INFO:\n " << BB->getName()
                   << " -> " << SuccBB->getName() << " : "
                   << SBP->getCFGBackEdgeProbability(*BB, *SuccBB) << "\n");

      SBP->setCFGBackEdgeProbability(CFGEdge, EdgeFreq);

      DEBUG(dbgs() << "UPDATED BACK EDGE PROB INFO:\n " << BB->getName()
                   << " -> " << SuccBB->getName() << " : "
                   << SBP->getCFGBackEdgeProbability(*BB, *SuccBB) << "\n");
    }
  }

  /// 3. Propagates the calculated frequencies to the successors of BB
  /// that are not back edges.
  for (BinaryBasicBlock *SuccBB : BB->successors()) {
    Edge CFGEdge = std::make_pair(BB->getLabel(), SuccBB->getLabel());
    if (!BSI->isBackEdge(CFGEdge)) {
      propagateFrequencies(SuccBB, Head);
    }
  }
}

bool BlockEdgeFrequency::checkPrecision(BinaryFunction &Function) const {

  // If the function has only one basic block the frequency
  // matches by definition.
  if (Function.size() == 1) {
    return true;
  }

  // Holds the sum of all edge frequencies that lead
  // to a terminator basic block.
  uint64_t OutFreq = 0;

  for (auto &BB : Function) {
    // A basic block that does not have a successor
    // is a terminator basic block.
    if (BB.succ_size() == 0) {
      for (BinaryBasicBlock *PredBB : BB.predecessors()) {
        int64_t BBCount = static_cast<int64_t>(PredBB->getBranchInfo(BB).Count);
        BBCount = (BBCount == INT64_MAX || BBCount < 0) ? 0 : BBCount;
        OutFreq += BBCount;
      }
    }
  }

  // Checks if the calculated frequency is within the defined boundary.
  return (
      OutFreq >= static_cast<uint64_t>(round(SCALING_FACTOR - (LOOSEBOUND))) &&
      OutFreq <= static_cast<uint64_t>(round(SCALING_FACTOR + (LOOSEBOUND))));
}

void BlockEdgeFrequency::updateLocalCallFrequencies(BinaryFunction &Function) {
  BinaryContext &BC = Function.getBinaryContext();
  for (auto &BB : Function) {
    uint64_t TakenFreqEdge = getBBExecutionCount(BB);

    for (auto &Inst : BB) {
      if (!BC.MIB->isCall(Inst))
        continue;

      StringRef CallAnnotation = "Count";
      if (BC.MIB->getConditionalTailCall(Inst)) {
        CallAnnotation = "CTCTakenCount";
      }

      if (!BC.MIB->hasAnnotation(Inst, CallAnnotation)) {
        BC.MIB->addAnnotation<uint64_t>(Inst, CallAnnotation, TakenFreqEdge);
      } else {
        if (auto CountAnnt =
                BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, CallAnnotation)) {

          int64_t Count = (*CountAnnt) + TakenFreqEdge;
          (*CountAnnt) += ((Count == INT64_MAX || Count < 0) ? 0 : Count);
        }
      }
    }
  }
}

void BlockEdgeFrequency::dumpProfileData(BinaryFunction &Function,
                                         raw_ostream &Printer) {
  BinaryContext &BC = Function.getBinaryContext();

  std::string FromFunName = Function.getPrintName();
  for (auto &BB : Function) {
    auto LastInst = BB.getLastNonPseudoInstr();
    for (auto &Inst : BB) {
      if (!BC.MIB->isCall(Inst))
        continue;

      auto Offset = BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, "Offset");

      if (!Offset)
        continue;

      uint64_t TakenFreqEdge = 0;
      uint64_t NotTakenFreqEdge = 0;
      auto FromBb = Offset.get();
      std::string ToFunName;
      uint32_t ToBb;

      auto *CalleeSymbol = BC.MIB->getTargetSymbol(Inst);
      if (!CalleeSymbol)
        continue;

      ToFunName = CalleeSymbol->getName();
      ToBb = 0;

      StringRef CallAnnotation = "Count";
      if (BC.MIB->getConditionalTailCall(Inst)) {
        CallAnnotation = "CTCTakenCount";
      }

      if (!BC.MIB->hasAnnotation(Inst, CallAnnotation)) {
        BC.MIB->addAnnotation<uint64_t>(Inst, CallAnnotation, TakenFreqEdge);
      } else {
        if (auto CountAnnt =
                BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, CallAnnotation)) {
          TakenFreqEdge = (*CountAnnt);
        }
      }

      if (TakenFreqEdge > 0)
        Printer << "1 " << FromFunName << " " << Twine::utohexstr(FromBb)
                << " 1 " << ToFunName << " " << Twine::utohexstr(ToBb) << " "
                << NotTakenFreqEdge << " " << TakenFreqEdge << "\n";
    }

    if (!LastInst)
      continue;

    auto Offset = BC.MIB->tryGetAnnotationAs<uint64_t>(*LastInst, "Offset");

    if (!Offset)
      continue;

    uint64_t TakenFreqEdge = 0;
    uint64_t NotTakenFreqEdge = 0;
    auto FromBb = Offset.get();
    std::string ToFunName;
    uint32_t ToBb;

    for (BinaryBasicBlock *SuccBB : BB.successors()) {
      TakenFreqEdge = getCFGEdgeFrequency(BB, *SuccBB);
      BinaryFunction *ToFun = SuccBB->getFunction();
      ToFunName = ToFun->getPrintName();
      ToBb = SuccBB->getInputOffset();

      if (TakenFreqEdge > 0)
        Printer << "1 " << FromFunName << " " << Twine::utohexstr(FromBb)
                << " 1 " << ToFunName << " " << Twine::utohexstr(ToBb) << " "
                << NotTakenFreqEdge << " " << TakenFreqEdge << "\n";
    }
  }
}

double BlockEdgeFrequency::getLocalEdgeFrequency(BinaryBasicBlock *SrcBB,
                                                 BinaryBasicBlock *DstBB) {
  int64_t LocalEdgeFreq =
      static_cast<int64_t>(SrcBB->getBranchInfo(*DstBB).Count);
  LocalEdgeFreq =
      (LocalEdgeFreq < 0 || LocalEdgeFreq == INT64_MAX) ? 0 : LocalEdgeFreq;
  return static_cast<double>(LocalEdgeFreq / SCALING_FACTOR);
}

double BlockEdgeFrequency::getLocalBlockFrequency(BinaryBasicBlock *BB) {
  return static_cast<double>(getBBExecutionCount(*BB) / SCALING_FACTOR);
}

void BlockEdgeFrequency::computeBlockEdgeFrequencies(BinaryFunction &Function) {

  if (!Function.isLoopFree()) {
    // Discover all loops of this function.
    Function.calculateLoopInfo();
    const BinaryLoopInfo &LoopInfo = Function.getLoopInfo();
    // Find all loop headers and loop back edges of this function.
    BSI->findLoopEdgesInfo(LoopInfo);
    for (BinaryLoop *BL : LoopInfo)
      propagateLoopFrequencies(BL);
  }

  /// Propagates the frequencies for all the basic blocks of the function
  /// making the entry block as the head of the function.
  BinaryBasicBlock *EntryBlock = &(*Function.begin());
  tagReachableBlocks(EntryBlock);
  propagateFrequencies(EntryBlock, EntryBlock);
}

void BlockEdgeFrequency::clear() {
  BSI->clear();
  SBP->clear();
  ReachableBBs.clear();
  VisitedLoops.clear();
  BlockFrequencies.clear();
  CFGEdgeFrequencies.clear();
}

bool BlockEdgeFrequency::computeFrequencies(BinaryFunction &Function) {
  computeBlockEdgeFrequencies(Function);

  // Checks if the computed frequencies are inside the precision
  // boundary.
  bool Holds = checkPrecision(Function);
  if (Holds) {
    updateLocalCallFrequencies(Function);
    Function.markProfiled(BinaryFunction::PF_SAMPLE);
    Function.setExecutionCount(SCALING_FACTOR);
  } else {

    Function.setExecutionCount(BinaryFunction::COUNT_NO_PROFILE);

    for (auto &BB : Function) {
      BB.setExecutionCount(0);
    }

    updateLocalCallFrequencies(Function);

    DEBUG(dbgs() << "BOLT-DEBUG: The local block and flow edge frequencies\n"
                 << " for function: " << Function.getPrintName()
                 << "BOLT-DEBUG: were calculated with accuracy bellow the "
                 << "desirable boundary.\n "
                 << "BOLT-DEBUG: Thus its CFG were dumpped in a dot and "
                 << "txt formats\n.";
          Function.dumpGraphForPass("unchecked-block-edge-frequency");
          Function.dumpGraphToTextFile("unchecked-block-edge-frequency"););
  }

  clear();
  return Holds;
}

void BlockEdgeFrequency::runOnFunctions(BinaryContext &BC) {
  outs()
      << "BOLT-INFO: statirng block and flow edge frequency inference pass\n";
  outs() << "BOLT-INFO: computing local static infered frequencies\n";

  if (!opts::MLBased) {
    outs() << "BOLT-INFO: based on probabilities infered by heuristics.\n";
  } else {
    outs() << "BOLT-INFO: based on probabilities infered by a ML model.\n";
    outs() << "BOLT-INFO: processing the file " << opts::ProbFilename << "\n";

    auto MB = MemoryBuffer::getFileOrSTDIN(opts::ProbFilename);

    if (auto EC = MB.getError()) {
      errs() << "BOLT-ERROR: Cannot open " << opts::ProbFilename << ": "
             << EC.message() << "\n";
      return;
    }

    SBP->parseProbabilitiesFile(std::move(MB.get()), BC);
  }

  const char *FileName = "localFrequencies.fdata";

  DEBUG(dbgs() << "BOLT-DEBUG: dumping local static infered frequencies to "
               << FileName << "\n";
        std::error_code EC;
        raw_fd_ostream Printer(FileName, EC, sys::fs::F_None); if (EC) {
          dbgs() << "BOLT-ERROR: " << EC.message() << ", unable to open "
                 << FileName << " for output.\n";
          return;
        });

  uint64_t Unchecked = 0;
  uint64_t Checked = 0;
  auto &BFs = BC.getBinaryFunctions();
  for (auto &BFI : BFs) {

    BinaryFunction &Function = BFI.second;

    if (Function.empty())
      continue;

    for (auto &B : Function) {
      B.setExecutionCount(0);
    }
  }
  for (auto &BFI : BFs) {

    BinaryFunction &Function = BFI.second;
    if (Function.empty())
      continue;

    BinaryBasicBlock &EntryBB = (*Function.begin());
    if (!Function.hasProfile()) {
      if (opts::MLBased && Function.size() != 1) {
        Function.setExecutionCount(BinaryFunction::COUNT_NO_PROFILE);
        continue;
      }

      Function.setExecutionCount(SCALING_FACTOR);
      EntryBB.setExecutionCount(SCALING_FACTOR);
    }

    if (opts::HeuristicBased != bolt::StaticBranchProbabilities::H_WU_LARUS)
      SBP->computeProbabilities(Function);

    Function.setExecutionCount(SCALING_FACTOR);
    bool Holds = computeFrequencies(Function);
    if (Holds) {
      ++Checked;

      DEBUG(std::error_code EC;
            raw_fd_ostream Printer(FileName, EC, sys::fs::F_Append); if (EC) {
              dbgs() << "BOLT-WARNING: " << EC.message() << ", unable to open"
                     << " " << FileName << " for output.\n";
              return;
            } dumpProfileData(Function, Printer););

    } else
      ++Unchecked;

    clear();
  }

  DEBUG(
      dbgs() << "BOLT-DEBUG: Number of unchecked functions: " << Unchecked
             << "\nBOLT-DEBUG: Number of functions that cheked: " << Checked
             << "\nBOLT-DEBUG: Total number of functions that were processed: "
             << (Checked + Unchecked) << "\n";);

  outs() << "BOLT-INFO: the BB counts and local Edge counts where updated"
         << " based on intraprodecural inference.\n";
}

} // namespace bolt
} // namespace llvm

