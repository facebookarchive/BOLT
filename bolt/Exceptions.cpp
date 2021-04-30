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
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Dwarf.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#undef  DEBUG_TYPE
#define DEBUG_TYPE "flo-exceptions"

STATISTIC(NumLSDAs, "Number of all LSDAs");
STATISTIC(NumTrivialLSDAs,
          "Number of LSDAs with single call site without landing pad or action");

using namespace llvm::dwarf;

namespace llvm {
namespace flo {

namespace opts {

static cl::opt<bool>
PrintExceptions("print-exceptions",
                cl::desc("print exception handling data"),
                cl::Hidden);

} // namespace opts

// readLSDA is reading and dumping the whole .gcc_exception_table section
// at once.
//
// .gcc_except_table section contains a set of Language-Specific Data Areas
// which are basically exception handling tables. One LSDA per function.
// One important observation - you can't actually tell which function LSDA
// refers to, and most addresses are relative to the function start. So you
// have to start with parsing .eh_frame entries that refers to LSDA to obtain
// a function context.
//
// The best visual representation of the tables comprising LSDA and relationship
// between them is illustrated at:
//   http://mentorembedded.github.io/cxx-abi/exceptions.pdf
// Keep in mind that GCC implementation deviates slightly from that document.
//
// To summarize, there are 4 tables in LSDA: call site table, actions table,
// types table, and types index table (indirection). The main table contains
// call site entries. Each call site includes a range that can throw an exception,
// a handler (landing pad), and a reference to an entry in the action table.
// A handler and/or action could be 0. An action entry is in fact a head
// of a list of actions associated with a call site and an action table contains
// all such lists (it could be optimize to share list tails). Each action could be
// either to catch an exception of a given type, to perform a cleanup, or to
// propagate an exception after filtering it out (e.g. to make sure function
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
// For the purpose of rewriting exception handling tables, we can reuse action
// table, types table, and type index table in a binary format when type
// references are hard-coded absolute addresses. We still have to parse all the
// table to determine their size. We have to parse call site table and associate
// discovered information with actual call instructions and landing pad blocks.
void readLSDA(ArrayRef<uint8_t> LSDAData, BinaryContext &BC) {
  const uint8_t *Ptr = LSDAData.data();

  while (Ptr < LSDAData.data() + LSDAData.size()) {
    uint8_t LPStartEncoding = *Ptr++;
    // Some of LSDAs are aligned while other are not. We use the hack below
    // to work around 0-filled alignment. However it could also mean 
    // DW_EH_PE_absptr format.
    //
    // FIXME: the proper way to parse these tables is to get the pointer
    //        from .eh_frame and parse one entry at a time.
    while (!LPStartEncoding)
      LPStartEncoding = *Ptr++;
    if (opts::PrintExceptions) {
      errs() << "[LSDA at 0x"
             << Twine::utohexstr(reinterpret_cast<uint64_t>(Ptr-1)) << "]:\n";
    }

    ++NumLSDAs;
    bool IsTrivial = true;

    uintptr_t LPStart = 0;
    if (LPStartEncoding != DW_EH_PE_omit) {
      LPStart = readEncodedPointer(Ptr, LPStartEncoding);
    }

    uint8_t TTypeEncoding = *Ptr++;
    uintptr_t TTypeEnd = 0;
    if (TTypeEncoding != DW_EH_PE_omit) {
      TTypeEnd = readULEB128(Ptr);
    }

    if (opts::PrintExceptions) {
      errs() << "LPStart Encoding = " << (unsigned)LPStartEncoding << '\n';
      errs() << "LPStart = 0x" << Twine::utohexstr(LPStart) << '\n';
      errs() << "TType Encoding = " << (unsigned)TTypeEncoding << '\n';
      errs() << "TType End = " << TTypeEnd << '\n';
    }

    // Table to store list of indices in type table. Entries are uleb128s values.
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
      errs() << "CallSite Encoding = " << (unsigned)CallSiteEncoding << '\n';
      errs() << "CallSite table length = " << CallSiteTableLength << '\n';
      errs() << '\n';
    }

    unsigned NumCallSites = 0;
    while (CallSitePtr < CallSiteTableEnd) {
      ++NumCallSites;
      uintptr_t Start = readEncodedPointer(CallSitePtr, CallSiteEncoding);
      uintptr_t Length = readEncodedPointer(CallSitePtr, CallSiteEncoding);
      uintptr_t LandingPad = readEncodedPointer(CallSitePtr, CallSiteEncoding);

      uintptr_t ActionEntry = readULEB128(CallSitePtr);
      uint64_t RangeBase = 0;
      if (opts::PrintExceptions) {
        errs() << "Call Site: [0x" << Twine::utohexstr(RangeBase + Start)
               << ", 0x" << Twine::utohexstr(RangeBase + Start + Length)
               << "); landing pad: 0x" << Twine::utohexstr(LPStart + LandingPad)
               << "; action entry: 0x" << Twine::utohexstr(ActionEntry) << "\n";
      }
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
          errs() << "    actions: ";
        const uint8_t *ActionPtr = ActionTableStart + ActionEntry - 1;
        long long ActionType;
        long long ActionNext;
        auto Sep = "";
        do {
          ActionType = readSLEB128(ActionPtr);
          auto Self = ActionPtr;
          ActionNext = readSLEB128(ActionPtr);
          if (opts::PrintExceptions)
            errs() << Sep << "(" << ActionType << ", " << ActionNext << ") ";
          if (ActionType == 0) {
            if (opts::PrintExceptions)
              errs() << "cleanup";
          } else if (ActionType > 0) {
            // It's an index into a type table.
            if (opts::PrintExceptions) {
              errs() << "catch type ";
              printType(ActionType, errs());
            }
          } else { // ActionType < 0
            if (opts::PrintExceptions)
              errs() << "filter exception types ";
            auto TSep = "";
            // ActionType is a negative byte offset into uleb128-encoded table
            // of indices with base 1.
            // E.g. -1 means offset 0, -2 is offset 1, etc. The indices are
            // encoded using uleb128 so we cannot directly dereference them.
            auto TypeIndexTablePtr = TypeIndexTableStart - ActionType - 1;
            while (auto Index = readULEB128(TypeIndexTablePtr)) {
              if (opts::PrintExceptions) {
                errs() << TSep;
                printType(Index, errs());
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
          errs() << '\n';
      }

      if (LandingPad != 0 || ActionEntry != 0)
        IsTrivial = false;
    }
    Ptr = CallSiteTableEnd;

    if (NumCallSites > 1)
      IsTrivial = false;

    if (IsTrivial)
      ++NumTrivialLSDAs;

    if (opts::PrintExceptions)
      errs() << '\n';

    if (CallSiteTableLength == 0 || TTypeEnd == 0)
      continue;

    Ptr = TypeIndexTableStart + MaxTypeIndexTableOffset;
  }
}

void BinaryFunction::parseLSDA(ArrayRef<uint8_t> LSDASectionData,
                               uint64_t LSDASectionAddress) {
  assert(CurrentState == State::Disassembled && "unexpecrted function state");

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
    errs() << "LPStart Encoding = " << (unsigned)LPStartEncoding << '\n';
    errs() << "LPStart = 0x" << Twine::utohexstr(LPStart) << '\n';
    errs() << "TType Encoding = " << (unsigned)TTypeEncoding << '\n';
    errs() << "TType End = " << TTypeEnd << '\n';
  }

  // Table to store list of indices in type table. Entries are uleb128s values.
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
    errs() << "CallSite Encoding = " << (unsigned)CallSiteEncoding << '\n';
    errs() << "CallSite table length = " << CallSiteTableLength << '\n';
    errs() << '\n';
  }

  unsigned NumCallSites = 0;
  while (CallSitePtr < CallSiteTableEnd) {
    ++NumCallSites;
    uintptr_t Start = readEncodedPointer(CallSitePtr, CallSiteEncoding);
    uintptr_t Length = readEncodedPointer(CallSitePtr, CallSiteEncoding);
    uintptr_t LandingPad = readEncodedPointer(CallSitePtr, CallSiteEncoding);

    uintptr_t ActionEntry = readULEB128(CallSitePtr);
    uint64_t RangeBase = getAddress();
    if (opts::PrintExceptions) {
      errs() << "Call Site: [0x" << Twine::utohexstr(RangeBase + Start)
             << ", 0x" << Twine::utohexstr(RangeBase + Start + Length)
             << "); landing pad: 0x" << Twine::utohexstr(LPStart + LandingPad)
             << "; action entry: 0x" << Twine::utohexstr(ActionEntry) << "\n";
    }

    // Create a handler entry if necessary.
    MCSymbol *LPSymbol{nullptr};
    if (LandingPad) {
      auto Label = Labels.find(LandingPad);
      if (Label != Labels.end()) {
        LPSymbol = Label->second;
      } else {
        LPSymbol = BC.Ctx->createTempSymbol("LP", true);
        Labels[LandingPad] = LPSymbol;
      }
      LandingPads.insert(LPSymbol);
    }

    // Mark all call instructions in the range.
    auto II = Instructions.find(Start);
    auto IE = Instructions.end();
    assert(II != IE && "exception range not pointing to an instruction");
    do {
      auto &Instruction = II->second;
      if (BC.MIA->isCall(Instruction)) {
        if (LPSymbol) {
          Instruction.addOperand(MCOperand::createExpr(
              MCSymbolRefExpr::create(LPSymbol,
                                      MCSymbolRefExpr::VK_None,
                                      *BC.Ctx)));
        } else {
          Instruction.addOperand(MCOperand::createImm(0));
        }
        Instruction.addOperand(MCOperand::createImm(ActionEntry));
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
        errs() << "    actions: ";
      const uint8_t *ActionPtr = ActionTableStart + ActionEntry - 1;
      long long ActionType;
      long long ActionNext;
      auto Sep = "";
      do {
        ActionType = readSLEB128(ActionPtr);
        auto Self = ActionPtr;
        ActionNext = readSLEB128(ActionPtr);
        if (opts::PrintExceptions)
          errs() << Sep << "(" << ActionType << ", " << ActionNext << ") ";
        if (ActionType == 0) {
          if (opts::PrintExceptions)
            errs() << "cleanup";
        } else if (ActionType > 0) {
          // It's an index into a type table.
          if (opts::PrintExceptions) {
            errs() << "catch type ";
            printType(ActionType, errs());
          }
        } else { // ActionType < 0
          if (opts::PrintExceptions)
            errs() << "filter exception types ";
          auto TSep = "";
          // ActionType is a negative byte offset into uleb128-encoded table
          // of indices with base 1.
          // E.g. -1 means offset 0, -2 is offset 1, etc. The indices are
          // encoded using uleb128 so we cannot directly dereference them.
          auto TypeIndexTablePtr = TypeIndexTableStart - ActionType - 1;
          while (auto Index = readULEB128(TypeIndexTablePtr)) {
            if (opts::PrintExceptions) {
              errs() << TSep;
              printType(Index, errs());
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
        errs() << '\n';
    }
  }
  if (opts::PrintExceptions)
    errs() << '\n';
}

void BinaryFunction::updateEHRanges() {
  assert(CurrentState == State::CFG && "unexpected state");

  // Build call sites table.
  struct EHInfo {
    const MCSymbol *LP; // landing pad
    uint64_t Action;
  };

  // Markers for begining and the end of exceptions range.
  const MCSymbol *StartRange{nullptr};
  const MCSymbol *EndRange{nullptr};

  // If previous call can throw, this is its exception handler.
  EHInfo PreviousEH = {nullptr, 0};

  for(auto &BB : BasicBlocksLayout) {
    for (auto II = BB->begin(); II != BB->end(); ++II) {
      auto Instr = *II;

      if (!BC.MIA->isCall(Instr))
        continue;

      // Instruction can throw an exception that should be handled.
      bool Throws = Instr.getNumOperands() > 1;

      // Ignore the call if it's a continuation of a no-throw gap.
      if (!Throws && !StartRange)
        continue;

      // Extract exception handling information from the instruction.
      const MCSymbol *LP =
        Throws ? (Instr.getOperand(1).isExpr()
                   ? &(cast<MCSymbolRefExpr>(
                                    Instr.getOperand(1).getExpr())->getSymbol())
                   : nullptr)
               : nullptr;
      uint64_t Action = Throws ? Instr.getOperand(2).getImm() : 0;

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
      II = BB->Instructions.insert(II, EHLabel);
      ++II;

      // At this point we could be in the one of the following states:
      //
      // I. Exception handler has changed and we need to close the prev range
      //    and start the new one.
      //
      // II. Start the new exception range after the gap.
      //
      // III. Close exception range and start the new gap.

      if (StartRange) {
        // I, III:
        EndRange = EHSymbol;
      } else {
        // II:
        StartRange = EHSymbol;
        EndRange = nullptr;
      }

      // Close the previous range.
      if (EndRange) {
        assert(StartRange && "beginning of the range expected");
        CallSites.emplace_back(CallSite{StartRange, EndRange,
                                        PreviousEH.LP, PreviousEH.Action});
        EndRange = nullptr;
      }

      if (Throws) {
        // I, II:
        StartRange = EHSymbol;
        PreviousEH = EHInfo{LP, Action};
      } else {
        StartRange = nullptr;
      }
    }
  }

  // Check if we need to close the range.
  if (StartRange) {
    assert(!EndRange && "unexpected end of range");
    EndRange = BC.Ctx->createTempSymbol("EH", true);
    MCInst EHLabel;
    BC.MIA->createEHLabel(EHLabel, EndRange, BC.Ctx.get());
    BasicBlocksLayout.back()->Instructions.emplace_back(EHLabel);

    CallSites.emplace_back(CallSite{StartRange, EndRange,
                                    PreviousEH.LP, PreviousEH.Action});
  }
}

const uint8_t DWARF_CFI_PRIMARY_OPCODE_MASK = 0xc0;
const uint8_t DWARF_CFI_PRIMARY_OPERAND_MASK = 0x3f;

void CFIReader::fillCFIInfoFor(BinaryFunction &Function) const {
  uint64_t Address = Function.getAddress();
  auto I = FDEs.find(Address);
  if (I == FDEs.end())
    return;

  const FDE &CurFDE = *I->second;
  if (Function.getSize() != CurFDE.getAddressRange()) {
    errs() << "FLO-WARNING: CFI information size mismatch for function \""
           << Function.getName() << "\""
           << format(": Function size is %dB, CFI covers "
                     "%dB\n",
                     Function.getSize(), CurFDE.getAddressRange());
  }

  Function.setLSDAAddress(CurFDE.getLSDAAddress());

  uint64_t Offset = 0;
  uint64_t CodeAlignment = CurFDE.getLinkedCIE()->getCodeAlignmentFactor();
  uint64_t DataAlignment = CurFDE.getLinkedCIE()->getDataAlignmentFactor();
  for (const FrameEntry::Instruction &Instr : CurFDE) {
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
      Function.addCFI(Offset, MCCFIInstruction::createOffset(
                                  nullptr, Instr.Ops[0],
                                  DataAlignment * int64_t(Instr.Ops[1])));
      break;
    case DW_CFA_offset_extended:
    case DW_CFA_offset:
      Function.addCFI(
          Offset, MCCFIInstruction::createOffset(nullptr, Instr.Ops[0],
                                                 DataAlignment * Instr.Ops[1]));
      break;
    case DW_CFA_restore_extended:
    case DW_CFA_restore:
      Function.addCFI(Offset,
                      MCCFIInstruction::createRestore(nullptr, Instr.Ops[0]));
      break;
    case DW_CFA_set_loc:
      assert(Instr.Ops[0] < Address && "set_loc out of function bounds");
      assert(Instr.Ops[0] > Address + Function.getSize() &&
             "set_loc out of function bounds");
      Offset = Instr.Ops[0] - Address;
      break;

    case DW_CFA_undefined:
      Function.addCFI(Offset,
                      MCCFIInstruction::createUndefined(nullptr, Instr.Ops[0]));
      break;
    case DW_CFA_same_value:
      Function.addCFI(Offset,
                      MCCFIInstruction::createSameValue(nullptr, Instr.Ops[0]));
      break;
    case DW_CFA_register:
      Function.addCFI(Offset, MCCFIInstruction::createRegister(
                                  nullptr, Instr.Ops[0], Instr.Ops[1]));
      break;
    case DW_CFA_remember_state:
      Function.addCFI(Offset, MCCFIInstruction::createRememberState(nullptr));
      break;
    case DW_CFA_restore_state:
      Function.addCFI(Offset, MCCFIInstruction::createRestoreState(nullptr));
      break;
    case DW_CFA_def_cfa:
      Function.addCFI(Offset, MCCFIInstruction::createDefCfa(
                                  nullptr, Instr.Ops[0], Instr.Ops[1]));
      break;
    case DW_CFA_def_cfa_sf:
      Function.addCFI(Offset, MCCFIInstruction::createDefCfa(
                                  nullptr, Instr.Ops[0],
                                  DataAlignment * int64_t(Instr.Ops[1])));
      break;
    case DW_CFA_def_cfa_register:
      Function.addCFI(Offset, MCCFIInstruction::createDefCfaRegister(
                                  nullptr, Instr.Ops[0]));
      break;
    case DW_CFA_def_cfa_offset:
      Function.addCFI(
          Offset, MCCFIInstruction::createDefCfaOffset(nullptr, Instr.Ops[0]));
      break;
    case DW_CFA_def_cfa_offset_sf:
      Function.addCFI(Offset,
                      MCCFIInstruction::createDefCfaOffset(
                          nullptr, DataAlignment * int64_t(Instr.Ops[0])));
      break;
    case DW_CFA_val_offset_sf:
    case DW_CFA_val_offset:
      llvm_unreachable("DWARF val_offset() unimplemented");
      break;
    case DW_CFA_expression:
    case DW_CFA_def_cfa_expression:
    case DW_CFA_val_expression:
      llvm_unreachable("DWARF CFA expressions unimplemented");
      break;
      dbgs() << "DW_CFA_val_expression";
      break;
    case DW_CFA_MIPS_advance_loc8:
      llvm_unreachable("DW_CFA_MIPS_advance_loc unimplemented");
      break;
    case DW_CFA_GNU_args_size:
    case DW_CFA_GNU_window_save:
    case DW_CFA_lo_user:
    case DW_CFA_hi_user:
      llvm_unreachable("DW_CFA_GNU_* and DW_CFA_*_use unimplemented");
      break;
    default:
      llvm_unreachable("Unrecognized CFI instruction");
    }
  }
}

} // namespace flo
} // namespace llvm
