/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CacheIR.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"

#include "jsapi.h"
#include "jsdate.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/DataViewObject.h"
#include "builtin/MapObject.h"
#include "builtin/ModuleObject.h"
#include "builtin/Object.h"
#include "builtin/WeakMapObject.h"
#include "builtin/WeakSetObject.h"
#include "jit/BaselineIC.h"
#include "jit/CacheIRCloner.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRGenerator.h"
#include "jit/CacheIRSpewer.h"
#include "jit/CacheIRWriter.h"
#include "jit/InlinableNatives.h"
#include "jit/JitContext.h"
#include "jit/JitZone.h"
#include "js/experimental/JitInfo.h"  // JSJitInfo
#include "js/friend/DOMProxy.h"       // JS::ExpandoAndGeneration
#include "js/friend/WindowProxy.h"  // js::IsWindow, js::IsWindowProxy, js::ToWindowIfWindowProxy
#include "js/friend/XrayJitInfo.h"  // js::jit::GetXrayJitInfo, JS::XrayJitInfo
#include "js/GCAPI.h"               // JS::AutoSuppressGCAnalysis
#include "js/Prefs.h"               // JS::Prefs
#include "js/RegExpFlags.h"         // JS::RegExpFlags
#include "js/ScalarType.h"          // js::Scalar::Type
#include "js/Utility.h"             // JS::AutoEnterOOMUnsafeRegion
#include "js/Wrapper.h"
#include "proxy/DOMProxy.h"  // js::GetDOMProxyHandlerFamily
#include "proxy/ScriptedProxyHandler.h"
#include "util/DifferentialTesting.h"
#include "util/Unicode.h"
#include "vm/ArrayBufferObject.h"
#include "vm/BoundFunctionObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compartment.h"
#include "vm/DateObject.h"
#include "vm/Iteration.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/ProxyObject.h"
#include "vm/RegExpObject.h"
#include "vm/SelfHosting.h"
#include "vm/ThrowMsgKind.h"     // ThrowCondition
#include "vm/TypeofEqOperand.h"  // TypeofEqOperand
#include "vm/Watchtower.h"
#include "wasm/WasmInstance.h"

#include "jit/BaselineFrame-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/List-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/PlainObject-inl.h"
#include "vm/StringObject-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::Maybe;

using JS::DOMProxyShadowsResult;
using JS::ExpandoAndGeneration;

const char* const js::jit::CacheKindNames[] = {
#define DEFINE_KIND(kind) #kind,
    CACHE_IR_KINDS(DEFINE_KIND)
#undef DEFINE_KIND
};

const char* const js::jit::CacheIROpNames[] = {
#define OPNAME(op, ...) #op,
    CACHE_IR_OPS(OPNAME)
#undef OPNAME
};

const CacheIROpInfo js::jit::CacheIROpInfos[] = {
#define OPINFO(op, len, transpile, ...) {len, transpile},
    CACHE_IR_OPS(OPINFO)
#undef OPINFO
};

const uint32_t js::jit::CacheIROpHealth[] = {
#define OPHEALTH(op, len, transpile, health) health,
    CACHE_IR_OPS(OPHEALTH)
#undef OPHEALTH
};

size_t js::jit::NumInputsForCacheKind(CacheKind kind) {
  switch (kind) {
    case CacheKind::NewArray:
    case CacheKind::NewObject:
    case CacheKind::Lambda:
    case CacheKind::LazyConstant:
    case CacheKind::GetImport:
      return 0;
    case CacheKind::GetProp:
    case CacheKind::TypeOf:
    case CacheKind::TypeOfEq:
    case CacheKind::ToPropertyKey:
    case CacheKind::GetIterator:
    case CacheKind::ToBool:
    case CacheKind::UnaryArith:
    case CacheKind::GetName:
    case CacheKind::BindName:
    case CacheKind::Call:
    case CacheKind::OptimizeSpreadCall:
    case CacheKind::CloseIter:
    case CacheKind::OptimizeGetIterator:
      return 1;
    case CacheKind::Compare:
    case CacheKind::GetElem:
    case CacheKind::GetPropSuper:
    case CacheKind::SetProp:
    case CacheKind::In:
    case CacheKind::HasOwn:
    case CacheKind::CheckPrivateField:
    case CacheKind::InstanceOf:
    case CacheKind::BinaryArith:
      return 2;
    case CacheKind::GetElemSuper:
    case CacheKind::SetElem:
      return 3;
  }
  MOZ_CRASH("Invalid kind");
}

#ifdef DEBUG
void CacheIRWriter::assertSameCompartment(JSObject* obj) {
  MOZ_ASSERT(cx_->compartment() == obj->compartment());
}
void CacheIRWriter::assertSameZone(Shape* shape) {
  MOZ_ASSERT(cx_->zone() == shape->zone());
}
#endif

StubField CacheIRWriter::readStubField(uint32_t offset,
                                       StubField::Type type) const {
  size_t index = 0;
  size_t currentOffset = 0;

  // If we've seen an offset earlier than this before, we know we can start the
  // search there at least, otherwise, we start the search from the beginning.
  if (lastOffset_ < offset) {
    currentOffset = lastOffset_;
    index = lastIndex_;
  }

  while (currentOffset != offset) {
    currentOffset += StubField::sizeInBytes(stubFields_[index].type());
    index++;
    MOZ_ASSERT(index < stubFields_.length());
  }

  MOZ_ASSERT(stubFields_[index].type() == type);

  lastOffset_ = currentOffset;
  lastIndex_ = index;

  return stubFields_[index];
}

CacheIRCloner::CacheIRCloner(ICCacheIRStub* stub)
    : stubInfo_(stub->stubInfo()), stubData_(stub->stubDataStart()) {}

void CacheIRCloner::cloneOp(CacheOp op, CacheIRReader& reader,
                            CacheIRWriter& writer) {
  switch (op) {
#define DEFINE_OP(op, ...)     \
  case CacheOp::op:            \
    clone##op(reader, writer); \
    break;
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP
    default:
      MOZ_CRASH("Invalid op");
  }
}

uintptr_t CacheIRCloner::readStubWord(uint32_t offset) {
  return stubInfo_->getStubRawWord(stubData_, offset);
}
int64_t CacheIRCloner::readStubInt64(uint32_t offset) {
  return stubInfo_->getStubRawInt64(stubData_, offset);
}

Shape* CacheIRCloner::getShapeField(uint32_t stubOffset) {
  return reinterpret_cast<Shape*>(readStubWord(stubOffset));
}
Shape* CacheIRCloner::getWeakShapeField(uint32_t stubOffset) {
  // No barrier is required to clone a weak pointer.
  return reinterpret_cast<Shape*>(readStubWord(stubOffset));
}
JSObject* CacheIRCloner::getObjectField(uint32_t stubOffset) {
  return reinterpret_cast<JSObject*>(readStubWord(stubOffset));
}
JSObject* CacheIRCloner::getWeakObjectField(uint32_t stubOffset) {
  // No barrier is required to clone a weak pointer.
  return reinterpret_cast<JSObject*>(readStubWord(stubOffset));
}
JSString* CacheIRCloner::getStringField(uint32_t stubOffset) {
  return reinterpret_cast<JSString*>(readStubWord(stubOffset));
}
JSAtom* CacheIRCloner::getAtomField(uint32_t stubOffset) {
  return reinterpret_cast<JSAtom*>(readStubWord(stubOffset));
}
JS::Symbol* CacheIRCloner::getSymbolField(uint32_t stubOffset) {
  return reinterpret_cast<JS::Symbol*>(readStubWord(stubOffset));
}
BaseScript* CacheIRCloner::getWeakBaseScriptField(uint32_t stubOffset) {
  // No barrier is required to clone a weak pointer.
  return reinterpret_cast<BaseScript*>(readStubWord(stubOffset));
}
JitCode* CacheIRCloner::getJitCodeField(uint32_t stubOffset) {
  return reinterpret_cast<JitCode*>(readStubWord(stubOffset));
}
uint32_t CacheIRCloner::getRawInt32Field(uint32_t stubOffset) {
  return uint32_t(reinterpret_cast<uintptr_t>(readStubWord(stubOffset)));
}
const void* CacheIRCloner::getRawPointerField(uint32_t stubOffset) {
  return reinterpret_cast<const void*>(readStubWord(stubOffset));
}
uint64_t CacheIRCloner::getRawInt64Field(uint32_t stubOffset) {
  return static_cast<uint64_t>(readStubInt64(stubOffset));
}
gc::AllocSite* CacheIRCloner::getAllocSiteField(uint32_t stubOffset) {
  return reinterpret_cast<gc::AllocSite*>(readStubWord(stubOffset));
}

jsid CacheIRCloner::getIdField(uint32_t stubOffset) {
  return jsid::fromRawBits(readStubWord(stubOffset));
}
Value CacheIRCloner::getValueField(uint32_t stubOffset) {
  return Value::fromRawBits(uint64_t(readStubInt64(stubOffset)));
}
Value CacheIRCloner::getWeakValueField(uint32_t stubOffset) {
  // No barrier is required to clone a weak pointer.
  return Value::fromRawBits(uint64_t(readStubInt64(stubOffset)));
}
double CacheIRCloner::getDoubleField(uint32_t stubOffset) {
  uint64_t bits = uint64_t(readStubInt64(stubOffset));
  return mozilla::BitwiseCast<double>(bits);
}

IRGenerator::IRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                         CacheKind cacheKind, ICState state,
                         BaselineFrame* maybeFrame)
    : writer(cx),
      cx_(cx),
      script_(script),
      pc_(pc),
      maybeFrame_(maybeFrame),
      cacheKind_(cacheKind),
      mode_(state.mode()),
      isFirstStub_(state.newStubIsFirstStub()),
      numOptimizedStubs_(state.numOptimizedStubs()) {}

// Allocation sites are usually created during baseline compilation, but we also
// need to create them when an IC stub is added to a baseline compiled script
// and when trial inlining.
gc::AllocSite* IRGenerator::maybeCreateAllocSite() {
  MOZ_ASSERT(BytecodeOpCanHaveAllocSite(JSOp(*pc_)));

  BaselineFrame* frame = maybeFrame_;
  MOZ_ASSERT(frame);

  JSScript* outerScript = frame->outerScript();
  bool hasBaselineScript = outerScript->hasBaselineScript();
  bool isInlined = frame->icScript()->isInlined();
  if (!hasBaselineScript && !isInlined) {
    MOZ_ASSERT(frame->runningInInterpreter());
    return outerScript->zone()->unknownAllocSite(JS::TraceKind::Object);
  }

  uint32_t pcOffset = frame->script()->pcToOffset(pc_);
  return frame->icScript()->getOrCreateAllocSite(outerScript, pcOffset);
}

GetPropIRGenerator::GetPropIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state,
                                       CacheKind cacheKind, HandleValue val,
                                       HandleValue idVal)
    : IRGenerator(cx, script, pc, cacheKind, state), val_(val), idVal_(idVal) {}

static void EmitLoadSlotResult(CacheIRWriter& writer, ObjOperandId holderId,
                               NativeObject* holder, PropertyInfo prop) {
  if (holder->isFixedSlot(prop.slot())) {
    writer.loadFixedSlotResult(holderId,
                               NativeObject::getFixedSlotOffset(prop.slot()));
  } else {
    size_t dynamicSlotOffset =
        holder->dynamicSlotIndex(prop.slot()) * sizeof(Value);
    writer.loadDynamicSlotResult(holderId, dynamicSlotOffset);
  }
}

// DOM proxies
// -----------
//
// DOM proxies are proxies that are used to implement various DOM objects like
// HTMLDocument and NodeList. DOM proxies may have an expando object - a native
// object that stores extra properties added to the object. The following
// CacheIR instructions are only used with DOM proxies:
//
// * LoadDOMExpandoValue: returns the Value in the proxy's expando slot. This
//   returns either an UndefinedValue (no expando), ObjectValue (the expando
//   object), or PrivateValue(ExpandoAndGeneration*).
//
// * LoadDOMExpandoValueGuardGeneration: guards the Value in the proxy's expando
//   slot is the same PrivateValue(ExpandoAndGeneration*), then guards on its
//   generation, then returns expandoAndGeneration->expando. This Value is
//   either an UndefinedValue or ObjectValue.
//
// * LoadDOMExpandoValueIgnoreGeneration: assumes the Value in the proxy's
//   expando slot is a PrivateValue(ExpandoAndGeneration*), unboxes it, and
//   returns the expandoAndGeneration->expando Value.
//
// * GuardDOMExpandoMissingOrGuardShape: takes an expando Value as input, then
//   guards it's either UndefinedValue or an object with the expected shape.

enum class ProxyStubType {
  None,
  DOMExpando,
  DOMShadowed,
  DOMUnshadowed,
  Generic
};

static bool IsCacheableDOMProxy(ProxyObject* obj) {
  const BaseProxyHandler* handler = obj->handler();
  if (handler->family() != GetDOMProxyHandlerFamily()) {
    return false;
  }

  // Some DOM proxies have dynamic prototypes.  We can't really cache those very
  // well.
  return obj->hasStaticPrototype();
}

static ProxyStubType GetProxyStubType(JSContext* cx, HandleObject obj,
                                      HandleId id) {
  if (!obj->is<ProxyObject>()) {
    return ProxyStubType::None;
  }
  auto proxy = obj.as<ProxyObject>();

  if (!IsCacheableDOMProxy(proxy)) {
    return ProxyStubType::Generic;
  }

  // Private fields are defined on a separate expando object.
  if (id.isPrivateName()) {
    return ProxyStubType::Generic;
  }

  DOMProxyShadowsResult shadows = GetDOMProxyShadowsCheck()(cx, proxy, id);
  if (shadows == DOMProxyShadowsResult::ShadowCheckFailed) {
    cx->clearPendingException();
    return ProxyStubType::None;
  }

  if (DOMProxyIsShadowing(shadows)) {
    if (shadows == DOMProxyShadowsResult::ShadowsViaDirectExpando ||
        shadows == DOMProxyShadowsResult::ShadowsViaIndirectExpando) {
      return ProxyStubType::DOMExpando;
    }
    return ProxyStubType::DOMShadowed;
  }

  MOZ_ASSERT(shadows == DOMProxyShadowsResult::DoesntShadow ||
             shadows == DOMProxyShadowsResult::DoesntShadowUnique);
  return ProxyStubType::DOMUnshadowed;
}

static bool ValueToNameOrSymbolId(JSContext* cx, HandleValue idVal,
                                  MutableHandleId id, bool* nameOrSymbol) {
  *nameOrSymbol = false;

  if (idVal.isObject() || idVal.isBigInt()) {
    return true;
  }

  MOZ_ASSERT(idVal.isString() || idVal.isSymbol() || idVal.isBoolean() ||
             idVal.isUndefined() || idVal.isNull() || idVal.isNumber());

  if (IsNumberIndex(idVal)) {
    return true;
  }

  if (!PrimitiveValueToId<CanGC>(cx, idVal, id)) {
    return false;
  }

  if (!id.isAtom() && !id.isSymbol()) {
    id.set(JS::PropertyKey::Void());
    return true;
  }

  if (id.isAtom() && id.toAtom()->isIndex()) {
    id.set(JS::PropertyKey::Void());
    return true;
  }

  *nameOrSymbol = true;
  return true;
}

AttachDecision GetPropIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));
  if (cacheKind_ != CacheKind::GetProp) {
    MOZ_ASSERT_IF(cacheKind_ == CacheKind::GetPropSuper,
                  getSuperReceiverValueId().id() == 1);
    MOZ_ASSERT_IF(cacheKind_ != CacheKind::GetPropSuper,
                  getElemKeyValueId().id() == 1);
    writer.setInputOperandId(1);
  }
  if (cacheKind_ == CacheKind::GetElemSuper) {
    MOZ_ASSERT(getSuperReceiverValueId().id() == 2);
    writer.setInputOperandId(2);
  }

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  // |super.prop| getter calls use a |this| value that differs from lookup
  // object.
  ValOperandId receiverId = isSuper() ? getSuperReceiverValueId() : valId;

  if (val_.isObject()) {
    RootedObject obj(cx_, &val_.toObject());
    ObjOperandId objId = writer.guardToObject(valId);

    TRY_ATTACH(tryAttachTypedArrayElement(obj, objId));

    if (nameOrSymbol) {
      TRY_ATTACH(tryAttachObjectLength(obj, objId, id));
      TRY_ATTACH(tryAttachTypedArray(obj, objId, id));
      TRY_ATTACH(tryAttachDataView(obj, objId, id));
      TRY_ATTACH(tryAttachArrayBufferMaybeShared(obj, objId, id));
      TRY_ATTACH(tryAttachRegExp(obj, objId, id));
      TRY_ATTACH(tryAttachMap(obj, objId, id));
      TRY_ATTACH(tryAttachSet(obj, objId, id));
      TRY_ATTACH(tryAttachNative(obj, objId, id, receiverId));
      TRY_ATTACH(tryAttachModuleNamespace(obj, objId, id));
      TRY_ATTACH(tryAttachWindowProxy(obj, objId, id));
      TRY_ATTACH(tryAttachCrossCompartmentWrapper(obj, objId, id));
      TRY_ATTACH(
          tryAttachXrayCrossCompartmentWrapper(obj, objId, id, receiverId));
      TRY_ATTACH(tryAttachFunction(obj, objId, id));
      TRY_ATTACH(tryAttachArgumentsObjectIterator(obj, objId, id));
      TRY_ATTACH(tryAttachArgumentsObjectCallee(obj, objId, id));
      TRY_ATTACH(tryAttachProxy(obj, objId, id, receiverId));

      if (!isSuper() && mode_ == ICState::Mode::Megamorphic &&
          JSOp(*pc_) != JSOp::GetBoundName) {
        attachMegamorphicNativeSlotPermissive(objId, id);
        return AttachDecision::Attach;
      }

      trackAttached(IRGenerator::NotAttached);
      return AttachDecision::NoAction;
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
               cacheKind_ == CacheKind::GetElemSuper);

    TRY_ATTACH(tryAttachProxyElement(obj, objId));

    uint32_t index;
    Int32OperandId indexId;
    if (maybeGuardInt32Index(idVal_, getElemKeyValueId(), &index, &indexId)) {
      TRY_ATTACH(tryAttachDenseElement(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachDenseElementHole(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachSparseElement(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachArgumentsObjectArg(obj, objId, index, indexId));
      TRY_ATTACH(tryAttachArgumentsObjectArgHole(obj, objId, index, indexId));
      TRY_ATTACH(
          tryAttachGenericElement(obj, objId, index, indexId, receiverId));

      trackAttached(IRGenerator::NotAttached);
      return AttachDecision::NoAction;
    }

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  if (nameOrSymbol) {
    TRY_ATTACH(tryAttachPrimitive(valId, id));
    TRY_ATTACH(tryAttachStringLength(valId, id));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  if (idVal_.isInt32()) {
    ValOperandId indexId = getElemKeyValueId();
    TRY_ATTACH(tryAttachStringChar(valId, indexId));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

#ifdef DEBUG
// Any property lookups performed when trying to attach ICs must be pure, i.e.
// must use LookupPropertyPure() or similar functions. Pure lookups are
// guaranteed to never modify the prototype chain. This ensures that the holder
// object can always be found on the prototype chain.
static bool IsCacheableProtoChain(NativeObject* obj, NativeObject* holder) {
  while (obj != holder) {
    JSObject* proto = obj->staticPrototype();
    if (!proto || !proto->is<NativeObject>()) {
      return false;
    }
    obj = &proto->as<NativeObject>();
  }
  return true;
}
#endif

static bool IsCacheableGetPropSlot(NativeObject* obj, NativeObject* holder,
                                   PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  return prop.isDataProperty();
}

static NativeGetPropKind IsCacheableGetPropCall(NativeObject* obj,
                                                NativeObject* holder,
                                                PropertyInfo prop,
                                                jsbytecode* pc = nullptr) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (pc && JSOp(*pc) == JSOp::GetBoundName) {
    return NativeGetPropKind::None;
  }

  if (!prop.isAccessorProperty()) {
    return NativeGetPropKind::None;
  }

  JSObject* getterObject = holder->getGetter(prop);
  if (!getterObject || !getterObject->is<JSFunction>()) {
    return NativeGetPropKind::None;
  }

  JSFunction& getter = getterObject->as<JSFunction>();

  if (getter.isClassConstructor()) {
    return NativeGetPropKind::None;
  }

  // Scripted functions and natives with JIT entry can use the scripted path.
  if (getter.hasJitEntry()) {
    return NativeGetPropKind::ScriptedGetter;
  }

  MOZ_ASSERT(getter.isNativeWithoutJitEntry());
  return NativeGetPropKind::NativeGetter;
}

static bool CheckHasNoSuchOwnProperty(JSContext* cx, JSObject* obj, jsid id) {
  if (!obj->is<NativeObject>()) {
    return false;
  }
  // Don't handle objects with resolve hooks.
  if (ClassMayResolveId(cx->names(), obj->getClass(), id, obj)) {
    return false;
  }
  if (obj->as<NativeObject>().contains(cx, id)) {
    return false;
  }
  if (obj->is<TypedArrayObject>() && ToTypedArrayIndex(id).isSome()) {
    return false;
  }
  return true;
}

static bool CheckHasNoSuchProperty(JSContext* cx, JSObject* obj, jsid id) {
  JSObject* curObj = obj;
  do {
    if (!CheckHasNoSuchOwnProperty(cx, curObj, id)) {
      return false;
    }

    curObj = curObj->staticPrototype();
  } while (curObj);

  return true;
}

static bool IsCacheableNoProperty(JSContext* cx, NativeObject* obj,
                                  NativeObject* holder, jsid id,
                                  jsbytecode* pc) {
  MOZ_ASSERT(!holder);

  // If we're doing a name lookup, we have to throw a ReferenceError.
  if (JSOp(*pc) == JSOp::GetBoundName) {
    return false;
  }

  return CheckHasNoSuchProperty(cx, obj, id);
}

static NativeGetPropKind CanAttachNativeGetProp(JSContext* cx, JSObject* obj,
                                                PropertyKey id,
                                                NativeObject** holder,
                                                Maybe<PropertyInfo>* propInfo,
                                                jsbytecode* pc) {
  MOZ_ASSERT(id.isString() || id.isSymbol());
  MOZ_ASSERT(!*holder);

  // The lookup needs to be universally pure, otherwise we risk calling hooks
  // out of turn. We don't mind doing this even when purity isn't required,
  // because we only miss out on shape hashification, which is only a temporary
  // perf cost. The limits were arbitrarily set, anyways.
  NativeObject* baseHolder = nullptr;
  PropertyResult prop;
  if (!LookupPropertyPure(cx, obj, id, &baseHolder, &prop)) {
    return NativeGetPropKind::None;
  }
  auto* nobj = &obj->as<NativeObject>();

  if (prop.isNativeProperty()) {
    MOZ_ASSERT(baseHolder);
    *holder = baseHolder;
    *propInfo = mozilla::Some(prop.propertyInfo());

    if (IsCacheableGetPropSlot(nobj, *holder, propInfo->ref())) {
      return NativeGetPropKind::Slot;
    }

    return IsCacheableGetPropCall(nobj, *holder, propInfo->ref(), pc);
  }

  if (!prop.isFound()) {
    if (IsCacheableNoProperty(cx, nobj, *holder, id, pc)) {
      return NativeGetPropKind::Missing;
    }
  }

  return NativeGetPropKind::None;
}

static void GuardReceiverProto(CacheIRWriter& writer, NativeObject* obj,
                               ObjOperandId objId) {
  // Note: we guard on the actual prototype and not on the shape because this is
  // used for sparse elements where we expect shape changes.

  if (JSObject* proto = obj->staticPrototype()) {
    writer.guardProto(objId, proto);
  } else {
    writer.guardNullProto(objId);
  }
}

// Guard that a given object has same class and same OwnProperties (excluding
// dense elements and dynamic properties).
static void TestMatchingNativeReceiver(CacheIRWriter& writer, NativeObject* obj,
                                       ObjOperandId objId) {
  writer.guardShapeForOwnProperties(objId, obj->shape());
}

// Similar to |TestMatchingNativeReceiver|, but specialized for ProxyObject.
static void TestMatchingProxyReceiver(CacheIRWriter& writer, ProxyObject* obj,
                                      ObjOperandId objId) {
  writer.guardShapeForClass(objId, obj->shape());
}

static void GeneratePrototypeGuards(CacheIRWriter& writer, JSObject* obj,
                                    NativeObject* holder, ObjOperandId objId) {
  // Assuming target property is on |holder|, generate appropriate guards to
  // ensure |holder| is still on the prototype chain of |obj| and we haven't
  // introduced any shadowing definitions.
  //
  // For each item in the proto chain before holder, we must ensure that
  // [[GetPrototypeOf]] still has the expected result, and that
  // [[GetOwnProperty]] has no definition of the target property.
  //
  //
  // [SMDOC] Shape Teleporting Optimization
  // --------------------------------------
  //
  // Starting with the assumption (and guideline to developers) that mutating
  // prototypes is an uncommon and fair-to-penalize operation we move cost
  // from the access side to the mutation side.
  //
  // Consider the following proto chain, with B defining a property 'x':
  //
  //      D  ->  C  ->  B{x: 3}  ->  A  -> null
  //
  // When accessing |D.x| we refer to D as the "receiver", and B as the
  // "holder". To optimize this access we need to ensure that neither D nor C
  // has since defined a shadowing property 'x'. Since C is a prototype that
  // we assume is rarely mutated we would like to avoid checking each time if
  // new properties are added. To do this we require that whenever C starts
  // shadowing a property on its proto chain, we invalidate (and opt out of) the
  // teleporting optimization by setting the InvalidatedTeleporting flag on the
  // object we're shadowing, triggering a shape change of that object. As a
  // result, checking the shape of D and B is sufficient. Note that we do not
  // care if the shape or properties of A change since the lookup of 'x' will
  // stop at B.
  //
  // The second condition we must verify is that the prototype chain was not
  // mutated. The same mechanism as above is used. When the prototype link is
  // changed, we generate a new shape for the object. If the object whose
  // link we are mutating is itself a prototype, we regenerate shapes down
  // the chain by setting the InvalidatedTeleporting flag on them. This means
  // the same two shape checks as above are sufficient.
  //
  // Once the InvalidatedTeleporting flag is set, it means the shape will no
  // longer be changed by ReshapeForProtoMutation and ReshapeForShadowedProp.
  // In this case we can no longer apply the optimization.
  //
  // See:
  //  - ReshapeForProtoMutation
  //  - ReshapeForShadowedProp

  MOZ_ASSERT(holder);
  MOZ_ASSERT(obj != holder);

  // Receiver guards (see TestMatchingReceiver) ensure the receiver's proto is
  // unchanged so peel off the receiver.
  JSObject* pobj = obj->staticPrototype();
  MOZ_ASSERT(pobj->isUsedAsPrototype());

  // If teleporting is supported for this holder, we are done.
  if (!holder->hasInvalidatedTeleporting()) {
    return;
  }

  // If already at the holder, no further proto checks are needed.
  if (pobj == holder) {
    return;
  }

  // Synchronize pobj and protoId.
  MOZ_ASSERT(pobj == obj->staticPrototype());
  ObjOperandId protoId = writer.loadProto(objId);

  // Shape guard each prototype object between receiver and holder. This guards
  // against both proto changes and shadowing properties.
  while (pobj != holder) {
    writer.guardShape(protoId, pobj->shape());

    pobj = pobj->staticPrototype();
    protoId = writer.loadProto(protoId);
  }
}

static void GeneratePrototypeHoleGuards(CacheIRWriter& writer,
                                        NativeObject* obj, ObjOperandId objId,
                                        bool alwaysGuardFirstProto) {
  if (alwaysGuardFirstProto) {
    GuardReceiverProto(writer, obj, objId);
  }

  JSObject* pobj = obj->staticPrototype();
  while (pobj) {
    ObjOperandId protoId = writer.loadObject(pobj);

    // Make sure the shape matches, to ensure the proto is unchanged and to
    // avoid non-dense elements or anything else that is being checked by
    // CanAttachDenseElementHole.
    MOZ_ASSERT(pobj->is<NativeObject>());
    writer.guardShape(protoId, pobj->shape());

    // Also make sure there are no dense elements.
    writer.guardNoDenseElements(protoId);

    pobj = pobj->staticPrototype();
  }
}

// Similar to |TestMatchingReceiver|, but for the holder object (when it
// differs from the receiver). The holder may also be the expando of the
// receiver if it exists.
static void TestMatchingHolder(CacheIRWriter& writer, NativeObject* obj,
                               ObjOperandId objId) {
  // The GeneratePrototypeGuards + TestMatchingHolder checks only support
  // prototype chains composed of NativeObject (excluding the receiver
  // itself).
  writer.guardShapeForOwnProperties(objId, obj->shape());
}

enum class IsCrossCompartment { No, Yes };

// Emit a shape guard for all objects on the proto chain. This does NOT include
// the receiver; callers must ensure the receiver's proto is the first proto by
// either emitting a shape guard or a prototype guard for |objId|.
//
// Note: this relies on shape implying proto.
template <IsCrossCompartment MaybeCrossCompartment = IsCrossCompartment::No>
static void ShapeGuardProtoChain(CacheIRWriter& writer, NativeObject* obj,
                                 ObjOperandId objId) {
  uint32_t depth = 0;
  static const uint32_t MAX_CACHED_LOADS = 4;
  ObjOperandId receiverObjId = objId;

  while (true) {
    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      return;
    }

    obj = &proto->as<NativeObject>();

    // After guarding the shape of an object, we can safely bake that
    // object's proto into the stub data. Compared to LoadProto, this
    // takes one load instead of three (object -> shape -> baseshape
    // -> proto). We cap the depth to avoid bloating the size of the
    // stub data. To avoid compartment mismatch, we skip this optimization
    // in the cross-compartment case.
    if (depth < MAX_CACHED_LOADS &&
        MaybeCrossCompartment == IsCrossCompartment::No) {
      objId = writer.loadProtoObject(obj, receiverObjId);
    } else {
      objId = writer.loadProto(objId);
    }
    depth++;

    writer.guardShape(objId, obj->shape());
  }
}

// For cross compartment guards we shape-guard the prototype chain to avoid
// referencing the holder object.
//
// This peels off the first layer because it's guarded against obj == holder.
//
// Returns the holder's OperandId.
static ObjOperandId ShapeGuardProtoChainForCrossCompartmentHolder(
    CacheIRWriter& writer, NativeObject* obj, ObjOperandId objId,
    NativeObject* holder) {
  MOZ_ASSERT(obj != holder);
  MOZ_ASSERT(holder);
  while (true) {
    MOZ_ASSERT(obj->staticPrototype());
    obj = &obj->staticPrototype()->as<NativeObject>();

    objId = writer.loadProto(objId);
    if (obj == holder) {
      TestMatchingHolder(writer, obj, objId);
      return objId;
    }
    writer.guardShapeForOwnProperties(objId, obj->shape());
  }
}

// Emit guards for reading a data property on |holder|. Returns the holder's
// OperandId.
template <IsCrossCompartment MaybeCrossCompartment = IsCrossCompartment::No>
static ObjOperandId EmitReadSlotGuard(CacheIRWriter& writer, NativeObject* obj,
                                      NativeObject* holder,
                                      ObjOperandId objId) {
  MOZ_ASSERT(holder);
  TestMatchingNativeReceiver(writer, obj, objId);

  if (obj == holder) {
    return objId;
  }

  if (MaybeCrossCompartment == IsCrossCompartment::Yes) {
    // Guard proto chain integrity.
    // We use a variant of guards that avoid baking in any cross-compartment
    // object pointers.
    return ShapeGuardProtoChainForCrossCompartmentHolder(writer, obj, objId,
                                                         holder);
  }

  // Guard proto chain integrity.
  GeneratePrototypeGuards(writer, obj, holder, objId);

  // Guard on the holder's shape.
  ObjOperandId holderId = writer.loadObject(holder);
  TestMatchingHolder(writer, holder, holderId);
  return holderId;
}

template <IsCrossCompartment MaybeCrossCompartment = IsCrossCompartment::No>
static void EmitMissingPropGuard(CacheIRWriter& writer, NativeObject* obj,
                                 ObjOperandId objId) {
  TestMatchingNativeReceiver(writer, obj, objId);

  // The property does not exist. Guard on everything in the prototype
  // chain. This is guaranteed to see only Native objects because of
  // CanAttachNativeGetProp().
  ShapeGuardProtoChain<MaybeCrossCompartment>(writer, obj, objId);
}

template <IsCrossCompartment MaybeCrossCompartment = IsCrossCompartment::No>
static void EmitReadSlotResult(CacheIRWriter& writer, NativeObject* obj,
                               NativeObject* holder, PropertyInfo prop,
                               ObjOperandId objId) {
  MOZ_ASSERT(holder);

  ObjOperandId holderId =
      EmitReadSlotGuard<MaybeCrossCompartment>(writer, obj, holder, objId);

  MOZ_ASSERT(holderId.valid());
  EmitLoadSlotResult(writer, holderId, holder, prop);
}

template <IsCrossCompartment MaybeCrossCompartment = IsCrossCompartment::No>
static void EmitMissingPropResult(CacheIRWriter& writer, NativeObject* obj,
                                  ObjOperandId objId) {
  EmitMissingPropGuard<MaybeCrossCompartment>(writer, obj, objId);
  writer.loadUndefinedResult();
}

static ValOperandId EmitLoadSlot(CacheIRWriter& writer, NativeObject* holder,
                                 ObjOperandId holderId, uint32_t slot) {
  if (holder->isFixedSlot(slot)) {
    return writer.loadFixedSlot(holderId,
                                NativeObject::getFixedSlotOffset(slot));
  }
  size_t dynamicSlotIndex = holder->dynamicSlotIndex(slot);
  return writer.loadDynamicSlot(holderId, dynamicSlotIndex);
}

void IRGenerator::emitCallGetterResultNoGuards(NativeGetPropKind kind,
                                               NativeObject* obj,
                                               NativeObject* holder,
                                               PropertyInfo prop,
                                               ValOperandId receiverId) {
  MOZ_ASSERT(IsCacheableGetPropCall(obj, holder, prop) == kind);

  JSFunction* target = &holder->getGetter(prop)->as<JSFunction>();
  bool sameRealm = cx_->realm() == target->realm();

  switch (kind) {
    case NativeGetPropKind::NativeGetter: {
      writer.callNativeGetterResult(receiverId, target, sameRealm);
      writer.returnFromIC();
      break;
    }
    case NativeGetPropKind::ScriptedGetter: {
      writer.callScriptedGetterResult(receiverId, target, sameRealm);
      writer.returnFromIC();
      break;
    }
    default:
      // CanAttachNativeGetProp guarantees that the getter is either a native or
      // a scripted function.
      MOZ_ASSERT_UNREACHABLE("Can't attach getter");
      break;
  }
}

static bool FunctionHasStableBaseScript(JSFunction* fun) {
  // When guarding a callee, guarding on the JSFunction* is most efficient,
  // but doesn't work well for lambda clones (multiple functions with the
  // same BaseScript). We can instead guard on the BaseScript itself.
  if (!fun->hasBaseScript()) {
    return false;
  }
  // Self-hosted functions are more complicated: top-level functions can be
  // relazified using SelfHostedLazyScript and this means they don't have a
  // stable BaseScript pointer. These functions are never lambda clones, though,
  // so we can just always guard on the JSFunction*. Self-hosted lambdas are
  // never relazified so there we use the normal heuristics.
  if (fun->isSelfHostedBuiltin() && !fun->isLambda()) {
    return false;
  }
  return true;
}

// See the SMDOC comment in vm/GetterSetter.h for more info on Getter/Setter
// properties
void IRGenerator::emitGuardGetterSetterSlot(NativeObject* holder,
                                            PropertyInfo prop,
                                            ObjOperandId holderId,
                                            AccessorKind kind,
                                            bool holderIsConstant) {
  // If the holder is guaranteed to be the same object, and it never had a
  // slot holding a GetterSetter mutated or deleted, its Shape will change when
  // that does happen so we don't need to guard on the GetterSetter.
  if (holderIsConstant && !holder->hadGetterSetterChange()) {
    return;
  }

  size_t slot = prop.slot();

  // For the same reasons as emitCalleeGuard, we guard on the BaseScript
  // instead of the GetterSetter if the callee is scripted and this isn't
  // the first IC stub.
  if (!isFirstStub_) {
    bool isGetter = kind == AccessorKind::Getter;
    JSObject* accessor =
        isGetter ? holder->getGetter(prop) : holder->getSetter(prop);
    JSFunction* fun = &accessor->as<JSFunction>();
    if (FunctionHasStableBaseScript(fun)) {
      bool needsClassGuard = holder->hasNonFunctionAccessor();
      ValOperandId getterSetterId =
          EmitLoadSlot(writer, holder, holderId, slot);
      ObjOperandId functionId = writer.loadGetterSetterFunction(
          getterSetterId, isGetter, needsClassGuard);
      writer.saveScriptedGetterSetterCallee(functionId);
      writer.guardFunctionScript(functionId, fun->baseScript());
      return;
    }
  }

  Value slotVal = holder->getSlot(slot);
  MOZ_ASSERT(slotVal.isPrivateGCThing());

  if (holder->isFixedSlot(slot)) {
    size_t offset = NativeObject::getFixedSlotOffset(slot);
    writer.guardFixedSlotValue(holderId, offset, slotVal);
  } else {
    size_t offset = holder->dynamicSlotIndex(slot) * sizeof(Value);
    writer.guardDynamicSlotValue(holderId, offset, slotVal);
  }
}

void GetPropIRGenerator::emitCallGetterResultGuards(NativeObject* obj,
                                                    NativeObject* holder,
                                                    HandleId id,
                                                    PropertyInfo prop,
                                                    ObjOperandId objId) {
  // Use the megamorphic guard if we're in megamorphic mode, except if |obj|
  // is a Window as GuardHasGetterSetter doesn't support this yet (Window may
  // require outerizing).

  MOZ_ASSERT(holder->containsPure(id, prop));

  if (mode_ == ICState::Mode::Specialized || IsWindow(obj)) {
    TestMatchingNativeReceiver(writer, obj, objId);

    if (obj != holder) {
      GeneratePrototypeGuards(writer, obj, holder, objId);

      // Guard on the holder's shape.
      ObjOperandId holderId = writer.loadObject(holder);
      TestMatchingHolder(writer, holder, holderId);

      emitGuardGetterSetterSlot(holder, prop, holderId, AccessorKind::Getter,
                                /* holderIsConstant = */ true);
    } else {
      emitGuardGetterSetterSlot(holder, prop, objId, AccessorKind::Getter);
    }
  } else {
    Value val = holder->getSlot(prop.slot());
    MOZ_ASSERT(val.isPrivateGCThing());
    MOZ_ASSERT(val.toGCThing()->is<GetterSetter>());
    writer.guardHasGetterSetter(objId, id, val);
  }
}

void GetPropIRGenerator::emitCallGetterResult(NativeGetPropKind kind,
                                              NativeObject* obj,
                                              NativeObject* holder, HandleId id,
                                              PropertyInfo prop,
                                              ObjOperandId objId,
                                              ValOperandId receiverId) {
  emitCallGetterResultGuards(obj, holder, id, prop, objId);
  emitCallGetterResultNoGuards(kind, obj, holder, prop, receiverId);
}

static bool CanAttachDOMCall(JSContext* cx, JSJitInfo::OpType type,
                             JSObject* obj, JSFunction* fun,
                             ICState::Mode mode) {
  MOZ_ASSERT(type == JSJitInfo::Getter || type == JSJitInfo::Setter ||
             type == JSJitInfo::Method);

  if (mode != ICState::Mode::Specialized) {
    return false;
  }

  if (!fun->hasJitInfo()) {
    return false;
  }

  if (cx->realm() != fun->realm()) {
    return false;
  }

  const JSJitInfo* jitInfo = fun->jitInfo();
  if (jitInfo->type() != type) {
    return false;
  }

  MOZ_ASSERT_IF(IsWindow(obj), !jitInfo->needsOuterizedThisObject());

  const JSClass* clasp = obj->getClass();
  if (!clasp->isDOMClass()) {
    return false;
  }

  if (type != JSJitInfo::Method && clasp->isProxyObject()) {
    return false;
  }

  // Ion codegen expects DOM_OBJECT_SLOT to be a fixed slot in LoadDOMPrivate.
  // It can be a dynamic slot if we transplanted this reflector object with a
  // proxy.
  if (obj->is<NativeObject>() && obj->as<NativeObject>().numFixedSlots() == 0) {
    return false;
  }

  // Tell the analysis the |DOMInstanceClassHasProtoAtDepth| hook can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  DOMInstanceClassHasProtoAtDepth instanceChecker =
      cx->runtime()->DOMcallbacks->instanceClassMatchesProto;
  return instanceChecker(clasp, jitInfo->protoID, jitInfo->depth);
}

static bool CanAttachDOMGetterSetter(JSContext* cx, JSJitInfo::OpType type,
                                     NativeObject* obj, NativeObject* holder,
                                     PropertyInfo prop, ICState::Mode mode) {
  MOZ_ASSERT(type == JSJitInfo::Getter || type == JSJitInfo::Setter);

  JSObject* accessor = type == JSJitInfo::Getter ? holder->getGetter(prop)
                                                 : holder->getSetter(prop);
  JSFunction* fun = &accessor->as<JSFunction>();

  return CanAttachDOMCall(cx, type, obj, fun, mode);
}

void IRGenerator::emitCallDOMGetterResultNoGuards(NativeObject* holder,
                                                  PropertyInfo prop,
                                                  ObjOperandId objId) {
  JSFunction* getter = &holder->getGetter(prop)->as<JSFunction>();
  writer.callDOMGetterResult(objId, getter->jitInfo());
  writer.returnFromIC();
}

void GetPropIRGenerator::emitCallDOMGetterResult(NativeObject* obj,
                                                 NativeObject* holder,
                                                 HandleId id, PropertyInfo prop,
                                                 ObjOperandId objId) {
  // Note: this relies on emitCallGetterResultGuards emitting a shape guard
  // for specialized stubs.
  // The shape guard ensures the receiver's Class is valid for this DOM getter.
  emitCallGetterResultGuards(obj, holder, id, prop, objId);
  emitCallDOMGetterResultNoGuards(holder, prop, objId);
}

void GetPropIRGenerator::attachMegamorphicNativeSlot(ObjOperandId objId,
                                                     jsid id) {
  MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);

  // We don't support GetBoundName because environment objects have
  // lookupProperty hooks and GetBoundName is usually not megamorphic.
  MOZ_ASSERT(JSOp(*pc_) != JSOp::GetBoundName);

  if (cacheKind_ == CacheKind::GetProp ||
      cacheKind_ == CacheKind::GetPropSuper) {
    writer.megamorphicLoadSlotResult(objId, id);
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
               cacheKind_ == CacheKind::GetElemSuper);
    writer.megamorphicLoadSlotByValueResult(objId, getElemKeyValueId());
  }
  writer.returnFromIC();

  trackAttached("GetProp.MegamorphicNativeSlot");
}

void GetPropIRGenerator::attachMegamorphicNativeSlotPermissive(
    ObjOperandId objId, jsid id) {
  MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);

  // We don't support GetBoundName because environment objects have
  // lookupProperty hooks and GetBoundName is usually not megamorphic.
  MOZ_ASSERT(JSOp(*pc_) != JSOp::GetBoundName);
  // It is not worth the complexity to support super here because we'd have
  // to plumb the receiver through everywhere, so we just skip it.
  MOZ_ASSERT(!isSuper());

  if (cacheKind_ == CacheKind::GetProp) {
    writer.megamorphicLoadSlotPermissiveResult(objId, id);
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
    writer.megamorphicLoadSlotByValuePermissiveResult(objId,
                                                      getElemKeyValueId());
  }
  writer.returnFromIC();

  trackAttached("GetProp.MegamorphicNativeSlotPermissive");
}

AttachDecision GetPropIRGenerator::tryAttachNative(HandleObject obj,
                                                   ObjOperandId objId,
                                                   HandleId id,
                                                   ValOperandId receiverId) {
  Maybe<PropertyInfo> prop;
  NativeObject* holder = nullptr;

  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  switch (kind) {
    case NativeGetPropKind::None:
      return AttachDecision::NoAction;
    case NativeGetPropKind::Missing:
    case NativeGetPropKind::Slot: {
      auto* nobj = &obj->as<NativeObject>();

      if (mode_ == ICState::Mode::Megamorphic &&
          JSOp(*pc_) != JSOp::GetBoundName) {
        attachMegamorphicNativeSlot(objId, id);
        return AttachDecision::Attach;
      }

      maybeEmitIdGuard(id);
      if (kind == NativeGetPropKind::Slot) {
        EmitReadSlotResult(writer, nobj, holder, *prop, objId);
        writer.returnFromIC();
        trackAttached("GetProp.NativeSlot");
      } else {
        EmitMissingPropResult(writer, nobj, objId);
        writer.returnFromIC();
        trackAttached("GetProp.Missing");
      }
      return AttachDecision::Attach;
    }
    case NativeGetPropKind::ScriptedGetter:
    case NativeGetPropKind::NativeGetter: {
      auto* nobj = &obj->as<NativeObject>();
      MOZ_ASSERT(!IsWindow(nobj));

      // If we're in megamorphic mode, we assume that a specialized
      // getter call is just going to end up failing later, so we let this
      // get handled further down the chain by
      // attachMegamorphicNativeSlotPermissive
      if (!isSuper() && mode_ == ICState::Mode::Megamorphic) {
        return AttachDecision::NoAction;
      }

      maybeEmitIdGuard(id);

      if (!isSuper() && CanAttachDOMGetterSetter(cx_, JSJitInfo::Getter, nobj,
                                                 holder, *prop, mode_)) {
        emitCallDOMGetterResult(nobj, holder, id, *prop, objId);

        trackAttached("GetProp.DOMGetter");
        return AttachDecision::Attach;
      }

      emitCallGetterResult(kind, nobj, holder, id, *prop, objId, receiverId);

      trackAttached("GetProp.NativeGetter");
      return AttachDecision::Attach;
    }
  }

  MOZ_CRASH("Bad NativeGetPropKind");
}

// Returns whether obj is a WindowProxy wrapping the script's global.
static bool IsWindowProxyForScriptGlobal(JSScript* script, JSObject* obj) {
  if (!IsWindowProxy(obj)) {
    return false;
  }

  MOZ_ASSERT(obj->getClass() ==
             script->runtimeFromMainThread()->maybeWindowProxyClass());

  JSObject* window = ToWindowIfWindowProxy(obj);

  // Ion relies on the WindowProxy's group changing (and the group getting
  // marked as having unknown properties) on navigation. If we ever stop
  // transplanting same-compartment WindowProxies, this assert will fail and we
  // need to fix that code.
  MOZ_ASSERT(window == &obj->nonCCWGlobal());

  // This must be a WindowProxy for a global in this compartment. Else it would
  // be a cross-compartment wrapper and IsWindowProxy returns false for
  // those.
  MOZ_ASSERT(script->compartment() == obj->compartment());

  // Only optimize lookups on the WindowProxy for the current global. Other
  // WindowProxies in the compartment may require security checks (based on
  // mutable document.domain). See bug 1516775.
  return window == &script->global();
}

// Guards objId is a WindowProxy for windowObj. Returns the window's operand id.
static ObjOperandId GuardAndLoadWindowProxyWindow(CacheIRWriter& writer,
                                                  ObjOperandId objId,
                                                  GlobalObject* windowObj) {
  writer.guardClass(objId, GuardClassKind::WindowProxy);
  ObjOperandId windowObjId = writer.loadWrapperTarget(objId,
                                                      /*fallible = */ false);
  writer.guardSpecificObject(windowObjId, windowObj);
  return windowObjId;
}

// Whether a getter/setter on the global should have the WindowProxy as |this|
// value instead of the Window (the global object). This always returns true for
// scripted functions.
static bool GetterNeedsWindowProxyThis(NativeObject* holder,
                                       PropertyInfo prop) {
  JSFunction* callee = &holder->getGetter(prop)->as<JSFunction>();
  return !callee->hasJitInfo() || callee->jitInfo()->needsOuterizedThisObject();
}
static bool SetterNeedsWindowProxyThis(NativeObject* holder,
                                       PropertyInfo prop) {
  JSFunction* callee = &holder->getSetter(prop)->as<JSFunction>();
  return !callee->hasJitInfo() || callee->jitInfo()->needsOuterizedThisObject();
}

AttachDecision GetPropIRGenerator::tryAttachWindowProxy(HandleObject obj,
                                                        ObjOperandId objId,
                                                        HandleId id) {
  // Attach a stub when the receiver is a WindowProxy and we can do the lookup
  // on the Window (the global object).

  if (!IsWindowProxyForScriptGlobal(script_, obj)) {
    return AttachDecision::NoAction;
  }

  // If we're megamorphic prefer a generic proxy stub that handles a lot more
  // cases.
  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  // Now try to do the lookup on the Window (the current global).
  GlobalObject* windowObj = cx_->global();
  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, windowObj, id, &holder, &prop, pc_);
  switch (kind) {
    case NativeGetPropKind::None:
      return AttachDecision::NoAction;

    case NativeGetPropKind::Slot: {
      maybeEmitIdGuard(id);
      ObjOperandId windowObjId =
          GuardAndLoadWindowProxyWindow(writer, objId, windowObj);
      EmitReadSlotResult(writer, windowObj, holder, *prop, windowObjId);
      writer.returnFromIC();

      trackAttached("GetProp.WindowProxySlot");
      return AttachDecision::Attach;
    }

    case NativeGetPropKind::Missing: {
      maybeEmitIdGuard(id);
      ObjOperandId windowObjId =
          GuardAndLoadWindowProxyWindow(writer, objId, windowObj);
      EmitMissingPropResult(writer, windowObj, windowObjId);
      writer.returnFromIC();

      trackAttached("GetProp.WindowProxyMissing");
      return AttachDecision::Attach;
    }

    case NativeGetPropKind::NativeGetter:
    case NativeGetPropKind::ScriptedGetter: {
      // If a |super| access, it is not worth the complexity to attach an IC.
      if (isSuper()) {
        return AttachDecision::NoAction;
      }

      bool needsWindowProxy = GetterNeedsWindowProxyThis(holder, *prop);

      // Guard the incoming object is a WindowProxy and inline a getter call
      // based on the Window object.
      maybeEmitIdGuard(id);
      ObjOperandId windowObjId =
          GuardAndLoadWindowProxyWindow(writer, objId, windowObj);

      if (CanAttachDOMGetterSetter(cx_, JSJitInfo::Getter, windowObj, holder,
                                   *prop, mode_)) {
        MOZ_ASSERT(!needsWindowProxy);
        emitCallDOMGetterResult(windowObj, holder, id, *prop, windowObjId);
        trackAttached("GetProp.WindowProxyDOMGetter");
      } else {
        ValOperandId receiverId =
            writer.boxObject(needsWindowProxy ? objId : windowObjId);
        emitCallGetterResult(kind, windowObj, holder, id, *prop, windowObjId,
                             receiverId);
        trackAttached("GetProp.WindowProxyGetter");
      }

      return AttachDecision::Attach;
    }
  }

  MOZ_CRASH("Unreachable");
}

AttachDecision GetPropIRGenerator::tryAttachCrossCompartmentWrapper(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  // We can only optimize this very wrapper-handler, because others might
  // have a security policy.
  if (!IsWrapper(obj) ||
      Wrapper::wrapperHandler(obj) != &CrossCompartmentWrapper::singleton) {
    return AttachDecision::NoAction;
  }

  // If we're megamorphic prefer a generic proxy stub that handles a lot more
  // cases.
  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  RootedObject unwrapped(cx_, Wrapper::wrappedObject(obj));
  MOZ_ASSERT(unwrapped == UnwrapOneCheckedStatic(obj));
  MOZ_ASSERT(!IsCrossCompartmentWrapper(unwrapped),
             "CCWs must not wrap other CCWs");

  // If we allowed different zones we would have to wrap strings.
  if (unwrapped->compartment()->zone() != cx_->compartment()->zone()) {
    return AttachDecision::NoAction;
  }

  // Take the unwrapped object's global, and wrap in a
  // this-compartment wrapper. This is what will be stored in the IC
  // keep the compartment alive.
  RootedObject wrappedTargetGlobal(cx_, &unwrapped->nonCCWGlobal());
  if (!cx_->compartment()->wrap(cx_, &wrappedTargetGlobal)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;

  // Enter realm of target to prevent failing compartment assertions when doing
  // the lookup.
  {
    AutoRealm ar(cx_, unwrapped);

    NativeGetPropKind kind =
        CanAttachNativeGetProp(cx_, unwrapped, id, &holder, &prop, pc_);
    if (kind != NativeGetPropKind::Slot && kind != NativeGetPropKind::Missing) {
      return AttachDecision::NoAction;
    }
  }
  auto* unwrappedNative = &unwrapped->as<NativeObject>();

  maybeEmitIdGuard(id);
  writer.guardIsProxy(objId);
  writer.guardHasProxyHandler(objId, Wrapper::wrapperHandler(obj));

  // Load the object wrapped by the CCW
  ObjOperandId wrapperTargetId =
      writer.loadWrapperTarget(objId, /*fallible = */ false);

  // If the compartment of the wrapped object is different we should fail.
  writer.guardCompartment(wrapperTargetId, wrappedTargetGlobal,
                          unwrappedNative->compartment());

  ObjOperandId unwrappedId = wrapperTargetId;
  if (holder) {
    EmitReadSlotResult<IsCrossCompartment::Yes>(writer, unwrappedNative, holder,
                                                *prop, unwrappedId);
    writer.wrapResult();
    writer.returnFromIC();
    trackAttached("GetProp.CCWSlot");
  } else {
    EmitMissingPropResult<IsCrossCompartment::Yes>(writer, unwrappedNative,
                                                   unwrappedId);
    writer.returnFromIC();
    trackAttached("GetProp.CCWMissing");
  }
  return AttachDecision::Attach;
}

static JSObject* NewWrapperWithObjectShape(JSContext* cx,
                                           Handle<NativeObject*> obj);

static bool GetXrayExpandoShapeWrapper(JSContext* cx, HandleObject xray,
                                       MutableHandleObject wrapper) {
  Value v = GetProxyReservedSlot(xray, GetXrayJitInfo()->xrayHolderSlot);
  if (v.isObject()) {
    NativeObject* holder = &v.toObject().as<NativeObject>();
    v = holder->getFixedSlot(GetXrayJitInfo()->holderExpandoSlot);
    if (v.isObject()) {
      Rooted<NativeObject*> expando(
          cx, &UncheckedUnwrap(&v.toObject())->as<NativeObject>());
      wrapper.set(NewWrapperWithObjectShape(cx, expando));
      return wrapper != nullptr;
    }
  }
  wrapper.set(nullptr);
  return true;
}

AttachDecision GetPropIRGenerator::tryAttachXrayCrossCompartmentWrapper(
    HandleObject obj, ObjOperandId objId, HandleId id,
    ValOperandId receiverId) {
  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  JS::XrayJitInfo* info = GetXrayJitInfo();
  if (!info || !info->isCrossCompartmentXray(GetProxyHandler(obj))) {
    return AttachDecision::NoAction;
  }

  if (!info->compartmentHasExclusiveExpandos(obj)) {
    return AttachDecision::NoAction;
  }

  RootedObject target(cx_, UncheckedUnwrap(obj));

  RootedObject expandoShapeWrapper(cx_);
  if (!GetXrayExpandoShapeWrapper(cx_, obj, &expandoShapeWrapper)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Look for a getter we can call on the xray or its prototype chain.
  Rooted<Maybe<PropertyDescriptor>> desc(cx_);
  RootedObject holder(cx_, obj);
  RootedObjectVector prototypes(cx_);
  RootedObjectVector prototypeExpandoShapeWrappers(cx_);
  while (true) {
    if (!GetOwnPropertyDescriptor(cx_, holder, id, &desc)) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }
    if (desc.isSome()) {
      break;
    }
    if (!GetPrototype(cx_, holder, &holder)) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }
    if (!holder || !holder->is<ProxyObject>() ||
        !info->isCrossCompartmentXray(GetProxyHandler(holder))) {
      return AttachDecision::NoAction;
    }
    RootedObject prototypeExpandoShapeWrapper(cx_);
    if (!GetXrayExpandoShapeWrapper(cx_, holder,
                                    &prototypeExpandoShapeWrapper) ||
        !prototypes.append(holder) ||
        !prototypeExpandoShapeWrappers.append(prototypeExpandoShapeWrapper)) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }
  }
  if (!desc->isAccessorDescriptor()) {
    return AttachDecision::NoAction;
  }

  RootedObject getter(cx_, desc->getter());
  if (!getter || !getter->is<JSFunction>() ||
      !getter->as<JSFunction>().isNativeWithoutJitEntry()) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  writer.guardIsProxy(objId);
  writer.guardHasProxyHandler(objId, GetProxyHandler(obj));

  // Load the object wrapped by the CCW
  ObjOperandId wrapperTargetId =
      writer.loadWrapperTarget(objId, /*fallible = */ false);

  // Test the wrapped object's class. The properties held by xrays or their
  // prototypes will be invariant for objects of a given class, except for
  // changes due to xray expandos or xray prototype mutations.
  writer.guardAnyClass(wrapperTargetId, target->getClass());

  // Make sure the expandos on the xray and its prototype chain match up with
  // what we expect. The expando shape needs to be consistent, to ensure it
  // has not had any shadowing properties added, and the expando cannot have
  // any custom prototype (xray prototypes are stable otherwise).
  //
  // We can only do this for xrays with exclusive access to their expandos
  // (as we checked earlier), which store a pointer to their expando
  // directly. Xrays in other compartments may share their expandos with each
  // other and a VM call is needed just to find the expando.
  if (expandoShapeWrapper) {
    writer.guardXrayExpandoShapeAndDefaultProto(objId, expandoShapeWrapper);
  } else {
    writer.guardXrayNoExpando(objId);
  }
  for (size_t i = 0; i < prototypes.length(); i++) {
    JSObject* proto = prototypes[i];
    ObjOperandId protoId = writer.loadObject(proto);
    if (JSObject* protoShapeWrapper = prototypeExpandoShapeWrappers[i]) {
      writer.guardXrayExpandoShapeAndDefaultProto(protoId, protoShapeWrapper);
    } else {
      writer.guardXrayNoExpando(protoId);
    }
  }

  bool sameRealm = cx_->realm() == getter->as<JSFunction>().realm();
  writer.callNativeGetterResult(receiverId, &getter->as<JSFunction>(),
                                sameRealm);
  writer.returnFromIC();

  trackAttached("GetProp.XrayCCW");
  return AttachDecision::Attach;
}

#ifdef JS_PUNBOX64
AttachDecision GetPropIRGenerator::tryAttachScriptedProxy(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id) {
  if (cacheKind_ != CacheKind::GetProp && cacheKind_ != CacheKind::GetElem) {
    return AttachDecision::NoAction;
  }
  if (cacheKind_ == CacheKind::GetElem) {
    if (!idVal_.isString() && !idVal_.isInt32() && !idVal_.isSymbol()) {
      return AttachDecision::NoAction;
    }
  }

  JSObject* handlerObj = ScriptedProxyHandler::handlerObject(obj);
  if (!handlerObj) {
    return AttachDecision::NoAction;
  }

  NativeObject* trapHolder = nullptr;
  Maybe<PropertyInfo> trapProp;
  // We call with pc_ even though that's not the actual corresponding pc. It
  // should, however, be fine, because it's just used to check if this is a
  // GetBoundName, which it's not.
  NativeGetPropKind trapKind = CanAttachNativeGetProp(
      cx_, handlerObj, NameToId(cx_->names().get), &trapHolder, &trapProp, pc_);

  if (trapKind != NativeGetPropKind::Missing &&
      trapKind != NativeGetPropKind::Slot) {
    return AttachDecision::NoAction;
  }

  if (trapKind != NativeGetPropKind::Missing) {
    uint32_t trapSlot = trapProp->slot();
    const Value& trapVal = trapHolder->getSlot(trapSlot);
    if (!trapVal.isObject()) {
      return AttachDecision::NoAction;
    }

    JSObject* trapObj = &trapVal.toObject();
    if (!trapObj->is<JSFunction>()) {
      return AttachDecision::NoAction;
    }

    JSFunction* trapFn = &trapObj->as<JSFunction>();
    if (trapFn->isClassConstructor()) {
      return AttachDecision::NoAction;
    }

    if (!trapFn->hasJitEntry()) {
      return AttachDecision::NoAction;
    }

    if (cx_->realm() != trapFn->realm()) {
      return AttachDecision::NoAction;
    }
  }

  NativeObject* nHandlerObj = &handlerObj->as<NativeObject>();
  JSObject* targetObj = obj->target();
  MOZ_ASSERT(targetObj, "Guaranteed by the scripted Proxy constructor");

  // We just require that the target is a NativeObject to make our lives
  // easier. There's too much nonsense we might have to handle otherwise and
  // we're not set up to recursively call GetPropIRGenerator::tryAttachStub
  // for the target object.
  if (!targetObj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  writer.guardIsProxy(objId);
  writer.guardHasProxyHandler(objId, &ScriptedProxyHandler::singleton);
  ObjOperandId handlerObjId = writer.loadScriptedProxyHandler(objId);
  ObjOperandId targetObjId =
      writer.loadWrapperTarget(objId, /*fallible =*/true);

  writer.guardIsNativeObject(targetObjId);

  if (trapKind == NativeGetPropKind::Missing) {
    EmitMissingPropGuard(writer, nHandlerObj, handlerObjId);
    if (cacheKind_ == CacheKind::GetProp) {
      writer.megamorphicLoadSlotResult(targetObjId, id);
    } else {
      writer.megamorphicLoadSlotByValueResult(objId, getElemKeyValueId());
    }
  } else {
    uint32_t trapSlot = trapProp->slot();
    const Value& trapVal = trapHolder->getSlot(trapSlot);
    JSObject* trapObj = &trapVal.toObject();
    JSFunction* trapFn = &trapObj->as<JSFunction>();
    ObjOperandId trapHolderId =
        EmitReadSlotGuard(writer, nHandlerObj, trapHolder, handlerObjId);

    ValOperandId fnValId =
        EmitLoadSlot(writer, trapHolder, trapHolderId, trapSlot);
    ObjOperandId fnObjId = writer.guardToObject(fnValId);
    emitCalleeGuard(fnObjId, trapFn);
    ValOperandId targetValId = writer.boxObject(targetObjId);
    if (cacheKind_ == CacheKind::GetProp) {
      writer.callScriptedProxyGetResult(targetValId, objId, handlerObjId,
                                        fnObjId, trapFn, id);
    } else {
      ValOperandId idId = getElemKeyValueId();
      ValOperandId stringIdId = writer.idToStringOrSymbol(idId);
      writer.callScriptedProxyGetByValueResult(targetValId, objId, handlerObjId,
                                               stringIdId, fnObjId, trapFn);
    }
  }
  writer.returnFromIC();

  trackAttached("GetScriptedProxy");
  return AttachDecision::Attach;
}
#endif

AttachDecision GetPropIRGenerator::tryAttachGenericProxy(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    bool handleDOMProxies) {
  writer.guardIsProxy(objId);

  if (!handleDOMProxies) {
    // Ensure that the incoming object is not a DOM proxy, so that we can get to
    // the specialized stubs
    writer.guardIsNotDOMProxy(objId);
  }

  if (cacheKind_ == CacheKind::GetProp || mode_ == ICState::Mode::Specialized) {
    MOZ_ASSERT(!isSuper());
    maybeEmitIdGuard(id);
    writer.proxyGetResult(objId, id);
  } else {
    // Attach a stub that handles every id.
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
    MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);
    MOZ_ASSERT(!isSuper());
    writer.proxyGetByValueResult(objId, getElemKeyValueId());
  }

  writer.returnFromIC();

  trackAttached("GetProp.GenericProxy");
  return AttachDecision::Attach;
}

static bool ValueIsInt64Index(const Value& val, int64_t* index) {
  // Try to convert the Value to a TypedArray index or DataView offset.

  if (val.isInt32()) {
    *index = val.toInt32();
    return true;
  }

  if (val.isDouble()) {
    // Use NumberEqualsInt64 because ToPropertyKey(-0) is 0.
    return mozilla::NumberEqualsInt64(val.toDouble(), index);
  }

  return false;
}

IntPtrOperandId IRGenerator::guardToIntPtrIndex(const Value& index,
                                                ValOperandId indexId,
                                                bool supportOOB) {
#ifdef DEBUG
  int64_t indexInt64;
  MOZ_ASSERT_IF(!supportOOB, ValueIsInt64Index(index, &indexInt64));
#endif

  if (index.isInt32()) {
    Int32OperandId int32IndexId = writer.guardToInt32(indexId);
    return writer.int32ToIntPtr(int32IndexId);
  }

  MOZ_ASSERT(index.isNumber());
  NumberOperandId numberIndexId = writer.guardIsNumber(indexId);
  return writer.guardNumberToIntPtrIndex(numberIndexId, supportOOB);
}

ObjOperandId IRGenerator::guardDOMProxyExpandoObjectAndShape(
    ProxyObject* obj, ObjOperandId objId, const Value& expandoVal,
    NativeObject* expandoObj) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  TestMatchingProxyReceiver(writer, obj, objId);

  // Shape determines Class, so now it must be a DOM proxy.
  ValOperandId expandoValId;
  if (expandoVal.isObject()) {
    expandoValId = writer.loadDOMExpandoValue(objId);
  } else {
    expandoValId = writer.loadDOMExpandoValueIgnoreGeneration(objId);
  }

  // Guard the expando is an object and shape guard.
  ObjOperandId expandoObjId = writer.guardToObject(expandoValId);
  TestMatchingHolder(writer, expandoObj, expandoObjId);
  return expandoObjId;
}

AttachDecision GetPropIRGenerator::tryAttachDOMProxyExpando(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId receiverId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  Value expandoVal = GetProxyPrivate(obj);
  JSObject* expandoObj;
  if (expandoVal.isObject()) {
    expandoObj = &expandoVal.toObject();
  } else {
    MOZ_ASSERT(!expandoVal.isUndefined(),
               "How did a missing expando manage to shadow things?");
    auto expandoAndGeneration =
        static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
    MOZ_ASSERT(expandoAndGeneration);
    expandoObj = &expandoAndGeneration->expando.toObject();
  }

  // Try to do the lookup on the expando object.
  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, expandoObj, id, &holder, &prop, pc_);
  if (kind == NativeGetPropKind::None) {
    return AttachDecision::NoAction;
  }
  if (!holder) {
    return AttachDecision::NoAction;
  }
  auto* nativeExpandoObj = &expandoObj->as<NativeObject>();

  MOZ_ASSERT(holder == nativeExpandoObj);

  maybeEmitIdGuard(id);
  ObjOperandId expandoObjId = guardDOMProxyExpandoObjectAndShape(
      obj, objId, expandoVal, nativeExpandoObj);

  if (kind == NativeGetPropKind::Slot) {
    // Load from the expando's slots.
    EmitLoadSlotResult(writer, expandoObjId, nativeExpandoObj, *prop);
    writer.returnFromIC();
  } else {
    // Call the getter. Note that we pass objId, the DOM proxy, as |this|
    // and not the expando object.
    MOZ_ASSERT(kind == NativeGetPropKind::NativeGetter ||
               kind == NativeGetPropKind::ScriptedGetter);
    emitGuardGetterSetterSlot(nativeExpandoObj, *prop, expandoObjId,
                              AccessorKind::Getter);
    emitCallGetterResultNoGuards(kind, nativeExpandoObj, nativeExpandoObj,
                                 *prop, receiverId);
  }

  trackAttached("GetProp.DOMProxyExpando");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDOMProxyShadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id) {
  MOZ_ASSERT(!isSuper());
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  maybeEmitIdGuard(id);
  TestMatchingProxyReceiver(writer, obj, objId);
  writer.proxyGetResult(objId, id);
  writer.returnFromIC();

  trackAttached("GetProp.DOMProxyShadowed");
  return AttachDecision::Attach;
}

// Emit CacheIR to guard the DOM proxy doesn't shadow |id|. There are two types
// of DOM proxies:
//
// (a) DOM proxies marked LegacyOverrideBuiltIns in WebIDL, for example
//     HTMLDocument or HTMLFormElement. These proxies look up properties in this
//     order:
//
//       (1) The expando object.
//       (2) The proxy's named-property handler.
//       (3) The prototype chain.
//
//     To optimize properties on the prototype chain, we have to guard that (1)
//     and (2) don't shadow (3). We handle (1) by either emitting a shape guard
//     for the expando object or by guarding the proxy has no expando object. To
//     efficiently handle (2), the proxy must have an ExpandoAndGeneration*
//     stored as PrivateValue. We guard on its generation field to ensure the
//     set of names hasn't changed.
//
//     Missing properties can be optimized in a similar way by emitting shape
//     guards for the prototype chain.
//
// (b) Other DOM proxies. These proxies look up properties in this
//     order:
//
//       (1) The expando object.
//       (2) The prototype chain.
//       (3) The proxy's named-property handler.
//
//     To optimize properties on the prototype chain, we only have to guard the
//     expando object doesn't shadow it.
//
//     Missing properties can't be optimized in this case because we don't have
//     an efficient way to guard against the proxy handler shadowing the
//     property (there's no ExpandoAndGeneration*).
//
// See also:
// * DOMProxyShadows in DOMJSProxyHandler.cpp
// * https://webidl.spec.whatwg.org/#dfn-named-property-visibility (the Note at
//   the end)
//
// Callers are expected to have already guarded on the shape of the
// object, which guarantees the object is a DOM proxy.
static void CheckDOMProxyDoesNotShadow(CacheIRWriter& writer, ProxyObject* obj,
                                       jsid id, ObjOperandId objId,
                                       bool* canOptimizeMissing) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  Value expandoVal = GetProxyPrivate(obj);

  ValOperandId expandoId;
  if (!expandoVal.isObject() && !expandoVal.isUndefined()) {
    // Case (a).
    auto expandoAndGeneration =
        static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
    uint64_t generation = expandoAndGeneration->generation;
    expandoId = writer.loadDOMExpandoValueGuardGeneration(
        objId, expandoAndGeneration, generation);
    expandoVal = expandoAndGeneration->expando;
    *canOptimizeMissing = true;
  } else {
    // Case (b).
    expandoId = writer.loadDOMExpandoValue(objId);
    *canOptimizeMissing = false;
  }

  if (expandoVal.isUndefined()) {
    // Guard there's no expando object.
    writer.guardNonDoubleType(expandoId, ValueType::Undefined);
  } else if (expandoVal.isObject()) {
    // Guard the proxy either has no expando object or, if it has one, that
    // the shape matches the current expando object.
    NativeObject& expandoObj = expandoVal.toObject().as<NativeObject>();
    MOZ_ASSERT(!expandoObj.containsPure(id));
    writer.guardDOMExpandoMissingOrGuardShape(expandoId, expandoObj.shape());
  } else {
    MOZ_CRASH("Invalid expando value");
  }
}

AttachDecision GetPropIRGenerator::tryAttachDOMProxyUnshadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId receiverId) {
  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  JSObject* protoObj = obj->staticPrototype();
  if (!protoObj) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, protoObj, id, &holder, &prop, pc_);
  if (kind == NativeGetPropKind::None) {
    return AttachDecision::NoAction;
  }
  auto* nativeProtoObj = &protoObj->as<NativeObject>();

  maybeEmitIdGuard(id);

  // Guard that our proxy (expando) object hasn't started shadowing this
  // property.
  TestMatchingProxyReceiver(writer, obj, objId);
  bool canOptimizeMissing = false;
  CheckDOMProxyDoesNotShadow(writer, obj, id, objId, &canOptimizeMissing);

  if (holder) {
    // Found the property on the prototype chain. Treat it like a native
    // getprop.
    GeneratePrototypeGuards(writer, obj, holder, objId);

    // Guard on the holder of the property.
    ObjOperandId holderId = writer.loadObject(holder);
    TestMatchingHolder(writer, holder, holderId);

    if (kind == NativeGetPropKind::Slot) {
      EmitLoadSlotResult(writer, holderId, holder, *prop);
      writer.returnFromIC();
    } else {
      // EmitCallGetterResultNoGuards expects |obj| to be the object the
      // property is on to do some checks. Since we actually looked at
      // checkObj, and no extra guards will be generated, we can just
      // pass that instead.
      MOZ_ASSERT(kind == NativeGetPropKind::NativeGetter ||
                 kind == NativeGetPropKind::ScriptedGetter);
      MOZ_ASSERT(!isSuper());
      emitGuardGetterSetterSlot(holder, *prop, holderId, AccessorKind::Getter,
                                /* holderIsConstant = */ true);
      emitCallGetterResultNoGuards(kind, nativeProtoObj, holder, *prop,
                                   receiverId);
    }
  } else {
    // Property was not found on the prototype chain.
    MOZ_ASSERT(kind == NativeGetPropKind::Missing);
    if (canOptimizeMissing) {
      // We already guarded on the proxy's shape, so now shape guard the proto
      // chain.
      ObjOperandId protoId = writer.loadObject(nativeProtoObj);
      EmitMissingPropResult(writer, nativeProtoObj, protoId);
    } else {
      MOZ_ASSERT(!isSuper());
      writer.proxyGetResult(objId, id);
    }
    writer.returnFromIC();
  }

  trackAttached("GetProp.DOMProxyUnshadowed");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachProxy(HandleObject obj,
                                                  ObjOperandId objId,
                                                  HandleId id,
                                                  ValOperandId receiverId) {
  // The proxy stubs don't currently support |super| access.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  // Always try to attach scripted proxy get even if we're megamorphic.
  // In Speedometer 3 we'll often run into cases where we're megamorphic
  // overall, but monomorphic for the proxy case. This is because there
  // are functions which lazily turn various differently-shaped objects
  // into proxies. So the un-proxified objects are megamorphic, but the
  // proxy handlers are actually monomorphic. There is room for a bit
  // more sophistication here, but this should do for now.
  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }
  auto proxy = obj.as<ProxyObject>();
#ifdef JS_PUNBOX64
  if (proxy->handler()->isScripted()) {
    TRY_ATTACH(tryAttachScriptedProxy(proxy, objId, id));
  }
#endif

  ProxyStubType type = GetProxyStubType(cx_, obj, id);
  if (type == ProxyStubType::None) {
    return AttachDecision::NoAction;
  }

  if (mode_ == ICState::Mode::Megamorphic) {
    return tryAttachGenericProxy(proxy, objId, id,
                                 /* handleDOMProxies = */ true);
  }

  switch (type) {
    case ProxyStubType::None:
      break;
    case ProxyStubType::DOMExpando:
      TRY_ATTACH(tryAttachDOMProxyExpando(proxy, objId, id, receiverId));
      [[fallthrough]];  // Fall through to the generic shadowed case.
    case ProxyStubType::DOMShadowed:
      return tryAttachDOMProxyShadowed(proxy, objId, id);
    case ProxyStubType::DOMUnshadowed:
      TRY_ATTACH(tryAttachDOMProxyUnshadowed(proxy, objId, id, receiverId));
      return tryAttachGenericProxy(proxy, objId, id,
                                   /* handleDOMProxies = */ true);
    case ProxyStubType::Generic:
      return tryAttachGenericProxy(proxy, objId, id,
                                   /* handleDOMProxies = */ false);
  }

  MOZ_CRASH("Unexpected ProxyStubType");
}

const JSClass* js::jit::ClassFor(GuardClassKind kind) {
  switch (kind) {
    case GuardClassKind::Array:
      return &ArrayObject::class_;
    case GuardClassKind::PlainObject:
      return &PlainObject::class_;
    case GuardClassKind::FixedLengthArrayBuffer:
      return &FixedLengthArrayBufferObject::class_;
    case GuardClassKind::ImmutableArrayBuffer:
      return &ImmutableArrayBufferObject::class_;
    case GuardClassKind::ResizableArrayBuffer:
      return &ResizableArrayBufferObject::class_;
    case GuardClassKind::FixedLengthSharedArrayBuffer:
      return &FixedLengthSharedArrayBufferObject::class_;
    case GuardClassKind::GrowableSharedArrayBuffer:
      return &GrowableSharedArrayBufferObject::class_;
    case GuardClassKind::FixedLengthDataView:
      return &FixedLengthDataViewObject::class_;
    case GuardClassKind::ImmutableDataView:
      return &ImmutableDataViewObject::class_;
    case GuardClassKind::ResizableDataView:
      return &ResizableDataViewObject::class_;
    case GuardClassKind::MappedArguments:
      return &MappedArgumentsObject::class_;
    case GuardClassKind::UnmappedArguments:
      return &UnmappedArgumentsObject::class_;
    case GuardClassKind::WindowProxy:
      // Caller needs to handle this case, see
      // JSRuntime::maybeWindowProxyClass().
      break;
    case GuardClassKind::JSFunction:
      // Caller needs to handle this case. Can be either |js::FunctionClass| or
      // |js::ExtendedFunctionClass|.
      break;
    case GuardClassKind::BoundFunction:
      return &BoundFunctionObject::class_;
    case GuardClassKind::Set:
      return &SetObject::class_;
    case GuardClassKind::Map:
      return &MapObject::class_;
    case GuardClassKind::Date:
      return &DateObject::class_;
    case GuardClassKind::WeakMap:
      return &WeakMapObject::class_;
    case GuardClassKind::WeakSet:
      return &WeakSetObject::class_;
  }
  MOZ_CRASH("unexpected kind");
}

// Guards the class of an object. Because shape implies class, and a shape guard
// is faster than a class guard, if this is our first time attaching a stub, we
// instead generate a shape guard.
void IRGenerator::emitOptimisticClassGuard(ObjOperandId objId, JSObject* obj,
                                           GuardClassKind kind) {
#ifdef DEBUG
  switch (kind) {
    case GuardClassKind::Array:
    case GuardClassKind::PlainObject:
    case GuardClassKind::FixedLengthArrayBuffer:
    case GuardClassKind::ImmutableArrayBuffer:
    case GuardClassKind::ResizableArrayBuffer:
    case GuardClassKind::FixedLengthSharedArrayBuffer:
    case GuardClassKind::GrowableSharedArrayBuffer:
    case GuardClassKind::FixedLengthDataView:
    case GuardClassKind::ImmutableDataView:
    case GuardClassKind::ResizableDataView:
    case GuardClassKind::Set:
    case GuardClassKind::Map:
    case GuardClassKind::Date:
    case GuardClassKind::WeakMap:
    case GuardClassKind::WeakSet:
      MOZ_ASSERT(obj->hasClass(ClassFor(kind)));
      break;

    case GuardClassKind::MappedArguments:
    case GuardClassKind::UnmappedArguments:
    case GuardClassKind::JSFunction:
    case GuardClassKind::BoundFunction:
    case GuardClassKind::WindowProxy:
      // Arguments, functions, and the global object have
      // less consistent shapes.
      MOZ_CRASH("GuardClassKind not supported");
  }
#endif

  if (isFirstStub_) {
    writer.guardShapeForClass(objId, obj->shape());
  } else {
    writer.guardClass(objId, kind);
  }
}

static void AssertArgumentsCustomDataProp(ArgumentsObject* obj,
                                          PropertyKey key) {
#ifdef DEBUG
  // The property must still be a custom data property if it has been resolved.
  // If this assertion fails, we're probably missing a call to mark this
  // property overridden.
  Maybe<PropertyInfo> prop = obj->lookupPure(key);
  MOZ_ASSERT_IF(prop, prop->isCustomDataProperty());
#endif
}

AttachDecision GetPropIRGenerator::tryAttachObjectLength(HandleObject obj,
                                                         ObjOperandId objId,
                                                         HandleId id) {
  if (!id.isAtom(cx_->names().length)) {
    return AttachDecision::NoAction;
  }

  if (obj->is<ArrayObject>()) {
    if (obj->as<ArrayObject>().length() > INT32_MAX) {
      return AttachDecision::NoAction;
    }

    maybeEmitIdGuard(id);
    emitOptimisticClassGuard(objId, obj, GuardClassKind::Array);
    writer.loadInt32ArrayLengthResult(objId);
    writer.returnFromIC();

    trackAttached("GetProp.ArrayLength");
    return AttachDecision::Attach;
  }

  if (obj->is<ArgumentsObject>() &&
      !obj->as<ArgumentsObject>().hasOverriddenLength()) {
    AssertArgumentsCustomDataProp(&obj->as<ArgumentsObject>(), id);
    maybeEmitIdGuard(id);
    if (obj->is<MappedArgumentsObject>()) {
      writer.guardClass(objId, GuardClassKind::MappedArguments);
    } else {
      MOZ_ASSERT(obj->is<UnmappedArgumentsObject>());
      writer.guardClass(objId, GuardClassKind::UnmappedArguments);
    }
    writer.loadArgumentsObjectLengthResult(objId);
    writer.returnFromIC();

    trackAttached("GetProp.ArgumentsObjectLength");
    return AttachDecision::Attach;
  }

  return AttachDecision::NoAction;
}

AttachDecision GetPropIRGenerator::tryAttachTypedArray(HandleObject obj,
                                                       ObjOperandId objId,
                                                       HandleId id) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  bool isLength = id.isAtom(cx_->names().length);
  bool isByteOffset = id.isAtom(cx_->names().byteOffset);
  if (!isLength && !isByteOffset && !id.isAtom(cx_->names().byteLength)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter) {
    return AttachDecision::NoAction;
  }

  JSFunction& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (isLength) {
    if (!TypedArrayObject::isOriginalLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else if (isByteOffset) {
    if (!TypedArrayObject::isOriginalByteOffsetGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!TypedArrayObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  }

  auto* tarr = &obj->as<TypedArrayObject>();

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  emitCallGetterResultGuards(tarr, holder, id, *prop, objId);
  if (isLength) {
    size_t length = tarr->length().valueOr(0);
    if (!tarr->is<ResizableTypedArrayObject>()) {
      if (length <= INT32_MAX) {
        writer.loadArrayBufferViewLengthInt32Result(objId);
      } else {
        writer.loadArrayBufferViewLengthDoubleResult(objId);
      }
    } else {
      if (length <= INT32_MAX) {
        writer.resizableTypedArrayLengthInt32Result(objId);
      } else {
        writer.resizableTypedArrayLengthDoubleResult(objId);
      }
    }
    trackAttached("GetProp.TypedArrayLength");
  } else if (isByteOffset) {
    // byteOffset doesn't need to use different code paths for fixed-length and
    // resizable TypedArrays.
    size_t byteOffset = tarr->byteOffset().valueOr(0);
    if (byteOffset <= INT32_MAX) {
      writer.arrayBufferViewByteOffsetInt32Result(objId);
    } else {
      writer.arrayBufferViewByteOffsetDoubleResult(objId);
    }
    trackAttached("GetProp.TypedArrayByteOffset");
  } else {
    size_t byteLength = tarr->byteLength().valueOr(0);
    if (!tarr->is<ResizableTypedArrayObject>()) {
      if (byteLength <= INT32_MAX) {
        writer.typedArrayByteLengthInt32Result(objId);
      } else {
        writer.typedArrayByteLengthDoubleResult(objId);
      }
    } else {
      if (byteLength <= INT32_MAX) {
        writer.resizableTypedArrayByteLengthInt32Result(objId);
      } else {
        writer.resizableTypedArrayByteLengthDoubleResult(objId);
      }
    }
    trackAttached("GetProp.TypedArrayByteLength");
  }
  writer.returnFromIC();

  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDataView(HandleObject obj,
                                                     ObjOperandId objId,
                                                     HandleId id) {
  if (!obj->is<DataViewObject>()) {
    return AttachDecision::NoAction;
  }
  auto* dv = &obj->as<DataViewObject>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  bool isByteOffset = id.isAtom(cx_->names().byteOffset);
  if (!isByteOffset && !id.isAtom(cx_->names().byteLength)) {
    return AttachDecision::NoAction;
  }

  // byteOffset and byteLength both throw when the ArrayBuffer is detached.
  if (dv->hasDetachedBuffer()) {
    // The has-attached-arraybuffer guard is elided for immutable views. Assert
    // we never see an immutable view with a detached buffer.
    MOZ_ASSERT(!dv->is<ImmutableDataViewObject>(),
               "immutable data views can't have their buffer detached");
    return AttachDecision::NoAction;
  }

  // byteOffset and byteLength both throw when the ArrayBuffer is out-of-bounds.
  if (dv->is<ResizableDataViewObject>() &&
      dv->as<ResizableDataViewObject>().isOutOfBounds()) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (isByteOffset) {
    if (!DataViewObject::isOriginalByteOffsetGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!DataViewObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  }

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  emitCallGetterResultGuards(dv, holder, id, *prop, objId);

  // Immutable array buffers can never get detached.
  if (!dv->is<ImmutableDataViewObject>()) {
    writer.guardHasAttachedArrayBuffer(objId);
  } else {
#ifdef DEBUG
    // Add a guard in debug-mode, so if the buffer unexpectedly got detached,
    // we bail out and rely on the above assertion to fire.
    writer.guardHasAttachedArrayBuffer(objId);
#endif
  }

  // Resizable array buffers can get out-of-bounds when shrunk.
  if (dv->is<ResizableDataViewObject>()) {
    writer.guardResizableArrayBufferViewInBounds(objId);
  }

  if (isByteOffset) {
    // byteOffset doesn't need to use different code paths for fixed-length,
    // resizable, or immutable DataViews.
    size_t byteOffset = dv->byteOffset().valueOr(0);
    if (byteOffset <= INT32_MAX) {
      writer.arrayBufferViewByteOffsetInt32Result(objId);
    } else {
      writer.arrayBufferViewByteOffsetDoubleResult(objId);
    }
    trackAttached("GetProp.DataViewByteOffset");
  } else {
    size_t byteLength = dv->byteLength().valueOr(0);
    if (!dv->is<ResizableDataViewObject>()) {
      if (byteLength <= INT32_MAX) {
        writer.loadArrayBufferViewLengthInt32Result(objId);
      } else {
        writer.loadArrayBufferViewLengthDoubleResult(objId);
      }
    } else {
      if (byteLength <= INT32_MAX) {
        writer.resizableDataViewByteLengthInt32Result(objId);
      } else {
        writer.resizableDataViewByteLengthDoubleResult(objId);
      }
    }
    trackAttached("GetProp.DataViewByteLength");
  }
  writer.returnFromIC();

  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArrayBufferMaybeShared(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  if (!obj->is<ArrayBufferObjectMaybeShared>()) {
    return AttachDecision::NoAction;
  }
  auto* buf = &obj->as<ArrayBufferObjectMaybeShared>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  if (!id.isAtom(cx_->names().byteLength)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (buf->is<ArrayBufferObject>()) {
    if (!ArrayBufferObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!SharedArrayBufferObject::isOriginalByteLengthGetter(fun.native())) {
      return AttachDecision::NoAction;
    }
  }

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  emitCallGetterResultGuards(buf, holder, id, *prop, objId);
  if (!buf->is<GrowableSharedArrayBufferObject>()) {
    if (buf->byteLength() <= INT32_MAX) {
      writer.loadArrayBufferByteLengthInt32Result(objId);
    } else {
      writer.loadArrayBufferByteLengthDoubleResult(objId);
    }
  } else {
    if (buf->byteLength() <= INT32_MAX) {
      writer.growableSharedArrayBufferByteLengthInt32Result(objId);
    } else {
      writer.growableSharedArrayBufferByteLengthDoubleResult(objId);
    }
  }
  writer.returnFromIC();

  trackAttached("GetProp.ArrayBufferMaybeSharedByteLength");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachRegExp(HandleObject obj,
                                                   ObjOperandId objId,
                                                   HandleId id) {
  if (!obj->is<RegExpObject>()) {
    return AttachDecision::NoAction;
  }
  auto* regExp = &obj->as<RegExpObject>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  JS::RegExpFlags flags = JS::RegExpFlag::NoFlags;
  if (!RegExpObject::isOriginalFlagGetter(fun.native(), &flags)) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  emitCallGetterResultGuards(regExp, holder, id, *prop, objId);

  writer.regExpFlagResult(objId, flags.value());
  writer.returnFromIC();

  trackAttached("GetProp.RegExpFlag");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachMap(HandleObject obj,
                                                ObjOperandId objId,
                                                HandleId id) {
  if (!obj->is<MapObject>()) {
    return AttachDecision::NoAction;
  }
  auto* mapObj = &obj->as<MapObject>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  if (!id.isAtom(cx_->names().size)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (!MapObject::isOriginalSizeGetter(fun.native())) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);

  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  emitCallGetterResultGuards(mapObj, holder, id, *prop, objId);

  writer.mapSizeResult(objId);
  writer.returnFromIC();

  trackAttached("GetProp.MapSize");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachSet(HandleObject obj,
                                                ObjOperandId objId,
                                                HandleId id) {
  if (!obj->is<SetObject>()) {
    return AttachDecision::NoAction;
  }
  auto* setObj = &obj->as<SetObject>();

  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  // Receiver should be the object.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  if (!id.isAtom(cx_->names().size)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, obj, id, &holder, &prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter) {
    return AttachDecision::NoAction;
  }

  auto& fun = holder->getGetter(*prop)->as<JSFunction>();
  if (!SetObject::isOriginalSizeGetter(fun.native())) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);

  // Emit all the normal guards for calling this native, but specialize
  // callNativeGetterResult.
  emitCallGetterResultGuards(setObj, holder, id, *prop, objId);

  writer.setSizeResult(objId);
  writer.returnFromIC();

  trackAttached("GetProp.SetSize");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachFunction(HandleObject obj,
                                                     ObjOperandId objId,
                                                     HandleId id) {
  // Function properties are lazily resolved so they might not be defined yet.
  // And we might end up in a situation where we always have a fresh function
  // object during the IC generation.
  if (!obj->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }

  bool isLength = id.isAtom(cx_->names().length);
  if (!isLength && !id.isAtom(cx_->names().name)) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  PropertyResult prop;
  // If this property exists already, don't attach the stub.
  if (LookupPropertyPure(cx_, obj, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }

  JSFunction* fun = &obj->as<JSFunction>();

  if (isLength) {
    // length was probably deleted from the function.
    if (fun->hasResolvedLength()) {
      return AttachDecision::NoAction;
    }

    // Lazy functions don't store the length.
    if (!fun->hasBytecode()) {
      return AttachDecision::NoAction;
    }
  } else {
    // name was probably deleted from the function.
    if (fun->hasResolvedName()) {
      return AttachDecision::NoAction;
    }
  }

  maybeEmitIdGuard(id);
  writer.guardClass(objId, GuardClassKind::JSFunction);
  if (isLength) {
    writer.loadFunctionLengthResult(objId);
    writer.returnFromIC();
    trackAttached("GetProp.FunctionLength");
  } else {
    writer.loadFunctionNameResult(objId);
    writer.returnFromIC();
    trackAttached("GetProp.FunctionName");
  }
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectIterator(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }

  if (!id.isWellKnownSymbol(JS::SymbolCode::iterator)) {
    return AttachDecision::NoAction;
  }

  Handle<ArgumentsObject*> args = obj.as<ArgumentsObject>();
  if (args->hasOverriddenIterator()) {
    return AttachDecision::NoAction;
  }
  if (cx_->realm() != args->realm()) {
    return AttachDecision::NoAction;
  }

  AssertArgumentsCustomDataProp(args, id);

  RootedValue iterator(cx_);
  if (!ArgumentsObject::getArgumentsIterator(cx_, &iterator)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(iterator.isObject());

  maybeEmitIdGuard(id);
  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }
  uint32_t flags = ArgumentsObject::ITERATOR_OVERRIDDEN_BIT;
  writer.guardArgumentsObjectFlags(objId, flags);
  writer.guardObjectHasSameRealm(objId);

  ObjOperandId iterId = writer.loadObject(&iterator.toObject());
  writer.loadObjectResult(iterId);
  writer.returnFromIC();

  trackAttached("GetProp.ArgumentsObjectIterator");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachModuleNamespace(HandleObject obj,
                                                            ObjOperandId objId,
                                                            HandleId id) {
  if (!obj->is<ModuleNamespaceObject>()) {
    return AttachDecision::NoAction;
  }

  auto* ns = &obj->as<ModuleNamespaceObject>();
  ModuleEnvironmentObject* env = nullptr;
  Maybe<PropertyInfo> prop;
  if (!ns->bindings().lookup(id, &env, &prop)) {
    return AttachDecision::NoAction;
  }

  // Don't emit a stub until the target binding has been initialized.
  if (env->getSlot(prop->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
    return AttachDecision::NoAction;
  }

  // Check for the specific namespace object.
  maybeEmitIdGuard(id);
  writer.guardSpecificObject(objId, ns);

  ObjOperandId envId = writer.loadObject(env);
  EmitLoadSlotResult(writer, envId, env, *prop);
  writer.returnFromIC();

  trackAttached("GetProp.ModuleNamespace");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachPrimitive(ValOperandId valId,
                                                      HandleId id) {
  MOZ_ASSERT(!isSuper(), "SuperBase is guaranteed to be an object");

  JSProtoKey protoKey;
  switch (val_.type()) {
    case ValueType::String:
      if (id.isAtom(cx_->names().length)) {
        // String length is special-cased, see js::GetProperty.
        return AttachDecision::NoAction;
      }
      protoKey = JSProto_String;
      break;
    case ValueType::Int32:
    case ValueType::Double:
      protoKey = JSProto_Number;
      break;
    case ValueType::Boolean:
      protoKey = JSProto_Boolean;
      break;
    case ValueType::Symbol:
      protoKey = JSProto_Symbol;
      break;
    case ValueType::BigInt:
      protoKey = JSProto_BigInt;
      break;
    case ValueType::Null:
    case ValueType::Undefined:
    case ValueType::Magic:
      return AttachDecision::NoAction;
    case ValueType::Object:
    case ValueType::PrivateGCThing:
      MOZ_CRASH("unexpected type");
  }

  JSObject* proto = GlobalObject::getOrCreatePrototype(cx_, protoKey);
  if (!proto) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx_, proto, id, &holder, &prop, pc_);
  switch (kind) {
    case NativeGetPropKind::None:
      return AttachDecision::NoAction;
    case NativeGetPropKind::Missing:
    case NativeGetPropKind::Slot: {
      auto* nproto = &proto->as<NativeObject>();

      if (val_.isNumber()) {
        writer.guardIsNumber(valId);
      } else {
        writer.guardNonDoubleType(valId, val_.type());
      }
      maybeEmitIdGuard(id);

      ObjOperandId protoId = writer.loadObject(nproto);
      if (kind == NativeGetPropKind::Slot) {
        EmitReadSlotResult(writer, nproto, holder, *prop, protoId);
        writer.returnFromIC();
        trackAttached("GetProp.PrimitiveSlot");
      } else {
        EmitMissingPropResult(writer, nproto, protoId);
        writer.returnFromIC();
        trackAttached("GetProp.PrimitiveMissing");
      }
      return AttachDecision::Attach;
    }
    case NativeGetPropKind::ScriptedGetter:
    case NativeGetPropKind::NativeGetter: {
      auto* nproto = &proto->as<NativeObject>();

      if (val_.isNumber()) {
        writer.guardIsNumber(valId);
      } else {
        writer.guardNonDoubleType(valId, val_.type());
      }
      maybeEmitIdGuard(id);

      ObjOperandId protoId = writer.loadObject(nproto);
      emitCallGetterResult(kind, nproto, holder, id, *prop, protoId, valId);

      trackAttached("GetProp.PrimitiveGetter");
      return AttachDecision::Attach;
    }
  }

  MOZ_CRASH("Bad NativeGetPropKind");
}

AttachDecision GetPropIRGenerator::tryAttachStringLength(ValOperandId valId,
                                                         HandleId id) {
  if (!val_.isString() || !id.isAtom(cx_->names().length)) {
    return AttachDecision::NoAction;
  }

  StringOperandId strId = writer.guardToString(valId);
  maybeEmitIdGuard(id);
  writer.loadStringLengthResult(strId);
  writer.returnFromIC();

  trackAttached("GetProp.StringLength");
  return AttachDecision::Attach;
}

enum class AttachStringChar { No, Yes, Linearize, OutOfBounds };

static AttachStringChar CanAttachStringChar(const Value& val,
                                            const Value& idVal,
                                            StringChar kind) {
  if (!val.isString() || !idVal.isInt32()) {
    return AttachStringChar::No;
  }

  JSString* str = val.toString();
  int32_t index = idVal.toInt32();

  if (index < 0 && kind == StringChar::At) {
    static_assert(JSString::MAX_LENGTH <= INT32_MAX,
                  "string length fits in int32");
    index += int32_t(str->length());
  }

  if (index < 0 || size_t(index) >= str->length()) {
    return AttachStringChar::OutOfBounds;
  }

  // This follows JSString::getChar and MacroAssembler::loadStringChar.
  if (str->isRope()) {
    JSRope* rope = &str->asRope();
    if (size_t(index) < rope->leftChild()->length()) {
      str = rope->leftChild();

      // MacroAssembler::loadStringChar doesn't support surrogate pairs which
      // are split between the left and right child of a rope.
      if (kind == StringChar::CodePointAt &&
          size_t(index) + 1 == str->length() && str->isLinear()) {
        // Linearize the string when the last character of the left child is a
        // a lead surrogate.
        char16_t ch = str->asLinear().latin1OrTwoByteChar(index);
        if (unicode::IsLeadSurrogate(ch)) {
          return AttachStringChar::Linearize;
        }
      }
    } else {
      str = rope->rightChild();
    }
  }

  if (!str->isLinear()) {
    return AttachStringChar::Linearize;
  }

  return AttachStringChar::Yes;
}

static Int32OperandId EmitGuardToInt32Index(CacheIRWriter& writer,
                                            const Value& index,
                                            ValOperandId indexId) {
  if (index.isInt32()) {
    return writer.guardToInt32(indexId);
  }
  MOZ_ASSERT(index.isDouble());
  return writer.guardToInt32Index(indexId);
}

AttachDecision GetPropIRGenerator::tryAttachStringChar(ValOperandId valId,
                                                       ValOperandId indexId) {
  MOZ_ASSERT(idVal_.isInt32());

  auto attach = CanAttachStringChar(val_, idVal_, StringChar::CharAt);
  if (attach == AttachStringChar::No) {
    return AttachDecision::NoAction;
  }

  // Can't attach for out-of-bounds access without guarding that indexed
  // properties aren't present along the prototype chain of |String.prototype|.
  if (attach == AttachStringChar::OutOfBounds) {
    return AttachDecision::NoAction;
  }

  StringOperandId strId = writer.guardToString(valId);
  Int32OperandId int32IndexId = EmitGuardToInt32Index(writer, idVal_, indexId);
  if (attach == AttachStringChar::Linearize) {
    strId = writer.linearizeForCharAccess(strId, int32IndexId);
  }
  writer.loadStringCharResult(strId, int32IndexId, /* handleOOB = */ false);
  writer.returnFromIC();

  trackAttached("GetProp.StringChar");
  return AttachDecision::Attach;
}

static bool ClassCanHaveExtraProperties(const JSClass* clasp) {
  return clasp->getResolve() || clasp->getOpsLookupProperty() ||
         clasp->getOpsGetProperty() || IsTypedArrayClass(clasp);
}

enum class OwnProperty : bool { No, Yes };
enum class AllowIndexedReceiver : bool { No, Yes };
enum class AllowExtraReceiverProperties : bool { No, Yes };

static bool CanAttachDenseElementHole(
    NativeObject* obj, OwnProperty ownProp,
    AllowIndexedReceiver allowIndexedReceiver = AllowIndexedReceiver::No,
    AllowExtraReceiverProperties allowExtraReceiverProperties =
        AllowExtraReceiverProperties::No) {
  // Make sure the objects on the prototype don't have any indexed properties
  // or that such properties can't appear without a shape change.
  // Otherwise returning undefined for holes would obviously be incorrect,
  // because we would have to lookup a property on the prototype instead.
  do {
    // The first two checks are also relevant to the receiver object.
    if (allowIndexedReceiver == AllowIndexedReceiver::No && obj->isIndexed()) {
      return false;
    }
    allowIndexedReceiver = AllowIndexedReceiver::No;

    if (allowExtraReceiverProperties == AllowExtraReceiverProperties::No &&
        ClassCanHaveExtraProperties(obj->getClass())) {
      return false;
    }
    allowExtraReceiverProperties = AllowExtraReceiverProperties::No;

    // Don't need to check prototype for OwnProperty checks
    if (ownProp == OwnProperty::Yes) {
      return true;
    }

    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      break;
    }

    if (!proto->is<NativeObject>()) {
      return false;
    }

    // Make sure objects on the prototype don't have dense elements.
    if (proto->as<NativeObject>().getDenseInitializedLength() != 0) {
      return false;
    }

    obj = &proto->as<NativeObject>();
  } while (true);

  return true;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectArg(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }
  auto* args = &obj->as<ArgumentsObject>();

  // No elements must have been overridden or deleted.
  if (args->hasOverriddenElement()) {
    return AttachDecision::NoAction;
  }

  // Check bounds.
  if (index >= args->initialLength()) {
    return AttachDecision::NoAction;
  }

  AssertArgumentsCustomDataProp(args, PropertyKey::Int(index));

  // And finally also check that the argument isn't forwarded.
  if (args->argIsForwarded(index)) {
    return AttachDecision::NoAction;
  }

  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }

  writer.loadArgumentsObjectArgResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("GetProp.ArgumentsObjectArg");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectArgHole(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }
  auto* args = &obj->as<ArgumentsObject>();

  // No elements must have been overridden or deleted.
  if (args->hasOverriddenElement()) {
    return AttachDecision::NoAction;
  }

  // And also check that the argument isn't forwarded.
  if (index < args->initialLength() && args->argIsForwarded(index)) {
    return AttachDecision::NoAction;
  }

  if (!CanAttachDenseElementHole(args, OwnProperty::No,
                                 AllowIndexedReceiver::Yes,
                                 AllowExtraReceiverProperties::Yes)) {
    return AttachDecision::NoAction;
  }

  // We don't need to guard on the shape, because we check if any element is
  // overridden. Elements are marked as overridden iff any element is defined,
  // irrespective of whether the element is in-bounds or out-of-bounds. So when
  // that flag isn't set, we can guarantee that the arguments object doesn't
  // have any additional own elements.

  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }

  GeneratePrototypeHoleGuards(writer, args, objId,
                              /* alwaysGuardFirstProto = */ true);

  writer.loadArgumentsObjectArgHoleResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("GetProp.ArgumentsObjectArgHole");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachArgumentsObjectCallee(
    HandleObject obj, ObjOperandId objId, HandleId id) {
  // Only mapped arguments objects have a `callee` property.
  if (!obj->is<MappedArgumentsObject>()) {
    return AttachDecision::NoAction;
  }

  if (!id.isAtom(cx_->names().callee)) {
    return AttachDecision::NoAction;
  }

  // The callee must not have been overridden or deleted.
  MappedArgumentsObject* args = &obj->as<MappedArgumentsObject>();
  if (args->hasOverriddenCallee()) {
    return AttachDecision::NoAction;
  }

  AssertArgumentsCustomDataProp(args, id);

  maybeEmitIdGuard(id);
  writer.guardClass(objId, GuardClassKind::MappedArguments);

  uint32_t flags = ArgumentsObject::CALLEE_OVERRIDDEN_BIT;
  writer.guardArgumentsObjectFlags(objId, flags);

  writer.loadFixedSlotResult(objId,
                             MappedArgumentsObject::getCalleeSlotOffset());
  writer.returnFromIC();

  trackAttached("GetProp.ArgumentsObjectCallee");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDenseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }

  if (mode_ == ICState::Mode::Megamorphic) {
    writer.guardIsNativeObject(objId);
  } else {
    TestMatchingNativeReceiver(writer, nobj, objId);
  }
  bool expectPackedElements = nobj->denseElementsArePacked();
  writer.loadDenseElementResult(objId, indexId, expectPackedElements);
  writer.returnFromIC();

  trackAttached("GetProp.DenseElement");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachDenseElementHole(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }
  if (!CanAttachDenseElementHole(nobj, OwnProperty::No)) {
    return AttachDecision::NoAction;
  }

  // Guard on the shape, to prevent non-dense elements from appearing.
  TestMatchingNativeReceiver(writer, nobj, objId);
  GeneratePrototypeHoleGuards(writer, nobj, objId,
                              /* alwaysGuardFirstProto = */ false);
  writer.loadDenseElementHoleResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("GetProp.DenseElementHole");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachSparseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  NativeObject* nobj = &obj->as<NativeObject>();

  // Stub doesn't handle negative indices.
  if (index > INT32_MAX) {
    return AttachDecision::NoAction;
  }

  // The object must have sparse elements.
  if (!nobj->isIndexed()) {
    return AttachDecision::NoAction;
  }

  // The index must not be for a dense element.
  if (nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }

  // Only handle ArrayObject and PlainObject in this stub.
  if (!nobj->is<ArrayObject>() && !nobj->is<PlainObject>()) {
    return AttachDecision::NoAction;
  }

  // GetSparseElementHelper assumes that the target and the receiver
  // are the same.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

  // Here, we ensure that the prototype chain does not define any sparse
  // indexed properties on the shape lineage. This allows us to guard on
  // the shapes up the prototype chain to ensure that no indexed properties
  // exist outside of the dense elements.
  //
  // The `GeneratePrototypeHoleGuards` call below will guard on the shapes,
  // as well as ensure that no prototypes contain dense elements, allowing
  // us to perform a pure shape-search for out-of-bounds integer-indexed
  // properties on the receiver object.
  if (PrototypeMayHaveIndexedProperties(nobj)) {
    return AttachDecision::NoAction;
  }

  // Ensure that obj is an ArrayObject or PlainObject.
  if (nobj->is<ArrayObject>()) {
    writer.guardClass(objId, GuardClassKind::Array);
  } else {
    MOZ_ASSERT(nobj->is<PlainObject>());
    writer.guardClass(objId, GuardClassKind::PlainObject);
  }

  // The helper we are going to call only applies to non-dense elements.
  writer.guardIndexIsNotDenseElement(objId, indexId);

  // Ensures we are able to efficiently able to map to an integral jsid.
  writer.guardInt32IsNonNegative(indexId);

  // Shape guard the prototype chain to avoid shadowing indexes from appearing.
  // The helper function also ensures that the index does not appear within the
  // dense element set of the prototypes.
  GeneratePrototypeHoleGuards(writer, nobj, objId,
                              /* alwaysGuardFirstProto = */ true);

  // At this point, we are guaranteed that the indexed property will not
  // be found on one of the prototypes. We are assured that we only have
  // to check that the receiving object has the property.

  writer.callGetSparseElementResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("GetProp.SparseElement");
  return AttachDecision::Attach;
}

// For Uint32Array we let the stub return an Int32 if we have not seen a
// double, to allow better codegen in Warp while avoiding bailout loops.
static bool ForceDoubleForUint32Array(TypedArrayObject* tarr, uint64_t index) {
  MOZ_ASSERT(index < tarr->length().valueOr(0));

  if (tarr->type() != Scalar::Type::Uint32) {
    // Return value is only relevant for Uint32Array.
    return false;
  }

  Value res;
  MOZ_ALWAYS_TRUE(tarr->getElementPure(index, &res));
  MOZ_ASSERT(res.isNumber());
  return res.isDouble();
}

static ArrayBufferViewKind ToArrayBufferViewKind(const TypedArrayObject* obj) {
  if (obj->is<FixedLengthTypedArrayObject>()) {
    return ArrayBufferViewKind::FixedLength;
  }

  if (obj->is<ImmutableTypedArrayObject>()) {
    return ArrayBufferViewKind::Immutable;
  }

  MOZ_ASSERT(obj->is<ResizableTypedArrayObject>());
  return ArrayBufferViewKind::Resizable;
}

static ArrayBufferViewKind ToArrayBufferViewKind(const DataViewObject* obj) {
  if (obj->is<FixedLengthDataViewObject>()) {
    return ArrayBufferViewKind::FixedLength;
  }

  if (obj->is<ImmutableDataViewObject>()) {
    return ArrayBufferViewKind::Immutable;
  }

  MOZ_ASSERT(obj->is<ResizableDataViewObject>());
  return ArrayBufferViewKind::Resizable;
}

AttachDecision GetPropIRGenerator::tryAttachTypedArrayElement(
    HandleObject obj, ObjOperandId objId) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  if (!idVal_.isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* tarr = &obj->as<TypedArrayObject>();

  bool handleOOB = false;
  int64_t indexInt64;
  if (!ValueIsInt64Index(idVal_, &indexInt64) || indexInt64 < 0 ||
      uint64_t(indexInt64) >= tarr->length().valueOr(0)) {
    handleOOB = true;
  }

  // If the number is not representable as an integer the result will be
  // |undefined| so we leave |forceDoubleForUint32| as false.
  bool forceDoubleForUint32 = false;
  if (!handleOOB) {
    uint64_t index = uint64_t(indexInt64);
    forceDoubleForUint32 = ForceDoubleForUint32Array(tarr, index);
  }

  writer.guardShapeForClass(objId, tarr->shape());

  ValOperandId keyId = getElemKeyValueId();
  IntPtrOperandId intPtrIndexId = guardToIntPtrIndex(idVal_, keyId, handleOOB);

  auto viewKind = ToArrayBufferViewKind(tarr);
  writer.loadTypedArrayElementResult(objId, intPtrIndexId, tarr->type(),
                                     handleOOB, forceDoubleForUint32, viewKind);
  writer.returnFromIC();

  trackAttached("GetProp.TypedElement");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachGenericElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId receiverId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

#ifdef JS_CODEGEN_X86
  if (isSuper()) {
    // There aren't enough registers available on x86.
    return AttachDecision::NoAction;
  }
#endif

  // To allow other types to attach in the non-megamorphic case we test the
  // specific matching native receiver; however, once megamorphic we can attach
  // for any native
  if (mode_ == ICState::Mode::Megamorphic) {
    writer.guardIsNativeObject(objId);
  } else {
    NativeObject* nobj = &obj->as<NativeObject>();
    TestMatchingNativeReceiver(writer, nobj, objId);
  }
  writer.guardIndexIsNotDenseElement(objId, indexId);
  if (isSuper()) {
    writer.callNativeGetElementSuperResult(objId, indexId, receiverId);
  } else {
    writer.callNativeGetElementResult(objId, indexId);
  }
  writer.returnFromIC();

  trackAttached(mode_ == ICState::Mode::Megamorphic
                    ? "GenericElementMegamorphic"
                    : "GenericElement");
  return AttachDecision::Attach;
}

AttachDecision GetPropIRGenerator::tryAttachProxyElement(HandleObject obj,
                                                         ObjOperandId objId) {
  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  // The proxy stubs don't currently support |super| access.
  if (isSuper()) {
    return AttachDecision::NoAction;
  }

#ifdef JS_PUNBOX64
  auto proxy = obj.as<ProxyObject>();
  if (proxy->handler()->isScripted()) {
    TRY_ATTACH(tryAttachScriptedProxy(proxy, objId, JS::VoidHandlePropertyKey));
  }
#endif

  writer.guardIsProxy(objId);

  // We are not guarding against DOM proxies here, because there is no other
  // specialized DOM IC we could attach.
  // We could call maybeEmitIdGuard here and then emit ProxyGetResult,
  // but for GetElem we prefer to attach a stub that can handle any Value
  // so we don't attach a new stub for every id.
  MOZ_ASSERT(cacheKind_ == CacheKind::GetElem);
  MOZ_ASSERT(!isSuper());
  writer.proxyGetByValueResult(objId, getElemKeyValueId());
  writer.returnFromIC();

  trackAttached("GetProp.ProxyElement");
  return AttachDecision::Attach;
}

void GetPropIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", val_);
    sp.valueProperty("property", idVal_);
  }
#endif
}

void IRGenerator::emitIdGuard(ValOperandId valId, const Value& idVal, jsid id) {
  if (id.isSymbol()) {
    MOZ_ASSERT(idVal.toSymbol() == id.toSymbol());
    SymbolOperandId symId = writer.guardToSymbol(valId);
    writer.guardSpecificSymbol(symId, id.toSymbol());
    return;
  }

  MOZ_ASSERT(id.isAtom());
  switch (idVal.type()) {
    case ValueType::String: {
      StringOperandId strId = writer.guardToString(valId);
      writer.guardSpecificAtom(strId, id.toAtom());
      break;
    }
    case ValueType::Null:
      MOZ_ASSERT(id.isAtom(cx_->names().null));
      writer.guardIsNull(valId);
      break;
    case ValueType::Undefined:
      MOZ_ASSERT(id.isAtom(cx_->names().undefined));
      writer.guardIsUndefined(valId);
      break;
    case ValueType::Boolean:
      MOZ_ASSERT(id.isAtom(cx_->names().true_) ||
                 id.isAtom(cx_->names().false_));
      writer.guardSpecificValue(valId, idVal);
      break;
    case ValueType::Int32:
    case ValueType::Double:
      MOZ_ASSERT(!IsNumberIndex(idVal));
      writer.guardSpecificValue(valId, idVal);
      break;
    default:
      MOZ_CRASH("Unexpected type in emitIdGuard");
  }
}

void GetPropIRGenerator::maybeEmitIdGuard(jsid id) {
  if (cacheKind_ == CacheKind::GetProp ||
      cacheKind_ == CacheKind::GetPropSuper) {
    // Constant PropertyName, no guards necessary.
    MOZ_ASSERT(&idVal_.toString()->asAtom() == id.toAtom());
    return;
  }

  MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
             cacheKind_ == CacheKind::GetElemSuper);
  emitIdGuard(getElemKeyValueId(), idVal_, id);
}

void SetPropIRGenerator::maybeEmitIdGuard(jsid id) {
  if (cacheKind_ == CacheKind::SetProp) {
    // Constant PropertyName, no guards necessary.
    MOZ_ASSERT(&idVal_.toString()->asAtom() == id.toAtom());
    return;
  }

  MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
  emitIdGuard(setElemKeyValueId(), idVal_, id);
}

GetNameIRGenerator::GetNameIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state,
                                       HandleObject env,
                                       Handle<PropertyName*> name)
    : IRGenerator(cx, script, pc, CacheKind::GetName, state),
      env_(env),
      name_(name) {}

AttachDecision GetNameIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::GetName);

  AutoAssertNoPendingException aanpe(cx_);

  ObjOperandId envId(writer.setInputOperandId(0));
  RootedId id(cx_, NameToId(name_));

  TRY_ATTACH(tryAttachGlobalNameValue(envId, id));
  TRY_ATTACH(tryAttachGlobalNameGetter(envId, id));
  TRY_ATTACH(tryAttachEnvironmentName(envId, id));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

static bool CanAttachGlobalName(JSContext* cx,
                                GlobalLexicalEnvironmentObject* globalLexical,
                                PropertyKey id, NativeObject** holder,
                                Maybe<PropertyInfo>* prop) {
  // The property must be found, and it must be found as a normal data property.
  NativeObject* current = globalLexical;
  while (true) {
    *prop = current->lookup(cx, id);
    if (prop->isSome()) {
      break;
    }

    if (current == globalLexical) {
      current = &globalLexical->global();
    } else {
      // In the browser the global prototype chain should be immutable.
      if (!current->staticPrototypeIsImmutable()) {
        return false;
      }

      JSObject* proto = current->staticPrototype();
      if (!proto || !proto->is<NativeObject>()) {
        return false;
      }

      current = &proto->as<NativeObject>();
    }
  }

  *holder = current;
  return true;
}

AttachDecision GetNameIRGenerator::tryAttachGlobalNameValue(ObjOperandId objId,
                                                            HandleId id) {
  if (!IsGlobalOp(JSOp(*pc_))) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(!script_->hasNonSyntacticScope());

  auto* globalLexical = &env_->as<GlobalLexicalEnvironmentObject>();

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachGlobalName(cx_, globalLexical, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }

  // The property must be found, and it must be found as a normal data property.
  if (!prop->isDataProperty()) {
    return AttachDecision::NoAction;
  }

  // This might still be an uninitialized lexical.
  if (holder->getSlot(prop->slot()).isMagic()) {
    return AttachDecision::NoAction;
  }

  if (holder == globalLexical) {
    // There is no need to guard on the shape. Lexical bindings are
    // non-configurable, and this stub cannot be shared across globals.
    size_t dynamicSlotOffset =
        holder->dynamicSlotIndex(prop->slot()) * sizeof(Value);
    writer.loadDynamicSlotResult(objId, dynamicSlotOffset);
  } else if (holder == &globalLexical->global()) {
    MOZ_ASSERT(globalLexical->global().isGenerationCountedGlobal());
    writer.guardGlobalGeneration(
        globalLexical->global().generationCount(),
        globalLexical->global().addressOfGenerationCount());
    ObjOperandId holderId = writer.loadObject(holder);
#ifdef DEBUG
    writer.assertPropertyLookup(holderId, id, prop->slot());
#endif
    EmitLoadSlotResult(writer, holderId, holder, *prop);
  } else {
    // Check the prototype chain from the global to the holder
    // prototype. Ignore the global lexical scope as it doesn't figure
    // into the prototype chain. We guard on the global lexical
    // scope's shape independently.
    if (!IsCacheableGetPropSlot(&globalLexical->global(), holder, *prop)) {
      return AttachDecision::NoAction;
    }

    // Shape guard for global lexical.
    writer.guardShape(objId, globalLexical->shape());

    // Guard on the shape of the GlobalObject.
    ObjOperandId globalId = writer.loadObject(&globalLexical->global());
    writer.guardShape(globalId, globalLexical->global().shape());

    // Shape guard holder.
    ObjOperandId holderId = writer.loadObject(holder);
    writer.guardShape(holderId, holder->shape());

    EmitLoadSlotResult(writer, holderId, holder, *prop);
  }

  writer.returnFromIC();

  trackAttached("GetName.GlobalNameValue");
  return AttachDecision::Attach;
}

AttachDecision GetNameIRGenerator::tryAttachGlobalNameGetter(ObjOperandId objId,
                                                             HandleId id) {
  if (!IsGlobalOp(JSOp(*pc_))) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(!script_->hasNonSyntacticScope());

  Handle<GlobalLexicalEnvironmentObject*> globalLexical =
      env_.as<GlobalLexicalEnvironmentObject>();
  MOZ_ASSERT(globalLexical->isGlobal());

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachGlobalName(cx_, globalLexical, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }

  if (holder == globalLexical) {
    return AttachDecision::NoAction;
  }

  GlobalObject* global = &globalLexical->global();

  NativeGetPropKind kind = IsCacheableGetPropCall(global, holder, *prop, pc_);
  if (kind != NativeGetPropKind::NativeGetter &&
      kind != NativeGetPropKind::ScriptedGetter) {
    return AttachDecision::NoAction;
  }

  bool needsWindowProxy =
      IsWindow(global) && GetterNeedsWindowProxyThis(holder, *prop);

  // Shape guard for global lexical.
  writer.guardShape(objId, globalLexical->shape());

  // Guard on the shape of the GlobalObject.
  ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
  writer.guardShape(globalId, global->shape());

  if (holder != global) {
    // Shape guard holder.
    ObjOperandId holderId = writer.loadObject(holder);
    writer.guardShape(holderId, holder->shape());
    emitGuardGetterSetterSlot(holder, *prop, holderId, AccessorKind::Getter,
                              /* holderIsConstant = */ true);
  } else {
    // Note: pass true for |holderIsConstant| because the holder must be the
    // current global object.
    emitGuardGetterSetterSlot(holder, *prop, globalId, AccessorKind::Getter,
                              /* holderIsConstant = */ true);
  }

  if (CanAttachDOMGetterSetter(cx_, JSJitInfo::Getter, global, holder, *prop,
                               mode_)) {
    // The global shape guard above ensures the instance JSClass is correct.
    MOZ_ASSERT(!needsWindowProxy);
    emitCallDOMGetterResultNoGuards(holder, *prop, globalId);
    trackAttached("GetName.GlobalNameDOMGetter");
  } else {
    ObjOperandId receiverObjId;
    if (needsWindowProxy) {
      MOZ_ASSERT(cx_->global()->maybeWindowProxy());
      receiverObjId = writer.loadObject(cx_->global()->maybeWindowProxy());
    } else {
      receiverObjId = globalId;
    }
    ValOperandId receiverId = writer.boxObject(receiverObjId);
    emitCallGetterResultNoGuards(kind, global, holder, *prop, receiverId);
    trackAttached("GetName.GlobalNameGetter");
  }

  return AttachDecision::Attach;
}

static bool NeedEnvironmentShapeGuard(JSContext* cx, JSObject* envObj) {
  // We can skip a guard on the call object if the script's bindings are
  // guaranteed to be immutable (and thus cannot introduce shadowing variables).
  // If the function is a relazified self-hosted function it has no BaseScript
  // and we pessimistically create the guard.
  if (envObj->is<CallObject>()) {
    auto* callObj = &envObj->as<CallObject>();
    JSFunction* fun = &callObj->callee();
    return !fun->hasBaseScript() ||
           fun->baseScript()->funHasExtensibleScope() ||
           DebugEnvironments::hasDebugEnvironment(cx, *callObj);
  }

  // Similar to the call object case, we can also skip a guard if the lexical
  // environment's bindings are immutable.
  if (envObj->is<LexicalEnvironmentObject>()) {
    return envObj->as<LexicalEnvironmentObject>().isExtensible();
  }

  // Use a shape guard for all other environment objects.
  return true;
}

AttachDecision GetNameIRGenerator::tryAttachEnvironmentName(ObjOperandId objId,
                                                            HandleId id) {
  if (IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  JSObject* env = env_;
  Maybe<PropertyInfo> prop;
  NativeObject* holder = nullptr;

  while (env) {
    if (env->is<GlobalObject>()) {
      prop = env->as<GlobalObject>().lookup(cx_, id);
      if (prop.isSome()) {
        break;
      }
      return AttachDecision::NoAction;
    }

    if (!env->is<EnvironmentObject>() || env->is<WithEnvironmentObject>()) {
      return AttachDecision::NoAction;
    }

    // Check for an 'own' property on the env. There is no need to
    // check the prototype as non-with scopes do not inherit properties
    // from any prototype.
    prop = env->as<NativeObject>().lookup(cx_, id);
    if (prop.isSome()) {
      break;
    }

    env = env->enclosingEnvironment();
  }

  holder = &env->as<NativeObject>();
  if (!IsCacheableGetPropSlot(holder, holder, *prop)) {
    return AttachDecision::NoAction;
  }
  if (holder->getSlot(prop->slot()).isMagic()) {
    MOZ_ASSERT(holder->is<EnvironmentObject>());
    return AttachDecision::NoAction;
  }

  ObjOperandId lastObjId = objId;
  env = env_;
  while (env) {
    if (NeedEnvironmentShapeGuard(cx_, env)) {
      writer.guardShape(lastObjId, env->shape());
    }

    if (env == holder) {
      break;
    }

    lastObjId = writer.loadEnclosingEnvironment(lastObjId);
    env = env->enclosingEnvironment();
  }

  ValOperandId resId = EmitLoadSlot(writer, holder, lastObjId, prop->slot());
  if (holder->is<EnvironmentObject>()) {
    writer.guardIsNotUninitializedLexical(resId);
  }
  writer.loadOperandResult(resId);
  writer.returnFromIC();

  trackAttached("GetName.EnvironmentName");
  return AttachDecision::Attach;
}

void GetNameIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", ObjectValue(*env_));
    sp.valueProperty("property", StringValue(name_));
  }
#endif
}

BindNameIRGenerator::BindNameIRGenerator(JSContext* cx, HandleScript script,
                                         jsbytecode* pc, ICState state,
                                         HandleObject env,
                                         Handle<PropertyName*> name)
    : IRGenerator(cx, script, pc, CacheKind::BindName, state),
      env_(env),
      name_(name) {}

AttachDecision BindNameIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::BindName);

  AutoAssertNoPendingException aanpe(cx_);

  ObjOperandId envId(writer.setInputOperandId(0));
  RootedId id(cx_, NameToId(name_));

  TRY_ATTACH(tryAttachGlobalName(envId, id));
  TRY_ATTACH(tryAttachEnvironmentName(envId, id));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision BindNameIRGenerator::tryAttachGlobalName(ObjOperandId objId,
                                                        HandleId id) {
  if (!IsGlobalOp(JSOp(*pc_))) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(!script_->hasNonSyntacticScope());

  Handle<GlobalLexicalEnvironmentObject*> globalLexical =
      env_.as<GlobalLexicalEnvironmentObject>();
  MOZ_ASSERT(globalLexical->isGlobal());

  JSObject* result = nullptr;
  if (Maybe<PropertyInfo> prop = globalLexical->lookup(cx_, id)) {
    // If this is an uninitialized lexical or a const, we need to return a
    // RuntimeLexicalErrorObject.
    if (globalLexical->getSlot(prop->slot()).isMagic() || !prop->writable()) {
      return AttachDecision::NoAction;
    }
    result = globalLexical;
  } else {
    result = &globalLexical->global();
  }

  if (result == globalLexical) {
    // Lexical bindings are non-configurable so we can just return the
    // global lexical.
    writer.loadObjectResult(objId);
  } else {
    // If the property exists on the global and is non-configurable, it cannot
    // be shadowed by the lexical scope so we can just return the global without
    // a shape guard.
    Maybe<PropertyInfo> prop = result->as<GlobalObject>().lookup(cx_, id);
    if (prop.isNothing() || prop->configurable()) {
      writer.guardShape(objId, globalLexical->shape());
    }
    ObjOperandId globalId = writer.loadEnclosingEnvironment(objId);
    writer.loadObjectResult(globalId);
  }
  writer.returnFromIC();

  trackAttached("BindName.GlobalName");
  return AttachDecision::Attach;
}

AttachDecision BindNameIRGenerator::tryAttachEnvironmentName(ObjOperandId objId,
                                                             HandleId id) {
  if (IsGlobalOp(JSOp(*pc_)) || script_->hasNonSyntacticScope()) {
    return AttachDecision::NoAction;
  }

  // JSOp::BindUnqualifiedName when writing to a dynamic environment binding.
  // JSOp::BindName when reading from a dynamic environment binding.
  bool unqualifiedLookup = JSOp(*pc_) == JSOp::BindUnqualifiedName;

  JSObject* env = env_;
  Maybe<PropertyInfo> prop;
  while (true) {
    // Stop when we've reached the global object.
    if (env->is<GlobalObject>()) {
      break;
    }

    if (!env->is<EnvironmentObject>() || env->is<WithEnvironmentObject>()) {
      return AttachDecision::NoAction;
    }

    // When we reach an unqualified variables object (like the global) we
    // have to stop looking and return that object.
    if (unqualifiedLookup && env->isUnqualifiedVarObj()) {
      break;
    }

    // Check for an 'own' property on the env. There is no need to
    // check the prototype as non-with scopes do not inherit properties
    // from any prototype.
    prop = env->as<NativeObject>().lookup(cx_, id);
    if (prop.isSome()) {
      break;
    }

    env = env->enclosingEnvironment();
  }

  // If this is an uninitialized lexical or a const, we need to return a
  // RuntimeLexicalErrorObject.
  auto* holder = &env->as<NativeObject>();
  if (prop.isSome() && holder->is<EnvironmentObject>()) {
    // Uninitialized lexical binding.
    if (holder->getSlot(prop->slot()).isMagic()) {
      return AttachDecision::NoAction;
    }

    // Attempt to write to a const binding.
    if (unqualifiedLookup && !prop->writable()) {
      return AttachDecision::NoAction;
    }
  }

  ObjOperandId lastObjId = objId;
  env = env_;
  while (env) {
    if (NeedEnvironmentShapeGuard(cx_, env) && !env->is<GlobalObject>()) {
      writer.guardShape(lastObjId, env->shape());
    }

    if (env == holder) {
      break;
    }

    lastObjId = writer.loadEnclosingEnvironment(lastObjId);
    env = env->enclosingEnvironment();
  }

  if (prop.isSome() && holder->is<EnvironmentObject>()) {
    ValOperandId valId = EmitLoadSlot(writer, holder, lastObjId, prop->slot());
    writer.guardIsNotUninitializedLexical(valId);
  }

  writer.loadObjectResult(lastObjId);
  writer.returnFromIC();

  trackAttached("BindName.EnvironmentName");
  return AttachDecision::Attach;
}

void BindNameIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", ObjectValue(*env_));
    sp.valueProperty("property", StringValue(name_));
  }
#endif
}

HasPropIRGenerator::HasPropIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state,
                                       CacheKind cacheKind, HandleValue idVal,
                                       HandleValue val)
    : IRGenerator(cx, script, pc, cacheKind, state), val_(val), idVal_(idVal) {}

AttachDecision HasPropIRGenerator::tryAttachDense(HandleObject obj,
                                                  ObjOperandId objId,
                                                  uint32_t index,
                                                  Int32OperandId indexId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }

  if (mode_ == ICState::Mode::Megamorphic) {
    writer.guardIsNativeObject(objId);
  } else {
    // Guard shape to ensure object class is NativeObject.
    TestMatchingNativeReceiver(writer, nobj, objId);
  }
  writer.loadDenseElementExistsResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("HasProp.Dense");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachDenseHole(HandleObject obj,
                                                      ObjOperandId objId,
                                                      uint32_t index,
                                                      Int32OperandId indexId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);
  OwnProperty ownProp = hasOwn ? OwnProperty::Yes : OwnProperty::No;

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }
  if (!CanAttachDenseElementHole(nobj, ownProp)) {
    return AttachDecision::NoAction;
  }

  // Guard shape to ensure class is NativeObject and to prevent non-dense
  // elements being added. Also ensures prototype doesn't change if dynamic
  // checks aren't emitted.
  TestMatchingNativeReceiver(writer, nobj, objId);

  // Generate prototype guards if needed. This includes monitoring that
  // properties were not added in the chain.
  if (!hasOwn) {
    GeneratePrototypeHoleGuards(writer, nobj, objId,
                                /* alwaysGuardFirstProto = */ false);
  }

  writer.loadDenseElementHoleExistsResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("HasProp.DenseHole");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachSparse(HandleObject obj,
                                                   ObjOperandId objId,
                                                   Int32OperandId indexId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);
  OwnProperty ownProp = hasOwn ? OwnProperty::Yes : OwnProperty::No;

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  if (!nobj->isIndexed()) {
    return AttachDecision::NoAction;
  }
  if (!CanAttachDenseElementHole(nobj, ownProp, AllowIndexedReceiver::Yes)) {
    return AttachDecision::NoAction;
  }

  // Guard that this is a native object.
  writer.guardIsNativeObject(objId);

  // Generate prototype guards if needed. This includes monitoring that
  // properties were not added in the chain.
  if (!hasOwn) {
    GeneratePrototypeHoleGuards(writer, nobj, objId,
                                /* alwaysGuardFirstProto = */ true);
  }

  // Because of the prototype guard we know that the prototype chain
  // does not include any dense or sparse (i.e indexed) properties.
  writer.callObjectHasSparseElementResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("HasProp.Sparse");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachArgumentsObjectArg(
    HandleObject obj, ObjOperandId objId, Int32OperandId indexId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);
  OwnProperty ownProp = hasOwn ? OwnProperty::Yes : OwnProperty::No;

  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }
  auto* args = &obj->as<ArgumentsObject>();

  // No elements must have been overridden or deleted.
  if (args->hasOverriddenElement()) {
    return AttachDecision::NoAction;
  }

  if (!CanAttachDenseElementHole(args, ownProp, AllowIndexedReceiver::Yes,
                                 AllowExtraReceiverProperties::Yes)) {
    return AttachDecision::NoAction;
  }

  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }

  if (!hasOwn) {
    GeneratePrototypeHoleGuards(writer, args, objId,
                                /* alwaysGuardFirstProto = */ true);
  }

  writer.loadArgumentsObjectArgExistsResult(objId, indexId);
  writer.returnFromIC();

  trackAttached("HasProp.ArgumentsObjectArg");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachNamedProp(HandleObject obj,
                                                      ObjOperandId objId,
                                                      HandleId key,
                                                      ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  Rooted<NativeObject*> holder(cx_);
  PropertyResult prop;

  if (hasOwn) {
    if (!LookupOwnPropertyPure(cx_, obj, key, &prop)) {
      return AttachDecision::NoAction;
    }

    holder.set(&obj->as<NativeObject>());
  } else {
    NativeObject* nHolder = nullptr;
    if (!LookupPropertyPure(cx_, obj, key, &nHolder, &prop)) {
      return AttachDecision::NoAction;
    }
    holder.set(nHolder);
  }
  if (prop.isNotFound()) {
    return AttachDecision::NoAction;
  }

  TRY_ATTACH(tryAttachSmallObjectVariableKey(obj, objId, key, keyId));
  TRY_ATTACH(tryAttachMegamorphic(objId, keyId));
  TRY_ATTACH(tryAttachNative(&obj->as<NativeObject>(), objId, key, keyId, prop,
                             holder.get()));

  return AttachDecision::NoAction;
}

AttachDecision HasPropIRGenerator::tryAttachSmallObjectVariableKey(
    HandleObject obj, ObjOperandId objId, jsid key, ValOperandId keyId) {
  MOZ_ASSERT(obj->is<NativeObject>());

  if (cacheKind_ != CacheKind::HasOwn) {
    return AttachDecision::NoAction;
  }

  if (mode_ != ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  if (numOptimizedStubs_ != 0) {
    return AttachDecision::NoAction;
  }

  if (!idVal_.isString()) {
    return AttachDecision::NoAction;
  }

  if (!obj->as<NativeObject>().hasEmptyElements()) {
    return AttachDecision::NoAction;
  }

  if (ClassCanHaveExtraProperties(obj->getClass())) {
    return AttachDecision::NoAction;
  }

  if (!obj->shape()->isShared()) {
    return AttachDecision::NoAction;
  }

  static constexpr size_t SMALL_OBJECT_SIZE = 5;

  if (obj->shape()->asShared().slotSpan() > SMALL_OBJECT_SIZE) {
    return AttachDecision::NoAction;
  }

  Rooted<ListObject*> keyListObj(cx_, ListObject::create(cx_));
  if (!keyListObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  for (SharedShapePropertyIter<CanGC> iter(cx_, &obj->shape()->asShared());
       !iter.done(); iter++) {
    if (!iter->key().isAtom()) {
      return AttachDecision::NoAction;
    }

    if (keyListObj->length() == SMALL_OBJECT_SIZE) {
      return AttachDecision::NoAction;
    }

    RootedValue key(cx_, StringValue(iter->key().toAtom()));
    if (!keyListObj->append(cx_, key)) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }
  }

  writer.guardShape(objId, obj->shape());
  writer.guardNoDenseElements(objId);
  StringOperandId keyStrId = writer.guardToString(keyId);
  StringOperandId keyAtomId = writer.stringToAtom(keyStrId);
  writer.smallObjectVariableKeyHasOwnResult(keyAtomId, keyListObj,
                                            obj->shape());
  writer.returnFromIC();
  trackAttached("HasProp.SmallObjectVariableKey");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachMegamorphic(ObjOperandId objId,
                                                        ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  if (mode_ != ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  writer.megamorphicHasPropResult(objId, keyId, hasOwn);
  writer.returnFromIC();
  trackAttached("HasProp.Megamorphic");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachNative(NativeObject* obj,
                                                   ObjOperandId objId, jsid key,
                                                   ValOperandId keyId,
                                                   PropertyResult prop,
                                                   NativeObject* holder) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (!prop.isNativeProperty()) {
    return AttachDecision::NoAction;
  }

  emitIdGuard(keyId, idVal_, key);
  EmitReadSlotGuard(writer, obj, holder, objId);
  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("HasProp.Native");
  return AttachDecision::Attach;
}

static void EmitGuardTypedArray(CacheIRWriter& writer, TypedArrayObject* obj,
                                ObjOperandId objId) {
  if (!obj->is<ResizableTypedArrayObject>()) {
    writer.guardIsNonResizableTypedArray(objId);
  } else {
    writer.guardIsResizableTypedArray(objId);
  }
}

AttachDecision HasPropIRGenerator::tryAttachTypedArray(HandleObject obj,
                                                       ObjOperandId objId,
                                                       ValOperandId keyId) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  if (!idVal_.isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* tarr = &obj->as<TypedArrayObject>();
  EmitGuardTypedArray(writer, tarr, objId);

  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(idVal_, keyId, /* supportOOB = */ true);

  auto viewKind = ToArrayBufferViewKind(tarr);
  writer.loadTypedArrayElementExistsResult(objId, intPtrIndexId, viewKind);
  writer.returnFromIC();

  trackAttached("HasProp.TypedArrayObject");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachSlotDoesNotExist(
    NativeObject* obj, ObjOperandId objId, jsid key, ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  emitIdGuard(keyId, idVal_, key);
  if (hasOwn) {
    TestMatchingNativeReceiver(writer, obj, objId);
  } else {
    EmitMissingPropGuard(writer, obj, objId);
  }
  writer.loadBooleanResult(false);
  writer.returnFromIC();

  trackAttached("HasProp.DoesNotExist");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachDoesNotExist(HandleObject obj,
                                                         ObjOperandId objId,
                                                         HandleId key,
                                                         ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  // Check that property doesn't exist on |obj| or it's prototype chain. These
  // checks allow NativeObjects with a NativeObject prototype chain. They return
  // NoAction if unknown such as resolve hooks or proxies.
  if (hasOwn) {
    if (!CheckHasNoSuchOwnProperty(cx_, obj, key)) {
      return AttachDecision::NoAction;
    }
  } else {
    if (!CheckHasNoSuchProperty(cx_, obj, key)) {
      return AttachDecision::NoAction;
    }
  }

  TRY_ATTACH(tryAttachSmallObjectVariableKey(obj, objId, key, keyId));
  TRY_ATTACH(tryAttachMegamorphic(objId, keyId));
  TRY_ATTACH(
      tryAttachSlotDoesNotExist(&obj->as<NativeObject>(), objId, key, keyId));

  return AttachDecision::NoAction;
}

AttachDecision HasPropIRGenerator::tryAttachProxyElement(HandleObject obj,
                                                         ObjOperandId objId,
                                                         ValOperandId keyId) {
  bool hasOwn = (cacheKind_ == CacheKind::HasOwn);

  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  writer.guardIsProxy(objId);
  writer.proxyHasPropResult(objId, keyId, hasOwn);
  writer.returnFromIC();

  trackAttached("HasProp.ProxyElement");
  return AttachDecision::Attach;
}

AttachDecision HasPropIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::In || cacheKind_ == CacheKind::HasOwn);

  AutoAssertNoPendingException aanpe(cx_);

  // NOTE: Argument order is PROPERTY, OBJECT
  ValOperandId keyId(writer.setInputOperandId(0));
  ValOperandId valId(writer.setInputOperandId(1));

  if (!val_.isObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }
  RootedObject obj(cx_, &val_.toObject());
  ObjOperandId objId = writer.guardToObject(valId);

  // Optimize Proxies
  TRY_ATTACH(tryAttachProxyElement(obj, objId, keyId));

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  TRY_ATTACH(tryAttachTypedArray(obj, objId, keyId));

  if (nameOrSymbol) {
    TRY_ATTACH(tryAttachNamedProp(obj, objId, id, keyId));
    TRY_ATTACH(tryAttachDoesNotExist(obj, objId, id, keyId));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  uint32_t index;
  Int32OperandId indexId;
  if (maybeGuardInt32Index(idVal_, keyId, &index, &indexId)) {
    TRY_ATTACH(tryAttachDense(obj, objId, index, indexId));
    TRY_ATTACH(tryAttachDenseHole(obj, objId, index, indexId));
    TRY_ATTACH(tryAttachSparse(obj, objId, indexId));
    TRY_ATTACH(tryAttachArgumentsObjectArg(obj, objId, indexId));

    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

void HasPropIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", val_);
    sp.valueProperty("property", idVal_);
  }
#endif
}

CheckPrivateFieldIRGenerator::CheckPrivateFieldIRGenerator(
    JSContext* cx, HandleScript script, jsbytecode* pc, ICState state,
    CacheKind cacheKind, HandleValue idVal, HandleValue val)
    : IRGenerator(cx, script, pc, cacheKind, state), val_(val), idVal_(idVal) {
  MOZ_ASSERT(idVal.isSymbol() && idVal.toSymbol()->isPrivateName());
}

AttachDecision CheckPrivateFieldIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));
  ValOperandId keyId(writer.setInputOperandId(1));

  if (!val_.isObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }
  JSObject* obj = &val_.toObject();
  ObjOperandId objId = writer.guardToObject(valId);
  PropertyKey key = PropertyKey::Symbol(idVal_.toSymbol());

  ThrowCondition condition;
  ThrowMsgKind msgKind;
  GetCheckPrivateFieldOperands(pc_, &condition, &msgKind);

  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx_, obj, key, &prop)) {
    return AttachDecision::NoAction;
  }

  if (CheckPrivateFieldWillThrow(condition, prop.isFound())) {
    // Don't attach a stub if the operation will throw.
    return AttachDecision::NoAction;
  }

  auto* nobj = &obj->as<NativeObject>();

  TRY_ATTACH(tryAttachNative(nobj, objId, key, keyId, prop));

  return AttachDecision::NoAction;
}

AttachDecision CheckPrivateFieldIRGenerator::tryAttachNative(
    NativeObject* obj, ObjOperandId objId, jsid key, ValOperandId keyId,
    PropertyResult prop) {
  MOZ_ASSERT(prop.isNativeProperty() || prop.isNotFound());

  emitIdGuard(keyId, idVal_, key);
  TestMatchingNativeReceiver(writer, obj, objId);
  writer.loadBooleanResult(prop.isFound());
  writer.returnFromIC();

  trackAttached("CheckPrivateField.Native");
  return AttachDecision::Attach;
}

void CheckPrivateFieldIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("base", val_);
    sp.valueProperty("property", idVal_);
  }
#endif
}

bool IRGenerator::maybeGuardInt32Index(const Value& index, ValOperandId indexId,
                                       uint32_t* int32Index,
                                       Int32OperandId* int32IndexId) {
  if (index.isNumber()) {
    int32_t indexSigned;
    if (index.isInt32()) {
      indexSigned = index.toInt32();
    } else {
      // We allow negative zero here.
      if (!mozilla::NumberEqualsInt32(index.toDouble(), &indexSigned)) {
        return false;
      }
    }

    if (indexSigned < 0) {
      return false;
    }

    *int32Index = uint32_t(indexSigned);
    *int32IndexId = EmitGuardToInt32Index(writer, index, indexId);
    return true;
  }

  if (index.isString()) {
    int32_t indexSigned = GetIndexFromString(index.toString());
    if (indexSigned < 0) {
      return false;
    }

    StringOperandId strId = writer.guardToString(indexId);
    *int32Index = uint32_t(indexSigned);
    *int32IndexId = writer.guardStringToIndex(strId);
    return true;
  }

  return false;
}

SetPropIRGenerator::SetPropIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, CacheKind cacheKind,
                                       ICState state, HandleValue lhsVal,
                                       HandleValue idVal, HandleValue rhsVal)
    : IRGenerator(cx, script, pc, cacheKind, state),
      lhsVal_(lhsVal),
      idVal_(idVal),
      rhsVal_(rhsVal) {}

AttachDecision SetPropIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId objValId(writer.setInputOperandId(0));
  ValOperandId rhsValId;
  if (cacheKind_ == CacheKind::SetProp) {
    rhsValId = ValOperandId(writer.setInputOperandId(1));
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    MOZ_ASSERT(setElemKeyValueId().id() == 1);
    writer.setInputOperandId(1);
    rhsValId = ValOperandId(writer.setInputOperandId(2));
  }

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  if (lhsVal_.isObject()) {
    RootedObject obj(cx_, &lhsVal_.toObject());

    ObjOperandId objId = writer.guardToObject(objValId);
    TRY_ATTACH(tryAttachSetTypedArrayElement(obj, objId, rhsValId));
    if (IsPropertySetOp(JSOp(*pc_))) {
      TRY_ATTACH(tryAttachMegamorphicSetElement(obj, objId, rhsValId));
    }
    if (nameOrSymbol) {
      TRY_ATTACH(tryAttachNativeSetSlot(obj, objId, id, rhsValId));
      if (IsPropertySetOp(JSOp(*pc_))) {
        TRY_ATTACH(tryAttachSetArrayLength(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachSetter(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachWindowProxy(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachProxy(obj, objId, id, rhsValId));
        TRY_ATTACH(tryAttachMegamorphicSetSlot(obj, objId, id, rhsValId));
      }
      if (canAttachAddSlotStub(obj, id)) {
        deferType_ = DeferType::AddSlot;
        return AttachDecision::Deferred;
      }
      return AttachDecision::NoAction;
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);

    if (IsPropertySetOp(JSOp(*pc_))) {
      TRY_ATTACH(tryAttachProxyElement(obj, objId, rhsValId));
    }

    uint32_t index;
    Int32OperandId indexId;
    if (maybeGuardInt32Index(idVal_, setElemKeyValueId(), &index, &indexId)) {
      TRY_ATTACH(
          tryAttachSetDenseElement(obj, objId, index, indexId, rhsValId));
      TRY_ATTACH(
          tryAttachSetDenseElementHole(obj, objId, index, indexId, rhsValId));
      TRY_ATTACH(tryAttachAddOrUpdateSparseElement(obj, objId, index, indexId,
                                                   rhsValId));
      return AttachDecision::NoAction;
    }
  }
  return AttachDecision::NoAction;
}

static void EmitStoreSlotAndReturn(CacheIRWriter& writer, ObjOperandId objId,
                                   NativeObject* nobj, PropertyInfo prop,
                                   ValOperandId rhsId) {
  if (nobj->isFixedSlot(prop.slot())) {
    size_t offset = NativeObject::getFixedSlotOffset(prop.slot());
    writer.storeFixedSlot(objId, offset, rhsId);
  } else {
    size_t offset = nobj->dynamicSlotIndex(prop.slot()) * sizeof(Value);
    writer.storeDynamicSlot(objId, offset, rhsId);
  }
  writer.returnFromIC();
}

static Maybe<PropertyInfo> LookupShapeForSetSlot(JSOp op, NativeObject* obj,
                                                 jsid id) {
  Maybe<PropertyInfo> prop = obj->lookupPure(id);
  if (prop.isNothing() || !prop->isDataProperty() || !prop->writable()) {
    return mozilla::Nothing();
  }

  // If this is a property init operation, the property's attributes may have to
  // be changed too, so make sure the current flags match.
  if (IsPropertyInitOp(op)) {
    // Don't support locked init operations.
    if (IsLockedInitOp(op)) {
      return mozilla::Nothing();
    }

    // Can't redefine a non-configurable property.
    if (!prop->configurable()) {
      return mozilla::Nothing();
    }

    // Make sure the enumerable flag matches the init operation.
    if (IsHiddenInitOp(op) == prop->enumerable()) {
      return mozilla::Nothing();
    }
  }

  return prop;
}

static bool CanAttachNativeSetSlot(JSOp op, JSObject* obj, PropertyKey id,
                                   Maybe<PropertyInfo>* prop) {
  if (!obj->is<NativeObject>()) {
    return false;
  }

  if (Watchtower::watchesPropertyValueChange(&obj->as<NativeObject>())) {
    return false;
  }

  *prop = LookupShapeForSetSlot(op, &obj->as<NativeObject>(), id);
  return prop->isSome();
}

// There is no need to guard on the shape. Global lexical bindings are
// non-configurable and can not be shadowed.
static bool IsGlobalLexicalSetGName(JSOp op, NativeObject* obj,
                                    PropertyInfo prop) {
  // Ensure that the env can't change.
  if (op != JSOp::SetGName && op != JSOp::StrictSetGName) {
    return false;
  }

  if (!obj->is<GlobalLexicalEnvironmentObject>()) {
    return false;
  }

  // Uninitialized let bindings use a RuntimeLexicalErrorObject.
  MOZ_ASSERT(!obj->getSlot(prop.slot()).isMagic());
  MOZ_ASSERT(prop.writable());
  MOZ_ASSERT(!prop.configurable());
  return true;
}

AttachDecision SetPropIRGenerator::tryAttachNativeSetSlot(HandleObject obj,
                                                          ObjOperandId objId,
                                                          HandleId id,
                                                          ValOperandId rhsId) {
  Maybe<PropertyInfo> prop;
  if (!CanAttachNativeSetSlot(JSOp(*pc_), obj, id, &prop)) {
    return AttachDecision::NoAction;
  }

  if (mode_ == ICState::Mode::Megamorphic && cacheKind_ == CacheKind::SetProp &&
      IsPropertySetOp(JSOp(*pc_))) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!IsGlobalLexicalSetGName(JSOp(*pc_), nobj, *prop)) {
    TestMatchingNativeReceiver(writer, nobj, objId);
  }
  EmitStoreSlotAndReturn(writer, objId, nobj, *prop, rhsId);

  trackAttached("SetProp.NativeSlot");
  return AttachDecision::Attach;
}

static bool ValueCanConvertToNumeric(Scalar::Type type, const Value& val) {
  if (Scalar::isBigIntType(type)) {
    return val.isBigInt();
  }
  return val.isNumber() || val.isNullOrUndefined() || val.isBoolean() ||
         val.isString();
}

OperandId IRGenerator::emitNumericGuard(ValOperandId valId, const Value& v,
                                        Scalar::Type type) {
  MOZ_ASSERT(ValueCanConvertToNumeric(type, v));
  switch (type) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32: {
      if (v.isNumber()) {
        return writer.guardToInt32ModUint32(valId);
      }
      if (v.isNullOrUndefined()) {
        writer.guardIsNullOrUndefined(valId);
        return writer.loadInt32Constant(0);
      }
      if (v.isBoolean()) {
        return writer.guardBooleanToInt32(valId);
      }
      MOZ_ASSERT(v.isString());
      StringOperandId strId = writer.guardToString(valId);
      NumberOperandId numId = writer.guardStringToNumber(strId);
      return writer.truncateDoubleToUInt32(numId);
    }

    case Scalar::Float16:
    case Scalar::Float32:
    case Scalar::Float64: {
      if (v.isNumber()) {
        return writer.guardIsNumber(valId);
      }
      if (v.isNull()) {
        writer.guardIsNull(valId);
        return writer.loadDoubleConstant(0.0);
      }
      if (v.isUndefined()) {
        writer.guardIsUndefined(valId);
        return writer.loadDoubleConstant(JS::GenericNaN());
      }
      if (v.isBoolean()) {
        BooleanOperandId boolId = writer.guardToBoolean(valId);
        return writer.booleanToNumber(boolId);
      }
      MOZ_ASSERT(v.isString());
      StringOperandId strId = writer.guardToString(valId);
      return writer.guardStringToNumber(strId);
    }

    case Scalar::Uint8Clamped: {
      if (v.isNumber()) {
        return writer.guardToUint8Clamped(valId);
      }
      if (v.isNullOrUndefined()) {
        writer.guardIsNullOrUndefined(valId);
        return writer.loadInt32Constant(0);
      }
      if (v.isBoolean()) {
        return writer.guardBooleanToInt32(valId);
      }
      MOZ_ASSERT(v.isString());
      StringOperandId strId = writer.guardToString(valId);
      NumberOperandId numId = writer.guardStringToNumber(strId);
      return writer.doubleToUint8Clamped(numId);
    }

    case Scalar::BigInt64:
    case Scalar::BigUint64:
      MOZ_ASSERT(v.isBigInt());
      return writer.guardToBigInt(valId);

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      break;
  }
  MOZ_CRASH("Unsupported TypedArray type");
}

void SetPropIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", JSOp(*pc_));
    sp.valueProperty("base", lhsVal_);
    sp.valueProperty("property", idVal_);
    sp.valueProperty("value", rhsVal_);
  }
#endif
}

static bool IsCacheableSetPropCallNative(NativeObject* obj,
                                         NativeObject* holder,
                                         PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (!prop.isAccessorProperty()) {
    return false;
  }

  JSObject* setterObject = holder->getSetter(prop);
  if (!setterObject || !setterObject->is<JSFunction>()) {
    return false;
  }

  JSFunction& setter = setterObject->as<JSFunction>();
  if (!setter.isNativeWithoutJitEntry()) {
    return false;
  }

  if (setter.isClassConstructor()) {
    return false;
  }

  return true;
}

static bool IsCacheableSetPropCallScripted(NativeObject* obj,
                                           NativeObject* holder,
                                           PropertyInfo prop) {
  MOZ_ASSERT(IsCacheableProtoChain(obj, holder));

  if (!prop.isAccessorProperty()) {
    return false;
  }

  JSObject* setterObject = holder->getSetter(prop);
  if (!setterObject || !setterObject->is<JSFunction>()) {
    return false;
  }

  JSFunction& setter = setterObject->as<JSFunction>();
  if (setter.isClassConstructor()) {
    return false;
  }

  // Scripted functions and natives with JIT entry can use the scripted path.
  return setter.hasJitEntry();
}

static bool CanAttachSetter(JSContext* cx, jsbytecode* pc, JSObject* obj,
                            PropertyKey id, NativeObject** holder,
                            Maybe<PropertyInfo>* propInfo) {
  // Don't attach a setter stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc)));

  PropertyResult prop;
  if (!LookupPropertyPure(cx, obj, id, holder, &prop)) {
    return false;
  }
  auto* nobj = &obj->as<NativeObject>();

  if (!prop.isNativeProperty()) {
    return false;
  }

  if (!IsCacheableSetPropCallScripted(nobj, *holder, prop.propertyInfo()) &&
      !IsCacheableSetPropCallNative(nobj, *holder, prop.propertyInfo())) {
    return false;
  }

  *propInfo = mozilla::Some(prop.propertyInfo());
  return true;
}

void SetPropIRGenerator::emitCallSetterNoGuards(NativeObject* obj,
                                                NativeObject* holder,
                                                PropertyInfo prop,
                                                ObjOperandId receiverId,
                                                ValOperandId rhsId) {
  JSFunction* target = &holder->getSetter(prop)->as<JSFunction>();
  bool sameRealm = cx_->realm() == target->realm();

  if (target->isNativeWithoutJitEntry()) {
    MOZ_ASSERT(IsCacheableSetPropCallNative(obj, holder, prop));
    writer.callNativeSetter(receiverId, target, rhsId, sameRealm);
    writer.returnFromIC();
    return;
  }

  MOZ_ASSERT(IsCacheableSetPropCallScripted(obj, holder, prop));
  writer.callScriptedSetter(receiverId, target, rhsId, sameRealm);
  writer.returnFromIC();
}

void SetPropIRGenerator::emitCallDOMSetterNoGuards(NativeObject* holder,
                                                   PropertyInfo prop,
                                                   ObjOperandId objId,
                                                   ValOperandId rhsId) {
  JSFunction* setter = &holder->getSetter(prop)->as<JSFunction>();
  MOZ_ASSERT(cx_->realm() == setter->realm());

  writer.callDOMSetter(objId, setter->jitInfo(), rhsId);
  writer.returnFromIC();
}

AttachDecision SetPropIRGenerator::tryAttachSetter(HandleObject obj,
                                                   ObjOperandId objId,
                                                   HandleId id,
                                                   ValOperandId rhsId) {
  // Don't attach a setter stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachSetter(cx_, pc_, obj, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  bool needsWindowProxy =
      IsWindow(nobj) && SetterNeedsWindowProxyThis(holder, *prop);

  maybeEmitIdGuard(id);

  // Use the megamorphic guard if we're in megamorphic mode, except if |obj|
  // is a Window as GuardHasGetterSetter doesn't support this yet (Window may
  // require outerizing).
  if (mode_ == ICState::Mode::Specialized || IsWindow(nobj)) {
    TestMatchingNativeReceiver(writer, nobj, objId);

    if (nobj != holder) {
      GeneratePrototypeGuards(writer, nobj, holder, objId);

      // Guard on the holder's shape.
      ObjOperandId holderId = writer.loadObject(holder);
      TestMatchingHolder(writer, holder, holderId);

      emitGuardGetterSetterSlot(holder, *prop, holderId, AccessorKind::Setter,
                                /* holderIsConstant = */ true);
    } else {
      emitGuardGetterSetterSlot(holder, *prop, objId, AccessorKind::Setter);
    }
  } else {
    Value val = holder->getSlot(prop->slot());
    MOZ_ASSERT(val.isPrivateGCThing());
    MOZ_ASSERT(val.toGCThing()->is<GetterSetter>());
    writer.guardHasGetterSetter(objId, id, val);
  }

  if (CanAttachDOMGetterSetter(cx_, JSJitInfo::Setter, nobj, holder, *prop,
                               mode_)) {
    MOZ_ASSERT(!needsWindowProxy);
    emitCallDOMSetterNoGuards(holder, *prop, objId, rhsId);

    trackAttached("SetProp.DOMSetter");
    return AttachDecision::Attach;
  }

  ObjOperandId receiverId;
  if (needsWindowProxy) {
    MOZ_ASSERT(cx_->global()->maybeWindowProxy());
    receiverId = writer.loadObject(cx_->global()->maybeWindowProxy());
  } else {
    receiverId = objId;
  }
  emitCallSetterNoGuards(nobj, holder, *prop, receiverId, rhsId);

  trackAttached("SetProp.Setter");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachSetArrayLength(HandleObject obj,
                                                           ObjOperandId objId,
                                                           HandleId id,
                                                           ValOperandId rhsId) {
  // Don't attach an array length stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  if (!obj->is<ArrayObject>() || !id.isAtom(cx_->names().length) ||
      !obj->as<ArrayObject>().lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);
  emitOptimisticClassGuard(objId, obj, GuardClassKind::Array);
  writer.callSetArrayLength(objId, IsStrictSetPC(pc_), rhsId);
  writer.returnFromIC();

  trackAttached("SetProp.ArrayLength");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachSetDenseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId rhsId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->containsDenseElement(index) || nobj->denseElementsAreFrozen()) {
    return AttachDecision::NoAction;
  }

  // Setting holes requires extra code for marking the elements non-packed.
  MOZ_ASSERT(!rhsVal_.isMagic(JS_ELEMENTS_HOLE));

  JSOp op = JSOp(*pc_);

  // We don't currently emit locked init for any indexed properties.
  MOZ_ASSERT(!IsLockedInitOp(op));

  // We don't currently emit hidden init for any existing indexed properties.
  MOZ_ASSERT(!IsHiddenInitOp(op));

  // Don't optimize InitElem (DefineProperty) on non-extensible objects: when
  // the elements are sealed, we have to throw an exception. Note that we have
  // to check !isExtensible instead of denseElementsAreSealed because sealing
  // a (non-extensible) object does not necessarily trigger a Shape change.
  if (IsPropertyInitOp(op) && !nobj->isExtensible()) {
    return AttachDecision::NoAction;
  }

  TestMatchingNativeReceiver(writer, nobj, objId);

  bool expectPackedElements = nobj->denseElementsArePacked();
  writer.storeDenseElement(objId, indexId, rhsId, expectPackedElements);
  writer.returnFromIC();

  trackAttached("SetProp.DenseElement");
  return AttachDecision::Attach;
}

static bool CanAttachAddElement(NativeObject* obj, bool isInit,
                                AllowIndexedReceiver allowIndexedReceiver) {
  MOZ_ASSERT(!obj->is<TypedArrayObject>());

  // Make sure the receiver doesn't have any indexed properties and that such
  // properties can't appear without a shape change.
  if (allowIndexedReceiver == AllowIndexedReceiver::No && obj->isIndexed()) {
    return false;
  }

  do {
    // This check is also relevant for the receiver object.
    const JSClass* clasp = obj->getClass();
    if (clasp != &ArrayObject::class_ &&
        (clasp->getAddProperty() || clasp->getResolve() ||
         clasp->getOpsLookupProperty() || clasp->getOpsSetProperty() ||
         obj->hasUnpreservedWrapper())) {
      return false;
    }

    // If we're initializing a property instead of setting one, the objects
    // on the prototype are not relevant.
    if (isInit) {
      break;
    }

    JSObject* proto = obj->staticPrototype();
    if (!proto) {
      break;
    }

    if (!proto->is<NativeObject>()) {
      return false;
    }

    // We shouldn't add an element if the index is OOB for a typed array on the
    // prototype chain.
    if (proto->is<TypedArrayObject>()) {
      return false;
    }

    NativeObject* nproto = &proto->as<NativeObject>();
    if (nproto->isIndexed()) {
      return false;
    }

    // We have to make sure the proto has no non-writable (frozen) elements
    // because we're not allowed to shadow them.
    if (nproto->denseElementsAreFrozen() &&
        nproto->getDenseInitializedLength() > 0) {
      return false;
    }

    obj = nproto;
  } while (true);

  return true;
}

AttachDecision SetPropIRGenerator::tryAttachSetDenseElementHole(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId rhsId) {
  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }

  // Setting holes requires extra code for marking the elements non-packed.
  if (rhsVal_.isMagic(JS_ELEMENTS_HOLE)) {
    return AttachDecision::NoAction;
  }

  JSOp op = JSOp(*pc_);
  MOZ_ASSERT(IsPropertySetOp(op) || IsPropertyInitOp(op));

  // We don't currently emit locked init for any indexed properties.
  MOZ_ASSERT(!IsLockedInitOp(op));

  // Hidden init can be emitted for absent indexed properties.
  if (IsHiddenInitOp(op)) {
    MOZ_ASSERT(op == JSOp::InitHiddenElem);
    return AttachDecision::NoAction;
  }

  NativeObject* nobj = &obj->as<NativeObject>();
  if (!nobj->isExtensible()) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(!nobj->denseElementsAreFrozen(),
             "Extensible objects should not have frozen elements");

  uint32_t initLength = nobj->getDenseInitializedLength();
  uint32_t capacity = nobj->getDenseCapacity();

  // Optimize if:
  // a) we're adding an element inside capacity, or one element past.
  // b) we're writing to a hole inside initLength.
  bool isAdd = index >= initLength && index <= capacity;
  bool isHoleInBounds =
      index < initLength && !nobj->containsDenseElement(index);
  if (!isAdd && !isHoleInBounds) {
    return AttachDecision::NoAction;
  }

  // Can't add new elements to arrays with non-writable length.
  if (isAdd && nobj->is<ArrayObject>() &&
      !nobj->as<ArrayObject>().lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  // Typed arrays don't have dense elements.
  if (nobj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Check for other indexed properties or class hooks.
  if (!CanAttachAddElement(nobj, IsPropertyInitOp(op),
                           AllowIndexedReceiver::No)) {
    return AttachDecision::NoAction;
  }

  TestMatchingNativeReceiver(writer, nobj, objId);

  // Also shape guard the proto chain, unless this is an InitElem.
  if (IsPropertySetOp(op)) {
    ShapeGuardProtoChain(writer, nobj, objId);
  }

  writer.storeDenseElementHole(objId, indexId, rhsId, isAdd);
  writer.returnFromIC();

  trackAttached(isAdd ? "AddDenseElement" : "StoreDenseElementHole");
  return AttachDecision::Attach;
}

// Add an IC for adding or updating a sparse element.
AttachDecision SetPropIRGenerator::tryAttachAddOrUpdateSparseElement(
    HandleObject obj, ObjOperandId objId, uint32_t index,
    Int32OperandId indexId, ValOperandId rhsId) {
  JSOp op = JSOp(*pc_);
  MOZ_ASSERT(IsPropertySetOp(op) || IsPropertyInitOp(op));

  if (op != JSOp::SetElem && op != JSOp::StrictSetElem) {
    return AttachDecision::NoAction;
  }

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  NativeObject* nobj = &obj->as<NativeObject>();

  // We cannot attach a stub to a non-extensible object
  if (!nobj->isExtensible()) {
    return AttachDecision::NoAction;
  }

  // Stub doesn't handle negative indices.
  if (index > INT32_MAX) {
    return AttachDecision::NoAction;
  }

  // The index must not be for a dense element.
  if (nobj->containsDenseElement(index)) {
    return AttachDecision::NoAction;
  }

  // Only handle ArrayObject and PlainObject in this stub.
  if (!nobj->is<ArrayObject>() && !nobj->is<PlainObject>()) {
    return AttachDecision::NoAction;
  }

  // Don't attach if we're adding to an array with non-writable length.
  if (nobj->is<ArrayObject>()) {
    ArrayObject* aobj = &nobj->as<ArrayObject>();
    bool isAdd = (index >= aobj->length());
    if (isAdd && !aobj->lengthIsWritable()) {
      return AttachDecision::NoAction;
    }
  }

  // Check for class hooks or indexed properties on the prototype chain that
  // we're not allowed to shadow.
  if (!CanAttachAddElement(nobj, /* isInit = */ false,
                           AllowIndexedReceiver::Yes)) {
    return AttachDecision::NoAction;
  }

  // Ensure that obj is an ArrayObject or PlainObject.
  if (nobj->is<ArrayObject>()) {
    writer.guardClass(objId, GuardClassKind::Array);
  } else {
    MOZ_ASSERT(nobj->is<PlainObject>());
    writer.guardClass(objId, GuardClassKind::PlainObject);
  }

  // The helper we are going to call only applies to non-dense elements.
  writer.guardIndexIsNotDenseElement(objId, indexId);

  // Guard extensible: We may be trying to add a new element, and so we'd best
  // be able to do so safely.
  writer.guardIsExtensible(objId);

  // Ensures we are able to efficiently able to map to an integral jsid.
  writer.guardInt32IsNonNegative(indexId);

  // Shape guard the prototype chain to avoid shadowing indexes from appearing.
  // Guard the prototype of the receiver explicitly, because the receiver's
  // shape is not being guarded as a proxy for that.
  GuardReceiverProto(writer, nobj, objId);

  // Dense elements may appear on the prototype chain (and prototypes may
  // have a different notion of which elements are dense), but they can
  // only be data properties, so our specialized Set handler is ok to bind
  // to them.
  if (IsPropertySetOp(op)) {
    ShapeGuardProtoChain(writer, nobj, objId);
  }

  // Ensure that if we're adding an element to the object, the object's
  // length is writable.
  if (nobj->is<ArrayObject>()) {
    writer.guardIndexIsValidUpdateOrAdd(objId, indexId);
  }

  writer.callAddOrUpdateSparseElementHelper(
      objId, indexId, rhsId,
      /* strict = */ op == JSOp::StrictSetElem);
  writer.returnFromIC();

  trackAttached("SetProp.AddOrUpdateSparseElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachSetTypedArrayElement(
    HandleObject obj, ObjOperandId objId, ValOperandId rhsId) {
  if (!obj->is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!idVal_.isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* tarr = &obj->as<TypedArrayObject>();
  Scalar::Type elementType = tarr->type();

  // Immutable TypedArrays can't be modified.
  if (tarr->is<ImmutableTypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Don't attach if the input type doesn't match the guard added below.
  if (!ValueCanConvertToNumeric(elementType, rhsVal_)) {
    return AttachDecision::NoAction;
  }

  bool handleOOB = false;
  int64_t indexInt64;
  if (!ValueIsInt64Index(idVal_, &indexInt64) || indexInt64 < 0 ||
      uint64_t(indexInt64) >= tarr->length().valueOr(0)) {
    handleOOB = true;
  }

  JSOp op = JSOp(*pc_);

  // The only expected property init operation is InitElem.
  MOZ_ASSERT_IF(IsPropertyInitOp(op), op == JSOp::InitElem);

  // InitElem (DefineProperty) has to throw an exception on out-of-bounds.
  if (handleOOB && IsPropertyInitOp(op)) {
    return AttachDecision::NoAction;
  }

  writer.guardShapeForClass(objId, tarr->shape());

  OperandId rhsValId = emitNumericGuard(rhsId, rhsVal_, elementType);

  ValOperandId keyId = setElemKeyValueId();
  IntPtrOperandId indexId = guardToIntPtrIndex(idVal_, keyId, handleOOB);

  auto viewKind = ToArrayBufferViewKind(tarr);
  writer.storeTypedArrayElement(objId, elementType, indexId, rhsValId,
                                handleOOB, viewKind);
  writer.returnFromIC();

  trackAttached(handleOOB ? "SetTypedElementOOB" : "SetTypedElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachGenericProxy(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId, bool handleDOMProxies) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  writer.guardIsProxy(objId);

  if (!handleDOMProxies) {
    // Ensure that the incoming object is not a DOM proxy, so that we can
    // get to the specialized stubs. If handleDOMProxies is true, we were
    // unable to attach a specialized DOM stub, so we just handle all
    // proxies here.
    writer.guardIsNotDOMProxy(objId);
  }

  if (cacheKind_ == CacheKind::SetProp || mode_ == ICState::Mode::Specialized) {
    maybeEmitIdGuard(id);
    writer.proxySet(objId, id, rhsId, IsStrictSetPC(pc_));
  } else {
    // Attach a stub that handles every id.
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    MOZ_ASSERT(mode_ == ICState::Mode::Megamorphic);
    writer.proxySetByValue(objId, setElemKeyValueId(), rhsId,
                           IsStrictSetPC(pc_));
  }

  writer.returnFromIC();

  trackAttached("SetProp.GenericProxy");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachDOMProxyShadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  maybeEmitIdGuard(id);
  TestMatchingProxyReceiver(writer, obj, objId);
  writer.proxySet(objId, id, rhsId, IsStrictSetPC(pc_));
  writer.returnFromIC();

  trackAttached("SetProp.DOMProxyShadowed");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachDOMProxyUnshadowed(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  JSObject* proto = obj->staticPrototype();
  if (!proto) {
    return AttachDecision::NoAction;
  }

  NativeObject* holder = nullptr;
  Maybe<PropertyInfo> prop;
  if (!CanAttachSetter(cx_, pc_, proto, id, &holder, &prop)) {
    return AttachDecision::NoAction;
  }
  auto* nproto = &proto->as<NativeObject>();

  maybeEmitIdGuard(id);

  // Guard that our proxy (expando) object hasn't started shadowing this
  // property.
  TestMatchingProxyReceiver(writer, obj, objId);
  bool canOptimizeMissing = false;
  CheckDOMProxyDoesNotShadow(writer, obj, id, objId, &canOptimizeMissing);

  GeneratePrototypeGuards(writer, obj, holder, objId);

  // Guard on the holder of the property.
  ObjOperandId holderId = writer.loadObject(holder);
  TestMatchingHolder(writer, holder, holderId);

  emitGuardGetterSetterSlot(holder, *prop, holderId, AccessorKind::Setter,
                            /* holderIsConstant = */ true);

  // EmitCallSetterNoGuards expects |obj| to be the object the property is
  // on to do some checks. Since we actually looked at proto, and no extra
  // guards will be generated, we can just pass that instead.
  emitCallSetterNoGuards(nproto, holder, *prop, objId, rhsId);

  trackAttached("SetProp.DOMProxyUnshadowed");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachDOMProxyExpando(
    Handle<ProxyObject*> obj, ObjOperandId objId, HandleId id,
    ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  MOZ_ASSERT(IsCacheableDOMProxy(obj));

  Value expandoVal = GetProxyPrivate(obj);
  JSObject* expandoObj;
  if (expandoVal.isObject()) {
    expandoObj = &expandoVal.toObject();
  } else {
    MOZ_ASSERT(!expandoVal.isUndefined(),
               "How did a missing expando manage to shadow things?");
    auto expandoAndGeneration =
        static_cast<ExpandoAndGeneration*>(expandoVal.toPrivate());
    MOZ_ASSERT(expandoAndGeneration);
    expandoObj = &expandoAndGeneration->expando.toObject();
  }

  Maybe<PropertyInfo> prop;
  if (CanAttachNativeSetSlot(JSOp(*pc_), expandoObj, id, &prop)) {
    auto* nativeExpandoObj = &expandoObj->as<NativeObject>();

    maybeEmitIdGuard(id);
    ObjOperandId expandoObjId = guardDOMProxyExpandoObjectAndShape(
        obj, objId, expandoVal, nativeExpandoObj);

    EmitStoreSlotAndReturn(writer, expandoObjId, nativeExpandoObj, *prop,
                           rhsId);
    trackAttached("SetProp.DOMProxyExpandoSlot");
    return AttachDecision::Attach;
  }

  NativeObject* holder = nullptr;
  if (CanAttachSetter(cx_, pc_, expandoObj, id, &holder, &prop)) {
    auto* nativeExpandoObj = &expandoObj->as<NativeObject>();

    // Call the setter. Note that we pass objId, the DOM proxy, as |this|
    // and not the expando object.
    maybeEmitIdGuard(id);
    ObjOperandId expandoObjId = guardDOMProxyExpandoObjectAndShape(
        obj, objId, expandoVal, nativeExpandoObj);

    MOZ_ASSERT(holder == nativeExpandoObj);
    emitGuardGetterSetterSlot(nativeExpandoObj, *prop, expandoObjId,
                              AccessorKind::Setter);
    emitCallSetterNoGuards(nativeExpandoObj, nativeExpandoObj, *prop, objId,
                           rhsId);
    trackAttached("SetProp.DOMProxyExpandoSetter");
    return AttachDecision::Attach;
  }

  return AttachDecision::NoAction;
}

AttachDecision SetPropIRGenerator::tryAttachProxy(HandleObject obj,
                                                  ObjOperandId objId,
                                                  HandleId id,
                                                  ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  ProxyStubType type = GetProxyStubType(cx_, obj, id);
  if (type == ProxyStubType::None) {
    return AttachDecision::NoAction;
  }
  auto proxy = obj.as<ProxyObject>();

  if (mode_ == ICState::Mode::Megamorphic) {
    return tryAttachGenericProxy(proxy, objId, id, rhsId,
                                 /* handleDOMProxies = */ true);
  }

  switch (type) {
    case ProxyStubType::None:
      break;
    case ProxyStubType::DOMExpando:
      TRY_ATTACH(tryAttachDOMProxyExpando(proxy, objId, id, rhsId));
      [[fallthrough]];  // Fall through to the generic shadowed case.
    case ProxyStubType::DOMShadowed:
      return tryAttachDOMProxyShadowed(proxy, objId, id, rhsId);
    case ProxyStubType::DOMUnshadowed:
      TRY_ATTACH(tryAttachDOMProxyUnshadowed(proxy, objId, id, rhsId));
      return tryAttachGenericProxy(proxy, objId, id, rhsId,
                                   /* handleDOMProxies = */ true);
    case ProxyStubType::Generic:
      return tryAttachGenericProxy(proxy, objId, id, rhsId,
                                   /* handleDOMProxies = */ false);
  }

  MOZ_CRASH("Unexpected ProxyStubType");
}

AttachDecision SetPropIRGenerator::tryAttachProxyElement(HandleObject obj,
                                                         ObjOperandId objId,
                                                         ValOperandId rhsId) {
  // Don't attach a proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  if (!obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  writer.guardIsProxy(objId);

  // Like GetPropIRGenerator::tryAttachProxyElement, don't check for DOM
  // proxies here as we don't have specialized DOM stubs for this.
  MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
  writer.proxySetByValue(objId, setElemKeyValueId(), rhsId, IsStrictSetPC(pc_));
  writer.returnFromIC();

  trackAttached("SetProp.ProxyElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachMegamorphicSetElement(
    HandleObject obj, ObjOperandId objId, ValOperandId rhsId) {
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  if (mode_ != ICState::Mode::Megamorphic || cacheKind_ != CacheKind::SetElem) {
    return AttachDecision::NoAction;
  }

  // The generic proxy stubs are faster.
  if (obj->is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  writer.megamorphicSetElement(objId, setElemKeyValueId(), rhsId,
                               IsStrictSetPC(pc_));
  writer.returnFromIC();

  trackAttached("SetProp.MegamorphicSetElement");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachMegamorphicSetSlot(
    HandleObject obj, ObjOperandId objId, HandleId id, ValOperandId rhsId) {
  if (mode_ != ICState::Mode::Megamorphic || cacheKind_ != CacheKind::SetProp) {
    return AttachDecision::NoAction;
  }

  writer.megamorphicStoreSlot(objId, id, rhsId, IsStrictSetPC(pc_));
  writer.returnFromIC();
  trackAttached("SetProp.MegamorphicNativeSlot");
  return AttachDecision::Attach;
}

AttachDecision SetPropIRGenerator::tryAttachWindowProxy(HandleObject obj,
                                                        ObjOperandId objId,
                                                        HandleId id,
                                                        ValOperandId rhsId) {
  // Don't attach a window proxy stub for ops like JSOp::InitElem.
  MOZ_ASSERT(IsPropertySetOp(JSOp(*pc_)));

  // Attach a stub when the receiver is a WindowProxy and we can do the set
  // on the Window (the global object).

  if (!IsWindowProxyForScriptGlobal(script_, obj)) {
    return AttachDecision::NoAction;
  }

  // If we're megamorphic prefer a generic proxy stub that handles a lot more
  // cases.
  if (mode_ == ICState::Mode::Megamorphic) {
    return AttachDecision::NoAction;
  }

  // Now try to do the set on the Window (the current global).
  GlobalObject* windowObj = cx_->global();

  Maybe<PropertyInfo> prop;
  if (!CanAttachNativeSetSlot(JSOp(*pc_), windowObj, id, &prop)) {
    return AttachDecision::NoAction;
  }

  maybeEmitIdGuard(id);

  ObjOperandId windowObjId =
      GuardAndLoadWindowProxyWindow(writer, objId, windowObj);
  writer.guardShape(windowObjId, windowObj->shape());

  EmitStoreSlotAndReturn(writer, windowObjId, windowObj, *prop, rhsId);

  trackAttached("SetProp.WindowProxySlot");
  return AttachDecision::Attach;
}

// Detect if |id| refers to the 'prototype' property of a function object. This
// property is special-cased in canAttachAddSlotStub().
static bool IsFunctionPrototype(const JSAtomState& names, JSObject* obj,
                                PropertyKey id) {
  return obj->is<JSFunction>() && id.isAtom(names.prototype);
}

bool SetPropIRGenerator::canAttachAddSlotStub(HandleObject obj, HandleId id) {
  if (!obj->is<NativeObject>()) {
    return false;
  }
  auto* nobj = &obj->as<NativeObject>();

  // Special-case JSFunction resolve hook to allow redefining the 'prototype'
  // property without triggering lazy expansion of property and object
  // allocation.
  if (IsFunctionPrototype(cx_->names(), nobj, id)) {
    MOZ_ASSERT(ClassMayResolveId(cx_->names(), nobj->getClass(), id, nobj));

    // We're only interested in functions that have a builtin .prototype
    // property (needsPrototypeProperty). The stub will guard on this because
    // the builtin .prototype property is non-configurable/non-enumerable and it
    // would be wrong to add a property with those attributes to a function that
    // doesn't have a builtin .prototype.
    //
    // Inlining needsPrototypeProperty in JIT code is complicated so we use
    // isNonBuiltinConstructor as a stronger condition that's easier to check
    // from JIT code.
    JSFunction* fun = &nobj->as<JSFunction>();
    if (!fun->isNonBuiltinConstructor()) {
      return false;
    }
    MOZ_ASSERT(fun->needsPrototypeProperty());

    // If property exists this isn't an "add".
    if (fun->lookupPure(id)) {
      return false;
    }
  } else {
    // Normal Case: If property exists this isn't an "add"
    PropertyResult prop;
    if (!LookupOwnPropertyPure(cx_, nobj, id, &prop)) {
      return false;
    }
    if (prop.isFound()) {
      return false;
    }
  }

  // For now we don't optimize Watchtower-monitored objects.
  if (Watchtower::watchesPropertyAdd(nobj)) {
    return false;
  }

  // Object must be extensible, or we must be initializing a private
  // elem.
  bool canAddNewProperty = nobj->isExtensible() || id.isPrivateName();
  if (!canAddNewProperty) {
    return false;
  }

  JSOp op = JSOp(*pc_);
  if (IsPropertyInitOp(op)) {
    return true;
  }

  MOZ_ASSERT(IsPropertySetOp(op));

  // Walk up the object prototype chain and ensure that all prototypes are
  // native, and that all prototypes have no setter defined on the property.
  for (JSObject* proto = nobj->staticPrototype(); proto;
       proto = proto->staticPrototype()) {
    if (!proto->is<NativeObject>()) {
      return false;
    }

    // If prototype defines this property in a non-plain way, don't optimize.
    Maybe<PropertyInfo> protoProp = proto->as<NativeObject>().lookup(cx_, id);
    if (protoProp.isSome() && !protoProp->isDataProperty()) {
      return false;
    }

    // Otherwise, if there's no such property, watch out for a resolve hook
    // that would need to be invoked and thus prevent inlining of property
    // addition. Allow the JSFunction resolve hook as it only defines plain
    // data properties and we don't need to invoke it for objects on the
    // proto chain.
    if (ClassMayResolveId(cx_->names(), proto->getClass(), id, proto) &&
        !proto->is<JSFunction>()) {
      return false;
    }
  }

  return true;
}

static PropertyFlags SetPropertyFlags(JSOp op, bool isFunctionPrototype) {
  // Locked properties are non-writable, non-enumerable, and non-configurable.
  if (IsLockedInitOp(op)) {
    return {};
  }

  // Hidden properties are writable, non-enumerable, and configurable.
  if (IsHiddenInitOp(op)) {
    return {
        PropertyFlag::Writable,
        PropertyFlag::Configurable,
    };
  }

  // This is a special case to overwrite an unresolved function.prototype
  // property. The initial property flags of this property are writable,
  // non-enumerable, and non-configurable. See canAttachAddSlotStub.
  if (isFunctionPrototype) {
    return {
        PropertyFlag::Writable,
    };
  }

  // Other properties are writable, enumerable, and configurable.
  return PropertyFlags::defaultDataPropFlags;
}

AttachDecision SetPropIRGenerator::tryAttachAddSlotStub(
    Handle<Shape*> oldShape) {
  ValOperandId objValId(writer.setInputOperandId(0));
  ValOperandId rhsValId;
  if (cacheKind_ == CacheKind::SetProp) {
    rhsValId = ValOperandId(writer.setInputOperandId(1));
  } else {
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    MOZ_ASSERT(setElemKeyValueId().id() == 1);
    writer.setInputOperandId(1);
    rhsValId = ValOperandId(writer.setInputOperandId(2));
  }

  RootedId id(cx_);
  bool nameOrSymbol;
  if (!ValueToNameOrSymbolId(cx_, idVal_, &id, &nameOrSymbol)) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  if (!lhsVal_.isObject() || !nameOrSymbol) {
    return AttachDecision::NoAction;
  }

  JSObject* obj = &lhsVal_.toObject();

  PropertyResult prop;
  if (!LookupOwnPropertyPure(cx_, obj, id, &prop)) {
    return AttachDecision::NoAction;
  }
  if (prop.isNotFound()) {
    return AttachDecision::NoAction;
  }

  if (!obj->is<NativeObject>()) {
    return AttachDecision::NoAction;
  }
  auto* nobj = &obj->as<NativeObject>();

  PropertyInfo propInfo = prop.propertyInfo();
  NativeObject* holder = nobj;

  if (holder->inDictionaryMode()) {
    return AttachDecision::NoAction;
  }

  SharedShape* oldSharedShape = &oldShape->asShared();

  // The property must be the last added property of the object.
  SharedShape* newShape = holder->sharedShape();
  MOZ_RELEASE_ASSERT(newShape->lastProperty() == propInfo);

#ifdef DEBUG
  // Verify exactly one property was added by comparing the property map
  // lengths.
  if (oldSharedShape->propMapLength() == PropMap::Capacity) {
    MOZ_ASSERT(newShape->propMapLength() == 1);
  } else {
    MOZ_ASSERT(newShape->propMapLength() ==
               oldSharedShape->propMapLength() + 1);
  }
#endif

  bool isFunctionPrototype = IsFunctionPrototype(cx_->names(), nobj, id);

  JSOp op = JSOp(*pc_);
  PropertyFlags flags = SetPropertyFlags(op, isFunctionPrototype);

  // Basic property checks.
  if (!propInfo.isDataProperty() || propInfo.flags() != flags) {
    return AttachDecision::NoAction;
  }

  ObjOperandId objId = writer.guardToObject(objValId);
  maybeEmitIdGuard(id);

  // Shape guard the object.
  writer.guardShape(objId, oldShape);

  // If this is the special function.prototype case, we need to guard the
  // function is a non-builtin constructor. See canAttachAddSlotStub.
  if (isFunctionPrototype) {
    MOZ_ASSERT(nobj->as<JSFunction>().isNonBuiltinConstructor());
    writer.guardFunctionIsNonBuiltinCtor(objId);
  }

  // Also shape guard the proto chain, unless this is an InitElem.
  if (IsPropertySetOp(op)) {
    ShapeGuardProtoChain(writer, nobj, objId);
  }

  // If the JSClass has an addProperty hook, we need to call a VM function to
  // invoke this hook. Ignore the Array addProperty hook, because it doesn't do
  // anything for non-index properties.
  DebugOnly<uint32_t> index;
  MOZ_ASSERT_IF(obj->is<ArrayObject>(), !IdIsIndex(id, &index));
  bool mustCallAddPropertyHook =
      !obj->is<ArrayObject>() &&
      (obj->getClass()->getAddProperty() ||
       (obj->getClass()->preservesWrapper() &&
        !oldShape->hasObjectFlag(ObjectFlag::HasPreservedWrapper)));

  if (mustCallAddPropertyHook) {
    writer.addSlotAndCallAddPropHook(objId, rhsValId, newShape);
    trackAttached("SetProp.AddSlotWithAddPropertyHook");
  } else if (holder->isFixedSlot(propInfo.slot())) {
    size_t offset = NativeObject::getFixedSlotOffset(propInfo.slot());
    writer.addAndStoreFixedSlot(objId, offset, rhsValId, newShape);
    trackAttached("SetProp.AddSlotFixed");
  } else {
    size_t offset = holder->dynamicSlotIndex(propInfo.slot()) * sizeof(Value);
    uint32_t numOldSlots = NativeObject::calculateDynamicSlots(oldSharedShape);
    uint32_t numNewSlots = holder->numDynamicSlots();
    if (numOldSlots == numNewSlots) {
      writer.addAndStoreDynamicSlot(objId, offset, rhsValId, newShape);
      trackAttached("SetProp.AddSlotDynamic");
    } else {
      MOZ_ASSERT(numNewSlots > numOldSlots);
      writer.allocateAndStoreDynamicSlot(objId, offset, rhsValId, newShape,
                                         numNewSlots);
      trackAttached("SetProp.AllocateSlot");
    }
  }
  writer.returnFromIC();

  return AttachDecision::Attach;
}

InstanceOfIRGenerator::InstanceOfIRGenerator(JSContext* cx, HandleScript script,
                                             jsbytecode* pc, ICState state,
                                             HandleValue lhs, HandleObject rhs)
    : IRGenerator(cx, script, pc, CacheKind::InstanceOf, state),
      lhsVal_(lhs),
      rhsObj_(rhs) {}

AttachDecision InstanceOfIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::InstanceOf);
  AutoAssertNoPendingException aanpe(cx_);

  // Ensure RHS is a function -- could be a Proxy, which the IC isn't prepared
  // to handle.
  if (!rhsObj_->is<JSFunction>()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  HandleFunction fun = rhsObj_.as<JSFunction>();

  // Look up the @@hasInstance property, and check that Function.__proto__ is
  // the property holder, and that no object further down the prototype chain
  // (including this function) has shadowed it; together with the fact that
  // Function.__proto__[@@hasInstance] is immutable, this ensures that the
  // hasInstance hook will not change without the need to guard on the actual
  // property value.
  PropertyResult hasInstanceProp;
  NativeObject* hasInstanceHolder = nullptr;
  jsid hasInstanceID = PropertyKey::Symbol(cx_->wellKnownSymbols().hasInstance);
  if (!LookupPropertyPure(cx_, fun, hasInstanceID, &hasInstanceHolder,
                          &hasInstanceProp) ||
      !hasInstanceProp.isNativeProperty()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  JSObject& funProto = cx_->global()->getPrototype(JSProto_Function);
  if (hasInstanceHolder != &funProto) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  // If the above succeeded, then these should be true about @@hasInstance,
  // because the property on Function.__proto__ is an immutable data property:
  MOZ_ASSERT(hasInstanceProp.propertyInfo().isDataProperty());
  MOZ_ASSERT(!hasInstanceProp.propertyInfo().configurable());
  MOZ_ASSERT(!hasInstanceProp.propertyInfo().writable());

  MOZ_ASSERT(IsCacheableProtoChain(fun, hasInstanceHolder));

  // Ensure that the function's prototype slot is the same.
  Maybe<PropertyInfo> prop = fun->lookupPure(cx_->names().prototype);
  if (prop.isNothing() || !prop->isDataProperty()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  uint32_t slot = prop->slot();
  MOZ_ASSERT(slot >= fun->numFixedSlots(), "Stub code relies on this");
  if (!fun->getSlot(slot).isObject()) {
    trackAttached(IRGenerator::NotAttached);
    return AttachDecision::NoAction;
  }

  // Abstract Objects
  ValOperandId lhs(writer.setInputOperandId(0));
  ValOperandId rhs(writer.setInputOperandId(1));

  ObjOperandId rhsId = writer.guardToObject(rhs);
  writer.guardShape(rhsId, fun->shape());

  // Ensure that the shapes up the prototype chain for the RHS remain the same
  // so that @@hasInstance is not shadowed by some intermediate prototype
  // object.
  if (hasInstanceHolder != fun) {
    GeneratePrototypeGuards(writer, fun, hasInstanceHolder, rhsId);
    ObjOperandId holderId = writer.loadObject(hasInstanceHolder);
    TestMatchingHolder(writer, hasInstanceHolder, holderId);
  }

  // Load the .prototype value and ensure it's an object.
  ValOperandId protoValId =
      writer.loadDynamicSlot(rhsId, slot - fun->numFixedSlots());
  ObjOperandId protoId = writer.guardToObject(protoValId);

  // Needn't guard LHS is object, because the actual stub can handle that
  // and correctly return false.
  writer.loadInstanceOfObjectResult(lhs, protoId);
  writer.returnFromIC();
  trackAttached("InstanceOf");
  return AttachDecision::Attach;
}

void InstanceOfIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("lhs", lhsVal_);
    sp.valueProperty("rhs", ObjectValue(*rhsObj_));
  }
#else
  // Silence Clang -Wunused-private-field warning.
  (void)lhsVal_;
#endif
}

TypeOfIRGenerator::TypeOfIRGenerator(JSContext* cx, HandleScript script,
                                     jsbytecode* pc, ICState state,
                                     HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::TypeOf, state), val_(value) {}

void TypeOfIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision TypeOfIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::TypeOf);

  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));

  TRY_ATTACH(tryAttachPrimitive(valId));
  TRY_ATTACH(tryAttachObject(valId));

  MOZ_ASSERT_UNREACHABLE("Failed to attach TypeOf");
  return AttachDecision::NoAction;
}

AttachDecision TypeOfIRGenerator::tryAttachPrimitive(ValOperandId valId) {
  if (!val_.isPrimitive()) {
    return AttachDecision::NoAction;
  }

  // Note: we don't use GuardIsNumber for int32 values because it's less
  // efficient in Warp (unboxing to double instead of int32).
  if (val_.isDouble()) {
    writer.guardIsNumber(valId);
  } else {
    writer.guardNonDoubleType(valId, val_.type());
  }

  writer.loadConstantStringResult(
      TypeName(js::TypeOfValue(val_), cx_->names()));
  writer.returnFromIC();
  writer.setTypeData(TypeData(JSValueType(val_.type())));
  trackAttached("TypeOf.Primitive");
  return AttachDecision::Attach;
}

AttachDecision TypeOfIRGenerator::tryAttachObject(ValOperandId valId) {
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId objId = writer.guardToObject(valId);
  writer.loadTypeOfObjectResult(objId);
  writer.returnFromIC();
  writer.setTypeData(TypeData(JSValueType(val_.type())));
  trackAttached("TypeOf.Object");
  return AttachDecision::Attach;
}

TypeOfEqIRGenerator::TypeOfEqIRGenerator(JSContext* cx, HandleScript script,
                                         jsbytecode* pc, ICState state,
                                         HandleValue value, JSType type,
                                         JSOp compareOp)
    : IRGenerator(cx, script, pc, CacheKind::TypeOfEq, state),
      val_(value),
      type_(type),
      compareOp_(compareOp) {}

void TypeOfEqIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
    sp.jstypeProperty("type", type_);
    sp.opcodeProperty("compareOp", compareOp_);
  }
#endif
}

AttachDecision TypeOfEqIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::TypeOfEq);

  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));

  TRY_ATTACH(tryAttachPrimitive(valId));
  TRY_ATTACH(tryAttachObject(valId));

  MOZ_ASSERT_UNREACHABLE("Failed to attach TypeOfEq");
  return AttachDecision::NoAction;
}

AttachDecision TypeOfEqIRGenerator::tryAttachPrimitive(ValOperandId valId) {
  if (!val_.isPrimitive()) {
    return AttachDecision::NoAction;
  }

  // Note: we don't use GuardIsNumber for int32 values because it's less
  // efficient in Warp (unboxing to double instead of int32).
  if (val_.isDouble()) {
    writer.guardIsNumber(valId);
  } else {
    writer.guardNonDoubleType(valId, val_.type());
  }

  bool result = js::TypeOfValue(val_) == type_;
  if (compareOp_ == JSOp::Ne) {
    result = !result;
  }
  writer.loadBooleanResult(result);
  writer.returnFromIC();
  writer.setTypeData(TypeData(JSValueType(val_.type())));
  trackAttached("TypeOfEq.Primitive");
  return AttachDecision::Attach;
}

AttachDecision TypeOfEqIRGenerator::tryAttachObject(ValOperandId valId) {
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId objId = writer.guardToObject(valId);
  writer.loadTypeOfEqObjectResult(objId, TypeofEqOperand(type_, compareOp_));
  writer.returnFromIC();
  writer.setTypeData(TypeData(JSValueType(val_.type())));
  trackAttached("TypeOfEq.Object");
  return AttachDecision::Attach;
}

GetIteratorIRGenerator::GetIteratorIRGenerator(JSContext* cx,
                                               HandleScript script,
                                               jsbytecode* pc, ICState state,
                                               HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::GetIterator, state), val_(value) {}

AttachDecision GetIteratorIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::GetIterator);

  AutoAssertNoPendingException aanpe(cx_);

  ValOperandId valId(writer.setInputOperandId(0));

  TRY_ATTACH(tryAttachObject(valId));
  TRY_ATTACH(tryAttachNullOrUndefined(valId));
  TRY_ATTACH(tryAttachGeneric(valId));

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision GetIteratorIRGenerator::tryAttachObject(ValOperandId valId) {
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(val_.toObject().compartment() == cx_->compartment());

  ObjOperandId objId = writer.guardToObject(valId);
  writer.objectToIteratorResult(objId, cx_->compartment()->enumeratorsAddr());
  writer.returnFromIC();

  trackAttached("GetIterator.Object");
  return AttachDecision::Attach;
}

AttachDecision GetIteratorIRGenerator::tryAttachNullOrUndefined(
    ValOperandId valId) {
  MOZ_ASSERT(JSOp(*pc_) == JSOp::Iter);

  // For null/undefined we can simply return the empty iterator singleton. This
  // works because this iterator is unlinked and immutable.

  if (!val_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  PropertyIteratorObject* emptyIter =
      GlobalObject::getOrCreateEmptyIterator(cx_);
  if (!emptyIter) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  writer.guardIsNullOrUndefined(valId);

  ObjOperandId iterId = writer.loadObject(emptyIter);
  writer.loadObjectResult(iterId);
  writer.returnFromIC();

  trackAttached("GetIterator.NullOrUndefined");
  return AttachDecision::Attach;
}

AttachDecision GetIteratorIRGenerator::tryAttachGeneric(ValOperandId valId) {
  writer.valueToIteratorResult(valId);
  writer.returnFromIC();

  trackAttached("GetIterator.Generic");
  return AttachDecision::Attach;
}

void GetIteratorIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

OptimizeSpreadCallIRGenerator::OptimizeSpreadCallIRGenerator(
    JSContext* cx, HandleScript script, jsbytecode* pc, ICState state,
    HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::OptimizeSpreadCall, state),
      val_(value) {}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::OptimizeSpreadCall);

  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachArray());
  TRY_ATTACH(tryAttachArguments());
  TRY_ATTACH(tryAttachNotOptimizable());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachArray() {
  if (!isFirstStub_) {
    return AttachDecision::NoAction;
  }

  // The value must be a packed array.
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }
  Rooted<JSObject*> obj(cx_, &val_.toObject());
  if (!IsArrayWithDefaultIterator<MustBePacked::Yes>(obj, cx_)) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  ObjOperandId objId = writer.guardToObject(valId);

  // Guard the object is a packed array with Array.prototype as proto.
  MOZ_ASSERT(obj->is<ArrayObject>());
  writer.guardShape(objId, obj->shape());
  writer.guardArrayIsPacked(objId);

  // Ensure Array.prototype[@@iterator] and %ArrayIteratorPrototype%.next
  // haven't been mutated.
  writer.guardFuse(RealmFuses::FuseIndex::OptimizeGetIteratorFuse);

  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("OptimizeSpreadCall.Array");
  return AttachDecision::Attach;
}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachArguments() {
  // The value must be an arguments object.
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }
  RootedObject obj(cx_, &val_.toObject());
  if (!obj->is<ArgumentsObject>()) {
    return AttachDecision::NoAction;
  }
  auto args = obj.as<ArgumentsObject>();

  // Ensure neither elements, nor the length, nor the iterator has been
  // overridden. Also ensure no args are forwarded to allow reading them
  // directly from the frame.
  if (args->hasOverriddenElement() || args->hasOverriddenLength() ||
      args->hasOverriddenIterator() || args->anyArgIsForwarded()) {
    return AttachDecision::NoAction;
  }

  // Don't optimize arguments objects from a different realm because in this
  // case we have to use the other realm's %ArrayIteratorPrototype% object.
  if (cx_->realm() != args->realm()) {
    return AttachDecision::NoAction;
  }

  if (!HasOptimizableArrayIteratorPrototype(cx_)) {
    return AttachDecision::NoAction;
  }

  Rooted<Shape*> shape(cx_, GlobalObject::getArrayShapeWithDefaultProto(cx_));
  if (!shape) {
    cx_->clearPendingException();
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  ObjOperandId objId = writer.guardToObject(valId);

  if (args->is<MappedArgumentsObject>()) {
    writer.guardClass(objId, GuardClassKind::MappedArguments);
  } else {
    MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
    writer.guardClass(objId, GuardClassKind::UnmappedArguments);
  }
  uint8_t flags = ArgumentsObject::ELEMENT_OVERRIDDEN_BIT |
                  ArgumentsObject::LENGTH_OVERRIDDEN_BIT |
                  ArgumentsObject::ITERATOR_OVERRIDDEN_BIT |
                  ArgumentsObject::FORWARDED_ARGUMENTS_BIT;
  writer.guardArgumentsObjectFlags(objId, flags);
  writer.guardObjectHasSameRealm(objId);

  writer.guardFuse(RealmFuses::FuseIndex::OptimizeArrayIteratorPrototypeFuse);

  writer.arrayFromArgumentsObjectResult(objId, shape);
  writer.returnFromIC();

  trackAttached("OptimizeSpreadCall.Arguments");
  return AttachDecision::Attach;
}

AttachDecision OptimizeSpreadCallIRGenerator::tryAttachNotOptimizable() {
  ValOperandId valId(writer.setInputOperandId(0));

  writer.loadUndefinedResult();
  writer.returnFromIC();

  trackAttached("OptimizeSpreadCall.NotOptimizable");
  return AttachDecision::Attach;
}

void OptimizeSpreadCallIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

CallIRGenerator::CallIRGenerator(JSContext* cx, HandleScript script,
                                 jsbytecode* pc, JSOp op, ICState state,
                                 BaselineFrame* frame, uint32_t argc,
                                 HandleValue callee, HandleValue thisval,
                                 HandleValue newTarget, HandleValueArray args)
    : IRGenerator(cx, script, pc, CacheKind::Call, state, frame),
      op_(op),
      argc_(argc),
      callee_(callee),
      thisval_(thisval),
      newTarget_(newTarget),
      args_(args) {}

bool InlinableNativeIRGenerator::isCalleeBoundFunction() const {
  return callee()->is<BoundFunctionObject>();
}

BoundFunctionObject* InlinableNativeIRGenerator::boundCallee() const {
  MOZ_ASSERT(isCalleeBoundFunction());
  return &callee()->as<BoundFunctionObject>();
}

bool InlinableNativeIRGenerator::isTargetBoundFunction() const {
  switch (flags_.getArgFormat()) {
    case CallFlags::Standard:
    case CallFlags::Spread:
      return false;
    case CallFlags::FunCall:
    case CallFlags::FunApplyArgsObj:
    case CallFlags::FunApplyArray:
    case CallFlags::FunApplyNullUndefined:
      if (callee()->is<JSFunction>()) {
        MOZ_ASSERT(generator_.thisval_.isObject());
        return generator_.thisval_.toObject().is<BoundFunctionObject>();
      }
      return false;
    case CallFlags::Unknown:
      break;
  }
  MOZ_CRASH("Unsupported arg format");
}

BoundFunctionObject* InlinableNativeIRGenerator::boundTarget() const {
  MOZ_ASSERT(isTargetBoundFunction());
  return &generator_.thisval_.toObject().as<BoundFunctionObject>();
}

ObjOperandId InlinableNativeIRGenerator::emitNativeCalleeGuard(
    Int32OperandId argcId) {
  // Note: we rely on GuardSpecificFunction to also guard against the same
  // native from a different realm.
  MOZ_ASSERT(target_->isNativeWithoutJitEntry());

  ValOperandId calleeValId;
  switch (flags_.getArgFormat()) {
    case CallFlags::Standard:
    case CallFlags::Spread:
      calleeValId = writer.loadArgumentFixedSlot(ArgumentKind::Callee,
                                                 stackArgc(), flags_);
      break;
    case CallFlags::FunCall:
    case CallFlags::FunApplyArray:
    case CallFlags::FunApplyNullUndefined:
      calleeValId = writer.loadArgumentFixedSlot(
          ArgumentKind::Callee, stackArgc(), CallFlags(CallFlags::Standard));
      break;
    case CallFlags::Unknown:
    case CallFlags::FunApplyArgsObj:
      MOZ_CRASH("Unsupported arg format");
  }

  // Guard that |callee| is an object.
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);
  ObjOperandId targetId = calleeObjId;

  // The callee is a bound function whose bound target is |target_|.
  //
  // Example:
  // ```
  // var boundPush = Array.prototype.push.bind(arr);
  // boundPush(1);
  // ```
  //
  // Relevant generator members:
  // - |CallIRGenerator::callee_| is `boundPush`
  // - |InlinableNativeIRGenerator::target_| is `Array.prototype.push`
  //
  // Also see tryAttachBound{Native,FunCall,FunApply}.
  if (isCalleeBoundFunction()) {
    // Ensure the callee is a bound function.
    writer.guardClass(calleeObjId, GuardClassKind::BoundFunction);

    // Ensure numBoundArgs matches.
    size_t numBoundArgs = boundCallee()->numBoundArgs();
    Int32OperandId numBoundArgsId =
        writer.loadBoundFunctionNumArgs(calleeObjId);
    writer.guardSpecificInt32(numBoundArgsId, numBoundArgs);

    // Load the bound function target.
    targetId = writer.loadBoundFunctionTarget(calleeObjId);
  }

  if (flags_.getArgFormat() == CallFlags::FunCall ||
      flags_.getArgFormat() == CallFlags::FunApplyArray ||
      flags_.getArgFormat() == CallFlags::FunApplyNullUndefined) {
    JSFunction* funCallOrApply;
    ValOperandId thisValId;
    if (isCalleeBoundFunction()) {
      MOZ_ASSERT(flags_.getArgFormat() == CallFlags::FunCall ||
                     flags_.getArgFormat() == CallFlags::FunApplyNullUndefined,
                 "unexpected bound function");

      funCallOrApply = &boundCallee()->getTarget()->as<JSFunction>();
      thisValId = writer.loadFixedSlot(
          calleeObjId, BoundFunctionObject::offsetOfBoundThisSlot());
    } else {
      funCallOrApply = &callee()->as<JSFunction>();
      thisValId = writer.loadArgumentFixedSlot(ArgumentKind::This, stackArgc(),
                                               CallFlags(CallFlags::Standard));
    }
    MOZ_ASSERT(funCallOrApply->native() == fun_call ||
               funCallOrApply->native() == fun_apply);

    // Guard that |target| is the |fun_call| or |fun_apply| native function.
    writer.guardSpecificFunction(targetId, funCallOrApply);

    // Guard that |this| is an object.
    targetId = writer.guardToObject(thisValId);
  }

  // The callee calls a bound function whose bound target is |target_|.
  //
  // For example:
  // ```
  // var boundPush = Array.prototype.push.bind(arr);
  // boundPush.call(null, 1);
  // ```
  //
  // Relevant generator members:
  // - |CallIRGenerator::callee_| is `Function.prototype.call`
  // - |CallIRGenerator::thisval_| is `boundPush`
  // - |InlinableNativeIRGenerator::target_| is `Array.prototype.push`
  //
  // Also see tryAttach{FunCall,FunApply}Bound.
  if (isTargetBoundFunction()) {
    MOZ_ASSERT(!isCalleeBoundFunction(), "unexpected nested bound functions");
    MOZ_ASSERT(flags_.getArgFormat() == CallFlags::FunCall ||
                   flags_.getArgFormat() == CallFlags::FunApplyNullUndefined,
               "unsupported arg-format for bound target");

    // Ensure that |target| is a bound function.
    writer.guardClass(targetId, GuardClassKind::BoundFunction);

    // Ensure numBoundArgs matches.
    size_t numBoundArgs = boundTarget()->numBoundArgs();
    Int32OperandId numBoundArgsId = writer.loadBoundFunctionNumArgs(targetId);
    writer.guardSpecificInt32(numBoundArgsId, numBoundArgs);

    // Return the bound function as callee to support loading bound arguments.
    calleeObjId = targetId;

    // Load the bound function target.
    targetId = writer.loadBoundFunctionTarget(targetId);
  }

  writer.guardSpecificFunction(targetId, target_);

  // If we're constructing we also need to guard newTarget == callee.
  if (flags_.isConstructing()) {
    MOZ_ASSERT(flags_.getArgFormat() == CallFlags::Standard);
    MOZ_ASSERT(&newTarget_.toObject() == callee());

    ValOperandId newTargetValId = writer.loadArgumentFixedSlot(
        ArgumentKind::NewTarget, stackArgc(), flags_);
    ObjOperandId newTargetObjId = writer.guardToObject(newTargetValId);

    if (isCalleeBoundFunction()) {
      writer.guardObjectIdentity(newTargetObjId, calleeObjId);
    } else {
      writer.guardSpecificFunction(newTargetObjId, target_);
    }
  }

  // Guard the second argument is null or undefined.
  if (flags_.getArgFormat() == CallFlags::FunApplyNullUndefined) {
    constexpr size_t argIndex = 1;

    size_t numBoundArgs = 0;
    if (isCalleeBoundFunction()) {
      numBoundArgs = boundCallee()->numBoundArgs();
    }
    MOZ_ASSERT(numBoundArgs <= 2);

    ValOperandId argValId;
    if (argIndex < numBoundArgs) {
      argValId = loadBoundArgument(calleeObjId, argIndex);
    } else {
      auto argKind = ArgumentKindForArgIndex(argIndex - numBoundArgs);
      argValId = writer.loadArgumentFixedSlot(argKind, stackArgc(),
                                              CallFlags(CallFlags::Standard));
    }

    writer.guardIsNullOrUndefined(argValId);
  }

  return calleeObjId;
}

ObjOperandId InlinableNativeIRGenerator::emitLoadArgsArray() {
  MOZ_ASSERT(!hasBoundArguments());

  if (flags_.getArgFormat() == CallFlags::Spread) {
    return writer.loadSpreadArgs();
  }

  MOZ_ASSERT(flags_.getArgFormat() == CallFlags::FunApplyArray);
  return generator_.emitFunApplyArgsGuard(flags_.getArgFormat()).ref();
}

ValOperandId InlinableNativeIRGenerator::loadBoundArgument(
    ObjOperandId calleeId, size_t argIndex) {
  MOZ_ASSERT(isCalleeBoundFunction() || isTargetBoundFunction());

  auto* bound = isCalleeBoundFunction() ? boundCallee() : boundTarget();
  size_t numBoundArgs = bound->numBoundArgs();
  MOZ_ASSERT(argIndex < numBoundArgs);

  if (numBoundArgs <= BoundFunctionObject::MaxInlineBoundArgs) {
    constexpr size_t inlineArgsOffset =
        BoundFunctionObject::offsetOfFirstInlineBoundArg();

    size_t argSlot = inlineArgsOffset + argIndex * sizeof(Value);
    return writer.loadFixedSlot(calleeId, argSlot);
  }
  return writer.loadBoundFunctionArgument(calleeId, argIndex);
}

ValOperandId InlinableNativeIRGenerator::loadThis(ObjOperandId calleeId) {
  switch (flags_.getArgFormat()) {
    case CallFlags::Standard:
    case CallFlags::Spread:
      MOZ_ASSERT(!isTargetBoundFunction());
      if (isCalleeBoundFunction()) {
        return writer.loadFixedSlot(
            calleeId, BoundFunctionObject::offsetOfBoundThisSlot());
      }
      return writer.loadArgumentFixedSlot(ArgumentKind::This, stackArgc(),
                                          flags_);
    case CallFlags::FunCall:
    case CallFlags::FunApplyNullUndefined:
      // Load |this| from bound this.
      if (isTargetBoundFunction()) {
        return writer.loadFixedSlot(
            calleeId, BoundFunctionObject::offsetOfBoundThisSlot());
      }

      // Load |this| from bound arguments, if present.
      if (hasBoundArguments()) {
        MOZ_ASSERT(isCalleeBoundFunction());
        return loadBoundArgument(calleeId, 0);
      }

      // The stack layout is already in the correct form for calls with at least
      // one argument.
      //
      // clang-format off
      //
      // *** STACK LAYOUT (bottom to top) ***   *** INDEX ***
      //   Callee                               <-- argc+1
      //   ThisValue                            <-- argc
      //   Args: | Arg0 |                       <-- argc-1
      //         | Arg1 |                       <-- argc-2
      //         | ...  |                       <-- ...
      //         | ArgN |                       <-- 0
      //
      // When passing |argc-1| as the number of arguments, we get:
      //
      // *** STACK LAYOUT (bottom to top) ***   *** INDEX ***
      //   Callee                               <-- (argc-1)+1 = argc   = ThisValue
      //   ThisValue                            <-- (argc-1)   = argc-1 = Arg0
      //   Args: | Arg0   |                     <-- (argc-1)-1 = argc-2 = Arg1
      //         | Arg1   |                     <-- (argc-1)-2 = argc-3 = Arg2
      //         | ...    |                     <-- ...
      //
      // clang-format on
      //
      // This allows to call |loadArgumentFixedSlot(ArgumentKind::This)| and we
      // still load the correct argument index from |ArgumentKind::Arg0|.
      //
      // When no arguments are passed, i.e. |argc==0|, we have to replace
      // |ArgumentKind::Arg0| with the undefined value.
      if (stackArgc() == 0) {
        return writer.loadUndefined();
      }
      return writer.loadArgumentFixedSlot(ArgumentKind::This, stackArgc() - 1,
                                          CallFlags(CallFlags::Standard));
    case CallFlags::FunApplyArray:
    case CallFlags::FunApplyArgsObj:
      MOZ_ASSERT(stackArgc() > 0);
      MOZ_ASSERT(!isCalleeBoundFunction());
      MOZ_ASSERT(!isTargetBoundFunction());
      return writer.loadArgumentFixedSlot(ArgumentKind::This, stackArgc() - 1,
                                          CallFlags(CallFlags::Standard));
    case CallFlags::Unknown:
      break;
  }
  MOZ_CRASH("Unsupported arg format");
}

ValOperandId InlinableNativeIRGenerator::loadArgument(ObjOperandId calleeId,
                                                      ArgumentKind kind) {
  MOZ_ASSERT(kind >= ArgumentKind::Arg0);
  MOZ_ASSERT(flags_.getArgFormat() == CallFlags::Standard ||
             flags_.getArgFormat() == CallFlags::FunCall ||
             flags_.getArgFormat() == CallFlags::FunApplyNullUndefined);
  MOZ_ASSERT_IF(flags_.getArgFormat() == CallFlags::FunApplyNullUndefined,
                isTargetBoundFunction() && hasBoundArguments());

  // Check if the |this| value is stored in the bound arguments.
  bool thisFromBoundArgs = flags_.getArgFormat() == CallFlags::FunCall &&
                           isCalleeBoundFunction() && hasBoundArguments();

  if (hasBoundArguments()) {
    auto* bound = isCalleeBoundFunction() ? boundCallee() : boundTarget();
    size_t numBoundArgs = bound->numBoundArgs();
    size_t argIndex = uint8_t(kind) - uint8_t(ArgumentKind::Arg0);

    // Skip over the first bound argument, which stores the |this| value for
    // bound FunCall.
    if (thisFromBoundArgs) {
      argIndex += 1;
    }

    // Load from bound args.
    if (argIndex < numBoundArgs) {
      return loadBoundArgument(calleeId, argIndex);
    }

    // Load from stack arguments.
    kind = ArgumentKindForArgIndex(argIndex - numBoundArgs);
  }

  switch (flags_.getArgFormat()) {
    case CallFlags::Standard:
      return writer.loadArgumentFixedSlot(kind, stackArgc(), flags_);
    case CallFlags::FunCall:
      if (thisFromBoundArgs) {
        return writer.loadArgumentFixedSlot(kind, stackArgc(),
                                            CallFlags(CallFlags::Standard));
      }
      MOZ_ASSERT(stackArgc() > 1);
      // See |loadThis| for why we subtract |argc - 1| here.
      return writer.loadArgumentFixedSlot(kind, stackArgc() - 1,
                                          CallFlags(CallFlags::Standard));
    case CallFlags::Spread:
    case CallFlags::FunApplyArray:
    case CallFlags::FunApplyArgsObj:
    case CallFlags::FunApplyNullUndefined:
    case CallFlags::Unknown:
      break;
  }
  MOZ_CRASH("Unsupported arg format");
}

bool InlinableNativeIRGenerator::hasBoundArguments() const {
  if (isCalleeBoundFunction()) {
    return boundCallee()->numBoundArgs() != 0;
  }
  if (isTargetBoundFunction()) {
    return boundTarget()->numBoundArgs() != 0;
  }
  return false;
}

void IRGenerator::emitCalleeGuard(ObjOperandId calleeId, JSFunction* callee) {
  // Guarding on the callee JSFunction* is most efficient, but doesn't work well
  // for lambda clones (multiple functions with the same BaseScript). We guard
  // on the function's BaseScript if the callee is scripted and this isn't the
  // first IC stub.
  if (isFirstStub_ || !FunctionHasStableBaseScript(callee)) {
    writer.guardSpecificFunction(calleeId, callee);
  } else {
    MOZ_ASSERT_IF(callee->isSelfHostedBuiltin(),
                  !callee->baseScript()->allowRelazify());
    writer.guardClass(calleeId, GuardClassKind::JSFunction);
    writer.guardFunctionScript(calleeId, callee->baseScript());
  }
}

ObjOperandId CallIRGenerator::emitFunCallOrApplyGuard(Int32OperandId argcId) {
  JSFunction* callee = &callee_.toObject().as<JSFunction>();
  MOZ_ASSERT(callee->native() == fun_call || callee->native() == fun_apply);

  // |GetIndexOfArgument| doesn't yet support FunCall/FunApply.
  CallFlags flags(CallFlags::Standard);

  // Guard that callee is the |fun_call| or |fun_apply| native function.
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);
  writer.guardSpecificFunction(calleeObjId, callee);

  // Guard that |this| is an object.
  ValOperandId thisValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId, flags);
  return writer.guardToObject(thisValId);
}

ObjOperandId CallIRGenerator::emitFunCallGuard(Int32OperandId argcId) {
  MOZ_ASSERT(callee_.toObject().as<JSFunction>().native() == fun_call);

  return emitFunCallOrApplyGuard(argcId);
}

ObjOperandId CallIRGenerator::emitFunApplyGuard(Int32OperandId argcId) {
  MOZ_ASSERT(callee_.toObject().as<JSFunction>().native() == fun_apply);

  return emitFunCallOrApplyGuard(argcId);
}

Maybe<ObjOperandId> CallIRGenerator::emitFunApplyArgsGuard(
    CallFlags::ArgFormat format) {
  MOZ_ASSERT(argc_ == 2);

  // |GetIndexOfArgument| doesn't yet support FunCall/FunApply.
  CallFlags flags(CallFlags::Standard);

  ValOperandId argValId =
      writer.loadArgumentFixedSlot(ArgumentKind::Arg1, argc_, flags);

  if (format == CallFlags::FunApplyArgsObj) {
    ObjOperandId argObjId = writer.guardToObject(argValId);
    if (args_[1].toObject().is<MappedArgumentsObject>()) {
      writer.guardClass(argObjId, GuardClassKind::MappedArguments);
    } else {
      MOZ_ASSERT(args_[1].toObject().is<UnmappedArgumentsObject>());
      writer.guardClass(argObjId, GuardClassKind::UnmappedArguments);
    }
    uint8_t flags = ArgumentsObject::ELEMENT_OVERRIDDEN_BIT |
                    ArgumentsObject::FORWARDED_ARGUMENTS_BIT;
    writer.guardArgumentsObjectFlags(argObjId, flags);
    return mozilla::Some(argObjId);
  }

  if (format == CallFlags::FunApplyArray) {
    ObjOperandId argObjId = writer.guardToObject(argValId);
    emitOptimisticClassGuard(argObjId, &args_[1].toObject(),
                             GuardClassKind::Array);
    writer.guardArrayIsPacked(argObjId);
    return mozilla::Some(argObjId);
  }

  MOZ_ASSERT(format == CallFlags::FunApplyNullUndefined);
  writer.guardIsNullOrUndefined(argValId);
  return mozilla::Nothing();
}

AttachDecision InlinableNativeIRGenerator::tryAttachArrayPush() {
  // Only optimize on obj.push(val);
  if (args_.length() != 1 || !thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Where |obj| is a native array.
  JSObject* thisobj = &thisval_.toObject();
  if (!thisobj->is<ArrayObject>()) {
    return AttachDecision::NoAction;
  }

  auto* thisarray = &thisobj->as<ArrayObject>();

  // Check for other indexed properties or class hooks.
  if (!CanAttachAddElement(thisarray, /* isInit = */ false,
                           AllowIndexedReceiver::No)) {
    return AttachDecision::NoAction;
  }

  // Can't add new elements to arrays with non-writable length.
  if (!thisarray->lengthIsWritable()) {
    return AttachDecision::NoAction;
  }

  // Check that array is extensible.
  if (!thisarray->isExtensible()) {
    return AttachDecision::NoAction;
  }

  // Check that the array is completely initialized (no holes).
  if (thisarray->getDenseInitializedLength() != thisarray->length()) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(!thisarray->denseElementsAreFrozen(),
             "Extensible arrays should not have frozen elements");

  // After this point, we can generate code fine.

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'push' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is an array object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  // Guard that the shape matches.
  TestMatchingNativeReceiver(writer, thisarray, thisObjId);

  // Guard proto chain shapes.
  ShapeGuardProtoChain(writer, thisarray, thisObjId);

  // arr.push(x) is equivalent to arr[arr.length] = x for regular arrays.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  writer.arrayPush(thisObjId, argId);

  writer.returnFromIC();

  trackAttached("ArrayPush");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachArrayPopShift(
    InlinableNative native) {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Only optimize if |this| is a packed array.
  if (!thisval_.isObject() || !IsPackedArray(&thisval_.toObject())) {
    return AttachDecision::NoAction;
  }

  // Other conditions:
  //
  // * The array length needs to be writable because we're changing it.
  // * The array must be extensible. Non-extensible arrays require preserving
  //   the |initializedLength == capacity| invariant on ObjectElements.
  //   See NativeObject::shrinkCapacityToInitializedLength.
  //   This also ensures the elements aren't sealed/frozen.
  // * There must not be a for-in iterator for the elements because the IC stub
  //   does not suppress deleted properties.
  ArrayObject* arr = &thisval_.toObject().as<ArrayObject>();
  if (!arr->lengthIsWritable() || !arr->isExtensible() ||
      arr->denseElementsHaveMaybeInIterationFlag()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'pop' or 'shift' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, arr, GuardClassKind::Array);

  if (native == InlinableNative::ArrayPop) {
    writer.packedArrayPopResult(objId);
  } else {
    MOZ_ASSERT(native == InlinableNative::ArrayShift);
    writer.packedArrayShiftResult(objId);
  }

  writer.returnFromIC();

  trackAttached("ArrayPopShift");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachArrayJoin() {
  // Only handle argc <= 1.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }

  // Only optimize if |this| is an array.
  if (!thisval_.isObject() || !thisval_.toObject().is<ArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // The separator argument must be a string, if present.
  if (args_.length() > 0 && !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // IC stub code can handle non-packed array.

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'join' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is an array object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(thisObjId, &thisval_.toObject(),
                           GuardClassKind::Array);

  StringOperandId sepId;
  if (args_.length() == 1) {
    // If argcount is 1, guard that the argument is a string.
    ValOperandId argValId = loadArgument(calleeId, ArgumentKind::Arg0);
    sepId = writer.guardToString(argValId);
  } else {
    sepId = writer.loadConstantString(cx_->names().comma_);
  }

  // Do the join.
  writer.arrayJoinResult(thisObjId, sepId);

  writer.returnFromIC();

  trackAttached("ArrayJoin");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachArraySlice() {
  // Only handle argc <= 2.
  if (args_.length() > 2) {
    return AttachDecision::NoAction;
  }

  // Only optimize if |this| is a packed array or an arguments object.
  if (!thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  bool isPackedArray = IsPackedArray(&thisval_.toObject());
  if (!isPackedArray) {
    if (!thisval_.toObject().is<ArgumentsObject>()) {
      return AttachDecision::NoAction;
    }
    auto* args = &thisval_.toObject().as<ArgumentsObject>();

    // No elements must have been overridden or deleted.
    if (args->hasOverriddenElement()) {
      return AttachDecision::NoAction;
    }

    // The length property mustn't be overridden.
    if (args->hasOverriddenLength()) {
      return AttachDecision::NoAction;
    }

    // And finally also check that no argument is forwarded.
    if (args->anyArgIsForwarded()) {
      return AttachDecision::NoAction;
    }
  }

  // Arguments for the sliced region must be integers.
  if (args_.length() > 0 && !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }
  if (args_.length() > 1 && !args_[1].isInt32()) {
    return AttachDecision::NoAction;
  }

  JSObject* templateObj = NewDenseFullyAllocatedArray(cx_, 0, TenuredObject);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'slice' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);

  if (isPackedArray) {
    emitOptimisticClassGuard(objId, &thisval_.toObject(),
                             GuardClassKind::Array);
  } else {
    auto* args = &thisval_.toObject().as<ArgumentsObject>();

    if (args->is<MappedArgumentsObject>()) {
      writer.guardClass(objId, GuardClassKind::MappedArguments);
    } else {
      MOZ_ASSERT(args->is<UnmappedArgumentsObject>());
      writer.guardClass(objId, GuardClassKind::UnmappedArguments);
    }

    uint8_t flags = ArgumentsObject::ELEMENT_OVERRIDDEN_BIT |
                    ArgumentsObject::LENGTH_OVERRIDDEN_BIT |
                    ArgumentsObject::FORWARDED_ARGUMENTS_BIT;
    writer.guardArgumentsObjectFlags(objId, flags);
  }

  Int32OperandId int32BeginId;
  if (args_.length() > 0) {
    ValOperandId beginId = loadArgument(calleeId, ArgumentKind::Arg0);
    int32BeginId = writer.guardToInt32(beginId);
  } else {
    int32BeginId = writer.loadInt32Constant(0);
  }

  Int32OperandId int32EndId;
  if (args_.length() > 1) {
    ValOperandId endId = loadArgument(calleeId, ArgumentKind::Arg1);
    int32EndId = writer.guardToInt32(endId);
  } else if (isPackedArray) {
    int32EndId = writer.loadInt32ArrayLength(objId);
  } else {
    int32EndId = writer.loadArgumentsObjectLength(objId);
  }

  if (isPackedArray) {
    writer.packedArraySliceResult(templateObj, objId, int32BeginId, int32EndId);
  } else {
    writer.argumentsSliceResult(templateObj, objId, int32BeginId, int32EndId);
  }
  writer.returnFromIC();

  trackAttached(isPackedArray ? "ArraySlice" : "ArgumentsSlice");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachArrayIsArray() {
  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'isArray' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Check if the argument is an Array and return result.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  writer.isArrayResult(argId);
  writer.returnFromIC();

  trackAttached("ArrayIsArray");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachDataViewGet(
    Scalar::Type type) {
  // Ensure |this| is a DataViewObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<DataViewObject>()) {
    return AttachDecision::NoAction;
  }

  // Expected arguments: offset (number), optional littleEndian (boolean).
  if (args_.length() < 1 || args_.length() > 2) {
    return AttachDecision::NoAction;
  }
  int64_t offsetInt64;
  if (!ValueIsInt64Index(args_[0], &offsetInt64)) {
    return AttachDecision::NoAction;
  }
  if (args_.length() > 1 && !args_[1].isBoolean()) {
    return AttachDecision::NoAction;
  }

  auto* dv = &thisval_.toObject().as<DataViewObject>();

  // Bounds check the offset.
  size_t byteLength = dv->byteLength().valueOr(0);
  if (offsetInt64 < 0 || !DataViewObject::offsetIsInBounds(
                             Scalar::byteSize(type), offsetInt64, byteLength)) {
    return AttachDecision::NoAction;
  }

  // For getUint32 we let the stub return an Int32 if we have not seen a
  // double, to allow better codegen in Warp while avoiding bailout loops.
  bool forceDoubleForUint32 = false;
  if (type == Scalar::Uint32) {
    bool isLittleEndian = args_.length() > 1 && args_[1].toBoolean();
    uint32_t res = dv->read<uint32_t>(offsetInt64, byteLength, isLittleEndian);
    forceDoubleForUint32 = res >= INT32_MAX;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is this DataView native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a DataViewObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);

  if (dv->is<FixedLengthDataViewObject>()) {
    emitOptimisticClassGuard(objId, &thisval_.toObject(),
                             GuardClassKind::FixedLengthDataView);
  } else if (dv->is<ImmutableDataViewObject>()) {
    emitOptimisticClassGuard(objId, &thisval_.toObject(),
                             GuardClassKind::ImmutableDataView);
  } else {
    emitOptimisticClassGuard(objId, &thisval_.toObject(),
                             GuardClassKind::ResizableDataView);
  }

  // Convert offset to intPtr.
  ValOperandId offsetId = loadArgument(calleeId, ArgumentKind::Arg0);
  IntPtrOperandId intPtrOffsetId =
      guardToIntPtrIndex(args_[0], offsetId, /* supportOOB = */ false);

  BooleanOperandId boolLittleEndianId;
  if (args_.length() > 1) {
    ValOperandId littleEndianId = loadArgument(calleeId, ArgumentKind::Arg1);
    boolLittleEndianId = writer.guardToBoolean(littleEndianId);
  } else {
    boolLittleEndianId = writer.loadBooleanConstant(false);
  }

  auto viewKind = ToArrayBufferViewKind(dv);
  writer.loadDataViewValueResult(objId, intPtrOffsetId, boolLittleEndianId,
                                 type, forceDoubleForUint32, viewKind);

  writer.returnFromIC();

  trackAttached("DataViewGet");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachDataViewSet(
    Scalar::Type type) {
  // Ensure |this| is a DataViewObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<DataViewObject>()) {
    return AttachDecision::NoAction;
  }

  // Expected arguments: offset (number), value, optional littleEndian (boolean)
  if (args_.length() < 2 || args_.length() > 3) {
    return AttachDecision::NoAction;
  }
  int64_t offsetInt64;
  if (!ValueIsInt64Index(args_[0], &offsetInt64)) {
    return AttachDecision::NoAction;
  }
  if (!ValueCanConvertToNumeric(type, args_[1])) {
    return AttachDecision::NoAction;
  }
  if (args_.length() > 2 && !args_[2].isBoolean()) {
    return AttachDecision::NoAction;
  }

  auto* dv = &thisval_.toObject().as<DataViewObject>();

  // Immutable DataViews can't be modified.
  if (dv->is<ImmutableDataViewObject>()) {
    return AttachDecision::NoAction;
  }

  // Bounds check the offset.
  size_t byteLength = dv->byteLength().valueOr(0);
  if (offsetInt64 < 0 || !DataViewObject::offsetIsInBounds(
                             Scalar::byteSize(type), offsetInt64, byteLength)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is this DataView native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a DataViewObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);

  if (dv->is<FixedLengthDataViewObject>()) {
    emitOptimisticClassGuard(objId, &thisval_.toObject(),
                             GuardClassKind::FixedLengthDataView);
  } else {
    emitOptimisticClassGuard(objId, &thisval_.toObject(),
                             GuardClassKind::ResizableDataView);
  }

  // Convert offset to intPtr.
  ValOperandId offsetId = loadArgument(calleeId, ArgumentKind::Arg0);
  IntPtrOperandId intPtrOffsetId =
      guardToIntPtrIndex(args_[0], offsetId, /* supportOOB = */ false);

  // Convert value to number or BigInt.
  ValOperandId valueId = loadArgument(calleeId, ArgumentKind::Arg1);
  OperandId numericValueId = emitNumericGuard(valueId, args_[1], type);

  BooleanOperandId boolLittleEndianId;
  if (args_.length() > 2) {
    ValOperandId littleEndianId = loadArgument(calleeId, ArgumentKind::Arg2);
    boolLittleEndianId = writer.guardToBoolean(littleEndianId);
  } else {
    boolLittleEndianId = writer.loadBooleanConstant(false);
  }

  auto viewKind = ToArrayBufferViewKind(dv);
  writer.storeDataViewValueResult(objId, intPtrOffsetId, numericValueId,
                                  boolLittleEndianId, type, viewKind);

  writer.returnFromIC();

  trackAttached("DataViewSet");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachUnsafeGetReservedSlot(
    InlinableNative native) {
  // Self-hosted code calls this with (object, int32) arguments.
  MOZ_ASSERT(args_.length() == 2);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isInt32());
  MOZ_ASSERT(args_[1].toInt32() >= 0);

  uint32_t slot = uint32_t(args_[1].toInt32());
  if (slot >= NativeObject::MAX_FIXED_SLOTS) {
    return AttachDecision::NoAction;
  }
  size_t offset = NativeObject::getFixedSlotOffset(slot);

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the first argument is an object.
  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  // BytecodeEmitter::assertSelfHostedUnsafeGetReservedSlot ensures that the
  // slot argument is constant. (At least for direct calls)

  switch (native) {
    case InlinableNative::IntrinsicUnsafeGetReservedSlot:
      writer.loadFixedSlotResult(objId, offset);
      break;
    case InlinableNative::IntrinsicUnsafeGetObjectFromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::Object);
      break;
    case InlinableNative::IntrinsicUnsafeGetInt32FromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::Int32);
      break;
    case InlinableNative::IntrinsicUnsafeGetStringFromReservedSlot:
      writer.loadFixedSlotTypedResult(objId, offset, ValueType::String);
      break;
    default:
      MOZ_CRASH("unexpected native");
  }

  writer.returnFromIC();

  trackAttached("UnsafeGetReservedSlot");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachUnsafeSetReservedSlot() {
  // Self-hosted code calls this with (object, int32, value) arguments.
  MOZ_ASSERT(args_.length() == 3);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isInt32());
  MOZ_ASSERT(args_[1].toInt32() >= 0);

  uint32_t slot = uint32_t(args_[1].toInt32());
  if (slot >= NativeObject::MAX_FIXED_SLOTS) {
    return AttachDecision::NoAction;
  }
  size_t offset = NativeObject::getFixedSlotOffset(slot);

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the first argument is an object.
  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  // BytecodeEmitter::assertSelfHostedUnsafeSetReservedSlot ensures that the
  // slot argument is constant. (At least for direct calls)

  // Get the value to set.
  ValOperandId valId = loadArgumentIntrinsic(ArgumentKind::Arg2);

  // Set the fixed slot and return undefined.
  writer.storeFixedSlotUndefinedResult(objId, offset, valId);

  // This stub always returns undefined.
  writer.returnFromIC();

  trackAttached("UnsafeSetReservedSlot");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsSuspendedGenerator() {
  // The IsSuspendedGenerator intrinsic is only called in
  // self-hosted code, so it's safe to assume we have a single
  // argument and the callee is our intrinsic.

  MOZ_ASSERT(args_.length() == 1);

  initializeInputOperand();

  // Stack layout here is (bottom to top):
  //  2: Callee
  //  1: ThisValue
  //  0: Arg <-- Top of stack.
  // We only care about the argument.
  ValOperandId valId = loadArgumentIntrinsic(ArgumentKind::Arg0);

  // Check whether the argument is a suspended generator.
  // We don't need guards, because IsSuspendedGenerator returns
  // false for values that are not generator objects.
  writer.callIsSuspendedGeneratorResult(valId);
  writer.returnFromIC();

  trackAttached("IsSuspendedGenerator");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachToObject() {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(args_.length() == 1);

  // Need a single object argument.
  // TODO(Warp): Support all or more conversions to object.
  if (!args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("ToObject");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachToInteger() {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(args_.length() == 1);

  // Need a single int32 argument.
  // TODO(Warp): Support all or more conversions to integer.
  // Make sure to update this code correctly if we ever start
  // returning non-int32 integers.
  if (!args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an int32.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  Int32OperandId int32Id = writer.guardToInt32(argId);

  // Return the int32.
  writer.loadInt32Result(int32Id);
  writer.returnFromIC();

  trackAttached("ToInteger");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachToLength() {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(args_.length() == 1);

  // Need a single int32 argument.
  if (!args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // ToLength(int32) is equivalent to max(int32, 0).
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  Int32OperandId int32ArgId = writer.guardToInt32(argId);
  Int32OperandId zeroId = writer.loadInt32Constant(0);
  bool isMax = true;
  Int32OperandId maxId = writer.int32MinMax(isMax, int32ArgId, zeroId);
  writer.loadInt32Result(maxId);
  writer.returnFromIC();

  trackAttached("ToLength");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsObject() {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(args_.length() == 1);

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Type check the argument and return result.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  writer.isObjectResult(argId);
  writer.returnFromIC();

  trackAttached("IsObject");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsPackedArray() {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Check if the argument is packed and return result.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.isPackedArrayResult(objArgId);
  writer.returnFromIC();

  trackAttached("IsPackedArray");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsCallable() {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(args_.length() == 1);

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Check if the argument is callable and return result.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  writer.isCallableResult(argId);
  writer.returnFromIC();

  trackAttached("IsCallable");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsConstructor() {
  // Self-hosted code calls this with a single argument.
  MOZ_ASSERT(args_.length() == 1);

  // Need a single object argument.
  if (!args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  // Check if the argument is a constructor and return result.
  writer.isConstructorResult(objId);
  writer.returnFromIC();

  trackAttached("IsConstructor");
  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachIsCrossRealmArrayConstructor() {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  if (args_[0].toObject().is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);
  writer.guardIsNotProxy(objId);
  writer.isCrossRealmArrayConstructorResult(objId);
  writer.returnFromIC();

  trackAttached("IsCrossRealmArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachCanOptimizeArraySpecies() {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  SharedShape* shape = GlobalObject::getArrayShapeWithDefaultProto(cx_);
  if (!shape) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  if (cx_->realm()->realmFuses.optimizeArraySpeciesFuse.intact()) {
    ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
    ObjOperandId objId = writer.guardToObject(argId);
    writer.guardFuse(RealmFuses::FuseIndex::OptimizeArraySpeciesFuse);
    writer.hasShapeResult(objId, shape);
    writer.returnFromIC();
    trackAttached("CanOptimizeArraySpecies.Optimized");
  } else {
    writer.loadBooleanResult(false);
    writer.returnFromIC();
    trackAttached("CanOptimizeArraySpecies.Deoptimized");
  }

  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachCanOptimizeStringProtoSymbolLookup() {
  // Self-hosted code calls this with no arguments.
  MOZ_ASSERT(args_.length() == 0);

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  if (cx_->realm()->realmFuses.optimizeStringPrototypeSymbolsFuse.intact()) {
    writer.guardFuse(RealmFuses::FuseIndex::OptimizeStringPrototypeSymbolsFuse);
    writer.loadBooleanResult(true);
    writer.returnFromIC();
    trackAttached("CanOptimizeStringProtoSymbolLookup.Optimized");
  } else {
    writer.loadBooleanResult(false);
    writer.returnFromIC();
    trackAttached("CanOptimizeStringProtoSymbolLookup.Deoptimized");
  }

  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachGuardToClass(
    InlinableNative native) {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Class must match.
  const JSClass* clasp = InlinableNativeGuardToClass(native);
  if (args_[0].toObject().getClass() != clasp) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  // Guard that the object has the correct class.
  writer.guardAnyClass(objId, clasp);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("GuardToClass");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachGuardToClass(
    GuardClassKind kind) {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Class must match.
  const JSClass* clasp = ClassFor(kind);
  if (args_[0].toObject().getClass() != clasp) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  // Guard that the object has the correct class.
  writer.guardClass(objId, kind);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("GuardToClass");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachGuardToArrayBuffer() {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Class must match.
  if (!args_[0].toObject().is<ArrayBufferObject>()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  // Guard that the object has the correct class.
  writer.guardToArrayBuffer(objId);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("GuardToArrayBuffer");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachGuardToSharedArrayBuffer() {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Class must match.
  if (!args_[0].toObject().is<SharedArrayBufferObject>()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard that the argument is an object.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  // Guard that the object has the correct class.
  writer.guardToSharedArrayBuffer(objId);

  // Return the object.
  writer.loadObjectResult(objId);
  writer.returnFromIC();

  trackAttached("GuardToSharedArrayBuffer");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachHasClass(
    const JSClass* clasp, bool isPossiblyWrapped) {
  // Self-hosted code calls this with an object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Only optimize when the object isn't a proxy.
  if (isPossiblyWrapped && args_[0].toObject().is<ProxyObject>()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Perform the Class check.
  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argId);

  if (isPossiblyWrapped) {
    writer.guardIsNotProxy(objId);
  }

  writer.hasClassResult(objId, clasp);
  writer.returnFromIC();

  trackAttached("HasClass");
  return AttachDecision::Attach;
}

// Returns whether the .lastIndex property is a non-negative int32 value and is
// still writable.
static bool HasOptimizableLastIndexSlot(RegExpObject* regexp, JSContext* cx) {
  auto lastIndexProp = regexp->lookupPure(cx->names().lastIndex);
  MOZ_ASSERT(lastIndexProp->isDataProperty());
  if (!lastIndexProp->writable()) {
    return false;
  }
  Value lastIndex = regexp->getLastIndex();
  if (!lastIndex.isInt32() || lastIndex.toInt32() < 0) {
    return false;
  }
  return true;
}

// Returns the RegExp stub used by the optimized code path for this intrinsic.
// We store a pointer to this in the IC stub to ensure GC doesn't discard it.
static JitCode* GetOrCreateRegExpStub(JSContext* cx, InlinableNative native) {
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
  return nullptr;
#else
  // The stubs assume the global has non-null RegExpStatics and match result
  // shape.
  if (!GlobalObject::getRegExpStatics(cx, cx->global()) ||
      !cx->global()->regExpRealm().getOrCreateMatchResultShape(cx)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory() || cx->isThrowingOverRecursed());
    cx->clearPendingException();
    return nullptr;
  }
  JitZone::StubKind kind;
  switch (native) {
    case InlinableNative::IntrinsicRegExpBuiltinExecForTest:
    case InlinableNative::IntrinsicRegExpExecForTest:
      kind = JitZone::StubKind::RegExpExecTest;
      break;
    case InlinableNative::IntrinsicRegExpBuiltinExec:
    case InlinableNative::IntrinsicRegExpExec:
      kind = JitZone::StubKind::RegExpExecMatch;
      break;
    case InlinableNative::RegExpMatcher:
      kind = JitZone::StubKind::RegExpMatcher;
      break;
    case InlinableNative::RegExpSearcher:
      kind = JitZone::StubKind::RegExpSearcher;
      break;
    default:
      MOZ_CRASH("Unexpected native");
  }
  JitCode* code = cx->zone()->jitZone()->ensureStubExists(cx, kind);
  if (!code) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory() || cx->isThrowingOverRecursed());
    cx->clearPendingException();
    return nullptr;
  }
  return code;
#endif
}

static void EmitGuardLastIndexIsNonNegativeInt32(CacheIRWriter& writer,
                                                 ObjOperandId regExpId) {
  size_t offset =
      NativeObject::getFixedSlotOffset(RegExpObject::lastIndexSlot());
  ValOperandId lastIndexValId = writer.loadFixedSlot(regExpId, offset);
  Int32OperandId lastIndexId = writer.guardToInt32(lastIndexValId);
  writer.guardInt32IsNonNegative(lastIndexId);
}

AttachDecision InlinableNativeIRGenerator::tryAttachIntrinsicRegExpBuiltinExec(
    InlinableNative native) {
  // Self-hosted code calls this with (regexp, string) arguments.
  MOZ_ASSERT(args_.length() == 2);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isString());

  JitCode* stub = GetOrCreateRegExpStub(cx_, native);
  if (!stub) {
    return AttachDecision::NoAction;
  }

  RegExpObject* re = &args_[0].toObject().as<RegExpObject>();
  if (!HasOptimizableLastIndexSlot(re, cx_)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId regExpId = writer.guardToObject(arg0Id);
  writer.guardShape(regExpId, re->shape());
  EmitGuardLastIndexIsNonNegativeInt32(writer, regExpId);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  StringOperandId inputId = writer.guardToString(arg1Id);

  if (native == InlinableNative::IntrinsicRegExpBuiltinExecForTest) {
    writer.regExpBuiltinExecTestResult(regExpId, inputId, stub);
  } else {
    writer.regExpBuiltinExecMatchResult(regExpId, inputId, stub);
  }
  writer.returnFromIC();

  trackAttached("IntrinsicRegExpBuiltinExec");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIntrinsicRegExpExec(
    InlinableNative native) {
  // Self-hosted code calls this with (object, string) arguments.
  MOZ_ASSERT(args_.length() == 2);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isString());

  // Ensure the object is a RegExpObject with the builtin RegExp.prototype.exec
  // function.
  if (!IsOptimizableRegExpObject(&args_[0].toObject(), cx_)) {
    return AttachDecision::NoAction;
  }

  JitCode* stub = GetOrCreateRegExpStub(cx_, native);
  if (!stub) {
    return AttachDecision::NoAction;
  }

  RegExpObject* re = &args_[0].toObject().as<RegExpObject>();
  if (!HasOptimizableLastIndexSlot(re, cx_)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId regExpId = writer.guardToObject(arg0Id);
  writer.guardShape(regExpId, re->shape());
  writer.guardFuse(RealmFuses::FuseIndex::OptimizeRegExpPrototypeFuse);
  EmitGuardLastIndexIsNonNegativeInt32(writer, regExpId);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  StringOperandId inputId = writer.guardToString(arg1Id);

  if (native == InlinableNative::IntrinsicRegExpExecForTest) {
    writer.regExpBuiltinExecTestResult(regExpId, inputId, stub);
  } else {
    writer.regExpBuiltinExecMatchResult(regExpId, inputId, stub);
  }
  writer.returnFromIC();

  trackAttached("IntrinsicRegExpExec");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachRegExpMatcherSearcher(
    InlinableNative native) {
  // Self-hosted code calls this with (object, string, number) arguments.
  MOZ_ASSERT(args_.length() == 3);
  MOZ_ASSERT(args_[0].isObject());
  MOZ_ASSERT(args_[1].isString());
  MOZ_ASSERT(args_[2].isNumber());

  // It's not guaranteed that the JITs have typed |lastIndex| as an Int32.
  if (!args_[2].isInt32()) {
    return AttachDecision::NoAction;
  }

  JitCode* stub = GetOrCreateRegExpStub(cx_, native);
  if (!stub) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  // Guard argument types.
  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId reId = writer.guardToObject(arg0Id);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  StringOperandId inputId = writer.guardToString(arg1Id);

  ValOperandId arg2Id = loadArgumentIntrinsic(ArgumentKind::Arg2);
  Int32OperandId lastIndexId = writer.guardToInt32(arg2Id);

  switch (native) {
    case InlinableNative::RegExpMatcher:
      writer.callRegExpMatcherResult(reId, inputId, lastIndexId, stub);
      writer.returnFromIC();
      trackAttached("RegExpMatcher");
      break;

    case InlinableNative::RegExpSearcher:
      writer.callRegExpSearcherResult(reId, inputId, lastIndexId, stub);
      writer.returnFromIC();
      trackAttached("RegExpSearcher");
      break;

    default:
      MOZ_CRASH("Unexpected native");
  }

  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachRegExpSearcherLastLimit() {
  // Self-hosted code calls this with a string argument that's only used for an
  // assertion.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isString());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.regExpSearcherLastLimitResult();
  writer.returnFromIC();

  trackAttached("RegExpSearcherLastLimit");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachRegExpHasCaptureGroups() {
  // Self-hosted code calls this with object and string arguments.
  MOZ_ASSERT(args_.length() == 2);
  MOZ_ASSERT(args_[0].toObject().is<RegExpObject>());
  MOZ_ASSERT(args_[1].isString());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  StringOperandId inputId = writer.guardToString(arg1Id);

  writer.regExpHasCaptureGroupsResult(objId, inputId);
  writer.returnFromIC();

  trackAttached("RegExpHasCaptureGroups");
  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachIsRegExpPrototypeOptimizable() {
  // Self-hosted code calls this with no arguments.
  MOZ_ASSERT(args_.length() == 0);

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  if (cx_->realm()->realmFuses.optimizeRegExpPrototypeFuse.intact()) {
    writer.guardFuse(RealmFuses::FuseIndex::OptimizeRegExpPrototypeFuse);
    writer.loadBooleanResult(true);
    writer.returnFromIC();
    trackAttached("IsRegExpPrototypeOptimizable.Optimized");
  } else {
    writer.loadBooleanResult(false);
    writer.returnFromIC();
    trackAttached("IsRegExpPrototypeOptimizable.Deoptimized");
  }

  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachIsOptimizableRegExpObject() {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  Shape* optimizableShape = cx_->global()->maybeRegExpShapeWithDefaultProto();
  if (!optimizableShape) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  if (cx_->realm()->realmFuses.optimizeRegExpPrototypeFuse.intact()) {
    ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
    ObjOperandId objId = writer.guardToObject(argId);
    writer.guardFuse(RealmFuses::FuseIndex::OptimizeRegExpPrototypeFuse);
    writer.hasShapeResult(objId, optimizableShape);
    writer.returnFromIC();
    trackAttached("IsOptimizableRegExpObject.Optimized");
  } else {
    writer.loadBooleanResult(false);
    writer.returnFromIC();
    trackAttached("IsOptimizableRegExpObject.Deoptimized");
  }

  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachGetFirstDollarIndex() {
  // Self-hosted code calls this with a single string argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isString());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  StringOperandId strId = writer.guardToString(arg0Id);

  writer.getFirstDollarIndexResult(strId);
  writer.returnFromIC();

  trackAttached("GetFirstDollarIndex");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSubstringKernel() {
  // Self-hosted code calls this with (string, int32, int32) arguments.
  MOZ_ASSERT(args_.length() == 3);
  MOZ_ASSERT(args_[0].isString());
  MOZ_ASSERT(args_[1].isInt32());
  MOZ_ASSERT(args_[2].isInt32());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  StringOperandId strId = writer.guardToString(arg0Id);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  Int32OperandId beginId = writer.guardToInt32(arg1Id);

  ValOperandId arg2Id = loadArgumentIntrinsic(ArgumentKind::Arg2);
  Int32OperandId lengthId = writer.guardToInt32(arg2Id);

  writer.callSubstringKernelResult(strId, beginId, lengthId);
  writer.returnFromIC();

  trackAttached("SubstringKernel");
  return AttachDecision::Attach;
}

static bool CanConvertToString(const Value& v) {
  return v.isString() || v.isNumber() || v.isBoolean() || v.isNullOrUndefined();
}

AttachDecision InlinableNativeIRGenerator::tryAttachString() {
  // Need a single argument that is or can be converted to a string.
  if (args_.length() != 1 || !CanConvertToString(args_[0])) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'String' function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard that the argument is a string or can be converted to one.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId strId = emitToStringGuard(argId, args_[0]);

  // Return the string.
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("String");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringConstructor() {
  // Need a single argument that is or can be converted to a string.
  if (args_.length() != 1 || !CanConvertToString(args_[0])) {
    return AttachDecision::NoAction;
  }

  RootedString emptyString(cx_, cx_->runtime()->emptyString);
  JSObject* templateObj = StringObject::create(
      cx_, emptyString, /* proto = */ nullptr, TenuredObject);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'String' function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard on number and convert to string.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId strId = emitToStringGuard(argId, args_[0]);

  writer.newStringObjectResult(templateObj, strId);
  writer.returnFromIC();

  trackAttached("StringConstructor");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringToStringValueOf() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'toString' OR 'valueOf' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Return the string
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("StringToStringValueOf");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringReplaceString() {
  // Self-hosted code calls this with (string, string, string) arguments.
  MOZ_ASSERT(args_.length() == 3);
  MOZ_ASSERT(args_[0].isString());
  MOZ_ASSERT(args_[1].isString());
  MOZ_ASSERT(args_[2].isString());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  StringOperandId strId = writer.guardToString(arg0Id);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  StringOperandId patternId = writer.guardToString(arg1Id);

  ValOperandId arg2Id = loadArgumentIntrinsic(ArgumentKind::Arg2);
  StringOperandId replacementId = writer.guardToString(arg2Id);

  writer.stringReplaceStringResult(strId, patternId, replacementId);
  writer.returnFromIC();

  trackAttached("StringReplaceString");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringSplitString() {
  // Self-hosted code calls this with (string, string) arguments.
  MOZ_ASSERT(args_.length() == 2);
  MOZ_ASSERT(args_[0].isString());
  MOZ_ASSERT(args_[1].isString());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId arg0Id = loadArgumentIntrinsic(ArgumentKind::Arg0);
  StringOperandId strId = writer.guardToString(arg0Id);

  ValOperandId arg1Id = loadArgumentIntrinsic(ArgumentKind::Arg1);
  StringOperandId separatorId = writer.guardToString(arg1Id);

  writer.stringSplitStringResult(strId, separatorId);
  writer.returnFromIC();

  trackAttached("StringSplitString");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringChar(
    StringChar kind) {
  // Need zero or one argument.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }

  // Absent index argument defaults to zero:
  // ToInteger(ToNumber(undefined)) = ToInteger(NaN) = 0.
  auto indexArg = args_.length() > 0 ? args_[0] : Int32Value(0);

  auto attach = CanAttachStringChar(thisval_, indexArg, kind);
  if (attach == AttachStringChar::No) {
    return AttachDecision::NoAction;
  }

  bool handleOOB = attach == AttachStringChar::OutOfBounds;

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'charCodeAt', 'codePointAt', 'charAt', or 'at' native
  // function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard int32 index.
  Int32OperandId int32IndexId;
  if (args_.length() > 0) {
    ValOperandId indexId = loadArgument(calleeId, ArgumentKind::Arg0);
    int32IndexId = EmitGuardToInt32Index(writer, args_[0], indexId);
  } else {
    int32IndexId = writer.loadInt32Constant(0);
  }

  // Handle relative string indices, if necessary.
  if (kind == StringChar::At) {
    int32IndexId = writer.toRelativeStringIndex(int32IndexId, strId);
  }

  // Linearize the string.
  //
  // AttachStringChar doesn't have a separate state when OOB access happens on
  // a string which needs to be linearized, so just linearize unconditionally
  // for out-of-bounds accesses.
  if (attach == AttachStringChar::Linearize ||
      attach == AttachStringChar::OutOfBounds) {
    switch (kind) {
      case StringChar::CharCodeAt:
      case StringChar::CharAt:
      case StringChar::At:
        strId = writer.linearizeForCharAccess(strId, int32IndexId);
        break;
      case StringChar::CodePointAt:
        strId = writer.linearizeForCodePointAccess(strId, int32IndexId);
        break;
    }
  }

  // Load string char or code.
  switch (kind) {
    case StringChar::CharCodeAt:
      writer.loadStringCharCodeResult(strId, int32IndexId, handleOOB);
      break;
    case StringChar::CodePointAt:
      writer.loadStringCodePointResult(strId, int32IndexId, handleOOB);
      break;
    case StringChar::CharAt:
      writer.loadStringCharResult(strId, int32IndexId, handleOOB);
      break;
    case StringChar::At:
      writer.loadStringAtResult(strId, int32IndexId, handleOOB);
      break;
  }

  writer.returnFromIC();

  switch (kind) {
    case StringChar::CharCodeAt:
      trackAttached("StringCharCodeAt");
      break;
    case StringChar::CodePointAt:
      trackAttached("StringCodePointAt");
      break;
    case StringChar::CharAt:
      trackAttached("StringCharAt");
      break;
    case StringChar::At:
      trackAttached("StringAt");
      break;
  }

  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringCharCodeAt() {
  return tryAttachStringChar(StringChar::CharCodeAt);
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringCodePointAt() {
  return tryAttachStringChar(StringChar::CodePointAt);
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringCharAt() {
  return tryAttachStringChar(StringChar::CharAt);
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringAt() {
  return tryAttachStringChar(StringChar::At);
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringFromCharCode() {
  // Need one number argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'fromCharCode' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard int32 argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  Int32OperandId codeId;
  if (args_[0].isInt32()) {
    codeId = writer.guardToInt32(argId);
  } else {
    // 'fromCharCode' performs ToUint16 on its input. We can use Uint32
    // semantics, because ToUint16(ToUint32(v)) == ToUint16(v).
    codeId = writer.guardToInt32ModUint32(argId);
  }

  // Return string created from code.
  writer.stringFromCharCodeResult(codeId);
  writer.returnFromIC();

  trackAttached("StringFromCharCode");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringFromCodePoint() {
  // Need one int32 argument.
  if (args_.length() != 1 || !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // String.fromCodePoint throws for invalid code points.
  int32_t codePoint = args_[0].toInt32();
  if (codePoint < 0 || codePoint > int32_t(unicode::NonBMPMax)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'fromCodePoint' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard int32 argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  Int32OperandId codeId = writer.guardToInt32(argId);

  // Return string created from code point.
  writer.stringFromCodePointResult(codeId);
  writer.returnFromIC();

  trackAttached("StringFromCodePoint");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringIncludes() {
  // Need one string argument.
  if (args_.length() != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'includes' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard string argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId searchStrId = writer.guardToString(argId);

  writer.stringIncludesResult(strId, searchStrId);
  writer.returnFromIC();

  trackAttached("StringIncludes");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringIndexOf() {
  // Need one string argument.
  if (args_.length() != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'indexOf' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard string argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId searchStrId = writer.guardToString(argId);

  writer.stringIndexOfResult(strId, searchStrId);
  writer.returnFromIC();

  trackAttached("StringIndexOf");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringLastIndexOf() {
  // Need one string argument.
  if (args_.length() != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'lastIndexOf' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard string argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId searchStrId = writer.guardToString(argId);

  writer.stringLastIndexOfResult(strId, searchStrId);
  writer.returnFromIC();

  trackAttached("StringLastIndexOf");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringStartsWith() {
  // Need one string argument.
  if (args_.length() != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'startsWith' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard string argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId searchStrId = writer.guardToString(argId);

  writer.stringStartsWithResult(strId, searchStrId);
  writer.returnFromIC();

  trackAttached("StringStartsWith");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringEndsWith() {
  // Need one string argument.
  if (args_.length() != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'endsWith' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Guard string argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId searchStrId = writer.guardToString(argId);

  writer.stringEndsWithResult(strId, searchStrId);
  writer.returnFromIC();

  trackAttached("StringEndsWith");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringToLowerCase() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'toLowerCase' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Return string converted to lower-case.
  writer.stringToLowerCaseResult(strId);
  writer.returnFromIC();

  trackAttached("StringToLowerCase");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringToUpperCase() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'toUpperCase' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  // Return string converted to upper-case.
  writer.stringToUpperCaseResult(strId);
  writer.returnFromIC();

  trackAttached("StringToUpperCase");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringTrim() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'trim' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  writer.stringTrimResult(strId);
  writer.returnFromIC();

  trackAttached("StringTrim");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringTrimStart() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'trimStart' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  writer.stringTrimStartResult(strId);
  writer.returnFromIC();

  trackAttached("StringTrimStart");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachStringTrimEnd() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive string value.
  if (!thisval_.isString()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'trimEnd' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is a string.
  ValOperandId thisValId = loadThis(calleeId);
  StringOperandId strId = writer.guardToString(thisValId);

  writer.stringTrimEndResult(strId);
  writer.returnFromIC();

  trackAttached("StringTrimEnd");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathRandom() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(cx_->realm() == target_->realm(),
             "Shouldn't inline cross-realm Math.random because per-realm RNG");

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'random' native function.
  emitNativeCalleeGuard(argcId);

  mozilla::non_crypto::XorShift128PlusRNG* rng =
      &cx_->realm()->getOrCreateRandomNumberGenerator();
  writer.mathRandomResult(rng);

  writer.returnFromIC();

  trackAttached("MathRandom");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathAbs() {
  // Need one argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'abs' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);

  // abs(INT_MIN) is a double.
  if (args_[0].isInt32() && args_[0].toInt32() != INT_MIN) {
    Int32OperandId int32Id = writer.guardToInt32(argumentId);
    writer.mathAbsInt32Result(int32Id);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);
    writer.mathAbsNumberResult(numberId);
  }

  writer.returnFromIC();

  trackAttached("MathAbs");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathClz32() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'clz32' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);

  Int32OperandId int32Id;
  if (args_[0].isInt32()) {
    int32Id = writer.guardToInt32(argId);
  } else {
    MOZ_ASSERT(args_[0].isDouble());
    NumberOperandId numId = writer.guardIsNumber(argId);
    int32Id = writer.truncateDoubleToUInt32(numId);
  }
  writer.mathClz32Result(int32Id);
  writer.returnFromIC();

  trackAttached("MathClz32");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathSign() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'sign' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);

  if (args_[0].isInt32()) {
    Int32OperandId int32Id = writer.guardToInt32(argId);
    writer.mathSignInt32Result(int32Id);
  } else {
    // Math.sign returns a double only if the input is -0 or NaN so try to
    // optimize the common Number => Int32 case.
    double d = math_sign_impl(args_[0].toDouble());
    int32_t unused;
    bool resultIsInt32 = mozilla::NumberIsInt32(d, &unused);

    NumberOperandId numId = writer.guardIsNumber(argId);
    if (resultIsInt32) {
      writer.mathSignNumberToInt32Result(numId);
    } else {
      writer.mathSignNumberResult(numId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathSign");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathImul() {
  // Need two (number) arguments.
  if (args_.length() != 2 || !args_[0].isNumber() || !args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'imul' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ValOperandId arg1Id = loadArgument(calleeId, ArgumentKind::Arg1);

  Int32OperandId int32Arg0Id, int32Arg1Id;
  if (args_[0].isInt32() && args_[1].isInt32()) {
    int32Arg0Id = writer.guardToInt32(arg0Id);
    int32Arg1Id = writer.guardToInt32(arg1Id);
  } else {
    // Treat both arguments as numbers if at least one of them is non-int32.
    NumberOperandId numArg0Id = writer.guardIsNumber(arg0Id);
    NumberOperandId numArg1Id = writer.guardIsNumber(arg1Id);
    int32Arg0Id = writer.truncateDoubleToUInt32(numArg0Id);
    int32Arg1Id = writer.truncateDoubleToUInt32(numArg1Id);
  }
  writer.mathImulResult(int32Arg0Id, int32Arg1Id);
  writer.returnFromIC();

  trackAttached("MathImul");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathFloor() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_floor_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'floor' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // Use an indirect truncation to inform the optimizer it needs to preserve
    // a bailout when the input can't be represented as an int32, even if the
    // final result is fully truncated.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.indirectTruncateInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathFloorToInt32Result(numberId);
    } else {
      writer.mathFloorNumberResult(numberId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathFloor");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathCeil() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_ceil_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'ceil' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // Use an indirect truncation to inform the optimizer it needs to preserve
    // a bailout when the input can't be represented as an int32, even if the
    // final result is fully truncated.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.indirectTruncateInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathCeilToInt32Result(numberId);
    } else {
      writer.mathCeilNumberResult(numberId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathCeil");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathTrunc() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_trunc_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'trunc' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // We don't need an indirect truncation barrier here, because Math.trunc
    // always truncates, but never rounds its input away from zero.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.loadInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathTruncToInt32Result(numberId);
    } else {
      writer.mathTruncNumberResult(numberId);
    }
  }

  writer.returnFromIC();

  trackAttached("MathTrunc");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathRound() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Check if the result fits in int32.
  double res = math_round_impl(args_[0].toNumber());
  int32_t unused;
  bool resultIsInt32 = mozilla::NumberIsInt32(res, &unused);

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'round' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);

  if (args_[0].isInt32()) {
    MOZ_ASSERT(resultIsInt32);

    // Use an indirect truncation to inform the optimizer it needs to preserve
    // a bailout when the input can't be represented as an int32, even if the
    // final result is fully truncated.
    Int32OperandId intId = writer.guardToInt32(argumentId);
    writer.indirectTruncateInt32Result(intId);
  } else {
    NumberOperandId numberId = writer.guardIsNumber(argumentId);

    if (resultIsInt32) {
      writer.mathRoundToInt32Result(numberId);
    } else {
      writer.mathFunctionNumberResult(numberId, UnaryMathFunction::Round);
    }
  }

  writer.returnFromIC();

  trackAttached("MathRound");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathSqrt() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'sqrt' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathSqrtNumberResult(numberId);
  writer.returnFromIC();

  trackAttached("MathSqrt");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathFRound() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'fround' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathFRoundNumberResult(numberId);
  writer.returnFromIC();

  trackAttached("MathFRound");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathF16Round() {
  // Need one (number) argument.
  if (args_.length() != 1 || !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'f16round' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathF16RoundNumberResult(numberId);
  writer.returnFromIC();

  trackAttached("MathF16Round");
  return AttachDecision::Attach;
}

static bool CanAttachInt32Pow(const Value& baseVal, const Value& powerVal) {
  auto valToInt32 = [](const Value& v) {
    if (v.isInt32()) {
      return v.toInt32();
    }
    if (v.isBoolean()) {
      return int32_t(v.toBoolean());
    }
    MOZ_ASSERT(v.isNull());
    return 0;
  };
  int32_t base = valToInt32(baseVal);
  int32_t power = valToInt32(powerVal);

  // x^y where y < 0 is most of the time not an int32, except when x is 1 or y
  // gets large enough. It's hard to determine when exactly y is "large enough",
  // so we don't use Int32PowResult when x != 1 and y < 0.
  // Note: it's important for this condition to match the code generated by
  // MacroAssembler::pow32 to prevent failure loops.
  if (power < 0) {
    return base == 1;
  }

  double res = powi(base, power);
  int32_t unused;
  return mozilla::NumberIsInt32(res, &unused);
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathPow() {
  // Need two number arguments.
  if (args_.length() != 2 || !args_[0].isNumber() || !args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'pow' function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId baseId = loadArgument(calleeId, ArgumentKind::Arg0);
  ValOperandId exponentId = loadArgument(calleeId, ArgumentKind::Arg1);

  if (args_[0].isInt32() && args_[1].isInt32() &&
      CanAttachInt32Pow(args_[0], args_[1])) {
    Int32OperandId baseInt32Id = writer.guardToInt32(baseId);
    Int32OperandId exponentInt32Id = writer.guardToInt32(exponentId);
    writer.int32PowResult(baseInt32Id, exponentInt32Id);
  } else {
    NumberOperandId baseNumberId = writer.guardIsNumber(baseId);
    NumberOperandId exponentNumberId = writer.guardIsNumber(exponentId);
    writer.doublePowResult(baseNumberId, exponentNumberId);
  }

  writer.returnFromIC();

  trackAttached("MathPow");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathHypot() {
  // Only optimize if there are 2-4 arguments.
  if (args_.length() < 2 || args_.length() > 4) {
    return AttachDecision::NoAction;
  }

  for (size_t i = 0; i < args_.length(); i++) {
    if (!args_[i].isNumber()) {
      return AttachDecision::NoAction;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'hypot' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId firstId = loadArgument(calleeId, ArgumentKind::Arg0);
  ValOperandId secondId = loadArgument(calleeId, ArgumentKind::Arg1);

  NumberOperandId firstNumId = writer.guardIsNumber(firstId);
  NumberOperandId secondNumId = writer.guardIsNumber(secondId);

  ValOperandId thirdId;
  ValOperandId fourthId;
  NumberOperandId thirdNumId;
  NumberOperandId fourthNumId;

  switch (args_.length()) {
    case 2:
      writer.mathHypot2NumberResult(firstNumId, secondNumId);
      break;
    case 3:
      thirdId = loadArgument(calleeId, ArgumentKind::Arg2);
      thirdNumId = writer.guardIsNumber(thirdId);
      writer.mathHypot3NumberResult(firstNumId, secondNumId, thirdNumId);
      break;
    case 4:
      thirdId = loadArgument(calleeId, ArgumentKind::Arg2);
      fourthId = loadArgument(calleeId, ArgumentKind::Arg3);
      thirdNumId = writer.guardIsNumber(thirdId);
      fourthNumId = writer.guardIsNumber(fourthId);
      writer.mathHypot4NumberResult(firstNumId, secondNumId, thirdNumId,
                                    fourthNumId);
      break;
    default:
      MOZ_CRASH("Unexpected number of arguments to hypot function.");
  }

  writer.returnFromIC();

  trackAttached("MathHypot");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathATan2() {
  // Requires two numbers as arguments.
  if (args_.length() != 2 || !args_[0].isNumber() || !args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'atan2' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId yId = loadArgument(calleeId, ArgumentKind::Arg0);
  ValOperandId xId = loadArgument(calleeId, ArgumentKind::Arg1);

  NumberOperandId yNumberId = writer.guardIsNumber(yId);
  NumberOperandId xNumberId = writer.guardIsNumber(xId);

  writer.mathAtan2NumberResult(yNumberId, xNumberId);
  writer.returnFromIC();

  trackAttached("MathAtan2");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathMinMax(bool isMax) {
  // For now only optimize if there are 1-4 arguments.
  if (args_.length() < 1 || args_.length() > 4) {
    return AttachDecision::NoAction;
  }

  // Ensure all arguments are numbers.
  bool allInt32 = true;
  for (size_t i = 0; i < args_.length(); i++) {
    if (!args_[i].isNumber()) {
      return AttachDecision::NoAction;
    }
    if (!args_[i].isInt32()) {
      allInt32 = false;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is this Math function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  if (allInt32) {
    ValOperandId valId = loadArgument(calleeId, ArgumentKind::Arg0);
    Int32OperandId resId = writer.guardToInt32(valId);
    for (size_t i = 1; i < args_.length(); i++) {
      ValOperandId argId = loadArgument(calleeId, ArgumentKindForArgIndex(i));
      Int32OperandId argInt32Id = writer.guardToInt32(argId);
      resId = writer.int32MinMax(isMax, resId, argInt32Id);
    }
    writer.loadInt32Result(resId);
  } else {
    ValOperandId valId = loadArgument(calleeId, ArgumentKind::Arg0);
    NumberOperandId resId = writer.guardIsNumber(valId);
    for (size_t i = 1; i < args_.length(); i++) {
      ValOperandId argId = loadArgument(calleeId, ArgumentKindForArgIndex(i));
      NumberOperandId argNumId = writer.guardIsNumber(argId);
      resId = writer.numberMinMax(isMax, resId, argNumId);
    }
    writer.loadDoubleResult(resId);
  }

  writer.returnFromIC();

  trackAttached(isMax ? "MathMax" : "MathMin");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSpreadMathMinMax(
    bool isMax) {
  MOZ_ASSERT(flags_.getArgFormat() == CallFlags::Spread ||
             flags_.getArgFormat() == CallFlags::FunApplyArray);

  // The result will be an int32 if there is at least one argument,
  // and all the arguments are int32.
  bool int32Result = args_.length() > 0;
  for (size_t i = 0; i < args_.length(); i++) {
    if (!args_[i].isNumber()) {
      return AttachDecision::NoAction;
    }
    if (!args_[i].isInt32()) {
      int32Result = false;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is this Math function.
  emitNativeCalleeGuard(argcId);

  // Load the argument array.
  ObjOperandId argsId = emitLoadArgsArray();

  if (int32Result) {
    writer.int32MinMaxArrayResult(argsId, isMax);
  } else {
    writer.numberMinMaxArrayResult(argsId, isMax);
  }

  writer.returnFromIC();

  trackAttached(isMax ? "MathMaxArray" : "MathMinArray");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMathFunction(
    UnaryMathFunction fun) {
  // Need one argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }

  if (math_use_fdlibm_for_sin_cos_tan() ||
      target_->realm()->creationOptions().alwaysUseFdlibm()) {
    switch (fun) {
      case UnaryMathFunction::SinNative:
        fun = UnaryMathFunction::SinFdlibm;
        break;
      case UnaryMathFunction::CosNative:
        fun = UnaryMathFunction::CosFdlibm;
        break;
      case UnaryMathFunction::TanNative:
        fun = UnaryMathFunction::TanFdlibm;
        break;
      default:
        break;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is this Math function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);
  NumberOperandId numberId = writer.guardIsNumber(argumentId);
  writer.mathFunctionNumberResult(numberId, fun);
  writer.returnFromIC();

  trackAttached("MathFunction");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachNumber() {
  // Expect a single string argument.
  if (args_.length() != 1 || !args_[0].isString()) {
    return AttachDecision::NoAction;
  }

  double num;
  if (!StringToNumber(cx_, args_[0].toString(), &num)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `Number` function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard that the argument is a string.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  StringOperandId strId = writer.guardToString(argId);

  // Return either an Int32 or Double result.
  int32_t unused;
  if (mozilla::NumberIsInt32(num, &unused)) {
    Int32OperandId resultId = writer.guardStringToInt32(strId);
    writer.loadInt32Result(resultId);
  } else {
    NumberOperandId resultId = writer.guardStringToNumber(strId);
    writer.loadDoubleResult(resultId);
  }
  writer.returnFromIC();

  trackAttached("Number");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachNumberParseInt() {
  // Expected arguments: input (string or number), optional radix (int32).
  if (args_.length() < 1 || args_.length() > 2) {
    return AttachDecision::NoAction;
  }
  if (!args_[0].isString() && !args_[0].isNumber()) {
    return AttachDecision::NoAction;
  }
  if (args_[0].isDouble()) {
    double d = args_[0].toDouble();

    // See num_parseInt for why we have to reject numbers smaller than 1.0e-6.
    // Negative numbers in the exclusive range (-1, -0) return -0.
    bool canTruncateToInt32 =
        (DOUBLE_DECIMAL_IN_SHORTEST_LOW <= d && d <= double(INT32_MAX)) ||
        (double(INT32_MIN) <= d && d <= -1.0) || (d == 0.0);
    if (!canTruncateToInt32) {
      return AttachDecision::NoAction;
    }
  }
  if (args_.length() > 1 && !args_[1].isInt32(10)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'parseInt' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  auto guardRadix = [&]() {
    ValOperandId radixId = loadArgument(calleeId, ArgumentKind::Arg1);
    Int32OperandId intRadixId = writer.guardToInt32(radixId);
    writer.guardSpecificInt32(intRadixId, 10);
    return intRadixId;
  };

  ValOperandId inputId = loadArgument(calleeId, ArgumentKind::Arg0);

  if (args_[0].isString()) {
    StringOperandId strId = writer.guardToString(inputId);

    Int32OperandId intRadixId;
    if (args_.length() > 1) {
      intRadixId = guardRadix();
    } else {
      intRadixId = writer.loadInt32Constant(0);
    }

    writer.numberParseIntResult(strId, intRadixId);
  } else if (args_[0].isInt32()) {
    Int32OperandId intId = writer.guardToInt32(inputId);
    if (args_.length() > 1) {
      guardRadix();
    }
    writer.loadInt32Result(intId);
  } else {
    MOZ_ASSERT(args_[0].isDouble());

    NumberOperandId numId = writer.guardIsNumber(inputId);
    if (args_.length() > 1) {
      guardRadix();
    }
    writer.doubleParseIntResult(numId);
  }

  writer.returnFromIC();

  trackAttached("NumberParseInt");
  return AttachDecision::Attach;
}

StringOperandId IRGenerator::emitToStringGuard(ValOperandId id,
                                               const Value& v) {
  MOZ_ASSERT(CanConvertToString(v));
  if (v.isString()) {
    return writer.guardToString(id);
  }
  if (v.isBoolean()) {
    BooleanOperandId boolId = writer.guardToBoolean(id);
    return writer.booleanToString(boolId);
  }
  if (v.isNull()) {
    writer.guardIsNull(id);
    return writer.loadConstantString(cx_->names().null);
  }
  if (v.isUndefined()) {
    writer.guardIsUndefined(id);
    return writer.loadConstantString(cx_->names().undefined);
  }
  if (v.isInt32()) {
    Int32OperandId intId = writer.guardToInt32(id);
    return writer.callInt32ToString(intId);
  }
  // At this point we are creating an IC that will handle
  // both Int32 and Double cases.
  MOZ_ASSERT(v.isNumber());
  NumberOperandId numId = writer.guardIsNumber(id);
  return writer.callNumberToString(numId);
}

AttachDecision InlinableNativeIRGenerator::tryAttachNumberToString() {
  // Expecting no arguments or a single int32 argument.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }
  if (args_.length() == 1 && !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a primitive number value.
  if (!thisval_.isNumber()) {
    return AttachDecision::NoAction;
  }

  // No arguments means base 10.
  int32_t base = 10;
  if (args_.length() > 0) {
    base = args_[0].toInt32();
    if (base < 2 || base > 36) {
      return AttachDecision::NoAction;
    }

    // Non-decimal bases currently only support int32 inputs.
    if (base != 10 && !thisval_.isInt32()) {
      return AttachDecision::NoAction;
    }
  }
  MOZ_ASSERT(2 <= base && base <= 36);

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'toString' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Initialize the |this| operand.
  ValOperandId thisValId = loadThis(calleeId);

  // Guard on number and convert to string.
  if (base == 10) {
    // If an explicit base was passed, guard its value.
    if (args_.length() > 0) {
      // Guard the `base` argument is an int32.
      ValOperandId baseId = loadArgument(calleeId, ArgumentKind::Arg0);
      Int32OperandId intBaseId = writer.guardToInt32(baseId);

      // Guard `base` is 10 for decimal toString representation.
      writer.guardSpecificInt32(intBaseId, 10);
    }

    StringOperandId strId = emitToStringGuard(thisValId, thisval_);

    // Return the string.
    writer.loadStringResult(strId);
  } else {
    MOZ_ASSERT(args_.length() > 0);

    // Guard the |this| value is an int32.
    Int32OperandId thisIntId = writer.guardToInt32(thisValId);

    // Guard the `base` argument is an int32.
    ValOperandId baseId = loadArgument(calleeId, ArgumentKind::Arg0);
    Int32OperandId intBaseId = writer.guardToInt32(baseId);

    // Return the string.
    writer.int32ToStringWithBaseResult(thisIntId, intBaseId);
  }

  writer.returnFromIC();

  trackAttached("NumberToString");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachReflectGetPrototypeOf() {
  // Need one argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'getPrototypeOf' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argumentId = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(argumentId);

  writer.reflectGetPrototypeOfResult(objId);
  writer.returnFromIC();

  trackAttached("ReflectGetPrototypeOf");
  return AttachDecision::Attach;
}

enum class AtomicAccess { Read, Write };

static bool AtomicsMeetsPreconditions(TypedArrayObject* typedArray,
                                      const Value& index, AtomicAccess access) {
  // Can't write into immutable TypedArrays.
  if (access == AtomicAccess::Write &&
      typedArray->is<ImmutableTypedArrayObject>()) {
    return false;
  }

  switch (typedArray->type()) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      break;

    case Scalar::Float16:
    case Scalar::Float32:
    case Scalar::Float64:
    case Scalar::Uint8Clamped:
      // Exclude floating types and Uint8Clamped.
      return false;

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      MOZ_CRASH("Unsupported TypedArray type");
  }

  // Bounds check the index argument.
  int64_t indexInt64;
  if (!ValueIsInt64Index(index, &indexInt64)) {
    return false;
  }
  if (indexInt64 < 0 ||
      uint64_t(indexInt64) >= typedArray->length().valueOr(0)) {
    return false;
  }

  return true;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsCompareExchange() {
  if (!JitSupportsAtomics()) {
    return AttachDecision::NoAction;
  }

  // Need four arguments.
  if (args_.length() != 4) {
    return AttachDecision::NoAction;
  }

  // Arguments: typedArray, index (number), expected, replacement.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1], AtomicAccess::Write)) {
    return AttachDecision::NoAction;
  }

  Scalar::Type elementType = typedArray->type();
  if (!ValueCanConvertToNumeric(elementType, args_[2])) {
    return AttachDecision::NoAction;
  }
  if (!ValueCanConvertToNumeric(elementType, args_[3])) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `compareExchange` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId = loadArgument(calleeId, ArgumentKind::Arg1);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  // Convert expected value to int32/BigInt.
  ValOperandId expectedId = loadArgument(calleeId, ArgumentKind::Arg2);
  OperandId numericExpectedId =
      emitNumericGuard(expectedId, args_[2], elementType);

  // Convert replacement value to int32/BigInt.
  ValOperandId replacementId = loadArgument(calleeId, ArgumentKind::Arg3);
  OperandId numericReplacementId =
      emitNumericGuard(replacementId, args_[3], elementType);

  auto viewKind = ToArrayBufferViewKind(typedArray);
  writer.atomicsCompareExchangeResult(objId, intPtrIndexId, numericExpectedId,
                                      numericReplacementId, typedArray->type(),
                                      viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsCompareExchange");
  return AttachDecision::Attach;
}

bool InlinableNativeIRGenerator::canAttachAtomicsReadWriteModify() {
  if (!JitSupportsAtomics()) {
    return false;
  }

  // Need three arguments.
  if (args_.length() != 3) {
    return false;
  }

  // Arguments: typedArray, index (number), value.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return false;
  }
  if (!args_[1].isNumber()) {
    return false;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1], AtomicAccess::Write)) {
    return false;
  }
  if (!ValueCanConvertToNumeric(typedArray->type(), args_[2])) {
    return false;
  }
  return true;
}

InlinableNativeIRGenerator::AtomicsReadWriteModifyOperands
InlinableNativeIRGenerator::emitAtomicsReadWriteModifyOperands() {
  MOZ_ASSERT(canAttachAtomicsReadWriteModify());

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is this Atomics function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId = loadArgument(calleeId, ArgumentKind::Arg1);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  // Convert value to int32/BigInt.
  ValOperandId valueId = loadArgument(calleeId, ArgumentKind::Arg2);
  OperandId numericValueId =
      emitNumericGuard(valueId, args_[2], typedArray->type());

  return {objId, intPtrIndexId, numericValueId};
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsExchange() {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands();

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  auto viewKind = ToArrayBufferViewKind(typedArray);

  writer.atomicsExchangeResult(objId, intPtrIndexId, numericValueId,
                               typedArray->type(), viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsExchange");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsAdd() {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands();

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = ignoresResult();
  auto viewKind = ToArrayBufferViewKind(typedArray);

  writer.atomicsAddResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect, viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsAdd");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsSub() {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands();

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = ignoresResult();
  auto viewKind = ToArrayBufferViewKind(typedArray);

  writer.atomicsSubResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect, viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsSub");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsAnd() {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands();

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = ignoresResult();
  auto viewKind = ToArrayBufferViewKind(typedArray);

  writer.atomicsAndResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect, viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsAnd");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsOr() {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands();

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = ignoresResult();
  auto viewKind = ToArrayBufferViewKind(typedArray);

  writer.atomicsOrResult(objId, intPtrIndexId, numericValueId,
                         typedArray->type(), forEffect, viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsOr");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsXor() {
  if (!canAttachAtomicsReadWriteModify()) {
    return AttachDecision::NoAction;
  }

  auto [objId, intPtrIndexId, numericValueId] =
      emitAtomicsReadWriteModifyOperands();

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  bool forEffect = ignoresResult();
  auto viewKind = ToArrayBufferViewKind(typedArray);

  writer.atomicsXorResult(objId, intPtrIndexId, numericValueId,
                          typedArray->type(), forEffect, viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsXor");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsLoad() {
  if (!JitSupportsAtomics()) {
    return AttachDecision::NoAction;
  }

  // Need two arguments.
  if (args_.length() != 2) {
    return AttachDecision::NoAction;
  }

  // Arguments: typedArray, index (number).
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1], AtomicAccess::Read)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `load` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId = loadArgument(calleeId, ArgumentKind::Arg1);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  auto viewKind = ToArrayBufferViewKind(typedArray);
  writer.atomicsLoadResult(objId, intPtrIndexId, typedArray->type(), viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsLoad");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsStore() {
  if (!JitSupportsAtomics()) {
    return AttachDecision::NoAction;
  }

  // Need three arguments.
  if (args_.length() != 3) {
    return AttachDecision::NoAction;
  }

  // Atomics.store() is annoying because it returns the result of converting the
  // value by ToInteger(), not the input value, nor the result of converting the
  // value by ToInt32(). It is especially annoying because almost nobody uses
  // the result value.
  //
  // As an expedient compromise, therefore, we inline only if the result is
  // obviously unused or if the argument is already Int32 and thus requires no
  // conversion.

  // Arguments: typedArray, index (number), value.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }
  if (!args_[1].isNumber()) {
    return AttachDecision::NoAction;
  }

  auto* typedArray = &args_[0].toObject().as<TypedArrayObject>();
  if (!AtomicsMeetsPreconditions(typedArray, args_[1], AtomicAccess::Write)) {
    return AttachDecision::NoAction;
  }

  Scalar::Type elementType = typedArray->type();
  if (!ValueCanConvertToNumeric(elementType, args_[2])) {
    return AttachDecision::NoAction;
  }

  bool guardIsInt32 = !Scalar::isBigIntType(elementType) && !ignoresResult();

  if (guardIsInt32 && !args_[2].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `store` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);
  writer.guardShapeForClass(objId, typedArray->shape());

  // Convert index to intPtr.
  ValOperandId indexId = loadArgument(calleeId, ArgumentKind::Arg1);
  IntPtrOperandId intPtrIndexId =
      guardToIntPtrIndex(args_[1], indexId, /* supportOOB = */ false);

  // Ensure value is int32 or BigInt.
  ValOperandId valueId = loadArgument(calleeId, ArgumentKind::Arg2);
  OperandId numericValueId;
  if (guardIsInt32) {
    numericValueId = writer.guardToInt32(valueId);
  } else {
    numericValueId = emitNumericGuard(valueId, args_[2], elementType);
  }

  auto viewKind = ToArrayBufferViewKind(typedArray);
  writer.atomicsStoreResult(objId, intPtrIndexId, numericValueId,
                            typedArray->type(), viewKind);
  writer.returnFromIC();

  trackAttached("AtomicsStore");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsIsLockFree() {
  // Need one argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  if (!args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `isLockFree` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Ensure value is int32.
  ValOperandId valueId = loadArgument(calleeId, ArgumentKind::Arg0);
  Int32OperandId int32ValueId = writer.guardToInt32(valueId);

  writer.atomicsIsLockFreeResult(int32ValueId);
  writer.returnFromIC();

  trackAttached("AtomicsIsLockFree");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAtomicsPause() {
  // We don't yet support inlining when the iteration count argument is present.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `pause` native function.
  emitNativeCalleeGuard(argcId);

  writer.atomicsPauseResult();
  writer.returnFromIC();

  trackAttached("AtomicsPause");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachBoolean() {
  // Need zero or one argument.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'Boolean' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  if (args_.length() == 0) {
    writer.loadBooleanResult(false);
  } else {
    ValOperandId valId = loadArgument(calleeId, ArgumentKind::Arg0);

    writer.loadValueTruthyResult(valId);
  }

  writer.returnFromIC();

  trackAttached("Boolean");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachBailout() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'bailout' native function.
  emitNativeCalleeGuard(argcId);

  writer.bailout();
  writer.loadUndefinedResult();
  writer.returnFromIC();

  trackAttached("Bailout");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAssertFloat32() {
  // Expecting two arguments.
  if (args_.length() != 2) {
    return AttachDecision::NoAction;
  }

  // (Fuzzing unsafe) testing function which must be called with a constant
  // boolean as its second argument.
  bool mustBeFloat32 = args_[1].toBoolean();

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'assertFloat32' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId valId = loadArgument(calleeId, ArgumentKind::Arg0);

  writer.assertFloat32Result(valId, mustBeFloat32);
  writer.returnFromIC();

  trackAttached("AssertFloat32");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachAssertRecoveredOnBailout() {
  // Expecting two arguments.
  if (args_.length() != 2) {
    return AttachDecision::NoAction;
  }

  // (Fuzzing unsafe) testing function which must be called with a constant
  // boolean as its second argument.
  bool mustBeRecovered = args_[1].toBoolean();

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'assertRecoveredOnBailout' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId valId = loadArgument(calleeId, ArgumentKind::Arg0);

  writer.assertRecoveredOnBailoutResult(valId, mustBeRecovered);
  writer.returnFromIC();

  trackAttached("AssertRecoveredOnBailout");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachObjectIs() {
  // Need two arguments.
  if (args_.length() != 2) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `is` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId lhsId = loadArgument(calleeId, ArgumentKind::Arg0);
  ValOperandId rhsId = loadArgument(calleeId, ArgumentKind::Arg1);

  HandleValue lhs = args_[0];
  HandleValue rhs = args_[1];

  if (!isFirstStub()) {
    writer.sameValueResult(lhsId, rhsId);
  } else if (lhs.isNumber() && rhs.isNumber() &&
             !(lhs.isInt32() && rhs.isInt32())) {
    NumberOperandId lhsNumId = writer.guardIsNumber(lhsId);
    NumberOperandId rhsNumId = writer.guardIsNumber(rhsId);
    writer.compareDoubleSameValueResult(lhsNumId, rhsNumId);
  } else if (!SameType(lhs, rhs)) {
    // Compare tags for strictly different types.
    ValueTagOperandId lhsTypeId = writer.loadValueTag(lhsId);
    ValueTagOperandId rhsTypeId = writer.loadValueTag(rhsId);
    writer.guardTagNotEqual(lhsTypeId, rhsTypeId);
    writer.loadBooleanResult(false);
  } else {
    MOZ_ASSERT(lhs.type() == rhs.type());
    MOZ_ASSERT(lhs.type() != JS::ValueType::Double);

    switch (lhs.type()) {
      case JS::ValueType::Int32: {
        Int32OperandId lhsIntId = writer.guardToInt32(lhsId);
        Int32OperandId rhsIntId = writer.guardToInt32(rhsId);
        writer.compareInt32Result(JSOp::StrictEq, lhsIntId, rhsIntId);
        break;
      }
      case JS::ValueType::Boolean: {
        Int32OperandId lhsIntId = writer.guardBooleanToInt32(lhsId);
        Int32OperandId rhsIntId = writer.guardBooleanToInt32(rhsId);
        writer.compareInt32Result(JSOp::StrictEq, lhsIntId, rhsIntId);
        break;
      }
      case JS::ValueType::Undefined: {
        writer.guardIsUndefined(lhsId);
        writer.guardIsUndefined(rhsId);
        writer.loadBooleanResult(true);
        break;
      }
      case JS::ValueType::Null: {
        writer.guardIsNull(lhsId);
        writer.guardIsNull(rhsId);
        writer.loadBooleanResult(true);
        break;
      }
      case JS::ValueType::String: {
        StringOperandId lhsStrId = writer.guardToString(lhsId);
        StringOperandId rhsStrId = writer.guardToString(rhsId);
        writer.compareStringResult(JSOp::StrictEq, lhsStrId, rhsStrId);
        break;
      }
      case JS::ValueType::Symbol: {
        SymbolOperandId lhsSymId = writer.guardToSymbol(lhsId);
        SymbolOperandId rhsSymId = writer.guardToSymbol(rhsId);
        writer.compareSymbolResult(JSOp::StrictEq, lhsSymId, rhsSymId);
        break;
      }
      case JS::ValueType::BigInt: {
        BigIntOperandId lhsBigIntId = writer.guardToBigInt(lhsId);
        BigIntOperandId rhsBigIntId = writer.guardToBigInt(rhsId);
        writer.compareBigIntResult(JSOp::StrictEq, lhsBigIntId, rhsBigIntId);
        break;
      }
      case JS::ValueType::Object: {
        ObjOperandId lhsObjId = writer.guardToObject(lhsId);
        ObjOperandId rhsObjId = writer.guardToObject(rhsId);
        writer.compareObjectResult(JSOp::StrictEq, lhsObjId, rhsObjId);
        break;
      }

      case JS::ValueType::Double:
      case JS::ValueType::Magic:
      case JS::ValueType::PrivateGCThing:
        MOZ_CRASH("Unexpected type");
    }
  }

  writer.returnFromIC();

  trackAttached("ObjectIs");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachObjectIsPrototypeOf() {
  // Ensure |this| is an object.
  if (!thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `isPrototypeOf` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard that |this| is an object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);

  writer.loadInstanceOfObjectResult(argId, thisObjId);
  writer.returnFromIC();

  trackAttached("ObjectIsPrototypeOf");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachObjectKeys() {
  // Only handle argc <= 1.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Do not attach any IC if the argument is not an object.
  if (!args_[0].isObject()) {
    return AttachDecision::NoAction;
  }
  // Do not attach any IC if the argument is a Proxy. While implementation could
  // work with proxies the goal of this implementation is to provide an
  // optimization for calls of `Object.keys(obj)` where there is no side-effect,
  // and where the computation of the array of property name can be moved.
  const JSClass* clasp = args_[0].toObject().getClass();
  if (clasp->isProxyObject()) {
    return AttachDecision::NoAction;
  }

  // Generate cache IR code to attach a new inline cache which will delegate the
  // call to Object.keys to the native function.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'keys' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Implicit: Note `Object.keys` is a property of the `Object` global. The fact
  // that we are in this function implies that we already identify the function
  // as being the proper one. Thus there should not be any need to validate that
  // this is the proper function. (test: ion/object-keys-05)

  // Guard `arg0` is an object.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId argObjId = writer.guardToObject(argId);

  // Guard against proxies.
  writer.guardIsNotProxy(argObjId);

  // Compute the keys array.
  writer.objectKeysResult(argObjId);

  writer.returnFromIC();

  trackAttached("ObjectKeys");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachObjectToString() {
  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Ensure |this| is an object.
  if (!thisval_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Don't attach if the object has @@toStringTag or is a proxy.
  if (!ObjectClassToString(cx_, &thisval_.toObject())) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'toString' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard that |this| is an object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId thisObjId = writer.guardToObject(thisValId);

  writer.objectToStringResult(thisObjId);
  writer.returnFromIC();

  trackAttached("ObjectToString");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachBigInt() {
  // Need a single argument (Int32).
  if (args_.length() != 1 || !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'BigInt' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard that the argument is an Int32.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  Int32OperandId int32Id = writer.guardToInt32(argId);

  // Convert Int32 to BigInt.
  IntPtrOperandId intptrId = writer.int32ToIntPtr(int32Id);
  writer.intPtrToBigIntResult(intptrId);
  writer.returnFromIC();

  trackAttached("BigInt");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachBigIntAsIntN() {
  // Need two arguments (Int32, BigInt).
  if (args_.length() != 2 || !args_[0].isInt32() || !args_[1].isBigInt()) {
    return AttachDecision::NoAction;
  }

  // Negative bits throws an error.
  if (args_[0].toInt32() < 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'BigInt.asIntN' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Convert bits to int32.
  ValOperandId bitsId = loadArgument(calleeId, ArgumentKind::Arg0);
  Int32OperandId int32BitsId = EmitGuardToInt32Index(writer, args_[0], bitsId);

  // Number of bits mustn't be negative.
  writer.guardInt32IsNonNegative(int32BitsId);

  ValOperandId arg1Id = loadArgument(calleeId, ArgumentKind::Arg1);
  BigIntOperandId bigIntId = writer.guardToBigInt(arg1Id);

  writer.bigIntAsIntNResult(int32BitsId, bigIntId);
  writer.returnFromIC();

  trackAttached("BigIntAsIntN");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachBigIntAsUintN() {
  // Need two arguments (Int32, BigInt).
  if (args_.length() != 2 || !args_[0].isInt32() || !args_[1].isBigInt()) {
    return AttachDecision::NoAction;
  }

  // Negative bits throws an error.
  if (args_[0].toInt32() < 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'BigInt.asUintN' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Convert bits to int32.
  ValOperandId bitsId = loadArgument(calleeId, ArgumentKind::Arg0);
  Int32OperandId int32BitsId = EmitGuardToInt32Index(writer, args_[0], bitsId);

  // Number of bits mustn't be negative.
  writer.guardInt32IsNonNegative(int32BitsId);

  ValOperandId arg1Id = loadArgument(calleeId, ArgumentKind::Arg1);
  BigIntOperandId bigIntId = writer.guardToBigInt(arg1Id);

  writer.bigIntAsUintNResult(int32BitsId, bigIntId);
  writer.returnFromIC();

  trackAttached("BigIntAsUintN");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSetHas() {
  // Ensure |this| is a SetObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<SetObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'has' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a SetObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Set);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);

#ifndef JS_CODEGEN_X86
  // Assume the hash key will likely always have the same type when attaching
  // the first stub. If the call is polymorphic on the hash key, attach a stub
  // which handles any value.
  if (isFirstStub()) {
    switch (args_[0].type()) {
      case ValueType::Double:
      case ValueType::Int32:
      case ValueType::Boolean:
      case ValueType::Undefined:
      case ValueType::Null: {
        writer.guardToNonGCThing(argId);
        writer.setHasNonGCThingResult(objId, argId);
        break;
      }
      case ValueType::String: {
        StringOperandId strId = writer.guardToString(argId);
        writer.setHasStringResult(objId, strId);
        break;
      }
      case ValueType::Symbol: {
        SymbolOperandId symId = writer.guardToSymbol(argId);
        writer.setHasSymbolResult(objId, symId);
        break;
      }
      case ValueType::BigInt: {
        BigIntOperandId bigIntId = writer.guardToBigInt(argId);
        writer.setHasBigIntResult(objId, bigIntId);
        break;
      }
      case ValueType::Object: {
        // Currently only supported on 64-bit platforms.
#  ifdef JS_PUNBOX64
        ObjOperandId valId = writer.guardToObject(argId);
        writer.setHasObjectResult(objId, valId);
#  else
        writer.setHasResult(objId, argId);
#  endif
        break;
      }

      case ValueType::Magic:
      case ValueType::PrivateGCThing:
        MOZ_CRASH("Unexpected type");
    }
  } else {
    writer.setHasResult(objId, argId);
  }
#else
  // The optimized versions require too many registers on x86.
  writer.setHasResult(objId, argId);
#endif

  writer.returnFromIC();

  trackAttached("SetHas");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSetDelete() {
  // Ensure |this| is a SetObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<SetObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'delete' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a SetObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Set);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  writer.setDeleteResult(objId, argId);
  writer.returnFromIC();

  trackAttached("SetDelete");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSetAdd() {
  // Ensure |this| is a SetObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<SetObject>()) {
    return AttachDecision::NoAction;
  }

  // Need one argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'add' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a SetObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Set);

  ValOperandId keyId = loadArgument(calleeId, ArgumentKind::Arg0);
  writer.setAddResult(objId, keyId);
  writer.returnFromIC();

  trackAttached("SetAdd");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSetSize() {
  // Ensure |this| is a SetObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<SetObject>()) {
    return AttachDecision::NoAction;
  }

  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'size' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a SetObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  writer.guardClass(objId, GuardClassKind::Set);

  writer.setSizeResult(objId);
  writer.returnFromIC();

  trackAttached("SetSize");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMapHas() {
  // Ensure |this| is a MapObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<MapObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'has' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a MapObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Map);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);

#ifndef JS_CODEGEN_X86
  // Assume the hash key will likely always have the same type when attaching
  // the first stub. If the call is polymorphic on the hash key, attach a stub
  // which handles any value.
  if (isFirstStub()) {
    switch (args_[0].type()) {
      case ValueType::Double:
      case ValueType::Int32:
      case ValueType::Boolean:
      case ValueType::Undefined:
      case ValueType::Null: {
        writer.guardToNonGCThing(argId);
        writer.mapHasNonGCThingResult(objId, argId);
        break;
      }
      case ValueType::String: {
        StringOperandId strId = writer.guardToString(argId);
        writer.mapHasStringResult(objId, strId);
        break;
      }
      case ValueType::Symbol: {
        SymbolOperandId symId = writer.guardToSymbol(argId);
        writer.mapHasSymbolResult(objId, symId);
        break;
      }
      case ValueType::BigInt: {
        BigIntOperandId bigIntId = writer.guardToBigInt(argId);
        writer.mapHasBigIntResult(objId, bigIntId);
        break;
      }
      case ValueType::Object: {
        // Currently only supported on 64-bit platforms.
#  ifdef JS_PUNBOX64
        ObjOperandId valId = writer.guardToObject(argId);
        writer.mapHasObjectResult(objId, valId);
#  else
        writer.mapHasResult(objId, argId);
#  endif
        break;
      }

      case ValueType::Magic:
      case ValueType::PrivateGCThing:
        MOZ_CRASH("Unexpected type");
    }
  } else {
    writer.mapHasResult(objId, argId);
  }
#else
  // The optimized versions require too many registers on x86.
  writer.mapHasResult(objId, argId);
#endif

  writer.returnFromIC();

  trackAttached("MapHas");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMapGet() {
  // Ensure |this| is a MapObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<MapObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'get' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a MapObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Map);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);

#ifndef JS_CODEGEN_X86
  // Assume the hash key will likely always have the same type when attaching
  // the first stub. If the call is polymorphic on the hash key, attach a stub
  // which handles any value.
  if (isFirstStub()) {
    switch (args_[0].type()) {
      case ValueType::Double:
      case ValueType::Int32:
      case ValueType::Boolean:
      case ValueType::Undefined:
      case ValueType::Null: {
        writer.guardToNonGCThing(argId);
        writer.mapGetNonGCThingResult(objId, argId);
        break;
      }
      case ValueType::String: {
        StringOperandId strId = writer.guardToString(argId);
        writer.mapGetStringResult(objId, strId);
        break;
      }
      case ValueType::Symbol: {
        SymbolOperandId symId = writer.guardToSymbol(argId);
        writer.mapGetSymbolResult(objId, symId);
        break;
      }
      case ValueType::BigInt: {
        BigIntOperandId bigIntId = writer.guardToBigInt(argId);
        writer.mapGetBigIntResult(objId, bigIntId);
        break;
      }
      case ValueType::Object: {
        // Currently only supported on 64-bit platforms.
#  ifdef JS_PUNBOX64
        ObjOperandId valId = writer.guardToObject(argId);
        writer.mapGetObjectResult(objId, valId);
#  else
        writer.mapGetResult(objId, argId);
#  endif
        break;
      }

      case ValueType::Magic:
      case ValueType::PrivateGCThing:
        MOZ_CRASH("Unexpected type");
    }
  } else {
    writer.mapGetResult(objId, argId);
  }
#else
  // The optimized versions require too many registers on x86.
  writer.mapGetResult(objId, argId);
#endif

  writer.returnFromIC();

  trackAttached("MapGet");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMapDelete() {
  // Ensure |this| is a MapObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<MapObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single argument.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'delete' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a MapObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Map);

  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  writer.mapDeleteResult(objId, argId);
  writer.returnFromIC();

  trackAttached("MapDelete");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMapSet() {
#ifdef JS_CODEGEN_X86
  // 32-bit x86 does not have enough registers for the AutoCallVM output, the
  // MapObject*, and two Values.
  return AttachDecision::NoAction;
#endif

  // Ensure |this| is a MapObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<MapObject>()) {
    return AttachDecision::NoAction;
  }

  // Need two arguments.
  if (args_.length() != 2) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'set' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a MapObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Map);

  ValOperandId keyId = loadArgument(calleeId, ArgumentKind::Arg0);
  ValOperandId valId = loadArgument(calleeId, ArgumentKind::Arg1);
  writer.mapSetResult(objId, keyId, valId);
  writer.returnFromIC();

  trackAttached("MapSet");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachWeakMapGet() {
  // Ensure |this| is a WeakMapObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<WeakMapObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single object argument.
  if (args_.length() != 1 || !args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'get' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a WeakMapObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(),
                           GuardClassKind::WeakMap);

  // Guard the argument is an object.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);

  writer.weakMapGetObjectResult(objId, objArgId);
  writer.returnFromIC();

  trackAttached("WeakMapGet");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachWeakMapHas() {
  // Ensure |this| is a WeakMapObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<WeakMapObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single object argument.
  if (args_.length() != 1 || !args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'has' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a WeakMapObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(),
                           GuardClassKind::WeakMap);

  // Guard the argument is an object.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);

  writer.weakMapHasObjectResult(objId, objArgId);
  writer.returnFromIC();

  trackAttached("WeakMapHas");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachWeakSetHas() {
  // Ensure |this| is a WeakSetObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<WeakSetObject>()) {
    return AttachDecision::NoAction;
  }

  // Need a single object argument.
  if (args_.length() != 1 || !args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'has' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a WeakSetObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(),
                           GuardClassKind::WeakSet);

  // Guard the argument is an object.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);

  writer.weakSetHasObjectResult(objId, objArgId);
  writer.returnFromIC();

  trackAttached("WeakSetHas");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachDateGetTime() {
  // Ensure |this| is a DateObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<DateObject>()) {
    return AttachDecision::NoAction;
  }

  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'getTime' (or 'valueOf') native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a DateObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Date);

  writer.loadFixedSlotTypedResult(objId, DateObject::offsetOfUTCTimeSlot(),
                                  ValueType::Double);

  writer.returnFromIC();

  trackAttached("DateGetTime");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachDateGet(
    DateComponent component) {
  // Ensure |this| is a DateObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<DateObject>()) {
    return AttachDecision::NoAction;
  }

  // Expecting no arguments.
  if (args_.length() != 0) {
    return AttachDecision::NoAction;
  }

  // Can't check DateTime cache when time zone is forced to UTC.
  if (cx_->realm()->creationOptions().forceUTC()) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the Date native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a DateObject.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);
  emitOptimisticClassGuard(objId, &thisval_.toObject(), GuardClassKind::Date);

  // Fill in the local time slots.
  writer.dateFillLocalTimeSlots(objId);

  switch (component) {
    case DateComponent::FullYear:
      writer.loadFixedSlotResult(objId, DateObject::offsetOfLocalYearSlot());
      break;
    case DateComponent::Month:
      writer.loadFixedSlotResult(objId, DateObject::offsetOfLocalMonthSlot());
      break;
    case DateComponent::Date:
      writer.loadFixedSlotResult(objId, DateObject::offsetOfLocalDateSlot());
      break;
    case DateComponent::Day:
      writer.loadFixedSlotResult(objId, DateObject::offsetOfLocalDaySlot());
      break;
    case DateComponent::Hours: {
      ValOperandId secondsIntoYearValId = writer.loadFixedSlot(
          objId, DateObject::offsetOfLocalSecondsIntoYearSlot());
      writer.dateHoursFromSecondsIntoYearResult(secondsIntoYearValId);
      break;
    }
    case DateComponent::Minutes: {
      ValOperandId secondsIntoYearValId = writer.loadFixedSlot(
          objId, DateObject::offsetOfLocalSecondsIntoYearSlot());
      writer.dateMinutesFromSecondsIntoYearResult(secondsIntoYearValId);
      break;
    }
    case DateComponent::Seconds: {
      ValOperandId secondsIntoYearValId = writer.loadFixedSlot(
          objId, DateObject::offsetOfLocalSecondsIntoYearSlot());
      writer.dateSecondsFromSecondsIntoYearResult(secondsIntoYearValId);
      break;
    }
  }

  writer.returnFromIC();

  switch (component) {
    case DateComponent::FullYear:
      trackAttached("DateGetFullYear");
      break;
    case DateComponent::Month:
      trackAttached("DateGetMonth");
      break;
    case DateComponent::Date:
      trackAttached("DateGetDate");
      break;
    case DateComponent::Day:
      trackAttached("DateGetDay");
      break;
    case DateComponent::Hours:
      trackAttached("DateGetHours");
      break;
    case DateComponent::Minutes:
      trackAttached("DateGetMinutes");
      break;
    case DateComponent::Seconds:
      trackAttached("DateGetSeconds");
      break;
  }
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachFunCall(HandleFunction callee) {
  MOZ_ASSERT(callee->isNativeWithoutJitEntry());

  if (callee->native() != fun_call) {
    return AttachDecision::NoAction;
  }

  if (!thisval_.isObject() || !thisval_.toObject().is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  RootedFunction target(cx_, &thisval_.toObject().as<JSFunction>());

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  if (target->isClassConstructor()) {
    return AttachDecision::NoAction;
  }

  CallFlags targetFlags(CallFlags::FunCall);
  if (mode_ == ICState::Mode::Specialized) {
    if (cx_->realm() == target->realm()) {
      targetFlags.setIsSameRealm();
    }
  }

  if (mode_ == ICState::Mode::Specialized && !isScripted) {
    HandleValue newTarget = NullHandleValue;
    HandleValue thisValue = argc_ > 0 ? args_[0] : UndefinedHandleValue;
    HandleValueArray args =
        argc_ > 0 ? HandleValueArray::subarray(args_, 1, args_.length() - 1)
                  : HandleValueArray::empty();

    // Check for specific native-function optimizations.
    InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                         args, targetFlags);
    TRY_ATTACH(nativeGen.tryAttachStub());
  }

  Int32OperandId argcId(writer.setInputOperandId(0));
  ObjOperandId thisObjId = emitFunCallGuard(argcId);

  if (mode_ == ICState::Mode::Specialized) {
    // Ensure that |this| is the expected target function.
    emitCalleeGuard(thisObjId, target);

    if (isScripted) {
      writer.callScriptedFunction(thisObjId, argcId, targetFlags,
                                  ClampFixedArgc(argc_));
    } else {
      writer.callNativeFunction(thisObjId, argcId, op_, target, targetFlags,
                                ClampFixedArgc(argc_));
    }
  } else {
    // Guard that |this| is a function.
    writer.guardClass(thisObjId, GuardClassKind::JSFunction);

    // Guard that function is not a class constructor.
    writer.guardNotClassConstructor(thisObjId);

    if (isScripted) {
      writer.guardFunctionHasJitEntry(thisObjId);
      writer.callScriptedFunction(thisObjId, argcId, targetFlags,
                                  ClampFixedArgc(argc_));
    } else {
      writer.guardFunctionHasNoJitEntry(thisObjId);
      writer.callAnyNativeFunction(thisObjId, argcId, targetFlags,
                                   ClampFixedArgc(argc_));
    }
  }

  writer.returnFromIC();

  if (isScripted) {
    trackAttached("Scripted fun_call");
  } else {
    trackAttached("Native fun_call");
  }

  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachTypedArrayFill() {
  // Expected arguments: value, optional start, optional end.
  if (args_.length() < 1 || args_.length() > 3) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a TypedArrayObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Both arguments must be valid indices.
  int64_t unusedIndex;
  if (args_.length() > 1 && !ValueIsInt64Index(args_[1], &unusedIndex)) {
    return AttachDecision::NoAction;
  }
  if (args_.length() > 2 && !ValueIsInt64Index(args_[2], &unusedIndex)) {
    return AttachDecision::NoAction;
  }

  auto* tarr = &thisval_.toObject().as<TypedArrayObject>();
  auto elementType = tarr->type();

  // Detached buffers throw.
  if (tarr->hasDetachedBuffer()) {
    return AttachDecision::NoAction;
  }

  // Must not be an immutable typed array.
  if (tarr->is<ImmutableTypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Resizable typed arrays not yet supported.
  if (tarr->is<ResizableTypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Don't attach if the input type doesn't match the guard added below.
  if (!ValueCanConvertToNumeric(elementType, args_[0])) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `fill` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is an object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);

  // Shape guard to check class.
  writer.guardShapeForClass(objId, tarr->shape());

  // Guard the array buffer is not detached.
  writer.guardHasAttachedArrayBuffer(objId);

  ValOperandId fillValId = loadArgument(calleeId, ArgumentKind::Arg0);
  OperandId fillNumericId = emitNumericGuard(fillValId, args_[0], elementType);

  // Convert |start| to IntPtr.
  IntPtrOperandId intPtrStartId;
  if (args_.length() > 1) {
    ValOperandId startId = loadArgument(calleeId, ArgumentKind::Arg1);
    intPtrStartId =
        guardToIntPtrIndex(args_[1], startId, /* supportOOB = */ false);
  } else {
    // Absent first argument defaults to zero.
    intPtrStartId = writer.loadInt32AsIntPtrConstant(0);
  }

  // Convert |end| to IntPtr.
  IntPtrOperandId intPtrEndId;
  if (args_.length() > 2) {
    ValOperandId endId = loadArgument(calleeId, ArgumentKind::Arg2);
    intPtrEndId = guardToIntPtrIndex(args_[2], endId, /* supportOOB = */ false);
  } else {
    // Absent second argument defaults to the typed array length.
    intPtrEndId = writer.loadArrayBufferViewLength(objId);
  }

  writer.typedArrayFillResult(objId, fillNumericId, intPtrStartId, intPtrEndId,
                              elementType);
  writer.returnFromIC();

  trackAttached("TypedArrayFill");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachTypedArraySet() {
  // Expected arguments: source (typed array), optional offset (int32).
  if (args_.length() < 1 || args_.length() > 2) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a TypedArrayObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Ensure first argument is a TypedArrayObject.
  if (!args_[0].isObject() || !args_[0].toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Ensure optional second argument is a non-negative index.
  uint64_t targetOffset = 0;
  if (args_.length() > 1) {
    int64_t offsetIndex;
    if (!ValueIsInt64Index(args_[1], &offsetIndex) || offsetIndex < 0) {
      return AttachDecision::NoAction;
    }
    targetOffset = uint64_t(offsetIndex);
  }

  auto* tarr = &thisval_.toObject().as<TypedArrayObject>();
  auto* source = &args_[0].toObject().as<TypedArrayObject>();

  // Detached buffers throw.
  if (tarr->hasDetachedBuffer() || source->hasDetachedBuffer()) {
    return AttachDecision::NoAction;
  }

  // Target must not be an immutable typed array.
  if (tarr->is<ImmutableTypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Typed array contents must be compatible.
  if (Scalar::isBigIntType(tarr->type()) !=
      Scalar::isBigIntType(source->type())) {
    return AttachDecision::NoAction;
  }

  // `set()` throws if `sourceLength + targetOffset > targetLength`.
  size_t targetLength = tarr->length().valueOr(0);
  size_t sourceLength = source->length().valueOr(0);
  if (targetOffset > targetLength ||
      sourceLength > targetLength - targetOffset) {
    return AttachDecision::NoAction;
  }

  // Resizable typed arrays not yet supported.
  if (tarr->is<ResizableTypedArrayObject>() ||
      source->is<ResizableTypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Infallible operation if bit-wise copying is possible.
  bool canUseBitwiseCopy = CanUseBitwiseCopy(tarr->type(), source->type());

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `set` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is an object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);

  // Shape guard to check class.
  writer.guardShapeForClass(objId, tarr->shape());

  // Guard the array buffer is not detached.
  writer.guardHasAttachedArrayBuffer(objId);

  // Guard first argument is an object.
  ValOperandId sourceId = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId sourceObjId = writer.guardToObject(sourceId);

  // Shape guard to check class of first argument.
  writer.guardShapeForClass(sourceObjId, source->shape());

  // Guard the source is not detached. (Immutable typed arrays can't get
  // detached.)
  if (!source->is<ImmutableTypedArrayObject>()) {
    writer.guardHasAttachedArrayBuffer(sourceObjId);
  }

  // Convert offset to IntPtr.
  IntPtrOperandId intPtrOffsetId;
  if (args_.length() > 1) {
    ValOperandId offsetId = loadArgument(calleeId, ArgumentKind::Arg1);
    intPtrOffsetId =
        guardToIntPtrIndex(args_[1], offsetId, /* supportOOB = */ false);
    writer.guardIntPtrIsNonNegative(intPtrOffsetId);
  } else {
    // Absent first argument defaults to zero.
    intPtrOffsetId = writer.loadInt32AsIntPtrConstant(0);
  }

  writer.typedArraySetResult(objId, sourceObjId, intPtrOffsetId,
                             canUseBitwiseCopy);
  writer.returnFromIC();

  trackAttached("TypedArraySet");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachTypedArraySubarray() {
  // Only handle argc <= 2.
  if (args_.length() > 2) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  // Ensure |this| is a TypedArrayObject.
  if (!thisval_.isObject() || !thisval_.toObject().is<TypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // Both arguments must be valid indices.
  int64_t unusedIndex;
  if (args_.length() > 0 && !ValueIsInt64Index(args_[0], &unusedIndex)) {
    return AttachDecision::NoAction;
  }
  if (args_.length() > 1 && !ValueIsInt64Index(args_[1], &unusedIndex)) {
    return AttachDecision::NoAction;
  }

  auto* tarr = &thisval_.toObject().as<TypedArrayObject>();

  // Detached buffer throws.
  if (tarr->hasDetachedBuffer()) {
    return AttachDecision::NoAction;
  }

  // Resizable typed arrays not yet supported.
  if (tarr->is<ResizableTypedArrayObject>()) {
    return AttachDecision::NoAction;
  }

  // TypedArray species fuse must still be intact.
  if (!cx_->realm()->realmFuses.optimizeTypedArraySpeciesFuse.intact()) {
    return AttachDecision::NoAction;
  }

  // Ensure |tarr|'s prototype is the actual concrete TypedArray.prototype.
  auto protoKey = StandardProtoKeyOrNull(tarr);
  auto* proto = cx_->global()->maybeGetPrototype(protoKey);
  if (!proto || tarr->staticPrototype() != proto) {
    return AttachDecision::NoAction;
  }

  // Ensure no own "constructor" property.
  if (tarr->containsPure(cx_->names().constructor)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the `subarray` native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard this is an object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId objId = writer.guardToObject(thisValId);

  // Shape guard to check class and prototype, and to ensure no own
  // "constructor" property is present.
  writer.guardShape(objId, tarr->shape());

  // Guard the array buffer is not detached. (Immutable typed arrays can't get
  // detached.)
  if (!tarr->is<ImmutableTypedArrayObject>()) {
    writer.guardHasAttachedArrayBuffer(objId);
  }

  // Guard the fuse is intact.
  writer.guardFuse(RealmFuses::FuseIndex::OptimizeTypedArraySpeciesFuse);

  // Convert |start| to IntPtr.
  IntPtrOperandId intPtrStartId;
  if (args_.length() > 0) {
    ValOperandId startId = loadArgument(calleeId, ArgumentKind::Arg0);
    intPtrStartId =
        guardToIntPtrIndex(args_[0], startId, /* supportOOB = */ false);
  } else {
    // Absent first argument defaults to zero.
    intPtrStartId = writer.loadInt32AsIntPtrConstant(0);
  }

  // Convert |end| to IntPtr.
  IntPtrOperandId intPtrEndId;
  if (args_.length() > 1) {
    ValOperandId endId = loadArgument(calleeId, ArgumentKind::Arg1);
    intPtrEndId = guardToIntPtrIndex(args_[1], endId, /* supportOOB = */ false);
  } else {
    // Absent second argument defaults to the typed array length.
    intPtrEndId = writer.loadArrayBufferViewLength(objId);
  }

  writer.typedArraySubarrayResult(objId, intPtrStartId, intPtrEndId);
  writer.returnFromIC();

  trackAttached("TypedArraySubarray");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsTypedArray(
    bool isPossiblyWrapped) {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.isTypedArrayResult(objArgId, isPossiblyWrapped);
  writer.returnFromIC();

  trackAttached(isPossiblyWrapped ? "IsPossiblyWrappedTypedArray"
                                  : "IsTypedArray");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsTypedArrayConstructor() {
  // Self-hosted code calls this with a single object argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);
  writer.isTypedArrayConstructorResult(objArgId);
  writer.returnFromIC();

  trackAttached("IsTypedArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachTypedArrayLength(
    bool isPossiblyWrapped) {
  // Self-hosted code calls this with a single, possibly wrapped,
  // TypedArrayObject argument.
  MOZ_ASSERT(args_.length() == 1);
  MOZ_ASSERT(args_[0].isObject());

  // Only optimize when the object isn't a wrapper.
  if (isPossiblyWrapped && IsWrapper(&args_[0].toObject())) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(args_[0].toObject().is<TypedArrayObject>());

  auto* tarr = &args_[0].toObject().as<TypedArrayObject>();

  // Don't optimize when a resizable TypedArray is out-of-bounds.
  auto length = tarr->length();
  if (length.isNothing() && !tarr->hasDetachedBuffer()) {
    MOZ_ASSERT(tarr->is<ResizableTypedArrayObject>());
    MOZ_ASSERT(tarr->isOutOfBounds());

    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId argId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objArgId = writer.guardToObject(argId);

  if (isPossiblyWrapped) {
    writer.guardIsNotProxy(objArgId);
  }

  EmitGuardTypedArray(writer, tarr, objArgId);

  if (!tarr->is<ResizableTypedArrayObject>()) {
    if (length.valueOr(0) <= INT32_MAX) {
      writer.loadArrayBufferViewLengthInt32Result(objArgId);
    } else {
      writer.loadArrayBufferViewLengthDoubleResult(objArgId);
    }
  } else {
    writer.guardResizableArrayBufferViewInBoundsOrDetached(objArgId);

    if (length.valueOr(0) <= INT32_MAX) {
      writer.resizableTypedArrayLengthInt32Result(objArgId);
    } else {
      writer.resizableTypedArrayLengthDoubleResult(objArgId);
    }
  }
  writer.returnFromIC();

  trackAttached("IntrinsicTypedArrayLength");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachIsConstructing() {
  // Self-hosted code calls this with no arguments in function scripts.
  MOZ_ASSERT(args_.length() == 0);
  MOZ_ASSERT(script()->isFunction());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.frameIsConstructingResult();
  writer.returnFromIC();

  trackAttached("IsConstructing");
  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachGetNextMapSetEntryForIterator(bool isMap) {
  // Self-hosted code calls this with two objects.
  MOZ_ASSERT(args_.length() == 2);
  if (isMap) {
    MOZ_ASSERT(args_[0].toObject().is<MapIteratorObject>());
  } else {
    MOZ_ASSERT(args_[0].toObject().is<SetIteratorObject>());
  }
  MOZ_ASSERT(args_[1].toObject().is<ArrayObject>());

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  ValOperandId iterId = loadArgumentIntrinsic(ArgumentKind::Arg0);
  ObjOperandId objIterId = writer.guardToObject(iterId);

  ValOperandId resultArrId = loadArgumentIntrinsic(ArgumentKind::Arg1);
  ObjOperandId objResultArrId = writer.guardToObject(resultArrId);

  writer.getNextMapSetEntryForIteratorResult(objIterId, objResultArrId, isMap);
  writer.returnFromIC();

  trackAttached("GetNextMapSetEntryForIterator");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachNewArrayIterator() {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(args_.length() == 0);

  JSObject* templateObj = NewArrayIteratorTemplate(cx_);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.newArrayIteratorResult(templateObj);
  writer.returnFromIC();

  trackAttached("NewArrayIterator");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachNewStringIterator() {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(args_.length() == 0);

  JSObject* templateObj = NewStringIteratorTemplate(cx_);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.newStringIteratorResult(templateObj);
  writer.returnFromIC();

  trackAttached("NewStringIterator");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachNewRegExpStringIterator() {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(args_.length() == 0);

  JSObject* templateObj = NewRegExpStringIteratorTemplate(cx_);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.newRegExpStringIteratorResult(templateObj);
  writer.returnFromIC();

  trackAttached("NewRegExpStringIterator");
  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachArrayIteratorPrototypeOptimizable() {
  // Self-hosted code calls this without any arguments
  MOZ_ASSERT(args_.length() == 0);

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  if (!HasOptimizableArrayIteratorPrototype(cx_)) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  initializeInputOperand();

  // Note: we don't need to call emitNativeCalleeGuard for intrinsics.

  writer.guardFuse(RealmFuses::FuseIndex::OptimizeArrayIteratorPrototypeFuse);
  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("ArrayIteratorPrototypeOptimizable");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachObjectCreate() {
  // Need a single object-or-null argument.
  if (args_.length() != 1 || !args_[0].isObjectOrNull()) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  RootedObject proto(cx_, args_[0].toObjectOrNull());
  JSObject* templateObj = ObjectCreateImpl(cx_, proto, TenuredObject);
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'create' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard on the proto argument.
  ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
  if (proto) {
    ObjOperandId protoId = writer.guardToObject(argId);
    writer.guardSpecificObject(protoId, proto);
  } else {
    writer.guardIsNull(argId);
  }

  writer.objectCreateResult(templateObj);
  writer.returnFromIC();

  trackAttached("ObjectCreate");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachObjectConstructor() {
  // Expecting no arguments or a single object argument.
  // TODO(Warp): Support all or more conversions to object.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }
  if (args_.length() == 1 && !args_[0].isObject()) {
    return AttachDecision::NoAction;
  }

  gc::AllocSite* site = nullptr;
  PlainObject* templateObj = nullptr;
  if (args_.length() == 0) {
    // Stub doesn't support metadata builder
    if (cx_->realm()->hasAllocationMetadataBuilder()) {
      return AttachDecision::NoAction;
    }

    site = generator_.maybeCreateAllocSite();
    if (!site) {
      return AttachDecision::NoAction;
    }

    // Create a temporary object to act as the template object.
    templateObj = NewPlainObjectWithAllocKind(cx_, NewObjectGCKind());
    if (!templateObj) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee and newTarget (if constructing) are this Object constructor
  // function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  if (args_.length() == 0) {
    uint32_t numFixedSlots = templateObj->numUsedFixedSlots();
    uint32_t numDynamicSlots = templateObj->numDynamicSlots();
    gc::AllocKind allocKind = templateObj->allocKindForTenure();
    Shape* shape = templateObj->shape();

    writer.guardNoAllocationMetadataBuilder(
        cx_->realm()->addressOfMetadataBuilder());
    writer.newPlainObjectResult(numFixedSlots, numDynamicSlots, allocKind,
                                shape, site);
  } else {
    // Guard that the argument is an object.
    ValOperandId argId = loadArgument(calleeId, ArgumentKind::Arg0);
    ObjOperandId objId = writer.guardToObject(argId);

    // Return the object.
    writer.loadObjectResult(objId);
  }

  writer.returnFromIC();

  trackAttached("ObjectConstructor");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachArrayConstructor() {
  // Only optimize the |Array()| and |Array(n)| cases (with or without |new|)
  // for now. Note that self-hosted code calls this without |new| via std_Array.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }
  if (args_.length() == 1 && !args_[0].isInt32()) {
    return AttachDecision::NoAction;
  }

  int32_t length = (args_.length() == 1) ? args_[0].toInt32() : 0;
  if (length < 0 || uint32_t(length) > ArrayObject::EagerAllocationMaxLength) {
    return AttachDecision::NoAction;
  }

  // We allow inlining this function across realms so make sure the template
  // object is allocated in that realm. See CanInlineNativeCrossRealm.
  JSObject* templateObj;
  {
    AutoRealm ar(cx_, target_);
    templateObj = NewDenseFullyAllocatedArray(cx_, length, TenuredObject);
    if (!templateObj) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }
  }

  gc::AllocSite* site = generator_.maybeCreateAllocSite();
  if (!site) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee and newTarget (if constructing) are this Array constructor
  // function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  Int32OperandId lengthId;
  if (args_.length() == 1) {
    ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
    lengthId = writer.guardToInt32(arg0Id);
  } else {
    MOZ_ASSERT(args_.length() == 0);
    lengthId = writer.loadInt32Constant(0);
  }

  writer.newArrayFromLengthResult(templateObj, lengthId, site);
  writer.returnFromIC();

  trackAttached("ArrayConstructor");
  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachTypedArrayConstructorFromLength() {
  MOZ_ASSERT(flags_.isConstructing());
  MOZ_ASSERT(args_.length() == 0 || args_[0].isInt32());

  // Expected arguments: Optional length (int32)
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }

  int32_t length = args_.length() > 0 ? args_[0].toInt32() : 0;

  Scalar::Type type = TypedArrayConstructorType(target_);
  Rooted<TypedArrayObject*> templateObj(cx_);
  if (!TypedArrayObject::GetTemplateObjectForLength(cx_, type, length,
                                                    &templateObj)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // This can happen for large length values.
  if (!templateObj) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee and newTarget are this TypedArray constructor function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  Int32OperandId lengthId;
  if (args_.length() > 0) {
    ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
    lengthId = writer.guardToInt32(arg0Id);
  } else {
    lengthId = writer.loadInt32Constant(0);
  }
  writer.newTypedArrayFromLengthResult(templateObj, lengthId);
  writer.returnFromIC();

  trackAttached("TypedArrayConstructorFromLength");
  return AttachDecision::Attach;
}

AttachDecision
InlinableNativeIRGenerator::tryAttachTypedArrayConstructorFromArrayBuffer() {
  MOZ_ASSERT(flags_.isConstructing());
  MOZ_ASSERT(args_.length() > 0);
  MOZ_ASSERT(args_[0].isObject());

  // Expected arguments: array buffer, optional byteOffset, optional length
  if (args_.length() > 3) {
    return AttachDecision::NoAction;
  }

#ifdef JS_CODEGEN_X86
  // Unfortunately NewTypedArrayFromArrayBufferResult needs more registers than
  // we can easily support on 32-bit x86 for now.
  return AttachDecision::NoAction;
#else
  Scalar::Type type = TypedArrayConstructorType(target_);

  Rooted<ArrayBufferObjectMaybeShared*> obj(
      cx_, &args_[0].toObject().as<ArrayBufferObjectMaybeShared>());

  Rooted<TypedArrayObject*> templateObj(
      cx_, TypedArrayObject::GetTemplateObjectForBuffer(cx_, type, obj));
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee and newTarget are this TypedArray constructor function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  if (obj->is<FixedLengthArrayBufferObject>()) {
    writer.guardClass(objId, GuardClassKind::FixedLengthArrayBuffer);
  } else if (obj->is<FixedLengthSharedArrayBufferObject>()) {
    writer.guardClass(objId, GuardClassKind::FixedLengthSharedArrayBuffer);
  } else if (obj->is<ResizableArrayBufferObject>()) {
    writer.guardClass(objId, GuardClassKind::ResizableArrayBuffer);
  } else if (obj->is<GrowableSharedArrayBufferObject>()) {
    writer.guardClass(objId, GuardClassKind::GrowableSharedArrayBuffer);
  } else {
    MOZ_ASSERT(obj->is<ImmutableArrayBufferObject>());
    writer.guardClass(objId, GuardClassKind::ImmutableArrayBuffer);
  }

  ValOperandId byteOffsetId;
  if (args_.length() > 1) {
    byteOffsetId = loadArgument(calleeId, ArgumentKind::Arg1);
  } else {
    byteOffsetId = writer.loadUndefined();
  }

  ValOperandId lengthId;
  if (args_.length() > 2) {
    lengthId = loadArgument(calleeId, ArgumentKind::Arg2);
  } else {
    lengthId = writer.loadUndefined();
  }

  writer.newTypedArrayFromArrayBufferResult(templateObj, objId, byteOffsetId,
                                            lengthId);
  writer.returnFromIC();

  trackAttached("TypedArrayConstructorFromArrayBuffer");
  return AttachDecision::Attach;
#endif
}

AttachDecision
InlinableNativeIRGenerator::tryAttachTypedArrayConstructorFromArray() {
  MOZ_ASSERT(flags_.isConstructing());
  MOZ_ASSERT(args_.length() > 0);
  MOZ_ASSERT(args_[0].isObject());

  // Expected arguments: Array-like object.
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  Rooted<JSObject*> obj(cx_, &args_[0].toObject());
  MOZ_ASSERT(!obj->is<ProxyObject>());
  MOZ_ASSERT(!obj->is<ArrayBufferObjectMaybeShared>());

  Scalar::Type type = TypedArrayConstructorType(target_);

  Rooted<TypedArrayObject*> templateObj(
      cx_, TypedArrayObject::GetTemplateObjectForArrayLike(cx_, type, obj));
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee and newTarget are this TypedArray constructor function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId arg0Id = loadArgument(calleeId, ArgumentKind::Arg0);
  ObjOperandId objId = writer.guardToObject(arg0Id);

  writer.guardIsNotArrayBufferMaybeShared(objId);
  writer.guardIsNotProxy(objId);
  writer.newTypedArrayFromArrayResult(templateObj, objId);
  writer.returnFromIC();

  trackAttached("TypedArrayConstructorFromArray");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachTypedArrayConstructor() {
  MOZ_ASSERT(flags_.isConstructing());

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  // The first argument, if present, must be int32 or a non-proxy object.

  if (args_.length() == 0 || args_[0].isInt32()) {
    return tryAttachTypedArrayConstructorFromLength();
  }

  if (args_[0].isObject()) {
    auto* obj = &args_[0].toObject();

    // Proxy objects not allowed, because handling Wrappers is complicated.
    if (obj->is<ProxyObject>()) {
      return AttachDecision::NoAction;
    }

    if (obj->is<ArrayBufferObjectMaybeShared>()) {
      return tryAttachTypedArrayConstructorFromArrayBuffer();
    }
    return tryAttachTypedArrayConstructorFromArray();
  }

  // Other argument types are not supported.
  return AttachDecision::NoAction;
}

AttachDecision InlinableNativeIRGenerator::tryAttachMapSetConstructor(
    InlinableNative native) {
  MOZ_ASSERT(native == InlinableNative::MapConstructor ||
             native == InlinableNative::SetConstructor);
  MOZ_ASSERT(flags_.isConstructing());

  // Must have either no arguments or a single (iterable) argument.
  if (args_.length() > 1) {
    return AttachDecision::NoAction;
  }

  if (!isFirstStub()) {
    // Attach only once to prevent slowdowns for polymorphic calls.
    return AttachDecision::NoAction;
  }

  JSObject* templateObj;
  if (native == InlinableNative::MapConstructor) {
    templateObj = GlobalObject::getOrCreateMapTemplateObject(cx_);
  } else {
    templateObj = GlobalObject::getOrCreateSetTemplateObject(cx_);
  }
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee and newTarget are this Map/Set constructor function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  if (args_.length() == 1) {
    ValOperandId iterableId = loadArgument(calleeId, ArgumentKind::Arg0);
    if (native == InlinableNative::MapConstructor) {
      writer.newMapObjectFromIterableResult(templateObj, iterableId);
    } else {
      writer.newSetObjectFromIterableResult(templateObj, iterableId);
    }
  } else {
    if (native == InlinableNative::MapConstructor) {
      writer.newMapObjectResult(templateObj);
    } else {
      writer.newSetObjectResult(templateObj);
    }
  }
  writer.returnFromIC();

  if (native == InlinableNative::MapConstructor) {
    trackAttached("MapConstructor");
  } else {
    trackAttached("SetConstructor");
  }
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachSpecializedFunctionBind(
    Handle<JSObject*> target, Handle<BoundFunctionObject*> templateObj) {
  // Try to attach a faster stub that's more specialized than what we emit in
  // tryAttachFunctionBind. This lets us allocate and initialize a bound
  // function object in Ion without calling into C++.
  //
  // We can do this if:
  //
  // * The target's prototype is Function.prototype, because that's the proto we
  //   use for the template object.
  // * All bound arguments can be stored inline.
  // * The `.name`, `.length`, and `IsConstructor` values match `target`.
  //
  // We initialize the template object with the bound function's name, length,
  // and flags. At runtime we then only have to clone the template object and
  // initialize the slots for the target, the bound `this` and the bound
  // arguments.

  if (!isFirstStub()) {
    return AttachDecision::NoAction;
  }
  if (!target->is<JSFunction>() && !target->is<BoundFunctionObject>()) {
    return AttachDecision::NoAction;
  }
  if (target->staticPrototype() != &cx_->global()->getFunctionPrototype()) {
    return AttachDecision::NoAction;
  }
  size_t numBoundArgs = args_.length() > 0 ? args_.length() - 1 : 0;
  if (numBoundArgs > BoundFunctionObject::MaxInlineBoundArgs) {
    return AttachDecision::NoAction;
  }

  const bool targetIsConstructor = target->isConstructor();
  Rooted<JSAtom*> targetName(cx_);
  uint32_t targetLength = 0;

  if (target->is<JSFunction>()) {
    Rooted<JSFunction*> fun(cx_, &target->as<JSFunction>());
    if (fun->isNativeFun()) {
      return AttachDecision::NoAction;
    }
    if (fun->hasResolvedLength() || fun->hasResolvedName()) {
      return AttachDecision::NoAction;
    }
    uint16_t len;
    if (!JSFunction::getUnresolvedLength(cx_, fun, &len)) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }
    targetName = fun->getUnresolvedName(cx_);
    if (!targetName) {
      cx_->clearPendingException();
      return AttachDecision::NoAction;
    }

    targetLength = len;
  } else {
    BoundFunctionObject* bound = &target->as<BoundFunctionObject>();
    if (!targetIsConstructor) {
      // Only support constructors for now. This lets us use
      // GuardBoundFunctionIsConstructor.
      return AttachDecision::NoAction;
    }
    Shape* initialShape =
        cx_->global()->maybeBoundFunctionShapeWithDefaultProto();
    if (bound->shape() != initialShape) {
      return AttachDecision::NoAction;
    }
    Value lenVal = bound->getLengthForInitialShape();
    Value nameVal = bound->getNameForInitialShape();
    if (!lenVal.isInt32() || lenVal.toInt32() < 0 || !nameVal.isString() ||
        !nameVal.toString()->isAtom()) {
      return AttachDecision::NoAction;
    }
    targetName = &nameVal.toString()->asAtom();
    targetLength = uint32_t(lenVal.toInt32());
  }

  if (!templateObj->initTemplateSlotsForSpecializedBind(
          cx_, numBoundArgs, targetIsConstructor, targetLength, targetName)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  Int32OperandId argcId = initializeInputOperand();
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId targetId = writer.guardToObject(thisValId);

  // Ensure the JSClass and proto match, and that the `length` and `name`
  // properties haven't been redefined.
  writer.guardShape(targetId, target->shape());

  // Emit guards for the `IsConstructor`, `.length`, and `.name` values.
  if (target->is<JSFunction>()) {
    // Guard on:
    // * The BaseScript (because that's what JSFunction uses for the `length`).
    //   Because MGuardFunctionScript doesn't support self-hosted functions yet,
    //   we use GuardSpecificFunction instead in this case.
    //   See assertion in MGuardFunctionScript::getAliasSet.
    // * The flags slot (for the CONSTRUCTOR, RESOLVED_NAME, RESOLVED_LENGTH,
    //   HAS_INFERRED_NAME, and HAS_GUESSED_ATOM flags).
    // * The atom slot.
    JSFunction* fun = &target->as<JSFunction>();
    if (fun->isSelfHostedBuiltin()) {
      writer.guardSpecificFunction(targetId, fun);
    } else {
      writer.guardFunctionScript(targetId, fun->baseScript());
    }
    writer.guardFixedSlotValue(
        targetId, JSFunction::offsetOfFlagsAndArgCount(),
        fun->getReservedSlot(JSFunction::FlagsAndArgCountSlot));
    writer.guardFixedSlotValue(targetId, JSFunction::offsetOfAtom(),
                               fun->getReservedSlot(JSFunction::AtomSlot));
  } else {
    BoundFunctionObject* bound = &target->as<BoundFunctionObject>();
    writer.guardBoundFunctionIsConstructor(targetId);
    writer.guardFixedSlotValue(targetId,
                               BoundFunctionObject::offsetOfLengthSlot(),
                               bound->getLengthForInitialShape());
    writer.guardFixedSlotValue(targetId,
                               BoundFunctionObject::offsetOfNameSlot(),
                               bound->getNameForInitialShape());
  }

  writer.specializedBindFunctionResult(targetId, args_.length(), templateObj);
  writer.returnFromIC();

  trackAttached("SpecializedFunctionBind");
  return AttachDecision::Attach;
}

AttachDecision InlinableNativeIRGenerator::tryAttachFunctionBind() {
  // Ensure |this| (the target) is a function object or a bound function object.
  // We could support other callables too, but note that we rely on the target
  // having a static prototype in BoundFunctionObject::functionBindImpl.
  if (!thisval_.isObject()) {
    return AttachDecision::NoAction;
  }
  Rooted<JSObject*> target(cx_, &thisval_.toObject());
  if (!target->is<JSFunction>() && !target->is<BoundFunctionObject>()) {
    return AttachDecision::NoAction;
  }

  // Only support standard, non-spread calls.
  if (flags_.getArgFormat() != CallFlags::Standard) {
    return AttachDecision::NoAction;
  }

  // Only support when no additional bound arguments are present.
  if (hasBoundArguments()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(stackArgc() == args_.length(), "argc matches number of arguments");

  // Only optimize if the number of arguments is small. This ensures we don't
  // compile a lot of different stubs (because we bake in argc) and that we
  // don't get anywhere near ARGS_LENGTH_MAX.
  static constexpr size_t MaxArguments = 6;
  if (args_.length() > MaxArguments) {
    return AttachDecision::NoAction;
  }

  Rooted<BoundFunctionObject*> templateObj(
      cx_, BoundFunctionObject::createTemplateObject(cx_));
  if (!templateObj) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  TRY_ATTACH(tryAttachSpecializedFunctionBind(target, templateObj));

  Int32OperandId argcId = initializeInputOperand();

  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  // Guard |this| is a function object or a bound function object.
  ValOperandId thisValId = loadThis(calleeId);
  ObjOperandId targetId = writer.guardToObject(thisValId);
  if (target->is<JSFunction>()) {
    writer.guardClass(targetId, GuardClassKind::JSFunction);
  } else {
    MOZ_ASSERT(target->is<BoundFunctionObject>());
    writer.guardClass(targetId, GuardClassKind::BoundFunction);
  }

  writer.bindFunctionResult(targetId, args_.length(), templateObj);
  writer.returnFromIC();

  trackAttached("FunctionBind");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachFunApply(HandleFunction calleeFunc) {
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  if (calleeFunc->native() != fun_apply) {
    return AttachDecision::NoAction;
  }

  if (argc_ > 2) {
    return AttachDecision::NoAction;
  }

  if (!thisval_.isObject() || !thisval_.toObject().is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  Rooted<JSFunction*> target(cx_, &thisval_.toObject().as<JSFunction>());

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  if (target->isClassConstructor()) {
    return AttachDecision::NoAction;
  }

  CallFlags::ArgFormat format = CallFlags::Standard;
  if (argc_ < 2) {
    // |fun.apply()| and |fun.apply(thisValue)| are equivalent to |fun.call()|
    // resp. |fun.call(thisValue)|.
    format = CallFlags::FunCall;
  } else if (args_[1].isNullOrUndefined()) {
    // |fun.apply(thisValue, null)| and |fun.apply(thisValue, undefined)| are
    // also equivalent to |fun.call(thisValue)|, but we can't use FunCall
    // because we have to discard the second argument.
    format = CallFlags::FunApplyNullUndefined;
  } else if (args_[1].isObject() && args_[1].toObject().is<ArgumentsObject>()) {
    auto* argsObj = &args_[1].toObject().as<ArgumentsObject>();
    if (argsObj->hasOverriddenElement() || argsObj->anyArgIsForwarded() ||
        argsObj->hasOverriddenLength() ||
        argsObj->initialLength() > JIT_ARGS_LENGTH_MAX) {
      return AttachDecision::NoAction;
    }
    format = CallFlags::FunApplyArgsObj;
  } else if (args_[1].isObject() && args_[1].toObject().is<ArrayObject>() &&
             args_[1].toObject().as<ArrayObject>().length() <=
                 JIT_ARGS_LENGTH_MAX &&
             IsPackedArray(&args_[1].toObject())) {
    format = CallFlags::FunApplyArray;
  } else {
    return AttachDecision::NoAction;
  }

  CallFlags targetFlags(format);
  if (mode_ == ICState::Mode::Specialized) {
    if (cx_->realm() == target->realm()) {
      targetFlags.setIsSameRealm();
    }
  }

  if (mode_ == ICState::Mode::Specialized && !isScripted &&
      format == CallFlags::FunApplyArray) {
    HandleValue newTarget = NullHandleValue;
    HandleValue thisValue = args_[0];
    Rooted<ArrayObject*> aobj(cx_, &args_[1].toObject().as<ArrayObject>());
    HandleValueArray args = HandleValueArray::fromMarkedLocation(
        aobj->length(), aobj->getDenseElements());

    // Check for specific native-function optimizations.
    InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                         args, targetFlags);
    TRY_ATTACH(nativeGen.tryAttachStub());
  }

  if (mode_ == ICState::Mode::Specialized && !isScripted &&
      (format == CallFlags::FunCall ||
       format == CallFlags::FunApplyNullUndefined)) {
    HandleValue newTarget = NullHandleValue;
    HandleValue thisValue = argc_ > 0 ? args_[0] : UndefinedHandleValue;
    HandleValueArray args = HandleValueArray::empty();

    // Check for specific native-function optimizations.
    InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                         args, targetFlags);
    TRY_ATTACH(nativeGen.tryAttachStub());
  }

  Int32OperandId argcId(writer.setInputOperandId(0));
  ObjOperandId thisObjId = emitFunApplyGuard(argcId);

  uint32_t fixedArgc;
  if (format == CallFlags::FunApplyArray ||
      format == CallFlags::FunApplyArgsObj ||
      format == CallFlags::FunApplyNullUndefined) {
    emitFunApplyArgsGuard(format);

    // We always use MaxUnrolledArgCopy here because the fixed argc is
    // meaningless in a FunApply case.
    fixedArgc = MaxUnrolledArgCopy;
  } else {
    MOZ_ASSERT(format == CallFlags::FunCall);

    // Whereas for the FunCall case we need to use the actual fixed argc value.
    fixedArgc = ClampFixedArgc(argc_);
  }

  if (mode_ == ICState::Mode::Specialized) {
    // Ensure that |this| is the expected target function.
    emitCalleeGuard(thisObjId, target);

    if (isScripted) {
      writer.callScriptedFunction(thisObjId, argcId, targetFlags, fixedArgc);
    } else {
      writer.callNativeFunction(thisObjId, argcId, op_, target, targetFlags,
                                fixedArgc);
    }
  } else {
    // Guard that |this| is a function.
    writer.guardClass(thisObjId, GuardClassKind::JSFunction);

    // Guard that function is not a class constructor.
    writer.guardNotClassConstructor(thisObjId);

    if (isScripted) {
      // Guard that function is scripted.
      writer.guardFunctionHasJitEntry(thisObjId);
      writer.callScriptedFunction(thisObjId, argcId, targetFlags, fixedArgc);
    } else {
      // Guard that function is native.
      writer.guardFunctionHasNoJitEntry(thisObjId);
      writer.callAnyNativeFunction(thisObjId, argcId, targetFlags, fixedArgc);
    }
  }

  writer.returnFromIC();

  if (isScripted) {
    trackAttached("Call.ScriptedFunApply");
  } else {
    trackAttached("Call.NativeFunApply");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachWasmCall(HandleFunction calleeFunc) {
  // Try to optimize calls into Wasm code by emitting the CallWasmFunction
  // CacheIR op. Baseline ICs currently treat this as a CallScriptedFunction op
  // (calling Wasm's JitEntry stub) but Warp transpiles it to a more direct call
  // into Wasm code.
  //
  // Note: some code refers to these optimized Wasm calls as "inlined" calls.

  MOZ_ASSERT(calleeFunc->isWasmWithJitEntry());

  if (!JitOptions.enableWasmIonFastCalls) {
    return AttachDecision::NoAction;
  }
  if (!isFirstStub_) {
    return AttachDecision::NoAction;
  }
  JSOp op = JSOp(*pc_);
  if (op != JSOp::Call && op != JSOp::CallContent &&
      op != JSOp::CallIgnoresRv) {
    return AttachDecision::NoAction;
  }
  if (cx_->realm() != calleeFunc->realm()) {
    return AttachDecision::NoAction;
  }

  wasm::Instance& inst = calleeFunc->wasmInstance();
  uint32_t funcIndex = calleeFunc->wasmFuncIndex();
  const wasm::CodeBlock& codeBlock = inst.code().funcCodeBlock(funcIndex);
  const wasm::FuncExport& funcExport = codeBlock.lookupFuncExport(funcIndex);
  const wasm::FuncType& sig = calleeFunc->wasmTypeDef()->funcType();

  MOZ_ASSERT(!IsInsideNursery(inst.object()));
  MOZ_ASSERT(sig.canHaveJitEntry(), "Function should allow a Wasm JitEntry");

  // If there are too many arguments, don't optimize (we won't be able to store
  // the arguments in the LIR node).
  static_assert(wasm::MaxArgsForJitInlineCall <= ArgumentKindArgIndexLimit);
  if (sig.args().length() > wasm::MaxArgsForJitInlineCall ||
      argc_ > ArgumentKindArgIndexLimit) {
    return AttachDecision::NoAction;
  }

  // If there are too many results, don't optimize as Warp currently doesn't
  // have code to handle this.
  if (sig.results().length() > wasm::MaxResultsForJitInlineCall) {
    return AttachDecision::NoAction;
  }

  // Bug 1631656 - Don't try to optimize with I64 args on 32-bit platforms
  // because it is more difficult (because it requires multiple LIR arguments
  // per I64).
  //
  // Bug 1631650 - On 64-bit platforms, we also give up optimizing for I64 args
  // spilled to the stack because it causes problems with register allocation.
#ifdef JS_64BIT
  constexpr bool optimizeWithI64 = true;
#else
  constexpr bool optimizeWithI64 = false;
#endif
  ABIArgGenerator abi(ABIKind::Wasm);
  for (const auto& valType : sig.args()) {
    MIRType mirType = valType.toMIRType();
    ABIArg abiArg = abi.next(mirType);
    if (mirType != MIRType::Int64) {
      continue;
    }
    if (!optimizeWithI64 || abiArg.kind() == ABIArg::Stack) {
      return AttachDecision::NoAction;
    }
  }

  // Check that all arguments can be converted to the Wasm type in Warp code
  // without bailing out.
  for (size_t i = 0; i < sig.args().length(); i++) {
    Value argVal = i < argc_ ? args_[i] : UndefinedValue();
    switch (sig.args()[i].kind()) {
      case wasm::ValType::I32:
      case wasm::ValType::F32:
      case wasm::ValType::F64:
        if (!argVal.isNumber() && !argVal.isBoolean() &&
            !argVal.isUndefined()) {
          return AttachDecision::NoAction;
        }
        break;
      case wasm::ValType::I64:
        if (!argVal.isBigInt() && !argVal.isBoolean() && !argVal.isString()) {
          return AttachDecision::NoAction;
        }
        break;
      case wasm::ValType::V128:
        MOZ_CRASH("Function should not have a Wasm JitEntry");
      case wasm::ValType::Ref:
        // canHaveJitEntry restricts args to externref, where all JS values are
        // valid and can be boxed.
        MOZ_ASSERT(sig.args()[i].refType().isExtern(),
                   "Unexpected type for Wasm JitEntry");
        break;
    }
  }

  CallFlags flags(/* isConstructing = */ false, /* isSpread = */ false,
                  /* isSameRealm = */ true);

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentFixedSlot(ArgumentKind::Callee, argc_, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  // Ensure the callee is this Wasm function.
  emitCalleeGuard(calleeObjId, calleeFunc);

  // Guard the argument types.
  uint32_t guardedArgs = std::min<uint32_t>(sig.args().length(), argc_);
  for (uint32_t i = 0; i < guardedArgs; i++) {
    ArgumentKind argKind = ArgumentKindForArgIndex(i);
    ValOperandId argId = writer.loadArgumentFixedSlot(argKind, argc_, flags);
    writer.guardWasmArg(argId, sig.args()[i].kind());
  }

  writer.callWasmFunction(calleeObjId, argcId, flags, ClampFixedArgc(argc_),
                          &funcExport, inst.object());
  writer.returnFromIC();

  trackAttached("Call.WasmCall");

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachInlinableNative(HandleFunction callee,
                                                         CallFlags flags) {
  MOZ_ASSERT(mode_ == ICState::Mode::Specialized);
  MOZ_ASSERT(callee->isNativeWithoutJitEntry());
  MOZ_ASSERT(flags.getArgFormat() == CallFlags::Standard ||
             flags.getArgFormat() == CallFlags::Spread);

  InlinableNativeIRGenerator nativeGen(*this, callee, newTarget_, thisval_,
                                       args_, flags);
  return nativeGen.tryAttachStub();
}

#ifdef FUZZING_JS_FUZZILLI
AttachDecision InlinableNativeIRGenerator::tryAttachFuzzilliHash() {
  if (args_.length() != 1) {
    return AttachDecision::NoAction;
  }

  // Initialize the input operand.
  Int32OperandId argcId = initializeInputOperand();

  // Guard callee is the 'fuzzilli_hash' native function.
  ObjOperandId calleeId = emitNativeCalleeGuard(argcId);

  ValOperandId argValId = loadArgument(calleeId, ArgumentKind::Arg0);

  writer.fuzzilliHashResult(argValId);
  writer.returnFromIC();

  trackAttached("FuzzilliHash");
  return AttachDecision::Attach;
}
#endif

AttachDecision InlinableNativeIRGenerator::tryAttachStub() {
  MOZ_ASSERT(generator_.mode_ == ICState::Mode::Specialized);
  MOZ_ASSERT(target_->isNativeWithoutJitEntry());

  // Special case functions are only optimized for normal calls.
  if (!BytecodeCallOpCanHaveInlinableNative(op())) {
    return AttachDecision::NoAction;
  }

  if (!target_->hasJitInfo() ||
      target_->jitInfo()->type() != JSJitInfo::InlinableNative) {
    return AttachDecision::NoAction;
  }

  InlinableNative native = target_->jitInfo()->inlinableNative;

  // Not all natives can be inlined cross-realm.
  if (cx_->realm() != target_->realm() && !CanInlineNativeCrossRealm(native)) {
    return AttachDecision::NoAction;
  }

  // Check for special-cased native constructors.
  if (flags_.isConstructing()) {
    MOZ_ASSERT(flags_.getArgFormat() == CallFlags::Standard);

    // newTarget must match the callee. CacheIR for this is emitted in
    // emitNativeCalleeGuard.
    if (ObjectValue(*callee()) != newTarget_) {
      return AttachDecision::NoAction;
    }
    switch (native) {
      case InlinableNative::Array:
        return tryAttachArrayConstructor();
      case InlinableNative::TypedArrayConstructor:
        return tryAttachTypedArrayConstructor();
      case InlinableNative::MapConstructor:
      case InlinableNative::SetConstructor:
        return tryAttachMapSetConstructor(native);
      case InlinableNative::String:
        return tryAttachStringConstructor();
      case InlinableNative::Object:
        return tryAttachObjectConstructor();
      default:
        break;
    }
    return AttachDecision::NoAction;
  }

  // Check for special-cased native spread calls.
  if (flags_.getArgFormat() == CallFlags::Spread ||
      flags_.getArgFormat() == CallFlags::FunApplyArray) {
    // Can't inline spread calls when bound arguments are present.
    MOZ_ASSERT(!hasBoundArguments());

    switch (native) {
      case InlinableNative::MathMin:
        return tryAttachSpreadMathMinMax(/*isMax = */ false);
      case InlinableNative::MathMax:
        return tryAttachSpreadMathMinMax(/*isMax = */ true);
      default:
        break;
    }
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(flags_.getArgFormat() == CallFlags::Standard ||
             flags_.getArgFormat() == CallFlags::FunCall ||
             flags_.getArgFormat() == CallFlags::FunApplyNullUndefined);

  // Check for special-cased native functions.
  switch (native) {
    // Array natives.
    case InlinableNative::Array:
      return tryAttachArrayConstructor();
    case InlinableNative::ArrayPush:
      return tryAttachArrayPush();
    case InlinableNative::ArrayPop:
    case InlinableNative::ArrayShift:
      return tryAttachArrayPopShift(native);
    case InlinableNative::ArrayJoin:
      return tryAttachArrayJoin();
    case InlinableNative::ArraySlice:
      return tryAttachArraySlice();
    case InlinableNative::ArrayIsArray:
      return tryAttachArrayIsArray();

    // DataView natives.
    case InlinableNative::DataViewGetInt8:
      return tryAttachDataViewGet(Scalar::Int8);
    case InlinableNative::DataViewGetUint8:
      return tryAttachDataViewGet(Scalar::Uint8);
    case InlinableNative::DataViewGetInt16:
      return tryAttachDataViewGet(Scalar::Int16);
    case InlinableNative::DataViewGetUint16:
      return tryAttachDataViewGet(Scalar::Uint16);
    case InlinableNative::DataViewGetInt32:
      return tryAttachDataViewGet(Scalar::Int32);
    case InlinableNative::DataViewGetUint32:
      return tryAttachDataViewGet(Scalar::Uint32);
    case InlinableNative::DataViewGetFloat16:
      return tryAttachDataViewGet(Scalar::Float16);
    case InlinableNative::DataViewGetFloat32:
      return tryAttachDataViewGet(Scalar::Float32);
    case InlinableNative::DataViewGetFloat64:
      return tryAttachDataViewGet(Scalar::Float64);
    case InlinableNative::DataViewGetBigInt64:
      return tryAttachDataViewGet(Scalar::BigInt64);
    case InlinableNative::DataViewGetBigUint64:
      return tryAttachDataViewGet(Scalar::BigUint64);
    case InlinableNative::DataViewSetInt8:
      return tryAttachDataViewSet(Scalar::Int8);
    case InlinableNative::DataViewSetUint8:
      return tryAttachDataViewSet(Scalar::Uint8);
    case InlinableNative::DataViewSetInt16:
      return tryAttachDataViewSet(Scalar::Int16);
    case InlinableNative::DataViewSetUint16:
      return tryAttachDataViewSet(Scalar::Uint16);
    case InlinableNative::DataViewSetInt32:
      return tryAttachDataViewSet(Scalar::Int32);
    case InlinableNative::DataViewSetUint32:
      return tryAttachDataViewSet(Scalar::Uint32);
    case InlinableNative::DataViewSetFloat16:
      return tryAttachDataViewSet(Scalar::Float16);
    case InlinableNative::DataViewSetFloat32:
      return tryAttachDataViewSet(Scalar::Float32);
    case InlinableNative::DataViewSetFloat64:
      return tryAttachDataViewSet(Scalar::Float64);
    case InlinableNative::DataViewSetBigInt64:
      return tryAttachDataViewSet(Scalar::BigInt64);
    case InlinableNative::DataViewSetBigUint64:
      return tryAttachDataViewSet(Scalar::BigUint64);

    // Function natives.
    case InlinableNative::FunctionBind:
      return tryAttachFunctionBind();

    // Intl natives.
    case InlinableNative::IntlGuardToCollator:
    case InlinableNative::IntlGuardToDateTimeFormat:
    case InlinableNative::IntlGuardToDisplayNames:
    case InlinableNative::IntlGuardToDurationFormat:
    case InlinableNative::IntlGuardToListFormat:
    case InlinableNative::IntlGuardToNumberFormat:
    case InlinableNative::IntlGuardToPluralRules:
    case InlinableNative::IntlGuardToRelativeTimeFormat:
    case InlinableNative::IntlGuardToSegmenter:
    case InlinableNative::IntlGuardToSegments:
    case InlinableNative::IntlGuardToSegmentIterator:
      return tryAttachGuardToClass(native);

    // Slot intrinsics.
    case InlinableNative::IntrinsicUnsafeGetReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetObjectFromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetInt32FromReservedSlot:
    case InlinableNative::IntrinsicUnsafeGetStringFromReservedSlot:
      return tryAttachUnsafeGetReservedSlot(native);
    case InlinableNative::IntrinsicUnsafeSetReservedSlot:
      return tryAttachUnsafeSetReservedSlot();

    // Intrinsics.
    case InlinableNative::IntrinsicIsSuspendedGenerator:
      return tryAttachIsSuspendedGenerator();
    case InlinableNative::IntrinsicToObject:
      return tryAttachToObject();
    case InlinableNative::IntrinsicToInteger:
      return tryAttachToInteger();
    case InlinableNative::IntrinsicToLength:
      return tryAttachToLength();
    case InlinableNative::IntrinsicIsObject:
      return tryAttachIsObject();
    case InlinableNative::IntrinsicIsPackedArray:
      return tryAttachIsPackedArray();
    case InlinableNative::IntrinsicIsCallable:
      return tryAttachIsCallable();
    case InlinableNative::IntrinsicIsConstructor:
      return tryAttachIsConstructor();
    case InlinableNative::IntrinsicIsCrossRealmArrayConstructor:
      return tryAttachIsCrossRealmArrayConstructor();
    case InlinableNative::IntrinsicCanOptimizeArraySpecies:
      return tryAttachCanOptimizeArraySpecies();
    case InlinableNative::IntrinsicCanOptimizeStringProtoSymbolLookup:
      return tryAttachCanOptimizeStringProtoSymbolLookup();
    case InlinableNative::IntrinsicGuardToArrayIterator:
    case InlinableNative::IntrinsicGuardToMapIterator:
    case InlinableNative::IntrinsicGuardToSetIterator:
    case InlinableNative::IntrinsicGuardToStringIterator:
    case InlinableNative::IntrinsicGuardToRegExpStringIterator:
    case InlinableNative::IntrinsicGuardToWrapForValidIterator:
    case InlinableNative::IntrinsicGuardToIteratorHelper:
#ifdef NIGHTLY_BUILD
    case InlinableNative::IntrinsicGuardToIteratorRange:
#endif
    case InlinableNative::IntrinsicGuardToAsyncIteratorHelper:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case InlinableNative::IntrinsicGuardToAsyncDisposableStack:
    case InlinableNative::IntrinsicGuardToDisposableStack:
#endif
      return tryAttachGuardToClass(native);
    case InlinableNative::IntrinsicSubstringKernel:
      return tryAttachSubstringKernel();
    case InlinableNative::IntrinsicIsConstructing:
      return tryAttachIsConstructing();
    case InlinableNative::IntrinsicNewArrayIterator:
      return tryAttachNewArrayIterator();
    case InlinableNative::IntrinsicNewStringIterator:
      return tryAttachNewStringIterator();
    case InlinableNative::IntrinsicNewRegExpStringIterator:
      return tryAttachNewRegExpStringIterator();
    case InlinableNative::IntrinsicArrayIteratorPrototypeOptimizable:
      return tryAttachArrayIteratorPrototypeOptimizable();

    // RegExp natives.
    case InlinableNative::IsRegExpObject:
      return tryAttachHasClass(&RegExpObject::class_,
                               /* isPossiblyWrapped = */ false);
    case InlinableNative::IsPossiblyWrappedRegExpObject:
      return tryAttachHasClass(&RegExpObject::class_,
                               /* isPossiblyWrapped = */ true);
    case InlinableNative::RegExpMatcher:
    case InlinableNative::RegExpSearcher:
      return tryAttachRegExpMatcherSearcher(native);
    case InlinableNative::RegExpSearcherLastLimit:
      return tryAttachRegExpSearcherLastLimit();
    case InlinableNative::RegExpHasCaptureGroups:
      return tryAttachRegExpHasCaptureGroups();
    case InlinableNative::IsRegExpPrototypeOptimizable:
      return tryAttachIsRegExpPrototypeOptimizable();
    case InlinableNative::IsOptimizableRegExpObject:
      return tryAttachIsOptimizableRegExpObject();
    case InlinableNative::GetFirstDollarIndex:
      return tryAttachGetFirstDollarIndex();
    case InlinableNative::IntrinsicRegExpBuiltinExec:
    case InlinableNative::IntrinsicRegExpBuiltinExecForTest:
      return tryAttachIntrinsicRegExpBuiltinExec(native);
    case InlinableNative::IntrinsicRegExpExec:
    case InlinableNative::IntrinsicRegExpExecForTest:
      return tryAttachIntrinsicRegExpExec(native);

    // String natives.
    case InlinableNative::String:
      return tryAttachString();
    case InlinableNative::StringToString:
    case InlinableNative::StringValueOf:
      return tryAttachStringToStringValueOf();
    case InlinableNative::StringCharCodeAt:
      return tryAttachStringCharCodeAt();
    case InlinableNative::StringCodePointAt:
      return tryAttachStringCodePointAt();
    case InlinableNative::StringCharAt:
      return tryAttachStringCharAt();
    case InlinableNative::StringAt:
      return tryAttachStringAt();
    case InlinableNative::StringFromCharCode:
      return tryAttachStringFromCharCode();
    case InlinableNative::StringFromCodePoint:
      return tryAttachStringFromCodePoint();
    case InlinableNative::StringIncludes:
      return tryAttachStringIncludes();
    case InlinableNative::StringIndexOf:
      return tryAttachStringIndexOf();
    case InlinableNative::StringLastIndexOf:
      return tryAttachStringLastIndexOf();
    case InlinableNative::StringStartsWith:
      return tryAttachStringStartsWith();
    case InlinableNative::StringEndsWith:
      return tryAttachStringEndsWith();
    case InlinableNative::StringToLowerCase:
      return tryAttachStringToLowerCase();
    case InlinableNative::StringToUpperCase:
      return tryAttachStringToUpperCase();
    case InlinableNative::StringTrim:
      return tryAttachStringTrim();
    case InlinableNative::StringTrimStart:
      return tryAttachStringTrimStart();
    case InlinableNative::StringTrimEnd:
      return tryAttachStringTrimEnd();
    case InlinableNative::IntrinsicStringReplaceString:
      return tryAttachStringReplaceString();
    case InlinableNative::IntrinsicStringSplitString:
      return tryAttachStringSplitString();

    // Math natives.
    case InlinableNative::MathRandom:
      return tryAttachMathRandom();
    case InlinableNative::MathAbs:
      return tryAttachMathAbs();
    case InlinableNative::MathClz32:
      return tryAttachMathClz32();
    case InlinableNative::MathSign:
      return tryAttachMathSign();
    case InlinableNative::MathImul:
      return tryAttachMathImul();
    case InlinableNative::MathFloor:
      return tryAttachMathFloor();
    case InlinableNative::MathCeil:
      return tryAttachMathCeil();
    case InlinableNative::MathTrunc:
      return tryAttachMathTrunc();
    case InlinableNative::MathRound:
      return tryAttachMathRound();
    case InlinableNative::MathSqrt:
      return tryAttachMathSqrt();
    case InlinableNative::MathFRound:
      return tryAttachMathFRound();
    case InlinableNative::MathF16Round:
      return tryAttachMathF16Round();
    case InlinableNative::MathHypot:
      return tryAttachMathHypot();
    case InlinableNative::MathATan2:
      return tryAttachMathATan2();
    case InlinableNative::MathSin:
      return tryAttachMathFunction(UnaryMathFunction::SinNative);
    case InlinableNative::MathTan:
      return tryAttachMathFunction(UnaryMathFunction::TanNative);
    case InlinableNative::MathCos:
      return tryAttachMathFunction(UnaryMathFunction::CosNative);
    case InlinableNative::MathExp:
      return tryAttachMathFunction(UnaryMathFunction::Exp);
    case InlinableNative::MathLog:
      return tryAttachMathFunction(UnaryMathFunction::Log);
    case InlinableNative::MathASin:
      return tryAttachMathFunction(UnaryMathFunction::ASin);
    case InlinableNative::MathATan:
      return tryAttachMathFunction(UnaryMathFunction::ATan);
    case InlinableNative::MathACos:
      return tryAttachMathFunction(UnaryMathFunction::ACos);
    case InlinableNative::MathLog10:
      return tryAttachMathFunction(UnaryMathFunction::Log10);
    case InlinableNative::MathLog2:
      return tryAttachMathFunction(UnaryMathFunction::Log2);
    case InlinableNative::MathLog1P:
      return tryAttachMathFunction(UnaryMathFunction::Log1P);
    case InlinableNative::MathExpM1:
      return tryAttachMathFunction(UnaryMathFunction::ExpM1);
    case InlinableNative::MathCosH:
      return tryAttachMathFunction(UnaryMathFunction::CosH);
    case InlinableNative::MathSinH:
      return tryAttachMathFunction(UnaryMathFunction::SinH);
    case InlinableNative::MathTanH:
      return tryAttachMathFunction(UnaryMathFunction::TanH);
    case InlinableNative::MathACosH:
      return tryAttachMathFunction(UnaryMathFunction::ACosH);
    case InlinableNative::MathASinH:
      return tryAttachMathFunction(UnaryMathFunction::ASinH);
    case InlinableNative::MathATanH:
      return tryAttachMathFunction(UnaryMathFunction::ATanH);
    case InlinableNative::MathCbrt:
      return tryAttachMathFunction(UnaryMathFunction::Cbrt);
    case InlinableNative::MathPow:
      return tryAttachMathPow();
    case InlinableNative::MathMin:
      return tryAttachMathMinMax(/* isMax = */ false);
    case InlinableNative::MathMax:
      return tryAttachMathMinMax(/* isMax = */ true);

    // Map intrinsics.
    case InlinableNative::IntrinsicGuardToMapObject:
      return tryAttachGuardToClass(GuardClassKind::Map);
    case InlinableNative::IntrinsicGetNextMapEntryForIterator:
      return tryAttachGetNextMapSetEntryForIterator(/* isMap = */ true);

    // Number natives.
    case InlinableNative::Number:
      return tryAttachNumber();
    case InlinableNative::NumberParseInt:
      return tryAttachNumberParseInt();
    case InlinableNative::NumberToString:
      return tryAttachNumberToString();

    // Object natives.
    case InlinableNative::Object:
      return tryAttachObjectConstructor();
    case InlinableNative::ObjectCreate:
      return tryAttachObjectCreate();
    case InlinableNative::ObjectIs:
      return tryAttachObjectIs();
    case InlinableNative::ObjectIsPrototypeOf:
      return tryAttachObjectIsPrototypeOf();
    case InlinableNative::ObjectKeys:
      return tryAttachObjectKeys();
    case InlinableNative::ObjectToString:
      return tryAttachObjectToString();

    // Set intrinsics.
    case InlinableNative::IntrinsicGuardToSetObject:
      return tryAttachGuardToClass(GuardClassKind::Set);
    case InlinableNative::IntrinsicGetNextSetEntryForIterator:
      return tryAttachGetNextMapSetEntryForIterator(/* isMap = */ false);

    // ArrayBuffer intrinsics.
    case InlinableNative::IntrinsicGuardToArrayBuffer:
      return tryAttachGuardToArrayBuffer();

    // SharedArrayBuffer intrinsics.
    case InlinableNative::IntrinsicGuardToSharedArrayBuffer:
      return tryAttachGuardToSharedArrayBuffer();

    // TypedArray natives.
    case InlinableNative::TypedArrayConstructor:
      return AttachDecision::NoAction;  // Not callable.
    case InlinableNative::TypedArrayFill:
      return tryAttachTypedArrayFill();
    case InlinableNative::TypedArraySet:
      return tryAttachTypedArraySet();
    case InlinableNative::TypedArraySubarray:
      return tryAttachTypedArraySubarray();

    // TypedArray intrinsics.
    case InlinableNative::IntrinsicIsTypedArray:
      return tryAttachIsTypedArray(/* isPossiblyWrapped = */ false);
    case InlinableNative::IntrinsicIsPossiblyWrappedTypedArray:
      return tryAttachIsTypedArray(/* isPossiblyWrapped = */ true);
    case InlinableNative::IntrinsicIsTypedArrayConstructor:
      return tryAttachIsTypedArrayConstructor();
    case InlinableNative::IntrinsicTypedArrayLength:
      return tryAttachTypedArrayLength(/* isPossiblyWrapped = */ false);
    case InlinableNative::IntrinsicPossiblyWrappedTypedArrayLength:
      return tryAttachTypedArrayLength(/* isPossiblyWrapped = */ true);

    // Reflect natives.
    case InlinableNative::ReflectGetPrototypeOf:
      return tryAttachReflectGetPrototypeOf();

    // Atomics intrinsics:
    case InlinableNative::AtomicsCompareExchange:
      return tryAttachAtomicsCompareExchange();
    case InlinableNative::AtomicsExchange:
      return tryAttachAtomicsExchange();
    case InlinableNative::AtomicsAdd:
      return tryAttachAtomicsAdd();
    case InlinableNative::AtomicsSub:
      return tryAttachAtomicsSub();
    case InlinableNative::AtomicsAnd:
      return tryAttachAtomicsAnd();
    case InlinableNative::AtomicsOr:
      return tryAttachAtomicsOr();
    case InlinableNative::AtomicsXor:
      return tryAttachAtomicsXor();
    case InlinableNative::AtomicsLoad:
      return tryAttachAtomicsLoad();
    case InlinableNative::AtomicsStore:
      return tryAttachAtomicsStore();
    case InlinableNative::AtomicsIsLockFree:
      return tryAttachAtomicsIsLockFree();
    case InlinableNative::AtomicsPause:
      return tryAttachAtomicsPause();

    // BigInt natives.
    case InlinableNative::BigInt:
      return tryAttachBigInt();
    case InlinableNative::BigIntAsIntN:
      return tryAttachBigIntAsIntN();
    case InlinableNative::BigIntAsUintN:
      return tryAttachBigIntAsUintN();

    // Boolean natives.
    case InlinableNative::Boolean:
      return tryAttachBoolean();

    // Set natives.
    case InlinableNative::SetConstructor:
      return AttachDecision::NoAction;  // Not callable.
    case InlinableNative::SetHas:
      return tryAttachSetHas();
    case InlinableNative::SetDelete:
      return tryAttachSetDelete();
    case InlinableNative::SetAdd:
      return tryAttachSetAdd();
    case InlinableNative::SetSize:
      return tryAttachSetSize();

    // Map natives.
    case InlinableNative::MapConstructor:
      return AttachDecision::NoAction;  // Not callable.
    case InlinableNative::MapHas:
      return tryAttachMapHas();
    case InlinableNative::MapGet:
      return tryAttachMapGet();
    case InlinableNative::MapDelete:
      return tryAttachMapDelete();
    case InlinableNative::MapSet:
      return tryAttachMapSet();

    // Date natives and intrinsics.
    case InlinableNative::DateGetTime:
      return tryAttachDateGetTime();
    case InlinableNative::DateGetFullYear:
      return tryAttachDateGet(DateComponent::FullYear);
    case InlinableNative::DateGetMonth:
      return tryAttachDateGet(DateComponent::Month);
    case InlinableNative::DateGetDate:
      return tryAttachDateGet(DateComponent::Date);
    case InlinableNative::DateGetDay:
      return tryAttachDateGet(DateComponent::Day);
    case InlinableNative::DateGetHours:
      return tryAttachDateGet(DateComponent::Hours);
    case InlinableNative::DateGetMinutes:
      return tryAttachDateGet(DateComponent::Minutes);
    case InlinableNative::DateGetSeconds:
      return tryAttachDateGet(DateComponent::Seconds);

    // WeakMap/WeakSet natives.
    case InlinableNative::WeakMapGet:
      return tryAttachWeakMapGet();
    case InlinableNative::WeakMapHas:
      return tryAttachWeakMapHas();
    case InlinableNative::WeakSetHas:
      return tryAttachWeakSetHas();

    // Testing functions.
    case InlinableNative::TestBailout:
      if (js::SupportDifferentialTesting()) {
        return AttachDecision::NoAction;
      }
      return tryAttachBailout();
    case InlinableNative::TestAssertFloat32:
      return tryAttachAssertFloat32();
    case InlinableNative::TestAssertRecoveredOnBailout:
      if (js::SupportDifferentialTesting()) {
        return AttachDecision::NoAction;
      }
      return tryAttachAssertRecoveredOnBailout();

#ifdef FUZZING_JS_FUZZILLI
    // Fuzzilli function
    case InlinableNative::FuzzilliHash:
      return tryAttachFuzzilliHash();
#endif

    case InlinableNative::Limit:
      break;
  }

  MOZ_CRASH("Shouldn't get here");
}

// Remember the shape of the this object for any script being called as a
// constructor, for later use during Ion compilation.
ScriptedThisResult CallIRGenerator::getThisShapeForScripted(
    HandleFunction calleeFunc, Handle<JSObject*> newTarget,
    MutableHandle<Shape*> result) {
  // Some constructors allocate their own |this| object.
  if (calleeFunc->constructorNeedsUninitializedThis()) {
    return ScriptedThisResult::UninitializedThis;
  }

  // Only attach a stub if the newTarget is a function with a
  // nonconfigurable prototype.
  if (!newTarget->is<JSFunction>() ||
      !newTarget->as<JSFunction>().hasNonConfigurablePrototypeDataProperty()) {
    return ScriptedThisResult::NoAction;
  }

  AutoRealm ar(cx_, calleeFunc);
  Shape* thisShape = ThisShapeForFunction(cx_, calleeFunc, newTarget);
  if (!thisShape) {
    cx_->clearPendingException();
    return ScriptedThisResult::NoAction;
  }

  MOZ_ASSERT(thisShape->realm() == calleeFunc->realm());
  result.set(thisShape);
  return ScriptedThisResult::PlainObjectShape;
}

static bool CanOptimizeScriptedCall(JSFunction* callee, bool isConstructing) {
  if (!callee->hasJitEntry()) {
    return false;
  }

  // If callee is not an interpreted constructor, we have to throw.
  if (isConstructing && !callee->isConstructor()) {
    return false;
  }

  // Likewise, if the callee is a class constructor, we have to throw.
  if (!isConstructing && callee->isClassConstructor()) {
    return false;
  }

  return true;
}

void CallIRGenerator::emitCallScriptedGuards(ObjOperandId calleeObjId,
                                             JSFunction* calleeFunc,
                                             Int32OperandId argcId,
                                             CallFlags flags, Shape* thisShape,
                                             bool isBoundFunction) {
  bool isConstructing = flags.isConstructing();

  if (mode_ == ICState::Mode::Specialized) {
    MOZ_ASSERT_IF(isConstructing, thisShape || flags.needsUninitializedThis());

    // Ensure callee matches this stub's callee
    emitCalleeGuard(calleeObjId, calleeFunc);
    if (thisShape) {
      // Emit guards to ensure the newTarget's .prototype property is what we
      // expect. Note that getThisForScripted checked newTarget is a function
      // with a non-configurable .prototype data property.

      JSFunction* newTarget;
      ObjOperandId newTargetObjId;
      if (isBoundFunction) {
        newTarget = calleeFunc;
        newTargetObjId = calleeObjId;
      } else {
        newTarget = &newTarget_.toObject().as<JSFunction>();
        ValOperandId newTargetValId = writer.loadArgumentDynamicSlot(
            ArgumentKind::NewTarget, argcId, flags);
        newTargetObjId = writer.guardToObject(newTargetValId);
      }

      Maybe<PropertyInfo> prop = newTarget->lookupPure(cx_->names().prototype);
      MOZ_ASSERT(prop.isSome());
      uint32_t slot = prop->slot();
      MOZ_ASSERT(slot >= newTarget->numFixedSlots(),
                 "Stub code relies on this");

      writer.guardShape(newTargetObjId, newTarget->shape());

      const Value& value = newTarget->getSlot(slot);
      if (value.isObject()) {
        JSObject* prototypeObject = &value.toObject();

        ObjOperandId protoId = writer.loadObject(prototypeObject);
        writer.guardDynamicSlotIsSpecificObject(
            newTargetObjId, protoId, slot - newTarget->numFixedSlots());
      } else {
        writer.guardDynamicSlotIsNotObject(newTargetObjId,
                                           slot - newTarget->numFixedSlots());
      }

      // Call metaScriptedThisShape before emitting the call, so that Warp can
      // use the shape to create the |this| object before transpiling the call.
      writer.metaScriptedThisShape(thisShape);
    }
  } else {
    // Guard that object is a scripted function
    writer.guardClass(calleeObjId, GuardClassKind::JSFunction);
    writer.guardFunctionHasJitEntry(calleeObjId);

    if (isConstructing) {
      // If callee is not a constructor, we have to throw.
      writer.guardFunctionIsConstructor(calleeObjId);
    } else {
      // If callee is a class constructor, we have to throw.
      writer.guardNotClassConstructor(calleeObjId);
    }
  }
}

AttachDecision CallIRGenerator::tryAttachCallScripted(
    HandleFunction calleeFunc) {
  MOZ_ASSERT(calleeFunc->hasJitEntry());

  if (calleeFunc->isWasmWithJitEntry()) {
    TRY_ATTACH(tryAttachWasmCall(calleeFunc));
  }

  bool isSpecialized = mode_ == ICState::Mode::Specialized;

  bool isConstructing = IsConstructPC(pc_);
  bool isSpread = IsSpreadPC(pc_);
  bool isSameRealm = isSpecialized && cx_->realm() == calleeFunc->realm();
  CallFlags flags(isConstructing, isSpread, isSameRealm);

  if (!CanOptimizeScriptedCall(calleeFunc, isConstructing)) {
    return AttachDecision::NoAction;
  }

  if (isConstructing && !calleeFunc->hasJitScript()) {
    // If we're constructing, require the callee to have a JitScript. This isn't
    // required for correctness but avoids allocating a template object below
    // for constructors that aren't hot. See bug 1419758.
    return AttachDecision::TemporarilyUnoptimizable;
  }

  // Verify that spread calls have a reasonable number of arguments.
  if (isSpread && args_.length() > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  Rooted<Shape*> thisShape(cx_);
  if (isConstructing && isSpecialized) {
    Rooted<JSObject*> newTarget(cx_, &newTarget_.toObject());
    switch (getThisShapeForScripted(calleeFunc, newTarget, &thisShape)) {
      case ScriptedThisResult::PlainObjectShape:
        break;
      case ScriptedThisResult::UninitializedThis:
        flags.setNeedsUninitializedThis();
        break;
      case ScriptedThisResult::NoAction:
        return AttachDecision::NoAction;
    }
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  emitCallScriptedGuards(calleeObjId, calleeFunc, argcId, flags, thisShape,
                         /* isBoundFunction = */ false);

  writer.callScriptedFunction(calleeObjId, argcId, flags,
                              ClampFixedArgc(argc_));
  writer.returnFromIC();

  if (isSpecialized) {
    trackAttached("Call.CallScripted");
  } else {
    trackAttached("Call.CallAnyScripted");
  }

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachCallNative(HandleFunction calleeFunc) {
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  bool isSpecialized = mode_ == ICState::Mode::Specialized;

  bool isSpread = IsSpreadPC(pc_);
  bool isSameRealm = isSpecialized && cx_->realm() == calleeFunc->realm();
  bool isConstructing = IsConstructPC(pc_);
  CallFlags flags(isConstructing, isSpread, isSameRealm);

  if (isConstructing && !calleeFunc->isConstructor()) {
    return AttachDecision::NoAction;
  }

  // Verify that spread calls have a reasonable number of arguments.
  if (isSpread && args_.length() > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Check for specific native-function optimizations.
  if (isSpecialized) {
    TRY_ATTACH(tryAttachInlinableNative(calleeFunc, flags));
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  // DOM calls need an additional guard so only try optimizing the first stub.
  // Can only optimize normal (non-spread) calls.
  if (isFirstStub_ && !isSpread && thisval_.isObject() &&
      CanAttachDOMCall(cx_, JSJitInfo::Method, &thisval_.toObject(), calleeFunc,
                       mode_)) {
    MOZ_ASSERT(!isConstructing, "DOM functions are not constructors");

    gc::AllocSite* allocSite = nullptr;
    if (calleeFunc->jitInfo()->returnType() == JSVAL_TYPE_OBJECT &&
        JS::Prefs::dom_alloc_site()) {
      allocSite = maybeCreateAllocSite();
      if (!allocSite) {
        return AttachDecision::NoAction;
      }
    }

    // Guard that |this| is an object.
    ValOperandId thisValId =
        writer.loadArgumentDynamicSlot(ArgumentKind::This, argcId, flags);
    ObjOperandId thisObjId = writer.guardToObject(thisValId);

    // Guard on the |this| shape to make sure it's the right instance. This also
    // ensures DOM_OBJECT_SLOT is stored in a fixed slot. See CanAttachDOMCall.
    writer.guardShape(thisObjId, thisval_.toObject().shape());

    // Ensure callee matches this stub's callee
    writer.guardSpecificFunction(calleeObjId, calleeFunc);

    if (allocSite) {
      writer.callDOMFunctionWithAllocSite(calleeObjId, argcId, thisObjId,
                                          calleeFunc, flags,
                                          ClampFixedArgc(argc_), allocSite);
    } else {
      writer.callDOMFunction(calleeObjId, argcId, thisObjId, calleeFunc, flags,
                             ClampFixedArgc(argc_));
    }

    trackAttached("Call.CallDOM");
  } else if (isSpecialized) {
    // Ensure callee matches this stub's callee
    writer.guardSpecificFunction(calleeObjId, calleeFunc);
    writer.callNativeFunction(calleeObjId, argcId, op_, calleeFunc, flags,
                              ClampFixedArgc(argc_));

    trackAttached("Call.CallNative");
  } else {
    // Guard that object is a native function
    writer.guardClass(calleeObjId, GuardClassKind::JSFunction);
    writer.guardFunctionHasNoJitEntry(calleeObjId);

    if (isConstructing) {
      // If callee is not a constructor, we have to throw.
      writer.guardFunctionIsConstructor(calleeObjId);
    } else {
      // If callee is a class constructor, we have to throw.
      writer.guardNotClassConstructor(calleeObjId);
    }
    writer.callAnyNativeFunction(calleeObjId, argcId, flags,
                                 ClampFixedArgc(argc_));

    trackAttached("Call.CallAnyNative");
  }

  writer.returnFromIC();

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachCallHook(HandleObject calleeObj) {
  if (mode_ != ICState::Mode::Specialized) {
    // We do not have megamorphic call hook stubs.
    // TODO: Should we attach specialized call hook stubs in
    // megamorphic mode to avoid going generic?
    return AttachDecision::NoAction;
  }

  bool isSpread = IsSpreadPC(pc_);
  bool isConstructing = IsConstructPC(pc_);
  CallFlags flags(isConstructing, isSpread);
  JSNative hook =
      isConstructing ? calleeObj->constructHook() : calleeObj->callHook();
  if (!hook) {
    return AttachDecision::NoAction;
  }

  // Bound functions have a JSClass construct hook but are not always
  // constructors.
  if (isConstructing && !calleeObj->isConstructor()) {
    return AttachDecision::NoAction;
  }

  // We don't support spread calls in the transpiler yet.
  if (isSpread) {
    return AttachDecision::NoAction;
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it is an object
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);

  // Ensure the callee's class matches the one in this stub.
  writer.guardAnyClass(calleeObjId, calleeObj->getClass());

  if (isConstructing && calleeObj->is<BoundFunctionObject>()) {
    writer.guardBoundFunctionIsConstructor(calleeObjId);
  }

  writer.callClassHook(calleeObjId, argcId, hook, flags, ClampFixedArgc(argc_));
  writer.returnFromIC();

  trackAttached("Call.CallHook");

  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachBoundFunction(
    Handle<BoundFunctionObject*> calleeObj) {
  // The target must be a JSFunction with a JitEntry.
  if (!calleeObj->getTarget()->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }

  bool isSpread = IsSpreadPC(pc_);
  bool isConstructing = IsConstructPC(pc_);

  // Spread calls are not supported yet.
  if (isSpread) {
    return AttachDecision::NoAction;
  }

  Rooted<JSFunction*> target(cx_, &calleeObj->getTarget()->as<JSFunction>());
  if (!CanOptimizeScriptedCall(target, isConstructing)) {
    return AttachDecision::NoAction;
  }

  // Limit the number of bound arguments to prevent us from compiling many
  // different stubs (we bake in numBoundArgs and it's usually very small).
  static constexpr size_t MaxBoundArgs = 10;
  size_t numBoundArgs = calleeObj->numBoundArgs();
  if (numBoundArgs > MaxBoundArgs) {
    return AttachDecision::NoAction;
  }

  // Ensure we don't exceed JIT_ARGS_LENGTH_MAX.
  if (numBoundArgs + argc_ > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  CallFlags flags(isConstructing, isSpread);

  if (mode_ == ICState::Mode::Specialized) {
    if (cx_->realm() == target->realm()) {
      flags.setIsSameRealm();
    }
  }

  Rooted<Shape*> thisShape(cx_);
  if (isConstructing) {
    // Only optimize if newTarget == callee. This is the common case and ensures
    // we can always pass the bound function's target as newTarget.
    if (newTarget_ != ObjectValue(*calleeObj)) {
      return AttachDecision::NoAction;
    }

    if (mode_ == ICState::Mode::Specialized) {
      Handle<JSFunction*> newTarget = target;
      switch (getThisShapeForScripted(target, newTarget, &thisShape)) {
        case ScriptedThisResult::PlainObjectShape:
          break;
        case ScriptedThisResult::UninitializedThis:
          flags.setNeedsUninitializedThis();
          break;
        case ScriptedThisResult::NoAction:
          return AttachDecision::NoAction;
      }
    }
  }

  // Load argc.
  Int32OperandId argcId(writer.setInputOperandId(0));

  // Load the callee and ensure it's a bound function.
  ValOperandId calleeValId =
      writer.loadArgumentDynamicSlot(ArgumentKind::Callee, argcId, flags);
  ObjOperandId calleeObjId = writer.guardToObject(calleeValId);
  writer.guardClass(calleeObjId, GuardClassKind::BoundFunction);

  // Ensure numBoundArgs matches.
  Int32OperandId numBoundArgsId = writer.loadBoundFunctionNumArgs(calleeObjId);
  writer.guardSpecificInt32(numBoundArgsId, numBoundArgs);

  if (isConstructing) {
    // Guard newTarget == callee. We depend on this in CallBoundScriptedFunction
    // and in emitCallScriptedGuards by using boundTarget as newTarget.
    ValOperandId newTargetValId =
        writer.loadArgumentDynamicSlot(ArgumentKind::NewTarget, argcId, flags);
    ObjOperandId newTargetObjId = writer.guardToObject(newTargetValId);
    writer.guardObjectIdentity(newTargetObjId, calleeObjId);
  }

  ObjOperandId targetId = writer.loadBoundFunctionTarget(calleeObjId);

  emitCallScriptedGuards(targetId, target, argcId, flags, thisShape,
                         /* isBoundFunction = */ true);

  writer.callBoundScriptedFunction(calleeObjId, targetId, argcId, flags,
                                   numBoundArgs);
  writer.returnFromIC();

  trackAttached("Call.BoundFunction");
  return AttachDecision::Attach;
}

AttachDecision CallIRGenerator::tryAttachBoundNative(
    Handle<BoundFunctionObject*> calleeObj) {
  // The target must be a native JSFunction without a JitEntry.
  Rooted<JSObject*> boundTarget(cx_, calleeObj->getTarget());
  if (!boundTarget->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto target = boundTarget.as<JSFunction>();

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  if (isScripted) {
    return AttachDecision::NoAction;
  }

  // Limit the number of bound arguments to prevent us from compiling many
  // different stubs (we bake in numBoundArgs and it's usually very small).
  static constexpr size_t MaxBoundArgs = 10;
  size_t numBoundArgs = calleeObj->numBoundArgs();
  if (numBoundArgs > MaxBoundArgs) {
    return AttachDecision::NoAction;
  }

  // Ensure we don't exceed JIT_ARGS_LENGTH_MAX.
  if (numBoundArgs + argc_ > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Don't try to optimize when we're already megamorphic.
  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  bool isSpread = IsSpreadPC(pc_);
  bool isSameRealm = cx_->realm() == target->realm();
  bool isConstructing = IsConstructPC(pc_);
  CallFlags flags(isConstructing, isSpread, isSameRealm);

  if (isConstructing && !target->isConstructor()) {
    return AttachDecision::NoAction;
  }

  // Verify that spread calls have a reasonable number of arguments.
  if (isSpread && args_.length() > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Spread calls are only supported when we don't have to insert bound args.
  if (isSpread && numBoundArgs != 0) {
    return AttachDecision::NoAction;
  }

  // Use the bound |this| value.
  Rooted<Value> thisValue(cx_, calleeObj->getBoundThis());

  // Concatenate the bound arguments and the stack arguments.
  JS::RootedVector<Value> concatenatedArgs(cx_);
  if (numBoundArgs != 0) {
    if (!concatenatedArgs.reserve(numBoundArgs + args_.length())) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }

    for (size_t i = 0; i < numBoundArgs; i++) {
      concatenatedArgs.infallibleAppend(calleeObj->getBoundArg(i));
    }
    concatenatedArgs.infallibleAppend(args_.begin(), args_.length());
  }
  auto args = numBoundArgs != 0 ? concatenatedArgs : args_;

  // Check for specific native-function optimizations.
  InlinableNativeIRGenerator nativeGen(*this, target, newTarget_, thisValue,
                                       args, flags);
  return nativeGen.tryAttachStub();
}

AttachDecision CallIRGenerator::tryAttachBoundFunCall(
    Handle<BoundFunctionObject*> calleeObj) {
  // Only optimize fun_call for simple calls.
  if (op_ != JSOp::Call && op_ != JSOp::CallContent &&
      op_ != JSOp::CallIgnoresRv) {
    return AttachDecision::NoAction;
  }

  // The target must be a native JSFunction to fun_call.
  JSObject* boundTarget = calleeObj->getTarget();
  if (!boundTarget->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto* boundTargetFn = &boundTarget->as<JSFunction>();

  bool isScripted = boundTargetFn->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, boundTargetFn->isNativeWithoutJitEntry());

  if (isScripted || boundTargetFn->native() != fun_call) {
    return AttachDecision::NoAction;
  }

  // Limit the number of bound arguments to prevent us from compiling many
  // different stubs (we bake in numBoundArgs and it's usually very small).
  static constexpr size_t MaxBoundArgs = 10;
  size_t numBoundArgs = calleeObj->numBoundArgs();
  if (numBoundArgs > MaxBoundArgs) {
    return AttachDecision::NoAction;
  }

  // Ensure we don't exceed JIT_ARGS_LENGTH_MAX.
  if (numBoundArgs + argc_ > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Don't try to optimize when we're already megamorphic.
  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  JSFunction* boundThis;
  if (!IsFunctionObject(calleeObj->getBoundThis(), &boundThis)) {
    return AttachDecision::NoAction;
  }

  bool boundThisIsScripted = boundThis->hasJitEntry();
  MOZ_ASSERT_IF(!boundThisIsScripted, boundThis->isNativeWithoutJitEntry());

  if (boundThisIsScripted) {
    return AttachDecision::NoAction;
  }

  CallFlags targetFlags(CallFlags::FunCall);
  if (cx_->realm() == boundThis->realm()) {
    targetFlags.setIsSameRealm();
  }

  Rooted<JSFunction*> target(cx_, boundThis);
  HandleValue newTarget = NullHandleValue;

  Rooted<Value> thisValue(cx_);
  if (numBoundArgs > 0) {
    thisValue = calleeObj->getBoundArg(0);
  } else if (argc_ > 0) {
    thisValue = args_[0];
  } else {
    MOZ_ASSERT(thisValue.isUndefined());
  }

  // Concatenate the bound arguments and the stack arguments.
  JS::RootedVector<Value> concatenatedArgs(cx_);
  if (numBoundArgs > 1) {
    if (!concatenatedArgs.reserve((numBoundArgs - 1) + args_.length())) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }

    for (size_t i = 1; i < numBoundArgs; i++) {
      concatenatedArgs.infallibleAppend(calleeObj->getBoundArg(i));
    }
    concatenatedArgs.infallibleAppend(args_.begin(), args_.length());
  }
  auto args = ([&]() -> HandleValueArray {
    if (numBoundArgs > 1) {
      // Return |concatenatedArgs| if there are any bound arguments.
      return concatenatedArgs;
    }
    if (numBoundArgs > 0) {
      // Return |args_| if only the |this| value is bound.
      return args_;
    }
    if (argc_ > 0) {
      // Nothing bound at all, return stack arguments starting from |args[1]|.
      return HandleValueArray::subarray(args_, 1, args_.length() - 1);
    }
    // No arguments at all.
    return HandleValueArray::empty();
  })();

  // Check for specific native-function optimizations.
  InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                       args, targetFlags);
  return nativeGen.tryAttachStub();
}

AttachDecision CallIRGenerator::tryAttachBoundFunApply(
    Handle<BoundFunctionObject*> calleeObj) {
  // Only optimize fun_apply for simple calls.
  if (op_ != JSOp::Call && op_ != JSOp::CallContent &&
      op_ != JSOp::CallIgnoresRv) {
    return AttachDecision::NoAction;
  }

  // The target must be a native JSFunction to fun_apply.
  JSObject* boundTarget = calleeObj->getTarget();
  if (!boundTarget->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto* boundTargetFn = &boundTarget->as<JSFunction>();

  bool isScripted = boundTargetFn->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, boundTargetFn->isNativeWithoutJitEntry());

  if (isScripted || boundTargetFn->native() != fun_apply) {
    return AttachDecision::NoAction;
  }

  size_t numBoundArgs = calleeObj->numBoundArgs();
  if (numBoundArgs + argc_ > 2) {
    return AttachDecision::NoAction;
  }

  // Don't try to optimize when we're already megamorphic.
  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  JSFunction* boundThis;
  if (!IsFunctionObject(calleeObj->getBoundThis(), &boundThis)) {
    return AttachDecision::NoAction;
  }

  bool boundThisIsScripted = boundThis->hasJitEntry();
  MOZ_ASSERT_IF(!boundThisIsScripted, boundThis->isNativeWithoutJitEntry());

  if (boundThisIsScripted) {
    return AttachDecision::NoAction;
  }

  // The second argument must be |null| or |undefined|, because we only support
  // |CallFlags::FunCall| and |CallFlags::FunApplyNullUndefined|.
  CallFlags::ArgFormat format;
  if (numBoundArgs + argc_ < 2) {
    format = CallFlags::FunCall;
  } else {
    Value arg;
    if (numBoundArgs == 2) {
      arg = calleeObj->getBoundArg(1);
    } else if (numBoundArgs == 1) {
      arg = args_[0];
    } else {
      arg = args_[1];
    }
    if (!arg.isNullOrUndefined()) {
      return AttachDecision::NoAction;
    }
    format = CallFlags::FunApplyNullUndefined;
  }

  CallFlags targetFlags(format);
  if (cx_->realm() == boundThis->realm()) {
    targetFlags.setIsSameRealm();
  }

  Rooted<JSFunction*> target(cx_, boundThis);
  HandleValue newTarget = NullHandleValue;

  Rooted<Value> thisValue(cx_);
  if (numBoundArgs > 0) {
    thisValue = calleeObj->getBoundArg(0);
  } else if (argc_ > 0) {
    thisValue = args_[0];
  } else {
    MOZ_ASSERT(thisValue.isUndefined());
  }
  HandleValueArray args = HandleValueArray::empty();

  // Check for specific native-function optimizations.
  InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                       args, targetFlags);
  return nativeGen.tryAttachStub();
}

AttachDecision CallIRGenerator::tryAttachFunCallBound(
    Handle<JSFunction*> callee) {
  MOZ_ASSERT(callee->isNativeWithoutJitEntry());

  if (callee->native() != fun_call) {
    return AttachDecision::NoAction;
  }

  if (!thisval_.isObject() || !thisval_.toObject().is<BoundFunctionObject>()) {
    return AttachDecision::NoAction;
  }
  Rooted<BoundFunctionObject*> bound(
      cx_, &thisval_.toObject().as<BoundFunctionObject>());

  // The target must be a native JSFunction without a JitEntry.
  Rooted<JSObject*> boundTarget(cx_, bound->getTarget());
  if (!boundTarget->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto target = boundTarget.as<JSFunction>();

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  // We don't yet supported scripted bound targets.
  if (isScripted) {
    return AttachDecision::NoAction;
  }

  // Limit the number of bound arguments to prevent us from compiling many
  // different stubs (we bake in numBoundArgs and it's usually very small).
  static constexpr size_t MaxBoundArgs = 10;
  size_t numBoundArgs = bound->numBoundArgs();
  if (numBoundArgs > MaxBoundArgs) {
    return AttachDecision::NoAction;
  }

  // Ensure we don't exceed JIT_ARGS_LENGTH_MAX.
  if (numBoundArgs + argc_ > JIT_ARGS_LENGTH_MAX) {
    return AttachDecision::NoAction;
  }

  // Don't try to optimize when we're already megamorphic.
  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  CallFlags targetFlags(CallFlags::FunCall);
  if (cx_->realm() == target->realm()) {
    targetFlags.setIsSameRealm();
  }

  HandleValue newTarget = NullHandleValue;

  // Use the bound |this| value.
  Rooted<Value> thisValue(cx_, bound->getBoundThis());

  auto callArgs = argc_ > 0
                      ? HandleValueArray::subarray(args_, 1, args_.length() - 1)
                      : HandleValueArray::empty();

  // Concatenate the bound arguments and the stack arguments.
  JS::RootedVector<Value> concatenatedArgs(cx_);
  if (numBoundArgs != 0) {
    if (!concatenatedArgs.reserve(numBoundArgs + callArgs.length())) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }

    for (size_t i = 0; i < numBoundArgs; i++) {
      concatenatedArgs.infallibleAppend(bound->getBoundArg(i));
    }
    concatenatedArgs.infallibleAppend(callArgs.begin(), callArgs.length());
  }

  // Actual args.
  auto args = numBoundArgs != 0 ? concatenatedArgs : callArgs;

  // Check for specific native-function optimizations.
  InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                       args, targetFlags);
  return nativeGen.tryAttachStub();
}

AttachDecision CallIRGenerator::tryAttachFunApplyBound(
    Handle<JSFunction*> callee) {
  MOZ_ASSERT(callee->isNativeWithoutJitEntry());

  if (callee->native() != fun_apply) {
    return AttachDecision::NoAction;
  }

  if (argc_ > 2) {
    return AttachDecision::NoAction;
  }

  if (!thisval_.isObject() || !thisval_.toObject().is<BoundFunctionObject>()) {
    return AttachDecision::NoAction;
  }
  Rooted<BoundFunctionObject*> bound(
      cx_, &thisval_.toObject().as<BoundFunctionObject>());

  // The target must be a native JSFunction without a JitEntry.
  Rooted<JSObject*> boundTarget(cx_, bound->getTarget());
  if (!boundTarget->is<JSFunction>()) {
    return AttachDecision::NoAction;
  }
  auto target = boundTarget.as<JSFunction>();

  bool isScripted = target->hasJitEntry();
  MOZ_ASSERT_IF(!isScripted, target->isNativeWithoutJitEntry());

  // We don't yet supported scripted bound targets.
  if (isScripted) {
    return AttachDecision::NoAction;
  }

  // Limit the number of bound arguments to prevent us from compiling many
  // different stubs (we bake in numBoundArgs and it's usually very small).
  static constexpr size_t MaxBoundArgs = 10;
  size_t numBoundArgs = bound->numBoundArgs();
  if (numBoundArgs > MaxBoundArgs) {
    return AttachDecision::NoAction;
  }

  // The second argument must be |null| or |undefined|, because we only support
  // |CallFlags::FunCall| and |CallFlags::FunApplyNullUndefined|.
  CallFlags::ArgFormat format;
  if (argc_ < 2) {
    format = CallFlags::FunCall;
  } else if (args_[1].isNullOrUndefined()) {
    format = CallFlags::FunApplyNullUndefined;
  } else {
    return AttachDecision::NoAction;
  }

  // Don't try to optimize when we're already megamorphic.
  if (mode_ != ICState::Mode::Specialized) {
    return AttachDecision::NoAction;
  }

  CallFlags targetFlags(format);
  if (cx_->realm() == target->realm()) {
    targetFlags.setIsSameRealm();
  }

  HandleValue newTarget = NullHandleValue;

  // Use the bound |this| value.
  Rooted<Value> thisValue(cx_, bound->getBoundThis());

  // Collect all bound arguments.
  JS::RootedVector<Value> args(cx_);
  if (numBoundArgs != 0) {
    if (!args.reserve(numBoundArgs)) {
      cx_->recoverFromOutOfMemory();
      return AttachDecision::NoAction;
    }

    for (size_t i = 0; i < numBoundArgs; i++) {
      args.infallibleAppend(bound->getBoundArg(i));
    }
  }

  // Check for specific native-function optimizations.
  InlinableNativeIRGenerator nativeGen(*this, target, newTarget, thisValue,
                                       args, targetFlags);
  return nativeGen.tryAttachStub();
}

AttachDecision CallIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  // Some opcodes are not yet supported.
  switch (op_) {
    case JSOp::Call:
    case JSOp::CallContent:
    case JSOp::CallIgnoresRv:
    case JSOp::CallIter:
    case JSOp::CallContentIter:
    case JSOp::SpreadCall:
    case JSOp::New:
    case JSOp::NewContent:
    case JSOp::SpreadNew:
    case JSOp::SuperCall:
    case JSOp::SpreadSuperCall:
      break;
    default:
      return AttachDecision::NoAction;
  }

  MOZ_ASSERT(mode_ != ICState::Mode::Generic);

  // Ensure callee is a function.
  if (!callee_.isObject()) {
    return AttachDecision::NoAction;
  }

  RootedObject calleeObj(cx_, &callee_.toObject());
  if (calleeObj->is<BoundFunctionObject>()) {
    auto boundCalleeObj = calleeObj.as<BoundFunctionObject>();

    TRY_ATTACH(tryAttachBoundFunction(boundCalleeObj));
    TRY_ATTACH(tryAttachBoundNative(boundCalleeObj));
    TRY_ATTACH(tryAttachBoundFunCall(boundCalleeObj));
    TRY_ATTACH(tryAttachBoundFunApply(boundCalleeObj));
  }
  if (!calleeObj->is<JSFunction>()) {
    return tryAttachCallHook(calleeObj);
  }

  HandleFunction calleeFunc = calleeObj.as<JSFunction>();

  // Check for scripted optimizations.
  if (calleeFunc->hasJitEntry()) {
    return tryAttachCallScripted(calleeFunc);
  }

  // Check for native-function optimizations.
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  // Try inlining Function.prototype.{call,apply}. We don't use the
  // InlinableNative mechanism for this because we want to optimize these more
  // aggressively than other natives.
  if (op_ == JSOp::Call || op_ == JSOp::CallContent ||
      op_ == JSOp::CallIgnoresRv) {
    TRY_ATTACH(tryAttachFunCall(calleeFunc));
    TRY_ATTACH(tryAttachFunApply(calleeFunc));
    TRY_ATTACH(tryAttachFunCallBound(calleeFunc));
    TRY_ATTACH(tryAttachFunApplyBound(calleeFunc));
  }

  return tryAttachCallNative(calleeFunc);
}

void CallIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("callee", callee_);
    sp.valueProperty("thisval", thisval_);
    sp.valueProperty("argc", Int32Value(argc_));

    // Try to log the first two arguments.
    if (args_.length() >= 1) {
      sp.valueProperty("arg0", args_[0]);
    }
    if (args_.length() >= 2) {
      sp.valueProperty("arg1", args_[1]);
    }
  }
#endif
}

// Class which holds a shape pointer for use when caches might reference data in
// other zones.
static const JSClass shapeContainerClass = {"ShapeContainer",
                                            JSCLASS_HAS_RESERVED_SLOTS(1)};

static const size_t SHAPE_CONTAINER_SLOT = 0;

static JSObject* NewWrapperWithObjectShape(JSContext* cx,
                                           Handle<NativeObject*> obj) {
  MOZ_ASSERT(cx->compartment() != obj->compartment());

  RootedObject wrapper(cx);
  {
    AutoRealm ar(cx, obj);
    wrapper = NewBuiltinClassInstance(cx, &shapeContainerClass);
    if (!wrapper) {
      return nullptr;
    }
    wrapper->as<NativeObject>().setReservedSlot(
        SHAPE_CONTAINER_SLOT, PrivateGCThingValue(obj->shape()));
  }
  if (!JS_WrapObject(cx, &wrapper)) {
    return nullptr;
  }
  MOZ_ASSERT(IsWrapper(wrapper));
  return wrapper;
}

void jit::LoadShapeWrapperContents(MacroAssembler& masm, Register obj,
                                   Register dst, Label* failure) {
  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), dst);
  Address privateAddr(dst,
                      js::detail::ProxyReservedSlots::offsetOfPrivateSlot());
  masm.fallibleUnboxObject(privateAddr, dst, failure);
  masm.unboxNonDouble(
      Address(dst, NativeObject::getFixedSlotOffset(SHAPE_CONTAINER_SLOT)), dst,
      JSVAL_TYPE_PRIVATE_GCTHING);
}

static bool CanConvertToInt32ForToNumber(const Value& v) {
  return v.isInt32() || v.isBoolean() || v.isNull();
}

static Int32OperandId EmitGuardToInt32ForToNumber(CacheIRWriter& writer,
                                                  ValOperandId id,
                                                  const Value& v) {
  if (v.isInt32()) {
    return writer.guardToInt32(id);
  }
  if (v.isNull()) {
    writer.guardIsNull(id);
    return writer.loadInt32Constant(0);
  }
  MOZ_ASSERT(v.isBoolean());
  return writer.guardBooleanToInt32(id);
}

static bool CanConvertToDoubleForToNumber(const Value& v) {
  return v.isNumber() || v.isBoolean() || v.isNullOrUndefined();
}

static NumberOperandId EmitGuardToDoubleForToNumber(CacheIRWriter& writer,
                                                    ValOperandId id,
                                                    const Value& v) {
  if (v.isNumber()) {
    return writer.guardIsNumber(id);
  }
  if (v.isBoolean()) {
    BooleanOperandId boolId = writer.guardToBoolean(id);
    return writer.booleanToNumber(boolId);
  }
  if (v.isNull()) {
    writer.guardIsNull(id);
    return writer.loadDoubleConstant(0.0);
  }
  MOZ_ASSERT(v.isUndefined());
  writer.guardIsUndefined(id);
  return writer.loadDoubleConstant(JS::GenericNaN());
}

CompareIRGenerator::CompareIRGenerator(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, ICState state, JSOp op,
                                       HandleValue lhsVal, HandleValue rhsVal)
    : IRGenerator(cx, script, pc, CacheKind::Compare, state),
      op_(op),
      lhsVal_(lhsVal),
      rhsVal_(rhsVal) {}

AttachDecision CompareIRGenerator::tryAttachString(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  if (!lhsVal_.isString() || !rhsVal_.isString()) {
    return AttachDecision::NoAction;
  }

  StringOperandId lhsStrId = writer.guardToString(lhsId);
  StringOperandId rhsStrId = writer.guardToString(rhsId);
  writer.compareStringResult(op_, lhsStrId, rhsStrId);
  writer.returnFromIC();

  trackAttached("Compare.String");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachObject(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  if (!lhsVal_.isObject() || !rhsVal_.isObject()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId lhsObjId = writer.guardToObject(lhsId);
  ObjOperandId rhsObjId = writer.guardToObject(rhsId);
  writer.compareObjectResult(op_, lhsObjId, rhsObjId);
  writer.returnFromIC();

  trackAttached("Compare.Object");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachSymbol(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  if (!lhsVal_.isSymbol() || !rhsVal_.isSymbol()) {
    return AttachDecision::NoAction;
  }

  SymbolOperandId lhsSymId = writer.guardToSymbol(lhsId);
  SymbolOperandId rhsSymId = writer.guardToSymbol(rhsId);
  writer.compareSymbolResult(op_, lhsSymId, rhsSymId);
  writer.returnFromIC();

  trackAttached("Compare.Symbol");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachStrictDifferentTypes(
    ValOperandId lhsId, ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  if (op_ != JSOp::StrictEq && op_ != JSOp::StrictNe) {
    return AttachDecision::NoAction;
  }

  // Probably can't hit some of these.
  if (SameType(lhsVal_, rhsVal_) ||
      (lhsVal_.isNumber() && rhsVal_.isNumber())) {
    return AttachDecision::NoAction;
  }

  // Compare tags
  ValueTagOperandId lhsTypeId = writer.loadValueTag(lhsId);
  ValueTagOperandId rhsTypeId = writer.loadValueTag(rhsId);
  writer.guardTagNotEqual(lhsTypeId, rhsTypeId);

  // Now that we've passed the guard, we know differing types, so return the
  // bool result.
  writer.loadBooleanResult(op_ == JSOp::StrictNe ? true : false);
  writer.returnFromIC();

  trackAttached("Compare.StrictDifferentTypes");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachInt32(ValOperandId lhsId,
                                                  ValOperandId rhsId) {
  if (!CanConvertToInt32ForToNumber(lhsVal_) ||
      !CanConvertToInt32ForToNumber(rhsVal_)) {
    return AttachDecision::NoAction;
  }

  // Strictly different types should have been handed by
  // tryAttachStrictDifferentTypes.
  MOZ_ASSERT_IF(op_ == JSOp::StrictEq || op_ == JSOp::StrictNe,
                lhsVal_.type() == rhsVal_.type());

  // Should have been handled by tryAttachAnyNullUndefined.
  MOZ_ASSERT_IF(lhsVal_.isNull() || rhsVal_.isNull(), !IsEqualityOp(op_));

  Int32OperandId lhsIntId = EmitGuardToInt32ForToNumber(writer, lhsId, lhsVal_);
  Int32OperandId rhsIntId = EmitGuardToInt32ForToNumber(writer, rhsId, rhsVal_);

  writer.compareInt32Result(op_, lhsIntId, rhsIntId);
  writer.returnFromIC();

  trackAttached("Compare.Int32");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachNumber(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  if (!CanConvertToDoubleForToNumber(lhsVal_) ||
      !CanConvertToDoubleForToNumber(rhsVal_)) {
    return AttachDecision::NoAction;
  }

  // Strictly different types should have been handed by
  // tryAttachStrictDifferentTypes.
  MOZ_ASSERT_IF(op_ == JSOp::StrictEq || op_ == JSOp::StrictNe,
                lhsVal_.type() == rhsVal_.type() ||
                    (lhsVal_.isNumber() && rhsVal_.isNumber()));

  // Should have been handled by tryAttachAnyNullUndefined.
  MOZ_ASSERT_IF(lhsVal_.isNullOrUndefined() || rhsVal_.isNullOrUndefined(),
                !IsEqualityOp(op_));

  NumberOperandId lhs = EmitGuardToDoubleForToNumber(writer, lhsId, lhsVal_);
  NumberOperandId rhs = EmitGuardToDoubleForToNumber(writer, rhsId, rhsVal_);
  writer.compareDoubleResult(op_, lhs, rhs);
  writer.returnFromIC();

  trackAttached("Compare.Number");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigInt(ValOperandId lhsId,
                                                   ValOperandId rhsId) {
  if (!lhsVal_.isBigInt() || !rhsVal_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  BigIntOperandId lhs = writer.guardToBigInt(lhsId);
  BigIntOperandId rhs = writer.guardToBigInt(rhsId);

  writer.compareBigIntResult(op_, lhs, rhs);
  writer.returnFromIC();

  trackAttached("Compare.BigInt");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachAnyNullUndefined(
    ValOperandId lhsId, ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  // Either RHS or LHS needs to be null/undefined.
  if (!lhsVal_.isNullOrUndefined() && !rhsVal_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  // We assume that the side with null/undefined is usually constant, in
  // code like `if (x === undefined) { x = {}; }`.
  // That is why we don't attach when both sides are undefined/null,
  // because we would basically need to decide by chance which side is
  // the likely constant.
  // The actual generated code however handles null/undefined of course.
  if (lhsVal_.isNullOrUndefined() && rhsVal_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  if (rhsVal_.isNullOrUndefined()) {
    if (rhsVal_.isNull()) {
      writer.guardIsNull(rhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ false, lhsId);
      trackAttached("Compare.AnyNull");
    } else {
      writer.guardIsUndefined(rhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ true, lhsId);
      trackAttached("Compare.AnyUndefined");
    }
  } else {
    if (lhsVal_.isNull()) {
      writer.guardIsNull(lhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ false, rhsId);
      trackAttached("Compare.NullAny");
    } else {
      writer.guardIsUndefined(lhsId);
      writer.compareNullUndefinedResult(op_, /* isUndefined */ true, rhsId);
      trackAttached("Compare.UndefinedAny");
    }
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

// Handle {null/undefined} x {null,undefined} equality comparisons
AttachDecision CompareIRGenerator::tryAttachNullUndefined(ValOperandId lhsId,
                                                          ValOperandId rhsId) {
  if (!lhsVal_.isNullOrUndefined() || !rhsVal_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  if (op_ == JSOp::Eq || op_ == JSOp::Ne) {
    writer.guardIsNullOrUndefined(lhsId);
    writer.guardIsNullOrUndefined(rhsId);
    // Sloppy equality means we actually only care about the op:
    writer.loadBooleanResult(op_ == JSOp::Eq);
    trackAttached("Compare.SloppyNullUndefined");
  } else {
    // Strict equality only hits this branch, and only in the
    // undef {!,=}==  undef and null {!,=}== null cases.
    // The other cases should have hit tryAttachStrictDifferentTypes.
    MOZ_ASSERT(lhsVal_.isNull() == rhsVal_.isNull());
    lhsVal_.isNull() ? writer.guardIsNull(lhsId)
                     : writer.guardIsUndefined(lhsId);
    rhsVal_.isNull() ? writer.guardIsNull(rhsId)
                     : writer.guardIsUndefined(rhsId);
    writer.loadBooleanResult(op_ == JSOp::StrictEq);
    trackAttached("Compare.StrictNullUndefinedEquality");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachStringNumber(ValOperandId lhsId,
                                                         ValOperandId rhsId) {
  // Ensure String x {Number, Boolean, Null, Undefined}
  if (!(lhsVal_.isString() && CanConvertToDoubleForToNumber(rhsVal_)) &&
      !(rhsVal_.isString() && CanConvertToDoubleForToNumber(lhsVal_))) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  auto createGuards = [&](const Value& v, ValOperandId vId) {
    if (v.isString()) {
      StringOperandId strId = writer.guardToString(vId);
      return writer.guardStringToNumber(strId);
    }
    return EmitGuardToDoubleForToNumber(writer, vId, v);
  };

  NumberOperandId lhsGuardedId = createGuards(lhsVal_, lhsId);
  NumberOperandId rhsGuardedId = createGuards(rhsVal_, rhsId);
  writer.compareDoubleResult(op_, lhsGuardedId, rhsGuardedId);
  writer.returnFromIC();

  trackAttached("Compare.StringNumber");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachPrimitiveSymbol(
    ValOperandId lhsId, ValOperandId rhsId) {
  MOZ_ASSERT(IsEqualityOp(op_));

  // The set of primitive cases we want to handle here (excluding null,
  // undefined, and symbol)
  auto isPrimitive = [](const Value& x) {
    return x.isString() || x.isBoolean() || x.isNumber() || x.isBigInt();
  };

  // Ensure Symbol x {String, Bool, Number, BigInt}.
  if (!(lhsVal_.isSymbol() && isPrimitive(rhsVal_)) &&
      !(rhsVal_.isSymbol() && isPrimitive(lhsVal_))) {
    return AttachDecision::NoAction;
  }

  auto guardPrimitive = [&](const Value& v, ValOperandId id) {
    MOZ_ASSERT(isPrimitive(v));
    if (v.isNumber()) {
      writer.guardIsNumber(id);
      return;
    }
    switch (v.extractNonDoubleType()) {
      case JSVAL_TYPE_STRING:
        writer.guardToString(id);
        return;
      case JSVAL_TYPE_BOOLEAN:
        writer.guardToBoolean(id);
        return;
      case JSVAL_TYPE_BIGINT:
        writer.guardToBigInt(id);
        return;
      default:
        MOZ_CRASH("unexpected type");
        return;
    }
  };

  if (lhsVal_.isSymbol()) {
    writer.guardToSymbol(lhsId);
    guardPrimitive(rhsVal_, rhsId);
  } else {
    guardPrimitive(lhsVal_, lhsId);
    writer.guardToSymbol(rhsId);
  }

  // Comparing a primitive with symbol will always be true for Ne/StrictNe, and
  // always be false for other compare ops.
  writer.loadBooleanResult(op_ == JSOp::Ne || op_ == JSOp::StrictNe);
  writer.returnFromIC();

  trackAttached("Compare.PrimitiveSymbol");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigIntInt32(ValOperandId lhsId,
                                                        ValOperandId rhsId) {
  // Ensure BigInt x {Int32, Boolean, Null}.
  if (!(lhsVal_.isBigInt() && CanConvertToInt32ForToNumber(rhsVal_)) &&
      !(rhsVal_.isBigInt() && CanConvertToInt32ForToNumber(lhsVal_))) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  if (lhsVal_.isBigInt()) {
    BigIntOperandId bigIntId = writer.guardToBigInt(lhsId);
    Int32OperandId intId = EmitGuardToInt32ForToNumber(writer, rhsId, rhsVal_);

    writer.compareBigIntInt32Result(op_, bigIntId, intId);
  } else {
    Int32OperandId intId = EmitGuardToInt32ForToNumber(writer, lhsId, lhsVal_);
    BigIntOperandId bigIntId = writer.guardToBigInt(rhsId);

    writer.compareBigIntInt32Result(ReverseCompareOp(op_), bigIntId, intId);
  }
  writer.returnFromIC();

  trackAttached("Compare.BigIntInt32");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigIntNumber(ValOperandId lhsId,
                                                         ValOperandId rhsId) {
  // Ensure BigInt x {Number, Undefined}.
  if (!(lhsVal_.isBigInt() && CanConvertToDoubleForToNumber(rhsVal_)) &&
      !(rhsVal_.isBigInt() && CanConvertToDoubleForToNumber(lhsVal_))) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  // Case should have been handled by tryAttachBigIntInt32.
  MOZ_ASSERT(!CanConvertToInt32ForToNumber(lhsVal_));
  MOZ_ASSERT(!CanConvertToInt32ForToNumber(rhsVal_));

  if (lhsVal_.isBigInt()) {
    BigIntOperandId bigIntId = writer.guardToBigInt(lhsId);
    NumberOperandId numId =
        EmitGuardToDoubleForToNumber(writer, rhsId, rhsVal_);

    writer.compareBigIntNumberResult(op_, bigIntId, numId);
  } else {
    NumberOperandId numId =
        EmitGuardToDoubleForToNumber(writer, lhsId, lhsVal_);
    BigIntOperandId bigIntId = writer.guardToBigInt(rhsId);

    writer.compareBigIntNumberResult(ReverseCompareOp(op_), bigIntId, numId);
  }
  writer.returnFromIC();

  trackAttached("Compare.BigIntNumber");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachBigIntString(ValOperandId lhsId,
                                                         ValOperandId rhsId) {
  // Ensure BigInt x String.
  if (!(lhsVal_.isBigInt() && rhsVal_.isString()) &&
      !(rhsVal_.isBigInt() && lhsVal_.isString())) {
    return AttachDecision::NoAction;
  }

  // Case should have been handled by tryAttachStrictDifferentTypes
  MOZ_ASSERT(op_ != JSOp::StrictEq && op_ != JSOp::StrictNe);

  if (lhsVal_.isBigInt()) {
    BigIntOperandId bigIntId = writer.guardToBigInt(lhsId);
    StringOperandId strId = writer.guardToString(rhsId);

    writer.compareBigIntStringResult(op_, bigIntId, strId);
  } else {
    StringOperandId strId = writer.guardToString(lhsId);
    BigIntOperandId bigIntId = writer.guardToBigInt(rhsId);

    writer.compareBigIntStringResult(ReverseCompareOp(op_), bigIntId, strId);
  }
  writer.returnFromIC();

  trackAttached("Compare.BigIntString");
  return AttachDecision::Attach;
}

AttachDecision CompareIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::Compare);
  MOZ_ASSERT(IsEqualityOp(op_) || IsRelationalOp(op_));

  AutoAssertNoPendingException aanpe(cx_);

  constexpr uint8_t lhsIndex = 0;
  constexpr uint8_t rhsIndex = 1;

  ValOperandId lhsId(writer.setInputOperandId(lhsIndex));
  ValOperandId rhsId(writer.setInputOperandId(rhsIndex));

  // For sloppy equality ops, there are cases this IC does not handle:
  // - {Object} x {String, Symbol, Bool, Number, BigInt}.
  //
  // For relational comparison ops, these cases aren't handled:
  // - Object x {String, Bool, Number, BigInt, Object, Null, Undefined}.
  // Note: |Symbol x any| always throws, so it doesn't need to be handled.
  //
  // (The above lists omits the equivalent case {B} x {A} when {A} x {B} is
  // already present.)

  if (IsEqualityOp(op_)) {
    TRY_ATTACH(tryAttachObject(lhsId, rhsId));
    TRY_ATTACH(tryAttachSymbol(lhsId, rhsId));

    // Handles any (non null or undefined) comparison with null/undefined.
    TRY_ATTACH(tryAttachAnyNullUndefined(lhsId, rhsId));

    // This covers -strict- equality/inequality using a type tag check, so
    // catches all different type pairs outside of Numbers, which cannot be
    // checked on tags alone.
    TRY_ATTACH(tryAttachStrictDifferentTypes(lhsId, rhsId));

    TRY_ATTACH(tryAttachNullUndefined(lhsId, rhsId));

    TRY_ATTACH(tryAttachPrimitiveSymbol(lhsId, rhsId));
  }

  // We want these to be last, to allow us to bypass the
  // strictly-different-types cases in the below attachment code
  TRY_ATTACH(tryAttachInt32(lhsId, rhsId));
  TRY_ATTACH(tryAttachNumber(lhsId, rhsId));
  TRY_ATTACH(tryAttachBigInt(lhsId, rhsId));
  TRY_ATTACH(tryAttachString(lhsId, rhsId));

  TRY_ATTACH(tryAttachStringNumber(lhsId, rhsId));

  TRY_ATTACH(tryAttachBigIntInt32(lhsId, rhsId));
  TRY_ATTACH(tryAttachBigIntNumber(lhsId, rhsId));
  TRY_ATTACH(tryAttachBigIntString(lhsId, rhsId));

  // Strict equality is always supported.
  MOZ_ASSERT(!IsStrictEqualityOp(op_));

  // Other operations are unsupported iff at least one operand is an object.
  MOZ_ASSERT(lhsVal_.isObject() || rhsVal_.isObject());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

void CompareIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("lhs", lhsVal_);
    sp.valueProperty("rhs", rhsVal_);
    sp.opcodeProperty("op", op_);
  }
#endif
}

ToBoolIRGenerator::ToBoolIRGenerator(JSContext* cx, HandleScript script,
                                     jsbytecode* pc, ICState state,
                                     HandleValue val)
    : IRGenerator(cx, script, pc, CacheKind::ToBool, state), val_(val) {}

void ToBoolIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision ToBoolIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  writer.setTypeData(TypeData(JSValueType(val_.type())));

  TRY_ATTACH(tryAttachBool());
  TRY_ATTACH(tryAttachInt32());
  TRY_ATTACH(tryAttachNumber());
  TRY_ATTACH(tryAttachString());
  TRY_ATTACH(tryAttachNullOrUndefined());
  TRY_ATTACH(tryAttachObject());
  TRY_ATTACH(tryAttachSymbol());
  TRY_ATTACH(tryAttachBigInt());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision ToBoolIRGenerator::tryAttachBool() {
  if (!val_.isBoolean()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardNonDoubleType(valId, ValueType::Boolean);
  writer.loadOperandResult(valId);
  writer.returnFromIC();
  trackAttached("ToBool.Bool");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachInt32() {
  if (!val_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardNonDoubleType(valId, ValueType::Int32);
  writer.loadInt32TruthyResult(valId);
  writer.returnFromIC();
  trackAttached("ToBool.Int32");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachNumber() {
  if (!val_.isNumber()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  NumberOperandId numId = writer.guardIsNumber(valId);
  writer.loadDoubleTruthyResult(numId);
  writer.returnFromIC();
  trackAttached("ToBool.Number");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachSymbol() {
  if (!val_.isSymbol()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardNonDoubleType(valId, ValueType::Symbol);
  writer.loadBooleanResult(true);
  writer.returnFromIC();
  trackAttached("ToBool.Symbol");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachString() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  StringOperandId strId = writer.guardToString(valId);
  writer.loadStringTruthyResult(strId);
  writer.returnFromIC();
  trackAttached("ToBool.String");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachNullOrUndefined() {
  if (!val_.isNullOrUndefined()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  writer.guardIsNullOrUndefined(valId);
  writer.loadBooleanResult(false);
  writer.returnFromIC();
  trackAttached("ToBool.NullOrUndefined");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachObject() {
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  ObjOperandId objId = writer.guardToObject(valId);
  writer.loadObjectTruthyResult(objId);
  writer.returnFromIC();
  trackAttached("ToBool.Object");
  return AttachDecision::Attach;
}

AttachDecision ToBoolIRGenerator::tryAttachBigInt() {
  if (!val_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  BigIntOperandId bigIntId = writer.guardToBigInt(valId);
  writer.loadBigIntTruthyResult(bigIntId);
  writer.returnFromIC();
  trackAttached("ToBool.BigInt");
  return AttachDecision::Attach;
}

LazyConstantIRGenerator::LazyConstantIRGenerator(JSContext* cx,
                                                 HandleScript script,
                                                 jsbytecode* pc, ICState state,
                                                 HandleValue val)
    : IRGenerator(cx, script, pc, CacheKind::LazyConstant, state), val_(val) {}

void LazyConstantIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision LazyConstantIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  writer.loadValueResult(val_);
  writer.returnFromIC();
  trackAttached("LazyConstant");
  return AttachDecision::Attach;
}

UnaryArithIRGenerator::UnaryArithIRGenerator(JSContext* cx, HandleScript script,
                                             jsbytecode* pc, ICState state,
                                             JSOp op, HandleValue val,
                                             HandleValue res)
    : IRGenerator(cx, script, pc, CacheKind::UnaryArith, state),
      op_(op),
      val_(val),
      res_(res) {}

void UnaryArithIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
    sp.valueProperty("res", res_);
  }
#endif
}

AttachDecision UnaryArithIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  TRY_ATTACH(tryAttachInt32());
  TRY_ATTACH(tryAttachNumber());
  TRY_ATTACH(tryAttachBitwise());
  TRY_ATTACH(tryAttachBigIntPtr());
  TRY_ATTACH(tryAttachBigInt());
  TRY_ATTACH(tryAttachStringInt32());
  TRY_ATTACH(tryAttachStringNumber());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision UnaryArithIRGenerator::tryAttachInt32() {
  if (op_ == JSOp::BitNot) {
    return AttachDecision::NoAction;
  }
  if (!CanConvertToInt32ForToNumber(val_) || !res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  Int32OperandId intId = EmitGuardToInt32ForToNumber(writer, valId, val_);
  switch (op_) {
    case JSOp::Pos:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.Int32Pos");
      break;
    case JSOp::Neg:
      writer.int32NegationResult(intId);
      trackAttached("UnaryArith.Int32Neg");
      break;
    case JSOp::Inc:
      writer.int32IncResult(intId);
      trackAttached("UnaryArith.Int32Inc");
      break;
    case JSOp::Dec:
      writer.int32DecResult(intId);
      trackAttached("UnaryArith.Int32Dec");
      break;
    case JSOp::ToNumeric:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.Int32ToNumeric");
      break;
    default:
      MOZ_CRASH("unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachNumber() {
  if (op_ == JSOp::BitNot) {
    return AttachDecision::NoAction;
  }
  if (!CanConvertToDoubleForToNumber(val_)) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isNumber());

  ValOperandId valId(writer.setInputOperandId(0));
  NumberOperandId numId = EmitGuardToDoubleForToNumber(writer, valId, val_);

  switch (op_) {
    case JSOp::Pos:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.DoublePos");
      break;
    case JSOp::Neg:
      writer.doubleNegationResult(numId);
      trackAttached("UnaryArith.DoubleNeg");
      break;
    case JSOp::Inc:
      writer.doubleIncResult(numId);
      trackAttached("UnaryArith.DoubleInc");
      break;
    case JSOp::Dec:
      writer.doubleDecResult(numId);
      trackAttached("UnaryArith.DoubleDec");
      break;
    case JSOp::ToNumeric:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.DoubleToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

static bool CanTruncateToInt32(const Value& val) {
  return val.isNumber() || val.isBoolean() || val.isNullOrUndefined() ||
         val.isString();
}

// Convert type into int32 for the bitwise/shift operands.
static Int32OperandId EmitTruncateToInt32Guard(CacheIRWriter& writer,
                                               ValOperandId id,
                                               const Value& val) {
  MOZ_ASSERT(CanTruncateToInt32(val));
  if (val.isInt32()) {
    return writer.guardToInt32(id);
  }
  if (val.isBoolean()) {
    return writer.guardBooleanToInt32(id);
  }
  if (val.isNullOrUndefined()) {
    writer.guardIsNullOrUndefined(id);
    return writer.loadInt32Constant(0);
  }
  NumberOperandId numId;
  if (val.isString()) {
    StringOperandId strId = writer.guardToString(id);
    numId = writer.guardStringToNumber(strId);
  } else {
    MOZ_ASSERT(val.isDouble());
    numId = writer.guardIsNumber(id);
  }
  return writer.truncateDoubleToUInt32(numId);
}

AttachDecision UnaryArithIRGenerator::tryAttachBitwise() {
  // Only bitwise operators.
  if (op_ != JSOp::BitNot) {
    return AttachDecision::NoAction;
  }

  // Check guard conditions
  if (!CanTruncateToInt32(val_)) {
    return AttachDecision::NoAction;
  }

  // Bitwise operators always produce Int32 values.
  MOZ_ASSERT(res_.isInt32());

  ValOperandId valId(writer.setInputOperandId(0));
  Int32OperandId intId = EmitTruncateToInt32Guard(writer, valId, val_);
  writer.int32NotResult(intId);
  trackAttached("UnaryArith.BitwiseBitNot");

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachBigInt() {
  if (!val_.isBigInt()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isBigInt());

  MOZ_ASSERT(op_ != JSOp::Pos,
             "Applying the unary + operator on BigInt values throws an error");

  ValOperandId valId(writer.setInputOperandId(0));
  BigIntOperandId bigIntId = writer.guardToBigInt(valId);
  switch (op_) {
    case JSOp::BitNot:
      writer.bigIntNotResult(bigIntId);
      trackAttached("UnaryArith.BigIntNot");
      break;
    case JSOp::Neg:
      writer.bigIntNegationResult(bigIntId);
      trackAttached("UnaryArith.BigIntNeg");
      break;
    case JSOp::Inc:
      writer.bigIntIncResult(bigIntId);
      trackAttached("UnaryArith.BigIntInc");
      break;
    case JSOp::Dec:
      writer.bigIntDecResult(bigIntId);
      trackAttached("UnaryArith.BigIntDec");
      break;
    case JSOp::ToNumeric:
      writer.loadBigIntResult(bigIntId);
      trackAttached("UnaryArith.BigIntToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachBigIntPtr() {
  if (!val_.isBigInt()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isBigInt());

  MOZ_ASSERT(op_ != JSOp::Pos,
             "Applying the unary + operator on BigInt values throws an error");

  switch (op_) {
    case JSOp::BitNot:
    case JSOp::Neg:
    case JSOp::Inc:
    case JSOp::Dec:
      break;
    case JSOp::ToNumeric:
      return AttachDecision::NoAction;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  intptr_t val;
  if (!BigInt::isIntPtr(val_.toBigInt(), &val)) {
    return AttachDecision::NoAction;
  }

  using CheckedIntPtr = mozilla::CheckedInt<intptr_t>;

  switch (op_) {
    case JSOp::BitNot: {
      // Bitwise operations always return an intptr-sized result.
      break;
    }
    case JSOp::Neg: {
      auto result = -CheckedIntPtr(val);
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Inc: {
      auto result = CheckedIntPtr(val) + intptr_t(1);
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Dec: {
      auto result = CheckedIntPtr(val) - intptr_t(1);
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    default:
      MOZ_CRASH("Unexpected OP");
  }

  ValOperandId valId(writer.setInputOperandId(0));
  BigIntOperandId bigIntId = writer.guardToBigInt(valId);
  IntPtrOperandId intPtrId = writer.bigIntToIntPtr(bigIntId);
  IntPtrOperandId resultId;
  switch (op_) {
    case JSOp::BitNot:
      resultId = writer.bigIntPtrNot(intPtrId);
      trackAttached("UnaryArith.BigIntPtrNot");
      break;
    case JSOp::Neg:
      resultId = writer.bigIntPtrNegation(intPtrId);
      trackAttached("UnaryArith.BigIntPtrNeg");
      break;
    case JSOp::Inc:
      resultId = writer.bigIntPtrInc(intPtrId);
      trackAttached("UnaryArith.BigIntPtrInc");
      break;
    case JSOp::Dec:
      resultId = writer.bigIntPtrDec(intPtrId);
      trackAttached("UnaryArith.BigIntPtrDec");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.intPtrToBigIntResult(resultId);
  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachStringInt32() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isNumber());

  // Case should have been handled by tryAttachBitwise.
  MOZ_ASSERT(op_ != JSOp::BitNot);

  if (!res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  StringOperandId stringId = writer.guardToString(valId);
  Int32OperandId intId = writer.guardStringToInt32(stringId);

  switch (op_) {
    case JSOp::Pos:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.StringInt32Pos");
      break;
    case JSOp::Neg:
      writer.int32NegationResult(intId);
      trackAttached("UnaryArith.StringInt32Neg");
      break;
    case JSOp::Inc:
      writer.int32IncResult(intId);
      trackAttached("UnaryArith.StringInt32Inc");
      break;
    case JSOp::Dec:
      writer.int32DecResult(intId);
      trackAttached("UnaryArith.StringInt32Dec");
      break;
    case JSOp::ToNumeric:
      writer.loadInt32Result(intId);
      trackAttached("UnaryArith.StringInt32ToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision UnaryArithIRGenerator::tryAttachStringNumber() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(res_.isNumber());

  // Case should have been handled by tryAttachBitwise.
  MOZ_ASSERT(op_ != JSOp::BitNot);

  ValOperandId valId(writer.setInputOperandId(0));
  StringOperandId stringId = writer.guardToString(valId);
  NumberOperandId numId = writer.guardStringToNumber(stringId);

  Int32OperandId truncatedId;
  switch (op_) {
    case JSOp::Pos:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.StringNumberPos");
      break;
    case JSOp::Neg:
      writer.doubleNegationResult(numId);
      trackAttached("UnaryArith.StringNumberNeg");
      break;
    case JSOp::Inc:
      writer.doubleIncResult(numId);
      trackAttached("UnaryArith.StringNumberInc");
      break;
    case JSOp::Dec:
      writer.doubleDecResult(numId);
      trackAttached("UnaryArith.StringNumberDec");
      break;
    case JSOp::ToNumeric:
      writer.loadDoubleResult(numId);
      trackAttached("UnaryArith.StringNumberToNumeric");
      break;
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

ToPropertyKeyIRGenerator::ToPropertyKeyIRGenerator(JSContext* cx,
                                                   HandleScript script,
                                                   jsbytecode* pc,
                                                   ICState state,
                                                   HandleValue val)
    : IRGenerator(cx, script, pc, CacheKind::ToPropertyKey, state), val_(val) {}

void ToPropertyKeyIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  TRY_ATTACH(tryAttachInt32());
  TRY_ATTACH(tryAttachNumber());
  TRY_ATTACH(tryAttachString());
  TRY_ATTACH(tryAttachSymbol());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachInt32() {
  if (!val_.isInt32()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  Int32OperandId intId = writer.guardToInt32(valId);
  writer.loadInt32Result(intId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.Int32");
  return AttachDecision::Attach;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachNumber() {
  if (!val_.isNumber()) {
    return AttachDecision::NoAction;
  }

  // We allow negative zero here because ToPropertyKey(-0.0) is 0.
  int32_t unused;
  if (!mozilla::NumberEqualsInt32(val_.toNumber(), &unused)) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  Int32OperandId intId = EmitGuardToInt32Index(writer, val_, valId);
  writer.loadInt32Result(intId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.Number");
  return AttachDecision::Attach;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachString() {
  if (!val_.isString()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  StringOperandId strId = writer.guardToString(valId);
  writer.loadStringResult(strId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.String");
  return AttachDecision::Attach;
}

AttachDecision ToPropertyKeyIRGenerator::tryAttachSymbol() {
  if (!val_.isSymbol()) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));

  SymbolOperandId strId = writer.guardToSymbol(valId);
  writer.loadSymbolResult(strId);
  writer.returnFromIC();

  trackAttached("ToPropertyKey.Symbol");
  return AttachDecision::Attach;
}

BinaryArithIRGenerator::BinaryArithIRGenerator(JSContext* cx,
                                               HandleScript script,
                                               jsbytecode* pc, ICState state,
                                               JSOp op, HandleValue lhs,
                                               HandleValue rhs, HandleValue res)
    : IRGenerator(cx, script, pc, CacheKind::BinaryArith, state),
      op_(op),
      lhs_(lhs),
      rhs_(rhs),
      res_(res) {}

void BinaryArithIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
    sp.valueProperty("rhs", rhs_);
    sp.valueProperty("lhs", lhs_);
  }
#endif
}

AttachDecision BinaryArithIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);
  // Arithmetic operations with Int32 operands
  TRY_ATTACH(tryAttachInt32());

  // Bitwise operations with Int32/Double/Boolean/Null/Undefined/String
  // operands.
  TRY_ATTACH(tryAttachBitwise());

  // Arithmetic operations with Double operands. This needs to come after
  // tryAttachInt32, as the guards overlap, and we'd prefer to attach the
  // more specialized Int32 IC if it is possible.
  TRY_ATTACH(tryAttachDouble());

  // String x {String,Number,Boolean,Null,Undefined}
  TRY_ATTACH(tryAttachStringConcat());

  // String x Object
  TRY_ATTACH(tryAttachStringObjectConcat());

  // Arithmetic operations or bitwise operations with intptr-sized BigInt
  // operands.
  TRY_ATTACH(tryAttachBigIntPtr());

  // Arithmetic operations or bitwise operations with BigInt operands
  TRY_ATTACH(tryAttachBigInt());

  // Arithmetic operations (without addition) with String x Int32.
  TRY_ATTACH(tryAttachStringInt32Arith());

  // Arithmetic operations (without addition) with String x Number. This needs
  // to come after tryAttachStringInt32Arith, as the guards overlap, and we'd
  // prefer to attach the more specialized Int32 IC if it is possible.
  TRY_ATTACH(tryAttachStringNumberArith());

  TRY_ATTACH(tryAttachDateArith());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

AttachDecision BinaryArithIRGenerator::tryAttachBitwise() {
  // Only bit-wise and shifts.
  if (op_ != JSOp::BitOr && op_ != JSOp::BitXor && op_ != JSOp::BitAnd &&
      op_ != JSOp::Lsh && op_ != JSOp::Rsh && op_ != JSOp::Ursh) {
    return AttachDecision::NoAction;
  }

  // Check guard conditions
  if (!CanTruncateToInt32(lhs_) || !CanTruncateToInt32(rhs_)) {
    return AttachDecision::NoAction;
  }

  // All ops, with the exception of Ursh, produce Int32 values.
  MOZ_ASSERT_IF(op_ != JSOp::Ursh, res_.isInt32());

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  Int32OperandId lhsIntId = EmitTruncateToInt32Guard(writer, lhsId, lhs_);
  Int32OperandId rhsIntId = EmitTruncateToInt32Guard(writer, rhsId, rhs_);

  switch (op_) {
    case JSOp::BitOr:
      writer.int32BitOrResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.BitwiseBitOr");
      break;
    case JSOp::BitXor:
      writer.int32BitXorResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.BitwiseBitXor");
      break;
    case JSOp::BitAnd:
      writer.int32BitAndResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.BitwiseBitAnd");
      break;
    case JSOp::Lsh:
      writer.int32LeftShiftResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.BitwiseLeftShift");
      break;
    case JSOp::Rsh:
      writer.int32RightShiftResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.BitwiseRightShift");
      break;
    case JSOp::Ursh:
      writer.int32URightShiftResult(lhsIntId, rhsIntId, res_.isDouble());
      trackAttached("BinaryArith.BitwiseUnsignedRightShift");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachBitwise");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachDouble() {
  // Check valid opcodes
  if (op_ != JSOp::Add && op_ != JSOp::Sub && op_ != JSOp::Mul &&
      op_ != JSOp::Div && op_ != JSOp::Mod && op_ != JSOp::Pow) {
    return AttachDecision::NoAction;
  }

  // Check guard conditions.
  if (!CanConvertToDoubleForToNumber(lhs_) ||
      !CanConvertToDoubleForToNumber(rhs_)) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  NumberOperandId lhs = EmitGuardToDoubleForToNumber(writer, lhsId, lhs_);
  NumberOperandId rhs = EmitGuardToDoubleForToNumber(writer, rhsId, rhs_);

  switch (op_) {
    case JSOp::Add:
      writer.doubleAddResult(lhs, rhs);
      trackAttached("BinaryArith.DoubleAdd");
      break;
    case JSOp::Sub:
      writer.doubleSubResult(lhs, rhs);
      trackAttached("BinaryArith.DoubleSub");
      break;
    case JSOp::Mul:
      writer.doubleMulResult(lhs, rhs);
      trackAttached("BinaryArith.DoubleMul");
      break;
    case JSOp::Div:
      writer.doubleDivResult(lhs, rhs);
      trackAttached("BinaryArith.DoubleDiv");
      break;
    case JSOp::Mod:
      writer.doubleModResult(lhs, rhs);
      trackAttached("BinaryArith.DoubleMod");
      break;
    case JSOp::Pow:
      writer.doublePowResult(lhs, rhs);
      trackAttached("BinaryArith.DoublePow");
      break;
    default:
      MOZ_CRASH("Unhandled Op");
  }
  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachInt32() {
  // Check guard conditions.
  if (!CanConvertToInt32ForToNumber(lhs_) ||
      !CanConvertToInt32ForToNumber(rhs_)) {
    return AttachDecision::NoAction;
  }

  // These ICs will failure() if result can't be encoded in an Int32:
  // If sample result is not Int32, we should avoid IC.
  if (!res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  if (op_ != JSOp::Add && op_ != JSOp::Sub && op_ != JSOp::Mul &&
      op_ != JSOp::Div && op_ != JSOp::Mod && op_ != JSOp::Pow) {
    return AttachDecision::NoAction;
  }

  if (op_ == JSOp::Pow && !CanAttachInt32Pow(lhs_, rhs_)) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  Int32OperandId lhsIntId = EmitGuardToInt32ForToNumber(writer, lhsId, lhs_);
  Int32OperandId rhsIntId = EmitGuardToInt32ForToNumber(writer, rhsId, rhs_);

  switch (op_) {
    case JSOp::Add:
      writer.int32AddResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32Add");
      break;
    case JSOp::Sub:
      writer.int32SubResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32Sub");
      break;
    case JSOp::Mul:
      writer.int32MulResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32Mul");
      break;
    case JSOp::Div:
      writer.int32DivResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32Div");
      break;
    case JSOp::Mod:
      writer.int32ModResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32Mod");
      break;
    case JSOp::Pow:
      writer.int32PowResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.Int32Pow");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachInt32");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringConcat() {
  // Only Addition
  if (op_ != JSOp::Add) {
    return AttachDecision::NoAction;
  }

  // One side must be a string, the other side a primitive value we can easily
  // convert to a string.
  if (!(lhs_.isString() && CanConvertToString(rhs_)) &&
      !(CanConvertToString(lhs_) && rhs_.isString())) {
    return AttachDecision::NoAction;
  }

  JitCode* code = cx_->zone()->jitZone()->ensureStubExists(
      cx_, JitZone::StubKind::StringConcat);
  if (!code) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  StringOperandId lhsStrId = emitToStringGuard(lhsId, lhs_);
  StringOperandId rhsStrId = emitToStringGuard(rhsId, rhs_);

  writer.concatStringsResult(lhsStrId, rhsStrId, code);

  writer.returnFromIC();
  trackAttached("BinaryArith.StringConcat");
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringObjectConcat() {
  // Only Addition
  if (op_ != JSOp::Add) {
    return AttachDecision::NoAction;
  }

  // Check Guards
  if (!(lhs_.isObject() && rhs_.isString()) &&
      !(lhs_.isString() && rhs_.isObject()))
    return AttachDecision::NoAction;

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  // This guard is actually overly tight, as the runtime
  // helper can handle lhs or rhs being a string, so long
  // as the other is an object.
  if (lhs_.isString()) {
    writer.guardToString(lhsId);
    writer.guardToObject(rhsId);
  } else {
    writer.guardToObject(lhsId);
    writer.guardToString(rhsId);
  }

  writer.callStringObjectConcatResult(lhsId, rhsId);

  writer.returnFromIC();
  trackAttached("BinaryArith.StringObjectConcat");
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachBigInt() {
  // Check Guards
  if (!lhs_.isBigInt() || !rhs_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  switch (op_) {
    case JSOp::Add:
    case JSOp::Sub:
    case JSOp::Mul:
    case JSOp::Div:
    case JSOp::Mod:
    case JSOp::Pow:
      // Arithmetic operations.
      break;

    case JSOp::BitOr:
    case JSOp::BitXor:
    case JSOp::BitAnd:
    case JSOp::Lsh:
    case JSOp::Rsh:
      // Bitwise operations.
      break;

    default:
      return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  BigIntOperandId lhsBigIntId = writer.guardToBigInt(lhsId);
  BigIntOperandId rhsBigIntId = writer.guardToBigInt(rhsId);

  switch (op_) {
    case JSOp::Add:
      writer.bigIntAddResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntAdd");
      break;
    case JSOp::Sub:
      writer.bigIntSubResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntSub");
      break;
    case JSOp::Mul:
      writer.bigIntMulResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntMul");
      break;
    case JSOp::Div:
      writer.bigIntDivResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntDiv");
      break;
    case JSOp::Mod:
      writer.bigIntModResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntMod");
      break;
    case JSOp::Pow:
      writer.bigIntPowResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntPow");
      break;
    case JSOp::BitOr:
      writer.bigIntBitOrResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntBitOr");
      break;
    case JSOp::BitXor:
      writer.bigIntBitXorResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntBitXor");
      break;
    case JSOp::BitAnd:
      writer.bigIntBitAndResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntBitAnd");
      break;
    case JSOp::Lsh:
      writer.bigIntLeftShiftResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntLeftShift");
      break;
    case JSOp::Rsh:
      writer.bigIntRightShiftResult(lhsBigIntId, rhsBigIntId);
      trackAttached("BinaryArith.BigIntRightShift");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachBigInt");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachBigIntPtr() {
  // Check Guards
  if (!lhs_.isBigInt() || !rhs_.isBigInt()) {
    return AttachDecision::NoAction;
  }

  switch (op_) {
    case JSOp::Add:
    case JSOp::Sub:
    case JSOp::Mul:
    case JSOp::Div:
    case JSOp::Mod:
    case JSOp::Pow:
      // Arithmetic operations.
      break;

    case JSOp::BitOr:
    case JSOp::BitXor:
    case JSOp::BitAnd:
    case JSOp::Lsh:
    case JSOp::Rsh:
      // Bitwise operations.
      break;

    default:
      return AttachDecision::NoAction;
  }

  intptr_t lhs;
  intptr_t rhs;
  if (!BigInt::isIntPtr(lhs_.toBigInt(), &lhs) ||
      !BigInt::isIntPtr(rhs_.toBigInt(), &rhs)) {
    return AttachDecision::NoAction;
  }

  using CheckedIntPtr = mozilla::CheckedInt<intptr_t>;

  switch (op_) {
    case JSOp::Add: {
      auto result = CheckedIntPtr(lhs) + rhs;
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Sub: {
      auto result = CheckedIntPtr(lhs) - rhs;
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Mul: {
      auto result = CheckedIntPtr(lhs) * rhs;
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Div: {
      auto result = CheckedIntPtr(lhs) / rhs;
      if (result.isValid()) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Mod: {
      // We can't use mozilla::CheckedInt here, because it disallows negative
      // inputs.
      if (rhs != 0) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Pow: {
      intptr_t result;
      if (BigInt::powIntPtr(lhs, rhs, &result)) {
        break;
      }
      return AttachDecision::NoAction;
    }
    case JSOp::BitOr:
    case JSOp::BitXor:
    case JSOp::BitAnd: {
      // Bitwise operations always return an intptr-sized result.
      break;
    }
    case JSOp::Lsh: {
      if (lhs == 0 || rhs <= 0) {
        break;
      }
      if (size_t(rhs) < BigInt::DigitBits) {
        intptr_t result = lhs << rhs;
        if ((result >> rhs) == lhs) {
          break;
        }
      }
      return AttachDecision::NoAction;
    }
    case JSOp::Rsh: {
      if (lhs == 0 || rhs >= 0) {
        break;
      }
      if (rhs > -intptr_t(BigInt::DigitBits)) {
        intptr_t result = lhs << -rhs;
        if ((result >> -rhs) == lhs) {
          break;
        }
      }
      return AttachDecision::NoAction;
    }
    default:
      MOZ_CRASH("Unexpected OP");
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  BigIntOperandId lhsBigIntId = writer.guardToBigInt(lhsId);
  BigIntOperandId rhsBigIntId = writer.guardToBigInt(rhsId);

  IntPtrOperandId lhsIntPtrId = writer.bigIntToIntPtr(lhsBigIntId);
  IntPtrOperandId rhsIntPtrId = writer.bigIntToIntPtr(rhsBigIntId);

  IntPtrOperandId resultId;
  switch (op_) {
    case JSOp::Add: {
      resultId = writer.bigIntPtrAdd(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.Add");
      break;
    }
    case JSOp::Sub: {
      resultId = writer.bigIntPtrSub(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.Sub");
      break;
    }
    case JSOp::Mul: {
      resultId = writer.bigIntPtrMul(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.Mul");
      break;
    }
    case JSOp::Div: {
      resultId = writer.bigIntPtrDiv(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.Div");
      break;
    }
    case JSOp::Mod: {
      resultId = writer.bigIntPtrMod(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.Mod");
      break;
    }
    case JSOp::Pow: {
      resultId = writer.bigIntPtrPow(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.Pow");
      break;
    }
    case JSOp::BitOr: {
      resultId = writer.bigIntPtrBitOr(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.BitOr");
      break;
    }
    case JSOp::BitXor: {
      resultId = writer.bigIntPtrBitXor(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.BitXor");
      break;
    }
    case JSOp::BitAnd: {
      resultId = writer.bigIntPtrBitAnd(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.BitAnd");
      break;
    }
    case JSOp::Lsh: {
      resultId = writer.bigIntPtrLeftShift(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.LeftShift");
      break;
    }
    case JSOp::Rsh: {
      resultId = writer.bigIntPtrRightShift(lhsIntPtrId, rhsIntPtrId);
      trackAttached("BinaryArith.BigIntPtr.RightShift");
      break;
    }
    default:
      MOZ_CRASH("Unexpected OP");
  }

  writer.intPtrToBigIntResult(resultId);
  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringInt32Arith() {
  // Check for either int32 x string or string x int32.
  if (!(lhs_.isInt32() && rhs_.isString()) &&
      !(lhs_.isString() && rhs_.isInt32())) {
    return AttachDecision::NoAction;
  }

  // The created ICs will fail if the result can't be encoded as as int32.
  // Thus skip this IC, if the sample result is not an int32.
  if (!res_.isInt32()) {
    return AttachDecision::NoAction;
  }

  // Must _not_ support Add, because it would be string concatenation instead.
  // For Pow we can't easily determine the CanAttachInt32Pow conditions so we
  // reject that as well.
  if (op_ != JSOp::Sub && op_ != JSOp::Mul && op_ != JSOp::Div &&
      op_ != JSOp::Mod) {
    return AttachDecision::NoAction;
  }

  // The string operand must be convertable to an int32 value.
  JSString* str = lhs_.isString() ? lhs_.toString() : rhs_.toString();

  double num;
  if (!StringToNumber(cx_, str, &num)) {
    cx_->recoverFromOutOfMemory();
    return AttachDecision::NoAction;
  }

  int32_t unused;
  if (!mozilla::NumberIsInt32(num, &unused)) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  auto guardToInt32 = [&](ValOperandId id, const Value& v) {
    if (v.isInt32()) {
      return writer.guardToInt32(id);
    }

    MOZ_ASSERT(v.isString());
    StringOperandId strId = writer.guardToString(id);
    return writer.guardStringToInt32(strId);
  };

  Int32OperandId lhsIntId = guardToInt32(lhsId, lhs_);
  Int32OperandId rhsIntId = guardToInt32(rhsId, rhs_);

  switch (op_) {
    case JSOp::Sub:
      writer.int32SubResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32Sub");
      break;
    case JSOp::Mul:
      writer.int32MulResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32Mul");
      break;
    case JSOp::Div:
      writer.int32DivResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32Div");
      break;
    case JSOp::Mod:
      writer.int32ModResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringInt32Mod");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachStringInt32Arith");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

AttachDecision BinaryArithIRGenerator::tryAttachStringNumberArith() {
  // Check for either number x string or string x number.
  if (!(lhs_.isNumber() && rhs_.isString()) &&
      !(lhs_.isString() && rhs_.isNumber())) {
    return AttachDecision::NoAction;
  }

  // Must _not_ support Add, because it would be string concatenation instead.
  if (op_ != JSOp::Sub && op_ != JSOp::Mul && op_ != JSOp::Div &&
      op_ != JSOp::Mod && op_ != JSOp::Pow) {
    return AttachDecision::NoAction;
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  auto guardToNumber = [&](ValOperandId id, const Value& v) {
    if (v.isNumber()) {
      return writer.guardIsNumber(id);
    }

    MOZ_ASSERT(v.isString());
    StringOperandId strId = writer.guardToString(id);
    return writer.guardStringToNumber(strId);
  };

  NumberOperandId lhsIntId = guardToNumber(lhsId, lhs_);
  NumberOperandId rhsIntId = guardToNumber(rhsId, rhs_);

  switch (op_) {
    case JSOp::Sub:
      writer.doubleSubResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringNumberSub");
      break;
    case JSOp::Mul:
      writer.doubleMulResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringNumberMul");
      break;
    case JSOp::Div:
      writer.doubleDivResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringNumberDiv");
      break;
    case JSOp::Mod:
      writer.doubleModResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringNumberMod");
      break;
    case JSOp::Pow:
      writer.doublePowResult(lhsIntId, rhsIntId);
      trackAttached("BinaryArith.StringNumberPow");
      break;
    default:
      MOZ_CRASH("Unhandled op in tryAttachStringNumberArith");
  }

  writer.returnFromIC();
  return AttachDecision::Attach;
}

static bool CheckPropertyIsNativeFunction(JSContext* cx, JSObject* obj,
                                          jsbytecode* pc, PropertyKey propKey,
                                          JSNative nativeFn, JSFunction** fn,
                                          NativeObject** holder, size_t* slot) {
  Maybe<PropertyInfo> prop;
  NativeGetPropKind kind =
      CanAttachNativeGetProp(cx, obj, propKey, holder, &prop, pc);
  if (kind != NativeGetPropKind::Slot) {
    return false;
  }

  MOZ_ASSERT(holder);
  MOZ_ASSERT(prop->isDataProperty());

  *slot = prop->slot();
  Value calleeVal = (*holder)->getSlot(*slot);
  if (!calleeVal.isObject() || !calleeVal.toObject().is<JSFunction>()) {
    return false;
  }

  if (!IsNativeFunction(calleeVal, nativeFn)) {
    return false;
  }

  *fn = &calleeVal.toObject().as<JSFunction>();
  return true;
}

static void EmitGuardPropertyIsNativeFunction(CacheIRWriter& writer,
                                              JSObject* dateObj, JSFunction* fn,
                                              NativeObject* holder, size_t slot,
                                              ObjOperandId objId) {
  MOZ_ASSERT(holder);
  ObjOperandId holderId =
      EmitReadSlotGuard(writer, &dateObj->as<NativeObject>(), holder, objId);
  ValOperandId calleeValId = EmitLoadSlot(writer, holder, holderId, slot);
  ObjOperandId calleeId = writer.guardToObject(calleeValId);
  writer.guardSpecificFunction(calleeId, fn);
}

AttachDecision BinaryArithIRGenerator::tryAttachDateArith() {
  // Only support subtractions
  if (op_ != JSOp::Sub) {
    return AttachDecision::NoAction;
  }

  // At least one side must be an object.
  if (!lhs_.isObject() && !rhs_.isObject()) {
    return AttachDecision::NoAction;
  }

  // Must be either object or numbers.
  if (!lhs_.isObject() && !lhs_.isNumber()) {
    return AttachDecision::NoAction;
  }

  if (!rhs_.isObject() && !rhs_.isNumber()) {
    return AttachDecision::NoAction;
  }

  // We can only operate on Date objects.
  if (lhs_.isObject() && !lhs_.toObject().is<DateObject>()) {
    return AttachDecision::NoAction;
  }

  if (rhs_.isObject() && !rhs_.toObject().is<DateObject>()) {
    return AttachDecision::NoAction;
  }

  JSFunction* lhsDateValueOfFn = nullptr;
  NativeObject* lhsDateValueOfHolder = nullptr;
  size_t lhsDateValueOfSlot;

  JSFunction* lhsToPrimitiveFn = nullptr;
  NativeObject* lhsToPrimitiveHolder = nullptr;
  size_t lhsToPrimitiveSlot;

  if (lhs_.isObject()) {
    if (!CheckPropertyIsNativeFunction(
            cx_, &lhs_.toObject(), pc_, NameToId(cx_->names().valueOf),
            date_valueOf, &lhsDateValueOfFn, &lhsDateValueOfHolder,
            &lhsDateValueOfSlot)) {
      return AttachDecision::NoAction;
    }

    if (!CheckPropertyIsNativeFunction(
            cx_, &lhs_.toObject(), pc_,
            PropertyKey::Symbol(cx_->wellKnownSymbols().toPrimitive),
            date_toPrimitive, &lhsToPrimitiveFn, &lhsToPrimitiveHolder,
            &lhsToPrimitiveSlot)) {
      return AttachDecision::NoAction;
    }
  }

  JSFunction* rhsDateValueOfFn = nullptr;
  NativeObject* rhsDateValueOfHolder = nullptr;
  size_t rhsDateValueOfSlot;

  JSFunction* rhsToPrimitiveFn = nullptr;
  NativeObject* rhsToPrimitiveHolder = nullptr;
  size_t rhsToPrimitiveSlot;

  if (rhs_.isObject()) {
    if (!CheckPropertyIsNativeFunction(
            cx_, &rhs_.toObject(), pc_, NameToId(cx_->names().valueOf),
            date_valueOf, &rhsDateValueOfFn, &rhsDateValueOfHolder,
            &rhsDateValueOfSlot)) {
      return AttachDecision::NoAction;
    }

    if (!CheckPropertyIsNativeFunction(
            cx_, &rhs_.toObject(), pc_,
            PropertyKey::Symbol(cx_->wellKnownSymbols().toPrimitive),
            date_toPrimitive, &rhsToPrimitiveFn, &rhsToPrimitiveHolder,
            &rhsToPrimitiveSlot)) {
      return AttachDecision::NoAction;
    }
  }

  ValOperandId lhsId(writer.setInputOperandId(0));
  ValOperandId rhsId(writer.setInputOperandId(1));

  NumberOperandId lhsNumId;
  NumberOperandId rhsNumId;

  if (lhs_.isObject()) {
    ObjOperandId lhsObjId = writer.guardToObject(lhsId);
    // The shape guard in EmitGuardPropertyIsNativeFunction ensures the object
    // is a Date object.
    EmitGuardPropertyIsNativeFunction(writer, &lhs_.toObject(),
                                      lhsDateValueOfFn, lhsDateValueOfHolder,
                                      lhsDateValueOfSlot, lhsObjId);
    EmitGuardPropertyIsNativeFunction(writer, &lhs_.toObject(),
                                      lhsToPrimitiveFn, lhsToPrimitiveHolder,
                                      lhsToPrimitiveSlot, lhsObjId);

    ValOperandId lhsUtcValId =
        writer.loadFixedSlot(lhsObjId, DateObject::offsetOfUTCTimeSlot());
    lhsNumId = writer.guardIsNumber(lhsUtcValId);
  } else {
    MOZ_ASSERT(lhs_.isNumber());
    lhsNumId = writer.guardIsNumber(lhsId);
  }

  if (rhs_.isObject()) {
    ObjOperandId rhsObjId = writer.guardToObject(rhsId);
    EmitGuardPropertyIsNativeFunction(writer, &rhs_.toObject(),
                                      rhsDateValueOfFn, rhsDateValueOfHolder,
                                      rhsDateValueOfSlot, rhsObjId);
    EmitGuardPropertyIsNativeFunction(writer, &rhs_.toObject(),
                                      rhsToPrimitiveFn, rhsToPrimitiveHolder,
                                      rhsToPrimitiveSlot, rhsObjId);

    ValOperandId rhsUtcValId =
        writer.loadFixedSlot(rhsObjId, DateObject::offsetOfUTCTimeSlot());
    rhsNumId = writer.guardIsNumber(rhsUtcValId);
  } else {
    MOZ_ASSERT(rhs_.isNumber());
    rhsNumId = writer.guardIsNumber(rhsId);
  }

  writer.doubleSubResult(lhsNumId, rhsNumId);
  trackAttached("BinaryArith.DateSub");

  writer.returnFromIC();
  return AttachDecision::Attach;
}

NewArrayIRGenerator::NewArrayIRGenerator(JSContext* cx, HandleScript script,
                                         jsbytecode* pc, ICState state, JSOp op,
                                         HandleObject templateObj,
                                         BaselineFrame* frame)
    : IRGenerator(cx, script, pc, CacheKind::NewArray, state, frame),
#ifdef JS_CACHEIR_SPEW
      op_(op),
#endif
      templateObject_(templateObj) {
  MOZ_ASSERT(templateObject_);
}

void NewArrayIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
  }
#endif
}

AttachDecision NewArrayIRGenerator::tryAttachArrayObject() {
  ArrayObject* arrayObj = &templateObject_->as<ArrayObject>();

  MOZ_ASSERT(arrayObj->numUsedFixedSlots() == 0);
  MOZ_ASSERT(arrayObj->numDynamicSlots() == 0);
  MOZ_ASSERT(!arrayObj->isSharedMemory());

  // The macro assembler only supports creating arrays with fixed elements.
  if (arrayObj->hasDynamicElements()) {
    return AttachDecision::NoAction;
  }

  // Stub doesn't support metadata builder
  if (cx_->realm()->hasAllocationMetadataBuilder()) {
    return AttachDecision::NoAction;
  }

  writer.guardNoAllocationMetadataBuilder(
      cx_->realm()->addressOfMetadataBuilder());

  gc::AllocSite* site = maybeCreateAllocSite();
  if (!site) {
    return AttachDecision::NoAction;
  }

  Shape* shape = arrayObj->shape();
  uint32_t length = arrayObj->length();

  writer.newArrayObjectResult(length, shape, site);

  writer.returnFromIC();

  trackAttached("NewArray.Object");
  return AttachDecision::Attach;
}

AttachDecision NewArrayIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachArrayObject());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

NewObjectIRGenerator::NewObjectIRGenerator(JSContext* cx, HandleScript script,
                                           jsbytecode* pc, ICState state,
                                           JSOp op, HandleObject templateObj,
                                           BaselineFrame* frame)
    : IRGenerator(cx, script, pc, CacheKind::NewObject, state, frame),
#ifdef JS_CACHEIR_SPEW
      op_(op),
#endif
      templateObject_(templateObj) {
  MOZ_ASSERT(templateObject_);
}

void NewObjectIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
  }
#endif
}

AttachDecision NewObjectIRGenerator::tryAttachPlainObject() {
  // Don't optimize allocations with too many dynamic slots. We use an unrolled
  // loop when initializing slots and this avoids generating too much code.
  static const uint32_t MaxDynamicSlotsToOptimize = 64;

  NativeObject* nativeObj = &templateObject_->as<NativeObject>();
  MOZ_ASSERT(nativeObj->is<PlainObject>());

  // Stub doesn't support metadata builder
  if (cx_->realm()->hasAllocationMetadataBuilder()) {
    return AttachDecision::NoAction;
  }

  if (nativeObj->numDynamicSlots() > MaxDynamicSlotsToOptimize) {
    return AttachDecision::NoAction;
  }

  MOZ_ASSERT(!nativeObj->hasDynamicElements());
  MOZ_ASSERT(!nativeObj->isSharedMemory());

  gc::AllocSite* site = maybeCreateAllocSite();
  if (!site) {
    return AttachDecision::NoAction;
  }

  uint32_t numFixedSlots = nativeObj->numUsedFixedSlots();
  uint32_t numDynamicSlots = nativeObj->numDynamicSlots();
  gc::AllocKind allocKind = nativeObj->allocKindForTenure();
  Shape* shape = nativeObj->shape();

  writer.guardNoAllocationMetadataBuilder(
      cx_->realm()->addressOfMetadataBuilder());
  writer.newPlainObjectResult(numFixedSlots, numDynamicSlots, allocKind, shape,
                              site);

  writer.returnFromIC();

  trackAttached("NewObject.PlainObject");
  return AttachDecision::Attach;
}

AttachDecision NewObjectIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachPlainObject());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

LambdaIRGenerator::LambdaIRGenerator(JSContext* cx, HandleScript script,
                                     jsbytecode* pc, ICState state, JSOp op,
                                     Handle<JSFunction*> canonicalFunction,
                                     BaselineFrame* frame)
    : IRGenerator(cx, script, pc, CacheKind::Lambda, state, frame),
#ifdef JS_CACHEIR_SPEW
      op_(op),
#endif
      canonicalFunction_(canonicalFunction) {
  MOZ_ASSERT(canonicalFunction_);
}

void LambdaIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.opcodeProperty("op", op_);
  }
#endif
}

AttachDecision LambdaIRGenerator::tryAttachFunctionClone() {
  // Don't optimize asm.js module functions.
  if (canonicalFunction_->isNativeFun()) {
    MOZ_ASSERT(IsAsmJSModule(canonicalFunction_));
    return AttachDecision::NoAction;
  }

  // Stub doesn't support metadata builder.
  if (cx_->realm()->hasAllocationMetadataBuilder()) {
    return AttachDecision::NoAction;
  }

  gc::AllocSite* site = maybeCreateAllocSite();
  if (!site) {
    return AttachDecision::NoAction;
  }

  writer.guardNoAllocationMetadataBuilder(
      cx_->realm()->addressOfMetadataBuilder());

  gc::AllocKind allocKind = canonicalFunction_->getAllocKind();
  MOZ_ASSERT(allocKind == gc::AllocKind::FUNCTION ||
             allocKind == gc::AllocKind::FUNCTION_EXTENDED);
  writer.newFunctionCloneResult(canonicalFunction_, allocKind, site);
  writer.returnFromIC();

  trackAttached("Lambda.FunctionClone");
  return AttachDecision::Attach;
}

AttachDecision LambdaIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachFunctionClone());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

CloseIterIRGenerator::CloseIterIRGenerator(JSContext* cx, HandleScript script,
                                           jsbytecode* pc, ICState state,
                                           HandleObject iter,
                                           CompletionKind kind)
    : IRGenerator(cx, script, pc, CacheKind::CloseIter, state),
      iter_(iter),
      kind_(kind) {}

void CloseIterIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("iter", ObjectValue(*iter_));
  }
#endif
}

AttachDecision CloseIterIRGenerator::tryAttachNoReturnMethod() {
  Maybe<PropertyInfo> prop;
  NativeObject* holder = nullptr;

  // If we can guard that the iterator does not have a |return| method,
  // then this CloseIter is a no-op.
  NativeGetPropKind kind = CanAttachNativeGetProp(
      cx_, iter_, NameToId(cx_->names().return_), &holder, &prop, pc_);
  if (kind != NativeGetPropKind::Missing) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(!holder);

  ObjOperandId objId(writer.setInputOperandId(0));

  EmitMissingPropGuard(writer, &iter_->as<NativeObject>(), objId);

  // There is no return method, so we don't have to do anything.
  writer.returnFromIC();

  trackAttached("CloseIter.NoReturn");
  return AttachDecision::Attach;
}

AttachDecision CloseIterIRGenerator::tryAttachScriptedReturn() {
  Maybe<PropertyInfo> prop;
  NativeObject* holder = nullptr;

  NativeGetPropKind kind = CanAttachNativeGetProp(
      cx_, iter_, NameToId(cx_->names().return_), &holder, &prop, pc_);
  if (kind != NativeGetPropKind::Slot) {
    return AttachDecision::NoAction;
  }
  MOZ_ASSERT(holder);
  MOZ_ASSERT(prop->isDataProperty());

  size_t slot = prop->slot();
  Value calleeVal = holder->getSlot(slot);
  if (!calleeVal.isObject() || !calleeVal.toObject().is<JSFunction>()) {
    return AttachDecision::NoAction;
  }

  JSFunction* callee = &calleeVal.toObject().as<JSFunction>();
  if (!callee->hasJitEntry()) {
    return AttachDecision::NoAction;
  }
  if (callee->isClassConstructor()) {
    return AttachDecision::NoAction;
  }

  // We don't support cross-realm |return|.
  if (cx_->realm() != callee->realm()) {
    return AttachDecision::NoAction;
  }

  ObjOperandId objId(writer.setInputOperandId(0));

  ObjOperandId holderId =
      EmitReadSlotGuard(writer, &iter_->as<NativeObject>(), holder, objId);

  ValOperandId calleeValId = EmitLoadSlot(writer, holder, holderId, slot);
  ObjOperandId calleeId = writer.guardToObject(calleeValId);
  emitCalleeGuard(calleeId, callee);

  writer.closeIterScriptedResult(objId, calleeId, kind_, callee->nargs());

  writer.returnFromIC();
  trackAttached("CloseIter.ScriptedReturn");

  return AttachDecision::Attach;
}

AttachDecision CloseIterIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachNoReturnMethod());
  TRY_ATTACH(tryAttachScriptedReturn());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

OptimizeGetIteratorIRGenerator::OptimizeGetIteratorIRGenerator(
    JSContext* cx, HandleScript script, jsbytecode* pc, ICState state,
    HandleValue value)
    : IRGenerator(cx, script, pc, CacheKind::OptimizeGetIterator, state),
      val_(value) {}

AttachDecision OptimizeGetIteratorIRGenerator::tryAttachStub() {
  MOZ_ASSERT(cacheKind_ == CacheKind::OptimizeGetIterator);

  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachArray());
  TRY_ATTACH(tryAttachNotOptimizable());

  MOZ_CRASH("Failed to attach unoptimizable case.");
}

AttachDecision OptimizeGetIteratorIRGenerator::tryAttachArray() {
  if (!isFirstStub_) {
    return AttachDecision::NoAction;
  }

  // The value must be a packed array.
  if (!val_.isObject()) {
    return AttachDecision::NoAction;
  }
  Rooted<JSObject*> obj(cx_, &val_.toObject());
  if (!IsArrayWithDefaultIterator<MustBePacked::Yes>(obj, cx_)) {
    return AttachDecision::NoAction;
  }

  ValOperandId valId(writer.setInputOperandId(0));
  ObjOperandId objId = writer.guardToObject(valId);

  // Guard the object is a packed array with Array.prototype as proto.
  MOZ_ASSERT(obj->is<ArrayObject>());
  writer.guardShape(objId, obj->shape());
  writer.guardArrayIsPacked(objId);

  // Guard on Array.prototype[@@iterator] and %ArrayIteratorPrototype%.next.
  // This fuse also ensures the prototype chain for Array Iterator is
  // maintained and that no return method is added.
  writer.guardFuse(RealmFuses::FuseIndex::OptimizeGetIteratorFuse);

  writer.loadBooleanResult(true);
  writer.returnFromIC();

  trackAttached("OptimizeGetIterator.Array.Fuse");
  return AttachDecision::Attach;
}

AttachDecision OptimizeGetIteratorIRGenerator::tryAttachNotOptimizable() {
  ValOperandId valId(writer.setInputOperandId(0));

  writer.loadBooleanResult(false);
  writer.returnFromIC();

  trackAttached("OptimizeGetIterator.NotOptimizable");
  return AttachDecision::Attach;
}

void OptimizeGetIteratorIRGenerator::trackAttached(const char* name) {
  stubName_ = name ? name : "NotAttached";

#ifdef JS_CACHEIR_SPEW
  if (const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name)) {
    sp.valueProperty("val", val_);
  }
#endif
}

GetImportIRGenerator::GetImportIRGenerator(JSContext* cx, HandleScript script,
                                           jsbytecode* pc, ICState state)
    : IRGenerator(cx, script, pc, CacheKind::GetImport, state) {}

void GetImportIRGenerator::trackAttached(const char* name) {
#ifdef JS_CACHEIR_SPEW
  const CacheIRSpewer::Guard& sp = CacheIRSpewer::Guard(*this, name);
  (void)sp;  // Silence unused warning
#endif
}

AttachDecision GetImportIRGenerator::tryAttachInitialized() {
  ModuleEnvironmentObject* env = GetModuleEnvironmentForScript(script_);
  MOZ_ASSERT(env);

  jsid id = NameToId(script_->getName(pc_));
  ModuleEnvironmentObject* holderEnv;
  Maybe<PropertyInfo> prop;
  MOZ_ALWAYS_TRUE(env->lookupImport(id, &holderEnv, &prop));

  // Imports are initialized by this point except in rare circumstances.
  if (holderEnv->getSlot(prop->slot()).isMagic(JS_UNINITIALIZED_LEXICAL)) {
    return AttachDecision::NoAction;
  }

  ObjOperandId holderEnvId = writer.loadObject(holderEnv);
  EmitLoadSlotResult(writer, holderEnvId, holderEnv, *prop);
  writer.returnFromIC();

  trackAttached("GetImport.Initialized");
  return AttachDecision::Attach;
}

AttachDecision GetImportIRGenerator::tryAttachStub() {
  AutoAssertNoPendingException aanpe(cx_);

  TRY_ATTACH(tryAttachInitialized());

  trackAttached(IRGenerator::NotAttached);
  return AttachDecision::NoAction;
}

#ifdef JS_SIMULATOR
bool js::jit::CallAnyNative(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSObject* calleeObj = &args.callee();

  MOZ_ASSERT(calleeObj->is<JSFunction>());
  auto* calleeFunc = &calleeObj->as<JSFunction>();
  MOZ_ASSERT(calleeFunc->isNativeWithoutJitEntry());

  JSNative native = calleeFunc->native();
  return native(cx, args.length(), args.base());
}

const void* js::jit::RedirectedCallAnyNative() {
  // The simulator requires native calls to be redirected to a
  // special swi instruction. If we are calling an arbitrary native
  // function, we can't wrap the real target ahead of time, so we
  // call a wrapper function (CallAnyNative) that calls the target
  // itself, and redirect that wrapper.
  JSNative target = CallAnyNative;
  void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, target);
  void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
  return redirected;
}
#endif
