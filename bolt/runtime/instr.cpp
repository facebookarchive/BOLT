//===-- instr.cpp -----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// This file contains code that is linked to the final binary with a function
// that is called at program exit to dump instrumented data collected during
// execution.
//
//===----------------------------------------------------------------------===//
//
// BOLT runtime instrumentation library for x86 Linux. Currently, BOLT does
// not support linking modules with dependencies on one another into the final
// binary (TODO?), which means this library has to be self-contained in a single
// module.
//
// All extern declarations here need to be defined by BOLT itself. Those will be
// undefined symbols that BOLT needs to resolve by emitting these symbols with
// MCStreamer. Currently, Passes/Instrumentation.cpp is the pass responsible
// for defining the symbols here and these two files have a tight coupling: one
// working statically when you run BOLT and another during program runtime when
// you run an instrumented binary. The main goal here is to output an fdata file
// (BOLT profile) with the instrumentation counters inserted by the static pass.
// Counters for indirect calls are an exception, as we can't know them
// statically. These counters are created and managed here. To allow this, we
// need a minimal framework for allocating memory dynamically. We provide this
// with the BumpPtrAllocator class (not LLVM's, but our own version of it).
//
// Since this code is intended to be inserted into any executable, we decided to
// make it standalone and do not depend on any external libraries (i.e. language
// support libraries, such as glibc or stdc++). To allow this, we provide a few
// light implementations of common OS interacting functionalities using direct
// syscall wrappers. Our simple allocator doesn't manage deallocations that
// fragment the memory space, so it's stack based. This is the minimal framework
// provided here to allow processing instrumented counters and writing fdata.
//
// In the C++ idiom used here, we never use or rely on constructors or
// destructors for global objects. That's because those need support from the
// linker in initialization/finalization code, and we want to keep our linker
// very simple. Similarly, we don't create any global objects that are zero
// initialized, since those would need to go .bss, which our simple linker also
// don't support (TODO?).
//
//===----------------------------------------------------------------------===//

#include <cstdint>

#include "config.h"
#ifdef HAVE_ELF_H
#include <elf.h>
#endif

// Enables a very verbose logging to stderr useful when debugging
//#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define DEBUG(X)                                                               \
  { X; }
#else
#define DEBUG(X)                                                               \
  {}
#endif

// Main counters inserted by instrumentation, incremented during runtime when
// points of interest (locations) in the program are reached. Those are direct
// calls and direct and indirect branches (local ones). There are also counters
// for basic block execution if they are a spanning tree leaf and need to be
// counted in order to infer the execution count of other edges of the CFG.
extern uint64_t __bolt_instr_locations[];
extern uint32_t __bolt_num_counters;
// Descriptions are serialized metadata about binary functions written by BOLT,
// so we have a minimal understanding about the program structure. For a
// reference on the exact format of this metadata, see *Description structs,
// Location, IntrumentedNode and EntryNode.
// Number of indirect call site descriptions
extern uint32_t __bolt_instr_num_ind_calls;
// Number of indirect call target descriptions
extern uint32_t __bolt_instr_num_ind_targets;
// Number of function descriptions
extern uint32_t __bolt_instr_num_funcs;
// Time to sleep across dumps (when we write the fdata profile to disk)
extern uint32_t __bolt_instr_sleep_time;
// Filename to dump data to
extern char __bolt_instr_filename[];
// If true, append current PID to the fdata filename when creating it so
// different invocations of the same program can be differentiated.
extern bool __bolt_instr_use_pid;
// Functions that will be used to instrument indirect calls. BOLT static pass
// will identify indirect calls and modify them to load the address in these
// trampolines and call this address instead. BOLT can't use direct calls to
// our handlers because our addresses here are not known at analysis time. We
// only support resolving dependencies from this file to the output of BOLT,
// *not* the other way around.
// TODO: We need better linking support to make that happen.
extern void (*__bolt_trampoline_ind_call)();
extern void (*__bolt_trampoline_ind_tailcall)();
// Function pointers to init/fini routines in the binary, so we can resume
// regular execution of these functions that we hooked
extern void (*__bolt_instr_init_ptr)();
extern void (*__bolt_instr_fini_ptr)();

// Anonymous namespace covering everything but our library entry point
namespace {

// We use a stack-allocated buffer for string manipulation in many pieces of
// this code, including the code that prints each line of the fdata file. This
// buffer needs to accomodate large function names, but shouldn't be arbitrarily
// large (dynamically allocated) for simplicity of our memory space usage.
constexpr uint32_t BufSize = 10240;

// Declare some syscall wrappers we use throughout this code to avoid linking
// against system libc.
uint64_t __open(const char *pathname, uint64_t flags, uint64_t mode) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $2, %%rax\n"
          "syscall"
          : "=a"(ret)
          : "D"(pathname), "S"(flags), "d"(mode)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

uint64_t __write(uint64_t fd, const void *buf, uint64_t count) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $1, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(fd), "S"(buf), "d"(count)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

uint64_t __lseek(uint64_t fd, uint64_t pos, uint64_t whence) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $8, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(fd), "S"(pos), "d"(whence)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

int __close(uint64_t fd) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $3, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(fd)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

struct timespec {
  uint64_t tv_sec; /* seconds */
  uint64_t tv_nsec;  /* nanoseconds */
};

uint64_t __nanosleep(const timespec *req, timespec *rem) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $35, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(req), "S"(rem)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

int64_t __fork() {
  uint64_t ret;
  __asm__ __volatile__("movq $57, %%rax\n"
                       "syscall\n"
                       : "=a"(ret)
                       :
                       : "cc", "rcx", "r11", "memory");
  return ret;
}

void *__mmap(uint64_t addr, uint64_t size, uint64_t prot, uint64_t flags,
             uint64_t fd, uint64_t offset) {
  void *ret;
  register uint64_t r8 asm("r8") = fd;
  register uint64_t r9 asm("r9") = offset;
  register uint64_t r10 asm("r10") = flags;
  __asm__ __volatile__ (
          "movq $9, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(addr), "S"(size), "d"(prot), "r"(r10), "r"(r8), "r"(r9)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

uint64_t __munmap(void *addr, uint64_t size) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $11, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(addr), "S"(size)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

uint64_t __getpid() {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $39, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          :
          : "cc", "rcx", "r11", "memory");
  return ret;
}

uint64_t __getppid() {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $110, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          :
          : "cc", "rcx", "r11", "memory");
  return ret;
}

uint64_t __exit(uint64_t code) {
  uint64_t ret;
  __asm__ __volatile__ (
          "movq $231, %%rax\n"
          "syscall\n"
          : "=a"(ret)
          : "D"(code)
          : "cc", "rcx", "r11", "memory");
  return ret;
}

// Helper functions for writing strings to the .fdata file. We intentionally
// avoid using libc names (lowercase memset) to make it clear it is our impl.

/// Write number Num using Base to the buffer in OutBuf, returns a pointer to
/// the end of the string.
char *intToStr(char *OutBuf, uint64_t Num, uint32_t Base) {
  const char *Chars = "0123456789abcdef";
  char Buf[21];
  char *Ptr = Buf;
  while (Num) {
    *Ptr++ = *(Chars + (Num % Base));
    Num /= Base;
  }
  if (Ptr == Buf) {
    *OutBuf++ = '0';
    return OutBuf;
  }
  while (Ptr != Buf) {
    *OutBuf++ = *--Ptr;
  }
  return OutBuf;
}

/// Copy Str to OutBuf, returns a pointer to the end of the copied string
char *strCopy(char *OutBuf, const char *Str, int32_t Size = BufSize) {
  while (*Str) {
    *OutBuf++ = *Str++;
    if (--Size <= 0)
      return OutBuf;
  }
  return OutBuf;
}

void memSet(char *Buf, char C, uint32_t Size) {
  for (int I = 0; I < Size; ++I)
    *Buf++ = C;
}

uint32_t strLen(const char *Str) {
  uint32_t Size = 0;
  while (*Str++)
    ++Size;
  return Size;
}

void reportError(const char *Msg, uint64_t Size) {
  __write(2, Msg, Size);
  __exit(1);
}

void assert(bool Assertion, const char *Msg) {
  if (Assertion)
    return;
  char Buf[BufSize];
  char *Ptr = Buf;
  Ptr = strCopy(Ptr, "Assertion failed: ");
  Ptr = strCopy(Ptr, Msg, BufSize - 40);
  Ptr = strCopy(Ptr, "\n");
  reportError(Buf, Ptr - Buf);
}

void reportNumber(const char *Msg, uint64_t Num, uint32_t Base) {
  char Buf[BufSize];
  char *Ptr = Buf;
  Ptr = strCopy(Ptr, Msg, BufSize - 23);
  Ptr = intToStr(Ptr, Num, Base);
  Ptr = strCopy(Ptr, "\n");
  __write(2, Buf, Ptr - Buf);
}

void report(const char *Msg) {
  __write(2, Msg, strLen(Msg));
}

/// 1B mutex accessed by lock xchg
class Mutex {
  volatile bool InUse{false};
public:
  bool acquire() {
    bool Result = true;
    asm volatile("lock; xchg %0, %1"
                 : "+m"(InUse), "=r"(Result)
                 :
                 : "cc");
    return !Result;
  }
  void release() {
    InUse = false;
  }
};

/// RAII wrapper for Mutex
class Lock {
  Mutex &M;
public:
  Lock(Mutex &M) : M(M) {
    while (!M.acquire()) {}
  }
  ~Lock() {
    M.release();
  }
};

inline uint64_t alignTo(uint64_t Value, uint64_t Align) {
  return (Value + Align - 1) / Align * Align;
}

/// A simple allocator that mmaps a fixed size region and manages this space
/// in a stack fashion, meaning you always deallocate the last element that
/// was allocated. In practice, we don't need to deallocate individual elements.
/// We monotonically increase our usage and then deallocate everything once we
/// are done processing something.
class BumpPtrAllocator {
  /// This is written before each allocation and act as a canary to detect when
  /// a bug caused our program to cross allocation boundaries.
  struct EntryMetadata {
    uint64_t Magic;
    uint64_t AllocSize;
  };
public:
  void *allocate(uintptr_t Size) {
    Lock L(M);
    if (StackBase == nullptr) {
      StackBase = reinterpret_cast<uint8_t *>(
          __mmap(0, MaxSize, 0x3 /* PROT_READ | PROT_WRITE*/,
                 Shared ? 0x21 /*MAP_SHARED | MAP_ANONYMOUS*/
                        : 0x22 /* MAP_PRIVATE | MAP_ANONYMOUS*/,
                 -1, 0));
      StackSize = 0;
    }
    Size = alignTo(Size + sizeof(EntryMetadata), 16);
    uint8_t * AllocAddress = StackBase + StackSize + sizeof(EntryMetadata);
    auto *M = reinterpret_cast<EntryMetadata *>(StackBase + StackSize);
    M->Magic = Magic;
    M->AllocSize = Size;
    StackSize += Size;
    assert(StackSize < MaxSize, "allocator ran out of memory");
    return AllocAddress;
  }

#ifdef DEBUG
  /// Element-wise deallocation is only used for debugging to catch memory
  /// bugs by checking magic bytes. Ordinarily, we reset the allocator once
  /// we are done with it. Reset is done with clear(). There's no need
  /// to deallocate each element individually.
  void deallocate(void *Ptr) {
    Lock L(M);
    uint8_t MetadataOffset = sizeof(EntryMetadata);
    auto *M = reinterpret_cast<EntryMetadata *>(
        reinterpret_cast<uint8_t *>(Ptr) - MetadataOffset);
    const uint8_t *StackTop = StackBase + StackSize + MetadataOffset;
    // Validate size
    if (Ptr != StackTop - M->AllocSize) {
      // Failed validation, check if it is a pointer returned by operator new []
      MetadataOffset +=
          sizeof(uint64_t); // Space for number of elements alloc'ed
      M = reinterpret_cast<EntryMetadata *>(reinterpret_cast<uint8_t *>(Ptr) -
                                            MetadataOffset);
      // Ok, it failed both checks if this assertion fails. Stop the program, we
      // have a memory bug.
      assert(Ptr == StackTop - M->AllocSize,
             "must deallocate the last element alloc'ed");
    }
    assert(M->Magic == Magic, "allocator magic is corrupt");
    StackSize -= M->AllocSize;
  }
#else
  void deallocate(void *) {}
#endif

