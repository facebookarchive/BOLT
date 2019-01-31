//===--- Passes/Inliner.h - Inlining infra for BOLT -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The set of optimization/analysis passes that run on BinaryFunctions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_INLINER_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_INLINER_H

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "BinaryPasses.h"

namespace llvm {
namespace bolt {

class Inliner : public BinaryFunctionPass {
private:

  enum InliningType : char {
    INL_NONE = 0, /// Cannot inline
    INL_TAILCALL, /// Can inline at tail call site
    INL_ANY       /// Can inline at any call site
  };

  struct InliningInfo {
    InliningType Type{INL_NONE};
    uint64_t SizeAfterInlining{0};
    uint64_t SizeAfterTailCallInlining{0};

    InliningInfo(InliningType Type = INL_NONE)
      : Type(Type)
    {}
  };
    
  std::unordered_map<const BinaryFunction *, InliningInfo> InliningCandidates;

  /// Count total amount of bytes inlined for all instances of Inliner.
  /// Note that this number could be negative indicating that the inliner
  /// reduced the size.
  int64_t TotalInlinedBytes{0};

  /// Dynamic count of calls eliminated.
  uint64_t NumInlinedDynamicCalls{0};

  /// Number of call sites that were inlined.
  uint64_t NumInlinedCallSites{0};

  /// Size in bytes of a regular call instruction.
  static uint64_t SizeOfCallInst;

  /// Size in bytes of a tail call instruction.
  static uint64_t SizeOfTailCallInst;
  
  /// Set of functions modified by inlining (used for printing).
  std::unordered_set<const BinaryFunction *> Modified;

  /// Return the size in bytes of a regular call instruction.
  uint64_t getSizeOfCallInst(const BinaryContext &BC);

  /// Return the size in bytes of a tail call instruction.
  uint64_t getSizeOfTailCallInst(const BinaryContext &BC);

  void findInliningCandidates(BinaryContext &BC,
                              const std::map<uint64_t, BinaryFunction> &BFs);

  bool inlineCallsInFunction(BinaryFunction &Function);

  /// Inline a function call \p CallInst to function \p Callee.
  ///
  /// Return the location (basic block and instruction iterator) where the code
  /// of the caller function continues after the inlined code.
  std::pair<BinaryBasicBlock *, BinaryBasicBlock::iterator>
  inlineCall(BinaryBasicBlock &CallerBB,
             BinaryBasicBlock::iterator CallInst,
             const BinaryFunction &Callee);

  /// Check if the inliner can handle inlining of \p BF.
  InliningInfo getInliningInfo(const BinaryFunction &BF) const;

public:
  explicit Inliner(const cl::opt<bool> &PrintPass)
    : BinaryFunctionPass(PrintPass) { }

  const char *getName() const override {
    return "inlining";
  }

  bool shouldPrint(const BinaryFunction &BF) const override {
    return BinaryFunctionPass::shouldPrint(BF) && Modified.count(&BF) > 0;
  }

  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

} // namespace bolt
} // namespace llvm

#endif
