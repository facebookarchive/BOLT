//===--- DWARFRewriter.cpp ------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//


#include "BinaryBasicBlock.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "RewriteInstance.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Timer.h"
#include <algorithm>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace llvm::support::endian;
using namespace object;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltCategory;
extern cl::opt<unsigned> Verbosity;

static cl::opt<bool>
KeepARanges("keep-aranges",
  cl::desc("keep or generate .debug_aranges section if .gdb_index is written"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

} // namespace opts

void RewriteInstance::updateDebugInfo() {
  SectionPatchers[".debug_abbrev"] = llvm::make_unique<DebugAbbrevPatcher>();
  SectionPatchers[".debug_info"]  = llvm::make_unique<SimpleBinaryPatcher>();

  RangesSectionsWriter = llvm::make_unique<DebugRangesSectionsWriter>(BC.get());
  LocationListWriter = llvm::make_unique<DebugLocWriter>(BC.get());

  for (auto &CU : BC->DwCtx->compile_units()) {
    updateUnitDebugInfo(CU->getUnitDIE(false),
                        std::vector<const BinaryFunction *>{});
  }

  finalizeDebugSections();

  updateGdbIndexSection();
}

void RewriteInstance::updateUnitDebugInfo(
    const DWARFDie DIE,
    std::vector<const BinaryFunction *> FunctionStack) {

  bool IsFunctionDef = false;
  switch (DIE.getTag()) {
  case dwarf::DW_TAG_compile_unit:
    {
      const auto ModuleRanges = DIE.getAddressRanges();
      auto OutputRanges = translateModuleAddressRanges(ModuleRanges);
      const auto RangesSectionOffset =
        RangesSectionsWriter->addCURanges(DIE.getDwarfUnit()->getOffset(),
                                          std::move(OutputRanges));
      updateDWARFObjectAddressRanges(DIE, RangesSectionOffset);
    }
    break;

  case dwarf::DW_TAG_subprogram:
    {
      // The function cannot have multiple ranges on the input.
      uint64_t SectionIndex, LowPC, HighPC;
      if (DIE.getLowAndHighPC(LowPC, HighPC, SectionIndex)) {
        IsFunctionDef = true;
        const auto *Function = getBinaryFunctionAtAddress(LowPC);
        if (Function && Function->isFolded()) {
          Function = nullptr;
        }
        FunctionStack.push_back(Function);
        auto RangesSectionOffset =
          RangesSectionsWriter->getEmptyRangesOffset();
        if (Function) {
          auto FunctionRanges = Function->getOutputAddressRanges();
          RangesSectionOffset =
            RangesSectionsWriter->addRanges(Function,
                                            std::move(FunctionRanges));
        }
        updateDWARFObjectAddressRanges(DIE, RangesSectionOffset);
      }
    }
    break;

  case dwarf::DW_TAG_lexical_block:
  case dwarf::DW_TAG_inlined_subroutine:
  case dwarf::DW_TAG_try_block:
  case dwarf::DW_TAG_catch_block:
    {
      auto RangesSectionOffset =
        RangesSectionsWriter->getEmptyRangesOffset();
      const BinaryFunction *Function =
        FunctionStack.empty() ? nullptr : FunctionStack.back();
      if (Function) {
        const auto Ranges = DIE.getAddressRanges();
        auto OutputRanges = Function->translateInputToOutputRanges(Ranges);
        DEBUG(
          if (OutputRanges.empty() != Ranges.empty()) {
            dbgs() << "BOLT-DEBUG: problem with DIE at 0x"
                   << Twine::utohexstr(DIE.getOffset()) << " in CU at 0x"
                   << Twine::utohexstr(DIE.getDwarfUnit()->getOffset()) << '\n';
          }
        );
        RangesSectionOffset =
          RangesSectionsWriter->addRanges(Function, std::move(OutputRanges));
      }
      updateDWARFObjectAddressRanges(DIE, RangesSectionOffset);
    }
    break;

  default:
    {
      // Handle any tag that can have DW_AT_location attribute.
      DWARFFormValue Value;
      uint32_t AttrOffset;
      const BinaryFunction *Function =
        FunctionStack.empty() ? nullptr : FunctionStack.back();
      if (auto V = DIE.find(dwarf::DW_AT_location, &AttrOffset)) {
        Value = *V;
        if (Value.isFormClass(DWARFFormValue::FC_Constant) ||
            Value.isFormClass(DWARFFormValue::FC_SectionOffset)) {
          auto LocListSectionOffset = LocationListWriter->getEmptyListOffset();
          if (Function) {
            // Limit parsing to a single list to save memory.
            DWARFDebugLoc::LocationList LL;
            LL.Offset = Value.isFormClass(DWARFFormValue::FC_Constant) ?
              Value.getAsUnsignedConstant().getValue() :
              Value.getAsSectionOffset().getValue();

            uint32_t LLOff = LL.Offset;
            auto OptLL =
                DIE.getDwarfUnit()->getContext().getOneDebugLocList(&LLOff);
            if (!OptLL || OptLL->Entries.empty()) {
              errs() << "BOLT-WARNING: empty location list detected at 0x"
                     << Twine::utohexstr(LLOff) << " for DIE at 0x"
                     << Twine::utohexstr(DIE.getOffset()) << " in CU at 0x"
                     << Twine::utohexstr(DIE.getDwarfUnit()->getOffset())
                     << '\n';
            } else {
              const auto OutputLL =
                  Function->translateInputToOutputLocationList(
                      *OptLL, *DIE.getDwarfUnit()->getBaseAddress());
              DEBUG(if (OutputLL.Entries.empty()) {
                dbgs() << "BOLT-DEBUG: location list translated to an empty "
                          "one at 0x"
                       << Twine::utohexstr(DIE.getOffset()) << " in CU at 0x"
                       << Twine::utohexstr(DIE.getDwarfUnit()->getOffset())
                       << '\n';
              });
              LocListSectionOffset = LocationListWriter->addList(OutputLL);
            }
          }

          auto DebugInfoPatcher =
              static_cast<SimpleBinaryPatcher *>(
                  SectionPatchers[".debug_info"].get());
          DebugInfoPatcher->addLE32Patch(AttrOffset, LocListSectionOffset);
        } else {
          assert((Value.isFormClass(DWARFFormValue::FC_Exprloc) ||
                  Value.isFormClass(DWARFFormValue::FC_Block)) &&
                 "unexpected DW_AT_location form");
        }
      } else if (auto V = DIE.find(dwarf::DW_AT_low_pc, &AttrOffset)) {
        Value = *V;
        const auto Result = Value.getAsAddress();
        if (Result.hasValue()) {
          uint64_t NewAddress = 0;
          if (Function) {
            const auto Address = Result.getValue();
            NewAddress = Function->translateInputToOutputAddress(Address);
            DEBUG(dbgs() << "BOLT-DEBUG: Fixing low_pc 0x"
                         << Twine::utohexstr(Address)
                         << " for DIE with tag " << DIE.getTag()
                         << " to 0x" << Twine::utohexstr(NewAddress) << '\n');
          }
          auto DebugInfoPatcher =
              static_cast<SimpleBinaryPatcher *>(
                  SectionPatchers[".debug_info"].get());
          DebugInfoPatcher->addLE64Patch(AttrOffset, NewAddress);
        } else if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: unexpected form value for attribute at 0x"
                 << Twine::utohexstr(AttrOffset);
        }
      }
    }
  }

  // Recursively update each child.
  for (auto Child = DIE.getFirstChild(); Child; Child = Child.getSibling()) {
    updateUnitDebugInfo(Child, FunctionStack);
  }

  if (IsFunctionDef)
    FunctionStack.pop_back();
}