  void clear() {
    Lock L(M);
    StackSize = 0;
  }

  /// Set mmap reservation size (only relevant before first allocation)
  void setMaxSize(uint64_t Size) {
    MaxSize = Size;
  }

  /// Set mmap reservation privacy (only relevant before first allocation)
  void setShared(bool S) {
    Shared = S;
  }

  void destroy() {
    if (StackBase == nullptr)
      return;
    __munmap(StackBase, MaxSize);
  }

private:
  static constexpr uint64_t Magic = 0x1122334455667788ull;
  uint64_t MaxSize = 0xa00000;
  uint8_t *StackBase{nullptr};
  uint64_t StackSize{0};
  bool Shared{false};
  Mutex M;
};

/// Used for allocating indirect call instrumentation counters. Initialized by
/// __bolt_instr_setup, our initialization routine.
BumpPtrAllocator GlobalAlloc;

} // anonymous namespace

// User-defined placement new operators. We only use those (as opposed to
// overriding the regular operator new) so we can keep our allocator in the
// stack instead of in a data section (global).
void *operator new(uintptr_t Sz, BumpPtrAllocator &A) {
  return A.allocate(Sz);
}
void *operator new(uintptr_t Sz, BumpPtrAllocator &A, char C) {
  auto *Ptr = reinterpret_cast<char *>(A.allocate(Sz));
  memSet(Ptr, C, Sz);
  return Ptr;
}
void *operator new[](uintptr_t Sz, BumpPtrAllocator &A) {
  return A.allocate(Sz);
}
void *operator new[](uintptr_t Sz, BumpPtrAllocator &A, char C) {
  auto *Ptr = reinterpret_cast<char *>(A.allocate(Sz));
  memSet(Ptr, C, Sz);
  return Ptr;
}
// Only called during exception unwinding (useless). We must manually dealloc.
// C++ language weirdness
void operator delete(void *Ptr, BumpPtrAllocator &A) {
  A.deallocate(Ptr);
}

namespace {

/// Basic key-val atom stored in our hash
struct SimpleHashTableEntryBase {
  uint64_t Key;
  uint64_t Val;
};

/// This hash table implementation starts by allocating a table of size
/// InitialSize. When conflicts happen in this main table, it resolves
/// them by chaining a new table of size IncSize. It never reallocs as our
/// allocator doesn't support it. The key is intended to be function pointers.
/// There's no clever hash function (it's just x mod size, size being prime).
/// I never tuned the coefficientes in the modular equation (TODO)
/// This is used for indirect calls (each call site has one of this, so it
/// should have a small footprint) and for tallying call counts globally for
/// each target to check if we missed the origin of some calls (this one is a
/// large instantiation of this template, since it is global for all call sites)
template <typename T = SimpleHashTableEntryBase, uint32_t InitialSize = 7,
          uint32_t IncSize = 7>
class SimpleHashTable {
public:
  using MapEntry = T;

  /// Increment by 1 the value of \p Key. If it is not in this table, it will be
  /// added to the table and its value set to 1.
  void incrementVal(uint64_t Key, BumpPtrAllocator &Alloc) {
    ++get(Key, Alloc).Val;
  }

  /// Basic member accessing interface. Here we pass the allocator explicitly to
  /// avoid storing a pointer to it as part of this table (remember there is one
  /// hash for each indirect call site, so we wan't to minimize our footprint).
  MapEntry &get(uint64_t Key, BumpPtrAllocator &Alloc) {
    Lock L(M);
    if (TableRoot)
      return getEntry(TableRoot, Key, Key, Alloc, 0);
    return firstAllocation(Key, Alloc);
  }

  /// Traverses all elements in the table
  template <typename... Args>
  void forEachElement(void (*Callback)(MapEntry &, Args...), Args... args) {
    if (!TableRoot)
      return;
    return forEachElement(Callback, InitialSize, TableRoot, args...);
  }

  void resetCounters();

private:
  constexpr static uint64_t VacantMarker = 0;
  constexpr static uint64_t FollowUpTableMarker = 0x8000000000000000ull;

  MapEntry *TableRoot{nullptr};
  Mutex M;

  template <typename... Args>
  void forEachElement(void (*Callback)(MapEntry &, Args...),
                      uint32_t NumEntries, MapEntry *Entries, Args... args) {
    for (int I = 0; I < NumEntries; ++I) {
      auto &Entry = Entries[I];
      if (Entry.Key == VacantMarker)
        continue;
      if (Entry.Key & FollowUpTableMarker) {
        forEachElement(Callback, IncSize,
                       reinterpret_cast<MapEntry *>(Entry.Key &
                                                    ~FollowUpTableMarker),
                       args...);
        continue;
      }
      Callback(Entry, args...);
    }
  }

  MapEntry &firstAllocation(uint64_t Key, BumpPtrAllocator &Alloc) {
    TableRoot = new (Alloc, 0) MapEntry[InitialSize];
    auto &Entry = TableRoot[Key % InitialSize];
    Entry.Key = Key;
    return Entry;
  }

