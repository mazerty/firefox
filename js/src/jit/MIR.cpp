/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/MIR.h"

#include "mozilla/EndianUtils.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"

#include <array>
#include <utility>

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/RegExp.h"
#include "jit/AtomicOperations.h"
#include "jit/CompileInfo.h"
#include "jit/KnownClass.h"
#include "jit/MIR-wasm.h"
#include "jit/MIRGraph.h"
#include "jit/RangeAnalysis.h"
#include "jit/VMFunctions.h"
#include "jit/WarpBuilderShared.h"
#include "js/Conversions.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo, JSTypedMethodJitInfo
#include "js/ScalarType.h"            // js::Scalar::Type
#include "util/Text.h"
#include "util/Unicode.h"
#include "vm/BigIntType.h"
#include "vm/ConstantCompareOperand.h"
#include "vm/Float16.h"
#include "vm/Iteration.h"    // js::NativeIterator
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Uint8Clamped.h"

#include "vm/BytecodeUtil-inl.h"
#include "vm/JSAtomUtils-inl.h"  // TypeName

using namespace js;
using namespace js::jit;

using JS::ToInt32;

using mozilla::IsFloat32Representable;
using mozilla::IsPowerOfTwo;
using mozilla::NumbersAreIdentical;

NON_GC_POINTER_TYPE_ASSERTIONS_GENERATED

#ifdef DEBUG
size_t MUse::index() const { return consumer()->indexOf(this); }
#endif

template <size_t Op>
static void ConvertDefinitionToDouble(TempAllocator& alloc, MDefinition* def,
                                      MInstruction* consumer) {
  MInstruction* replace = MToDouble::New(alloc, def);
  consumer->replaceOperand(Op, replace);
  consumer->block()->insertBefore(consumer, replace);
}

template <size_t Arity, size_t Index>
static void ConvertOperandToDouble(MAryInstruction<Arity>* def,
                                   TempAllocator& alloc) {
  static_assert(Index < Arity);
  auto* operand = def->getOperand(Index);
  if (operand->type() == MIRType::Float32) {
    ConvertDefinitionToDouble<Index>(alloc, operand, def);
  }
}

template <size_t Arity, size_t... ISeq>
static void ConvertOperandsToDouble(MAryInstruction<Arity>* def,
                                    TempAllocator& alloc,
                                    std::index_sequence<ISeq...>) {
  (ConvertOperandToDouble<Arity, ISeq>(def, alloc), ...);
}

template <size_t Arity>
static void ConvertOperandsToDouble(MAryInstruction<Arity>* def,
                                    TempAllocator& alloc) {
  ConvertOperandsToDouble<Arity>(def, alloc, std::make_index_sequence<Arity>{});
}

template <size_t Arity, size_t... ISeq>
static bool AllOperandsCanProduceFloat32(MAryInstruction<Arity>* def,
                                         std::index_sequence<ISeq...>) {
  return (def->getOperand(ISeq)->canProduceFloat32() && ...);
}

template <size_t Arity>
static bool AllOperandsCanProduceFloat32(MAryInstruction<Arity>* def) {
  return AllOperandsCanProduceFloat32<Arity>(def,
                                             std::make_index_sequence<Arity>{});
}

static bool CheckUsesAreFloat32Consumers(const MInstruction* ins) {
  if (ins->isImplicitlyUsed()) {
    return false;
  }
  bool allConsumerUses = true;
  for (MUseDefIterator use(ins); allConsumerUses && use; use++) {
    allConsumerUses &= use.def()->canConsumeFloat32(use.use());
  }
  return allConsumerUses;
}

#ifdef JS_JITSPEW
static const char* OpcodeName(MDefinition::Opcode op) {
  static const char* const names[] = {
#  define NAME(x) #x,
      MIR_OPCODE_LIST(NAME)
#  undef NAME
  };
  return names[unsigned(op)];
}

void MDefinition::PrintOpcodeName(GenericPrinter& out, Opcode op) {
  out.printf("%s", OpcodeName(op));
}

uint32_t js::jit::GetMBasicBlockId(const MBasicBlock* block) {
  return block->id();
}
#endif

static MConstant* EvaluateInt64ConstantOperands(TempAllocator& alloc,
                                                MBinaryInstruction* ins) {
  MDefinition* left = ins->getOperand(0);
  MDefinition* right = ins->getOperand(1);

  if (!left->isConstant() || !right->isConstant()) {
    return nullptr;
  }

  MOZ_ASSERT(left->type() == MIRType::Int64);
  MOZ_ASSERT(right->type() == MIRType::Int64);

  int64_t lhs = left->toConstant()->toInt64();
  int64_t rhs = right->toConstant()->toInt64();
  int64_t ret;

  switch (ins->op()) {
    case MDefinition::Opcode::BitAnd:
      ret = lhs & rhs;
      break;
    case MDefinition::Opcode::BitOr:
      ret = lhs | rhs;
      break;
    case MDefinition::Opcode::BitXor:
      ret = lhs ^ rhs;
      break;
    case MDefinition::Opcode::Lsh:
      ret = lhs << (rhs & 0x3F);
      break;
    case MDefinition::Opcode::Rsh:
      ret = lhs >> (rhs & 0x3F);
      break;
    case MDefinition::Opcode::Ursh:
      ret = uint64_t(lhs) >> (uint64_t(rhs) & 0x3F);
      break;
    case MDefinition::Opcode::Add:
      ret = lhs + rhs;
      break;
    case MDefinition::Opcode::Sub:
      ret = lhs - rhs;
      break;
    case MDefinition::Opcode::Mul:
      ret = lhs * rhs;
      break;
    case MDefinition::Opcode::Div:
      if (rhs == 0) {
        // Division by zero will trap at runtime.
        return nullptr;
      }
      if (ins->toDiv()->isUnsigned()) {
        ret = int64_t(uint64_t(lhs) / uint64_t(rhs));
      } else if (lhs == INT64_MIN || rhs == -1) {
        // Overflow will trap at runtime.
        return nullptr;
      } else {
        ret = lhs / rhs;
      }
      break;
    case MDefinition::Opcode::Mod:
      if (rhs == 0) {
        // Division by zero will trap at runtime.
        return nullptr;
      }
      if (!ins->toMod()->isUnsigned() && (lhs < 0 || rhs < 0)) {
        // Handle all negative values at runtime, for simplicity.
        return nullptr;
      }
      ret = int64_t(uint64_t(lhs) % uint64_t(rhs));
      break;
    default:
      MOZ_CRASH("NYI");
  }

  return MConstant::NewInt64(alloc, ret);
}

static MConstant* EvaluateConstantOperands(TempAllocator& alloc,
                                           MBinaryInstruction* ins) {
  MDefinition* left = ins->getOperand(0);
  MDefinition* right = ins->getOperand(1);

  MOZ_ASSERT(IsTypeRepresentableAsDouble(left->type()));
  MOZ_ASSERT(IsTypeRepresentableAsDouble(right->type()));

  if (!left->isConstant() || !right->isConstant()) {
    return nullptr;
  }

  MConstant* lhs = left->toConstant();
  MConstant* rhs = right->toConstant();
  double ret = JS::GenericNaN();

  switch (ins->op()) {
    case MDefinition::Opcode::BitAnd:
      ret = double(lhs->toInt32() & rhs->toInt32());
      break;
    case MDefinition::Opcode::BitOr:
      ret = double(lhs->toInt32() | rhs->toInt32());
      break;
    case MDefinition::Opcode::BitXor:
      ret = double(lhs->toInt32() ^ rhs->toInt32());
      break;
    case MDefinition::Opcode::Lsh:
      ret = double(uint32_t(lhs->toInt32()) << (rhs->toInt32() & 0x1F));
      break;
    case MDefinition::Opcode::Rsh:
      ret = double(lhs->toInt32() >> (rhs->toInt32() & 0x1F));
      break;
    case MDefinition::Opcode::Ursh:
      ret = double(uint32_t(lhs->toInt32()) >> (rhs->toInt32() & 0x1F));
      break;
    case MDefinition::Opcode::Add:
      ret = lhs->numberToDouble() + rhs->numberToDouble();
      break;
    case MDefinition::Opcode::Sub:
      ret = lhs->numberToDouble() - rhs->numberToDouble();
      break;
    case MDefinition::Opcode::Mul:
      ret = lhs->numberToDouble() * rhs->numberToDouble();
      break;
    case MDefinition::Opcode::Div:
      if (ins->toDiv()->isUnsigned()) {
        if (rhs->isInt32(0)) {
          if (ins->toDiv()->trapOnError()) {
            return nullptr;
          }
          ret = 0.0;
        } else {
          ret = double(uint32_t(lhs->toInt32()) / uint32_t(rhs->toInt32()));
        }
      } else {
        ret = NumberDiv(lhs->numberToDouble(), rhs->numberToDouble());
      }
      break;
    case MDefinition::Opcode::Mod:
      if (ins->toMod()->isUnsigned()) {
        if (rhs->isInt32(0)) {
          if (ins->toMod()->trapOnError()) {
            return nullptr;
          }
          ret = 0.0;
        } else {
          ret = double(uint32_t(lhs->toInt32()) % uint32_t(rhs->toInt32()));
        }
      } else {
        ret = NumberMod(lhs->numberToDouble(), rhs->numberToDouble());
      }
      break;
    default:
      MOZ_CRASH("NYI");
  }

  if (ins->type() == MIRType::Float32) {
    return MConstant::NewFloat32(alloc, float(ret));
  }
  if (ins->type() == MIRType::Double) {
    return MConstant::NewDouble(alloc, ret);
  }
  MOZ_ASSERT(ins->type() == MIRType::Int32);

  // If the result isn't an int32 (for example, a division where the numerator
  // isn't evenly divisible by the denominator), decline folding.
  int32_t intRet;
  if (!mozilla::NumberIsInt32(ret, &intRet)) {
    return nullptr;
  }
  return MConstant::NewInt32(alloc, intRet);
}

static MConstant* EvaluateConstantNaNOperand(MBinaryInstruction* ins) {
  auto* left = ins->lhs();
  auto* right = ins->rhs();

  MOZ_ASSERT(IsTypeRepresentableAsDouble(left->type()));
  MOZ_ASSERT(IsTypeRepresentableAsDouble(right->type()));
  MOZ_ASSERT(left->type() == ins->type());
  MOZ_ASSERT(right->type() == ins->type());

  // Don't fold NaN if we can't return a floating point type.
  if (!IsFloatingPointType(ins->type())) {
    return nullptr;
  }

  MOZ_ASSERT(!left->isConstant() || !right->isConstant(),
             "EvaluateConstantOperands should have handled this case");

  // One operand must be a constant NaN.
  MConstant* cst;
  if (left->isConstant()) {
    cst = left->toConstant();
  } else if (right->isConstant()) {
    cst = right->toConstant();
  } else {
    return nullptr;
  }
  if (!std::isnan(cst->numberToDouble())) {
    return nullptr;
  }

  // Fold to constant NaN.
  return cst;
}

static MMul* EvaluateExactReciprocal(TempAllocator& alloc, MDiv* ins) {
  // we should fold only when it is a floating point operation
  if (!IsFloatingPointType(ins->type())) {
    return nullptr;
  }

  MDefinition* left = ins->getOperand(0);
  MDefinition* right = ins->getOperand(1);

  if (!right->isConstant()) {
    return nullptr;
  }

  int32_t num;
  if (!mozilla::NumberIsInt32(right->toConstant()->numberToDouble(), &num)) {
    return nullptr;
  }

  // check if rhs is a power of two or zero
  if (num != 0 && !mozilla::IsPowerOfTwo(mozilla::Abs(num))) {
    return nullptr;
  }

  double ret = 1.0 / double(num);

  MConstant* foldedRhs;
  if (ins->type() == MIRType::Float32) {
    foldedRhs = MConstant::NewFloat32(alloc, ret);
  } else {
    foldedRhs = MConstant::NewDouble(alloc, ret);
  }

  MOZ_ASSERT(foldedRhs->type() == ins->type());
  ins->block()->insertBefore(ins, foldedRhs);

  MMul* mul = MMul::New(alloc, left, foldedRhs, ins->type());
  mul->setMustPreserveNaN(ins->mustPreserveNaN());
  return mul;
}

#ifdef JS_JITSPEW
const char* MDefinition::opName() const { return OpcodeName(op()); }

void MDefinition::printName(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  out.printf("#%u", id());
}
#endif

HashNumber MDefinition::valueHash() const {
  HashNumber out = HashNumber(op());
  for (size_t i = 0, e = numOperands(); i < e; i++) {
    out = addU32ToHash(out, getOperand(i)->id());
  }
  if (MDefinition* dep = dependency()) {
    out = addU32ToHash(out, dep->id());
  }
  return out;
}

HashNumber MNullaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MUnaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MBinaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  hash = addU32ToHash(hash, getOperand(1)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MTernaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  hash = addU32ToHash(hash, getOperand(1)->id());
  hash = addU32ToHash(hash, getOperand(2)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

HashNumber MQuaternaryInstruction::valueHash() const {
  HashNumber hash = HashNumber(op());
  hash = addU32ToHash(hash, getOperand(0)->id());
  hash = addU32ToHash(hash, getOperand(1)->id());
  hash = addU32ToHash(hash, getOperand(2)->id());
  hash = addU32ToHash(hash, getOperand(3)->id());
  if (MDefinition* dep = dependency()) {
    hash = addU32ToHash(hash, dep->id());
  }
  MOZ_ASSERT(hash == MDefinition::valueHash());
  return hash;
}

const MDefinition* MDefinition::skipObjectGuards() const {
  const MDefinition* result = this;
  // These instructions don't modify the object and just guard specific
  // properties.
  while (true) {
    if (result->isGuardShape()) {
      result = result->toGuardShape()->object();
      continue;
    }
    if (result->isGuardNullProto()) {
      result = result->toGuardNullProto()->object();
      continue;
    }
    if (result->isGuardProto()) {
      result = result->toGuardProto()->object();
      continue;
    }

    break;
  }

  return result;
}

bool MDefinition::congruentIfOperandsEqual(const MDefinition* ins) const {
  if (op() != ins->op()) {
    return false;
  }

  if (type() != ins->type()) {
    return false;
  }

  if (isEffectful() || ins->isEffectful()) {
    return false;
  }

  if (numOperands() != ins->numOperands()) {
    return false;
  }

  for (size_t i = 0, e = numOperands(); i < e; i++) {
    if (getOperand(i) != ins->getOperand(i)) {
      return false;
    }
  }

  return true;
}

MDefinition* MDefinition::foldsTo(TempAllocator& alloc) {
  // In the default case, there are no constants to fold.
  return this;
}

MDefinition* MInstruction::foldsToStore(TempAllocator& alloc) {
  if (!dependency()) {
    return nullptr;
  }

  MDefinition* store = dependency();
  if (mightAlias(store) != AliasType::MustAlias) {
    return nullptr;
  }

  if (!store->block()->dominates(block())) {
    return nullptr;
  }

  MDefinition* value;
  switch (store->op()) {
    case Opcode::StoreFixedSlot:
      value = store->toStoreFixedSlot()->value();
      break;
    case Opcode::StoreDynamicSlot:
      value = store->toStoreDynamicSlot()->value();
      break;
    case Opcode::StoreElement:
      value = store->toStoreElement()->value();
      break;
    default:
      MOZ_CRASH("unknown store");
  }

  // If the type are matching then we return the value which is used as
  // argument of the store.
  if (value->type() != type()) {
    // If we expect to read a type which is more generic than the type seen
    // by the store, then we box the value used by the store.
    if (type() != MIRType::Value) {
      return nullptr;
    }

    MOZ_ASSERT(value->type() < MIRType::Value);
    MBox* box = MBox::New(alloc, value);
    value = box;
  }

  return value;
}

void MDefinition::analyzeEdgeCasesForward() {}

void MDefinition::analyzeEdgeCasesBackward() {}

void MInstruction::setResumePoint(MResumePoint* resumePoint) {
  MOZ_ASSERT(!resumePoint_);
  resumePoint_ = resumePoint;
  resumePoint_->setInstruction(this);
}

void MInstruction::stealResumePoint(MInstruction* other) {
  MResumePoint* resumePoint = other->resumePoint_;
  other->resumePoint_ = nullptr;

  resumePoint->resetInstruction();
  setResumePoint(resumePoint);
}

void MInstruction::moveResumePointAsEntry() {
  MOZ_ASSERT(isNop());
  block()->clearEntryResumePoint();
  block()->setEntryResumePoint(resumePoint_);
  resumePoint_->resetInstruction();
  resumePoint_ = nullptr;
}

void MInstruction::clearResumePoint() {
  resumePoint_->resetInstruction();
  block()->discardPreAllocatedResumePoint(resumePoint_);
  resumePoint_ = nullptr;
}

MDefinition* MTest::foldsDoubleNegation(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);

  if (op->isNot()) {
    // If the operand of the Not is itself a Not, they cancel out.
    MDefinition* opop = op->getOperand(0);
    if (opop->isNot()) {
      return MTest::New(alloc, opop->toNot()->input(), ifTrue(), ifFalse());
    }
    return MTest::New(alloc, op->toNot()->input(), ifFalse(), ifTrue());
  }
  return nullptr;
}

MDefinition* MTest::foldsConstant(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);
  if (MConstant* opConst = op->maybeConstantValue()) {
    bool b;
    if (opConst->valueToBoolean(&b)) {
      return MGoto::New(alloc, b ? ifTrue() : ifFalse());
    }
  }
  return nullptr;
}

MDefinition* MTest::foldsTypes(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);

  switch (op->type()) {
    case MIRType::Undefined:
    case MIRType::Null:
      return MGoto::New(alloc, ifFalse());
    case MIRType::Symbol:
      return MGoto::New(alloc, ifTrue());
    default:
      break;
  }
  return nullptr;
}

class UsesIterator {
  MDefinition* def_;

 public:
  explicit UsesIterator(MDefinition* def) : def_(def) {}
  auto begin() const { return def_->usesBegin(); }
  auto end() const { return def_->usesEnd(); }
};

static bool AllInstructionsDeadIfUnused(MBasicBlock* block) {
  for (auto* ins : *block) {
    // Skip trivial instructions.
    if (ins->isNop() || ins->isGoto()) {
      continue;
    }

    // All uses must be within the current block.
    for (auto* use : UsesIterator(ins)) {
      if (use->consumer()->block() != block) {
        return false;
      }
    }

    // All instructions within this block must be dead if unused.
    if (!DeadIfUnused(ins)) {
      return false;
    }
  }
  return true;
}

MDefinition* MTest::foldsNeedlessControlFlow(TempAllocator& alloc) {
  // All instructions within both successors need be dead if unused.
  if (!AllInstructionsDeadIfUnused(ifTrue()) ||
      !AllInstructionsDeadIfUnused(ifFalse())) {
    return nullptr;
  }

  // Both successors must have the same target successor.
  if (ifTrue()->numSuccessors() != 1 || ifFalse()->numSuccessors() != 1) {
    return nullptr;
  }
  if (ifTrue()->getSuccessor(0) != ifFalse()->getSuccessor(0)) {
    return nullptr;
  }

  // The target successor's phis must be redundant. Redundant phis should have
  // been removed in an earlier pass, so only check if any phis are present,
  // which is a stronger condition.
  if (ifTrue()->successorWithPhis()) {
    return nullptr;
  }

  return MGoto::New(alloc, ifTrue());
}

// If a test is dominated by either the true or false path of a previous test of
// the same condition, then the test is redundant and can be converted into a
// goto true or goto false, respectively.
MDefinition* MTest::foldsRedundantTest(TempAllocator& alloc) {
  MBasicBlock* myBlock = this->block();
  MDefinition* originalInput = getOperand(0);

  // Handle single and double negatives. This ensures that we do not miss a
  // folding opportunity due to a condition being inverted.
  MDefinition* newInput = input();
  bool inverted = false;
  if (originalInput->isNot()) {
    newInput = originalInput->toNot()->input();
    inverted = true;
    if (originalInput->toNot()->input()->isNot()) {
      newInput = originalInput->toNot()->input()->toNot()->input();
      inverted = false;
    }
  }

  // The specific order of traversal does not matter. If there are multiple
  // dominating redundant tests, they will either agree on direction (in which
  // case we will prune the same way regardless of order), or they will
  // disagree, in which case we will eventually be marked entirely dead by the
  // folding of the redundant parent.
  for (MUseIterator i(newInput->usesBegin()), e(newInput->usesEnd()); i != e;
       ++i) {
    if (!i->consumer()->isDefinition()) {
      continue;
    }
    if (!i->consumer()->toDefinition()->isTest()) {
      continue;
    }
    MTest* otherTest = i->consumer()->toDefinition()->toTest();
    if (otherTest == this) {
      continue;
    }

    if (otherTest->ifFalse()->dominates(myBlock)) {
      // This test cannot be true, so fold to a goto false.
      return MGoto::New(alloc, inverted ? ifTrue() : ifFalse());
    }
    if (otherTest->ifTrue()->dominates(myBlock)) {
      // This test cannot be false, so fold to a goto true.
      return MGoto::New(alloc, inverted ? ifFalse() : ifTrue());
    }
  }

  return nullptr;
}

MDefinition* MTest::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsRedundantTest(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsDoubleNegation(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsConstant(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsTypes(alloc)) {
    return def;
  }

  if (MDefinition* def = foldsNeedlessControlFlow(alloc)) {
    return def;
  }

  return this;
}

AliasSet MThrow::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MThrowWithStack::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MNewArrayDynamicLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MNewTypedArrayDynamicLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

#ifdef JS_JITSPEW
void MDefinition::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  if (numOperands() > 0) {
    out.printf(" <- ");
  }
  for (size_t j = 0, e = numOperands(); j < e; j++) {
    if (j > 0) {
      out.printf(", ");
    }
    if (getUseFor(j)->hasProducer()) {
      getOperand(j)->printName(out);
    } else {
      out.printf("(null)");
    }
  }
}

void MDefinition::dump(GenericPrinter& out) const {
  printName(out);
  out.printf(":%s", StringFromMIRType(type()));
  out.printf(" = ");
  printOpcode(out);
  out.printf("\n");

  if (isInstruction()) {
    if (MResumePoint* resume = toInstruction()->resumePoint()) {
      resume->dump(out);
    }
  }
}

void MDefinition::dump() const {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}

void MDefinition::dumpLocation(GenericPrinter& out) const {
  MResumePoint* rp = nullptr;
  const char* linkWord = nullptr;
  if (isInstruction() && toInstruction()->resumePoint()) {
    rp = toInstruction()->resumePoint();
    linkWord = "at";
  } else {
    rp = block()->entryResumePoint();
    linkWord = "after";
  }

  while (rp) {
    JSScript* script = rp->block()->info().script();
    uint32_t lineno = PCToLineNumber(rp->block()->info().script(), rp->pc());
    out.printf("  %s %s:%u\n", linkWord, script->filename(), lineno);
    rp = rp->caller();
    linkWord = "in";
  }
}

void MDefinition::dumpLocation() const {
  Fprinter out(stderr);
  dumpLocation(out);
  out.finish();
}
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)
size_t MDefinition::useCount() const {
  size_t count = 0;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    count++;
  }
  return count;
}

size_t MDefinition::defUseCount() const {
  size_t count = 0;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if ((*i)->consumer()->isDefinition()) {
      count++;
    }
  }
  return count;
}
#endif

bool MDefinition::hasOneUse() const {
  MUseIterator i(uses_.begin());
  if (i == uses_.end()) {
    return false;
  }
  i++;
  return i == uses_.end();
}

bool MDefinition::hasOneDefUse() const {
  bool hasOneDefUse = false;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if (!(*i)->consumer()->isDefinition()) {
      continue;
    }

    // We already have a definition use. So 1+
    if (hasOneDefUse) {
      return false;
    }

    // We saw one definition. Loop to test if there is another.
    hasOneDefUse = true;
  }

  return hasOneDefUse;
}

bool MDefinition::hasOneLiveDefUse() const {
  bool hasOneDefUse = false;
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if (!(*i)->consumer()->isDefinition()) {
      continue;
    }

    MDefinition* def = (*i)->consumer()->toDefinition();
    if (def->isRecoveredOnBailout()) {
      continue;
    }

    // We already have a definition use. So 1+
    if (hasOneDefUse) {
      return false;
    }

    // We saw one definition. Loop to test if there is another.
    hasOneDefUse = true;
  }

  return hasOneDefUse;
}

bool MDefinition::hasDefUses() const {
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    if ((*i)->consumer()->isDefinition()) {
      return true;
    }
  }

  return false;
}

bool MDefinition::hasLiveDefUses() const {
  for (MUseIterator i(uses_.begin()); i != uses_.end(); i++) {
    MNode* ins = (*i)->consumer();
    if (ins->isDefinition()) {
      if (!ins->toDefinition()->isRecoveredOnBailout()) {
        return true;
      }
    } else {
      MOZ_ASSERT(ins->isResumePoint());
      if (!ins->toResumePoint()->isRecoverableOperand(*i)) {
        return true;
      }
    }
  }
  return false;
}

MDefinition* MDefinition::maybeSingleDefUse() const {
  MUseDefIterator use(this);
  if (!use) {
    // No def-uses.
    return nullptr;
  }

  MDefinition* useDef = use.def();

  use++;
  if (use) {
    // More than one def-use.
    return nullptr;
  }

  return useDef;
}

MDefinition* MDefinition::maybeMostRecentlyAddedDefUse() const {
  MUseDefIterator use(this);
  if (!use) {
    // No def-uses.
    return nullptr;
  }

  MDefinition* mostRecentUse = use.def();

#ifdef DEBUG
  // This function relies on addUse adding new uses to the front of the list.
  // Check this invariant by asserting the next few uses are 'older'. Skip this
  // for phis because setBackedge can add a new use for a loop phi even if the
  // loop body has a use with an id greater than the loop phi's id.
  if (!mostRecentUse->isPhi()) {
    static constexpr size_t NumUsesToCheck = 3;
    use++;
    for (size_t i = 0; use && i < NumUsesToCheck; i++, use++) {
      MOZ_ASSERT(use.def()->id() <= mostRecentUse->id());
    }
  }
#endif

  return mostRecentUse;
}

void MDefinition::replaceAllUsesWith(MDefinition* dom) {
  for (size_t i = 0, e = numOperands(); i < e; ++i) {
    getOperand(i)->setImplicitlyUsedUnchecked();
  }

  justReplaceAllUsesWith(dom);
}

void MDefinition::justReplaceAllUsesWith(MDefinition* dom) {
  MOZ_ASSERT(dom != nullptr);
  MOZ_ASSERT(dom != this);

  // Carry over the fact the value has uses which are no longer inspectable
  // with the graph.
  if (isImplicitlyUsed()) {
    dom->setImplicitlyUsedUnchecked();
  }

  for (MUseIterator i(usesBegin()), e(usesEnd()); i != e; ++i) {
    i->setProducerUnchecked(dom);
  }
  dom->uses_.takeElements(uses_);
}

bool MDefinition::optimizeOutAllUses(TempAllocator& alloc) {
  for (MUseIterator i(usesBegin()), e(usesEnd()); i != e;) {
    MUse* use = *i++;
    MConstant* constant = use->consumer()->block()->optimizedOutConstant(alloc);
    if (!alloc.ensureBallast()) {
      return false;
    }

    // Update the resume point operand to use the optimized-out constant.
    use->setProducerUnchecked(constant);
    constant->addUseUnchecked(use);
  }

  // Remove dangling pointers.
  this->uses_.clear();
  return true;
}

void MDefinition::replaceAllLiveUsesWith(MDefinition* dom) {
  for (MUseIterator i(usesBegin()), e(usesEnd()); i != e;) {
    MUse* use = *i++;
    MNode* consumer = use->consumer();
    if (consumer->isResumePoint()) {
      continue;
    }
    if (consumer->isDefinition() &&
        consumer->toDefinition()->isRecoveredOnBailout()) {
      continue;
    }

    // Update the operand to use the dominating definition.
    use->replaceProducer(dom);
  }
}

MConstant* MConstant::New(TempAllocator& alloc, const Value& v) {
  return new (alloc) MConstant(alloc, v);
}

MConstant* MConstant::New(TempAllocator::Fallible alloc, const Value& v) {
  return new (alloc) MConstant(alloc.alloc, v);
}

MConstant* MConstant::NewBoolean(TempAllocator& alloc, bool b) {
  return new (alloc) MConstant(b);
}

MConstant* MConstant::NewDouble(TempAllocator& alloc, double d) {
  return new (alloc) MConstant(d);
}

MConstant* MConstant::NewFloat32(TempAllocator& alloc, double d) {
  MOZ_ASSERT(mozilla::IsFloat32Representable(d));
  return new (alloc) MConstant(float(d));
}

MConstant* MConstant::NewInt32(TempAllocator& alloc, int32_t i) {
  return new (alloc) MConstant(i);
}

MConstant* MConstant::NewInt64(TempAllocator& alloc, int64_t i) {
  return new (alloc) MConstant(MIRType::Int64, i);
}