void RewriteInstance::updateDWARFObjectAddressRanges(
    const DWARFDie DIE, uint64_t DebugRangesOffset) {

  // Some objects don't have an associated DIE and cannot be updated (such as
  // compiler-generated functions).
  if (!DIE) {
    return;
  }

  if (opts::Verbosity >= 2 && DebugRangesOffset == -1U) {
    errs() << "BOLT-WARNING: using invalid DW_AT_range for DIE at offset 0x"
           << Twine::utohexstr(DIE.getOffset()) << '\n';
  }

  auto DebugInfoPatcher =
      static_cast<SimpleBinaryPatcher *>(SectionPatchers[".debug_info"].get());
  auto AbbrevPatcher =
      static_cast<DebugAbbrevPatcher*>(SectionPatchers[".debug_abbrev"].get());

  assert(DebugInfoPatcher && AbbrevPatcher && "Patchers not initialized.");

  const auto *AbbreviationDecl = DIE.getAbbreviationDeclarationPtr();
  if (!AbbreviationDecl) {
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: object's DIE doesn't have an abbreviation: "
             << "skipping update. DIE at offset 0x"
             << Twine::utohexstr(DIE.getOffset()) << '\n';
    }
    return;
  }

  auto AbbrevCode = AbbreviationDecl->getCode();

  if (AbbreviationDecl->findAttributeIndex(dwarf::DW_AT_ranges)) {
    // Case 1: The object was already non-contiguous and had DW_AT_ranges.
    // In this case we simply need to update the value of DW_AT_ranges.
    uint32_t AttrOffset = -1U;
    DIE.find(dwarf::DW_AT_ranges, &AttrOffset);
    assert(AttrOffset != -1U &&  "failed to locate DWARF attribute");
    DebugInfoPatcher->addLE32Patch(AttrOffset, DebugRangesOffset);
  } else {
    // Case 2: The object has both DW_AT_low_pc and DW_AT_high_pc emitted back
    // to back. We replace the attributes with DW_AT_ranges and DW_AT_low_pc.
    // The low_pc attribute is required for DW_TAG_compile_units to set a base
    // address.
    //
    // Since DW_AT_ranges takes 4-byte DW_FROM_sec_offset value, we have to fill
    // in up to 12-bytes left after removal of low/high pc field from
    // .debug_info.
    //
    // To fill in the gap we use a variable length DW_FORM_udata encoding for
    // DW_AT_low_pc. We exploit the fact that the encoding can take an arbitrary
    // large size.
    if (AbbreviationDecl->findAttributeIndex(dwarf::DW_AT_low_pc) &&
        AbbreviationDecl->findAttributeIndex(dwarf::DW_AT_high_pc)) {
      uint32_t LowPCOffset = -1U;
      uint32_t HighPCOffset = -1U;
      DWARFFormValue LowPCFormValue =
          *DIE.find(dwarf::DW_AT_low_pc, &LowPCOffset);
      DWARFFormValue HighPCFormValue =
          *DIE.find(dwarf::DW_AT_high_pc, &HighPCOffset);

      if (LowPCFormValue.getForm() != dwarf::DW_FORM_addr ||
          (HighPCFormValue.getForm() != dwarf::DW_FORM_addr &&
           HighPCFormValue.getForm() != dwarf::DW_FORM_data8 &&
           HighPCFormValue.getForm() != dwarf::DW_FORM_data4)) {
        errs() << "BOLT-WARNING: unexpected form value. Cannot update DIE "
                 << "at offset 0x" << Twine::utohexstr(DIE.getOffset())
                 << "\n";
        return;
      }
      if (LowPCOffset == -1U || (LowPCOffset + 8 != HighPCOffset)) {
        errs() << "BOLT-WARNING: high_pc expected immediately after low_pc. "
               << "Cannot update DIE at offset 0x"
               << Twine::utohexstr(DIE.getOffset()) << '\n';
        return;
      }

      AbbrevPatcher->addAttributePatch(DIE.getDwarfUnit(),
                                       AbbrevCode,
                                       dwarf::DW_AT_low_pc,
                                       dwarf::DW_AT_ranges,
                                       dwarf::DW_FORM_sec_offset);
      AbbrevPatcher->addAttributePatch(DIE.getDwarfUnit(),
                                       AbbrevCode,
                                       dwarf::DW_AT_high_pc,
                                       dwarf::DW_AT_low_pc,
                                       dwarf::DW_FORM_udata);
      unsigned LowPCSize = 0;
      if (HighPCFormValue.getForm() == dwarf::DW_FORM_addr ||
          HighPCFormValue.getForm() == dwarf::DW_FORM_data8) {
        LowPCSize = 12;
      } else if (HighPCFormValue.getForm() == dwarf::DW_FORM_data4) {
        LowPCSize = 8;
      } else {
        llvm_unreachable("unexpected form");
      }
      DebugInfoPatcher->addLE32Patch(LowPCOffset, DebugRangesOffset);
      DebugInfoPatcher->addUDataPatch(LowPCOffset + 4, 0, LowPCSize);
    } else {
      if (opts::Verbosity >= 1) {
        errs() << "BOLT-WARNING: Cannot update ranges for DIE at offset 0x"
               << Twine::utohexstr(DIE.getOffset()) << '\n';
      }
    }
  }
}