  MapEntry &getEntry(MapEntry *Entries, uint64_t Key, uint64_t Selector,
                     BumpPtrAllocator &Alloc, int CurLevel) {
    const uint32_t NumEntries = CurLevel == 0 ? InitialSize : IncSize;
    uint64_t Remainder = Selector / NumEntries;
    Selector = Selector % NumEntries;
    auto &Entry = Entries[Selector];

    // A hit
    if (Entry.Key == Key) {
      return Entry;
    }

    // Vacant - add new entry
    if (Entry.Key == VacantMarker) {
      Entry.Key = Key;
      return Entry;
    }

    // Defer to the next level
    if (Entry.Key & FollowUpTableMarker) {
      return getEntry(
          reinterpret_cast<MapEntry *>(Entry.Key & ~FollowUpTableMarker),
          Key, Remainder, Alloc, CurLevel + 1);
    }

    // Conflict - create the next level
    MapEntry *NextLevelTbl = new (Alloc, 0) MapEntry[IncSize];
    uint64_t CurEntrySelector = Entry.Key / InitialSize;
    for (int I = 0; I < CurLevel; ++I)
      CurEntrySelector /= IncSize;
    CurEntrySelector = CurEntrySelector % IncSize;
    NextLevelTbl[CurEntrySelector] = Entry;
    Entry.Key = reinterpret_cast<uint64_t>(NextLevelTbl) | FollowUpTableMarker;
    return getEntry(NextLevelTbl, Key, Remainder, Alloc, CurLevel + 1);
  }
};

template <typename T> void resetIndCallCounter(T &Entry) {
  Entry.Val = 0;
}

template <typename T, uint32_t X, uint32_t Y>
void SimpleHashTable<T, X, Y>::resetCounters() {
  Lock L(M);
  forEachElement(resetIndCallCounter);
}

/// Represents a hash table mapping a function target address to its counter.
using IndirectCallHashTable = SimpleHashTable<>;

/// Initialize with number 1 instead of 0 so we don't go into .bss. This is the
/// global array of all hash tables storing indirect call destinations happening
/// during runtime, one table per call site.
IndirectCallHashTable *GlobalIndCallCounters{
    reinterpret_cast<IndirectCallHashTable *>(1)};

/// Don't allow reentrancy in the fdata writing phase - only one thread writes
/// it
Mutex *GlobalWriteProfileMutex{reinterpret_cast<Mutex *>(1)};

/// Store number of calls in additional to target address (Key) and frequency
/// as perceived by the basic block counter (Val).
struct CallFlowEntryBase : public SimpleHashTableEntryBase {
  uint64_t Calls;
};

using CallFlowHashTableBase = SimpleHashTable<CallFlowEntryBase, 11939, 233>;

/// This is a large table indexing all possible call targets (indirect and
/// direct ones). The goal is to find mismatches between number of calls (for
/// those calls we were able to track) and the entry basic block counter of the
/// callee. In most cases, these two should be equal. If not, there are two
/// possible scenarios here:
///
///  * Entry BB has higher frequency than all known calls to this function.
///    In this case, we have dynamic library code or any uninstrumented code
///    calling this function. We will write the profile for these untracked
///    calls as having source "0 [unknown] 0" in the fdata file.
///
///  * Number of known calls is higher than the frequency of entry BB
///    This only happens when there is no counter for the entry BB / callee
///    function is not simple (in BOLT terms). We don't do anything special
///    here and just ignore those (we still report all calls to the non-simple
///    function, though).
///
class CallFlowHashTable : public CallFlowHashTableBase {
public:
  CallFlowHashTable(BumpPtrAllocator &Alloc) : Alloc(Alloc) {}

  MapEntry &get(uint64_t Key) { return CallFlowHashTableBase::get(Key, Alloc); }

private:
  // Different than the hash table for indirect call targets, we do store the
  // allocator here since there is only one call flow hash and space overhead
  // is negligible.
  BumpPtrAllocator &Alloc;
};

///
/// Description metadata emitted by BOLT to describe the program - refer to
/// Passes/Instrumentation.cpp - Instrumentation::emitTablesAsELFNote()
///
struct Location {
  uint32_t FunctionName;
  uint32_t Offset;
};

struct CallDescription {
  Location From;
  uint32_t FromNode;
  Location To;
  uint32_t Counter;
  uint64_t TargetAddress;
};

using IndCallDescription = Location;

struct IndCallTargetDescription {
  Location Loc;
  uint64_t Address;
};

struct EdgeDescription {
  Location From;
  uint32_t FromNode;
  Location To;
  uint32_t ToNode;
  uint32_t Counter;
};

struct InstrumentedNode {
  uint32_t Node;
  uint32_t Counter;
};

struct EntryNode {
  uint64_t Node;
  uint64_t Address;
};

struct FunctionDescription {
  uint32_t NumLeafNodes;
  const InstrumentedNode *LeafNodes;
  uint32_t NumEdges;
  const EdgeDescription *Edges;
  uint32_t NumCalls;
  const CallDescription *Calls;
  uint32_t NumEntryNodes;
  const EntryNode *EntryNodes;

  /// Constructor will parse the serialized function metadata written by BOLT
  FunctionDescription(const uint8_t *FuncDesc);

  uint64_t getSize() const {
    return 16 + NumLeafNodes * sizeof(InstrumentedNode) +
           NumEdges * sizeof(EdgeDescription) +
           NumCalls * sizeof(CallDescription) +
           NumEntryNodes * sizeof(EntryNode);
  }
};

/// The context is created when the fdata profile needs to be written to disk
/// and we need to interpret our runtime counters. It contains pointers to the
/// mmaped binary (only the BOLT written metadata section). Deserialization
/// should be straightforward as most data is POD or an array of POD elements.
/// This metadata is used to reconstruct function CFGs.
struct ProfileWriterContext {
  IndCallDescription *IndCallDescriptions;
  IndCallTargetDescription *IndCallTargets;
  uint8_t *FuncDescriptions;
  char *Strings;  // String table with function names used in this binary
  int FileDesc;   // File descriptor for the file on disk backing this
                  // information in memory via mmap
  void *MMapPtr;  // The mmap ptr
  int MMapSize;   // The mmap size

  /// Hash table storing all possible call destinations to detect untracked
  /// calls and correctly report them as [unknown] in output fdata.
  CallFlowHashTable *CallFlowTable;

