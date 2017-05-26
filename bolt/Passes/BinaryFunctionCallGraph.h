//===--- Passes/CallGraph.h -----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_BINARY_FUNCTION_CALLGRAPH_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_BINARY_FUNCTION_CALLGRAPH_H

#include "CallGraph.h"

#include <unordered_map>
#include <functional>
#include <deque>
#include <map>

namespace llvm {
namespace bolt {

class BinaryFunction;
class BinaryContext;

class BinaryFunctionCallGraph : public CallGraph {
public:
  NodeId maybeGetNodeId(const BinaryFunction *BF) const {
    auto Itr = FuncToNodeId.find(BF);
    return Itr != FuncToNodeId.end() ? Itr->second : InvalidId;
  }
  NodeId getNodeId(const BinaryFunction *BF) const {
    auto Itr = FuncToNodeId.find(BF);
    assert(Itr != FuncToNodeId.end());
    return Itr->second;
  }
  BinaryFunction *nodeIdToFunc(NodeId Id) {
    assert(Id < Funcs.size());
    return Funcs[Id];
  }
  const BinaryFunction *nodeIdToFunc(NodeId Id) const {
    assert(Id < Funcs.size());
    return Funcs[Id];
  }
  NodeId addNode(BinaryFunction *BF, uint32_t Size, uint64_t Samples = 0);

  /// Compute a DFS traversal of the call graph.
  std::deque<BinaryFunction *> buildTraversalOrder();

private:
  std::unordered_map<const BinaryFunction *, NodeId> FuncToNodeId;
  std::vector<BinaryFunction *> Funcs;
};

using CgFilterFunction = std::function<bool (const BinaryFunction &BF)>;
inline bool NoFilter(const BinaryFunction &) { return false; }

/// Builds a call graph from the map of BinaryFunctions provided in BFs.
/// The arguments control how the graph is constructed.
/// Filter is called on each function, any function that it returns true for
/// is omitted from the graph.
/// If IncludeColdCalls is true, then calls from cold BBs are considered for the
/// graph, otherwise they are ignored.
/// UseFunctionHotSize controls whether the hot size of a function is used when
/// filling in the Size attribute of new Nodes.
/// UseEdgeCounts is used to control if the AvgCallOffset attribute on Arcs is
/// computed using the offsets of call instructions.
BinaryFunctionCallGraph buildCallGraph(BinaryContext &BC,
                                       std::map<uint64_t, BinaryFunction> &BFs,
                                       CgFilterFunction Filter = NoFilter,
                                       bool IncludeColdCalls = true,
                                       bool UseFunctionHotSize = false,
                                       bool UseEdgeCounts = false);

}
}

#endif
