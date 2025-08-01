/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Everything needed to build actual MIR instructions: the actual opcodes and
 * instructions, the instruction interface, and use chains.
 */

#ifndef jit_MIR_h
#define jit_MIR_h

#include "mozilla/Array.h"
#include "mozilla/EnumSet.h"
#include "mozilla/HashFunctions.h"
#ifdef JS_JITSPEW
#  include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#endif
#include "mozilla/MacroForEach.h"
#ifdef JS_JITSPEW
#  include "mozilla/Sprintf.h"
#  include "mozilla/Vector.h"
#endif

#include <algorithm>
#include <initializer_list>

#include "NamespaceImports.h"

#include "jit/AtomicOp.h"
#include "jit/FixedList.h"
#include "jit/InlineList.h"
#include "jit/JitAllocPolicy.h"
#include "jit/MacroAssembler.h"
#include "jit/MIROpsGenerated.h"
#include "jit/ShuffleAnalysis.h"
#include "jit/TypeData.h"
#include "jit/TypePolicy.h"
#include "js/experimental/JitInfo.h"  // JSJit{Getter,Setter}Op, JSJitInfo
#include "js/HeapAPI.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/Value.h"
#include "js/Vector.h"
#include "vm/BigIntType.h"
#include "vm/EnvironmentObject.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/JSContext.h"
#include "vm/RegExpObject.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmJS.h"  // for WasmInstanceObject
#include "wasm/WasmValType.h"

namespace JS {
struct ExpandoAndGeneration;
}

namespace js {

namespace wasm {
class FuncExport;
extern uint32_t MIRTypeToABIResultSize(jit::MIRType);
}  // namespace wasm

class JS_PUBLIC_API GenericPrinter;
class NativeIteratorListHead;
class StringObject;

enum class UnaryMathFunction : uint8_t;

bool CurrentThreadIsIonCompiling();

namespace jit {

class CallInfo;

#ifdef JS_JITSPEW
// Helper for debug printing.  Avoids creating a MIR.h <--> MIRGraph.h cycle.
// Implementation of this needs to see inside `MBasicBlock`; that is possible
// in MIR.cpp since it also includes MIRGraph.h, whereas this file does not.
class MBasicBlock;
uint32_t GetMBasicBlockId(const MBasicBlock* block);

// Helper class for debug printing.  This class allows `::getExtras` methods
// to add strings to be printed, on a per-MIR-node basis.  The strings are
// copied into storage owned by this class when `::add` is called, so the
// `::getExtras` methods do not need to be concerned about storage management.
class MOZ_STACK_CLASS ExtrasCollector {
  mozilla::Vector<UniqueChars, 4> strings_;

 public:
  // Add `str` to the collection.  A copy, owned by this object, is made.  In
  // case of OOM the call has no effect.
  void add(const char* str) {
    UniqueChars dup = DuplicateString(str);
    if (dup) {
      (void)strings_.append(std::move(dup));
    }
  }
  size_t count() const { return strings_.length(); }
  UniqueChars get(size_t ix) { return std::move(strings_[ix]); }
};
#endif

// Forward declarations of MIR types.
#define FORWARD_DECLARE(op) class M##op;
MIR_OPCODE_LIST(FORWARD_DECLARE)
#undef FORWARD_DECLARE

// MDefinition visitor which ignores non-overloaded visit functions.
class MDefinitionVisitorDefaultNoop {
 public:
#define VISIT_INS(op) \
  void visit##op(M##op*) {}
  MIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

class BytecodeSite;
class CompactBufferWriter;
class Range;

#define MIR_FLAG_LIST(_)                                                       \
  _(InWorklist)                                                                \
  _(EmittedAtUses)                                                             \
  _(Commutative)                                                               \
  _(Movable) /* Allow passes like LICM to move this instruction */             \
  _(Lowered) /* (Debug only) has a virtual register */                         \
  _(Guard)   /* Not removable if uses == 0 */                                  \
                                                                               \
  /* Flag an instruction to be considered as a Guard if the instructions       \
   * bails out on some inputs.                                                 \
   *                                                                           \
   * Some optimizations can replace an instruction, and leave its operands     \
   * unused. When the type information of the operand got used as a            \
   * predicate of the transformation, then we have to flag the operands as     \
   * GuardRangeBailouts.                                                       \
   *                                                                           \
   * This flag prevents further optimization of instructions, which            \
   * might remove the run-time checks (bailout conditions) used as a           \
   * predicate of the previous transformation.                                 \
   */                                                                          \
  _(GuardRangeBailouts)                                                        \
                                                                               \
  /* Some instructions have uses that aren't directly represented in the       \
   * graph, and need to be handled specially. As an example, this is used to   \
   * keep the flagged instruction in resume points, not substituting with an   \
   * UndefinedValue. This can be used by call inlining when a function         \
   * argument is not used by the inlined instructions. It is also used         \
   * to annotate instructions which were used in removed branches.             \
   */                                                                          \
  _(ImplicitlyUsed)                                                            \
                                                                               \
  /* The instruction has been marked dead for lazy removal from resume         \
   * points.                                                                   \
   */                                                                          \
  _(Unused)                                                                    \
                                                                               \
  /* Marks if the current instruction should go to the bailout paths instead   \
   * of producing code as part of the control flow.  This flag can only be set \
   * on instructions which are only used by ResumePoint or by other flagged    \
   * instructions.                                                             \
   */                                                                          \
  _(RecoveredOnBailout)                                                        \
                                                                               \
  /* Some instructions might represent an object, but the memory of these      \
   * objects might be incomplete if we have not recovered all the stores which \
   * were supposed to happen before. This flag is used to annotate             \
   * instructions which might return a pointer to a memory area which is not   \
   * yet fully initialized. This flag is used to ensure that stores are        \
   * executed before returning the value.                                      \
   */                                                                          \
  _(IncompleteObject)                                                          \
                                                                               \
  /* For WebAssembly, there are functions with multiple results.  Instead of   \
   * having the results defined by one call instruction, they are instead      \
   * captured in subsequent result capture instructions, because modelling     \
   * multi-value results in Ion is too complicated.  However since they        \
   * capture ambient live registers, it would be an error to move an unrelated \
   * instruction between the call and the result capture.  This flag is used   \
   * to prevent code motion from moving instructions in invalid ways.          \
   */                                                                          \
  _(CallResultCapture)                                                         \
                                                                               \
  /* The current instruction got discarded from the MIR Graph. This is useful  \
   * when we want to iterate over resume points and instructions, while        \
   * handling instructions which are discarded without reporting to the        \
   * iterator.                                                                 \
   */                                                                          \
  _(Discarded)

class MDefinition;
class MInstruction;
class MBasicBlock;
class MNode;
class MUse;
class MPhi;
class MIRGraph;
class MResumePoint;
class MControlInstruction;

// Represents a use of a node.
class MUse : public TempObject, public InlineListNode<MUse> {
  // Grant access to setProducerUnchecked.
  friend class MDefinition;
  friend class MPhi;

  MDefinition* producer_;  // MDefinition that is being used.
  MNode* consumer_;        // The node that is using this operand.

  // Low-level unchecked edit method for replaceAllUsesWith and
  // MPhi::removeOperand. This doesn't update use lists!
  // replaceAllUsesWith and MPhi::removeOperand do that manually.
  void setProducerUnchecked(MDefinition* producer) {
    MOZ_ASSERT(consumer_);
    MOZ_ASSERT(producer_);
    MOZ_ASSERT(producer);
    producer_ = producer;
  }

 public:
  // Default constructor for use in vectors.
  MUse() : producer_(nullptr), consumer_(nullptr) {}

  // Move constructor for use in vectors. When an MUse is moved, it stays
  // in its containing use list.
  MUse(MUse&& other)
      : InlineListNode<MUse>(std::move(other)),
        producer_(other.producer_),
        consumer_(other.consumer_) {}

  // Construct an MUse initialized with |producer| and |consumer|.
  MUse(MDefinition* producer, MNode* consumer) {
    initUnchecked(producer, consumer);
  }

  // Set this use, which was previously clear.
  inline void init(MDefinition* producer, MNode* consumer);
  // Like init, but works even when the use contains uninitialized data.
  inline void initUnchecked(MDefinition* producer, MNode* consumer);
  // Like initUnchecked, but set the producer to nullptr.
  inline void initUncheckedWithoutProducer(MNode* consumer);
  // Set this use, which was not previously clear.
  inline void replaceProducer(MDefinition* producer);
  // Clear this use.
  inline void releaseProducer();

  MDefinition* producer() const {
    MOZ_ASSERT(producer_ != nullptr);
    return producer_;
  }
  bool hasProducer() const { return producer_ != nullptr; }
  MNode* consumer() const {
    MOZ_ASSERT(consumer_ != nullptr);
    return consumer_;
  }

#ifdef DEBUG
  // Return the operand index of this MUse in its consumer. This is DEBUG-only
  // as normal code should instead call indexOf on the cast consumer directly,
  // to allow it to be devirtualized and inlined.
  size_t index() const;
#endif
};

using MUseIterator = InlineList<MUse>::iterator;

// A node is an entry in the MIR graph. It has two kinds:
//   MInstruction: an instruction which appears in the IR stream.
//   MResumePoint: a list of instructions that correspond to the state of the
//                 interpreter/Baseline stack.
//
// Nodes can hold references to MDefinitions. Each MDefinition has a list of
// nodes holding such a reference (its use chain).
class MNode : public TempObject {
 protected:
  enum class Kind { Definition = 0, ResumePoint };

 private:
  static const uintptr_t KindMask = 0x1;
  uintptr_t blockAndKind_;

  Kind kind() const { return Kind(blockAndKind_ & KindMask); }

 protected:
  explicit MNode(const MNode& other) : blockAndKind_(other.blockAndKind_) {}

  MNode(MBasicBlock* block, Kind kind) { setBlockAndKind(block, kind); }

  void setBlockAndKind(MBasicBlock* block, Kind kind) {
    blockAndKind_ = uintptr_t(block) | uintptr_t(kind);
    MOZ_ASSERT(this->block() == block);
  }

  MBasicBlock* definitionBlock() const {
    MOZ_ASSERT(isDefinition());
    static_assert(unsigned(Kind::Definition) == 0,
                  "Code below relies on low bit being 0");
    return reinterpret_cast<MBasicBlock*>(blockAndKind_);
  }
  MBasicBlock* resumePointBlock() const {
    MOZ_ASSERT(isResumePoint());
    static_assert(unsigned(Kind::ResumePoint) == 1,
                  "Code below relies on low bit being 1");
    // Use a subtraction: if the caller does block()->foo, the compiler
    // will be able to fold it with the load.
    return reinterpret_cast<MBasicBlock*>(blockAndKind_ - 1);
  }

 public:
  // Returns the definition at a given operand.
  virtual MDefinition* getOperand(size_t index) const = 0;
  virtual size_t numOperands() const = 0;
  virtual size_t indexOf(const MUse* u) const = 0;

  bool isDefinition() const { return kind() == Kind::Definition; }
  bool isResumePoint() const { return kind() == Kind::ResumePoint; }
  MBasicBlock* block() const {
    return reinterpret_cast<MBasicBlock*>(blockAndKind_ & ~KindMask);
  }
  MBasicBlock* caller() const;

  // Sets an already set operand, updating use information. If you're looking
  // for setOperand, this is probably what you want.
  virtual void replaceOperand(size_t index, MDefinition* operand) = 0;

  // Resets the operand to an uninitialized state, breaking the link
  // with the previous operand's producer.
  void releaseOperand(size_t index) { getUseFor(index)->releaseProducer(); }
  bool hasOperand(size_t index) const {
    return getUseFor(index)->hasProducer();
  }

  inline MDefinition* toDefinition();
  inline MResumePoint* toResumePoint();

  [[nodiscard]] virtual bool writeRecoverData(
      CompactBufferWriter& writer) const;

#ifdef JS_JITSPEW
  virtual void dump(GenericPrinter& out) const = 0;
  virtual void dump() const = 0;
#endif

 protected:
  // Need visibility on getUseFor to avoid O(n^2) complexity.
  friend void AssertBasicGraphCoherency(MIRGraph& graph, bool force);

  // Gets the MUse corresponding to given operand.
  virtual MUse* getUseFor(size_t index) = 0;
  virtual const MUse* getUseFor(size_t index) const = 0;
};

class AliasSet {
 private:
  uint32_t flags_;

 public:
  enum Flag {
    None_ = 0,
    ObjectFields = 1 << 0,      // shape, class, slots, length etc.
    Element = 1 << 1,           // A Value member of obj->elements or
                                // a typed object.
    UnboxedElement = 1 << 2,    // An unboxed scalar or reference member of
                                // typed object.
    DynamicSlot = 1 << 3,       // A Value member of obj->slots.
    FixedSlot = 1 << 4,         // A Value member of obj->fixedSlots().
    DOMProperty = 1 << 5,       // A DOM property
    WasmInstanceData = 1 << 6,  // An asm.js/wasm private global var
    WasmHeap = 1 << 7,          // An asm.js/wasm heap load
    WasmHeapMeta = 1 << 8,      // The asm.js/wasm heap base pointer and
                                // bounds check limit, in Instance.
    ArrayBufferViewLengthOrOffset =
        1 << 9,                  // An array buffer view's length or byteOffset
    WasmGlobalCell = 1 << 10,    // A wasm global cell
    WasmTableElement = 1 << 11,  // An element of a wasm table
    WasmTableMeta = 1 << 12,     // A wasm table elements pointer and
                                 // length field, in instance data.
    WasmStackResult = 1 << 13,   // A stack result from the current function

    // JSContext's exception state. This is used on instructions like MThrow
    // or MNewArrayDynamicLength that throw exceptions (other than OOM) but have
    // no other side effect, to ensure that they get their own up-to-date resume
    // point. (This resume point will be used when constructing the Baseline
    // frame during exception bailouts.)
    ExceptionState = 1 << 14,

    // Used for instructions that load the privateSlot of DOM proxies and
    // the ExpandoAndGeneration.
    DOMProxyExpando = 1 << 15,

    // Hash table of a Map/Set/WeakMap/WeakSet object.
    MapOrSetHashTable = 1 << 16,

    // Internal state of the random number generator
    RNG = 1 << 17,

    // The pendingException slot on the wasm instance object.
    WasmPendingException = 1 << 18,

    // The fuzzilliHash slot
    FuzzilliHash = 1 << 19,

    // The WasmStructObject::inlineData_[..] storage area
    WasmStructInlineDataArea = 1 << 20,

    // The WasmStructObject::outlineData_ pointer only
    WasmStructOutlineDataPointer = 1 << 21,

    // The malloc'd block that WasmStructObject::outlineData_ points at
    WasmStructOutlineDataArea = 1 << 22,

    // The WasmArrayObject::numElements_ field
    WasmArrayNumElements = 1 << 23,

    // The WasmArrayObject::data_ pointer only
    WasmArrayDataPointer = 1 << 24,

    // The malloc'd block that WasmArrayObject::data_ points at
    WasmArrayDataArea = 1 << 25,

    // The generation counter associated with the global object
    GlobalGenerationCounter = 1 << 26,

    // The SharedArrayRawBuffer::length field.
    SharedArrayRawBufferLength = 1 << 27,

    Last = SharedArrayRawBufferLength,

    Any = Last | (Last - 1),
    NumCategories = 28,

    // Indicates load or store.
    Store_ = 1 << 31
  };

  static_assert((1 << NumCategories) - 1 == Any,
                "NumCategories must include all flags present in Any");

  explicit constexpr AliasSet(uint32_t flags) : flags_(flags) {}

 public:
  inline constexpr bool isNone() const { return flags_ == None_; }
  constexpr uint32_t flags() const { return flags_ & Any; }
  inline constexpr bool isStore() const { return !!(flags_ & Store_); }
  inline constexpr bool isLoad() const { return !isStore() && !isNone(); }
  inline constexpr AliasSet operator|(const AliasSet& other) const {
    return AliasSet(flags_ | other.flags_);
  }
  inline constexpr AliasSet operator&(const AliasSet& other) const {
    return AliasSet(flags_ & other.flags_);
  }
  inline constexpr AliasSet operator~() const { return AliasSet(~flags_); }
  static constexpr AliasSet None() { return AliasSet(None_); }
  static constexpr AliasSet Load(uint32_t flags) {
    MOZ_ASSERT(flags && !(flags & Store_));
    return AliasSet(flags);
  }
  static constexpr AliasSet Store(uint32_t flags) {
    MOZ_ASSERT(flags && !(flags & Store_));
    return AliasSet(flags | Store_);
  }
};

using MDefinitionVector = Vector<MDefinition*, 6, JitAllocPolicy>;
using MInstructionVector = Vector<MInstruction*, 6, JitAllocPolicy>;

// When a floating-point value is used by nodes which would prefer to
// receive integer inputs, we may be able to help by computing our result
// into an integer directly.
//
// A value can be truncated in 4 differents ways:
//   1. Ignore Infinities (x / 0 --> 0).
//   2. Ignore overflow (INT_MIN / -1 == (INT_MAX + 1) --> INT_MIN)
//   3. Ignore negative zeros. (-0 --> 0)
//   4. Ignore remainder. (3 / 4 --> 0)
//
// Indirect truncation is used to represent that we are interested in the
// truncated result, but only if it can safely flow into operations which
// are computed modulo 2^32, such as (2) and (3). Infinities are not safe,
// as they would have absorbed other math operations. Remainders are not
// safe, as fractions can be scaled up by multiplication.
//
// Division is a particularly interesting node here because it covers all 4
// cases even when its own operands are integers.
//
// Note that these enum values are ordered from least value-modifying to
// most value-modifying, and code relies on this ordering.
enum class TruncateKind {
  // No correction.
  NoTruncate = 0,
  // An integer is desired, but we can't skip bailout checks.
  TruncateAfterBailouts = 1,
  // The value will be truncated after some arithmetic (see above).
  IndirectTruncate = 2,
  // Direct and infallible truncation to int32.
  Truncate = 3
};

// An MDefinition is an SSA name.
class MDefinition : public MNode {
  friend class MBasicBlock;

 public:
  enum class Opcode : uint16_t {
#define DEFINE_OPCODES(op) op,
    MIR_OPCODE_LIST(DEFINE_OPCODES)
#undef DEFINE_OPCODES
  };

 private:
  InlineList<MUse> uses_;  // Use chain.
  uint32_t id_;            // Instruction ID, which after block re-ordering
                           // is sorted within a basic block.
  Opcode op_;              // Opcode.
  uint16_t flags_;         // Bit flags.
  Range* range_;           // Any computed range for this def.
  union {
    MDefinition*
        loadDependency_;  // Implicit dependency (store, call, etc.) of this
                          // instruction. Used by alias analysis, GVN and LICM.
    uint32_t virtualRegister_;  // Used by lowering to map definitions to
                                // virtual registers.
  };

  // Track bailouts by storing the current pc in MIR instruction. Also used
  // for profiling and keeping track of what the last known pc was.
  const BytecodeSite* trackedSite_;

  // For nodes of MIRType::WasmAnyRef, a type precisely describing the value of
  // the node. It is set by the "Track wasm ref types" pass in Ion, and enables
  // GVN and LICM to perform more advanced optimizations (such as allowing
  // instructions to move if the source values are non-null, or omitting casts
  // that are statically known to succeed or fail).
  wasm::MaybeRefType wasmRefType_;

  // If we generate a bailout path for this instruction, this is the
  // bailout kind that will be encoded in the snapshot. When we bail out,
  // FinishBailoutToBaseline may take action based on the bailout kind to
  // prevent bailout loops. (For example, if an instruction bails out after
  // being hoisted by LICM, we will disable LICM when recompiling the script.)
  BailoutKind bailoutKind_;

  MIRType resultType_;  // Representation of result type.

 private:
  enum Flag {
    None = 0,
#define DEFINE_FLAG(flag) flag,
    MIR_FLAG_LIST(DEFINE_FLAG)
#undef DEFINE_FLAG
        Total
  };

  bool hasFlags(uint32_t flags) const { return (flags_ & flags) == flags; }
  void removeFlags(uint32_t flags) { flags_ &= ~flags; }
  void setFlags(uint32_t flags) { flags_ |= flags; }

  // Calling isDefinition or isResumePoint on MDefinition is unnecessary.
  bool isDefinition() const = delete;
  bool isResumePoint() const = delete;

 protected:
  void setInstructionBlock(MBasicBlock* block, const BytecodeSite* site) {
    MOZ_ASSERT(isInstruction());
    setBlockAndKind(block, Kind::Definition);
    setTrackedSite(site);
  }

  void setPhiBlock(MBasicBlock* block) {
    MOZ_ASSERT(isPhi());
    setBlockAndKind(block, Kind::Definition);
  }

  static HashNumber addU32ToHash(HashNumber hash, uint32_t data) {
    return data + (hash << 6) + (hash << 16) - hash;
  }

  static HashNumber addU64ToHash(HashNumber hash, uint64_t data) {
    hash = addU32ToHash(hash, uint32_t(data));
    hash = addU32ToHash(hash, uint32_t(data >> 32));
    return hash;
  }

 public:
  explicit MDefinition(Opcode op)
      : MNode(nullptr, Kind::Definition),
        id_(0),
        op_(op),
        flags_(0),
        range_(nullptr),
        loadDependency_(nullptr),
        trackedSite_(nullptr),
        bailoutKind_(BailoutKind::Unknown),
        resultType_(MIRType::None) {}

  // Copying a definition leaves the list of uses empty.
  explicit MDefinition(const MDefinition& other)
      : MNode(other),
        id_(0),
        op_(other.op_),
        flags_(other.flags_),
        range_(other.range_),
        loadDependency_(other.loadDependency_),
        trackedSite_(other.trackedSite_),
        bailoutKind_(other.bailoutKind_),
        resultType_(other.resultType_) {}

  Opcode op() const { return op_; }

#ifdef JS_JITSPEW
  const char* opName() const;
  void printName(GenericPrinter& out) const;
  static void PrintOpcodeName(GenericPrinter& out, Opcode op);
  virtual void printOpcode(GenericPrinter& out) const;
  void dump(GenericPrinter& out) const override;
  void dump() const override;
  void dumpLocation(GenericPrinter& out) const;
  void dumpLocation() const;
  // Dump any other stuff the node wants to have printed in `extras`.  The
  // added strings are copied, with the `ExtrasCollector` taking ownership of
  // the copies.
  virtual void getExtras(ExtrasCollector* extras) const {}
#endif

  // Also for LICM. Test whether this definition is likely to be a call, which
  // would clobber all or many of the floating-point registers, such that
  // hoisting floating-point constants out of containing loops isn't likely to
  // be worthwhile.
  virtual bool possiblyCalls() const { return false; }

  MBasicBlock* block() const { return definitionBlock(); }

 private:
  void setTrackedSite(const BytecodeSite* site) {
    MOZ_ASSERT(site);
    trackedSite_ = site;
  }

 public:
  const BytecodeSite* trackedSite() const {
    MOZ_ASSERT(trackedSite_,
               "missing tracked bytecode site; node not assigned to a block?");
    return trackedSite_;
  }

  BailoutKind bailoutKind() const { return bailoutKind_; }
  void setBailoutKind(BailoutKind kind) { bailoutKind_ = kind; }

  // Return the range of this value, *before* any bailout checks. Contrast
  // this with the type() method, and the Range constructor which takes an
  // MDefinition*, which describe the value *after* any bailout checks.
  //
  // Warning: Range analysis is removing the bit-operations such as '| 0' at
  // the end of the transformations. Using this function to analyse any
  // operands after the truncate phase of the range analysis will lead to
  // errors. Instead, one should define the collectRangeInfoPreTrunc() to set
  // the right set of flags which are dependent on the range of the inputs.
  Range* range() const {
    MOZ_ASSERT(type() != MIRType::None);
    return range_;
  }
  void setRange(Range* range) {
    MOZ_ASSERT(type() != MIRType::None);
    range_ = range;
  }

  virtual HashNumber valueHash() const;
  virtual bool congruentTo(const MDefinition* ins) const { return false; }
  const MDefinition* skipObjectGuards() const;

  // Note that, for a call `congruentIfOperandsEqual(ins)` inside some class
  // MFoo, if `true` is returned then we are ensured that `ins` is also an
  // MFoo, so it is safe to do `ins->toMFoo()` without first checking whether
  // `ins->isMFoo()`.
  bool congruentIfOperandsEqual(const MDefinition* ins) const;

  virtual MDefinition* foldsTo(TempAllocator& alloc);
  virtual void analyzeEdgeCasesForward();
  virtual void analyzeEdgeCasesBackward();

  // |canTruncate| reports if this instruction supports truncation. If
  // |canTruncate| function returns true, then the |truncate| function is
  // called on the same instruction to mutate the instruction, such as updating
  // the return type, the range and the specialization of the instruction.
  virtual bool canTruncate() const;
  virtual void truncate(TruncateKind kind);

  // Determine what kind of truncate this node prefers for the operand at the
  // given index.
  virtual TruncateKind operandTruncateKind(size_t index) const;

  // Compute an absolute or symbolic range for the value of this node.
  virtual void computeRange(TempAllocator& alloc) {}

  // Collect information from the pre-truncated ranges.
  virtual void collectRangeInfoPreTrunc() {}

  uint32_t id() const {
    MOZ_ASSERT(block());
    return id_;
  }
  void setId(uint32_t id) { id_ = id; }

#define FLAG_ACCESSOR(flag)                            \
  bool is##flag() const {                              \
    static_assert(Flag::Total <= sizeof(flags_) * 8,   \
                  "Flags should fit in flags_ field"); \
    return hasFlags(1 << flag);                        \
  }                                                    \
  void set##flag() {                                   \
    MOZ_ASSERT(!hasFlags(1 << flag));                  \
    setFlags(1 << flag);                               \
  }                                                    \
  void setNot##flag() {                                \
    MOZ_ASSERT(hasFlags(1 << flag));                   \
    removeFlags(1 << flag);                            \
  }                                                    \
  void set##flag##Unchecked() { setFlags(1 << flag); } \
  void setNot##flag##Unchecked() { removeFlags(1 << flag); }

  MIR_FLAG_LIST(FLAG_ACCESSOR)
#undef FLAG_ACCESSOR

  bool hasAnyFlags() const { return flags_ != 0; }

  // Return the type of this value. This may be speculative, and enforced
  // dynamically with the use of bailout checks. If all the bailout checks
  // pass, the value will have this type.
  //
  // Unless this is an MUrsh that has bailouts disabled, which, as a special
  // case, may return a value in (INT32_MAX,UINT32_MAX] even when its type()
  // is MIRType::Int32.
  MIRType type() const { return resultType_; }

  // Default EnumSet serialization is based on the enum's underlying type, which
  // means uint8_t for MIRType. To store all possible MIRType values we need at
  // least uint32_t.
  using MIRTypeEnumSet = mozilla::EnumSet<MIRType, uint32_t>;
  static_assert(static_cast<size_t>(MIRType::Last) <
                sizeof(MIRTypeEnumSet::serializedType) * CHAR_BIT);

  // Get the wasm reference type stored on the node. Do NOT use in congruentTo,
  // as this value can change throughout the optimization process. See
  // ReplaceAllUsesWith in ValueNumbering.cpp.
  wasm::MaybeRefType wasmRefType() const { return wasmRefType_; }

  // Sets the wasm reference type stored on the node. Does not check if there
  // was already a type on the node, which may lead to bugs; consider using
  // `initWasmRefType` instead if it applies.
  void setWasmRefType(wasm::MaybeRefType refType) { wasmRefType_ = refType; }

  // Sets the wasm reference type stored on the node. To be used for nodes that
  // have a fixed ref type that is set up front, which is a common case. Must be
  // called only during the node constructor and never again afterward.
  void initWasmRefType(wasm::MaybeRefType refType) {
    MOZ_ASSERT(!wasmRefType_);
    setWasmRefType(refType);
  }

  // Compute the wasm reference type for this node. This method is called by
  // updateWasmRefType. By default it returns the ref type stored on the node,
  // which means it will return either Nothing or a value set by
  // initWasmRefType.
  virtual wasm::MaybeRefType computeWasmRefType() const { return wasmRefType_; }

  // Return true if the result type is a member of the given types.
  bool typeIsOneOf(MIRTypeEnumSet types) const {
    MOZ_ASSERT(!types.isEmpty());
    return types.contains(type());
  }

  // Float32 specialization operations (see big comment in IonAnalysis before
  // the Float32 specialization algorithm).
  virtual bool isFloat32Commutative() const { return false; }
  virtual bool canProduceFloat32() const { return false; }
  virtual bool canConsumeFloat32(MUse* use) const { return false; }
  virtual void trySpecializeFloat32(TempAllocator& alloc) {}
#ifdef DEBUG
  // Used during the pass that checks that Float32 flow into valid MDefinitions
  virtual bool isConsistentFloat32Use(MUse* use) const {
    return type() == MIRType::Float32 || canConsumeFloat32(use);
  }
#endif

  // Returns the beginning of this definition's use chain.
  MUseIterator usesBegin() const { return uses_.begin(); }

  // Returns the end of this definition's use chain.
  MUseIterator usesEnd() const { return uses_.end(); }

  bool canEmitAtUses() const { return !isEmittedAtUses(); }

  // Removes a use at the given position
  void removeUse(MUse* use) { uses_.remove(use); }

#if defined(DEBUG) || defined(JS_JITSPEW)
  // Number of uses of this instruction. This function is only available
  // in DEBUG mode since it requires traversing the list. Most users should
  // use hasUses() or hasOneUse() instead.
  size_t useCount() const;

  // Number of uses of this instruction (only counting MDefinitions, ignoring
  // MResumePoints). This function is only available in DEBUG mode since it
  // requires traversing the list. Most users should use hasUses() or
  // hasOneUse() instead.
  size_t defUseCount() const;
#endif

  // Test whether this MDefinition has exactly one use.
  bool hasOneUse() const;

  // Test whether this MDefinition has exactly one use.
  // (only counting MDefinitions, ignoring MResumePoints)
  bool hasOneDefUse() const;

  // Test whether this MDefinition has exactly one live use. (only counting
  // MDefinitions which are not recovered on bailout and ignoring MResumePoints)
  bool hasOneLiveDefUse() const;

  // Test whether this MDefinition has at least one use.
  // (only counting MDefinitions, ignoring MResumePoints)
  bool hasDefUses() const;

  // Test whether this MDefinition has at least one non-recovered use.
  // (only counting MDefinitions, ignoring MResumePoints)
  bool hasLiveDefUses() const;

  bool hasUses() const { return !uses_.empty(); }

  // If this MDefinition has a single use (ignoring MResumePoints), returns that
  // use's definition. Else returns nullptr.
  MDefinition* maybeSingleDefUse() const;

  // Returns the most recently added use (ignoring MResumePoints) for this
  // MDefinition. Returns nullptr if there are no uses. Note that this relies on
  // addUse adding new uses to the front of the list, and should only be called
  // during MIR building (before optimization passes make changes to the uses).
  MDefinition* maybeMostRecentlyAddedDefUse() const;

  void addUse(MUse* use) {
    MOZ_ASSERT(use->producer() == this);
    uses_.pushFront(use);
  }
  void addUseUnchecked(MUse* use) {
    MOZ_ASSERT(use->producer() == this);
    uses_.pushFrontUnchecked(use);
  }
  void replaceUse(MUse* old, MUse* now) {
    MOZ_ASSERT(now->producer() == this);
    uses_.replace(old, now);
  }

  // Replace the current instruction by a dominating instruction |dom| in all
  // uses of the current instruction.
  void replaceAllUsesWith(MDefinition* dom);

  // Like replaceAllUsesWith, but doesn't set ImplicitlyUsed on |this|'s
  // operands.
  void justReplaceAllUsesWith(MDefinition* dom);

  // Replace the current instruction by an optimized-out constant in all uses
  // of the current instruction. Note, that optimized-out constant should not
  // be observed, and thus they should not flow in any computation.
  [[nodiscard]] bool optimizeOutAllUses(TempAllocator& alloc);

  // Replace the current instruction by a dominating instruction |dom| in all
  // instruction, but keep the current instruction for resume point and
  // instruction which are recovered on bailouts.
  void replaceAllLiveUsesWith(MDefinition* dom);

  void setVirtualRegister(uint32_t vreg) {
    virtualRegister_ = vreg;
    setLoweredUnchecked();
  }
  uint32_t virtualRegister() const {
    MOZ_ASSERT(isLowered());
    return virtualRegister_;
  }

 public:
  // Opcode testing and casts.
  template <typename MIRType>
  bool is() const {
    return op() == MIRType::classOpcode;
  }
  template <typename MIRType>
  MIRType* to() {
    MOZ_ASSERT(this->is<MIRType>());
    return static_cast<MIRType*>(this);
  }
  template <typename MIRType>
  const MIRType* to() const {
    MOZ_ASSERT(this->is<MIRType>());
    return static_cast<const MIRType*>(this);
  }
#define OPCODE_CASTS(opcode)                                \
  bool is##opcode() const { return this->is<M##opcode>(); } \
  M##opcode* to##opcode() { return this->to<M##opcode>(); } \
  const M##opcode* to##opcode() const { return this->to<M##opcode>(); }
  MIR_OPCODE_LIST(OPCODE_CASTS)
#undef OPCODE_CASTS

  inline MConstant* maybeConstantValue();

  inline MInstruction* toInstruction();
  inline const MInstruction* toInstruction() const;
  bool isInstruction() const { return !isPhi(); }

  virtual bool isControlInstruction() const { return false; }
  inline MControlInstruction* toControlInstruction();

  void setResultType(MIRType type) { resultType_ = type; }
  virtual AliasSet getAliasSet() const {
    // Instructions are effectful by default.
    return AliasSet::Store(AliasSet::Any);
  }

#ifdef DEBUG
  bool hasDefaultAliasSet() const {
    AliasSet set = getAliasSet();
    return set.isStore() && set.flags() == AliasSet::Flag::Any;
  }
#endif

