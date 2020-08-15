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

PredictionInfo BranchHeuristicsInfo::getApplicableHeuristic(
    BranchHeuristics BH, BinaryBasicBlock *BB,
    DominatorAnalysis<true> &PDA) const {
  switch (BH) {
  case LOOP_BRANCH_HEURISTIC:
    return loopBranchHeuristic(BB);
  case POINTER_HEURISTIC:
    return pointerHeuristic(BB);
  case CALL_HEURISTIC:
    return callHeuristic(BB, PDA);
  case OPCODE_HEURISTIC:
    return opcodeHeuristic(BB);
  case LOOP_EXIT_HEURISTIC:
    return loopExitHeuristic(BB);
  case RETURN_HEURISTIC:
    return returnHeuristic(BB);
  case STORE_HEURISTIC:
    return storeHeuristic(BB, PDA);
  case LOOP_HEADER_HEURISTIC:
    return loopHeaderHeuristic(BB, PDA);
  case GUARD_HEURISTIC:
    return guardHeuristic(BB);
  }
}

PredictionInfo
BranchHeuristicsInfo::loopBranchHeuristic(BinaryBasicBlock *BB) const {
  bool Applies = false;
  PredictionInfo Prediction;

  BinaryBasicBlock *TakenSucc = BB->getConditionalSuccessor(true);
  BinaryBasicBlock *FallthroughSucc = BB->getConditionalSuccessor(false);

  // If the taken branch is a back edge to a loop's head or
  // the not taken branch is an exit edge, the heuristic applies to
  // the conditional branch
  if ((BPI->isBackEdge(BB, TakenSucc) && BPI->isLoopHeader(TakenSucc)) ||
      BPI->isExitEdge(BB, FallthroughSucc)) {
    Applies = true;
    Prediction = std::make_pair(TakenSucc, FallthroughSucc);
  }

  // Checks the opposite situation, to verify if the premisse holds.
  if ((BPI->isBackEdge(BB, FallthroughSucc) &&
       BPI->isLoopHeader(FallthroughSucc)) ||
      BPI->isExitEdge(BB, TakenSucc)) {

    // If the heuristic applies to both branches, predict none.
    if (Applies)
      return NONE;

    Applies = true;
    Prediction = std::make_pair(FallthroughSucc, TakenSucc);
  }

  return (Applies ? Prediction : NONE);
}

PredictionInfo
BranchHeuristicsInfo::pointerHeuristic(BinaryBasicBlock *BB) const {
  // TO-DO
  return NONE;
}

PredictionInfo
BranchHeuristicsInfo::callHeuristic(BinaryBasicBlock *BB,
                                    DominatorAnalysis<true> &PDA) const {
  bool Applies = false;
  PredictionInfo Prediction;

  BinaryBasicBlock *TakenSucc = BB->getConditionalSuccessor(true);
  BinaryBasicBlock *FallthroughSucc = BB->getConditionalSuccessor(false);

  if (TakenSucc->size() == 0 || FallthroughSucc->size() == 0)
    return NONE;

  MCInst *LastSuccInst = TakenSucc->getLastNonPseudoInstr();

  if (!LastSuccInst)
    return NONE;

  MCInst &FirstBBInst = BB->front();

  // Checks if the successor contains a call instruction
  // and does not post-dominate.
  if (LastSuccInst && BPI->hasCallInst(TakenSucc) &&
      !PDA.doesADominateB(*LastSuccInst, FirstBBInst)) {
    Applies = true;
    Prediction = std::make_pair(FallthroughSucc, TakenSucc);
  }

  LastSuccInst = FallthroughSucc->getLastNonPseudoInstr();
  if (!LastSuccInst)
    return NONE;

  // Checks the opposite situation, to verify if the premisse holds.
  if (BPI->hasCallInst(FallthroughSucc) &&
      !PDA.doesADominateB(*LastSuccInst, FirstBBInst)) {

    // If the heuristic applies to both branches, predict none.
    if (Applies)
      return NONE;

    Applies = true;
    Prediction = std::make_pair(TakenSucc, FallthroughSucc);
  }

  return (Applies ? Prediction : NONE);
}

PredictionInfo
BranchHeuristicsInfo::opcodeHeuristic(BinaryBasicBlock *BB) const {
  // TO-DO
  return NONE;
}

PredictionInfo
BranchHeuristicsInfo::loopExitHeuristic(BinaryBasicBlock *BB) const {

  BinaryBasicBlock *TakenSucc = BB->getConditionalSuccessor(true);
  BinaryBasicBlock *FallthroughSucc = BB->getConditionalSuccessor(false);

  // Checks if neither of the branches are loop headers.
  if (BPI->isLoopHeader(TakenSucc) || BPI->isLoopHeader(FallthroughSucc))
    return NONE;

  // If the analized edge is an exit edge the taken basic block must be the
  // one that is not in this edge.
  // Reminder: In this case it is impossible for both of the successor to be
  // exit block.
  if (BPI->isExitEdge(BB, TakenSucc))
    return std::make_pair(FallthroughSucc, TakenSucc);
  else if (BPI->isExitEdge(BB, FallthroughSucc))
    return std::make_pair(TakenSucc, FallthroughSucc);

  return NONE;
}

