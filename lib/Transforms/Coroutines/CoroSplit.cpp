//===- CoroSplit.cpp - Converts a coroutine into a state machine ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This pass builds the coroutine frame and outlines resume and destroy parts
// of the coroutine into separate functions.
//
// We present a coroutine to an LLVM as an ordinary function with suspension
// points marked up with intrinsics. We let the optimizer party on the coroutine
// as a single function for as long as possible. Shortly before the coroutine is
// eligible to be inlined into its callers, we split up the coroutine into parts
// corresponding to an initial, resume and destroy invocations of the coroutine,
// add them to the current SCC and restart the IPO pipeline to optimize the
// coroutine subfunctions we extracted before proceeding to the caller of the
// coroutine.
//===----------------------------------------------------------------------===//

#include "CoroInstr.h"
#include "CoroInternal.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>

using namespace llvm;

#define DEBUG_TYPE "coro-split"

namespace {

/// A little helper class for building 
class CoroCloner {
public:
  enum class Kind {
    /// The shared resume function for a switch lowering.
    SwitchResume,

    /// The shared unwind function for a switch lowering.
    SwitchUnwind,

    /// The shared cleanup function for a switch lowering.
    SwitchCleanup,

    /// An individual continuation function.
    Continuation,
  };
private:
  Function &OrigF;
  Function *NewF;
  const Twine &Suffix;
  coro::Shape &Shape;
  Kind FKind;
  ValueToValueMapTy VMap;
  IRBuilder<> Builder;
  Value *NewFramePtr = nullptr;

  /// The active suspend instruction; meaningful only for continuation ABIs.
  AnyCoroSuspendInst *ActiveSuspend = nullptr;

public:
  /// Create a cloner for a switch lowering.
  CoroCloner(Function &OrigF, const Twine &Suffix, coro::Shape &Shape,
             Kind FKind)
    : OrigF(OrigF), NewF(nullptr), Suffix(Suffix), Shape(Shape),
      FKind(FKind), Builder(OrigF.getContext()) {
    assert(Shape.ABI == coro::ABI::Switch);
  }

  /// Create a cloner for a continuation lowering.
  CoroCloner(Function &OrigF, const Twine &Suffix, coro::Shape &Shape,
             Function *NewF, AnyCoroSuspendInst *ActiveSuspend)
    : OrigF(OrigF), NewF(NewF), Suffix(Suffix), Shape(Shape),
      FKind(Kind::Continuation), Builder(OrigF.getContext()),
      ActiveSuspend(ActiveSuspend) {
    assert(Shape.ABI == coro::ABI::Retcon ||
           Shape.ABI == coro::ABI::RetconOnce);
    assert(NewF && "need existing function for continuation");
    assert(ActiveSuspend && "need active suspend point for continuation");
  }

  Function *getFunction() const {
    assert(NewF != nullptr && "declaration not yet set");
    return NewF;
  }

  void create();

private:
  bool isSwitchDestroyFunction() {
    switch (FKind) {
    case Kind::Continuation:
    case Kind::SwitchResume:
      return false;
    case Kind::SwitchUnwind:
    case Kind::SwitchCleanup:
      return true;
    }
  }

  void createDeclaration();
  void replaceEntryBlock();
  Value *deriveNewFramePointer();
  void replaceRetconSuspendUses();
  void replaceCoroSuspends();
  void replaceCoroEnds();
  void handleFinalSuspend();
  void maybeFreeContinuationStorage();
};

} // end anonymous namespace

static void maybeFreeRetconStorage(IRBuilder<> &Builder, coro::Shape &Shape,
                                   Value *FramePtr, CallGraph *CG) {
  assert(Shape.ABI == coro::ABI::Retcon ||
         Shape.ABI == coro::ABI::RetconOnce);
  if (Shape.RetconLowering.IsFrameInlineInStorage)
    return;

  Shape.emitDealloc(Builder, FramePtr, CG);
}

/// Replace a non-unwind call to llvm.coro.end.
static void replaceFallthroughCoroEnd(CoroEndInst *End, coro::Shape &Shape,
                                      Value *FramePtr, bool InResume,
                                      CallGraph *CG) {
  // Start inserting right before the coro.end.
  IRBuilder<> Builder(End);

  // Create the return instruction.
  switch (Shape.ABI) {
  // The cloned functions in switch-lowering always return void.
  case coro::ABI::Switch:
    // coro.end doesn't immediately end the coroutine in the main function
    // in this lowering, because we need to deallocate the coroutine.
    if (!InResume)
      return;
    Builder.CreateRetVoid();
    break;

  // In unique continuation lowering, the continuations always return void.
  // But we may have implicitly allocated storage.
  case coro::ABI::RetconOnce:
    maybeFreeRetconStorage(Builder, Shape, FramePtr, CG);
    Builder.CreateRetVoid();
    break;

  // In non-unique continuation lowering, we signal completion by returning
  // a null continuation.
  case coro::ABI::Retcon: {
    maybeFreeRetconStorage(Builder, Shape, FramePtr, CG);
    auto RetTy = Shape.getResumeFunctionType()->getReturnType();
    auto RetStructTy = dyn_cast<StructType>(RetTy);
    PointerType *ContinuationTy =
      cast<PointerType>(RetStructTy ? RetStructTy->getElementType(0) : RetTy);

    Value *ReturnValue = ConstantPointerNull::get(ContinuationTy);
    if (RetStructTy) {
      ReturnValue = Builder.CreateInsertValue(UndefValue::get(RetStructTy),
                                              ReturnValue, 0);
    }
    Builder.CreateRet(ReturnValue);
    break;
  }
  }

  // Remove the rest of the block, by splitting it into an unreachable block.
  auto *BB = End->getParent();
  BB->splitBasicBlock(End);
  BB->getTerminator()->eraseFromParent();
}

/// Replace an unwind call to llvm.coro.end.
static void replaceUnwindCoroEnd(CoroEndInst *End, coro::Shape &Shape,
                                 Value *FramePtr, bool InResume, CallGraph *CG){
  IRBuilder<> Builder(End);

  switch (Shape.ABI) {
  // In switch-lowering, this does nothing in the main function.
  case coro::ABI::Switch:
    if (!InResume)
      return;
    break;

  // In continuation-lowering, this frees the continuation storage.
  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce:
    maybeFreeRetconStorage(Builder, Shape, FramePtr, CG);
    break;
  }

  // If coro.end has an associated bundle, add cleanupret instruction.
  if (auto Bundle = End->getOperandBundle(LLVMContext::OB_funclet)) {
    auto *FromPad = cast<CleanupPadInst>(Bundle->Inputs[0]);
    auto *CleanupRet = Builder.CreateCleanupRet(FromPad, nullptr);
    End->getParent()->splitBasicBlock(End);
    CleanupRet->getParent()->getTerminator()->eraseFromParent();
  }
}

static void replaceCoroEnd(CoroEndInst *End, coro::Shape &Shape,
                           Value *FramePtr, bool InResume, CallGraph *CG) {
  if (End->isUnwind())
    replaceUnwindCoroEnd(End, Shape, FramePtr, InResume, CG);
  else
    replaceFallthroughCoroEnd(End, Shape, FramePtr, InResume, CG);

  auto &Context = End->getContext();
  End->replaceAllUsesWith(InResume ? ConstantInt::getTrue(Context)
                                   : ConstantInt::getFalse(Context));
  End->eraseFromParent();
}