  MDefinition* dependency() const {
    if (getAliasSet().isStore()) {
      return nullptr;
    }
    return loadDependency_;
  }
  void setDependency(MDefinition* dependency) {
    MOZ_ASSERT(!getAliasSet().isStore());
    loadDependency_ = dependency;
  }
  bool isEffectful() const { return getAliasSet().isStore(); }

#ifdef DEBUG
  bool needsResumePoint() const {
    // Return whether this instruction should have its own resume point.
    return isEffectful();
  }
#endif

  enum class AliasType : uint32_t { NoAlias = 0, MayAlias = 1, MustAlias = 2 };
  virtual AliasType mightAlias(const MDefinition* store) const {
    // Return whether this load may depend on the specified store, given
    // that the alias sets intersect. This may be refined to exclude
    // possible aliasing in cases where alias set flags are too imprecise.
    if (!(getAliasSet().flags() & store->getAliasSet().flags())) {
      return AliasType::NoAlias;
    }
    MOZ_ASSERT(!isEffectful() && store->isEffectful());
    return AliasType::MayAlias;
  }

  virtual bool canRecoverOnBailout() const { return false; }
};

// An MUseDefIterator walks over uses in a definition, skipping any use that is
// not a definition. Items from the use list must not be deleted during
// iteration.
class MUseDefIterator {
  const MDefinition* def_;
  MUseIterator current_;

  MUseIterator search(MUseIterator start) {
    MUseIterator i(start);
    for (; i != def_->usesEnd(); i++) {
      if (i->consumer()->isDefinition()) {
        return i;
      }
    }
    return def_->usesEnd();
  }

 public:
  explicit MUseDefIterator(const MDefinition* def)
      : def_(def), current_(search(def->usesBegin())) {}

  explicit operator bool() const { return current_ != def_->usesEnd(); }
  MUseDefIterator operator++() {
    MOZ_ASSERT(current_ != def_->usesEnd());
    ++current_;
    current_ = search(current_);
    return *this;
  }
  MUseDefIterator operator++(int) {
    MUseDefIterator old(*this);
    operator++();
    return old;
  }
  MUse* use() const { return *current_; }
  MDefinition* def() const { return current_->consumer()->toDefinition(); }
};

// Helper class to check that GC pointers embedded in MIR instructions are not
// in the nursery. Off-thread compilation and nursery GCs can happen in
// parallel. Nursery pointers are handled with MNurseryObject and the
// nurseryObjects lists in WarpSnapshot and IonScript.
//
// These GC things are rooted through the WarpSnapshot. Compacting GCs cancel
// off-thread compilations.
template <typename T>
class CompilerGCPointer {
  js::gc::Cell* ptr_;

 public:
  explicit CompilerGCPointer(T ptr) : ptr_(ptr) {
    MOZ_ASSERT_IF(ptr, !IsInsideNursery(ptr));
    MOZ_ASSERT_IF(!CurrentThreadIsIonCompiling(), TlsContext.get()->suppressGC);
  }

  operator T() const { return static_cast<T>(ptr_); }
  T operator->() const { return static_cast<T>(ptr_); }

 private:
  CompilerGCPointer() = delete;
  CompilerGCPointer(const CompilerGCPointer<T>&) = delete;
  CompilerGCPointer<T>& operator=(const CompilerGCPointer<T>&) = delete;
};

using CompilerObject = CompilerGCPointer<JSObject*>;
using CompilerNativeObject = CompilerGCPointer<NativeObject*>;
using CompilerFunction = CompilerGCPointer<JSFunction*>;
using CompilerBaseScript = CompilerGCPointer<BaseScript*>;
using CompilerPropertyName = CompilerGCPointer<PropertyName*>;
using CompilerShape = CompilerGCPointer<Shape*>;
using CompilerGetterSetter = CompilerGCPointer<GetterSetter*>;

// An instruction is an SSA name that is inserted into a basic block's IR
// stream.
class MInstruction : public MDefinition, public InlineListNode<MInstruction> {
  MResumePoint* resumePoint_;

 protected:
  // All MInstructions are using the "MFoo::New(alloc)" notation instead of
  // the TempObject new operator. This code redefines the new operator as
  // protected, and delegates to the TempObject new operator. Thus, the
  // following code prevents calls to "new(alloc) MFoo" outside the MFoo
  // members.
  inline void* operator new(size_t nbytes,
                            TempAllocator::Fallible view) noexcept(true) {
    return TempObject::operator new(nbytes, view);
  }
  inline void* operator new(size_t nbytes, TempAllocator& alloc) {
    return TempObject::operator new(nbytes, alloc);
  }
  template <class T>
  inline void* operator new(size_t nbytes, T* pos) {
    return TempObject::operator new(nbytes, pos);
  }

 public:
  explicit MInstruction(Opcode op) : MDefinition(op), resumePoint_(nullptr) {}

  // Copying an instruction leaves the resume point as empty.
  explicit MInstruction(const MInstruction& other)
      : MDefinition(other), resumePoint_(nullptr) {}

  // Convenient function used for replacing a load by the value of the store
  // if the types are match, and boxing the value if they do not match.
  MDefinition* foldsToStore(TempAllocator& alloc);

  void setResumePoint(MResumePoint* resumePoint);
  void stealResumePoint(MInstruction* other);

  void moveResumePointAsEntry();
  void clearResumePoint();
  MResumePoint* resumePoint() const { return resumePoint_; }

  // For instructions which can be cloned with new inputs, with all other
  // information being the same. clone() implementations do not need to worry
  // about cloning generic MInstruction/MDefinition state like flags and
  // resume points.
  virtual bool canClone() const { return false; }
  virtual MInstruction* clone(TempAllocator& alloc,
                              const MDefinitionVector& inputs) const {
    MOZ_CRASH();
  }

  // Instructions needing to hook into type analysis should return a
  // TypePolicy.
  virtual const TypePolicy* typePolicy() = 0;
  virtual MIRType typePolicySpecialization() = 0;
};

// Note: GenerateOpcodeFiles.py generates MOpcodesGenerated.h based on the
// INSTRUCTION_HEADER* macros.
#define INSTRUCTION_HEADER_WITHOUT_TYPEPOLICY(opcode) \
  static const Opcode classOpcode = Opcode::opcode;   \
  using MThisOpcode = M##opcode;

#define INSTRUCTION_HEADER(opcode)                 \
  INSTRUCTION_HEADER_WITHOUT_TYPEPOLICY(opcode)    \
  virtual const TypePolicy* typePolicy() override; \
  virtual MIRType typePolicySpecialization() override;

#define ALLOW_CLONE(typename)                                                \
  bool canClone() const override { return true; }                            \
  MInstruction* clone(TempAllocator& alloc, const MDefinitionVector& inputs) \
      const override {                                                       \
    MOZ_ASSERT(numOperands() == inputs.length());                            \
    MInstruction* res = new (alloc) typename(*this);                         \
    if (!res) {                                                              \
      return nullptr;                                                        \
    }                                                                        \
    for (size_t i = 0; i < numOperands(); i++) {                             \
      res->replaceOperand(i, inputs[i]);                                     \
    }                                                                        \
    return res;                                                              \
  }

// Adds MFoo::New functions which are mirroring the arguments of the
// constructors. Opcodes which are using this macro can be called with a
// TempAllocator, or the fallible version of the TempAllocator.
#define TRIVIAL_NEW_WRAPPERS                                               \
  template <typename... Args>                                              \
  static MThisOpcode* New(TempAllocator& alloc, Args&&... args) {          \
    return new (alloc) MThisOpcode(std::forward<Args>(args)...);           \
  }                                                                        \
  template <typename... Args>                                              \
  static MThisOpcode* New(TempAllocator::Fallible alloc, Args&&... args) { \
    return new (alloc) MThisOpcode(std::forward<Args>(args)...);           \
  }

// These macros are used as a syntactic sugar for writting getOperand
// accessors. They are meant to be used in the body of MIR Instructions as
// follows:
//
//   public:
//     INSTRUCTION_HEADER(Foo)
//     NAMED_OPERANDS((0, lhs), (1, rhs))
//
// The above example defines 2 accessors, one named "lhs" accessing the first
// operand, and a one named "rhs" accessing the second operand.
#define NAMED_OPERAND_ACCESSOR(Index, Name) \
  MDefinition* Name() const { return getOperand(Index); }
#define NAMED_OPERAND_ACCESSOR_APPLY(Args) NAMED_OPERAND_ACCESSOR Args
#define NAMED_OPERANDS(...) \
  MOZ_FOR_EACH(NAMED_OPERAND_ACCESSOR_APPLY, (), (__VA_ARGS__))

template <size_t Arity>
class MAryInstruction : public MInstruction {
  mozilla::Array<MUse, Arity> operands_;

 protected:
  MUse* getUseFor(size_t index) final { return &operands_[index]; }
  const MUse* getUseFor(size_t index) const final { return &operands_[index]; }
  void initOperand(size_t index, MDefinition* operand) {
    operands_[index].init(operand, this);
  }

 public:
  MDefinition* getOperand(size_t index) const final {
    return operands_[index].producer();
  }
  size_t numOperands() const final { return Arity; }
#ifdef DEBUG
  static const size_t staticNumOperands = Arity;
#endif
  size_t indexOf(const MUse* u) const final {
    MOZ_ASSERT(u >= &operands_[0]);
    MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
    return u - &operands_[0];
  }
  void replaceOperand(size_t index, MDefinition* operand) final {
    operands_[index].replaceProducer(operand);
  }

  explicit MAryInstruction(Opcode op) : MInstruction(op) {}

  explicit MAryInstruction(const MAryInstruction<Arity>& other)
      : MInstruction(other) {
    for (int i = 0; i < (int)Arity;
         i++) {  // N.B. use |int| to avoid warnings when Arity == 0
      operands_[i].init(other.operands_[i].producer(), this);
    }
  }
};

class MNullaryInstruction : public MAryInstruction<0>,
                            public NoTypePolicy::Data {
 protected:
  explicit MNullaryInstruction(Opcode op) : MAryInstruction(op) {}

  HashNumber valueHash() const override;
};

class MUnaryInstruction : public MAryInstruction<1> {
 protected:
  MUnaryInstruction(Opcode op, MDefinition* ins) : MAryInstruction(op) {
    initOperand(0, ins);
  }

  HashNumber valueHash() const override;

 public:
  NAMED_OPERANDS((0, input))
};

class MBinaryInstruction : public MAryInstruction<2> {
 protected:
  MBinaryInstruction(Opcode op, MDefinition* left, MDefinition* right)
      : MAryInstruction(op) {
    initOperand(0, left);
    initOperand(1, right);
  }

 public:
  NAMED_OPERANDS((0, lhs), (1, rhs))

 protected:
  HashNumber valueHash() const override;

  bool binaryCongruentTo(const MDefinition* ins) const {
    if (op() != ins->op()) {
      return false;
    }

    if (type() != ins->type()) {
      return false;
    }

    if (isEffectful() || ins->isEffectful()) {
      return false;
    }

    const MDefinition* left = getOperand(0);
    const MDefinition* right = getOperand(1);
    if (isCommutative() && left->id() > right->id()) {
      std::swap(left, right);
    }

    const MBinaryInstruction* bi = static_cast<const MBinaryInstruction*>(ins);
    const MDefinition* insLeft = bi->getOperand(0);
    const MDefinition* insRight = bi->getOperand(1);
    if (bi->isCommutative() && insLeft->id() > insRight->id()) {
      std::swap(insLeft, insRight);
    }

    return left == insLeft && right == insRight;
  }

 public:
  // Return if the operands to this instruction are both unsigned.
  static bool unsignedOperands(MDefinition* left, MDefinition* right);
  bool unsignedOperands();

  // Replace any wrapping operands with the underlying int32 operands
  // in case of unsigned operands.
  void replaceWithUnsignedOperands();
};

class MTernaryInstruction : public MAryInstruction<3> {
 protected:
  MTernaryInstruction(Opcode op, MDefinition* first, MDefinition* second,
                      MDefinition* third)
      : MAryInstruction(op) {
    initOperand(0, first);
    initOperand(1, second);
    initOperand(2, third);
  }

  HashNumber valueHash() const override;
};

class MQuaternaryInstruction : public MAryInstruction<4> {
 protected:
  MQuaternaryInstruction(Opcode op, MDefinition* first, MDefinition* second,
                         MDefinition* third, MDefinition* fourth)
      : MAryInstruction(op) {
    initOperand(0, first);
    initOperand(1, second);
    initOperand(2, third);
    initOperand(3, fourth);
  }

  HashNumber valueHash() const override;
};

template <class T>
class MVariadicT : public T {
  FixedList<MUse> operands_;

 protected:
  explicit MVariadicT(typename T::Opcode op) : T(op) {}
  [[nodiscard]] bool init(TempAllocator& alloc, size_t length) {
    return operands_.init(alloc, length);
  }
  void initOperand(size_t index, MDefinition* operand) {
    // FixedList doesn't initialize its elements, so do an unchecked init.
    operands_[index].initUnchecked(operand, this);
  }
  MUse* getUseFor(size_t index) final { return &operands_[index]; }
  const MUse* getUseFor(size_t index) const final { return &operands_[index]; }

  // The MWasmCallBase mixin performs initialization for it's subclasses.
  friend class MWasmCallBase;

 public:
  // Will assert if called before initialization.
  MDefinition* getOperand(size_t index) const final {
    return operands_[index].producer();
  }
  size_t numOperands() const final { return operands_.length(); }
  size_t indexOf(const MUse* u) const final {
    MOZ_ASSERT(u >= &operands_[0]);
    MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
    return u - &operands_[0];
  }
  void replaceOperand(size_t index, MDefinition* operand) final {
    operands_[index].replaceProducer(operand);
  }
};

// An instruction with a variable number of operands. Note that the
// MFoo::New constructor for variadic instructions fallibly
// initializes the operands_ array and must be checked for OOM.
using MVariadicInstruction = MVariadicT<MInstruction>;

// All barriered operations:
// - MCompareExchangeTypedArrayElement
// - MExchangeTypedArrayElement
// - MAtomicTypedArrayElementBinop
// - MGrowableSharedArrayBufferByteLength
//
// And operations which are optionally barriered:
// - MLoadUnboxedScalar
// - MStoreUnboxedScalar
// - MResizableTypedArrayLength
// - MResizableDataViewByteLength
//
// Must have the following attributes:
//
// - Not movable
// - Not removable
// - Not congruent with any other instruction
// - Effectful (they alias every TypedArray store)
//
// The intended effect of those constraints is to prevent all loads and stores
// preceding the barriered operation from being moved to after the barriered
// operation, and vice versa, and to prevent the barriered operation from being
// removed or hoisted.

enum class MemoryBarrierRequirement : bool {
  NotRequired,
  Required,
};

MIR_OPCODE_CLASS_GENERATED

// Truncation barrier. This is intended for protecting its input against
// follow-up truncation optimizations.
class MLimitedTruncate : public MUnaryInstruction,
                         public ConvertToInt32Policy<0>::Data {
  TruncateKind truncate_;
  TruncateKind truncateLimit_;

  MLimitedTruncate(MDefinition* input, TruncateKind limit)
      : MUnaryInstruction(classOpcode, input),
        truncate_(TruncateKind::NoTruncate),
        truncateLimit_(limit) {
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(LimitedTruncate)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  TruncateKind operandTruncateKind(size_t index) const override;
  TruncateKind truncateKind() const { return truncate_; }
  void setTruncateKind(TruncateKind kind) { truncate_ = kind; }
};

// Truncation barrier. This is intended for protecting its input against
// follow-up truncation optimizations.
class MIntPtrLimitedTruncate : public MUnaryInstruction,
                               public NoTypePolicy::Data {
  explicit MIntPtrLimitedTruncate(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(input->type() == MIRType::IntPtr);
    setResultType(MIRType::IntPtr);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(IntPtrLimitedTruncate)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Truncation barrier. This is intended for protecting its input against
// follow-up truncation optimizations.
class MInt64LimitedTruncate : public MUnaryInstruction,
                              public NoTypePolicy::Data {
  explicit MInt64LimitedTruncate(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(input->type() == MIRType::Int64);
    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Int64LimitedTruncate)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// A constant js::Value.
class MConstant : public MNullaryInstruction {
  struct Payload {
    union {
      bool b;
      int32_t i32;
      int64_t i64;
      intptr_t iptr;
      float f;
      double d;
      JSOffThreadAtom* str;
      JS::Symbol* sym;
      BigInt* bi;
      JSObject* obj;
      Shape* shape;
      uint64_t asBits;
    };
    Payload() : asBits(0) {}
  };

  Payload payload_;

  static_assert(sizeof(Payload) == sizeof(uint64_t),
                "asBits must be big enough for all payload bits");

#ifdef DEBUG
  void assertInitializedPayload() const;
#else
  void assertInitializedPayload() const {}
#endif

  explicit MConstant(MIRType type) : MNullaryInstruction(classOpcode) {
    setResultType(type);
    setMovable();
  }
  explicit MConstant(bool b) : MConstant(MIRType::Boolean) { payload_.b = b; }
  explicit MConstant(double d) : MConstant(MIRType::Double) { payload_.d = d; }
  explicit MConstant(float f) : MConstant(MIRType::Float32) { payload_.f = f; }
  explicit MConstant(int32_t i) : MConstant(MIRType::Int32) {
    payload_.i32 = i;
  }
  MConstant(MIRType type, int64_t i) : MConstant(type) {
    MOZ_ASSERT(type == MIRType::Int64 || type == MIRType::IntPtr);
    if (type == MIRType::Int64) {
      payload_.i64 = i;
    } else {
      payload_.iptr = i;
    }
  }

  MConstant(TempAllocator& alloc, const Value& v);
  explicit MConstant(JSObject* obj);
  explicit MConstant(Shape* shape);

 public:
  INSTRUCTION_HEADER(Constant)
  static MConstant* New(TempAllocator& alloc, const Value& v);
  static MConstant* New(TempAllocator::Fallible alloc, const Value& v);
  static MConstant* NewBoolean(TempAllocator& alloc, bool b);
  static MConstant* NewDouble(TempAllocator& alloc, double d);
  static MConstant* NewFloat32(TempAllocator& alloc, double d);
  static MConstant* NewInt32(TempAllocator& alloc, int32_t i);
  static MConstant* NewInt64(TempAllocator& alloc, int64_t i);
  static MConstant* NewIntPtr(TempAllocator& alloc, intptr_t i);
  static MConstant* NewMagic(TempAllocator& alloc, JSWhyMagic m);
  static MConstant* NewNull(TempAllocator& alloc);
  static MConstant* NewObject(TempAllocator& alloc, JSObject* v);
  static MConstant* NewShape(TempAllocator& alloc, Shape* s);
  static MConstant* NewString(TempAllocator& alloc, JSString* s);
  static MConstant* NewUndefined(TempAllocator& alloc);

  // Try to convert this constant to boolean, similar to js::ToBoolean.
  // Returns false if the type is MIRType::Magic* or MIRType::Object.
  [[nodiscard]] bool valueToBoolean(bool* res) const;

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  HashNumber valueHash() const override;
  bool congruentTo(const MDefinition* ins) const override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;

  bool canProduceFloat32() const override;

  ALLOW_CLONE(MConstant)

  bool equals(const MConstant* other) const {
    assertInitializedPayload();
    return type() == other->type() && payload_.asBits == other->payload_.asBits;
  }

  bool toBoolean() const {
    MOZ_ASSERT(type() == MIRType::Boolean);
    return payload_.b;
  }
  int32_t toInt32() const {
    MOZ_ASSERT(type() == MIRType::Int32);
    return payload_.i32;
  }
  int64_t toInt64() const {
    MOZ_ASSERT(type() == MIRType::Int64);
    return payload_.i64;
  }
  intptr_t toIntPtr() const {
    MOZ_ASSERT(type() == MIRType::IntPtr);
    return payload_.iptr;
  }
  bool isInt32(int32_t i) const {
    return type() == MIRType::Int32 && payload_.i32 == i;
  }
  bool isInt64(int64_t i) const {
    return type() == MIRType::Int64 && payload_.i64 == i;
  }
  const double& toDouble() const {
    MOZ_ASSERT(type() == MIRType::Double);
    return payload_.d;
  }
  const float& toFloat32() const {
    MOZ_ASSERT(type() == MIRType::Float32);
    return payload_.f;
  }
  JSOffThreadAtom* toString() const {
    MOZ_ASSERT(type() == MIRType::String);
    return payload_.str;
  }
  JS::Symbol* toSymbol() const {
    MOZ_ASSERT(type() == MIRType::Symbol);
    return payload_.sym;
  }
  BigInt* toBigInt() const {
    MOZ_ASSERT(type() == MIRType::BigInt);
    return payload_.bi;
  }
  JSObject& toObject() const {
    MOZ_ASSERT(type() == MIRType::Object);
    return *payload_.obj;
  }
  JSObject* toObjectOrNull() const {
    if (type() == MIRType::Object) {
      return payload_.obj;
    }
    MOZ_ASSERT(type() == MIRType::Null);
    return nullptr;
  }
  Shape* toShape() const {
    MOZ_ASSERT(type() == MIRType::Shape);
    return payload_.shape;
  }

  bool isTypeRepresentableAsDouble() const {
    return IsTypeRepresentableAsDouble(type());
  }
  double numberToDouble() const {
    MOZ_ASSERT(isTypeRepresentableAsDouble());
    if (type() == MIRType::Int32) {
      return toInt32();
    }
    if (type() == MIRType::Double) {
      return toDouble();
    }
    return toFloat32();
  }

  // Convert this constant to a js::Value. Float32 constants will be stored
  // as DoubleValue and NaNs are canonicalized. Callers must be careful: not
  // all constants can be represented by js::Value (wasm supports int64).
  Value toJSValue() const;
};

inline HashNumber ConstantValueHash(MIRType type, uint64_t payload) {
  // Build a 64-bit value holding both the payload and the type.
  static const size_t TypeBits = 8;
  static const size_t TypeShift = 64 - TypeBits;
  MOZ_ASSERT(uintptr_t(type) <= (1 << TypeBits) - 1);
  uint64_t bits = (uint64_t(type) << TypeShift) ^ payload;

  // Fold all 64 bits into the 32-bit result. It's tempting to just discard
  // half of the bits, as this is just a hash, however there are many common
  // patterns of values where only the low or the high bits vary, so
  // discarding either side would lead to excessive hash collisions.
  return (HashNumber)bits ^ (HashNumber)(bits >> 32);
}

class MParameter : public MNullaryInstruction {
  int32_t index_;

  explicit MParameter(int32_t index)
      : MNullaryInstruction(classOpcode), index_(index) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(Parameter)
  TRIVIAL_NEW_WRAPPERS

  static const int32_t THIS_SLOT = -1;
  int32_t index() const { return index_; }
#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif
  HashNumber valueHash() const override;
  bool congruentTo(const MDefinition* ins) const override;
};

class MControlInstruction : public MInstruction {
 protected:
  explicit MControlInstruction(Opcode op) : MInstruction(op) {}

 public:
  virtual size_t numSuccessors() const = 0;
  virtual MBasicBlock* getSuccessor(size_t i) const = 0;
  virtual void replaceSuccessor(size_t i, MBasicBlock* successor) = 0;

  void initSuccessor(size_t i, MBasicBlock* successor) {
    MOZ_ASSERT(!getSuccessor(i));
    replaceSuccessor(i, successor);
  }

  bool isControlInstruction() const override { return true; }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif
};

class MTableSwitch final : public MControlInstruction,
                           public NoFloatPolicy<0>::Data {
  // The successors of the tableswitch
  // - First successor = the default case
  // - Successors 2 and higher = the cases
  Vector<MBasicBlock*, 0, JitAllocPolicy> successors_;
  // Index into successors_ sorted on case index
  Vector<size_t, 0, JitAllocPolicy> cases_;

  MUse operand_;
  int32_t low_;
  int32_t high_;

  void initOperand(size_t index, MDefinition* operand) {
    MOZ_ASSERT(index == 0);
    operand_.init(operand, this);
  }

  MTableSwitch(TempAllocator& alloc, MDefinition* ins, int32_t low,
               int32_t high)
      : MControlInstruction(classOpcode),
        successors_(alloc),
        cases_(alloc),
        low_(low),
        high_(high) {
    initOperand(0, ins);
  }

 protected:
  MUse* getUseFor(size_t index) override {
    MOZ_ASSERT(index == 0);
    return &operand_;
  }

  const MUse* getUseFor(size_t index) const override {
    MOZ_ASSERT(index == 0);
    return &operand_;
  }

 public:
  INSTRUCTION_HEADER(TableSwitch)

  static MTableSwitch* New(TempAllocator& alloc, MDefinition* ins, int32_t low,
                           int32_t high) {
    return new (alloc) MTableSwitch(alloc, ins, low, high);
  }

  size_t numSuccessors() const override { return successors_.length(); }

  [[nodiscard]] bool addSuccessor(MBasicBlock* successor, size_t* index) {
    MOZ_ASSERT(successors_.length() < (size_t)(high_ - low_ + 2));
    MOZ_ASSERT(!successors_.empty());
    *index = successors_.length();
    return successors_.append(successor);
  }

  MBasicBlock* getSuccessor(size_t i) const override {
    MOZ_ASSERT(i < numSuccessors());
    return successors_[i];
  }

  void replaceSuccessor(size_t i, MBasicBlock* successor) override {
    MOZ_ASSERT(i < numSuccessors());
    successors_[i] = successor;
  }

  int32_t low() const { return low_; }

  int32_t high() const { return high_; }

  MBasicBlock* getDefault() const { return getSuccessor(0); }

  MBasicBlock* getCase(size_t i) const { return getSuccessor(cases_[i]); }

  [[nodiscard]] bool addDefault(MBasicBlock* block, size_t* index = nullptr) {
    MOZ_ASSERT(successors_.empty());
    if (index) {
      *index = 0;
    }
    return successors_.append(block);
  }

  [[nodiscard]] bool addCase(size_t successorIndex) {
    return cases_.append(successorIndex);
  }

  size_t numCases() const { return high() - low() + 1; }

  MDefinition* getOperand(size_t index) const override {
    MOZ_ASSERT(index == 0);
    return operand_.producer();
  }

  size_t numOperands() const override { return 1; }

  size_t indexOf(const MUse* u) const final {
    MOZ_ASSERT(u == getUseFor(0));
    return 0;
  }

  void replaceOperand(size_t index, MDefinition* operand) final {
    MOZ_ASSERT(index == 0);
    operand_.replaceProducer(operand);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  // It does read memory in that it must read an entry from the jump table,
  // but that's effectively data that is private to this MIR.  And it should
  // certainly never be modified by any other MIR.  Hence it is effect-free
  // from an alias-analysis standpoint.
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

template <size_t Arity, size_t Successors>
class MAryControlInstruction : public MControlInstruction {
  mozilla::Array<MUse, Arity> operands_;
  mozilla::Array<MBasicBlock*, Successors> successors_;

 protected:
  explicit MAryControlInstruction(Opcode op) : MControlInstruction(op) {}
  void setSuccessor(size_t index, MBasicBlock* successor) {
    successors_[index] = successor;
  }

  MUse* getUseFor(size_t index) final { return &operands_[index]; }
  const MUse* getUseFor(size_t index) const final { return &operands_[index]; }
  void initOperand(size_t index, MDefinition* operand) {
    operands_[index].init(operand, this);
  }

 public:
  MDefinition* getOperand(size_t index) const final {
    return operands_[index].producer();
  }
  size_t numOperands() const final { return Arity; }
  size_t indexOf(const MUse* u) const final {
    MOZ_ASSERT(u >= &operands_[0]);
    MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
    return u - &operands_[0];
  }
  void replaceOperand(size_t index, MDefinition* operand) final {
    operands_[index].replaceProducer(operand);
  }
  size_t numSuccessors() const final { return Successors; }
  MBasicBlock* getSuccessor(size_t i) const final { return successors_[i]; }
  void replaceSuccessor(size_t i, MBasicBlock* succ) final {
    successors_[i] = succ;
  }
};

template <size_t Successors>
class MVariadicControlInstruction : public MVariadicT<MControlInstruction> {
  mozilla::Array<MBasicBlock*, Successors> successors_;

 protected:
  explicit MVariadicControlInstruction(Opcode op)
      : MVariadicT<MControlInstruction>(op) {}
  void setSuccessor(size_t index, MBasicBlock* successor) {
    successors_[index] = successor;
  }

 public:
  size_t numSuccessors() const final { return Successors; }
  MBasicBlock* getSuccessor(size_t i) const final { return successors_[i]; }
  void replaceSuccessor(size_t i, MBasicBlock* succ) final {
    successors_[i] = succ;
  }
};

// Jump to the start of another basic block.
class MGoto : public MAryControlInstruction<0, 1>, public NoTypePolicy::Data {
  explicit MGoto(MBasicBlock* target) : MAryControlInstruction(classOpcode) {
    setSuccessor(TargetIndex, target);
  }

 public:
  INSTRUCTION_HEADER(Goto)
  static MGoto* New(TempAllocator& alloc, MBasicBlock* target);
  static MGoto* New(TempAllocator::Fallible alloc, MBasicBlock* target);

  // Variant that may patch the target later.
  static MGoto* New(TempAllocator& alloc);

  static constexpr size_t TargetIndex = 0;

  MBasicBlock* target() const { return getSuccessor(TargetIndex); }
  void setTarget(MBasicBlock* target) { setSuccessor(TargetIndex, target); }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "Block%u", GetMBasicBlockId(target()));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MGoto)
};

// Tests if the input instruction evaluates to true or false, and jumps to the
// start of a corresponding basic block.
class MTest : public MAryControlInstruction<1, 2>, public TestPolicy::Data {
  // It is allowable to specify `trueBranch` or `falseBranch` as nullptr and
  // patch it in later.
  MTest(MDefinition* ins, MBasicBlock* trueBranch, MBasicBlock* falseBranch)
      : MAryControlInstruction(classOpcode) {
    initOperand(0, ins);
    setSuccessor(TrueBranchIndex, trueBranch);
    setSuccessor(FalseBranchIndex, falseBranch);
  }

  TypeDataList observedTypes_;

 public:
  INSTRUCTION_HEADER(Test)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, input))

  const TypeDataList& observedTypes() const { return observedTypes_; }
  void setObservedTypes(const TypeDataList& observed) {
    observedTypes_ = observed;
  }

  static constexpr size_t TrueBranchIndex = 0;
  static constexpr size_t FalseBranchIndex = 1;

  MBasicBlock* ifTrue() const { return getSuccessor(TrueBranchIndex); }
  MBasicBlock* ifFalse() const { return getSuccessor(FalseBranchIndex); }
  void setIfTrue(MBasicBlock* target) { setSuccessor(TrueBranchIndex, target); }
  void setIfFalse(MBasicBlock* target) {
    setSuccessor(FalseBranchIndex, target);
  }

  MBasicBlock* branchSuccessor(BranchDirection dir) const {
    return (dir == TRUE_BRANCH) ? ifTrue() : ifFalse();
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsDoubleNegation(TempAllocator& alloc);
  MDefinition* foldsConstant(TempAllocator& alloc);
  MDefinition* foldsTypes(TempAllocator& alloc);
  MDefinition* foldsNeedlessControlFlow(TempAllocator& alloc);
  MDefinition* foldsRedundantTest(TempAllocator& alloc);
  MDefinition* foldsTo(TempAllocator& alloc) override;

#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "true->Block%u false->Block%u",
                   GetMBasicBlockId(ifTrue()), GetMBasicBlockId(ifFalse()));
    extras->add(buf);
  }
#endif

  bool canClone() const override { return true; }
  MInstruction* clone(TempAllocator& alloc,
                      const MDefinitionVector& inputs) const override {
    MInstruction* res = new (alloc) MTest(input(), ifTrue(), ifFalse());
    if (!res) {
      return nullptr;
    }
    for (size_t i = 0; i < numOperands(); i++) {
      res->replaceOperand(i, inputs[i]);
    }
    return res;
  }
};

// Returns from this function to the previous caller.
class MReturn : public MAryControlInstruction<1, 0>,
                public BoxInputsPolicy::Data {
  explicit MReturn(MDefinition* ins) : MAryControlInstruction(classOpcode) {
    initOperand(0, ins);
  }

 public:
  INSTRUCTION_HEADER(Return)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, input))

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MNewArray : public MUnaryInstruction, public NoTypePolicy::Data {
 private:
  // Number of elements to allocate for the array.
  uint32_t length_;

  // Heap where the array should be allocated.
  gc::Heap initialHeap_;

  bool vmCall_;

  MNewArray(uint32_t length, MConstant* templateConst, gc::Heap initialHeap,
            bool vmCall = false);

 public:
  INSTRUCTION_HEADER(NewArray)
  TRIVIAL_NEW_WRAPPERS

  static MNewArray* NewVM(TempAllocator& alloc, uint32_t length,
                          MConstant* templateConst, gc::Heap initialHeap) {
    return new (alloc) MNewArray(length, templateConst, initialHeap, true);
  }

  uint32_t length() const { return length_; }

  JSObject* templateObject() const {
    return getOperand(0)->toConstant()->toObjectOrNull();
  }

  gc::Heap initialHeap() const { return initialHeap_; }

  bool isVMCall() const { return vmCall_; }

  // NewArray is marked as non-effectful because all our allocations are
  // either lazy when we are using "new Array(length)" or bounded by the
  // script or the stack size when we are using "new Array(...)" or "[...]"
  // notations.  So we might have to allocate the array twice if we bail
  // during the computation of the first element of the square braket
  // notation.
  virtual AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    // The template object can safely be used in the recover instruction
    // because it can never be mutated by any other function execution.
    return templateObject() != nullptr;
  }
};

class MNewTypedArray : public MUnaryInstruction, public NoTypePolicy::Data {
  gc::Heap initialHeap_;

  MNewTypedArray(MConstant* templateConst, gc::Heap initialHeap)
      : MUnaryInstruction(classOpcode, templateConst),
        initialHeap_(initialHeap) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(NewTypedArray)
  TRIVIAL_NEW_WRAPPERS

