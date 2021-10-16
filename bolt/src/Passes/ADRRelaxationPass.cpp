//===--- ADRRelaxationPass.cpp --------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "ADRRelaxationPass.h"
#include "ParallelUtilities.h"

using namespace llvm;

namespace opts {
extern cl::OptionCategory BoltCategory;

static cl::opt<bool>
    AdrPassOpt("adr-relaxation",
               cl::desc("Replace ARM non-local ADR instructions with ADRP"),
               cl::init(true), cl::cat(BoltCategory), cl::ReallyHidden);
} // namespace opts

namespace llvm {
namespace bolt {

void ADRRelaxationPass::runOnFunction(BinaryContext &BC, BinaryFunction &BF) {
  for (BinaryBasicBlock *BB : BF.layout()) {
    for (auto It = BB->begin(); It != BB->end(); ++It) {
      MCInst &Inst = *It;
      if (!BC.MIB->isADR(Inst))
        continue;

      const MCSymbol *Symbol = BC.MIB->getTargetSymbol(Inst);
      if (!Symbol)
        continue;

      if (BF.hasIslandsInfo()) {
        BinaryFunction::IslandInfo &Islands = BF.getIslandInfo();
        if (Islands.Symbols.count(Symbol) || Islands.ProxySymbols.count(Symbol))
          continue;
      }

      BinaryFunction *TargetBF = BC.getFunctionForSymbol(Symbol);
      if (TargetBF && TargetBF == &BF)
        continue;

      MCPhysReg Reg;
      BC.MIB->getADRReg(Inst, Reg);
      int64_t Addend = BC.MIB->getTargetAddend(Inst);
      std::vector<MCInst> Addr =
          BC.MIB->materializeAddress(Symbol, BC.Ctx.get(), Reg, Addend);
      It = BB->replaceInstruction(It, Addr);
    }
  }
}

void ADRRelaxationPass::runOnFunctions(BinaryContext &BC) {
  if (!opts::AdrPassOpt || !BC.HasRelocations)
    return;

  ParallelUtilities::WorkFuncTy WorkFun = [&](BinaryFunction &BF) {
    runOnFunction(BC, BF);
  };

  ParallelUtilities::runOnEachFunction(
      BC, ParallelUtilities::SchedulingPolicy::SP_TRIVIAL, WorkFun, nullptr,
      "ADRRelaxationPass", /* ForceSequential */ true);
}

} // end namespace bolt
} // end namespace llvm
