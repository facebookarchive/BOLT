//===-- DataReader.cpp - Perf data reader -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions reads profile data written by the perf2flo
// utility and stores it in memory for llvm-flo consumption.
//
//===----------------------------------------------------------------------===//


#include "DataReader.h"

namespace llvm {
namespace flo {

ErrorOr<std::unique_ptr<DataReader>>
DataReader::readPerfData(StringRef Path, raw_ostream &Diag) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(Path);
  if (std::error_code EC = MB.getError()) {
    Diag << "Cannot open " << Path << ": " << EC.message() << "\n";
  }
  auto DR = make_unique<DataReader>(std::move(MB.get()), Diag);
  DR->parse();
  return std::move(DR);
}

void DataReader::reportError(StringRef ErrorMsg) {
  Diag << "Error reading flo data input file: line " << Line << ", column "
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

ErrorOr<StringRef> DataReader::parseString(char EndChar) {
  auto StringEnd = ParsingBuf.find(EndChar);
  if (StringEnd == StringRef::npos || StringEnd == 0) {
    reportError("malformed field");
    return make_error_code(llvm::errc::io_error);
  }
  StringRef Str = ParsingBuf.substr(0, StringEnd);
  ParsingBuf = ParsingBuf.drop_front(StringEnd + 1);
  Col += StringEnd + 1;
  return Str;
}

ErrorOr<int64_t> DataReader::parseNumberField(char EndChar) {
  auto NumStrRes = parseString(EndChar);
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

ErrorOr<Location> DataReader::parseLocation() {
  // Read whether the location of the branch should be DSO or a symbol
  if (ParsingBuf[0] != '0' && ParsingBuf[0] != '1') {
    reportError("expected 0 or 1");
    return make_error_code(llvm::errc::io_error);
  }

  bool IsSymbol = ParsingBuf[0] == '1';
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
  auto OffsetStrRes = parseString(FieldSeparator);
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

ErrorOr<BranchInfo> DataReader::parseBranchInfo() {
  auto Res = parseLocation();
  if (std::error_code EC = Res.getError())
    return EC;
  Location From = Res.get();

  Res = parseLocation();
  if (std::error_code EC = Res.getError())
    return EC;
  Location To = Res.get();

  auto MRes = parseNumberField(FieldSeparator);
  if (std::error_code EC = MRes.getError())
    return EC;
  int64_t NumMispreds = MRes.get();

  auto BRes = parseNumberField('\n');
  if (std::error_code EC = BRes.getError())
    return EC;
  int64_t NumBranches = BRes.get();

  return BranchInfo(std::move(From), std::move(To), NumMispreds, NumBranches);
}

bool DataReader::hasData() {
  if (ParsingBuf.size() == 0)
    return false;

  if (ParsingBuf[0] == '0' || ParsingBuf[0] == '1')
    return true;
  return false;
}

std::error_code DataReader::parse() {
  Col = 0;
  Line = 1;
  while (hasData()) {
    auto Res = parseBranchInfo();
    if (std::error_code EC = Res.getError())
      return EC;
    Col = 0;
    Line += 1;
    BranchInfo BI = Res.get();
    ParsedData.emplace_back(std::move(BI));
  }
  return std::error_code();
}

void DataReader::dump() {
  for (auto &BI : ParsedData) {
    Diag << BI.From.Name << " " << BI.From.Offset << " " << BI.To.Name << " "
         << BI.To.Offset << " " << BI.Mispreds << " " << BI.Branches << "\n";
  }
}
}
}
