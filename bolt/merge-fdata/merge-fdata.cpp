//===-- merge-fdata.cpp - Tool for merging profile in fdata format --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// merge-fdata 1.fdata 2.fdata 3.fdata > merged.fdata
//
//===----------------------------------------------------------------------===//

#include "../DataReader.h"
#include "llvm/Object/Binary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringPool.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;
using namespace object;
using namespace bolt;

namespace opts {

cl::OptionCategory MergeFdataCategory("merge-fdata options");

enum SortType : char {
  ST_NONE,
  ST_EXEC_COUNT,      /// Sort based on function execution count.
  ST_TOTAL_BRANCHES,  /// Sort based on all branches in the function.
};

static cl::list<std::string>
InputDataFilenames(
  cl::Positional,
  cl::CommaSeparated,
  cl::desc("<fdata1> [<fdata2>]..."),
  cl::OneOrMore,
  cl::cat(MergeFdataCategory));

static cl::opt<SortType>
PrintFunctionList("print",
  cl::desc("print the list of objects with count to stderr"),
  cl::init(ST_NONE),
  cl::values(clEnumValN(ST_NONE,
      "none",
      "do not print objects/functions"),
    clEnumValN(ST_EXEC_COUNT,
      "exec",
      "print functions sorted by execution count"),
    clEnumValN(ST_TOTAL_BRANCHES,
      "branches",
      "print functions sorted by total branch count"),
    clEnumValEnd),
  cl::cat(MergeFdataCategory));

static cl::opt<bool>
SuppressMergedDataOutput("q",
  cl::desc("do not print merged data to stdout"),
  cl::init(false),
  cl::Optional,
  cl::cat(MergeFdataCategory));

} // namespace opts

static StringRef ToolName;

