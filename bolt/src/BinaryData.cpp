//===--- BinaryData.cpp - Representation of section data objects ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "BinaryData.h"
#include "BinarySection.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Regex.h"

using namespace llvm;
using namespace bolt;

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

namespace opts {
extern cl::OptionCategory BoltCategory;
extern cl::opt<unsigned> Verbosity;

cl::opt<bool>
PrintSymbolAliases("print-aliases",
  cl::desc("print aliases when printing objects"),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));
}

bool BinaryData::isAbsolute() const {
  return Flags & SymbolRef::SF_Absolute;
}

bool BinaryData::isMoveable() const {
  return (!isAbsolute() &&
          (IsMoveable &&
           (!Parent || isTopLevelJumpTable())));
}

void BinaryData::merge(const BinaryData *Other) {
  assert(!Size || !Other->Size || Size == Other->Size);
  assert(Address == Other->Address);
  assert(*Section == *Other->Section);
  assert(OutputOffset == Other->OutputOffset);
  assert(OutputSection == Other->OutputSection);
  Names.insert(Names.end(), Other->Names.begin(), Other->Names.end());
  Symbols.insert(Symbols.end(), Other->Symbols.begin(), Other->Symbols.end());
  MemData.insert(MemData.end(), Other->MemData.begin(), Other->MemData.end());
  Flags |= Other->Flags;
  if (!Size)
    Size = Other->Size;
}

bool BinaryData::hasNameRegex(StringRef NameRegex) const {
  Regex MatchName(NameRegex);
  for (auto &Name : Names)
    if (MatchName.match(Name))
      return true;
  return false;
}

StringRef BinaryData::getSectionName() const {
  return getSection().getName();
}

StringRef BinaryData::getOutputSectionName() const {
  return getOutputSection().getName();
}

uint64_t BinaryData::getOutputAddress() const {
  assert(OutputSection->getOutputAddress());
  return OutputSection->getOutputAddress() + OutputOffset;
}

uint64_t BinaryData::getOffset() const {
  return Address - getSection().getAddress();
}

void BinaryData::setSection(BinarySection &NewSection) {
  if (OutputSection == Section)
    OutputSection = &NewSection;
  Section = &NewSection;
}

bool BinaryData::isMoved() const {
  return (getOffset() != OutputOffset || OutputSection != Section);
}

void BinaryData::print(raw_ostream &OS) const {
  printBrief(OS);
}

void BinaryData::printBrief(raw_ostream &OS) const {
  OS << "(";

  if (isJumpTable())
    OS << "jump-table: ";
  else
    OS << "object: ";

  OS << getName();

  if ((opts::PrintSymbolAliases || opts::Verbosity > 1) && Names.size() > 1) {
    OS << ", aliases:";
    for (unsigned I = 1u; I < Names.size(); ++I) {
      OS << (I == 1 ? " (" : ", ") << Names[I];
    }
    OS << ")";
  }

  if (Parent) {
    OS << " (parent: ";
    Parent->printBrief(OS);
    OS << ")";
  }

  OS << ", 0x" << Twine::utohexstr(getAddress())
     << ":0x" << Twine::utohexstr(getEndAddress())
     << "/" << getSize() << "/" << getAlignment()
     << "/0x" << Twine::utohexstr(Flags);

  if (opts::Verbosity > 1) {
    for (auto &MI : memData()) {
      OS << ", " << MI;
    }
  }

  OS << ")";
}

BinaryData::BinaryData(StringRef Name,
                       uint64_t Address,
                       uint64_t Size,
                       uint16_t Alignment,
                       BinarySection &Section,
                       unsigned Flags)
: Names({Name}),
  Section(&Section),
  Address(Address),
  Size(Size),
  Alignment(Alignment),
  Flags(Flags),
  OutputSection(&Section),
  OutputOffset(getOffset())
{ }
