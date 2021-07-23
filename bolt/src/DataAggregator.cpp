//===-- DataAggregator.cpp - Perf data aggregator ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions reads profile data written by perf record,
// aggregate it and then write it back to an output file.
//
//===----------------------------------------------------------------------===//

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "DataAggregator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Options.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Timer.h"

#include <unistd.h>

#define DEBUG_TYPE "aggregator"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory AggregatorCategory;

static cl::opt<bool>
BasicAggregation("nl",
  cl::desc("aggregate basic samples (without LBR info)"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

static cl::opt<bool>
IgnoreBuildID("ignore-build-id",
  cl::desc("continue even if build-ids in input binary and perf.data mismatch"),
  cl::init(false),
  cl::cat(AggregatorCategory));

static cl::opt<bool>
TimeAggregator("time-aggr",
  cl::desc("time BOLT aggregator"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

}

namespace {

const char TimerGroupName[] = "aggregator";
const char TimerGroupDesc[] = "Aggregator";

}

void DataAggregator::findPerfExecutable() {
  auto PerfExecutable = sys::Process::FindInEnvPath("PATH", "perf");
  if (!PerfExecutable) {
    outs() << "PERF2BOLT: No perf executable found!\n";
    exit(1);
  }
  PerfPath = *PerfExecutable;
}

void DataAggregator::start(StringRef PerfDataFilename) {
  Enabled = true;
  this->PerfDataFilename = PerfDataFilename;
  outs() << "PERF2BOLT: Starting data aggregation job for " << PerfDataFilename
         << "\n";
  findPerfExecutable();
  launchPerfBranchEventsNoWait();
  launchPerfMemEventsNoWait();
  launchPerfTasksNoWait();
}

void DataAggregator::abort() {
  std::string Error;

  // Kill subprocesses in case they are not finished
  sys::Wait(TasksPI, 1, false, &Error);
  sys::Wait(BranchEventsPI, 1, false, &Error);
  sys::Wait(MemEventsPI, 1, false, &Error);

  deleteTempFiles();
}

bool DataAggregator::launchPerfBranchEventsNoWait() {
  SmallVector<const char*, 4> Argv;

  if (opts::BasicAggregation)
    outs()
        << "PERF2BOLT: Spawning perf-script job to read events without LBR\n";
  else
    outs() << "PERF2BOLT: Spawning perf-script job to read branch events\n";
  Argv.push_back(PerfPath.data());
  Argv.push_back("script");
  Argv.push_back("-F");
  if (opts::BasicAggregation)
    Argv.push_back("pid,event,ip");
  else
    Argv.push_back("pid,brstack");
  Argv.push_back("-i");
  Argv.push_back(PerfDataFilename.data());
  Argv.push_back(nullptr);

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "out",
                                               PerfBranchEventsOutputPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << PerfBranchEventsOutputPath << " with error " << Errc.message()
           << "\n";
    exit(1);
  }

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "err",
                                               PerfBranchEventsErrPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << PerfBranchEventsErrPath << " with error " << Errc.message()
           << "\n";
    exit(1);
  }
  Optional<StringRef> Redirects[] = {
      llvm::None,                                   // Stdin
      StringRef(PerfBranchEventsOutputPath.data()), // Stdout
      StringRef(PerfBranchEventsErrPath.data())};   // Stderr

  DEBUG(dbgs() << "Launching perf: " << PerfPath.data() << " 1> "
               << PerfBranchEventsOutputPath.data() << " 2> "
               << PerfBranchEventsErrPath.data() << "\n");

  BranchEventsPI = sys::ExecuteNoWait(PerfPath.data(), Argv.data(),
                                      /*envp*/ nullptr, Redirects);

  return true;
}

bool DataAggregator::launchPerfMemEventsNoWait() {
  SmallVector<const char*, 4> Argv;

  outs() << "PERF2BOLT: Spawning perf-script job to read mem events\n";
  Argv.push_back(PerfPath.data());
  Argv.push_back("script");
  Argv.push_back("-F");
  Argv.push_back("pid,event,addr,ip");
  Argv.push_back("-i");
  Argv.push_back(PerfDataFilename.data());
  Argv.push_back(nullptr);

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "out",
                                               PerfMemEventsOutputPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << PerfMemEventsOutputPath << " with error " << Errc.message() << "\n";
    exit(1);
  }

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "err",
                                               PerfMemEventsErrPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << PerfMemEventsErrPath << " with error " << Errc.message() << "\n";
    exit(1);
  }

  Optional<StringRef> Redirects[] = {
      llvm::None,                                // Stdin
      StringRef(PerfMemEventsOutputPath.data()), // Stdout
      StringRef(PerfMemEventsErrPath.data())};   // Stderr

  DEBUG(dbgs() << "Launching perf: " << PerfPath.data() << " 1> "
               << PerfMemEventsOutputPath.data() << " 2> "
               << PerfMemEventsErrPath.data() << "\n");

  MemEventsPI = sys::ExecuteNoWait(PerfPath.data(), Argv.data(),
                                   /*envp*/ nullptr, Redirects);

  return true;
}

