//=  InstrumentationRuntimeLibrary.cpp - The Instrumentation Runtime Library =//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "InstrumentationRuntimeLibrary.h"
#include "BinaryFunction.h"
#include "JumpTable.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltOptCategory;

extern cl::opt<bool> InstrumentationFileAppendPID;
extern cl::opt<std::string> InstrumentationFilename;
extern cl::opt<uint32_t> InstrumentationSleepTime;
extern cl::opt<bool> InstrumentationNoCountersClear;
extern cl::opt<bool> InstrumentationWaitForks;
extern cl::opt<JumpTableSupportLevel> JumpTables;

cl::opt<bool>
    Instrument("instrument",
               cl::desc("instrument code to generate accurate profile data"),
               cl::ZeroOrMore, cl::cat(BoltOptCategory));

cl::opt<std::string> RuntimeInstrumentationLib(
    "runtime-instrumentation-lib",
    cl::desc("specify file name of the runtime instrumentation library"),
    cl::ZeroOrMore, cl::init("libbolt_rt_instr.a"), cl::cat(BoltOptCategory));

} // namespace opts

void InstrumentationRuntimeLibrary::adjustCommandLineOptions(
    const BinaryContext &BC) const {
  if (!BC.HasRelocations) {
    errs() << "BOLT-ERROR: instrumentation runtime libraries require "
              "relocations\n";
    exit(1);
  }
  if (opts::JumpTables != JTS_MOVE) {
    opts::JumpTables = JTS_MOVE;
    outs() << "BOLT-INFO: forcing -jump-tables=move for instrumentation\n";
  }
  if (!BC.StartFunctionAddress) {
    errs() << "BOLT-ERROR: instrumentation runtime libraries require a known "
              "entry point of "
              "the input binary\n";
    exit(1);
  }
  if (!BC.FiniFunctionAddress) {
    errs() << "BOLT-ERROR: input binary lacks DT_FINI entry in the dynamic "
              "section but instrumentation currently relies on patching "
              "DT_FINI to write the profile\n";
    exit(1);
  }
}

void InstrumentationRuntimeLibrary::emitBinary(BinaryContext &BC,
                                               MCStreamer &Streamer) {
  const BinaryFunction *StartFunction =
      BC.getBinaryFunctionAtAddress(*BC.StartFunctionAddress);
  assert(!StartFunction->isFragment() && "expected main function fragment");
  if (!StartFunction) {
    errs() << "BOLT-ERROR: failed to locate function at binary start address\n";
    exit(1);
  }

  const BinaryFunction *FiniFunction =
      BC.FiniFunctionAddress
          ? BC.getBinaryFunctionAtAddress(*BC.FiniFunctionAddress)
          : nullptr;
  if (BC.isELF()) {
    assert(!FiniFunction->isFragment() && "expected main function fragment");
    if (!FiniFunction) {
      errs()
          << "BOLT-ERROR: failed to locate function at binary fini address\n";
      exit(1);
    }
  }

  MCSection *Section = BC.isELF()
                           ? static_cast<MCSection *>(BC.Ctx->getELFSection(
                                 ".bolt.instr.counters", ELF::SHT_PROGBITS,
                                 BinarySection::getFlags(/*IsReadOnly=*/false,
                                                         /*IsText=*/false,
                                                         /*IsAllocatable=*/true)

                                     ))
                           : static_cast<MCSection *>(BC.Ctx->getMachOSection(
                                 "__BOLT", "__counters", MachO::S_REGULAR,
                                 SectionKind::getData()));

  Section->setAlignment(llvm::Align(BC.RegularPageSize));
  Streamer.SwitchSection(Section);

  auto EmitLabel = [&](MCSymbol *Symbol, bool IsGlobal = true) {
    Streamer.emitLabel(Symbol);
    if (IsGlobal)
      Streamer.emitSymbolAttribute(Symbol, MCSymbolAttr::MCSA_Global);
  };

  auto EmitLabelByName = [&](StringRef Name, bool IsGlobal = true) {
    MCSymbol *Symbol = BC.Ctx->getOrCreateSymbol(Name);
    EmitLabel(Symbol, IsGlobal);
  };

  auto EmitValue = [&](MCSymbol *Symbol, const MCExpr *Value) {
    EmitLabel(Symbol);
    Streamer.emitValue(Value, /*Size*/ 8);
  };

  auto EmitIntValue = [&](StringRef Name, uint64_t Value, unsigned Size = 4) {
    EmitLabelByName(Name);
    Streamer.emitIntValue(Value, Size);
  };

  auto EmitString = [&](StringRef Name, StringRef Contents) {
    EmitLabelByName(Name);
    Streamer.emitBytes(Contents);
    Streamer.emitFill(1, 0);
  };

  // All of the following symbols will be exported as globals to be used by the
  // instrumentation runtime library to dump the instrumentation data to disk.
  // Label marking start of the memory region containing instrumentation
  // counters, total vector size is Counters.size() 8-byte counters
  EmitLabelByName("__bolt_instr_locations");
  for (MCSymbol *const &Label : Summary->Counters) {
    EmitLabel(Label, /*IsGlobal*/ false);
    Streamer.emitFill(8, 0);
  }
  const uint64_t Padding =
      alignTo(8 * Summary->Counters.size(), BC.RegularPageSize) -
      8 * Summary->Counters.size();
  if (Padding)
    Streamer.emitFill(Padding, 0);

  EmitIntValue("__bolt_instr_sleep_time", opts::InstrumentationSleepTime);
  EmitIntValue("__bolt_instr_no_counters_clear",
               !!opts::InstrumentationNoCountersClear, 1);
  EmitIntValue("__bolt_instr_wait_forks", !!opts::InstrumentationWaitForks, 1);
  EmitIntValue("__bolt_num_counters", Summary->Counters.size());
  EmitValue(Summary->IndCallHandlerFunc,
            MCSymbolRefExpr::create(
                Summary->InitialIndCallHandlerFunction->getSymbol(), *BC.Ctx));
  EmitValue(
      Summary->IndTailCallHandlerFunc,
      MCSymbolRefExpr::create(
          Summary->InitialIndTailCallHandlerFunction->getSymbol(), *BC.Ctx));
  EmitIntValue("__bolt_instr_num_ind_calls",
               Summary->IndCallDescriptions.size());
  EmitIntValue("__bolt_instr_num_ind_targets",
               Summary->IndCallTargetDescriptions.size());
  EmitIntValue("__bolt_instr_num_funcs", Summary->FunctionDescriptions.size());
  EmitString("__bolt_instr_filename", opts::InstrumentationFilename);
  EmitIntValue("__bolt_instr_use_pid", !!opts::InstrumentationFileAppendPID, 1);
  EmitValue(BC.Ctx->getOrCreateSymbol("__bolt_instr_init_ptr"),
            MCSymbolRefExpr::create(StartFunction->getSymbol(), *BC.Ctx));
  if (FiniFunction) {
    EmitValue(BC.Ctx->getOrCreateSymbol("__bolt_instr_fini_ptr"),
              MCSymbolRefExpr::create(FiniFunction->getSymbol(), *BC.Ctx));
  }

  if (BC.isMachO()) {
    MCSection *TablesSection = BC.Ctx->getMachOSection(
                                 "__BOLT", "__tables", MachO::S_REGULAR,
                                 SectionKind::getData());
    TablesSection->setAlignment(llvm::Align(BC.RegularPageSize));
    Streamer.SwitchSection(TablesSection);
    EmitString("__bolt_instr_tables", buildTables(BC));
  }
}