// Create an entry block for a resume function with a switch that will jump to
// suspend points.
static void createResumeEntryBlock(Function &F, coro::Shape &Shape) {
  assert(Shape.ABI == coro::ABI::Switch);
  LLVMContext &C = F.getContext();

  // resume.entry:
  //  %index.addr = getelementptr inbounds %f.Frame, %f.Frame* %FramePtr, i32 0,
  //  i32 2
  //  % index = load i32, i32* %index.addr
  //  switch i32 %index, label %unreachable [
  //    i32 0, label %resume.0
  //    i32 1, label %resume.1
  //    ...
  //  ]

  auto *NewEntry = BasicBlock::Create(C, "resume.entry", &F);
  auto *UnreachBB = BasicBlock::Create(C, "unreachable", &F);

  IRBuilder<> Builder(NewEntry);
  auto *FramePtr = Shape.FramePtr;
  auto *FrameTy = Shape.FrameTy;
  auto *GepIndex = Builder.CreateStructGEP(
      FrameTy, FramePtr, coro::Shape::SwitchFieldIndex::Index, "index.addr");
  auto *Index = Builder.CreateLoad(GepIndex, "index");
  auto *Switch =
      Builder.CreateSwitch(Index, UnreachBB, Shape.CoroSuspends.size());
  Shape.SwitchLowering.ResumeSwitch = Switch;

  size_t SuspendIndex = 0;
  for (auto *AnyS : Shape.CoroSuspends) {
    auto *S = cast<CoroSuspendInst>(AnyS);
    ConstantInt *IndexVal = Shape.getIndex(SuspendIndex);

    // Replace CoroSave with a store to Index:
    //    %index.addr = getelementptr %f.frame... (index field number)
    //    store i32 0, i32* %index.addr1
    auto *Save = S->getCoroSave();
    Builder.SetInsertPoint(Save);
    if (S->isFinal()) {
      // Final suspend point is represented by storing zero in ResumeFnAddr.
      auto *GepIndex = Builder.CreateStructGEP(FrameTy, FramePtr,
                                 coro::Shape::SwitchFieldIndex::Resume,
                                  "ResumeFn.addr");
      auto *NullPtr = ConstantPointerNull::get(cast<PointerType>(
          cast<PointerType>(GepIndex->getType())->getElementType()));
      Builder.CreateStore(NullPtr, GepIndex);
    } else {
      auto *GepIndex = Builder.CreateStructGEP(
          FrameTy, FramePtr, coro::Shape::SwitchFieldIndex::Index, "index.addr");
      Builder.CreateStore(IndexVal, GepIndex);
    }
    Save->replaceAllUsesWith(ConstantTokenNone::get(C));
    Save->eraseFromParent();

    // Split block before and after coro.suspend and add a jump from an entry
    // switch:
    //
    //  whateverBB:
    //    whatever
    //    %0 = call i8 @llvm.coro.suspend(token none, i1 false)
    //    switch i8 %0, label %suspend[i8 0, label %resume
    //                                 i8 1, label %cleanup]
    // becomes:
    //
    //  whateverBB:
    //     whatever
    //     br label %resume.0.landing
    //
    //  resume.0: ; <--- jump from the switch in the resume.entry
    //     %0 = tail call i8 @llvm.coro.suspend(token none, i1 false)
    //     br label %resume.0.landing
    //
    //  resume.0.landing:
    //     %1 = phi i8[-1, %whateverBB], [%0, %resume.0]
    //     switch i8 % 1, label %suspend [i8 0, label %resume
    //                                    i8 1, label %cleanup]

    auto *SuspendBB = S->getParent();
    auto *ResumeBB =
        SuspendBB->splitBasicBlock(S, "resume." + Twine(SuspendIndex));
    auto *LandingBB = ResumeBB->splitBasicBlock(
        S->getNextNode(), ResumeBB->getName() + Twine(".landing"));
    Switch->addCase(IndexVal, ResumeBB);

    cast<BranchInst>(SuspendBB->getTerminator())->setSuccessor(0, LandingBB);
    auto *PN = PHINode::Create(Builder.getInt8Ty(), 2, "", &LandingBB->front());
    S->replaceAllUsesWith(PN);
    PN->addIncoming(Builder.getInt8(-1), SuspendBB);
    PN->addIncoming(S, ResumeBB);

    ++SuspendIndex;
  }

  Builder.SetInsertPoint(UnreachBB);
  Builder.CreateUnreachable();

  Shape.SwitchLowering.ResumeEntryBlock = NewEntry;
}


// Rewrite final suspend point handling. We do not use suspend index to
// represent the final suspend point. Instead we zero-out ResumeFnAddr in the
// coroutine frame, since it is undefined behavior to resume a coroutine
// suspended at the final suspend point. Thus, in the resume function, we can
// simply remove the last case (when coro::Shape is built, the final suspend
// point (if present) is always the last element of CoroSuspends array).
// In the destroy function, we add a code sequence to check if ResumeFnAddress
// is Null, and if so, jump to the appropriate label to handle cleanup from the
// final suspend point.
void CoroCloner::handleFinalSuspend() {
  assert(Shape.ABI == coro::ABI::Switch &&
         Shape.SwitchLowering.HasFinalSuspend);
  auto *Switch = cast<SwitchInst>(VMap[Shape.SwitchLowering.ResumeSwitch]);
  auto FinalCaseIt = std::prev(Switch->case_end());
  BasicBlock *ResumeBB = FinalCaseIt->getCaseSuccessor();
  Switch->removeCase(FinalCaseIt);
  if (isSwitchDestroyFunction()) {
    BasicBlock *OldSwitchBB = Switch->getParent();
    auto *NewSwitchBB = OldSwitchBB->splitBasicBlock(Switch, "Switch");
    Builder.SetInsertPoint(OldSwitchBB->getTerminator());
    auto *GepIndex = Builder.CreateStructGEP(Shape.FrameTy, NewFramePtr,
                                       coro::Shape::SwitchFieldIndex::Resume,
                                             "ResumeFn.addr");
    auto *Load = Builder.CreateLoad(GepIndex);
    auto *Cond = Builder.CreateIsNull(Load);
    Builder.CreateCondBr(Cond, ResumeBB, NewSwitchBB);
    OldSwitchBB->getTerminator()->eraseFromParent();
  }
}

static Function *createCloneDeclaration(Function &OrigF, coro::Shape &Shape,
                                        const Twine &Suffix,
                                        Module::iterator InsertBefore) {
  Module *M = OrigF.getParent();
  auto *FnTy = Shape.getResumeFunctionType();

  auto *F = Function::Create(FnTy, GlobalValue::LinkageTypes::InternalLinkage,
                             OrigF.getName() + Suffix);

  M->getFunctionList().insert(InsertBefore, F);

  return F;
}

