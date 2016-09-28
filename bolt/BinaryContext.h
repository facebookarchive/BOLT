//===--- BinaryContext.h  - Interface for machine-level context -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Context for processing binary executables in files and/or memory.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_BINARY_CONTEXT_H
#define LLVM_TOOLS_LLVM_BOLT_BINARY_CONTEXT_H

#include "DebugData.h"
#include "llvm/ADT/Triple.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace llvm {

class DWARFDebugInfoEntryMinimal;

using namespace object;

namespace bolt {

class BinaryFunction;
class DataReader;

/// Relocation class.
struct Relocation {
  uint64_t Offset;
  MCSymbol *Symbol;
  uint64_t Type;
  uint64_t Addend;
  uint64_t Value;

  /// Return size of the given relocation \p Type.
  static size_t getSizeForType(uint64_t Type);

  /// Return true if relocation type is PC-relative. Return false otherwise.
  static bool isPCRelative(uint64_t Type);

  /// Emit relocation at a current \p Streamer' position. The caller is
  /// responsible for setting the position correctly.
  size_t emit(MCStreamer *Streamer);
};

/// Relocation ordering by offset.
inline bool operator<(const Relocation &A, const Relocation &B) {
  return A.Offset < B.Offset;
}

class BinaryContext {

  BinaryContext() = delete;

public:

  /// [name] -> [address] map used for global symbol resolution.
  typedef std::map<std::string, uint64_t> SymbolMapType;
  SymbolMapType GlobalSymbols;

  /// [address] -> [name1], [name2], ...
  std::multimap<uint64_t, std::string> GlobalAddresses;

  /// [MCSymbol] -> [BinaryFunction]
  std::unordered_map<const MCSymbol *,
                     const BinaryFunction *> SymbolToFunctionMap;

  /// Map virtual address to a section.
  std::map<uint64_t, SectionRef> AllocatableSections;

  /// Set of addresses in the code that are not a function start, and are
  /// referenced from outside of containing function. E.g. this could happen
  /// when a function has more than a single entry point.
  std::set<uint64_t> InterproceduralReferences;

  /// List of DWARF location lists in .debug_loc.
  std::vector<LocationList> LocationLists;

  /// List of relocation offsets where relocations should be ignored.
  std::set<uint64_t> IgnoredRelocations;

  /// List of PC-relative relocations from data to code.
  std::set<uint64_t> PCRelativeDataRelocations;

  /// Section relocations.
  std::map<SectionRef, std::vector<Relocation>> SectionRelocations;

  /// List of DWARF entries in .debug_info that have address ranges to be
  /// updated. These include lexical blocks (DW_TAG_lexical_block) and concrete
  /// instances of inlined subroutines (DW_TAG_inlined_subroutine).
  std::vector<AddressRangesDWARFObject> AddressRangesObjects;

  using DIECompileUnitVector =
    std::vector<std::pair<const DWARFDebugInfoEntryMinimal *,
                          const DWARFCompileUnit *>> ;

  /// List of subprogram DIEs that have addresses that don't match any
  /// function, along with their CU.
  DIECompileUnitVector UnknownFunctions;

  std::unique_ptr<MCContext> Ctx;

  std::unique_ptr<DWARFContext> DwCtx;

  std::unique_ptr<Triple> TheTriple;

  const Target *TheTarget;

  std::string TripleName;

  std::unique_ptr<MCCodeEmitter> MCE;

  std::unique_ptr<MCObjectFileInfo> MOFI;

  std::unique_ptr<const MCAsmInfo> AsmInfo;

  std::unique_ptr<const MCInstrInfo> MII;

  std::unique_ptr<const MCSubtargetInfo> STI;

  std::unique_ptr<MCInstPrinter> InstPrinter;

  std::unique_ptr<const MCInstrAnalysis> MIA;

  std::unique_ptr<const MCRegisterInfo> MRI;

  std::unique_ptr<MCDisassembler> DisAsm;

  std::function<void(std::error_code)> ErrorCheck;

  const DataReader &DR;