PredictionInfo
BranchHeuristicsInfo::returnHeuristic(BinaryBasicBlock *BB) const {
  BinaryFunction *Function = BB->getFunction();
  BinaryContext &BC = Function->getBinaryContext();
  bool Applies = false;
  PredictionInfo Prediction;

  BinaryBasicBlock *TakenSucc = BB->getConditionalSuccessor(true);
  BinaryBasicBlock *FallthroughSucc = BB->getConditionalSuccessor(false);

  if (TakenSucc->size() == 0 || FallthroughSucc->size() == 0)
    return NONE;

  MCInst *LastSuccInst = TakenSucc->getLastNonPseudoInstr();

  if (!LastSuccInst)
    return NONE;

  // Checks if the taken basic block contains a return instruction.
  if (BC.MIB->isReturn(*LastSuccInst)) {
    Applies = true;
    Prediction = std::make_pair(FallthroughSucc, TakenSucc);
  }

  LastSuccInst = FallthroughSucc->getLastNonPseudoInstr();
  if (!LastSuccInst)
    return NONE;

  // Checks the opposite situation, to verify if the premisse holds.
  if (BC.MIB->isReturn(*LastSuccInst)) {

    // If the heuristic applies to both branches, predict none.
    if (Applies)
      return NONE;

    Applies = true;
    Prediction = std::make_pair(TakenSucc, FallthroughSucc);
  }

  return (Applies ? Prediction : NONE);
}

PredictionInfo
BranchHeuristicsInfo::storeHeuristic(BinaryBasicBlock *BB,
                                     DominatorAnalysis<true> &PDA) const {
  bool Applies = false;
  PredictionInfo Prediction;

  BinaryBasicBlock *TakenSucc = BB->getConditionalSuccessor(true);
  BinaryBasicBlock *FallthroughSucc = BB->getConditionalSuccessor(false);

  if (TakenSucc->size() == 0 || FallthroughSucc->size() == 0)
    return NONE;

  MCInst *LastSuccInst = TakenSucc->getLastNonPseudoInstr();

  if (!LastSuccInst)
    return NONE;

  MCInst &FirstBBInst = BB->front();

  // Checks if the taken basic block contains a store instruction
  // and does not post-dominate.
  if (BPI->hasStoreInst(TakenSucc) &&
      !PDA.doesADominateB(*LastSuccInst, FirstBBInst)) {
    Applies = true;
    Prediction = std::make_pair(FallthroughSucc, TakenSucc);
  }

  LastSuccInst = FallthroughSucc->getLastNonPseudoInstr();

  if (!LastSuccInst)
    return NONE;

  // Checks the opposite situation, to verify if the premisse holds.
  if (BPI->hasStoreInst(FallthroughSucc) &&
      !PDA.doesADominateB(*LastSuccInst, FirstBBInst)) {

    // If the heuristic applies to both branches, predict none.
    if (Applies)
      return NONE;

    Applies = true;
    Prediction = std::make_pair(TakenSucc, FallthroughSucc);
  }

  return (Applies ? Prediction : NONE);
}

PredictionInfo
BranchHeuristicsInfo::loopHeaderHeuristic(BinaryBasicBlock *BB,
                                          DominatorAnalysis<true> &PDA) const {
  bool Applies = false;
  PredictionInfo Prediction;

  BinaryBasicBlock *TakenSucc = BB->getConditionalSuccessor(true);
  BinaryBasicBlock *FallthroughSucc = BB->getConditionalSuccessor(false);

  if (TakenSucc->succ_size() == 1 && TakenSucc->pred_size() == 1) {
    if (BinaryBasicBlock *PreHeaderTrueSucc = TakenSucc->getFallthrough()) {
      TakenSucc = PreHeaderTrueSucc;
    }
  }

  MCInst *LastSuccInst = TakenSucc->getLastNonPseudoInstr();

  if (!LastSuccInst)
    return NONE;

  MCInst &FirstBBInst = BB->front();

  // Checks if the taken basic block is a loop header and
  // if does not post dominates
  if (BPI->isLoopHeader(TakenSucc) &&
      !PDA.doesADominateB(*LastSuccInst, FirstBBInst)) {
    Applies = true;
    Prediction = std::make_pair(TakenSucc, FallthroughSucc);
  }

  if (FallthroughSucc->succ_size() == 1 && FallthroughSucc->pred_size() == 1) {
    if (BinaryBasicBlock *preHeaderFallthroughSucc =
            FallthroughSucc->getFallthrough()) {
      FallthroughSucc = preHeaderFallthroughSucc;
    }
  }

  LastSuccInst = FallthroughSucc->getLastNonPseudoInstr();

  // Checks if the not taken basic block is a loop header and
  // if does not post dominates
  if (BPI->isLoopHeader(FallthroughSucc) &&
      !PDA.doesADominateB(*LastSuccInst, FirstBBInst)) {

    // If the heuristic matches both branches, predict none.
    if (Applies)
      return NONE;

    Applies = true;
    Prediction = std::make_pair(FallthroughSucc, TakenSucc);
  }

  return (Applies ? Prediction : NONE);
}

PredictionInfo
BranchHeuristicsInfo::guardHeuristic(BinaryBasicBlock *BB) const {
  // TO-DO
  return NONE;
}

} // namespace bolt
} // namespace llvm