MConstant* MConstant::NewIntPtr(TempAllocator& alloc, intptr_t i) {
  return new (alloc) MConstant(MIRType::IntPtr, i);
}

MConstant* MConstant::NewMagic(TempAllocator& alloc, JSWhyMagic m) {
  return new (alloc) MConstant(alloc, MagicValue(m));
}

MConstant* MConstant::NewNull(TempAllocator& alloc) {
  return new (alloc) MConstant(MIRType::Null);
}

MConstant* MConstant::NewObject(TempAllocator& alloc, JSObject* v) {
  return new (alloc) MConstant(v);
}

MConstant* MConstant::NewShape(TempAllocator& alloc, Shape* s) {
  return new (alloc) MConstant(s);
}

MConstant* MConstant::NewString(TempAllocator& alloc, JSString* s) {
  return new (alloc) MConstant(alloc, StringValue(s));
}

MConstant* MConstant::NewUndefined(TempAllocator& alloc) {
  return new (alloc) MConstant(MIRType::Undefined);
}

static MIRType MIRTypeFromValue(const js::Value& vp) {
  if (vp.isDouble()) {
    return MIRType::Double;
  }
  if (vp.isMagic()) {
    switch (vp.whyMagic()) {
      case JS_OPTIMIZED_OUT:
        return MIRType::MagicOptimizedOut;
      case JS_ELEMENTS_HOLE:
        return MIRType::MagicHole;
      case JS_IS_CONSTRUCTING:
        return MIRType::MagicIsConstructing;
      case JS_UNINITIALIZED_LEXICAL:
        return MIRType::MagicUninitializedLexical;
      default:
        MOZ_ASSERT_UNREACHABLE("Unexpected magic constant");
    }
  }
  return MIRTypeFromValueType(vp.extractNonDoubleType());
}

MConstant::MConstant(TempAllocator& alloc, const js::Value& vp)
    : MNullaryInstruction(classOpcode) {
  setResultType(MIRTypeFromValue(vp));

  MOZ_ASSERT(payload_.asBits == 0);

  switch (type()) {
    case MIRType::Undefined:
    case MIRType::Null:
      break;
    case MIRType::Boolean:
      payload_.b = vp.toBoolean();
      break;
    case MIRType::Int32:
      payload_.i32 = vp.toInt32();
      break;
    case MIRType::Double:
      payload_.d = vp.toDouble();
      break;
    case MIRType::String: {
      JSString* str = vp.toString();
      MOZ_ASSERT(!IsInsideNursery(str));
      payload_.str = &str->asOffThreadAtom();
      break;
    }
    case MIRType::Symbol:
      payload_.sym = vp.toSymbol();
      break;
    case MIRType::BigInt:
      MOZ_ASSERT(!IsInsideNursery(vp.toBigInt()));
      payload_.bi = vp.toBigInt();
      break;
    case MIRType::Object:
      MOZ_ASSERT(!IsInsideNursery(&vp.toObject()));
      payload_.obj = &vp.toObject();
      break;
    case MIRType::MagicOptimizedOut:
    case MIRType::MagicHole:
    case MIRType::MagicIsConstructing:
    case MIRType::MagicUninitializedLexical:
      break;
    default:
      MOZ_CRASH("Unexpected type");
  }

  setMovable();
}

MConstant::MConstant(JSObject* obj) : MConstant(MIRType::Object) {
  MOZ_ASSERT(!IsInsideNursery(obj));
  payload_.obj = obj;
}

MConstant::MConstant(Shape* shape) : MConstant(MIRType::Shape) {
  payload_.shape = shape;
}

#ifdef DEBUG
void MConstant::assertInitializedPayload() const {
  // valueHash() and equals() expect the unused payload bits to be
  // initialized to zero. Assert this in debug builds.

  switch (type()) {
    case MIRType::Int32:
    case MIRType::Float32:
#  if MOZ_LITTLE_ENDIAN()
      MOZ_ASSERT((payload_.asBits >> 32) == 0);
#  else
      MOZ_ASSERT((payload_.asBits << 32) == 0);
#  endif
      break;
    case MIRType::Boolean:
#  if MOZ_LITTLE_ENDIAN()
      MOZ_ASSERT((payload_.asBits >> 1) == 0);
#  else
      MOZ_ASSERT((payload_.asBits & ~(1ULL << 56)) == 0);
#  endif
      break;
    case MIRType::Double:
    case MIRType::Int64:
      break;
    case MIRType::String:
    case MIRType::Object:
    case MIRType::Symbol:
    case MIRType::BigInt:
    case MIRType::IntPtr:
    case MIRType::Shape:
#  if MOZ_LITTLE_ENDIAN()
      MOZ_ASSERT_IF(JS_BITS_PER_WORD == 32, (payload_.asBits >> 32) == 0);
#  else
      MOZ_ASSERT_IF(JS_BITS_PER_WORD == 32, (payload_.asBits << 32) == 0);
#  endif
      break;
    default:
      MOZ_ASSERT(IsNullOrUndefined(type()) || IsMagicType(type()));
      MOZ_ASSERT(payload_.asBits == 0);
      break;
  }
}
#endif

HashNumber MConstant::valueHash() const {
  static_assert(sizeof(Payload) == sizeof(uint64_t),
                "Code below assumes payload fits in 64 bits");

  assertInitializedPayload();
  return ConstantValueHash(type(), payload_.asBits);
}

HashNumber MConstantProto::valueHash() const {
  HashNumber hash = protoObject()->valueHash();
  const MDefinition* receiverObject = getReceiverObject();
  if (receiverObject) {
    hash = addU32ToHash(hash, receiverObject->id());
  }
  return hash;
}

bool MConstant::congruentTo(const MDefinition* ins) const {
  return ins->isConstant() && equals(ins->toConstant());
}

#ifdef JS_JITSPEW
void MConstant::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  out.printf(" ");
  switch (type()) {
    case MIRType::Undefined:
      out.printf("undefined");
      break;
    case MIRType::Null:
      out.printf("null");
      break;
    case MIRType::Boolean:
      out.printf(toBoolean() ? "true" : "false");
      break;
    case MIRType::Int32:
      out.printf("0x%x", uint32_t(toInt32()));
      break;
    case MIRType::Int64:
      out.printf("0x%" PRIx64, uint64_t(toInt64()));
      break;
    case MIRType::IntPtr:
      out.printf("0x%" PRIxPTR, uintptr_t(toIntPtr()));
      break;
    case MIRType::Double:
      out.printf("%.16g", toDouble());
      break;
    case MIRType::Float32: {
      float val = toFloat32();
      out.printf("%.16g", val);
      break;
    }
    case MIRType::Object:
      if (toObject().is<JSFunction>()) {
        JSFunction* fun = &toObject().as<JSFunction>();
        if (fun->maybePartialDisplayAtom()) {
          out.put("function ");
          EscapedStringPrinter(out, fun->maybePartialDisplayAtom(), 0);
        } else {
          out.put("unnamed function");
        }
        if (fun->hasBaseScript()) {
          BaseScript* script = fun->baseScript();
          out.printf(" (%s:%u)", script->filename() ? script->filename() : "",
                     script->lineno());
        }
        out.printf(" at %p", (void*)fun);
        break;
      }
      out.printf("object %p (%s)", (void*)&toObject(),
                 toObject().getClass()->name);
      break;
    case MIRType::Symbol:
      out.printf("symbol at %p", (void*)toSymbol());
      break;
    case MIRType::BigInt:
      out.printf("BigInt at %p", (void*)toBigInt());
      break;
    case MIRType::String:
      out.printf("string %p", (void*)toString());
      break;
    case MIRType::Shape:
      out.printf("shape at %p", (void*)toShape());
      break;
    case MIRType::MagicHole:
      out.printf("magic hole");
      break;
    case MIRType::MagicIsConstructing:
      out.printf("magic is-constructing");
      break;
    case MIRType::MagicOptimizedOut:
      out.printf("magic optimized-out");
      break;
    case MIRType::MagicUninitializedLexical:
      out.printf("magic uninitialized-lexical");
      break;
    default:
      MOZ_CRASH("unexpected type");
  }
}
#endif

bool MConstant::canProduceFloat32() const {
  if (!isTypeRepresentableAsDouble()) {
    return false;
  }

  if (type() == MIRType::Int32) {
    return IsFloat32Representable(static_cast<double>(toInt32()));
  }
  if (type() == MIRType::Double) {
    return IsFloat32Representable(toDouble());
  }
  MOZ_ASSERT(type() == MIRType::Float32);
  return true;
}

Value MConstant::toJSValue() const {
  // Wasm has types like int64 that cannot be stored as js::Value. It also
  // doesn't want the NaN canonicalization enforced by js::Value.
  MOZ_ASSERT(!IsCompilingWasm());

  switch (type()) {
    case MIRType::Undefined:
      return UndefinedValue();
    case MIRType::Null:
      return NullValue();
    case MIRType::Boolean:
      return BooleanValue(toBoolean());
    case MIRType::Int32:
      return Int32Value(toInt32());
    case MIRType::Double:
      return DoubleValue(toDouble());
    case MIRType::Float32:
      return Float32Value(toFloat32());
    case MIRType::String:
      return StringValue(toString()->unwrap());
    case MIRType::Symbol:
      return SymbolValue(toSymbol());
    case MIRType::BigInt:
      return BigIntValue(toBigInt());
    case MIRType::Object:
      return ObjectValue(toObject());
    case MIRType::Shape:
      return PrivateGCThingValue(toShape());
    case MIRType::MagicOptimizedOut:
      return MagicValue(JS_OPTIMIZED_OUT);
    case MIRType::MagicHole:
      return MagicValue(JS_ELEMENTS_HOLE);
    case MIRType::MagicIsConstructing:
      return MagicValue(JS_IS_CONSTRUCTING);
    case MIRType::MagicUninitializedLexical:
      return MagicValue(JS_UNINITIALIZED_LEXICAL);
    default:
      MOZ_CRASH("Unexpected type");
  }
}

bool MConstant::valueToBoolean(bool* res) const {
  switch (type()) {
    case MIRType::Boolean:
      *res = toBoolean();
      return true;
    case MIRType::Int32:
      *res = toInt32() != 0;
      return true;
    case MIRType::Int64:
      *res = toInt64() != 0;
      return true;
    case MIRType::IntPtr:
      *res = toIntPtr() != 0;
      return true;
    case MIRType::Double:
      *res = !std::isnan(toDouble()) && toDouble() != 0.0;
      return true;
    case MIRType::Float32:
      *res = !std::isnan(toFloat32()) && toFloat32() != 0.0f;
      return true;
    case MIRType::Null:
    case MIRType::Undefined:
      *res = false;
      return true;
    case MIRType::Symbol:
      *res = true;
      return true;
    case MIRType::BigInt:
      *res = !toBigInt()->isZero();
      return true;
    case MIRType::String:
      *res = toString()->length() != 0;
      return true;
    case MIRType::Object:
      // TODO(Warp): Lazy groups have been removed.
      // We have to call EmulatesUndefined but that reads obj->group->clasp
      // and so it's racy when the object has a lazy group. The main callers
      // of this (MTest, MNot) already know how to fold the object case, so
      // just give up.
      return false;
    default:
      MOZ_ASSERT(IsMagicType(type()));
      return false;
  }
}

#ifdef JS_JITSPEW
void MControlInstruction::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  if (numSuccessors() > 0) {
    out.printf(" -> ");
  }
  for (size_t j = 0; j < numSuccessors(); j++) {
    if (j > 0) {
      out.printf(", ");
    }
    if (getSuccessor(j)) {
      out.printf("block %u", getSuccessor(j)->id());
    } else {
      out.printf("(null-to-be-patched)");
    }
  }
}

void MCompare::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", CodeName(jsop()));
}

void MTypeOfIs::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", CodeName(jsop()));

  const char* name = "";
  switch (jstype()) {
    case JSTYPE_UNDEFINED:
      name = "undefined";
      break;
    case JSTYPE_OBJECT:
      name = "object";
      break;
    case JSTYPE_FUNCTION:
      name = "function";
      break;
    case JSTYPE_STRING:
      name = "string";
      break;
    case JSTYPE_NUMBER:
      name = "number";
      break;
    case JSTYPE_BOOLEAN:
      name = "boolean";
      break;
    case JSTYPE_SYMBOL:
      name = "symbol";
      break;
    case JSTYPE_BIGINT:
      name = "bigint";
      break;
    case JSTYPE_LIMIT:
      MOZ_CRASH("Unexpected type");
  }
  out.printf(" '%s'", name);
}

void MLoadUnboxedScalar::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", Scalar::name(storageType()));
}

void MLoadDataViewElement::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", Scalar::name(storageType()));
}

void MAssertRange::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.put(" ");
  assertedRange()->dump(out);
}

void MNearbyInt::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  const char* roundingModeStr = nullptr;
  switch (roundingMode_) {
    case RoundingMode::Up:
      roundingModeStr = "(up)";
      break;
    case RoundingMode::Down:
      roundingModeStr = "(down)";
      break;
    case RoundingMode::NearestTiesToEven:
      roundingModeStr = "(nearest ties even)";
      break;
    case RoundingMode::TowardsZero:
      roundingModeStr = "(towards zero)";
      break;
  }
  out.printf(" %s", roundingModeStr);
}
#endif

AliasSet MRandom::getAliasSet() const { return AliasSet::Store(AliasSet::RNG); }

MDefinition* MSign::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (!input->isConstant() ||
      !input->toConstant()->isTypeRepresentableAsDouble()) {
    return this;
  }

  double in = input->toConstant()->numberToDouble();
  double out = js::math_sign_impl(in);

  if (type() == MIRType::Int32) {
    // Decline folding if this is an int32 operation, but the result type
    // isn't an int32.
    int32_t i;
    if (!mozilla::NumberIsInt32(out, &i)) {
      return this;
    }
    return MConstant::NewInt32(alloc, i);
  }

  return MConstant::NewDouble(alloc, out);
}

const char* MMathFunction::FunctionName(UnaryMathFunction function) {
  return GetUnaryMathFunctionName(function);
}

#ifdef JS_JITSPEW
void MMathFunction::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" %s", FunctionName(function()));
}
#endif

MDefinition* MMathFunction::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (!input->isConstant() ||
      !input->toConstant()->isTypeRepresentableAsDouble()) {
    return this;
  }

  UnaryMathFunctionType funPtr = GetUnaryMathFunctionPtr(function());

  double in = input->toConstant()->numberToDouble();

  // The function pointer call can't GC.
  JS::AutoSuppressGCAnalysis nogc;
  double out = funPtr(in);

  if (input->type() == MIRType::Float32) {
    return MConstant::NewFloat32(alloc, out);
  }
  return MConstant::NewDouble(alloc, out);
}

MDefinition* MAtomicIsLockFree::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (!input->isConstant() || input->type() != MIRType::Int32) {
    return this;
  }

  int32_t i = input->toConstant()->toInt32();
  return MConstant::NewBoolean(alloc, AtomicOperations::isLockfreeJS(i));
}

// Define |THIS_SLOT| as part of this translation unit, as it is used to
// specialized the parameterized |New| function calls introduced by
// TRIVIAL_NEW_WRAPPERS.
const int32_t MParameter::THIS_SLOT;

#ifdef JS_JITSPEW
void MParameter::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  if (index() == THIS_SLOT) {
    out.printf(" THIS_SLOT");
  } else {
    out.printf(" %d", index());
  }
}
#endif

HashNumber MParameter::valueHash() const {
  HashNumber hash = MDefinition::valueHash();
  hash = addU32ToHash(hash, index_);
  return hash;
}

bool MParameter::congruentTo(const MDefinition* ins) const {
  if (!ins->isParameter()) {
    return false;
  }

  return ins->toParameter()->index() == index_;
}

WrappedFunction::WrappedFunction(JSFunction* nativeFun, uint16_t nargs,
                                 FunctionFlags flags)
    : nativeFun_(nativeFun), nargs_(nargs), flags_(flags) {
  MOZ_ASSERT_IF(nativeFun, isNativeWithoutJitEntry());

#ifdef DEBUG
  // If we are not running off-main thread we can assert that the
  // metadata is consistent.
  if (!CanUseExtraThreads() && nativeFun) {
    MOZ_ASSERT(nativeFun->nargs() == nargs);

    MOZ_ASSERT(nativeFun->isNativeWithoutJitEntry() ==
               isNativeWithoutJitEntry());
    MOZ_ASSERT(nativeFun->hasJitEntry() == hasJitEntry());
    MOZ_ASSERT(nativeFun->isConstructor() == isConstructor());
    MOZ_ASSERT(nativeFun->isClassConstructor() == isClassConstructor());
  }
#endif
}

MCall* MCall::New(TempAllocator& alloc, WrappedFunction* target, size_t maxArgc,
                  size_t numActualArgs, bool construct, bool ignoresReturnValue,
                  bool isDOMCall, mozilla::Maybe<DOMObjectKind> objectKind,
                  mozilla::Maybe<gc::Heap> initialHeap) {
  MOZ_ASSERT(isDOMCall == objectKind.isSome());
  MOZ_ASSERT(isDOMCall == initialHeap.isSome());

  MOZ_ASSERT(maxArgc >= numActualArgs);
  MCall* ins;
  if (isDOMCall) {
    MOZ_ASSERT(!construct);
    ins = new (alloc)
        MCallDOMNative(target, numActualArgs, *objectKind, *initialHeap);
  } else {
    ins =
        new (alloc) MCall(target, numActualArgs, construct, ignoresReturnValue);
  }
  if (!ins->init(alloc, maxArgc + NumNonArgumentOperands)) {
    return nullptr;
  }
  return ins;
}

AliasSet MCallDOMNative::getAliasSet() const {
  const JSJitInfo* jitInfo = getJitInfo();

  // If we don't know anything about the types of our arguments, we have to
  // assume that type-coercions can have side-effects, so we need to alias
  // everything.
  if (jitInfo->aliasSet() == JSJitInfo::AliasEverything ||
      !jitInfo->isTypedMethodJitInfo()) {
    return AliasSet::Store(AliasSet::Any);
  }

  uint32_t argIndex = 0;
  const JSTypedMethodJitInfo* methodInfo =
      reinterpret_cast<const JSTypedMethodJitInfo*>(jitInfo);
  for (const JSJitInfo::ArgType* argType = methodInfo->argTypes;
       *argType != JSJitInfo::ArgTypeListEnd; ++argType, ++argIndex) {
    if (argIndex >= numActualArgs()) {
      // Passing through undefined can't have side-effects
      continue;
    }
    // getArg(0) is "this", so skip it
    MDefinition* arg = getArg(argIndex + 1);
    MIRType actualType = arg->type();
    // The only way to reliably avoid side-effects given the information we
    // have here is if we're passing in a known primitive value to an
    // argument that expects a primitive value.
    //
    // XXXbz maybe we need to communicate better information.  For example,
    // a sequence argument will sort of unavoidably have side effects, while
    // a typed array argument won't have any, but both are claimed to be
    // JSJitInfo::Object.  But if we do that, we need to watch out for our
    // movability/DCE-ability bits: if we have an arg type that can reliably
    // throw an exception on conversion, that might not affect our alias set
    // per se, but it should prevent us being moved or DCE-ed, unless we
    // know the incoming things match that arg type and won't throw.
    //
    if ((actualType == MIRType::Value || actualType == MIRType::Object) ||
        (*argType & JSJitInfo::Object)) {
      return AliasSet::Store(AliasSet::Any);
    }
  }

  // We checked all the args, and they check out.  So we only alias DOM
  // mutations or alias nothing, depending on the alias set in the jitinfo.
  if (jitInfo->aliasSet() == JSJitInfo::AliasNone) {
    return AliasSet::None();
  }

  MOZ_ASSERT(jitInfo->aliasSet() == JSJitInfo::AliasDOMSets);
  return AliasSet::Load(AliasSet::DOMProperty);
}

void MCallDOMNative::computeMovable() {
  // We are movable if the jitinfo says we can be and if we're also not
  // effectful.  The jitinfo can't check for the latter, since it depends on
  // the types of our arguments.
  const JSJitInfo* jitInfo = getJitInfo();

  MOZ_ASSERT_IF(jitInfo->isMovable,
                jitInfo->aliasSet() != JSJitInfo::AliasEverything);

  if (jitInfo->isMovable && !isEffectful()) {
    setMovable();
  }
}

bool MCallDOMNative::congruentTo(const MDefinition* ins) const {
  if (!isMovable()) {
    return false;
  }

  if (!ins->isCall()) {
    return false;
  }

  const MCall* call = ins->toCall();

  if (!call->isCallDOMNative()) {
    return false;
  }

  if (getSingleTarget() != call->getSingleTarget()) {
    return false;
  }

  if (isConstructing() != call->isConstructing()) {
    return false;
  }

  if (numActualArgs() != call->numActualArgs()) {
    return false;
  }

  if (!congruentIfOperandsEqual(call)) {
    return false;
  }

  // The other call had better be movable at this point!
  MOZ_ASSERT(call->isMovable());

  return true;
}

const JSJitInfo* MCallDOMNative::getJitInfo() const {
  MOZ_ASSERT(getSingleTarget()->hasJitInfo());
  return getSingleTarget()->jitInfo();
}

MCallClassHook* MCallClassHook::New(TempAllocator& alloc, JSNative target,
                                    uint32_t argc, bool constructing) {
  auto* ins = new (alloc) MCallClassHook(target, constructing);

  // Add callee + |this| + (if constructing) newTarget.
  uint32_t numOperands = 2 + argc + constructing;

  if (!ins->init(alloc, numOperands)) {
    return nullptr;
  }

  return ins;
}

MDefinition* MStringLength::foldsTo(TempAllocator& alloc) {
  if (string()->isConstant()) {
    JSOffThreadAtom* str = string()->toConstant()->toString();
    return MConstant::NewInt32(alloc, str->length());
  }

  // MFromCharCode returns a one-element string.
  if (string()->isFromCharCode()) {
    return MConstant::NewInt32(alloc, 1);
  }

  return this;
}

MDefinition* MConcat::foldsTo(TempAllocator& alloc) {
  if (lhs()->isConstant() && lhs()->toConstant()->toString()->empty()) {
    return rhs();
  }

  if (rhs()->isConstant() && rhs()->toConstant()->toString()->empty()) {
    return lhs();
  }

  return this;
}

MDefinition* MStringConvertCase::foldsTo(TempAllocator& alloc) {
  MDefinition* string = this->string();

  // Handle the pattern |str[idx].toUpperCase()| and simplify it from
  // |StringConvertCase(FromCharCode(CharCodeAt(str, idx)))| to just
  // |CharCodeConvertCase(CharCodeAt(str, idx))|.
  if (string->isFromCharCode()) {
    auto* charCode = string->toFromCharCode()->code();
    auto mode = mode_ == Mode::LowerCase ? MCharCodeConvertCase::LowerCase
                                         : MCharCodeConvertCase::UpperCase;
    return MCharCodeConvertCase::New(alloc, charCode, mode);
  }

  // Handle the pattern |num.toString(base).toUpperCase()| and simplify it to
  // directly return the string representation in the correct case.
  if (string->isInt32ToStringWithBase()) {
    auto* toString = string->toInt32ToStringWithBase();

    bool lowerCase = mode_ == Mode::LowerCase;
    if (toString->lowerCase() == lowerCase) {
      return toString;
    }
    return MInt32ToStringWithBase::New(alloc, toString->input(),
                                       toString->base(), lowerCase);
  }

  return this;
}

// Return true if |def| is `MConstant(Int32(0))`.
static bool IsConstantZeroInt32(MDefinition* def) {
  return def->isConstant() && def->toConstant()->isInt32(0);
}

// If |def| is `MBitOr` and one operand is `MConstant(Int32(0))`, then return
// the other operand. Otherwise return |def|.
static MDefinition* RemoveUnnecessaryBitOps(MDefinition* def) {
  if (def->isBitOr()) {
    auto* bitOr = def->toBitOr();
    if (IsConstantZeroInt32(bitOr->lhs())) {
      return bitOr->rhs();
    }
    if (IsConstantZeroInt32(bitOr->rhs())) {
      return bitOr->lhs();
    }
  }
  return def;
}

// Return a match if both operands of |binary| have the requested types. If
// |binary| is commutative, the operands may appear in any order.
template <typename Lhs, typename Rhs>
static mozilla::Maybe<std::pair<Lhs*, Rhs*>> MatchOperands(
    MBinaryInstruction* binary) {
  auto* lhs = binary->lhs();
  auto* rhs = binary->rhs();
  if (lhs->is<Lhs>() && rhs->is<Rhs>()) {
    return mozilla::Some(std::pair{lhs->to<Lhs>(), rhs->to<Rhs>()});
  }
  if (binary->isCommutative() && rhs->is<Lhs>() && lhs->is<Rhs>()) {
    return mozilla::Some(std::pair{rhs->to<Lhs>(), lhs->to<Rhs>()});
  }
  return mozilla::Nothing();
}

static bool IsSubstrTo(MSubstr* substr, int32_t len) {
  // We want to match this pattern:
  //
  // Substr(string, Constant(0), Min(Constant(length), StringLength(string)))
  //
  // which is generated for the self-hosted `String.p.{substring,slice,substr}`
  // functions when called with constants `start` and `end` parameters.

  if (!IsConstantZeroInt32(substr->begin())) {
    return false;
  }

  // Unnecessary bit-ops haven't yet been removed.
  auto* length = RemoveUnnecessaryBitOps(substr->length());
  if (!length->isMinMax() || length->toMinMax()->isMax()) {
    return false;
  }

  auto match = MatchOperands<MConstant, MStringLength>(length->toMinMax());
  if (!match) {
    return false;
  }

  // Ensure |len| matches the substring's length.
  auto [cst, strLength] = *match;
  return cst->isInt32(len) && strLength->string() == substr->string();
}

static bool IsSubstrLast(MSubstr* substr, int32_t start) {
  MOZ_ASSERT(start < 0, "start from end is negative");

  // We want to match either this pattern:
  //
  // begin = Max(StringLength(string) + start, 0)
  // length = Max(StringLength(string) - begin, 0)
  // Substr(string, begin, length)
  //
  // or this pattern:
  //
  // begin = Max(StringLength(string) + start, 0)
  // length = Min(StringLength(string), StringLength(string) - begin)
  // Substr(string, begin, length)
  //
  // which is generated for the self-hosted `String.p.{slice,substr}`
  // functions when called with parameters `start < 0` and `end = undefined`.

  auto* string = substr->string();

  // Unnecessary bit-ops haven't yet been removed.
  auto* begin = RemoveUnnecessaryBitOps(substr->begin());
  auto* length = RemoveUnnecessaryBitOps(substr->length());

  // Matches: Max(StringLength(string) + start, 0)
  auto matchesBegin = [&]() {
    if (!begin->isMinMax() || !begin->toMinMax()->isMax()) {
      return false;
    }

    auto maxOperands = MatchOperands<MAdd, MConstant>(begin->toMinMax());
    if (!maxOperands) {
      return false;
    }

    auto [add, cst] = *maxOperands;
    if (!cst->isInt32(0)) {
      return false;
    }

    auto addOperands = MatchOperands<MStringLength, MConstant>(add);
    if (!addOperands) {
      return false;
    }

    auto [strLength, cstAdd] = *addOperands;
    return strLength->string() == string && cstAdd->isInt32(start);
  };

  // Matches: Max(StringLength(string) - begin, 0)
  auto matchesSliceLength = [&]() {
    if (!length->isMinMax() || !length->toMinMax()->isMax()) {
      return false;
    }

    auto maxOperands = MatchOperands<MSub, MConstant>(length->toMinMax());
    if (!maxOperands) {
      return false;
    }

    auto [sub, cst] = *maxOperands;
    if (!cst->isInt32(0)) {
      return false;
    }

    auto subOperands = MatchOperands<MStringLength, MMinMax>(sub);
    if (!subOperands) {
      return false;
    }

    auto [strLength, minmax] = *subOperands;
    return strLength->string() == string && minmax == begin;
  };

  // Matches: Min(StringLength(string), StringLength(string) - begin)
  auto matchesSubstrLength = [&]() {
    if (!length->isMinMax() || length->toMinMax()->isMax()) {
      return false;
    }

    auto minOperands = MatchOperands<MStringLength, MSub>(length->toMinMax());
    if (!minOperands) {
      return false;
    }

    auto [strLength1, sub] = *minOperands;
    if (strLength1->string() != string) {
      return false;
    }

    auto subOperands = MatchOperands<MStringLength, MMinMax>(sub);
    if (!subOperands) {
      return false;
    }

    auto [strLength2, minmax] = *subOperands;
    return strLength2->string() == string && minmax == begin;
  };

  return matchesBegin() && (matchesSliceLength() || matchesSubstrLength());
}