void RewriteInstance::updateDebugLineInfoForNonSimpleFunctions() {
  for (auto &It : BinaryFunctions) {
    const auto &Function = It.second;

    if (Function.isSimple())
      continue;

    auto ULT = Function.getDWARFUnitLineTable();
    auto Unit = ULT.first;
    auto LineTable = ULT.second;

    if (!LineTable)
      continue; // nothing to update for this function

    std::vector<uint32_t> Results;
    MCSectionELF *FunctionSection =
        BC->Ctx->getELFSection(Function.getCodeSectionName(),
                               ELF::SHT_PROGBITS,
                               ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);

    uint64_t Address = It.first;
    if (LineTable->lookupAddressRange(Address, Function.getMaxSize(),
                                      Results)) {
      auto &OutputLineTable =
          BC->Ctx->getMCDwarfLineTable(Unit->getOffset()).getMCLineSections();
      for (auto RowIndex : Results) {
        const auto &Row = LineTable->Rows[RowIndex];
        BC->Ctx->setCurrentDwarfLoc(
            Row.File,
            Row.Line,
            Row.Column,
            (DWARF2_FLAG_IS_STMT * Row.IsStmt) |
            (DWARF2_FLAG_BASIC_BLOCK * Row.BasicBlock) |
            (DWARF2_FLAG_PROLOGUE_END * Row.PrologueEnd) |
            (DWARF2_FLAG_EPILOGUE_BEGIN * Row.EpilogueBegin),
            Row.Isa,
            Row.Discriminator,
            Row.Address);
        auto Loc = BC->Ctx->getCurrentDwarfLoc();
        BC->Ctx->clearDwarfLocSeen();
        OutputLineTable.addLineEntry(MCDwarfLineEntry{nullptr, Loc},
                                     FunctionSection);
      }
      // Add an empty entry past the end of the function
      // for end_sequence mark.
      BC->Ctx->setCurrentDwarfLoc(0, 0, 0, 0, 0, 0,
                                  Address + Function.getMaxSize());
      auto Loc = BC->Ctx->getCurrentDwarfLoc();
      BC->Ctx->clearDwarfLocSeen();
      OutputLineTable.addLineEntry(MCDwarfLineEntry{nullptr, Loc},
                                   FunctionSection);
    } else {
      DEBUG(dbgs() << "BOLT-DEBUG: Function " << Function
                   << " has no associated line number information.\n");
    }
  }
}

