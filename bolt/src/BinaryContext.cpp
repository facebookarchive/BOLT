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
#include "DataReader.h"
#include "llvm/ADT/Twine.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/CommandLine.h"
#include <iterator>

using namespace llvm;
using namespace bolt;

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

namespace opts {

extern cl::OptionCategory BoltCategory;

extern cl::opt<unsigned> Verbosity;

cl::opt<bool>
NoHugePages("no-huge-pages",
  cl::desc("use regular size pages for code alignment"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintDebugInfo("print-debug-info",
  cl::desc("print debug info when printing functions"),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

cl::opt<bool>
PrintRelocations("print-relocations",
  cl::desc("print relocations when printing functions/objects"),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintMemData("print-mem-data",
  cl::desc("print memory data annotations when printing functions"),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

} // namespace opts

BinaryContext::BinaryContext(std::unique_ptr<MCContext> Ctx,
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
                             std::unique_ptr<MCPlusBuilder> MIB,
                             std::unique_ptr<const MCRegisterInfo> MRI,
                             std::unique_ptr<MCDisassembler> DisAsm,
                             DataReader &DR)
    : Ctx(std::move(Ctx)),
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
      MIB(std::move(MIB)),
      MRI(std::move(MRI)),
      DisAsm(std::move(DisAsm)),
      DR(DR) {
  Relocation::Arch = this->TheTriple->getArch();
  PageAlign = opts::NoHugePages ? RegularPageSize : HugePageSize;
}

BinaryContext::~BinaryContext() {
  for (auto *Section : Sections) {
    delete Section;
  }
  for (auto *InjectedFunction : InjectedBinaryFunctions) {
    delete InjectedFunction;
  }
  clearBinaryData();
}

std::unique_ptr<MCObjectWriter>
BinaryContext::createObjectWriter(raw_pwrite_stream &OS) {
  if (!MAB) {
    MAB = std::unique_ptr<MCAsmBackend>(
        TheTarget->createMCAsmBackend(*STI, *MRI, MCTargetOptions()));
  }

  return MAB->createObjectWriter(OS);
}

bool BinaryContext::validateObjectNesting() const {
  auto Itr = BinaryDataMap.begin();
  auto End = BinaryDataMap.end();
  bool Valid = true;
  while (Itr != End) {
    auto Next = std::next(Itr);
    while (Next != End &&
           Itr->second->getSection() == Next->second->getSection() &&
           Itr->second->containsRange(Next->second->getAddress(),
                                      Next->second->getSize())) {
      if (Next->second->Parent != Itr->second) {
        errs() << "BOLT-WARNING: object nesting incorrect for:\n"
               << "BOLT-WARNING:  " << *Itr->second << "\n"
               << "BOLT-WARNING:  " << *Next->second << "\n";
        Valid = false;
      }
      ++Next;
    }
    Itr = Next;
  }
  return Valid;
}

bool BinaryContext::validateHoles() const {
  bool Valid = true;
  for (auto &Section : sections()) {
    for (const auto &Rel : Section.relocations()) {
      auto RelAddr = Rel.Offset + Section.getAddress();
      auto *BD = getBinaryDataContainingAddress(RelAddr);
      if (!BD) {
        errs() << "BOLT-WARNING: no BinaryData found for relocation at address"
               << " 0x" << Twine::utohexstr(RelAddr) << " in "
               << Section.getName() << "\n";
        Valid = false;
      } else if (!BD->getAtomicRoot()) {
        errs() << "BOLT-WARNING: no atomic BinaryData found for relocation at "
               << "address 0x" << Twine::utohexstr(RelAddr) << " in "
               << Section.getName() << "\n";
        Valid = false;
      }
    }
  }
  return Valid;
}

void BinaryContext::updateObjectNesting(BinaryDataMapType::iterator GAI) {
  const auto Address = GAI->second->getAddress();
  const auto Size = GAI->second->getSize();

  auto fixParents =
    [&](BinaryDataMapType::iterator Itr, BinaryData *NewParent) {
    auto *OldParent = Itr->second->Parent;
    Itr->second->Parent = NewParent;
    ++Itr;
    while (Itr != BinaryDataMap.end() && OldParent &&
           Itr->second->Parent == OldParent) {
      Itr->second->Parent = NewParent;
      ++Itr;
    }
  };

  // Check if the previous symbol contains the newly added symbol.
  if (GAI != BinaryDataMap.begin()) {
    auto *Prev = std::prev(GAI)->second;
    while (Prev) {
      if (Prev->getSection() == GAI->second->getSection() &&
          Prev->containsRange(Address, Size)) {
        fixParents(GAI, Prev);
      } else {
        fixParents(GAI, nullptr);
      }
      Prev = Prev->Parent;
    }
  }

  // Check if the newly added symbol contains any subsequent symbols.
  if (Size != 0) {
    auto *BD = GAI->second->Parent ? GAI->second->Parent : GAI->second;
    auto Itr = std::next(GAI);
    while (Itr != BinaryDataMap.end() &&
           BD->containsRange(Itr->second->getAddress(),
                             Itr->second->getSize())) {
      Itr->second->Parent = BD;
      ++Itr;
    }
  }
}

iterator_range<BinaryContext::binary_data_iterator>
BinaryContext::getSubBinaryData(BinaryData *BD) {
  auto Start = std::next(BinaryDataMap.find(BD->getAddress()));
  auto End = Start;
  while (End != BinaryDataMap.end() &&
         BD->isAncestorOf(End->second)) {
    ++End;
  }
  return make_range(Start, End);
}

MCSymbol *BinaryContext::getOrCreateGlobalSymbol(uint64_t Address,
                                                 Twine Prefix,
                                                 uint64_t Size,
                                                 uint16_t Alignment,
                                                 unsigned Flags) {
  auto Itr = BinaryDataMap.find(Address);
  if (Itr != BinaryDataMap.end()) {
    assert(Itr->second->getSize() == Size || !Size);
    return Itr->second->getSymbol();
  }

  std::string Name = (Prefix + "0x" + Twine::utohexstr(Address)).str();
  assert(!GlobalSymbols.count(Name) && "created name is not unique");
  return registerNameAtAddress(Name, Address, Size, Alignment, Flags);
}

BinaryFunction *BinaryContext::createBinaryFunction(
    const std::string &Name, BinarySection &Section, uint64_t Address,
    uint64_t Size, bool IsSimple, uint64_t SymbolSize, uint16_t Alignment) {
  auto Result = BinaryFunctions.emplace(
      Address, BinaryFunction(Name, Section, Address, Size, *this, IsSimple));
  assert(Result.second == true && "unexpected duplicate function");
  auto *BF = &Result.first->second;
  registerNameAtAddress(Name, Address, SymbolSize ? SymbolSize : Size,
                        Alignment);
  setSymbolToFunctionMap(BF->getSymbol(), BF);
  return BF;
}

std::pair<JumpTable *, const MCSymbol *>
BinaryContext::createJumpTable(BinaryFunction &Function,
                               uint64_t Address,
                               JumpTable::JumpTableType Type,
                               JumpTable::OffsetEntriesType &&OffsetEntries) {
  const auto JumpTableName = generateJumpTableName(Function, Address);
  if (auto *JT = getJumpTableContainingAddress(Address)) {
    assert(JT->Type == Type && "jump table types have to match");
    assert(JT->Parent == &Function &&
           "cannot re-use jump table of a different function");
    assert((Address == JT->getAddress() || Type != JumpTable::JTT_PIC) &&
           "cannot re-use part of PIC jump table");
    // Get or create a new label for the table.
    const auto JTOffset = Address - JT->getAddress();
    auto LI = JT->Labels.find(JTOffset);
    if (LI == JT->Labels.end()) {
      auto *JTStartLabel = registerNameAtAddress(JumpTableName,
                                                 Address,
                                                 0,
                                                 JT->EntrySize);
      auto Result = JT->Labels.emplace(JTOffset, JTStartLabel);
      assert(Result.second && "error adding jump table label");
      LI = Result.first;
    }

    return std::make_pair(JT, LI->second);
  }

  auto *JTStartLabel = Ctx->getOrCreateSymbol(JumpTableName);
  const auto EntrySize =
    Type == JumpTable::JTT_PIC ? 4 : AsmInfo->getCodePointerSize();

  DEBUG(dbgs() << "BOLT-DEBUG: creating jump table "
               << JTStartLabel->getName()
               << " in function " << Function << " with "
               << OffsetEntries.size() << " entries\n");

  auto *JT = new JumpTable(JumpTableName,
                           Address,
                           EntrySize,
                           Type,
                           std::move(OffsetEntries),
                           JumpTable::LabelMapType{{0, JTStartLabel}},
                           Function,
                           *getSectionForAddress(Address));

  const auto *JTLabel = registerNameAtAddress(JumpTableName, Address, JT);
  assert(JTLabel == JTStartLabel);

  JumpTables.emplace(Address, JT);

  // Duplicate the entry for the parent function for easy access.
  Function.JumpTables.emplace(Address, JT);

  return std::make_pair(JT, JTLabel);
}

std::string BinaryContext::generateJumpTableName(const BinaryFunction &BF,
                                                 uint64_t Address) {
  size_t Id;
  uint64_t Offset = 0;
  if (const auto *JT = BF.getJumpTableContainingAddress(Address)) {
    Offset = Address - JT->getAddress();
    auto Itr = JT->Labels.find(Offset);
    if (Itr != JT->Labels.end()) {
      return Itr->second->getName();
    }
    Id = JumpTableIds.at(JT->getAddress());
  } else {
    Id = JumpTableIds[Address] = BF.JumpTables.size();
  }
  return ("JUMP_TABLE/" + BF.Names[0] + "." + std::to_string(Id) +
          (Offset ? ("." + std::to_string(Offset)) : ""));
}

MCSymbol *BinaryContext::registerNameAtAddress(StringRef Name,
                                               uint64_t Address,
                                               uint64_t Size,
                                               uint16_t Alignment,
                                               unsigned Flags) {
  auto SectionOrErr = getSectionForAddress(Address);
  auto &Section = SectionOrErr ? SectionOrErr.get() : absoluteSection();
  auto GAI = BinaryDataMap.find(Address);
  BinaryData *BD;
  if (GAI == BinaryDataMap.end()) {
    BD = new BinaryData(Name,
                        Address,
                        Size,
                        Alignment ? Alignment : 1,
                        Section,
                        Flags);
  } else {
    BD = GAI->second;
  }
  return registerNameAtAddress(Name, Address, BD);
}

MCSymbol *BinaryContext::registerNameAtAddress(StringRef Name,
                                               uint64_t Address,
                                               BinaryData *BD) {
  auto GAI = BinaryDataMap.find(Address);
  if (GAI != BinaryDataMap.end()) {
    if (BD != GAI->second) {
      // Note: this could be a source of bugs if client code holds
      // on to BinaryData*'s in data structures for any length of time.
      auto *OldBD = GAI->second;
      BD->merge(GAI->second);
      delete OldBD;
      GAI->second = BD;
      for (auto &Name : BD->names()) {
        GlobalSymbols[Name] = BD;
      }
      updateObjectNesting(GAI);
      BD = nullptr;
    } else if (!GAI->second->hasName(Name)) {
      GAI->second->Names.push_back(Name);
      GlobalSymbols[Name] = GAI->second;
    } else {
      BD = nullptr;
    }
  } else {
    GAI = BinaryDataMap.emplace(Address, BD).first;
    GlobalSymbols[Name] = BD;
    updateObjectNesting(GAI);
  }

  // Register the name with MCContext.
  auto *Symbol = Ctx->getOrCreateSymbol(Name);
  if (BD) {
    BD->Symbols.push_back(Symbol);
    assert(BD->Symbols.size() == BD->Names.size() &&
           "there should be a 1:1 mapping between names and symbols");
  }
  return Symbol;
}

const BinaryData *
BinaryContext::getBinaryDataContainingAddressImpl(uint64_t Address,
                                                  bool IncludeEnd,
                                                  bool BestFit) const {
  auto NI = BinaryDataMap.lower_bound(Address);
  auto End = BinaryDataMap.end();
  if ((NI != End && Address == NI->first && !IncludeEnd) ||
      (NI-- != BinaryDataMap.begin())) {
    if (NI->second->containsAddress(Address) ||
        (IncludeEnd && NI->second->getEndAddress() == Address)) {
      while (BestFit &&
             std::next(NI) != End &&
             (std::next(NI)->second->containsAddress(Address) ||
              (IncludeEnd && std::next(NI)->second->getEndAddress() == Address))) {
        ++NI;
      }
      return NI->second;
    }

    // If this is a sub-symbol, see if a parent data contains the address.
    auto *BD = NI->second->getParent();
    while (BD) {
      if (BD->containsAddress(Address) ||
          (IncludeEnd && NI->second->getEndAddress() == Address))
        return BD;
      BD = BD->getParent();
    }
  }
  return nullptr;
}

bool BinaryContext::setBinaryDataSize(uint64_t Address, uint64_t Size) {
  auto NI = BinaryDataMap.find(Address);
  assert(NI != BinaryDataMap.end());
  if (NI == BinaryDataMap.end())
    return false;
  // TODO: it's possible that a jump table starts at the same address
  // as a larger blob of private data.  When we set the size of the
  // jump table, it might be smaller than the total blob size.  In this
  // case we just leave the original size since (currently) it won't really
  // affect anything.  See T26915981.
  assert((!NI->second->Size || NI->second->Size == Size ||
          (NI->second->isJumpTable() && NI->second->Size > Size)) &&
         "can't change the size of a symbol that has already had its "
         "size set");
  if (!NI->second->Size) {
    NI->second->Size = Size;
    updateObjectNesting(NI);
    return true;
  }
  return false;
}

void BinaryContext::generateSymbolHashes() {
  auto isNonAnonymousName = [](StringRef Name) {
    return !(Name.startswith("SYMBOLat") ||
             Name.startswith("DATAat") ||
             Name.startswith("HOLEat"));
  };

  auto isPadding = [](const BinaryData &BD) {
    auto Contents = BD.getSection().getContents();
    auto SymData = Contents.substr(BD.getOffset(), BD.getSize());
    return (BD.getName().startswith("HOLEat") ||
            SymData.find_first_not_of(0) == StringRef::npos);
  };

  uint64_t NumCollisions = 0;
  for (auto &Entry : BinaryDataMap) {
    auto &BD = *Entry.second;
    auto Name = BD.getName();

    if (isNonAnonymousName(Name))
      continue;

    // First check if a non-anonymous alias exists and move it to the front.
    if (BD.getNames().size() > 1) {
      auto Itr = std::find_if(BD.Names.begin(),
                              BD.Names.end(),
                              isNonAnonymousName);
      if (Itr != BD.Names.end()) {
        assert(BD.Names.size() == BD.Symbols.size() &&
               "there should be a 1:1 mapping between names and symbols");
        auto Idx = std::distance(BD.Names.begin(), Itr);
        std::swap(BD.Names[0], *Itr);
        std::swap(BD.Symbols[0], BD.Symbols[Idx]);
        continue;
      }
    }

    // We have to skip 0 size symbols since they will all collide.
    if (BD.getSize() == 0) {
      continue;
    }

    const auto Hash = BD.getSection().hash(BD);
    const auto Idx = Name.find("0x");
    std::string NewName = (Twine(Name.substr(0, Idx)) +
                 "_" + Twine::utohexstr(Hash)).str();
    if (getBinaryDataByName(NewName)) {
      // Ignore collisions for symbols that appear to be padding
      // (i.e. all zeros or a "hole")
      if (!isPadding(BD)) {
        if (opts::Verbosity) {
          errs() << "BOLT-WARNING: collision detected when hashing " << BD
                 << " with new name (" << NewName << "), skipping.\n";
        }
        ++NumCollisions;
      }
      continue;
    }
    BD.Names.insert(BD.Names.begin(), NewName);
    BD.Symbols.insert(BD.Symbols.begin(),
                       Ctx->getOrCreateSymbol(NewName));
    assert(BD.Names.size() == BD.Symbols.size() &&
           "there should be a 1:1 mapping between names and symbols");
    GlobalSymbols[NewName] = &BD;
  }
  if (NumCollisions) {
    errs() << "BOLT-WARNING: " << NumCollisions
           << " collisions detected while hashing binary objects";
    if (!opts::Verbosity)
      errs() << ". Use -v=1 to see the list.";
    errs() << '\n';
  }
}

void BinaryContext::processInterproceduralReferences() {
  for (auto &Pair : InterproceduralReferences) {
    auto *FromBF = Pair.first;
    auto Addr = Pair.second;
    auto *ContainingFunction = getBinaryFunctionContainingAddress(Addr);
    if (FromBF == ContainingFunction)
      continue;

    if (ContainingFunction) {
      // Only a parent function (or a sibling) can reach its fragment.
      if (ContainingFunction->IsFragment) {
        assert(!FromBF->IsFragment &&
               "only one cold fragment is supported at this time");
        ContainingFunction->setParentFunction(FromBF);
        FromBF->addFragment(ContainingFunction);
        if (!HasRelocations) {
          ContainingFunction->setSimple(false);
          FromBF->setSimple(false);
        }
        if (opts::Verbosity >= 1) {
          outs() << "BOLT-INFO: marking " << *ContainingFunction
                 << " as a fragment of " << *FromBF << '\n';
        }
        continue;
      }

      if (ContainingFunction->getAddress() != Addr) {
        ContainingFunction->addEntryPoint(Addr);
        if (!HasRelocations) {
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: Function " << *ContainingFunction
                   << " has internal BBs that are target of a reference "
                   << "located in another function. Skipping the function.\n";
          }
          ContainingFunction->setSimple(false);
        }
      }
    } else if (Addr) {
      // Check if address falls in function padding space - this could be
      // unmarked data in code. In this case adjust the padding space size.
      auto Section = getSectionForAddress(Addr);
      assert(Section && "cannot get section for referenced address");

      if (!Section->isText())
        continue;

      // PLT requires special handling and could be ignored in this context.
      StringRef SectionName = Section->getName();
      if (SectionName == ".plt" || SectionName == ".plt.got")
        continue;

      if (HasRelocations) {
        errs() << "BOLT-ERROR: cannot process binaries with unmarked "
               << "object in code at address 0x"
               << Twine::utohexstr(Addr) << " belonging to section "
               << SectionName << " in relocation mode.\n";
        exit(1);
      }

      ContainingFunction =
        getBinaryFunctionContainingAddress(Addr,
                                           /*CheckPastEnd=*/false,
                                           /*UseMaxSize=*/true);
      // We are not going to overwrite non-simple functions, but for simple
      // ones - adjust the padding size.
      if (ContainingFunction && ContainingFunction->isSimple()) {
        errs() << "BOLT-WARNING: function " << *ContainingFunction
               << " has an object detected in a padding region at address 0x"
               << Twine::utohexstr(Addr) << '\n';
        ContainingFunction->setMaxSize(Addr -
                                       ContainingFunction->getAddress());
      }
    }
  }

  InterproceduralReferences.clear();
}

void BinaryContext::postProcessSymbolTable() {
  fixBinaryDataHoles();
  bool Valid = true;
  for (auto &Entry : BinaryDataMap) {
    auto *BD = Entry.second;
    if ((BD->getName().startswith("SYMBOLat") ||
         BD->getName().startswith("DATAat")) &&
        !BD->getParent() &&
        !BD->getSize() &&
        !BD->isAbsolute() &&
        BD->getSection()) {
      errs() << "BOLT-WARNING: zero-sized top level symbol: " << *BD << "\n";
      Valid = false;
    }
  }
  assert(Valid);
  assignMemData();
  generateSymbolHashes();
}

void BinaryContext::foldFunction(BinaryFunction &ChildBF,
                                 BinaryFunction &ParentBF) {
  // Copy name list.
  ParentBF.addNewNames(ChildBF.getNames());

  // Update internal bookkeeping info.
  for (auto &Name : ChildBF.getNames()) {
    // Calls to functions are handled via symbols, and we keep the lookup table
    // that we need to update.
    auto *Symbol = Ctx->lookupSymbol(Name);
    assert(Symbol && "symbol cannot be NULL at this point");
    SymbolToFunctionMap[Symbol] = &ParentBF;

    // NB: there's no need to update BinaryDataMap and GlobalSymbols.
  }

  // Merge execution counts of ChildBF into those of ParentBF.
  ChildBF.mergeProfileDataInto(ParentBF);

  if (HasRelocations) {
    // Remove ChildBF from the global set of functions in relocs mode.
    auto FI = BinaryFunctions.find(ChildBF.getAddress());
    assert(FI != BinaryFunctions.end() && "function not found");
    assert(&ChildBF == &FI->second && "function mismatch");
    FI = BinaryFunctions.erase(FI);
  } else {
    // In non-relocation mode we keep the function, but rename it.
    std::string NewName = "__ICF_" + ChildBF.Names.back();
    ChildBF.Names.clear();
    ChildBF.Names.push_back(NewName);
    ChildBF.OutputSymbol = Ctx->getOrCreateSymbol(NewName);
    ChildBF.setFolded();
  }
}

void BinaryContext::fixBinaryDataHoles() {
  assert(validateObjectNesting() && "object nesting inconsitency detected");

  for (auto &Section : allocatableSections()) {
    std::vector<std::pair<uint64_t, uint64_t>> Holes;

    auto isNotHole = [&Section](const binary_data_iterator &Itr) {
      auto *BD = Itr->second;
      bool isHole = (!BD->getParent() &&
                     !BD->getSize() &&
                     BD->isObject() &&
                     (BD->getName().startswith("SYMBOLat0x") ||
                      BD->getName().startswith("DATAat0x") ||
                      BD->getName().startswith("ANONYMOUS")));
      return !isHole && BD->getSection() == Section && !BD->getParent();
    };

    auto BDStart = BinaryDataMap.begin();
    auto BDEnd = BinaryDataMap.end();
    auto Itr = FilteredBinaryDataIterator(isNotHole, BDStart, BDEnd);
    auto End = FilteredBinaryDataIterator(isNotHole, BDEnd, BDEnd);

    uint64_t EndAddress = Section.getAddress();

    while (Itr != End) {
      if (Itr->second->getAddress() > EndAddress) {
        auto Gap = Itr->second->getAddress() - EndAddress;
        Holes.push_back(std::make_pair(EndAddress, Gap));
      }
      EndAddress = Itr->second->getEndAddress();
      ++Itr;
    }

    if (EndAddress < Section.getEndAddress()) {
      Holes.push_back(std::make_pair(EndAddress,
                                     Section.getEndAddress() - EndAddress));
    }

    // If there is already a symbol at the start of the hole, grow that symbol
    // to cover the rest.  Otherwise, create a new symbol to cover the hole.
    for (auto &Hole : Holes) {
      auto *BD = getBinaryDataAtAddress(Hole.first);
      if (BD) {
        // BD->getSection() can be != Section if there are sections that
        // overlap.  In this case it is probably safe to just skip the holes
        // since the overlapping section will not(?) have any symbols in it.
        if (BD->getSection() == Section)
          setBinaryDataSize(Hole.first, Hole.second);
      } else {
        getOrCreateGlobalSymbol(Hole.first, "HOLEat", Hole.second, 1);
      }
    }
  }

  assert(validateObjectNesting() && "object nesting inconsitency detected");
  assert(validateHoles() && "top level hole detected in object map");
}

void BinaryContext::printGlobalSymbols(raw_ostream& OS) const {
  const BinarySection* CurrentSection = nullptr;
  bool FirstSection = true;

  for (auto &Entry : BinaryDataMap) {
    const auto *BD = Entry.second;
    const auto &Section = BD->getSection();
    if (FirstSection || Section != *CurrentSection) {
      uint64_t Address, Size;
      StringRef Name = Section.getName();
      if (Section) {
        Address = Section.getAddress();
        Size = Section.getSize();
      } else {
        Address = BD->getAddress();
        Size = BD->getSize();
      }
      OS << "BOLT-INFO: Section " << Name << ", "
         << "0x" + Twine::utohexstr(Address) << ":"
         << "0x" + Twine::utohexstr(Address + Size) << "/"
         << Size << "\n";
      CurrentSection = &Section;
      FirstSection = false;
    }

    OS << "BOLT-INFO: ";
    auto *P = BD->getParent();
    while (P) {
      OS << "  ";
      P = P->getParent();
    }
    OS << *BD << "\n";
  }
}

void BinaryContext::assignMemData() {
  auto getAddress = [&](const MemInfo &MI) -> uint64_t {
    if (!MI.Addr.IsSymbol)
      return MI.Addr.Offset;

    if (auto *BD = getBinaryDataByName(MI.Addr.Name))
      return BD->getAddress() + MI.Addr.Offset;

    return 0;
  };

  // Map of sections (or heap/stack) to count/size.
  std::map<StringRef, uint64_t> Counts;
  std::map<StringRef, uint64_t> JumpTableCounts;

  uint64_t TotalCount = 0;
  for (auto &Entry : DR.getAllFuncsMemData()) {
    for (auto &MI : Entry.second.Data) {
      const auto Addr = getAddress(MI);
      auto *BD = getBinaryDataContainingAddress(Addr);
      if (BD) {
        BD->getAtomicRoot()->addMemData(MI);
        Counts[BD->getSectionName()] += MI.Count;
        if (BD->getAtomicRoot()->isJumpTable()) {
          JumpTableCounts[BD->getSectionName()] += MI.Count;
        }
      } else {
        Counts["Heap/stack"] += MI.Count;
      }
      TotalCount += MI.Count;
    }
  }

  if (!Counts.empty()) {
    outs() << "BOLT-INFO: Memory stats breakdown:\n";
    for (auto &Entry : Counts) {
      const auto Section = Entry.first;
      const auto Count = Entry.second;
      outs() << "BOLT-INFO:   " << Section << " = " << Count
             << format(" (%.1f%%)\n", 100.0*Count/TotalCount);
      if (JumpTableCounts.count(Section) != 0) {
        const auto JTCount = JumpTableCounts[Section];
        outs() << "BOLT-INFO:     jump tables = " << JTCount
               << format(" (%.1f%%)\n", 100.0*JTCount/Count);
      }
    }
    outs() << "BOLT-INFO: Total memory events: " << TotalCount << "\n";
  }
}

namespace {

/// Recursively finds DWARF DW_TAG_subprogram DIEs and match them with
/// BinaryFunctions. Record DIEs for unknown subprograms (mostly functions that
/// are never called and removed from the binary) in Unknown.
void findSubprograms(const DWARFDie DIE,
                     std::map<uint64_t, BinaryFunction> &BinaryFunctions) {
  if (DIE.isSubprogramDIE()) {
    uint64_t LowPC, HighPC, SectionIndex;
    if (DIE.getLowAndHighPC(LowPC, HighPC, SectionIndex)) {
      auto It = BinaryFunctions.find(LowPC);
      if (It != BinaryFunctions.end()) {
        It->second.addSubprogramDIE(DIE);
      } else {
        // The function must have been optimized away by GC.
      }
    } else {
      const auto RangesVector = DIE.getAddressRanges();
      for (const auto Range : DIE.getAddressRanges()) {
        auto It = BinaryFunctions.find(Range.LowPC);
        if (It != BinaryFunctions.end()) {
          It->second.addSubprogramDIE(DIE);
        }
      }
    }
  }

  for (auto ChildDIE = DIE.getFirstChild(); ChildDIE && !ChildDIE.isNULL();
       ChildDIE = ChildDIE.getSibling()) {
    findSubprograms(ChildDIE, BinaryFunctions);
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
  StringRef Dir = "";
  if (FileNames[FileIndex - 1].DirIdx != 0) {
    if (auto DirName =
            LineTable->Prologue
                .IncludeDirectories[FileNames[FileIndex - 1].DirIdx - 1]
                .getAsCString()) {
      Dir = *DirName;
    }
  }
  StringRef FileName = "";
  if (auto FName = FileNames[FileIndex - 1].Name.getAsCString())
    FileName = *FName;
  assert(FileName != "");
  return cantFail(Ctx->getDwarfFile(Dir, FileName, 0, nullptr, None, DestCUID));
}

std::vector<BinaryFunction *> BinaryContext::getSortedFunctions() {
  std::vector<BinaryFunction *> SortedFunctions(BinaryFunctions.size());
  std::transform(BinaryFunctions.begin(), BinaryFunctions.end(),
                 SortedFunctions.begin(),
                 [](std::pair<const uint64_t, BinaryFunction> &BFI) {
                   return &BFI.second;
                 });

  std::stable_sort(SortedFunctions.begin(), SortedFunctions.end(),
                   [] (const BinaryFunction *A, const BinaryFunction *B) {
                     if (A->hasValidIndex() && B->hasValidIndex()) {
                       return A->getIndex() < B->getIndex();
                     }
                     return A->hasValidIndex();
                   });
  return SortedFunctions;
}

void BinaryContext::preprocessDebugInfo() {
  // Populate MCContext with DWARF files.
  for (const auto &CU : DwCtx->compile_units()) {
    const auto CUID = CU->getOffset();
    auto *LineTable = DwCtx->getLineTableForUnit(CU.get());
    const auto &FileNames = LineTable->Prologue.FileNames;
    // Make sure empty debug line tables are registered too.
    if (FileNames.empty()) {
      cantFail(Ctx->getDwarfFile("", "<unknown>", 0, nullptr, None, CUID));
      continue;
    }
    for (size_t I = 0, Size = FileNames.size(); I != Size; ++I) {
      // Dir indexes start at 1, as DWARF file numbers, and a dir index 0
      // means empty dir.
      StringRef Dir = "";
      if (FileNames[I].DirIdx != 0)
        if (auto DirName =
                LineTable->Prologue.IncludeDirectories[FileNames[I].DirIdx - 1]
                    .getAsCString())
          Dir = *DirName;
      StringRef FileName = "";
      if (auto FName = FileNames[I].Name.getAsCString())
        FileName = *FName;
      assert(FileName != "");
      cantFail(Ctx->getDwarfFile(Dir, FileName, 0, nullptr, None, CUID));
    }
  }

  // For each CU, iterate over its children DIEs and match subprogram DIEs to
  // BinaryFunctions.
  for (auto &CU : DwCtx->compile_units()) {
    findSubprograms(CU->getUnitDIE(false), BinaryFunctions);
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
        Function.addSubprogramDIE(
            DWARFDie(DwCtx->getCompileUnitForOffset(CUOffset), nullptr));
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
          Function.addSubprogramDIE(DWARFDie(CU.get(), nullptr));
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
    OS << "OpRelOffset Reg" << Inst.getRegister() << " " << Inst.getOffset();
    break;
  case MCCFIInstruction::OpAdjustCfaOffset:
    OS << "OfAdjustCfaOffset " << Inst.getOffset();
    break;
  case MCCFIInstruction::OpEscape:
    OS << "OpEscape";
    break;
  case MCCFIInstruction::OpRestore:
    OS << "OpRestore Reg" << Inst.getRegister();
    break;
  case MCCFIInstruction::OpUndefined:
    OS << "OpUndefined Reg" << Inst.getRegister();
    break;
  case MCCFIInstruction::OpRegister:
    OS << "OpRegister Reg" << Inst.getRegister() << " Reg"
       << Inst.getRegister2();
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
  if (MIB->isEHLabel(Instruction)) {
    OS << "  EH_LABEL: " << *MIB->getTargetSymbol(Instruction) << '\n';
    return;
  }
  OS << format("    %08" PRIx64 ": ", Offset);
  if (MIB->isCFI(Instruction)) {
    uint32_t Offset = Instruction.getOperand(0).getImm();
    OS << "\t!CFI\t$" << Offset << "\t; ";
    if (Function)
      printCFI(OS, *Function->getCFIFor(Instruction));
    OS << "\n";
    return;
  }
  InstPrinter->printInst(&Instruction, OS, "", *STI);
  if (MIB->isCall(Instruction)) {
    if (MIB->isTailCall(Instruction))
      OS << " # TAILCALL ";
    if (MIB->isInvoke(Instruction)) {
      const auto EHInfo = MIB->getEHInfo(Instruction);
      OS << " # handler: ";
      if (EHInfo->first)
        OS << *EHInfo->first;
      else
        OS << '0';
      OS << "; action: " << EHInfo->second;
      const auto GnuArgsSize = MIB->getGnuArgsSize(Instruction);
      if (GnuArgsSize >= 0)
        OS << "; GNU_args_size = " << GnuArgsSize;
    }
  }
  if (MIB->isIndirectBranch(Instruction)) {
    if (auto JTAddress = MIB->getJumpTable(Instruction)) {
      OS << " # JUMPTABLE @0x" << Twine::utohexstr(JTAddress);
    }
  }

  MIB->printAnnotations(Instruction, OS);

  const DWARFDebugLine::LineTable *LineTable =
    Function && opts::PrintDebugInfo ? Function->getDWARFUnitLineTable().second
                                     : nullptr;

  if (LineTable) {
    auto RowRef = DebugLineTableRowRef::fromSMLoc(Instruction.getLoc());

    if (RowRef != DebugLineTableRowRef::NULL_ROW) {
      const auto &Row = LineTable->Rows[RowRef.RowIndex - 1];
      StringRef FileName = "";
      if (auto FName =
              LineTable->Prologue.FileNames[Row.File - 1].Name.getAsCString())
        FileName = *FName;
      OS << " # debug line " << FileName << ":" << Row.Line;

      if (Row.Column) {
        OS << ":" << Row.Column;
      }
    }
  }

  if ((opts::PrintMemData || PrintMemData) && Function) {
    const auto *MD = Function->getMemData();
    const auto MemDataOffset =
      MIB->tryGetAnnotationAs<uint64_t>(Instruction, "MemDataOffset");
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
  auto SI = AddressToSection.upper_bound(Address);
  if (SI != AddressToSection.begin()) {
    --SI;
    if (SI->first + SI->second->getSize() > Address)
      return *SI->second;
  }
  return std::make_error_code(std::errc::bad_address);
}

ErrorOr<const BinarySection &>
BinaryContext::getSectionForAddress(uint64_t Address) const {
  auto SI = AddressToSection.upper_bound(Address);
  if (SI != AddressToSection.begin()) {
    --SI;
    if (SI->first + SI->second->getSize() > Address)
      return *SI->second;
  }
  return std::make_error_code(std::errc::bad_address);
}

ErrorOr<StringRef>
BinaryContext::getSectionNameForAddress(uint64_t Address) const {
  if (auto Section = getSectionForAddress(Address)) {
    return Section->getName();
  }
  return std::make_error_code(std::errc::bad_address);
}

BinarySection &BinaryContext::registerSection(BinarySection *Section) {
  assert(!Section->getName().empty() &&
         "can't register sections without a name");
  auto Res = Sections.insert(Section);
  assert(Res.second && "can't register the same section twice.");
  // Only register sections with addresses in the AddressToSection map.
  if (Section->getAddress())
    AddressToSection.insert(std::make_pair(Section->getAddress(), Section));
  NameToSection.insert(std::make_pair(Section->getName(), Section));
  DEBUG(dbgs() << "BOLT-DEBUG: registering " << *Section << "\n");
  return *Section;
}

BinarySection &BinaryContext::registerSection(SectionRef Section) {
  return registerSection(new BinarySection(*this, Section));
}

BinarySection &
BinaryContext::registerSection(StringRef SectionName,
                               const BinarySection &OriginalSection) {
  return registerSection(new BinarySection(*this,
                                           SectionName,
                                           OriginalSection));
}

BinarySection &BinaryContext::registerOrUpdateSection(StringRef Name,
                                                      unsigned ELFType,
                                                      unsigned ELFFlags,
                                                      uint8_t *Data,
                                                      uint64_t Size,
                                                      unsigned Alignment,
                                                      bool IsLocal) {
  auto NamedSections = getSectionByName(Name);
  if (NamedSections.begin() != NamedSections.end()) {
    assert(std::next(NamedSections.begin()) == NamedSections.end() &&
           "can only update unique sections");
    auto *Section = NamedSections.begin()->second;

    DEBUG(dbgs() << "BOLT-DEBUG: updating " << *Section << " -> ");
    const auto Flag = Section->isAllocatable();
    Section->update(Data, Size, Alignment, ELFType, ELFFlags, IsLocal);
    DEBUG(dbgs() << *Section << "\n");
    assert(Flag == Section->isAllocatable() &&
           "can't change section allocation status");
    return *Section;
  }

  return registerSection(new BinarySection(*this, Name, Data, Size, Alignment,
                                           ELFType, ELFFlags, IsLocal));
}

bool BinaryContext::deregisterSection(BinarySection &Section) {
  auto *SectionPtr = &Section;
  auto Itr = Sections.find(SectionPtr);
  if (Itr != Sections.end()) {
    auto Range = AddressToSection.equal_range(SectionPtr->getAddress());
    while (Range.first != Range.second) {
      if (Range.first->second == SectionPtr) {
        AddressToSection.erase(Range.first);
        break;
      }
      ++Range.first;
    }

    auto NameRange = NameToSection.equal_range(SectionPtr->getName());
    while (NameRange.first != NameRange.second) {
      if (NameRange.first->second == SectionPtr) {
        NameToSection.erase(NameRange.first);
        break;
      }
      ++NameRange.first;
    }

    Sections.erase(Itr);
    delete SectionPtr;
    return true;
  }
  return false;
}

void BinaryContext::printSections(raw_ostream &OS) const {
  for (auto &Section : Sections) {
    OS << "BOLT-INFO: " << *Section << "\n";
  }
}

BinarySection &BinaryContext::absoluteSection() {
  if (auto Section = getUniqueSectionByName("<absolute>"))
    return *Section;
  return registerOrUpdateSection("<absolute>", ELF::SHT_NULL, 0u);
}

ErrorOr<uint64_t>
BinaryContext::getUnsignedValueAtAddress(uint64_t Address,
                                         size_t Size) const {
  const auto Section = getSectionForAddress(Address);
  if (!Section)
    return std::make_error_code(std::errc::bad_address);

  if (Section->isVirtual())
    return 0;

  DataExtractor DE(Section->getContents(), AsmInfo->isLittleEndian(),
                   AsmInfo->getCodePointerSize());
  auto ValueOffset = static_cast<uint32_t>(Address - Section->getAddress());
  return DE.getUnsigned(&ValueOffset, Size);
}

ErrorOr<uint64_t>
BinaryContext::getSignedValueAtAddress(uint64_t Address,
                                       size_t Size) const {
  const auto Section = getSectionForAddress(Address);
  if (!Section)
    return std::make_error_code(std::errc::bad_address);

  if (Section->isVirtual())
    return 0;

  DataExtractor DE(Section->getContents(), AsmInfo->isLittleEndian(),
                   AsmInfo->getCodePointerSize());
  auto ValueOffset = static_cast<uint32_t>(Address - Section->getAddress());
  return DE.getSigned(&ValueOffset, Size);
}

void BinaryContext::addRelocation(uint64_t Address,
                                  MCSymbol *Symbol,
                                  uint64_t Type,
                                  uint64_t Addend,
                                  uint64_t Value) {
  auto Section = getSectionForAddress(Address);
  assert(Section && "cannot find section for address");
  Section->addRelocation(Address - Section->getAddress(),
                         Symbol,
                         Type,
                         Addend,
                         Value);
}

bool BinaryContext::removeRelocationAt(uint64_t Address) {
  auto Section = getSectionForAddress(Address);
  assert(Section && "cannot find section for address");
  return Section->removeRelocationAt(Address - Section->getAddress());
}

const Relocation *BinaryContext::getRelocationAt(uint64_t Address) {
  auto Section = getSectionForAddress(Address);
  assert(Section && "cannot find section for address");
  return Section->getRelocationAt(Address - Section->getAddress());
}

void BinaryContext::exitWithBugReport(StringRef Message,
                                      const BinaryFunction &Function) const {
  errs() << "=======================================\n";
  errs() << "BOLT is unable to proceed because it couldn't properly understand "
            "this function.\n";
  errs() << "If you are running the most recent version of BOLT, you may "
            "want to "
            "report this and paste this dump.\nPlease check that there is no "
            "sensitive contents being shared in this dump.\n";
  errs() << "\nOffending function: " << Function.getPrintName() << "\n\n";
  ScopedPrinter SP(errs());
  SP.printBinaryBlock("Function contents", *getFunctionData(Function));
  errs() << "\n";
  Function.dump();
  errs() << "ERROR: " << Message;
  errs() << "\n=======================================\n";
  exit(1);
}

BinaryFunction *
BinaryContext::createInjectedBinaryFunction(const std::string &Name,
                                            bool IsSimple) {
  InjectedBinaryFunctions.push_back(new BinaryFunction(Name, *this, IsSimple));
  auto *BF = InjectedBinaryFunctions.back();
  setSymbolToFunctionMap(BF->getSymbol(), BF);
  return BF;
}

std::pair<size_t, size_t>
BinaryContext::calculateEmittedSize(BinaryFunction &BF) {
  // Adjust branch instruction to match the current layout.
  BF.fixBranches();

  // Create local MC context to isolate the effect of ephemeral code emission.
  std::unique_ptr<MCObjectFileInfo> LocalMOFI =
    llvm::make_unique<MCObjectFileInfo>();
  std::unique_ptr<MCContext> LocalCtx =
    llvm::make_unique<MCContext>(AsmInfo.get(), MRI.get(), LocalMOFI.get());
  LocalMOFI->InitMCObjectFileInfo(*TheTriple, /*PIC=*/false, *LocalCtx);
  auto *MAB = TheTarget->createMCAsmBackend(*STI, *MRI, MCTargetOptions());
  auto *MCE = TheTarget->createMCCodeEmitter(*MII, *MRI, *LocalCtx);
  SmallString<256> Code;
  raw_svector_ostream VecOS(Code);

  std::unique_ptr<MCStreamer> Streamer(TheTarget->createMCObjectStreamer(
      *TheTriple, *LocalCtx, std::unique_ptr<MCAsmBackend>(MAB), VecOS,
      std::unique_ptr<MCCodeEmitter>(MCE), *STI,
      /* RelaxAll */ false,
      /* IncrementalLinkerCompatible */ false,
      /* DWARFMustBeAtTheEnd */ false));

  Streamer->InitSections(false);

  auto *Section = LocalMOFI->getTextSection();
  Section->setHasInstructions(true);

  auto *StartLabel = LocalCtx->getOrCreateSymbol("__hstart");
  auto *EndLabel = LocalCtx->getOrCreateSymbol("__hend");
  auto *ColdStartLabel = LocalCtx->getOrCreateSymbol("__cstart");
  auto *ColdEndLabel = LocalCtx->getOrCreateSymbol("__cend");

  Streamer->SwitchSection(Section);
  Streamer->EmitLabel(StartLabel);
  BF.emitBody(*Streamer, /*EmitColdPart = */false, /*EmitCodeOnly = */true);
  Streamer->EmitLabel(EndLabel);

  if (BF.isSplit()) {
    auto *ColdSection =
      LocalCtx->getELFSection(BF.getColdCodeSectionName(),
                              ELF::SHT_PROGBITS,
                              ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);
    ColdSection->setHasInstructions(true);

    Streamer->SwitchSection(ColdSection);
    Streamer->EmitLabel(ColdStartLabel);
    BF.emitBody(*Streamer, /*EmitColdPart = */true, /*EmitCodeOnly = */true);
    Streamer->EmitLabel(ColdEndLabel);
  }

  // To avoid calling MCObjectStreamer::flushPendingLabels() which is private.
  Streamer->EmitBytes(StringRef(""));

  auto &Assembler =
      static_cast<MCObjectStreamer *>(Streamer.get())->getAssembler();
  MCAsmLayout Layout(Assembler);
  Assembler.layout(Layout);

  const auto HotSize = Layout.getSymbolOffset(*EndLabel) -
                       Layout.getSymbolOffset(*StartLabel);
  const auto ColdSize = BF.isSplit() ? Layout.getSymbolOffset(*ColdEndLabel) -
                                       Layout.getSymbolOffset(*ColdStartLabel)
                                     : 0ULL;

  // Clean-up the effect of the code emission.
  for (const auto &Symbol : Assembler.symbols()) {
    auto *MutableSymbol = const_cast<MCSymbol *>(&Symbol);
    MutableSymbol->setUndefined();
    MutableSymbol->setIsRegistered(false);
  }

  return std::make_pair(HotSize, ColdSize);
}

BinaryFunction *
BinaryContext::getBinaryFunctionContainingAddress(uint64_t Address,
                                                  bool CheckPastEnd,
                                                  bool UseMaxSize,
                                                  bool Shallow) {
  auto FI = BinaryFunctions.upper_bound(Address);
  if (FI == BinaryFunctions.begin())
    return nullptr;
  --FI;

  const auto UsedSize = UseMaxSize ? FI->second.getMaxSize()
                                   : FI->second.getSize();

  if (Address >= FI->first + UsedSize + (CheckPastEnd ? 1 : 0))
    return nullptr;

  auto *BF = &FI->second;
  if (Shallow)
    return BF;

  while (BF->getParentFunction())
    BF = BF->getParentFunction();

  return BF;
}

BinaryFunction *
BinaryContext::getBinaryFunctionAtAddress(uint64_t Address, bool Shallow) {
  if (const auto *BD = getBinaryDataAtAddress(Address)) {
    if (auto *BF = getFunctionForSymbol(BD->getSymbol())) {
      while (BF->getParentFunction() && !Shallow) {
        BF = BF->getParentFunction();
      }
      return BF;
    }
  }
  return nullptr;
}

DebugAddressRangesVector BinaryContext::translateModuleAddressRanges(
      const DWARFAddressRangesVector &InputRanges) const {
  DebugAddressRangesVector OutputRanges;

  for (const auto Range : InputRanges) {
    auto BFI = BinaryFunctions.lower_bound(Range.LowPC);
    while (BFI != BinaryFunctions.end()) {
      const auto &Function = BFI->second;
      if (Function.getAddress() >= Range.HighPC)
        break;
      const auto FunctionRanges = Function.getOutputAddressRanges();
      std::move(std::begin(FunctionRanges),
                std::end(FunctionRanges),
                std::back_inserter(OutputRanges));
      std::advance(BFI, 1);
    }
  }

  return OutputRanges;
}