MDefinition* MSubstr::foldsTo(TempAllocator& alloc) {
  // Fold |str.substring(0, 1)| to |str.charAt(0)|.
  if (IsSubstrTo(this, 1)) {
    MOZ_ASSERT(IsConstantZeroInt32(begin()));

    auto* charCode = MCharCodeAtOrNegative::New(alloc, string(), begin());
    block()->insertBefore(this, charCode);

    return MFromCharCodeEmptyIfNegative::New(alloc, charCode);
  }

  // Fold |str.slice(-1)| and |str.substr(-1)| to |str.charAt(str.length + -1)|.
  if (IsSubstrLast(this, -1)) {
    auto* length = MStringLength::New(alloc, string());
    block()->insertBefore(this, length);

    auto* index = MConstant::NewInt32(alloc, -1);
    block()->insertBefore(this, index);

    // Folded MToRelativeStringIndex, see MToRelativeStringIndex::foldsTo.
    //
    // Safe to truncate because |length| is never negative.
    auto* add = MAdd::New(alloc, index, length, TruncateKind::Truncate);
    block()->insertBefore(this, add);

    auto* charCode = MCharCodeAtOrNegative::New(alloc, string(), add);
    block()->insertBefore(this, charCode);

    return MFromCharCodeEmptyIfNegative::New(alloc, charCode);
  }

  return this;
}

MDefinition* MCharCodeAt::foldsTo(TempAllocator& alloc) {
  MDefinition* string = this->string();
  if (!string->isConstant() && !string->isFromCharCode()) {
    return this;
  }

  MDefinition* index = this->index();
  if (index->isSpectreMaskIndex()) {
    index = index->toSpectreMaskIndex()->index();
  }
  if (!index->isConstant()) {
    return this;
  }
  int32_t idx = index->toConstant()->toInt32();

  // Handle the pattern |s[idx].charCodeAt(0)|.
  if (string->isFromCharCode()) {
    if (idx != 0) {
      return this;
    }

    // Simplify |CharCodeAt(FromCharCode(CharCodeAt(s, idx)), 0)| to just
    // |CharCodeAt(s, idx)|.
    auto* charCode = string->toFromCharCode()->code();
    if (!charCode->isCharCodeAt()) {
      return this;
    }

    return charCode;
  }

  JSOffThreadAtom* str = string->toConstant()->toString();
  if (idx < 0 || uint32_t(idx) >= str->length()) {
    return this;
  }

  char16_t ch = str->latin1OrTwoByteChar(idx);
  return MConstant::NewInt32(alloc, ch);
}

MDefinition* MCodePointAt::foldsTo(TempAllocator& alloc) {
  MDefinition* string = this->string();
  if (!string->isConstant() && !string->isFromCharCode()) {
    return this;
  }

  MDefinition* index = this->index();
  if (index->isSpectreMaskIndex()) {
    index = index->toSpectreMaskIndex()->index();
  }
  if (!index->isConstant()) {
    return this;
  }
  int32_t idx = index->toConstant()->toInt32();

  // Handle the pattern |s[idx].codePointAt(0)|.
  if (string->isFromCharCode()) {
    if (idx != 0) {
      return this;
    }

    // Simplify |CodePointAt(FromCharCode(CharCodeAt(s, idx)), 0)| to just
    // |CharCodeAt(s, idx)|.
    auto* charCode = string->toFromCharCode()->code();
    if (!charCode->isCharCodeAt()) {
      return this;
    }

    return charCode;
  }

  JSOffThreadAtom* str = string->toConstant()->toString();
  if (idx < 0 || uint32_t(idx) >= str->length()) {
    return this;
  }

  char32_t first = str->latin1OrTwoByteChar(idx);
  if (unicode::IsLeadSurrogate(first) && uint32_t(idx) + 1 < str->length()) {
    char32_t second = str->latin1OrTwoByteChar(idx + 1);
    if (unicode::IsTrailSurrogate(second)) {
      first = unicode::UTF16Decode(first, second);
    }
  }
  return MConstant::NewInt32(alloc, first);
}

MDefinition* MToRelativeStringIndex::foldsTo(TempAllocator& alloc) {
  MDefinition* index = this->index();
  MDefinition* length = this->length();

  if (!index->isConstant()) {
    return this;
  }
  if (!length->isStringLength() && !length->isConstant()) {
    return this;
  }
  MOZ_ASSERT_IF(length->isConstant(), length->toConstant()->toInt32() >= 0);

  int32_t relativeIndex = index->toConstant()->toInt32();
  if (relativeIndex >= 0) {
    return index;
  }

  // Safe to truncate because |length| is never negative.
  return MAdd::New(alloc, index, length, TruncateKind::Truncate);
}

template <size_t Arity>
[[nodiscard]] static bool EnsureFloatInputOrConvert(
    MAryInstruction<Arity>* owner, TempAllocator& alloc) {
  MOZ_ASSERT(!IsFloatingPointType(owner->type()),
             "Floating point types must check consumers");

  if (AllOperandsCanProduceFloat32(owner)) {
    return true;
  }
  ConvertOperandsToDouble(owner, alloc);
  return false;
}

template <size_t Arity>
[[nodiscard]] static bool EnsureFloatConsumersAndInputOrConvert(
    MAryInstruction<Arity>* owner, TempAllocator& alloc) {
  MOZ_ASSERT(IsFloatingPointType(owner->type()),
             "Integer types don't need to check consumers");

  if (AllOperandsCanProduceFloat32(owner) &&
      CheckUsesAreFloat32Consumers(owner)) {
    return true;
  }
  ConvertOperandsToDouble(owner, alloc);
  return false;
}

void MFloor::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MCeil::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MRound::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MTrunc::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(type() == MIRType::Int32);
  if (EnsureFloatInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
  }
}

void MNearbyInt::trySpecializeFloat32(TempAllocator& alloc) {
  if (EnsureFloatConsumersAndInputOrConvert(this, alloc)) {
    specialization_ = MIRType::Float32;
    setResultType(MIRType::Float32);
  }
}

MGoto* MGoto::New(TempAllocator& alloc, MBasicBlock* target) {
  return new (alloc) MGoto(target);
}

MGoto* MGoto::New(TempAllocator::Fallible alloc, MBasicBlock* target) {
  MOZ_ASSERT(target);
  return new (alloc) MGoto(target);
}

MGoto* MGoto::New(TempAllocator& alloc) { return new (alloc) MGoto(nullptr); }

MDefinition* MBox::foldsTo(TempAllocator& alloc) {
  if (input()->isUnbox()) {
    return input()->toUnbox()->input();
  }
  return this;
}

#ifdef JS_JITSPEW
void MUnbox::printOpcode(GenericPrinter& out) const {
  PrintOpcodeName(out, op());
  out.printf(" ");
  getOperand(0)->printName(out);
  out.printf(" ");

  switch (type()) {
    case MIRType::Int32:
      out.printf("to Int32");
      break;
    case MIRType::Double:
      out.printf("to Double");
      break;
    case MIRType::Boolean:
      out.printf("to Boolean");
      break;
    case MIRType::String:
      out.printf("to String");
      break;
    case MIRType::Symbol:
      out.printf("to Symbol");
      break;
    case MIRType::BigInt:
      out.printf("to BigInt");
      break;
    case MIRType::Object:
      out.printf("to Object");
      break;
    default:
      break;
  }

  switch (mode()) {
    case Fallible:
      out.printf(" (fallible)");
      break;
    case Infallible:
      out.printf(" (infallible)");
      break;
    default:
      break;
  }
}
#endif

MDefinition* MUnbox::foldsTo(TempAllocator& alloc) {
  if (input()->isBox()) {
    MDefinition* unboxed = input()->toBox()->input();

    // Fold MUnbox(MBox(x)) => x if types match.
    if (unboxed->type() == type()) {
      if (fallible()) {
        unboxed->setImplicitlyUsedUnchecked();
      }
      return unboxed;
    }

    // Fold MUnbox(MBox(x)) => MToDouble(x) if possible.
    if (type() == MIRType::Double &&
        IsTypeRepresentableAsDouble(unboxed->type())) {
      if (unboxed->isConstant()) {
        return MConstant::NewDouble(alloc,
                                    unboxed->toConstant()->numberToDouble());
      }

      return MToDouble::New(alloc, unboxed);
    }

    // MUnbox<Int32>(MBox<Double>(x)) will always fail, even if x can be
    // represented as an Int32. Fold to avoid unnecessary bailouts.
    if (type() == MIRType::Int32 && unboxed->type() == MIRType::Double) {
      auto* folded = MToNumberInt32::New(alloc, unboxed,
                                         IntConversionInputKind::NumbersOnly);
      folded->setGuard();
      return folded;
    }
  }

  return this;
}

#ifdef DEBUG
void MPhi::assertLoopPhi() const {
  // getLoopPredecessorOperand and getLoopBackedgeOperand rely on these
  // predecessors being at known indices.
  if (block()->numPredecessors() == 2) {
    MBasicBlock* pred = block()->getPredecessor(0);
    MBasicBlock* back = block()->getPredecessor(1);
    MOZ_ASSERT(pred == block()->loopPredecessor());
    MOZ_ASSERT(pred->successorWithPhis() == block());
    MOZ_ASSERT(pred->positionInPhiSuccessor() == 0);
    MOZ_ASSERT(back == block()->backedge());
    MOZ_ASSERT(back->successorWithPhis() == block());
    MOZ_ASSERT(back->positionInPhiSuccessor() == 1);
  } else {
    // After we remove fake loop predecessors for loop headers that
    // are only reachable via OSR, the only predecessor is the
    // loop backedge.
    MOZ_ASSERT(block()->numPredecessors() == 1);
    MOZ_ASSERT(block()->graph().osrBlock());
    MOZ_ASSERT(!block()->graph().canBuildDominators());
    MBasicBlock* back = block()->getPredecessor(0);
    MOZ_ASSERT(back == block()->backedge());
    MOZ_ASSERT(back->successorWithPhis() == block());
    MOZ_ASSERT(back->positionInPhiSuccessor() == 0);
  }
}
#endif

MDefinition* MPhi::getLoopPredecessorOperand() const {
  // This should not be called after removing fake loop predecessors.
  MOZ_ASSERT(block()->numPredecessors() == 2);
  assertLoopPhi();
  return getOperand(0);
}

MDefinition* MPhi::getLoopBackedgeOperand() const {
  assertLoopPhi();
  uint32_t idx = block()->numPredecessors() == 2 ? 1 : 0;
  return getOperand(idx);
}

void MPhi::removeOperand(size_t index) {
  MOZ_ASSERT(index < numOperands());
  MOZ_ASSERT(getUseFor(index)->index() == index);
  MOZ_ASSERT(getUseFor(index)->consumer() == this);

  // If we have phi(..., a, b, c, d, ..., z) and we plan
  // on removing a, then first shift downward so that we have
  // phi(..., b, c, d, ..., z, z):
  MUse* p = inputs_.begin() + index;
  MUse* e = inputs_.end();
  p->producer()->removeUse(p);
  for (; p < e - 1; ++p) {
    MDefinition* producer = (p + 1)->producer();
    p->setProducerUnchecked(producer);
    producer->replaceUse(p + 1, p);
  }

  // truncate the inputs_ list:
  inputs_.popBack();
}

void MPhi::removeAllOperands() {
  for (MUse& p : inputs_) {
    p.producer()->removeUse(&p);
  }
  inputs_.clear();
}

MDefinition* MPhi::foldsTernary(TempAllocator& alloc) {
  /* Look if this MPhi is a ternary construct.
   * This is a very loose term as it actually only checks for
   *
   *      MTest X
   *       /  \
   *    ...    ...
   *       \  /
   *     MPhi X Y
   *
   * Which we will simply call:
   * x ? x : y or x ? y : x
   */

  if (numOperands() != 2) {
    return nullptr;
  }

  MOZ_ASSERT(block()->numPredecessors() == 2);

  MBasicBlock* pred = block()->immediateDominator();
  if (!pred || !pred->lastIns()->isTest()) {
    return nullptr;
  }

  MTest* test = pred->lastIns()->toTest();

  // True branch may only dominate one edge of MPhi.
  if (test->ifTrue()->dominates(block()->getPredecessor(0)) ==
      test->ifTrue()->dominates(block()->getPredecessor(1))) {
    return nullptr;
  }

  // False branch may only dominate one edge of MPhi.
  if (test->ifFalse()->dominates(block()->getPredecessor(0)) ==
      test->ifFalse()->dominates(block()->getPredecessor(1))) {
    return nullptr;
  }

  // True and false branch must dominate different edges of MPhi.
  if (test->ifTrue()->dominates(block()->getPredecessor(0)) ==
      test->ifFalse()->dominates(block()->getPredecessor(0))) {
    return nullptr;
  }

  // We found a ternary construct.
  bool firstIsTrueBranch =
      test->ifTrue()->dominates(block()->getPredecessor(0));
  MDefinition* trueDef = firstIsTrueBranch ? getOperand(0) : getOperand(1);
  MDefinition* falseDef = firstIsTrueBranch ? getOperand(1) : getOperand(0);

  // Accept either
  // testArg ? testArg : constant or
  // testArg ? constant : testArg
  if (!trueDef->isConstant() && !falseDef->isConstant()) {
    return nullptr;
  }

  MConstant* c =
      trueDef->isConstant() ? trueDef->toConstant() : falseDef->toConstant();
  MDefinition* testArg = (trueDef == c) ? falseDef : trueDef;
  if (testArg != test->input()) {
    return nullptr;
  }

  // This check should be a tautology, except that the constant might be the
  // result of the removal of a branch.  In such case the domination scope of
  // the block which is holding the constant might be incomplete. This
  // condition is used to prevent doing this optimization based on incomplete
  // information.
  //
  // As GVN removed a branch, it will update the dominations rules before
  // trying to fold this MPhi again. Thus, this condition does not inhibit
  // this optimization.
  MBasicBlock* truePred = block()->getPredecessor(firstIsTrueBranch ? 0 : 1);
  MBasicBlock* falsePred = block()->getPredecessor(firstIsTrueBranch ? 1 : 0);
  if (!trueDef->block()->dominates(truePred) ||
      !falseDef->block()->dominates(falsePred)) {
    return nullptr;
  }

  // If testArg is an int32 type we can:
  // - fold testArg ? testArg : 0 to testArg
  // - fold testArg ? 0 : testArg to 0
  if (testArg->type() == MIRType::Int32 && c->numberToDouble() == 0) {
    testArg->setGuardRangeBailoutsUnchecked();

    // When folding to the constant we need to hoist it.
    if (trueDef == c && !c->block()->dominates(block())) {
      c->block()->moveBefore(pred->lastIns(), c);
    }
    return trueDef;
  }

  // If testArg is an double type we can:
  // - fold testArg ? testArg : 0.0 to MNaNToZero(testArg)
  if (testArg->type() == MIRType::Double &&
      mozilla::IsPositiveZero(c->numberToDouble()) && c != trueDef) {
    MNaNToZero* replace = MNaNToZero::New(alloc, testArg);
    test->block()->insertBefore(test, replace);
    return replace;
  }

  // If testArg is a string type we can:
  // - fold testArg ? testArg : "" to testArg
  // - fold testArg ? "" : testArg to ""
  if (testArg->type() == MIRType::String && c->toString()->empty()) {
    // When folding to the constant we need to hoist it.
    if (trueDef == c && !c->block()->dominates(block())) {
      c->block()->moveBefore(pred->lastIns(), c);
    }
    return trueDef;
  }

  return nullptr;
}

MDefinition* MPhi::operandIfRedundant() {
  if (inputs_.length() == 0) {
    return nullptr;
  }

  // If this phi is redundant (e.g., phi(a,a) or b=phi(a,this)),
  // returns the operand that it will always be equal to (a, in
  // those two cases).
  MDefinition* first = getOperand(0);
  for (size_t i = 1, e = numOperands(); i < e; i++) {
    MDefinition* op = getOperand(i);
    if (op != first && op != this) {
      return nullptr;
    }
  }
  return first;
}

MDefinition* MPhi::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = operandIfRedundant()) {
    return def;
  }

  if (MDefinition* def = foldsTernary(alloc)) {
    return def;
  }

  return this;
}

bool MPhi::congruentTo(const MDefinition* ins) const {
  if (!ins->isPhi()) {
    return false;
  }

  // Phis in different blocks may have different control conditions.
  // For example, these phis:
  //
  //   if (p)
  //     goto a
  //   a:
  //     t = phi(x, y)
  //
  //   if (q)
  //     goto b
  //   b:
  //     s = phi(x, y)
  //
  // have identical operands, but they are not equvalent because t is
  // effectively p?x:y and s is effectively q?x:y.
  //
  // For now, consider phis in different blocks incongruent.
  if (ins->block() != block()) {
    return false;
  }

  return congruentIfOperandsEqual(ins);
}

void MPhi::updateForReplacement(MPhi* other) {
  // This function is called to fix the current Phi flags using it as a
  // replacement of the other Phi instruction |other|.
  //
  // When dealing with usage analysis, any Use will replace all other values,
  // such as Unused and Unknown. Unless both are Unused, the merge would be
  // Unknown.
  if (usageAnalysis_ == PhiUsage::Used ||
      other->usageAnalysis_ == PhiUsage::Used) {
    usageAnalysis_ = PhiUsage::Used;
  } else if (usageAnalysis_ != other->usageAnalysis_) {
    //    this == unused && other == unknown
    // or this == unknown && other == unused
    usageAnalysis_ = PhiUsage::Unknown;
  } else {
    //    this == unused && other == unused
    // or this == unknown && other = unknown
    MOZ_ASSERT(usageAnalysis_ == PhiUsage::Unused ||
               usageAnalysis_ == PhiUsage::Unknown);
    MOZ_ASSERT(usageAnalysis_ == other->usageAnalysis_);
  }
}

/* static */
bool MPhi::markIteratorPhis(const PhiVector& iterators) {
  // Find and mark phis that must transitively hold an iterator live.

  Vector<MPhi*, 8, SystemAllocPolicy> worklist;

  for (MPhi* iter : iterators) {
    if (!iter->isInWorklist()) {
      if (!worklist.append(iter)) {
        return false;
      }
      iter->setInWorklist();
    }
  }

  while (!worklist.empty()) {
    MPhi* phi = worklist.popCopy();
    phi->setNotInWorklist();

    phi->setIterator();
    phi->setImplicitlyUsedUnchecked();

    for (MUseDefIterator iter(phi); iter; iter++) {
      MDefinition* use = iter.def();
      if (!use->isInWorklist() && use->isPhi() && !use->toPhi()->isIterator()) {
        if (!worklist.append(use->toPhi())) {
          return false;
        }
        use->setInWorklist();
      }
    }
  }

  return true;
}

bool MPhi::typeIncludes(MDefinition* def) {
  MOZ_ASSERT(!IsMagicType(def->type()));

  if (def->type() == this->type()) {
    return true;
  }

  // This phi must be able to be any value.
  if (this->type() == MIRType::Value) {
    return true;
  }

  if (def->type() == MIRType::Int32 && this->type() == MIRType::Double) {
    return true;
  }

  return false;
}

void MCallBase::addArg(size_t argnum, MDefinition* arg) {
  // The operand vector is initialized in reverse order by WarpBuilder.
  // It cannot be checked for consistency until all arguments are added.
  // FixedList doesn't initialize its elements, so do an unchecked init.
  initOperand(argnum + NumNonArgumentOperands, arg);
}

static inline bool IsConstant(MDefinition* def, double v) {
  if (!def->isConstant()) {
    return false;
  }

  return NumbersAreIdentical(def->toConstant()->numberToDouble(), v);
}

MDefinition* MBinaryBitwiseInstruction::foldsTo(TempAllocator& alloc) {
  // Identity operations are removed (for int32 only) in foldUnnecessaryBitop.

  if (type() == MIRType::Int32) {
    if (MDefinition* folded = EvaluateConstantOperands(alloc, this)) {
      return folded;
    }
  } else if (type() == MIRType::Int64) {
    if (MDefinition* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      return folded;
    }
  }

  return this;
}

MDefinition* MBinaryBitwiseInstruction::foldUnnecessaryBitop() {
  // It's probably OK to perform this optimization only for int32, as it will
  // have the greatest effect for asm.js code that is compiled with the JS
  // pipeline, and that code will not see int64 values.

  if (type() != MIRType::Int32) {
    return this;
  }

  // Fold unsigned shift right operator when the second operand is zero and
  // the only use is an unsigned modulo. Thus, the expression
  // |(x >>> 0) % y| becomes |x % y|.
  if (isUrsh() && IsUint32Type(this)) {
    MDefinition* defUse = maybeSingleDefUse();
    if (defUse && defUse->isMod() && defUse->toMod()->isUnsigned()) {
      return getOperand(0);
    }
  }

  // Eliminate bitwise operations that are no-ops when used on integer
  // inputs, such as (x | 0).

  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  if (IsConstant(lhs, 0)) {
    return foldIfZero(0);
  }

  if (IsConstant(rhs, 0)) {
    return foldIfZero(1);
  }

  if (IsConstant(lhs, -1)) {
    return foldIfNegOne(0);
  }

  if (IsConstant(rhs, -1)) {
    return foldIfNegOne(1);
  }

  if (lhs == rhs) {
    return foldIfEqual();
  }

  if (maskMatchesRightRange) {
    MOZ_ASSERT(lhs->isConstant());
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    return foldIfAllBitsSet(0);
  }

  if (maskMatchesLeftRange) {
    MOZ_ASSERT(rhs->isConstant());
    MOZ_ASSERT(rhs->type() == MIRType::Int32);
    return foldIfAllBitsSet(1);
  }

  return this;
}

static inline bool CanProduceNegativeZero(MDefinition* def) {
  // Test if this instruction can produce negative zero even when bailing out
  // and changing types.
  switch (def->op()) {
    case MDefinition::Opcode::Constant:
      if (def->type() == MIRType::Double &&
          def->toConstant()->toDouble() == -0.0) {
        return true;
      }
      [[fallthrough]];
    case MDefinition::Opcode::BitAnd:
    case MDefinition::Opcode::BitOr:
    case MDefinition::Opcode::BitXor:
    case MDefinition::Opcode::BitNot:
    case MDefinition::Opcode::Lsh:
    case MDefinition::Opcode::Rsh:
      return false;
    default:
      return true;
  }
}

static inline bool NeedNegativeZeroCheck(MDefinition* def) {
  if (def->isGuard() || def->isGuardRangeBailouts()) {
    return true;
  }

  // Test if all uses have the same semantics for -0 and 0
  for (MUseIterator use = def->usesBegin(); use != def->usesEnd(); use++) {
    if (use->consumer()->isResumePoint()) {
      return true;
    }

    MDefinition* use_def = use->consumer()->toDefinition();
    switch (use_def->op()) {
      case MDefinition::Opcode::Add: {
        // If add is truncating -0 and 0 are observed as the same.
        if (use_def->toAdd()->isTruncated()) {
          break;
        }

        // x + y gives -0, when both x and y are -0

        // Figure out the order in which the addition's operands will
        // execute. EdgeCaseAnalysis::analyzeLate has renumbered the MIR
        // definitions for us so that this just requires comparing ids.
        MDefinition* first = use_def->toAdd()->lhs();
        MDefinition* second = use_def->toAdd()->rhs();
        if (first->id() > second->id()) {
          std::swap(first, second);
        }
        // Negative zero checks can be removed on the first executed
        // operand only if it is guaranteed the second executed operand
        // will produce a value other than -0. While the second is
        // typed as an int32, a bailout taken between execution of the
        // operands may change that type and cause a -0 to flow to the
        // second.
        //
        // There is no way to test whether there are any bailouts
        // between execution of the operands, so remove negative
        // zero checks from the first only if the second's type is
        // independent from type changes that may occur after bailing.
        if (def == first && CanProduceNegativeZero(second)) {
          return true;
        }

        // The negative zero check can always be removed on the second
        // executed operand; by the time this executes the first will have
        // been evaluated as int32 and the addition's result cannot be -0.
        break;
      }
      case MDefinition::Opcode::Sub: {
        // If sub is truncating -0 and 0 are observed as the same
        if (use_def->toSub()->isTruncated()) {
          break;
        }

        // x + y gives -0, when x is -0 and y is 0

        // We can remove the negative zero check on the rhs, only if we
        // are sure the lhs isn't negative zero.

        // The lhs is typed as integer (i.e. not -0.0), but it can bailout
        // and change type. This should be fine if the lhs is executed
        // first. However if the rhs is executed first, the lhs can bail,
        // change type and become -0.0 while the rhs has already been
        // optimized to not make a difference between zero and negative zero.
        MDefinition* lhs = use_def->toSub()->lhs();
        MDefinition* rhs = use_def->toSub()->rhs();
        if (rhs->id() < lhs->id() && CanProduceNegativeZero(lhs)) {
          return true;
        }

        [[fallthrough]];
      }
      case MDefinition::Opcode::StoreElement:
      case MDefinition::Opcode::StoreHoleValueElement:
      case MDefinition::Opcode::LoadElement:
      case MDefinition::Opcode::LoadElementHole:
      case MDefinition::Opcode::LoadUnboxedScalar:
      case MDefinition::Opcode::LoadDataViewElement:
      case MDefinition::Opcode::LoadTypedArrayElementHole:
      case MDefinition::Opcode::CharCodeAt:
      case MDefinition::Opcode::Mod:
      case MDefinition::Opcode::InArray:
        // Only allowed to remove check when definition is the second operand
        if (use_def->getOperand(0) == def) {
          return true;
        }
        for (size_t i = 2, e = use_def->numOperands(); i < e; i++) {
          if (use_def->getOperand(i) == def) {
            return true;
          }
        }
        break;
      case MDefinition::Opcode::BoundsCheck:
        // Only allowed to remove check when definition is the first operand
        if (use_def->toBoundsCheck()->getOperand(1) == def) {
          return true;
        }
        break;
      case MDefinition::Opcode::ToString:
      case MDefinition::Opcode::FromCharCode:
      case MDefinition::Opcode::FromCodePoint:
      case MDefinition::Opcode::TableSwitch:
      case MDefinition::Opcode::Compare:
      case MDefinition::Opcode::BitAnd:
      case MDefinition::Opcode::BitOr:
      case MDefinition::Opcode::BitXor:
      case MDefinition::Opcode::Abs:
      case MDefinition::Opcode::TruncateToInt32:
        // Always allowed to remove check. No matter which operand.
        break;
      case MDefinition::Opcode::StoreElementHole:
      case MDefinition::Opcode::StoreTypedArrayElementHole:
      case MDefinition::Opcode::PostWriteElementBarrier:
        // Only allowed to remove check when definition is the third operand.
        for (size_t i = 0, e = use_def->numOperands(); i < e; i++) {
          if (i == 2) {
            continue;
          }
          if (use_def->getOperand(i) == def) {
            return true;
          }
        }
        break;
      default:
        return true;
    }
  }
  return false;
}

#ifdef JS_JITSPEW
void MBinaryArithInstruction::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);

  switch (type()) {
    case MIRType::Int32:
      if (isDiv()) {
        out.printf(" [%s]", toDiv()->isUnsigned() ? "uint32" : "int32");
      } else if (isMod()) {
        out.printf(" [%s]", toMod()->isUnsigned() ? "uint32" : "int32");
      } else {
        out.printf(" [int32]");
      }
      break;
    case MIRType::Int64:
      if (isDiv()) {
        out.printf(" [%s]", toDiv()->isUnsigned() ? "uint64" : "int64");
      } else if (isMod()) {
        out.printf(" [%s]", toMod()->isUnsigned() ? "uint64" : "int64");
      } else {
        out.printf(" [int64]");
      }
      break;
    case MIRType::Float32:
      out.printf(" [float]");
      break;
    case MIRType::Double:
      out.printf(" [double]");
      break;
    default:
      break;
  }
}
#endif

MDefinition* MRsh::foldsTo(TempAllocator& alloc) {
  MDefinition* f = MBinaryBitwiseInstruction::foldsTo(alloc);

  if (f != this) {
    return f;
  }

  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  // It's probably OK to perform this optimization only for int32, as it will
  // have the greatest effect for asm.js code that is compiled with the JS
  // pipeline, and that code will not see int64 values.

  if (!lhs->isLsh() || !rhs->isConstant() || rhs->type() != MIRType::Int32) {
    return this;
  }

  if (!lhs->getOperand(1)->isConstant() ||
      lhs->getOperand(1)->type() != MIRType::Int32) {
    return this;
  }

  uint32_t shift = rhs->toConstant()->toInt32();
  uint32_t shift_lhs = lhs->getOperand(1)->toConstant()->toInt32();
  if (shift != shift_lhs) {
    return this;
  }

  switch (shift) {
    case 16:
      return MSignExtendInt32::New(alloc, lhs->getOperand(0),
                                   MSignExtendInt32::Half);
    case 24:
      return MSignExtendInt32::New(alloc, lhs->getOperand(0),
                                   MSignExtendInt32::Byte);
  }

  return this;
}

