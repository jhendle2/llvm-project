//===- DependencyGraph.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Vectorize/SandboxVectorizer/DependencyGraph.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/SandboxIR/Instruction.h"
#include "llvm/SandboxIR/Utils.h"

namespace llvm::sandboxir {

PredIterator::value_type PredIterator::operator*() {
  // If it's a DGNode then we dereference the operand iterator.
  if (!isa<MemDGNode>(N)) {
    assert(OpIt != OpItE && "Can't dereference end iterator!");
    return DAG->getNode(cast<Instruction>((Value *)*OpIt));
  }
  // It's a MemDGNode, so we check if we return either the use-def operand,
  // or a mem predecessor.
  if (OpIt != OpItE)
    return DAG->getNode(cast<Instruction>((Value *)*OpIt));
  // It's a MemDGNode with OpIt == end, so we need to use MemIt.
  assert(MemIt != cast<MemDGNode>(N)->MemPreds.end() &&
         "Cant' dereference end iterator!");
  return *MemIt;
}

PredIterator &PredIterator::operator++() {
  // If it's a DGNode then we increment the use-def iterator.
  if (!isa<MemDGNode>(N)) {
    assert(OpIt != OpItE && "Already at end!");
    ++OpIt;
    // Skip operands that are not instructions.
    OpIt = skipNonInstr(OpIt, OpItE);
    return *this;
  }
  // It's a MemDGNode, so if we are not at the end of the use-def iterator we
  // need to first increment that.
  if (OpIt != OpItE) {
    ++OpIt;
    // Skip operands that are not instructions.
    OpIt = skipNonInstr(OpIt, OpItE);
    return *this;
  }
  // It's a MemDGNode with OpIt == end, so we need to increment MemIt.
  assert(MemIt != cast<MemDGNode>(N)->MemPreds.end() && "Already at end!");
  ++MemIt;
  return *this;
}

bool PredIterator::operator==(const PredIterator &Other) const {
  assert(DAG == Other.DAG && "Iterators of different DAGs!");
  assert(N == Other.N && "Iterators of different nodes!");
  return OpIt == Other.OpIt && MemIt == Other.MemIt;
}

#ifndef NDEBUG
void DGNode::print(raw_ostream &OS, bool PrintDeps) const { I->dumpOS(OS); }
void DGNode::dump() const {
  print(dbgs());
  dbgs() << "\n";
}
void MemDGNode::print(raw_ostream &OS, bool PrintDeps) const {
  I->dumpOS(OS);
  if (PrintDeps) {
    // Print memory preds.
    static constexpr const unsigned Indent = 4;
    for (auto *Pred : MemPreds) {
      OS.indent(Indent) << "<-";
      Pred->print(OS, false);
      OS << "\n";
    }
  }
}
#endif // NDEBUG

MemDGNode *
MemDGNodeIntervalBuilder::getTopMemDGNode(const Interval<Instruction> &Intvl,
                                          const DependencyGraph &DAG) {
  Instruction *I = Intvl.top();
  Instruction *BeforeI = Intvl.bottom();
  // Walk down the chain looking for a mem-dep candidate instruction.
  while (!DGNode::isMemDepNodeCandidate(I) && I != BeforeI)
    I = I->getNextNode();
  if (!DGNode::isMemDepNodeCandidate(I))
    return nullptr;
  return cast<MemDGNode>(DAG.getNode(I));
}

MemDGNode *
MemDGNodeIntervalBuilder::getBotMemDGNode(const Interval<Instruction> &Intvl,
                                          const DependencyGraph &DAG) {
  Instruction *I = Intvl.bottom();
  Instruction *AfterI = Intvl.top();
  // Walk up the chain looking for a mem-dep candidate instruction.
  while (!DGNode::isMemDepNodeCandidate(I) && I != AfterI)
    I = I->getPrevNode();
  if (!DGNode::isMemDepNodeCandidate(I))
    return nullptr;
  return cast<MemDGNode>(DAG.getNode(I));
}

Interval<MemDGNode>
MemDGNodeIntervalBuilder::make(const Interval<Instruction> &Instrs,
                               DependencyGraph &DAG) {
  auto *TopMemN = getTopMemDGNode(Instrs, DAG);
  // If we couldn't find a mem node in range TopN - BotN then it's empty.
  if (TopMemN == nullptr)
    return {};
  auto *BotMemN = getBotMemDGNode(Instrs, DAG);
  assert(BotMemN != nullptr && "TopMemN should be null too!");
  // Now that we have the mem-dep nodes, create and return the range.
  return Interval<MemDGNode>(TopMemN, BotMemN);
}

DependencyGraph::DependencyType
DependencyGraph::getRoughDepType(Instruction *FromI, Instruction *ToI) {
  // TODO: Perhaps compile-time improvement by skipping if neither is mem?
  if (FromI->mayWriteToMemory()) {
    if (ToI->mayReadFromMemory())
      return DependencyType::ReadAfterWrite;
    if (ToI->mayWriteToMemory())
      return DependencyType::WriteAfterWrite;
  } else if (FromI->mayReadFromMemory()) {
    if (ToI->mayWriteToMemory())
      return DependencyType::WriteAfterRead;
  }
  if (isa<sandboxir::PHINode>(FromI) || isa<sandboxir::PHINode>(ToI))
    return DependencyType::Control;
  if (ToI->isTerminator())
    return DependencyType::Control;
  if (DGNode::isStackSaveOrRestoreIntrinsic(FromI) ||
      DGNode::isStackSaveOrRestoreIntrinsic(ToI))
    return DependencyType::Other;
  return DependencyType::None;
}

static bool isOrdered(Instruction *I) {
  auto IsOrdered = [](Instruction *I) {
    if (auto *LI = dyn_cast<LoadInst>(I))
      return !LI->isUnordered();
    if (auto *SI = dyn_cast<StoreInst>(I))
      return !SI->isUnordered();
    if (DGNode::isFenceLike(I))
      return true;
    return false;
  };
  bool Is = IsOrdered(I);
  assert((!Is || DGNode::isMemDepCandidate(I)) &&
         "An ordered instruction must be a MemDepCandidate!");
  return Is;
}

bool DependencyGraph::alias(Instruction *SrcI, Instruction *DstI,
                            DependencyType DepType) {
  std::optional<MemoryLocation> DstLocOpt =
      Utils::memoryLocationGetOrNone(DstI);
  if (!DstLocOpt)
    return true;
  // Check aliasing.
  assert((SrcI->mayReadFromMemory() || SrcI->mayWriteToMemory()) &&
         "Expected a mem instr");
  // TODO: Check AABudget
  ModRefInfo SrcModRef =
      isOrdered(SrcI)
          ? ModRefInfo::ModRef
          : Utils::aliasAnalysisGetModRefInfo(*BatchAA, SrcI, *DstLocOpt);
  switch (DepType) {
  case DependencyType::ReadAfterWrite:
  case DependencyType::WriteAfterWrite:
    return isModSet(SrcModRef);
  case DependencyType::WriteAfterRead:
    return isRefSet(SrcModRef);
  default:
    llvm_unreachable("Expected only RAW, WAW and WAR!");
  }
}

bool DependencyGraph::hasDep(Instruction *SrcI, Instruction *DstI) {
  DependencyType RoughDepType = getRoughDepType(SrcI, DstI);
  switch (RoughDepType) {
  case DependencyType::ReadAfterWrite:
  case DependencyType::WriteAfterWrite:
  case DependencyType::WriteAfterRead:
    return alias(SrcI, DstI, RoughDepType);
  case DependencyType::Control:
    // Adding actual dep edges from PHIs/to terminator would just create too
    // many edges, which would be bad for compile-time.
    // So we ignore them in the DAG formation but handle them in the
    // scheduler, while sorting the ready list.
    return false;
  case DependencyType::Other:
    return true;
  case DependencyType::None:
    return false;
  }
  llvm_unreachable("Unknown DependencyType enum");
}

void DependencyGraph::scanAndAddDeps(MemDGNode &DstN,
                                     const Interval<MemDGNode> &SrcScanRange) {
  assert(isa<MemDGNode>(DstN) &&
         "DstN is the mem dep destination, so it must be mem");
  Instruction *DstI = DstN.getInstruction();
  // Walk up the instruction chain from ScanRange bottom to top, looking for
  // memory instrs that may alias.
  for (MemDGNode &SrcN : reverse(SrcScanRange)) {
    Instruction *SrcI = SrcN.getInstruction();
    if (hasDep(SrcI, DstI))
      DstN.addMemPred(&SrcN);
  }
}

void DependencyGraph::createNewNodes(const Interval<Instruction> &NewInterval) {
  // Create Nodes only for the new sections of the DAG.
  DGNode *LastN = getOrCreateNode(NewInterval.top());
  MemDGNode *LastMemN = dyn_cast<MemDGNode>(LastN);
  for (Instruction &I : drop_begin(NewInterval)) {
    auto *N = getOrCreateNode(&I);
    // Build the Mem node chain.
    if (auto *MemN = dyn_cast<MemDGNode>(N)) {
      MemN->setPrevNode(LastMemN);
      if (LastMemN != nullptr)
        LastMemN->setNextNode(MemN);
      LastMemN = MemN;
    }
  }
  // Link new MemDGNode chain with the old one, if any.
  if (!DAGInterval.empty()) {
    // TODO: Implement Interval::comesBefore() to replace this check.
    bool NewIsAbove = NewInterval.bottom()->comesBefore(DAGInterval.top());
    assert(
        (NewIsAbove || DAGInterval.bottom()->comesBefore(NewInterval.top())) &&
        "Expected NewInterval below DAGInterval.");
    const auto &TopInterval = NewIsAbove ? NewInterval : DAGInterval;
    const auto &BotInterval = NewIsAbove ? DAGInterval : NewInterval;
    MemDGNode *LinkTopN =
        MemDGNodeIntervalBuilder::getBotMemDGNode(TopInterval, *this);
    MemDGNode *LinkBotN =
        MemDGNodeIntervalBuilder::getTopMemDGNode(BotInterval, *this);
    assert(LinkTopN->comesBefore(LinkBotN) && "Wrong order!");
    if (LinkTopN != nullptr && LinkBotN != nullptr) {
      LinkTopN->setNextNode(LinkBotN);
      LinkBotN->setPrevNode(LinkTopN);
    }
#ifndef NDEBUG
    // TODO: Remove this once we've done enough testing.
    // Check that the chain is well formed.
    auto UnionIntvl = DAGInterval.getUnionInterval(NewInterval);
    MemDGNode *ChainTopN =
        MemDGNodeIntervalBuilder::getTopMemDGNode(UnionIntvl, *this);
    MemDGNode *ChainBotN =
        MemDGNodeIntervalBuilder::getBotMemDGNode(UnionIntvl, *this);
    if (ChainTopN != nullptr && ChainBotN != nullptr) {
      for (auto *N = ChainTopN->getNextNode(), *LastN = ChainTopN; N != nullptr;
           LastN = N, N = N->getNextNode()) {
        assert(N == LastN->getNextNode() && "Bad chain!");
        assert(N->getPrevNode() == LastN && "Bad chain!");
      }
    }
#endif // NDEBUG
  }
}

Interval<Instruction> DependencyGraph::extend(ArrayRef<Instruction *> Instrs) {
  if (Instrs.empty())
    return {};

  Interval<Instruction> InstrsInterval(Instrs);
  Interval<Instruction> Union = DAGInterval.getUnionInterval(InstrsInterval);
  auto NewInterval = Union.getSingleDiff(DAGInterval);
  if (NewInterval.empty())
    return {};

  createNewNodes(NewInterval);

  // Create the dependencies.
  //
  // 1. DAGInterval empty      2. New is below Old     3. New is above old
  // ------------------------  -------------------      -------------------
  //                                         Scan:           DstN:    Scan:
  //                           +---+         -ScanTopN  +---+DstTopN  -ScanTopN
  //                           |   |         |          |New|         |
  //                           |Old|         |          +---+         -ScanBotN
  //                           |   |         |          +---+
  //      DstN:    Scan:       +---+DstN:    |          |   |
  // +---+DstTopN  -ScanTopN   +---+DstTopN  |          |Old|
  // |New|         |           |New|         |          |   |
  // +---+DstBotN  -ScanBotN   +---+DstBotN  -ScanBotN  +---+DstBotN

  // 1. This is a new DAG.
  if (DAGInterval.empty()) {
    assert(NewInterval == InstrsInterval && "Expected empty DAGInterval!");
    auto DstRange = MemDGNodeIntervalBuilder::make(NewInterval, *this);
    if (!DstRange.empty()) {
      for (MemDGNode &DstN : drop_begin(DstRange)) {
        auto SrcRange = Interval<MemDGNode>(DstRange.top(), DstN.getPrevNode());
        scanAndAddDeps(DstN, SrcRange);
      }
    }
  }
  // 2. The new section is below the old section.
  else if (DAGInterval.bottom()->comesBefore(NewInterval.top())) {
    auto DstRange = MemDGNodeIntervalBuilder::make(NewInterval, *this);
    auto SrcRangeFull = MemDGNodeIntervalBuilder::make(
        DAGInterval.getUnionInterval(NewInterval), *this);
    for (MemDGNode &DstN : DstRange) {
      auto SrcRange =
          Interval<MemDGNode>(SrcRangeFull.top(), DstN.getPrevNode());
      scanAndAddDeps(DstN, SrcRange);
    }
  }
  // 3. The new section is above the old section.
  else if (NewInterval.bottom()->comesBefore(DAGInterval.top())) {
    auto DstRange = MemDGNodeIntervalBuilder::make(
        NewInterval.getUnionInterval(DAGInterval), *this);
    auto SrcRangeFull = MemDGNodeIntervalBuilder::make(NewInterval, *this);
    if (!DstRange.empty()) {
      for (MemDGNode &DstN : drop_begin(DstRange)) {
        auto SrcRange =
            Interval<MemDGNode>(SrcRangeFull.top(), DstN.getPrevNode());
        scanAndAddDeps(DstN, SrcRange);
      }
    }
  } else {
    llvm_unreachable("We don't expect extending in both directions!");
  }

  DAGInterval = Union;
  return NewInterval;
}

#ifndef NDEBUG
void DependencyGraph::print(raw_ostream &OS) const {
  // InstrToNodeMap is unordered so we need to create an ordered vector.
  SmallVector<DGNode *> Nodes;
  Nodes.reserve(InstrToNodeMap.size());
  for (const auto &Pair : InstrToNodeMap)
    Nodes.push_back(Pair.second.get());
  // Sort them based on which one comes first in the BB.
  sort(Nodes, [](DGNode *N1, DGNode *N2) {
    return N1->getInstruction()->comesBefore(N2->getInstruction());
  });
  for (auto *N : Nodes)
    N->print(OS, /*PrintDeps=*/true);
}

void DependencyGraph::dump() const {
  print(dbgs());
  dbgs() << "\n";
}
#endif // NDEBUG

} // namespace llvm::sandboxir
