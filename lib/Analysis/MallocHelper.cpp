//===-- MallocHelper.cpp - Functions to identify malloc calls -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions identifies calls to malloc, bitcasts of malloc
// calls, and the types and array sizes associated with them.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/MallocHelper.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Analysis/ConstantFolding.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
//  malloc Call Utility Functions.
//

/// isMalloc - Returns true if the the value is either a malloc call or a
/// bitcast of the result of a malloc call.
bool llvm::isMalloc(const Value* I) {
  return extractMallocCall(I) || extractMallocCallFromBitCast(I);
}

static bool isMallocCall(const CallInst *CI) {
  if (!CI)
    return false;

  const Module* M = CI->getParent()->getParent()->getParent();
  Constant *MallocFunc = M->getFunction("malloc");

  if (CI->getOperand(0) != MallocFunc)
    return false;

  return true;
}

/// extractMallocCall - Returns the corresponding CallInst if the instruction
/// is a malloc call.  Since CallInst::CreateMalloc() only creates calls, we
/// ignore InvokeInst here.
const CallInst* llvm::extractMallocCall(const Value* I) {
  const CallInst *CI = dyn_cast<CallInst>(I);
  return (isMallocCall(CI)) ? CI : NULL;
}

CallInst* llvm::extractMallocCall(Value* I) {
  CallInst *CI = dyn_cast<CallInst>(I);
  return (isMallocCall(CI)) ? CI : NULL;
}

static bool isBitCastOfMallocCall(const BitCastInst* BCI) {
  if (!BCI)
    return false;
    
  return isMallocCall(dyn_cast<CallInst>(BCI->getOperand(0)));
}

/// extractMallocCallFromBitCast - Returns the corresponding CallInst if the
/// instruction is a bitcast of the result of a malloc call.
CallInst* llvm::extractMallocCallFromBitCast(Value* I) {
  BitCastInst *BCI = dyn_cast<BitCastInst>(I);
  return (isBitCastOfMallocCall(BCI)) ? cast<CallInst>(BCI->getOperand(0))
                                      : NULL;
}

const CallInst* llvm::extractMallocCallFromBitCast(const Value* I) {
  const BitCastInst *BCI = dyn_cast<BitCastInst>(I);
  return (isBitCastOfMallocCall(BCI)) ? cast<CallInst>(BCI->getOperand(0))
                                      : NULL;
}

static bool isArrayMallocHelper(const CallInst *CI, LLVMContext &Context,
                                const TargetData* TD) {
  if (!CI)
    return false;

  const Type* T = getMallocAllocatedType(CI);

  // We can only indentify an array malloc if we know the type of the malloc 
  // call.
  if (!T) return false;

  Value* MallocArg = CI->getOperand(1);
  Constant *ElementSize = ConstantExpr::getSizeOf(T);
  ElementSize = ConstantExpr::getTruncOrBitCast(ElementSize, 
                                                MallocArg->getType());
  Constant *FoldedElementSize = ConstantFoldConstantExpression(
                                       cast<ConstantExpr>(ElementSize), 
                                       Context, TD);


  if (isa<ConstantExpr>(MallocArg))
    return (MallocArg != ElementSize);

  BinaryOperator *BI = dyn_cast<BinaryOperator>(MallocArg);
  if (!BI)
    return false;

  if (BI->getOpcode() == Instruction::Mul)
    // ArraySize * ElementSize
    if (BI->getOperand(1) == ElementSize ||
        (FoldedElementSize && BI->getOperand(1) == FoldedElementSize))
      return true;

  // TODO: Detect case where MallocArg mul has been transformed to shl.

  return false;
}

/// isArrayMalloc - Returns the corresponding CallInst if the instruction 
/// matches the malloc call IR generated by CallInst::CreateMalloc().  This 
/// means that it is a malloc call with one bitcast use AND the malloc call's 
/// size argument is:
///  1. a constant not equal to the malloc's allocated type
/// or
///  2. the result of a multiplication by the malloc's allocated type
/// Otherwise it returns NULL.
/// The unique bitcast is needed to determine the type/size of the array
/// allocation.
CallInst* llvm::isArrayMalloc(Value* I, LLVMContext &Context,
                              const TargetData* TD) {
  CallInst *CI = extractMallocCall(I);
  return (isArrayMallocHelper(CI, Context, TD)) ? CI : NULL;
}