MDefinition* MBinaryArithInstruction::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  MDefinition* lhs = getOperand(0);
  MDefinition* rhs = getOperand(1);

  if (type() == MIRType::Int64) {
    MOZ_ASSERT(!isTruncated());

    if (MConstant* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      if (!folded->block()) {
        block()->insertBefore(this, folded);
      }
      return folded;
    }
    if (isSub() || isDiv() || isMod()) {
      return this;
    }
    if (rhs->isConstant() &&
        rhs->toConstant()->toInt64() == int64_t(getIdentity())) {
      return lhs;
    }
    if (lhs->isConstant() &&
        lhs->toConstant()->toInt64() == int64_t(getIdentity())) {
      return rhs;
    }
    return this;
  }

  if (MConstant* folded = EvaluateConstantOperands(alloc, this)) {
    if (isTruncated()) {
      if (!folded->block()) {
        block()->insertBefore(this, folded);
      }
      if (folded->type() != MIRType::Int32) {
        return MTruncateToInt32::New(alloc, folded);
      }
    }
    return folded;
  }

  if (MConstant* folded = EvaluateConstantNaNOperand(this)) {
    MOZ_ASSERT(!isTruncated());
    return folded;
  }

  if (mustPreserveNaN_) {
    return this;
  }

  // 0 + -0 = 0. So we can't remove addition
  if (isAdd() && type() != MIRType::Int32) {
    return this;
  }

  if (IsConstant(rhs, getIdentity())) {
    if (isTruncated()) {
      return MTruncateToInt32::New(alloc, lhs);
    }
    return lhs;
  }

  // subtraction isn't commutative. So we can't remove subtraction when lhs
  // equals 0
  if (isSub()) {
    return this;
  }

  if (IsConstant(lhs, getIdentity())) {
    if (isTruncated()) {
      return MTruncateToInt32::New(alloc, rhs);
    }
    return rhs;  // id op x => x
  }

  return this;
}

void MBinaryArithInstruction::trySpecializeFloat32(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  // Do not use Float32 if we can use int32.
  if (type() == MIRType::Int32) {
    return;
  }

  if (EnsureFloatConsumersAndInputOrConvert(this, alloc)) {
    setResultType(MIRType::Float32);
  }
}

void MMinMax::trySpecializeFloat32(TempAllocator& alloc) {
  if (type() == MIRType::Int32) {
    return;
  }

  MDefinition* left = lhs();
  MDefinition* right = rhs();

  if ((left->canProduceFloat32() ||
       (left->isMinMax() && left->type() == MIRType::Float32)) &&
      (right->canProduceFloat32() ||
       (right->isMinMax() && right->type() == MIRType::Float32))) {
    setResultType(MIRType::Float32);
  } else {
    ConvertOperandsToDouble(this, alloc);
  }
}

MDefinition* MMinMax::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(lhs()->type() == type());
  MOZ_ASSERT(rhs()->type() == type());

  if (lhs() == rhs()) {
    return lhs();
  }

  auto foldConstants = [&alloc](MDefinition* lhs, MDefinition* rhs,
                                bool isMax) -> MConstant* {
    MOZ_ASSERT(lhs->type() == rhs->type());
    MOZ_ASSERT(lhs->toConstant()->isTypeRepresentableAsDouble());
    MOZ_ASSERT(rhs->toConstant()->isTypeRepresentableAsDouble());

    double lnum = lhs->toConstant()->numberToDouble();
    double rnum = rhs->toConstant()->numberToDouble();

    double result;
    if (isMax) {
      result = js::math_max_impl(lnum, rnum);
    } else {
      result = js::math_min_impl(lnum, rnum);
    }

    // The folded MConstant should maintain the same MIRType with the original
    // inputs.
    if (lhs->type() == MIRType::Int32) {
      int32_t cast;
      if (mozilla::NumberEqualsInt32(result, &cast)) {
        return MConstant::NewInt32(alloc, cast);
      }
      return nullptr;
    }
    if (lhs->type() == MIRType::Float32) {
      return MConstant::NewFloat32(alloc, result);
    }
    MOZ_ASSERT(lhs->type() == MIRType::Double);
    return MConstant::NewDouble(alloc, result);
  };

  // Try to fold the following patterns when |x| and |y| are constants.
  //
  // min(min(x, z), min(y, z)) = min(min(x, y), z)
  // max(max(x, z), max(y, z)) = max(max(x, y), z)
  // max(min(x, z), min(y, z)) = min(max(x, y), z)
  // min(max(x, z), max(y, z)) = max(min(x, y), z)
  if (lhs()->isMinMax() && rhs()->isMinMax()) {
    do {
      auto* left = lhs()->toMinMax();
      auto* right = rhs()->toMinMax();
      if (left->isMax() != right->isMax()) {
        break;
      }

      MDefinition* x;
      MDefinition* y;
      MDefinition* z;
      if (left->lhs() == right->lhs()) {
        std::tie(x, y, z) = std::tuple{left->rhs(), right->rhs(), left->lhs()};
      } else if (left->lhs() == right->rhs()) {
        std::tie(x, y, z) = std::tuple{left->rhs(), right->lhs(), left->lhs()};
      } else if (left->rhs() == right->lhs()) {
        std::tie(x, y, z) = std::tuple{left->lhs(), right->rhs(), left->rhs()};
      } else if (left->rhs() == right->rhs()) {
        std::tie(x, y, z) = std::tuple{left->lhs(), right->lhs(), left->rhs()};
      } else {
        break;
      }

      if (!x->isConstant() || !x->toConstant()->isTypeRepresentableAsDouble() ||
          !y->isConstant() || !y->toConstant()->isTypeRepresentableAsDouble()) {
        break;
      }

      if (auto* folded = foldConstants(x, y, isMax())) {
        block()->insertBefore(this, folded);
        return MMinMax::New(alloc, folded, z, type(), left->isMax());
      }
    } while (false);
  }

  // Fold min/max operations with same inputs.
  if (lhs()->isMinMax() || rhs()->isMinMax()) {
    auto* other = lhs()->isMinMax() ? lhs()->toMinMax() : rhs()->toMinMax();
    auto* operand = lhs()->isMinMax() ? rhs() : lhs();

    if (operand == other->lhs() || operand == other->rhs()) {
      if (isMax() == other->isMax()) {
        // min(x, min(x, y)) = min(x, y)
        // max(x, max(x, y)) = max(x, y)
        return other;
      }
      if (!IsFloatingPointType(type())) {
        // When neither value is NaN:
        // max(x, min(x, y)) = x
        // min(x, max(x, y)) = x

        // Ensure that any bailouts that we depend on to guarantee that |y| is
        // Int32 are not removed.
        auto* otherOp = operand == other->lhs() ? other->rhs() : other->lhs();
        otherOp->setGuardRangeBailoutsUnchecked();

        return operand;
      }
    }
  }

  if (!lhs()->isConstant() && !rhs()->isConstant()) {
    return this;
  }

  // Directly apply math utility to compare the rhs() and lhs() when
  // they are both constants.
  if (lhs()->isConstant() && rhs()->isConstant()) {
    if (!lhs()->toConstant()->isTypeRepresentableAsDouble() ||
        !rhs()->toConstant()->isTypeRepresentableAsDouble()) {
      return this;
    }

    if (auto* folded = foldConstants(lhs(), rhs(), isMax())) {
      return folded;
    }
  }

  MDefinition* operand = lhs()->isConstant() ? rhs() : lhs();
  MConstant* constant =
      lhs()->isConstant() ? lhs()->toConstant() : rhs()->toConstant();

  if (operand->isToDouble() &&
      operand->getOperand(0)->type() == MIRType::Int32) {
    // min(int32, cte >= INT32_MAX) = int32
    if (!isMax() && constant->isTypeRepresentableAsDouble() &&
        constant->numberToDouble() >= INT32_MAX) {
      MLimitedTruncate* limit = MLimitedTruncate::New(
          alloc, operand->getOperand(0), TruncateKind::NoTruncate);
      block()->insertBefore(this, limit);
      MToDouble* toDouble = MToDouble::New(alloc, limit);
      return toDouble;
    }

    // max(int32, cte <= INT32_MIN) = int32
    if (isMax() && constant->isTypeRepresentableAsDouble() &&
        constant->numberToDouble() <= INT32_MIN) {
      MLimitedTruncate* limit = MLimitedTruncate::New(
          alloc, operand->getOperand(0), TruncateKind::NoTruncate);
      block()->insertBefore(this, limit);
      MToDouble* toDouble = MToDouble::New(alloc, limit);
      return toDouble;
    }
  }

  auto foldLength = [](MDefinition* operand, MConstant* constant,
                       bool isMax) -> MDefinition* {
    if ((operand->isArrayLength() || operand->isArrayBufferViewLength() ||
         operand->isArgumentsLength() || operand->isStringLength()) &&
        constant->type() == MIRType::Int32) {
      // (Array|ArrayBufferView|Arguments|String)Length is always >= 0.
      // max(array.length, cte <= 0) = array.length
      // min(array.length, cte <= 0) = cte
      if (constant->toInt32() <= 0) {
        return isMax ? operand : constant;
      }
    }
    return nullptr;
  };

  if (auto* folded = foldLength(operand, constant, isMax())) {
    return folded;
  }

  // Attempt to fold nested min/max operations which are produced by
  // self-hosted built-in functions.
  if (operand->isMinMax()) {
    auto* other = operand->toMinMax();
    MOZ_ASSERT(other->lhs()->type() == type());
    MOZ_ASSERT(other->rhs()->type() == type());

    MConstant* otherConstant = nullptr;
    MDefinition* otherOperand = nullptr;
    if (other->lhs()->isConstant()) {
      otherConstant = other->lhs()->toConstant();
      otherOperand = other->rhs();
    } else if (other->rhs()->isConstant()) {
      otherConstant = other->rhs()->toConstant();
      otherOperand = other->lhs();
    }

    if (otherConstant && constant->isTypeRepresentableAsDouble() &&
        otherConstant->isTypeRepresentableAsDouble()) {
      if (isMax() == other->isMax()) {
        // Fold min(x, min(y, z)) to min(min(x, y), z) with constant min(x, y).
        // Fold max(x, max(y, z)) to max(max(x, y), z) with constant max(x, y).
        if (auto* left = foldConstants(constant, otherConstant, isMax())) {
          block()->insertBefore(this, left);
          return MMinMax::New(alloc, left, otherOperand, type(), isMax());
        }
      } else {
        // Fold min(x, max(y, z)) to max(min(x, y), min(x, z)).
        // Fold max(x, min(y, z)) to min(max(x, y), max(x, z)).
        //
        // But only do this when min(x, z) can also be simplified.
        if (auto* right = foldLength(otherOperand, constant, isMax())) {
          if (auto* left = foldConstants(constant, otherConstant, isMax())) {
            block()->insertBefore(this, left);
            return MMinMax::New(alloc, left, right, type(), !isMax());
          }
        }
      }
    }
  }

  return this;
}

#ifdef JS_JITSPEW
void MMinMax::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (%s)", isMax() ? "max" : "min");
}

void MMinMaxArray::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (%s)", isMax() ? "max" : "min");
}
#endif

MDefinition* MPow::foldsConstant(TempAllocator& alloc) {
  // Both `x` and `p` in `x^p` must be constants in order to precompute.
  if (!input()->isConstant() || !power()->isConstant()) {
    return nullptr;
  }
  if (!power()->toConstant()->isTypeRepresentableAsDouble()) {
    return nullptr;
  }
  if (!input()->toConstant()->isTypeRepresentableAsDouble()) {
    return nullptr;
  }

  double x = input()->toConstant()->numberToDouble();
  double p = power()->toConstant()->numberToDouble();
  double result = js::ecmaPow(x, p);
  if (type() == MIRType::Int32) {
    int32_t cast;
    if (!mozilla::NumberIsInt32(result, &cast)) {
      // Reject folding if the result isn't an int32, because we'll bail anyway.
      return nullptr;
    }
    return MConstant::NewInt32(alloc, cast);
  }
  return MConstant::NewDouble(alloc, result);
}

MDefinition* MPow::foldsConstantPower(TempAllocator& alloc) {
  // If `p` in `x^p` isn't constant, we can't apply these folds.
  if (!power()->isConstant()) {
    return nullptr;
  }
  if (!power()->toConstant()->isTypeRepresentableAsDouble()) {
    return nullptr;
  }

  MOZ_ASSERT(type() == MIRType::Double || type() == MIRType::Int32);

  // NOTE: The optimizations must match the optimizations used in |js::ecmaPow|
  // resp. |js::powi| to avoid differential testing issues.

  double pow = power()->toConstant()->numberToDouble();

  // Math.pow(x, 0.5) is a sqrt with edge-case detection.
  if (pow == 0.5) {
    MOZ_ASSERT(type() == MIRType::Double);
    return MPowHalf::New(alloc, input());
  }

  // Math.pow(x, -0.5) == 1 / Math.pow(x, 0.5), even for edge cases.
  if (pow == -0.5) {
    MOZ_ASSERT(type() == MIRType::Double);
    MPowHalf* half = MPowHalf::New(alloc, input());
    block()->insertBefore(this, half);
    MConstant* one = MConstant::NewDouble(alloc, 1.0);
    block()->insertBefore(this, one);
    return MDiv::New(alloc, one, half, MIRType::Double);
  }

  // Math.pow(x, 1) == x.
  if (pow == 1.0) {
    return input();
  }

  auto multiply = [this, &alloc](MDefinition* lhs, MDefinition* rhs) {
    MMul* mul = MMul::New(alloc, lhs, rhs, type());
    mul->setBailoutKind(bailoutKind());

    // Multiplying the same number can't yield negative zero.
    mul->setCanBeNegativeZero(lhs != rhs && canBeNegativeZero());
    return mul;
  };

  // Math.pow(x, 2) == x*x.
  if (pow == 2.0) {
    return multiply(input(), input());
  }

  // Math.pow(x, 3) == x*x*x.
  if (pow == 3.0) {
    MMul* mul1 = multiply(input(), input());
    block()->insertBefore(this, mul1);
    return multiply(input(), mul1);
  }

  // Math.pow(x, 4) == y*y, where y = x*x.
  if (pow == 4.0) {
    MMul* y = multiply(input(), input());
    block()->insertBefore(this, y);
    return multiply(y, y);
  }

  // Math.pow(x, NaN) == NaN.
  if (std::isnan(pow)) {
    return power();
  }

  // No optimization
  return nullptr;
}

MDefinition* MPow::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsConstant(alloc)) {
    return def;
  }
  if (MDefinition* def = foldsConstantPower(alloc)) {
    return def;
  }
  return this;
}

MDefinition* MBigIntPow::foldsTo(TempAllocator& alloc) {
  auto* base = lhs();
  MOZ_ASSERT(base->type() == MIRType::BigInt);

  auto* power = rhs();
  MOZ_ASSERT(power->type() == MIRType::BigInt);

  // |power| must be a constant.
  if (!power->isConstant()) {
    return this;
  }

  int32_t pow;
  if (BigInt::isInt32(power->toConstant()->toBigInt(), &pow)) {
    // x ** 1n == x.
    if (pow == 1) {
      return base;
    }

    // x ** 2n == x*x.
    if (pow == 2) {
      auto* mul = MBigIntMul::New(alloc, base, base);
      mul->setBailoutKind(bailoutKind());
      return mul;
    }
  }

  // No optimization
  return this;
}

MDefinition* MBigIntAsIntN::foldsTo(TempAllocator& alloc) {
  auto* bitsDef = bits();
  if (!bitsDef->isConstant()) {
    return this;
  }

  // Negative |bits| throw an error and too large |bits| don't fit into Int64.
  int32_t bitsInt = bitsDef->toConstant()->toInt32();
  if (bitsInt < 0 || bitsInt > 64) {
    return this;
  }

  // Prefer sign-extension if possible.
  bool canSignExtend = false;
  switch (bitsInt) {
    case 8:
    case 16:
    case 32:
    case 64:
      canSignExtend = true;
      break;
  }

  // Ensure the input is either IntPtr or Int64 typed.
  auto* inputDef = input();
  if (inputDef->isIntPtrToBigInt()) {
    inputDef = inputDef->toIntPtrToBigInt()->input();

    if (!canSignExtend) {
      auto* int64 = MIntPtrToInt64::New(alloc, inputDef);
      block()->insertBefore(this, int64);
      inputDef = int64;
    }
  } else if (inputDef->isInt64ToBigInt()) {
    inputDef = inputDef->toInt64ToBigInt()->input();
  } else {
    auto* truncate = MTruncateBigIntToInt64::New(alloc, inputDef);
    block()->insertBefore(this, truncate);
    inputDef = truncate;
  }

  if (inputDef->type() == MIRType::IntPtr) {
    MOZ_ASSERT(canSignExtend);

    // If |bits| is larger-or-equal to |BigInt::DigitBits|, return the input.
    if (size_t(bitsInt) >= BigInt::DigitBits) {
      auto* limited = MIntPtrLimitedTruncate::New(alloc, inputDef);
      block()->insertBefore(this, limited);
      inputDef = limited;
    } else {
      MOZ_ASSERT(bitsInt < 64);

      // Otherwise extension is the way to go.
      MSignExtendIntPtr::Mode mode;
      switch (bitsInt) {
        case 8:
          mode = MSignExtendIntPtr::Byte;
          break;
        case 16:
          mode = MSignExtendIntPtr::Half;
          break;
        case 32:
          mode = MSignExtendIntPtr::Word;
          break;
      }

      auto* extend = MSignExtendIntPtr::New(alloc, inputDef, mode);
      block()->insertBefore(this, extend);
      inputDef = extend;
    }

    return MIntPtrToBigInt::New(alloc, inputDef);
  }
  MOZ_ASSERT(inputDef->type() == MIRType::Int64);

  if (canSignExtend) {
    // If |bits| is equal to 64, return the input.
    if (bitsInt == 64) {
      auto* limited = MInt64LimitedTruncate::New(alloc, inputDef);
      block()->insertBefore(this, limited);
      inputDef = limited;
    } else {
      MOZ_ASSERT(bitsInt < 64);

      // Otherwise extension is the way to go.
      MSignExtendInt64::Mode mode;
      switch (bitsInt) {
        case 8:
          mode = MSignExtendInt64::Byte;
          break;
        case 16:
          mode = MSignExtendInt64::Half;
          break;
        case 32:
          mode = MSignExtendInt64::Word;
          break;
      }

      auto* extend = MSignExtendInt64::New(alloc, inputDef, mode);
      block()->insertBefore(this, extend);
      inputDef = extend;
    }
  } else {
    MOZ_ASSERT(bitsInt < 64);

    uint64_t mask = 0;
    if (bitsInt > 0) {
      mask = uint64_t(-1) >> (64 - bitsInt);
    }

    auto* cst = MConstant::NewInt64(alloc, int64_t(mask));
    block()->insertBefore(this, cst);

    // Mask off any excess bits.
    auto* bitAnd = MBitAnd::New(alloc, inputDef, cst, MIRType::Int64);
    block()->insertBefore(this, bitAnd);

    auto* shift = MConstant::NewInt64(alloc, int64_t(64 - bitsInt));
    block()->insertBefore(this, shift);

    // Left-shift to make the sign-bit the left-most bit.
    auto* lsh = MLsh::New(alloc, bitAnd, shift, MIRType::Int64);
    block()->insertBefore(this, lsh);

    // Right-shift to propagate the sign-bit.
    auto* rsh = MRsh::New(alloc, lsh, shift, MIRType::Int64);
    block()->insertBefore(this, rsh);

    inputDef = rsh;
  }

  return MInt64ToBigInt::New(alloc, inputDef, /* isSigned = */ true);
}

MDefinition* MBigIntAsUintN::foldsTo(TempAllocator& alloc) {
  auto* bitsDef = bits();
  if (!bitsDef->isConstant()) {
    return this;
  }

  // Negative |bits| throw an error and too large |bits| don't fit into Int64.
  int32_t bitsInt = bitsDef->toConstant()->toInt32();
  if (bitsInt < 0 || bitsInt > 64) {
    return this;
  }

  // Ensure the input is Int64 typed.
  auto* inputDef = input();
  if (inputDef->isIntPtrToBigInt()) {
    inputDef = inputDef->toIntPtrToBigInt()->input();

    auto* int64 = MIntPtrToInt64::New(alloc, inputDef);
    block()->insertBefore(this, int64);
    inputDef = int64;
  } else if (inputDef->isInt64ToBigInt()) {
    inputDef = inputDef->toInt64ToBigInt()->input();
  } else {
    auto* truncate = MTruncateBigIntToInt64::New(alloc, inputDef);
    block()->insertBefore(this, truncate);
    inputDef = truncate;
  }
  MOZ_ASSERT(inputDef->type() == MIRType::Int64);

  if (bitsInt < 64) {
    uint64_t mask = 0;
    if (bitsInt > 0) {
      mask = uint64_t(-1) >> (64 - bitsInt);
    }

    // Mask off any excess bits.
    auto* cst = MConstant::NewInt64(alloc, int64_t(mask));
    block()->insertBefore(this, cst);

    auto* bitAnd = MBitAnd::New(alloc, inputDef, cst, MIRType::Int64);
    block()->insertBefore(this, bitAnd);

    inputDef = bitAnd;
  }

  return MInt64ToBigInt::New(alloc, inputDef, /* isSigned = */ false);
}

bool MBigIntPtrBinaryArithInstruction::isMaybeZero(MDefinition* ins) {
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);
  if (ins->isBigIntToIntPtr()) {
    ins = ins->toBigIntToIntPtr()->input();
  }
  if (ins->isConstant()) {
    if (ins->type() == MIRType::IntPtr) {
      return ins->toConstant()->toIntPtr() == 0;
    }
    MOZ_ASSERT(ins->type() == MIRType::BigInt);
    return ins->toConstant()->toBigInt()->isZero();
  }
  return true;
}

bool MBigIntPtrBinaryArithInstruction::isMaybeNegative(MDefinition* ins) {
  MOZ_ASSERT(ins->type() == MIRType::IntPtr);
  if (ins->isBigIntToIntPtr()) {
    ins = ins->toBigIntToIntPtr()->input();
  }
  if (ins->isConstant()) {
    if (ins->type() == MIRType::IntPtr) {
      return ins->toConstant()->toIntPtr() < 0;
    }
    MOZ_ASSERT(ins->type() == MIRType::BigInt);
    return ins->toConstant()->toBigInt()->isNegative();
  }
  return true;
}

MDefinition* MInt32ToIntPtr::foldsTo(TempAllocator& alloc) {
  MDefinition* def = input();
  if (def->isConstant()) {
    int32_t i = def->toConstant()->toInt32();
    return MConstant::NewIntPtr(alloc, intptr_t(i));
  }

  if (def->isNonNegativeIntPtrToInt32()) {
    return def->toNonNegativeIntPtrToInt32()->input();
  }

  return this;
}

bool MAbs::fallible() const {
  return !implicitTruncate_ && (!range() || !range()->hasInt32Bounds());
}

void MAbs::trySpecializeFloat32(TempAllocator& alloc) {
  // Do not use Float32 if we can use int32.
  if (input()->type() == MIRType::Int32) {
    return;
  }

  if (EnsureFloatConsumersAndInputOrConvert(this, alloc)) {
    setResultType(MIRType::Float32);
  }
}

MDefinition* MDiv::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  if (type() == MIRType::Int64) {
    if (MDefinition* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      return folded;
    }
    return this;
  }

  if (MDefinition* folded = EvaluateConstantOperands(alloc, this)) {
    return folded;
  }

  if (MDefinition* folded = EvaluateExactReciprocal(alloc, this)) {
    return folded;
  }

  return this;
}

void MDiv::analyzeEdgeCasesForward() {
  // This is only meaningful when doing integer division.
  if (type() != MIRType::Int32) {
    return;
  }

  MOZ_ASSERT(lhs()->type() == MIRType::Int32);
  MOZ_ASSERT(rhs()->type() == MIRType::Int32);

  // Try removing divide by zero check
  if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(0)) {
    canBeDivideByZero_ = false;
  }

  // If lhs is a constant int != INT32_MIN, then
  // negative overflow check can be skipped.
  if (lhs()->isConstant() && !lhs()->toConstant()->isInt32(INT32_MIN)) {
    canBeNegativeOverflow_ = false;
  }

  // If rhs is a constant int != -1, likewise.
  if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(-1)) {
    canBeNegativeOverflow_ = false;
  }

  // If lhs is != 0, then negative zero check can be skipped.
  if (lhs()->isConstant() && !lhs()->toConstant()->isInt32(0)) {
    setCanBeNegativeZero(false);
  }

  // If rhs is >= 0, likewise.
  if (rhs()->isConstant() && rhs()->type() == MIRType::Int32) {
    if (rhs()->toConstant()->toInt32() >= 0) {
      setCanBeNegativeZero(false);
    }
  }
}

void MDiv::analyzeEdgeCasesBackward() {
  // In general, canBeNegativeZero_ is only valid for integer divides.
  // It's fine to access here because we're only using it to avoid
  // wasting effort to decide whether we can clear an already cleared
  // flag.
  if (canBeNegativeZero_ && !NeedNegativeZeroCheck(this)) {
    setCanBeNegativeZero(false);
  }
}

bool MDiv::fallible() const { return !isTruncated(); }

MDefinition* MMod::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(IsNumberType(type()));

  if (type() == MIRType::Int64) {
    if (MDefinition* folded = EvaluateInt64ConstantOperands(alloc, this)) {
      return folded;
    }
  } else {
    if (MDefinition* folded = EvaluateConstantOperands(alloc, this)) {
      return folded;
    }
  }
  return this;
}

void MMod::analyzeEdgeCasesForward() {
  // These optimizations make sense only for integer division
  if (type() != MIRType::Int32) {
    return;
  }

  if (rhs()->isConstant() && !rhs()->toConstant()->isInt32(0)) {
    canBeDivideByZero_ = false;
  }

  if (rhs()->isConstant()) {
    int32_t n = rhs()->toConstant()->toInt32();
    if (n > 0 && !IsPowerOfTwo(uint32_t(n))) {
      canBePowerOfTwoDivisor_ = false;
    }
  }
}

bool MMod::fallible() const {
  return !isTruncated() &&
         (isUnsigned() || canBeDivideByZero() || canBeNegativeDividend());
}

void MMathFunction::trySpecializeFloat32(TempAllocator& alloc) {
  if (EnsureFloatConsumersAndInputOrConvert(this, alloc)) {
    setResultType(MIRType::Float32);
    specialization_ = MIRType::Float32;
  }
}

bool MMathFunction::isFloat32Commutative() const {
  switch (function_) {
    case UnaryMathFunction::Floor:
    case UnaryMathFunction::Ceil:
    case UnaryMathFunction::Round:
    case UnaryMathFunction::Trunc:
      return true;
    default:
      return false;
  }
}

MHypot* MHypot::New(TempAllocator& alloc, const MDefinitionVector& vector) {
  uint32_t length = vector.length();
  MHypot* hypot = new (alloc) MHypot;
  if (!hypot->init(alloc, length)) {
    return nullptr;
  }

  for (uint32_t i = 0; i < length; ++i) {
    hypot->initOperand(i, vector[i]);
  }
  return hypot;
}

bool MAdd::fallible() const {
  // the add is fallible if range analysis does not say that it is finite, AND
  // either the truncation analysis shows that there are non-truncated uses.
  if (truncateKind() >= TruncateKind::IndirectTruncate) {
    return false;
  }
  if (range() && range()->hasInt32Bounds()) {
    return false;
  }
  return true;
}

bool MSub::fallible() const {
  // see comment in MAdd::fallible()
  if (truncateKind() >= TruncateKind::IndirectTruncate) {
    return false;
  }
  if (range() && range()->hasInt32Bounds()) {
    return false;
  }
  return true;
}

MDefinition* MSub::foldsTo(TempAllocator& alloc) {
  MDefinition* out = MBinaryArithInstruction::foldsTo(alloc);
  if (out != this) {
    return out;
  }

  if (type() != MIRType::Int32) {
    return this;
  }

  // Optimize X - X to 0. This optimization is only valid for Int32
  // values. Subtracting a floating point value from itself returns
  // NaN when the operand is either Infinity or NaN.
  if (lhs() == rhs()) {
    // Ensure that any bailouts that we depend on to guarantee that X
    // is Int32 are not removed.
    lhs()->setGuardRangeBailoutsUnchecked();
    return MConstant::NewInt32(alloc, 0);
  }

  return this;
}

MDefinition* MMul::foldsTo(TempAllocator& alloc) {
  MDefinition* out = MBinaryArithInstruction::foldsTo(alloc);
  if (out != this) {
    return out;
  }

  if (type() != MIRType::Int32) {
    return this;
  }

  if (lhs() == rhs()) {
    setCanBeNegativeZero(false);
  }

  return this;
}