bool DataAggregator::launchPerfTasksNoWait() {
  SmallVector<const char*, 4> Argv;

  outs() << "PERF2BOLT: Spawning perf-script job to read tasks\n";
  Argv.push_back(PerfPath.data());
  Argv.push_back("script");
  Argv.push_back("--show-task-events");
  Argv.push_back("-i");
  Argv.push_back(PerfDataFilename.data());
  Argv.push_back(nullptr);

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "out",
                                               PerfTasksOutputPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << PerfTasksOutputPath << " with error " << Errc.message() << "\n";
    exit(1);
  }

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "err",
                                               PerfTasksErrPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << PerfTasksErrPath << " with error " << Errc.message() << "\n";
    exit(1);
  }

  Optional<StringRef> Redirects[] = {
      llvm::None,                            // Stdin
      StringRef(PerfTasksOutputPath.data()), // Stdout
      StringRef(PerfTasksErrPath.data())};   // Stderr

  DEBUG(dbgs() << "Launching perf: " << PerfPath.data() << " 1> "
               << PerfTasksOutputPath.data() << " 2> "
               << PerfTasksErrPath.data() << "\n");

  TasksPI = sys::ExecuteNoWait(PerfPath.data(), Argv.data(),
                               /*envp*/ nullptr, Redirects);

  return true;
}

void DataAggregator::processFileBuildID(StringRef FileBuildID) {
  SmallVector<const char *, 4> Argv;
  SmallVector<char, 256> OutputPath;
  SmallVector<char, 256> ErrPath;

  Argv.push_back(PerfPath.data());
  Argv.push_back("buildid-list");
  Argv.push_back("-i");
  Argv.push_back(PerfDataFilename.data());
  Argv.push_back(nullptr);

  if (auto Errc = sys::fs::createTemporaryFile("perf.buildid", "out",
                                               OutputPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << OutputPath << " with error " << Errc.message() << "\n";
    exit(1);
  }

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "err",
                                               ErrPath)) {
    outs() << "PERF2BOLT: Failed to create temporary file "
           << ErrPath << " with error " << Errc.message() << "\n";
    exit(1);
  }

  Optional<StringRef> Redirects[] = {
      llvm::None,                   // Stdin
      StringRef(OutputPath.data()), // Stdout
      StringRef(ErrPath.data())};   // Stderr

  DEBUG(dbgs() << "Launching perf: " << PerfPath.data() << " 1> "
               << OutputPath.data() << " 2> "
               << ErrPath.data() << "\n");

  auto RetCode = sys::ExecuteAndWait(PerfPath.data(), Argv.data(),
                                     /*envp*/ nullptr, Redirects);

  if (RetCode != 0) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(ErrPath.data());
    StringRef ErrBuf = (*MB)->getBuffer();

    errs() << "PERF-ERROR: Return code " << RetCode << "\n";
    errs() << ErrBuf;
    deleteTempFile(ErrPath.data());
    deleteTempFile(OutputPath.data());
    return;
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
    MemoryBuffer::getFileOrSTDIN(OutputPath.data());
  if (std::error_code EC = MB.getError()) {
    errs() << "Cannot open " << PerfTasksOutputPath.data() << ": "
           << EC.message() << "\n";
    deleteTempFile(ErrPath.data());
    deleteTempFile(OutputPath.data());
    return;
  }

  FileBuf.reset(MB->release());
  ParsingBuf = FileBuf->getBuffer();
  if (ParsingBuf.empty()) {
    errs() << "PERF2BOLT-WARNING: build-id will not be checked because perf "
              "data was recorded without it\n";
    deleteTempFile(ErrPath.data());
    deleteTempFile(OutputPath.data());
    return;
  }

  Col = 0;
  Line = 1;
  auto FileName = getFileNameForBuildID(FileBuildID);
  if (!FileName) {
    errs() << "PERF2BOLT-ERROR: failed to match build-id from perf output. "
              "This indicates the input binary supplied for data aggregation "
              "is not the same recorded by perf when collecting profiling "
              "data. Use -ignore-build-id option to override.\n";
    if (!opts::IgnoreBuildID) {
      deleteTempFile(ErrPath.data());
      deleteTempFile(OutputPath.data());
      abort();
      exit(1);
    }
  } else if (*FileName != BinaryName) {
    errs() << "PERF2BOLT-WARNING: build-id matched a different file name. "
              "Using \"" << *FileName << "\" for profile parsing.\n";
    BinaryName = *FileName;
  } else {
    outs() << "PERF2BOLT: matched build-id and file name\n";
  }

  deleteTempFile(ErrPath.data());
  deleteTempFile(OutputPath.data());
  return;
}