/// Replace uses of the active llvm.coro.suspend.retcon call with the
/// arguments to the continuation function.
///
/// This assumes that the builder has a meaningful insertion point.
void CoroCloner::replaceRetconSuspendUses() {
  assert(Shape.ABI == coro::ABI::Retcon ||
         Shape.ABI == coro::ABI::RetconOnce);

  auto NewS = VMap[ActiveSuspend];
  if (NewS->use_empty()) return;

  // Copy out all the continuation arguments after the buffer pointer into
  // an easily-indexed data structure for convenience.
  SmallVector<Value*, 8> Args;
  for (auto I = std::next(NewF->arg_begin()), E = NewF->arg_end(); I != E; ++I)
    Args.push_back(&*I);

  // If the suspend returns a single scalar value, we can just do a simple
  // replacement.
  if (!isa<StructType>(NewS->getType())) {
    assert(Args.size() == 1);
    NewS->replaceAllUsesWith(Args.front());
    return;
  }

  // Try to peephole extracts of an aggregate return.
  for (auto UI = NewS->use_begin(), UE = NewS->use_end(); UI != UE; ) {
    auto EVI = dyn_cast<ExtractValueInst>((UI++)->getUser());
    if (!EVI || EVI->getNumIndices() != 1)
      continue;

    EVI->replaceAllUsesWith(Args[EVI->getIndices().front()]);
    EVI->eraseFromParent();
  }

  // If we have no remaining uses, we're done.
  if (NewS->use_empty()) return;

  // Otherwise, we need to create an aggregate.
  Value *Agg = UndefValue::get(NewS->getType());
  for (size_t I = 0, E = Args.size(); I != E; ++I)
    Agg = Builder.CreateInsertValue(Agg, Args[I], I);

  NewS->replaceAllUsesWith(Agg);
}

void CoroCloner::replaceCoroSuspends() {
  Value *SuspendResult;

  switch (Shape.ABI) {
  // In switch lowering, replace coro.suspend with the appropriate value
  // for the type of function we're extracting.
  // Replacing coro.suspend with (0) will result in control flow proceeding to
  // a resume label associated with a suspend point, replacing it with (1) will
  // result in control flow proceeding to a cleanup label associated with this
  // suspend point.
  case coro::ABI::Switch:
    SuspendResult = Builder.getInt8(isSwitchDestroyFunction() ? 1 : 0);
    break;

  // In returned-continuation lowering, the arguments from earlier
  // continuations are theoretically arbitrary, and they should have been
  // spilled.
  case coro::ABI::RetconOnce:
  case coro::ABI::Retcon:
    return;
  }

  for (AnyCoroSuspendInst *CS : Shape.CoroSuspends) {
    // The active suspend was handled earlier.
    if (CS == ActiveSuspend) continue;

    auto *MappedCS = cast<AnyCoroSuspendInst>(VMap[CS]);
    MappedCS->replaceAllUsesWith(SuspendResult);
    MappedCS->eraseFromParent();
  }
}

void CoroCloner::replaceCoroEnds() {
  for (CoroEndInst *CE : Shape.CoroEnds) {
    // We use a null call graph because there's no call graph node for
    // the cloned function yet.  We'll just be rebuilding that later.
    auto NewCE = cast<CoroEndInst>(VMap[CE]);
    replaceCoroEnd(NewCE, Shape, NewFramePtr, /*in resume*/ true, nullptr);
  }
}

void CoroCloner::replaceEntryBlock() {
  // In the original function, the AllocaSpillBlock is a block immediately
  // following the allocation of the frame object which defines GEPs for
  // all the allocas that have been moved into the frame, and it ends by
  // branching to the original beginning of the coroutine.  Make this 
  // the entry block of the cloned function.
  auto *Entry = cast<BasicBlock>(VMap[Shape.AllocaSpillBlock]);
  Entry->setName("entry" + Suffix);
  Entry->moveBefore(&NewF->getEntryBlock());
  Entry->getTerminator()->eraseFromParent();

  // Clear all predecessors of the new entry block.  There should be
  // exactly one predecessor, which we created when splitting out
  // AllocaSpillBlock to begin with.
  assert(Entry->hasOneUse());
  auto BranchToEntry = cast<BranchInst>(Entry->user_back());
  assert(BranchToEntry->isUnconditional());
  Builder.SetInsertPoint(BranchToEntry);
  Builder.CreateUnreachable();
  BranchToEntry->eraseFromParent();

  // TODO: move any allocas into Entry that weren't moved into the frame.
  // (Currently we move all allocas into the frame.)

  // Branch from the entry to the appropriate place.
  Builder.SetInsertPoint(Entry);
  switch (Shape.ABI) {
  case coro::ABI::Switch: {
    // In switch-lowering, we built a resume-entry block in the original
    // function.  Make the entry block branch to this.
    auto *SwitchBB =
      cast<BasicBlock>(VMap[Shape.SwitchLowering.ResumeEntryBlock]);
    Builder.CreateBr(SwitchBB);
    break;
  }

  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce: {
    // In continuation ABIs, we want to branch to immediately after the
    // active suspend point.  Earlier phases will have put the suspend in its
    // own basic block, so just thread our jump directly to its successor.
    auto MappedCS = cast<CoroSuspendRetconInst>(VMap[ActiveSuspend]);
    auto Branch = cast<BranchInst>(MappedCS->getNextNode());
    assert(Branch->isUnconditional());
    Builder.CreateBr(Branch->getSuccessor(0));
    break;
  }
  }
}

/// Derive the value of the new frame pointer.
Value *CoroCloner::deriveNewFramePointer() {
  // Builder should be inserting to the front of the new entry block.

  switch (Shape.ABI) {
  // In switch-lowering, the argument is the frame pointer.
  case coro::ABI::Switch:
    return &*NewF->arg_begin();

  // In continuation-lowering, the argument is the opaque storage.
  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce: {
    Argument *NewStorage = &*NewF->arg_begin();
    auto FramePtrTy = Shape.FrameTy->getPointerTo();

    // If the storage is inline, just bitcast to the storage to the frame type.
    if (Shape.RetconLowering.IsFrameInlineInStorage)
      return Builder.CreateBitCast(NewStorage, FramePtrTy);

    // Otherwise, load the real frame from the opaque storage.
    auto FramePtrPtr =
      Builder.CreateBitCast(NewStorage, FramePtrTy->getPointerTo());
    return Builder.CreateLoad(FramePtrPtr);
  }
  }
  llvm_unreachable("bad ABI");
}