void RewriteInstance::updateLineTableOffsets() {
  const auto *LineSection =
    BC->Ctx->getObjectFileInfo()->getDwarfLineSection();
  auto CurrentFragment = LineSection->begin();
  uint32_t CurrentOffset = 0;
  uint32_t Offset = 0;

  // Line tables are stored in MCContext in ascending order of offset in the
  // output file, thus we can compute all table's offset by passing through
  // each fragment at most once, continuing from the last CU's beginning
  // instead of from the first fragment.
  for (const auto &CUIDLineTablePair : BC->Ctx->getMCDwarfLineTables()) {
    auto Label = CUIDLineTablePair.second.getLabel();
    if (!Label)
      continue;

    auto CUOffset = CUIDLineTablePair.first;
    if (CUOffset == -1U)
      continue;

    auto *CU = BC->DwCtx->getCompileUnitForOffset(CUOffset);
    assert(CU && "no CU found at offset");
    auto LTOffset =
      BC->DwCtx->getAttrFieldOffsetForUnit(CU, dwarf::DW_AT_stmt_list);
    if (!LTOffset)
      continue;

    auto Fragment = Label->getFragment();
    while (&*CurrentFragment != Fragment) {
      switch (CurrentFragment->getKind()) {
      case MCFragment::FT_Dwarf:
        Offset += cast<MCDwarfLineAddrFragment>(*CurrentFragment)
          .getContents().size() - CurrentOffset;
        break;
      case MCFragment::FT_Data:
        Offset += cast<MCDataFragment>(*CurrentFragment)
          .getContents().size() - CurrentOffset;
        break;
      default:
        llvm_unreachable(".debug_line section shouldn't contain other types "
                         "of fragments.");
      }
      ++CurrentFragment;
      CurrentOffset = 0;
    }

    Offset += Label->getOffset() - CurrentOffset;
    CurrentOffset = Label->getOffset();

    auto DbgInfoSection = BC->getUniqueSectionByName(".debug_info");
    assert(DbgInfoSection && ".debug_info section must exist");
    auto *Zero = BC->registerNameAtAddress("Zero", 0, 0, 0);
    DbgInfoSection->addRelocation(LTOffset,
                                  Zero,
                                  ELF::R_X86_64_32,
                                  Offset,
                                  0,
                                  /*Pending=*/true);
    // Set .debug_info as finalized so it won't be skipped over when
    // we process sections while writing out the new binary.  This ensures
    // that the pending relocations will be processed and not ignored.
    DbgInfoSection->setIsFinalized();

    DEBUG(dbgs() << "BOLT-DEBUG: CU " << CUIDLineTablePair.first
                << " has line table at " << Offset << "\n");
  }
}