const CallInst* llvm::isArrayMalloc(const Value* I, LLVMContext &Context,
                                    const TargetData* TD) {
  const CallInst *CI = extractMallocCall(I);
  return (isArrayMallocHelper(CI, Context, TD)) ? CI : NULL;
}

/// getMallocType - Returns the PointerType resulting from the malloc call.
/// This PointerType is the result type of the call's only bitcast use.
/// If there is no unique bitcast use, then return NULL.
const PointerType* llvm::getMallocType(const CallInst* CI) {
  assert(isMalloc(CI) && "GetMallocType and not malloc call");
  
  const BitCastInst* BCI = NULL;
  
  // Determine if CallInst has a bitcast use.
  for (Value::use_const_iterator UI = CI->use_begin(), E = CI->use_end();
       UI != E; )
    if ((BCI = dyn_cast<BitCastInst>(cast<Instruction>(*UI++))))
      break;

  // Malloc call has 1 bitcast use and no other uses, so type is the bitcast's
  // destination type.
  if (BCI && CI->hasOneUse())
    return cast<PointerType>(BCI->getDestTy());

  // Malloc call was not bitcast, so the type is the malloc's return type, i8*.
  if (!BCI)
    return cast<PointerType>(CI->getType());

  // Type could not be determined.
  return NULL;
}

/// getMallocAllocatedType - Returns the Type allocated by malloc call. This
/// Type is the result type of the call's only bitcast use. If there is no
/// unique bitcast use, then return NULL.
const Type* llvm::getMallocAllocatedType(const CallInst* CI) {
  const PointerType* PT = getMallocType(CI);
  return PT ? PT->getElementType() : NULL;
}

/// isConstantOne - Return true only if val is constant int 1.
static bool isConstantOne(Value *val) {
  return isa<ConstantInt>(val) && cast<ConstantInt>(val)->isOne();
}

/// getMallocArraySize - Returns the array size of a malloc call.  The array
/// size is computated in 1 of 3 ways:
///  1. If the element type if of size 1, then array size is the argument to 
///     malloc.
///  2. Else if the malloc's argument is a constant, the array size is that
///     argument divided by the element type's size.
///  3. Else the malloc argument must be a multiplication and the array size is
///     the first operand of the multiplication.
/// This function returns constant 1 if:
///  1. The malloc call's allocated type cannot be determined.
///  2. IR wasn't created by a call to CallInst::CreateMalloc() with a non-NULL
///     ArraySize.
Value* llvm::getMallocArraySize(CallInst* CI, LLVMContext &Context,
                                const TargetData* TD) {
  // Match CreateMalloc's use of constant 1 array-size for non-array mallocs.
  if (!isArrayMalloc(CI, Context, TD))
    return ConstantInt::get(CI->getOperand(1)->getType(), 1);

  Value* MallocArg = CI->getOperand(1);
  assert(getMallocAllocatedType(CI) && "getMallocArraySize and no type");
  Constant *ElementSize = ConstantExpr::getSizeOf(getMallocAllocatedType(CI));
  ElementSize = ConstantExpr::getTruncOrBitCast(ElementSize, 
                                                MallocArg->getType());

  Constant* CO = dyn_cast<Constant>(MallocArg);
  BinaryOperator* BO = dyn_cast<BinaryOperator>(MallocArg);
  assert((isConstantOne(ElementSize) || CO || BO) &&
         "getMallocArraySize and malformed malloc IR");
      
  if (isConstantOne(ElementSize))
    return MallocArg;
    
  if (CO)
    return CO->getOperand(0);
    
  // TODO: Detect case where MallocArg mul has been transformed to shl.

  assert(BO && "getMallocArraySize not constant but not multiplication either");
  return BO->getOperand(0);
}