/// Clone the body of the original function into a resume function of
/// some sort.
void CoroCloner::create() {
  // Create the new function if we don't already have one.
  if (!NewF) {
    NewF = createCloneDeclaration(OrigF, Shape, Suffix,
                                  OrigF.getParent()->end());
  }

  // Replace all args with undefs. The buildCoroutineFrame algorithm already
  // rewritten access to the args that occurs after suspend points with loads
  // and stores to/from the coroutine frame.
  for (Argument &A : OrigF.args())
    VMap[&A] = UndefValue::get(A.getType());

  SmallVector<ReturnInst *, 4> Returns;

  // Ignore attempts to change certain attributes of the function.
  // TODO: maybe there should be a way to suppress this during cloning?
  auto savedVisibility = NewF->getVisibility();
  auto savedUnnamedAddr = NewF->getUnnamedAddr();
  auto savedDLLStorageClass = NewF->getDLLStorageClass();

  // NewF's linkage (which CloneFunctionInto does *not* change) might not
  // be compatible with the visibility of OrigF (which it *does* change),
  // so protect against that.
  auto savedLinkage = NewF->getLinkage();
  NewF->setLinkage(llvm::GlobalValue::ExternalLinkage);

  CloneFunctionInto(NewF, &OrigF, VMap, /*ModuleLevelChanges=*/true, Returns);

  NewF->setLinkage(savedLinkage);
  NewF->setVisibility(savedVisibility);
  NewF->setUnnamedAddr(savedUnnamedAddr);
  NewF->setDLLStorageClass(savedDLLStorageClass);

  auto &Context = NewF->getContext();

  // Replace the attributes of the new function:
  auto OrigAttrs = NewF->getAttributes();
  auto NewAttrs = AttributeList();

  switch (Shape.ABI) {
  case coro::ABI::Switch:
    // Bootstrap attributes by copying function attributes from the
    // original function.  This should include optimization settings and so on.
    NewAttrs = NewAttrs.addAttributes(Context, AttributeList::FunctionIndex,
                                      OrigAttrs.getFnAttributes());
    break;

  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce:
    // If we have a continuation prototype, just use its attributes,
    // full-stop.
    NewAttrs = Shape.RetconLowering.ResumePrototype->getAttributes();
    break;
  }

  // Make the frame parameter nonnull and noalias.
  NewAttrs = NewAttrs.addParamAttribute(Context, 0, Attribute::NonNull);
  NewAttrs = NewAttrs.addParamAttribute(Context, 0, Attribute::NoAlias);

  switch (Shape.ABI) {
  // In these ABIs, the cloned functions always return 'void', and the
  // existing return sites are meaningless.  Note that for unique
  // continuations, this includes the returns associated with suspends;
  // this is fine because we can't suspend twice.
  case coro::ABI::Switch:
  case coro::ABI::RetconOnce:
    // Remove old returns.
    for (ReturnInst *Return : Returns)
      changeToUnreachable(Return, /*UseLLVMTrap=*/false);
    break;

  // With multi-suspend continuations, we'll already have eliminated the
  // original returns and inserted returns before all the suspend points,
  // so we want to leave any returns in place.
  case coro::ABI::Retcon:
    break;
  }

  NewF->setAttributes(NewAttrs);
  NewF->setCallingConv(Shape.getResumeFunctionCC());

  // Set up the new entry block.
  replaceEntryBlock();

  Builder.SetInsertPoint(&NewF->getEntryBlock().front());
  NewFramePtr = deriveNewFramePointer();

  // Remap frame pointer.
  Value *OldFramePtr = VMap[Shape.FramePtr];
  NewFramePtr->takeName(OldFramePtr);
  OldFramePtr->replaceAllUsesWith(NewFramePtr);

  // Remap vFrame pointer.
  auto *NewVFrame = Builder.CreateBitCast(
      NewFramePtr, Type::getInt8PtrTy(Builder.getContext()), "vFrame");
  Value *OldVFrame = cast<Value>(VMap[Shape.CoroBegin]);
  OldVFrame->replaceAllUsesWith(NewVFrame);

  switch (Shape.ABI) {
  case coro::ABI::Switch:
    // Rewrite final suspend handling as it is not done via switch (allows to
    // remove final case from the switch, since it is undefined behavior to
    // resume the coroutine suspended at the final suspend point.
    if (Shape.SwitchLowering.HasFinalSuspend)
      handleFinalSuspend();
    break;

  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce:
    // Replace uses of the active suspend with the corresponding
    // continuation-function arguments.
    assert(ActiveSuspend != nullptr &&
           "no active suspend when lowering a continuation-style coroutine");
    replaceRetconSuspendUses();
    break;
  }

  // Handle suspends.
  replaceCoroSuspends();

  // Remove coro.end intrinsics.
  replaceCoroEnds();

  // Eliminate coro.free from the clones, replacing it with 'null' in cleanup,
  // to suppress deallocation code.
  if (Shape.ABI == coro::ABI::Switch)
    coro::replaceCoroFree(cast<CoroIdInst>(VMap[Shape.CoroBegin->getId()]),
                          /*Elide=*/ FKind == CoroCloner::Kind::SwitchCleanup);
}

// Create a resume clone by cloning the body of the original function, setting
// new entry block and replacing coro.suspend an appropriate value to force
// resume or cleanup pass for every suspend point.
static Function *createClone(Function &F, const Twine &Suffix,
                             coro::Shape &Shape, CoroCloner::Kind FKind) {
  CoroCloner Cloner(F, Suffix, Shape, FKind);
  Cloner.create();
  return Cloner.getFunction();
}

/// Remove calls to llvm.coro.end in the original function.
static void removeCoroEnds(coro::Shape &Shape, CallGraph *CG) {
  for (auto End : Shape.CoroEnds) {
    replaceCoroEnd(End, Shape, Shape.FramePtr, /*in resume*/ false, CG);
  }
}

static void replaceFrameSize(coro::Shape &Shape) {
  if (Shape.CoroSizes.empty())
    return;

  // In the same function all coro.sizes should have the same result type.
  auto *SizeIntrin = Shape.CoroSizes.back();
  Module *M = SizeIntrin->getModule();
  const DataLayout &DL = M->getDataLayout();
  auto Size = DL.getTypeAllocSize(Shape.FrameTy);
  auto *SizeConstant = ConstantInt::get(SizeIntrin->getType(), Size);

  for (CoroSizeInst *CS : Shape.CoroSizes) {
    CS->replaceAllUsesWith(SizeConstant);
    CS->eraseFromParent();
  }
}

// Create a global constant array containing pointers to functions provided and
// set Info parameter of CoroBegin to point at this constant. Example:
//
//   @f.resumers = internal constant [2 x void(%f.frame*)*]
//                    [void(%f.frame*)* @f.resume, void(%f.frame*)* @f.destroy]
//   define void @f() {
//     ...
//     call i8* @llvm.coro.begin(i8* null, i32 0, i8* null,
//                    i8* bitcast([2 x void(%f.frame*)*] * @f.resumers to i8*))
//
// Assumes that all the functions have the same signature.
static void setCoroInfo(Function &F, coro::Shape &Shape,
                        ArrayRef<Function *> Fns) {
  // This only works under the switch-lowering ABI because coro elision
  // only works on the switch-lowering ABI.
  assert(Shape.ABI == coro::ABI::Switch);

  SmallVector<Constant *, 4> Args(Fns.begin(), Fns.end());
  assert(!Args.empty());
  Function *Part = *Fns.begin();
  Module *M = Part->getParent();
  auto *ArrTy = ArrayType::get(Part->getType(), Args.size());

  auto *ConstVal = ConstantArray::get(ArrTy, Args);
  auto *GV = new GlobalVariable(*M, ConstVal->getType(), /*isConstant=*/true,
                                GlobalVariable::PrivateLinkage, ConstVal,
                                F.getName() + Twine(".resumers"));

  // Update coro.begin instruction to refer to this constant.
  LLVMContext &C = F.getContext();
  auto *BC = ConstantExpr::getPointerCast(GV, Type::getInt8PtrTy(C));
  Shape.getSwitchCoroId()->setInfo(BC);
}