  /// Lookup the sorted indirect call target vector to fetch function name and
  /// offset for an arbitrary function pointer.
  const IndCallTargetDescription *lookupIndCallTarget(uint64_t Target) const;
};

/// Perform a string comparison and returns zero if Str1 matches Str2. Compares
/// at most Size characters.
int compareStr(const char *Str1, const char *Str2, int Size) {
  while (*Str1 == *Str2) {
    if (*Str1 == '\0' || --Size == 0)
      return 0;
    ++Str1;
    ++Str2;
  }
  return 1;
}

/// Output Location to the fdata file
char *serializeLoc(const ProfileWriterContext &Ctx, char *OutBuf,
                   const Location Loc, uint32_t BufSize) {
  // fdata location format: Type Name Offset
  // Type 1 - regular symbol
  OutBuf = strCopy(OutBuf, "1 ");
  const char *Str = Ctx.Strings + Loc.FunctionName;
  uint32_t Size = 25;
  while (*Str) {
    *OutBuf++ = *Str++;
    if (++Size >= BufSize)
      break;
  }
  assert(!*Str, "buffer overflow, function name too large");
  *OutBuf++ = ' ';
  OutBuf = intToStr(OutBuf, Loc.Offset, 16);
  *OutBuf++ = ' ';
  return OutBuf;
}

/// Read and deserialize a function description written by BOLT. \p FuncDesc
/// points at the beginning of the function metadata structure in the file.
/// See Instrumentation::emitTablesAsELFNote()
FunctionDescription::FunctionDescription(const uint8_t *FuncDesc) {
  NumLeafNodes = *reinterpret_cast<const uint32_t *>(FuncDesc);
  DEBUG(reportNumber("NumLeafNodes = ", NumLeafNodes, 10));
  LeafNodes = reinterpret_cast<const InstrumentedNode *>(FuncDesc + 4);

  NumEdges = *reinterpret_cast<const uint32_t *>(
      FuncDesc + 4 + NumLeafNodes * sizeof(InstrumentedNode));
  DEBUG(reportNumber("NumEdges = ", NumEdges, 10));
  Edges = reinterpret_cast<const EdgeDescription *>(
      FuncDesc + 8 + NumLeafNodes * sizeof(InstrumentedNode));

  NumCalls = *reinterpret_cast<const uint32_t *>(
      FuncDesc + 8 + NumLeafNodes * sizeof(InstrumentedNode) +
      NumEdges * sizeof(EdgeDescription));
  DEBUG(reportNumber("NumCalls = ", NumCalls, 10));
  Calls = reinterpret_cast<const CallDescription *>(
      FuncDesc + 12 + NumLeafNodes * sizeof(InstrumentedNode) +
      NumEdges * sizeof(EdgeDescription));
  NumEntryNodes = *reinterpret_cast<const uint32_t *>(
      FuncDesc + 12 + NumLeafNodes * sizeof(InstrumentedNode) +
      NumEdges * sizeof(EdgeDescription) + NumCalls * sizeof(CallDescription));
  DEBUG(reportNumber("NumEntryNodes = ", NumEntryNodes, 10));
  EntryNodes = reinterpret_cast<const EntryNode *>(
      FuncDesc + 16 + NumLeafNodes * sizeof(InstrumentedNode) +
      NumEdges * sizeof(EdgeDescription) + NumCalls * sizeof(CallDescription));
}

/// Read and mmap descriptions written by BOLT from the executable's notes
/// section
#ifdef HAVE_ELF_H
ProfileWriterContext readDescriptions() {
  ProfileWriterContext Result;
  uint64_t FD = __open("/proc/self/exe",
                       /*flags=*/0 /*O_RDONLY*/,
                       /*mode=*/0666);
  assert(static_cast<int64_t>(FD) > 0, "Failed to open /proc/self/exe");
  Result.FileDesc = FD;

  // mmap our binary to memory
  uint64_t Size = __lseek(FD, 0, 2 /*SEEK_END*/);
  uint8_t *BinContents = reinterpret_cast<uint8_t *>(
      __mmap(0, Size, 0x1 /* PROT_READ*/, 0x2 /* MAP_PRIVATE*/, FD, 0));
  Result.MMapPtr = BinContents;
  Result.MMapSize = Size;
  Elf64_Ehdr *Hdr = reinterpret_cast<Elf64_Ehdr *>(BinContents);
  Elf64_Shdr *Shdr = reinterpret_cast<Elf64_Shdr *>(BinContents + Hdr->e_shoff);
  Elf64_Shdr *StringTblHeader = reinterpret_cast<Elf64_Shdr *>(
      BinContents + Hdr->e_shoff + Hdr->e_shstrndx * Hdr->e_shentsize);

  // Find .bolt.instr.tables with the data we need and set pointers to it
  for (int I = 0; I < Hdr->e_shnum; ++I) {
    char *SecName = reinterpret_cast<char *>(
        BinContents + StringTblHeader->sh_offset + Shdr->sh_name);
    if (compareStr(SecName, ".bolt.instr.tables", 64) != 0) {
      Shdr = reinterpret_cast<Elf64_Shdr *>(BinContents + Hdr->e_shoff +
                                            (I + 1) * Hdr->e_shentsize);
      continue;
    }
    // Actual contents of the ELF note start after offset 20 decimal:
    // Offset 0: Producer name size (4 bytes)
    // Offset 4: Contents size (4 bytes)
    // Offset 8: Note type (4 bytes)
    // Offset 12: Producer name (BOLT\0) (5 bytes + align to 4-byte boundary)
    // Offset 20: Contents
    uint32_t IndCallDescSize =
        *reinterpret_cast<uint32_t *>(BinContents + Shdr->sh_offset + 20);
    uint32_t IndCallTargetDescSize = *reinterpret_cast<uint32_t *>(
        BinContents + Shdr->sh_offset + 24 + IndCallDescSize);
    uint32_t FuncDescSize =
        *reinterpret_cast<uint32_t *>(BinContents + Shdr->sh_offset + 28 +
                                      IndCallDescSize + IndCallTargetDescSize);
    Result.IndCallDescriptions = reinterpret_cast<IndCallDescription *>(
        BinContents + Shdr->sh_offset + 24);
    Result.IndCallTargets = reinterpret_cast<IndCallTargetDescription *>(
        BinContents + Shdr->sh_offset + 28 + IndCallDescSize);
    Result.FuncDescriptions = BinContents + Shdr->sh_offset + 32 +
                              IndCallDescSize + IndCallTargetDescSize;
    Result.Strings = reinterpret_cast<char *>(
        BinContents + Shdr->sh_offset + 32 + IndCallDescSize +
        IndCallTargetDescSize + FuncDescSize);
    return Result;
  }
  const char ErrMsg[] =
      "BOLT instrumentation runtime error: could not find section "
      ".bolt.instr.tables\n";
  reportError(ErrMsg, sizeof(ErrMsg));
  return Result;
}
#else
ProfileWriterContext readDescriptions() {
  ProfileWriterContext Result;
  const char ErrMsg[] =
    "BOLT instrumentation runtime error: unsupported binary format.\n";
  reportError(ErrMsg, sizeof(ErrMsg));
  return Result;
}
#endif

/// Debug by printing overall metadata global numbers to check it is sane
void printStats(const ProfileWriterContext &Ctx) {
  char StatMsg[BufSize];
  char *StatPtr = StatMsg;
  StatPtr =
      strCopy(StatPtr,
              "\nBOLT INSTRUMENTATION RUNTIME STATISTICS\n\nIndCallDescSize: ");
  StatPtr = intToStr(StatPtr,
                     Ctx.FuncDescriptions -
                         reinterpret_cast<uint8_t *>(Ctx.IndCallDescriptions),
                     10);
  StatPtr = strCopy(StatPtr, "\nFuncDescSize: ");
  StatPtr = intToStr(
      StatPtr,
      reinterpret_cast<uint8_t *>(Ctx.Strings) - Ctx.FuncDescriptions, 10);
  StatPtr = strCopy(StatPtr, "\n__bolt_instr_num_ind_calls: ");
  StatPtr = intToStr(StatPtr, __bolt_instr_num_ind_calls, 10);
  StatPtr = strCopy(StatPtr, "\n__bolt_instr_num_funcs: ");
  StatPtr = intToStr(StatPtr, __bolt_instr_num_funcs, 10);
  StatPtr = strCopy(StatPtr, "\n");
  __write(2, StatMsg, StatPtr - StatMsg);
}

/// This is part of a simple CFG representation in memory, where we store
/// a dynamically sized array of input and output edges per node, and store
/// a dynamically sized array of nodes per graph. We also store the spanning
/// tree edges for that CFG in a separate array of nodes in
/// \p SpanningTreeNodes, while the regular nodes live in \p CFGNodes.
struct Edge {
  uint32_t Node; // Index in nodes array regarding the destination of this edge
  uint32_t ID;   // Edge index in an array comprising all edges of the graph
};

/// A regular graph node or a spanning tree node
struct Node {
  uint32_t NumInEdges{0};  // Input edge count used to size InEdge
  uint32_t NumOutEdges{0}; // Output edge count used to size OutEdges
  Edge *InEdges{nullptr};  // Created and managed by \p Graph
  Edge *OutEdges{nullptr}; // ditto
};

/// Main class for CFG representation in memory. Manages object creation and
/// destruction, populates an array of CFG nodes as well as corresponding
/// spanning tree nodes.
struct Graph {
  uint32_t NumNodes;
  Node *CFGNodes;
  Node *SpanningTreeNodes;
  uint64_t *EdgeFreqs;
  uint64_t *CallFreqs;
  BumpPtrAllocator &Alloc;
  const FunctionDescription &D;