bool DataAggregator::checkPerfDataMagic(StringRef FileName) {
  int FD;
  if (sys::fs::openFileForRead(FileName, FD)) {
    return false;
  }

  char Buf[7] = {0, 0, 0, 0, 0, 0, 0};

  if (::read(FD, Buf, 7) == -1) {
    ::close(FD);
    return false;
  }
  ::close(FD);

  if (strncmp(Buf, "PERFILE", 7) == 0)
    return true;
  return false;
}

void DataAggregator::deleteTempFile(StringRef File) {
  if (auto Errc = sys::fs::remove(File.data())) {
    outs() << "PERF2BOLT: Failed to delete temporary file "
           << File << " with error " << Errc.message() << "\n";
  }
}

void DataAggregator::deleteTempFiles() {
  deleteTempFile(PerfBranchEventsErrPath.data());
  deleteTempFile(PerfBranchEventsOutputPath.data());
  deleteTempFile(PerfMemEventsErrPath.data());
  deleteTempFile(PerfMemEventsOutputPath.data());
  deleteTempFile(PerfTasksErrPath.data());
  deleteTempFile(PerfTasksOutputPath.data());
}

bool DataAggregator::aggregate(BinaryContext &BC,
                               std::map<uint64_t, BinaryFunction> &BFs) {
  std::string Error;

  this->BC = &BC;
  this->BFs = &BFs;

  outs() << "PERF2BOLT: Waiting for perf tasks collection to finish...\n";
  auto PI1 = sys::Wait(TasksPI, 0, true, &Error);

  if (!Error.empty()) {
    errs() << "PERF-ERROR: " << Error << "\n";
    deleteTempFiles();
    exit(1);
  }

  if (PI1.ReturnCode != 0) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(PerfTasksErrPath.data());
    StringRef ErrBuf = (*MB)->getBuffer();

    errs() << "PERF-ERROR: Return code " << PI1.ReturnCode << "\n";
    errs() << ErrBuf;
    deleteTempFiles();
    exit(1);
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MB1 =
    MemoryBuffer::getFileOrSTDIN(PerfTasksOutputPath.data());
  if (std::error_code EC = MB1.getError()) {
    errs() << "Cannot open " << PerfTasksOutputPath.data() << ": "
           << EC.message() << "\n";
    deleteTempFiles();
    exit(1);
  }

  FileBuf.reset(MB1->release());
  ParsingBuf = FileBuf->getBuffer();
  Col = 0;
  Line = 1;
  if (parseTasks()) {
    outs() << "PERF2BOLT: Failed to parse tasks\n";
  }

  outs()
      << "PERF2BOLT: Waiting for perf events collection to finish...\n";
  auto PI2 = sys::Wait(BranchEventsPI, 0, true, &Error);

  if (!Error.empty()) {
    errs() << "PERF-ERROR: " << Error << "\n";
    deleteTempFiles();
    exit(1);
  }

  if (PI2.ReturnCode != 0) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(PerfBranchEventsErrPath.data());
    StringRef ErrBuf = (*MB)->getBuffer();

    errs() << "PERF-ERROR: Return code " << PI2.ReturnCode << "\n";
    errs() << ErrBuf;
    deleteTempFiles();
    exit(1);
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MB2 =
    MemoryBuffer::getFileOrSTDIN(PerfBranchEventsOutputPath.data());
  if (std::error_code EC = MB2.getError()) {
    errs() << "Cannot open " << PerfBranchEventsOutputPath.data() << ": "
           << EC.message() << "\n";
    deleteTempFiles();
    exit(1);
  }

  FileBuf.reset(MB2->release());
  ParsingBuf = FileBuf->getBuffer();
  Col = 0;
  Line = 1;
  if ((!opts::BasicAggregation && parseBranchEvents()) ||
      (opts::BasicAggregation && parseBasicEvents())) {
    outs() << "PERF2BOLT: Failed to parse samples\n";
  }

  // Mark all functions with registered events as having a valid profile.
  for (auto &BFI : BFs) {
    auto &BF = BFI.second;
    if (BF.getBranchData()) {
      const auto Flags = opts::BasicAggregation ? BinaryFunction::PF_SAMPLE
                                                : BinaryFunction::PF_LBR;
      BF.markProfiled(Flags);
    }
  }

  auto PI3 = sys::Wait(MemEventsPI, 0, true, &Error);
  if (PI3.ReturnCode != 0) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(PerfMemEventsErrPath.data());
    StringRef ErrBuf = (*MB)->getBuffer();

    deleteTempFiles();

    Regex NoData("Samples for '.*' event do not have ADDR attribute set. "
                 "Cannot print 'addr' field.");
    if (!NoData.match(ErrBuf)) {
      errs() << "PERF-ERROR: Return code " << PI3.ReturnCode << "\n";
      errs() << ErrBuf;
      exit(1);
    }
    return true;
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MB3 =
    MemoryBuffer::getFileOrSTDIN(PerfMemEventsOutputPath.data());
  if (std::error_code EC = MB3.getError()) {
    errs() << "Cannot open " << PerfMemEventsOutputPath.data() << ": "
           << EC.message() << "\n";
    deleteTempFiles();
    exit(1);
  }

  FileBuf.reset(MB3->release());
  ParsingBuf = FileBuf->getBuffer();
  Col = 0;
  Line = 1;
  if (const auto EC = parseMemEvents()) {
    errs() << "PERF2BOLT: Failed to parse memory events: "
           << EC.message() << '\n';
  }

  deleteTempFiles();

  return true;
}