void MMul::analyzeEdgeCasesForward() {
  // Try to remove the check for negative zero
  // This only makes sense when using the integer multiplication
  if (type() != MIRType::Int32) {
    return;
  }

  // If lhs is > 0, no need for negative zero check.
  if (lhs()->isConstant() && lhs()->type() == MIRType::Int32) {
    if (lhs()->toConstant()->toInt32() > 0) {
      setCanBeNegativeZero(false);
    }
  }

  // If rhs is > 0, likewise.
  if (rhs()->isConstant() && rhs()->type() == MIRType::Int32) {
    if (rhs()->toConstant()->toInt32() > 0) {
      setCanBeNegativeZero(false);
    }
  }
}

void MMul::analyzeEdgeCasesBackward() {
  if (canBeNegativeZero() && !NeedNegativeZeroCheck(this)) {
    setCanBeNegativeZero(false);
  }
}

bool MMul::canOverflow() const {
  if (isTruncated()) {
    return false;
  }
  return !range() || !range()->hasInt32Bounds();
}

bool MUrsh::fallible() const {
  if (bailoutsDisabled()) {
    return false;
  }
  return !range() || !range()->hasInt32Bounds();
}

static inline bool MustBeUInt32(MDefinition* def, MDefinition** pwrapped) {
  if (def->isUrsh()) {
    *pwrapped = def->toUrsh()->lhs();
    MDefinition* rhs = def->toUrsh()->rhs();
    return def->toUrsh()->bailoutsDisabled() && rhs->maybeConstantValue() &&
           rhs->maybeConstantValue()->isInt32(0);
  }

  if (MConstant* defConst = def->maybeConstantValue()) {
    *pwrapped = defConst;
    return defConst->type() == MIRType::Int32 && defConst->toInt32() >= 0;
  }

  *pwrapped = nullptr;  // silence GCC warning
  return false;
}

/* static */
bool MBinaryInstruction::unsignedOperands(MDefinition* left,
                                          MDefinition* right) {
  MDefinition* replace;
  if (!MustBeUInt32(left, &replace)) {
    return false;
  }
  if (replace->type() != MIRType::Int32) {
    return false;
  }
  if (!MustBeUInt32(right, &replace)) {
    return false;
  }
  if (replace->type() != MIRType::Int32) {
    return false;
  }
  return true;
}

bool MBinaryInstruction::unsignedOperands() {
  return unsignedOperands(getOperand(0), getOperand(1));
}

void MBinaryInstruction::replaceWithUnsignedOperands() {
  MOZ_ASSERT(unsignedOperands());

  for (size_t i = 0; i < numOperands(); i++) {
    MDefinition* replace;
    MustBeUInt32(getOperand(i), &replace);
    if (replace == getOperand(i)) {
      continue;
    }

    getOperand(i)->setImplicitlyUsedUnchecked();
    replaceOperand(i, replace);
  }
}

MDefinition* MBitNot::foldsTo(TempAllocator& alloc) {
  if (type() == MIRType::Int64) {
    return this;
  }
  MOZ_ASSERT(type() == MIRType::Int32);

  MDefinition* input = getOperand(0);

  if (input->isConstant()) {
    int32_t v = ~(input->toConstant()->toInt32());
    return MConstant::NewInt32(alloc, v);
  }

  if (input->isBitNot()) {
    MOZ_ASSERT(input->toBitNot()->type() == MIRType::Int32);
    MOZ_ASSERT(input->toBitNot()->getOperand(0)->type() == MIRType::Int32);
    return MTruncateToInt32::New(alloc,
                                 input->toBitNot()->input());  // ~~x => x | 0
  }

  return this;
}

static void AssertKnownClass(TempAllocator& alloc, MInstruction* ins,
                             MDefinition* obj) {
#ifdef DEBUG
  const JSClass* clasp = GetObjectKnownJSClass(obj);
  MOZ_ASSERT(clasp);

  auto* assert = MAssertClass::New(alloc, obj, clasp);
  ins->block()->insertBefore(ins, assert);
#endif
}

MDefinition* MBoxNonStrictThis::foldsTo(TempAllocator& alloc) {
  MDefinition* in = input();
  if (!in->isBox()) {
    return this;
  }

  MDefinition* unboxed = in->toBox()->input();
  if (unboxed->type() == MIRType::Object) {
    return unboxed;
  }

  return this;
}

AliasSet MLoadArgumentsObjectArg::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

AliasSet MLoadArgumentsObjectArgHole::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

AliasSet MInArgumentsObjectArg::getAliasSet() const {
  // Loads |arguments.length|, but not the actual element, so we can use the
  // same alias-set as MArgumentsObjectLength.
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

AliasSet MArgumentsObjectLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MGuardArgumentsObjectFlags::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardArgumentsObjectFlags() ||
      ins->toGuardArgumentsObjectFlags()->flags() != flags()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardArgumentsObjectFlags::getAliasSet() const {
  // The flags are packed with the length in a fixed private slot.
  return AliasSet::Load(AliasSet::FixedSlot);
}

MDefinition* MIdToStringOrSymbol::foldsTo(TempAllocator& alloc) {
  if (idVal()->isBox()) {
    auto* input = idVal()->toBox()->input();
    MIRType idType = input->type();
    if (idType == MIRType::String || idType == MIRType::Symbol) {
      return idVal();
    }
    if (idType == MIRType::Int32) {
      auto* toString =
          MToString::New(alloc, input, MToString::SideEffectHandling::Bailout);
      block()->insertBefore(this, toString);

      return MBox::New(alloc, toString);
    }
  }

  return this;
}

MDefinition* MReturnFromCtor::foldsTo(TempAllocator& alloc) {
  MDefinition* rval = value();
  if (!rval->isBox()) {
    return this;
  }

  MDefinition* unboxed = rval->toBox()->input();
  if (unboxed->type() == MIRType::Object) {
    return unboxed;
  }

  return object();
}

MDefinition* MTypeOf::foldsTo(TempAllocator& alloc) {
  MDefinition* unboxed = input();
  if (unboxed->isBox()) {
    unboxed = unboxed->toBox()->input();
  }

  JSType type;
  switch (unboxed->type()) {
    case MIRType::Double:
    case MIRType::Float32:
    case MIRType::Int32:
      type = JSTYPE_NUMBER;
      break;
    case MIRType::String:
      type = JSTYPE_STRING;
      break;
    case MIRType::Symbol:
      type = JSTYPE_SYMBOL;
      break;
    case MIRType::BigInt:
      type = JSTYPE_BIGINT;
      break;
    case MIRType::Null:
      type = JSTYPE_OBJECT;
      break;
    case MIRType::Undefined:
      type = JSTYPE_UNDEFINED;
      break;
    case MIRType::Boolean:
      type = JSTYPE_BOOLEAN;
      break;
    case MIRType::Object: {
      KnownClass known = GetObjectKnownClass(unboxed);
      if (known != KnownClass::None) {
        if (known == KnownClass::Function) {
          type = JSTYPE_FUNCTION;
        } else {
          type = JSTYPE_OBJECT;
        }

        AssertKnownClass(alloc, this, unboxed);
        break;
      }
      [[fallthrough]];
    }
    default:
      return this;
  }

  return MConstant::NewInt32(alloc, static_cast<int32_t>(type));
}

MDefinition* MTypeOfName::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(input()->type() == MIRType::Int32);

  if (!input()->isConstant()) {
    return this;
  }

  static_assert(JSTYPE_UNDEFINED == 0);

  int32_t type = input()->toConstant()->toInt32();
  MOZ_ASSERT(JSTYPE_UNDEFINED <= type && type < JSTYPE_LIMIT);

  JSString* name =
      TypeName(static_cast<JSType>(type), GetJitContext()->runtime->names());
  return MConstant::NewString(alloc, name);
}

MUrsh* MUrsh::NewWasm(TempAllocator& alloc, MDefinition* left,
                      MDefinition* right, MIRType type) {
  MUrsh* ins = new (alloc) MUrsh(left, right, type);

  // Since Ion has no UInt32 type, we use Int32 and we have a special
  // exception to the type rules: we can return values in
  // (INT32_MIN,UINT32_MAX] and still claim that we have an Int32 type
  // without bailing out. This is necessary because Ion has no UInt32
  // type and we can't have bailouts in wasm code.
  ins->bailoutsDisabled_ = true;

  return ins;
}

MResumePoint* MResumePoint::New(TempAllocator& alloc, MBasicBlock* block,
                                jsbytecode* pc, ResumeMode mode) {
  MResumePoint* resume = new (alloc) MResumePoint(block, pc, mode);
  if (!resume->init(alloc)) {
    block->discardPreAllocatedResumePoint(resume);
    return nullptr;
  }
  resume->inherit(block);
  return resume;
}

MResumePoint::MResumePoint(MBasicBlock* block, jsbytecode* pc, ResumeMode mode)
    : MNode(block, Kind::ResumePoint),
      pc_(pc),
      instruction_(nullptr),
      mode_(mode) {
  block->addResumePoint(this);
}

bool MResumePoint::init(TempAllocator& alloc) {
  return operands_.init(alloc, block()->stackDepth());
}

MResumePoint* MResumePoint::caller() const {
  return block()->callerResumePoint();
}

void MResumePoint::inherit(MBasicBlock* block) {
  // FixedList doesn't initialize its elements, so do unchecked inits.
  for (size_t i = 0; i < stackDepth(); i++) {
    initOperand(i, block->getSlot(i));
  }
}

void MResumePoint::addStore(TempAllocator& alloc, MDefinition* store,
                            const MResumePoint* cache) {
  MOZ_ASSERT(block()->outerResumePoint() != this);
  MOZ_ASSERT_IF(cache, !cache->stores_.empty());

  if (cache && cache->stores_.begin()->operand == store) {
    // If the last resume point had the same side-effect stack, then we can
    // reuse the current side effect without cloning it. This is a simple
    // way to share common context by making a spaghetti stack.
    if (++cache->stores_.begin() == stores_.begin()) {
      stores_.copy(cache->stores_);
      return;
    }
  }

  // Ensure that the store would not be deleted by DCE.
  MOZ_ASSERT(store->isEffectful());

  MStoreToRecover* top = new (alloc) MStoreToRecover(store);
  stores_.push(top);
}

#ifdef JS_JITSPEW
void MResumePoint::dump(GenericPrinter& out) const {
  out.printf("resumepoint mode=");

  switch (mode()) {
    case ResumeMode::ResumeAt:
      if (instruction_) {
        out.printf("ResumeAt(%u)", instruction_->id());
      } else {
        out.printf("ResumeAt");
      }
      break;
    default:
      out.put(ResumeModeToString(mode()));
      break;
  }

  if (MResumePoint* c = caller()) {
    out.printf(" (caller in block%u)", c->block()->id());
  }

  for (size_t i = 0; i < numOperands(); i++) {
    out.printf(" ");
    if (operands_[i].hasProducer()) {
      getOperand(i)->printName(out);
    } else {
      out.printf("(null)");
    }
  }
  out.printf("\n");
}

void MResumePoint::dump() const {
  Fprinter out(stderr);
  dump(out);
  out.finish();
}
#endif

bool MResumePoint::isObservableOperand(MUse* u) const {
  return isObservableOperand(indexOf(u));
}

bool MResumePoint::isObservableOperand(size_t index) const {
  return block()->info().isObservableSlot(index);
}

bool MResumePoint::isRecoverableOperand(MUse* u) const {
  return block()->info().isRecoverableOperand(indexOf(u));
}

MDefinition* MBigIntToIntPtr::foldsTo(TempAllocator& alloc) {
  MDefinition* def = input();

  // If the operand converts an IntPtr to BigInt, drop both conversions.
  if (def->isIntPtrToBigInt()) {
    return def->toIntPtrToBigInt()->input();
  }

  // Fold this operation if the input operand is constant.
  if (def->isConstant()) {
    BigInt* bigInt = def->toConstant()->toBigInt();
    intptr_t i;
    if (BigInt::isIntPtr(bigInt, &i)) {
      return MConstant::NewIntPtr(alloc, i);
    }
  }

  // Fold BigIntToIntPtr(Int64ToBigInt(int64)) to Int64ToIntPtr(int64)
  if (def->isInt64ToBigInt()) {
    auto* toBigInt = def->toInt64ToBigInt();
    return MInt64ToIntPtr::New(alloc, toBigInt->input(), toBigInt->isSigned());
  }

  return this;
}

MDefinition* MIntPtrToBigInt::foldsTo(TempAllocator& alloc) {
  MDefinition* def = input();

  // If the operand converts a BigInt to IntPtr, drop both conversions.
  if (def->isBigIntToIntPtr()) {
    return def->toBigIntToIntPtr()->input();
  }

  return this;
}

MDefinition* MTruncateBigIntToInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  MOZ_ASSERT(input->type() == MIRType::BigInt);

  // If the operand converts an I64 to BigInt, drop both conversions.
  if (input->isInt64ToBigInt()) {
    return input->toInt64ToBigInt()->input();
  }

  // If the operand is an IntPtr, extend the IntPtr to I64.
  if (input->isIntPtrToBigInt()) {
    auto* intPtr = input->toIntPtrToBigInt()->input();
    if (intPtr->isConstant()) {
      intptr_t c = intPtr->toConstant()->toIntPtr();
      return MConstant::NewInt64(alloc, int64_t(c));
    }
    return MIntPtrToInt64::New(alloc, intPtr);
  }

  // Fold this operation if the input operand is constant.
  if (input->isConstant()) {
    return MConstant::NewInt64(
        alloc, BigInt::toInt64(input->toConstant()->toBigInt()));
  }

  return this;
}

MDefinition* MToInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);

  if (input->isBox()) {
    input = input->getOperand(0);
  }

  // Unwrap MInt64ToBigInt: MToInt64(MInt64ToBigInt(int64)) = int64.
  if (input->isInt64ToBigInt()) {
    return input->getOperand(0);
  }

  // Unwrap IntPtrToBigInt:
  // MToInt64(MIntPtrToBigInt(intptr)) = MIntPtrToInt64(intptr).
  if (input->isIntPtrToBigInt()) {
    auto* intPtr = input->toIntPtrToBigInt()->input();
    if (intPtr->isConstant()) {
      intptr_t c = intPtr->toConstant()->toIntPtr();
      return MConstant::NewInt64(alloc, int64_t(c));
    }
    return MIntPtrToInt64::New(alloc, intPtr);
  }

  // When the input is an Int64 already, just return it.
  if (input->type() == MIRType::Int64) {
    return input;
  }

  // Fold this operation if the input operand is constant.
  if (input->isConstant()) {
    switch (input->type()) {
      case MIRType::Boolean:
        return MConstant::NewInt64(alloc, input->toConstant()->toBoolean());
      default:
        break;
    }
  }

  return this;
}

MDefinition* MToNumberInt32::foldsTo(TempAllocator& alloc) {
  // Fold this operation if the input operand is constant.
  if (MConstant* cst = input()->maybeConstantValue()) {
    switch (cst->type()) {
      case MIRType::Null:
        if (conversion() == IntConversionInputKind::Any) {
          return MConstant::NewInt32(alloc, 0);
        }
        break;
      case MIRType::Boolean:
        if (conversion() == IntConversionInputKind::Any) {
          return MConstant::NewInt32(alloc, cst->toBoolean());
        }
        break;
      case MIRType::Int32:
        return MConstant::NewInt32(alloc, cst->toInt32());
      case MIRType::Float32:
      case MIRType::Double:
        int32_t ival;
        // Only the value within the range of Int32 can be substituted as
        // constant.
        if (mozilla::NumberIsInt32(cst->numberToDouble(), &ival)) {
          return MConstant::NewInt32(alloc, ival);
        }
        break;
      default:
        break;
    }
  }

  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->toBox()->input();
  }

  // Do not fold the TruncateToInt32 node when the input is uint32 (e.g. ursh
  // with a zero constant. Consider the test jit-test/tests/ion/bug1247880.js,
  // where the relevant code is: |(imul(1, x >>> 0) % 2)|. The imul operator
  // is folded to a MTruncateToInt32 node, which will result in this MIR:
  // MMod(MTruncateToInt32(MUrsh(x, MConstant(0))), MConstant(2)). Note that
  // the MUrsh node's type is int32 (since uint32 is not implemented), and
  // that would fold the MTruncateToInt32 node. This will make the modulo
  // unsigned, while is should have been signed.
  if (input->type() == MIRType::Int32 && !IsUint32Type(input)) {
    return input;
  }

  return this;
}

MDefinition* MBooleanToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  MOZ_ASSERT(input->type() == MIRType::Boolean);

  if (input->isConstant()) {
    return MConstant::NewInt32(alloc, input->toConstant()->toBoolean());
  }

  return this;
}

void MToNumberInt32::analyzeEdgeCasesBackward() {
  if (!NeedNegativeZeroCheck(this)) {
    setNeedsNegativeZeroCheck(false);
  }
}

MDefinition* MTruncateToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->getOperand(0);
  }

  // Do not fold the TruncateToInt32 node when the input is uint32 (e.g. ursh
  // with a zero constant. Consider the test jit-test/tests/ion/bug1247880.js,
  // where the relevant code is: |(imul(1, x >>> 0) % 2)|. The imul operator
  // is folded to a MTruncateToInt32 node, which will result in this MIR:
  // MMod(MTruncateToInt32(MUrsh(x, MConstant(0))), MConstant(2)). Note that
  // the MUrsh node's type is int32 (since uint32 is not implemented), and
  // that would fold the MTruncateToInt32 node. This will make the modulo
  // unsigned, while is should have been signed.
  if (input->type() == MIRType::Int32 && !IsUint32Type(input)) {
    return input;
  }

  if (input->type() == MIRType::Double && input->isConstant()) {
    int32_t ret = ToInt32(input->toConstant()->toDouble());
    return MConstant::NewInt32(alloc, ret);
  }

  return this;
}

MDefinition* MWrapInt64ToInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    uint64_t c = input->toConstant()->toInt64();
    int32_t output = bottomHalf() ? int32_t(c) : int32_t(c >> 32);
    return MConstant::NewInt32(alloc, output);
  }

  return this;
}

MDefinition* MExtendInt32ToInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    int32_t c = input->toConstant()->toInt32();
    int64_t res = isUnsigned() ? int64_t(uint32_t(c)) : int64_t(c);
    return MConstant::NewInt64(alloc, res);
  }

  return this;
}

MDefinition* MSignExtendInt32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    int32_t c = input->toConstant()->toInt32();
    int32_t res;
    switch (mode_) {
      case Byte:
        res = int32_t(int8_t(c & 0xFF));
        break;
      case Half:
        res = int32_t(int16_t(c & 0xFFFF));
        break;
    }
    return MConstant::NewInt32(alloc, res);
  }

  return this;
}

MDefinition* MSignExtendInt64::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    int64_t c = input->toConstant()->toInt64();
    int64_t res;
    switch (mode_) {
      case Byte:
        res = int64_t(int8_t(c & 0xFF));
        break;
      case Half:
        res = int64_t(int16_t(c & 0xFFFF));
        break;
      case Word:
        res = int64_t(int32_t(c & 0xFFFFFFFFU));
        break;
    }
    return MConstant::NewInt64(alloc, res);
  }

  return this;
}

MDefinition* MSignExtendIntPtr::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();
  if (input->isConstant()) {
    intptr_t c = input->toConstant()->toIntPtr();
    intptr_t res;
    switch (mode_) {
      case Byte:
        res = intptr_t(int8_t(c & 0xFF));
        break;
      case Half:
        res = intptr_t(int16_t(c & 0xFFFF));
        break;
      case Word:
        res = intptr_t(int32_t(c & 0xFFFFFFFFU));
        break;
    }
    return MConstant::NewIntPtr(alloc, res);
  }

  return this;
}

MDefinition* MToDouble::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->getOperand(0);
  }

  if (input->type() == MIRType::Double) {
    return input;
  }

  if (input->isConstant() &&
      input->toConstant()->isTypeRepresentableAsDouble()) {
    return MConstant::NewDouble(alloc, input->toConstant()->numberToDouble());
  }

  return this;
}

MDefinition* MToFloat32::foldsTo(TempAllocator& alloc) {
  MDefinition* input = getOperand(0);
  if (input->isBox()) {
    input = input->getOperand(0);
  }

  if (input->type() == MIRType::Float32) {
    return input;
  }

  // If x is a Float32, Float32(Double(x)) == x
  if (!mustPreserveNaN_ && input->isToDouble() &&
      input->toToDouble()->input()->type() == MIRType::Float32) {
    return input->toToDouble()->input();
  }

  if (input->isConstant() &&
      input->toConstant()->isTypeRepresentableAsDouble()) {
    return MConstant::NewFloat32(alloc,
                                 float(input->toConstant()->numberToDouble()));
  }

  // Fold ToFloat32(ToDouble(int32)) to ToFloat32(int32).
  if (input->isToDouble() &&
      input->toToDouble()->input()->type() == MIRType::Int32) {
    return MToFloat32::New(alloc, input->toToDouble()->input());
  }

  return this;
}

MDefinition* MToFloat16::foldsTo(TempAllocator& alloc) {
  MDefinition* in = input();
  if (in->isBox()) {
    in = in->toBox()->input();
  }

  if (in->isConstant()) {
    auto* cst = in->toConstant();
    if (cst->isTypeRepresentableAsDouble()) {
      double num = cst->numberToDouble();
      return MConstant::NewFloat32(alloc, static_cast<float>(js::float16{num}));
    }
  }

  auto isFloat16 = [](auto* def) -> MDefinition* {
    // ToFloat16(ToDouble(float16)) => float16
    // ToFloat16(ToFloat32(float16)) => float16
    if (def->isToDouble()) {
      def = def->toToDouble()->input();
    } else if (def->isToFloat32()) {
      def = def->toToFloat32()->input();
    }

    // ToFloat16(ToFloat16(x)) => ToFloat16(x)
    if (def->isToFloat16()) {
      return def;
    }

    // ToFloat16(LoadFloat16(x)) => LoadFloat16(x)
    if (def->isLoadUnboxedScalar() &&
        def->toLoadUnboxedScalar()->storageType() == Scalar::Float16) {
      return def;
    }
    if (def->isLoadDataViewElement() &&
        def->toLoadDataViewElement()->storageType() == Scalar::Float16) {
      return def;
    }
    return nullptr;
  };

  // Fold loads which are guaranteed to return Float16.
  if (auto* f16 = isFloat16(in)) {
    return f16;
  }

  // Fold ToFloat16(ToDouble(float32)) to ToFloat16(float32).
  // Fold ToFloat16(ToDouble(int32)) to ToFloat16(int32).
  if (in->isToDouble()) {
    auto* toDoubleInput = in->toToDouble()->input();
    if (toDoubleInput->type() == MIRType::Float32 ||
        toDoubleInput->type() == MIRType::Int32) {
      return MToFloat16::New(alloc, toDoubleInput);
    }
  }

  return this;
}

MDefinition* MToString::foldsTo(TempAllocator& alloc) {
  MDefinition* in = input();
  if (in->isBox()) {
    in = in->getOperand(0);
  }

  if (in->type() == MIRType::String) {
    return in;
  }
  return this;
}

MDefinition* MClampToUint8::foldsTo(TempAllocator& alloc) {
  if (MConstant* inputConst = input()->maybeConstantValue()) {
    if (inputConst->isTypeRepresentableAsDouble()) {
      int32_t clamped = ClampDoubleToUint8(inputConst->numberToDouble());
      return MConstant::NewInt32(alloc, clamped);
    }
  }
  return this;
}

bool MCompare::tryFoldEqualOperands(bool* result) {
  if (lhs() != rhs()) {
    return false;
  }

  // Intuitively somebody would think that if lhs === rhs,
  // then we can just return true. (Or false for !==)
  // However NaN !== NaN is true! So we spend some time trying
  // to eliminate this case.

  if (!IsEqualityOp(jsop())) {
    return false;
  }

  switch (compareType_) {
    case Compare_Int32:
    case Compare_UInt32:
    case Compare_Int64:
    case Compare_UInt64:
    case Compare_IntPtr:
    case Compare_UIntPtr:
    case Compare_Float32:
    case Compare_Double:
    case Compare_String:
    case Compare_Object:
    case Compare_Symbol:
    case Compare_BigInt:
    case Compare_WasmAnyRef:
    case Compare_Null:
    case Compare_Undefined:
      break;
    case Compare_BigInt_Int32:
    case Compare_BigInt_String:
    case Compare_BigInt_Double:
      MOZ_CRASH("Expecting different operands for lhs and rhs");
  }

  if (isDoubleComparison() || isFloat32Comparison()) {
    if (!operandsAreNeverNaN()) {
      return false;
    }
  } else {
    MOZ_ASSERT(!IsFloatingPointType(lhs()->type()));
  }

  lhs()->setGuardRangeBailoutsUnchecked();

  *result = (jsop() == JSOp::StrictEq || jsop() == JSOp::Eq);
  return true;
}

static JSType TypeOfName(const JSOffThreadAtom* str) {
  static constexpr std::array types = {
      JSTYPE_UNDEFINED, JSTYPE_OBJECT,  JSTYPE_FUNCTION, JSTYPE_STRING,
      JSTYPE_NUMBER,    JSTYPE_BOOLEAN, JSTYPE_SYMBOL,   JSTYPE_BIGINT,
  };
  static_assert(types.size() == JSTYPE_LIMIT);

  const JSAtomState& names = GetJitContext()->runtime->names();
  for (auto type : types) {
    // Both sides are atoms, so we can simply compare pointer identity.
    if (TypeName(type, names) == str->unwrap()) {
      return type;
    }
  }
  return JSTYPE_LIMIT;
}

struct TypeOfCompareInput {
  // The `typeof expr` side of the comparison.
  // MTypeOfName for JSOp::Typeof/JSOp::TypeofExpr, and
  // MTypeOf for JSOp::TypeofEq (same pointer as typeOf).
  MDefinition* typeOfSide;

  // The actual `typeof` operation.
  MTypeOf* typeOf;

  // The string side of the comparison.
  JSType type;

  // True if the comparison uses raw JSType (Generated for JSOp::TypeofEq).
  bool isIntComparison;

  TypeOfCompareInput(MDefinition* typeOfSide, MTypeOf* typeOf, JSType type,
                     bool isIntComparison)
      : typeOfSide(typeOfSide),
        typeOf(typeOf),
        type(type),
        isIntComparison(isIntComparison) {}
};

static mozilla::Maybe<TypeOfCompareInput> IsTypeOfCompare(MCompare* ins) {
  if (!IsEqualityOp(ins->jsop())) {
    return mozilla::Nothing();
  }

  if (ins->compareType() == MCompare::Compare_Int32) {
    auto* lhs = ins->lhs();
    auto* rhs = ins->rhs();

    // NOTE: The comparison is generated inside JIT, and typeof should always
    //       be in the LHS.
    if (!lhs->isTypeOf() || !rhs->isConstant()) {
      return mozilla::Nothing();
    }

    MOZ_ASSERT(ins->type() == MIRType::Boolean);
    MOZ_ASSERT(lhs->type() == MIRType::Int32);
    MOZ_ASSERT(rhs->type() == MIRType::Int32);

    auto* typeOf = lhs->toTypeOf();
    auto* constant = rhs->toConstant();

    JSType type = JSType(constant->toInt32());
    return mozilla::Some(TypeOfCompareInput(typeOf, typeOf, type, true));
  }

  if (ins->compareType() != MCompare::Compare_String) {
    return mozilla::Nothing();
  }

  auto* lhs = ins->lhs();
  auto* rhs = ins->rhs();

  MOZ_ASSERT(ins->type() == MIRType::Boolean);
  MOZ_ASSERT(lhs->type() == MIRType::String);
  MOZ_ASSERT(rhs->type() == MIRType::String);

  if (!lhs->isTypeOfName() && !rhs->isTypeOfName()) {
    return mozilla::Nothing();
  }
  if (!lhs->isConstant() && !rhs->isConstant()) {
    return mozilla::Nothing();
  }

  auto* typeOfName =
      lhs->isTypeOfName() ? lhs->toTypeOfName() : rhs->toTypeOfName();
  auto* typeOf = typeOfName->input()->toTypeOf();

  auto* constant = lhs->isConstant() ? lhs->toConstant() : rhs->toConstant();

  JSType type = TypeOfName(constant->toString());
  return mozilla::Some(TypeOfCompareInput(typeOfName, typeOf, type, false));
}

