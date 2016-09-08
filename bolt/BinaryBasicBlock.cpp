//===--- BinaryBasicBlock.cpp - Interface for assembly-level basic block --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "BinaryBasicBlock.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInst.h"
#include <limits>
#include <string>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

namespace llvm {
namespace bolt {

bool operator<(const BinaryBasicBlock &LHS, const BinaryBasicBlock &RHS) {
  return LHS.Index < RHS.Index;
}

void BinaryBasicBlock::adjustNumPseudos(const MCInst &Inst, int Sign) {
  auto &BC = Function->getBinaryContext();
  if (BC.MII->get(Inst.getOpcode()).isPseudo())
    NumPseudos += Sign;
}

MCInst *BinaryBasicBlock::getFirstNonPseudo() {
  auto &BC = Function->getBinaryContext();
  for (auto &Inst : Instructions) {
    if (!BC.MII->get(Inst.getOpcode()).isPseudo())
      return &Inst;
  }
  return nullptr;
}

MCInst *BinaryBasicBlock::getLastNonPseudo() {
  auto &BC = Function->getBinaryContext();
  for (auto Itr = Instructions.rbegin(); Itr != Instructions.rend(); ++Itr) {
    if (!BC.MII->get(Itr->getOpcode()).isPseudo())
      return &*Itr;
  }
  return nullptr;
}

bool BinaryBasicBlock::validateSuccessorInvariants() {
  const MCSymbol *TBB = nullptr;
  const MCSymbol *FBB = nullptr;
  MCInst *CondBranch = nullptr;
  MCInst *UncondBranch = nullptr;

  assert(getNumPseudos() == getNumPseudos());

  if (analyzeBranch(TBB, FBB, CondBranch, UncondBranch)) {
    switch (Successors.size()) {
    case 0:
      return !CondBranch && !UncondBranch;
    case 1:
      return !CondBranch;
    case 2:
      if (CondBranch) {
        return (TBB == getConditionalSuccessor(true)->getLabel() &&
                ((!UncondBranch && !FBB) ||
                 (UncondBranch && FBB == getConditionalSuccessor(false)->getLabel())));
      }
      return true;
    default:
      return true;
    }
  }
  return true;
}
  
BinaryBasicBlock *BinaryBasicBlock::getSuccessor(const MCSymbol *Label) const {
  if (!Label && succ_size() == 1)
    return *succ_begin();

  for (BinaryBasicBlock *BB : successors()) {
    if (BB->getLabel() == Label)
      return BB;
  }

  return nullptr;
}

BinaryBasicBlock *BinaryBasicBlock::getLandingPad(const MCSymbol *Label) const {
  for (BinaryBasicBlock *BB : landing_pads()) {
    if (BB->getLabel() == Label)
      return BB;
  }

  return nullptr;
}

void BinaryBasicBlock::addSuccessor(BinaryBasicBlock *Succ,
                                    uint64_t Count,
                                    uint64_t MispredictedCount) {
  Successors.push_back(Succ);
  BranchInfo.push_back({Count, MispredictedCount});
  Succ->Predecessors.push_back(this);
}

void BinaryBasicBlock::replaceSuccessor(BinaryBasicBlock *Succ,
                                        BinaryBasicBlock *NewSucc,
                                        uint64_t Count,
                                        uint64_t MispredictedCount) {
  auto I = succ_begin();
  auto BI = BranchInfo.begin();
  for (; I != succ_end(); ++I) {
    assert(BI != BranchInfo.end() && "missing BranchInfo entry");
    if (*I == Succ)
      break;
    ++BI;
  }
  assert(I != succ_end() && "no such successor!");

  *I = NewSucc;
  *BI = BinaryBranchInfo{Count, MispredictedCount};
}

void BinaryBasicBlock::removeSuccessor(BinaryBasicBlock *Succ) {
  Succ->removePredecessor(this);
  auto I = succ_begin();
  auto BI = BranchInfo.begin();
  for (; I != succ_end(); ++I) {
    assert(BI != BranchInfo.end() && "missing BranchInfo entry");
    if (*I == Succ)
      break;
    ++BI;
  }
  assert(I != succ_end() && "no such successor!");

  Successors.erase(I);
  BranchInfo.erase(BI);
}

void BinaryBasicBlock::addPredecessor(BinaryBasicBlock *Pred) {
  Predecessors.push_back(Pred);
}

void BinaryBasicBlock::removePredecessor(BinaryBasicBlock *Pred) {
  auto I = std::find(pred_begin(), pred_end(), Pred);
  assert(I != pred_end() && "Pred is not a predecessor of this block!");
  Predecessors.erase(I);
}

void BinaryBasicBlock::addLandingPad(BinaryBasicBlock *LPBlock) {
  if (std::find(LandingPads.begin(), LandingPads.end(), LPBlock) == LandingPads.end()) {
    LandingPads.push_back(LPBlock);
  }
  LPBlock->Throwers.insert(this);
}

void BinaryBasicBlock::clearLandingPads() {
  for (auto *LPBlock : LandingPads) {
    auto count = LPBlock->Throwers.erase(this);
    assert(count == 1 && "Possible duplicate entry in LandingPads");
  }
  LandingPads.clear();
}

bool BinaryBasicBlock::analyzeBranch(const MCSymbol *&TBB,
                                     const MCSymbol *&FBB,
                                     MCInst *&CondBranch,
                                     MCInst *&UncondBranch) {
  auto &MIA = Function->getBinaryContext().MIA;
  return MIA->analyzeBranch(Instructions, TBB, FBB, CondBranch, UncondBranch);
}

bool BinaryBasicBlock::swapConditionalSuccessors() {
  if (succ_size() != 2)
    return false;

  std::swap(Successors[0], Successors[1]);
  std::swap(BranchInfo[0], BranchInfo[1]);
  return true;
}

void BinaryBasicBlock::addBranchInstruction(const BinaryBasicBlock *Successor) {
  assert(isSuccessor(Successor));
  auto &BC = Function->getBinaryContext();
  MCInst NewInst;
  BC.MIA->createUncondBranch(NewInst, Successor->getLabel(), BC.Ctx.get());
  Instructions.emplace_back(std::move(NewInst));
}

void BinaryBasicBlock::addTailCallInstruction(const MCSymbol *Target) {
  auto &BC = Function->getBinaryContext();
  MCInst NewInst;
  BC.MIA->createTailCall(NewInst, Target, BC.Ctx.get());
  Instructions.emplace_back(std::move(NewInst));
}

uint32_t BinaryBasicBlock::getNumPseudos() const {
#ifndef NDEBUG
  auto &BC = Function->getBinaryContext();
  uint32_t N = 0;
  for (auto &Instr : Instructions) {
    if (BC.MII->get(Instr.getOpcode()).isPseudo())
      ++N;
  }
  if (N != NumPseudos) {
    errs() << "BOLT-ERROR: instructions for basic block " << getName()
           << " in function " << *Function << ": calculated pseudos "
           << N << ", set pseudos " << NumPseudos << ", size " << size()
           << '\n';
    llvm_unreachable("pseudos mismatch");
  }
#endif
  return NumPseudos;
}

ErrorOr<std::pair<double, double>>
BinaryBasicBlock::getBranchStats(const BinaryBasicBlock *Succ) const {
  if (Function->hasValidProfile()) {
    uint64_t TotalCount = 0;
    uint64_t TotalMispreds = 0;
    for (const auto &BI : BranchInfo) {
      if (BI.Count != COUNT_NO_PROFILE) {
        TotalCount += BI.Count;
        TotalMispreds += BI.MispredictedCount;
      }
    }

    if (TotalCount > 0) {
      auto Itr = std::find(Successors.begin(), Successors.end(), Succ);
      assert(Itr != Successors.end());
      const auto &BI = BranchInfo[Itr - Successors.begin()];
      if (BI.Count && BI.Count != COUNT_NO_PROFILE) {
        if (TotalMispreds == 0) TotalMispreds = 1;
        return std::make_pair(double(BI.Count) / TotalCount,
                              double(BI.MispredictedCount) / TotalMispreds);
      }
    }
  }
  return make_error_code(llvm::errc::result_out_of_range);
}

void BinaryBasicBlock::dump() const {
  auto &BC = Function->getBinaryContext();
  if (Label) outs() << Label->getName() << ":\n";
  BC.printInstructions(outs(), Instructions.begin(), Instructions.end(), Offset);
  outs() << "preds:";
  for (auto itr = pred_begin(); itr != pred_end(); ++itr) {
    outs() << " " << (*itr)->getName();
  }
  outs() << "\nsuccs:";
  for (auto itr = succ_begin(); itr != succ_end(); ++itr) {
    outs() << " " << (*itr)->getName();
  }
  outs() << "\n";
}

} // namespace bolt
} // namespace llvm