  /// Reads a list of edges from function description \p D and builds
  /// the graph from it. Allocates several internal dynamic structures that are
  /// later destroyed by ~Graph() and uses \p Alloc. D.LeafNodes contain all
  /// spanning tree leaf nodes descriptions (their counters). They are the seed
  /// used to compute the rest of the missing edge counts in a bottom-up
  /// traversal of the spanning tree.
  Graph(BumpPtrAllocator &Alloc, const FunctionDescription &D,
        const uint64_t *Counters, ProfileWriterContext &Ctx);
  ~Graph();
  void dump() const;

private:
  void computeEdgeFrequencies(const uint64_t *Counters,
                              ProfileWriterContext &Ctx);
  void dumpEdgeFreqs() const;
};

Graph::Graph(BumpPtrAllocator &Alloc, const FunctionDescription &D,
             const uint64_t *Counters, ProfileWriterContext &Ctx)
    : Alloc(Alloc), D(D) {
  DEBUG(reportNumber("G = 0x", (uint64_t)this, 16));
  // First pass to determine number of nodes
  int32_t MaxNodes = -1;
  CallFreqs = nullptr;
  EdgeFreqs = nullptr;
  for (int I = 0; I < D.NumEdges; ++I) {
    if (static_cast<int32_t>(D.Edges[I].FromNode) > MaxNodes)
      MaxNodes = D.Edges[I].FromNode;
    if (static_cast<int32_t>(D.Edges[I].ToNode) > MaxNodes)
      MaxNodes = D.Edges[I].ToNode;
  }
  for (int I = 0; I < D.NumLeafNodes; ++I) {
    if (static_cast<int32_t>(D.LeafNodes[I].Node) > MaxNodes)
      MaxNodes = D.LeafNodes[I].Node;
  }
  for (int I = 0; I < D.NumCalls; ++I) {
    if (static_cast<int32_t>(D.Calls[I].FromNode) > MaxNodes)
      MaxNodes = D.Calls[I].FromNode;
  }
  // No nodes? Nothing to do
  if (MaxNodes < 0) {
    DEBUG(report("No nodes!\n"));
    CFGNodes = nullptr;
    SpanningTreeNodes = nullptr;
    NumNodes = 0;
    return;
  }
  ++MaxNodes;
  DEBUG(reportNumber("NumNodes = ", MaxNodes, 10));
  NumNodes = static_cast<uint32_t>(MaxNodes);

  // Initial allocations
  CFGNodes = new (Alloc) Node[MaxNodes];
  DEBUG(reportNumber("G->CFGNodes = 0x", (uint64_t)CFGNodes, 16));
  SpanningTreeNodes = new (Alloc) Node[MaxNodes];
  DEBUG(reportNumber("G->SpanningTreeNodes = 0x",
                     (uint64_t)SpanningTreeNodes, 16));

  // Figure out how much to allocate to each vector (in/out edge sets)
  for (int I = 0; I < D.NumEdges; ++I) {
    CFGNodes[D.Edges[I].FromNode].NumOutEdges++;
    CFGNodes[D.Edges[I].ToNode].NumInEdges++;
    if (D.Edges[I].Counter != 0xffffffff)
      continue;

    SpanningTreeNodes[D.Edges[I].FromNode].NumOutEdges++;
    SpanningTreeNodes[D.Edges[I].ToNode].NumInEdges++;
  }

  // Allocate in/out edge sets
  for (int I = 0; I < MaxNodes; ++I) {
    if (CFGNodes[I].NumInEdges > 0)
      CFGNodes[I].InEdges = new (Alloc) Edge[CFGNodes[I].NumInEdges];
    if (CFGNodes[I].NumOutEdges > 0)
      CFGNodes[I].OutEdges = new (Alloc) Edge[CFGNodes[I].NumOutEdges];
    if (SpanningTreeNodes[I].NumInEdges > 0)
      SpanningTreeNodes[I].InEdges =
          new (Alloc) Edge[SpanningTreeNodes[I].NumInEdges];
    if (SpanningTreeNodes[I].NumOutEdges > 0)
      SpanningTreeNodes[I].OutEdges =
          new (Alloc) Edge[SpanningTreeNodes[I].NumOutEdges];
    CFGNodes[I].NumInEdges = 0;
    CFGNodes[I].NumOutEdges = 0;
    SpanningTreeNodes[I].NumInEdges = 0;
    SpanningTreeNodes[I].NumOutEdges = 0;
  }

  // Fill in/out edge sets
  for (int I = 0; I < D.NumEdges; ++I) {
    const uint32_t Src = D.Edges[I].FromNode;
    const uint32_t Dst = D.Edges[I].ToNode;
    Edge *E = &CFGNodes[Src].OutEdges[CFGNodes[Src].NumOutEdges++];
    E->Node = Dst;
    E->ID = I;

    E = &CFGNodes[Dst].InEdges[CFGNodes[Dst].NumInEdges++];
    E->Node = Src;
    E->ID = I;

    if (D.Edges[I].Counter != 0xffffffff)
      continue;

    E = &SpanningTreeNodes[Src]
             .OutEdges[SpanningTreeNodes[Src].NumOutEdges++];
    E->Node = Dst;
    E->ID = I;

    E = &SpanningTreeNodes[Dst]
             .InEdges[SpanningTreeNodes[Dst].NumInEdges++];
    E->Node = Src;
    E->ID = I;
  }

  computeEdgeFrequencies(Counters, Ctx);
}

Graph::~Graph() {
  if (CallFreqs)
    Alloc.deallocate(CallFreqs);
  if (EdgeFreqs)
    Alloc.deallocate(EdgeFreqs);
  for (int I = NumNodes - 1; I >= 0; --I) {
    if (SpanningTreeNodes[I].OutEdges)
      Alloc.deallocate(SpanningTreeNodes[I].OutEdges);
    if (SpanningTreeNodes[I].InEdges)
      Alloc.deallocate(SpanningTreeNodes[I].InEdges);
    if (CFGNodes[I].OutEdges)
      Alloc.deallocate(CFGNodes[I].OutEdges);
    if (CFGNodes[I].InEdges)
      Alloc.deallocate(CFGNodes[I].InEdges);
  }
  if (SpanningTreeNodes)
    Alloc.deallocate(SpanningTreeNodes);
  if (CFGNodes)
    Alloc.deallocate(CFGNodes);
}

void Graph::dump() const {
  reportNumber("Dumping graph with number of nodes: ", NumNodes, 10);
  report("  Full graph:\n");
  for (int I = 0; I < NumNodes; ++I) {
    const Node *N = &CFGNodes[I];
    reportNumber("    Node #", I, 10);
    reportNumber("      InEdges total ", N->NumInEdges, 10);
    for (int J = 0; J < N->NumInEdges; ++J)
      reportNumber("        ", N->InEdges[J].Node, 10);
    reportNumber("      OutEdges total ", N->NumOutEdges, 10);
    for (int J = 0; J < N->NumOutEdges; ++J)
      reportNumber("        ", N->OutEdges[J].Node, 10);
    report("\n");
  }
  report("  Spanning tree:\n");
  for (int I = 0; I < NumNodes; ++I) {
    const Node *N = &SpanningTreeNodes[I];
    reportNumber("    Node #", I, 10);
    reportNumber("      InEdges total ", N->NumInEdges, 10);
    for (int J = 0; J < N->NumInEdges; ++J)
      reportNumber("        ", N->InEdges[J].Node, 10);
    reportNumber("      OutEdges total ", N->NumOutEdges, 10);
    for (int J = 0; J < N->NumOutEdges; ++J)
      reportNumber("        ", N->OutEdges[J].Node, 10);
    report("\n");
  }
}

void Graph::dumpEdgeFreqs() const {
  reportNumber(
      "Dumping edge frequencies for graph with num edges: ", D.NumEdges, 10);
  for (int I = 0; I < D.NumEdges; ++I) {
    reportNumber("* Src: ", D.Edges[I].FromNode, 10);
    reportNumber("  Dst: ", D.Edges[I].ToNode, 10);
    reportNumber("    Cnt: ", EdgeFreqs[I], 10);
  }
}

/// Auxiliary map structure for fast lookups of which calls map to each node of
/// the function CFG
struct NodeToCallsMap {
  struct MapEntry {
    uint32_t NumCalls;
    uint32_t *Calls;
  };
  MapEntry *Entries;
  BumpPtrAllocator &Alloc;
  const uint32_t NumNodes;

  NodeToCallsMap(BumpPtrAllocator &Alloc, const FunctionDescription &D,
                 uint32_t NumNodes)
      : Alloc(Alloc), NumNodes(NumNodes) {
    Entries = new (Alloc, 0) MapEntry[NumNodes];
    for (int I = 0; I < D.NumCalls; ++I) {
      DEBUG(reportNumber("Registering call in node ", D.Calls[I].FromNode, 10));
      ++Entries[D.Calls[I].FromNode].NumCalls;
    }
    for (int I = 0; I < NumNodes; ++I) {
      Entries[I].Calls = Entries[I].NumCalls ? new (Alloc)
                                                   uint32_t[Entries[I].NumCalls]
                                             : nullptr;
      Entries[I].NumCalls = 0;
    }
    for (int I = 0; I < D.NumCalls; ++I) {
      auto &Entry = Entries[D.Calls[I].FromNode];
      Entry.Calls[Entry.NumCalls++] = I;
    }
  }

  /// Set the frequency of all calls in node \p NodeID to Freq. However, if
  /// the calls have their own counters and do not depend on the basic block
  /// counter, this means they have landing pads and throw exceptions. In this
  /// case, set their frequency with their counters and return the maximum
  /// value observed in such counters. This will be used as the new frequency
  /// at basic block entry. This is used to fix the CFG edge frequencies in the
  /// presence of exceptions.
  uint64_t visitAllCallsIn(uint32_t NodeID, uint64_t Freq, uint64_t *CallFreqs,
                           const FunctionDescription &D,
                           const uint64_t *Counters,
                           ProfileWriterContext &Ctx) const {
    const auto &Entry = Entries[NodeID];
    uint64_t MaxValue = 0ull;
    for (int I = 0, E = Entry.NumCalls; I != E; ++I) {
      const auto CallID = Entry.Calls[I];
      DEBUG(reportNumber("  Setting freq for call ID: ", CallID, 10));
      auto &CallDesc = D.Calls[CallID];
      if (CallDesc.Counter == 0xffffffff) {
        CallFreqs[CallID] = Freq;
        DEBUG(reportNumber("  with : ", Freq, 10));
      } else {
        const auto CounterVal = Counters[CallDesc.Counter];
        CallFreqs[CallID] = CounterVal;
        MaxValue = CounterVal > MaxValue ? CounterVal : MaxValue;
        DEBUG(reportNumber("  with (private counter) : ", CounterVal, 10));
      }
      DEBUG(reportNumber("  Address: 0x", CallDesc.TargetAddress, 16));
      if (CallFreqs[CallID] > 0)
        Ctx.CallFlowTable->get(CallDesc.TargetAddress).Calls +=
            CallFreqs[CallID];
    }
    return MaxValue;
  }

