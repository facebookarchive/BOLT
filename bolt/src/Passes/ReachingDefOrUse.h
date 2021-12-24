//===--- Passes/ReachingDefOrUse.h ----------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_REACHINGDEFORUSE_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_REACHINGDEFORUSE_H

#include "DataflowAnalysis.h"
#include "RegAnalysis.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Timer.h"

namespace opts {
extern llvm::cl::opt<bool> TimeOpts;
}

namespace llvm {
namespace bolt {

/// If \p Def is true, this computes a forward dataflow equation to
/// propagate reaching definitions.
/// If false, this computes a backward dataflow equation propagating
/// uses to their definitions.
template <bool Def = false>
class ReachingDefOrUse
    : public InstrsDataflowAnalysis<ReachingDefOrUse<Def>, !Def> {
  friend class DataflowAnalysis<ReachingDefOrUse<Def>, BitVector, !Def>;

public:
  ReachingDefOrUse(const RegAnalysis &RA, const BinaryContext &BC,
                   BinaryFunction &BF, Optional<MCPhysReg> TrackingReg = None,
                   MCPlusBuilder::AllocatorIdTy AllocId = 0)
      : InstrsDataflowAnalysis<ReachingDefOrUse<Def>, !Def>(BC, BF, AllocId),
        RA(RA), TrackingReg(TrackingReg) {}
  virtual ~ReachingDefOrUse() {}

  bool isReachedBy(MCPhysReg Reg, ExprIterator Candidates) {
    for (auto I = Candidates; I != this->expr_end(); ++I) {
      auto BV = BitVector(this->BC.MRI->getNumRegs(), false);
      if (Def) {
        RA.getInstClobberList(**I, BV);
      } else {
        this->BC.MIB->getTouchedRegs(**I, BV);
      }
      if (BV[Reg])
        return true;
    }
    return false;
  }

  bool doesAReachesB(const MCInst &A, const MCInst &B) {
    return (*this->getStateAt(B))[this->ExprToIdx[&A]];
  }

  void run() {
    InstrsDataflowAnalysis<ReachingDefOrUse<Def>, !Def>::run();
  }

protected:
  /// Reference to the result of reg analysis
  const RegAnalysis &RA;

  /// If set, limit the dataflow to only track instructions affecting this
  /// register. Otherwise the analysis can be too permissive.
  Optional<MCPhysReg> TrackingReg;

  void preflight() {
    // Populate our universe of tracked expressions with all instructions
    // except pseudos
    for (auto &BB : this->Func) {
      for (auto &Inst : BB) {
        this->Expressions.push_back(&Inst);
        this->ExprToIdx[&Inst] = this->NumInstrs++;
      }
    }
  }

  BitVector getStartingStateAtBB(const BinaryBasicBlock &BB) {
    return BitVector(this->NumInstrs, false);
  }

  BitVector getStartingStateAtPoint(const MCInst &Point) {
    return BitVector(this->NumInstrs, false);
  }

  void doConfluence(BitVector &StateOut, const BitVector &StateIn) {
    StateOut |= StateIn;
  }

  /// Define the function computing the kill set -- whether expression Y, a
  /// tracked expression, will be considered to be dead after executing X.
  bool doesXKillsY(const MCInst *X, const MCInst *Y) {
    // getClobberedRegs for X and Y. If they intersect, return true
    auto XClobbers = BitVector(this->BC.MRI->getNumRegs(), false);
    auto YClobbers = BitVector(this->BC.MRI->getNumRegs(), false);
    RA.getInstClobberList(*X, XClobbers);
    // In defs, write after write -> kills first write
    // In uses, write after access (read or write) -> kills access
    if (Def)
      RA.getInstClobberList(*Y, YClobbers);
    else
      this->BC.MIB->getTouchedRegs(*Y, YClobbers);
    // Limit the analysis, if requested
    if (TrackingReg) {
      XClobbers &= this->BC.MIB->getAliases(*TrackingReg);
      YClobbers &= this->BC.MIB->getAliases(*TrackingReg);
    }
    // X kills Y if it clobbers Y completely -- this is a conservative approach.
    // In practice, we may produce use-def links that may not exist.
    XClobbers &= YClobbers;
    return XClobbers == YClobbers;
  }

  BitVector computeNext(const MCInst &Point, const BitVector &Cur) {
    BitVector Next = Cur;
    // Kill
    for (auto I = this->expr_begin(Next), E = this->expr_end(); I != E; ++I) {
      assert(*I != nullptr && "Lost pointers");
      if (doesXKillsY(&Point, *I)) {
        Next.reset(I.getBitVectorIndex());
      }
    }
    // Gen
    if (!this->BC.MIB->isCFI(Point)) {
      if (TrackingReg == None) {
        // Track all instructions
        Next.set(this->ExprToIdx[&Point]);
      }
      else {
        // Track only instructions relevant to TrackingReg
        auto Regs = BitVector(this->BC.MRI->getNumRegs(), false);
        if (Def)
          RA.getInstClobberList(Point, Regs);
        else
          RA.getInstUsedRegsList(Point, Regs, false);
        Regs &= this->BC.MIB->getAliases(*TrackingReg);
        if (Regs.any())
          Next.set(this->ExprToIdx[&Point]);
      }
    }
    return Next;
  }

  StringRef getAnnotationName() const {
    if (Def)
      return StringRef("ReachingDefs");
    return StringRef("ReachingUses");
  }
};

} // end namespace bolt
} // end namespace llvm


#endif
