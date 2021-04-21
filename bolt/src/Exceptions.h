//===-- Exceptions.h - Helpers for processing C++ exceptions --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_EXCEPTIONS_H
#define LLVM_TOOLS_LLVM_BOLT_EXCEPTIONS_H

#include "BinaryContext.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/Support/Casting.h"
#include <map>

namespace llvm {
namespace bolt {

class BinaryFunction;
class RewriteInstance;

/// \brief Wraps up information to read all CFI instructions and feed them to a
/// BinaryFunction, as well as rewriting CFI sections.
class CFIReaderWriter {
public:
  explicit CFIReaderWriter(const DWARFDebugFrame &EHFrame);

  bool fillCFIInfoFor(BinaryFunction &Function) const;

  /// Generate .eh_frame_hdr from old and new .eh_frame sections.
  ///
  /// Take FDEs from the \p NewEHFrame unless their initial_pc is listed
  /// in \p FailedAddresses. All other entries are taken from the
  /// \p OldEHFrame.
  ///
  /// \p EHFrameHeaderAddress specifies location of .eh_frame_hdr,
  /// and is required for relative addressing used in the section.
  std::vector<char> generateEHFrameHeader(
      const DWARFDebugFrame &OldEHFrame,
      const DWARFDebugFrame &NewEHFrame,
      uint64_t EHFrameHeaderAddress,
      std::vector<uint64_t> &FailedAddresses) const;

  using FDEsMap = std::map<uint64_t, const dwarf::FDE *>;

  const FDEsMap &getFDEs() const {
    return FDEs;
  }

private:
  FDEsMap FDEs;
};

} // namespace bolt
} // namespace llvm

#endif