  ~NodeToCallsMap() {
    for (int I = NumNodes - 1; I >= 0; --I) {
      if (Entries[I].Calls)
        Alloc.deallocate(Entries[I].Calls);
    }
    Alloc.deallocate(Entries);
  }
};

/// Fill an array with the frequency of each edge in the function represented
/// by G, as well as another array for each call.
void Graph::computeEdgeFrequencies(const uint64_t *Counters,
                                   ProfileWriterContext &Ctx) {
  if (NumNodes == 0)
    return;

  EdgeFreqs = D.NumEdges ? new (Alloc, 0) uint64_t [D.NumEdges] : nullptr;
  CallFreqs = D.NumCalls ? new (Alloc, 0) uint64_t [D.NumCalls] : nullptr;

  // Setup a lookup for calls present in each node (BB)
  NodeToCallsMap *CallMap = new (Alloc) NodeToCallsMap(Alloc, D, NumNodes);

  // Perform a bottom-up, BFS traversal of the spanning tree in G. Edges in the
  // spanning tree don't have explicit counters. We must infer their value using
  // a linear combination of other counters (sum of counters of the outgoing
  // edges minus sum of counters of the incoming edges).
  uint32_t *Stack = new (Alloc) uint32_t [NumNodes];
  uint32_t StackTop = 0;
  enum Status : uint8_t { S_NEW = 0, S_VISITING, S_VISITED };
  Status *Visited = new (Alloc, 0) Status[NumNodes];
  uint64_t *LeafFrequency = new (Alloc, 0) uint64_t[NumNodes];
  uint64_t *EntryAddress = new (Alloc, 0) uint64_t[NumNodes];

  // Setup a fast lookup for frequency of leaf nodes, which have special
  // basic block frequency instrumentation (they are not edge profiled).
  for (int I = 0; I < D.NumLeafNodes; ++I) {
    LeafFrequency[D.LeafNodes[I].Node] = Counters[D.LeafNodes[I].Counter];
    DEBUG({
      if (Counters[D.LeafNodes[I].Counter] > 0) {
        reportNumber("Leaf Node# ", D.LeafNodes[I].Node, 10);
        reportNumber("     Counter: ", Counters[D.LeafNodes[I].Counter], 10);
      }
    });
  }
  for (int I = 0; I < D.NumEntryNodes; ++I) {
    EntryAddress[D.EntryNodes[I].Node] = D.EntryNodes[I].Address;
    DEBUG({
        reportNumber("Entry Node# ", D.EntryNodes[I].Node, 10);
        reportNumber("      Address: ", D.EntryNodes[I].Address, 16);
    });
  }
  // Add all root nodes to the stack
  for (int I = 0; I < NumNodes; ++I) {
    if (SpanningTreeNodes[I].NumInEdges == 0)
      Stack[StackTop++] = I;
  }
  // Empty stack?
  if (StackTop == 0) {
    DEBUG(report("Empty stack!\n"));
    Alloc.deallocate(EntryAddress);
    Alloc.deallocate(LeafFrequency);
    Alloc.deallocate(Visited);
    Alloc.deallocate(Stack);
    CallMap->~NodeToCallsMap();
    Alloc.deallocate(CallMap);
    if (CallFreqs)
      Alloc.deallocate(CallFreqs);
    if (EdgeFreqs)
      Alloc.deallocate(EdgeFreqs);
    EdgeFreqs = nullptr;
    CallFreqs = nullptr;
    return;
  }
  // Add all known edge counts, will infer the rest
  for (int I = 0; I < D.NumEdges; ++I) {
    const uint32_t C = D.Edges[I].Counter;
    if (C == 0xffffffff) // inferred counter - we will compute its value
      continue;
    EdgeFreqs[I] = Counters[C];
  }

  while (StackTop > 0) {
    const uint32_t Cur = Stack[--StackTop];
    DEBUG({
      if (Visited[Cur] == S_VISITING)
        report("(visiting) ");
      else
        report("(new) ");
      reportNumber("Cur: ", Cur, 10);
    });

    // This shouldn't happen in a tree
    assert(Visited[Cur] != S_VISITED, "should not have visited nodes in stack");
    if (Visited[Cur] == S_NEW) {
      Visited[Cur] = S_VISITING;
      Stack[StackTop++] = Cur;
      assert(StackTop <= NumNodes, "stack grew too large");
      for (int I = 0, E = SpanningTreeNodes[Cur].NumOutEdges; I < E; ++I) {
        const uint32_t Succ = SpanningTreeNodes[Cur].OutEdges[I].Node;
        Stack[StackTop++] = Succ;
        assert(StackTop <= NumNodes, "stack grew too large");
     }
      continue;
    }
    Visited[Cur] = S_VISITED;

    // Establish our node frequency based on outgoing edges, which should all be
    // resolved by now.
    int64_t CurNodeFreq = LeafFrequency[Cur];
    // Not a leaf?
    if (!CurNodeFreq) {
      for (int I = 0, E = CFGNodes[Cur].NumOutEdges; I != E; ++I) {
        const uint32_t SuccEdge = CFGNodes[Cur].OutEdges[I].ID;
        CurNodeFreq += EdgeFreqs[SuccEdge];
      }
    }
    if (CurNodeFreq < 0)
      CurNodeFreq = 0;

    const uint64_t CallFreq = CallMap->visitAllCallsIn(
        Cur, CurNodeFreq > 0 ? CurNodeFreq : 0, CallFreqs, D, Counters, Ctx);

    // Exception handling affected our output flow? Fix with calls info
    DEBUG({
      if (CallFreq > CurNodeFreq)
        report("Bumping node frequency with call info\n");
    });
    CurNodeFreq = CallFreq > CurNodeFreq ? CallFreq : CurNodeFreq;

    if (CurNodeFreq > 0) {
      if (uint64_t Addr = EntryAddress[Cur]) {
        DEBUG(
            reportNumber("  Setting flow at entry point address 0x", Addr, 16));
        DEBUG(reportNumber("  with: ", CurNodeFreq, 10));
        Ctx.CallFlowTable->get(Addr).Val = CurNodeFreq;
      }
    }

    // No parent? Reached a tree root, limit to call frequency updating.
    if (SpanningTreeNodes[Cur].NumInEdges == 0) {
      continue;
    }

    assert(SpanningTreeNodes[Cur].NumInEdges == 1, "must have 1 parent");
    const uint32_t Parent = SpanningTreeNodes[Cur].InEdges[0].Node;
    const uint32_t ParentEdge = SpanningTreeNodes[Cur].InEdges[0].ID;

    // Calculate parent edge freq.
    int64_t ParentEdgeFreq = CurNodeFreq;
    for (int I = 0, E = CFGNodes[Cur].NumInEdges; I != E; ++I) {
      const uint32_t PredEdge = CFGNodes[Cur].InEdges[I].ID;
      ParentEdgeFreq -= EdgeFreqs[PredEdge];
    }

    // Sometimes the conservative CFG that BOLT builds will lead to incorrect
    // flow computation. For example, in a BB that transitively calls the exit
    // syscall, BOLT will add a fall-through successor even though it should not
    // have any successors. So this block execution will likely be wrong. We
    // tolerate this imperfection since this case should be quite infrequent.
    if (ParentEdgeFreq < 0) {
      DEBUG(dumpEdgeFreqs());
      DEBUG(report("WARNING: incorrect flow"));
      ParentEdgeFreq = 0;
    }
    DEBUG(reportNumber("  Setting freq for ParentEdge: ", ParentEdge, 10));
    DEBUG(reportNumber("  with ParentEdgeFreq: ", ParentEdgeFreq, 10));
    EdgeFreqs[ParentEdge] = ParentEdgeFreq;
  }

  Alloc.deallocate(EntryAddress);
  Alloc.deallocate(LeafFrequency);
  Alloc.deallocate(Visited);
  Alloc.deallocate(Stack);
  CallMap->~NodeToCallsMap();
  Alloc.deallocate(CallMap);
  DEBUG(dumpEdgeFreqs());
}

/// Write to \p FD all of the edge profiles for function \p FuncDesc. Uses
/// \p Alloc to allocate helper dynamic structures used to compute profile for
/// edges that we do not explictly instrument.
const uint8_t *writeFunctionProfile(int FD, ProfileWriterContext &Ctx,
                                    const uint8_t *FuncDesc,
                                    BumpPtrAllocator &Alloc) {
  const FunctionDescription F(FuncDesc);
  const uint8_t *next = FuncDesc + F.getSize();

  // Skip funcs we know are cold
#ifndef ENABLE_DEBUG
  uint64_t CountersFreq = 0;
  for (int I = 0; I < F.NumLeafNodes; ++I) {
    CountersFreq += __bolt_instr_locations[F.LeafNodes[I].Counter];
  }
  if (CountersFreq == 0) {
    for (int I = 0; I < F.NumEdges; ++I) {
      const uint32_t C = F.Edges[I].Counter;
      if (C == 0xffffffff)
        continue;
      CountersFreq += __bolt_instr_locations[C];
    }
    if (CountersFreq == 0) {
      for (int I = 0; I < F.NumCalls; ++I) {
        const uint32_t C = F.Calls[I].Counter;
        if (C == 0xffffffff)
          continue;
        CountersFreq += __bolt_instr_locations[C];
      }
      if (CountersFreq == 0)
        return next;
    }
  }
#endif

  Graph *G = new (Alloc) Graph(Alloc, F, __bolt_instr_locations, Ctx);
  DEBUG(G->dump());
  if (!G->EdgeFreqs && !G->CallFreqs) {
    G->~Graph();
    Alloc.deallocate(G);
    return next;
  }

  for (int I = 0; I < F.NumEdges; ++I) {
    const uint64_t Freq = G->EdgeFreqs[I];
    if (Freq == 0)
      continue;
    const EdgeDescription *Desc = &F.Edges[I];
    char LineBuf[BufSize];
    char *Ptr = LineBuf;
    Ptr = serializeLoc(Ctx, Ptr, Desc->From, BufSize);
    Ptr = serializeLoc(Ctx, Ptr, Desc->To, BufSize - (Ptr - LineBuf));
    Ptr = strCopy(Ptr, "0 ", BufSize - (Ptr - LineBuf) - 22);
    Ptr = intToStr(Ptr, Freq, 10);
    *Ptr++ = '\n';
    __write(FD, LineBuf, Ptr - LineBuf);
  }

  for (int I = 0; I < F.NumCalls; ++I) {
    const uint64_t Freq = G->CallFreqs[I];
    if (Freq == 0)
      continue;
    char LineBuf[BufSize];
    char *Ptr = LineBuf;
    const CallDescription *Desc = &F.Calls[I];
    Ptr = serializeLoc(Ctx, Ptr, Desc->From, BufSize);
    Ptr = serializeLoc(Ctx, Ptr, Desc->To, BufSize - (Ptr - LineBuf));
    Ptr = strCopy(Ptr, "0 ", BufSize - (Ptr - LineBuf) - 25);
    Ptr = intToStr(Ptr, Freq, 10);
    *Ptr++ = '\n';
    __write(FD, LineBuf, Ptr - LineBuf);
  }

  G->~Graph();
  Alloc.deallocate(G);
  return next;
}

const IndCallTargetDescription *
ProfileWriterContext::lookupIndCallTarget(uint64_t Target) const {
  uint32_t B = 0;
  uint32_t E = __bolt_instr_num_ind_targets;
  if (E == 0)
    return nullptr;
  do {
    uint32_t I = (E - B) / 2 + B;
    if (IndCallTargets[I].Address == Target)
      return &IndCallTargets[I];
    if (IndCallTargets[I].Address < Target)
      B = I + 1;
    else
      E = I;
  } while (B < E);
  return nullptr;
}

/// Write a single indirect call <src, target> pair to the fdata file
void visitIndCallCounter(IndirectCallHashTable::MapEntry &Entry,
                         int FD, int CallsiteID,
                         ProfileWriterContext *Ctx) {
  if (Entry.Val == 0)
    return;
  DEBUG(reportNumber("Target func 0x", Entry.Key, 16));
  DEBUG(reportNumber("Target freq: ", Entry.Val, 10));
  const IndCallDescription *CallsiteDesc =
      &Ctx->IndCallDescriptions[CallsiteID];
  const IndCallTargetDescription *TargetDesc =
      Ctx->lookupIndCallTarget(Entry.Key);
  if (!TargetDesc) {
    DEBUG(report("Failed to lookup indirect call target\n"));
    char LineBuf[BufSize];
    char *Ptr = LineBuf;
    Ptr = serializeLoc(*Ctx, Ptr, *CallsiteDesc, BufSize);
    Ptr = strCopy(Ptr, "0 [unknown] 0 0 ", BufSize - (Ptr - LineBuf) - 40);
    Ptr = intToStr(Ptr, Entry.Val, 10);
    *Ptr++ = '\n';
    __write(FD, LineBuf, Ptr - LineBuf);
    return;
  }
  Ctx->CallFlowTable->get(TargetDesc->Address).Calls += Entry.Val;
  char LineBuf[BufSize];
  char *Ptr = LineBuf;
  Ptr = serializeLoc(*Ctx, Ptr, *CallsiteDesc, BufSize);
  Ptr = serializeLoc(*Ctx, Ptr, TargetDesc->Loc, BufSize - (Ptr - LineBuf));
  Ptr = strCopy(Ptr, "0 ", BufSize - (Ptr - LineBuf) - 25);
  Ptr = intToStr(Ptr, Entry.Val, 10);
  *Ptr++ = '\n';
  __write(FD, LineBuf, Ptr - LineBuf);
}

/// Write to \p FD all of the indirect call profiles.
void writeIndirectCallProfile(int FD, ProfileWriterContext &Ctx) {
  for (int I = 0; I < __bolt_instr_num_ind_calls; ++I) {
    DEBUG(reportNumber("IndCallsite #", I, 10));
    GlobalIndCallCounters[I].forEachElement(visitIndCallCounter, FD, I, &Ctx);
  }
}

/// Check a single call flow for a callee versus all known callers. If there are
/// less callers than what the callee expects, write the difference with source
/// [unknown] in the profile.
void visitCallFlowEntry(CallFlowHashTable::MapEntry &Entry, int FD,
                        ProfileWriterContext *Ctx) {
  DEBUG(reportNumber("Call flow entry address: 0x", Entry.Key, 16));
  DEBUG(reportNumber("Calls: ", Entry.Calls, 10));
  DEBUG(reportNumber("Reported entry frequency: ", Entry.Val, 10));
  DEBUG({
    if (Entry.Calls > Entry.Val)
      report("  More calls than expected!\n");
  });
  if (Entry.Val <= Entry.Calls)
    return;
  DEBUG(reportNumber(
      "  Balancing calls with traffic: ", Entry.Val - Entry.Calls, 10));
  const IndCallTargetDescription *TargetDesc =
      Ctx->lookupIndCallTarget(Entry.Key);
  if (!TargetDesc) {
    // There is probably something wrong with this callee and this should be
    // investigated, but I don't want to assert and lose all data collected.
    DEBUG(report("WARNING: failed to look up call target!\n"));
    return;
  }
  char LineBuf[BufSize];
  char *Ptr = LineBuf;
  Ptr = strCopy(Ptr, "0 [unknown] 0 ", BufSize);
  Ptr = serializeLoc(*Ctx, Ptr, TargetDesc->Loc, BufSize - (Ptr - LineBuf));
  Ptr = strCopy(Ptr, "0 ", BufSize - (Ptr - LineBuf) - 25);
  Ptr = intToStr(Ptr, Entry.Val - Entry.Calls, 10);
  *Ptr++ = '\n';
  __write(FD, LineBuf, Ptr - LineBuf);
}

/// Open fdata file for writing and return a valid file descriptor, aborting
/// program upon failure.
int openProfile() {
  // Build the profile name string by appending our PID
  char Buf[BufSize];
  char *Ptr = Buf;
  uint64_t PID = __getpid();
  Ptr = strCopy(Buf, __bolt_instr_filename, BufSize);
  if (__bolt_instr_use_pid) {
    Ptr = strCopy(Ptr, ".", BufSize - (Ptr - Buf + 1));
    Ptr = intToStr(Ptr, PID, 10);
    Ptr = strCopy(Ptr, ".fdata", BufSize - (Ptr - Buf + 1));
  }
  *Ptr++ = '\0';
  uint64_t FD = __open(Buf,
                       /*flags=*/0x241 /*O_WRONLY|O_TRUNC|O_CREAT*/,
                       /*mode=*/0666);
  if (static_cast<int64_t>(FD) < 0) {
    report("Error while trying to open profile file for writing: ");
    report(Buf);
    reportNumber("\nFailed with error number: 0x",
                 0 - static_cast<int64_t>(FD), 16);
    __exit(1);
  }
  return FD;
}

} // anonymous namespace