// Store addresses of Resume/Destroy/Cleanup functions in the coroutine frame.
static void updateCoroFrame(coro::Shape &Shape, Function *ResumeFn,
                            Function *DestroyFn, Function *CleanupFn) {
  assert(Shape.ABI == coro::ABI::Switch);

  IRBuilder<> Builder(Shape.FramePtr->getNextNode());
  auto *ResumeAddr = Builder.CreateStructGEP(
      Shape.FrameTy, Shape.FramePtr, coro::Shape::SwitchFieldIndex::Resume,
      "resume.addr");
  Builder.CreateStore(ResumeFn, ResumeAddr);

  Value *DestroyOrCleanupFn = DestroyFn;

  CoroIdInst *CoroId = Shape.getSwitchCoroId();
  if (CoroAllocInst *CA = CoroId->getCoroAlloc()) {
    // If there is a CoroAlloc and it returns false (meaning we elide the
    // allocation, use CleanupFn instead of DestroyFn).
    DestroyOrCleanupFn = Builder.CreateSelect(CA, DestroyFn, CleanupFn);
  }

  auto *DestroyAddr = Builder.CreateStructGEP(
      Shape.FrameTy, Shape.FramePtr, coro::Shape::SwitchFieldIndex::Destroy,
      "destroy.addr");
  Builder.CreateStore(DestroyOrCleanupFn, DestroyAddr);
}

static void postSplitCleanup(Function &F) {
  removeUnreachableBlocks(F);
  legacy::FunctionPassManager FPM(F.getParent());

  FPM.add(createVerifierPass());
  FPM.add(createSCCPPass());
  FPM.add(createCFGSimplificationPass());
  FPM.add(createEarlyCSEPass());
  FPM.add(createCFGSimplificationPass());

  FPM.doInitialization();
  FPM.run(F);
  FPM.doFinalization();
}

// Assuming we arrived at the block NewBlock from Prev instruction, store
// PHI's incoming values in the ResolvedValues map.
static void
scanPHIsAndUpdateValueMap(Instruction *Prev, BasicBlock *NewBlock,
                          DenseMap<Value *, Value *> &ResolvedValues) {
  auto *PrevBB = Prev->getParent();
  for (PHINode &PN : NewBlock->phis()) {
    auto V = PN.getIncomingValueForBlock(PrevBB);
    // See if we already resolved it.
    auto VI = ResolvedValues.find(V);
    if (VI != ResolvedValues.end())
      V = VI->second;
    // Remember the value.
    ResolvedValues[&PN] = V;
  }
}

// Replace a sequence of branches leading to a ret, with a clone of a ret
// instruction. Suspend instruction represented by a switch, track the PHI
// values and select the correct case successor when possible.
static bool simplifyTerminatorLeadingToRet(Instruction *InitialInst) {
  DenseMap<Value *, Value *> ResolvedValues;

  Instruction *I = InitialInst;
  while (isa<TerminatorInst>(I)) {
    if (isa<ReturnInst>(I)) {
      if (I != InitialInst)
        ReplaceInstWithInst(InitialInst, I->clone());
      return true;
    }
    if (auto *BR = dyn_cast<BranchInst>(I)) {
      if (BR->isUnconditional()) {
        BasicBlock *BB = BR->getSuccessor(0);
        scanPHIsAndUpdateValueMap(I, BB, ResolvedValues);
        I = BB->getFirstNonPHIOrDbgOrLifetime();
        continue;
      }
    } else if (auto *SI = dyn_cast<SwitchInst>(I)) {
      Value *V = SI->getCondition();
      auto it = ResolvedValues.find(V);
      if (it != ResolvedValues.end())
        V = it->second;
      if (ConstantInt *Cond = dyn_cast<ConstantInt>(V)) {
        BasicBlock *BB = SI->findCaseValue(Cond)->getCaseSuccessor();
        scanPHIsAndUpdateValueMap(I, BB, ResolvedValues);
        I = BB->getFirstNonPHIOrDbgOrLifetime();
        continue;
      }
    }
    return false;
  }
  return false;
}

// Add musttail to any resume instructions that is immediately followed by a
// suspend (i.e. ret). We do this even in -O0 to support guaranteed tail call
// for symmetrical coroutine control transfer (C++ Coroutines TS extension).
// This transformation is done only in the resume part of the coroutine that has
// identical signature and calling convention as the coro.resume call.
static void addMustTailToCoroResumes(Function &F) {
  bool changed = false;

  // Collect potential resume instructions.
  SmallVector<CallInst *, 4> Resumes;
  for (auto &I : instructions(F))
    if (auto *Call = dyn_cast<CallInst>(&I))
      if (auto *CalledValue = Call->getCalledValue())
        // CoroEarly pass replaced coro resumes with indirect calls to an
        // address return by CoroSubFnInst intrinsic. See if it is one of those.
        if (isa<CoroSubFnInst>(CalledValue->stripPointerCasts()))
          Resumes.push_back(Call);

  // Set musttail on those that are followed by a ret instruction.
  for (CallInst *Call : Resumes)
    if (simplifyTerminatorLeadingToRet(Call->getNextNode())) {
      Call->setTailCallKind(CallInst::TCK_MustTail);
      changed = true;
    }

  if (changed)
    removeUnreachableBlocks(F);
}

// Coroutine has no suspend points. Remove heap allocation for the coroutine
// frame if possible.
static void handleNoSuspendCoroutine(coro::Shape &Shape) {
  auto *CoroBegin = Shape.CoroBegin;
  auto *CoroId = CoroBegin->getId();
  auto *AllocInst = CoroId->getCoroAlloc();
  switch (Shape.ABI) {
  case coro::ABI::Switch: {
    auto SwitchId = cast<CoroIdInst>(CoroId);
    coro::replaceCoroFree(SwitchId, /*Elide=*/AllocInst != nullptr);
    if (AllocInst) {
      IRBuilder<> Builder(AllocInst);
      // FIXME: Need to handle overaligned members.
      auto *Frame = Builder.CreateAlloca(Shape.FrameTy);
      auto *VFrame = Builder.CreateBitCast(Frame, Builder.getInt8PtrTy());
      AllocInst->replaceAllUsesWith(Builder.getFalse());
      AllocInst->eraseFromParent();
      CoroBegin->replaceAllUsesWith(VFrame);
    } else {
      CoroBegin->replaceAllUsesWith(CoroBegin->getMem());
    }
    break;
  }

  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce:
    CoroBegin->replaceAllUsesWith(UndefValue::get(CoroBegin->getType()));
    break;
  }

  CoroBegin->eraseFromParent();
}

