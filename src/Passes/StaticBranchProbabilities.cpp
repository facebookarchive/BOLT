//===- Passes/StaticBranchProbabilities.cpp - Infered Branch Probabilities -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "Passes/BranchHeuristicsInfo.h"

#undef DEBUG_TYPE
#define DEBUG_TYPE "bolt-branch-heuristics"

namespace llvm {
namespace bolt {

double
StaticBranchProbabilities::getCFGBackEdgeProbability(BinaryBasicBlock &SrcBB,
                                                     BinaryBasicBlock &DstBB) {
  Edge CFGEdge = std::make_pair(SrcBB.getLabel(), DstBB.getLabel());
  auto It = CFGBackEdgeProbabilities.find(CFGEdge);
  if (It != CFGBackEdgeProbabilities.end()) {
    if (static_cast<int64_t>(It->second) < 0 ||
        static_cast<int64_t>(It->second) == INT64_MAX)
      return 0.0;
    return It->second;
  }

  auto Function = SrcBB.getFunction();
  return getCFGEdgeProbability(CFGEdge, Function);
}

void StaticBranchProbabilities::setCFGBackEdgeProbability(Edge &CFGEdge,
                                                          double Prob) {
  if (static_cast<int64_t>(Prob) < 0 || static_cast<int64_t>(Prob) == INT64_MAX)
    CFGBackEdgeProbabilities[CFGEdge] = 0.0;
  CFGBackEdgeProbabilities[CFGEdge] = Prob;
}

double
StaticBranchProbabilities::getCFGEdgeProbability(Edge &CFGEdge,
                                                 BinaryFunction *Function) {
  auto It = CFGEdgeProbabilities.find(CFGEdge);
  if (It != CFGEdgeProbabilities.end()) {
    if (static_cast<int64_t>(It->second) < 0 ||
        static_cast<int64_t>(It->second) == INT64_MAX)
      return 0;
    return It->second;
  }

  auto *BB = Function->getBasicBlockForLabel(CFGEdge.first);
  auto &BC = Function->getBinaryContext();
  auto LastInst = BB->getLastNonPseudoInstr();

  if (LastInst && BC.MIB->isConditionalBranch(*LastInst))
    return 0.5;

  return 1.0;
}

double
StaticBranchProbabilities::getCFGEdgeProbability(BinaryBasicBlock &SrcBB,
                                                 BinaryBasicBlock &DstBB) {
  Edge CFGEdge = std::make_pair(SrcBB.getLabel(), DstBB.getLabel());

  auto Function = SrcBB.getFunction();

  return getCFGEdgeProbability(CFGEdge, Function);
}

void StaticBranchProbabilities::clear() {
  BSI->clear();
  CFGBackEdgeProbabilities.clear();
  CFGEdgeProbabilities.clear();
}

void StaticBranchProbabilities::parseProbabilitiesFile(
    std::unique_ptr<MemoryBuffer> MemBuf, BinaryContext &BC) {

  std::vector<BasicBlockOffset> BasicBlockOffsets;
  auto populateBasicBlockOffsets =
      [&](BinaryFunction &Function,
          std::vector<BasicBlockOffset> &BasicBlockOffsets) {
        for (auto &BB : Function) {
          BasicBlockOffsets.emplace_back(
              std::make_pair(BB.getInputOffset(), &BB));
        }
      };

  auto getBasicBlockAtOffset = [&](uint64_t Offset) -> BinaryBasicBlock * {
    if (BasicBlockOffsets.empty())
      return nullptr;

    auto It = std::upper_bound(
        BasicBlockOffsets.begin(), BasicBlockOffsets.end(),
        BasicBlockOffset(Offset, nullptr), CompareBasicBlockOffsets());
    assert(It != BasicBlockOffsets.begin() &&
           "first basic block not at offset 0");
    --It;
    auto *BB = It->second;
    return (Offset == BB->getInputOffset()) ? BB : nullptr;
  };

  auto ParsingBuf = MemBuf.get()->getBuffer();
  BinaryFunction *Function = nullptr;
  while (ParsingBuf.size() > 0) {
    auto LineEnd = ParsingBuf.find_first_of("\n");
    if (LineEnd == StringRef::npos) {
      errs() << "BOLT-ERROR: File not in the correct format.\n";
      exit(1);
    }

    StringRef Line = ParsingBuf.substr(0, LineEnd);
    auto Type = Line.split(" ");
    if (!Type.first.equals("EDGE") && !Type.first.equals("FUNCTION") &&
        !Line.equals("END")) {
      errs() << "BOLT-ERROR: File not in the correct format, found: " << Line
             << "\n";
      exit(1);
    }

    if (Type.first.equals("FUNCTION")) {
      clear();
      BasicBlockOffsets.clear();
      auto FunLine = Type.second.split(" ");
      StringRef NumStr = FunLine.second;
      uint64_t FunctionAddress;
      if (NumStr.getAsInteger(16, FunctionAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(1);
      }

      Function = BC.getBinaryFunctionAtAddress(FunctionAddress);
      if (Function)
        populateBasicBlockOffsets(*Function, BasicBlockOffsets);
      
    }

    if (!Function){
      ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
      continue;
    }
    
    if (Type.first.equals("EDGE")) {
      auto EdgeLine = Type.second.split(" ");

      StringRef SrcBBAddressStr = EdgeLine.first;
      uint64_t SrcBBAddress;
  
      if (SrcBBAddressStr.getAsInteger(16, SrcBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(1);
      }

      auto SrcBB = getBasicBlockAtOffset(SrcBBAddress);

      auto EdgeInfo = EdgeLine.second.split(" ");
      StringRef DstBBAddressStr = EdgeInfo.first;
      uint64_t DstBBAddress;
      
      if (DstBBAddressStr.getAsInteger(16, DstBBAddress)) {
        errs() << "BOLT-ERROR: File not in the correct format.\n";
        exit(1);
      }

      auto DstBB = getBasicBlockAtOffset(DstBBAddress);

      if (SrcBB && DstBB) {
        uint64_t Prob;
        StringRef ProbStr = EdgeInfo.second;

        if (ProbStr.getAsInteger(10, Prob)) {
          errs() << "BOLT-ERROR: File not in the correct format.\n";
          exit(1);
        }

        SrcBB->setSuccessorBranchInfo(*DstBB, Prob, 0);
      }
    } else if (Line.equals("END")) {
      BasicBlockOffsets.clear();
      Function->setExecutionCount(1);
    }
    
    ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
  }
}

void StaticBranchProbabilities::computeProbabilities(BinaryFunction &Function) {
  Function.setExecutionCount(1);

  double EdgeProbTaken = 0.5;
  switch (opts::HeuristicBased) {
  case bolt::StaticBranchProbabilities::H_ALWAYS_TAKEN:
    EdgeProbTaken = 1.0;
    break;
  case bolt::StaticBranchProbabilities::H_NEVER_TAKEN:
    EdgeProbTaken = 0.0;
    break;
  case bolt::StaticBranchProbabilities::H_WEAKLY_TAKEN:
    EdgeProbTaken = 0.2;
    break;
  case bolt::StaticBranchProbabilities::H_WEAKLY_NOT_TAKEN:
    EdgeProbTaken = 0.8;
    break;
  default:
    EdgeProbTaken = 0.5;
    break;
  }

  double EdgeProbNotTaken = 1 - EdgeProbTaken;

  for (auto &BB : Function) {
    BB.setExecutionCount(0);

    if (BB.succ_size() == 0)
      continue;

    if (BB.succ_size() == 1) {
      BinaryBasicBlock *SuccBB = *BB.succ_begin();

      BB.setSuccessorBranchInfo(*SuccBB, 0.0, 0.0);
      Edge CFGEdge = std::make_pair(BB.getLabel(), SuccBB->getLabel());

      // Since it is an unconditional branch, when this branch is reached
      // it has a chance of 100% of being taken (1.0).
      CFGEdgeProbabilities[CFGEdge] = 1.0;
    } else if (opts::MLBased) {
      for (BinaryBasicBlock *SuccBB : BB.successors()) {
        uint64_t Frequency = BB.getBranchInfo(*SuccBB).Count;
        double EdgeProb = (Frequency == UINT64_MAX)
                              ? 0
                              : Frequency / DIVISOR;
        Edge CFGEdge = std::make_pair(BB.getLabel(), SuccBB->getLabel());
        CFGEdgeProbabilities[CFGEdge] = EdgeProb;
        BB.setSuccessorBranchInfo(*SuccBB, 0.0, 0.0);
      }
    } else {
      BinaryBasicBlock *TakenSuccBB = BB.getConditionalSuccessor(true);
      if (TakenSuccBB) {
        Edge CFGEdge = std::make_pair(BB.getLabel(), TakenSuccBB->getLabel());
        CFGEdgeProbabilities[CFGEdge] = EdgeProbTaken;
        BB.setSuccessorBranchInfo(*TakenSuccBB, 0.0, 0.0);
      }

      BinaryBasicBlock *NotTakenSuccBB = BB.getConditionalSuccessor(false);
      if (NotTakenSuccBB) {
        Edge CFGEdge =
            std::make_pair(BB.getLabel(), NotTakenSuccBB->getLabel());
        CFGEdgeProbabilities[CFGEdge] = EdgeProbNotTaken;
        BB.setSuccessorBranchInfo(*NotTakenSuccBB, 0.0, 0.0);
      }
    }
  }
}

void StaticBranchProbabilities::computeHeuristicBasedProbabilities(
    BinaryFunction &Function) {

  Function.setExecutionCount(1);

  BinaryContext &BC = Function.getBinaryContext();
  auto Info = DataflowInfoManager(BC, Function, nullptr, nullptr);
  auto &PDA = Info.getPostDominatorAnalysis();

  for (auto &BB : Function) {
    unsigned NumSucc = BB.succ_size();
    if (NumSucc == 0)
      continue;

    unsigned NumBackedges = BSI->countBackEdges(&BB);

    // If the basic block that conatins the branch has an exit call,
    // then we assume that its successors will never be reached.
    if (BSI->callToExit(&BB, BC)) {
      for (BinaryBasicBlock *SuccBB : BB.successors()) {
        double EdgeProb = 0.0;
        Edge CFGEdge = std::make_pair(BB.getLabel(), SuccBB->getLabel());
        CFGEdgeProbabilities[CFGEdge] = EdgeProb;
        BB.setSuccessorBranchInfo(*SuccBB, 0.0, 0.0);
      }

    } else if (NumBackedges > 0 && NumBackedges < NumSucc) {
      // Both back edges and exit edges
      for (BinaryBasicBlock *SuccBB : BB.successors()) {
        Edge CFGEdge = std::make_pair(BB.getLabel(), SuccBB->getLabel());

        if (BSI->isBackEdge(CFGEdge)) {
          double EdgeProb =
              BHI->getTakenProbability(LOOP_BRANCH_HEURISTIC) / NumBackedges;
          CFGEdgeProbabilities[CFGEdge] = EdgeProb;
        } else {
          double EdgeProb = BHI->getNotTakenProbability(LOOP_BRANCH_HEURISTIC) /
                            (NumSucc - NumBackedges);
          CFGEdgeProbabilities[CFGEdge] = EdgeProb;
        }
        BB.setSuccessorBranchInfo(*SuccBB, 0.0, 0.0);
      }

    } else if (NumBackedges > 0 || NumSucc != 2) {
      // Only back edges, or not a 2-way branch.
      for (BinaryBasicBlock *SuccBB : BB.successors()) {
        Edge CFGEdge = std::make_pair(BB.getLabel(), SuccBB->getLabel());
        CFGEdgeProbabilities[CFGEdge] = 1.0 / NumSucc;
        BB.setSuccessorBranchInfo(*SuccBB, 0.0, 0.0);
      }
    } else {
      assert(NumSucc == 2 && "Expected a two way conditional branch.");

      BinaryBasicBlock *TakenBB = BB.getConditionalSuccessor(true);
      BinaryBasicBlock *FallThroughBB = BB.getConditionalSuccessor(false);

      if (!TakenBB || !FallThroughBB)
        continue;

      Edge TakenEdge = std::make_pair(BB.getLabel(), TakenBB->getLabel());
      Edge FallThroughEdge =
          std::make_pair(BB.getLabel(), FallThroughBB->getLabel());

      // Consider that each edge is unbiased, thus each edge
      // has a likelihood of 50% of being taken.
      CFGEdgeProbabilities[TakenEdge] = 0.5f;
      CFGEdgeProbabilities[FallThroughEdge] = 0.5f;

      for (unsigned BHId = 0; BHId < BHI->getNumHeuristics(); ++BHId) {
        BranchHeuristics Heuristic = BHI->getHeuristic(BHId);
        PredictionInfo Prediction =
            BHI->getApplicableHeuristic(Heuristic, &BB, PDA);
        if (!Prediction.first)
          continue;

        /// If the heuristic applies then combines the probabilities and
        /// updates the edge weights

        BinaryBasicBlock *TakenBB = Prediction.first;
        BinaryBasicBlock *FallThroughBB = Prediction.second;

        Edge TakenEdge = std::make_pair(BB.getLabel(), TakenBB->getLabel());
        Edge FallThroughEdge =
            std::make_pair(BB.getLabel(), FallThroughBB->getLabel());

        double ProbTaken = BHI->getTakenProbability(Heuristic);
        double ProbNotTaken = BHI->getNotTakenProbability(Heuristic);

        double OldProbTaken = getCFGEdgeProbability(BB, *TakenBB);
        double OldProbNotTaken = getCFGEdgeProbability(BB, *FallThroughBB);

        double Divisor =
            OldProbTaken * ProbTaken + OldProbNotTaken * ProbNotTaken;

        CFGEdgeProbabilities[TakenEdge] = OldProbTaken * ProbTaken / Divisor;
        CFGEdgeProbabilities[FallThroughEdge] =
            OldProbNotTaken * ProbNotTaken / Divisor;
      }

      BB.setSuccessorBranchInfo(*TakenBB, 0.0, 0.0);
      BB.setSuccessorBranchInfo(*FallThroughBB, 0.0, 0.0);
    }
  }
}

} // namespace bolt

} // namespace llvm