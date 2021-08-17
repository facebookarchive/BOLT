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

#ifndef LLVM_TOOLS_LLVM_BOLT_PASSES_CALLGRAPH_H
#define LLVM_TOOLS_LLVM_BOLT_PASSES_CALLGRAPH_H

#include <cassert>
#include <string>
#include <unordered_set>
#include <vector>

namespace llvm {
namespace bolt {

// TODO: find better place for this
inline int64_t hashCombine(const int64_t Seed, const int64_t Val) {
  std::hash<int64_t> Hasher;
  return Seed ^ (Hasher(Val) + 0x9e3779b9 + (Seed << 6) + (Seed >> 2));
}

/// A call graph class.
class CallGraph {
public:
  using NodeId = size_t;
  static constexpr NodeId InvalidId = -1;

  class Arc {
  public:
    struct Hash {
      int64_t operator()(const Arc &Arc) const;
    };

    Arc(NodeId S, NodeId D, double W = 0)
      : Src(S)
      , Dst(D)
      , Weight(W)
    {}
    Arc(const Arc&) = delete;

    friend bool operator==(const Arc &Lhs, const Arc &Rhs) {
      return Lhs.Src == Rhs.Src && Lhs.Dst == Rhs.Dst;
    }

    NodeId src() const { return Src; }
    NodeId dst() const { return Dst; }
    double weight() const { return Weight; }
    double avgCallOffset() const { return AvgCallOffset; }
    double normalizedWeight() const { return NormalizedWeight; }

  private:
    friend class CallGraph;
    const NodeId Src;
    const NodeId Dst;
    mutable double Weight;
    mutable double NormalizedWeight{0};
    mutable double AvgCallOffset{0};
  };

  using ArcsType = std::unordered_set<Arc, Arc::Hash>;
  using ArcIterator = ArcsType::iterator;
  using ArcConstIterator = ArcsType::const_iterator;

  class Node {
  public:
    explicit Node(uint32_t Size, uint64_t Samples = 0)
      : Size(Size), Samples(Samples)
    {}

    uint32_t size() const { return Size; }
    uint64_t samples() const { return Samples; }

    const std::vector<NodeId> &successors() const {
      return Succs;
    }
    const std::vector<NodeId> &predecessors() const {
      return Preds;
    }

  private:
    friend class CallGraph;
    uint32_t Size;
    uint64_t Samples;

    // preds and succs contain no duplicate elements and self arcs are not allowed
    std::vector<NodeId> Preds;
    std::vector<NodeId> Succs;
  };

  size_t numNodes() const {
    return Nodes.size();
  }
  const Node &getNode(const NodeId Id) const {
    assert(Id < Nodes.size());
    return Nodes[Id];
  }
  uint32_t size(const NodeId Id) const {
    assert(Id < Nodes.size());
    return Nodes[Id].Size;
  }
  uint64_t samples(const NodeId Id) const {
    assert(Id < Nodes.size());
    return Nodes[Id].Samples;
  }
  const std::vector<NodeId> &successors(const NodeId Id) const {
    assert(Id < Nodes.size());
    return Nodes[Id].Succs;
  }
  const std::vector<NodeId> &predecessors(const NodeId Id) const {
    assert(Id < Nodes.size());
    return Nodes[Id].Preds;
  }
  NodeId addNode(uint32_t Size, uint64_t Samples = 0);
  const Arc &incArcWeight(NodeId Src, NodeId Dst, double W = 1.0,
                          double Offset = 0.0);
  ArcIterator findArc(NodeId Src, NodeId Dst) {
    return Arcs.find(Arc(Src, Dst));
  }
  ArcConstIterator findArc(NodeId Src, NodeId Dst) const {
    return Arcs.find(Arc(Src, Dst));
  }
  const ArcsType &getArcs() const {
    return Arcs;
  }

  double density() const {
    return double(Arcs.size()) / (Nodes.size()*Nodes.size());
  }

  void normalizeArcWeights(bool UseEdgeCounts);

  template <typename L>
  void printDot(char* fileName, L getLabel) const;
private:
  std::vector<Node> Nodes;
  ArcsType Arcs;
};

template<class L>
void CallGraph::printDot(char* FileName, L GetLabel) const {
  FILE* File = fopen(FileName, "wt");
  if (!File) return;

  fprintf(File, "digraph g {\n");
  for (NodeId F = 0; F < Nodes.size(); F++) {
    if (Nodes[F].samples() == 0) continue;
    fprintf(
            File,
            "f%lu [label=\"%s\\nsamples=%u\\nsize=%u\"];\n",
            F,
            GetLabel(F),
            Nodes[F].samples(),
            Nodes[F].size());
  }
  for (NodeId F = 0; F < Nodes.size(); F++) {
    if (Nodes[F].samples() == 0) continue;
    for (auto Dst : Nodes[F].successors()) {
      auto Arc = findArc(F, Dst);
      fprintf(
              File,
              "f%lu -> f%u [label=\"normWgt=%.3lf,weight=%.0lf,callOffset=%.1lf\"];"
              "\n",
              F,
              Dst,
              Arc->normalizedWeight(),
              Arc->weight(),
              Arc->avgCallOffset());
    }
  }
  fprintf(File, "}\n");
  fclose(File);
}

} // namespace bolt
} // namespace llvm

#endif