BinaryFunction *
DataAggregator::getBinaryFunctionContainingAddress(uint64_t Address) {
  auto FI = BFs->upper_bound(Address);
  if (FI == BFs->begin())
    return nullptr;
  --FI;

  const auto UsedSize = FI->second.getMaxSize();
  if (Address >= FI->first + UsedSize)
    return nullptr;
  return &FI->second;
}

bool
DataAggregator::doSample(BinaryFunction &Func, uint64_t Address) {
  auto I = FuncsToSamples.find(Func.getNames()[0]);
  if (I == FuncsToSamples.end()) {
    bool Success;
    std::tie(I, Success) = FuncsToSamples.insert(std::make_pair(
        Func.getNames()[0],
        FuncSampleData(Func.getNames()[0], FuncSampleData::ContainerTy())));
  }

  I->second.bumpCount(Address - Func.getAddress());
  return true;
}

bool
DataAggregator::doIntraBranch(BinaryFunction &Func, const LBREntry &Branch) {
  FuncBranchData *AggrData = Func.getBranchData();
  if (!AggrData) {
    AggrData = &FuncsToBranches[Func.getNames()[0]];
    AggrData->Name = Func.getNames()[0];
    Func.setBranchData(AggrData);
  }

  AggrData->bumpBranchCount(Branch.From - Func.getAddress(),
                            Branch.To - Func.getAddress(),
                            Branch.Mispred);
  return true;
}

bool DataAggregator::doInterBranch(BinaryFunction *FromFunc,
                                   BinaryFunction *ToFunc,
                                   const LBREntry &Branch) {
  FuncBranchData *FromAggrData{nullptr};
  FuncBranchData *ToAggrData{nullptr};
  StringRef SrcFunc;
  StringRef DstFunc;
  auto From = Branch.From;
  auto To = Branch.To;
  if (FromFunc) {
    SrcFunc = FromFunc->getNames()[0];
    FromAggrData = FromFunc->getBranchData();
    if (!FromAggrData) {
      FromAggrData = &FuncsToBranches[SrcFunc];
      FromAggrData->Name = SrcFunc;
      FromFunc->setBranchData(FromAggrData);
    }
    From -= FromFunc->getAddress();

    FromFunc->recordExit(From, Branch.Mispred);
  }
  if (ToFunc) {
    DstFunc = ToFunc->getNames()[0];
    ToAggrData = ToFunc->getBranchData();
    if (!ToAggrData) {
      ToAggrData = &FuncsToBranches[DstFunc];
      ToAggrData->Name = DstFunc;
      ToFunc->setBranchData(ToAggrData);
    }
    To -= ToFunc->getAddress();

    ToFunc->recordEntry(To, Branch.Mispred);
  }

  if (FromAggrData)
    FromAggrData->bumpCallCount(From, Location(!DstFunc.empty(), DstFunc, To),
                                Branch.Mispred);
  if (ToAggrData)
    ToAggrData->bumpEntryCount(Location(!SrcFunc.empty(), SrcFunc, From), To,
                               Branch.Mispred);
  return true;
}

bool DataAggregator::doBranch(const LBREntry &Branch) {
  auto *FromFunc = getBinaryFunctionContainingAddress(Branch.From);
  auto *ToFunc = getBinaryFunctionContainingAddress(Branch.To);
  if (!FromFunc && !ToFunc)
    return false;

  if (FromFunc == ToFunc) {
    FromFunc->recordBranch(Branch.From - FromFunc->getAddress(),
                           Branch.To - FromFunc->getAddress(),
                           1,
                           Branch.Mispred);
    return doIntraBranch(*FromFunc, Branch);
  }

  return doInterBranch(FromFunc, ToFunc, Branch);
}

