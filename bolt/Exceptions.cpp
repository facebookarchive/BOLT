//===-- Exceptions.cpp - Helpers for processing C++ exceptions ------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Some of the code is taken from examples/ExceptionDemo
//
//===----------------------------------------------------------------------===//

#include "Exceptions.h"
#include "BinaryFunction.h"
#include "RewriteInstance.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/DebugInfo/DWARF/DWARFFrame.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt-exceptions"

using namespace llvm::dwarf;

namespace opts {

extern llvm::cl::opt<unsigned> Verbosity;

static llvm::cl::opt<bool>
PrintExceptions("print-exceptions",
                llvm::cl::desc("print exception handling data"),
                llvm::cl::ZeroOrMore,
                llvm::cl::Hidden);

} // namespace opts

namespace llvm {
namespace bolt {

// Read and dump the .gcc_exception_table section entry.
//
// .gcc_except_table section contains a set of Language-Specific Data Areas -
// a fancy name for exception handling tables. There's one  LSDA entry per
// function. However, we can't actually tell which function LSDA refers to
// unless we parse .eh_frame entry that refers to the LSDA.
// Then inside LSDA most addresses are encoded relative to the function start,
// so we need the function context in order to get to real addresses.
//
// The best visual representation of the tables comprising LSDA and
// relationships between them is illustrated at:
//   http://mentorembedded.github.io/cxx-abi/exceptions.pdf
// Keep in mind that GCC implementation deviates slightly from that document.
//
// To summarize, there are 4 tables in LSDA: call site table, actions table,
// types table, and types index table (for indirection). The main table contains
// call site entries. Each call site includes a PC range that can throw an
// exception, a handler (landing pad), and a reference to an entry in the action
// table. The handler and/or action could be 0. The action entry is a head
// of a list of actions associated with a call site. The action table contains
// all such lists (it could be optimized to share list tails). Each action could
// be either to catch an exception of a given type, to perform a cleanup, or to
// propagate the exception after filtering it out (e.g. to make sure function
// exception specification is not violated). Catch action contains a reference
// to an entry in the type table, and filter action refers to an entry in the
// type index table to encode a set of types to filter.
//
// Call site table follows LSDA header. Action table immediately follows the
// call site table.
//
// Both types table and type index table start at the same location, but they
// grow in opposite directions (types go up, indices go down). The beginning of
// these tables is encoded in LSDA header. Sizes for both of the tables are not
// included anywhere.
//
// For the purpose of rewriting exception handling tables, we can reuse action,
// types, and type index tables in their original binary format.
// This is only possible when type references are encoded as absolute addresses.
// We still have to parse all the tables to determine their sizes. Then we have
// to parse the call site table and associate discovered information with
// actual call instructions and landing pad blocks.
//
// Ideally we should be able to re-write LSDA in-place, without the need to
// allocate a new space for it. Sadly there's no guarantee that the new call
// site table will be the same size as GCC uses uleb encodings for PC offsets.
//
// For split function re-writing we would need to split LSDA too.
//
// Note: some functions have LSDA entries with 0 call site entries.
void BinaryFunction::parseLSDA(ArrayRef<uint8_t> LSDASectionData,
                               uint64_t LSDASectionAddress) {
  assert(CurrentState == State::Disassembled && "unexpected function state");

  if (!getLSDAAddress())
    return;

  assert(getLSDAAddress() < LSDASectionAddress + LSDASectionData.size() &&
         "wrong LSDA address");

  const uint8_t *Ptr =
      LSDASectionData.data() + getLSDAAddress() - LSDASectionAddress;

  uint8_t LPStartEncoding = *Ptr++;
  uintptr_t LPStart = 0;
  if (LPStartEncoding != DW_EH_PE_omit) {
    LPStart = readEncodedPointer(Ptr, LPStartEncoding);
  }

  assert(LPStart == 0 && "support for split functions not implemented");

  uint8_t TTypeEncoding = *Ptr++;
  uintptr_t TTypeEnd = 0;
  if (TTypeEncoding != DW_EH_PE_omit) {
    TTypeEnd = readULEB128(Ptr);
  }

  if (opts::PrintExceptions) {
    outs() << "[LSDA at 0x" << Twine::utohexstr(getLSDAAddress())
           << " for function " << *this << "]:\n";
    outs() << "LPStart Encoding = " << (unsigned)LPStartEncoding << '\n';
    outs() << "LPStart = 0x" << Twine::utohexstr(LPStart) << '\n';
    outs() << "TType Encoding = " << (unsigned)TTypeEncoding << '\n';
    outs() << "TType End = " << TTypeEnd << '\n';
  }

  // Table to store list of indices in type table. Entries are uleb128 values.
  auto TypeIndexTableStart = Ptr + TTypeEnd;

  // Offset past the last decoded index.
  intptr_t MaxTypeIndexTableOffset = 0;

  // The actual type info table starts at the same location, but grows in
  // different direction. Encoding is different too (TTypeEncoding).
  auto TypeTableStart = reinterpret_cast<const uint32_t *>(Ptr + TTypeEnd);

  uint8_t       CallSiteEncoding = *Ptr++;
  uint32_t      CallSiteTableLength = readULEB128(Ptr);
  const uint8_t *CallSiteTableStart = Ptr;
  const uint8_t *CallSiteTableEnd = CallSiteTableStart + CallSiteTableLength;
  const uint8_t *CallSitePtr = CallSiteTableStart;
  const uint8_t *ActionTableStart = CallSiteTableEnd;

  if (opts::PrintExceptions) {
    outs() << "CallSite Encoding = " << (unsigned)CallSiteEncoding << '\n';
    outs() << "CallSite table length = " << CallSiteTableLength << '\n';
    outs() << '\n';
  }

  HasEHRanges = CallSitePtr < CallSiteTableEnd;
  uint64_t RangeBase = getAddress();
  while (CallSitePtr < CallSiteTableEnd) {
    uintptr_t Start = readEncodedPointer(CallSitePtr, CallSiteEncoding);
    uintptr_t Length = readEncodedPointer(CallSitePtr, CallSiteEncoding);
    uintptr_t LandingPad = readEncodedPointer(CallSitePtr, CallSiteEncoding);
    uintptr_t ActionEntry = readULEB128(CallSitePtr);

    if (opts::PrintExceptions) {
      outs() << "Call Site: [0x" << Twine::utohexstr(RangeBase + Start)
             << ", 0x" << Twine::utohexstr(RangeBase + Start + Length)
             << "); landing pad: 0x" << Twine::utohexstr(LPStart + LandingPad)
             << "; action entry: 0x" << Twine::utohexstr(ActionEntry) << "\n";
      outs() << "  current offset is " << (CallSitePtr - CallSiteTableStart)
             << '\n';
    }

    // Create a handler entry if necessary.
    MCSymbol *LPSymbol{nullptr};
    if (LandingPad) {
      if (Instructions.find(LandingPad) == Instructions.end()) {
        if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: landing pad " << Twine::utohexstr(LandingPad)
                 << " not pointing to an instruction in function "
                 << *this << " - ignoring.\n";
        }
      } else {
        auto Label = Labels.find(LandingPad);
        if (Label != Labels.end()) {
          LPSymbol = Label->second;
        } else {
          LPSymbol = BC.Ctx->createTempSymbol("LP", true);
          Labels[LandingPad] = LPSymbol;
        }
        LandingPads.insert(LPSymbol);
      }
    }

    // Mark all call instructions in the range.
    auto II = Instructions.find(Start);
    auto IE = Instructions.end();
    assert(II != IE && "exception range not pointing to an instruction");
    do {
      auto &Instruction = II->second;
      if (BC.MIA->isCall(Instruction)) {
        assert(!BC.MIA->isInvoke(Instruction) &&
               "overlapping exception ranges detected");
        // Add extra operands to a call instruction making it an invoke from
        // now on.
        BC.MIA->addEHInfo(Instruction,
                          MCLandingPad(LPSymbol, ActionEntry),
                          BC.Ctx.get());
      }
      ++II;
    } while (II != IE && II->first < Start + Length);

    if (ActionEntry != 0) {
      auto printType = [&] (int Index, raw_ostream &OS) {
        assert(Index > 0 && "only positive indices are valid");
        assert(TTypeEncoding == DW_EH_PE_udata4 &&
               "only udata4 supported for TTypeEncoding");
        auto TypeAddress = *(TypeTableStart - Index);
        if (TypeAddress == 0) {
          OS << "<all>";
          return;
        }
        auto NI = BC.GlobalAddresses.find(TypeAddress);
        if (NI != BC.GlobalAddresses.end()) {
          OS << NI->second;
        } else {
          OS << "0x" << Twine::utohexstr(TypeAddress);
        }
      };
      if (opts::PrintExceptions)
        outs() << "    actions: ";
      const uint8_t *ActionPtr = ActionTableStart + ActionEntry - 1;
      long long ActionType;
      long long ActionNext;
      auto Sep = "";
      do {
        ActionType = readSLEB128(ActionPtr);
        auto Self = ActionPtr;
        ActionNext = readSLEB128(ActionPtr);
        if (opts::PrintExceptions)
          outs() << Sep << "(" << ActionType << ", " << ActionNext << ") ";
        if (ActionType == 0) {
          if (opts::PrintExceptions)
            outs() << "cleanup";
        } else if (ActionType > 0) {
          // It's an index into a type table.
          if (opts::PrintExceptions) {
            outs() << "catch type ";
            printType(ActionType, outs());
          }
        } else { // ActionType < 0
          if (opts::PrintExceptions)
            outs() << "filter exception types ";
          auto TSep = "";
          // ActionType is a negative *byte* offset into *uleb128-encoded* table
          // of indices with base 1.
          // E.g. -1 means offset 0, -2 is offset 1, etc. The indices are
          // encoded using uleb128 thus we cannot directly dereference them.
          auto TypeIndexTablePtr = TypeIndexTableStart - ActionType - 1;
          while (auto Index = readULEB128(TypeIndexTablePtr)) {
            if (opts::PrintExceptions) {
              outs() << TSep;
              printType(Index, outs());
              TSep = ", ";
            }
          }
          MaxTypeIndexTableOffset =
              std::max(MaxTypeIndexTableOffset,
                       TypeIndexTablePtr - TypeIndexTableStart);
        }

        Sep = "; ";

        ActionPtr = Self + ActionNext;
      } while (ActionNext);
      if (opts::PrintExceptions)
        outs() << '\n';
    }
  }
  if (opts::PrintExceptions)
    outs() << '\n';

