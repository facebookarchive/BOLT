//===-- DataAggregator.h - Perf data aggregator -----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions reads profile data written by perf record,
// aggregates it and then writes it back to an output file.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_DATA_AGGREGATOR_H
#define LLVM_TOOLS_LLVM_BOLT_DATA_AGGREGATOR_H

#include "DataReader.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include <map>

namespace llvm {
namespace bolt {

class BinaryFunction;
class BinaryContext;

struct LBREntry {
  uint64_t From;
  uint64_t To;
  bool Mispred;
};

struct PerfSample {
  SmallVector<LBREntry, 16> LBR;
};

/// DataAggregator inherits all parsing logic from DataReader as well as
/// its data structures used to represent aggregated profile data in memory.
///
/// The aggregator works by dispatching two separate perf-script jobs that
/// read perf samples and perf task annotations. Later, we read the output
/// files to extract information about which PID was used for this binary.
/// With the PID, we filter the samples and extract all LBR entries.
///
/// To aggregate LBR entries, we rely on a BinaryFunction map to locate the
/// original function where the event happened. Then, we convert a raw address
/// to an offset relative to the start of this function and aggregate branch
/// information for each function.
///
/// This must be coordinated with RewriteInstance so we have BinaryFunctions in
/// State::Disassembled. After this state, BinaryFunction will drop the
/// instruction map with original addresses we rely on to validate the traces
/// found in the LBR.
///
/// The last step is to write the aggregated data to disk in the output file
/// specified by the user.
class DataAggregator : public DataReader {
  // Perf process spawning bookkeeping
  std::string PerfPath;
  sys::ProcessInfo EventsPI;
  sys::ProcessInfo TasksPI;
  SmallVector<char, 256> PerfEventsOutputPath;
  SmallVector<char, 256> PerfEventsErrPath;
  SmallVector<char, 256> PerfTasksOutputPath;
  SmallVector<char, 256> PerfTasksErrPath;

  /// Whether aggregator was scheduled to run
  bool Enabled{false};

  /// Output file name to write aggregated fdata to
  StringRef OutputFDataName;

  /// Our sampled binary name to look for in perf.data
  StringRef BinaryName;

  DenseSet<int64_t> PIDs;

  /// References to core BOLT data structures
  BinaryContext *BC{nullptr};
  std::map<uint64_t, BinaryFunction> *BFs{nullptr};

  /// Aggregation statistics
  uint64_t NumInvalidTraces{0};
  uint64_t NumLongRangeTraces{0};

  /// Looks into system PATH for Linux Perf and set up the aggregator to use it
  void findPerfExecutable();

  /// Launch a subprocess to read all perf samples and write them to an output
  /// file we will parse later
  bool launchPerfEventsNoWait(StringRef PerfDataFilename);

  /// Launch a subprocess to read all perf task events. They contain the mapping
  /// of binary file name to PIDs used during data collection time. We later use
  /// the PIDs to filter samples.
  bool launchPerfTasksNoWait(StringRef PerfDataFilename);

  /// Delete all temporary files created to hold the output generated by spawned
  /// subprocesses during the aggregation job
  void deleteTempFiles();

  // Semantic pass helpers
  /// Look up which function contains an address by using out map of
  /// disassembled BinaryFunctions
  BinaryFunction *getBinaryFunctionContainingAddress(uint64_t Address);

  /// Semantic actions - parser hooks to interpret parsed perf samples
  /// Register an intraprocedural branch in \p Func with offsets \p From and
  /// \p To (relative to \p Func start address).
  bool doIntraBranch(BinaryFunction *Func, uint64_t From, uint64_t To,
                     bool Mispred);

  /// Register an interprocedural branch from \p FromFunc to \p ToFunc with
  /// offsets \p From and \p To, respectively.
  bool doInterBranch(BinaryFunction *FromFunc, BinaryFunction *ToFunc,
                     uint64_t From, uint64_t To, bool Mispred);

  /// Register a branch with raw addresses \p From and \p To extracted from the
  /// LBR
  bool doBranch(uint64_t From, uint64_t To, bool Mispred);

  /// Register a trace starting in raw address \p From and ending in \p To
  /// This will add all intermediate conditional branches in this trace as not
  /// taken.
  bool doTrace(uint64_t From, uint64_t To);

  /// Parser helpers
  /// Return false if we exhausted our parser buffer and finished parsing
  /// everything
  bool hasData();

  /// Parse a single perf sample containing a PID associated with a sequence of
  /// LBR entries
  ErrorOr<PerfSample> parseSample();

  /// Check if a field separator is the next char to parse and, if yes, consume
  /// it and return true
  bool checkAndConsumeFS();

  /// Consume the entire line
  void consumeRestOfLine();

  /// Parse a single LBR entry as output by perf script -Fbrstack
  ErrorOr<LBREntry> parseLBREntry();

  /// Parse the full output generated by perf script to report LBR samples
  std::error_code parseEvents();

  /// Parse a single line of a PERF_RECORD_COMM event looking for an association
  /// between the binary name and its PID. Return -1 if binary  name is not
  /// correct.
  ErrorOr<int64_t> parseTaskPID();

  /// Parse the full output generated by perf script to report PERF_RECORD_COMM
  /// events with the association of binary file names and their PIDs.
  std::error_code parseTasks();

public:
  DataAggregator(raw_ostream &Diag, StringRef BinaryName)
      : DataReader(Diag), BinaryName(llvm::sys::path::filename(BinaryName)) {}

  /// Set the file name to save aggregate data to
  void setOutputFDataName(StringRef Name) { OutputFDataName = Name; }

  /// Start an aggregation job asynchronously. Call "aggregate" to finish it
  /// with a list of disassembled functions.
  void start(StringRef PerfDataFilename);

  /// True if DataAggregator has asynchronously been started and an aggregation
  /// job is in progress
  bool started() const { return Enabled; }

  /// Dump data structures into a file readable by llvm-bolt
  std::error_code writeAggregatedFile() const;

  /// Join child subprocesses and finalize aggregation populating data
  /// structures
  bool aggregate(BinaryContext &BC, std::map<uint64_t, BinaryFunction> &BFs);

  /// Check whether \p FileName is a perf.data file
  static bool checkPerfDataMagic(StringRef FileName);

  /// Debugging dump methods
  void dump() const;
  void dump(const LBREntry &LBR) const;
  void dump(const PerfSample &Sample) const;
};


}
}

#endif