  auto* templateObject() const {
    auto* cst = getOperand(0)->toConstant();
    return &cst->toObject().as<FixedLengthTypedArrayObject>();
  }

  gc::Heap initialHeap() const { return initialHeap_; }

  virtual AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MNewObject : public MUnaryInstruction, public NoTypePolicy::Data {
 public:
  enum Mode { ObjectLiteral, ObjectCreate };

 private:
  gc::Heap initialHeap_;
  Mode mode_;
  bool vmCall_;

  MNewObject(MConstant* templateConst, gc::Heap initialHeap, Mode mode,
             bool vmCall = false)
      : MUnaryInstruction(classOpcode, templateConst),
        initialHeap_(initialHeap),
        mode_(mode),
        vmCall_(vmCall) {
    if (mode == ObjectLiteral) {
      MOZ_ASSERT(!templateObject());
    } else {
      MOZ_ASSERT(templateObject());
    }
    setResultType(MIRType::Object);

    // The constant is kept separated in a MConstant, this way we can safely
    // mark it during GC if we recover the object allocation.  Otherwise, by
    // making it emittedAtUses, we do not produce register allocations for
    // it and inline its content inside the code produced by the
    // CodeGenerator.
    if (templateConst->toConstant()->type() == MIRType::Object) {
      templateConst->setEmittedAtUses();
    }
  }

 public:
  INSTRUCTION_HEADER(NewObject)
  TRIVIAL_NEW_WRAPPERS

  static MNewObject* NewVM(TempAllocator& alloc, MConstant* templateConst,
                           gc::Heap initialHeap, Mode mode) {
    return new (alloc) MNewObject(templateConst, initialHeap, mode, true);
  }

  Mode mode() const { return mode_; }

  JSObject* templateObject() const {
    return getOperand(0)->toConstant()->toObjectOrNull();
  }

  gc::Heap initialHeap() const { return initialHeap_; }

  bool isVMCall() const { return vmCall_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    // The template object can safely be used in the recover instruction
    // because it can never be mutated by any other function execution.
    return templateObject() != nullptr;
  }
};

class MNewPlainObject : public MUnaryInstruction, public NoTypePolicy::Data {
 private:
  uint32_t numFixedSlots_;
  uint32_t numDynamicSlots_;
  gc::AllocKind allocKind_;
  gc::Heap initialHeap_;

  MNewPlainObject(MConstant* shapeConst, uint32_t numFixedSlots,
                  uint32_t numDynamicSlots, gc::AllocKind allocKind,
                  gc::Heap initialHeap)
      : MUnaryInstruction(classOpcode, shapeConst),
        numFixedSlots_(numFixedSlots),
        numDynamicSlots_(numDynamicSlots),
        allocKind_(allocKind),
        initialHeap_(initialHeap) {
    setResultType(MIRType::Object);

    // The shape constant is kept separated in a MConstant. This way we can
    // safely mark it during GC if we recover the object allocation. Otherwise,
    // by making it emittedAtUses, we do not produce register allocations for it
    // and inline its content inside the code produced by the CodeGenerator.
    MOZ_ASSERT(shapeConst->toConstant()->type() == MIRType::Shape);
    shapeConst->setEmittedAtUses();
  }

 public:
  INSTRUCTION_HEADER(NewPlainObject)
  TRIVIAL_NEW_WRAPPERS

  const Shape* shape() const { return getOperand(0)->toConstant()->toShape(); }