  assert(TypeIndexTableStart + MaxTypeIndexTableOffset <=
         LSDASectionData.data() + LSDASectionData.size() &&
         "LSDA entry has crossed section boundary");

  if (TTypeEnd) {
    // TypeIndexTableStart is a <uint8_t *> alias for TypeTableStart.
    LSDAActionAndTypeTables =
      ArrayRef<uint8_t>(ActionTableStart,
                        TypeIndexTableStart - ActionTableStart);
    LSDATypeIndexTable =
      ArrayRef<uint8_t>(TypeIndexTableStart, MaxTypeIndexTableOffset);
  }
}

void BinaryFunction::updateEHRanges() {
  if (getSize() == 0)
    return;

  assert(CurrentState == State::CFG_Finalized && "unexpected state");

  // Build call sites table.
  struct EHInfo {
    const MCSymbol *LP; // landing pad
    uint64_t Action;
  };

  // If previous call can throw, this is its exception handler.
  EHInfo PreviousEH = {nullptr, 0};

  // Marker for the beginning of exceptions range.
  const MCSymbol *StartRange = nullptr;

  // Indicates whether the start range is located in a cold part.
  bool IsStartInCold = false;

  // Have we crossed hot/cold border for split functions?
  bool SeenCold = false;

  // Sites to update - either regular or cold.
  auto *Sites = &CallSites;

  for (auto &BB : BasicBlocksLayout) {

    if (BB->isCold() && !SeenCold) {
      SeenCold = true;

      // Close the range (if any) and change the target call sites.
      if (StartRange) {
        Sites->emplace_back(CallSite{StartRange, getFunctionEndLabel(),
                                     PreviousEH.LP, PreviousEH.Action});
      }
      Sites = &ColdCallSites;

      // Reset the range.
      StartRange = nullptr;
      PreviousEH = {nullptr, 0};
    }

    for (auto II = BB->begin(); II != BB->end(); ++II) {
      auto Instr = *II;

      if (!BC.MIA->isCall(Instr))
        continue;

      // Instruction can throw an exception that should be handled.
      const bool Throws = BC.MIA->isInvoke(Instr);

      // Ignore the call if it's a continuation of a no-throw gap.
      if (!Throws && !StartRange)
        continue;

      // Extract exception handling information from the instruction.
      const MCSymbol *LP = nullptr;
      uint64_t Action = 0;
      std::tie(LP, Action) = BC.MIA->getEHInfo(Instr);

      // No action if the exception handler has not changed.
      if (Throws &&
          StartRange &&
          PreviousEH.LP == LP &&
          PreviousEH.Action == Action)
        continue;

      // Same symbol is used for the beginning and the end of the range.
      const MCSymbol *EHSymbol = BC.Ctx->createTempSymbol("EH", true);
      MCInst EHLabel;
      BC.MIA->createEHLabel(EHLabel, EHSymbol, BC.Ctx.get());
      II = std::next(BB->insertPseudoInstr(II, EHLabel));

      // At this point we could be in one of the following states:
      //
      // I. Exception handler has changed and we need to close previous range
      //    and start a new one.
      //
      // II. Start a new exception range after the gap.
      //
      // III. Close current exception range and start a new gap.
      const MCSymbol *EndRange;
      if (StartRange) {
        // I, III:
        EndRange = EHSymbol;
      } else {
        // II:
        StartRange = EHSymbol;
        IsStartInCold = SeenCold;
        EndRange = nullptr;
      }

      // Close the previous range.
      if (EndRange) {
        Sites->emplace_back(CallSite{StartRange, EndRange,
                                     PreviousEH.LP, PreviousEH.Action});
      }

      if (Throws) {
        // I, II:
        StartRange = EHSymbol;
        IsStartInCold = SeenCold;
        PreviousEH = EHInfo{LP, Action};
      } else {
        StartRange = nullptr;
      }
    }
  }

  // Check if we need to close the range.
  if (StartRange) {
    assert((!isSplit() || Sites == &ColdCallSites) && "sites mismatch");
    const auto *EndRange = IsStartInCold ? getFunctionColdEndLabel()
                                         : getFunctionEndLabel();
    Sites->emplace_back(CallSite{StartRange, EndRange,
                                 PreviousEH.LP, PreviousEH.Action});
  }
}

// The code is based on EHStreamer::emitExceptionTable().
void BinaryFunction::emitLSDA(MCStreamer *Streamer, bool EmitColdPart) {
  const auto *Sites = EmitColdPart ? &ColdCallSites : &CallSites;

  auto *StartSymbol = EmitColdPart ? getColdSymbol() : getSymbol();

  if (Sites->empty()) {
    return;
  }

  // Calculate callsite table size. Size of each callsite entry is:
  //
  //  sizeof(start) + sizeof(length) + sizeof(LP) + sizeof(uleb128(action))
  //
  // or
  //
  //  sizeof(dwarf::DW_EH_PE_data4) * 3 + sizeof(uleb128(action))
  uint64_t CallSiteTableLength = Sites->size() * 4 * 3;
  for (const auto &CallSite : *Sites) {
    CallSiteTableLength += getULEB128Size(CallSite.Action);
  }

  Streamer->SwitchSection(BC.MOFI->getLSDASection());

  // When we read we make sure only the following encoding is supported.
  constexpr unsigned TTypeEncoding = dwarf::DW_EH_PE_udata4;

  // Type tables have to be aligned at 4 bytes.
  Streamer->EmitValueToAlignment(4);

  // Emit the LSDA label.
  auto LSDASymbol = EmitColdPart ? getColdLSDASymbol() : getLSDASymbol();
  assert(LSDASymbol && "no LSDA symbol set");
  Streamer->EmitLabel(LSDASymbol);

  // Emit the LSDA header.
  Streamer->EmitIntValue(dwarf::DW_EH_PE_omit, 1); // LPStart format
  Streamer->EmitIntValue(TTypeEncoding, 1);        // TType format

  // See the comment in EHStreamer::emitExceptionTable() on to use
  // uleb128 encoding (which can use variable number of bytes to encode the same
  // value) to ensure type info table is properly aligned at 4 bytes without
  // iteratively fixing sizes of the tables.
  unsigned CallSiteTableLengthSize = getULEB128Size(CallSiteTableLength);
  unsigned TTypeBaseOffset =
    sizeof(int8_t) +                            // Call site format
    CallSiteTableLengthSize +                   // Call site table length size
    CallSiteTableLength +                       // Call site table length
    LSDAActionAndTypeTables.size();             // Actions + Types size
  unsigned TTypeBaseOffsetSize = getULEB128Size(TTypeBaseOffset);
  unsigned TotalSize =
    sizeof(int8_t) +                            // LPStart format
    sizeof(int8_t) +                            // TType format
    TTypeBaseOffsetSize +                       // TType base offset size
    TTypeBaseOffset;                            // TType base offset
  unsigned SizeAlign = (4 - TotalSize) & 3;

  // Account for any extra padding that will be added to the call site table
  // length.
  Streamer->EmitULEB128IntValue(TTypeBaseOffset, SizeAlign);

  // Emit the landing pad call site table. We use signed data4 since we can emit
  // a landing pad in a different part of the split function that could appear
  // earlier in the address space than LPStart.
  Streamer->EmitIntValue(dwarf::DW_EH_PE_sdata4, 1);
  Streamer->EmitULEB128IntValue(CallSiteTableLength);

  for (const auto &CallSite : *Sites) {

    const MCSymbol *BeginLabel = CallSite.Start;
    const MCSymbol *EndLabel = CallSite.End;

    assert(BeginLabel && "start EH label expected");
    assert(EndLabel && "end EH label expected");

    // Start of the range is emitted relative to the start of current
    // function split part.
    Streamer->emitAbsoluteSymbolDiff(BeginLabel, StartSymbol, 4);
    Streamer->emitAbsoluteSymbolDiff(EndLabel, BeginLabel, 4);

    if (!CallSite.LP) {
      Streamer->EmitIntValue(0, 4);
    } else {
      // Difference can get negative if the handler is in hot part.
      Streamer->emitAbsoluteSymbolDiff(CallSite.LP, StartSymbol, 4);
    }

    Streamer->EmitULEB128IntValue(CallSite.Action);
  }

  // Write out action, type, and type index tables at the end.
  //
  // There's no need to change the original format we saw on input
  // unless we are doing a function splitting in which case we can
  // perhaps split and optimize the tables.
  for (auto const &Byte : LSDAActionAndTypeTables) {
    Streamer->EmitIntValue(Byte, 1);
  }
  for (auto const &Byte : LSDATypeIndexTable) {
    Streamer->EmitIntValue(Byte, 1);
  }
}

const uint8_t DWARF_CFI_PRIMARY_OPCODE_MASK = 0xc0;

bool CFIReaderWriter::fillCFIInfoFor(BinaryFunction &Function) const {
  uint64_t Address = Function.getAddress();
  auto I = FDEs.find(Address);
  if (I == FDEs.end())
    return true;

  const FDE &CurFDE = *I->second;
  if (Function.getSize() != CurFDE.getAddressRange()) {
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: CFI information size mismatch for function \""
             << Function << "\""
             << format(": Function size is %dB, CFI covers "
                       "%dB\n",
                       Function.getSize(), CurFDE.getAddressRange());
    }
    return false;
  }