bool MCompare::tryFoldTypeOf(bool* result) {
  auto typeOfCompare = IsTypeOfCompare(this);
  if (!typeOfCompare) {
    return false;
  }
  auto* typeOf = typeOfCompare->typeOf;
  JSType type = typeOfCompare->type;

  // Can't fold if the input is boxed. (Unless the typeof string is bogus.)
  MIRType inputType = typeOf->input()->type();
  if (inputType == MIRType::Value && type != JSTYPE_LIMIT) {
    return false;
  }

  bool matchesInputType;
  switch (type) {
    case JSTYPE_BOOLEAN:
      matchesInputType = (inputType == MIRType::Boolean);
      break;
    case JSTYPE_NUMBER:
      matchesInputType = IsTypeRepresentableAsDouble(inputType);
      break;
    case JSTYPE_STRING:
      matchesInputType = (inputType == MIRType::String);
      break;
    case JSTYPE_SYMBOL:
      matchesInputType = (inputType == MIRType::Symbol);
      break;
    case JSTYPE_BIGINT:
      matchesInputType = (inputType == MIRType::BigInt);
      break;
    case JSTYPE_OBJECT:
      // Watch out for `object-emulating-undefined` and callable objects.
      if (inputType == MIRType::Object) {
        return false;
      }
      matchesInputType = (inputType == MIRType::Null);
      break;
    case JSTYPE_UNDEFINED:
      // Watch out for `object-emulating-undefined`.
      if (inputType == MIRType::Object) {
        return false;
      }
      matchesInputType = (inputType == MIRType::Undefined);
      break;
    case JSTYPE_FUNCTION:
      // Can't decide at compile-time if an object is callable.
      if (inputType == MIRType::Object) {
        return false;
      }
      matchesInputType = false;
      break;
    case JSTYPE_LIMIT:
      matchesInputType = false;
      break;
  }

  if (matchesInputType) {
    *result = (jsop() == JSOp::StrictEq || jsop() == JSOp::Eq);
  } else {
    *result = (jsop() == JSOp::StrictNe || jsop() == JSOp::Ne);
  }
  return true;
}

bool MCompare::tryFold(bool* result) {
  JSOp op = jsop();

  if (tryFoldEqualOperands(result)) {
    return true;
  }

  if (tryFoldTypeOf(result)) {
    return true;
  }

  if (compareType_ == Compare_Null || compareType_ == Compare_Undefined) {
    // The LHS is the value we want to test against null or undefined.
    if (IsStrictEqualityOp(op)) {
      MIRType expectedType =
          compareType_ == Compare_Null ? MIRType::Null : MIRType::Undefined;
      if (lhs()->type() == expectedType) {
        *result = (op == JSOp::StrictEq);
        return true;
      }
      if (lhs()->type() != MIRType::Value) {
        *result = (op == JSOp::StrictNe);
        return true;
      }
    } else {
      MOZ_ASSERT(IsLooseEqualityOp(op));
      if (IsNullOrUndefined(lhs()->type())) {
        *result = (op == JSOp::Eq);
        return true;
      }
      if (lhs()->type() != MIRType::Object && lhs()->type() != MIRType::Value) {
        *result = (op == JSOp::Ne);
        return true;
      }
    }
    return false;
  }

  return false;
}

template <typename T>
static bool FoldComparison(JSOp op, T left, T right) {
  switch (op) {
    case JSOp::Lt:
      return left < right;
    case JSOp::Le:
      return left <= right;
    case JSOp::Gt:
      return left > right;
    case JSOp::Ge:
      return left >= right;
    case JSOp::StrictEq:
    case JSOp::Eq:
      return left == right;
    case JSOp::StrictNe:
    case JSOp::Ne:
      return left != right;
    default:
      MOZ_CRASH("Unexpected op.");
  }
}

static bool FoldBigIntComparison(JSOp op, const BigInt* left, double right) {
  switch (op) {
    case JSOp::Lt:
      return BigInt::lessThan(left, right).valueOr(false);
    case JSOp::Le:
      return !BigInt::lessThan(right, left).valueOr(true);
    case JSOp::Gt:
      return BigInt::lessThan(right, left).valueOr(false);
    case JSOp::Ge:
      return !BigInt::lessThan(left, right).valueOr(true);
    case JSOp::StrictEq:
    case JSOp::Eq:
      return BigInt::equal(left, right);
    case JSOp::StrictNe:
    case JSOp::Ne:
      return !BigInt::equal(left, right);
    default:
      MOZ_CRASH("Unexpected op.");
  }
}

bool MCompare::evaluateConstantOperands(TempAllocator& alloc, bool* result) {
  if (type() != MIRType::Boolean && type() != MIRType::Int32) {
    return false;
  }

  MDefinition* left = getOperand(0);
  MDefinition* right = getOperand(1);

  if (compareType() == Compare_Double) {
    // Optimize "MCompare MConstant (MToDouble SomethingInInt32Range).
    // In most cases the MToDouble was added, because the constant is
    // a double.
    // e.g. v < 9007199254740991, where v is an int32 is always true.
    if (!lhs()->isConstant() && !rhs()->isConstant()) {
      return false;
    }

    MDefinition* operand = left->isConstant() ? right : left;
    MConstant* constant =
        left->isConstant() ? left->toConstant() : right->toConstant();
    MOZ_ASSERT(constant->type() == MIRType::Double);
    double cte = constant->toDouble();

    if (operand->isToDouble() &&
        operand->getOperand(0)->type() == MIRType::Int32) {
      bool replaced = false;
      switch (jsop_) {
        case JSOp::Lt:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = !((constant == lhs()) ^ (cte < INT32_MIN));
            replaced = true;
          }
          break;
        case JSOp::Le:
          if (constant == lhs()) {
            if (cte > INT32_MAX || cte <= INT32_MIN) {
              *result = (cte <= INT32_MIN);
              replaced = true;
            }
          } else {
            if (cte >= INT32_MAX || cte < INT32_MIN) {
              *result = (cte >= INT32_MIN);
              replaced = true;
            }
          }
          break;
        case JSOp::Gt:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = !((constant == rhs()) ^ (cte < INT32_MIN));
            replaced = true;
          }
          break;
        case JSOp::Ge:
          if (constant == lhs()) {
            if (cte >= INT32_MAX || cte < INT32_MIN) {
              *result = (cte >= INT32_MAX);
              replaced = true;
            }
          } else {
            if (cte > INT32_MAX || cte <= INT32_MIN) {
              *result = (cte <= INT32_MIN);
              replaced = true;
            }
          }
          break;
        case JSOp::StrictEq:  // Fall through.
        case JSOp::Eq:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = false;
            replaced = true;
          }
          break;
        case JSOp::StrictNe:  // Fall through.
        case JSOp::Ne:
          if (cte > INT32_MAX || cte < INT32_MIN) {
            *result = true;
            replaced = true;
          }
          break;
        default:
          MOZ_CRASH("Unexpected op.");
      }
      if (replaced) {
        MLimitedTruncate* limit = MLimitedTruncate::New(
            alloc, operand->getOperand(0), TruncateKind::NoTruncate);
        limit->setGuardUnchecked();
        block()->insertBefore(this, limit);
        return true;
      }
    }

    // Optimize comparison against NaN.
    if (std::isnan(cte)) {
      switch (jsop_) {
        case JSOp::Lt:
        case JSOp::Le:
        case JSOp::Gt:
        case JSOp::Ge:
        case JSOp::Eq:
        case JSOp::StrictEq:
          *result = false;
          break;
        case JSOp::Ne:
        case JSOp::StrictNe:
          *result = true;
          break;
        default:
          MOZ_CRASH("Unexpected op.");
      }
      return true;
    }
  }

  if (!left->isConstant() || !right->isConstant()) {
    return false;
  }

  MConstant* lhs = left->toConstant();
  MConstant* rhs = right->toConstant();

  switch (compareType()) {
    case Compare_Int32:
    case Compare_Double:
    case Compare_Float32: {
      *result =
          FoldComparison(jsop_, lhs->numberToDouble(), rhs->numberToDouble());
      return true;
    }
    case Compare_UInt32: {
      *result = FoldComparison(jsop_, uint32_t(lhs->toInt32()),
                               uint32_t(rhs->toInt32()));
      return true;
    }
    case Compare_Int64: {
      *result = FoldComparison(jsop_, lhs->toInt64(), rhs->toInt64());
      return true;
    }
    case Compare_UInt64: {
      *result = FoldComparison(jsop_, uint64_t(lhs->toInt64()),
                               uint64_t(rhs->toInt64()));
      return true;
    }
    case Compare_IntPtr: {
      *result = FoldComparison(jsop_, lhs->toIntPtr(), rhs->toIntPtr());
      return true;
    }
    case Compare_UIntPtr: {
      *result = FoldComparison(jsop_, uintptr_t(lhs->toIntPtr()),
                               uintptr_t(rhs->toIntPtr()));
      return true;
    }
    case Compare_String: {
      int32_t comp = CompareStrings(lhs->toString(), rhs->toString());
      *result = FoldComparison(jsop_, comp, 0);
      return true;
    }
    case Compare_BigInt: {
      int32_t comp = BigInt::compare(lhs->toBigInt(), rhs->toBigInt());
      *result = FoldComparison(jsop_, comp, 0);
      return true;
    }
    case Compare_BigInt_Int32:
    case Compare_BigInt_Double: {
      *result =
          FoldBigIntComparison(jsop_, lhs->toBigInt(), rhs->numberToDouble());
      return true;
    }
    case Compare_BigInt_String: {
      JSOffThreadAtom* str = rhs->toString();
      if (!str->hasIndexValue()) {
        return false;
      }
      *result =
          FoldBigIntComparison(jsop_, lhs->toBigInt(), str->getIndexValue());
      return true;
    }

    case Compare_Undefined:
    case Compare_Null:
    case Compare_Symbol:
    case Compare_Object:
    case Compare_WasmAnyRef:
      return false;
  }

  MOZ_CRASH("unexpected compare type");
}

MDefinition* MCompare::tryFoldTypeOf(TempAllocator& alloc) {
  auto typeOfCompare = IsTypeOfCompare(this);
  if (!typeOfCompare) {
    return this;
  }
  auto* typeOf = typeOfCompare->typeOf;
  JSType type = typeOfCompare->type;

  auto* input = typeOf->input();
  MOZ_ASSERT(input->type() == MIRType::Value ||
             input->type() == MIRType::Object);

  // Constant typeof folding handles the other cases.
  MOZ_ASSERT_IF(input->type() == MIRType::Object, type == JSTYPE_UNDEFINED ||
                                                      type == JSTYPE_OBJECT ||
                                                      type == JSTYPE_FUNCTION);

  MOZ_ASSERT(type != JSTYPE_LIMIT, "unknown typeof strings folded earlier");

  // If there's only a single use, assume this |typeof| is used in a simple
  // comparison context.
  //
  // if (typeof thing === "number") { ... }
  //
  // It'll be compiled into something similar to:
  //
  // if (IsNumber(thing)) { ... }
  //
  // This heuristic can go wrong when repeated |typeof| are used in consecutive
  // if-statements.
  //
  // if (typeof thing === "number") { ... }
  // else if (typeof thing === "string") { ... }
  // ... repeated for all possible types
  //
  // In that case it'd more efficient to emit MTypeOf compared to MTypeOfIs. We
  // don't yet handle that case, because it'd require a separate optimization
  // pass to correctly detect it.
  if (typeOfCompare->typeOfSide->hasOneUse()) {
    return MTypeOfIs::New(alloc, input, jsop(), type);
  }

  if (typeOfCompare->isIntComparison) {
    // Already optimized.
    return this;
  }

  MConstant* cst = MConstant::NewInt32(alloc, type);
  block()->insertBefore(this, cst);

  return MCompare::New(alloc, typeOf, cst, jsop(), MCompare::Compare_Int32);
}

MDefinition* MCompare::tryFoldCharCompare(TempAllocator& alloc) {
  if (compareType() != Compare_String) {
    return this;
  }

  MDefinition* left = lhs();
  MOZ_ASSERT(left->type() == MIRType::String);

  MDefinition* right = rhs();
  MOZ_ASSERT(right->type() == MIRType::String);

  // |str[i]| is compiled as |MFromCharCode(MCharCodeAt(str, i))|.
  // Out-of-bounds access is compiled as
  // |FromCharCodeEmptyIfNegative(CharCodeAtOrNegative(str, i))|.
  auto isCharAccess = [](MDefinition* ins) {
    if (ins->isFromCharCode()) {
      return ins->toFromCharCode()->code()->isCharCodeAt();
    }
    if (ins->isFromCharCodeEmptyIfNegative()) {
      auto* fromCharCode = ins->toFromCharCodeEmptyIfNegative();
      return fromCharCode->code()->isCharCodeAtOrNegative();
    }
    return false;
  };

  auto charAccessCode = [](MDefinition* ins) {
    if (ins->isFromCharCode()) {
      return ins->toFromCharCode()->code();
    }
    return ins->toFromCharCodeEmptyIfNegative()->code();
  };

  if (left->isConstant() || right->isConstant()) {
    // Try to optimize |MConstant(string) <compare> (MFromCharCode MCharCodeAt)|
    // as |MConstant(charcode) <compare> MCharCodeAt|.
    MConstant* constant;
    MDefinition* operand;
    if (left->isConstant()) {
      constant = left->toConstant();
      operand = right;
    } else {
      constant = right->toConstant();
      operand = left;
    }

    if (constant->toString()->length() != 1 || !isCharAccess(operand)) {
      return this;
    }

    char16_t charCode = constant->toString()->latin1OrTwoByteChar(0);
    MConstant* charCodeConst = MConstant::NewInt32(alloc, charCode);
    block()->insertBefore(this, charCodeConst);

    MDefinition* charCodeAt = charAccessCode(operand);

    if (left->isConstant()) {
      left = charCodeConst;
      right = charCodeAt;
    } else {
      left = charCodeAt;
      right = charCodeConst;
    }
  } else if (isCharAccess(left) && isCharAccess(right)) {
    // Try to optimize |(MFromCharCode MCharCodeAt) <compare> (MFromCharCode
    // MCharCodeAt)| as |MCharCodeAt <compare> MCharCodeAt|.

    left = charAccessCode(left);
    right = charAccessCode(right);
  } else {
    return this;
  }

  return MCompare::New(alloc, left, right, jsop(), MCompare::Compare_Int32);
}

MDefinition* MCompare::tryFoldStringCompare(TempAllocator& alloc) {
  if (compareType() != Compare_String) {
    return this;
  }

  MDefinition* left = lhs();
  MOZ_ASSERT(left->type() == MIRType::String);

  MDefinition* right = rhs();
  MOZ_ASSERT(right->type() == MIRType::String);

  if (!left->isConstant() && !right->isConstant()) {
    return this;
  }

  // Try to optimize |string <compare> MConstant("")| as |MStringLength(string)
  // <compare> MConstant(0)|.

  MConstant* constant =
      left->isConstant() ? left->toConstant() : right->toConstant();
  if (!constant->toString()->empty()) {
    return this;
  }

  MDefinition* operand = left->isConstant() ? right : left;

  auto* strLength = MStringLength::New(alloc, operand);
  block()->insertBefore(this, strLength);

  auto* zero = MConstant::NewInt32(alloc, 0);
  block()->insertBefore(this, zero);

  if (left->isConstant()) {
    left = zero;
    right = strLength;
  } else {
    left = strLength;
    right = zero;
  }

  return MCompare::New(alloc, left, right, jsop(), MCompare::Compare_Int32);
}

MDefinition* MCompare::tryFoldStringSubstring(TempAllocator& alloc) {
  if (compareType() != Compare_String) {
    return this;
  }
  if (!IsEqualityOp(jsop())) {
    return this;
  }

  auto* left = lhs();
  MOZ_ASSERT(left->type() == MIRType::String);

  auto* right = rhs();
  MOZ_ASSERT(right->type() == MIRType::String);

  // One operand must be a constant string.
  if (!left->isConstant() && !right->isConstant()) {
    return this;
  }

  // The constant string must be non-empty.
  auto* constant =
      left->isConstant() ? left->toConstant() : right->toConstant();
  if (constant->toString()->empty()) {
    return this;
  }

  // The other operand must be a substring operation.
  auto* operand = left->isConstant() ? right : left;
  if (!operand->isSubstr()) {
    return this;
  }
  auto* substr = operand->toSubstr();

  static_assert(JSString::MAX_LENGTH < INT32_MAX,
                "string length can be casted to int32_t");

  int32_t stringLength = int32_t(constant->toString()->length());

  MInstruction* replacement;
  if (IsSubstrTo(substr, stringLength)) {
    // Fold |str.substring(0, 2) == "aa"| to |str.startsWith("aa")|.
    replacement = MStringStartsWith::New(alloc, substr->string(), constant);
  } else if (IsSubstrLast(substr, -stringLength)) {
    // Fold |str.slice(-2) == "aa"| to |str.endsWith("aa")|.
    replacement = MStringEndsWith::New(alloc, substr->string(), constant);
  } else {
    return this;
  }

  if (jsop() == JSOp::Eq || jsop() == JSOp::StrictEq) {
    return replacement;
  }

  // Invert for inequality.
  MOZ_ASSERT(jsop() == JSOp::Ne || jsop() == JSOp::StrictNe);

  block()->insertBefore(this, replacement);
  return MNot::New(alloc, replacement);
}

MDefinition* MCompare::tryFoldStringIndexOf(TempAllocator& alloc) {
  if (compareType() != Compare_Int32) {
    return this;
  }
  if (!IsEqualityOp(jsop())) {
    return this;
  }

  auto* left = lhs();
  MOZ_ASSERT(left->type() == MIRType::Int32);

  auto* right = rhs();
  MOZ_ASSERT(right->type() == MIRType::Int32);

  // One operand must be a constant integer.
  if (!left->isConstant() && !right->isConstant()) {
    return this;
  }

  // The constant must be zero.
  auto* constant =
      left->isConstant() ? left->toConstant() : right->toConstant();
  if (!constant->isInt32(0)) {
    return this;
  }

  // The other operand must be an indexOf operation.
  auto* operand = left->isConstant() ? right : left;
  if (!operand->isStringIndexOf()) {
    return this;
  }

  // Fold |str.indexOf(searchStr) == 0| to |str.startsWith(searchStr)|.

  auto* indexOf = operand->toStringIndexOf();
  auto* startsWith =
      MStringStartsWith::New(alloc, indexOf->string(), indexOf->searchString());
  if (jsop() == JSOp::Eq || jsop() == JSOp::StrictEq) {
    return startsWith;
  }

  // Invert for inequality.
  MOZ_ASSERT(jsop() == JSOp::Ne || jsop() == JSOp::StrictNe);

  block()->insertBefore(this, startsWith);
  return MNot::New(alloc, startsWith);
}

MDefinition* MCompare::tryFoldBigInt64(TempAllocator& alloc) {
  if (compareType() == Compare_BigInt) {
    auto* left = lhs();
    MOZ_ASSERT(left->type() == MIRType::BigInt);

    auto* right = rhs();
    MOZ_ASSERT(right->type() == MIRType::BigInt);

    // At least one operand must be MInt64ToBigInt.
    if (!left->isInt64ToBigInt() && !right->isInt64ToBigInt()) {
      return this;
    }

    // Unwrap MInt64ToBigInt on both sides and perform a Int64 comparison.
    if (left->isInt64ToBigInt() && right->isInt64ToBigInt()) {
      auto* lhsInt64 = left->toInt64ToBigInt();
      auto* rhsInt64 = right->toInt64ToBigInt();

      // Don't optimize if Int64 against Uint64 comparison.
      if (lhsInt64->isSigned() != rhsInt64->isSigned()) {
        return this;
      }

      bool isSigned = lhsInt64->isSigned();
      auto compareType =
          isSigned ? MCompare::Compare_Int64 : MCompare::Compare_UInt64;
      return MCompare::New(alloc, lhsInt64->input(), rhsInt64->input(), jsop_,
                           compareType);
    }

    // Optimize IntPtr x Int64 comparison to Int64 x Int64 comparison.
    if (left->isIntPtrToBigInt() || right->isIntPtrToBigInt()) {
      auto* int64ToBigInt = left->isInt64ToBigInt() ? left->toInt64ToBigInt()
                                                    : right->toInt64ToBigInt();

      // Can't optimize when comparing Uint64 against IntPtr.
      if (!int64ToBigInt->isSigned()) {
        return this;
      }

      auto* intPtrToBigInt = left->isIntPtrToBigInt()
                                 ? left->toIntPtrToBigInt()
                                 : right->toIntPtrToBigInt();

      auto* intPtrToInt64 = MIntPtrToInt64::New(alloc, intPtrToBigInt->input());
      block()->insertBefore(this, intPtrToInt64);

      if (left == int64ToBigInt) {
        left = int64ToBigInt->input();
        right = intPtrToInt64;
      } else {
        left = intPtrToInt64;
        right = int64ToBigInt->input();
      }
      return MCompare::New(alloc, left, right, jsop_, MCompare::Compare_Int64);
    }

    // The other operand must be a constant.
    if (!left->isConstant() && !right->isConstant()) {
      return this;
    }

    auto* int64ToBigInt = left->isInt64ToBigInt() ? left->toInt64ToBigInt()
                                                  : right->toInt64ToBigInt();
    bool isSigned = int64ToBigInt->isSigned();

    auto* constant =
        left->isConstant() ? left->toConstant() : right->toConstant();
    auto* bigInt = constant->toBigInt();

    // Extract the BigInt value if representable as Int64/Uint64.
    mozilla::Maybe<int64_t> value;
    if (isSigned) {
      int64_t x;
      if (BigInt::isInt64(bigInt, &x)) {
        value = mozilla::Some(x);
      }
    } else {
      uint64_t x;
      if (BigInt::isUint64(bigInt, &x)) {
        value = mozilla::Some(static_cast<int64_t>(x));
      }
    }

    // The comparison is a constant if the BigInt has too many digits.
    if (!value) {
      int32_t repr = bigInt->isNegative() ? -1 : 1;

      bool result;
      if (left == int64ToBigInt) {
        result = FoldComparison(jsop_, 0, repr);
      } else {
        result = FoldComparison(jsop_, repr, 0);
      }
      return MConstant::NewBoolean(alloc, result);
    }

    auto* cst = MConstant::NewInt64(alloc, *value);
    block()->insertBefore(this, cst);

    auto compareType =
        isSigned ? MCompare::Compare_Int64 : MCompare::Compare_UInt64;
    if (left == int64ToBigInt) {
      return MCompare::New(alloc, int64ToBigInt->input(), cst, jsop_,
                           compareType);
    }
    return MCompare::New(alloc, cst, int64ToBigInt->input(), jsop_,
                         compareType);
  }

  if (compareType() == Compare_BigInt_Int32) {
    auto* left = lhs();
    MOZ_ASSERT(left->type() == MIRType::BigInt);

    auto* right = rhs();
    MOZ_ASSERT(right->type() == MIRType::Int32);

    // Optimize MInt64ToBigInt against a constant int32.
    if (!left->isInt64ToBigInt() || !right->isConstant()) {
      return this;
    }

    auto* int64ToBigInt = left->toInt64ToBigInt();
    bool isSigned = int64ToBigInt->isSigned();

    int32_t constInt32 = right->toConstant()->toInt32();

    // The unsigned comparison against a negative operand is a constant.
    if (!isSigned && constInt32 < 0) {
      bool result = FoldComparison(jsop_, 0, constInt32);
      return MConstant::NewBoolean(alloc, result);
    }

    auto* cst = MConstant::NewInt64(alloc, int64_t(constInt32));
    block()->insertBefore(this, cst);

    auto compareType =
        isSigned ? MCompare::Compare_Int64 : MCompare::Compare_UInt64;
    return MCompare::New(alloc, int64ToBigInt->input(), cst, jsop_,
                         compareType);
  }

  return this;
}

MDefinition* MCompare::tryFoldBigIntPtr(TempAllocator& alloc) {
  if (compareType() == Compare_BigInt) {
    auto* left = lhs();
    MOZ_ASSERT(left->type() == MIRType::BigInt);

    auto* right = rhs();
    MOZ_ASSERT(right->type() == MIRType::BigInt);

    // At least one operand must be MIntPtrToBigInt.
    if (!left->isIntPtrToBigInt() && !right->isIntPtrToBigInt()) {
      return this;
    }

    // Unwrap MIntPtrToBigInt on both sides and perform an IntPtr comparison.
    if (left->isIntPtrToBigInt() && right->isIntPtrToBigInt()) {
      auto* lhsIntPtr = left->toIntPtrToBigInt();
      auto* rhsIntPtr = right->toIntPtrToBigInt();

      return MCompare::New(alloc, lhsIntPtr->input(), rhsIntPtr->input(), jsop_,
                           MCompare::Compare_IntPtr);
    }

    // The other operand must be a constant.
    if (!left->isConstant() && !right->isConstant()) {
      return this;
    }

    auto* intPtrToBigInt = left->isIntPtrToBigInt() ? left->toIntPtrToBigInt()
                                                    : right->toIntPtrToBigInt();

    auto* constant =
        left->isConstant() ? left->toConstant() : right->toConstant();
    auto* bigInt = constant->toBigInt();

    // Extract the BigInt value if representable as intptr_t.
    intptr_t value;
    if (!BigInt::isIntPtr(bigInt, &value)) {
      // The comparison is a constant if the BigInt has too many digits.
      int32_t repr = bigInt->isNegative() ? -1 : 1;

      bool result;
      if (left == intPtrToBigInt) {
        result = FoldComparison(jsop_, 0, repr);
      } else {
        result = FoldComparison(jsop_, repr, 0);
      }
      return MConstant::NewBoolean(alloc, result);
    }

    auto* cst = MConstant::NewIntPtr(alloc, value);
    block()->insertBefore(this, cst);

    if (left == intPtrToBigInt) {
      left = intPtrToBigInt->input();
      right = cst;
    } else {
      left = cst;
      right = intPtrToBigInt->input();
    }
    return MCompare::New(alloc, left, right, jsop_, MCompare::Compare_IntPtr);
  }

  if (compareType() == Compare_BigInt_Int32) {
    auto* left = lhs();
    MOZ_ASSERT(left->type() == MIRType::BigInt);

    auto* right = rhs();
    MOZ_ASSERT(right->type() == MIRType::Int32);

    // Optimize MIntPtrToBigInt against a constant int32.
    if (!left->isIntPtrToBigInt() || !right->isConstant()) {
      return this;
    }

    auto* cst =
        MConstant::NewIntPtr(alloc, intptr_t(right->toConstant()->toInt32()));
    block()->insertBefore(this, cst);

    return MCompare::New(alloc, left->toIntPtrToBigInt()->input(), cst, jsop_,
                         MCompare::Compare_IntPtr);
  }

  return this;
}

MDefinition* MCompare::tryFoldBigInt(TempAllocator& alloc) {
  if (compareType() != Compare_BigInt) {
    return this;
  }

  auto* left = lhs();
  MOZ_ASSERT(left->type() == MIRType::BigInt);

  auto* right = rhs();
  MOZ_ASSERT(right->type() == MIRType::BigInt);

  // One operand must be a constant.
  if (!left->isConstant() && !right->isConstant()) {
    return this;
  }

  auto* constant =
      left->isConstant() ? left->toConstant() : right->toConstant();
  auto* operand = left->isConstant() ? right : left;

  // The constant must be representable as an Int32.
  int32_t x;
  if (!BigInt::isInt32(constant->toBigInt(), &x)) {
    return this;
  }

  MConstant* int32Const = MConstant::NewInt32(alloc, x);
  block()->insertBefore(this, int32Const);

  auto op = jsop();
  if (IsStrictEqualityOp(op)) {
    // Compare_BigInt_Int32 is only valid for loose comparison.
    op = op == JSOp::StrictEq ? JSOp::Eq : JSOp::Ne;
  } else if (operand == right) {
    // Reverse the comparison operator if the operands were reordered.
    op = ReverseCompareOp(op);
  }

  return MCompare::New(alloc, operand, int32Const, op,
                       MCompare::Compare_BigInt_Int32);
}