static void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << ToolName << ": '" << Message << "': " << EC.message() << ".\n";
  exit(1);
}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  cl::HideUnrelatedOptions(opts::MergeFdataCategory);

  cl::ParseCommandLineOptions(argc, argv,
                              "merge fdata into a single file");

  ToolName = argv[0];

  // All merged data.
  DataReader::FuncsMapType MergedFunctionsData;

  // Merged functions data has to replace strings refs with strings from the
  // pool.
  StringPool MergedStringPool;

  // Temporary storage for all strings so they don't get destroyed.
  std::vector<PooledStringPtr> AllStrings;

  // Copy branch info replacing string references with internal storage
  // references.
  auto CopyBranchInfo = [&](const BranchInfo &BI,
                            std::vector<BranchInfo> &BIData) {
    auto FromNamePtr = MergedStringPool.intern(BI.From.Name);
    auto ToNamePtr = MergedStringPool.intern(BI.To.Name);
    BranchHistories Histories;
    for (const auto &HI : BI.Histories) {
      BranchContext Context;
      for (const auto &CI : HI.Context) {
        const auto &CtxFrom = CI.first;
        const auto CtxFromNamePtr = MergedStringPool.intern(CtxFrom.Name);
        const auto &CtxTo = CI.second;
        const auto CtxToNamePtr = MergedStringPool.intern(CtxTo.Name);
        Context.emplace_back(std::make_pair(Location(CtxFrom.IsSymbol,
                                                     *CtxFromNamePtr,
                                                     CtxFrom.Offset),
                                            Location(CtxTo.IsSymbol,
                                                     *CtxToNamePtr,
                                                     CtxTo.Offset)));
        AllStrings.emplace_back(CtxFromNamePtr); // keep the reference
        AllStrings.emplace_back(CtxToNamePtr); // keep the reference
      }
      Histories.emplace_back(BranchHistory(HI.Mispreds,
                                           HI.Branches,
                                           std::move(Context)));
    }
    BIData.emplace_back(BranchInfo(Location(BI.From.IsSymbol,
                                            *FromNamePtr,
                                            BI.From.Offset),
                                   Location(BI.To.IsSymbol,
                                            *ToNamePtr,
                                            BI.To.Offset),
                                   BI.Mispreds,
                                   BI.Branches,
                                   std::move(Histories)));
    AllStrings.emplace_back(FromNamePtr); // keep the reference
    AllStrings.emplace_back(ToNamePtr);   // keep the reference
  };

  // Simply replace string references in BranchInfo with internal storage
  // references.
  auto replaceStringRefs = [&] (BranchInfo &BI) {
    auto FromNamePtr = MergedStringPool.intern(BI.From.Name);
    BI.From.Name = *FromNamePtr;
    AllStrings.emplace_back(FromNamePtr); // keep the reference

    auto ToNamePtr = MergedStringPool.intern(BI.To.Name);
    BI.To.Name = *ToNamePtr;
    AllStrings.emplace_back(ToNamePtr);   // keep the reference

    for (auto &HI : BI.Histories) {
      for (auto &CI : HI.Context) {
        auto CtxFromNamePtr = MergedStringPool.intern(CI.first.Name);
        CI.first.Name = *CtxFromNamePtr;
        AllStrings.emplace_back(CtxFromNamePtr); // keep the reference
        auto CtxToNamePtr = MergedStringPool.intern(CI.second.Name);
        CI.second.Name = *CtxToNamePtr;
        AllStrings.emplace_back(CtxToNamePtr); // keep the reference
      }
    }
  };

  for (auto &InputDataFilename : opts::InputDataFilenames) {
    if (!sys::fs::exists(InputDataFilename))
      report_error(InputDataFilename, errc::no_such_file_or_directory);

    errs() << "Merging data from " << InputDataFilename << "...\n";

    // Attempt to read input bolt data
    auto ReaderOrErr =
      bolt::DataReader::readPerfData(InputDataFilename, errs());
    if (std::error_code EC = ReaderOrErr.getError())
      report_error(InputDataFilename, EC);

    for (auto &FI : ReaderOrErr.get()->getAllFuncsData()) {
      auto MI = MergedFunctionsData.find(FI.second.Name);
      if (MI != MergedFunctionsData.end()) {
        MI->second.ExecutionCount += FI.second.ExecutionCount;
        std::vector<BranchInfo> TmpBI;
        for (auto &BI : FI.second.Data) {
          // Find and merge a corresponding entry or copy data.
          auto TI = std::lower_bound(MI->second.Data.begin(),
                                     MI->second.Data.end(),
                                     BI);
          if (TI != MI->second.Data.end() && *TI == BI) {
            replaceStringRefs(BI);
            TI->mergeWith(BI);
          } else {
            CopyBranchInfo(BI, TmpBI);
          }
        }
        // Merge in the temp vector making sure it doesn't contain duplicates.
        std::sort(TmpBI.begin(), TmpBI.end());
        BranchInfo *PrevBI = nullptr;
        for (auto &BI : TmpBI) {
          if (PrevBI && *PrevBI == BI) {
            PrevBI->mergeWith(BI);
          } else {
            MI->second.Data.emplace_back(BI);
            PrevBI = &MI->second.Data.back();
          }
        }
        std::sort(MI->second.Data.begin(), MI->second.Data.end());
      } else {
        auto NamePtr = MergedStringPool.intern(FI.second.Name);
        AllStrings.emplace_back(NamePtr); // keep the ref
        bool Success;
        std::tie(MI, Success) = MergedFunctionsData.insert(
            std::make_pair(*NamePtr,
                           FuncBranchData(*NamePtr,
                                          FuncBranchData::ContainerTy())));
        MI->second.ExecutionCount = FI.second.ExecutionCount;
        // Copy with string conversion while eliminating duplicates.
        std::sort(FI.second.Data.begin(), FI.second.Data.end());
        BranchInfo *PrevBI = nullptr;
        for (auto &BI : FI.second.Data) {
          if (PrevBI && *PrevBI == BI) {
            replaceStringRefs(BI);
            PrevBI->mergeWith(BI);
          } else {
            CopyBranchInfo(BI, MI->second.Data);
            PrevBI = &MI->second.Data.back();
          }
        }
      }
    }
  }

  if (!opts::SuppressMergedDataOutput) {
    // Print all the data in the original format
    for (const auto &FDI : MergedFunctionsData) {
      for (const auto &BD : FDI.second.Data) {
        BD.print(outs());
      }
    }
  }

  errs() << "Data for " << MergedFunctionsData.size()
         << " unique objects successfully merged.\n";

  if (opts::PrintFunctionList != opts::ST_NONE) {
    // List of function names with execution count.
    std::vector<std::pair<uint64_t, StringRef>>
      FunctionList(MergedFunctionsData.size());
    using CountFuncType =
      std::function<std::pair<uint64_t,StringRef>(
          const StringMapEntry<FuncBranchData>&)>;
    CountFuncType ExecCountFunc = [](const StringMapEntry<FuncBranchData> &v) {
      return std::make_pair(v.second.ExecutionCount,
                            v.second.Name);
    };
    CountFuncType BranchCountFunc = [](const StringMapEntry<FuncBranchData> &v){
      // Return total branch count.
      uint64_t BranchCount = 0;
      for (const auto &BI : v.second.Data)
        BranchCount += BI.Branches;
      return std::make_pair(BranchCount,
                            v.second.Name);
    };

    CountFuncType CountFunc = (opts::PrintFunctionList == opts::ST_EXEC_COUNT)
       ? ExecCountFunc
       : BranchCountFunc;
    std::transform(MergedFunctionsData.begin(),
                   MergedFunctionsData.end(),
                   FunctionList.begin(),
                   CountFunc);
    std::stable_sort(FunctionList.rbegin(), FunctionList.rend());
    errs() << "Functions sorted by "
           << (opts::PrintFunctionList == opts::ST_EXEC_COUNT
                ? "execution"
                : "total branch")
           << " count:\n";
    for (auto &FI : FunctionList) {
      errs() << FI.second << " : " << FI.first << '\n';
    }
  }

  AllStrings.clear();

  return EXIT_SUCCESS;
}