bool DataAggregator::doTrace(const LBREntry &First, const LBREntry &Second) {
  auto *FromFunc = getBinaryFunctionContainingAddress(First.To);
  auto *ToFunc = getBinaryFunctionContainingAddress(Second.From);
  if (!FromFunc || !ToFunc) {
    ++NumLongRangeTraces;
    return false;
  }
  if (FromFunc != ToFunc) {
    ++NumInvalidTraces;
    DEBUG(dbgs() << "Trace starting in " << FromFunc->getPrintName() << " @ "
                 << Twine::utohexstr(First.To - FromFunc->getAddress())
                 << " and ending in " << ToFunc->getPrintName() << " @ "
                 << ToFunc->getPrintName() << " @ "
                 << Twine::utohexstr(Second.From - ToFunc->getAddress())
                 << '\n');
    return false;
  }

  auto FTs = FromFunc->getFallthroughsInTrace(First, Second);
  if (!FTs) {
    ++NumInvalidTraces;
    return false;
  }

  for (const auto &Pair : *FTs) {
    doIntraBranch(*FromFunc,
                  LBREntry{Pair.first + FromFunc->getAddress(),
                           Pair.second + FromFunc->getAddress(),
                           false});
  }

  return true;
}

ErrorOr<LBREntry> DataAggregator::parseLBREntry() {
  LBREntry Res;
  auto FromStrRes = parseString('/');
  if (std::error_code EC = FromStrRes.getError())
    return EC;
  StringRef OffsetStr = FromStrRes.get();
  if (OffsetStr.getAsInteger(0, Res.From)) {
    reportError("expected hexadecimal number with From address");
    Diag << "Found: " << OffsetStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }

  auto ToStrRes = parseString('/');
  if (std::error_code EC = ToStrRes.getError())
    return EC;
  OffsetStr = ToStrRes.get();
  if (OffsetStr.getAsInteger(0, Res.To)) {
    reportError("expected hexadecimal number with To address");
    Diag << "Found: " << OffsetStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }

  auto MispredStrRes = parseString('/');
  if (std::error_code EC = MispredStrRes.getError())
    return EC;
  StringRef MispredStr = MispredStrRes.get();
  if (MispredStr.size() != 1 ||
      (MispredStr[0] != 'P' && MispredStr[0] != 'M' && MispredStr[0] != '-')) {
    reportError("expected single char for mispred bit");
    Diag << "Found: " << MispredStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }
  Res.Mispred = MispredStr[0] == 'M';

  static bool MispredWarning = true;;
  if (MispredStr[0] == '-' && MispredWarning) {
    errs() << "PERF2BOLT-WARNING: misprediction bit is missing in profile\n";
    MispredWarning = false;
  }

  auto Rest = parseString(FieldSeparator, true);
  if (std::error_code EC = Rest.getError())
    return EC;
  if (Rest.get().size() < 5) {
    reportError("expected rest of LBR entry");
    Diag << "Found: " << Rest.get() << "\n";
    return make_error_code(llvm::errc::io_error);
  }
  return Res;
}

bool DataAggregator::checkAndConsumeFS() {
  if (ParsingBuf[0] != FieldSeparator) {
    return false;
  }
  ParsingBuf = ParsingBuf.drop_front(1);
  Col += 1;
  return true;
}

void DataAggregator::consumeRestOfLine() {
  auto LineEnd = ParsingBuf.find_first_of('\n');
  if (LineEnd == StringRef::npos) {
    ParsingBuf = StringRef();
    Col = 0;
    Line += 1;
    return;
  }
  ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
  Col = 0;
  Line += 1;
}

ErrorOr<PerfBranchSample> DataAggregator::parseBranchSample() {
  PerfBranchSample Res;

  while (checkAndConsumeFS()) {}

  auto PIDRes = parseNumberField(FieldSeparator, true);
  if (std::error_code EC = PIDRes.getError())
    return EC;
  if (!PIDs.empty() && !PIDs.count(PIDRes.get())) {
    consumeRestOfLine();
    return Res;
  }

  while (!checkAndConsumeNewLine()) {
    checkAndConsumeFS();

    auto LBRRes = parseLBREntry();
    if (std::error_code EC = LBRRes.getError())
      return EC;
    Res.LBR.push_back(LBRRes.get());
  }

  return Res;
}

ErrorOr<PerfBasicSample> DataAggregator::parseBasicSample() {
  while (checkAndConsumeFS()) {}

  auto PIDRes = parseNumberField(FieldSeparator, true);
  if (std::error_code EC = PIDRes.getError())
    return EC;
  if (!PIDs.empty() && !PIDs.count(PIDRes.get())) {
    consumeRestOfLine();
    return PerfBasicSample{StringRef(), 0};
  }

  while (checkAndConsumeFS()) {}

  auto Event = parseString(FieldSeparator);
  if (std::error_code EC = Event.getError())
    return EC;

  while (checkAndConsumeFS()) {}

  auto AddrRes = parseHexField(FieldSeparator, true);
  if (std::error_code EC = AddrRes.getError()) {
    return EC;
  }

  if (!checkAndConsumeNewLine()) {
    reportError("expected end of line");
    return make_error_code(llvm::errc::io_error);
  }

  return PerfBasicSample{Event.get(), AddrRes.get()};
}