// look for a very simple pattern
//    coro.save
//    no other calls
//    resume or destroy call
//    coro.suspend
//
// If there are other calls between coro.save and coro.suspend, they can
// potentially resume or destroy the coroutine, so it is unsafe to eliminate a
// suspend point.
static bool simplifySuspendPoint(CoroSuspendInst *Suspend,
                                 CoroBeginInst *CoroBegin) {
  auto *Save = Suspend->getCoroSave();
  auto *BB = Suspend->getParent();
  if (BB != Save->getParent())
    return false;

  CallSite SingleCallSite;

  // Check that we have only one CallSite.
  for (Instruction *I = Save->getNextNode(); I != Suspend;
       I = I->getNextNode()) {
    if (isa<CoroFrameInst>(I))
      continue;
    if (isa<CoroSubFnInst>(I))
      continue;
    if (CallSite CS = CallSite(I)) {
      if (SingleCallSite)
        return false;
      else
        SingleCallSite = CS;
    }
  }
  auto *CallInstr = SingleCallSite.getInstruction();
  if (!CallInstr)
    return false;

  auto *Callee = SingleCallSite.getCalledValue()->stripPointerCasts();

  // See if the callsite is for resumption or destruction of the coroutine.
  auto *SubFn = dyn_cast<CoroSubFnInst>(Callee);
  if (!SubFn)
    return false;

  // Does not refer to the current coroutine, we cannot do anything with it.
  if (SubFn->getFrame() != CoroBegin)
    return false;

  // Replace llvm.coro.suspend with the value that results in resumption over
  // the resume or cleanup path.
  Suspend->replaceAllUsesWith(SubFn->getRawIndex());
  Suspend->eraseFromParent();
  Save->eraseFromParent();

  // No longer need a call to coro.resume or coro.destroy.
  CallInstr->eraseFromParent();

  if (SubFn->user_empty())
    SubFn->eraseFromParent();

  return true;
}

// Remove suspend points that are simplified.
static void simplifySuspendPoints(coro::Shape &Shape) {
  // Currently, the only simplification we do is switch-lowering-specific.
  if (Shape.ABI != coro::ABI::Switch)
    return;

  auto &S = Shape.CoroSuspends;
  size_t I = 0, N = S.size();
  if (N == 0)
    return;
  while (true) {
    if (simplifySuspendPoint(cast<CoroSuspendInst>(S[I]), Shape.CoroBegin)) {
      if (--N == I)
        break;
      std::swap(S[I], S[N]);
      continue;
    }
    if (++I == N)
      break;
  }
  S.resize(N);
}

static SmallPtrSet<BasicBlock *, 4> getCoroBeginPredBlocks(CoroBeginInst *CB) {
  // Collect all blocks that we need to look for instructions to relocate.
  SmallPtrSet<BasicBlock *, 4> RelocBlocks;
  SmallVector<BasicBlock *, 4> Work;
  Work.push_back(CB->getParent());

  do {
    BasicBlock *Current = Work.pop_back_val();
    for (BasicBlock *BB : predecessors(Current))
      if (RelocBlocks.count(BB) == 0) {
        RelocBlocks.insert(BB);
        Work.push_back(BB);
      }
  } while (!Work.empty());
  return RelocBlocks;
}

static SmallPtrSet<Instruction *, 8>
getNotRelocatableInstructions(CoroBeginInst *CoroBegin,
                              SmallPtrSetImpl<BasicBlock *> &RelocBlocks) {
  SmallPtrSet<Instruction *, 8> DoNotRelocate;
  // Collect all instructions that we should not relocate
  SmallVector<Instruction *, 8> Work;

  // Start with CoroBegin and terminators of all preceding blocks.
  Work.push_back(CoroBegin);
  BasicBlock *CoroBeginBB = CoroBegin->getParent();
  for (BasicBlock *BB : RelocBlocks)
    if (BB != CoroBeginBB)
      Work.push_back(BB->getTerminator());

  // For every instruction in the Work list, place its operands in DoNotRelocate
  // set.
  do {
    Instruction *Current = Work.pop_back_val();
    DoNotRelocate.insert(Current);
    for (Value *U : Current->operands()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I)
        continue;
      if (isa<AllocaInst>(U))
        continue;
      if (DoNotRelocate.count(I) == 0) {
        Work.push_back(I);
        DoNotRelocate.insert(I);
      }
    }
  } while (!Work.empty());
  return DoNotRelocate;
}

static void relocateInstructionBefore(CoroBeginInst *CoroBegin, Function &F) {
  // Analyze which non-alloca instructions are needed for allocation and
  // relocate the rest to after coro.begin. We need to do it, since some of the
  // targets of those instructions may be placed into coroutine frame memory
  // for which becomes available after coro.begin intrinsic.

  auto BlockSet = getCoroBeginPredBlocks(CoroBegin);
  auto DoNotRelocateSet = getNotRelocatableInstructions(CoroBegin, BlockSet);

  Instruction *InsertPt = CoroBegin->getNextNode();
  BasicBlock &BB = F.getEntryBlock(); // TODO: Look at other blocks as well.
  for (auto B = BB.begin(), E = BB.end(); B != E;) {
    Instruction &I = *B++;
    if (isa<AllocaInst>(&I))
      continue;
    if (&I == CoroBegin)
      break;
    if (DoNotRelocateSet.count(&I))
      continue;
    I.moveBefore(InsertPt);
  }
}

static void splitSwitchCoroutine(Function &F, coro::Shape &Shape,
                                 SmallVectorImpl<Function *> &Clones) {
  assert(Shape.ABI == coro::ABI::Switch);

  createResumeEntryBlock(F, Shape);
  auto ResumeClone = createClone(F, ".resume", Shape,
                                 CoroCloner::Kind::SwitchResume);
  auto DestroyClone = createClone(F, ".destroy", Shape,
                                  CoroCloner::Kind::SwitchUnwind);
  auto CleanupClone = createClone(F, ".cleanup", Shape,
                                  CoroCloner::Kind::SwitchCleanup);

  postSplitCleanup(*ResumeClone);
  postSplitCleanup(*DestroyClone);
  postSplitCleanup(*CleanupClone);

  addMustTailToCoroResumes(*ResumeClone);

  // Store addresses resume/destroy/cleanup functions in the coroutine frame.
  updateCoroFrame(Shape, ResumeClone, DestroyClone, CleanupClone);

  assert(Clones.empty());
  Clones.push_back(ResumeClone);
  Clones.push_back(DestroyClone);
  Clones.push_back(CleanupClone);

  // Create a constant array referring to resume/destroy/clone functions pointed
  // by the last argument of @llvm.coro.info, so that CoroElide pass can
  // determined correct function to call.
  setCoroInfo(F, Shape, Clones);
}