  uint32_t numFixedSlots() const { return numFixedSlots_; }
  uint32_t numDynamicSlots() const { return numDynamicSlots_; }
  gc::AllocKind allocKind() const { return allocKind_; }
  gc::Heap initialHeap() const { return initialHeap_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MNewArrayObject : public MUnaryInstruction, public NoTypePolicy::Data {
 private:
  uint32_t length_;
  gc::Heap initialHeap_;

  MNewArrayObject(TempAllocator& alloc, MConstant* shapeConst, uint32_t length,
                  gc::Heap initialHeap)
      : MUnaryInstruction(classOpcode, shapeConst),
        length_(length),
        initialHeap_(initialHeap) {
    setResultType(MIRType::Object);
    MOZ_ASSERT(shapeConst->toConstant()->type() == MIRType::Shape);
    shapeConst->setEmittedAtUses();
  }

 public:
  INSTRUCTION_HEADER(NewArrayObject)
  TRIVIAL_NEW_WRAPPERS

  static MNewArrayObject* New(TempAllocator& alloc, MConstant* shapeConst,
                              uint32_t length, gc::Heap initialHeap) {
    return new (alloc) MNewArrayObject(alloc, shapeConst, length, initialHeap);
  }

  const Shape* shape() const { return getOperand(0)->toConstant()->toShape(); }

  // See MNewArray::getAliasSet comment.
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  uint32_t length() const { return length_; }
  gc::Heap initialHeap() const { return initialHeap_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MNewIterator : public MUnaryInstruction, public NoTypePolicy::Data {
 public:
  enum Type {
    ArrayIterator,
    StringIterator,
    RegExpStringIterator,
  };

 private:
  Type type_;

  MNewIterator(MConstant* templateConst, Type type)
      : MUnaryInstruction(classOpcode, templateConst), type_(type) {
    setResultType(MIRType::Object);
    templateConst->setEmittedAtUses();
  }

 public:
  INSTRUCTION_HEADER(NewIterator)
  TRIVIAL_NEW_WRAPPERS

  Type type() const { return type_; }

  JSObject* templateObject() {
    return getOperand(0)->toConstant()->toObjectOrNull();
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

// Represent the content of all slots of an object.  This instruction is not
// lowered and is not used to generate code.
class MObjectState : public MVariadicInstruction,
                     public NoFloatPolicyAfter<1>::Data {
 private:
  uint32_t numSlots_;
  uint32_t numFixedSlots_;

  explicit MObjectState(JSObject* templateObject);
  explicit MObjectState(const Shape* shape);
  explicit MObjectState(MObjectState* state);

  [[nodiscard]] bool init(TempAllocator& alloc, MDefinition* obj);

  void initSlot(uint32_t slot, MDefinition* def) { initOperand(slot + 1, def); }

 public:
  INSTRUCTION_HEADER(ObjectState)
  NAMED_OPERANDS((0, object))

  // Return the template object of any object creation which can be recovered
  // on bailout.
  static JSObject* templateObjectOf(MDefinition* obj);

  static MObjectState* New(TempAllocator& alloc, MDefinition* obj);
  static MObjectState* Copy(TempAllocator& alloc, MObjectState* state);

  // As we might do read of uninitialized properties, we have to copy the
  // initial values from the template object.
  void initFromTemplateObject(TempAllocator& alloc, MDefinition* undefinedVal);

  size_t numFixedSlots() const { return numFixedSlots_; }
  size_t numSlots() const { return numSlots_; }

  MDefinition* getSlot(uint32_t slot) const { return getOperand(slot + 1); }
  void setSlot(uint32_t slot, MDefinition* def) {
    replaceOperand(slot + 1, def);
  }

  bool hasFixedSlot(uint32_t slot) const {
    return slot < numSlots() && slot < numFixedSlots();
  }
  MDefinition* getFixedSlot(uint32_t slot) const {
    MOZ_ASSERT(slot < numFixedSlots());
    return getSlot(slot);
  }
  void setFixedSlot(uint32_t slot, MDefinition* def) {
    MOZ_ASSERT(slot < numFixedSlots());
    setSlot(slot, def);
  }

  bool hasDynamicSlot(uint32_t slot) const {
    return numFixedSlots() < numSlots() && slot < numSlots() - numFixedSlots();
  }
  MDefinition* getDynamicSlot(uint32_t slot) const {
    return getSlot(slot + numFixedSlots());
  }
  void setDynamicSlot(uint32_t slot, MDefinition* def) {
    setSlot(slot + numFixedSlots(), def);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

// Represent the contents of all elements of an array.  This instruction is not
// lowered and is not used to generate code.
class MArrayState : public MVariadicInstruction,
                    public NoFloatPolicyAfter<2>::Data {
 private:
  uint32_t numElements_;

  explicit MArrayState(MDefinition* arr);

  [[nodiscard]] bool init(TempAllocator& alloc, MDefinition* obj,
                          MDefinition* len);

  void initElement(uint32_t index, MDefinition* def) {
    initOperand(index + 2, def);
  }

 public:
  INSTRUCTION_HEADER(ArrayState)
  NAMED_OPERANDS((0, array), (1, initializedLength))

  static MArrayState* New(TempAllocator& alloc, MDefinition* arr,
                          MDefinition* initLength);
  static MArrayState* Copy(TempAllocator& alloc, MArrayState* state);

  void initFromTemplateObject(TempAllocator& alloc, MDefinition* undefinedVal);

  void setInitializedLength(MDefinition* def) { replaceOperand(1, def); }

  size_t numElements() const { return numElements_; }

  MDefinition* getElement(uint32_t index) const {
    return getOperand(index + 2);
  }
  void setElement(uint32_t index, MDefinition* def) {
    replaceOperand(index + 2, def);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

// WrappedFunction stores information about a function that can safely be used
// off-thread. In particular, a function's flags can be modified on the main
// thread as functions are relazified and delazified, so we must be careful not
// to access these flags off-thread.
class WrappedFunction : public TempObject {
  // If this is a native function without a JitEntry, the JSFunction*.
  CompilerFunction nativeFun_;
  uint16_t nargs_;
  js::FunctionFlags flags_;

 public:
  WrappedFunction(JSFunction* nativeFun, uint16_t nargs, FunctionFlags flags);

  // Note: When adding new accessors be sure to add consistency asserts
  // to the constructor.

  size_t nargs() const { return nargs_; }

  bool isNativeWithoutJitEntry() const {
    return flags_.isNativeWithoutJitEntry();
  }
  bool hasJitEntry() const { return flags_.hasJitEntry(); }
  bool isConstructor() const { return flags_.isConstructor(); }
  bool isClassConstructor() const { return flags_.isClassConstructor(); }

  // These fields never change, they can be accessed off-main thread.
  JSNative native() const {
    MOZ_ASSERT(isNativeWithoutJitEntry());
    return nativeFun_->nativeUnchecked();
  }
  bool hasJitInfo() const {
    return flags_.canHaveJitInfo() && nativeFun_->jitInfoUnchecked();
  }
  const JSJitInfo* jitInfo() const {
    MOZ_ASSERT(hasJitInfo());
    return nativeFun_->jitInfoUnchecked();
  }

  JSFunction* rawNativeJSFunction() const { return nativeFun_; }
};

enum class DOMObjectKind : uint8_t { Proxy, Native };

class MCallBase : public MVariadicInstruction, public CallPolicy::Data {
 protected:
  // The callee, this, and the actual arguments are all operands of MCall.
  static const size_t CalleeOperandIndex = 0;
  static const size_t NumNonArgumentOperands = 1;

  explicit MCallBase(Opcode op) : MVariadicInstruction(op) {}

 public:
  void initCallee(MDefinition* func) { initOperand(CalleeOperandIndex, func); }
  MDefinition* getCallee() const { return getOperand(CalleeOperandIndex); }

  void replaceCallee(MInstruction* newfunc) {
    replaceOperand(CalleeOperandIndex, newfunc);
  }

  void addArg(size_t argnum, MDefinition* arg);

  MDefinition* getArg(uint32_t index) const {
    return getOperand(NumNonArgumentOperands + index);
  }

  // The number of stack arguments is the max between the number of formal
  // arguments and the number of actual arguments. The number of stack
  // argument includes the |undefined| padding added in case of underflow.
  // Includes |this|.
  uint32_t numStackArgs() const {
    return numOperands() - NumNonArgumentOperands;
  }
  uint32_t paddedNumStackArgs() const {
    if (JitStackValueAlignment > 1) {
      return AlignBytes(numStackArgs(), JitStackValueAlignment);
    }
    return numStackArgs();
  }

  static size_t IndexOfThis() { return NumNonArgumentOperands; }
  static size_t IndexOfArgument(size_t index) {
    return NumNonArgumentOperands + index + 1;  // +1 to skip |this|.
  }
  static size_t IndexOfStackArg(size_t index) {
    return NumNonArgumentOperands + index;
  }
};

class MCall : public MCallBase {
 protected:
  // Monomorphic cache for MCalls with a single JSFunction target.
  WrappedFunction* target_;

  // Original value of argc from the bytecode.
  uint32_t numActualArgs_;

  // True if the call is for JSOp::New or JSOp::SuperCall.
  bool construct_ : 1;

  // True if the caller does not use the return value.
  bool ignoresReturnValue_ : 1;

  bool needsClassCheck_ : 1;
  bool maybeCrossRealm_ : 1;
  bool needsThisCheck_ : 1;

  MCall(WrappedFunction* target, uint32_t numActualArgs, bool construct,
        bool ignoresReturnValue)
      : MCallBase(classOpcode),
        target_(target),
        numActualArgs_(numActualArgs),
        construct_(construct),
        ignoresReturnValue_(ignoresReturnValue),
        needsClassCheck_(true),
        maybeCrossRealm_(true),
        needsThisCheck_(false) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(Call)
  static MCall* New(TempAllocator& alloc, WrappedFunction* target,
                    size_t maxArgc, size_t numActualArgs, bool construct,
                    bool ignoresReturnValue, bool isDOMCall,
                    mozilla::Maybe<DOMObjectKind> objectKind,
                    mozilla::Maybe<gc::Heap> initialHeap);

  bool needsClassCheck() const { return needsClassCheck_; }
  void disableClassCheck() { needsClassCheck_ = false; }

  bool maybeCrossRealm() const { return maybeCrossRealm_; }
  void setNotCrossRealm() { maybeCrossRealm_ = false; }

  bool needsThisCheck() const { return needsThisCheck_; }
  void setNeedsThisCheck() {
    MOZ_ASSERT(construct_);
    needsThisCheck_ = true;
  }

  // For monomorphic callsites.
  WrappedFunction* getSingleTarget() const { return target_; }

  bool isConstructing() const { return construct_; }

  bool ignoresReturnValue() const { return ignoresReturnValue_; }

  // Does not include |this|.
  uint32_t numActualArgs() const { return numActualArgs_; }

  bool possiblyCalls() const override { return true; }

  virtual bool isCallDOMNative() const { return false; }

  // A method that can be called to tell the MCall to figure out whether it's
  // movable or not.  This can't be done in the constructor, because it
  // depends on the arguments to the call, and those aren't passed to the
  // constructor but are set up later via addArg.
  virtual void computeMovable() {}
};

class MCallDOMNative : public MCall {
  // A helper class for MCalls for DOM natives.  Note that this is NOT
  // actually a separate MIR op from MCall, because all sorts of places use
  // isCall() to check for calls and all we really want is to overload a few
  // virtual things from MCall.

  DOMObjectKind objectKind_;

  // Allow wrapper pre-tenuring
  gc::Heap initialHeap_ = gc::Heap::Default;

  MCallDOMNative(WrappedFunction* target, uint32_t numActualArgs,
                 DOMObjectKind objectKind, gc::Heap initialHeap)
      : MCall(target, numActualArgs, false, false),
        objectKind_(objectKind),
        initialHeap_(initialHeap) {
    MOZ_ASSERT(getJitInfo()->type() != JSJitInfo::InlinableNative);

    // If our jitinfo is not marked eliminatable, that means that our C++
    // implementation is fallible or that it never wants to be eliminated or
    // that we have no hope of ever doing the sort of argument analysis that
    // would allow us to detemine that we're side-effect-free.  In the
    // latter case we wouldn't get DCEd no matter what, but for the former
    // two cases we have to explicitly say that we can't be DCEd.
    if (!getJitInfo()->isEliminatable) {
      setGuard();
    }
  }

  friend MCall* MCall::New(TempAllocator& alloc, WrappedFunction* target,
                           size_t maxArgc, size_t numActualArgs, bool construct,
                           bool ignoresReturnValue, bool isDOMCall,
                           mozilla::Maybe<DOMObjectKind> objectKind,
                           mozilla::Maybe<gc::Heap> initalHeap);

  const JSJitInfo* getJitInfo() const;

 public:
  DOMObjectKind objectKind() const { return objectKind_; }

  virtual AliasSet getAliasSet() const override;

  virtual bool congruentTo(const MDefinition* ins) const override;

  virtual bool isCallDOMNative() const override { return true; }

  virtual void computeMovable() override;

  gc::Heap initialHeap() { return initialHeap_; }
};

// Used to invoke a JSClass call/construct hook.
class MCallClassHook : public MCallBase {
  const JSNative target_;
  bool constructing_ : 1;
  bool ignoresReturnValue_ : 1;

  MCallClassHook(JSNative target, bool constructing)
      : MCallBase(classOpcode),
        target_(target),
        constructing_(constructing),
        ignoresReturnValue_(false) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(CallClassHook)
  static MCallClassHook* New(TempAllocator& alloc, JSNative target,
                             uint32_t argc, bool constructing);

  JSNative target() const { return target_; }
  bool isConstructing() const { return constructing_; }

  uint32_t numActualArgs() const {
    uint32_t thisAndNewTarget = 1 + constructing_;
    MOZ_ASSERT(numStackArgs() >= thisAndNewTarget);
    return numStackArgs() - thisAndNewTarget;
  }

  bool maybeCrossRealm() const { return true; }

  bool ignoresReturnValue() const { return ignoresReturnValue_; }
  void setIgnoresReturnValue() { ignoresReturnValue_ = true; }

  bool possiblyCalls() const override { return true; }
};

// fun.apply(self, arguments)
class MApplyArgs : public MTernaryInstruction,
                   public MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>,
                                    BoxPolicy<2>>::Data {
  // Single target from CacheIR, or nullptr
  WrappedFunction* target_;
  // Number of extra initial formals to skip.
  uint32_t numExtraFormals_;
  bool maybeCrossRealm_ = true;
  bool ignoresReturnValue_ = false;

  MApplyArgs(WrappedFunction* target, MDefinition* fun, MDefinition* argc,
             MDefinition* self, uint32_t numExtraFormals = 0)
      : MTernaryInstruction(classOpcode, fun, argc, self),
        target_(target),
        numExtraFormals_(numExtraFormals) {
    MOZ_ASSERT(argc->type() == MIRType::Int32);
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(ApplyArgs)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, getFunction), (1, getArgc), (2, getThis))

  WrappedFunction* getSingleTarget() const { return target_; }

  uint32_t numExtraFormals() const { return numExtraFormals_; }

  bool maybeCrossRealm() const { return maybeCrossRealm_; }
  void setNotCrossRealm() { maybeCrossRealm_ = false; }

  bool ignoresReturnValue() const { return ignoresReturnValue_; }
  void setIgnoresReturnValue() { ignoresReturnValue_ = true; }

  bool isConstructing() const { return false; }

  bool possiblyCalls() const override { return true; }
};

class MApplyArgsObj
    : public MTernaryInstruction,
      public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>, BoxPolicy<2>>::Data {
  WrappedFunction* target_;
  bool maybeCrossRealm_ = true;
  bool ignoresReturnValue_ = false;

  MApplyArgsObj(WrappedFunction* target, MDefinition* fun, MDefinition* argsObj,
                MDefinition* thisArg)
      : MTernaryInstruction(classOpcode, fun, argsObj, thisArg),
        target_(target) {
    MOZ_ASSERT(argsObj->type() == MIRType::Object);
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(ApplyArgsObj)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, getFunction), (1, getArgsObj), (2, getThis))

  WrappedFunction* getSingleTarget() const { return target_; }

  bool maybeCrossRealm() const { return maybeCrossRealm_; }
  void setNotCrossRealm() { maybeCrossRealm_ = false; }

  bool ignoresReturnValue() const { return ignoresReturnValue_; }
  void setIgnoresReturnValue() { ignoresReturnValue_ = true; }

  bool isConstructing() const { return false; }

  bool possiblyCalls() const override { return true; }
};

// fun.apply(fn, array)
class MApplyArray : public MTernaryInstruction,
                    public MixPolicy<ObjectPolicy<0>, BoxPolicy<2>>::Data {
  // Single target from CacheIR, or nullptr
  WrappedFunction* target_;
  bool maybeCrossRealm_ = true;
  bool ignoresReturnValue_ = false;

  MApplyArray(WrappedFunction* target, MDefinition* fun, MDefinition* elements,
              MDefinition* self)
      : MTernaryInstruction(classOpcode, fun, elements, self), target_(target) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(ApplyArray)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, getFunction), (1, getElements), (2, getThis))

  WrappedFunction* getSingleTarget() const { return target_; }

  bool maybeCrossRealm() const { return maybeCrossRealm_; }
  void setNotCrossRealm() { maybeCrossRealm_ = false; }

  bool ignoresReturnValue() const { return ignoresReturnValue_; }
  void setIgnoresReturnValue() { ignoresReturnValue_ = true; }

  bool isConstructing() const { return false; }

  bool possiblyCalls() const override { return true; }
};

// |new F(...arguments)| and |super(...arguments)|.
class MConstructArgs : public MQuaternaryInstruction,
                       public MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<1>,
                                        BoxPolicy<2>, ObjectPolicy<3>>::Data {
  // Single target from CacheIR, or nullptr
  WrappedFunction* target_;
  // Number of extra initial formals to skip.
  uint32_t numExtraFormals_;
  bool maybeCrossRealm_ = true;

  MConstructArgs(WrappedFunction* target, MDefinition* fun, MDefinition* argc,
                 MDefinition* thisValue, MDefinition* newTarget,
                 uint32_t numExtraFormals = 0)
      : MQuaternaryInstruction(classOpcode, fun, argc, thisValue, newTarget),
        target_(target),
        numExtraFormals_(numExtraFormals) {
    MOZ_ASSERT(argc->type() == MIRType::Int32);
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(ConstructArgs)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, getFunction), (1, getArgc), (2, getThis),
                 (3, getNewTarget))

  WrappedFunction* getSingleTarget() const { return target_; }

  uint32_t numExtraFormals() const { return numExtraFormals_; }

  bool maybeCrossRealm() const { return maybeCrossRealm_; }
  void setNotCrossRealm() { maybeCrossRealm_ = false; }

  bool ignoresReturnValue() const { return false; }
  bool isConstructing() const { return true; }

  bool possiblyCalls() const override { return true; }
};

// |new F(...args)| and |super(...args)|.
class MConstructArray
    : public MQuaternaryInstruction,
      public MixPolicy<ObjectPolicy<0>, BoxPolicy<2>, ObjectPolicy<3>>::Data {
  // Single target from CacheIR, or nullptr
  WrappedFunction* target_;
  bool maybeCrossRealm_ = true;
  bool needsThisCheck_ = false;

  MConstructArray(WrappedFunction* target, MDefinition* fun,
                  MDefinition* elements, MDefinition* thisValue,
                  MDefinition* newTarget)
      : MQuaternaryInstruction(classOpcode, fun, elements, thisValue,
                               newTarget),
        target_(target) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(ConstructArray)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, getFunction), (1, getElements), (2, getThis),
                 (3, getNewTarget))

  WrappedFunction* getSingleTarget() const { return target_; }

  bool maybeCrossRealm() const { return maybeCrossRealm_; }
  void setNotCrossRealm() { maybeCrossRealm_ = false; }

  bool needsThisCheck() const { return needsThisCheck_; }
  void setNeedsThisCheck() { needsThisCheck_ = true; }

  bool ignoresReturnValue() const { return false; }
  bool isConstructing() const { return true; }

  bool possiblyCalls() const override { return true; }
};

class MBail : public MNullaryInstruction {
  explicit MBail(BailoutKind kind) : MNullaryInstruction(classOpcode) {
    setBailoutKind(kind);
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(Bail)

  static MBail* New(TempAllocator& alloc, BailoutKind kind) {
    return new (alloc) MBail(kind);
  }
  static MBail* New(TempAllocator& alloc) {
    return new (alloc) MBail(BailoutKind::Inevitable);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MUnreachable : public MAryControlInstruction<0, 0>,
                     public NoTypePolicy::Data {
  MUnreachable() : MAryControlInstruction(classOpcode) {}

 public:
  INSTRUCTION_HEADER(Unreachable)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MAssertRecoveredOnBailout : public MUnaryInstruction,
                                  public NoTypePolicy::Data {
  bool mustBeRecovered_;

  MAssertRecoveredOnBailout(MDefinition* ins, bool mustBeRecovered)
      : MUnaryInstruction(classOpcode, ins), mustBeRecovered_(mustBeRecovered) {
    setResultType(MIRType::Value);
    setRecoveredOnBailout();
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(AssertRecoveredOnBailout)
  TRIVIAL_NEW_WRAPPERS

  // Needed to assert that float32 instructions are correctly recovered.
  bool canConsumeFloat32(MUse* use) const override { return true; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MAssertFloat32 : public MUnaryInstruction, public NoTypePolicy::Data {
  bool mustBeFloat32_;

  MAssertFloat32(MDefinition* value, bool mustBeFloat32)
      : MUnaryInstruction(classOpcode, value), mustBeFloat32_(mustBeFloat32) {}

 public:
  INSTRUCTION_HEADER(AssertFloat32)
  TRIVIAL_NEW_WRAPPERS

  bool canConsumeFloat32(MUse* use) const override { return true; }

  bool mustBeFloat32() const { return mustBeFloat32_; }
};

class MCompare : public MBinaryInstruction, public ComparePolicy::Data {
 public:
  enum CompareType {

    // Anything compared to Undefined
    Compare_Undefined,

    // Anything compared to Null
    Compare_Null,

    // Int32   compared to Int32
    // Boolean compared to Boolean
    Compare_Int32,

    // Int32 compared as unsigneds
    Compare_UInt32,

    // Int64 compared to Int64.
    Compare_Int64,

    // Int64 compared as unsigneds.
    Compare_UInt64,

    // IntPtr compared to IntPtr.
    Compare_IntPtr,

    // IntPtr compared as unsigneds.
    Compare_UIntPtr,

    // Double compared to Double
    Compare_Double,

    // Float compared to Float
    Compare_Float32,

    // String compared to String
    Compare_String,

    // Symbol compared to Symbol
    Compare_Symbol,

    // Object compared to Object
    Compare_Object,

    // BigInt compared to BigInt
    Compare_BigInt,

    // BigInt compared to Int32
    Compare_BigInt_Int32,

    // BigInt compared to Double
    Compare_BigInt_Double,

    // BigInt compared to String
    Compare_BigInt_String,

    // Wasm Ref/AnyRef/NullRef compared to Ref/AnyRef/NullRef
    Compare_WasmAnyRef,
  };

 private:
  CompareType compareType_;
  JSOp jsop_;
  bool operandsAreNeverNaN_;

  // When a floating-point comparison is converted to an integer comparison
  // (when range analysis proves it safe), we need to convert the operands
  // to integer as well.
  bool truncateOperands_;

  MCompare(MDefinition* left, MDefinition* right, JSOp jsop,
           CompareType compareType)
      : MBinaryInstruction(classOpcode, left, right),
        compareType_(compareType),
        jsop_(jsop),
        operandsAreNeverNaN_(false),
        truncateOperands_(false) {
    setResultType(MIRType::Boolean);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Compare)
  TRIVIAL_NEW_WRAPPERS

  static MCompare* NewWasm(TempAllocator& alloc, MDefinition* left,
                           MDefinition* right, JSOp jsop,
                           CompareType compareType) {
    MOZ_ASSERT(compareType == Compare_Int32 || compareType == Compare_UInt32 ||
               compareType == Compare_Int64 || compareType == Compare_UInt64 ||
               compareType == Compare_Double ||
               compareType == Compare_Float32 ||
               compareType == Compare_WasmAnyRef);
    auto* ins = MCompare::New(alloc, left, right, jsop, compareType);
    ins->setResultType(MIRType::Int32);
    return ins;
  }

  [[nodiscard]] bool tryFold(bool* result);
  [[nodiscard]] bool evaluateConstantOperands(TempAllocator& alloc,
                                              bool* result);
  MDefinition* foldsTo(TempAllocator& alloc) override;

  CompareType compareType() const { return compareType_; }
  bool isInt32Comparison() const { return compareType() == Compare_Int32; }
  bool isDoubleComparison() const { return compareType() == Compare_Double; }
  bool isFloat32Comparison() const { return compareType() == Compare_Float32; }
  bool isNumericComparison() const {
    return isInt32Comparison() || isDoubleComparison() || isFloat32Comparison();
  }

  JSOp jsop() const { return jsop_; }
  bool operandsAreNeverNaN() const { return operandsAreNeverNaN_; }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif
  void collectRangeInfoPreTrunc() override;

  void trySpecializeFloat32(TempAllocator& alloc) override;
  bool isFloat32Commutative() const override { return true; }
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  TruncateKind operandTruncateKind(size_t index) const override;

#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override {
    // Both sides of the compare can be Float32
    return compareType_ == Compare_Float32;
  }
#endif

  ALLOW_CLONE(MCompare)

 private:
  [[nodiscard]] bool tryFoldEqualOperands(bool* result);
  [[nodiscard]] bool tryFoldTypeOf(bool* result);
  [[nodiscard]] MDefinition* tryFoldTypeOf(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldCharCompare(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldStringCompare(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldStringSubstring(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldStringIndexOf(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldBigInt64(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldBigIntPtr(TempAllocator& alloc);
  [[nodiscard]] MDefinition* tryFoldBigInt(TempAllocator& alloc);

 public:
  bool congruentTo(const MDefinition* ins) const override {
    if (!binaryCongruentTo(ins)) {
      return false;
    }
    return compareType() == ins->toCompare()->compareType() &&
           jsop() == ins->toCompare()->jsop();
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    switch (compareType_) {
      case Compare_Undefined:
      case Compare_Null:
      case Compare_Int32:
      case Compare_UInt32:
      case Compare_Double:
      case Compare_Float32:
      case Compare_String:
      case Compare_Symbol:
      case Compare_Object:
      case Compare_BigInt:
      case Compare_BigInt_Int32:
      case Compare_BigInt_Double:
      case Compare_BigInt_String:
        return true;

      case Compare_Int64:
      case Compare_UInt64:
      case Compare_IntPtr:
      case Compare_UIntPtr:
      case Compare_WasmAnyRef:
        return false;
    }
    MOZ_CRASH("unexpected compare type");
  }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    const char* ty = nullptr;
    switch (compareType_) {
      case Compare_Undefined:
        ty = "Undefined";
        break;
      case Compare_Null:
        ty = "Null";
        break;
      case Compare_Int32:
        ty = "Int32";
        break;
      case Compare_UInt32:
        ty = "UInt32";
        break;
      case Compare_Int64:
        ty = "Int64";
        break;
      case Compare_UInt64:
        ty = "UInt64";
        break;
      case Compare_IntPtr:
        ty = "IntPtr";
        break;
      case Compare_UIntPtr:
        ty = "UIntPtr";
        break;
      case Compare_Double:
        ty = "Double";
        break;
      case Compare_Float32:
        ty = "Float32";
        break;
      case Compare_String:
        ty = "String";
        break;
      case Compare_Symbol:
        ty = "Symbol";
        break;
      case Compare_Object:
        ty = "Object";
        break;
      case Compare_BigInt:
        ty = "BigInt";
        break;
      case Compare_BigInt_Int32:
        ty = "BigInt_Int32";
        break;
      case Compare_BigInt_Double:
        ty = "BigInt_Double";
        break;
      case Compare_BigInt_String:
        ty = "BigInt_String";
        break;
      case Compare_WasmAnyRef:
        ty = "WasmAnyRef";
        break;
      default:
        ty = "!!unknown!!";
        break;
    };
    char buf[64];
    SprintfLiteral(buf, "ty=%s jsop=%s", ty, CodeName(jsop()));
    extras->add(buf);
  }
#endif
};

// Takes a typed value and returns an untyped value.
class MBox : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MBox(MDefinition* ins) : MUnaryInstruction(classOpcode, ins) {
    // Cannot box a box.
    MOZ_ASSERT(ins->type() != MIRType::Value);

    setResultType(MIRType::Value);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Box)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MBox)
};

// Note: the op may have been inverted during lowering (to put constants in a
// position where they can be immediates), so it is important to use the
// lir->jsop() instead of the mir->jsop() when it is present.
static inline Assembler::Condition JSOpToCondition(
    MCompare::CompareType compareType, JSOp op) {
  bool isSigned = (compareType != MCompare::Compare_UInt32 &&
                   compareType != MCompare::Compare_UInt64 &&
                   compareType != MCompare::Compare_UIntPtr);
  return JSOpToCondition(op, isSigned);
}

// Takes a typed value and checks if it is a certain type. If so, the payload
// is unpacked and returned as that type. Otherwise, it is considered a
// deoptimization.
class MUnbox final : public MUnaryInstruction, public BoxInputsPolicy::Data {
 public:
  enum Mode {
    Fallible,    // Check the type, and deoptimize if unexpected.
    Infallible,  // Type guard is not necessary.
  };

 private:
  Mode mode_;

  MUnbox(MDefinition* ins, MIRType type, Mode mode)
      : MUnaryInstruction(classOpcode, ins), mode_(mode) {
    // Only allow unboxing a non MIRType::Value when input and output types
    // don't match. This is often used to force a bailout. Boxing happens
    // during type analysis.
    MOZ_ASSERT_IF(ins->type() != MIRType::Value, type != ins->type());

    MOZ_ASSERT(type == MIRType::Boolean || type == MIRType::Int32 ||
               type == MIRType::Double || type == MIRType::String ||
               type == MIRType::Symbol || type == MIRType::BigInt ||
               type == MIRType::Object);

    setResultType(type);
    setMovable();

    if (mode_ == Fallible) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(Unbox)
  TRIVIAL_NEW_WRAPPERS

  Mode mode() const { return mode_; }
  bool fallible() const { return mode() != Infallible; }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isUnbox() || ins->toUnbox()->mode() != mode()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }
#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  ALLOW_CLONE(MUnbox)
};

class MAssertRange : public MUnaryInstruction, public NoTypePolicy::Data {
  // This is the range checked by the assertion. Don't confuse this with the
  // range_ member or the range() accessor. Since MAssertRange doesn't return
  // a value, it doesn't use those.
  const Range* assertedRange_;

  MAssertRange(MDefinition* ins, const Range* assertedRange)
      : MUnaryInstruction(classOpcode, ins), assertedRange_(assertedRange) {
    setGuard();
    setResultType(MIRType::None);
  }

 public:
  INSTRUCTION_HEADER(AssertRange)
  TRIVIAL_NEW_WRAPPERS

  const Range* assertedRange() const { return assertedRange_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif
};

class MAssertClass : public MUnaryInstruction, public NoTypePolicy::Data {
  const JSClass* class_;

  MAssertClass(MDefinition* obj, const JSClass* clasp)
      : MUnaryInstruction(classOpcode, obj), class_(clasp) {
    MOZ_ASSERT(obj->type() == MIRType::Object);

    setGuard();
    setResultType(MIRType::None);
  }

 public:
  INSTRUCTION_HEADER(AssertClass)
  TRIVIAL_NEW_WRAPPERS

  const JSClass* getClass() const { return class_; }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MAssertShape : public MUnaryInstruction, public NoTypePolicy::Data {
  CompilerShape shape_;

  MAssertShape(MDefinition* obj, Shape* shape)
      : MUnaryInstruction(classOpcode, obj), shape_(shape) {
    MOZ_ASSERT(obj->type() == MIRType::Object);

    setGuard();
    setResultType(MIRType::None);
  }

 public:
  INSTRUCTION_HEADER(AssertShape)
  TRIVIAL_NEW_WRAPPERS

  const Shape* shape() const { return shape_; }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Eager initialization of arguments object.
class MCreateArgumentsObject : public MUnaryInstruction,
                               public ObjectPolicy<0>::Data {
  CompilerGCPointer<ArgumentsObject*> templateObj_;

  MCreateArgumentsObject(MDefinition* callObj, ArgumentsObject* templateObj)
      : MUnaryInstruction(classOpcode, callObj), templateObj_(templateObj) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(CreateArgumentsObject)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, getCallObject))

  ArgumentsObject* templateObject() const { return templateObj_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool possiblyCalls() const override { return true; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

// Eager initialization of arguments object for inlined function
class MCreateInlinedArgumentsObject : public MVariadicInstruction,
                                      public NoFloatPolicyAfter<0>::Data {
  CompilerGCPointer<ArgumentsObject*> templateObj_;

  explicit MCreateInlinedArgumentsObject(ArgumentsObject* templateObj)
      : MVariadicInstruction(classOpcode), templateObj_(templateObj) {
    setResultType(MIRType::Object);
  }

  static const size_t NumNonArgumentOperands = 2;

 public:
  INSTRUCTION_HEADER(CreateInlinedArgumentsObject)
  static MCreateInlinedArgumentsObject* New(TempAllocator& alloc,
                                            MDefinition* callObj,
                                            MDefinition* callee,
                                            MDefinitionVector& args,
                                            ArgumentsObject* templateObj);
  NAMED_OPERANDS((0, getCallObject), (1, getCallee))

  ArgumentsObject* templateObject() const { return templateObj_; }

  MDefinition* getArg(uint32_t idx) const {
    return getOperand(idx + NumNonArgumentOperands);
  }
  uint32_t numActuals() const { return numOperands() - NumNonArgumentOperands; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool possiblyCalls() const override { return true; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MGetInlinedArgument
    : public MVariadicInstruction,
      public MixPolicy<UnboxedInt32Policy<0>, NoFloatPolicyAfter<1>>::Data {
  MGetInlinedArgument() : MVariadicInstruction(classOpcode) {
    setResultType(MIRType::Value);
  }

  static const size_t NumNonArgumentOperands = 1;

 public:
  INSTRUCTION_HEADER(GetInlinedArgument)
  static MGetInlinedArgument* New(TempAllocator& alloc, MDefinition* index,
                                  MCreateInlinedArgumentsObject* args);
  static MGetInlinedArgument* New(TempAllocator& alloc, MDefinition* index,
                                  const CallInfo& callInfo);
  NAMED_OPERANDS((0, index))

  MDefinition* getArg(uint32_t idx) const {
    return getOperand(idx + NumNonArgumentOperands);
  }
  uint32_t numActuals() const { return numOperands() - NumNonArgumentOperands; }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;
};

class MGetInlinedArgumentHole
    : public MVariadicInstruction,
      public MixPolicy<UnboxedInt32Policy<0>, NoFloatPolicyAfter<1>>::Data {
  MGetInlinedArgumentHole() : MVariadicInstruction(classOpcode) {
    setGuard();
    setResultType(MIRType::Value);
  }

  static const size_t NumNonArgumentOperands = 1;

 public:
  INSTRUCTION_HEADER(GetInlinedArgumentHole)
  static MGetInlinedArgumentHole* New(TempAllocator& alloc, MDefinition* index,
                                      MCreateInlinedArgumentsObject* args);
  NAMED_OPERANDS((0, index))

  MDefinition* getArg(uint32_t idx) const {
    return getOperand(idx + NumNonArgumentOperands);
  }
  uint32_t numActuals() const { return numOperands() - NumNonArgumentOperands; }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;
};

class MInlineArgumentsSlice
    : public MVariadicInstruction,
      public MixPolicy<UnboxedInt32Policy<0>, UnboxedInt32Policy<1>,
                       NoFloatPolicyAfter<2>>::Data {
  JSObject* templateObj_;
  gc::Heap initialHeap_;

  MInlineArgumentsSlice(JSObject* templateObj, gc::Heap initialHeap)
      : MVariadicInstruction(classOpcode),
        templateObj_(templateObj),
        initialHeap_(initialHeap) {
    setResultType(MIRType::Object);
  }

  static const size_t NumNonArgumentOperands = 2;

 public:
  INSTRUCTION_HEADER(InlineArgumentsSlice)
  static MInlineArgumentsSlice* New(TempAllocator& alloc, MDefinition* begin,
                                    MDefinition* count,
                                    MCreateInlinedArgumentsObject* args,
                                    JSObject* templateObj,
                                    gc::Heap initialHeap);
  NAMED_OPERANDS((0, begin), (1, count))

  JSObject* templateObj() const { return templateObj_; }
  gc::Heap initialHeap() const { return initialHeap_; }

  MDefinition* getArg(uint32_t idx) const {
    return getOperand(idx + NumNonArgumentOperands);
  }
  uint32_t numActuals() const { return numOperands() - NumNonArgumentOperands; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool possiblyCalls() const override { return true; }
};

// Allocates a new BoundFunctionObject and calls
// BoundFunctionObject::functionBindImpl. This instruction can have arbitrary
// side-effects because the GetProperty calls for length/name can call into JS.
class MBindFunction
    : public MVariadicInstruction,
      public MixPolicy<ObjectPolicy<0>, NoFloatPolicyAfter<1>>::Data {
  CompilerGCPointer<JSObject*> templateObj_;

  explicit MBindFunction(JSObject* templateObj)
      : MVariadicInstruction(classOpcode), templateObj_(templateObj) {
    setResultType(MIRType::Object);
  }

  // The target object is operand 0.
  static const size_t NumNonArgumentOperands = 1;

 public:
  INSTRUCTION_HEADER(BindFunction)
  static MBindFunction* New(TempAllocator& alloc, MDefinition* target,
                            uint32_t argc, JSObject* templateObj);
  NAMED_OPERANDS((0, target))

  JSObject* templateObject() const { return templateObj_; }

  MDefinition* getArg(uint32_t idx) const {
    return getOperand(idx + NumNonArgumentOperands);
  }
  void initArg(size_t i, MDefinition* arg) {
    initOperand(NumNonArgumentOperands + i, arg);
  }
  uint32_t numStackArgs() const {
    return numOperands() - NumNonArgumentOperands;
  }

  bool possiblyCalls() const override { return true; }
};

class MToFPInstruction : public MUnaryInstruction, public ToDoublePolicy::Data {
 protected:
  MToFPInstruction(Opcode op, MDefinition* def, MIRType resultType)
      : MUnaryInstruction(op, def) {
    setResultType(resultType);
    setMovable();

    // Guard unless the conversion is known to be non-effectful & non-throwing.
    if (!def->typeIsOneOf({MIRType::Undefined, MIRType::Null, MIRType::Boolean,
                           MIRType::Int32, MIRType::Double, MIRType::Float32,
                           MIRType::String})) {
      setGuard();
    }
  }
};

// Converts a primitive (either typed or untyped) to a double. If the input is
// not primitive at runtime, a bailout occurs.
class MToDouble : public MToFPInstruction {
 private:
  TruncateKind implicitTruncate_ = TruncateKind::NoTruncate;

  explicit MToDouble(MDefinition* def)
      : MToFPInstruction(classOpcode, def, MIRType::Double) {}

 public:
  INSTRUCTION_HEADER(ToDouble)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  TruncateKind operandTruncateKind(size_t index) const override;

#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

  TruncateKind truncateKind() const { return implicitTruncate_; }
  void setTruncateKind(TruncateKind kind) {
    implicitTruncate_ = std::max(implicitTruncate_, kind);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    if (input()->type() == MIRType::Value) {
      return false;
    }
    if (input()->type() == MIRType::Symbol) {
      return false;
    }
    if (input()->type() == MIRType::BigInt) {
      return false;
    }

    return true;
  }

  ALLOW_CLONE(MToDouble)
};

// Converts a primitive (either typed or untyped) to a float32. If the input is
// not primitive at runtime, a bailout occurs.
class MToFloat32 : public MToFPInstruction {
  bool mustPreserveNaN_ = false;

  explicit MToFloat32(MDefinition* def)
      : MToFPInstruction(classOpcode, def, MIRType::Float32) {}

  explicit MToFloat32(MDefinition* def, bool mustPreserveNaN)
      : MToFloat32(def) {
    mustPreserveNaN_ = mustPreserveNaN;
  }

 public:
  INSTRUCTION_HEADER(ToFloat32)
  TRIVIAL_NEW_WRAPPERS

  virtual MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!congruentIfOperandsEqual(ins)) {
      return false;
    }
    return ins->toToFloat32()->mustPreserveNaN_ == mustPreserveNaN_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;

  bool canConsumeFloat32(MUse* use) const override { return true; }
  bool canProduceFloat32() const override { return true; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MToFloat32)
};

// Converts a primitive (either typed or untyped) to a float16. If the input is
// not primitive at runtime, a bailout occurs.
class MToFloat16 : public MToFPInstruction {
  explicit MToFloat16(MDefinition* def)
      : MToFPInstruction(classOpcode, def, MIRType::Float32) {}

 public:
  INSTRUCTION_HEADER(ToFloat16)
  TRIVIAL_NEW_WRAPPERS

  virtual MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  // This instruction can produce but NOT consume float32.
  bool canProduceFloat32() const override { return true; }

#ifdef DEBUG
  // Float16 inputs are typed as float32, but this instruction can NOT consume
  // float32.
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MToFloat16)
};

class MWrapInt64ToInt32 : public MUnaryInstruction, public NoTypePolicy::Data {
  bool bottomHalf_;

  explicit MWrapInt64ToInt32(MDefinition* def, bool bottomHalf = true)
      : MUnaryInstruction(classOpcode, def), bottomHalf_(bottomHalf) {
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(WrapInt64ToInt32)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isWrapInt64ToInt32()) {
      return false;
    }
    if (ins->toWrapInt64ToInt32()->bottomHalf() != bottomHalf()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool bottomHalf() const { return bottomHalf_; }

  ALLOW_CLONE(MWrapInt64ToInt32)
};

class MExtendInt32ToInt64 : public MUnaryInstruction,
                            public NoTypePolicy::Data {
  bool isUnsigned_;

  MExtendInt32ToInt64(MDefinition* def, bool isUnsigned)
      : MUnaryInstruction(classOpcode, def), isUnsigned_(isUnsigned) {
    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(ExtendInt32ToInt64)
  TRIVIAL_NEW_WRAPPERS

  bool isUnsigned() const { return isUnsigned_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isExtendInt32ToInt64()) {
      return false;
    }
    if (ins->toExtendInt32ToInt64()->isUnsigned_ != isUnsigned_) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MExtendInt32ToInt64)
};

// Converts an int32 value to intptr by sign-extending it.
class MInt32ToIntPtr : public MUnaryInstruction,
                       public UnboxedInt32Policy<0>::Data {
  bool canBeNegative_ = true;

  explicit MInt32ToIntPtr(MDefinition* def)
      : MUnaryInstruction(classOpcode, def) {
    setResultType(MIRType::IntPtr);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Int32ToIntPtr)
  TRIVIAL_NEW_WRAPPERS

  bool canBeNegative() const { return canBeNegative_; }
  void setCanNotBeNegative() { canBeNegative_ = false; }

  void computeRange(TempAllocator& alloc) override;
  void collectRangeInfoPreTrunc() override;

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Converts an IntPtr value >= 0 to Int32. Bails out if the value > INT32_MAX.
class MNonNegativeIntPtrToInt32 : public MUnaryInstruction,
                                  public NoTypePolicy::Data {
  explicit MNonNegativeIntPtrToInt32(MDefinition* def)
      : MUnaryInstruction(classOpcode, def) {
    MOZ_ASSERT(def->type() == MIRType::IntPtr);
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(NonNegativeIntPtrToInt32)
  TRIVIAL_NEW_WRAPPERS

  void computeRange(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Converts an IntPtr value to Double.
class MIntPtrToDouble : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MIntPtrToDouble(MDefinition* def)
      : MUnaryInstruction(classOpcode, def) {
    MOZ_ASSERT(def->type() == MIRType::IntPtr);
    setResultType(MIRType::Double);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(IntPtrToDouble)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Subtracts (byteSize - 1) from the input value. Bails out if the result is
// negative. This is used to implement bounds checks for DataView accesses.
class MAdjustDataViewLength : public MUnaryInstruction,
                              public NoTypePolicy::Data {
  const uint32_t byteSize_;

  MAdjustDataViewLength(MDefinition* input, uint32_t byteSize)
      : MUnaryInstruction(classOpcode, input), byteSize_(byteSize) {
    MOZ_ASSERT(input->type() == MIRType::IntPtr);
    MOZ_ASSERT(byteSize > 1);
    setResultType(MIRType::IntPtr);
    setMovable();
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(AdjustDataViewLength)
  TRIVIAL_NEW_WRAPPERS

  uint32_t byteSize() const { return byteSize_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isAdjustDataViewLength()) {
      return false;
    }
    if (ins->toAdjustDataViewLength()->byteSize() != byteSize()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MInt64ToFloatingPoint : public MUnaryInstruction,
                              public NoTypePolicy::Data {
  bool isUnsigned_;
  wasm::BytecodeOffset bytecodeOffset_;

  MInt64ToFloatingPoint(MDefinition* def, MIRType type,
                        wasm::BytecodeOffset bytecodeOffset, bool isUnsigned)
      : MUnaryInstruction(classOpcode, def),
        isUnsigned_(isUnsigned),
        bytecodeOffset_(bytecodeOffset) {
    MOZ_ASSERT(IsFloatingPointType(type));
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Int64ToFloatingPoint)
  TRIVIAL_NEW_WRAPPERS

  bool isUnsigned() const { return isUnsigned_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isInt64ToFloatingPoint()) {
      return false;
    }
    if (ins->toInt64ToFloatingPoint()->isUnsigned_ != isUnsigned_) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// It used only for arm now because on arm we need to call builtin to convert
// i64 to float.
class MBuiltinInt64ToFloatingPoint : public MAryInstruction<2>,
                                     public NoTypePolicy::Data {
  bool isUnsigned_;
  wasm::BytecodeOffset bytecodeOffset_;

  MBuiltinInt64ToFloatingPoint(MDefinition* def, MDefinition* instance,
                               MIRType type,
                               wasm::BytecodeOffset bytecodeOffset,
                               bool isUnsigned)
      : MAryInstruction(classOpcode),
        isUnsigned_(isUnsigned),
        bytecodeOffset_(bytecodeOffset) {
    MOZ_ASSERT(IsFloatingPointType(type));
    initOperand(0, def);
    initOperand(1, instance);
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(BuiltinInt64ToFloatingPoint)
  NAMED_OPERANDS((0, input), (1, instance));
  TRIVIAL_NEW_WRAPPERS

  bool isUnsigned() const { return isUnsigned_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isBuiltinInt64ToFloatingPoint()) {
      return false;
    }
    if (ins->toBuiltinInt64ToFloatingPoint()->isUnsigned_ != isUnsigned_) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Applies ECMA's ToNumber on a primitive (either typed or untyped) and expects
// the result to be precisely representable as an Int32, otherwise bails.
//
// If the input is not primitive at runtime, a bailout occurs. If the input
// cannot be converted to an int32 without loss (i.e. 5.5 or undefined) then a
// bailout occurs.
class MToNumberInt32 : public MUnaryInstruction, public ToInt32Policy::Data {
  bool needsNegativeZeroCheck_;
  IntConversionInputKind conversion_;

  explicit MToNumberInt32(MDefinition* def, IntConversionInputKind conversion =
                                                IntConversionInputKind::Any)
      : MUnaryInstruction(classOpcode, def),
        needsNegativeZeroCheck_(true),
        conversion_(conversion) {
    setResultType(MIRType::Int32);
    setMovable();

    // Guard unless the conversion is known to be non-effectful & non-throwing.
    if (!def->typeIsOneOf({MIRType::Undefined, MIRType::Null, MIRType::Boolean,
                           MIRType::Int32, MIRType::Double, MIRType::Float32,
                           MIRType::String})) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(ToNumberInt32)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;

  // this only has backwards information flow.
  void analyzeEdgeCasesBackward() override;

  bool needsNegativeZeroCheck() const { return needsNegativeZeroCheck_; }
  void setNeedsNegativeZeroCheck(bool needsCheck) {
    needsNegativeZeroCheck_ = needsCheck;
  }

  IntConversionInputKind conversion() const { return conversion_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isToNumberInt32() ||
        ins->toToNumberInt32()->conversion() != conversion()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;
  void collectRangeInfoPreTrunc() override;

#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

  ALLOW_CLONE(MToNumberInt32)
};

// Converts a value or typed input to a truncated int32, for use with bitwise
// operations. This is an infallible ValueToECMAInt32.
class MTruncateToInt32 : public MUnaryInstruction, public ToInt32Policy::Data {
  wasm::TrapSiteDesc trapSiteDesc_;

  explicit MTruncateToInt32(
      MDefinition* def, wasm::TrapSiteDesc trapSiteDesc = wasm::TrapSiteDesc())
      : MUnaryInstruction(classOpcode, def), trapSiteDesc_(trapSiteDesc) {
    setResultType(MIRType::Int32);
    setMovable();

    // Guard unless the conversion is known to be non-effectful & non-throwing.
    if (mightHaveSideEffects(def)) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(TruncateToInt32)
  TRIVIAL_NEW_WRAPPERS

  static bool mightHaveSideEffects(MDefinition* def) {
    return !def->typeIsOneOf({MIRType::Undefined, MIRType::Null,
                              MIRType::Boolean, MIRType::Int32, MIRType::Double,
                              MIRType::Float32, MIRType::String});
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;
  TruncateKind operandTruncateKind(size_t index) const override;
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    return input()->type() < MIRType::Symbol;
  }

  const wasm::TrapSiteDesc& trapSiteDesc() const { return trapSiteDesc_; }

  ALLOW_CLONE(MTruncateToInt32)
};

// Converts a primitive (either typed or untyped) to a BigInt. If the input is
// not primitive at runtime, a bailout occurs.
class MToBigInt : public MUnaryInstruction, public ToBigIntPolicy::Data {
 private:
  explicit MToBigInt(MDefinition* def) : MUnaryInstruction(classOpcode, def) {
    setResultType(MIRType::BigInt);
    setMovable();

    // Guard unless the conversion is known to be non-effectful & non-throwing.
    if (!def->typeIsOneOf({MIRType::Boolean, MIRType::BigInt})) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(ToBigInt)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MToBigInt)
};

// Takes a Value or typed input and returns a suitable Int64 using the
// ToBigInt algorithm, possibly calling out to the VM for string, etc inputs.
class MToInt64 : public MUnaryInstruction, public ToInt64Policy::Data {
  explicit MToInt64(MDefinition* def) : MUnaryInstruction(classOpcode, def) {
    setResultType(MIRType::Int64);
    setMovable();

    // Guard unless the conversion is known to be non-effectful & non-throwing.
    if (!def->typeIsOneOf(
            {MIRType::Boolean, MIRType::BigInt, MIRType::Int64})) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(ToInt64)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MToInt64)
};

// Takes a BigInt pointer and returns its toInt64 value.
class MTruncateBigIntToInt64 : public MUnaryInstruction,
                               public NoTypePolicy::Data {
  explicit MTruncateBigIntToInt64(MDefinition* def)
      : MUnaryInstruction(classOpcode, def) {
    MOZ_ASSERT(def->type() == MIRType::BigInt);
    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(TruncateBigIntToInt64)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MTruncateBigIntToInt64)
};

// Takes an Int64 and returns a fresh BigInt pointer.
class MInt64ToBigInt : public MUnaryInstruction, public NoTypePolicy::Data {
  bool isSigned_;

  MInt64ToBigInt(MDefinition* def, bool isSigned)
      : MUnaryInstruction(classOpcode, def), isSigned_(isSigned) {
    MOZ_ASSERT(def->type() == MIRType::Int64);
    setResultType(MIRType::BigInt);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Int64ToBigInt)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toInt64ToBigInt()->isSigned() == isSigned();
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isSigned() const { return isSigned_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MInt64ToBigInt)
};

// Takes an Int64 and returns a IntPtr.
class MInt64ToIntPtr : public MUnaryInstruction, public NoTypePolicy::Data {
  bool isSigned_;

  MInt64ToIntPtr(MDefinition* def, bool isSigned)
      : MUnaryInstruction(classOpcode, def), isSigned_(isSigned) {
    MOZ_ASSERT(def->type() == MIRType::Int64);
    setResultType(MIRType::IntPtr);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Int64ToIntPtr)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toInt64ToIntPtr()->isSigned() == isSigned();
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isSigned() const { return isSigned_; }

  ALLOW_CLONE(MInt64ToIntPtr)
};

// Takes a IntPtr and returns an Int64.
class MIntPtrToInt64 : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MIntPtrToInt64(MDefinition* def)
      : MUnaryInstruction(classOpcode, def) {
    MOZ_ASSERT(def->type() == MIRType::IntPtr);
    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(IntPtrToInt64)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MIntPtrToInt64)
};

// Converts any type to a string
class MToString : public MUnaryInstruction, public ToStringPolicy::Data {
 public:
  // MToString has two modes for handling of object/symbol arguments: if the
  // to-string conversion happens as part of another opcode, we have to bail out
  // to Baseline. If the conversion is for a stand-alone JSOp we can support
  // side-effects.
  enum class SideEffectHandling { Bailout, Supported };

 private:
  SideEffectHandling sideEffects_;
  bool mightHaveSideEffects_ = false;

  MToString(MDefinition* def, SideEffectHandling sideEffects)
      : MUnaryInstruction(classOpcode, def), sideEffects_(sideEffects) {
    setResultType(MIRType::String);

    if (!def->typeIsOneOf({MIRType::Undefined, MIRType::Null, MIRType::Boolean,
                           MIRType::Int32, MIRType::Double, MIRType::Float32,
                           MIRType::String, MIRType::BigInt})) {
      mightHaveSideEffects_ = true;
    }

    // If this instruction is not effectful, mark it as movable and set the
    // Guard flag if needed. If the operation is effectful it won't be
    // optimized anyway so there's no need to set any flags.
    if (!isEffectful()) {
      setMovable();
      // Objects might override toString; Symbol throws. We bailout in those
      // cases and run side-effects in baseline instead.
      if (mightHaveSideEffects_) {
        setGuard();
      }
    }
  }

 public:
  INSTRUCTION_HEADER(ToString)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isToString()) {
      return false;
    }
    if (sideEffects_ != ins->toToString()->sideEffects_) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    if (supportSideEffects() && mightHaveSideEffects_) {
      return AliasSet::Store(AliasSet::Any);
    }
    return AliasSet::None();
  }

  bool mightHaveSideEffects() const { return mightHaveSideEffects_; }

  bool supportSideEffects() const {
    return sideEffects_ == SideEffectHandling::Supported;
  }

  bool needsSnapshot() const {
    return sideEffects_ == SideEffectHandling::Bailout && mightHaveSideEffects_;
  }

  ALLOW_CLONE(MToString)
};

class MBitNot : public MUnaryInstruction, public BitwisePolicy::Data {
  MBitNot(MDefinition* input, MIRType type)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64);
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(BitNot)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MBitNot)
};

class MTypeOf : public MUnaryInstruction,
                public BoxExceptPolicy<0, MIRType::Object>::Data {
  explicit MTypeOf(MDefinition* def) : MUnaryInstruction(classOpcode, def) {
    setResultType(MIRType::Int32);
    setMovable();
  }
  TypeDataList observed_;

 public:
  INSTRUCTION_HEADER(TypeOf)
  TRIVIAL_NEW_WRAPPERS

  void setObservedTypes(const TypeDataList& observed) { observed_ = observed; }
  bool hasObservedTypes() const { return observed_.count() > 0; }
  const TypeDataList& observedTypes() const { return observed_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MTypeOfIs : public MUnaryInstruction, public NoTypePolicy::Data {
  JSOp jsop_;
  JSType jstype_;

  MTypeOfIs(MDefinition* def, JSOp jsop, JSType jstype)
      : MUnaryInstruction(classOpcode, def), jsop_(jsop), jstype_(jstype) {
    MOZ_ASSERT(def->type() == MIRType::Object || def->type() == MIRType::Value);

    setResultType(MIRType::Boolean);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(TypeOfIs)
  TRIVIAL_NEW_WRAPPERS

  JSOp jsop() const { return jsop_; }
  JSType jstype() const { return jstype_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    if (!congruentIfOperandsEqual(ins)) {
      return false;
    }
    return jsop() == ins->toTypeOfIs()->jsop() &&
           jstype() == ins->toTypeOfIs()->jstype();
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif
};

class MBinaryBitwiseInstruction : public MBinaryInstruction,
                                  public BitwisePolicy::Data {
 protected:
  MBinaryBitwiseInstruction(Opcode op, MDefinition* left, MDefinition* right,
                            MIRType type)
      : MBinaryInstruction(op, left, right),
        maskMatchesLeftRange(false),
        maskMatchesRightRange(false) {
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64 ||
               (isUrsh() && type == MIRType::Double));
    setResultType(type);
    setMovable();
  }

  bool maskMatchesLeftRange;
  bool maskMatchesRightRange;

 public:
  MDefinition* foldsTo(TempAllocator& alloc) override;
  MDefinition* foldUnnecessaryBitop();
  virtual MDefinition* foldIfZero(size_t operand) = 0;
  virtual MDefinition* foldIfNegOne(size_t operand) = 0;
  virtual MDefinition* foldIfEqual() = 0;
  virtual MDefinition* foldIfAllBitsSet(size_t operand) = 0;
  void collectRangeInfoPreTrunc() override;

  bool congruentTo(const MDefinition* ins) const override {
    return binaryCongruentTo(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  TruncateKind operandTruncateKind(size_t index) const override;
};

class MBitAnd : public MBinaryBitwiseInstruction {
  MBitAnd(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryBitwiseInstruction(classOpcode, left, right, type) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BitAnd)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldIfZero(size_t operand) override {
    return getOperand(operand);  // 0 & x => 0;
  }
  MDefinition* foldIfNegOne(size_t operand) override {
    return getOperand(1 - operand);  // x & -1 => x
  }
  MDefinition* foldIfEqual() override {
    return getOperand(0);  // x & x => x;
  }
  MDefinition* foldIfAllBitsSet(size_t operand) override {
    // e.g. for uint16: x & 0xffff => x;
    return getOperand(1 - operand);
  }
  void computeRange(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MBitAnd)
};

class MBitOr : public MBinaryBitwiseInstruction {
  MBitOr(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryBitwiseInstruction(classOpcode, left, right, type) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BitOr)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldIfZero(size_t operand) override {
    return getOperand(1 -
                      operand);  // 0 | x => x, so if ith is 0, return (1-i)th
  }
  MDefinition* foldIfNegOne(size_t operand) override {
    return getOperand(operand);  // x | -1 => -1
  }
  MDefinition* foldIfEqual() override {
    return getOperand(0);  // x | x => x
  }
  MDefinition* foldIfAllBitsSet(size_t operand) override { return this; }
  void computeRange(TempAllocator& alloc) override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MBitOr)
};

class MBitXor : public MBinaryBitwiseInstruction {
  MBitXor(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryBitwiseInstruction(classOpcode, left, right, type) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BitXor)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldIfZero(size_t operand) override {
    return getOperand(1 - operand);  // 0 ^ x => x
  }
  MDefinition* foldIfNegOne(size_t operand) override { return this; }
  MDefinition* foldIfEqual() override { return this; }
  MDefinition* foldIfAllBitsSet(size_t operand) override { return this; }
  void computeRange(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MBitXor)
};

class MShiftInstruction : public MBinaryBitwiseInstruction {
 protected:
  MShiftInstruction(Opcode op, MDefinition* left, MDefinition* right,
                    MIRType type)
      : MBinaryBitwiseInstruction(op, left, right, type) {}

 public:
  MDefinition* foldIfNegOne(size_t operand) override { return this; }
  MDefinition* foldIfEqual() override { return this; }
  MDefinition* foldIfAllBitsSet(size_t operand) override { return this; }
};

class MLsh : public MShiftInstruction {
  MLsh(MDefinition* left, MDefinition* right, MIRType type)
      : MShiftInstruction(classOpcode, left, right, type) {}

 public:
  INSTRUCTION_HEADER(Lsh)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldIfZero(size_t operand) override {
    // 0 << x => 0
    // x << 0 => x
    return getOperand(0);
  }

  void computeRange(TempAllocator& alloc) override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MLsh)
};

class MRsh : public MShiftInstruction {
  MRsh(MDefinition* left, MDefinition* right, MIRType type)
      : MShiftInstruction(classOpcode, left, right, type) {}

 public:
  INSTRUCTION_HEADER(Rsh)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldIfZero(size_t operand) override {
    // 0 >> x => 0
    // x >> 0 => x
    return getOperand(0);
  }
  void computeRange(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MRsh)
};

class MUrsh : public MShiftInstruction {
  bool bailoutsDisabled_;

  MUrsh(MDefinition* left, MDefinition* right, MIRType type)
      : MShiftInstruction(classOpcode, left, right, type),
        bailoutsDisabled_(false) {}

 public:
  INSTRUCTION_HEADER(Ursh)
  TRIVIAL_NEW_WRAPPERS

  static MUrsh* NewWasm(TempAllocator& alloc, MDefinition* left,
                        MDefinition* right, MIRType type);

  MDefinition* foldIfZero(size_t operand) override {
    // 0 >>> x => 0
    if (operand == 0) {
      return getOperand(0);
    }

    return this;
  }

  bool bailoutsDisabled() const { return bailoutsDisabled_; }

  bool fallible() const;

  void computeRange(TempAllocator& alloc) override;
  void collectRangeInfoPreTrunc() override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MUrsh)
};

class MSignExtendInt32 : public MUnaryInstruction, public NoTypePolicy::Data {
 public:
  enum Mode { Byte, Half };

 private:
  Mode mode_;

  MSignExtendInt32(MDefinition* op, Mode mode)
      : MUnaryInstruction(classOpcode, op), mode_(mode) {
    MOZ_ASSERT(op->type() == MIRType::Int32);
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(SignExtendInt32)
  TRIVIAL_NEW_WRAPPERS

  Mode mode() const { return mode_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!congruentIfOperandsEqual(ins)) {
      return false;
    }
    return ins->toSignExtendInt32()->mode_ == mode_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MSignExtendInt32)
};

class MSignExtendInt64 : public MUnaryInstruction, public NoTypePolicy::Data {
 public:
  enum Mode { Byte, Half, Word };

 private:
  Mode mode_;

  MSignExtendInt64(MDefinition* op, Mode mode)
      : MUnaryInstruction(classOpcode, op), mode_(mode) {
    MOZ_ASSERT(op->type() == MIRType::Int64);
    setResultType(MIRType::Int64);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(SignExtendInt64)
  TRIVIAL_NEW_WRAPPERS

  Mode mode() const { return mode_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!congruentIfOperandsEqual(ins)) {
      return false;
    }
    return ins->toSignExtendInt64()->mode_ == mode_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MSignExtendInt64)
};

class MSignExtendIntPtr : public MUnaryInstruction, public NoTypePolicy::Data {
 public:
  enum Mode { Byte, Half, Word };

 private:
  Mode mode_;

  MSignExtendIntPtr(MDefinition* op, Mode mode)
      : MUnaryInstruction(classOpcode, op), mode_(mode) {
    MOZ_ASSERT(op->type() == MIRType::IntPtr);
    setResultType(MIRType::IntPtr);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(SignExtendIntPtr)
  TRIVIAL_NEW_WRAPPERS

  Mode mode() const { return mode_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!congruentIfOperandsEqual(ins)) {
      return false;
    }
    return ins->toSignExtendIntPtr()->mode_ == mode_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MSignExtendIntPtr)
};

class MBinaryArithInstruction : public MBinaryInstruction,
                                public ArithPolicy::Data {
  // Implicit truncate flag is set by the truncate backward range analysis
  // optimization phase, and by wasm pre-processing. It is used in
  // NeedNegativeZeroCheck to check if the result of a multiplication needs to
  // produce -0 double value, and for avoiding overflow checks.

  // This optimization happens when the multiplication cannot be truncated
  // even if all uses are truncating its result, such as when the range
  // analysis detect a precision loss in the multiplication.
  TruncateKind implicitTruncate_;

  // Whether we must preserve NaN semantics, and in particular not fold
  // (x op id) or (id op x) to x, or replace a division by a multiply of the
  // exact reciprocal.
  bool mustPreserveNaN_;

 protected:
  MBinaryArithInstruction(Opcode op, MDefinition* left, MDefinition* right,
                          MIRType type)
      : MBinaryInstruction(op, left, right),
        implicitTruncate_(TruncateKind::NoTruncate),
        mustPreserveNaN_(false) {
    MOZ_ASSERT(IsNumberType(type));
    setResultType(type);
    setMovable();
  }

 public:
  void setMustPreserveNaN(bool b) { mustPreserveNaN_ = b; }
  bool mustPreserveNaN() const { return mustPreserveNaN_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  virtual double getIdentity() = 0;

  void setSpecialization(MIRType type) {
    MOZ_ASSERT(IsNumberType(type));
    setResultType(type);
  }

  virtual void trySpecializeFloat32(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    if (!binaryCongruentTo(ins)) {
      return false;
    }
    const auto* other = static_cast<const MBinaryArithInstruction*>(ins);
    return other->mustPreserveNaN_ == mustPreserveNaN_;
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isTruncated() const {
    return implicitTruncate_ == TruncateKind::Truncate;
  }
  TruncateKind truncateKind() const { return implicitTruncate_; }
  void setTruncateKind(TruncateKind kind) {
    implicitTruncate_ = std::max(implicitTruncate_, kind);
  }
};

class MMinMax : public MBinaryInstruction, public ArithPolicy::Data {
  bool isMax_;

  MMinMax(MDefinition* left, MDefinition* right, MIRType type, bool isMax)
      : MBinaryInstruction(classOpcode, left, right), isMax_(isMax) {
    MOZ_ASSERT(IsNumberType(type));
    setResultType(type);
    setMovable();
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(MinMax)
  TRIVIAL_NEW_WRAPPERS

  template <typename... Args>
  static MMinMax* NewMin(Args&&... args) {
    return New(std::forward<Args>(args)..., /* isMax= */ false);
  }

  template <typename... Args>
  static MMinMax* NewMax(Args&&... args) {
    return New(std::forward<Args>(args)..., /* isMax= */ true);
  }

  static MMinMax* NewWasm(TempAllocator& alloc, MDefinition* left,
                          MDefinition* right, MIRType type, bool isMax) {
    return New(alloc, left, right, type, isMax);
  }

  bool isMax() const { return isMax_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!congruentIfOperandsEqual(ins)) {
      return false;
    }
    const MMinMax* other = ins->toMinMax();
    return other->isMax() == isMax();
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  MDefinition* foldsTo(TempAllocator& alloc) override;
  void computeRange(TempAllocator& alloc) override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  ALLOW_CLONE(MMinMax)
};

class MMinMaxArray : public MUnaryInstruction, public SingleObjectPolicy::Data {
  bool isMax_;

  MMinMaxArray(MDefinition* array, MIRType type, bool isMax)
      : MUnaryInstruction(classOpcode, array), isMax_(isMax) {
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Double);
    setResultType(type);

    // We can't DCE this, even if the result is unused, in case one of the
    // elements of the array is an object with a `valueOf` function that
    // must be called.
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(MinMaxArray)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, array))

  bool isMax() const { return isMax_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isMinMaxArray() || ins->toMinMaxArray()->isMax() != isMax()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::ObjectFields | AliasSet::Element);
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif
};

class MAbs : public MUnaryInstruction, public ArithPolicy::Data {
  bool implicitTruncate_;

  MAbs(MDefinition* num, MIRType type)
      : MUnaryInstruction(classOpcode, num), implicitTruncate_(false) {
    MOZ_ASSERT(IsNumberType(type));
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Abs)
  TRIVIAL_NEW_WRAPPERS

  static MAbs* NewWasm(TempAllocator& alloc, MDefinition* num, MIRType type) {
    auto* ins = new (alloc) MAbs(num, type);
    if (type == MIRType::Int32) {
      ins->implicitTruncate_ = true;
    }
    return ins;
  }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  bool fallible() const;

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;
  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MAbs)
};

class MClz : public MUnaryInstruction, public BitwisePolicy::Data {
  bool operandIsNeverZero_;

  explicit MClz(MDefinition* num, MIRType type)
      : MUnaryInstruction(classOpcode, num), operandIsNeverZero_(false) {
    MOZ_ASSERT(IsIntType(type));
    MOZ_ASSERT(IsNumberType(num->type()));
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Clz)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, num))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool operandIsNeverZero() const { return operandIsNeverZero_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  void computeRange(TempAllocator& alloc) override;
  void collectRangeInfoPreTrunc() override;

  ALLOW_CLONE(MClz)
};

class MCtz : public MUnaryInstruction, public BitwisePolicy::Data {
  bool operandIsNeverZero_;

  explicit MCtz(MDefinition* num, MIRType type)
      : MUnaryInstruction(classOpcode, num), operandIsNeverZero_(false) {
    MOZ_ASSERT(IsIntType(type));
    MOZ_ASSERT(IsNumberType(num->type()));
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Ctz)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, num))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool operandIsNeverZero() const { return operandIsNeverZero_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  void computeRange(TempAllocator& alloc) override;
  void collectRangeInfoPreTrunc() override;

  ALLOW_CLONE(MCtz)
};

class MPopcnt : public MUnaryInstruction, public BitwisePolicy::Data {
  explicit MPopcnt(MDefinition* num, MIRType type)
      : MUnaryInstruction(classOpcode, num) {
    MOZ_ASSERT(IsNumberType(num->type()));
    MOZ_ASSERT(IsIntType(type));
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Popcnt)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, num))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  void computeRange(TempAllocator& alloc) override;

  ALLOW_CLONE(MPopcnt)
};

// Inline implementation of Math.sqrt().
class MSqrt : public MUnaryInstruction, public FloatingPointPolicy<0>::Data {
  MSqrt(MDefinition* num, MIRType type) : MUnaryInstruction(classOpcode, num) {
    setResultType(type);
    specialization_ = type;
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Sqrt)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;

  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MSqrt)
};

class MCopySign : public MBinaryInstruction, public NoTypePolicy::Data {
  MCopySign(MDefinition* lhs, MDefinition* rhs, MIRType type)
      : MBinaryInstruction(classOpcode, lhs, rhs) {
    setResultType(type);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(CopySign)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MCopySign)
};

// Inline implementation of Math.hypot().
class MHypot : public MVariadicInstruction, public AllDoublePolicy::Data {
  MHypot() : MVariadicInstruction(classOpcode) {
    setResultType(MIRType::Double);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Hypot)
  static MHypot* New(TempAllocator& alloc, const MDefinitionVector& vector);

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool possiblyCalls() const override { return true; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  bool canClone() const override { return true; }

  MInstruction* clone(TempAllocator& alloc,
                      const MDefinitionVector& inputs) const override {
    return MHypot::New(alloc, inputs);
  }
};

// Inline implementation of Math.pow().
//
// Supports the following three specializations:
//
// 1. MPow(FloatingPoint, FloatingPoint) -> Double
//   - The most general mode, calls js::ecmaPow.
//   - Never performs a bailout.
// 2. MPow(FloatingPoint, Int32) -> Double
//   - Optimization to call js::powi instead of js::ecmaPow.
//   - Never performs a bailout.
// 3. MPow(Int32, Int32) -> Int32
//   - Performs the complete exponentiation operation in assembly code.
//   - Bails out if the result doesn't fit in Int32.
class MPow : public MBinaryInstruction, public PowPolicy::Data {
  // If true, the result is guaranteed to never be negative zero, as long as the
  // power is a positive number.
  bool canBeNegativeZero_;

  MPow(MDefinition* input, MDefinition* power, MIRType specialization)
      : MBinaryInstruction(classOpcode, input, power) {
    MOZ_ASSERT(specialization == MIRType::Int32 ||
               specialization == MIRType::Double);
    setResultType(specialization);
    setMovable();

    // The result can't be negative zero if the base is an Int32 value.
    canBeNegativeZero_ = input->type() != MIRType::Int32;
  }

  // Helpers for `foldsTo`
  MDefinition* foldsConstant(TempAllocator& alloc);
  MDefinition* foldsConstantPower(TempAllocator& alloc);

  bool canBeNegativeZero() const { return canBeNegativeZero_; }

 public:
  INSTRUCTION_HEADER(Pow)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* input() const { return lhs(); }
  MDefinition* power() const { return rhs(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool possiblyCalls() const override { return type() != MIRType::Int32; }
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  ALLOW_CLONE(MPow)
};

// Inline implementation of Math.pow(x, 0.5), which subtly differs from
// Math.sqrt(x).
class MPowHalf : public MUnaryInstruction, public DoublePolicy<0>::Data {
  bool operandIsNeverNegativeInfinity_;
  bool operandIsNeverNegativeZero_;
  bool operandIsNeverNaN_;

  explicit MPowHalf(MDefinition* input)
      : MUnaryInstruction(classOpcode, input),
        operandIsNeverNegativeInfinity_(false),
        operandIsNeverNegativeZero_(false),
        operandIsNeverNaN_(false) {
    setResultType(MIRType::Double);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(PowHalf)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  bool operandIsNeverNegativeInfinity() const {
    return operandIsNeverNegativeInfinity_;
  }
  bool operandIsNeverNegativeZero() const {
    return operandIsNeverNegativeZero_;
  }
  bool operandIsNeverNaN() const { return operandIsNeverNaN_; }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void collectRangeInfoPreTrunc() override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MPowHalf)
};

class MSign : public MUnaryInstruction, public SignPolicy::Data {
 private:
  MSign(MDefinition* input, MIRType resultType)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(IsNumberType(input->type()));
    MOZ_ASSERT(resultType == MIRType::Int32 || resultType == MIRType::Double);
    specialization_ = input->type();
    setResultType(resultType);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Sign)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  void computeRange(TempAllocator& alloc) override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MSign)
};

class MMathFunction : public MUnaryInstruction,
                      public FloatingPointPolicy<0>::Data {
  UnaryMathFunction function_;

  // A nullptr cache means this function will neither access nor update the
  // cache.
  MMathFunction(MDefinition* input, UnaryMathFunction function)
      : MUnaryInstruction(classOpcode, input), function_(function) {
    setResultType(MIRType::Double);
    specialization_ = MIRType::Double;
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(MathFunction)
  TRIVIAL_NEW_WRAPPERS

  UnaryMathFunction function() const { return function_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isMathFunction()) {
      return false;
    }
    if (ins->toMathFunction()->function() != function()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool possiblyCalls() const override { return true; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  static const char* FunctionName(UnaryMathFunction function);

  bool isFloat32Commutative() const override;
  void trySpecializeFloat32(TempAllocator& alloc) override;

  void computeRange(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MMathFunction)
};

class MAdd : public MBinaryArithInstruction {
  MAdd(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryArithInstruction(classOpcode, left, right, type) {
    setCommutative();
  }

  MAdd(MDefinition* left, MDefinition* right, TruncateKind truncateKind)
      : MAdd(left, right, MIRType::Int32) {
    setTruncateKind(truncateKind);
  }

 public:
  INSTRUCTION_HEADER(Add)
  TRIVIAL_NEW_WRAPPERS

  static MAdd* NewWasm(TempAllocator& alloc, MDefinition* left,
                       MDefinition* right, MIRType type) {
    auto* ret = new (alloc) MAdd(left, right, type);
    if (type == MIRType::Int32) {
      ret->setTruncateKind(TruncateKind::Truncate);
    }
    return ret;
  }

  bool isFloat32Commutative() const override { return true; }

  double getIdentity() override { return 0; }

  bool fallible() const;
  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  TruncateKind operandTruncateKind(size_t index) const override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MAdd)
};

class MSub : public MBinaryArithInstruction {
  MSub(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryArithInstruction(classOpcode, left, right, type) {}

 public:
  INSTRUCTION_HEADER(Sub)
  TRIVIAL_NEW_WRAPPERS

  static MSub* NewWasm(TempAllocator& alloc, MDefinition* left,
                       MDefinition* right, MIRType type, bool mustPreserveNaN) {
    auto* ret = new (alloc) MSub(left, right, type);
    ret->setMustPreserveNaN(mustPreserveNaN);
    if (type == MIRType::Int32) {
      ret->setTruncateKind(TruncateKind::Truncate);
    }
    return ret;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  double getIdentity() override { return 0; }

  bool isFloat32Commutative() const override { return true; }

  bool fallible() const;
  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  TruncateKind operandTruncateKind(size_t index) const override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MSub)
};

class MMul : public MBinaryArithInstruction {
 public:
  enum Mode { Normal, Integer };

 private:
  // Annotation the result could be a negative zero
  // and we need to guard this during execution.
  bool canBeNegativeZero_;

  Mode mode_;

  MMul(MDefinition* left, MDefinition* right, MIRType type, Mode mode)
      : MBinaryArithInstruction(classOpcode, left, right, type),
        canBeNegativeZero_(true),
        mode_(mode) {
    setCommutative();
    if (mode == Integer) {
      // This implements the required behavior for Math.imul, which
      // can never fail and always truncates its output to int32.
      canBeNegativeZero_ = false;
      setTruncateKind(TruncateKind::Truncate);
    }
    MOZ_ASSERT_IF(mode != Integer, mode == Normal);
  }

 public:
  INSTRUCTION_HEADER(Mul)

  static MMul* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                   MIRType type, Mode mode = Normal) {
    return new (alloc) MMul(left, right, type, mode);
  }
  static MMul* NewWasm(TempAllocator& alloc, MDefinition* left,
                       MDefinition* right, MIRType type, Mode mode,
                       bool mustPreserveNaN) {
    auto* ret = new (alloc) MMul(left, right, type, mode);
    ret->setMustPreserveNaN(mustPreserveNaN);
    return ret;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  void analyzeEdgeCasesForward() override;
  void analyzeEdgeCasesBackward() override;
  void collectRangeInfoPreTrunc() override;

  double getIdentity() override { return 1; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isMul()) {
      return false;
    }

    const MMul* mul = ins->toMul();
    if (canBeNegativeZero_ != mul->canBeNegativeZero()) {
      return false;
    }

    if (mode_ != mul->mode()) {
      return false;
    }

    if (mustPreserveNaN() != mul->mustPreserveNaN()) {
      return false;
    }

    return binaryCongruentTo(ins);
  }

  bool canOverflow() const;

  bool canBeNegativeZero() const { return canBeNegativeZero_; }
  void setCanBeNegativeZero(bool negativeZero) {
    canBeNegativeZero_ = negativeZero;
  }

  bool fallible() const { return canBeNegativeZero_ || canOverflow(); }

  bool isFloat32Commutative() const override { return true; }

  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  TruncateKind operandTruncateKind(size_t index) const override;

  Mode mode() const { return mode_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  ALLOW_CLONE(MMul)
};

class MDiv : public MBinaryArithInstruction {
  bool canBeNegativeZero_;
  bool canBeNegativeOverflow_;
  bool canBeDivideByZero_;
  bool canBeNegativeDividend_;
  bool unsigned_;  // If false, signedness will be derived from operands
  bool trapOnError_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MDiv(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryArithInstruction(classOpcode, left, right, type),
        canBeNegativeZero_(true),
        canBeNegativeOverflow_(true),
        canBeDivideByZero_(true),
        canBeNegativeDividend_(true),
        unsigned_(false),
        trapOnError_(false) {}

 public:
  INSTRUCTION_HEADER(Div)

  static MDiv* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                   MIRType type) {
    return new (alloc) MDiv(left, right, type);
  }
  static MDiv* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                   MIRType type, bool unsignd, bool trapOnError = false,
                   wasm::TrapSiteDesc trapSiteDesc = wasm::TrapSiteDesc(),
                   bool mustPreserveNaN = false) {
    auto* div = new (alloc) MDiv(left, right, type);
    div->unsigned_ = unsignd;
    div->trapOnError_ = trapOnError;
    div->trapSiteDesc_ = trapSiteDesc;
    if (trapOnError) {
      div->setGuard();  // not removable because of possible side-effects.
      div->setNotMovable();
    }
    div->setMustPreserveNaN(mustPreserveNaN);
    if (type == MIRType::Int32) {
      div->setTruncateKind(TruncateKind::Truncate);
    }
    return div;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  void analyzeEdgeCasesForward() override;
  void analyzeEdgeCasesBackward() override;

  double getIdentity() override { MOZ_CRASH("not used"); }

  bool canBeNegativeZero() const {
    // This flag is only valid for integer division.
    MOZ_ASSERT(type() == MIRType::Int32);
    return canBeNegativeZero_;
  }
  void setCanBeNegativeZero(bool negativeZero) {
    canBeNegativeZero_ = negativeZero;
  }

  bool canBeNegativeOverflow() const { return canBeNegativeOverflow_; }

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  bool canBeNegativeDividend() const {
    // "Dividend" is an ambiguous concept for unsigned truncated
    // division, because of the truncation procedure:
    // ((x>>>0)/2)|0, for example, gets transformed in
    // MDiv::truncate into a node with lhs representing x (not
    // x>>>0) and rhs representing the constant 2; in other words,
    // the MIR node corresponds to "cast operands to unsigned and
    // divide" operation. In this case, is the dividend x or is it
    // x>>>0? In order to resolve such ambiguities, we disallow
    // the usage of this method for unsigned division.
    MOZ_ASSERT(!unsigned_);
    return canBeNegativeDividend_;
  }

  bool isUnsigned() const { return unsigned_; }

  bool isTruncatedIndirectly() const {
    return truncateKind() >= TruncateKind::IndirectTruncate;
  }

  bool canTruncateInfinities() const { return isTruncated(); }
  bool canTruncateRemainder() const { return isTruncated(); }
  bool canTruncateOverflow() const {
    return isTruncated() || isTruncatedIndirectly();
  }
  bool canTruncateNegativeZero() const {
    return isTruncated() || isTruncatedIndirectly();
  }

  bool trapOnError() const { return trapOnError_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const {
    MOZ_ASSERT(trapSiteDesc_.isValid());
    return trapSiteDesc_;
  }

  bool isFloat32Commutative() const override { return true; }

  void computeRange(TempAllocator& alloc) override;
  bool fallible() const;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  void collectRangeInfoPreTrunc() override;
  TruncateKind operandTruncateKind(size_t index) const override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!MBinaryArithInstruction::congruentTo(ins)) {
      return false;
    }
    const MDiv* other = ins->toDiv();
    MOZ_ASSERT(other->trapOnError() == trapOnError_);
    return unsigned_ == other->isUnsigned();
  }

  ALLOW_CLONE(MDiv)
};

class MMod : public MBinaryArithInstruction {
  bool unsigned_;  // If false, signedness will be derived from operands
  bool canBeNegativeDividend_;
  bool canBePowerOfTwoDivisor_;
  bool canBeDivideByZero_;
  bool trapOnError_;
  wasm::TrapSiteDesc trapSiteDesc_;

  MMod(MDefinition* left, MDefinition* right, MIRType type)
      : MBinaryArithInstruction(classOpcode, left, right, type),
        unsigned_(false),
        canBeNegativeDividend_(true),
        canBePowerOfTwoDivisor_(true),
        canBeDivideByZero_(true),
        trapOnError_(false) {}

 public:
  INSTRUCTION_HEADER(Mod)

  static MMod* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                   MIRType type) {
    return new (alloc) MMod(left, right, type);
  }
  static MMod* New(TempAllocator& alloc, MDefinition* left, MDefinition* right,
                   MIRType type, bool unsignd, bool trapOnError = false,
                   wasm::TrapSiteDesc trapSiteDesc = wasm::TrapSiteDesc()) {
    auto* mod = new (alloc) MMod(left, right, type);
    mod->unsigned_ = unsignd;
    mod->trapOnError_ = trapOnError;
    mod->trapSiteDesc_ = trapSiteDesc;
    if (trapOnError) {
      mod->setGuard();  // not removable because of possible side-effects.
      mod->setNotMovable();
    }
    if (type == MIRType::Int32) {
      mod->setTruncateKind(TruncateKind::Truncate);
    }
    return mod;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  double getIdentity() override { MOZ_CRASH("not used"); }

  bool canBeNegativeDividend() const {
    MOZ_ASSERT(type() == MIRType::Int32 || type() == MIRType::Int64);
    MOZ_ASSERT(!unsigned_);
    return canBeNegativeDividend_;
  }

  bool canBeDivideByZero() const {
    MOZ_ASSERT(type() == MIRType::Int32 || type() == MIRType::Int64);
    return canBeDivideByZero_;
  }

  bool canBePowerOfTwoDivisor() const {
    MOZ_ASSERT(type() == MIRType::Int32);
    return canBePowerOfTwoDivisor_;
  }

  void analyzeEdgeCasesForward() override;

  bool isUnsigned() const { return unsigned_; }

  bool trapOnError() const { return trapOnError_; }
  const wasm::TrapSiteDesc& trapSiteDesc() const {
    MOZ_ASSERT(trapSiteDesc_.isValid());
    return trapSiteDesc_;
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return type() != MIRType::Int64; }

  bool fallible() const;

  void computeRange(TempAllocator& alloc) override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;
  void collectRangeInfoPreTrunc() override;
  TruncateKind operandTruncateKind(size_t index) const override;

  bool congruentTo(const MDefinition* ins) const override {
    return MBinaryArithInstruction::congruentTo(ins) &&
           unsigned_ == ins->toMod()->isUnsigned();
  }

  bool possiblyCalls() const override { return type() == MIRType::Double; }

  ALLOW_CLONE(MMod)
};

class MBigIntBinaryArithInstruction : public MBinaryInstruction,
                                      public BigIntArithPolicy::Data {
 protected:
  MBigIntBinaryArithInstruction(Opcode op, MDefinition* left,
                                MDefinition* right)
      : MBinaryInstruction(op, left, right) {
    setResultType(MIRType::BigInt);
    setMovable();
  }

 public:
  bool congruentTo(const MDefinition* ins) const override {
    return binaryCongruentTo(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MBigIntAdd : public MBigIntBinaryArithInstruction {
  MBigIntAdd(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();

    // Don't guard this instruction even though adding two BigInts can throw
    // JSMSG_BIGINT_TOO_LARGE. This matches the behavior when adding too large
    // strings in MConcat.
  }

 public:
  INSTRUCTION_HEADER(BigIntAdd)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntAdd)
};

class MBigIntSub : public MBigIntBinaryArithInstruction {
  MBigIntSub(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntSub)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntSub)
};

class MBigIntMul : public MBigIntBinaryArithInstruction {
  MBigIntMul(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();

    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntMul)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntMul)
};

class MBigIntDiv : public MBigIntBinaryArithInstruction {
  bool canBeDivideByZero_;

  MBigIntDiv(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    MOZ_ASSERT(right->type() == MIRType::BigInt);
    canBeDivideByZero_ =
        !right->isConstant() || right->toConstant()->toBigInt()->isZero();

    // Throws when the divisor is zero.
    if (canBeDivideByZero_) {
      setGuard();
      setNotMovable();
    }
  }

 public:
  INSTRUCTION_HEADER(BigIntDiv)
  TRIVIAL_NEW_WRAPPERS

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  AliasSet getAliasSet() const override {
    if (canBeDivideByZero()) {
      return AliasSet::Store(AliasSet::ExceptionState);
    }
    return AliasSet::None();
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return !canBeDivideByZero(); }

  ALLOW_CLONE(MBigIntDiv)
};

class MBigIntMod : public MBigIntBinaryArithInstruction {
  bool canBeDivideByZero_;

  MBigIntMod(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    MOZ_ASSERT(right->type() == MIRType::BigInt);
    canBeDivideByZero_ =
        !right->isConstant() || right->toConstant()->toBigInt()->isZero();

    // Throws when the divisor is zero.
    if (canBeDivideByZero_) {
      setGuard();
      setNotMovable();
    }
  }

 public:
  INSTRUCTION_HEADER(BigIntMod)
  TRIVIAL_NEW_WRAPPERS

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  AliasSet getAliasSet() const override {
    if (canBeDivideByZero()) {
      return AliasSet::Store(AliasSet::ExceptionState);
    }
    return AliasSet::None();
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return !canBeDivideByZero(); }

  ALLOW_CLONE(MBigIntMod)
};

class MBigIntPow : public MBigIntBinaryArithInstruction {
  bool canBeNegativeExponent_;

  MBigIntPow(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    MOZ_ASSERT(right->type() == MIRType::BigInt);
    canBeNegativeExponent_ =
        !right->isConstant() || right->toConstant()->toBigInt()->isNegative();

    // Throws when the exponent is negative.
    if (canBeNegativeExponent_) {
      setGuard();
      setNotMovable();
    }
  }

 public:
  INSTRUCTION_HEADER(BigIntPow)
  TRIVIAL_NEW_WRAPPERS

  bool canBeNegativeExponent() const { return canBeNegativeExponent_; }

  AliasSet getAliasSet() const override {
    if (canBeNegativeExponent()) {
      return AliasSet::Store(AliasSet::ExceptionState);
    }
    return AliasSet::None();
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return !canBeNegativeExponent(); }

  ALLOW_CLONE(MBigIntPow)
};

class MBigIntBitAnd : public MBigIntBinaryArithInstruction {
  MBigIntBitAnd(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();

    // We don't need to guard this instruction because it can only fail on OOM.
  }

 public:
  INSTRUCTION_HEADER(BigIntBitAnd)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntBitAnd)
};

class MBigIntBitOr : public MBigIntBinaryArithInstruction {
  MBigIntBitOr(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();

    // We don't need to guard this instruction because it can only fail on OOM.
  }

 public:
  INSTRUCTION_HEADER(BigIntBitOr)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntBitOr)
};

class MBigIntBitXor : public MBigIntBinaryArithInstruction {
  MBigIntBitXor(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();

    // We don't need to guard this instruction because it can only fail on OOM.
  }

 public:
  INSTRUCTION_HEADER(BigIntBitXor)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntBitXor)
};

class MBigIntLsh : public MBigIntBinaryArithInstruction {
  MBigIntLsh(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntLsh)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntLsh)
};

class MBigIntRsh : public MBigIntBinaryArithInstruction {
  MBigIntRsh(MDefinition* left, MDefinition* right)
      : MBigIntBinaryArithInstruction(classOpcode, left, right) {
    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntRsh)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntRsh)
};

class MBigIntUnaryArithInstruction : public MUnaryInstruction,
                                     public BigIntArithPolicy::Data {
 protected:
  MBigIntUnaryArithInstruction(Opcode op, MDefinition* input)
      : MUnaryInstruction(op, input) {
    setResultType(MIRType::BigInt);
    setMovable();
  }

 public:
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MBigIntIncrement : public MBigIntUnaryArithInstruction {
  explicit MBigIntIncrement(MDefinition* input)
      : MBigIntUnaryArithInstruction(classOpcode, input) {
    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntIncrement)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntIncrement)
};

class MBigIntDecrement : public MBigIntUnaryArithInstruction {
  explicit MBigIntDecrement(MDefinition* input)
      : MBigIntUnaryArithInstruction(classOpcode, input) {
    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntDecrement)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntDecrement)
};

class MBigIntNegate : public MBigIntUnaryArithInstruction {
  explicit MBigIntNegate(MDefinition* input)
      : MBigIntUnaryArithInstruction(classOpcode, input) {
    // We don't need to guard this instruction because it can only fail on OOM.
  }

 public:
  INSTRUCTION_HEADER(BigIntNegate)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntNegate)
};

class MBigIntBitNot : public MBigIntUnaryArithInstruction {
  explicit MBigIntBitNot(MDefinition* input)
      : MBigIntUnaryArithInstruction(classOpcode, input) {
    // See MBigIntAdd for why we don't guard this instruction.
  }

 public:
  INSTRUCTION_HEADER(BigIntBitNot)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntBitNot)
};

class MBigIntPtrBinaryArithInstruction : public MBinaryInstruction,
                                         public NoTypePolicy::Data {
 protected:
  MBigIntPtrBinaryArithInstruction(Opcode op, MDefinition* left,
                                   MDefinition* right)
      : MBinaryInstruction(op, left, right) {
    MOZ_ASSERT(left->type() == MIRType::IntPtr);
    MOZ_ASSERT(right->type() == MIRType::IntPtr);
    setResultType(MIRType::IntPtr);
    setMovable();
  }

  static bool isMaybeZero(MDefinition* ins);
  static bool isMaybeNegative(MDefinition* ins);

 public:
  bool congruentTo(const MDefinition* ins) const override {
    return binaryCongruentTo(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MBigIntPtrAdd : public MBigIntPtrBinaryArithInstruction {
  MBigIntPtrAdd(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrAdd)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrAdd)
};

class MBigIntPtrSub : public MBigIntPtrBinaryArithInstruction {
  MBigIntPtrSub(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryArithInstruction(classOpcode, left, right) {}

 public:
  INSTRUCTION_HEADER(BigIntPtrSub)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrSub)
};

class MBigIntPtrMul : public MBigIntPtrBinaryArithInstruction {
  MBigIntPtrMul(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryArithInstruction(classOpcode, left, right) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrMul)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrMul)
};

class MBigIntPtrDiv : public MBigIntPtrBinaryArithInstruction {
  bool canBeDivideByZero_;

  MBigIntPtrDiv(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryArithInstruction(classOpcode, left, right) {
    canBeDivideByZero_ = isMaybeZero(right);

    // Bails when the divisor is zero.
    if (canBeDivideByZero_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrDiv)
  TRIVIAL_NEW_WRAPPERS

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrDiv)
};

class MBigIntPtrMod : public MBigIntPtrBinaryArithInstruction {
  bool canBeDivideByZero_;

  MBigIntPtrMod(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryArithInstruction(classOpcode, left, right) {
    canBeDivideByZero_ = isMaybeZero(right);

    // Bails when the divisor is zero.
    if (canBeDivideByZero_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrMod)
  TRIVIAL_NEW_WRAPPERS

  bool canBeDivideByZero() const { return canBeDivideByZero_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrMod)
};

class MBigIntPtrPow : public MBigIntPtrBinaryArithInstruction {
  bool canBeNegativeExponent_;

  MBigIntPtrPow(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryArithInstruction(classOpcode, left, right) {
    canBeNegativeExponent_ = isMaybeNegative(right);

    // Bails when the exponent is negative.
    if (canBeNegativeExponent_) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrPow)
  TRIVIAL_NEW_WRAPPERS

  bool canBeNegativeExponent() const { return canBeNegativeExponent_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrPow)
};

class MBigIntPtrBinaryBitwiseInstruction : public MBinaryInstruction,
                                           public NoTypePolicy::Data {
 protected:
  MBigIntPtrBinaryBitwiseInstruction(Opcode op, MDefinition* left,
                                     MDefinition* right)
      : MBinaryInstruction(op, left, right) {
    MOZ_ASSERT(left->type() == MIRType::IntPtr);
    MOZ_ASSERT(right->type() == MIRType::IntPtr);
    setResultType(MIRType::IntPtr);
    setMovable();
  }

 public:
  bool congruentTo(const MDefinition* ins) const override {
    return binaryCongruentTo(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MBigIntPtrBitAnd : public MBigIntPtrBinaryBitwiseInstruction {
  MBigIntPtrBitAnd(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryBitwiseInstruction(classOpcode, left, right) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrBitAnd)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrBitAnd)
};

class MBigIntPtrBitOr : public MBigIntPtrBinaryBitwiseInstruction {
  MBigIntPtrBitOr(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryBitwiseInstruction(classOpcode, left, right) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrBitOr)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrBitOr)
};

class MBigIntPtrBitXor : public MBigIntPtrBinaryBitwiseInstruction {
  MBigIntPtrBitXor(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryBitwiseInstruction(classOpcode, left, right) {
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrBitXor)
  TRIVIAL_NEW_WRAPPERS

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrBitXor)
};

class MBigIntPtrLsh : public MBigIntPtrBinaryBitwiseInstruction {
  MBigIntPtrLsh(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryBitwiseInstruction(classOpcode, left, right) {}

 public:
  INSTRUCTION_HEADER(BigIntPtrLsh)
  TRIVIAL_NEW_WRAPPERS

  bool fallible() const {
    return !rhs()->isConstant() || rhs()->toConstant()->toIntPtr() > 0;
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrLsh)
};

class MBigIntPtrRsh : public MBigIntPtrBinaryBitwiseInstruction {
  MBigIntPtrRsh(MDefinition* left, MDefinition* right)
      : MBigIntPtrBinaryBitwiseInstruction(classOpcode, left, right) {}

 public:
  INSTRUCTION_HEADER(BigIntPtrRsh)
  TRIVIAL_NEW_WRAPPERS

  bool fallible() const {
    return !rhs()->isConstant() || rhs()->toConstant()->toIntPtr() < 0;
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrRsh)
};

class MBigIntPtrBitNot : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MBigIntPtrBitNot(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(input->type() == MIRType::IntPtr);
    setResultType(MIRType::IntPtr);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(BigIntPtrBitNot)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MBigIntPtrBitNot)
};

class MConcat : public MBinaryInstruction,
                public MixPolicy<ConvertToStringPolicy<0>,
                                 ConvertToStringPolicy<1>>::Data {
  MConcat(MDefinition* left, MDefinition* right)
      : MBinaryInstruction(classOpcode, left, right) {
    // At least one input should be definitely string
    MOZ_ASSERT(left->type() == MIRType::String ||
               right->type() == MIRType::String);

    setMovable();
    setResultType(MIRType::String);
  }

 public:
  INSTRUCTION_HEADER(Concat)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MConcat)
};

class MStringConvertCase : public MUnaryInstruction,
                           public StringPolicy<0>::Data {
 public:
  enum Mode { LowerCase, UpperCase };

 private:
  Mode mode_;

  MStringConvertCase(MDefinition* string, Mode mode)
      : MUnaryInstruction(classOpcode, string), mode_(mode) {
    setResultType(MIRType::String);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(StringConvertCase)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, string))

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toStringConvertCase()->mode() == mode();
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool possiblyCalls() const override { return true; }
  Mode mode() const { return mode_; }
};

class MCharCodeConvertCase : public MUnaryInstruction,
                             public UnboxedInt32Policy<0>::Data {
 public:
  enum Mode { LowerCase, UpperCase };

 private:
  Mode mode_;

  MCharCodeConvertCase(MDefinition* code, Mode mode)
      : MUnaryInstruction(classOpcode, code), mode_(mode) {
    setResultType(MIRType::String);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(CharCodeConvertCase)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, code))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toCharCodeConvertCase()->mode() == mode();
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  Mode mode() const { return mode_; }
};

// This is a 3 state flag used by FlagPhiInputsAsImplicitlyUsed to record and
// propagate the information about the consumers of a Phi instruction. This is
// then used to set ImplicitlyUsed flags on the inputs of such Phi instructions.
enum class PhiUsage : uint8_t { Unknown, Unused, Used };

using PhiVector = Vector<MPhi*, 4, JitAllocPolicy>;

class MPhi final : public MDefinition,
                   public InlineListNode<MPhi>,
                   public NoTypePolicy::Data {
  using InputVector = js::Vector<MUse, 2, JitAllocPolicy>;
  InputVector inputs_;

  // Important: if you add fields to this class, be sure to update its ::clone
  // method accordingly.
  TruncateKind truncateKind_;
  bool triedToSpecialize_;
  bool isIterator_;
  bool canProduceFloat32_;
  bool canConsumeFloat32_;
  // Record the state of the data flow before any mutation made to the control
  // flow, such that removing branches is properly accounted for.
  PhiUsage usageAnalysis_;

 protected:
  MUse* getUseFor(size_t index) override {
    MOZ_ASSERT(index < numOperands());
    return &inputs_[index];
  }
  const MUse* getUseFor(size_t index) const override { return &inputs_[index]; }

 public:
  INSTRUCTION_HEADER_WITHOUT_TYPEPOLICY(Phi)
  virtual const TypePolicy* typePolicy();
  virtual MIRType typePolicySpecialization();

  MPhi(TempAllocator& alloc, MIRType resultType)
      : MDefinition(classOpcode),
        inputs_(alloc),
        truncateKind_(TruncateKind::NoTruncate),
        triedToSpecialize_(false),
        isIterator_(false),
        canProduceFloat32_(false),
        canConsumeFloat32_(false),
        usageAnalysis_(PhiUsage::Unknown) {
    setResultType(resultType);
  }

  static MPhi* New(TempAllocator& alloc, MIRType resultType = MIRType::Value) {
    return new (alloc) MPhi(alloc, resultType);
  }
  static MPhi* New(TempAllocator::Fallible alloc,
                   MIRType resultType = MIRType::Value) {
    return new (alloc) MPhi(alloc.alloc, resultType);
  }

  MPhi* clone(TempAllocator& alloc, const MDefinitionVector& inputs) const {
    MOZ_ASSERT(inputs.length() == inputs_.length());
    MPhi* phi = MPhi::New(alloc);
    if (!phi || !phi->reserveLength(inputs.length())) {
      return nullptr;
    }
    for (const auto& inp : inputs) {
      phi->addInput(inp);
    }
    phi->truncateKind_ = truncateKind_;
    phi->triedToSpecialize_ = triedToSpecialize_;
    phi->isIterator_ = isIterator_;
    phi->canProduceFloat32_ = canProduceFloat32_;
    phi->canConsumeFloat32_ = canConsumeFloat32_;
    phi->usageAnalysis_ = usageAnalysis_;
    phi->setResultType(type());
    return phi;
  }

  void removeOperand(size_t index);
  void removeAllOperands();

  MDefinition* getOperand(size_t index) const override {
    return inputs_[index].producer();
  }
  size_t numOperands() const override { return inputs_.length(); }
  size_t indexOf(const MUse* u) const final {
    MOZ_ASSERT(u >= &inputs_[0]);
    MOZ_ASSERT(u <= &inputs_[numOperands() - 1]);
    return u - &inputs_[0];
  }
  void replaceOperand(size_t index, MDefinition* operand) final {
    inputs_[index].replaceProducer(operand);
  }
  bool triedToSpecialize() const { return triedToSpecialize_; }
  void specialize(MIRType type) {
    triedToSpecialize_ = true;
    setResultType(type);
  }

#ifdef DEBUG
  // Assert that this is a phi in a loop header with a unique predecessor and
  // a unique backedge.
  void assertLoopPhi() const;
#else
  void assertLoopPhi() const {}
#endif

  // Assuming this phi is in a loop header with a unique loop entry, return
  // the phi operand along the loop entry.
  MDefinition* getLoopPredecessorOperand() const;

  // Assuming this phi is in a loop header with a unique loop entry, return
  // the phi operand along the loop backedge.
  MDefinition* getLoopBackedgeOperand() const;

  // Whether this phi's type already includes information for def.
  bool typeIncludes(MDefinition* def);

  // Mark all phis in |iterators|, and the phis they flow into, as having
  // implicit uses.
  [[nodiscard]] static bool markIteratorPhis(const PhiVector& iterators);

  // Initializes the operands vector to the given capacity,
  // permitting use of addInput() instead of addInputSlow().
  [[nodiscard]] bool reserveLength(size_t length) {
    return inputs_.reserve(length);
  }

  // Use only if capacity has been reserved by reserveLength
  void addInput(MDefinition* ins) {
    MOZ_ASSERT_IF(type() != MIRType::Value, ins->type() == type());
    inputs_.infallibleEmplaceBack(ins, this);
  }

  // Append a new input to the input vector.  May fail.
  [[nodiscard]] bool addInputFallible(MDefinition* ins) {
    MOZ_ASSERT_IF(type() != MIRType::Value, ins->type() == type());
    return inputs_.emplaceBack(ins, this);
  }

  // Appends a new input to the input vector. May perform reallocation.
  // Prefer reserveLength() and addInput() instead, where possible.
  [[nodiscard]] bool addInputSlow(MDefinition* ins) {
    MOZ_ASSERT_IF(type() != MIRType::Value, ins->type() == type());
    return inputs_.emplaceBack(ins, this);
  }

  // Appends a new input to the input vector. Infallible because
  // we know the inputs fits in the vector's inline storage.
  void addInlineInput(MDefinition* ins) {
    MOZ_ASSERT(inputs_.length() < InputVector::InlineLength);
    MOZ_ALWAYS_TRUE(addInputSlow(ins));
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  MDefinition* foldsTernary(TempAllocator& alloc);

  bool congruentTo(const MDefinition* ins) const override;

  // Mark this phi-node as having replaced all uses of |other|, as during GVN.
  // For use when GVN eliminates phis which are not equivalent to one another.
  void updateForReplacement(MPhi* other);

  bool isIterator() const { return isIterator_; }
  void setIterator() { isIterator_ = true; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;

  MDefinition* operandIfRedundant();

  bool canProduceFloat32() const override { return canProduceFloat32_; }

  void setCanProduceFloat32(bool can) { canProduceFloat32_ = can; }

  bool canConsumeFloat32(MUse* use) const override {
    return canConsumeFloat32_;
  }

  void setCanConsumeFloat32(bool can) { canConsumeFloat32_ = can; }

  TruncateKind operandTruncateKind(size_t index) const override;
  bool canTruncate() const override;
  void truncate(TruncateKind kind) override;

  PhiUsage getUsageAnalysis() const { return usageAnalysis_; }
  void setUsageAnalysis(PhiUsage pu) {
    MOZ_ASSERT(usageAnalysis_ == PhiUsage::Unknown);
    usageAnalysis_ = pu;
    MOZ_ASSERT(usageAnalysis_ != PhiUsage::Unknown);
  }

  wasm::MaybeRefType computeWasmRefType() const override {
    if (numOperands() == 0) {
      return wasm::MaybeRefType();
    }
    wasm::MaybeRefType firstRefType = getOperand(0)->wasmRefType();
    if (firstRefType.isNothing()) {
      return wasm::MaybeRefType();
    }

    wasm::RefType topTypeOfOperands = firstRefType.value();
    for (size_t i = 1; i < numOperands(); i++) {
      MDefinition* op = getOperand(i);
      wasm::MaybeRefType opType = op->wasmRefType();
      if (opType.isNothing()) {
        return wasm::MaybeRefType();
      }
      topTypeOfOperands =
          wasm::RefType::leastUpperBound(topTypeOfOperands, opType.value());
    }
    return wasm::MaybeRefType(topTypeOfOperands);
  }
};

// The goal of a Beta node is to split a def at a conditionally taken
// branch, so that uses dominated by it have a different name.
class MBeta : public MUnaryInstruction, public NoTypePolicy::Data {
 private:
  // This is the range induced by a comparison and branch in a preceding
  // block. Note that this does not reflect any range constraints from
  // the input value itself, so this value may differ from the range()
  // range after it is computed.
  const Range* comparison_;

  MBeta(MDefinition* val, const Range* comp)
      : MUnaryInstruction(classOpcode, val), comparison_(comp) {
    setResultType(val->type());
  }

 public:
  INSTRUCTION_HEADER(Beta)
  TRIVIAL_NEW_WRAPPERS

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;
};

// If input evaluates to false (i.e. it's NaN, 0 or -0), 0 is returned, else the
// input is returned
class MNaNToZero : public MUnaryInstruction, public DoublePolicy<0>::Data {
  bool operandIsNeverNaN_;
  bool operandIsNeverNegativeZero_;

  explicit MNaNToZero(MDefinition* input)
      : MUnaryInstruction(classOpcode, input),
        operandIsNeverNaN_(false),
        operandIsNeverNegativeZero_(false) {
    setResultType(MIRType::Double);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(NaNToZero)
  TRIVIAL_NEW_WRAPPERS

  bool operandIsNeverNaN() const { return operandIsNeverNaN_; }

  bool operandIsNeverNegativeZero() const {
    return operandIsNeverNegativeZero_;
  }

  void collectRangeInfoPreTrunc() override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  void computeRange(TempAllocator& alloc) override;

  bool writeRecoverData(CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MNaNToZero)
};

// MIR representation of a Value on the OSR BaselineFrame.
// The Value is indexed off of OsrFrameReg.
class MOsrValue : public MUnaryInstruction, public NoTypePolicy::Data {
 private:
  ptrdiff_t frameOffset_;

  MOsrValue(MOsrEntry* entry, ptrdiff_t frameOffset)
      : MUnaryInstruction(classOpcode, entry), frameOffset_(frameOffset) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(OsrValue)
  TRIVIAL_NEW_WRAPPERS

  ptrdiff_t frameOffset() const { return frameOffset_; }

  MOsrEntry* entry() { return getOperand(0)->toOsrEntry(); }

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// MIR representation of a JSObject scope chain pointer on the OSR
// BaselineFrame. The pointer is indexed off of OsrFrameReg.
class MOsrEnvironmentChain : public MUnaryInstruction,
                             public NoTypePolicy::Data {
 private:
  explicit MOsrEnvironmentChain(MOsrEntry* entry)
      : MUnaryInstruction(classOpcode, entry) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(OsrEnvironmentChain)
  TRIVIAL_NEW_WRAPPERS

  MOsrEntry* entry() { return getOperand(0)->toOsrEntry(); }
};

// MIR representation of a JSObject ArgumentsObject pointer on the OSR
// BaselineFrame. The pointer is indexed off of OsrFrameReg.
class MOsrArgumentsObject : public MUnaryInstruction,
                            public NoTypePolicy::Data {
 private:
  explicit MOsrArgumentsObject(MOsrEntry* entry)
      : MUnaryInstruction(classOpcode, entry) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(OsrArgumentsObject)
  TRIVIAL_NEW_WRAPPERS

  MOsrEntry* entry() { return getOperand(0)->toOsrEntry(); }
};

// MIR representation of the return value on the OSR BaselineFrame.
// The Value is indexed off of OsrFrameReg.
class MOsrReturnValue : public MUnaryInstruction, public NoTypePolicy::Data {
 private:
  explicit MOsrReturnValue(MOsrEntry* entry)
      : MUnaryInstruction(classOpcode, entry) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(OsrReturnValue)
  TRIVIAL_NEW_WRAPPERS

  MOsrEntry* entry() { return getOperand(0)->toOsrEntry(); }
};

class MBinaryCache : public MBinaryInstruction,
                     public MixPolicy<BoxPolicy<0>, BoxPolicy<1>>::Data {
  explicit MBinaryCache(MDefinition* left, MDefinition* right, MIRType resType)
      : MBinaryInstruction(classOpcode, left, right) {
    setResultType(resType);
  }

 public:
  INSTRUCTION_HEADER(BinaryCache)
  TRIVIAL_NEW_WRAPPERS
};

// Checks if a value is JS_UNINITIALIZED_LEXICAL, bailout out if so, leaving
// it to baseline to throw at the correct pc.
class MLexicalCheck : public MUnaryInstruction, public BoxPolicy<0>::Data {
  explicit MLexicalCheck(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    setResultType(MIRType::Value);
    setMovable();
    setGuard();

    // If this instruction bails out, we will set a flag to prevent
    // lexical checks in this script from being moved.
    setBailoutKind(BailoutKind::UninitializedLexical);
  }

 public:
  INSTRUCTION_HEADER(LexicalCheck)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
};

// Unconditionally throw a known error number.
class MThrowMsg : public MNullaryInstruction {
  const ThrowMsgKind throwMsgKind_;

  explicit MThrowMsg(ThrowMsgKind throwMsgKind)
      : MNullaryInstruction(classOpcode), throwMsgKind_(throwMsgKind) {
    setGuard();
    setResultType(MIRType::None);
  }

 public:
  INSTRUCTION_HEADER(ThrowMsg)
  TRIVIAL_NEW_WRAPPERS

  ThrowMsgKind throwMsgKind() const { return throwMsgKind_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::ExceptionState);
  }
};

class MGetFirstDollarIndex : public MUnaryInstruction,
                             public StringPolicy<0>::Data {
  explicit MGetFirstDollarIndex(MDefinition* str)
      : MUnaryInstruction(classOpcode, str) {
    setResultType(MIRType::Int32);

    // Codegen assumes string length > 0. Don't allow LICM to move this
    // before the .length > 1 check in RegExpReplace in RegExp.js.
    MOZ_ASSERT(!isMovable());
  }

 public:
  INSTRUCTION_HEADER(GetFirstDollarIndex)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, str))

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  MDefinition* foldsTo(TempAllocator& alloc) override;
};

class MStringReplace : public MTernaryInstruction,
                       public MixPolicy<StringPolicy<0>, StringPolicy<1>,
                                        StringPolicy<2>>::Data {
 private:
  bool isFlatReplacement_;

  MStringReplace(MDefinition* string, MDefinition* pattern,
                 MDefinition* replacement)
      : MTernaryInstruction(classOpcode, string, pattern, replacement),
        isFlatReplacement_(false) {
    setMovable();
    setResultType(MIRType::String);
  }

 public:
  INSTRUCTION_HEADER(StringReplace)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, string), (1, pattern), (2, replacement))

  void setFlatReplacement() {
    MOZ_ASSERT(!isFlatReplacement_);
    isFlatReplacement_ = true;
  }

  bool isFlatReplacement() const { return isFlatReplacement_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isStringReplace()) {
      return false;
    }
    if (isFlatReplacement_ != ins->toStringReplace()->isFlatReplacement()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    if (isFlatReplacement_) {
      MOZ_ASSERT(!pattern()->isRegExp());
      return true;
    }
    return false;
  }

  bool possiblyCalls() const override { return true; }
};

class MLambda : public MBinaryInstruction, public SingleObjectPolicy::Data {
  MLambda(MDefinition* envChain, MConstant* cst, gc::Heap initialHeap)
      : MBinaryInstruction(classOpcode, envChain, cst),
        initialHeap_(initialHeap) {
    setResultType(MIRType::Object);
  }

  // Heap where the lambda should be allocated.
  gc::Heap initialHeap_;

 public:
  INSTRUCTION_HEADER(Lambda)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, environmentChain))

  MConstant* functionOperand() const { return getOperand(1)->toConstant(); }
  JSFunction* templateFunction() const {
    return &functionOperand()->toObject().as<JSFunction>();
  }

  gc::Heap initialHeap() const { return initialHeap_; }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MFunctionWithProto : public MTernaryInstruction,
                           public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>,
                                            ObjectPolicy<2>>::Data {
  CompilerFunction fun_;

  MFunctionWithProto(MDefinition* envChain, MDefinition* prototype,
                     MConstant* cst)
      : MTernaryInstruction(classOpcode, envChain, prototype, cst),
        fun_(&cst->toObject().as<JSFunction>()) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(FunctionWithProto)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, environmentChain), (1, prototype))

  MConstant* functionOperand() const { return getOperand(2)->toConstant(); }
  JSFunction* function() const { return fun_; }
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  bool possiblyCalls() const override { return true; }
};

class MGetNextEntryForIterator
    : public MBinaryInstruction,
      public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>>::Data {
 public:
  enum Mode { Map, Set };

 private:
  Mode mode_;

  explicit MGetNextEntryForIterator(MDefinition* iter, MDefinition* result,
                                    Mode mode)
      : MBinaryInstruction(classOpcode, iter, result), mode_(mode) {
    setResultType(MIRType::Boolean);
  }

 public:
  INSTRUCTION_HEADER(GetNextEntryForIterator)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, iter), (1, result))

  Mode mode() const { return mode_; }
};

// Convert a Double into an IntPtr value for accessing a TypedArray or DataView
// element. If the input is non-finite, not an integer, negative, or outside the
// IntPtr range, either bails out or produces a value which is known to trigger
// an out-of-bounds access (this depends on the supportOOB flag).
class MGuardNumberToIntPtrIndex : public MUnaryInstruction,
                                  public DoublePolicy<0>::Data {
  // If true, produce an out-of-bounds index for non-IntPtr doubles instead of
  // bailing out.
  const bool supportOOB_;

  MGuardNumberToIntPtrIndex(MDefinition* def, bool supportOOB)
      : MUnaryInstruction(classOpcode, def), supportOOB_(supportOOB) {
    MOZ_ASSERT(IsNumberType(def->type()));
    setResultType(MIRType::IntPtr);
    setMovable();
    if (!supportOOB) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(GuardNumberToIntPtrIndex)
  TRIVIAL_NEW_WRAPPERS

  bool supportOOB() const { return supportOOB_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardNumberToIntPtrIndex()) {
      return false;
    }
    if (ins->toGuardNumberToIntPtrIndex()->supportOOB() != supportOOB()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  ALLOW_CLONE(MGuardNumberToIntPtrIndex)
};

// Perform !-operation
class MNot : public MUnaryInstruction, public TestPolicy::Data {
  bool operandIsNeverNaN_;
  TypeDataList observedTypes_;

  explicit MNot(MDefinition* input)
      : MUnaryInstruction(classOpcode, input), operandIsNeverNaN_(false) {
    setResultType(MIRType::Boolean);
    setMovable();
  }

 public:
  static MNot* NewInt32(TempAllocator& alloc, MDefinition* input) {
    MOZ_ASSERT(input->type() == MIRType::Int32 ||
               input->type() == MIRType::Int64);
    auto* ins = new (alloc) MNot(input);
    ins->setResultType(MIRType::Int32);
    return ins;
  }

  INSTRUCTION_HEADER(Not)
  TRIVIAL_NEW_WRAPPERS

  void setObservedTypes(const TypeDataList& observed) {
    observedTypes_ = observed;
  }
  const TypeDataList& observedTypes() const { return observedTypes_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool operandIsNeverNaN() const { return operandIsNeverNaN_; }

  virtual AliasSet getAliasSet() const override { return AliasSet::None(); }
  void collectRangeInfoPreTrunc() override;

  void trySpecializeFloat32(TempAllocator& alloc) override;
  bool isFloat32Commutative() const override { return true; }
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override {
    return type() == MIRType::Boolean;
  }

  ALLOW_CLONE(MNot)
};

// Bailout if index + minimum < 0 or index + maximum >= length. The length used
// in a bounds check must not be negative, or the wrong result may be computed
// (unsigned comparisons may be used).
class MBoundsCheck
    : public MBinaryInstruction,
      public MixPolicy<Int32OrIntPtrPolicy<0>, Int32OrIntPtrPolicy<1>>::Data {
  // Range over which to perform the bounds check, may be modified by GVN.
  int32_t minimum_;
  int32_t maximum_;
  bool fallible_;

  MBoundsCheck(MDefinition* index, MDefinition* length)
      : MBinaryInstruction(classOpcode, index, length),
        minimum_(0),
        maximum_(0),
        fallible_(true) {
    setGuard();
    setMovable();
    MOZ_ASSERT(index->type() == MIRType::Int32 ||
               index->type() == MIRType::IntPtr);
    MOZ_ASSERT(index->type() == length->type());

    // Returns the checked index.
    setResultType(index->type());
  }

 public:
  INSTRUCTION_HEADER(BoundsCheck)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, index), (1, length))

  int32_t minimum() const { return minimum_; }
  void setMinimum(int32_t n) {
    MOZ_ASSERT(fallible_);
    minimum_ = n;
  }
  int32_t maximum() const { return maximum_; }
  void setMaximum(int32_t n) {
    MOZ_ASSERT(fallible_);
    maximum_ = n;
  }
  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isBoundsCheck()) {
      return false;
    }
    const MBoundsCheck* other = ins->toBoundsCheck();
    if (minimum() != other->minimum() || maximum() != other->maximum()) {
      return false;
    }
    if (fallible() != other->fallible()) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }
  virtual AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;
  bool fallible() const { return fallible_; }
  void collectRangeInfoPreTrunc() override;

  ALLOW_CLONE(MBoundsCheck)
};

// Bailout if index < minimum.
class MBoundsCheckLower : public MUnaryInstruction,
                          public UnboxedInt32Policy<0>::Data {
  int32_t minimum_;
  bool fallible_;

  explicit MBoundsCheckLower(MDefinition* index)
      : MUnaryInstruction(classOpcode, index), minimum_(0), fallible_(true) {
    setGuard();
    setMovable();
    MOZ_ASSERT(index->type() == MIRType::Int32);
  }

 public:
  INSTRUCTION_HEADER(BoundsCheckLower)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, index))

  int32_t minimum() const { return minimum_; }
  void setMinimum(int32_t n) { minimum_ = n; }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool fallible() const { return fallible_; }
  void collectRangeInfoPreTrunc() override;
};

class MSpectreMaskIndex
    : public MBinaryInstruction,
      public MixPolicy<Int32OrIntPtrPolicy<0>, Int32OrIntPtrPolicy<1>>::Data {
  MSpectreMaskIndex(MDefinition* index, MDefinition* length)
      : MBinaryInstruction(classOpcode, index, length) {
    // Note: this instruction does not need setGuard(): if there are no uses
    // it's fine for DCE to eliminate this instruction.
    setMovable();
    MOZ_ASSERT(index->type() == MIRType::Int32 ||
               index->type() == MIRType::IntPtr);
    MOZ_ASSERT(index->type() == length->type());

    // Returns the masked index.
    setResultType(index->type());
  }

 public:
  INSTRUCTION_HEADER(SpectreMaskIndex)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, index), (1, length))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  virtual AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;

  ALLOW_CLONE(MSpectreMaskIndex)
};

// Load a value from a dense array's element vector. Bails out if the element is
// a hole.
class MLoadElement : public MBinaryInstruction, public NoTypePolicy::Data {
  bool needsHoleCheck_;

  MLoadElement(MDefinition* elements, MDefinition* index, bool needsHoleCheck)
      : MBinaryInstruction(classOpcode, elements, index),
        needsHoleCheck_(needsHoleCheck) {
    if (needsHoleCheck) {
      // Uses may be optimized away based on this instruction's result
      // type. This means it's invalid to DCE this instruction, as we
      // have to invalidate when we read a hole.
      setGuard();
    }
    setResultType(MIRType::Value);
    setMovable();
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
  }

 public:
  INSTRUCTION_HEADER(LoadElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index))

  bool needsHoleCheck() const { return needsHoleCheck_; }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasType mightAlias(const MDefinition* store) const override;
  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::Element);
  }

  ALLOW_CLONE(MLoadElement)
};

class MLoadElementAndUnbox : public MBinaryInstruction,
                             public NoTypePolicy::Data {
  MUnbox::Mode mode_;

  MLoadElementAndUnbox(MDefinition* elements, MDefinition* index,
                       MUnbox::Mode mode, MIRType type)
      : MBinaryInstruction(classOpcode, elements, index), mode_(mode) {
    setResultType(type);
    setMovable();
    if (mode_ == MUnbox::Fallible) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(LoadElementAndUnbox)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index))

  MUnbox::Mode mode() const { return mode_; }
  bool fallible() const { return mode_ != MUnbox::Infallible; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadElementAndUnbox() ||
        mode() != ins->toLoadElementAndUnbox()->mode()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::Element);
  }

  ALLOW_CLONE(MLoadElementAndUnbox);
};

// Load a value from the elements vector of a native object. If the index is
// out-of-bounds, or the indexed slot has a hole, undefined is returned instead.
class MLoadElementHole : public MTernaryInstruction, public NoTypePolicy::Data {
  bool needsNegativeIntCheck_ = true;

  MLoadElementHole(MDefinition* elements, MDefinition* index,
                   MDefinition* initLength)
      : MTernaryInstruction(classOpcode, elements, index, initLength) {
    setResultType(MIRType::Value);
    setMovable();

    // Set the guard flag to make sure we bail when we see a negative
    // index. We can clear this flag (and needsNegativeIntCheck_) in
    // collectRangeInfoPreTrunc.
    setGuard();

    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    MOZ_ASSERT(initLength->type() == MIRType::Int32);
  }

 public:
  INSTRUCTION_HEADER(LoadElementHole)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, initLength))

  bool needsNegativeIntCheck() const { return needsNegativeIntCheck_; }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadElementHole()) {
      return false;
    }
    const MLoadElementHole* other = ins->toLoadElementHole();
    if (needsNegativeIntCheck() != other->needsNegativeIntCheck()) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::Element);
  }
  void collectRangeInfoPreTrunc() override;

  ALLOW_CLONE(MLoadElementHole)
};

// Store a value to a dense array slots vector.
class MStoreElement : public MTernaryInstruction,
                      public NoFloatPolicy<2>::Data {
  bool needsHoleCheck_;
  bool needsBarrier_;

  MStoreElement(MDefinition* elements, MDefinition* index, MDefinition* value,
                bool needsHoleCheck, bool needsBarrier)
      : MTernaryInstruction(classOpcode, elements, index, value) {
    needsHoleCheck_ = needsHoleCheck;
    needsBarrier_ = needsBarrier;
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    MOZ_ASSERT(value->type() != MIRType::MagicHole);
  }

 public:
  INSTRUCTION_HEADER(StoreElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, value))

  static MStoreElement* NewUnbarriered(TempAllocator& alloc,
                                       MDefinition* elements,
                                       MDefinition* index, MDefinition* value,
                                       bool needsHoleCheck) {
    return new (alloc)
        MStoreElement(elements, index, value, needsHoleCheck, false);
  }

  static MStoreElement* NewBarriered(TempAllocator& alloc,
                                     MDefinition* elements, MDefinition* index,
                                     MDefinition* value, bool needsHoleCheck) {
    return new (alloc)
        MStoreElement(elements, index, value, needsHoleCheck, true);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::Element);
  }
  bool needsBarrier() const { return needsBarrier_; }
  bool needsHoleCheck() const { return needsHoleCheck_; }
  bool fallible() const { return needsHoleCheck(); }

  ALLOW_CLONE(MStoreElement)
};

// Stores MagicValue(JS_ELEMENTS_HOLE) and marks the elements as non-packed.
class MStoreHoleValueElement : public MBinaryInstruction,
                               public NoTypePolicy::Data {
  MStoreHoleValueElement(MDefinition* elements, MDefinition* index)
      : MBinaryInstruction(classOpcode, elements, index) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
  }