void InstrumentationRuntimeLibrary::link(
    BinaryContext &BC, StringRef ToolPath, RuntimeDyld &RTDyld,
    std::function<void(RuntimeDyld &)> OnLoad) {
  std::string LibPath = getLibPath(ToolPath, opts::RuntimeInstrumentationLib);
  loadLibrary(LibPath, RTDyld);
  OnLoad(RTDyld);
  RTDyld.finalizeWithMemoryManagerLocking();
  if (RTDyld.hasError()) {
    outs() << "BOLT-ERROR: RTDyld failed: " << RTDyld.getErrorString() << "\n";
    exit(1);
  }

  if (BC.isMachO())
    return;

  RuntimeFiniAddress = RTDyld.getSymbol("__bolt_instr_fini").getAddress();
  if (!RuntimeFiniAddress) {
    errs() << "BOLT-ERROR: instrumentation library does not define "
              "__bolt_instr_fini: "
           << LibPath << "\n";
    exit(1);
  }
  RuntimeStartAddress = RTDyld.getSymbol("__bolt_instr_start").getAddress();
  if (!RuntimeStartAddress) {
    errs() << "BOLT-ERROR: instrumentation library does not define "
              "__bolt_instr_start: "
           << LibPath << "\n";
    exit(1);
  }
  outs() << "BOLT-INFO: output linked against instrumentation runtime "
            "library, lib entry point is 0x"
         << Twine::utohexstr(RuntimeFiniAddress) << "\n";
  outs() << "BOLT-INFO: clear procedure is 0x"
         << Twine::utohexstr(
                RTDyld.getSymbol("__bolt_instr_clear_counters").getAddress())
         << "\n";

  emitTablesAsELFNote(BC);
}