ErrorOr<PerfMemSample> DataAggregator::parseMemSample() {
  PerfMemSample Res{0,0};

  while (checkAndConsumeFS()) {}

  auto PIDRes = parseNumberField(FieldSeparator, true);
  if (std::error_code EC = PIDRes.getError())
    return EC;
  if (!PIDs.empty() && !PIDs.count(PIDRes.get())) {
    consumeRestOfLine();
    return Res;
  }

  while (checkAndConsumeFS()) {}

  auto Event = parseString(FieldSeparator);
  if (std::error_code EC = Event.getError())
    return EC;
  if (Event.get().find("mem-loads") == StringRef::npos) {
    consumeRestOfLine();
    return Res;
  }

  while (checkAndConsumeFS()) {}

  auto AddrRes = parseHexField(FieldSeparator);
  if (std::error_code EC = AddrRes.getError()) {
    return EC;
  }

  while (checkAndConsumeFS()) {}

  auto PCRes = parseHexField(FieldSeparator, true);
  if (std::error_code EC = PCRes.getError()) {
    consumeRestOfLine();
    return EC;
  }

  if (!checkAndConsumeNewLine()) {
    reportError("expected end of line");
    return make_error_code(llvm::errc::io_error);
  }

  return PerfMemSample{PCRes.get(), AddrRes.get()};
}

bool DataAggregator::hasData() {
  if (ParsingBuf.size() == 0)
    return false;

  return true;
}

std::error_code DataAggregator::parseBranchEvents() {
  outs() << "PERF2BOLT: Aggregating branch events...\n";
  NamedRegionTimer T("parseBranch", "Branch samples parsing", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);
  uint64_t NumEntries{0};
  uint64_t NumSamples{0};
  uint64_t NumTraces{0};
  while (hasData()) {
    auto SampleRes = parseBranchSample();
    if (std::error_code EC = SampleRes.getError())
      return EC;

    auto &Sample = SampleRes.get();
    if (Sample.LBR.empty())
      continue;

    ++NumSamples;
    NumEntries += Sample.LBR.size();

    // LBRs are stored in reverse execution order. NextLBR refers to the next
    // executed branch record.
    const LBREntry *NextLBR{nullptr};
    for (const auto &LBR : Sample.LBR) {
      if (NextLBR) {
        doTrace(LBR, *NextLBR);
        ++NumTraces;
      }
      doBranch(LBR);
      NextLBR = &LBR;
    }
  }
  outs() << "PERF2BOLT: Read " << NumSamples << " samples and "
         << NumEntries << " LBR entries\n";
  outs() << "PERF2BOLT: Traces mismatching disassembled function contents: "
         << NumInvalidTraces;
  float Perc{0.0f};
  if (NumTraces > 0) {
    outs() << " (";
    Perc = NumInvalidTraces * 100.0f / NumTraces;
    if (outs().has_colors()) {
      if (Perc > 10.0f) {
        outs().changeColor(raw_ostream::RED);
      } else if (Perc > 5.0f) {
        outs().changeColor(raw_ostream::YELLOW);
      } else {
        outs().changeColor(raw_ostream::GREEN);
      }
    }
    outs() << format("%.1f%%", Perc);
    if (outs().has_colors())
      outs().resetColor();
    outs() << ")";
  }
  outs() << "\n";
  if (Perc > 10.0f) {
    outs() << "\n !! WARNING !! This high mismatch ratio indicates the input "
              "binary is probably not the same binary used during profiling "
              "collection. The generated data may be ineffective for improving "
              "performance.\n\n";
  }

  outs() << "PERF2BOLT: Out of range traces involving unknown regions: "
         << NumLongRangeTraces;
  if (NumTraces > 0) {
    outs() << format(" (%.1f%%)", NumLongRangeTraces * 100.0f / NumTraces);
  }
  outs() << "\n";

  return std::error_code();
}

