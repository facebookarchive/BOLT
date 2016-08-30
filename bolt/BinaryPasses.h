//===--- BinaryPasses.h - Binary-level analysis/optimization passes -------===//
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

#ifndef LLVM_TOOLS_LLVM_BOLT_BINARY_PASSES_H
#define LLVM_TOOLS_LLVM_BOLT_BINARY_PASSES_H

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>

namespace llvm {
namespace bolt {

/// An optimization/analysis pass that runs on functions.
class BinaryFunctionPass {
public:
  virtual ~BinaryFunctionPass() = default;
  virtual void runOnFunctions(BinaryContext &BC,
                              std::map<uint64_t, BinaryFunction> &BFs,
                              std::set<uint64_t> &LargeFunctions) = 0;
};

/// Detects functions that simply do a tail call when they are called and
/// optimizes calls to these functions.
class OptimizeBodylessFunctions : public BinaryFunctionPass {
private:
  /// EquivalentCallTarget[F] = G ==> function F is simply a tail call to G,
  /// thus calls to F can be optimized to calls to G.
  std::unordered_map<const MCSymbol *, const BinaryFunction *>
    EquivalentCallTarget;

  void analyze(BinaryFunction &BF,
               BinaryContext &BC,
               std::map<uint64_t, BinaryFunction> &BFs);

  void optimizeCalls(BinaryFunction &BF,
                     BinaryContext &BC);

public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// Inlining of single basic block functions.
/// The pass currently does not handle CFI instructions. This is needed for
/// correctness and we may break exception handling because of this.
class InlineSmallFunctions : public BinaryFunctionPass {
private:
  std::set<const BinaryFunction *> InliningCandidates;

  /// Maximum number of instructions in an inlined function.
  static const unsigned kMaxInstructions = 8;
  /// Maximum code size (in bytes) of inlined function (used by aggressive
  /// inlining).
  static const uint64_t kMaxSize = 60;
  /// Maximum number of functions that will be considered for inlining (in
  /// descending hottness order).
  static const unsigned kMaxFunctions = 30000;

  /// Statistics collected for debugging.
  uint64_t TotalDynamicCalls = 0;
  uint64_t InlinedDynamicCalls = 0;
  uint64_t TotalInlineableCalls = 0;

  static bool mustConsider(const BinaryFunction &BF);

  void findInliningCandidates(BinaryContext &BC,
                              const std::map<uint64_t, BinaryFunction> &BFs);

  /// Inline the call in CallInst to InlinedFunctionBB (the only BB of the
  /// called function).
  void inlineCall(BinaryContext &BC,
                  BinaryBasicBlock &BB,
                  MCInst *CallInst,
                  const BinaryBasicBlock &InlinedFunctionBB);

  bool inlineCallsInFunction(BinaryContext &BC,
                             BinaryFunction &Function);

  /// The following methods do a more aggressive inlining pass, where we
  /// inline calls as well as tail calls and we are not limited to inlining
  /// functions with only one basic block.
  /// FIXME: Currently these are broken since they do not work with the split
  /// function option.
  void findInliningCandidatesAggressive(
      BinaryContext &BC, const std::map<uint64_t, BinaryFunction> &BFs);

  bool inlineCallsInFunctionAggressive(
      BinaryContext &BC, BinaryFunction &Function);

  /// Inline the call in CallInst to InlinedFunction. Inlined function should not
  /// contain any landing pad or thrower edges but can have more than one blocks.
  ///
  /// Return the location (basic block and instruction index) where the code of
  /// the caller function continues after the the inlined code.
  std::pair<BinaryBasicBlock *, unsigned>
  inlineCall(BinaryContext &BC,
             BinaryFunction &CallerFunction,
             BinaryBasicBlock *CallerBB,
             const unsigned CallInstIdex,
             const BinaryFunction &InlinedFunction);

public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// Detect and eliminate unreachable basic blocks. We could have those
/// filled with nops and they are used for alignment.
class EliminateUnreachableBlocks : public BinaryFunctionPass {
  bool& NagUser;
  void runOnFunction(BinaryFunction& Function);
 public:
  explicit EliminateUnreachableBlocks(bool &nagUser) : NagUser(nagUser) { }

  void runOnFunctions(BinaryContext&,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

// Reorder the basic blocks for each function based on hotness.
class ReorderBasicBlocks : public BinaryFunctionPass {
 public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// Sync local branches with CFG.
class FixupBranches : public BinaryFunctionPass {
 public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// Fix the CFI state and exception handling information after all other
/// passes have completed.
class FixupFunctions : public BinaryFunctionPass {
 public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// An optimization to simplify conditional tail calls by removing
/// unnecessary branches.
///
/// Convert the sequence:
///
///     j<cc> L1
///     ...
/// L1: jmp foo # tail call
///
/// into:
///     j<cc> foo
///
/// but only if 'j<cc> foo' turns out to be a forward branch.
///
class SimplifyConditionalTailCalls : public BinaryFunctionPass {
  uint64_t NumTailCallCandidates{0};
  uint64_t NumTailCallsPatched{0};
  uint64_t NumOrigForwardBranches{0};

  bool fixTailCalls(BinaryContext &BC, BinaryFunction &BF);
 public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// Perform simple peephole optimizations.
class Peepholes : public BinaryFunctionPass {
  void shortenInstructions(BinaryContext &BC, BinaryFunction &Function);
 public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// An optimization to simplify loads from read-only sections.The pass converts
/// load instructions with statically computed target address such as:
///
///      mov 0x12f(%rip), %eax
///
/// to their counterparts that use immediate opreands instead of memory loads:
///
///     mov $0x4007dc, %eax
///
/// when the target address points somewhere inside a read-only section.
///
class SimplifyRODataLoads : public BinaryFunctionPass {
  uint64_t NumLoadsSimplified{0};
  uint64_t NumDynamicLoadsSimplified{0};
  uint64_t NumLoadsFound{0};
  uint64_t NumDynamicLoadsFound{0};

  bool simplifyRODataLoads(BinaryContext &BC, BinaryFunction &BF);

public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

/// An optimization that replaces references to identical functions with
/// references to a single one of them.
///
class IdenticalCodeFolding : public BinaryFunctionPass {
  uint64_t NumIdenticalFunctionsFound{0};
  uint64_t NumFunctionsFolded{0};
  uint64_t NumDynamicCallsFolded{0};
  uint64_t BytesSavedEstimate{0};

  /// Map from a binary function to its callers.
  struct CallSite {
    BinaryFunction *Caller;
    unsigned BlockIndex;
    unsigned InstrIndex;

    CallSite(BinaryFunction *Caller, unsigned BlockIndex, unsigned InstrIndex) :
      Caller(Caller), BlockIndex(BlockIndex), InstrIndex(InstrIndex) { }
  };
  using CallerMap = std::map<const BinaryFunction *, std::vector<CallSite>>;
  CallerMap Callers;

  /// Replaces all calls to BFTOFold with calls to BFToReplaceWith and merges
  /// the profile data of BFToFold with those of BFToReplaceWith. All modified
  /// functions are added to the Modified set.
  void foldFunction(BinaryContext &BC,
                    std::map<uint64_t, BinaryFunction> &BFs,
                    BinaryFunction *BFToFold,
                    BinaryFunction *BFToReplaceWith,
                    std::set<BinaryFunction *> &Modified);

  /// Finds callers for each binary function and populates the Callers
  /// map.
  void discoverCallers(BinaryContext &BC,
                       std::map<uint64_t, BinaryFunction> &BFs);

 public:
  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};

} // namespace bolt
} // namespace llvm

#endif