void RewriteInstance::finalizeDebugSections() {
  // Skip .debug_aranges if we are re-generating .gdb_index.
  if (opts::KeepARanges || !GdbIndexSection) {
    SmallVector<char, 16> ARangesBuffer;
    raw_svector_ostream OS(ARangesBuffer);

    auto MAB = std::unique_ptr<MCAsmBackend>(BC->TheTarget->createMCAsmBackend(
        *BC->STI, *BC->MRI, MCTargetOptions()));
    auto Writer = std::unique_ptr<MCObjectWriter>(MAB->createObjectWriter(OS));

    RangesSectionsWriter->writeArangesSection(Writer.get());
    const auto &ARangesContents = OS.str();

    BC->registerOrUpdateNoteSection(".debug_aranges",
                                    copyByteArray(ARangesContents),
                                    ARangesContents.size());
  }

  auto RangesSectionContents = RangesSectionsWriter->finalize();
  BC->registerOrUpdateNoteSection(".debug_ranges",
                                  copyByteArray(*RangesSectionContents),
                                  RangesSectionContents->size());

  auto LocationListSectionContents = LocationListWriter->finalize();
  BC->registerOrUpdateNoteSection(".debug_loc",
                                  copyByteArray(*LocationListSectionContents),
                                  LocationListSectionContents->size());
}