static void splitRetconCoroutine(Function &F, coro::Shape &Shape,
                                 SmallVectorImpl<Function *> &Clones) {
  assert(Shape.ABI == coro::ABI::Retcon ||
         Shape.ABI == coro::ABI::RetconOnce);
  assert(Clones.empty());

  // Reset various things that the optimizer might have decided it
  // "knows" about the coroutine function due to not seeing a return.
  F.removeFnAttr(Attribute::NoReturn);
  F.removeAttribute(AttributeList::ReturnIndex, Attribute::NoAlias);
  F.removeAttribute(AttributeList::ReturnIndex, Attribute::NonNull);

  // Allocate the frame.
  auto *Id = cast<AnyCoroIdRetconInst>(Shape.CoroBegin->getId());
  Value *RawFramePtr;
  if (Shape.RetconLowering.IsFrameInlineInStorage) {
    RawFramePtr = Id->getStorage();
  } else {
    IRBuilder<> Builder(Id);

    // Determine the size of the frame.
    const DataLayout &DL = F.getParent()->getDataLayout();
    auto Size = DL.getTypeAllocSize(Shape.FrameTy);

    // Allocate.  We don't need to update the call graph node because we're
    // going to recompute it from scratch after splitting.
    RawFramePtr = Shape.emitAlloc(Builder, Builder.getInt64(Size), nullptr);
    RawFramePtr =
      Builder.CreateBitCast(RawFramePtr, Shape.CoroBegin->getType());

    // Stash the allocated frame pointer in the continuation storage.
    auto Dest = Builder.CreateBitCast(Id->getStorage(),
                                      RawFramePtr->getType()->getPointerTo());
    Builder.CreateStore(RawFramePtr, Dest);
  }

  // Map all uses of llvm.coro.begin to the allocated frame pointer.
  {
    // Make sure we don't invalidate Shape.FramePtr.
    TrackingVH<Instruction> Handle(Shape.FramePtr);
    Shape.CoroBegin->replaceAllUsesWith(RawFramePtr);
    Shape.FramePtr = Handle.getValPtr();
  }

  // Create a unique return block.
  BasicBlock *ReturnBB = nullptr;
  SmallVector<PHINode *, 4> ReturnPHIs;

  // Create all the functions in order after the main function.
  auto NextF = std::next(F.getIterator());

  // Create a continuation function for each of the suspend points.
  Clones.reserve(Shape.CoroSuspends.size());
  for (size_t i = 0, e = Shape.CoroSuspends.size(); i != e; ++i) {
    auto Suspend = cast<CoroSuspendRetconInst>(Shape.CoroSuspends[i]);

    // Create the clone declaration.
    auto Continuation =
      createCloneDeclaration(F, Shape, ".resume." + Twine(i), NextF);
    Clones.push_back(Continuation);

    // Insert a branch to the unified return block immediately before
    // the suspend point.
    auto SuspendBB = Suspend->getParent();
    auto NewSuspendBB = SuspendBB->splitBasicBlock(Suspend);
    auto Branch = cast<BranchInst>(SuspendBB->getTerminator());

    // Create the unified return block.
    if (!ReturnBB) {
      // Place it before the first suspend.
      ReturnBB = BasicBlock::Create(F.getContext(), "coro.return", &F,
                                    NewSuspendBB);
      Shape.RetconLowering.ReturnBlock = ReturnBB;

      IRBuilder<> Builder(ReturnBB);

      // Create PHIs for all the return values.
      assert(ReturnPHIs.empty());

      // First, the continuation.
      ReturnPHIs.push_back(Builder.CreatePHI(Continuation->getType(),
                                             Shape.CoroSuspends.size()));

      // Next, all the directly-yielded values.
      for (auto ResultTy : Shape.getRetconResultTypes())
        ReturnPHIs.push_back(Builder.CreatePHI(ResultTy,
                                               Shape.CoroSuspends.size()));

      // Build the return value.
      auto RetTy = F.getReturnType();

      // Cast the continuation value if necessary.
      // We can't rely on the types matching up because that type would
      // have to be infinite.
      auto CastedContinuationTy =
        (ReturnPHIs.size() == 1 ? RetTy : RetTy->getStructElementType(0));
      auto *CastedContinuation =
        Builder.CreateBitCast(ReturnPHIs[0], CastedContinuationTy);

      Value *RetV;
      if (ReturnPHIs.size() == 1) {
        RetV = CastedContinuation;
      } else {
        RetV = UndefValue::get(RetTy);
        RetV = Builder.CreateInsertValue(RetV, CastedContinuation, 0);
        for (size_t I = 1, E = ReturnPHIs.size(); I != E; ++I)
          RetV = Builder.CreateInsertValue(RetV, ReturnPHIs[I], I);
      }

      Builder.CreateRet(RetV);
    }

    // Branch to the return block.
    Branch->setSuccessor(0, ReturnBB);
    ReturnPHIs[0]->addIncoming(Continuation, SuspendBB);
    size_t NextPHIIndex = 1;
    for (auto &VUse : Suspend->value_operands())
      ReturnPHIs[NextPHIIndex++]->addIncoming(&*VUse, SuspendBB);
    assert(NextPHIIndex == ReturnPHIs.size());
  }

  assert(Clones.size() == Shape.CoroSuspends.size());
  for (size_t i = 0, e = Shape.CoroSuspends.size(); i != e; ++i) {
    auto Suspend = Shape.CoroSuspends[i];
    auto Clone = Clones[i];

    CoroCloner(F, "resume." + Twine(i), Shape, Clone, Suspend).create();
  }
}

static void splitCoroutine(Function &F, coro::Shape &Shape,
                           SmallVectorImpl<Function *> &Clones) {
  switch (Shape.ABI) {
  case coro::ABI::Switch:
    return splitSwitchCoroutine(F, Shape, Clones);
  case coro::ABI::Retcon:
  case coro::ABI::RetconOnce:
    return splitRetconCoroutine(F, Shape, Clones);
  }
  llvm_unreachable("bad ABI kind");
}

static void splitCoroutine(Function &F, CallGraph &CG, CallGraphSCC &SCC) {
  coro::Shape Shape(F);
  if (!Shape.CoroBegin)
    return;

  simplifySuspendPoints(Shape);
  relocateInstructionBefore(Shape.CoroBegin, F);
  buildCoroutineFrame(F, Shape);
  replaceFrameSize(Shape);

  SmallVector<Function*, 4> Clones;

  // If there are no suspend points, no split required, just remove
  // the allocation and deallocation blocks, they are not needed.
  if (Shape.CoroSuspends.empty()) {
    handleNoSuspendCoroutine(Shape);
  } else {
    splitCoroutine(F, Shape, Clones);
  }

  removeCoroEnds(Shape, &CG);
  postSplitCleanup(F);

  // Update call graph and add the functions we created to the SCC.
  coro::updateCallGraph(F, Clones, CG, SCC);
}

// When we see the coroutine the first time, we insert an indirect call to a
// devirt trigger function and mark the coroutine that it is now ready for
// split.
static void prepareForSplit(Function &F, CallGraph &CG) {
  Module &M = *F.getParent();
#ifndef NDEBUG
  Function *DevirtFn = M.getFunction(CORO_DEVIRT_TRIGGER_FN);
  assert(DevirtFn && "coro.devirt.trigger function not found");
#endif

  F.addFnAttr(CORO_PRESPLIT_ATTR, PREPARED_FOR_SPLIT);

  // Insert an indirect call sequence that will be devirtualized by CoroElide
  // pass:
  //    %0 = call i8* @llvm.coro.subfn.addr(i8* null, i8 -1)
  //    %1 = bitcast i8* %0 to void(i8*)*
  //    call void %1(i8* null)
  coro::LowererBase Lowerer(M);
  Instruction *InsertPt = F.getEntryBlock().getTerminator();
  auto *Null = ConstantPointerNull::get(Type::getInt8PtrTy(F.getContext()));
  auto *DevirtFnAddr =
      Lowerer.makeSubFnCall(Null, CoroSubFnInst::RestartTrigger, InsertPt);
  auto *IndirectCall = CallInst::Create(DevirtFnAddr, Null, "", InsertPt);

  // Update CG graph with an indirect call we just added.
  CG[&F]->addCalledFunction(IndirectCall, CG.getCallsExternalNode());
}

