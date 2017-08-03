//===-- DataReader.cpp - Perf data reader -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions reads profile data written by the perf2bolt
// utility and stores it in memory for llvm-bolt consumption.
//
//===----------------------------------------------------------------------===//


#include "DataReader.h"
#include "llvm/Support/Debug.h"
#include <map>

namespace llvm {
namespace bolt {

Optional<StringRef> getLTOCommonName(const StringRef Name) {
  auto LTOSuffixPos = Name.find(".lto_priv.");
  if (LTOSuffixPos != StringRef::npos) {
    return Name.substr(0, LTOSuffixPos + 10);
  } else if ((LTOSuffixPos = Name.find(".constprop.")) != StringRef::npos) {
    return Name.substr(0, LTOSuffixPos + 11);
  } else {
    return NoneType();
  }
}

namespace {

/// Return standard name of the function possibly renamed by BOLT.
StringRef normalizeName(StringRef Name) {
  // Strip "PG." prefix used for globalized locals.
  return Name.startswith("PG.") ? Name.substr(2) : Name;
}

} // anonymous namespace

iterator_range<FuncBranchData::ContainerTy::const_iterator>
FuncBranchData::getBranchRange(uint64_t From) const {
  assert(std::is_sorted(Data.begin(), Data.end()));
  struct Compare {
    bool operator()(const BranchInfo &BI, const uint64_t Val) const {
      return BI.From.Offset < Val;
    }
    bool operator()(const uint64_t Val, const BranchInfo &BI) const {
      return Val < BI.From.Offset;
    }
  };
  auto Range = std::equal_range(Data.begin(), Data.end(), From, Compare());
  return iterator_range<ContainerTy::const_iterator>(Range.first, Range.second);
}

void FuncBranchData::appendFrom(const FuncBranchData &FBD, uint64_t Offset) {
  Data.insert(Data.end(), FBD.Data.begin(), FBD.Data.end());
  for (auto I = Data.begin(), E = Data.end(); I != E; ++I) {
    if (I->From.Name == FBD.Name) {
      I->From.Name = this->Name;
      I->From.Offset += Offset;
    }
    if (I->To.Name == FBD.Name) {
      I->To.Name = this->Name;
      I->To.Offset += Offset;
    }
  }
  std::stable_sort(Data.begin(), Data.end());
  ExecutionCount += FBD.ExecutionCount;
  for (auto I = FBD.EntryData.begin(), E = FBD.EntryData.end(); I != E; ++I) {
    assert(I->To.Name == FBD.Name);
    auto NewElmt = EntryData.insert(EntryData.end(), *I);
    NewElmt->To.Name = this->Name;
    NewElmt->To.Offset += Offset;
  }
}

void BranchInfo::mergeWith(const BranchInfo &BI) {

  // Merge branch and misprediction counts.
  Branches += BI.Branches;
  Mispreds += BI.Mispreds;

  // Trivial cases
  if (BI.Histories.size() == 0)
    return;

  if (Histories.size() == 0) {
    Histories = BI.Histories;
    return;
  }

  // map BranchContext -> (mispreds, count), used to merge histories
  std::map<BranchContext, std::pair<uint64_t, uint64_t>> HistMap;

  // Add histories of this BranchInfo into HistMap.
  for (const auto &H : Histories) {
    BranchContext C;
    for (const auto &LocPair : H.Context) {
      C.emplace_back(LocPair);
      const auto I = HistMap.find(C);
      if (I == HistMap.end()) {
        HistMap.insert(
            std::make_pair(C, std::make_pair(H.Mispreds, H.Branches)));
      }
      else {
        I->second.first += H.Mispreds;
        I->second.second += H.Branches;
      }
    }
  }

  // Add histories of BI into HistMap.
  for (const auto &H : BI.Histories) {
    BranchContext C;
    for (const auto &LocPair : H.Context) {
      C.emplace_back(LocPair);
      const auto I = HistMap.find(C);
      if (I == HistMap.end()) {
        HistMap.insert(
            std::make_pair(C, std::make_pair(H.Mispreds, H.Branches)));
      }
      else {
        I->second.first += H.Mispreds;
        I->second.second += H.Branches;
      }
    }
  }

  // Helper function that checks whether context A is a prefix of context B.
  auto isPrefix = [] (const BranchContext &A, const BranchContext &B) -> bool {
    for (unsigned i = 0; i < A.size(); ++i) {
      if (i >= B.size() || A[i] != B[i])
        return false;
    }
    return true;
  };

  // Extract merged histories from HistMap. Keep only the longest history
  // between histories that share a common prefix.
  Histories.clear();
  auto I = HistMap.begin(), E = HistMap.end();
  auto NextI = I;
  ++NextI;
  for ( ; I != E; ++I, ++NextI) {
    if (NextI != E && isPrefix(I->first, NextI->first))
      continue;

    Histories.emplace_back(BranchHistory(I->second.first,
                                         I->second.second,
                                         I->first));
  }
}

void BranchInfo::print(raw_ostream &OS) const {
  OS << From.IsSymbol << " " << From.Name << " "
     << Twine::utohexstr(From.Offset) << " "
     << To.IsSymbol << " " << To.Name << " "
     << Twine::utohexstr(To.Offset) << " "
     << Mispreds << " " << Branches;

  if (Histories.size() == 0) {
    OS << "\n";
    return;
  }

  OS << " " << Histories.size() << "\n";
  for (const auto &H : Histories) {
    OS << H.Mispreds << " " << H.Branches << " " << H.Context.size() << "\n";
    for (const auto &C : H.Context) {
      OS << C.first.IsSymbol << " " << C.first.Name << " "
         << Twine::utohexstr(C.first.Offset) << " "
         << C.second.IsSymbol << " " << C.second.Name << " "
         << Twine::utohexstr(C.second.Offset) << "\n";
    }
  }
}

ErrorOr<const BranchInfo &> FuncBranchData::getBranch(uint64_t From,
                                                      uint64_t To) const {
  for (const auto &I : Data) {
    if (I.From.Offset == From && I.To.Offset == To &&
        I.From.Name == I.To.Name)
      return I;
  }
  return make_error_code(llvm::errc::invalid_argument);
}

ErrorOr<const BranchInfo &>
FuncBranchData::getDirectCallBranch(uint64_t From) const {
  // Commented out because it can be expensive.
  // assert(std::is_sorted(Data.begin(), Data.end()));
  struct Compare {
    bool operator()(const BranchInfo &BI, const uint64_t Val) const {
      return BI.From.Offset < Val;
    }
    bool operator()(const uint64_t Val, const BranchInfo &BI) const {
      return Val < BI.From.Offset;
    }
  };
  auto Range = std::equal_range(Data.begin(), Data.end(), From, Compare());
  for (auto I = Range.first; I != Range.second; ++I) {
    if (I->From.Name != I->To.Name)
      return *I;
  }
  return make_error_code(llvm::errc::invalid_argument);
}

ErrorOr<std::unique_ptr<DataReader>>
DataReader::readPerfData(StringRef Path, raw_ostream &Diag) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(Path);
  if (std::error_code EC = MB.getError()) {
    Diag << "Cannot open " << Path << ": " << EC.message() << "\n";
  }
  auto DR = make_unique<DataReader>(std::move(MB.get()), Diag);
  DR->parse();
  DR->buildLTONameMap();
  return std::move(DR);
}

void DataReader::reportError(StringRef ErrorMsg) {
  Diag << "Error reading bolt data input file: line " << Line << ", column "
       << Col << ": " << ErrorMsg << '\n';
}

bool DataReader::expectAndConsumeFS() {
  if (ParsingBuf[0] != FieldSeparator) {
    reportError("expected field separator");
    return false;
  }
  ParsingBuf = ParsingBuf.drop_front(1);
  Col += 1;
  return true;
}

bool DataReader::checkAndConsumeNewLine() {
  if (ParsingBuf[0] != '\n')
    return false;

  ParsingBuf = ParsingBuf.drop_front(1);
  Col = 0;
  Line += 1;
  return true;
}

ErrorOr<StringRef> DataReader::parseString(char EndChar, bool EndNl) {
  std::string EndChars(1, EndChar);
  if (EndNl)
    EndChars.push_back('\n');
  auto StringEnd = ParsingBuf.find_first_of(EndChars);
  if (StringEnd == StringRef::npos || StringEnd == 0) {
    reportError("malformed field");
    return make_error_code(llvm::errc::io_error);
  }

  StringRef Str = ParsingBuf.substr(0, StringEnd);

  // If EndNl was set and nl was found instead of EndChar, do not consume the
  // new line.
  bool EndNlInstreadOfEndChar =
    ParsingBuf[StringEnd] == '\n' && EndChar != '\n';
  unsigned End = EndNlInstreadOfEndChar ? StringEnd : StringEnd + 1;

  ParsingBuf = ParsingBuf.drop_front(End);
  if (EndChar == '\n') {
    Col = 0;
    Line += 1;
  } else {
    Col += End;
  }
  return Str;
}

ErrorOr<int64_t> DataReader::parseNumberField(char EndChar, bool EndNl) {
  auto NumStrRes = parseString(EndChar, EndNl);
  if (std::error_code EC = NumStrRes.getError())
    return EC;
  StringRef NumStr = NumStrRes.get();
  int64_t Num;
  if (NumStr.getAsInteger(10, Num)) {
    reportError("expected decimal number");
    Diag << "Found: " << NumStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }
  return Num;
}

ErrorOr<Location> DataReader::parseLocation(char EndChar, bool EndNl) {
  // Read whether the location of the branch should be DSO or a symbol
  // 0 means it is a DSO. 1 means it is a global symbol. 2 means it is a local
  // symbol.
  if (ParsingBuf[0] != '0' && ParsingBuf[0] != '1' && ParsingBuf[0] != '2') {
    reportError("expected 0, 1 or 2");
    return make_error_code(llvm::errc::io_error);
  }

  bool IsSymbol = ParsingBuf[0] == '1' || ParsingBuf[0] == '2';
  ParsingBuf = ParsingBuf.drop_front(1);
  Col += 1;

  if (!expectAndConsumeFS())
    return make_error_code(llvm::errc::io_error);

  // Read the string containing the symbol or the DSO name
  auto NameRes = parseString(FieldSeparator);
  if (std::error_code EC = NameRes.getError())
    return EC;
  StringRef Name = NameRes.get();

  // Read the offset
  auto OffsetStrRes = parseString(EndChar, EndNl);
  if (std::error_code EC = OffsetStrRes.getError())
    return EC;
  StringRef OffsetStr = OffsetStrRes.get();
  uint64_t Offset;
  if (OffsetStr.getAsInteger(16, Offset)) {
    reportError("expected hexadecimal number");
    Diag << "Found: " << OffsetStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }

  return Location(IsSymbol, Name, Offset);
}

ErrorOr<BranchHistory> DataReader::parseBranchHistory() {
  auto MRes = parseNumberField(FieldSeparator);
  if (std::error_code EC = MRes.getError())
    return EC;
  int64_t NumMispreds = MRes.get();

  auto BRes = parseNumberField(FieldSeparator);
  if (std::error_code EC = BRes.getError())
    return EC;
  int64_t NumBranches = BRes.get();

  auto LRes = parseNumberField('\n');
  if (std::error_code EC = LRes.getError())
    return EC;
  int64_t ContextLength = LRes.get();
  assert(ContextLength > 0 && "found branch context with length 0");

  BranchContext Context;
  for (unsigned i = 0; i < ContextLength; ++i) {
    auto Res = parseLocation(FieldSeparator);
    if (std::error_code EC = Res.getError())
      return EC;
    Location CtxFrom = Res.get();

    Res = parseLocation('\n');
    if (std::error_code EC = Res.getError())
      return EC;
    Location CtxTo = Res.get();

    Context.emplace_back(std::make_pair(std::move(CtxFrom),
                                        std::move(CtxTo)));
  }

  return BranchHistory(NumMispreds, NumBranches, std::move(Context));
}

ErrorOr<BranchInfo> DataReader::parseBranchInfo() {
  auto Res = parseLocation(FieldSeparator);
  if (std::error_code EC = Res.getError())
    return EC;
  Location From = Res.get();

  Res = parseLocation(FieldSeparator);
  if (std::error_code EC = Res.getError())
    return EC;
  Location To = Res.get();

  auto MRes = parseNumberField(FieldSeparator);
  if (std::error_code EC = MRes.getError())
    return EC;
  int64_t NumMispreds = MRes.get();

  auto BRes = parseNumberField(FieldSeparator, /* EndNl = */ true);
  if (std::error_code EC = BRes.getError())
    return EC;
  int64_t NumBranches = BRes.get();

  BranchHistories Histories;

  if (!checkAndConsumeNewLine()) {
    auto HRes = parseNumberField('\n');
    if (std::error_code EC = HRes.getError())
      return EC;
    int64_t NumHistories = HRes.get();
    assert(NumHistories > 0 && "found branch history list with length 0");

    for (unsigned i = 0; i < NumHistories; ++i) {
      auto Res = parseBranchHistory();
      if (std::error_code EC = Res.getError())
        return EC;
      BranchHistory Hist = Res.get();

      Histories.emplace_back(std::move(Hist));
    }
  }

  return BranchInfo(std::move(From), std::move(To), NumMispreds, NumBranches,
                    std::move(Histories));
}

bool DataReader::hasData() {
  if (ParsingBuf.size() == 0)
    return false;

  if (ParsingBuf[0] == '0' || ParsingBuf[0] == '1' || ParsingBuf[0] == '2')
    return true;
  return false;
}

std::error_code DataReader::parse() {
  auto GetOrCreateFuncEntry = [&](StringRef Name) {
    auto I = FuncsMap.find(Name);
    if (I == FuncsMap.end()) {
      bool success;
      std::tie(I, success) = FuncsMap.insert(
          std::make_pair(Name, FuncBranchData(Name,
                                              FuncBranchData::ContainerTy(),
                                              FuncBranchData::ContainerTy())));
      assert(success && "unexpected result of insert");
    }
    return I;
  };

  Col = 0;
  Line = 1;
  while (hasData()) {
    auto Res = parseBranchInfo();
    if (std::error_code EC = Res.getError())
      return EC;

    BranchInfo BI = Res.get();

    // Ignore branches not involving known location.
    if (!BI.From.IsSymbol && !BI.To.IsSymbol)
      continue;

    auto I = GetOrCreateFuncEntry(BI.From.Name);
    I->getValue().Data.emplace_back(std::move(BI));

    // Add entry data for branches to another function or branches
    // to entry points (including recursive calls)
    if (BI.To.IsSymbol &&
        (!BI.From.Name.equals(BI.To.Name) || BI.To.Offset == 0)) {
      I = GetOrCreateFuncEntry(BI.To.Name);
      I->getValue().EntryData.emplace_back(std::move(BI));
    }

    // If destination is the function start - update execution count.
    // NB: the data is skewed since we cannot tell tail recursion from
    //     branches to the function start.
    if (BI.To.IsSymbol && BI.To.Offset == 0) {
      I = GetOrCreateFuncEntry(BI.To.Name);
      I->getValue().ExecutionCount += BI.Branches;
    }
  }

  for (auto &FuncBranches : FuncsMap) {
    std::stable_sort(FuncBranches.second.Data.begin(),
                     FuncBranches.second.Data.end());
  }

  return std::error_code();
}

void DataReader::buildLTONameMap() {
  for (auto &FuncData : FuncsMap) {
    const auto FuncName = FuncData.getKey();
    const auto CommonName = getLTOCommonName(FuncName);
    if (CommonName)
      LTOCommonNameMap[*CommonName].push_back(&FuncData.getValue());
  }
}

FuncBranchData *
DataReader::getFuncBranchData(const std::vector<std::string> &FuncNames)  {
  // Do a reverse order iteration since the name in profile has a higher chance
  // of matching a name at the end of the list.
  for (auto FI = FuncNames.rbegin(), FE = FuncNames.rend(); FI != FE; ++FI) {
    auto I = FuncsMap.find(normalizeName(*FI));
    if (I != FuncsMap.end())
      return &I->getValue();
  }
  return nullptr;
}

std::vector<FuncBranchData *>
DataReader::getFuncBranchDataRegex(const std::vector<std::string> &FuncNames) {
  std::vector<FuncBranchData *> AllData;
  // Do a reverse order iteration since the name in profile has a higher chance
  // of matching a name at the end of the list.
  for (auto FI = FuncNames.rbegin(), FE = FuncNames.rend(); FI != FE; ++FI) {
    StringRef Name = *FI;
    Name = normalizeName(Name);
    const auto LTOCommonName = getLTOCommonName(Name);
    if (LTOCommonName) {
      auto I = LTOCommonNameMap.find(*LTOCommonName);
      if (I != LTOCommonNameMap.end()) {
        auto &CommonData = I->getValue();
        AllData.insert(AllData.end(), CommonData.begin(), CommonData.end());
      }
    } else {
      auto I = FuncsMap.find(Name);
      if (I != FuncsMap.end()) {
        return {&I->getValue()};
      }
    }
  }
  return AllData;
}

bool DataReader::hasLocalsWithFileName() const {
  for (const auto &Func : FuncsMap) {
    const auto &FuncName = Func.getKey();
    if (FuncName.count('/') == 2 && FuncName[0] != '/')
      return true;
  }
  return false;
}

void DataReader::dump() const {
  for (const auto &Func : FuncsMap) {
    Diag << Func.getKey() << " branches:\n";
    for (const auto &BI : Func.getValue().Data) {
      Diag << BI.From.Name << " " << BI.From.Offset << " " << BI.To.Name << " "
           << BI.To.Offset << " " << BI.Mispreds << " " << BI.Branches << "\n";
      for (const auto &HI : BI.Histories) {
        Diag << "\thistory " << HI.Mispreds << " " << HI.Branches << "\n";
        for (const auto &CI : HI.Context) {
          Diag << "\t" <<  CI.first.Name << " " << CI.first.Offset << " "
                       << CI.second.Name << " " << CI.second.Offset << "\n";
        }
      }
    }
    Diag << Func.getKey() << " entry points:\n";
    for (const auto &BI : Func.getValue().EntryData) {
      Diag << BI.From.Name << " " << BI.From.Offset << " " << BI.To.Name << " "
           << BI.To.Offset << " " << BI.Mispreds << " " << BI.Branches << "\n";
      for (const auto &HI : BI.Histories) {
        Diag << "\thistory " << HI.Mispreds << " " << HI.Branches << "\n";
        for (const auto &CI : HI.Context) {
          Diag << "\t" <<  CI.first.Name << " " << CI.first.Offset << " "
                       << CI.second.Name << " " << CI.second.Offset << "\n";
        }
      }
    }
  }
}

} // namespace bolt
} // namespace llvm