std::error_code DataAggregator::parseBasicEvents() {
  outs() << "PERF2BOLT: Aggregating basic events (without LBR)...\n";
  NamedRegionTimer T("parseBasic", "Perf samples parsing", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);
  uint64_t NumSamples{0};
  uint64_t OutOfRangeSamples{0};
  while (hasData()) {
    auto SampleRes = parseBasicSample();
    if (std::error_code EC = SampleRes.getError())
      return EC;

    auto &Sample = SampleRes.get();
    if (!Sample.PC)
      continue;

    ++NumSamples;
    auto *Func = getBinaryFunctionContainingAddress(Sample.PC);
    if (!Func) {
      ++OutOfRangeSamples;
      continue;
    }

    doSample(*Func, Sample.PC);
    EventNames.insert(Sample.EventName);
  }
  outs() << "PERF2BOLT: Read " << NumSamples << " samples\n";

  outs() << "PERF2BOLT: Out of range samples recorded in unknown regions: "
         << OutOfRangeSamples;
  float Perc{0.0f};
  if (NumSamples > 0) {
    outs() << " (";
    Perc = OutOfRangeSamples * 100.0f / NumSamples;
    if (outs().has_colors()) {
      if (Perc > 60.0f) {
        outs().changeColor(raw_ostream::RED);
      } else if (Perc > 40.0f) {
        outs().changeColor(raw_ostream::YELLOW);
      } else {
        outs().changeColor(raw_ostream::GREEN);
      }
    }
    outs() << format("%.1f%%", Perc);
    if (outs().has_colors())
      outs().resetColor();
    outs() << ")";
  }
  outs() << "\n";
  if (Perc > 80.0f) {
    outs() << "\n !! WARNING !! This high mismatch ratio indicates the input "
              "binary is probably not the same binary used during profiling "
              "collection. The generated data may be ineffective for improving "
              "performance.\n\n";
  }

  return std::error_code();
}

std::error_code DataAggregator::parseMemEvents() {
  outs() << "PERF2BOLT: Aggregating memory events...\n";
  NamedRegionTimer T("memevents", "Mem samples parsing", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);

  while (hasData()) {
    auto SampleRes = parseMemSample();
    if (std::error_code EC = SampleRes.getError())
      return EC;

    auto PC = SampleRes.get().PC;
    auto Addr = SampleRes.get().Addr;
    StringRef FuncName;
    StringRef MemName;

    // Try to resolve symbol for PC
    auto *Func = getBinaryFunctionContainingAddress(PC);
    if (Func) {
      FuncName = Func->getNames()[0];
      PC -= Func->getAddress();
    }

    // Try to resolve symbol for memory load
    auto *MemFunc = getBinaryFunctionContainingAddress(Addr);
    if (MemFunc) {
      MemName = MemFunc->getNames()[0];
      Addr -= MemFunc->getAddress();
    } else if (Addr) {  // TODO: filter heap/stack/nulls here?
      if (auto *BD = BC->getBinaryDataContainingAddress(Addr)) {
        MemName = BD->getName();
        Addr -= BD->getAddress();
      }
    }

    const Location FuncLoc(!FuncName.empty(), FuncName, PC);
    const Location AddrLoc(!MemName.empty(), MemName, Addr);

    // TODO what does it mean when PC is 0 (or not a known function)?
    DEBUG(if (!Func && PC != 0) {
      dbgs() << "Skipped mem event: " << FuncLoc << " = " << AddrLoc << "\n";
    });

    if (Func) {
      auto *MemData = &FuncsToMemEvents[FuncName];
      Func->setMemData(MemData);
      MemData->update(FuncLoc, AddrLoc);
      DEBUG(dbgs() << "Mem event: " << FuncLoc << " = " << AddrLoc << "\n");
    }
  }

  return std::error_code();
}

ErrorOr<int64_t> DataAggregator::parseTaskPID() {
  while (checkAndConsumeFS()) {}

  auto CommNameStr = parseString(FieldSeparator, true);
  if (std::error_code EC = CommNameStr.getError())
    return EC;
  if (CommNameStr.get() != BinaryName.substr(0, 15)) {
    consumeRestOfLine();
    return -1;
  }

  auto LineEnd = ParsingBuf.find_first_of("\n");
  if (LineEnd == StringRef::npos) {
    reportError("expected rest of line");
    Diag << "Found: " << ParsingBuf << "\n";
    return make_error_code(llvm::errc::io_error);
  }

  StringRef Line = ParsingBuf.substr(0, LineEnd);

  if (Line.find("PERF_RECORD_COMM") != StringRef::npos) {
    int64_t PID;
    StringRef PIDStr = Line.rsplit(':').second.split('/').first;
    if (PIDStr.getAsInteger(10, PID)) {
      reportError("expected PID");
      Diag << "Found: " << PIDStr << "\n";
      return make_error_code(llvm::errc::io_error);
    }
    return PID;
  }

  consumeRestOfLine();
  return -1;
}