void RewriteInstance::updateGdbIndexSection() {
  if (!GdbIndexSection)
    return;

  // See https://sourceware.org/gdb/onlinedocs/gdb/Index-Section-Format.html for
  // .gdb_index section format.

  StringRef GdbIndexContents = GdbIndexSection->getContents();

  const auto *Data = GdbIndexContents.data();

  // Parse the header.
  const auto Version = read32le(Data);
  if (Version != 7 && Version != 8) {
    errs() << "BOLT-ERROR: can only process .gdb_index versions 7 and 8\n";
    exit(1);
  }

  // Some .gdb_index generators use file offsets while others use section
  // offsets. Hence we can only rely on offsets relative to each other,
  // and ignore their absolute values.
  const auto CUListOffset = read32le(Data + 4);
  const auto CUTypesOffset = read32le(Data + 8);
  const auto AddressTableOffset = read32le(Data + 12);
  const auto SymbolTableOffset = read32le(Data + 16);
  const auto ConstantPoolOffset = read32le(Data + 20);
  Data += 24;

  // Map CUs offsets to indices and verify existing index table.
  std::map<uint32_t, uint32_t> OffsetToIndexMap;
  const auto CUListSize = CUTypesOffset - CUListOffset;
  const auto NumCUs = BC->DwCtx->getNumCompileUnits();
  if (CUListSize != NumCUs * 16) {
    errs() << "BOLT-ERROR: .gdb_index: CU count mismatch\n";
    exit(1);
  }
  for (unsigned Index = 0; Index < NumCUs; ++Index, Data += 16) {
    const auto *CU = BC->DwCtx->getCompileUnitAtIndex(Index);
    const auto Offset = read64le(Data);
    if (CU->getOffset() != Offset) {
      errs() << "BOLT-ERROR: .gdb_index CU offset mismatch\n";
      exit(1);
    }

    OffsetToIndexMap[Offset] = Index;
  }

  // Ignore old address table.
  const auto OldAddressTableSize = SymbolTableOffset - AddressTableOffset;
  // Move Data to the beginning of symbol table.
  Data += SymbolTableOffset - CUTypesOffset;

  // Calculate the size of the new address table.
  uint32_t NewAddressTableSize = 0;
  for (const auto &CURangesPair : RangesSectionsWriter->getCUAddressRanges()) {
    const auto &Ranges = CURangesPair.second;
    NewAddressTableSize += Ranges.size() * 20;
  }

  // Difference between old and new table (and section) sizes.
  // Could be negative.
  int32_t Delta = NewAddressTableSize - OldAddressTableSize;

  size_t NewGdbIndexSize = GdbIndexContents.size() + Delta;

  // Free'd by ExecutableFileMemoryManager.
  auto *NewGdbIndexContents = new uint8_t[NewGdbIndexSize];
  auto *Buffer = NewGdbIndexContents;

  write32le(Buffer, Version);
  write32le(Buffer + 4, CUListOffset);
  write32le(Buffer + 8, CUTypesOffset);
  write32le(Buffer + 12, AddressTableOffset);
  write32le(Buffer + 16, SymbolTableOffset + Delta);
  write32le(Buffer + 20, ConstantPoolOffset + Delta);
  Buffer += 24;

  // Copy over CU list and types CU list.
  memcpy(Buffer, GdbIndexContents.data() + 24,
         AddressTableOffset - CUListOffset);
  Buffer += AddressTableOffset - CUListOffset;

  // Generate new address table.
  for (const auto &CURangesPair : RangesSectionsWriter->getCUAddressRanges()) {
    const auto CUIndex = OffsetToIndexMap[CURangesPair.first];
    const auto &Ranges = CURangesPair.second;
    for (const auto &Range : Ranges) {
      write64le(Buffer, Range.LowPC);
      write64le(Buffer + 8, Range.HighPC);
      write32le(Buffer + 16, CUIndex);
      Buffer += 20;
    }
  }

  const auto TrailingSize =
    GdbIndexContents.data() + GdbIndexContents.size() - Data;
  assert(Buffer + TrailingSize == NewGdbIndexContents + NewGdbIndexSize &&
         "size calculation error");

  // Copy over the rest of the original data.
  memcpy(Buffer, Data, TrailingSize);

  // Register the new section.
  BC->registerOrUpdateNoteSection(".gdb_index",
                                  NewGdbIndexContents,
                                  NewGdbIndexSize);
}
