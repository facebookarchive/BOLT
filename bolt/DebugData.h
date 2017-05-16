//===-- DebugData.h - Representation and writing of debugging information. -==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Classes that represent and serialize DWARF-related entities.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_DEBUG_DATA_H
#define LLVM_TOOLS_LLVM_BOLT_DEBUG_DATA_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "BinaryBasicBlock.h"

namespace llvm {

class DWARFCompileUnit;
class DWARFDebugInfoEntryMinimal;
class MCObjectWriter;

namespace bolt {

class BinaryContext;
class BasicBlockTable;
class BinaryBasicBlock;
class BinaryFunction;

/// Eeferences a row in a DWARFDebugLine::LineTable by the DWARF
/// Context index of the DWARF Compile Unit that owns the Line Table and the row
/// index. This is tied to our IR during disassembly so that we can later update
/// .debug_line information. RowIndex has a base of 1, which means a RowIndex
/// of 1 maps to the first row of the line table and a RowIndex of 0 is invalid.
struct DebugLineTableRowRef {
  uint32_t DwCompileUnitIndex;
  uint32_t RowIndex;

  const static DebugLineTableRowRef NULL_ROW;

  bool operator==(const DebugLineTableRowRef &Rhs) const {
    return DwCompileUnitIndex == Rhs.DwCompileUnitIndex &&
      RowIndex == Rhs.RowIndex;
  }

  bool operator!=(const DebugLineTableRowRef &Rhs) const {
    return !(*this == Rhs);
  }

  static DebugLineTableRowRef fromSMLoc(const SMLoc &Loc) {
    union {
      decltype(Loc.getPointer()) Ptr;
      DebugLineTableRowRef Ref;
    } U;
    U.Ptr = Loc.getPointer();
    return U.Ref;
  }

  SMLoc toSMLoc() const {
    union {
      decltype(SMLoc().getPointer()) Ptr;
      DebugLineTableRowRef Ref;
    } U;
    U.Ref = *this;
    return SMLoc::getFromPointer(U.Ptr);
  }
};

/// Serializes the .debug_ranges and .debug_aranges DWARF sections.
class DebugRangesSectionsWriter {
public:
  DebugRangesSectionsWriter(BinaryContext *BC);

  /// Add ranges for CU matching \p CUOffset and return offset into section.
  uint64_t addCURanges(uint64_t CUOffset, DWARFAddressRangesVector &&Ranges);

  /// Add ranges with caching for \p Function.
  uint64_t addRanges(const BinaryFunction *Function,
                     DWARFAddressRangesVector &&Ranges);

  /// Add ranges and return offset into section.
  uint64_t addRanges(const DWARFAddressRangesVector &Ranges);

  /// Writes .debug_aranges with the added ranges to the MCObjectWriter.
  void writeArangesSection(MCObjectWriter *Writer) const;

  /// Resets the writer to a clear state.
  void reset() {
    CUAddressRanges.clear();
  }

  /// Returns an offset of an empty address ranges list that is always written
  /// to .debug_ranges
  uint64_t getEmptyRangesOffset() const { return EmptyRangesOffset; }

  /// Map DWARFCompileUnit index to ranges.
  using CUAddressRangesType = std::map<uint64_t, DWARFAddressRangesVector>;

  /// Return ranges for a given CU.
  const CUAddressRangesType &getCUAddressRanges() const {
    return CUAddressRanges;
  }

  SmallVectorImpl<char> *finalize() {
    return RangesBuffer.release();
  }

private:
  std::unique_ptr<SmallVector<char, 16>> RangesBuffer;

  std::unique_ptr<raw_svector_ostream> RangesStream;

  std::unique_ptr<MCObjectWriter> Writer;

  /// Current offset in the section (updated as new entries are written).
  /// Starts with 16 since the first 16 bytes are reserved for an empty range.
  uint32_t SectionOffset{0};

  /// Map from compile unit offset to the list of address intervals that belong
  /// to that compile unit. Each interval is a pair
  /// (first address, interval size).
  CUAddressRangesType CUAddressRanges;

  /// Offset of an empty address ranges list.
  static constexpr uint64_t EmptyRangesOffset{0};