/// Reset all counters in case you want to start profiling a new phase of your
/// program independently of prior phases.
/// The address of this function is printed by BOLT and this can be called by
/// any attached debugger during runtime. There is a useful oneliner for gdb:
///
///   gdb -p $(pgrep -xo PROCESSNAME) -ex 'p ((void(*)())0xdeadbeef)()' \
///     -ex 'set confirm off' -ex quit
///
/// Where 0xdeadbeef is this function address and PROCESSNAME your binary file
/// name.
extern "C" void __bolt_instr_clear_counters() {
  memSet(reinterpret_cast<char *>(__bolt_instr_locations), 0,
         __bolt_num_counters * 8);
  for (int I = 0; I < __bolt_instr_num_ind_calls; ++I) {
    GlobalIndCallCounters[I].resetCounters();
  }
}

/// This is the entry point for profile writing.
/// There are three ways of getting here:
///
///  * Program execution ended, finalization methods are running and BOLT
///    hooked into FINI from your binary dynamic section;
///  * You used the sleep timer option and during initialization we forked
///    a separete process that will call this function periodically;
///  * BOLT prints this function address so you can attach a debugger and
///    call this function directly to get your profile written to disk
///    on demand.
///
extern "C" void __bolt_instr_data_dump() {
  // Already dumping
  if (!GlobalWriteProfileMutex->acquire())
    return;

  BumpPtrAllocator HashAlloc;
  HashAlloc.setMaxSize(0x6400000);
  ProfileWriterContext Ctx = readDescriptions();
  Ctx.CallFlowTable = new (HashAlloc, 0) CallFlowHashTable(HashAlloc);

  DEBUG(printStats(Ctx));

  int FD = openProfile();

  BumpPtrAllocator Alloc;
  const uint8_t *FuncDesc = Ctx.FuncDescriptions;
  for (int I = 0, E = __bolt_instr_num_funcs; I < E; ++I) {
    FuncDesc = writeFunctionProfile(FD, Ctx, FuncDesc, Alloc);
    Alloc.clear();
    DEBUG(reportNumber("FuncDesc now: ", (uint64_t)FuncDesc, 16));
  }
  assert(FuncDesc == (void *)Ctx.Strings,
         "FuncDesc ptr must be equal to stringtable");

  writeIndirectCallProfile(FD, Ctx);
  Ctx.CallFlowTable->forEachElement(visitCallFlowEntry, FD, &Ctx);

  __close(FD);
  __munmap(Ctx.MMapPtr, Ctx.MMapSize);
  __close(Ctx.FileDesc);
  HashAlloc.destroy();
  GlobalWriteProfileMutex->release();
  DEBUG(report("Finished writing profile.\n"));
}

