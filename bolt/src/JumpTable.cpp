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

#include "JumpTable.h"
#include "BinaryFunction.h"
#include "BinarySection.h"
#include "Relocation.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace bolt;

namespace opts {
extern cl::opt<JumpTableSupportLevel> JumpTables;
extern cl::opt<unsigned> Verbosity;
}

JumpTable::JumpTable(MCSymbol &Symbol,
                     uint64_t Address,
                     std::size_t EntrySize,
                     JumpTableType Type,
                     LabelMapType &&Labels,
                     BinaryFunction &BF,
                     BinarySection &Section)
  : BinaryData(Symbol, Address, 0, EntrySize, Section),
    EntrySize(EntrySize),
    OutputEntrySize(EntrySize),
    Type(Type),
    Labels(Labels),
    Parent(&BF) {
}

std::pair<size_t, size_t>
JumpTable::getEntriesForAddress(const uint64_t Addr) const {
  // Check if this is not an address, but a cloned JT id
  if ((int64_t)Addr < 0ll)
    return std::make_pair(0, Entries.size());

  const uint64_t InstOffset = Addr - getAddress();
  size_t StartIndex = 0, EndIndex = 0;
  uint64_t Offset = 0;

  for (size_t I = 0; I < Entries.size(); ++I) {
    auto LI = Labels.find(Offset);
    if (LI != Labels.end()) {
      const auto NextLI = std::next(LI);
      const auto NextOffset =
        NextLI == Labels.end() ? getSize() : NextLI->first;
      if (InstOffset >= LI->first && InstOffset < NextOffset) {
        StartIndex = I;
        EndIndex = I;
        while (Offset < NextOffset) {
          ++EndIndex;
          Offset += EntrySize;
        }
        break;
      }
    }
    Offset += EntrySize;
  }

  return std::make_pair(StartIndex, EndIndex);
}

bool JumpTable::replaceDestination(uint64_t JTAddress, const MCSymbol *OldDest,
                                   MCSymbol *NewDest) {
  bool Patched{false};
  const auto Range = getEntriesForAddress(JTAddress);
  for (auto I = &Entries[Range.first], E = &Entries[Range.second]; I != E;
       ++I) {
    auto &Entry = *I;
    if (Entry == OldDest) {
      Patched = true;
      Entry = NewDest;
    }
  }
  return Patched;
}

void JumpTable::updateOriginal() {
  // In non-relocation mode we have to emit jump tables in local sections.
  // This way we only overwrite them when a corresponding function is
  // overwritten.
  const uint64_t BaseOffset = getAddress() - getSection().getAddress();
  uint64_t Offset = BaseOffset;
  for (auto *Entry : Entries) {
    const auto RelType =
      Type == JTT_NORMAL ? ELF::R_X86_64_64 : ELF::R_X86_64_PC32;
    const uint64_t RelAddend = (Type == JTT_NORMAL ? 0 : Offset - BaseOffset);
    DEBUG(dbgs() << "BOLT-DEBUG: adding relocation to section "
                 << getSectionName() << " at offset 0x"
                 << Twine::utohexstr(Offset) << " for symbol "
                 << Entry->getName() << " with addend "
                 << Twine::utohexstr(RelAddend) << '\n');
    getOutputSection().addRelocation(Offset, Entry, RelType, RelAddend);
    Offset += EntrySize;
  }
}

void JumpTable::print(raw_ostream &OS) const {
  uint64_t Offset = 0;
  if (Type == JTT_PIC)
    OS << "PIC ";
  OS << "Jump table " << getName() << " for function " << *Parent << " at 0x"
     << Twine::utohexstr(getAddress()) << " with a total count of " << Count
     << ":\n";
  for (const auto EntryOffset : OffsetEntries) {
    OS << "  0x" << Twine::utohexstr(EntryOffset) << '\n';
  }
  for (const auto *Entry : Entries) {
    auto LI = Labels.find(Offset);
    if (Offset && LI != Labels.end()) {
      OS << "Jump Table " << LI->second->getName() << " at 0x"
         << Twine::utohexstr(getAddress() + Offset)
        << " (possibly part of larger jump table):\n";
    }
    OS << format("  0x%04" PRIx64 " : ", Offset) << Entry->getName();
    if (!Counts.empty()) {
      OS << " : " << Counts[Offset / EntrySize].Mispreds
         << "/" << Counts[Offset / EntrySize].Count;
    }
    OS << '\n';
    Offset += EntrySize;
  }
  OS << "\n\n";
}
