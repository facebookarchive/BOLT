//===--- InstrumentationRuntimeLibrary.h - The Instrument Runtime Library -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_INSTRUMENTATION_RUNTIME_LIBRARY_H
#define LLVM_TOOLS_LLVM_BOLT_INSTRUMENTATION_RUNTIME_LIBRARY_H

#include "Passes/InstrumentationSummary.h"
#include "RuntimeLibs/RuntimeLibrary.h"

namespace llvm {
namespace bolt {

class InstrumentationRuntimeLibrary : public RuntimeLibrary {
public:
  InstrumentationRuntimeLibrary(std::unique_ptr<InstrumentationSummary> Summary)
      : Summary(std::move(Summary)) {}

  void
  addRuntimeLibSections(std::vector<std::string> &SecNames) const override {
    SecNames.push_back(".bolt.instr.counters");
  }

  void adjustCommandLineOptions(const BinaryContext &BC) const override;

  void emitBinary(BinaryContext &BC, MCStreamer &Streamer) override;

  void link(BinaryContext &BC, StringRef ToolPath, orc::ExecutionSession &ES,
            orc::RTDyldObjectLinkingLayer &OLT) override;

private:
  /// Create a non-allocatable ELF section with read-only tables necessary for
  /// writing the instrumented data profile during program finish. The runtime
  /// library needs to open the program executable file and read this data from
  /// disk, this is not loaded by the system.
  void emitTablesAsELFNote(BinaryContext &BC);

  std::unique_ptr<InstrumentationSummary> Summary;
};

} // namespace bolt
} // namespace llvm

#endif