std::string InstrumentationRuntimeLibrary::buildTables(BinaryContext &BC) {
  std::string TablesStr;
  raw_string_ostream OS(TablesStr);

  // This is sync'ed with runtime/instr.cpp:readDescriptions()
  auto getOutputAddress = [](const BinaryFunction &Func,
                             uint64_t Offset) -> uint64_t {
    return Offset == 0
               ? Func.getOutputAddress()
               : Func.translateInputToOutputAddress(Func.getAddress() + Offset);
  };

  // Indirect targets need to be sorted for fast lookup during runtime
  std::sort(Summary->IndCallTargetDescriptions.begin(),
            Summary->IndCallTargetDescriptions.end(),
            [&](const IndCallTargetDescription &A,
                const IndCallTargetDescription &B) {
              return getOutputAddress(*A.Target, A.ToLoc.Offset) <
                     getOutputAddress(*B.Target, B.ToLoc.Offset);
            });

  // Start of the vector with descriptions (one CounterDescription for each
  // counter), vector size is Counters.size() CounterDescription-sized elmts
  const size_t IDSize =
      Summary->IndCallDescriptions.size() * sizeof(IndCallDescription);
  OS.write(reinterpret_cast<const char *>(&IDSize), 4);
  for (const IndCallDescription &Desc : Summary->IndCallDescriptions) {
    OS.write(reinterpret_cast<const char *>(&Desc.FromLoc.FuncString), 4);
    OS.write(reinterpret_cast<const char *>(&Desc.FromLoc.Offset), 4);
  }

  const size_t ITDSize = Summary->IndCallTargetDescriptions.size() *
                         sizeof(IndCallTargetDescription);
  OS.write(reinterpret_cast<const char *>(&ITDSize), 4);
  for (const IndCallTargetDescription &Desc :
       Summary->IndCallTargetDescriptions) {
    OS.write(reinterpret_cast<const char *>(&Desc.ToLoc.FuncString), 4);
    OS.write(reinterpret_cast<const char *>(&Desc.ToLoc.Offset), 4);
    uint64_t TargetFuncAddress =
        getOutputAddress(*Desc.Target, Desc.ToLoc.Offset);
    OS.write(reinterpret_cast<const char *>(&TargetFuncAddress), 8);
  }

  uint32_t FuncDescSize = Summary->getFDSize();
  OS.write(reinterpret_cast<const char *>(&FuncDescSize), 4);
  for (const FunctionDescription &Desc : Summary->FunctionDescriptions) {
    const size_t LeafNum = Desc.LeafNodes.size();
    OS.write(reinterpret_cast<const char *>(&LeafNum), 4);
    for (const InstrumentedNode &LeafNode : Desc.LeafNodes) {
      OS.write(reinterpret_cast<const char *>(&LeafNode.Node), 4);
      OS.write(reinterpret_cast<const char *>(&LeafNode.Counter), 4);
    }
    const size_t EdgesNum = Desc.Edges.size();
    OS.write(reinterpret_cast<const char *>(&EdgesNum), 4);
    for (const EdgeDescription &Edge : Desc.Edges) {
      OS.write(reinterpret_cast<const char *>(&Edge.FromLoc.FuncString), 4);
      OS.write(reinterpret_cast<const char *>(&Edge.FromLoc.Offset), 4);
      OS.write(reinterpret_cast<const char *>(&Edge.FromNode), 4);
      OS.write(reinterpret_cast<const char *>(&Edge.ToLoc.FuncString), 4);
      OS.write(reinterpret_cast<const char *>(&Edge.ToLoc.Offset), 4);
      OS.write(reinterpret_cast<const char *>(&Edge.ToNode), 4);
      OS.write(reinterpret_cast<const char *>(&Edge.Counter), 4);
    }
    const size_t CallsNum = Desc.Calls.size();
    OS.write(reinterpret_cast<const char *>(&CallsNum), 4);
    for (const CallDescription &Call : Desc.Calls) {
      OS.write(reinterpret_cast<const char *>(&Call.FromLoc.FuncString), 4);
      OS.write(reinterpret_cast<const char *>(&Call.FromLoc.Offset), 4);
      OS.write(reinterpret_cast<const char *>(&Call.FromNode), 4);
      OS.write(reinterpret_cast<const char *>(&Call.ToLoc.FuncString), 4);
      OS.write(reinterpret_cast<const char *>(&Call.ToLoc.Offset), 4);
      OS.write(reinterpret_cast<const char *>(&Call.Counter), 4);
      uint64_t TargetFuncAddress =
          getOutputAddress(*Call.Target, Call.ToLoc.Offset);
      OS.write(reinterpret_cast<const char *>(&TargetFuncAddress), 8);
    }
    const size_t EntryNum = Desc.EntryNodes.size();
    OS.write(reinterpret_cast<const char *>(&EntryNum), 4);
    for (const EntryNode &EntryNode : Desc.EntryNodes) {
      OS.write(reinterpret_cast<const char *>(&EntryNode.Node), 8);
      uint64_t TargetFuncAddress =
          getOutputAddress(*Desc.Function, EntryNode.Address);
      OS.write(reinterpret_cast<const char *>(&TargetFuncAddress), 8);
    }
  }
  // Our string table lives immediately after descriptions vector
  OS << Summary->StringTable;
  OS.flush();

  return TablesStr;
}

void InstrumentationRuntimeLibrary::emitTablesAsELFNote(BinaryContext &BC) {
  std::string TablesStr = buildTables(BC);
  const std::string BoltInfo = BinarySection::encodeELFNote(
      "BOLT", TablesStr, BinarySection::NT_BOLT_INSTRUMENTATION_TABLES);
  BC.registerOrUpdateNoteSection(".bolt.instr.tables", copyByteArray(BoltInfo),
                                 BoltInfo.size(),
                                 /*Alignment=*/1,
                                 /*IsReadOnly=*/true, ELF::SHT_NOTE);
}
