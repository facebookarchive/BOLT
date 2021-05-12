//===--- JumpTable.h - Representation of a jump table ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_JUMP_TABLE_H
#define LLVM_TOOLS_LLVM_BOLT_JUMP_TABLE_H

#include "BinaryData.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include <map>
#include <vector>

namespace llvm {
namespace bolt {

enum JumpTableSupportLevel : char {
  JTS_NONE = 0,       /// Disable jump tables support.
  JTS_BASIC = 1,      /// Enable basic jump tables support (in-place).
  JTS_MOVE = 2,       /// Move jump tables to a separate section.
  JTS_SPLIT = 3,      /// Enable hot/cold splitting of jump tables.
  JTS_AGGRESSIVE = 4, /// Aggressive splitting of jump tables.
};

/// Representation of a jump table.
///
/// The jump table may include other jump tables that are referenced by
/// a different label at a different offset in this jump table.
class JumpTable : public BinaryData {
public:
  enum JumpTableType : char {
    JTT_NORMAL,
    JTT_PIC,
  };

  /// Branch statistics for jump table entries.
  struct JumpInfo {
    uint64_t Mispreds{0};
    uint64_t Count{0};
  };

  /// Size of the entry used for storage.
  std::size_t EntrySize;

  /// Size of the entry size we will write (we may use a more compact layout)
  std::size_t OutputEntrySize;

  /// The type of this jump table.
  JumpTableType Type;

  /// All the entries as labels.
  std::vector<MCSymbol *> Entries;

  /// All the entries as offsets into a function. Invalid after CFG is built.
  std::vector<uint64_t> OffsetEntries;

  /// Map <Offset> -> <Label> used for embedded jump tables. Label at 0 offset
  /// is the main label for the jump table.
  using LabelMapType = std::map<unsigned, MCSymbol *>;
  LabelMapType Labels;

  /// Dynamic number of times each entry in the table was referenced.
  /// Identical entries will have a shared count (identical for every
  /// entry in the set).
  std::vector<JumpInfo> Counts;

  /// Total number of times this jump table was used.
  uint64_t Count{0};

  /// Return the size of the jump table.
  uint64_t getSize() const {
    return std::max(OffsetEntries.size(), Entries.size()) * EntrySize;
  }

  const MCSymbol *getFirstLabel() const {
    assert(Labels.count(0) != 0 && "labels must have an entry at 0");
    return Labels.find(0)->second;
  }

  /// Get the indexes for symbol entries that correspond to the jump table
  /// starting at (or containing) 'Addr'.
  std::pair<size_t, size_t> getEntriesForAddress(const uint64_t Addr) const;

  /// Constructor.
  JumpTable(StringRef Name,
            uint64_t Address,
            std::size_t EntrySize,
            JumpTableType Type,
            decltype(OffsetEntries) &&OffsetEntries,
            LabelMapType &&Labels,
            BinarySection &Section);

  virtual bool isJumpTable() const override { return true; }

  /// Change all entries of the jump table in \p JTAddress pointing to
  /// \p OldDest to \p NewDest. Return false if unsuccessful.
  bool replaceDestination(uint64_t JTAddress, const MCSymbol *OldDest,
                          MCSymbol *NewDest);

  /// Update jump table at its original location.
  void updateOriginal();

  /// Emit jump table data. Callee supplies sections for the data.
  /// Return the number of total bytes emitted.
  uint64_t emit(MCStreamer *Streamer, MCSection *HotSection,
                MCSection *ColdSection);

  /// Print for debugging purposes.
  virtual void print(raw_ostream &OS) const override;
};

} // namespace bolt
} // namespace llvm

#endif