MDefinition* MCompare::foldsTo(TempAllocator& alloc) {
  bool result;

  if (tryFold(&result) || evaluateConstantOperands(alloc, &result)) {
    if (type() == MIRType::Int32) {
      return MConstant::NewInt32(alloc, result);
    }

    MOZ_ASSERT(type() == MIRType::Boolean);
    return MConstant::NewBoolean(alloc, result);
  }

  if (MDefinition* folded = tryFoldTypeOf(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldCharCompare(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldStringCompare(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldStringSubstring(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldStringIndexOf(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldBigInt64(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldBigIntPtr(alloc); folded != this) {
    return folded;
  }

  if (MDefinition* folded = tryFoldBigInt(alloc); folded != this) {
    return folded;
  }

  return this;
}

void MCompare::trySpecializeFloat32(TempAllocator& alloc) {
  if (AllOperandsCanProduceFloat32(this) && compareType_ == Compare_Double) {
    compareType_ = Compare_Float32;
  } else {
    ConvertOperandsToDouble(this, alloc);
  }
}

MDefinition* MSameValue::foldsTo(TempAllocator& alloc) {
  MDefinition* lhs = left();
  if (lhs->isBox()) {
    lhs = lhs->toBox()->input();
  }

  MDefinition* rhs = right();
  if (rhs->isBox()) {
    rhs = rhs->toBox()->input();
  }

  // Trivially true if both operands are the same.
  if (lhs == rhs) {
    return MConstant::NewBoolean(alloc, true);
  }

  // CacheIR optimizes the following cases, so don't bother to handle them here:
  // 1. Both inputs are numbers (int32 or double).
  // 2. Both inputs are strictly different types.
  // 3. Both inputs are the same type.

  // Optimize when one operand is guaranteed to be |null|.
  if (lhs->type() == MIRType::Null || rhs->type() == MIRType::Null) {
    // The `null` value must be the right-hand side operand.
    auto* input = lhs->type() == MIRType::Null ? rhs : lhs;
    auto* cst = lhs->type() == MIRType::Null ? lhs : rhs;
    return MCompare::New(alloc, input, cst, JSOp::StrictEq,
                         MCompare::Compare_Null);
  }

  // Optimize when one operand is guaranteed to be |undefined|.
  if (lhs->type() == MIRType::Undefined || rhs->type() == MIRType::Undefined) {
    // The `undefined` value must be the right-hand side operand.
    auto* input = lhs->type() == MIRType::Undefined ? rhs : lhs;
    auto* cst = lhs->type() == MIRType::Undefined ? lhs : rhs;
    return MCompare::New(alloc, input, cst, JSOp::StrictEq,
                         MCompare::Compare_Undefined);
  }

  return this;
}

MDefinition* MSameValueDouble::foldsTo(TempAllocator& alloc) {
  // Trivially true if both operands are the same.
  if (left() == right()) {
    return MConstant::NewBoolean(alloc, true);
  }

  // At least one operand must be a constant.
  if (!left()->isConstant() && !right()->isConstant()) {
    return this;
  }

  auto* input = left()->isConstant() ? right() : left();
  auto* cst = left()->isConstant() ? left() : right();
  double dbl = cst->toConstant()->toDouble();

  // Use bitwise comparison for +/-0.
  if (dbl == 0.0) {
    auto* reinterp = MReinterpretCast::New(alloc, input, MIRType::Int64);
    block()->insertBefore(this, reinterp);

    auto* zeroBitsCst =
        MConstant::NewInt64(alloc, mozilla::BitwiseCast<int64_t>(dbl));
    block()->insertBefore(this, zeroBitsCst);

    return MCompare::New(alloc, reinterp, zeroBitsCst, JSOp::StrictEq,
                         MCompare::Compare_Int64);
  }

  // Fold `Object.is(d, NaN)` to `d !== d`.
  if (std::isnan(dbl)) {
    return MCompare::New(alloc, input, input, JSOp::StrictNe,
                         MCompare::Compare_Double);
  }

  // Otherwise fold to MCompare.
  return MCompare::New(alloc, left(), right(), JSOp::StrictEq,
                       MCompare::Compare_Double);
}

MDefinition* MNot::foldsTo(TempAllocator& alloc) {
  auto foldConstant = [&alloc](MDefinition* input, MIRType type) -> MConstant* {
    MConstant* inputConst = input->maybeConstantValue();
    if (!inputConst) {
      return nullptr;
    }
    bool b;
    if (!inputConst->valueToBoolean(&b)) {
      return nullptr;
    }
    if (type == MIRType::Int32) {
      return MConstant::NewInt32(alloc, !b);
    }
    MOZ_ASSERT(type == MIRType::Boolean);
    return MConstant::NewBoolean(alloc, !b);
  };

  // Fold if the input is constant.
  if (MConstant* folded = foldConstant(input(), type())) {
    return folded;
  }

  // If the operand of the Not is itself a Not, they cancel out. But we can't
  // always convert Not(Not(x)) to x because that may loose the conversion to
  // boolean. We can simplify Not(Not(Not(x))) to Not(x) though.
  MDefinition* op = getOperand(0);
  if (op->isNot()) {
    MDefinition* opop = op->getOperand(0);
    if (opop->isNot()) {
      return opop;
    }
  }

  // Not of an undefined or null value is always true
  if (input()->type() == MIRType::Undefined ||
      input()->type() == MIRType::Null) {
    return MConstant::NewBoolean(alloc, true);
  }

  // Not of a symbol is always false.
  if (input()->type() == MIRType::Symbol) {
    return MConstant::NewBoolean(alloc, false);
  }

  // Drop the conversion in `Not(Int64ToBigInt(int64))` to `Not(int64)`.
  if (input()->isInt64ToBigInt()) {
    MDefinition* int64 = input()->toInt64ToBigInt()->input();
    if (MConstant* folded = foldConstant(int64, type())) {
      return folded;
    }
    return MNot::New(alloc, int64);
  }

  // Drop the conversion in `Not(IntPtrToBigInt(intptr))` to `Not(intptr)`.
  if (input()->isIntPtrToBigInt()) {
    MDefinition* intPtr = input()->toIntPtrToBigInt()->input();
    if (MConstant* folded = foldConstant(intPtr, type())) {
      return folded;
    }
    return MNot::New(alloc, intPtr);
  }

  return this;
}

void MNot::trySpecializeFloat32(TempAllocator& alloc) {
  (void)EnsureFloatInputOrConvert(this, alloc);
}

#ifdef JS_JITSPEW
void MBeta::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);

  out.printf(" ");
  comparison_->dump(out);
}
#endif

AliasSet MCreateThis::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

bool MGetArgumentsObjectArg::congruentTo(const MDefinition* ins) const {
  if (!ins->isGetArgumentsObjectArg()) {
    return false;
  }
  if (ins->toGetArgumentsObjectArg()->argno() != argno()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGetArgumentsObjectArg::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

AliasSet MSetArgumentsObjectArg::getAliasSet() const {
  return AliasSet::Store(AliasSet::Any);
}

MObjectState::MObjectState(MObjectState* state)
    : MVariadicInstruction(classOpcode),
      numSlots_(state->numSlots_),
      numFixedSlots_(state->numFixedSlots_) {
  // This instruction is only used as a summary for bailout paths.
  setResultType(MIRType::Object);
  setRecoveredOnBailout();
}

MObjectState::MObjectState(JSObject* templateObject)
    : MObjectState(templateObject->as<NativeObject>().shape()) {}

MObjectState::MObjectState(const Shape* shape)
    : MVariadicInstruction(classOpcode) {
  // This instruction is only used as a summary for bailout paths.
  setResultType(MIRType::Object);
  setRecoveredOnBailout();

  numSlots_ = shape->asShared().slotSpan();
  numFixedSlots_ = shape->asShared().numFixedSlots();
}

/* static */
JSObject* MObjectState::templateObjectOf(MDefinition* obj) {
  // MNewPlainObject uses a shape constant, not an object.
  MOZ_ASSERT(!obj->isNewPlainObject());

  if (obj->isNewObject()) {
    return obj->toNewObject()->templateObject();
  } else if (obj->isNewCallObject()) {
    return obj->toNewCallObject()->templateObject();
  } else if (obj->isNewIterator()) {
    return obj->toNewIterator()->templateObject();
  }

  MOZ_CRASH("unreachable");
}

bool MObjectState::init(TempAllocator& alloc, MDefinition* obj) {
  if (!MVariadicInstruction::init(alloc, numSlots() + 1)) {
    return false;
  }
  // +1, for the Object.
  initOperand(0, obj);
  return true;
}

void MObjectState::initFromTemplateObject(TempAllocator& alloc,
                                          MDefinition* undefinedVal) {
  if (object()->isNewPlainObject()) {
    MOZ_ASSERT(object()->toNewPlainObject()->shape()->asShared().slotSpan() ==
               numSlots());
    for (size_t i = 0; i < numSlots(); i++) {
      initSlot(i, undefinedVal);
    }
    return;
  }

  JSObject* templateObject = templateObjectOf(object());

  // Initialize all the slots of the object state with the value contained in
  // the template object. This is needed to account values which are baked in
  // the template objects and not visible in IonMonkey, such as the
  // uninitialized-lexical magic value of call objects.

  MOZ_ASSERT(templateObject->is<NativeObject>());
  NativeObject& nativeObject = templateObject->as<NativeObject>();
  MOZ_ASSERT(nativeObject.slotSpan() == numSlots());

  for (size_t i = 0; i < numSlots(); i++) {
    Value val = nativeObject.getSlot(i);
    MDefinition* def = undefinedVal;
    if (!val.isUndefined()) {
      MConstant* ins = MConstant::New(alloc, val);
      block()->insertBefore(this, ins);
      def = ins;
    }
    initSlot(i, def);
  }
}

MObjectState* MObjectState::New(TempAllocator& alloc, MDefinition* obj) {
  MObjectState* res;
  if (obj->isNewPlainObject()) {
    const Shape* shape = obj->toNewPlainObject()->shape();
    res = new (alloc) MObjectState(shape);
  } else {
    JSObject* templateObject = templateObjectOf(obj);
    MOZ_ASSERT(templateObject, "Unexpected object creation.");
    res = new (alloc) MObjectState(templateObject);
  }

  if (!res || !res->init(alloc, obj)) {
    return nullptr;
  }
  return res;
}

MObjectState* MObjectState::Copy(TempAllocator& alloc, MObjectState* state) {
  MObjectState* res = new (alloc) MObjectState(state);
  if (!res || !res->init(alloc, state->object())) {
    return nullptr;
  }
  for (size_t i = 0; i < res->numSlots(); i++) {
    res->initSlot(i, state->getSlot(i));
  }
  return res;
}

MArrayState::MArrayState(MDefinition* arr) : MVariadicInstruction(classOpcode) {
  // This instruction is only used as a summary for bailout paths.
  setResultType(MIRType::Object);
  setRecoveredOnBailout();
  if (arr->isNewArrayObject()) {
    numElements_ = arr->toNewArrayObject()->length();
  } else {
    numElements_ = arr->toNewArray()->length();
  }
}

bool MArrayState::init(TempAllocator& alloc, MDefinition* obj,
                       MDefinition* len) {
  if (!MVariadicInstruction::init(alloc, numElements() + 2)) {
    return false;
  }
  // +1, for the Array object.
  initOperand(0, obj);
  // +1, for the length value of the array.
  initOperand(1, len);
  return true;
}

void MArrayState::initFromTemplateObject(TempAllocator& alloc,
                                         MDefinition* undefinedVal) {
  for (size_t i = 0; i < numElements(); i++) {
    initElement(i, undefinedVal);
  }
}

MArrayState* MArrayState::New(TempAllocator& alloc, MDefinition* arr,
                              MDefinition* initLength) {
  MArrayState* res = new (alloc) MArrayState(arr);
  if (!res || !res->init(alloc, arr, initLength)) {
    return nullptr;
  }
  return res;
}

MArrayState* MArrayState::Copy(TempAllocator& alloc, MArrayState* state) {
  MDefinition* arr = state->array();
  MDefinition* len = state->initializedLength();
  MArrayState* res = new (alloc) MArrayState(arr);
  if (!res || !res->init(alloc, arr, len)) {
    return nullptr;
  }
  for (size_t i = 0; i < res->numElements(); i++) {
    res->initElement(i, state->getElement(i));
  }
  return res;
}

MNewArray::MNewArray(uint32_t length, MConstant* templateConst,
                     gc::Heap initialHeap, bool vmCall)
    : MUnaryInstruction(classOpcode, templateConst),
      length_(length),
      initialHeap_(initialHeap),
      vmCall_(vmCall) {
  setResultType(MIRType::Object);
}

MDefinition::AliasType MLoadFixedSlot::mightAlias(
    const MDefinition* def) const {
  if (def->isStoreFixedSlot()) {
    const MStoreFixedSlot* store = def->toStoreFixedSlot();
    if (store->slot() != slot()) {
      return AliasType::NoAlias;
    }
    if (store->object() != object()) {
      return AliasType::MayAlias;
    }
    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

MDefinition* MLoadFixedSlot::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

MDefinition::AliasType MLoadFixedSlotAndUnbox::mightAlias(
    const MDefinition* def) const {
  if (def->isStoreFixedSlot()) {
    const MStoreFixedSlot* store = def->toStoreFixedSlot();
    if (store->slot() != slot()) {
      return AliasType::NoAlias;
    }
    if (store->object() != object()) {
      return AliasType::MayAlias;
    }
    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

MDefinition* MLoadFixedSlotAndUnbox::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

MDefinition::AliasType MLoadDynamicSlot::mightAlias(
    const MDefinition* def) const {
  if (def->isStoreDynamicSlot()) {
    const MStoreDynamicSlot* store = def->toStoreDynamicSlot();
    if (store->slot() != slot()) {
      return AliasType::NoAlias;
    }

    if (store->slots() != slots()) {
      return AliasType::MayAlias;
    }

    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

HashNumber MLoadDynamicSlot::valueHash() const {
  HashNumber hash = MDefinition::valueHash();
  hash = addU32ToHash(hash, slot_);
  return hash;
}

MDefinition* MLoadDynamicSlot::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

#ifdef JS_JITSPEW
void MLoadDynamicSlot::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (slot %u)", slot());
}

void MLoadDynamicSlotAndUnbox::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (slot %zu)", slot());
}

void MStoreDynamicSlot::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (slot %u)", slot());
}

void MLoadFixedSlot::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (slot %zu)", slot());
}

void MLoadFixedSlotAndUnbox::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (slot %zu)", slot());
}

void MStoreFixedSlot::printOpcode(GenericPrinter& out) const {
  MDefinition::printOpcode(out);
  out.printf(" (slot %zu)", slot());
}
#endif

MDefinition* MGuardFunctionScript::foldsTo(TempAllocator& alloc) {
  MDefinition* in = input();
  if (in->isLambda() &&
      in->toLambda()->templateFunction()->baseScript() == expected()) {
    return in;
  }
  return this;
}

MDefinition* MFunctionEnvironment::foldsTo(TempAllocator& alloc) {
  if (input()->isLambda()) {
    return input()->toLambda()->environmentChain();
  }
  if (input()->isFunctionWithProto()) {
    return input()->toFunctionWithProto()->environmentChain();
  }
  return this;
}

static bool AddIsANonZeroAdditionOf(MAdd* add, MDefinition* ins) {
  if (add->lhs() != ins && add->rhs() != ins) {
    return false;
  }
  MDefinition* other = (add->lhs() == ins) ? add->rhs() : add->lhs();
  if (!IsNumberType(other->type())) {
    return false;
  }
  if (!other->isConstant()) {
    return false;
  }
  if (other->toConstant()->numberToDouble() == 0) {
    return false;
  }
  return true;
}

// Skip over instructions that usually appear between the actual index
// value being used and the MLoadElement.
// They don't modify the index value in a meaningful way.
static MDefinition* SkipUninterestingInstructions(MDefinition* ins) {
  // Drop the MToNumberInt32 added by the TypePolicy for double and float
  // values.
  if (ins->isToNumberInt32()) {
    return SkipUninterestingInstructions(ins->toToNumberInt32()->input());
  }

  // Ignore the bounds check, which don't modify the index.
  if (ins->isBoundsCheck()) {
    return SkipUninterestingInstructions(ins->toBoundsCheck()->index());
  }

  // Masking the index for Spectre-mitigation is not observable.
  if (ins->isSpectreMaskIndex()) {
    return SkipUninterestingInstructions(ins->toSpectreMaskIndex()->index());
  }

  return ins;
}

static bool DefinitelyDifferentValue(MDefinition* ins1, MDefinition* ins2) {
  ins1 = SkipUninterestingInstructions(ins1);
  ins2 = SkipUninterestingInstructions(ins2);

  if (ins1 == ins2) {
    return false;
  }

  // For constants check they are not equal.
  if (ins1->isConstant() && ins2->isConstant()) {
    MConstant* cst1 = ins1->toConstant();
    MConstant* cst2 = ins2->toConstant();

    if (!cst1->isTypeRepresentableAsDouble() ||
        !cst2->isTypeRepresentableAsDouble()) {
      return false;
    }

    // Be conservative and only allow values that fit into int32.
    int32_t n1, n2;
    if (!mozilla::NumberIsInt32(cst1->numberToDouble(), &n1) ||
        !mozilla::NumberIsInt32(cst2->numberToDouble(), &n2)) {
      return false;
    }

    return n1 != n2;
  }

  // Check if "ins1 = ins2 + cte", which would make both instructions
  // have different values.
  if (ins1->isAdd()) {
    if (AddIsANonZeroAdditionOf(ins1->toAdd(), ins2)) {
      return true;
    }
  }
  if (ins2->isAdd()) {
    if (AddIsANonZeroAdditionOf(ins2->toAdd(), ins1)) {
      return true;
    }
  }

  return false;
}

MDefinition::AliasType MLoadElement::mightAlias(const MDefinition* def) const {
  if (def->isStoreElement()) {
    const MStoreElement* store = def->toStoreElement();
    if (store->index() != index()) {
      if (DefinitelyDifferentValue(store->index(), index())) {
        return AliasType::NoAlias;
      }
      return AliasType::MayAlias;
    }

    if (store->elements() != elements()) {
      return AliasType::MayAlias;
    }

    return AliasType::MustAlias;
  }
  return AliasType::MayAlias;
}

MDefinition* MLoadElement::foldsTo(TempAllocator& alloc) {
  if (MDefinition* def = foldsToStore(alloc)) {
    return def;
  }

  return this;
}

void MSqrt::trySpecializeFloat32(TempAllocator& alloc) {
  if (EnsureFloatConsumersAndInputOrConvert(this, alloc)) {
    setResultType(MIRType::Float32);
    specialization_ = MIRType::Float32;
  }
}

MDefinition* MClz::foldsTo(TempAllocator& alloc) {
  if (num()->isConstant()) {
    MConstant* c = num()->toConstant();
    if (type() == MIRType::Int32) {
      int32_t n = c->toInt32();
      if (n == 0) {
        return MConstant::NewInt32(alloc, 32);
      }
      return MConstant::NewInt32(alloc, mozilla::CountLeadingZeroes32(n));
    }
    int64_t n = c->toInt64();
    if (n == 0) {
      return MConstant::NewInt64(alloc, int64_t(64));
    }
    return MConstant::NewInt64(alloc,
                               int64_t(mozilla::CountLeadingZeroes64(n)));
  }

  return this;
}

MDefinition* MCtz::foldsTo(TempAllocator& alloc) {
  if (num()->isConstant()) {
    MConstant* c = num()->toConstant();
    if (type() == MIRType::Int32) {
      int32_t n = num()->toConstant()->toInt32();
      if (n == 0) {
        return MConstant::NewInt32(alloc, 32);
      }
      return MConstant::NewInt32(alloc, mozilla::CountTrailingZeroes32(n));
    }
    int64_t n = c->toInt64();
    if (n == 0) {
      return MConstant::NewInt64(alloc, int64_t(64));
    }
    return MConstant::NewInt64(alloc,
                               int64_t(mozilla::CountTrailingZeroes64(n)));
  }

  return this;
}

MDefinition* MPopcnt::foldsTo(TempAllocator& alloc) {
  if (num()->isConstant()) {
    MConstant* c = num()->toConstant();
    if (type() == MIRType::Int32) {
      int32_t n = num()->toConstant()->toInt32();
      return MConstant::NewInt32(alloc, mozilla::CountPopulation32(n));
    }
    int64_t n = c->toInt64();
    return MConstant::NewInt64(alloc, int64_t(mozilla::CountPopulation64(n)));
  }

  return this;
}

MDefinition* MBoundsCheck::foldsTo(TempAllocator& alloc) {
  if (type() == MIRType::Int32 && index()->isConstant() &&
      length()->isConstant()) {
    uint32_t len = length()->toConstant()->toInt32();
    uint32_t idx = index()->toConstant()->toInt32();
    if (idx + uint32_t(minimum()) < len && idx + uint32_t(maximum()) < len) {
      return index();
    }
  }

  return this;
}

MDefinition* MTableSwitch::foldsTo(TempAllocator& alloc) {
  MDefinition* op = getOperand(0);

  // If we only have one successor, convert to a plain goto to the only
  // successor. TableSwitch indices are numeric; other types will always go to
  // the only successor.
  if (numSuccessors() == 1 ||
      (op->type() != MIRType::Value && !IsNumberType(op->type()))) {
    return MGoto::New(alloc, getDefault());
  }

  if (MConstant* opConst = op->maybeConstantValue()) {
    if (op->type() == MIRType::Int32) {
      int32_t i = opConst->toInt32() - low_;
      MBasicBlock* target;
      if (size_t(i) < numCases()) {
        target = getCase(size_t(i));
      } else {
        target = getDefault();
      }
      MOZ_ASSERT(target);
      return MGoto::New(alloc, target);
    }
  }

  return this;
}

MDefinition* MArrayJoin::foldsTo(TempAllocator& alloc) {
  MDefinition* arr = array();

  if (!arr->isStringSplit()) {
    return this;
  }

  setRecoveredOnBailout();
  if (arr->hasLiveDefUses()) {
    setNotRecoveredOnBailout();
    return this;
  }

  // The MStringSplit won't generate any code.
  arr->setRecoveredOnBailout();

  // We're replacing foo.split(bar).join(baz) by
  // foo.replace(bar, baz).  MStringSplit could be recovered by
  // a bailout.  As we are removing its last use, and its result
  // could be captured by a resume point, this MStringSplit will
  // be executed on the bailout path.
  MDefinition* string = arr->toStringSplit()->string();
  MDefinition* pattern = arr->toStringSplit()->separator();
  MDefinition* replacement = separator();

  MStringReplace* substr =
      MStringReplace::New(alloc, string, pattern, replacement);
  substr->setFlatReplacement();
  return substr;
}

MDefinition* MGetFirstDollarIndex::foldsTo(TempAllocator& alloc) {
  MDefinition* strArg = str();
  if (!strArg->isConstant()) {
    return this;
  }

  JSOffThreadAtom* str = strArg->toConstant()->toString();
  int32_t index = GetFirstDollarIndexRawFlat(str);
  return MConstant::NewInt32(alloc, index);
}

AliasSet MThrowRuntimeLexicalError::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MSlots::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

MDefinition::AliasType MSlots::mightAlias(const MDefinition* store) const {
  // ArrayPush only modifies object elements, but not object slots.
  if (store->isArrayPush()) {
    return AliasType::NoAlias;
  }
  return MInstruction::mightAlias(store);
}

AliasSet MElements::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MInitializedLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MSetInitializedLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields);
}

AliasSet MObjectKeysLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MArrayLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MSetArrayLength::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields);
}

AliasSet MFunctionLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

AliasSet MFunctionName::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

AliasSet MArrayBufferByteLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::FixedSlot);
}

AliasSet MArrayBufferViewLength::getAliasSet() const {
  return AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset);
}

AliasSet MArrayBufferViewByteOffset::getAliasSet() const {
  return AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset);
}

AliasSet MArrayBufferViewElements::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardHasAttachedArrayBuffer::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot);
}

AliasSet MResizableTypedArrayLength::getAliasSet() const {
  // Loads the length and byteOffset slots, the shared-elements flag, the
  // auto-length fixed slot, and the shared raw-buffer length.
  auto flags = AliasSet::ArrayBufferViewLengthOrOffset |
               AliasSet::ObjectFields | AliasSet::FixedSlot |
               AliasSet::SharedArrayRawBufferLength;

  // When a barrier is needed make the instruction effectful by giving it a
  // "store" effect. Also prevent reordering LoadUnboxedScalar before this
  // instruction by including |UnboxedElement| in the alias set.
  if (requiresMemoryBarrier() == MemoryBarrierRequirement::Required) {
    return AliasSet::Store(flags | AliasSet::UnboxedElement);
  }
  return AliasSet::Load(flags);
}

bool MResizableTypedArrayLength::congruentTo(const MDefinition* ins) const {
  if (requiresMemoryBarrier() == MemoryBarrierRequirement::Required) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MResizableDataViewByteLength::getAliasSet() const {
  // Loads the length and byteOffset slots, the shared-elements flag, the
  // auto-length fixed slot, and the shared raw-buffer length.
  auto flags = AliasSet::ArrayBufferViewLengthOrOffset |
               AliasSet::ObjectFields | AliasSet::FixedSlot |
               AliasSet::SharedArrayRawBufferLength;

  // When a barrier is needed make the instruction effectful by giving it a
  // "store" effect. Also prevent reordering LoadUnboxedScalar before this
  // instruction by including |UnboxedElement| in the alias set.
  if (requiresMemoryBarrier() == MemoryBarrierRequirement::Required) {
    return AliasSet::Store(flags | AliasSet::UnboxedElement);
  }
  return AliasSet::Load(flags);
}

bool MResizableDataViewByteLength::congruentTo(const MDefinition* ins) const {
  if (requiresMemoryBarrier() == MemoryBarrierRequirement::Required) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGrowableSharedArrayBufferByteLength::getAliasSet() const {
  // Requires a barrier, so make the instruction effectful by giving it a
  // "store" effect. Also prevent reordering LoadUnboxedScalar before this
  // instruction by including |UnboxedElement| in the alias set.
  return AliasSet::Store(AliasSet::FixedSlot |
                         AliasSet::SharedArrayRawBufferLength |
                         AliasSet::UnboxedElement);
}

AliasSet MGuardResizableArrayBufferViewInBounds::getAliasSet() const {
  // Additionally reads the |initialLength| and |initialByteOffset| slots, but
  // since these can't change after construction, we don't need to track them.
  return AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset);
}

AliasSet MGuardResizableArrayBufferViewInBoundsOrDetached::getAliasSet() const {
  // Loads the byteOffset and additionally checks for detached buffers, so the
  // alias set also has to include |ObjectFields| and |FixedSlot|.
  return AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset |
                        AliasSet::ObjectFields | AliasSet::FixedSlot);
}

AliasSet MTypedArraySet::getAliasSet() const {
  // Loads typed array length and elements.
  constexpr auto load =
      AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset |
                     AliasSet::ObjectFields | AliasSet::UnboxedElement);

  // Stores into typed array elements.
  constexpr auto store = AliasSet::Store(AliasSet::UnboxedElement);

  return load | store;
}

AliasSet MArrayPush::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields | AliasSet::Element);
}

MDefinition* MGuardNumberToIntPtrIndex::foldsTo(TempAllocator& alloc) {
  MDefinition* input = this->input();

  if (input->isToDouble() && input->getOperand(0)->type() == MIRType::Int32) {
    return MInt32ToIntPtr::New(alloc, input->getOperand(0));
  }

  if (!input->isConstant()) {
    return this;
  }

  // Fold constant double representable as intptr to intptr.
  int64_t ival;
  if (!mozilla::NumberEqualsInt64(input->toConstant()->toDouble(), &ival)) {
    // If not representable as an int64, this access is equal to an OOB access.
    // So replace it with a known int64/intptr value which also produces an OOB
    // access. If we don't support OOB accesses we have to bail out.
    if (!supportOOB()) {
      return this;
    }
    ival = -1;
  }

  if (ival < INTPTR_MIN || ival > INTPTR_MAX) {
    return this;
  }

  return MConstant::NewIntPtr(alloc, intptr_t(ival));
}

MDefinition* MIsObject::foldsTo(TempAllocator& alloc) {
  MDefinition* input = object();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->toBox()->input();
  return MConstant::NewBoolean(alloc, unboxed->type() == MIRType::Object);
}

MDefinition* MIsNullOrUndefined::foldsTo(TempAllocator& alloc) {
  // MIsNullOrUndefined doesn't have a type-policy, so the value can already be
  // unboxed.
  MDefinition* unboxed = value();
  if (unboxed->type() == MIRType::Value) {
    if (!unboxed->isBox()) {
      return this;
    }
    unboxed = unboxed->toBox()->input();
  }

  return MConstant::NewBoolean(alloc, IsNullOrUndefined(unboxed->type()));
}

AliasSet MHomeObjectSuperBase::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

MDefinition* MGuardValue::foldsTo(TempAllocator& alloc) {
  if (MConstant* cst = value()->maybeConstantValue()) {
    if (expected().isValue() && cst->toJSValue() == expected().toValue()) {
      return value();
    }
  }

  return this;
}

MDefinition* MGuardNullOrUndefined::foldsTo(TempAllocator& alloc) {
  MDefinition* input = value();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->toBox()->input();
  if (IsNullOrUndefined(unboxed->type())) {
    return input;
  }

  return this;
}

MDefinition* MGuardIsNotObject::foldsTo(TempAllocator& alloc) {
  MDefinition* input = value();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->toBox()->input();
  if (unboxed->type() == MIRType::Object) {
    return this;
  }

  return input;
}

MDefinition* MGuardObjectIdentity::foldsTo(TempAllocator& alloc) {
  if (object()->isConstant() && expected()->isConstant()) {
    JSObject* obj = &object()->toConstant()->toObject();
    JSObject* other = &expected()->toConstant()->toObject();
    if (!bailOnEquality()) {
      if (obj == other) {
        return object();
      }
    } else {
      if (obj != other) {
        return object();
      }
    }
  }

  if (!bailOnEquality() && object()->isNurseryObject() &&
      expected()->isNurseryObject()) {
    uint32_t objIndex = object()->toNurseryObject()->nurseryObjectIndex();
    uint32_t otherIndex = expected()->toNurseryObject()->nurseryObjectIndex();
    if (objIndex == otherIndex) {
      return object();
    }
  }

  return this;
}