/// Event loop for our child process spawned during setup to dump profile data
/// at user-specified intervals
void watchProcess() {
  timespec ts, rem;
  uint64_t Ellapsed = 0ull;
  ts.tv_sec = 1;
  ts.tv_nsec = 0;
  while (1) {
    __nanosleep(&ts, &rem);
    // This means our parent process died, so no need for us to keep dumping.
    // Notice that make and some systems will wait until all child processes
    // of a command finishes before proceeding, so it is important to exit as
    // early as possible once our parent dies.
    if (__getppid() == 1) {
      break;
    }
    if (++Ellapsed < __bolt_instr_sleep_time)
      continue;
    Ellapsed = 0;
    __bolt_instr_data_dump();
    __bolt_instr_clear_counters();
  }
  DEBUG(report("My parent process is dead, bye!\n"));
  __exit(0);
}

extern "C" void __bolt_instr_indirect_call();
extern "C" void __bolt_instr_indirect_tailcall();

/// Initialization code
extern "C" void __bolt_instr_setup() {
  const uint64_t CountersStart =
      reinterpret_cast<uint64_t>(&__bolt_instr_locations[0]);
  const uint64_t CountersEnd = alignTo(
      reinterpret_cast<uint64_t>(&__bolt_instr_locations[__bolt_num_counters]),
      0x1000);
  DEBUG(reportNumber("replace mmap start: ", CountersStart, 16));
  DEBUG(reportNumber("replace mmap stop: ", CountersEnd, 16));
  assert (CountersEnd > CountersStart, "no counters");
  // Maps our counters to be shared instead of private, so we keep counting for
  // forked processes
  __mmap(CountersStart, CountersEnd - CountersStart,
         0x3 /*PROT_READ|PROT_WRITE*/,
         0x31 /*MAP_ANONYMOUS | MAP_SHARED | MAP_FIXED*/, -1, 0);

  __bolt_trampoline_ind_call = __bolt_instr_indirect_call;
  __bolt_trampoline_ind_tailcall = __bolt_instr_indirect_tailcall;
  // Conservatively reserve 100MiB shared pages
  GlobalAlloc.setMaxSize(0x6400000);
  GlobalAlloc.setShared(true);
  GlobalWriteProfileMutex = new (GlobalAlloc, 0) Mutex();
  if (__bolt_instr_num_ind_calls > 0)
    GlobalIndCallCounters =
        new (GlobalAlloc, 0) IndirectCallHashTable[__bolt_instr_num_ind_calls];

  if (__bolt_instr_sleep_time != 0) {
    if (auto PID = __fork())
      return;
    watchProcess();
  }
}

extern "C" void instrumentIndirectCall(uint64_t Target, uint64_t IndCallID) {
  GlobalIndCallCounters[IndCallID].incrementVal(Target, GlobalAlloc);
}

#define SAVE_ALL                                                               \
  "push %%rax\n"                                                               \
  "push %%rbx\n"                                                               \
  "push %%rcx\n"                                                               \
  "push %%rdx\n"                                                               \
  "push %%rdi\n"                                                               \
  "push %%rsi\n"                                                               \
  "push %%rbp\n"                                                               \
  "push %%r8\n"                                                                \
  "push %%r9\n"                                                                \
  "push %%r10\n"                                                               \
  "push %%r11\n"                                                               \
  "push %%r12\n"                                                               \
  "push %%r13\n"                                                               \
  "push %%r14\n"                                                               \
  "push %%r15\n"

#define RESTORE_ALL                                                            \
  "pop %%r15\n"                                                                \
  "pop %%r14\n"                                                                \
  "pop %%r13\n"                                                                \
  "pop %%r12\n"                                                                \
  "pop %%r11\n"                                                                \
  "pop %%r10\n"                                                                \
  "pop %%r9\n"                                                                 \
  "pop %%r8\n"                                                                 \
  "pop %%rbp\n"                                                                \
  "pop %%rsi\n"                                                                \
  "pop %%rdi\n"                                                                \
  "pop %%rdx\n"                                                                \
  "pop %%rcx\n"                                                                \
  "pop %%rbx\n"                                                                \
  "pop %%rax\n"

/// We receive as in-stack arguments the identifier of the indirect call site
/// as well as the target address for the call
extern "C" __attribute((naked)) void __bolt_instr_indirect_call()
{
  __asm__ __volatile__(SAVE_ALL
                       "mov 0x88(%%rsp), %%rdi\n"
                       "mov 0x80(%%rsp), %%rsi\n"
                       "call instrumentIndirectCall\n"
                       RESTORE_ALL
                       "pop %%rdi\n"
                       "add $16, %%rsp\n"
                       "xchg (%%rsp), %%rdi\n"
                       "jmp *-8(%%rsp)\n"
                       :::);
}

extern "C" __attribute((naked)) void __bolt_instr_indirect_tailcall()
{
  __asm__ __volatile__(SAVE_ALL
                       "mov 0x80(%%rsp), %%rdi\n"
                       "mov 0x78(%%rsp), %%rsi\n"
                       "call instrumentIndirectCall\n"
                       RESTORE_ALL
                       "add $16, %%rsp\n"
                       "pop %%rdi\n"
                       "jmp *-16(%%rsp)\n"
                       :::);
}

/// This is hooking ELF's entry, it needs to save all machine state.
extern "C" __attribute((naked)) void __bolt_instr_start()
{
  __asm__ __volatile__(SAVE_ALL
                       "call __bolt_instr_setup\n"
                       RESTORE_ALL
                       "jmp *__bolt_instr_init_ptr\n"
                       :::);
}

/// This is hooking into ELF's DT_FINI
extern "C" void __bolt_instr_fini() {
  __bolt_instr_fini_ptr();
  if (__bolt_instr_sleep_time == 0)
    __bolt_instr_data_dump();
  DEBUG(report("Finished.\n"));
}