  BinaryContext(std::unique_ptr<MCContext> Ctx,
                std::unique_ptr<DWARFContext> DwCtx,
                std::unique_ptr<Triple> TheTriple,
                const Target *TheTarget,
                std::string TripleName,
                std::unique_ptr<MCCodeEmitter> MCE,
                std::unique_ptr<MCObjectFileInfo> MOFI,
                std::unique_ptr<const MCAsmInfo> AsmInfo,
                std::unique_ptr<const MCInstrInfo> MII,
                std::unique_ptr<const MCSubtargetInfo> STI,
                std::unique_ptr<MCInstPrinter> InstPrinter,
                std::unique_ptr<const MCInstrAnalysis> MIA,
                std::unique_ptr<const MCRegisterInfo> MRI,
                std::unique_ptr<MCDisassembler> DisAsm,
                const DataReader &DR) :
      Ctx(std::move(Ctx)),
      DwCtx(std::move(DwCtx)),
      TheTriple(std::move(TheTriple)),
      TheTarget(TheTarget),
      TripleName(TripleName),
      MCE(std::move(MCE)),
      MOFI(std::move(MOFI)),
      AsmInfo(std::move(AsmInfo)),
      MII(std::move(MII)),
      STI(std::move(STI)),
      InstPrinter(std::move(InstPrinter)),
      MIA(std::move(MIA)),
      MRI(std::move(MRI)),
      DisAsm(std::move(DisAsm)),
      DR(DR) {}

  ~BinaryContext();

  /// Return a global symbol registered at a given \p Address. If no symbol
  /// exists, create one with unique name using \p Prefix.
  /// If there are multiple symbols registered at the \p Address, then
  /// return the first one.
  MCSymbol *getOrCreateGlobalSymbol(uint64_t Address, Twine Prefix);

  /// Return MCSymbol registered at a given \p Address or nullptr if no
  /// global symbol was registered at the location.
  MCSymbol *getGlobalSymbolAtAddress(uint64_t Address) const;

  /// Print the global symbol table.
  void printGlobalSymbols(raw_ostream& OS) const;

  /// Return (allocatable) section containing the given \p Address.
  ErrorOr<SectionRef> getSectionForAddress(uint64_t Address) const;

  /// Register a symbol with \p Name at a given \p Address.
  MCSymbol *registerNameAtAddress(const std::string &Name, uint64_t Address) {
    // Add the name to global symbols map.
    GlobalSymbols[Name] = Address;

    // Add to the reverse map. There could multiple names at the same address.
    GlobalAddresses.emplace(std::make_pair(Address, Name));

    // Register the name with MCContext.
    return Ctx->getOrCreateSymbol(Name);
  }

  /// Add section relocation.
  void addSectionRelocation(SectionRef Section, uint64_t Address,
                            MCSymbol *Symbol, uint64_t Type,
                            uint64_t Addend = 0);

  const BinaryFunction *getFunctionForSymbol(const MCSymbol *Symbol) const {
    auto BFI = SymbolToFunctionMap.find(Symbol);
    return BFI == SymbolToFunctionMap.end() ? nullptr : BFI->second;
  }

  /// Populate some internal data structures with debug info.
  void preprocessDebugInfo(
      std::map<uint64_t, BinaryFunction> &BinaryFunctions);

  /// Populate internal data structures with debug info that depends on
  /// disassembled functions.
  void preprocessFunctionDebugInfo(
      std::map<uint64_t, BinaryFunction> &BinaryFunctions);

  /// Add a filename entry from SrcCUID to DestCUID.
  unsigned addDebugFilenameToUnit(const uint32_t DestCUID,
                                  const uint32_t SrcCUID,
                                  unsigned FileIndex);

  /// Compute the native code size for a range of instructions.
  /// Note: this can be imprecise wrt the final binary since happening prior to
  /// relaxation, as well as wrt the original binary because of opcode
  /// shortening.
  template <typename Itr>
  uint64_t computeCodeSize(Itr Beg, Itr End) const {
    uint64_t Size = 0;
    while (Beg != End) {
      // Calculate the size of the instruction.
      SmallString<256> Code;
      SmallVector<MCFixup, 4> Fixups;
      raw_svector_ostream VecOS(Code);
      MCE->encodeInstruction(*Beg++, VecOS, Fixups, *STI);
      Size += Code.size();
    }
    return Size;
  }

  /// Print the string name for a CFI operation.
  static void printCFI(raw_ostream &OS, uint32_t Operation);

  /// Print a single MCInst in native format.  If Function is non-null,
  /// the instruction will be annotated with CFI and possibly DWARF line table
  /// info.
  /// If printMCInst is true, the instruction is also printed in the
  /// architecture independent format.
  void printInstruction(raw_ostream &OS,
                        const MCInst &Instruction,
                        uint64_t Offset = 0,
                        const BinaryFunction *Function = nullptr,
                        bool printMCInst = false) const;

  /// Print a range of instructions.
  template <typename Itr>
  uint64_t printInstructions(raw_ostream &OS,
                             Itr Begin,
                             Itr End,
                             uint64_t Offset = 0,
                             const BinaryFunction *Function = nullptr,
                             bool printMCInst = false) const {
    while (Begin != End) {
      printInstruction(OS, *Begin, Offset, Function, printMCInst);
      Offset += computeCodeSize(Begin, Begin + 1);
      ++Begin;
    }
    return Offset;
  }
};

} // namespace bolt
} // namespace llvm

#endif