 public:
  INSTRUCTION_HEADER(StoreHoleValueElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index))

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::Element | AliasSet::ObjectFields);
  }

  ALLOW_CLONE(MStoreHoleValueElement)
};

// Like MStoreElement, but also supports index == initialized length. The
// downside is that we cannot hoist the elements vector and bounds check, since
// this instruction may update the (initialized) length and reallocate the
// elements vector.
class MStoreElementHole
    : public MQuaternaryInstruction,
      public MixPolicy<SingleObjectPolicy, NoFloatPolicy<3>>::Data {
  MStoreElementHole(MDefinition* object, MDefinition* elements,
                    MDefinition* index, MDefinition* value)
      : MQuaternaryInstruction(classOpcode, object, elements, index, value) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    MOZ_ASSERT(value->type() != MIRType::MagicHole);
  }

 public:
  INSTRUCTION_HEADER(StoreElementHole)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, elements), (2, index), (3, value))

  AliasSet getAliasSet() const override {
    // StoreElementHole can update the initialized length, the array length
    // or reallocate obj->elements.
    return AliasSet::Store(AliasSet::ObjectFields | AliasSet::Element);
  }

  ALLOW_CLONE(MStoreElementHole)
};

// Array.prototype.pop or Array.prototype.shift on a dense array.
class MArrayPopShift : public MUnaryInstruction,
                       public SingleObjectPolicy::Data {
 public:
  enum Mode { Pop, Shift };

 private:
  Mode mode_;

  MArrayPopShift(MDefinition* object, Mode mode)
      : MUnaryInstruction(classOpcode, object), mode_(mode) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(ArrayPopShift)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  bool mode() const { return mode_; }
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::ObjectFields | AliasSet::Element);
  }

  ALLOW_CLONE(MArrayPopShift)
};