MDefinition* MGuardSpecificFunction::foldsTo(TempAllocator& alloc) {
  if (function()->isConstant() && expected()->isConstant()) {
    JSObject* fun = &function()->toConstant()->toObject();
    JSObject* other = &expected()->toConstant()->toObject();
    if (fun == other) {
      return function();
    }
  }

  if (function()->isNurseryObject() && expected()->isNurseryObject()) {
    uint32_t funIndex = function()->toNurseryObject()->nurseryObjectIndex();
    uint32_t otherIndex = expected()->toNurseryObject()->nurseryObjectIndex();
    if (funIndex == otherIndex) {
      return function();
    }
  }

  return this;
}

MDefinition* MGuardSpecificAtom::foldsTo(TempAllocator& alloc) {
  if (str()->isConstant()) {
    JSOffThreadAtom* s = str()->toConstant()->toString();
    if (s == atom()) {
      return str();
    }
  }

  return this;
}

MDefinition* MGuardSpecificSymbol::foldsTo(TempAllocator& alloc) {
  if (symbol()->isConstant()) {
    if (symbol()->toConstant()->toSymbol() == expected()) {
      return symbol();
    }
  }

  return this;
}

MDefinition* MGuardSpecificInt32::foldsTo(TempAllocator& alloc) {
  if (num()->isConstant() && num()->toConstant()->isInt32(expected())) {
    return num();
  }
  return this;
}

bool MCallBindVar::congruentTo(const MDefinition* ins) const {
  if (!ins->isCallBindVar()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

bool MGuardShape::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardShape()) {
    return false;
  }
  if (shape() != ins->toGuardShape()->shape()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardShape::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MHasShape::congruentTo(const MDefinition* ins) const {
  if (!ins->isHasShape()) {
    return false;
  }
  if (shape() != ins->toHasShape()->shape()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MHasShape::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

MDefinition::AliasType MGuardShape::mightAlias(const MDefinition* store) const {
  // These instructions only modify object elements, but not the shape.
  if (store->isStoreElementHole() || store->isArrayPush()) {
    return AliasType::NoAlias;
  }
  if (object()->isConstantProto()) {
    const MDefinition* receiverObject =
        object()->toConstantProto()->getReceiverObject();
    switch (store->op()) {
      case MDefinition::Opcode::StoreFixedSlot:
        if (store->toStoreFixedSlot()->object()->skipObjectGuards() ==
            receiverObject) {
          return AliasType::NoAlias;
        }
        break;
      case MDefinition::Opcode::StoreDynamicSlot:
        if (store->toStoreDynamicSlot()
                ->slots()
                ->toSlots()
                ->object()
                ->skipObjectGuards() == receiverObject) {
          return AliasType::NoAlias;
        }
        break;
      case MDefinition::Opcode::AddAndStoreSlot:
        if (store->toAddAndStoreSlot()->object()->skipObjectGuards() ==
            receiverObject) {
          return AliasType::NoAlias;
        }
        break;
      case MDefinition::Opcode::AllocateAndStoreSlot:
        if (store->toAllocateAndStoreSlot()->object()->skipObjectGuards() ==
            receiverObject) {
          return AliasType::NoAlias;
        }
        break;
      default:
        break;
    }
  }
  return MInstruction::mightAlias(store);
}

bool MGuardFuse::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardFuse()) {
    return false;
  }
  if (fuseIndex() != ins->toGuardFuse()->fuseIndex()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardFuse::getAliasSet() const {
  // The alias set below reflects the set of operations which could cause a fuse
  // to be popped, and therefore MGuardFuse aliases with.
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::DynamicSlot |
                        AliasSet::FixedSlot |
                        AliasSet::GlobalGenerationCounter);
}

AliasSet MGuardMultipleShapes::getAliasSet() const {
  // Note: This instruction loads the elements of the ListObject used to
  // store the list of shapes, but that object is internal and not exposed
  // to script, so it doesn't have to be in the alias set.
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardGlobalGeneration::getAliasSet() const {
  return AliasSet::Load(AliasSet::GlobalGenerationCounter);
}

bool MGuardGlobalGeneration::congruentTo(const MDefinition* ins) const {
  return ins->isGuardGlobalGeneration() &&
         ins->toGuardGlobalGeneration()->expected() == expected() &&
         ins->toGuardGlobalGeneration()->generationAddr() == generationAddr();
}

MDefinition* MGuardIsNotProxy::foldsTo(TempAllocator& alloc) {
  KnownClass known = GetObjectKnownClass(object());
  if (known == KnownClass::None) {
    return this;
  }

  MOZ_ASSERT(!GetObjectKnownJSClass(object())->isProxyObject());
  AssertKnownClass(alloc, this, object());
  return object();
}

AliasSet MMegamorphicLoadSlotByValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

static PropertyKey ToNonIntPropertyKey(MDefinition* idval) {
  MConstant* constant = idval->maybeConstantValue();
  if (!constant) {
    return PropertyKey::Void();
  }
  if (constant->type() == MIRType::String) {
    JSOffThreadAtom* str = constant->toString();
    if (str->isIndex()) {
      return PropertyKey::Void();
    }
    return PropertyKey::NonIntAtom(str->unwrap());
  }
  if (constant->type() == MIRType::Symbol) {
    return PropertyKey::Symbol(constant->toSymbol());
  }
  return PropertyKey::Void();
}

MDefinition* MMegamorphicLoadSlotByValue::foldsTo(TempAllocator& alloc) {
  PropertyKey id = ToNonIntPropertyKey(idVal());
  if (id.isVoid()) {
    return this;
  }

  auto* result = MMegamorphicLoadSlot::New(alloc, object(), id);
  result->setDependency(dependency());
  return result;
}

MDefinition* MMegamorphicLoadSlotByValuePermissive::foldsTo(
    TempAllocator& alloc) {
  PropertyKey id = ToNonIntPropertyKey(idVal());
  if (id.isVoid()) {
    return this;
  }

  auto* result = MMegamorphicLoadSlotPermissive::New(alloc, object(), id);
  result->stealResumePoint(this);
  return result;
}

bool MMegamorphicLoadSlot::congruentTo(const MDefinition* ins) const {
  if (!ins->isMegamorphicLoadSlot()) {
    return false;
  }
  if (ins->toMegamorphicLoadSlot()->name() != name()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MMegamorphicLoadSlot::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MSmallObjectVariableKeyHasProp::congruentTo(const MDefinition* ins) const {
  if (!ins->isSmallObjectVariableKeyHasProp()) {
    return false;
  }
  if (ins->toSmallObjectVariableKeyHasProp()->shape() != shape()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MSmallObjectVariableKeyHasProp::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MMegamorphicHasProp::congruentTo(const MDefinition* ins) const {
  if (!ins->isMegamorphicHasProp()) {
    return false;
  }
  if (ins->toMegamorphicHasProp()->hasOwn() != hasOwn()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MMegamorphicHasProp::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

bool MNurseryObject::congruentTo(const MDefinition* ins) const {
  if (!ins->isNurseryObject()) {
    return false;
  }
  return nurseryObjectIndex() == ins->toNurseryObject()->nurseryObjectIndex();
}

AliasSet MGuardFunctionIsNonBuiltinCtor::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MGuardFunctionKind::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardFunctionKind()) {
    return false;
  }
  if (expected() != ins->toGuardFunctionKind()->expected()) {
    return false;
  }
  if (bailOnEquality() != ins->toGuardFunctionKind()->bailOnEquality()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardFunctionKind::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MGuardFunctionScript::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardFunctionScript()) {
    return false;
  }
  if (expected() != ins->toGuardFunctionScript()->expected()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardFunctionScript::getAliasSet() const {
  // A JSFunction's BaseScript pointer is immutable. Relazification of
  // top-level/named self-hosted functions is an exception to this, but we don't
  // use this guard for those self-hosted functions.
  // See IRGenerator::emitCalleeGuard.
  MOZ_ASSERT_IF(flags_.isSelfHostedOrIntrinsic(), flags_.isLambda());
  return AliasSet::None();
}

bool MGuardSpecificAtom::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardSpecificAtom()) {
    return false;
  }
  if (atom() != ins->toGuardSpecificAtom()->atom()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

MDefinition* MGuardStringToIndex::foldsTo(TempAllocator& alloc) {
  if (!string()->isConstant()) {
    return this;
  }

  JSOffThreadAtom* str = string()->toConstant()->toString();

  uint32_t index = UINT32_MAX;
  if (!str->isIndex(&index) || index > INT32_MAX) {
    return this;
  }

  return MConstant::NewInt32(alloc, index);
}

MDefinition* MGuardStringToInt32::foldsTo(TempAllocator& alloc) {
  if (!string()->isConstant()) {
    return this;
  }

  JSOffThreadAtom* str = string()->toConstant()->toString();
  double number = OffThreadAtomToNumber(str);

  int32_t n;
  if (!mozilla::NumberIsInt32(number, &n)) {
    return this;
  }

  return MConstant::NewInt32(alloc, n);
}

MDefinition* MGuardStringToDouble::foldsTo(TempAllocator& alloc) {
  if (!string()->isConstant()) {
    return this;
  }

  JSOffThreadAtom* str = string()->toConstant()->toString();
  double number = OffThreadAtomToNumber(str);
  return MConstant::NewDouble(alloc, number);
}

AliasSet MGuardNoDenseElements::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MIteratorHasIndices::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MAllocateAndStoreSlot::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields | AliasSet::DynamicSlot);
}

AliasSet MLoadDOMExpandoValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::DOMProxyExpando);
}

AliasSet MLoadDOMExpandoValueIgnoreGeneration::getAliasSet() const {
  return AliasSet::Load(AliasSet::DOMProxyExpando);
}

bool MGuardDOMExpandoMissingOrGuardShape::congruentTo(
    const MDefinition* ins) const {
  if (!ins->isGuardDOMExpandoMissingOrGuardShape()) {
    return false;
  }
  if (shape() != ins->toGuardDOMExpandoMissingOrGuardShape()->shape()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardDOMExpandoMissingOrGuardShape::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

MDefinition* MGuardToClass::foldsTo(TempAllocator& alloc) {
  const JSClass* clasp = GetObjectKnownJSClass(object());
  if (!clasp || getClass() != clasp) {
    return this;
  }

  AssertKnownClass(alloc, this, object());
  return object();
}

MDefinition* MGuardToFunction::foldsTo(TempAllocator& alloc) {
  if (GetObjectKnownClass(object()) != KnownClass::Function) {
    return this;
  }

  AssertKnownClass(alloc, this, object());
  return object();
}

MDefinition* MHasClass::foldsTo(TempAllocator& alloc) {
  const JSClass* clasp = GetObjectKnownJSClass(object());
  if (!clasp) {
    return this;
  }

  AssertKnownClass(alloc, this, object());
  return MConstant::NewBoolean(alloc, getClass() == clasp);
}

MDefinition* MIsCallable::foldsTo(TempAllocator& alloc) {
  if (input()->type() != MIRType::Object) {
    return this;
  }

  KnownClass known = GetObjectKnownClass(input());
  if (known == KnownClass::None) {
    return this;
  }

  AssertKnownClass(alloc, this, input());
  return MConstant::NewBoolean(alloc, known == KnownClass::Function);
}

MDefinition* MIsArray::foldsTo(TempAllocator& alloc) {
  if (input()->type() != MIRType::Object) {
    return this;
  }

  KnownClass known = GetObjectKnownClass(input());
  if (known == KnownClass::None) {
    return this;
  }

  AssertKnownClass(alloc, this, input());
  return MConstant::NewBoolean(alloc, known == KnownClass::Array);
}

AliasSet MObjectClassToString::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot);
}

MDefinition* MGuardIsNotArrayBufferMaybeShared::foldsTo(TempAllocator& alloc) {
  switch (GetObjectKnownClass(object())) {
    case KnownClass::PlainObject:
    case KnownClass::Array:
    case KnownClass::Function:
    case KnownClass::RegExp:
    case KnownClass::ArrayIterator:
    case KnownClass::StringIterator:
    case KnownClass::RegExpStringIterator: {
      AssertKnownClass(alloc, this, object());
      return object();
    }
    case KnownClass::None:
      break;
  }

  return this;
}

MDefinition* MCheckIsObj::foldsTo(TempAllocator& alloc) {
  if (!input()->isBox()) {
    return this;
  }

  MDefinition* unboxed = input()->toBox()->input();
  if (unboxed->type() == MIRType::Object) {
    return unboxed;
  }

  return this;
}

AliasSet MCheckIsObj::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

#ifdef JS_PUNBOX64
AliasSet MCheckScriptedProxyGetResult::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}
#endif

static bool IsBoxedObject(MDefinition* def) {
  MOZ_ASSERT(def->type() == MIRType::Value);

  if (def->isBox()) {
    return def->toBox()->input()->type() == MIRType::Object;
  }

  // Construct calls are always returning a boxed object.
  //
  // TODO: We should consider encoding this directly in the graph instead of
  // having to special case it here.
  if (def->isCall()) {
    return def->toCall()->isConstructing();
  }
  if (def->isConstructArray()) {
    return true;
  }
  if (def->isConstructArgs()) {
    return true;
  }

  return false;
}

MDefinition* MCheckReturn::foldsTo(TempAllocator& alloc) {
  auto* returnVal = returnValue();
  if (!returnVal->isBox()) {
    return this;
  }

  auto* unboxedReturnVal = returnVal->toBox()->input();
  if (unboxedReturnVal->type() == MIRType::Object) {
    return returnVal;
  }

  if (unboxedReturnVal->type() != MIRType::Undefined) {
    return this;
  }

  auto* thisVal = thisValue();
  if (IsBoxedObject(thisVal)) {
    return thisVal;
  }

  return this;
}

MDefinition* MCheckThis::foldsTo(TempAllocator& alloc) {
  MDefinition* input = thisValue();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->toBox()->input();
  if (IsMagicType(unboxed->type())) {
    return this;
  }

  return input;
}

MDefinition* MCheckThisReinit::foldsTo(TempAllocator& alloc) {
  MDefinition* input = thisValue();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->toBox()->input();
  if (unboxed->type() != MIRType::MagicUninitializedLexical) {
    return this;
  }

  return input;
}

MDefinition* MCheckObjCoercible::foldsTo(TempAllocator& alloc) {
  MDefinition* input = checkValue();
  if (!input->isBox()) {
    return this;
  }

  MDefinition* unboxed = input->toBox()->input();
  if (IsNullOrUndefined(unboxed->type())) {
    return this;
  }

  return input;
}

AliasSet MCheckObjCoercible::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MCheckReturn::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MCheckThis::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MCheckThisReinit::getAliasSet() const {
  return AliasSet::Store(AliasSet::ExceptionState);
}

AliasSet MIsPackedArray::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardArrayIsPacked::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardElementsArePacked::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MSuperFunction::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MInitHomeObject::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields);
}

AliasSet MLoadWrapperTarget::getAliasSet() const {
  return AliasSet::Load(AliasSet::Any);
}

bool MLoadWrapperTarget::congruentTo(const MDefinition* ins) const {
  if (!ins->isLoadWrapperTarget()) {
    return false;
  }
  if (ins->toLoadWrapperTarget()->fallible() != fallible()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardHasGetterSetter::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

bool MGuardHasGetterSetter::congruentTo(const MDefinition* ins) const {
  if (!ins->isGuardHasGetterSetter()) {
    return false;
  }
  if (ins->toGuardHasGetterSetter()->propId() != propId()) {
    return false;
  }
  if (ins->toGuardHasGetterSetter()->getterSetterValue() !=
      getterSetterValue()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}

AliasSet MGuardIsExtensible::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MGuardIndexIsNotDenseElement::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::Element);
}

AliasSet MGuardIndexIsValidUpdateOrAdd::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields);
}

AliasSet MCallObjectHasSparseElement::getAliasSet() const {
  return AliasSet::Load(AliasSet::Element | AliasSet::ObjectFields |
                        AliasSet::FixedSlot | AliasSet::DynamicSlot);
}

AliasSet MLoadSlotByIteratorIndex::getAliasSet() const {
  return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                        AliasSet::DynamicSlot | AliasSet::Element);
}

AliasSet MStoreSlotByIteratorIndex::getAliasSet() const {
  return AliasSet::Store(AliasSet::ObjectFields | AliasSet::FixedSlot |
                         AliasSet::DynamicSlot | AliasSet::Element);
}

MDefinition* MGuardInt32IsNonNegative::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(index()->type() == MIRType::Int32);

  MDefinition* input = index();
  if (!input->isConstant() || input->toConstant()->toInt32() < 0) {
    return this;
  }
  return input;
}

MDefinition* MGuardIntPtrIsNonNegative::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(index()->type() == MIRType::IntPtr);

  MDefinition* input = index();
  if (!input->isConstant() || input->toConstant()->toIntPtr() < 0) {
    return this;
  }
  return input;
}

MDefinition* MGuardInt32Range::foldsTo(TempAllocator& alloc) {
  MOZ_ASSERT(input()->type() == MIRType::Int32);
  MOZ_ASSERT(minimum() <= maximum());

  MDefinition* in = input();
  if (!in->isConstant()) {
    return this;
  }
  int32_t cst = in->toConstant()->toInt32();
  if (cst < minimum() || cst > maximum()) {
    return this;
  }
  return in;
}

MDefinition* MGuardNonGCThing::foldsTo(TempAllocator& alloc) {
  if (!input()->isBox()) {
    return this;
  }

  MDefinition* unboxed = input()->toBox()->input();
  if (!IsNonGCThing(unboxed->type())) {
    return this;
  }
  return input();
}

AliasSet MSetObjectHasNonBigInt::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MSetObjectHasBigInt::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MSetObjectHasValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MSetObjectHasValueVMCall::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MSetObjectSize::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectHasNonBigInt::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectHasBigInt::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectHasValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectHasValueVMCall::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectGetNonBigInt::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectGetBigInt::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectGetValue::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectGetValueVMCall::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MMapObjectSize::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MWeakMapGetObject::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MWeakMapHasObject::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MWeakSetHasObject::getAliasSet() const {
  return AliasSet::Load(AliasSet::MapOrSetHashTable);
}

AliasSet MDateFillLocalTimeSlots::getAliasSet() const {
  // Reads and stores fixed slots. Additional reads from DateTimeInfo don't need
  // to be tracked, because they don't interact with other alias set states.
  return AliasSet::Store(AliasSet::FixedSlot);
}

MBindFunction* MBindFunction::New(TempAllocator& alloc, MDefinition* target,
                                  uint32_t argc, JSObject* templateObj) {
  auto* ins = new (alloc) MBindFunction(templateObj);
  if (!ins->init(alloc, NumNonArgumentOperands + argc)) {
    return nullptr;
  }
  ins->initOperand(0, target);
  return ins;
}

MCreateInlinedArgumentsObject* MCreateInlinedArgumentsObject::New(
    TempAllocator& alloc, MDefinition* callObj, MDefinition* callee,
    MDefinitionVector& args, ArgumentsObject* templateObj) {
  MCreateInlinedArgumentsObject* ins =
      new (alloc) MCreateInlinedArgumentsObject(templateObj);

  uint32_t argc = args.length();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, callObj);
  ins->initOperand(1, callee);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, args[i]);
  }

  return ins;
}

MGetInlinedArgument* MGetInlinedArgument::New(
    TempAllocator& alloc, MDefinition* index,
    MCreateInlinedArgumentsObject* args) {
  MGetInlinedArgument* ins = new (alloc) MGetInlinedArgument();

  uint32_t argc = args->numActuals();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, index);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, args->getArg(i));
  }

  return ins;
}

MGetInlinedArgument* MGetInlinedArgument::New(TempAllocator& alloc,
                                              MDefinition* index,
                                              const CallInfo& callInfo) {
  MGetInlinedArgument* ins = new (alloc) MGetInlinedArgument();

  uint32_t argc = callInfo.argc();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, index);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, callInfo.getArg(i));
  }

  return ins;
}

MDefinition* MGetInlinedArgument::foldsTo(TempAllocator& alloc) {
  MDefinition* indexDef = SkipUninterestingInstructions(index());
  if (!indexDef->isConstant() || indexDef->type() != MIRType::Int32) {
    return this;
  }

  int32_t indexConst = indexDef->toConstant()->toInt32();
  if (indexConst < 0 || uint32_t(indexConst) >= numActuals()) {
    return this;
  }

  MDefinition* arg = getArg(indexConst);
  if (arg->type() != MIRType::Value) {
    arg = MBox::New(alloc, arg);
  }

  return arg;
}

MGetInlinedArgumentHole* MGetInlinedArgumentHole::New(
    TempAllocator& alloc, MDefinition* index,
    MCreateInlinedArgumentsObject* args) {
  auto* ins = new (alloc) MGetInlinedArgumentHole();

  uint32_t argc = args->numActuals();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, index);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, args->getArg(i));
  }

  return ins;
}

MDefinition* MGetInlinedArgumentHole::foldsTo(TempAllocator& alloc) {
  MDefinition* indexDef = SkipUninterestingInstructions(index());
  if (!indexDef->isConstant() || indexDef->type() != MIRType::Int32) {
    return this;
  }

  int32_t indexConst = indexDef->toConstant()->toInt32();
  if (indexConst < 0) {
    return this;
  }

  MDefinition* arg;
  if (uint32_t(indexConst) < numActuals()) {
    arg = getArg(indexConst);

    if (arg->type() != MIRType::Value) {
      arg = MBox::New(alloc, arg);
    }
  } else {
    auto* undefined = MConstant::NewUndefined(alloc);
    block()->insertBefore(this, undefined);

    arg = MBox::New(alloc, undefined);
  }

  return arg;
}

MInlineArgumentsSlice* MInlineArgumentsSlice::New(
    TempAllocator& alloc, MDefinition* begin, MDefinition* count,
    MCreateInlinedArgumentsObject* args, JSObject* templateObj,
    gc::Heap initialHeap) {
  auto* ins = new (alloc) MInlineArgumentsSlice(templateObj, initialHeap);

  uint32_t argc = args->numActuals();
  MOZ_ASSERT(argc <= ArgumentsObject::MaxInlinedArgs);

  if (!ins->init(alloc, argc + NumNonArgumentOperands)) {
    return nullptr;
  }

  ins->initOperand(0, begin);
  ins->initOperand(1, count);
  for (uint32_t i = 0; i < argc; i++) {
    ins->initOperand(i + NumNonArgumentOperands, args->getArg(i));
  }

  return ins;
}

MDefinition* MArrayLength::foldsTo(TempAllocator& alloc) {
  // Object.keys() is potentially effectful, in case of Proxies. Otherwise, when
  // it is only computed for its length property, there is no need to
  // materialize the Array which results from it and it can be marked as
  // recovered on bailout as long as no properties are added to / removed from
  // the object.
  MDefinition* elems = elements();
  if (!elems->isElements()) {
    return this;
  }

  MDefinition* guardshape = elems->toElements()->object();
  if (!guardshape->isGuardShape()) {
    return this;
  }

  // The Guard shape is guarding the shape of the object returned by
  // Object.keys, this guard can be removed as knowing the function is good
  // enough to infer that we are returning an array.
  MDefinition* keys = guardshape->toGuardShape()->object();
  if (!keys->isObjectKeys()) {
    return this;
  }

  // Object.keys() inline cache guards against proxies when creating the IC. We
  // rely on this here as we are looking to elide `Object.keys(...)` call, which
  // is only possible if we know for sure that no side-effect might have
  // happened.
  MDefinition* noproxy = keys->toObjectKeys()->object();
  if (!noproxy->isGuardIsNotProxy()) {
    // The guard might have been replaced by an assertion, in case the class is
    // known at compile time. IF the guard has been removed check whether check
    // has been removed.
    MOZ_RELEASE_ASSERT(GetObjectKnownClass(noproxy) != KnownClass::None);
    MOZ_RELEASE_ASSERT(!GetObjectKnownJSClass(noproxy)->isProxyObject());
  }

  // Check if both the elements and the Object.keys() have a single use. We only
  // check for live uses, and are ok if a branch which was previously using the
  // keys array has been removed since.
  if (!elems->hasOneLiveDefUse() || !guardshape->hasOneLiveDefUse() ||
      !keys->hasOneLiveDefUse()) {
    return this;
  }

  // Check that the latest active resume point is the one from Object.keys(), in
  // order to steal it. If this is not the latest active resume point then some
  // side-effect might happen which updates the content of the object, making
  // any recovery of the keys exhibit a different behavior than expected.
  if (keys->toObjectKeys()->resumePoint() != block()->activeResumePoint(this)) {
    return this;
  }

  // Verify whether any resume point captures the keys array after any aliasing
  // mutations. If this were to be the case the recovery of ObjectKeys on
  // bailout might compute a version which might not match with the elided
  // result.
  //
  // Iterate over the resume point uses of ObjectKeys, and check whether the
  // instructions they are attached to are aliasing Object fields. If so, skip
  // this optimization.
  AliasSet enumKeysAliasSet = AliasSet::Load(AliasSet::Flag::ObjectFields);
  for (auto* use : UsesIterator(keys)) {
    if (!use->consumer()->isResumePoint()) {
      // There is only a single use, and this is the length computation as
      // asserted with `hasOneLiveDefUse`.
      continue;
    }

    MResumePoint* rp = use->consumer()->toResumePoint();
    if (!rp->instruction()) {
      // If there is no instruction, this is a resume point which is attached to
      // the entry of a block. Thus no risk of mutating the object on which the
      // keys are queried.
      continue;
    }

    MInstruction* ins = rp->instruction();
    if (ins == keys) {
      continue;
    }

    // Check whether the instruction can potentially alias the object fields of
    // the object from which we are querying the keys.
    AliasSet mightAlias = ins->getAliasSet() & enumKeysAliasSet;
    if (!mightAlias.isNone()) {
      return this;
    }
  }

  // Flag every instructions since Object.keys(..) as recovered on bailout, and
  // make Object.keys(..) be the recovered value in-place of the shape guard.
  setRecoveredOnBailout();
  elems->setRecoveredOnBailout();
  guardshape->replaceAllUsesWith(keys);
  guardshape->block()->discard(guardshape->toGuardShape());
  keys->setRecoveredOnBailout();

  // Steal the resume point from Object.keys, which is ok as we confirmed that
  // there is no other resume point in-between.
  MObjectKeysLength* keysLength = MObjectKeysLength::New(alloc, noproxy);
  keysLength->stealResumePoint(keys->toObjectKeys());

  // Set the dependency of the newly created instruction. Unfortunately
  // MObjectKeys (keys) is an instruction with a Store(Any) alias set, as it
  // could be used with proxies which can re-enter JavaScript.
  //
  // Thus, the loadDependency field of MObjectKeys is null. On the other hand
  // MObjectKeysLength has a Load alias set. Thus, instead of reconstructing the
  // Alias Analysis by updating every instructions which depends on MObjectKeys
  // and finding the matching store instruction, we reuse the MObjectKeys as any
  // store instruction, despite it being marked as recovered-on-bailout.
  keysLength->setDependency(keys);

  return keysLength;
}

MDefinition* MNormalizeSliceTerm::foldsTo(TempAllocator& alloc) {
  auto* length = this->length();
  if (!length->isConstant() && !length->isArgumentsLength()) {
    return this;
  }

  if (length->isConstant()) {
    int32_t lengthConst = length->toConstant()->toInt32();
    MOZ_ASSERT(lengthConst >= 0);

    // Result is always zero when |length| is zero.
    if (lengthConst == 0) {
      return length;
    }

    auto* value = this->value();
    if (value->isConstant()) {
      int32_t valueConst = value->toConstant()->toInt32();

      int32_t normalized;
      if (valueConst < 0) {
        normalized = std::max(valueConst + lengthConst, 0);
      } else {
        normalized = std::min(valueConst, lengthConst);
      }

      if (normalized == valueConst) {
        return value;
      }
      if (normalized == lengthConst) {
        return length;
      }
      return MConstant::NewInt32(alloc, normalized);
    }

    return this;
  }

  auto* value = this->value();
  if (value->isConstant()) {
    int32_t valueConst = value->toConstant()->toInt32();

    // Minimum of |value| and |length|.
    if (valueConst > 0) {
      return MMinMax::NewMin(alloc, value, length, MIRType::Int32);
    }

    // Maximum of |value + length| and zero.
    if (valueConst < 0) {
      // Safe to truncate because |length| is never negative.
      auto* add = MAdd::New(alloc, value, length, TruncateKind::Truncate);
      block()->insertBefore(this, add);

      auto* zero = MConstant::NewInt32(alloc, 0);
      block()->insertBefore(this, zero);

      return MMinMax::NewMax(alloc, add, zero, MIRType::Int32);
    }

    // Directly return the value when it's zero.
    return value;
  }

  // Normalizing MArgumentsLength is a no-op.
  if (value->isArgumentsLength()) {
    return value;
  }

  return this;
}

bool MInt32ToStringWithBase::congruentTo(const MDefinition* ins) const {
  if (!ins->isInt32ToStringWithBase()) {
    return false;
  }
  if (ins->toInt32ToStringWithBase()->lowerCase() != lowerCase()) {
    return false;
  }
  return congruentIfOperandsEqual(ins);
}