  Function.setLSDAAddress(CurFDE.getLSDAAddress());

  uint64_t Offset = 0;
  uint64_t CodeAlignment = CurFDE.getLinkedCIE()->getCodeAlignmentFactor();
  uint64_t DataAlignment = CurFDE.getLinkedCIE()->getDataAlignmentFactor();
  if (CurFDE.getLinkedCIE()->getPersonalityAddress() != 0) {
    Function.setPersonalityFunction(
        CurFDE.getLinkedCIE()->getPersonalityAddress());
    Function.setPersonalityEncoding(
        CurFDE.getLinkedCIE()->getPersonalityEncoding());
  }

  auto decodeFrameInstruction =
      [&Function, &Offset, Address, CodeAlignment, DataAlignment](
          const FrameEntry::Instruction &Instr) {
        uint8_t Opcode = Instr.Opcode;
        if (Opcode & DWARF_CFI_PRIMARY_OPCODE_MASK)
          Opcode &= DWARF_CFI_PRIMARY_OPCODE_MASK;
        switch (Instr.Opcode) {
        case DW_CFA_nop:
          break;
        case DW_CFA_advance_loc4:
        case DW_CFA_advance_loc2:
        case DW_CFA_advance_loc1:
        case DW_CFA_advance_loc:
          // Advance our current address
          Offset += CodeAlignment * int64_t(Instr.Ops[0]);
          break;
        case DW_CFA_offset_extended_sf:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createOffset(
                          nullptr, Instr.Ops[0],
                          DataAlignment * int64_t(Instr.Ops[1])));
          break;
        case DW_CFA_offset_extended:
        case DW_CFA_offset:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createOffset(
                          nullptr, Instr.Ops[0], DataAlignment * Instr.Ops[1]));
          break;
        case DW_CFA_restore_extended:
        case DW_CFA_restore:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createRestore(nullptr, Instr.Ops[0]));
          break;
        case DW_CFA_set_loc:
          assert(Instr.Ops[0] >= Address && "set_loc out of function bounds");
          assert(Instr.Ops[0] <= Address + Function.getSize() &&
                 "set_loc out of function bounds");
          Offset = Instr.Ops[0] - Address;
          break;

        case DW_CFA_undefined:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createUndefined(nullptr, Instr.Ops[0]));
          break;
        case DW_CFA_same_value:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createSameValue(nullptr, Instr.Ops[0]));
          break;
        case DW_CFA_register:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createRegister(nullptr, Instr.Ops[0],
                                                       Instr.Ops[1]));
          break;
        case DW_CFA_remember_state:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createRememberState(nullptr));
          break;
        case DW_CFA_restore_state:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createRestoreState(nullptr));
          break;
        case DW_CFA_def_cfa:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createDefCfa(nullptr, Instr.Ops[0],
                                                     Instr.Ops[1]));
          break;
        case DW_CFA_def_cfa_sf:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createDefCfa(
                          nullptr, Instr.Ops[0],
                          DataAlignment * int64_t(Instr.Ops[1])));
          break;
        case DW_CFA_def_cfa_register:
          Function.addCFIInstruction(
              Offset,
              MCCFIInstruction::createDefCfaRegister(nullptr, Instr.Ops[0]));
          break;
        case DW_CFA_def_cfa_offset:
          Function.addCFIInstruction(
              Offset,
              MCCFIInstruction::createDefCfaOffset(nullptr, Instr.Ops[0]));
          break;
        case DW_CFA_def_cfa_offset_sf:
          Function.addCFIInstruction(
              Offset, MCCFIInstruction::createDefCfaOffset(
                          nullptr, DataAlignment * int64_t(Instr.Ops[0])));
          break;
        case DW_CFA_GNU_args_size:
          Function.addCFIInstruction(
              Offset,
              MCCFIInstruction::createGnuArgsSize(nullptr, Instr.Ops[0]));
          Function.setUsesGnuArgsSize();
          break;
        case DW_CFA_val_offset_sf:
        case DW_CFA_val_offset:
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: DWARF val_offset() unimplemented\n";
          }
          return false;
        case DW_CFA_expression:
        case DW_CFA_def_cfa_expression:
        case DW_CFA_val_expression: {
          MCDwarfExprBuilder Builder;
          for (const auto &Operation : Instr.ExprOps) {
            switch (Operation.Ops.size()) {
            case 0:
              Builder.appendOperation(Operation.Opcode);
              break;
            case 1:
              Builder.appendOperation(Operation.Opcode, Operation.Ops[0]);
              break;
            case 2:
              Builder.appendOperation(Operation.Opcode, Operation.Ops[0],
                                      Operation.Ops[1]);
              break;
            default:
              llvm_unreachable("Unrecognized DWARF expression");
            }
          }
          if (Opcode == DW_CFA_expression) {
            Function.addCFIInstruction(
                Offset, MCCFIInstruction::createExpression(
                            nullptr, Instr.Ops[0], Builder.take()));
          } else if (Opcode == DW_CFA_def_cfa_expression) {
            Function.addCFIInstruction(Offset,
                                       MCCFIInstruction::createDefCfaExpression(
                                           nullptr, Builder.take()));
          } else {
            assert(Opcode == DW_CFA_val_expression && "Unexpected opcode");
            Function.addCFIInstruction(
                Offset, MCCFIInstruction::createValExpression(
                            nullptr, Instr.Ops[0], Builder.take()));
          }
          break;
        }
        case DW_CFA_MIPS_advance_loc8:
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: DW_CFA_MIPS_advance_loc unimplemented\n";
          }
          return false;
        case DW_CFA_GNU_window_save:
        case DW_CFA_lo_user:
        case DW_CFA_hi_user:
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: DW_CFA_GNU_* and DW_CFA_*_user "
                      "unimplemented\n";
          }
          return false;
        default:
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: Unrecognized CFI instruction\n";
          }
          return false;
        }

        return true;
      };

  for (const FrameEntry::Instruction &Instr : *(CurFDE.getLinkedCIE())) {
    if (!decodeFrameInstruction(Instr))
      return false;
  }

  for (const FrameEntry::Instruction &Instr : CurFDE) {
    if (!decodeFrameInstruction(Instr))
      return false;
  }

  return true;
}

