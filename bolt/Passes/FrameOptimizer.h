//===--- Passes/FrameOptimizer.h ------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_FRAMEOPTIMIZER_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_FRAMEOPTIMIZER_H

#include "BinaryPasses.h"
#include "FrameAnalysis.h"

namespace llvm {
namespace bolt {

/// FrameOptimizerPass strives for removing or moving stack frame accesses to
/// less frequently executed basic blocks, reducing the pressure on icache
/// usage as well as dynamic instruction count.
///
/// This is accomplished by analyzing both caller-saved register spills and
/// callee-saved register spills. This class handles the former while delegating
/// the latter to the class ShrinkWrapping. We discuss caller-saved register
/// spills optimization below.
///
/// Caller-saved registers must be conservatively pushed to the stack because
/// the callee may write to these registers. If we can prove the callee will
/// never touch these registers, we can remove this spill.
///
/// This optimization analyzes the call graph and first computes the set of
/// registers that may get overwritten when executing a function (this includes
/// the set of registers touched by all functions this function may call during
/// its execution) -- see the FrameAnalysis class for implementation details.
///
/// The second step is to perform an analysis to disambiguate which stack
/// position is being accessed by each load/store instruction -- see the
/// FrameAnalysis class.
///
/// The third step performs a forward dataflow analysis, using intersection as
/// the confluence operator, to propagate information about available
/// stack definitions at each point of the program. See the
/// StackAvailableExpressions class. This definition shows an equivalence
/// between the value in a stack position and the value of a register or
/// immediate. To have those preserved, both register and the value in the stack
/// position cannot be touched by another instruction.
/// These definitions we are tracking occur in the form:
///
///     stack def:  MEM[FRAME - 0x5c]  <= RAX
///
/// Any instruction that writes to RAX will kill this definition, meaning RAX
/// cannot be used to recover the same value that is in FRAME - 0x5c. Any memory
/// write instruction to FRAME - 0x5c will also kill this definition.
///
/// If such a definition is available at an instruction that loads from this
/// frame offset, we have detected a redundant load. For example, if the
/// previous stack definition is available at the following instruction, this
/// is an example of a redundant stack load:
///
///     stack load:  RAX  <= MEM[FRAME - 0x5c]
///
/// The fourth step will use this info to actually modify redundant loads. In
/// our running example, we would change the stack load to the following reg
/// move:
///
///     RAX <= RAX  // can be deleted
///
/// In this example, since the store source register is the same as the load
/// destination register, this creates a redundant MOV that can be deleted.
///
/// Finally, another analysis propagates information about which instructions
/// are using (loading from) a stack position -- see StackReachingUses. If a
/// store sees no use of the value it is storing, it is eliminated.
///
class FrameOptimizerPass : public BinaryFunctionPass {
  /// Stats aggregating variables
  uint64_t NumRedundantLoads{0};
  uint64_t NumRedundantStores{0};
  uint64_t NumLoadsChangedToReg{0};
  uint64_t NumLoadsChangedToImm{0};
  uint64_t NumLoadsDeleted{0};

  /// Perform a dataflow analysis in \p BF to reveal unnecessary reloads from
  /// the frame. Use the analysis to convert memory loads to register moves or
  /// immediate loads. Delete redundant register moves.
  void removeUnnecessaryLoads(const FrameAnalysis &FA,
                              const BinaryContext &BC,
                              BinaryFunction &BF);

  /// Use information from stack frame usage to delete unused stores.
  void removeUnusedStores(const FrameAnalysis &FA,
                          const BinaryContext &BC,
                          BinaryFunction &BF);

public:
  explicit FrameOptimizerPass(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override {
    return "frame-optimizer";
  }

  /// Pass entry point
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

} // namespace bolt

} // namespace llvm


#endif
