//===--- Passes/PatchEntries.h - pass for patching function entries -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pass for patching original function entry points.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_PATCH_ENTRIES_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_PATCH_ENTRIES_H

#include "Passes/BinaryPasses.h"
#include "Relocation.h"
#include "llvm/ADT/SmallString.h"

namespace llvm {
namespace bolt {

/// Pass for patching original function entry points.
class PatchEntries : public BinaryFunctionPass {
  // If the function size is below the threshold, attempt to skip patching it.
  static constexpr uint64_t PatchThreshold = 128;

  struct Patch {
    const MCSymbol *Symbol;
    uint64_t Address;
    uint64_t FileOffset;
    BinarySection *Section;
  };

public:
  explicit PatchEntries() : BinaryFunctionPass(false) {
  }

  const char *getName() const override {
    return "patch-entries";
  }
  void runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