  /// Cached used for de-duplicating entries for the same function.
  std::map<DWARFAddressRangesVector, uint64_t> CachedRanges;
};

/// Serializes the .debug_loc DWARF section with LocationLists.
class DebugLocWriter {
public:
  DebugLocWriter(BinaryContext *BC);

  uint64_t addList(const DWARFDebugLoc::LocationList &LocList);

  uint64_t getEmptyListOffset() const { return EmptyListOffset; }

  SmallVectorImpl<char> *finalize() {
    return LocBuffer.release();
  }

private:
  std::unique_ptr<SmallVector<char, 16>> LocBuffer;

  std::unique_ptr<raw_svector_ostream> LocStream;

  std::unique_ptr<MCObjectWriter> Writer;

  /// Offset of an empty location list.
  static uint64_t const EmptyListOffset = 0;

  /// Current offset in the section (updated as new entries are written).
  /// Starts with 16 since the first 16 bytes are reserved for an empty range.
  uint32_t SectionOffset{0};
};

/// Abstract interface for classes that apply modifications to a binary string.
class BinaryPatcher {
public:
  virtual ~BinaryPatcher() {}
  /// Applies in-place modifications to the binary string \p BinaryContents .
  virtual void patchBinary(std::string &BinaryContents) = 0;
};

/// Applies simple modifications to a binary string, such as directly replacing
/// the contents of a certain portion with a string or an integer.
class SimpleBinaryPatcher : public BinaryPatcher {
private:
  std::vector<std::pair<uint32_t, std::string>> Patches;

  /// Adds a patch to replace the contents of \p ByteSize bytes with the integer
  /// \p NewValue encoded in little-endian, with the least-significant byte
  /// being written at the offset \p Offset .
  void addLEPatch(uint32_t Offset, uint64_t NewValue, size_t ByteSize);

public:
  ~SimpleBinaryPatcher() {}

  /// Adds a patch to replace the contents of the binary string starting at the
  /// specified \p Offset with the string \p NewValue.
  void addBinaryPatch(uint32_t Offset, const std::string &NewValue);

  /// Adds a patch to replace the contents of a single byte of the string, at
  /// the offset \p Offset, with the value \Value .
  void addBytePatch(uint32_t Offset, uint8_t Value);

  /// Adds a patch to put the integer \p NewValue encoded as a 64-bit
  /// little-endian value at offset \p Offset.
  void addLE64Patch(uint32_t Offset, uint64_t NewValue);

  /// Adds a patch to put the integer \p NewValue encoded as a 32-bit
  /// little-endian value at offset \p Offset.
  void addLE32Patch(uint32_t Offset, uint32_t NewValue);

  /// Add a patch at \p Offset with \p Value using unsigned LEB128 encoding with
  /// size \p Size. \p Size should not be less than a minimum number of bytes
  /// needed to encode \p Value.
  void addUDataPatch(uint32_t Offset, uint64_t Value, uint64_t Size);

  void patchBinary(std::string &BinaryContents) override;
};

/// Apply small modifications to the .debug_abbrev DWARF section.
class DebugAbbrevPatcher : public BinaryPatcher {
private:
  /// Patch of changing one attribute to another.
  struct AbbrevAttrPatch {
    uint32_t Code;    // Code of abbreviation to be modified.
    uint16_t Attr;    // ID of attribute to be replaced.
    uint8_t NewAttr;  // ID of the new attribute.
    uint8_t NewForm;  // Form of the new attribute.
  };

  std::map<const DWARFUnit *, std::vector<AbbrevAttrPatch>> Patches;

public:
  ~DebugAbbrevPatcher() { }
  /// Adds a patch to change an attribute of an abbreviation that belongs to
  /// \p Unit to another attribute.
  /// \p AbbrevCode code of the abbreviation to be modified.
  /// \p AttrTag ID of the attribute to be replaced.
  /// \p NewAttrTag ID of the new attribute.
  /// \p NewAttrForm Form of the new attribute.
  /// We only handle standard forms, that are encoded in a single byte.
  void addAttributePatch(const DWARFUnit *Unit,
                         uint32_t AbbrevCode,
                         uint16_t AttrTag,
                         uint8_t NewAttrTag,
                         uint8_t NewAttrForm);

  void patchBinary(std::string &Contents) override;
};

} // namespace bolt
} // namespace llvm

#endif
