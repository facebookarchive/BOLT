//===- DebugData.cpp - Representation and writing of debugging information. ==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/DebugData.h"
#include "bolt/Core/BinaryBasicBlock.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Utils/Utils.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/LEB128.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>

#define DEBUG_TYPE "bolt-debug-info"

namespace opts {
extern llvm::cl::opt<unsigned> Verbosity;
}

namespace llvm {
namespace bolt {

const DebugLineTableRowRef DebugLineTableRowRef::NULL_ROW{0, 0};

namespace {

// Writes address ranges to Writer as pairs of 64-bit (address, size).
// If RelativeRange is true, assumes the address range to be written must be of
// the form (begin address, range size), otherwise (begin address, end address).
// Terminates the list by writing a pair of two zeroes.
// Returns the number of written bytes.
uint64_t writeAddressRanges(raw_svector_ostream &Stream,
                            const DebugAddressRangesVector &AddressRanges,
                            const bool WriteRelativeRanges = false) {
  for (const DebugAddressRange &Range : AddressRanges) {
    support::endian::write(Stream, Range.LowPC, support::little);
    support::endian::write(
        Stream, WriteRelativeRanges ? Range.HighPC - Range.LowPC : Range.HighPC,
        support::little);
  }
  // Finish with 0 entries.
  support::endian::write(Stream, 0ULL, support::little);
  support::endian::write(Stream, 0ULL, support::little);
  return AddressRanges.size() * 16 + 16;
}

} // namespace

DebugRangesSectionWriter::DebugRangesSectionWriter() {
  RangesBuffer = std::make_unique<DebugBufferVector>();
  RangesStream = std::make_unique<raw_svector_ostream>(*RangesBuffer);

  // Add an empty range as the first entry;
  SectionOffset +=
      writeAddressRanges(*RangesStream.get(), DebugAddressRangesVector{});
}

uint64_t DebugRangesSectionWriter::addRanges(
    DebugAddressRangesVector &&Ranges,
    std::map<DebugAddressRangesVector, uint64_t> &CachedRanges) {
  if (Ranges.empty())
    return getEmptyRangesOffset();

  const auto RI = CachedRanges.find(Ranges);
  if (RI != CachedRanges.end())
    return RI->second;

  const uint64_t EntryOffset = addRanges(Ranges);
  CachedRanges.emplace(std::move(Ranges), EntryOffset);

  return EntryOffset;
}

uint64_t
DebugRangesSectionWriter::addRanges(const DebugAddressRangesVector &Ranges) {
  if (Ranges.empty())
    return getEmptyRangesOffset();

  // Reading the SectionOffset and updating it should be atomic to guarantee
  // unique and correct offsets in patches.
  std::lock_guard<std::mutex> Lock(WriterMutex);
  const uint32_t EntryOffset = SectionOffset;
  SectionOffset += writeAddressRanges(*RangesStream.get(), Ranges);

  return EntryOffset;
}

uint64_t DebugRangesSectionWriter::getSectionOffset() {
  std::lock_guard<std::mutex> Lock(WriterMutex);
  return SectionOffset;
}

void DebugARangesSectionWriter::addCURanges(uint64_t CUOffset,
                                            DebugAddressRangesVector &&Ranges) {
  std::lock_guard<std::mutex> Lock(CUAddressRangesMutex);
  CUAddressRanges.emplace(CUOffset, std::move(Ranges));
}

void DebugARangesSectionWriter::writeARangesSection(
    raw_svector_ostream &RangesStream) const {
  // For reference on the format of the .debug_aranges section, see the DWARF4
  // specification, section 6.1.4 Lookup by Address
  // http://www.dwarfstd.org/doc/DWARF4.pdf
  for (const auto &CUOffsetAddressRangesPair : CUAddressRanges) {
    const uint64_t Offset = CUOffsetAddressRangesPair.first;
    const DebugAddressRangesVector &AddressRanges =
        CUOffsetAddressRangesPair.second;

    // Emit header.

    // Size of this set: 8 (size of the header) + 4 (padding after header)
    // + 2*sizeof(uint64_t) bytes for each of the ranges, plus an extra
    // pair of uint64_t's for the terminating, zero-length range.
    // Does not include size field itself.
    uint32_t Size = 8 + 4 + 2 * sizeof(uint64_t) * (AddressRanges.size() + 1);

    // Header field #1: set size.
    support::endian::write(RangesStream, Size, support::little);

    // Header field #2: version number, 2 as per the specification.
    support::endian::write(RangesStream, static_cast<uint16_t>(2),
                           support::little);

    // Header field #3: debug info offset of the correspondent compile unit.
    support::endian::write(RangesStream, static_cast<uint32_t>(Offset),
                           support::little);

    // Header field #4: address size.
    // 8 since we only write ELF64 binaries for now.
    RangesStream << char(8);

    // Header field #5: segment size of target architecture.
    RangesStream << char(0);

    // Padding before address table - 4 bytes in the 64-bit-pointer case.
    support::endian::write(RangesStream, static_cast<uint32_t>(0),
                           support::little);

    writeAddressRanges(RangesStream, AddressRanges, true);
  }
}

DebugAddrWriter::DebugAddrWriter(BinaryContext *Bc) { BC = Bc; }

void DebugAddrWriter::AddressForDWOCU::dump() {
  std::vector<IndexAddressPair> SortedMap(indexToAddressBegin(),
                                          indexToAdddessEnd());
  // Sorting address in increasing order of indices.
  std::sort(SortedMap.begin(), SortedMap.end(),
            [](const IndexAddressPair &A, const IndexAddressPair &B) {
              return A.first < B.first;
            });
  for (auto &Pair : SortedMap)
    dbgs() << Twine::utohexstr(Pair.second) << "\t" << Pair.first << "\n";
}
uint32_t DebugAddrWriter::getIndexFromAddress(uint64_t Address,
                                              uint64_t DWOId) {
  if (!AddressMaps.count(DWOId))
    AddressMaps[DWOId] = AddressForDWOCU();

  AddressForDWOCU &Map = AddressMaps[DWOId];
  auto Entry = Map.find(Address);
  if (Entry == Map.end()) {
    auto Index = Map.getNextIndex();
    Entry = Map.insert(Address, Index).first;
  }
  return Entry->second;
}

// Case1) Address is not in map insert in to AddresToIndex and IndexToAddres
// Case2) Address is in the map but Index is higher or equal. Need to update
// IndexToAddrss. Case3) Address is in the map but Index is lower. Need to
// update AddressToIndex and IndexToAddress
void DebugAddrWriter::addIndexAddress(uint64_t Address, uint32_t Index,
                                      uint64_t DWOId) {
  AddressForDWOCU &Map = AddressMaps[DWOId];
  auto Entry = Map.find(Address);
  if (Entry != Map.end()) {
    if (Entry->second > Index)
      Map.updateAddressToIndex(Address, Index);
    Map.updateIndexToAddrss(Address, Index);
  } else
    Map.insert(Address, Index);
}

AddressSectionBuffer DebugAddrWriter::finalize() {
  // Need to layout all sections within .debug_addr
  // Within each section sort Address by index.
  AddressSectionBuffer Buffer;
  raw_svector_ostream AddressStream(Buffer);
  for (std::unique_ptr<DWARFUnit> &CU : BC->DwCtx->compile_units()) {
    Optional<uint64_t> DWOId = CU->getDWOId();
    // Handling the case wehre debug information is a mix of Debug fission and
    // monolitic.
    if (!DWOId)
      continue;
    auto AM = AddressMaps.find(*DWOId);
    // Adding to map even if it did not contribute to .debug_addr.
    // The Skeleton CU will still have DW_AT_GNU_addr_base.
    DWOIdToOffsetMap[*DWOId] = Buffer.size();
    // If does not exist this CUs DWO section didn't contribute to .debug_addr.
    if (AM == AddressMaps.end())
      continue;
    std::vector<IndexAddressPair> SortedMap(AM->second.indexToAddressBegin(),
                                            AM->second.indexToAdddessEnd());
    // Sorting address in increasing order of indices.
    std::sort(SortedMap.begin(), SortedMap.end(),
              [](const IndexAddressPair &A, const IndexAddressPair &B) {
                return A.first < B.first;
              });

    uint8_t AddrSize = CU->getAddressByteSize();
    uint32_t Counter = 0;
    auto WriteAddress = [&](uint64_t Address) -> void {
      ++Counter;
      switch (AddrSize) {
      default:
        assert(false && "Address Size is invalid.");
        break;
      case 4:
        support::endian::write(AddressStream, static_cast<uint32_t>(Address),
                               support::little);
        break;
      case 8:
        support::endian::write(AddressStream, Address, support::little);
        break;
      }
    };

    for (const IndexAddressPair &Val : SortedMap) {
      while (Val.first > Counter)
        WriteAddress(0);
      WriteAddress(Val.second);
    }
  }

  return Buffer;
}

uint64_t DebugAddrWriter::getOffset(uint64_t DWOId) {
  auto Iter = DWOIdToOffsetMap.find(DWOId);
  assert(Iter != DWOIdToOffsetMap.end() &&
         "Offset in to.debug_addr was not found for DWO ID.");
  return Iter->second;
}

DebugLocWriter::DebugLocWriter(BinaryContext *BC) {
  LocBuffer = std::make_unique<DebugBufferVector>();
  LocStream = std::make_unique<raw_svector_ostream>(*LocBuffer);
}

void DebugLocWriter::addList(uint64_t AttrOffset,
                             DebugLocationsVector &&LocList) {
  if (LocList.empty()) {
    EmptyAttrLists.push_back(AttrOffset);
    return;
  }
  // Since there is a separate DebugLocWriter for each thread,
  // we don't need a lock to read the SectionOffset and update it.
  const uint32_t EntryOffset = SectionOffset;

  for (const DebugLocationEntry &Entry : LocList) {
    support::endian::write(*LocStream, static_cast<uint64_t>(Entry.LowPC),
                           support::little);
    support::endian::write(*LocStream, static_cast<uint64_t>(Entry.HighPC),
                           support::little);
    support::endian::write(*LocStream, static_cast<uint16_t>(Entry.Expr.size()),
                           support::little);
    *LocStream << StringRef(reinterpret_cast<const char *>(Entry.Expr.data()),
                            Entry.Expr.size());
    SectionOffset += 2 * 8 + 2 + Entry.Expr.size();
  }
  LocStream->write_zeros(16);
  SectionOffset += 16;
  LocListDebugInfoPatches.push_back({AttrOffset, EntryOffset});
}

void DebugLoclistWriter::addList(uint64_t AttrOffset,
                                 DebugLocationsVector &&LocList) {
  Patches.push_back({AttrOffset, std::move(LocList)});
}

std::unique_ptr<DebugBufferVector> DebugLocWriter::getBuffer() {
  return std::move(LocBuffer);
}

// DWARF 4: 2.6.2
void DebugLocWriter::finalize(uint64_t SectionOffset,
                              SimpleBinaryPatcher &DebugInfoPatcher) {
  for (const auto LocListDebugInfoPatchType : LocListDebugInfoPatches) {
    uint64_t Offset = SectionOffset + LocListDebugInfoPatchType.LocListOffset;
    DebugInfoPatcher.addLE32Patch(LocListDebugInfoPatchType.DebugInfoAttrOffset,
                                  Offset);
  }

  for (uint64_t DebugInfoAttrOffset : EmptyAttrLists)
    DebugInfoPatcher.addLE32Patch(DebugInfoAttrOffset,
                                  DebugLocWriter::EmptyListOffset);
}

void DebugLoclistWriter::finalize(uint64_t SectionOffset,
                                  SimpleBinaryPatcher &DebugInfoPatcher) {
  for (LocPatch &Patch : Patches) {
    if (Patch.LocList.empty()) {
      DebugInfoPatcher.addLE32Patch(Patch.AttrOffset,
                                    DebugLocWriter::EmptyListOffset);
      continue;
    }
    const uint32_t EntryOffset = LocBuffer->size();
    for (const DebugLocationEntry &Entry : Patch.LocList) {
      support::endian::write(*LocStream,
                             static_cast<uint8_t>(dwarf::DW_LLE_startx_length),
                             support::little);
      uint32_t Index = AddrWriter->getIndexFromAddress(Entry.LowPC, DWOId);
      encodeULEB128(Index, *LocStream);

      // TODO: Support DWARF5
      support::endian::write(*LocStream,
                             static_cast<uint32_t>(Entry.HighPC - Entry.LowPC),
                             support::little);
      support::endian::write(*LocStream,
                             static_cast<uint16_t>(Entry.Expr.size()),
                             support::little);
      *LocStream << StringRef(reinterpret_cast<const char *>(Entry.Expr.data()),
                              Entry.Expr.size());
    }
    support::endian::write(*LocStream,
                           static_cast<uint8_t>(dwarf::DW_LLE_end_of_list),
                           support::little);
    DebugInfoPatcher.addLE32Patch(Patch.AttrOffset, EntryOffset);
    clearList(Patch.LocList);
  }
  clearList(Patches);
}

DebugAddrWriter *DebugLoclistWriter::AddrWriter = nullptr;

void SimpleBinaryPatcher::addBinaryPatch(uint32_t Offset,
                                         const std::string &NewValue) {
  Patches.emplace_back(Offset, NewValue);
}

void SimpleBinaryPatcher::addBytePatch(uint32_t Offset, uint8_t Value) {
  Patches.emplace_back(Offset, std::string(1, Value));
}

void SimpleBinaryPatcher::addLEPatch(uint32_t Offset, uint64_t NewValue,
                                     size_t ByteSize) {
  std::string LE64(ByteSize, 0);
  for (size_t I = 0; I < ByteSize; ++I) {
    LE64[I] = NewValue & 0xff;
    NewValue >>= 8;
  }
  Patches.emplace_back(Offset, LE64);
}

void SimpleBinaryPatcher::addUDataPatch(uint32_t Offset, uint64_t Value,
                                        uint64_t Size) {
  std::string Buff;
  raw_string_ostream OS(Buff);
  encodeULEB128(Value, OS, Size);

  Patches.emplace_back(Offset, OS.str());
}

void SimpleBinaryPatcher::addLE64Patch(uint32_t Offset, uint64_t NewValue) {
  addLEPatch(Offset, NewValue, 8);
}

void SimpleBinaryPatcher::addLE32Patch(uint32_t Offset, uint32_t NewValue) {
  addLEPatch(Offset, NewValue, 4);
}

void SimpleBinaryPatcher::patchBinary(std::string &BinaryContents,
                                      uint32_t DWPOffset = 0) {
  for (const auto &Patch : Patches) {
    uint32_t Offset = Patch.first - DWPOffset;
    const std::string &ByteSequence = Patch.second;
    assert(Offset + ByteSequence.size() <= BinaryContents.size() &&
           "Applied patch runs over binary size.");
    for (uint64_t I = 0, Size = ByteSequence.size(); I < Size; ++I) {
      BinaryContents[Offset + I] = ByteSequence[I];
    }
  }
}

void DebugStrWriter::create() {
  StrBuffer = std::make_unique<DebugStrBufferVector>();
  StrStream = std::make_unique<raw_svector_ostream>(*StrBuffer);
}

void DebugStrWriter::initialize() {
  auto StrSection = BC->DwCtx->getDWARFObj().getStrSection();
  (*StrStream) << StrSection;
}

uint32_t DebugStrWriter::addString(StringRef Str) {
  if (StrBuffer->empty())
    initialize();
  auto Offset = StrBuffer->size();
  (*StrStream) << Str;
  StrStream->write_zeros(1);
  return Offset;
}

void DebugAbbrevWriter::addUnitAbbreviations(DWARFUnit &Unit) {
  const DWARFAbbreviationDeclarationSet *Abbrevs = Unit.getAbbreviations();
  if (!Abbrevs)
    return;

  // Multiple units may share the same abbreviations. Only add abbreviations
  // for the first unit and reuse them.
  const uint64_t AbbrevOffset = Unit.getAbbreviationsOffset();
  if (UnitsAbbrevData.find(AbbrevOffset) != UnitsAbbrevData.end())
    return;

  AbbrevData &UnitData = UnitsAbbrevData[AbbrevOffset];
  UnitData.Buffer = std::make_unique<DebugBufferVector>();
  UnitData.Stream = std::make_unique<raw_svector_ostream>(*UnitData.Buffer);

  const PatchesTy &UnitPatches = Patches[&Unit];

  raw_svector_ostream &OS = *UnitData.Stream.get();

  // Take a fast path if there are no patches to apply. Simply copy the original
  // contents.
  if (UnitPatches.empty()) {
    StringRef AbbrevSectionContents =
        Unit.isDWOUnit() ? Unit.getContext().getDWARFObj().getAbbrevDWOSection()
                         : Unit.getContext().getDWARFObj().getAbbrevSection();
    StringRef AbbrevContents;

    const DWARFUnitIndex &CUIndex = Unit.getContext().getCUIndex();
    if (!CUIndex.getRows().empty()) {
      // Handle DWP section contribution.
      const DWARFUnitIndex::Entry *DWOEntry =
          CUIndex.getFromHash(*Unit.getDWOId());
      if (!DWOEntry)
        return;

      const DWARFUnitIndex::Entry::SectionContribution *DWOContrubution =
          DWOEntry->getContribution(DWARFSectionKind::DW_SECT_ABBREV);
      AbbrevContents = AbbrevSectionContents.substr(DWOContrubution->Offset,
                                                    DWOContrubution->Length);
    } else if (!Unit.isDWOUnit()) {
      const uint64_t StartOffset = Unit.getAbbreviationsOffset();

      // We know where the unit's abbreviation set starts, but not where it ends
      // as such data is not readily available. Hence, we have to build a sorted
      // list of start addresses and find the next starting address to determine
      // the set boundaries.
      //
      // FIXME: if we had a full access to DWARFDebugAbbrev::AbbrDeclSets
      // we wouldn't have to build our own sorted list for the quick lookup.
      if (AbbrevSetOffsets.empty()) {
        llvm::for_each(
            *Unit.getContext().getDebugAbbrev(),
            [&](const std::pair<uint64_t, DWARFAbbreviationDeclarationSet> &P) {
              AbbrevSetOffsets.push_back(P.first);
            });
        llvm::sort(AbbrevSetOffsets);
      }
      auto It = llvm::upper_bound(AbbrevSetOffsets, StartOffset);
      const uint64_t EndOffset =
          It == AbbrevSetOffsets.end() ? AbbrevSectionContents.size() : *It;
      AbbrevContents = AbbrevSectionContents.slice(StartOffset, EndOffset);
    } else {
      // For DWO unit outside of DWP, we expect the entire section to hold
      // abbreviations for this unit only.
      AbbrevContents = AbbrevSectionContents;
    }

    OS.reserveExtraSpace(AbbrevContents.size());
    OS << AbbrevContents;

    return;
  }

  for (auto I = Abbrevs->begin(), E = Abbrevs->end(); I != E; ++I) {
    const DWARFAbbreviationDeclaration &Abbrev = *I;
    auto Patch = UnitPatches.find(&Abbrev);

    encodeULEB128(Abbrev.getCode(), OS);
    encodeULEB128(Abbrev.getTag(), OS);
    encodeULEB128(Abbrev.hasChildren(), OS);
    for (const DWARFAbbreviationDeclaration::AttributeSpec &AttrSpec :
         Abbrev.attributes()) {
      if (Patch != UnitPatches.end()) {
        bool Patched = false;
        // Patches added later take a precedence over earlier ones.
        for (auto I = Patch->second.rbegin(), E = Patch->second.rend(); I != E;
             ++I) {
          if (I->OldAttr != AttrSpec.Attr)
            continue;

          encodeULEB128(I->NewAttr, OS);
          encodeULEB128(I->NewAttrForm, OS);
          Patched = true;
          break;
        }
        if (Patched)
          continue;
      }

      encodeULEB128(AttrSpec.Attr, OS);
      encodeULEB128(AttrSpec.Form, OS);
      if (AttrSpec.isImplicitConst())
        encodeSLEB128(AttrSpec.getImplicitConstValue(), OS);
    }

    encodeULEB128(0, OS);
    encodeULEB128(0, OS);
  }
  encodeULEB128(0, OS);
}

std::unique_ptr<DebugBufferVector> DebugAbbrevWriter::finalize() {
  if (DWOId) {
    // We expect abbrev_offset to always be zero for DWO units as there
    // should be one CU per DWO, and TUs should share the same abbreviation
    // set with the CU.
    // For DWP AbbreviationsOffset is an Abbrev contribution in the DWP file, so
    // can be none zero. Thus we are skipping the check for DWP.
    bool IsDWP = !Context.getCUIndex().getRows().empty();
    if (!IsDWP) {
      for (const std::unique_ptr<DWARFUnit> &Unit : Context.dwo_units()) {
        if (Unit->getAbbreviationsOffset() != 0) {
          errs() << "BOLT-ERROR: detected DWO unit with non-zero abbr_offset. "
                    "Unable to update debug info.\n";
          exit(1);
        }
      }
    }

    // Issue abbreviations for the DWO CU only.
    addUnitAbbreviations(*Context.getDWOCompileUnitForHash(*DWOId));
  } else {
    // Add abbreviations from compile and type non-DWO units.
    for (const std::unique_ptr<DWARFUnit> &Unit : Context.normal_units())
      addUnitAbbreviations(*Unit);
  }

  DebugBufferVector ReturnBuffer;

  // Pre-calculate the total size of abbrev section.
  uint64_t Size = 0;
  for (const auto &KV : UnitsAbbrevData) {
    const AbbrevData &UnitData = KV.second;
    Size += UnitData.Buffer->size();
  }
  ReturnBuffer.reserve(Size);

  uint64_t Pos = 0;
  for (auto &KV : UnitsAbbrevData) {
    AbbrevData &UnitData = KV.second;
    ReturnBuffer.append(*UnitData.Buffer);
    UnitData.Offset = Pos;
    Pos += UnitData.Buffer->size();

    UnitData.Buffer.reset();
    UnitData.Stream.reset();
  }

  return std::make_unique<DebugBufferVector>(ReturnBuffer);
}

static void emitDwarfSetLineAddrAbs(MCStreamer &OS,
                                    MCDwarfLineTableParams Params,
                                    int64_t LineDelta, uint64_t Address,
                                    int PointerSize) {
  // emit the sequence to set the address
  OS.emitIntValue(dwarf::DW_LNS_extended_op, 1);
  OS.emitULEB128IntValue(PointerSize + 1);
  OS.emitIntValue(dwarf::DW_LNE_set_address, 1);
  OS.emitIntValue(Address, PointerSize);

  // emit the sequence for the LineDelta (from 1) and a zero address delta.
  MCDwarfLineAddr::Emit(&OS, Params, LineDelta, 0);
}

static inline void emitBinaryDwarfLineTable(
    MCStreamer *MCOS, MCDwarfLineTableParams Params,
    const DWARFDebugLine::LineTable *Table,
    const std::vector<DwarfLineTable::RowSequence> &InputSequences) {
  if (InputSequences.empty())
    return;

  constexpr uint64_t InvalidAddress = UINT64_MAX;
  unsigned FileNum = 1;
  unsigned LastLine = 1;
  unsigned Column = 0;
  unsigned Flags = DWARF2_LINE_DEFAULT_IS_STMT ? DWARF2_FLAG_IS_STMT : 0;
  unsigned Isa = 0;
  unsigned Discriminator = 0;
  uint64_t LastAddress = InvalidAddress;
  uint64_t PrevEndOfSequence = InvalidAddress;
  const MCAsmInfo *AsmInfo = MCOS->getContext().getAsmInfo();

  auto emitEndOfSequence = [&](uint64_t Address) {
    MCDwarfLineAddr::Emit(MCOS, Params, INT64_MAX, Address - LastAddress);
    FileNum = 1;
    LastLine = 1;
    Column = 0;
    Flags = DWARF2_LINE_DEFAULT_IS_STMT ? DWARF2_FLAG_IS_STMT : 0;
    Isa = 0;
    Discriminator = 0;
    LastAddress = InvalidAddress;
  };

  for (const DwarfLineTable::RowSequence &Sequence : InputSequences) {
    const uint64_t SequenceStart =
        Table->Rows[Sequence.FirstIndex].Address.Address;

    // Check if we need to mark the end of the sequence.
    if (PrevEndOfSequence != InvalidAddress && LastAddress != InvalidAddress &&
        PrevEndOfSequence != SequenceStart) {
      emitEndOfSequence(PrevEndOfSequence);
    }

    for (uint32_t RowIndex = Sequence.FirstIndex;
         RowIndex <= Sequence.LastIndex; ++RowIndex) {
      const DWARFDebugLine::Row &Row = Table->Rows[RowIndex];
      int64_t LineDelta = static_cast<int64_t>(Row.Line) - LastLine;
      const uint64_t Address = Row.Address.Address;

      if (FileNum != Row.File) {
        FileNum = Row.File;
        MCOS->emitInt8(dwarf::DW_LNS_set_file);
        MCOS->emitULEB128IntValue(FileNum);
      }
      if (Column != Row.Column) {
        Column = Row.Column;
        MCOS->emitInt8(dwarf::DW_LNS_set_column);
        MCOS->emitULEB128IntValue(Column);
      }
      if (Discriminator != Row.Discriminator &&
          MCOS->getContext().getDwarfVersion() >= 4) {
        Discriminator = Row.Discriminator;
        unsigned Size = getULEB128Size(Discriminator);
        MCOS->emitInt8(dwarf::DW_LNS_extended_op);
        MCOS->emitULEB128IntValue(Size + 1);
        MCOS->emitInt8(dwarf::DW_LNE_set_discriminator);
        MCOS->emitULEB128IntValue(Discriminator);
      }
      if (Isa != Row.Isa) {
        Isa = Row.Isa;
        MCOS->emitInt8(dwarf::DW_LNS_set_isa);
        MCOS->emitULEB128IntValue(Isa);
      }
      if (Row.IsStmt != Flags) {
        Flags = Row.IsStmt;
        MCOS->emitInt8(dwarf::DW_LNS_negate_stmt);
      }
      if (Row.BasicBlock)
        MCOS->emitInt8(dwarf::DW_LNS_set_basic_block);
      if (Row.PrologueEnd)
        MCOS->emitInt8(dwarf::DW_LNS_set_prologue_end);
      if (Row.EpilogueBegin)
        MCOS->emitInt8(dwarf::DW_LNS_set_epilogue_begin);

      // The end of the sequence is not normal in the middle of the input
      // sequence, but could happen, e.g. for assembly code.
      if (Row.EndSequence) {
        emitEndOfSequence(Address);
      } else {
        if (LastAddress == InvalidAddress)
          emitDwarfSetLineAddrAbs(*MCOS, Params, LineDelta, Address,
                                  AsmInfo->getCodePointerSize());
        else
          MCDwarfLineAddr::Emit(MCOS, Params, LineDelta, Address - LastAddress);

        LastAddress = Address;
        LastLine = Row.Line;
      }

      Discriminator = 0;
    }
    PrevEndOfSequence = Sequence.EndAddress;
  }

  // Finish with the end of the sequence.
  if (LastAddress != InvalidAddress)
    emitEndOfSequence(PrevEndOfSequence);
}

// This function is similar to the one from MCDwarfLineTable, except it handles
// end-of-sequence entries differently by utilizing line entries with
// DWARF2_FLAG_END_SEQUENCE flag.
static inline void emitDwarfLineTable(
    MCStreamer *MCOS, MCSection *Section,
    const MCLineSection::MCDwarfLineEntryCollection &LineEntries) {
  unsigned FileNum = 1;
  unsigned LastLine = 1;
  unsigned Column = 0;
  unsigned Flags = DWARF2_LINE_DEFAULT_IS_STMT ? DWARF2_FLAG_IS_STMT : 0;
  unsigned Isa = 0;
  unsigned Discriminator = 0;
  MCSymbol *LastLabel = nullptr;
  const MCAsmInfo *AsmInfo = MCOS->getContext().getAsmInfo();

  // Loop through each MCDwarfLineEntry and encode the dwarf line number table.
  for (const MCDwarfLineEntry &LineEntry : LineEntries) {
    if (LineEntry.getFlags() & DWARF2_FLAG_END_SEQUENCE) {
      MCOS->emitDwarfAdvanceLineAddr(INT64_MAX, LastLabel, LineEntry.getLabel(),
                                     AsmInfo->getCodePointerSize());
      FileNum = 1;
      LastLine = 1;
      Column = 0;
      Flags = DWARF2_LINE_DEFAULT_IS_STMT ? DWARF2_FLAG_IS_STMT : 0;
      Isa = 0;
      Discriminator = 0;
      LastLabel = nullptr;
      continue;
    }

    int64_t LineDelta = static_cast<int64_t>(LineEntry.getLine()) - LastLine;

    if (FileNum != LineEntry.getFileNum()) {
      FileNum = LineEntry.getFileNum();
      MCOS->emitInt8(dwarf::DW_LNS_set_file);
      MCOS->emitULEB128IntValue(FileNum);
    }
    if (Column != LineEntry.getColumn()) {
      Column = LineEntry.getColumn();
      MCOS->emitInt8(dwarf::DW_LNS_set_column);
      MCOS->emitULEB128IntValue(Column);
    }
    if (Discriminator != LineEntry.getDiscriminator() &&
        MCOS->getContext().getDwarfVersion() >= 4) {
      Discriminator = LineEntry.getDiscriminator();
      unsigned Size = getULEB128Size(Discriminator);
      MCOS->emitInt8(dwarf::DW_LNS_extended_op);
      MCOS->emitULEB128IntValue(Size + 1);
      MCOS->emitInt8(dwarf::DW_LNE_set_discriminator);
      MCOS->emitULEB128IntValue(Discriminator);
    }
    if (Isa != LineEntry.getIsa()) {
      Isa = LineEntry.getIsa();
      MCOS->emitInt8(dwarf::DW_LNS_set_isa);
      MCOS->emitULEB128IntValue(Isa);
    }
    if ((LineEntry.getFlags() ^ Flags) & DWARF2_FLAG_IS_STMT) {
      Flags = LineEntry.getFlags();
      MCOS->emitInt8(dwarf::DW_LNS_negate_stmt);
    }
    if (LineEntry.getFlags() & DWARF2_FLAG_BASIC_BLOCK)
      MCOS->emitInt8(dwarf::DW_LNS_set_basic_block);
    if (LineEntry.getFlags() & DWARF2_FLAG_PROLOGUE_END)
      MCOS->emitInt8(dwarf::DW_LNS_set_prologue_end);
    if (LineEntry.getFlags() & DWARF2_FLAG_EPILOGUE_BEGIN)
      MCOS->emitInt8(dwarf::DW_LNS_set_epilogue_begin);

    MCSymbol *Label = LineEntry.getLabel();

    // At this point we want to emit/create the sequence to encode the delta
    // in line numbers and the increment of the address from the previous
    // Label and the current Label.
    MCOS->emitDwarfAdvanceLineAddr(LineDelta, LastLabel, Label,
                                   AsmInfo->getCodePointerSize());
    Discriminator = 0;
    LastLine = LineEntry.getLine();
    LastLabel = Label;
  }

  assert(LastLabel == nullptr && "end of sequence expected");
}

void DwarfLineTable::emitCU(MCStreamer *MCOS, MCDwarfLineTableParams Params,
                            Optional<MCDwarfLineStr> &LineStr,
                            BinaryContext &BC) const {
  if (!RawData.empty()) {
    assert(MCLineSections.getMCLineEntries().empty() &&
           InputSequences.empty() &&
           "cannot combine raw data with new line entries");
    MCOS->emitLabel(getLabel());
    MCOS->emitBytes(RawData);

    // Emit fake relocation for RuntimeDyld to always allocate the section.
    //
    // FIXME: remove this once RuntimeDyld stops skipping allocatable sections
    //        without relocations.
    MCOS->emitRelocDirective(
        *MCConstantExpr::create(0, *BC.Ctx), "BFD_RELOC_NONE",
        MCSymbolRefExpr::create(getLabel(), *BC.Ctx), SMLoc(), *BC.STI);

    return;
  }

  MCSymbol *LineEndSym = Header.Emit(MCOS, Params, LineStr).second;

  // Put out the line tables.
  for (const auto &LineSec : MCLineSections.getMCLineEntries())
    emitDwarfLineTable(MCOS, LineSec.first, LineSec.second);

  // Emit line tables for the original code.
  emitBinaryDwarfLineTable(MCOS, Params, InputTable, InputSequences);

  // This is the end of the section, so set the value of the symbol at the end
  // of this section (that was used in a previous expression).
  MCOS->emitLabel(LineEndSym);
}

void DwarfLineTable::emit(BinaryContext &BC, MCStreamer &Streamer) {
  MCAssembler &Assembler =
      static_cast<MCObjectStreamer *>(&Streamer)->getAssembler();

  MCDwarfLineTableParams Params = Assembler.getDWARFLinetableParams();

  auto &LineTables = BC.getDwarfLineTables();

  // Bail out early so we don't switch to the debug_line section needlessly and
  // in doing so create an unnecessary (if empty) section.
  if (LineTables.empty())
    return;

  // In a v5 non-split line table, put the strings in a separate section.
  Optional<MCDwarfLineStr> LineStr(None);
  if (BC.Ctx->getDwarfVersion() >= 5)
    LineStr = MCDwarfLineStr(*BC.Ctx);

  // Switch to the section where the table will be emitted into.
  Streamer.SwitchSection(BC.MOFI->getDwarfLineSection());

  // Handle the rest of the Compile Units.
  for (auto &CUIDTablePair : LineTables) {
    CUIDTablePair.second.emitCU(&Streamer, Params, LineStr, BC);
  }
}

} // namespace bolt
} // namespace llvm