// Load an unboxed scalar value from an array buffer view or other object.
class MLoadUnboxedScalar : public MBinaryInstruction,
                           public NoTypePolicy::Data {
  Scalar::Type storageType_;
  MemoryBarrierRequirement requiresBarrier_;

  MLoadUnboxedScalar(MDefinition* elements, MDefinition* index,
                     Scalar::Type storageType,
                     MemoryBarrierRequirement requiresBarrier =
                         MemoryBarrierRequirement::NotRequired)
      : MBinaryInstruction(classOpcode, elements, index),
        storageType_(storageType),
        requiresBarrier_(requiresBarrier) {
    setResultType(MIRType::Value);
    if (requiresBarrier_ == MemoryBarrierRequirement::Required) {
      setGuard();  // Not removable or movable
    } else {
      setMovable();
    }
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(storageType >= 0 && storageType < Scalar::MaxTypedArrayViewType);
  }

 public:
  INSTRUCTION_HEADER(LoadUnboxedScalar)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index))

  Scalar::Type storageType() const { return storageType_; }
  bool fallible() const {
    // Bailout if the result does not fit in an int32.
    return storageType_ == Scalar::Uint32 && type() == MIRType::Int32;
  }
  auto requiresMemoryBarrier() const { return requiresBarrier_; }
  AliasSet getAliasSet() const override {
    // When a barrier is needed make the instruction effectful by
    // giving it a "store" effect.
    if (requiresBarrier_ == MemoryBarrierRequirement::Required) {
      return AliasSet::Store(AliasSet::UnboxedElement);
    }
    return AliasSet::Load(AliasSet::UnboxedElement);
  }

  bool congruentTo(const MDefinition* ins) const override {
    if (requiresBarrier_ == MemoryBarrierRequirement::Required) {
      return false;
    }
    if (!ins->isLoadUnboxedScalar()) {
      return false;
    }
    const MLoadUnboxedScalar* other = ins->toLoadUnboxedScalar();
    if (storageType_ != other->storageType_) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  void computeRange(TempAllocator& alloc) override;

  bool canProduceFloat32() const override {
    return storageType_ == Scalar::Float32 || storageType_ == Scalar::Float16;
  }

  ALLOW_CLONE(MLoadUnboxedScalar)
};

// Load an unboxed scalar value from a dataview object.
class MLoadDataViewElement : public MTernaryInstruction,
                             public NoTypePolicy::Data {
  Scalar::Type storageType_;

  MLoadDataViewElement(MDefinition* elements, MDefinition* index,
                       MDefinition* littleEndian, Scalar::Type storageType)
      : MTernaryInstruction(classOpcode, elements, index, littleEndian),
        storageType_(storageType) {
    setResultType(MIRType::Value);
    setMovable();
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(littleEndian->type() == MIRType::Boolean);
    MOZ_ASSERT(storageType >= 0 && storageType < Scalar::MaxTypedArrayViewType);
    MOZ_ASSERT(Scalar::byteSize(storageType) > 1);
  }

 public:
  INSTRUCTION_HEADER(LoadDataViewElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, littleEndian))

  Scalar::Type storageType() const { return storageType_; }
  bool fallible() const {
    // Bailout if the result does not fit in an int32.
    return storageType_ == Scalar::Uint32 && type() == MIRType::Int32;
  }
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::UnboxedElement);
  }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadDataViewElement()) {
      return false;
    }
    const MLoadDataViewElement* other = ins->toLoadDataViewElement();
    if (storageType_ != other->storageType_) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  void computeRange(TempAllocator& alloc) override;

  bool canProduceFloat32() const override {
    return storageType_ == Scalar::Float32 || storageType_ == Scalar::Float16;
  }

  ALLOW_CLONE(MLoadDataViewElement)
};

// Load a value from a typed array. Out-of-bounds accesses are handled in-line.
class MLoadTypedArrayElementHole : public MTernaryInstruction,
                                   public NoTypePolicy::Data {
  Scalar::Type arrayType_;
  bool forceDouble_;

  MLoadTypedArrayElementHole(MDefinition* elements, MDefinition* index,
                             MDefinition* length, Scalar::Type arrayType,
                             bool forceDouble)
      : MTernaryInstruction(classOpcode, elements, index, length),
        arrayType_(arrayType),
        forceDouble_(forceDouble) {
    setResultType(MIRType::Value);
    setMovable();
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(length->type() == MIRType::IntPtr);
    MOZ_ASSERT(arrayType >= 0 && arrayType < Scalar::MaxTypedArrayViewType);
  }

 public:
  INSTRUCTION_HEADER(LoadTypedArrayElementHole)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, length))

  Scalar::Type arrayType() const { return arrayType_; }
  bool forceDouble() const { return forceDouble_; }
  bool fallible() const {
    return arrayType_ == Scalar::Uint32 && !forceDouble_;
  }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadTypedArrayElementHole()) {
      return false;
    }
    const MLoadTypedArrayElementHole* other =
        ins->toLoadTypedArrayElementHole();
    if (arrayType() != other->arrayType()) {
      return false;
    }
    if (forceDouble() != other->forceDouble()) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::UnboxedElement);
  }
  bool canProduceFloat32() const override {
    return arrayType_ == Scalar::Float32 || arrayType_ == Scalar::Float16;
  }

  ALLOW_CLONE(MLoadTypedArrayElementHole)
};

