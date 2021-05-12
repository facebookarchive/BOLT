//===--- BinaryContext.cpp  - Interface for machine-level context ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "llvm/ADT/Twine.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltCategory;

static cl::opt<bool>
PrintDebugInfo("print-debug-info",
  cl::desc("print debug info when printing functions"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintRelocations("print-relocations",
  cl::desc("print relocations when printing functions"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintMemData("print-mem-data",
  cl::desc("print memory data annotations when printing functions"),
  cl::Hidden,
  cl::cat(BoltCategory));

} // namespace opts

BinaryContext::~BinaryContext() { }

MCObjectWriter *BinaryContext::createObjectWriter(raw_pwrite_stream &OS) {
  if (!MAB) {
    MAB = std::unique_ptr<MCAsmBackend>(
        TheTarget->createMCAsmBackend(*MRI, TripleName, ""));
  }

  return MAB->createObjectWriter(OS);
}

MCSymbol *BinaryContext::getOrCreateGlobalSymbol(uint64_t Address,
                                                 Twine Prefix) {
  MCSymbol *Symbol{nullptr};
  std::string Name;
  auto NI = GlobalAddresses.find(Address);
  if (NI != GlobalAddresses.end()) {
    // Even though there could be multiple names registered at the address,
    // we only use the first one.
    Name = NI->second;
  } else {
    Name = (Prefix + "0x" + Twine::utohexstr(Address)).str();
    assert(GlobalSymbols.find(Name) == GlobalSymbols.end() &&
           "created name is not unique");
    GlobalAddresses.emplace(std::make_pair(Address, Name));
  }

  Symbol = Ctx->lookupSymbol(Name);
  if (Symbol)
    return Symbol;

  Symbol = Ctx->getOrCreateSymbol(Name);
  GlobalSymbols[Name] = Address;

  return Symbol;
}

MCSymbol *BinaryContext::getGlobalSymbolAtAddress(uint64_t Address) const {
  auto NI = GlobalAddresses.find(Address);
  if (NI == GlobalAddresses.end())
    return nullptr;

  auto *Symbol = Ctx->lookupSymbol(NI->second);
  assert(Symbol && "symbol cannot be NULL at this point");

  return Symbol;
}

MCSymbol *BinaryContext::getGlobalSymbolByName(const std::string &Name) const {
  auto Itr = GlobalSymbols.find(Name);
  return Itr == GlobalSymbols.end()
    ? nullptr : getGlobalSymbolAtAddress(Itr->second);
}

void BinaryContext::foldFunction(BinaryFunction &ChildBF,
                                 BinaryFunction &ParentBF,
                                 std::map<uint64_t, BinaryFunction> &BFs) {
  // Copy name list.
  ParentBF.addNewNames(ChildBF.getNames());

  // Update internal bookkeeping info.
  for (auto &Name : ChildBF.getNames()) {
    // Calls to functions are handled via symbols, and we keep the lookup table
    // that we need to update.
    auto *Symbol = Ctx->lookupSymbol(Name);
    assert(Symbol && "symbol cannot be NULL at this point");
    SymbolToFunctionMap[Symbol] = &ParentBF;

    // NB: there's no need to update GlobalAddresses and GlobalSymbols.
  }

  // Merge execution counts of ChildBF into those of ParentBF.
  ChildBF.mergeProfileDataInto(ParentBF);

  if (HasRelocations) {
    // Remove ChildBF from the global set of functions in relocs mode.
    auto FI = BFs.find(ChildBF.getAddress());
    assert(FI != BFs.end() && "function not found");
    assert(&ChildBF == &FI->second && "function mismatch");
    FI = BFs.erase(FI);
  } else {
    // In non-relocation mode we keep the function, but rename it.
    std::string NewName = "__ICF_" + ChildBF.Names.back();
    ChildBF.Names.clear();
    ChildBF.Names.push_back(NewName);
    ChildBF.OutputSymbol = Ctx->getOrCreateSymbol(NewName);
    ChildBF.setFolded();
  }
}

void BinaryContext::printGlobalSymbols(raw_ostream& OS) const {
  for (auto &Entry : GlobalSymbols) {
    OS << "(" << Entry.first << " -> 0x"
       << Twine::utohexstr(Entry.second) << ")\n";
  }
}

namespace {

/// Recursively finds DWARF DW_TAG_subprogram DIEs and match them with
/// BinaryFunctions. Record DIEs for unknown subprograms (mostly functions that
/// are never called and removed from the binary) in Unknown.
void findSubprograms(DWARFCompileUnit *Unit,
                     const DWARFDebugInfoEntryMinimal *DIE,
                     std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  if (DIE->isSubprogramDIE()) {
    // TODO: handle DW_AT_ranges.
    uint64_t LowPC, HighPC;
    if (DIE->getLowAndHighPC(Unit, LowPC, HighPC)) {
      auto It = BinaryFunctions.find(LowPC);
      if (It != BinaryFunctions.end()) {
        It->second.addSubprogramDIE(Unit, DIE);
      } else {
        // The function must have been optimized away by GC.
      }
    } else {
      const auto RangesVector = DIE->getAddressRanges(Unit);
      if (!RangesVector.empty()) {
        errs() << "BOLT-ERROR: split function detected in .debug_info. "
                  "Split functions are not supported.\n";
        exit(1);
      }
    }
  }

  for (auto ChildDIE = DIE->getFirstChild();
       ChildDIE != nullptr && !ChildDIE->isNULL();
       ChildDIE = ChildDIE->getSibling()) {
    findSubprograms(Unit, ChildDIE, BinaryFunctions);
  }
}

} // namespace

unsigned BinaryContext::addDebugFilenameToUnit(const uint32_t DestCUID,
                                               const uint32_t SrcCUID,
                                               unsigned FileIndex) {
  auto SrcUnit = DwCtx->getCompileUnitForOffset(SrcCUID);
  auto LineTable = DwCtx->getLineTableForUnit(SrcUnit);
  const auto &FileNames = LineTable->Prologue.FileNames;
  // Dir indexes start at 1, as DWARF file numbers, and a dir index 0
  // means empty dir.
  assert(FileIndex > 0 && FileIndex <= FileNames.size() &&
         "FileIndex out of range for the compilation unit.");
  const char *Dir = FileNames[FileIndex - 1].DirIdx ?
    LineTable->Prologue.IncludeDirectories[FileNames[FileIndex - 1].DirIdx - 1] :
    "";
  return Ctx->getDwarfFile(Dir, FileNames[FileIndex - 1].Name, 0, DestCUID);
}

std::vector<BinaryFunction *> BinaryContext::getSortedFunctions(
    std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  std::vector<BinaryFunction *> SortedFunctions(BinaryFunctions.size());
  std::transform(BinaryFunctions.begin(), BinaryFunctions.end(),
                 SortedFunctions.begin(),
                 [](std::pair<const uint64_t, BinaryFunction> &BFI) {
                   return &BFI.second;
                 });

  std::stable_sort(SortedFunctions.begin(), SortedFunctions.end(),
                   [](const BinaryFunction *A, const BinaryFunction *B) {
                     if (A->hasValidIndex() && B->hasValidIndex()) {
                       return A->getIndex() < B->getIndex();
                     } else {
                       return A->hasValidIndex();
                     }
                   });
  return SortedFunctions;
}

void BinaryContext::preprocessDebugInfo(
    std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  // Populate MCContext with DWARF files.
  for (const auto &CU : DwCtx->compile_units()) {
    const auto CUID = CU->getOffset();
    auto LineTable = DwCtx->getLineTableForUnit(CU.get());
    const auto &FileNames = LineTable->Prologue.FileNames;
    for (size_t I = 0, Size = FileNames.size(); I != Size; ++I) {
      // Dir indexes start at 1, as DWARF file numbers, and a dir index 0
      // means empty dir.
      const char *Dir = FileNames[I].DirIdx ?
          LineTable->Prologue.IncludeDirectories[FileNames[I].DirIdx - 1] :
          "";
      Ctx->getDwarfFile(Dir, FileNames[I].Name, 0, CUID);
    }
  }

  // For each CU, iterate over its children DIEs and match subprogram DIEs to
  // BinaryFunctions.
  for (auto &CU : DwCtx->compile_units()) {
    findSubprograms(CU.get(), CU->getUnitDIE(false), BinaryFunctions);
  }

  // Some functions may not have a corresponding subprogram DIE
  // yet they will be included in some CU and will have line number information.
  // Hence we need to associate them with the CU and include in CU ranges.
  for (auto &AddrFunctionPair : BinaryFunctions) {
    auto FunctionAddress = AddrFunctionPair.first;
    auto &Function = AddrFunctionPair.second;
    if (!Function.getSubprogramDIEs().empty())
      continue;
    if (auto DebugAranges = DwCtx->getDebugAranges()) {
      auto CUOffset = DebugAranges->findAddress(FunctionAddress);
      if (CUOffset != -1U) {
        Function.addSubprogramDIE(DwCtx->getCompileUnitForOffset(CUOffset),
                                  nullptr);
        continue;
      }
    }

#ifdef DWARF_LOOKUP_ALL_RANGES
    // Last resort - iterate over all compile units. This should not happen
    // very often. If it does, we need to create a separate lookup table
    // similar to .debug_aranges internally. This slows down processing
    // considerably.
    for (const auto &CU : DwCtx->compile_units()) {
      const auto *CUDie = CU->getUnitDIE();
      for (const auto &Range : CUDie->getAddressRanges(CU.get())) {
        if (FunctionAddress >= Range.first &&
            FunctionAddress < Range.second) {
          Function.addSubprogramDIE(CU.get(), nullptr);
          break;
        }
      }
    }
#endif
  }
}

void BinaryContext::printCFI(raw_ostream &OS, const MCCFIInstruction &Inst) {
  uint32_t Operation = Inst.getOperation();
  switch (Operation) {
  case MCCFIInstruction::OpSameValue:
    OS << "OpSameValue Reg" << Inst.getRegister();
    break;
  case MCCFIInstruction::OpRememberState:
    OS << "OpRememberState";
    break;
  case MCCFIInstruction::OpRestoreState:
    OS << "OpRestoreState";
    break;
  case MCCFIInstruction::OpOffset:
    OS << "OpOffset Reg" << Inst.getRegister() << " " << Inst.getOffset();
    break;
  case MCCFIInstruction::OpDefCfaRegister:
    OS << "OpDefCfaRegister Reg" << Inst.getRegister();
    break;
  case MCCFIInstruction::OpDefCfaOffset:
    OS << "OpDefCfaOffset " << Inst.getOffset();
    break;
  case MCCFIInstruction::OpDefCfa:
    OS << "OpDefCfa Reg" << Inst.getRegister() << " " << Inst.getOffset();
    break;
  case MCCFIInstruction::OpRelOffset:
    OS << "OpRelOffset";
    break;
  case MCCFIInstruction::OpAdjustCfaOffset:
    OS << "OfAdjustCfaOffset";
    break;
  case MCCFIInstruction::OpEscape:
    OS << "OpEscape";
    break;
  case MCCFIInstruction::OpRestore:
    OS << "OpRestore";
    break;
  case MCCFIInstruction::OpUndefined:
    OS << "OpUndefined";
    break;
  case MCCFIInstruction::OpRegister:
    OS << "OpRegister";
    break;
  case MCCFIInstruction::OpWindowSave:
    OS << "OpWindowSave";
    break;
  case MCCFIInstruction::OpGnuArgsSize:
    OS << "OpGnuArgsSize";
    break;
  default:
    OS << "Op#" << Operation;
    break;
  }
}

void BinaryContext::printInstruction(raw_ostream &OS,
                                     const MCInst &Instruction,
                                     uint64_t Offset,
                                     const BinaryFunction* Function,
                                     bool PrintMCInst,
                                     bool PrintMemData,
                                     bool PrintRelocations) const {
  if (MIA->isEHLabel(Instruction)) {
    OS << "  EH_LABEL: " << *MIA->getTargetSymbol(Instruction) << '\n';
    return;
  }
  OS << format("    %08" PRIx64 ": ", Offset);
  if (MIA->isCFI(Instruction)) {
    uint32_t Offset = Instruction.getOperand(0).getImm();
    OS << "\t!CFI\t$" << Offset << "\t; ";
    if (Function)
      printCFI(OS, *Function->getCFIFor(Instruction));
    OS << "\n";
    return;
  }
  InstPrinter->printInst(&Instruction, OS, "", *STI);
  if (MIA->isCall(Instruction)) {
    if (MIA->isTailCall(Instruction))
      OS << " # TAILCALL ";
    if (MIA->isInvoke(Instruction)) {
      const MCSymbol *LP;
      uint64_t Action;
      std::tie(LP, Action) = MIA->getEHInfo(Instruction);
      OS << " # handler: ";
      if (LP)
        OS << *LP;
      else
        OS << '0';
      OS << "; action: " << Action;
      auto GnuArgsSize = MIA->getGnuArgsSize(Instruction);
      if (GnuArgsSize >= 0)
        OS << "; GNU_args_size = " << GnuArgsSize;
    }
  }
  if (MIA->isIndirectBranch(Instruction)) {
    if (auto JTAddress = MIA->getJumpTable(Instruction)) {
      OS << " # JUMPTABLE @0x" << Twine::utohexstr(JTAddress);
    }
  }

  MIA->forEachAnnotation(
    Instruction,
    [&OS](const MCAnnotation *Annotation) {
      OS << " # " << Annotation->getName() << ": ";
      Annotation->print(OS);
    }
  );

  const DWARFDebugLine::LineTable *LineTable =
    Function && opts::PrintDebugInfo ? Function->getDWARFUnitLineTable().second
                                     : nullptr;

  if (LineTable) {
    auto RowRef = DebugLineTableRowRef::fromSMLoc(Instruction.getLoc());

    if (RowRef != DebugLineTableRowRef::NULL_ROW) {
      const auto &Row = LineTable->Rows[RowRef.RowIndex - 1];
      OS << " # debug line "
         << LineTable->Prologue.FileNames[Row.File - 1].Name
         << ":" << Row.Line;

      if (Row.Column) {
        OS << ":" << Row.Column;
      }
    }
  }

  if ((opts::PrintMemData || PrintMemData) && Function) {
    const auto *MD = Function->getMemData();
    const auto MemDataOffset =
      MIA->tryGetAnnotationAs<uint64_t>(Instruction, "MemDataOffset");
    if (MD && MemDataOffset) {
      bool DidPrint = false;
      for (auto &MI : MD->getMemInfoRange(MemDataOffset.get())) {
        OS << (DidPrint ? ", " : " # Loads: ");
        OS << MI.Addr << "/" << MI.Count;
        DidPrint = true;
      }
    }
  }

  if ((opts::PrintRelocations || PrintRelocations) && Function) {
    const auto Size = computeCodeSize(&Instruction, &Instruction + 1);
    Function->printRelocations(OS, Offset, Size);
  }

  OS << "\n";

  if (PrintMCInst) {
    Instruction.dump_pretty(OS, InstPrinter.get());
    OS << "\n";
  }
}

ErrorOr<ArrayRef<uint8_t>>
BinaryContext::getFunctionData(const BinaryFunction &Function) const {
  auto &Section = Function.getSection();
  assert(Section.containsRange(Function.getAddress(), Function.getSize()) &&
         "wrong section for function");

  if (!Section.isText() || Section.isVirtual() || !Section.getSize()) {
    return std::make_error_code(std::errc::bad_address);
  }

  StringRef SectionContents = Section.getContents();

  assert(SectionContents.size() == Section.getSize() &&
         "section size mismatch");

  // Function offset from the section start.
  auto FunctionOffset = Function.getAddress() - Section.getAddress();
  auto *Bytes = reinterpret_cast<const uint8_t *>(SectionContents.data());
  return ArrayRef<uint8_t>(Bytes + FunctionOffset, Function.getSize());
}

ErrorOr<BinarySection&> BinaryContext::getSectionForAddress(uint64_t Address) {
  auto SI = AllocatableSections.upper_bound(Address);
  if (SI != AllocatableSections.begin()) {
    --SI;
    if (SI->first + SI->second.getSize() > Address)
      return SI->second;
  }
  return std::make_error_code(std::errc::bad_address);
}

ErrorOr<const BinarySection &>
BinaryContext::getSectionForAddress(uint64_t Address) const {
  auto SI = AllocatableSections.upper_bound(Address);
  if (SI != AllocatableSections.begin()) {
    --SI;
    if (SI->first + SI->second.getSize() > Address)
      return SI->second;
  }
  return std::make_error_code(std::errc::bad_address);
}

ErrorOr<uint64_t>
BinaryContext::extractPointerAtAddress(uint64_t Address) const {
  auto Section = getSectionForAddress(Address);
  if (!Section)
    return std::make_error_code(std::errc::bad_address);

  StringRef SectionContents = Section->getContents();
  DataExtractor DE(SectionContents,
                   AsmInfo->isLittleEndian(),
                   AsmInfo->getPointerSize());
  uint32_t SectionOffset = Address - Section->getAddress();
  return DE.getAddress(&SectionOffset);
}

void BinaryContext::addSectionRelocation(BinarySection &Section,
                                         uint64_t Offset,
                                         MCSymbol *Symbol,
                                         uint64_t Type,
                                         uint64_t Addend) {
  Section.addRelocation(Offset, Symbol, Type, Addend);
}

void BinaryContext::addRelocation(uint64_t Address,
                                  MCSymbol *Symbol,
                                  uint64_t Type,
                                  uint64_t Addend) {
  auto Section = getSectionForAddress(Address);
  assert(Section && "cannot find section for address");
  Section->addRelocation(Address - Section->getAddress(), Symbol, Type, Addend);
}

void BinaryContext::removeRelocationAt(uint64_t Address) {
  auto Section = getSectionForAddress(Address);
  assert(Section && "cannot find section for address");
  Section->removeRelocationAt(Address - Section->getAddress());
}

const Relocation *BinaryContext::getRelocationAt(uint64_t Address) {
  auto Section = getSectionForAddress(Address);
  assert(Section && "cannot find section for address");
  return Section->getRelocationAt(Address - Section->getAddress());
}