std::vector<char> CFIReaderWriter::generateEHFrameHeader(
    const DWARFFrame &OldEHFrame,
    const DWARFFrame &NewEHFrame,
    uint64_t EHFrameHeaderAddress,
    std::vector<uint64_t> &FailedAddresses) const {
  // Common PC -> FDE map to be written into .eh_frame_hdr.
  std::map<uint64_t, uint64_t> PCToFDE;

  // Presort array for binary search.
  std::sort(FailedAddresses.begin(), FailedAddresses.end());

  // Initialize PCToFDE using NewEHFrame.
  NewEHFrame.for_each_FDE([&](const dwarf::FDE *FDE) {
    const auto FuncAddress = FDE->getInitialLocation();
    const auto FDEAddress = NewEHFrame.EHFrameAddress + FDE->getOffset();

    // Ignore unused FDEs.
    if (FuncAddress == 0)
      return;

    // Add the address to the map unless we failed to write it.
    if (!std::binary_search(FailedAddresses.begin(), FailedAddresses.end(),
                            FuncAddress)) {
      DEBUG(dbgs() << "BOLT-DEBUG: FDE for function at 0x"
                   << Twine::utohexstr(FuncAddress) << " is at 0x"
                   << Twine::utohexstr(FDEAddress) << '\n');
      PCToFDE[FuncAddress] = FDEAddress;
    }
  });

  DEBUG(dbgs() << "BOLT-DEBUG: new .eh_frame contains "
               << NewEHFrame.Entries.size() << " entries\n");

  // Add entries from the original .eh_frame corresponding to the functions
  // that we did not update.
  OldEHFrame.for_each_FDE([&](const dwarf::FDE *FDE) {
    const auto FuncAddress = FDE->getInitialLocation();
    const auto FDEAddress = OldEHFrame.EHFrameAddress + FDE->getOffset();

    // Add the address if we failed to write it.
    if (PCToFDE.count(FuncAddress) == 0) {
      DEBUG(dbgs() << "BOLT-DEBUG: old FDE for function at 0x"
                   << Twine::utohexstr(FuncAddress) << " is at 0x"
                   << Twine::utohexstr(FDEAddress) << '\n');
      PCToFDE[FuncAddress] = FDEAddress;
    }
  });

  DEBUG(dbgs() << "BOLT-DEBUG: old .eh_frame contains "
               << OldEHFrame.Entries.size() << " entries\n");

  // Generate a new .eh_frame_hdr based on the new map.

  // Header plus table of entries of size 8 bytes.
  std::vector<char> EHFrameHeader(12 + PCToFDE.size() * 8);

  // Version is 1.
  EHFrameHeader[0] = 1;
  // Encoding of the eh_frame pointer.
  EHFrameHeader[1] = DW_EH_PE_pcrel | DW_EH_PE_sdata4;
  // Encoding of the count field to follow.
  EHFrameHeader[2] = DW_EH_PE_udata4;
  // Encoding of the table entries - 4-byte offset from the start of the header.
  EHFrameHeader[3] = DW_EH_PE_datarel | DW_EH_PE_sdata4;

  // Address of eh_frame. Use the new one.
  support::ulittle32_t::ref(EHFrameHeader.data() + 4) =
    NewEHFrame.EHFrameAddress - (EHFrameHeaderAddress + 4);

  // Number of entries in the table (FDE count).
  support::ulittle32_t::ref(EHFrameHeader.data() + 8) = PCToFDE.size();

  // Write the table at offset 12.
  auto *Ptr = EHFrameHeader.data();
  uint32_t Offset = 12;
  for (const auto &PCI : PCToFDE) {
    int64_t InitialPCOffset = PCI.first - EHFrameHeaderAddress;
    assert(isInt<32>(InitialPCOffset) && "PC offset out of bounds");
    support::ulittle32_t::ref(Ptr + Offset) = InitialPCOffset;
    Offset += 4;
    int64_t FDEOffset = PCI.second - EHFrameHeaderAddress;
    assert(isInt<32>(FDEOffset) && "FDE offset out of bounds");
    support::ulittle32_t::ref(Ptr + Offset) = FDEOffset;
    Offset += 4;
  }

  return EHFrameHeader;
}

} // namespace bolt
} // namespace llvm