// Base class for MIR ops that write unboxed scalar values.
class StoreUnboxedScalarBase {
  Scalar::Type writeType_;

 protected:
  explicit StoreUnboxedScalarBase(Scalar::Type writeType)
      : writeType_(writeType) {
    MOZ_ASSERT(isIntegerWrite() || isFloatWrite() || isBigIntWrite());
  }

 public:
  Scalar::Type writeType() const { return writeType_; }
  bool isByteWrite() const {
    return writeType_ == Scalar::Int8 || writeType_ == Scalar::Uint8 ||
           writeType_ == Scalar::Uint8Clamped;
  }
  bool isIntegerWrite() const {
    return isByteWrite() || writeType_ == Scalar::Int16 ||
           writeType_ == Scalar::Uint16 || writeType_ == Scalar::Int32 ||
           writeType_ == Scalar::Uint32;
  }
  bool isFloatWrite() const {
    return writeType_ == Scalar::Float16 || writeType_ == Scalar::Float32 ||
           writeType_ == Scalar::Float64;
  }
  bool isBigIntWrite() const { return Scalar::isBigIntType(writeType_); }
};

// Store an unboxed scalar value to an array buffer view or other object.
class MStoreUnboxedScalar : public MTernaryInstruction,
                            public StoreUnboxedScalarBase,
                            public StoreUnboxedScalarPolicy::Data {
  MemoryBarrierRequirement requiresBarrier_;

  MStoreUnboxedScalar(MDefinition* elements, MDefinition* index,
                      MDefinition* value, Scalar::Type storageType,
                      MemoryBarrierRequirement requiresBarrier =
                          MemoryBarrierRequirement::NotRequired)
      : MTernaryInstruction(classOpcode, elements, index, value),
        StoreUnboxedScalarBase(storageType),
        requiresBarrier_(requiresBarrier) {
    if (requiresBarrier_ == MemoryBarrierRequirement::Required) {
      setGuard();  // Not removable or movable
    }
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(storageType >= 0 && storageType < Scalar::MaxTypedArrayViewType);
  }

 public:
  INSTRUCTION_HEADER(StoreUnboxedScalar)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, value))

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::UnboxedElement);
  }
  auto requiresMemoryBarrier() const { return requiresBarrier_; }
  TruncateKind operandTruncateKind(size_t index) const override;

  bool canConsumeFloat32(MUse* use) const override {
    return use == getUseFor(2) && writeType() == Scalar::Float32;
  }

#ifdef DEBUG
  // Float16 inputs are typed as float32, but this instruction can NOT consume
  // float32 when its write-type is float16.
  bool isConsistentFloat32Use(MUse* use) const override {
    return use == getUseFor(2) &&
           (writeType() == Scalar::Float32 || writeType() == Scalar::Float16);
  }
#endif

  ALLOW_CLONE(MStoreUnboxedScalar)
};

// Store an unboxed scalar value to a dataview object.
class MStoreDataViewElement : public MQuaternaryInstruction,
                              public StoreUnboxedScalarBase,
                              public StoreDataViewElementPolicy::Data {
  MStoreDataViewElement(MDefinition* elements, MDefinition* index,
                        MDefinition* value, MDefinition* littleEndian,
                        Scalar::Type storageType)
      : MQuaternaryInstruction(classOpcode, elements, index, value,
                               littleEndian),
        StoreUnboxedScalarBase(storageType) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(storageType >= 0 && storageType < Scalar::MaxTypedArrayViewType);
    MOZ_ASSERT(Scalar::byteSize(storageType) > 1);
  }

 public:
  INSTRUCTION_HEADER(StoreDataViewElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, value), (3, littleEndian))

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::UnboxedElement);
  }
  TruncateKind operandTruncateKind(size_t index) const override;

  bool canConsumeFloat32(MUse* use) const override {
    return use == getUseFor(2) && writeType() == Scalar::Float32;
  }

#ifdef DEBUG
  // Float16 inputs are typed as float32, but this instruction can NOT consume
  // float32 when its write-type is float16.
  bool isConsistentFloat32Use(MUse* use) const override {
    return use == getUseFor(2) &&
           (writeType() == Scalar::Float32 || writeType() == Scalar::Float16);
  }
#endif

  ALLOW_CLONE(MStoreDataViewElement)
};

class MStoreTypedArrayElementHole : public MQuaternaryInstruction,
                                    public StoreUnboxedScalarBase,
                                    public StoreTypedArrayHolePolicy::Data {
  MStoreTypedArrayElementHole(MDefinition* elements, MDefinition* length,
                              MDefinition* index, MDefinition* value,
                              Scalar::Type arrayType)
      : MQuaternaryInstruction(classOpcode, elements, length, index, value),
        StoreUnboxedScalarBase(arrayType) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(length->type() == MIRType::IntPtr);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(arrayType >= 0 && arrayType < Scalar::MaxTypedArrayViewType);
  }

 public:
  INSTRUCTION_HEADER(StoreTypedArrayElementHole)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, length), (2, index), (3, value))

  Scalar::Type arrayType() const { return writeType(); }
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::UnboxedElement);
  }
  TruncateKind operandTruncateKind(size_t index) const override;

  bool canConsumeFloat32(MUse* use) const override {
    return use == getUseFor(3) && arrayType() == Scalar::Float32;
  }

#ifdef DEBUG
  // Float16 inputs are typed as float32, but this instruction can NOT consume
  // float32 when its array-type is float16.
  bool isConsistentFloat32Use(MUse* use) const override {
    return use == getUseFor(3) &&
           (arrayType() == Scalar::Float32 || arrayType() == Scalar::Float16);
  }
#endif

  ALLOW_CLONE(MStoreTypedArrayElementHole)
};

class MTypedArrayFill : public MQuaternaryInstruction,
                        public StoreUnboxedScalarBase,
                        public TypedArrayFillPolicy::Data {
  MTypedArrayFill(MDefinition* object, MDefinition* value, MDefinition* start,
                  MDefinition* end, Scalar::Type arrayType)
      : MQuaternaryInstruction(classOpcode, object, value, start, end),
        StoreUnboxedScalarBase(arrayType) {
    MOZ_ASSERT(object->type() == MIRType::Object);
    MOZ_ASSERT(start->type() == MIRType::IntPtr);
    MOZ_ASSERT(end->type() == MIRType::IntPtr);
    MOZ_ASSERT(arrayType >= 0 && arrayType < Scalar::MaxTypedArrayViewType);
  }

 public:
  INSTRUCTION_HEADER(TypedArrayFill)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, value), (2, start), (3, end))

  Scalar::Type elementType() const { return writeType(); }
  AliasSet getAliasSet() const override {
    // Loads typed array length and elements.
    constexpr auto load =
        AliasSet::Load(AliasSet::ArrayBufferViewLengthOrOffset |
                       AliasSet::ObjectFields | AliasSet::UnboxedElement);

    // Stores into typed array elements.
    constexpr auto store = AliasSet::Store(AliasSet::UnboxedElement);

    return load | store;
  }

  TruncateKind operandTruncateKind(size_t index) const override;

  bool canConsumeFloat32(MUse* use) const override {
    return use == getUseFor(1) && elementType() == Scalar::Float32;
  }

#ifdef DEBUG
  // Float16 inputs are typed as float32, but this instruction can NOT consume
  // float32 when its array-type is float16.
  bool isConsistentFloat32Use(MUse* use) const override {
    return use == getUseFor(1) && (elementType() == Scalar::Float32 ||
                                   elementType() == Scalar::Float16);
  }
#endif

  bool possiblyCalls() const override { return true; }

  ALLOW_CLONE(MTypedArrayFill)
};

// Compute a 3-component "effective address":
//   base + index * scale + displacement
class MEffectiveAddress3 : public MBinaryInstruction,
                           public NoTypePolicy::Data {
  MEffectiveAddress3(MDefinition* base, MDefinition* index, Scale scale,
                     int32_t displacement)
      : MBinaryInstruction(classOpcode, base, index),
        scale_(scale),
        displacement_(displacement) {
    MOZ_ASSERT(base->type() == MIRType::Int32);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    setMovable();
    setResultType(MIRType::Int32);
  }

  Scale scale_;
  int32_t displacement_;

 public:
  INSTRUCTION_HEADER(EffectiveAddress3)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* base() const { return lhs(); }
  MDefinition* index() const { return rhs(); }
  Scale scale() const { return scale_; }
  int32_t displacement() const { return displacement_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(disp=%d, scale=%s)", int(displacement_),
                   StringFromScale(scale_));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MEffectiveAddress3)
};

// Compute a 2-component "effective address":
//   index * scale + displacement
class MEffectiveAddress2 : public MUnaryInstruction, public NoTypePolicy::Data {
  MEffectiveAddress2(MDefinition* index, Scale scale, int32_t displacement)
      : MUnaryInstruction(classOpcode, index),
        scale_(scale),
        displacement_(displacement) {
    MOZ_ASSERT(index->type() == MIRType::Int32);
    setMovable();
    setResultType(MIRType::Int32);
  }

  Scale scale_;
  int32_t displacement_;

 public:
  INSTRUCTION_HEADER(EffectiveAddress2)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* index() const { return input(); }
  Scale scale() const { return scale_; }
  int32_t displacement() const { return displacement_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef JS_JITSPEW
  void getExtras(ExtrasCollector* extras) const override {
    char buf[64];
    SprintfLiteral(buf, "(disp=%d, scale=%s)", int(displacement_),
                   StringFromScale(scale_));
    extras->add(buf);
  }
#endif

  ALLOW_CLONE(MEffectiveAddress2)
};

// Clamp input to range [0, 255] for Uint8ClampedArray.
class MClampToUint8 : public MUnaryInstruction, public ClampPolicy::Data {
  explicit MClampToUint8(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(ClampToUint8)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  void computeRange(TempAllocator& alloc) override;

  ALLOW_CLONE(MClampToUint8)
};

class MLoadFixedSlot : public MUnaryInstruction,
                       public SingleObjectPolicy::Data {
  size_t slot_;
  bool usedAsPropertyKey_ = false;

 protected:
  MLoadFixedSlot(MDefinition* obj, size_t slot)
      : MUnaryInstruction(classOpcode, obj), slot_(slot) {
    setResultType(MIRType::Value);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(LoadFixedSlot)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  size_t slot() const { return slot_; }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadFixedSlot()) {
      return false;
    }
    if (slot() != ins->toLoadFixedSlot()->slot()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::FixedSlot);
  }

  AliasType mightAlias(const MDefinition* store) const override;

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  void setUsedAsPropertyKey() { usedAsPropertyKey_ = true; }
  bool usedAsPropertyKey() const { return usedAsPropertyKey_; }

  ALLOW_CLONE(MLoadFixedSlot)
};

class MLoadFixedSlotAndUnbox : public MUnaryInstruction,
                               public SingleObjectPolicy::Data {
  size_t slot_;
  MUnbox::Mode mode_;
  bool usedAsPropertyKey_;

  MLoadFixedSlotAndUnbox(MDefinition* obj, size_t slot, MUnbox::Mode mode,
                         MIRType type, bool usedAsPropertyKey = false)
      : MUnaryInstruction(classOpcode, obj),
        slot_(slot),
        mode_(mode),
        usedAsPropertyKey_(usedAsPropertyKey) {
    setResultType(type);
    setMovable();
    if (mode_ == MUnbox::Fallible) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(LoadFixedSlotAndUnbox)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  size_t slot() const { return slot_; }
  MUnbox::Mode mode() const { return mode_; }
  bool fallible() const { return mode_ != MUnbox::Infallible; }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadFixedSlotAndUnbox() ||
        slot() != ins->toLoadFixedSlotAndUnbox()->slot() ||
        mode() != ins->toLoadFixedSlotAndUnbox()->mode()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::FixedSlot);
  }

  AliasType mightAlias(const MDefinition* store) const override;

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  bool usedAsPropertyKey() const { return usedAsPropertyKey_; }

  ALLOW_CLONE(MLoadFixedSlotAndUnbox);
};

class MLoadDynamicSlotAndUnbox : public MUnaryInstruction,
                                 public NoTypePolicy::Data {
  size_t slot_;
  MUnbox::Mode mode_;
  bool usedAsPropertyKey_ = false;

  MLoadDynamicSlotAndUnbox(MDefinition* slots, size_t slot, MUnbox::Mode mode,
                           MIRType type, bool usedAsPropertyKey = false)
      : MUnaryInstruction(classOpcode, slots),
        slot_(slot),
        mode_(mode),
        usedAsPropertyKey_(usedAsPropertyKey) {
    setResultType(type);
    setMovable();
    if (mode_ == MUnbox::Fallible) {
      setGuard();
    }
  }

 public:
  INSTRUCTION_HEADER(LoadDynamicSlotAndUnbox)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, slots))

  size_t slot() const { return slot_; }
  MUnbox::Mode mode() const { return mode_; }
  bool fallible() const { return mode_ != MUnbox::Infallible; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadDynamicSlotAndUnbox() ||
        slot() != ins->toLoadDynamicSlotAndUnbox()->slot() ||
        mode() != ins->toLoadDynamicSlotAndUnbox()->mode()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::DynamicSlot);
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  bool usedAsPropertyKey() const { return usedAsPropertyKey_; }

  ALLOW_CLONE(MLoadDynamicSlotAndUnbox);
};

class MStoreFixedSlot
    : public MBinaryInstruction,
      public MixPolicy<SingleObjectPolicy, NoFloatPolicy<1>>::Data {
  bool needsBarrier_;
  size_t slot_;

  MStoreFixedSlot(MDefinition* obj, MDefinition* rval, size_t slot,
                  bool barrier)
      : MBinaryInstruction(classOpcode, obj, rval),
        needsBarrier_(barrier),
        slot_(slot) {}

 public:
  INSTRUCTION_HEADER(StoreFixedSlot)
  NAMED_OPERANDS((0, object), (1, value))

  static MStoreFixedSlot* NewUnbarriered(TempAllocator& alloc, MDefinition* obj,
                                         size_t slot, MDefinition* rval) {
    return new (alloc) MStoreFixedSlot(obj, rval, slot, false);
  }
  static MStoreFixedSlot* NewBarriered(TempAllocator& alloc, MDefinition* obj,
                                       size_t slot, MDefinition* rval) {
    return new (alloc) MStoreFixedSlot(obj, rval, slot, true);
  }

  size_t slot() const { return slot_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::FixedSlot);
  }
  bool needsBarrier() const { return needsBarrier_; }
  void setNeedsBarrier(bool needsBarrier = true) {
    needsBarrier_ = needsBarrier;
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  ALLOW_CLONE(MStoreFixedSlot)
};

class MGetPropertyCache : public MBinaryInstruction,
                          public MixPolicy<BoxExceptPolicy<0, MIRType::Object>,
                                           CacheIdPolicy<1>>::Data {
  MGetPropertyCache(MDefinition* obj, MDefinition* id)
      : MBinaryInstruction(classOpcode, obj, id) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(GetPropertyCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value), (1, idval))
};

class MGetPropSuperCache
    : public MTernaryInstruction,
      public MixPolicy<ObjectPolicy<0>, BoxExceptPolicy<1, MIRType::Object>,
                       CacheIdPolicy<2>>::Data {
  MGetPropSuperCache(MDefinition* obj, MDefinition* receiver, MDefinition* id)
      : MTernaryInstruction(classOpcode, obj, receiver, id) {
    setResultType(MIRType::Value);
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(GetPropSuperCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, receiver), (2, idval))
};

// Guard the object's proto is |expected|.
class MGuardProto : public MBinaryInstruction, public SingleObjectPolicy::Data {
  MGuardProto(MDefinition* obj, MDefinition* expected)
      : MBinaryInstruction(classOpcode, obj, expected) {
    MOZ_ASSERT(expected->isConstant() || expected->isNurseryObject());
    setGuard();
    setMovable();
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(GuardProto)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, expected))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::ObjectFields);
  }
  AliasType mightAlias(const MDefinition* def) const override {
    // These instructions never modify the [[Prototype]].
    if (def->isAddAndStoreSlot() || def->isAllocateAndStoreSlot()) {
      return AliasType::NoAlias;
    }
    return AliasType::MayAlias;
  }
};

// Guard the object has no proto.
class MGuardNullProto : public MUnaryInstruction,
                        public SingleObjectPolicy::Data {
  explicit MGuardNullProto(MDefinition* obj)
      : MUnaryInstruction(classOpcode, obj) {
    setGuard();
    setMovable();
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(GuardNullProto)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::ObjectFields);
  }
  AliasType mightAlias(const MDefinition* def) const override {
    // These instructions never modify the [[Prototype]].
    if (def->isAddAndStoreSlot() || def->isAllocateAndStoreSlot()) {
      return AliasType::NoAlias;
    }
    return AliasType::MayAlias;
  }
};

// Guard on a specific Value.
class MGuardValue : public MUnaryInstruction, public BoxInputsPolicy::Data {
  ValueOrNurseryValueIndex expected_;

  MGuardValue(MDefinition* val, ValueOrNurseryValueIndex expected)
      : MUnaryInstruction(classOpcode, val), expected_(expected) {
    setGuard();
    setMovable();
    setResultType(MIRType::Value);
  }
  MGuardValue(MDefinition* val, Value expected)
      : MGuardValue(val, ValueOrNurseryValueIndex::fromValue(expected)) {}

 public:
  INSTRUCTION_HEADER(GuardValue)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value))

  ValueOrNurseryValueIndex expected() const { return expected_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardValue()) {
      return false;
    }
    if (expected() != ins->toGuardValue()->expected()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Guard on function flags
class MGuardFunctionFlags : public MUnaryInstruction,
                            public SingleObjectPolicy::Data {
  // At least one of the expected flags must be set, but not necessarily all
  // expected flags.
  uint16_t expectedFlags_;

  // None of the unexpected flags must be set.
  uint16_t unexpectedFlags_;

  explicit MGuardFunctionFlags(MDefinition* fun, uint16_t expectedFlags,
                               uint16_t unexpectedFlags)
      : MUnaryInstruction(classOpcode, fun),
        expectedFlags_(expectedFlags),
        unexpectedFlags_(unexpectedFlags) {
    MOZ_ASSERT((expectedFlags & unexpectedFlags) == 0,
               "Can't guard inconsistent flags");
    MOZ_ASSERT((expectedFlags | unexpectedFlags) != 0,
               "Can't guard zero flags");
    setGuard();
    setMovable();
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(GuardFunctionFlags)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, function))

  uint16_t expectedFlags() const { return expectedFlags_; };
  uint16_t unexpectedFlags() const { return unexpectedFlags_; };

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardFunctionFlags()) {
      return false;
    }
    if (expectedFlags() != ins->toGuardFunctionFlags()->expectedFlags()) {
      return false;
    }
    if (unexpectedFlags() != ins->toGuardFunctionFlags()->unexpectedFlags()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::ObjectFields);
  }
};

// Guard on an object's identity, inclusively or exclusively.
class MGuardObjectIdentity : public MBinaryInstruction,
                             public SingleObjectPolicy::Data {
  bool bailOnEquality_;

  MGuardObjectIdentity(MDefinition* obj, MDefinition* expected,
                       bool bailOnEquality)
      : MBinaryInstruction(classOpcode, obj, expected),
        bailOnEquality_(bailOnEquality) {
    setGuard();
    setMovable();
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(GuardObjectIdentity)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, expected))

  bool bailOnEquality() const { return bailOnEquality_; }
  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardObjectIdentity()) {
      return false;
    }
    if (bailOnEquality() != ins->toGuardObjectIdentity()->bailOnEquality()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

// Guard on a specific JSFunction. Used instead of MGuardObjectIdentity,
// so we can store some metadata related to the expected function.
class MGuardSpecificFunction : public MBinaryInstruction,
                               public SingleObjectPolicy::Data {
  uint16_t nargs_;
  FunctionFlags flags_;

  MGuardSpecificFunction(MDefinition* obj, MDefinition* expected,
                         uint16_t nargs, FunctionFlags flags)
      : MBinaryInstruction(classOpcode, obj, expected),
        nargs_(nargs),
        flags_(flags) {
    MOZ_ASSERT(expected->isConstant() || expected->isNurseryObject());
    setGuard();
    setMovable();
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(GuardSpecificFunction)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, function), (1, expected))

  uint16_t nargs() const { return nargs_; }
  FunctionFlags flags() const { return flags_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardSpecificFunction()) {
      return false;
    }

    auto* other = ins->toGuardSpecificFunction();
    if (nargs() != other->nargs() ||
        flags().toRaw() != other->flags().toRaw()) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MGuardSpecificSymbol : public MUnaryInstruction,
                             public SymbolPolicy<0>::Data {
  CompilerGCPointer<JS::Symbol*> expected_;

  MGuardSpecificSymbol(MDefinition* symbol, JS::Symbol* expected)
      : MUnaryInstruction(classOpcode, symbol), expected_(expected) {
    setGuard();
    setMovable();
    setResultType(MIRType::Symbol);
  }

 public:
  INSTRUCTION_HEADER(GuardSpecificSymbol)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, symbol))

  JS::Symbol* expected() const { return expected_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardSpecificSymbol()) {
      return false;
    }
    if (expected() != ins->toGuardSpecificSymbol()->expected()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MGuardTagNotEqual
    : public MBinaryInstruction,
      public MixPolicy<UnboxedInt32Policy<0>, UnboxedInt32Policy<1>>::Data {
  MGuardTagNotEqual(MDefinition* left, MDefinition* right)
      : MBinaryInstruction(classOpcode, left, right) {
    setGuard();
    setMovable();
    setCommutative();
  }

 public:
  INSTRUCTION_HEADER(GuardTagNotEqual)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return binaryCongruentTo(ins);
  }
};

// Load from vp[slot] (slots that are not inline in an object).
class MLoadDynamicSlot : public MUnaryInstruction, public NoTypePolicy::Data {
  uint32_t slot_;
  bool usedAsPropertyKey_ = false;

  MLoadDynamicSlot(MDefinition* slots, uint32_t slot)
      : MUnaryInstruction(classOpcode, slots), slot_(slot) {
    setResultType(MIRType::Value);
    setMovable();
    MOZ_ASSERT(slots->type() == MIRType::Slots);
  }

 public:
  INSTRUCTION_HEADER(LoadDynamicSlot)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, slots))

  uint32_t slot() const { return slot_; }

  HashNumber valueHash() const override;
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadDynamicSlot()) {
      return false;
    }
    if (slot() != ins->toLoadDynamicSlot()->slot()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override {
    MOZ_ASSERT(slots()->type() == MIRType::Slots);
    return AliasSet::Load(AliasSet::DynamicSlot);
  }
  AliasType mightAlias(const MDefinition* store) const override;

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  void setUsedAsPropertyKey() { usedAsPropertyKey_ = true; }
  bool usedAsPropertyKey() const { return usedAsPropertyKey_; }

  ALLOW_CLONE(MLoadDynamicSlot)
};

class MAddAndStoreSlot
    : public MBinaryInstruction,
      public MixPolicy<SingleObjectPolicy, BoxPolicy<1>>::Data {
 public:
  enum class Kind {
    FixedSlot,
    DynamicSlot,
  };

 private:
  Kind kind_;
  uint32_t slotOffset_;
  CompilerShape shape_;

  MAddAndStoreSlot(MDefinition* obj, MDefinition* value, Kind kind,
                   uint32_t slotOffset, Shape* shape)
      : MBinaryInstruction(classOpcode, obj, value),
        kind_(kind),
        slotOffset_(slotOffset),
        shape_(shape) {}

 public:
  INSTRUCTION_HEADER(AddAndStoreSlot)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, value))

  Kind kind() const { return kind_; }
  uint32_t slotOffset() const { return slotOffset_; }
  Shape* shape() const { return shape_; }

  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::ObjectFields |
                           (kind() == Kind::FixedSlot ? AliasSet::FixedSlot
                                                      : AliasSet::DynamicSlot));
  }
};

// Store to vp[slot] (slots that are not inline in an object).
class MStoreDynamicSlot : public MBinaryInstruction,
                          public NoFloatPolicy<1>::Data {
  uint32_t slot_;
  bool needsBarrier_;

  MStoreDynamicSlot(MDefinition* slots, uint32_t slot, MDefinition* value,
                    bool barrier)
      : MBinaryInstruction(classOpcode, slots, value),
        slot_(slot),
        needsBarrier_(barrier) {
    MOZ_ASSERT(slots->type() == MIRType::Slots);
  }

 public:
  INSTRUCTION_HEADER(StoreDynamicSlot)
  NAMED_OPERANDS((0, slots), (1, value))

  static MStoreDynamicSlot* NewUnbarriered(TempAllocator& alloc,
                                           MDefinition* slots, uint32_t slot,
                                           MDefinition* value) {
    return new (alloc) MStoreDynamicSlot(slots, slot, value, false);
  }
  static MStoreDynamicSlot* NewBarriered(TempAllocator& alloc,
                                         MDefinition* slots, uint32_t slot,
                                         MDefinition* value) {
    return new (alloc) MStoreDynamicSlot(slots, slot, value, true);
  }

  uint32_t slot() const { return slot_; }
  bool needsBarrier() const { return needsBarrier_; }
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::DynamicSlot);
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  ALLOW_CLONE(MStoreDynamicSlot)
};

class MSetPropertyCache : public MTernaryInstruction,
                          public MixPolicy<SingleObjectPolicy, CacheIdPolicy<1>,
                                           NoFloatPolicy<2>>::Data {
  bool strict_ : 1;

  MSetPropertyCache(MDefinition* obj, MDefinition* id, MDefinition* value,
                    bool strict)
      : MTernaryInstruction(classOpcode, obj, id, value), strict_(strict) {}

 public:
  INSTRUCTION_HEADER(SetPropertyCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, idval), (2, value))

  bool strict() const { return strict_; }
};

class MMegamorphicSetElement : public MTernaryInstruction,
                               public MegamorphicSetElementPolicy::Data {
  bool strict_;

  MMegamorphicSetElement(MDefinition* object, MDefinition* index,
                         MDefinition* value, bool strict)
      : MTernaryInstruction(classOpcode, object, index, value),
        strict_(strict) {}

 public:
  INSTRUCTION_HEADER(MegamorphicSetElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, index), (2, value))

  bool strict() const { return strict_; }

  bool possiblyCalls() const override { return true; }
};

class MSetDOMProperty : public MBinaryInstruction,
                        public MixPolicy<ObjectPolicy<0>, BoxPolicy<1>>::Data {
  const JSJitSetterOp func_;
  Realm* setterRealm_;
  DOMObjectKind objectKind_;

  MSetDOMProperty(const JSJitSetterOp func, DOMObjectKind objectKind,
                  Realm* setterRealm, MDefinition* obj, MDefinition* val)
      : MBinaryInstruction(classOpcode, obj, val),
        func_(func),
        setterRealm_(setterRealm),
        objectKind_(objectKind) {}

 public:
  INSTRUCTION_HEADER(SetDOMProperty)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, value))

  JSJitSetterOp fun() const { return func_; }
  Realm* setterRealm() const { return setterRealm_; }
  DOMObjectKind objectKind() const { return objectKind_; }

  bool possiblyCalls() const override { return true; }
};

class MGetDOMPropertyBase : public MVariadicInstruction,
                            public ObjectPolicy<0>::Data {
  const JSJitInfo* info_;

 protected:
  MGetDOMPropertyBase(Opcode op, const JSJitInfo* jitinfo)
      : MVariadicInstruction(op), info_(jitinfo) {
    MOZ_ASSERT(jitinfo);
    MOZ_ASSERT(jitinfo->type() == JSJitInfo::Getter);

    // We are movable iff the jitinfo says we can be.
    if (isDomMovable()) {
      MOZ_ASSERT(jitinfo->aliasSet() != JSJitInfo::AliasEverything);
      setMovable();
    } else {
      // If we're not movable, that means we shouldn't be DCEd either,
      // because we might throw an exception when called, and getting rid
      // of that is observable.
      setGuard();
    }

    setResultType(MIRType::Value);
  }

  const JSJitInfo* info() const { return info_; }

  [[nodiscard]] bool init(TempAllocator& alloc, MDefinition* obj,
                          MDefinition* guard, MDefinition* globalGuard) {
    MOZ_ASSERT(obj);
    // guard can be null.
    // globalGuard can be null.
    size_t operandCount = 1;
    if (guard) {
      ++operandCount;
    }
    if (globalGuard) {
      ++operandCount;
    }
    if (!MVariadicInstruction::init(alloc, operandCount)) {
      return false;
    }
    initOperand(0, obj);

    size_t operandIndex = 1;
    // Pin the guard, if we have one as an operand if we want to hoist later.
    if (guard) {
      initOperand(operandIndex++, guard);
    }

    // And the same for the global guard, if we have one.
    if (globalGuard) {
      initOperand(operandIndex, globalGuard);
    }

    return true;
  }

 public:
  NAMED_OPERANDS((0, object))

  JSJitGetterOp fun() const { return info_->getter; }
  bool isInfallible() const { return info_->isInfallible; }
  bool isDomMovable() const { return info_->isMovable; }
  JSJitInfo::AliasSet domAliasSet() const { return info_->aliasSet(); }
  size_t domMemberSlotIndex() const {
    MOZ_ASSERT(info_->isAlwaysInSlot || info_->isLazilyCachedInSlot);
    return info_->slotIndex;
  }
  bool valueMayBeInSlot() const { return info_->isLazilyCachedInSlot; }

  bool baseCongruentTo(const MGetDOMPropertyBase* ins) const {
    if (!isDomMovable()) {
      return false;
    }

    // Checking the jitinfo is the same as checking the constant function
    if (!(info() == ins->info())) {
      return false;
    }

    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    JSJitInfo::AliasSet aliasSet = domAliasSet();
    if (aliasSet == JSJitInfo::AliasNone) {
      return AliasSet::None();
    }
    if (aliasSet == JSJitInfo::AliasDOMSets) {
      return AliasSet::Load(AliasSet::DOMProperty);
    }
    MOZ_ASSERT(aliasSet == JSJitInfo::AliasEverything);
    return AliasSet::Store(AliasSet::Any);
  }
};

class MGetDOMProperty : public MGetDOMPropertyBase {
  Realm* getterRealm_;
  DOMObjectKind objectKind_;

  MGetDOMProperty(const JSJitInfo* jitinfo, DOMObjectKind objectKind,
                  Realm* getterRealm)
      : MGetDOMPropertyBase(classOpcode, jitinfo),
        getterRealm_(getterRealm),
        objectKind_(objectKind) {}

 public:
  INSTRUCTION_HEADER(GetDOMProperty)

  static MGetDOMProperty* New(TempAllocator& alloc, const JSJitInfo* info,
                              DOMObjectKind objectKind, Realm* getterRealm,
                              MDefinition* obj, MDefinition* guard,
                              MDefinition* globalGuard) {
    auto* res = new (alloc) MGetDOMProperty(info, objectKind, getterRealm);
    if (!res || !res->init(alloc, obj, guard, globalGuard)) {
      return nullptr;
    }
    return res;
  }

  Realm* getterRealm() const { return getterRealm_; }
  DOMObjectKind objectKind() const { return objectKind_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGetDOMProperty()) {
      return false;
    }

    if (ins->toGetDOMProperty()->getterRealm() != getterRealm()) {
      return false;
    }

    return baseCongruentTo(ins->toGetDOMProperty());
  }

  bool possiblyCalls() const override { return true; }
};

class MGetDOMMember : public MGetDOMPropertyBase {
  explicit MGetDOMMember(const JSJitInfo* jitinfo)
      : MGetDOMPropertyBase(classOpcode, jitinfo) {
    setResultType(MIRTypeFromValueType(jitinfo->returnType()));
  }

 public:
  INSTRUCTION_HEADER(GetDOMMember)

  static MGetDOMMember* New(TempAllocator& alloc, const JSJitInfo* info,
                            MDefinition* obj, MDefinition* guard,
                            MDefinition* globalGuard) {
    auto* res = new (alloc) MGetDOMMember(info);
    if (!res || !res->init(alloc, obj, guard, globalGuard)) {
      return nullptr;
    }
    return res;
  }

  bool possiblyCalls() const override { return false; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGetDOMMember()) {
      return false;
    }

    return baseCongruentTo(ins->toGetDOMMember());
  }
};

class MLoadDOMExpandoValueGuardGeneration : public MUnaryInstruction,
                                            public SingleObjectPolicy::Data {
  JS::ExpandoAndGeneration* expandoAndGeneration_;
  uint64_t generation_;

  MLoadDOMExpandoValueGuardGeneration(
      MDefinition* proxy, JS::ExpandoAndGeneration* expandoAndGeneration,
      uint64_t generation)
      : MUnaryInstruction(classOpcode, proxy),
        expandoAndGeneration_(expandoAndGeneration),
        generation_(generation) {
    setGuard();
    setMovable();
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(LoadDOMExpandoValueGuardGeneration)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, proxy))

  JS::ExpandoAndGeneration* expandoAndGeneration() const {
    return expandoAndGeneration_;
  }
  uint64_t generation() const { return generation_; }

  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isLoadDOMExpandoValueGuardGeneration()) {
      return false;
    }
    const auto* other = ins->toLoadDOMExpandoValueGuardGeneration();
    if (expandoAndGeneration() != other->expandoAndGeneration() ||
        generation() != other->generation()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::DOMProxyExpando);
  }
};

// Inlined assembly for Math.floor(double | float32) -> int32.
class MFloor : public MUnaryInstruction, public FloatingPointPolicy<0>::Data {
  explicit MFloor(MDefinition* num) : MUnaryInstruction(classOpcode, num) {
    setResultType(MIRType::Int32);
    specialization_ = MIRType::Double;
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Floor)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  void computeRange(TempAllocator& alloc) override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MFloor)
};