std::error_code DataAggregator::parseTasks() {
  outs() << "PERF2BOLT: Parsing perf-script tasks output\n";
  NamedRegionTimer T("parseTasks", "Tasks parsing", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);

  while (hasData()) {
    auto PIDRes = parseTaskPID();
    if (std::error_code EC = PIDRes.getError())
      return EC;

    auto PID = PIDRes.get();
    if (PID == -1) {
      continue;
    }

    PIDs.insert(PID);
  }
  if (!PIDs.empty()) {
    outs() << "PERF2BOLT: Input binary is associated with " << PIDs.size()
           << " PID(s)\n";
  } else {
    if (errs().has_colors())
      errs().changeColor(raw_ostream::YELLOW);
    errs() << "PERF2BOLT-WARNING: Could not bind input binary to a PID - will "
              "parse all samples in perf data. This could result in corrupted "
              "samples for the input binary if system-wide profile collection "
              "was used.\n";
    if (errs().has_colors())
      errs().resetColor();
  }

  return std::error_code();
}

Optional<std::pair<StringRef, StringRef>>
DataAggregator::parseNameBuildIDPair() {
  while (checkAndConsumeFS()) {}

  auto BuildIDStr = parseString(FieldSeparator, true);
  if (std::error_code EC = BuildIDStr.getError())
    return NoneType();

  auto NameStr = parseString(FieldSeparator, true);
  if (std::error_code EC = NameStr.getError())
    return NoneType();

  consumeRestOfLine();
  return std::make_pair(NameStr.get(), BuildIDStr.get());
}

Optional<StringRef>
DataAggregator::getFileNameForBuildID(StringRef FileBuildID) {
  while (hasData()) {
    auto IDPair = parseNameBuildIDPair();
    if (!IDPair)
      return NoneType();

    if (IDPair->second == FileBuildID)
      return sys::path::filename(IDPair->first);
  }
  return NoneType();
}

std::error_code DataAggregator::writeAggregatedFile() const {
  std::error_code EC;
  raw_fd_ostream OutFile(OutputFDataName, EC, sys::fs::OpenFlags::F_None);
  if (EC)
    return EC;

  bool WriteMemLocs = false;

  auto writeLocation = [&OutFile,&WriteMemLocs](const Location &Loc) {
    if (WriteMemLocs)
      OutFile << (Loc.IsSymbol ? "4 " : "3 ");
    else
      OutFile << (Loc.IsSymbol ? "1 " : "0 ");
    OutFile << (Loc.Name.empty() ? "[unknown]" : Loc.Name)  << " "
            << Twine::utohexstr(Loc.Offset)
            << FieldSeparator;
  };

  uint64_t BranchValues{0};
  uint64_t MemValues{0};

  if (opts::BasicAggregation) {
    OutFile << "no_lbr";
    for (const auto &Entry : EventNames) {
      OutFile << " " << Entry.getKey();
    }
    OutFile << "\n";

    for (const auto &Func : FuncsToSamples) {
      for (const auto &SI : Func.getValue().Data) {
        writeLocation(SI.Loc);
        OutFile << SI.Hits << "\n";
        ++BranchValues;
      }
    }
  } else {
    for (const auto &Func : FuncsToBranches) {
      for (const auto &BI : Func.getValue().Data) {
        writeLocation(BI.From);
        writeLocation(BI.To);
        OutFile << BI.Mispreds << " " << BI.Branches << "\n";
        ++BranchValues;
      }
      for (const auto &BI : Func.getValue().EntryData) {
        // Do not output if source is a known symbol, since this was already
        // accounted for in the source function
        if (BI.From.IsSymbol)
          continue;
        writeLocation(BI.From);
        writeLocation(BI.To);
        OutFile << BI.Mispreds << " " << BI.Branches << "\n";
        ++BranchValues;
      }
    }

    WriteMemLocs = true;
    for (const auto &Func : FuncsToMemEvents) {
      for (const auto &MemEvent : Func.getValue().Data) {
        writeLocation(MemEvent.Offset);
        writeLocation(MemEvent.Addr);
        OutFile << MemEvent.Count << "\n";
        ++MemValues;
      }
    }
  }

  outs() << "PERF2BOLT: Wrote " << BranchValues << " objects and "
         << MemValues << " memory objects to " << OutputFDataName << "\n";

  return std::error_code();
}

void DataAggregator::dump() const {
  DataReader::dump();
}

void DataAggregator::dump(const LBREntry &LBR) const {
  Diag << "From: " << Twine::utohexstr(LBR.From)
       << " To: " << Twine::utohexstr(LBR.To) << " Mispred? " << LBR.Mispred
       << "\n";
}

void DataAggregator::dump(const PerfBranchSample &Sample) const {
  Diag << "Sample LBR entries: " << Sample.LBR.size() << "\n";
  for (const auto &LBR : Sample.LBR) {
    dump(LBR);
  }
}

void DataAggregator::dump(const PerfMemSample &Sample) const {
  Diag << "Sample mem entries: " << Sample.PC << ": " << Sample.Addr << "\n";
}