// Make sure that there is a devirtualization trigger function that CoroSplit
// pass uses the force restart CGSCC pipeline. If devirt trigger function is not
// found, we will create one and add it to the current SCC.
static void createDevirtTriggerFunc(CallGraph &CG, CallGraphSCC &SCC) {
  Module &M = CG.getModule();
  if (M.getFunction(CORO_DEVIRT_TRIGGER_FN))
    return;

  LLVMContext &C = M.getContext();
  auto *FnTy = FunctionType::get(Type::getVoidTy(C), Type::getInt8PtrTy(C),
                                 /*IsVarArgs=*/false);
  Function *DevirtFn =
      Function::Create(FnTy, GlobalValue::LinkageTypes::PrivateLinkage,
                       CORO_DEVIRT_TRIGGER_FN, &M);
  DevirtFn->addFnAttr(Attribute::AlwaysInline);
  auto *Entry = BasicBlock::Create(C, "entry", DevirtFn);
  ReturnInst::Create(C, Entry);

  auto *Node = CG.getOrInsertFunction(DevirtFn);

  SmallVector<CallGraphNode *, 8> Nodes(SCC.begin(), SCC.end());
  Nodes.push_back(Node);
  SCC.initialize(Nodes);
}

/// Replace a call to llvm.coro.prepare.retcon.
static void replacePrepare(CallInst *Prepare, CallGraph &CG) {
  auto CastFn = Prepare->getArgOperand(0); // as an i8*
  auto Fn = CastFn->stripPointerCasts(); // as its original type

  // Find call graph nodes for the preparation.
  CallGraphNode *PrepareUserNode = nullptr, *FnNode = nullptr;
  if (auto ConcreteFn = dyn_cast<Function>(Fn)) {
    PrepareUserNode = CG[Prepare->getFunction()];
    FnNode = CG[ConcreteFn];
  }

  // Attempt to peephole this pattern:
  //    %0 = bitcast [[TYPE]] @some_function to i8*
  //    %1 = call @llvm.coro.prepare.retcon(i8* %0)
  //    %2 = bitcast %1 to [[TYPE]]
  // ==>
  //    %2 = @some_function
  for (auto UI = Prepare->use_begin(), UE = Prepare->use_end();
         UI != UE; ) {
    // Look for bitcasts back to the original function type.
    auto *Cast = dyn_cast<BitCastInst>((UI++)->getUser());
    if (!Cast || Cast->getType() != Fn->getType()) continue;

    // Check whether the replacement will introduce new direct calls.
    // If so, we'll need to update the call graph.
    if (PrepareUserNode) {
      for (auto &Use : Cast->uses()) {
        auto CS = CallSite(Use.getUser());
        if (!CS || !CS.isCallee(&Use)) continue;
        PrepareUserNode->removeCallEdgeFor(CS);
        PrepareUserNode->addCalledFunction(CS, FnNode);
      }
    }

    // Replace and remove the cast.
    Cast->replaceAllUsesWith(Fn);
    Cast->eraseFromParent();
  }

  // Replace any remaining uses with the function as an i8*.
  // This can never directly be a callee, so we don't need to update CG.
  Prepare->replaceAllUsesWith(CastFn);
  Prepare->eraseFromParent();

  // Kill dead bitcasts.
  while (auto *Cast = dyn_cast<BitCastInst>(CastFn)) {
    if (!Cast->use_empty()) break;
    CastFn = Cast->getOperand(0);
    Cast->eraseFromParent();
  }
}

/// Remove calls to llvm.coro.prepare.retcon, a barrier meant to prevent
/// IPO from operating on calls to a retcon coroutine before it's been
/// split.  This is only safe to do after we've split all retcon
/// coroutines in the module.  We can do that this in this pass because
/// this pass does promise to split all retcon coroutines (as opposed to
/// switch coroutines, which are lowered in multiple stages).
static bool replaceAllPrepares(Function *PrepareFn, CallGraph &CG) {
  bool Changed = false;
  for (auto PI = PrepareFn->use_begin(), PE = PrepareFn->use_end();
         PI != PE; ) {
    // Intrinsics can only be used in calls.
    auto *Prepare = cast<CallInst>((PI++)->getUser());
    replacePrepare(Prepare, CG);
    Changed = true;
  }

  return Changed;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

namespace {

struct CoroSplit : public CallGraphSCCPass {
  static char ID; // Pass identification, replacement for typeid

  CoroSplit() : CallGraphSCCPass(ID) {
    initializeCoroSplitPass(*PassRegistry::getPassRegistry());
  }

  bool Run = false;

  // A coroutine is identified by the presence of coro.begin intrinsic, if
  // we don't have any, this pass has nothing to do.
  bool doInitialization(CallGraph &CG) override {
    Run = coro::declaresIntrinsics(CG.getModule(),
                                   {"llvm.coro.begin",
                                    "llvm.coro.prepare.retcon"});
    return CallGraphSCCPass::doInitialization(CG);
  }

  bool runOnSCC(CallGraphSCC &SCC) override {
    if (!Run)
      return false;

    // Check for uses of llvm.coro.prepare.retcon.
    auto PrepareFn =
      SCC.getCallGraph().getModule().getFunction("llvm.coro.prepare.retcon");
    if (PrepareFn && PrepareFn->use_empty())
      PrepareFn = nullptr;

    // Find coroutines for processing.
    SmallVector<Function *, 4> Coroutines;
    for (CallGraphNode *CGN : SCC)
      if (auto *F = CGN->getFunction())
        if (F->hasFnAttribute(CORO_PRESPLIT_ATTR))
          Coroutines.push_back(F);

    if (Coroutines.empty() && !PrepareFn)
      return false;

    CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

    if (Coroutines.empty())
      return replaceAllPrepares(PrepareFn, CG);

    createDevirtTriggerFunc(CG, SCC);

    // Split all the coroutines.
    for (Function *F : Coroutines) {
      Attribute Attr = F->getFnAttribute(CORO_PRESPLIT_ATTR);
      StringRef Value = Attr.getValueAsString();
      DEBUG(dbgs() << "CoroSplit: Processing coroutine '" << F->getName()
                   << "' state: " << Value << "\n");
      if (Value == UNPREPARED_FOR_SPLIT) {
        prepareForSplit(*F, CG);
        continue;
      }
      F->removeFnAttr(CORO_PRESPLIT_ATTR);
      splitCoroutine(*F, CG, SCC);
    }

    if (PrepareFn)
      replaceAllPrepares(PrepareFn, CG);

    return true;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    CallGraphSCCPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return "Coroutine Splitting"; }
};

} // end anonymous namespace

char CoroSplit::ID = 0;

INITIALIZE_PASS(
    CoroSplit, "coro-split",
    "Split coroutine into a set of functions driving its state machine", false,
    false)

Pass *llvm::createCoroSplitPass() { return new CoroSplit(); }