// Inlined assembly version for Math.ceil(double | float32) -> int32.
class MCeil : public MUnaryInstruction, public FloatingPointPolicy<0>::Data {
  explicit MCeil(MDefinition* num) : MUnaryInstruction(classOpcode, num) {
    setResultType(MIRType::Int32);
    specialization_ = MIRType::Double;
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Ceil)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  void computeRange(TempAllocator& alloc) override;
  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MCeil)
};

// Inlined version of Math.round(double | float32) -> int32.
class MRound : public MUnaryInstruction, public FloatingPointPolicy<0>::Data {
  explicit MRound(MDefinition* num) : MUnaryInstruction(classOpcode, num) {
    setResultType(MIRType::Int32);
    specialization_ = MIRType::Double;
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Round)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MRound)
};

// Inlined version of Math.trunc(double | float32) -> int32.
class MTrunc : public MUnaryInstruction, public FloatingPointPolicy<0>::Data {
  explicit MTrunc(MDefinition* num) : MUnaryInstruction(classOpcode, num) {
    setResultType(MIRType::Int32);
    specialization_ = MIRType::Double;
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(Trunc)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MTrunc)
};

// NearbyInt rounds the floating-point input to the nearest integer, according
// to the RoundingMode.
class MNearbyInt : public MUnaryInstruction,
                   public FloatingPointPolicy<0>::Data {
  RoundingMode roundingMode_;

  explicit MNearbyInt(MDefinition* num, MIRType resultType,
                      RoundingMode roundingMode)
      : MUnaryInstruction(classOpcode, num), roundingMode_(roundingMode) {
    MOZ_ASSERT(HasAssemblerSupport(roundingMode));

    MOZ_ASSERT(IsFloatingPointType(resultType));
    setResultType(resultType);
    specialization_ = resultType;

    setMovable();
  }

 public:
  INSTRUCTION_HEADER(NearbyInt)
  TRIVIAL_NEW_WRAPPERS

  static bool HasAssemblerSupport(RoundingMode mode) {
    return Assembler::HasRoundInstruction(mode);
  }

  RoundingMode roundingMode() const { return roundingMode_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool isFloat32Commutative() const override { return true; }
  void trySpecializeFloat32(TempAllocator& alloc) override;
#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#endif

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toNearbyInt()->roundingMode() == roundingMode_;
  }

#ifdef JS_JITSPEW
  void printOpcode(GenericPrinter& out) const override;
#endif

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;

  bool canRecoverOnBailout() const override {
    switch (roundingMode_) {
      case RoundingMode::Up:
      case RoundingMode::Down:
      case RoundingMode::TowardsZero:
        return true;
      default:
        return false;
    }
  }

  ALLOW_CLONE(MNearbyInt)
};

class MGetIteratorCache : public MUnaryInstruction,
                          public BoxExceptPolicy<0, MIRType::Object>::Data {
  explicit MGetIteratorCache(MDefinition* val)
      : MUnaryInstruction(classOpcode, val) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(GetIteratorCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value))
};

// Implementation for 'in' operator using instruction cache
class MInCache : public MBinaryInstruction,
                 public MixPolicy<CacheIdPolicy<0>, ObjectPolicy<1>>::Data {
  MInCache(MDefinition* key, MDefinition* obj)
      : MBinaryInstruction(classOpcode, key, obj) {
    setResultType(MIRType::Boolean);
  }

 public:
  INSTRUCTION_HEADER(InCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, key), (1, object))
};

// Test whether the index is in the array bounds or a hole.
class MInArray : public MTernaryInstruction, public NoTypePolicy::Data {
  bool needsNegativeIntCheck_ = true;

  MInArray(MDefinition* elements, MDefinition* index, MDefinition* initLength)
      : MTernaryInstruction(classOpcode, elements, index, initLength) {
    setResultType(MIRType::Boolean);
    setMovable();

    // Set the guard flag to make sure we bail when we see a negative index.
    // We can clear this flag (and needsNegativeIntCheck_) in
    // collectRangeInfoPreTrunc.
    setGuard();

    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    MOZ_ASSERT(initLength->type() == MIRType::Int32);
  }

 public:
  INSTRUCTION_HEADER(InArray)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, initLength))

  bool needsNegativeIntCheck() const { return needsNegativeIntCheck_; }
  void collectRangeInfoPreTrunc() override;
  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::Element);
  }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isInArray()) {
      return false;
    }
    const MInArray* other = ins->toInArray();
    if (needsNegativeIntCheck() != other->needsNegativeIntCheck()) {
      return false;
    }
    return congruentIfOperandsEqual(other);
  }
};

// Bail when the element is a hole.
class MGuardElementNotHole : public MBinaryInstruction,
                             public NoTypePolicy::Data {
  MGuardElementNotHole(MDefinition* elements, MDefinition* index)
      : MBinaryInstruction(classOpcode, elements, index) {
    setMovable();
    setGuard();
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::Int32);
  }

 public:
  INSTRUCTION_HEADER(GuardElementNotHole)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index))

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::Element);
  }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
};

class MCheckPrivateFieldCache
    : public MBinaryInstruction,
      public MixPolicy<BoxExceptPolicy<0, MIRType::Object>,
                       CacheIdPolicy<1>>::Data {
  MCheckPrivateFieldCache(MDefinition* obj, MDefinition* id)
      : MBinaryInstruction(classOpcode, obj, id) {
    setResultType(MIRType::Boolean);
  }

 public:
  INSTRUCTION_HEADER(CheckPrivateFieldCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value), (1, idval))
};

class MHasOwnCache : public MBinaryInstruction,
                     public MixPolicy<BoxExceptPolicy<0, MIRType::Object>,
                                      CacheIdPolicy<1>>::Data {
  MHasOwnCache(MDefinition* obj, MDefinition* id)
      : MBinaryInstruction(classOpcode, obj, id) {
    setResultType(MIRType::Boolean);
  }

 public:
  INSTRUCTION_HEADER(HasOwnCache)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value), (1, idval))
};

// Implementation for instanceof operator with specific rhs.
class MInstanceOf : public MBinaryInstruction,
                    public MixPolicy<BoxExceptPolicy<0, MIRType::Object>,
                                     ObjectPolicy<1>>::Data {
  MInstanceOf(MDefinition* obj, MDefinition* proto)
      : MBinaryInstruction(classOpcode, obj, proto) {
    setResultType(MIRType::Boolean);
  }

 public:
  INSTRUCTION_HEADER(InstanceOf)
  TRIVIAL_NEW_WRAPPERS
};

// Given a value being written to another object, update the generational store
// buffer if the value is in the nursery and object is in the tenured heap.
class MPostWriteBarrier : public MBinaryInstruction,
                          public ObjectPolicy<0>::Data {
  MPostWriteBarrier(MDefinition* obj, MDefinition* value)
      : MBinaryInstruction(classOpcode, obj, value) {
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(PostWriteBarrier)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, value))

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override {
    // During lowering, values that neither have object nor value MIR type
    // are ignored, thus Float32 can show up at this point without any issue.
    return use == getUseFor(1);
  }
#endif

  ALLOW_CLONE(MPostWriteBarrier)
};

// Given a value being written to another object's elements at the specified
// index, update the generational store buffer if the value is in the nursery
// and object is in the tenured heap.
class MPostWriteElementBarrier
    : public MTernaryInstruction,
      public MixPolicy<ObjectPolicy<0>, UnboxedInt32Policy<2>>::Data {
  MPostWriteElementBarrier(MDefinition* obj, MDefinition* value,
                           MDefinition* index)
      : MTernaryInstruction(classOpcode, obj, value, index) {
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(PostWriteElementBarrier)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object), (1, value), (2, index))

  AliasSet getAliasSet() const override { return AliasSet::None(); }

#ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override {
    // During lowering, values that neither have object nor value MIR type
    // are ignored, thus Float32 can show up at this point without any issue.
    return use == getUseFor(1);
  }
#endif

  ALLOW_CLONE(MPostWriteElementBarrier)
};

class MNewCallObject : public MUnaryInstruction,
                       public SingleObjectPolicy::Data {
  gc::Heap initialHeap_;

 public:
  INSTRUCTION_HEADER(NewCallObject)
  TRIVIAL_NEW_WRAPPERS

  explicit MNewCallObject(MConstant* templateObj, gc::Heap initialHeap)
      : MUnaryInstruction(classOpcode, templateObj), initialHeap_(initialHeap) {
    setResultType(MIRType::Object);
  }

  CallObject* templateObject() const {
    return &getOperand(0)->toConstant()->toObject().as<CallObject>();
  }
  gc::Heap initialHeap() const { return initialHeap_; }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }
};

class MNewStringObject : public MUnaryInstruction,
                         public ConvertToStringPolicy<0>::Data {
  CompilerObject templateObj_;

  MNewStringObject(MDefinition* input, JSObject* templateObj)
      : MUnaryInstruction(classOpcode, input), templateObj_(templateObj) {
    setResultType(MIRType::Object);
  }

 public:
  INSTRUCTION_HEADER(NewStringObject)
  TRIVIAL_NEW_WRAPPERS

  StringObject* templateObj() const;
};

// This is an alias for MLoadFixedSlot.
class MEnclosingEnvironment : public MLoadFixedSlot {
  explicit MEnclosingEnvironment(MDefinition* obj)
      : MLoadFixedSlot(obj, EnvironmentObject::enclosingEnvironmentSlot()) {
    setResultType(MIRType::Object);
  }

 public:
  static MEnclosingEnvironment* New(TempAllocator& alloc, MDefinition* obj) {
    return new (alloc) MEnclosingEnvironment(obj);
  }

  AliasSet getAliasSet() const override {
    // EnvironmentObject reserved slots are immutable.
    return AliasSet::None();
  }
};

// This is an element of a spaghetti stack which is used to represent the memory
// context which has to be restored in case of a bailout.
struct MStoreToRecover : public TempObject,
                         public InlineSpaghettiStackNode<MStoreToRecover> {
  MDefinition* operand;

  explicit MStoreToRecover(MDefinition* operand) : operand(operand) {}
};

using MStoresToRecoverList = InlineSpaghettiStack<MStoreToRecover>;

// A resume point contains the information needed to reconstruct the Baseline
// Interpreter state from a position in Warp JIT code. A resume point is a
// mapping of stack slots to MDefinitions.
//
// We capture stack state at critical points:
//   * (1) At the beginning of every basic block.
//   * (2) After every effectful operation.
//
// As long as these two properties are maintained, instructions can be moved,
// hoisted, or, eliminated without problems, and ops without side effects do not
// need to worry about capturing state at precisely the right point in time.
//
// Effectful instructions, of course, need to capture state after completion,
// where the interpreter will not attempt to repeat the operation. For this,
// ResumeAfter must be used. The state is attached directly to the effectful
// instruction to ensure that no intermediate instructions could be injected
// in between by a future analysis pass.
//
// During LIR construction, if an instruction can bail back to the interpreter,
// we create an LSnapshot, which uses the last known resume point to request
// register/stack assignments for every live value.
class MResumePoint final : public MNode
#ifdef DEBUG
    ,
                           public InlineForwardListNode<MResumePoint>
#endif
{
 private:
  friend class MBasicBlock;
  friend void AssertBasicGraphCoherency(MIRGraph& graph, bool force);

  // List of stack slots needed to reconstruct the BaselineFrame.
  FixedList<MUse> operands_;

  // List of stores needed to reconstruct the content of objects which are
  // emulated by EmulateStateOf variants.
  MStoresToRecoverList stores_;

  jsbytecode* pc_;
  MInstruction* instruction_;
  ResumeMode mode_;
  bool isDiscarded_ = false;

  MResumePoint(MBasicBlock* block, jsbytecode* pc, ResumeMode mode);
  void inherit(MBasicBlock* state);

  // Calling isDefinition or isResumePoint on MResumePoint is unnecessary.
  bool isDefinition() const = delete;
  bool isResumePoint() const = delete;

  void setBlock(MBasicBlock* block) {
    setBlockAndKind(block, Kind::ResumePoint);
  }

 protected:
  // Initializes operands_ to an empty array of a fixed length.
  // The array may then be filled in by inherit().
  [[nodiscard]] bool init(TempAllocator& alloc);

  void clearOperand(size_t index) {
    // FixedList doesn't initialize its elements, so do an unchecked init.
    operands_[index].initUncheckedWithoutProducer(this);
  }

  MUse* getUseFor(size_t index) override { return &operands_[index]; }
  const MUse* getUseFor(size_t index) const override {
    return &operands_[index];
  }

 public:
  static MResumePoint* New(TempAllocator& alloc, MBasicBlock* block,
                           jsbytecode* pc, ResumeMode mode);

  MBasicBlock* block() const { return resumePointBlock(); }

  size_t numAllocatedOperands() const { return operands_.length(); }
  uint32_t stackDepth() const { return numAllocatedOperands(); }
  size_t numOperands() const override { return numAllocatedOperands(); }
  size_t indexOf(const MUse* u) const final {
    MOZ_ASSERT(u >= &operands_[0]);
    MOZ_ASSERT(u <= &operands_[numOperands() - 1]);
    return u - &operands_[0];
  }
  void initOperand(size_t index, MDefinition* operand) {
    // FixedList doesn't initialize its elements, so do an unchecked init.
    operands_[index].initUnchecked(operand, this);
  }
  void replaceOperand(size_t index, MDefinition* operand) final {
    operands_[index].replaceProducer(operand);
  }

  bool isObservableOperand(MUse* u) const;
  bool isObservableOperand(size_t index) const;
  bool isRecoverableOperand(MUse* u) const;

  MDefinition* getOperand(size_t index) const override {
    return operands_[index].producer();
  }
  jsbytecode* pc() const { return pc_; }
  MResumePoint* caller() const;
  uint32_t frameCount() const {
    uint32_t count = 1;
    for (MResumePoint* it = caller(); it; it = it->caller()) {
      count++;
    }
    return count;
  }
  MInstruction* instruction() { return instruction_; }
  void setInstruction(MInstruction* ins) {
    MOZ_ASSERT(!instruction_);
    instruction_ = ins;
  }
  void resetInstruction() {
    MOZ_ASSERT(instruction_);
    instruction_ = nullptr;
  }
  ResumeMode mode() const { return mode_; }

  void releaseUses() {
    for (size_t i = 0, e = numOperands(); i < e; i++) {
      if (operands_[i].hasProducer()) {
        operands_[i].releaseProducer();
      }
    }
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;

  // Register a store instruction on the current resume point. This
  // instruction would be recovered when we are bailing out. The |cache|
  // argument can be any resume point, it is used to share memory if we are
  // doing the same modification.
  void addStore(TempAllocator& alloc, MDefinition* store,
                const MResumePoint* cache = nullptr);

  MStoresToRecoverList::iterator storesBegin() const { return stores_.begin(); }
  MStoresToRecoverList::iterator storesEnd() const { return stores_.end(); }
  bool storesEmpty() const { return stores_.empty(); }

  void setDiscarded() { isDiscarded_ = true; }
  bool isDiscarded() const { return isDiscarded_; }

#ifdef JS_JITSPEW
  virtual void dump(GenericPrinter& out) const override;
  virtual void dump() const override;
#endif
};

class MIsCallable : public MUnaryInstruction,
                    public BoxExceptPolicy<0, MIRType::Object>::Data {
  explicit MIsCallable(MDefinition* object)
      : MUnaryInstruction(classOpcode, object) {
    setResultType(MIRType::Boolean);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(IsCallable)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MHasClass : public MUnaryInstruction, public SingleObjectPolicy::Data {
  const JSClass* class_;

  MHasClass(MDefinition* object, const JSClass* clasp)
      : MUnaryInstruction(classOpcode, object), class_(clasp) {
    MOZ_ASSERT(object->type() == MIRType::Object);
    setResultType(MIRType::Boolean);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(HasClass)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  const JSClass* getClass() const { return class_; }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isHasClass()) {
      return false;
    }
    if (getClass() != ins->toHasClass()->getClass()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
};

class MGuardToClass : public MUnaryInstruction,
                      public SingleObjectPolicy::Data {
  const JSClass* class_;

  MGuardToClass(MDefinition* object, const JSClass* clasp)
      : MUnaryInstruction(classOpcode, object), class_(clasp) {
    MOZ_ASSERT(object->type() == MIRType::Object);
    MOZ_ASSERT(!clasp->isJSFunction(), "Use MGuardToFunction instead");
    setResultType(MIRType::Object);
    setMovable();

    // We will bail out if the class type is incorrect, so we need to ensure we
    // don't eliminate this instruction
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(GuardToClass)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  const JSClass* getClass() const { return class_; }
  bool isArgumentsObjectClass() const {
    return class_ == &MappedArgumentsObject::class_ ||
           class_ == &UnmappedArgumentsObject::class_;
  }

  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardToClass()) {
      return false;
    }
    if (getClass() != ins->toGuardToClass()->getClass()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
};

class MGuardToFunction : public MUnaryInstruction,
                         public SingleObjectPolicy::Data {
  explicit MGuardToFunction(MDefinition* object)
      : MUnaryInstruction(classOpcode, object) {
    MOZ_ASSERT(object->type() == MIRType::Object);
    setResultType(MIRType::Object);
    setMovable();

    // We will bail out if the class type is incorrect, so we need to ensure we
    // don't eliminate this instruction
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(GuardToFunction)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  MDefinition* foldsTo(TempAllocator& alloc) override;
  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    if (!ins->isGuardToFunction()) {
      return false;
    }
    return congruentIfOperandsEqual(ins);
  }
};

// Note: we might call a proxy trap, so this instruction is effectful.
class MIsArray : public MUnaryInstruction,
                 public BoxExceptPolicy<0, MIRType::Object>::Data {
  explicit MIsArray(MDefinition* value)
      : MUnaryInstruction(classOpcode, value) {
    setResultType(MIRType::Boolean);
  }

 public:
  INSTRUCTION_HEADER(IsArray)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value))

  MDefinition* foldsTo(TempAllocator& alloc) override;
};

class MIsTypedArray : public MUnaryInstruction,
                      public SingleObjectPolicy::Data {
  bool possiblyWrapped_;

  explicit MIsTypedArray(MDefinition* value, bool possiblyWrapped)
      : MUnaryInstruction(classOpcode, value),
        possiblyWrapped_(possiblyWrapped) {
    setResultType(MIRType::Boolean);

    if (possiblyWrapped) {
      // Proxy checks may throw, so we're neither removable nor movable.
      setGuard();
    } else {
      setMovable();
    }
  }

 public:
  INSTRUCTION_HEADER(IsTypedArray)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value))

  bool isPossiblyWrapped() const { return possiblyWrapped_; }
  AliasSet getAliasSet() const override {
    if (isPossiblyWrapped()) {
      return AliasSet::Store(AliasSet::Any);
    }
    return AliasSet::None();
  }
};

// Allocate the generator object for a frame.
class MGenerator : public MTernaryInstruction,
                   public MixPolicy<ObjectPolicy<0>, ObjectPolicy<1>>::Data {
  explicit MGenerator(MDefinition* callee, MDefinition* environmentChain,
                      MDefinition* argsObject)
      : MTernaryInstruction(classOpcode, callee, environmentChain, argsObject) {
    setResultType(MIRType::Object);
  };

 public:
  INSTRUCTION_HEADER(Generator)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, callee), (1, environmentChain), (2, argsObject))
};

class MMaybeExtractAwaitValue : public MBinaryInstruction,
                                public BoxPolicy<0>::Data {
  explicit MMaybeExtractAwaitValue(MDefinition* value, MDefinition* canSkip)
      : MBinaryInstruction(classOpcode, value, canSkip) {
    setResultType(MIRType::Value);
  }

 public:
  INSTRUCTION_HEADER(MaybeExtractAwaitValue)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, value), (1, canSkip))
};

class MAtomicIsLockFree : public MUnaryInstruction,
                          public ConvertToInt32Policy<0>::Data {
  explicit MAtomicIsLockFree(MDefinition* value)
      : MUnaryInstruction(classOpcode, value) {
    setResultType(MIRType::Boolean);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(AtomicIsLockFree)
  TRIVIAL_NEW_WRAPPERS

  MDefinition* foldsTo(TempAllocator& alloc) override;

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  [[nodiscard]] bool writeRecoverData(
      CompactBufferWriter& writer) const override;
  bool canRecoverOnBailout() const override { return true; }

  ALLOW_CLONE(MAtomicIsLockFree)
};

class MCompareExchangeTypedArrayElement
    : public MQuaternaryInstruction,
      public MixPolicy<TruncateToInt32OrToInt64Policy<2>,
                       TruncateToInt32OrToInt64Policy<3>>::Data {
  Scalar::Type arrayType_;

  explicit MCompareExchangeTypedArrayElement(MDefinition* elements,
                                             MDefinition* index,
                                             Scalar::Type arrayType,
                                             MDefinition* oldval,
                                             MDefinition* newval)
      : MQuaternaryInstruction(classOpcode, elements, index, oldval, newval),
        arrayType_(arrayType) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    setGuard();  // Not removable
  }

 public:
  INSTRUCTION_HEADER(CompareExchangeTypedArrayElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, oldval), (3, newval))

  bool isByteArray() const {
    return (arrayType_ == Scalar::Int8 || arrayType_ == Scalar::Uint8);
  }
  Scalar::Type arrayType() const { return arrayType_; }
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::UnboxedElement);
  }
};

class MAtomicExchangeTypedArrayElement
    : public MTernaryInstruction,
      public TruncateToInt32OrToInt64Policy<2>::Data {
  Scalar::Type arrayType_;

  MAtomicExchangeTypedArrayElement(MDefinition* elements, MDefinition* index,
                                   MDefinition* value, Scalar::Type arrayType)
      : MTernaryInstruction(classOpcode, elements, index, value),
        arrayType_(arrayType) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(arrayType <= Scalar::Uint32 || Scalar::isBigIntType(arrayType));
    setGuard();  // Not removable
  }

 public:
  INSTRUCTION_HEADER(AtomicExchangeTypedArrayElement)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, value))

  bool isByteArray() const {
    return (arrayType_ == Scalar::Int8 || arrayType_ == Scalar::Uint8);
  }
  Scalar::Type arrayType() const { return arrayType_; }
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::UnboxedElement);
  }
};

class MAtomicTypedArrayElementBinop
    : public MTernaryInstruction,
      public TruncateToInt32OrToInt64Policy<2>::Data {
 private:
  AtomicOp op_;
  Scalar::Type arrayType_;
  bool forEffect_;

  explicit MAtomicTypedArrayElementBinop(AtomicOp op, MDefinition* elements,
                                         MDefinition* index,
                                         Scalar::Type arrayType,
                                         MDefinition* value, bool forEffect)
      : MTernaryInstruction(classOpcode, elements, index, value),
        op_(op),
        arrayType_(arrayType),
        forEffect_(forEffect) {
    MOZ_ASSERT(elements->type() == MIRType::Elements);
    MOZ_ASSERT(index->type() == MIRType::IntPtr);
    MOZ_ASSERT(arrayType <= Scalar::Uint32 || Scalar::isBigIntType(arrayType));
    setGuard();  // Not removable
  }

 public:
  INSTRUCTION_HEADER(AtomicTypedArrayElementBinop)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, elements), (1, index), (2, value))

  bool isByteArray() const {
    return (arrayType_ == Scalar::Int8 || arrayType_ == Scalar::Uint8);
  }
  AtomicOp operation() const { return op_; }
  Scalar::Type arrayType() const { return arrayType_; }
  bool isForEffect() const { return forEffect_; }
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::UnboxedElement);
  }
};

class MDebugger : public MNullaryInstruction {
  MDebugger() : MNullaryInstruction(classOpcode) {
    setBailoutKind(BailoutKind::Debugger);
  }

 public:
  INSTRUCTION_HEADER(Debugger)
  TRIVIAL_NEW_WRAPPERS
};

// Used to load the prototype of an object known to have
// a static prototype.
class MObjectStaticProto : public MUnaryInstruction,
                           public SingleObjectPolicy::Data {
  explicit MObjectStaticProto(MDefinition* object)
      : MUnaryInstruction(classOpcode, object) {
    setResultType(MIRType::Object);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(ObjectStaticProto)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  AliasSet getAliasSet() const override {
    return AliasSet::Load(AliasSet::ObjectFields);
  }
  AliasType mightAlias(const MDefinition* def) const override {
    // These instructions never modify the [[Prototype]].
    if (def->isAddAndStoreSlot() || def->isAllocateAndStoreSlot() ||
        def->isStoreElementHole() || def->isArrayPush()) {
      return AliasType::NoAlias;
    }
    return AliasType::MayAlias;
  }
};

class MConstantProto : public MUnaryInstruction,
                       public SingleObjectPolicy::Data {
  // NOTE: we're not going to actually use the underlying receiver object for
  // anything. This is just here for giving extra information to MGuardShape
  // to MGuardShape::mightAlias. Accordingly, we don't take it as an operand,
  // but instead just keep a pointer to it. This means we need to ensure it's
  // not discarded before we try to access it. If this is discarded, we
  // basically just become an MConstant for the object's proto, which is fine.
  const MDefinition* receiverObject_;

  explicit MConstantProto(MDefinition* protoObject,
                          const MDefinition* receiverObject)
      : MUnaryInstruction(classOpcode, protoObject),
        receiverObject_(receiverObject) {
    MOZ_ASSERT(protoObject->isConstant());
    setResultType(MIRType::Object);
    setMovable();
  }

  ALLOW_CLONE(MConstantProto)

 public:
  INSTRUCTION_HEADER(ConstantProto)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, protoObject))

  HashNumber valueHash() const override;

  bool congruentTo(const MDefinition* ins) const override {
    if (this == ins) {
      return true;
    }
    const MDefinition* receiverObject = getReceiverObject();
    return congruentIfOperandsEqual(ins) && receiverObject &&
           receiverObject == ins->toConstantProto()->getReceiverObject();
  }

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  const MDefinition* getReceiverObject() const {
    if (receiverObject_->isDiscarded()) {
      return nullptr;
    }
    return receiverObject_;
  }
};

class MObjectToIterator : public MUnaryInstruction,
                          public ObjectPolicy<0>::Data {
  NativeIteratorListHead* enumeratorsAddr_;
  bool wantsIndices_ = false;

  explicit MObjectToIterator(MDefinition* object,
                             NativeIteratorListHead* enumeratorsAddr)
      : MUnaryInstruction(classOpcode, object),
        enumeratorsAddr_(enumeratorsAddr) {
    setResultType(MIRType::Object);
  }

 public:
  NativeIteratorListHead* enumeratorsAddr() const { return enumeratorsAddr_; }
  INSTRUCTION_HEADER(ObjectToIterator)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, object))

  bool wantsIndices() const { return wantsIndices_; }
  void setWantsIndices(bool value) { wantsIndices_ = value; }
};

class MPostIntPtrConversion : public MUnaryInstruction,
                              public NoTypePolicy::Data {
  explicit MPostIntPtrConversion(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    // Passes through the input.
    setResultType(input->type());

    // Note: Must be non-movable so we can attach a resume point.
  }

 public:
  INSTRUCTION_HEADER(PostIntPtrConversion)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

class MCanonicalizeNaN : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MCanonicalizeNaN(MDefinition* input)
      : MUnaryInstruction(classOpcode, input) {
    MOZ_ASSERT(IsFloatingPointType(input->type()));
    setResultType(input->type());
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(CanonicalizeNaN)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }

  bool canProduceFloat32() const override { return type() == MIRType::Float32; }

  ALLOW_CLONE(MCanonicalizeNaN)
};

class MRotate : public MBinaryInstruction, public NoTypePolicy::Data {
  bool isLeftRotate_;

  MRotate(MDefinition* input, MDefinition* count, MIRType type,
          bool isLeftRotate)
      : MBinaryInstruction(classOpcode, input, count),
        isLeftRotate_(isLeftRotate) {
    setMovable();
    setResultType(type);
    // Prevent reordering.  Although there's no problem eliding call result
    // definitions, there's also no need, as they cause no codegen.
    setGuard();
  }

 public:
  INSTRUCTION_HEADER(Rotate)
  TRIVIAL_NEW_WRAPPERS
  NAMED_OPERANDS((0, input), (1, count))

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins) &&
           ins->toRotate()->isLeftRotate() == isLeftRotate_;
  }

  bool isLeftRotate() const { return isLeftRotate_; }

  ALLOW_CLONE(MRotate)
};

class MReinterpretCast : public MUnaryInstruction, public NoTypePolicy::Data {
  MReinterpretCast(MDefinition* val, MIRType toType)
      : MUnaryInstruction(classOpcode, val) {
    switch (val->type()) {
      case MIRType::Int32:
        MOZ_ASSERT(toType == MIRType::Float32);
        break;
      case MIRType::Float32:
        MOZ_ASSERT(toType == MIRType::Int32);
        break;
      case MIRType::Double:
        MOZ_ASSERT(toType == MIRType::Int64);
        break;
      case MIRType::Int64:
        MOZ_ASSERT(toType == MIRType::Double);
        break;
      default:
        MOZ_CRASH("unexpected reinterpret conversion");
    }
    setMovable();
    setResultType(toType);
  }

 public:
  INSTRUCTION_HEADER(ReinterpretCast)
  TRIVIAL_NEW_WRAPPERS

  AliasSet getAliasSet() const override { return AliasSet::None(); }
  bool congruentTo(const MDefinition* ins) const override {
    // No need to check type() here, because congruentIfOperandsEqual will
    // check it.
    return congruentIfOperandsEqual(ins);
  }

  ALLOW_CLONE(MReinterpretCast)
};

// Used by MIR building to represent the bytecode result of an operation for
// which an MBail was generated, to balance the basic block's MDefinition stack.
class MUnreachableResult : public MNullaryInstruction {
  explicit MUnreachableResult(MIRType type) : MNullaryInstruction(classOpcode) {
    MOZ_ASSERT(type != MIRType::None);
    setResultType(type);
  }

 public:
  INSTRUCTION_HEADER(UnreachableResult)
  TRIVIAL_NEW_WRAPPERS

  bool congruentTo(const MDefinition* ins) const override {
    return congruentIfOperandsEqual(ins);
  }
  AliasSet getAliasSet() const override { return AliasSet::None(); }
};

#ifdef FUZZING_JS_FUZZILLI
class MFuzzilliHash : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MFuzzilliHash(MDefinition* obj)
      : MUnaryInstruction(classOpcode, obj) {
    setResultType(MIRType::Int32);
    setMovable();
  }

 public:
  INSTRUCTION_HEADER(FuzzilliHash);
  TRIVIAL_NEW_WRAPPERS
  ALLOW_CLONE(MFuzzilliHash)

#  ifdef DEBUG
  bool isConsistentFloat32Use(MUse* use) const override { return true; }
#  endif

  AliasSet getAliasSet() const override {
    MDefinition* obj = getOperand(0);
    if (obj->type() == MIRType::Object || obj->type() == MIRType::Value) {
      return AliasSet::Load(AliasSet::ObjectFields | AliasSet::FixedSlot |
                            AliasSet::DynamicSlot | AliasSet::Element |
                            AliasSet::UnboxedElement);
    }
    return AliasSet::None();
  }
};

class MFuzzilliHashStore : public MUnaryInstruction, public NoTypePolicy::Data {
  explicit MFuzzilliHashStore(MDefinition* obj)
      : MUnaryInstruction(classOpcode, obj) {
    MOZ_ASSERT(obj->type() == MIRType::Int32);

    setResultType(MIRType::None);
  }

 public:
  INSTRUCTION_HEADER(FuzzilliHashStore);
  TRIVIAL_NEW_WRAPPERS
  ALLOW_CLONE(MFuzzilliHashStore)

  // this is a store and hence effectful, however no other load can
  // alias with the store
  AliasSet getAliasSet() const override {
    return AliasSet::Store(AliasSet::FuzzilliHash);
  }
};
#endif

void MUse::init(MDefinition* producer, MNode* consumer) {
  MOZ_ASSERT(!consumer_, "Initializing MUse that already has a consumer");
  MOZ_ASSERT(!producer_, "Initializing MUse that already has a producer");
  initUnchecked(producer, consumer);
}

void MUse::initUnchecked(MDefinition* producer, MNode* consumer) {
  MOZ_ASSERT(consumer, "Initializing to null consumer");
  consumer_ = consumer;
  producer_ = producer;
  producer_->addUseUnchecked(this);
}

void MUse::initUncheckedWithoutProducer(MNode* consumer) {
  MOZ_ASSERT(consumer, "Initializing to null consumer");
  consumer_ = consumer;
  producer_ = nullptr;
}

void MUse::replaceProducer(MDefinition* producer) {
  MOZ_ASSERT(consumer_, "Resetting MUse without a consumer");
  producer_->removeUse(this);
  producer_ = producer;
  producer_->addUse(this);
}

void MUse::releaseProducer() {
  MOZ_ASSERT(consumer_, "Clearing MUse without a consumer");
  producer_->removeUse(this);
  producer_ = nullptr;
}

// Implement cast functions now that the compiler can see the inheritance.

MDefinition* MNode::toDefinition() {
  MOZ_ASSERT(isDefinition());
  return (MDefinition*)this;
}

MResumePoint* MNode::toResumePoint() {
  MOZ_ASSERT(isResumePoint());
  return (MResumePoint*)this;
}

MInstruction* MDefinition::toInstruction() {
  MOZ_ASSERT(!isPhi());
  return (MInstruction*)this;
}

const MInstruction* MDefinition::toInstruction() const {
  MOZ_ASSERT(!isPhi());
  return (const MInstruction*)this;
}

MControlInstruction* MDefinition::toControlInstruction() {
  MOZ_ASSERT(isControlInstruction());
  return (MControlInstruction*)this;
}

MConstant* MDefinition::maybeConstantValue() {
  MDefinition* op = this;
  if (op->isBox()) {
    op = op->toBox()->input();
  }
  if (op->isConstant()) {
    return op->toConstant();
  }
  return nullptr;
}

}  // namespace jit
}  // namespace js

#endif /* jit_MIR_h */
