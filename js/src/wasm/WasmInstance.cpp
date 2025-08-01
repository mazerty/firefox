/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmInstance-inl.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"

#include <algorithm>
#include <utility>

#include "jsmath.h"

#include "builtin/String.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "jit/AtomicOperations.h"
#include "jit/Disassemble.h"
#include "jit/JitCommon.h"
#include "jit/JitRuntime.h"
#include "jit/Registers.h"
#include "js/ForOfIterator.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Stack.h"                 // JS::NativeStackLimitMin
#include "util/StringBuilder.h"
#include "util/Text.h"
#include "util/Unicode.h"
#include "vm/ArrayBufferObject.h"
#include "vm/BigIntType.h"
#include "vm/BoundFunctionObject.h"
#include "vm/Compartment.h"
#include "vm/ErrorObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JitActivation.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmDebugFrame.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmHeuristics.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmPI.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmValue.h"

#include "gc/Marking-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"
#include "wasm/WasmGcObject-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedUint32;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

// Instance must be aligned at least as much as any of the integer, float,
// or SIMD values that we'd like to store in it.
static_assert(alignof(Instance) >=
              std::max(sizeof(Registers::RegisterContent),
                       sizeof(FloatRegisters::RegisterContent)));

// The globalArea must be aligned at least as much as an instance. This is
// guaranteed to be sufficient for all data types we care about, including
// SIMD values. See the above assertion.
static_assert(Instance::offsetOfData() % alignof(Instance) == 0);

// We want the memory base to be the first field, and accessible with no
// offset. This incidentally is also an assertion that there is no superclass
// with fields.
static_assert(Instance::offsetOfMemory0Base() == 0);

// We want instance fields that are commonly accessed by the JIT to have
// compact encodings. A limit of less than 128 bytes is chosen to fit within
// the signed 8-bit mod r/m x86 encoding.
static_assert(Instance::offsetOfLastCommonJitField() < 128);

//////////////////////////////////////////////////////////////////////////////
//
// Functions and invocation.

FuncDefInstanceData* Instance::funcDefInstanceData(uint32_t funcIndex) const {
  MOZ_ASSERT(funcIndex >= codeMeta().numFuncImports);
  uint32_t funcDefIndex = funcIndex - codeMeta().numFuncImports;
  FuncDefInstanceData* instanceData =
      (FuncDefInstanceData*)(data() + codeMeta().funcDefsOffsetStart);
  return &instanceData[funcDefIndex];
}

TypeDefInstanceData* Instance::typeDefInstanceData(uint32_t typeIndex) const {
  TypeDefInstanceData* instanceData =
      (TypeDefInstanceData*)(data() + codeMeta().typeDefsOffsetStart);
  return &instanceData[typeIndex];
}

const void* Instance::addressOfGlobalCell(const GlobalDesc& global) const {
  const void* cell = data() + global.offset();
  // Indirect globals store a pointer to their cell in the instance global
  // data. Dereference it to find the real cell.
  if (global.isIndirect()) {
    cell = *(const void**)cell;
  }
  return cell;
}

FuncImportInstanceData& Instance::funcImportInstanceData(uint32_t funcIndex) {
  MOZ_ASSERT(funcIndex < codeMeta().numFuncImports);
  FuncImportInstanceData* instanceData =
      (FuncImportInstanceData*)(data() + codeMeta().funcImportsOffsetStart);
  return instanceData[funcIndex];
}

FuncExportInstanceData& Instance::funcExportInstanceData(
    uint32_t funcExportIndex) {
  FuncExportInstanceData* instanceData =
      (FuncExportInstanceData*)(data() + codeMeta().funcExportsOffsetStart);
  return instanceData[funcExportIndex];
}

MemoryInstanceData& Instance::memoryInstanceData(uint32_t memoryIndex) const {
  MemoryInstanceData* instanceData =
      (MemoryInstanceData*)(data() + codeMeta().memoriesOffsetStart);
  return instanceData[memoryIndex];
}

TableInstanceData& Instance::tableInstanceData(uint32_t tableIndex) const {
  TableInstanceData* instanceData =
      (TableInstanceData*)(data() + codeMeta().tablesOffsetStart);
  return instanceData[tableIndex];
}

TagInstanceData& Instance::tagInstanceData(uint32_t tagIndex) const {
  TagInstanceData* instanceData =
      (TagInstanceData*)(data() + codeMeta().tagsOffsetStart);
  return instanceData[tagIndex];
}

static bool UnpackResults(JSContext* cx, const ValTypeVector& resultTypes,
                          const Maybe<char*> stackResultsArea, uint64_t* argv,
                          MutableHandleValue rval) {
  if (!stackResultsArea) {
    MOZ_ASSERT(resultTypes.length() <= 1);
    // Result is either one scalar value to unpack to a wasm value, or
    // an ignored value for a zero-valued function.
    if (resultTypes.length() == 1) {
      return ToWebAssemblyValue(cx, rval, resultTypes[0], argv, true);
    }
    return true;
  }

  MOZ_ASSERT(stackResultsArea.isSome());
  Rooted<ArrayObject*> array(cx);
  if (!IterableToArray(cx, rval, &array)) {
    return false;
  }

  if (resultTypes.length() != array->length()) {
    UniqueChars expected(JS_smprintf("%zu", resultTypes.length()));
    UniqueChars got(JS_smprintf("%u", array->length()));
    if (!expected || !got) {
      ReportOutOfMemory(cx);
      return false;
    }

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_WRONG_NUMBER_OF_VALUES, expected.get(),
                             got.get());
    return false;
  }

  DebugOnly<uint64_t> previousOffset = ~(uint64_t)0;

  ABIResultIter iter(ResultType::Vector(resultTypes));
  // The values are converted in the order they are pushed on the
  // abstract WebAssembly stack; switch to iterate in push order.
  while (!iter.done()) {
    iter.next();
  }
  DebugOnly<bool> seenRegisterResult = false;
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    MOZ_ASSERT(!seenRegisterResult);
    // Use rval as a scratch area to hold the extracted result.
    rval.set(array->getDenseElement(iter.index()));
    if (result.inRegister()) {
      // Currently, if a function type has results, there can be only
      // one register result.  If there is only one result, it is
      // returned as a scalar and not an iterable, so we don't get here.
      // If there are multiple results, we extract the register result
      // and set `argv[0]` set to the extracted result, to be returned by
      // register in the stub.  The register result follows any stack
      // results, so this preserves conversion order.
      if (!ToWebAssemblyValue(cx, rval, result.type(), argv, true)) {
        return false;
      }
      seenRegisterResult = true;
      continue;
    }
    uint32_t result_size = result.size();
    MOZ_ASSERT(result_size == 4 || result_size == 8);
#ifdef DEBUG
    if (previousOffset == ~(uint64_t)0) {
      previousOffset = (uint64_t)result.stackOffset();
    } else {
      MOZ_ASSERT(previousOffset - (uint64_t)result_size ==
                 (uint64_t)result.stackOffset());
      previousOffset -= (uint64_t)result_size;
    }
#endif
    char* loc = stackResultsArea.value() + result.stackOffset();
    if (!ToWebAssemblyValue(cx, rval, result.type(), loc, result_size == 8)) {
      return false;
    }
  }

  return true;
}

bool Instance::callImport(JSContext* cx, uint32_t funcImportIndex,
                          unsigned argc, uint64_t* argv) {
  AssertRealmUnchanged aru(cx);

  FuncImportInstanceData& instanceFuncImport =
      funcImportInstanceData(funcImportIndex);
  const FuncType& funcType = codeMeta().getFuncType(funcImportIndex);

  if (funcType.hasUnexposableArgOrRet()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  ArgTypeVector argTypes(funcType);
  size_t invokeArgsLength = argTypes.lengthWithoutStackResults();

  // If we're applying the Function.prototype.call.bind optimization, the
  // number of arguments to the target function is decreased by one to account
  // for the 'this' parameter we're passing
  bool isFunctionCallBind = instanceFuncImport.isFunctionCallBind;
  if (isFunctionCallBind) {
    // Guarded against in MaybeOptimizeFunctionCallBind.
    MOZ_ASSERT(invokeArgsLength != 0);
    invokeArgsLength -= 1;
  }

  RootedValue thisv(cx, UndefinedValue());
  InvokeArgs invokeArgs(cx);
  if (!invokeArgs.init(cx, invokeArgsLength)) {
    return false;
  }

  MOZ_ASSERT(argTypes.lengthWithStackResults() == argc);
  Maybe<char*> stackResultPointer;
  size_t lastBoxIndexPlusOne = 0;
  {
    JS::AutoAssertNoGC nogc;
    for (size_t i = 0; i < argc; i++) {
      const void* rawArgLoc = &argv[i];

      if (argTypes.isSyntheticStackResultPointerArg(i)) {
        stackResultPointer = Some(*(char**)rawArgLoc);
        continue;
      }

      size_t naturalIndex = argTypes.naturalIndex(i);
      ValType type = funcType.args()[naturalIndex];

      // Skip JS value conversion that may GC (as the argument array is not
      // rooted), and do that in a follow up loop.
      if (ToJSValueMayGC(type)) {
        lastBoxIndexPlusOne = i + 1;
        continue;
      }

      MutableHandleValue argValue =
          isFunctionCallBind
              ? ((naturalIndex == 0) ? &thisv : invokeArgs[naturalIndex - 1])
              : invokeArgs[naturalIndex];
      if (!ToJSValue(cx, rawArgLoc, type, argValue)) {
        return false;
      }
    }
  }

  // Visit arguments that need to perform allocation in a second loop
  // after the rest of arguments are converted.
  for (size_t i = 0; i < lastBoxIndexPlusOne; i++) {
    if (argTypes.isSyntheticStackResultPointerArg(i)) {
      continue;
    }

    size_t naturalIndex = argTypes.naturalIndex(i);
    ValType type = funcType.args()[naturalIndex];

    // Visit the arguments that could trigger a GC now.
    if (!ToJSValueMayGC(type)) {
      continue;
    }
    // All value types that require boxing when converted to a JS value are not
    // references.
    MOZ_ASSERT(!type.isRefRepr());

    // The conversions are safe here because source values are not references
    // and will not be moved. This may move the unrooted arguments in the array
    // but that's okay because those were handled in the above loop.
    const void* rawArgLoc = &argv[i];
    MutableHandleValue argValue =
        isFunctionCallBind
            ? ((naturalIndex == 0) ? &thisv : invokeArgs[naturalIndex - 1])
            : invokeArgs[naturalIndex];
    if (!ToJSValue(cx, rawArgLoc, type, argValue)) {
      return false;
    }
  }

  Rooted<JSObject*> importCallable(cx, instanceFuncImport.callable);
  MOZ_ASSERT(cx->realm() == importCallable->nonCCWRealm());

  RootedValue fval(cx, ObjectValue(*importCallable));
  RootedValue rval(cx);
  if (!Call(cx, fval, thisv, invokeArgs, &rval)) {
    return false;
  }

  if (!UnpackResults(cx, funcType.results(), stackResultPointer, argv, &rval)) {
    return false;
  }

  if (!JitOptions.enableWasmJitExit) {
    return true;
  }

  // JIT exits have not been updated to support the Function.prototype.call.bind
  // optimization.
  if (instanceFuncImport.isFunctionCallBind) {
    return true;
  }

  // The import may already have become optimized.
  const FuncImport& funcImport = code().funcImport(funcImportIndex);
  void* jitExitCode =
      code().sharedStubs().base() + funcImport.jitExitCodeOffset();
  if (instanceFuncImport.code == jitExitCode) {
    return true;
  }

  if (!importCallable->is<JSFunction>()) {
    return true;
  }

  // Test if the function is JIT compiled.
  if (!importCallable->as<JSFunction>().hasBytecode()) {
    return true;
  }

  JSScript* script = importCallable->as<JSFunction>().nonLazyScript();
  if (!script->hasJitScript()) {
    return true;
  }

  // Skip if the function does not have a signature that allows for a JIT exit.
  if (!funcType.canHaveJitExit()) {
    return true;
  }

  // Let's optimize it!

  instanceFuncImport.code = jitExitCode;
  return true;
}

/* static */ int32_t /* 0 to signal trap; 1 to signal OK */
Instance::callImport_general(Instance* instance, int32_t funcImportIndex,
                             int32_t argc, uint64_t* argv) {
  JSContext* cx = instance->cx();
#ifdef ENABLE_WASM_JSPI
  if (IsSuspendableStackActive(cx)) {
    struct ImportCallData {
      Instance* instance;
      int32_t funcImportIndex;
      int32_t argc;
      uint64_t* argv;
      static bool Call(ImportCallData* data) {
        Instance* instance = data->instance;
        JSContext* cx = instance->cx();
        return instance->callImport(cx, data->funcImportIndex, data->argc,
                                    data->argv);
      }
    } data = {instance, funcImportIndex, argc, argv};
    return CallOnMainStack(
        cx, reinterpret_cast<CallOnMainStackFn>(ImportCallData::Call), &data);
  }
#endif
  return instance->callImport(cx, funcImportIndex, argc, argv);
}

//////////////////////////////////////////////////////////////////////////////
//
// Atomic operations and shared memory.

template <typename ValT, typename PtrT>
static int32_t PerformWait(Instance* instance, uint32_t memoryIndex,
                           PtrT byteOffset, ValT value, int64_t timeout_ns) {
  JSContext* cx = instance->cx();

  if (!instance->memory(memoryIndex)->isShared()) {
    ReportTrapError(cx, JSMSG_WASM_NONSHARED_WAIT);
    return -1;
  }

  if (byteOffset & (sizeof(ValT) - 1)) {
    ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
    return -1;
  }

  if (byteOffset + sizeof(ValT) >
      instance->memory(memoryIndex)->volatileMemoryLength()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  mozilla::Maybe<mozilla::TimeDuration> timeout;
  if (timeout_ns >= 0) {
    timeout = mozilla::Some(
        mozilla::TimeDuration::FromMicroseconds(double(timeout_ns) / 1000));
  }

  MOZ_ASSERT(byteOffset <= SIZE_MAX, "Bounds check is broken");
  switch (atomics_wait_impl(cx, instance->sharedMemoryBuffer(memoryIndex),
                            size_t(byteOffset), value, timeout)) {
    case FutexThread::WaitResult::OK:
      return 0;
    case FutexThread::WaitResult::NotEqual:
      return 1;
    case FutexThread::WaitResult::TimedOut:
      return 2;
    case FutexThread::WaitResult::Error:
      return -1;
    default:
      MOZ_CRASH();
  }
}

/* static */ int32_t Instance::wait_i32_m32(Instance* instance,
                                            uint32_t byteOffset, int32_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI32M32.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

/* static */ int32_t Instance::wait_i32_m64(Instance* instance,
                                            uint64_t byteOffset, int32_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI32M64.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

/* static */ int32_t Instance::wait_i64_m32(Instance* instance,
                                            uint32_t byteOffset, int64_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI64M32.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

/* static */ int32_t Instance::wait_i64_m64(Instance* instance,
                                            uint64_t byteOffset, int64_t value,
                                            int64_t timeout_ns,
                                            uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWaitI64M64.failureMode == FailureMode::FailOnNegI32);
  return PerformWait(instance, memoryIndex, byteOffset, value, timeout_ns);
}

template <typename PtrT>
static int32_t PerformWake(Instance* instance, PtrT byteOffset, int32_t count,
                           uint32_t memoryIndex) {
  JSContext* cx = instance->cx();

  // The alignment guard is not in the wasm spec as of 2017-11-02, but is
  // considered likely to appear, as 4-byte alignment is required for WAKE by
  // the spec's validation algorithm.

  if (byteOffset & 3) {
    ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
    return -1;
  }

  if (byteOffset >= instance->memory(memoryIndex)->volatileMemoryLength()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  if (!instance->memory(memoryIndex)->isShared()) {
    return 0;
  }

  MOZ_ASSERT(byteOffset <= SIZE_MAX, "Bounds check is broken");
  int64_t woken;
  if (!atomics_notify_impl(cx, instance->sharedMemoryBuffer(memoryIndex),
                           size_t(byteOffset), int64_t(count), &woken)) {
    return -1;
  }

  if (woken > INT32_MAX) {
    ReportTrapError(cx, JSMSG_WASM_WAKE_OVERFLOW);
    return -1;
  }

  return int32_t(woken);
}

/* static */ int32_t Instance::wake_m32(Instance* instance, uint32_t byteOffset,
                                        int32_t count, uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWakeM32.failureMode == FailureMode::FailOnNegI32);
  return PerformWake(instance, byteOffset, count, memoryIndex);
}

/* static */ int32_t Instance::wake_m64(Instance* instance, uint64_t byteOffset,
                                        int32_t count, uint32_t memoryIndex) {
  MOZ_ASSERT(SASigWakeM32.failureMode == FailureMode::FailOnNegI32);
  return PerformWake(instance, byteOffset, count, memoryIndex);
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk memory operations.

/* static */ uint32_t Instance::memoryGrow_m32(Instance* instance,
                                               uint32_t delta,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemoryGrowM32.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(!instance->isAsmJS());

  JSContext* cx = instance->cx();
  Rooted<WasmMemoryObject*> memory(cx, instance->memory(memoryIndex));

  // It is safe to cast to uint32_t, as all limits have been checked inside
  // grow() and will not have been exceeded for a 32-bit memory.
  uint32_t ret = uint32_t(WasmMemoryObject::grow(memory, uint64_t(delta), cx));

  // If there has been a moving grow, this Instance should have been notified.
  MOZ_RELEASE_ASSERT(
      instance->memoryBase(memoryIndex) ==
      instance->memory(memoryIndex)->buffer().dataPointerEither());

  return ret;
}

/* static */ uint64_t Instance::memoryGrow_m64(Instance* instance,
                                               uint64_t delta,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemoryGrowM64.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(!instance->isAsmJS());

  JSContext* cx = instance->cx();
  Rooted<WasmMemoryObject*> memory(cx, instance->memory(memoryIndex));

  uint64_t ret = WasmMemoryObject::grow(memory, delta, cx);

  // If there has been a moving grow, this Instance should have been notified.
  MOZ_RELEASE_ASSERT(
      instance->memoryBase(memoryIndex) ==
      instance->memory(memoryIndex)->buffer().dataPointerEither());

  return ret;
}

/* static */ uint32_t Instance::memorySize_m32(Instance* instance,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemorySizeM32.failureMode == FailureMode::Infallible);

  // This invariant must hold when running Wasm code. Assert it here so we can
  // write tests for cross-realm calls.
  DebugOnly<JSContext*> cx = instance->cx();
  MOZ_ASSERT(cx->realm() == instance->realm());

  Pages pages = instance->memory(memoryIndex)->volatilePages();
#ifdef JS_64BIT
  // Ensure that the memory size is no more than 4GiB.
  MOZ_ASSERT(pages <= Pages(MaxMemory32PagesValidation));
#endif
  return uint32_t(pages.value());
}

/* static */ uint64_t Instance::memorySize_m64(Instance* instance,
                                               uint32_t memoryIndex) {
  MOZ_ASSERT(SASigMemorySizeM64.failureMode == FailureMode::Infallible);

  // This invariant must hold when running Wasm code. Assert it here so we can
  // write tests for cross-realm calls.
  DebugOnly<JSContext*> cx = instance->cx();
  MOZ_ASSERT(cx->realm() == instance->realm());

  Pages pages = instance->memory(memoryIndex)->volatilePages();
#ifdef JS_64BIT
  MOZ_ASSERT(pages <= Pages(MaxMemory64PagesValidation));
#endif
  return pages.value();
}

template <typename PointerT, typename CopyFuncT, typename IndexT>
inline int32_t WasmMemoryCopy(JSContext* cx, PointerT dstMemBase,
                              PointerT srcMemBase, size_t dstMemLen,
                              size_t srcMemLen, IndexT dstByteOffset,
                              IndexT srcByteOffset, IndexT len,
                              CopyFuncT memMove) {
  if (!MemoryBoundsCheck(dstByteOffset, len, dstMemLen) ||
      !MemoryBoundsCheck(srcByteOffset, len, srcMemLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  memMove(dstMemBase + uintptr_t(dstByteOffset),
          srcMemBase + uintptr_t(srcByteOffset), size_t(len));
  return 0;
}

template <typename I>
inline int32_t MemoryCopy(JSContext* cx, I dstByteOffset, I srcByteOffset,
                          I len, uint8_t* memBase) {
  const WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();
  return WasmMemoryCopy(cx, memBase, memBase, memLen, memLen, dstByteOffset,
                        srcByteOffset, len, memmove);
}

template <typename I>
inline int32_t MemoryCopyShared(JSContext* cx, I dstByteOffset, I srcByteOffset,
                                I len, uint8_t* memBase) {
  using RacyMemMove =
      void (*)(SharedMem<uint8_t*>, SharedMem<uint8_t*>, size_t);

  const WasmSharedArrayRawBuffer* rawBuf =
      WasmSharedArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->volatileByteLength();

  SharedMem<uint8_t*> sharedMemBase = SharedMem<uint8_t*>::shared(memBase);
  return WasmMemoryCopy<SharedMem<uint8_t*>, RacyMemMove>(
      cx, sharedMemBase, sharedMemBase, memLen, memLen, dstByteOffset,
      srcByteOffset, len, AtomicOperations::memmoveSafeWhenRacy);
}

/* static */ int32_t Instance::memCopy_m32(Instance* instance,
                                           uint32_t dstByteOffset,
                                           uint32_t srcByteOffset, uint32_t len,
                                           uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopyM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopy(cx, dstByteOffset, srcByteOffset, len, memBase);
}

/* static */ int32_t Instance::memCopyShared_m32(Instance* instance,
                                                 uint32_t dstByteOffset,
                                                 uint32_t srcByteOffset,
                                                 uint32_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopySharedM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopyShared(cx, dstByteOffset, srcByteOffset, len, memBase);
}

/* static */ int32_t Instance::memCopy_m64(Instance* instance,
                                           uint64_t dstByteOffset,
                                           uint64_t srcByteOffset, uint64_t len,
                                           uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopyM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopy(cx, dstByteOffset, srcByteOffset, len, memBase);
}

/* static */ int32_t Instance::memCopyShared_m64(Instance* instance,
                                                 uint64_t dstByteOffset,
                                                 uint64_t srcByteOffset,
                                                 uint64_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemCopySharedM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryCopyShared(cx, dstByteOffset, srcByteOffset, len, memBase);
}

// Dynamic dispatch to get the length of a memory given just the base and
// whether it is shared or not. This is only used for memCopy_any, where being
// slower is okay.
static inline size_t GetVolatileByteLength(uint8_t* memBase, bool isShared) {
  if (isShared) {
    return WasmSharedArrayRawBuffer::fromDataPtr(memBase)->volatileByteLength();
  }
  return WasmArrayRawBuffer::fromDataPtr(memBase)->byteLength();
}

/* static */ int32_t Instance::memCopy_any(Instance* instance,
                                           uint64_t dstByteOffset,
                                           uint64_t srcByteOffset, uint64_t len,
                                           uint32_t dstMemIndex,
                                           uint32_t srcMemIndex) {
  MOZ_ASSERT(SASigMemCopyAny.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  using RacyMemMove =
      void (*)(SharedMem<uint8_t*>, SharedMem<uint8_t*>, size_t);

  const MemoryInstanceData& dstMemory =
      instance->memoryInstanceData(dstMemIndex);
  const MemoryInstanceData& srcMemory =
      instance->memoryInstanceData(srcMemIndex);

  uint8_t* dstMemBase = dstMemory.base;
  uint8_t* srcMemBase = srcMemory.base;

  size_t dstMemLen = GetVolatileByteLength(dstMemBase, dstMemory.isShared);
  size_t srcMemLen = GetVolatileByteLength(srcMemBase, srcMemory.isShared);

  return WasmMemoryCopy<SharedMem<uint8_t*>, RacyMemMove>(
      cx, SharedMem<uint8_t*>::shared(dstMemBase),
      SharedMem<uint8_t*>::shared(srcMemBase), dstMemLen, srcMemLen,
      dstByteOffset, srcByteOffset, len, AtomicOperations::memmoveSafeWhenRacy);
}

template <typename T, typename F, typename I>
inline int32_t WasmMemoryFill(JSContext* cx, T memBase, size_t memLen,
                              I byteOffset, uint32_t value, I len, F memSet) {
  if (!MemoryBoundsCheck(byteOffset, len, memLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // The required write direction is upward, but that is not currently
  // observable as there are no fences nor any read/write protect operation.
  memSet(memBase + uintptr_t(byteOffset), int(value), size_t(len));
  return 0;
}

template <typename I>
inline int32_t MemoryFill(JSContext* cx, I byteOffset, uint32_t value, I len,
                          uint8_t* memBase) {
  const WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();
  return WasmMemoryFill(cx, memBase, memLen, byteOffset, value, len, memset);
}

template <typename I>
inline int32_t MemoryFillShared(JSContext* cx, I byteOffset, uint32_t value,
                                I len, uint8_t* memBase) {
  const WasmSharedArrayRawBuffer* rawBuf =
      WasmSharedArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->volatileByteLength();
  return WasmMemoryFill(cx, SharedMem<uint8_t*>::shared(memBase), memLen,
                        byteOffset, value, len,
                        AtomicOperations::memsetSafeWhenRacy);
}

/* static */ int32_t Instance::memFill_m32(Instance* instance,
                                           uint32_t byteOffset, uint32_t value,
                                           uint32_t len, uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFill(cx, byteOffset, value, len, memBase);
}

/* static */ int32_t Instance::memFillShared_m32(Instance* instance,
                                                 uint32_t byteOffset,
                                                 uint32_t value, uint32_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillSharedM32.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFillShared(cx, byteOffset, value, len, memBase);
}

/* static */ int32_t Instance::memFill_m64(Instance* instance,
                                           uint64_t byteOffset, uint32_t value,
                                           uint64_t len, uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFill(cx, byteOffset, value, len, memBase);
}

/* static */ int32_t Instance::memFillShared_m64(Instance* instance,
                                                 uint64_t byteOffset,
                                                 uint32_t value, uint64_t len,
                                                 uint8_t* memBase) {
  MOZ_ASSERT(SASigMemFillSharedM64.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  return MemoryFillShared(cx, byteOffset, value, len, memBase);
}

static bool BoundsCheckInit(uint32_t dstOffset, uint32_t srcOffset,
                            uint32_t len, size_t memLen, uint32_t segLen) {
  uint64_t dstOffsetLimit = uint64_t(dstOffset) + uint64_t(len);
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + uint64_t(len);

  return dstOffsetLimit > memLen || srcOffsetLimit > segLen;
}

static bool BoundsCheckInit(uint64_t dstOffset, uint32_t srcOffset,
                            uint32_t len, size_t memLen, uint32_t segLen) {
  uint64_t dstOffsetLimit = dstOffset + uint64_t(len);
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + uint64_t(len);

  return dstOffsetLimit < dstOffset || dstOffsetLimit > memLen ||
         srcOffsetLimit > segLen;
}

template <typename I>
static int32_t MemoryInit(JSContext* cx, Instance* instance,
                          uint32_t memoryIndex, I dstOffset, uint32_t srcOffset,
                          uint32_t len, const DataSegment* maybeSeg) {
  if (!maybeSeg) {
    if (len == 0 && srcOffset == 0) {
      return 0;
    }

    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  const DataSegment& seg = *maybeSeg;
  MOZ_RELEASE_ASSERT(!seg.active());

  const uint32_t segLen = seg.bytes.length();
  WasmMemoryObject* mem = instance->memory(memoryIndex);
  const size_t memLen = mem->volatileMemoryLength();

  // We are proposing to copy
  //
  //   seg.bytes.begin()[ srcOffset .. srcOffset + len - 1 ]
  // to
  //   memoryBase[ dstOffset .. dstOffset + len - 1 ]

  if (BoundsCheckInit(dstOffset, srcOffset, len, memLen, segLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // The required read/write direction is upward, but that is not currently
  // observable as there are no fences nor any read/write protect operation.
  SharedMem<uint8_t*> dataPtr = mem->buffer().dataPointerEither();
  if (mem->isShared()) {
    AtomicOperations::memcpySafeWhenRacy(
        dataPtr + uintptr_t(dstOffset), (uint8_t*)seg.bytes.begin() + srcOffset,
        len);
  } else {
    uint8_t* rawBuf = dataPtr.unwrap(/*Unshared*/);
    memcpy(rawBuf + uintptr_t(dstOffset),
           (const char*)seg.bytes.begin() + srcOffset, len);
  }
  return 0;
}

/* static */ int32_t Instance::memInit_m32(Instance* instance,
                                           uint32_t dstOffset,
                                           uint32_t srcOffset, uint32_t len,
                                           uint32_t segIndex,
                                           uint32_t memIndex) {
  MOZ_ASSERT(SASigMemInitM32.failureMode == FailureMode::FailOnNegI32);
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");

  JSContext* cx = instance->cx();
  return MemoryInit(cx, instance, memIndex, dstOffset, srcOffset, len,
                    instance->passiveDataSegments_[segIndex]);
}

/* static */ int32_t Instance::memInit_m64(Instance* instance,
                                           uint64_t dstOffset,
                                           uint32_t srcOffset, uint32_t len,
                                           uint32_t segIndex,
                                           uint32_t memIndex) {
  MOZ_ASSERT(SASigMemInitM64.failureMode == FailureMode::FailOnNegI32);
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");

  JSContext* cx = instance->cx();
  return MemoryInit(cx, instance, memIndex, dstOffset, srcOffset, len,
                    instance->passiveDataSegments_[segIndex]);
}

//////////////////////////////////////////////////////////////////////////////
//
// Bulk table operations.

/* static */ int32_t Instance::tableCopy(Instance* instance, uint32_t dstOffset,
                                         uint32_t srcOffset, uint32_t len,
                                         uint32_t dstTableIndex,
                                         uint32_t srcTableIndex) {
  MOZ_ASSERT(SASigTableCopy.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  const SharedTable& srcTable = instance->tables()[srcTableIndex];
  uint32_t srcTableLen = srcTable->length();

  const SharedTable& dstTable = instance->tables()[dstTableIndex];
  uint32_t dstTableLen = dstTable->length();

  // Bounds check and deal with arithmetic overflow.
  uint64_t dstOffsetLimit = uint64_t(dstOffset) + len;
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + len;

  if (dstOffsetLimit > dstTableLen || srcOffsetLimit > srcTableLen) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  bool isOOM = false;

  if (&srcTable == &dstTable && dstOffset > srcOffset) {
    for (uint32_t i = len; i > 0; i--) {
      if (!dstTable->copy(cx, *srcTable, dstOffset + (i - 1),
                          srcOffset + (i - 1))) {
        isOOM = true;
        break;
      }
    }
  } else if (&srcTable == &dstTable && dstOffset == srcOffset) {
    // No-op
  } else {
    for (uint32_t i = 0; i < len; i++) {
      if (!dstTable->copy(cx, *srcTable, dstOffset + i, srcOffset + i)) {
        isOOM = true;
        break;
      }
    }
  }

  if (isOOM) {
    return -1;
  }
  return 0;
}

#ifdef DEBUG
static bool AllSegmentsArePassive(const DataSegmentVector& vec) {
  for (const DataSegment* seg : vec) {
    if (seg->active()) {
      return false;
    }
  }
  return true;
}
#endif

bool Instance::initSegments(JSContext* cx,
                            const DataSegmentVector& dataSegments,
                            const ModuleElemSegmentVector& elemSegments) {
  MOZ_ASSERT_IF(codeMeta().memories.length() == 0,
                AllSegmentsArePassive(dataSegments));

  Rooted<WasmInstanceObject*> instanceObj(cx, object());

  // Write data/elem segments into memories/tables.

  for (const ModuleElemSegment& seg : elemSegments) {
    if (seg.active()) {
      RootedVal offsetVal(cx);
      if (!seg.offset().evaluate(cx, instanceObj, &offsetVal)) {
        return false;  // OOM
      }

      const wasm::Table* table = tables()[seg.tableIndex];
      uint64_t offset = table->addressType() == AddressType::I32
                            ? offsetVal.get().i32()
                            : offsetVal.get().i64();

      uint64_t tableLength = table->length();
      if (offset > tableLength || tableLength - offset < seg.numElements()) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                                 JSMSG_WASM_OUT_OF_BOUNDS);
        return false;
      }

      if (!initElems(cx, seg.tableIndex, seg, offset)) {
        return false;  // OOM
      }
    }
  }

  for (const DataSegment* seg : dataSegments) {
    if (!seg->active()) {
      continue;
    }

    Rooted<const WasmMemoryObject*> memoryObj(cx, memory(seg->memoryIndex));
    size_t memoryLength = memoryObj->volatileMemoryLength();
    uint8_t* memoryBase =
        memoryObj->buffer().dataPointerEither().unwrap(/* memcpy */);

    RootedVal offsetVal(cx);
    if (!seg->offset().evaluate(cx, instanceObj, &offsetVal)) {
      return false;  // OOM
    }
    uint64_t offset = memoryObj->addressType() == AddressType::I32
                          ? offsetVal.get().i32()
                          : offsetVal.get().i64();
    uint32_t count = seg->bytes.length();

    if (offset > memoryLength || memoryLength - offset < count) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                               JSMSG_WASM_OUT_OF_BOUNDS);
      return false;
    }
    memcpy(memoryBase + uintptr_t(offset), seg->bytes.begin(), count);
  }

  return true;
}

bool Instance::initElems(JSContext* cx, uint32_t tableIndex,
                         const ModuleElemSegment& seg, uint32_t dstOffset) {
  Table& table = *tables_[tableIndex];
  MOZ_ASSERT(dstOffset <= table.length());
  MOZ_ASSERT(seg.numElements() <= table.length() - dstOffset);

  if (seg.numElements() == 0) {
    return true;
  }

  if (table.isFunction() &&
      seg.encoding == ModuleElemSegment::Encoding::Indices) {
    // Initialize this table of functions without creating any intermediate
    // JSFunctions.
    bool ok = iterElemsFunctions(
        seg, [&](uint32_t i, void* code, Instance* instance) -> bool {
          table.setFuncRef(dstOffset + i, code, instance);
          return true;
        });
    if (!ok) {
      return false;
    }
  } else {
    bool ok = iterElemsAnyrefs(cx, seg, [&](uint32_t i, AnyRef ref) -> bool {
      table.setRef(dstOffset + i, ref);
      return true;
    });
    if (!ok) {
      return false;
    }
  }

  return true;
}

template <typename F>
bool Instance::iterElemsFunctions(const ModuleElemSegment& seg,
                                  const F& onFunc) {
  // In the future, we could theoretically get function data (instance + code
  // pointer) from segments with the expression encoding without creating
  // JSFunctions. But that is not how it works today. We can only bypass the
  // creation of JSFunctions for the index encoding.
  MOZ_ASSERT(seg.encoding == ModuleElemSegment::Encoding::Indices);

  if (seg.numElements() == 0) {
    return true;
  }

  const FuncImportVector& funcImports = code().funcImports();

  for (uint32_t i = 0; i < seg.numElements(); i++) {
    uint32_t elemFuncIndex = seg.elemIndices[i];

    if (elemFuncIndex < funcImports.length()) {
      FuncImportInstanceData& import = funcImportInstanceData(elemFuncIndex);
      MOZ_ASSERT(import.callable->isCallable());

      if (import.callable->is<JSFunction>()) {
        JSFunction* fun = &import.callable->as<JSFunction>();
        if (!codeMeta().funcImportsAreJS && fun->isWasm()) {
          // This element is a wasm function imported from another
          // instance. To preserve the === function identity required by
          // the JS embedding spec, we must get the imported function's
          // underlying CodeRange.funcCheckedCallEntry and Instance so that
          // future Table.get()s produce the same function object as was
          // imported.
          if (!onFunc(i, fun->wasmCheckedCallEntry(), &fun->wasmInstance())) {
            return false;
          }
          continue;
        }
      }
    }

    const CodeRange* codeRange;
    uint8_t* codeBase;
    code().funcCodeRange(elemFuncIndex, &codeRange, &codeBase);
    if (!onFunc(i, codeBase + codeRange->funcCheckedCallEntry(), this)) {
      return false;
    }
  }

  return true;
}

template <typename F>
bool Instance::iterElemsAnyrefs(JSContext* cx, const ModuleElemSegment& seg,
                                const F& onAnyRef) {
  if (seg.numElements() == 0) {
    return true;
  }

  switch (seg.encoding) {
    case ModuleElemSegment::Encoding::Indices: {
      // The only types of indices that exist right now are function indices, so
      // this code is specialized to functions.

      RootedFunction fun(cx);
      for (uint32_t i = 0; i < seg.numElements(); i++) {
        uint32_t funcIndex = seg.elemIndices[i];
        if (!getExportedFunction(cx, funcIndex, &fun) ||
            !onAnyRef(i, AnyRef::fromJSObject(*fun.get()))) {
          return false;
        }
      }
      break;
    }
    case ModuleElemSegment::Encoding::Expressions: {
      Rooted<WasmInstanceObject*> instanceObj(cx, object());
      const ModuleElemSegment::Expressions& exprs = seg.elemExpressions;

      UniqueChars error;
      // The offset is a dummy because the expression has already been
      // validated.
      Decoder d(exprs.exprBytes.begin(), exprs.exprBytes.end(), 0, &error);
      for (uint32_t i = 0; i < seg.numElements(); i++) {
        RootedVal result(cx);
        if (!InitExpr::decodeAndEvaluate(cx, instanceObj, d, seg.elemType,
                                         &result)) {
          MOZ_ASSERT(!error);  // The only possible failure should be OOM.
          return false;
        }
        // We would need to root this AnyRef if we were doing anything other
        // than storing it.
        AnyRef ref = result.get().ref();
        if (!onAnyRef(i, ref)) {
          return false;
        }
      }
      break;
    }
    default:
      MOZ_CRASH("unknown encoding type for element segment");
  }
  return true;
}

/* static */ int32_t Instance::tableInit(Instance* instance, uint32_t dstOffset,
                                         uint32_t srcOffset, uint32_t len,
                                         uint32_t segIndex,
                                         uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableInit.failureMode == FailureMode::FailOnNegI32);

  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");

  JSContext* cx = instance->cx();

  const InstanceElemSegment& seg = instance->passiveElemSegments_[segIndex];
  const uint32_t segLen = seg.length();

  Table& table = *instance->tables()[tableIndex];
  const uint32_t tableLen = table.length();

  // We are proposing to copy
  //
  //   seg[ srcOffset .. srcOffset + len - 1 ]
  // to
  //   tableBase[ dstOffset .. dstOffset + len - 1 ]

  // Bounds check and deal with arithmetic overflow.
  uint64_t dstOffsetLimit = uint64_t(dstOffset) + uint64_t(len);
  uint64_t srcOffsetLimit = uint64_t(srcOffset) + uint64_t(len);

  if (dstOffsetLimit > tableLen || srcOffsetLimit > segLen) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  for (size_t i = 0; i < len; i++) {
    table.setRef(dstOffset + i, seg[srcOffset + i]);
  }

  return 0;
}

/* static */ int32_t Instance::tableFill(Instance* instance, uint32_t start,
                                         void* value, uint32_t len,
                                         uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableFill.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  Table& table = *instance->tables()[tableIndex];

  // Bounds check and deal with arithmetic overflow.
  uint64_t offsetLimit = uint64_t(start) + uint64_t(len);

  if (offsetLimit > table.length()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  switch (table.repr()) {
    case TableRepr::Ref:
      table.fillAnyRef(start, len, AnyRef::fromCompiledCode(value));
      break;
    case TableRepr::Func:
      MOZ_RELEASE_ASSERT(!table.isAsmJS());
      table.fillFuncRef(start, len, FuncRef::fromCompiledCode(value), cx);
      break;
  }

  return 0;
}

template <typename I>
static bool WasmDiscardCheck(Instance* instance, I byteOffset, I byteLen,
                             size_t memLen, bool shared) {
  JSContext* cx = instance->cx();

  if (byteOffset % wasm::PageSize != 0 || byteLen % wasm::PageSize != 0) {
    ReportTrapError(cx, JSMSG_WASM_UNALIGNED_ACCESS);
    return false;
  }

  if (!MemoryBoundsCheck(byteOffset, byteLen, memLen)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  return true;
}

template <typename I>
static int32_t MemDiscardNotShared(Instance* instance, I byteOffset, I byteLen,
                                   uint8_t* memBase) {
  WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();

  if (!WasmDiscardCheck(instance, byteOffset, byteLen, memLen, false)) {
    return -1;
  }
  rawBuf->discard(byteOffset, byteLen);

  return 0;
}

template <typename I>
static int32_t MemDiscardShared(Instance* instance, I byteOffset, I byteLen,
                                uint8_t* memBase) {
  WasmSharedArrayRawBuffer* rawBuf =
      WasmSharedArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->volatileByteLength();

  if (!WasmDiscardCheck(instance, byteOffset, byteLen, memLen, true)) {
    return -1;
  }
  rawBuf->discard(byteOffset, byteLen);

  return 0;
}

/* static */ int32_t Instance::memDiscard_m32(Instance* instance,
                                              uint32_t byteOffset,
                                              uint32_t byteLen,
                                              uint8_t* memBase) {
  return MemDiscardNotShared(instance, byteOffset, byteLen, memBase);
}

/* static */ int32_t Instance::memDiscard_m64(Instance* instance,
                                              uint64_t byteOffset,
                                              uint64_t byteLen,
                                              uint8_t* memBase) {
  return MemDiscardNotShared(instance, byteOffset, byteLen, memBase);
}

/* static */ int32_t Instance::memDiscardShared_m32(Instance* instance,
                                                    uint32_t byteOffset,
                                                    uint32_t byteLen,
                                                    uint8_t* memBase) {
  return MemDiscardShared(instance, byteOffset, byteLen, memBase);
}

/* static */ int32_t Instance::memDiscardShared_m64(Instance* instance,
                                                    uint64_t byteOffset,
                                                    uint64_t byteLen,
                                                    uint8_t* memBase) {
  return MemDiscardShared(instance, byteOffset, byteLen, memBase);
}

/* static */ void* Instance::tableGet(Instance* instance, uint32_t address,
                                      uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableGet.failureMode == FailureMode::FailOnInvalidRef);

  JSContext* cx = instance->cx();
  const Table& table = *instance->tables()[tableIndex];
  if (address >= table.length()) {
    ReportTrapError(cx, JSMSG_WASM_TABLE_OUT_OF_BOUNDS);
    return AnyRef::invalid().forCompiledCode();
  }

  switch (table.repr()) {
    case TableRepr::Ref:
      return table.getAnyRef(address).forCompiledCode();
    case TableRepr::Func: {
      MOZ_RELEASE_ASSERT(!table.isAsmJS());
      RootedFunction fun(cx);
      if (!table.getFuncRef(cx, address, &fun)) {
        return AnyRef::invalid().forCompiledCode();
      }
      return FuncRef::fromJSFunction(fun).forCompiledCode();
    }
  }
  MOZ_CRASH("Should not happen");
}

/* static */ uint32_t Instance::tableGrow(Instance* instance, void* initValue,
                                          uint32_t delta, uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableGrow.failureMode == FailureMode::Infallible);

  JSContext* cx = instance->cx();
  RootedAnyRef ref(cx, AnyRef::fromCompiledCode(initValue));
  Table& table = *instance->tables()[tableIndex];

  uint32_t oldSize = table.grow(delta);

  if (oldSize != uint32_t(-1) && initValue != nullptr) {
    table.fillUninitialized(oldSize, delta, ref, cx);
  }

#ifdef DEBUG
  if (!table.elemType().isNullable()) {
    table.assertRangeNotNull(oldSize, delta);
  }
#endif  // DEBUG
  return oldSize;
}

/* static */ int32_t Instance::tableSet(Instance* instance, uint32_t address,
                                        void* value, uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableSet.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  Table& table = *instance->tables()[tableIndex];

  if (address >= table.length()) {
    ReportTrapError(cx, JSMSG_WASM_TABLE_OUT_OF_BOUNDS);
    return -1;
  }

  switch (table.repr()) {
    case TableRepr::Ref:
      table.setAnyRef(address, AnyRef::fromCompiledCode(value));
      break;
    case TableRepr::Func:
      MOZ_RELEASE_ASSERT(!table.isAsmJS());
      table.fillFuncRef(address, 1, FuncRef::fromCompiledCode(value), cx);
      break;
  }

  return 0;
}

/* static */ uint32_t Instance::tableSize(Instance* instance,
                                          uint32_t tableIndex) {
  MOZ_ASSERT(SASigTableSize.failureMode == FailureMode::Infallible);
  Table& table = *instance->tables()[tableIndex];
  return table.length();
}

/* static */ void* Instance::refFunc(Instance* instance, uint32_t funcIndex) {
  MOZ_ASSERT(SASigRefFunc.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();

  RootedFunction exportedFunc(cx);
  if (!instance->getExportedFunction(cx, funcIndex, &exportedFunc)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return AnyRef::invalid().forCompiledCode();
  }
  return FuncRef::fromJSFunction(exportedFunc.get()).forCompiledCode();
}

//////////////////////////////////////////////////////////////////////////////
//
// Segment management.

/* static */ int32_t Instance::elemDrop(Instance* instance, uint32_t segIndex) {
  MOZ_ASSERT(SASigElemDrop.failureMode == FailureMode::FailOnNegI32);

  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");

  instance->passiveElemSegments_[segIndex].clearAndFree();
  return 0;
}

/* static */ int32_t Instance::dataDrop(Instance* instance, uint32_t segIndex) {
  MOZ_ASSERT(SASigDataDrop.failureMode == FailureMode::FailOnNegI32);

  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");

  if (!instance->passiveDataSegments_[segIndex]) {
    return 0;
  }

  SharedDataSegment& segRefPtr = instance->passiveDataSegments_[segIndex];
  MOZ_RELEASE_ASSERT(!segRefPtr->active());

  // Drop this instance's reference to the DataSegment so it can be released.
  segRefPtr = nullptr;
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// AnyRef support.

/* static */ void Instance::postBarrierEdge(Instance* instance,
                                            AnyRef* location) {
  MOZ_ASSERT(SASigPostBarrierEdge.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(location);
  instance->storeBuffer_->putWasmAnyRef(location);
}

/* static */ void Instance::postBarrierEdgePrecise(Instance* instance,
                                                   AnyRef* location,
                                                   void* prev) {
  MOZ_ASSERT(SASigPostBarrierEdgePrecise.failureMode ==
             FailureMode::Infallible);
  MOZ_ASSERT(location);
  AnyRef next = *location;
  InternalBarrierMethods<AnyRef>::postBarrier(
      location, wasm::AnyRef::fromCompiledCode(prev), next);
}

/* static */ void Instance::postBarrierWholeCell(Instance* instance,
                                                 gc::Cell* object) {
  MOZ_ASSERT(SASigPostBarrierWholeCell.failureMode == FailureMode::Infallible);
  MOZ_ASSERT(object);
  instance->storeBuffer_->putWholeCell(object);
}

//////////////////////////////////////////////////////////////////////////////
//
// GC and exception handling support.

/* static */
template <bool ZeroFields>
void* Instance::structNewIL(Instance* instance, uint32_t typeDefIndex,
                            gc::AllocSite* allocSite) {
  MOZ_ASSERT((ZeroFields ? SASigStructNewIL_true : SASigStructNewIL_false)
                 .failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  TypeDefInstanceData* typeDefData =
      instance->typeDefInstanceData(typeDefIndex);
  // The new struct will be allocated in an initial heap as determined by
  // pretenuring logic as set up in `Instance::init`.
  return WasmStructObject::createStructIL<ZeroFields>(
      cx, typeDefData, allocSite, allocSite->initialHeap());
}

template void* Instance::structNewIL<true>(Instance* instance,
                                           uint32_t typeDefIndex,
                                           gc::AllocSite* allocSite);
template void* Instance::structNewIL<false>(Instance* instance,
                                            uint32_t typeDefIndex,
                                            gc::AllocSite* allocSite);

/* static */
template <bool ZeroFields>
void* Instance::structNewOOL(Instance* instance, uint32_t typeDefIndex,
                             gc::AllocSite* allocSite) {
  MOZ_ASSERT((ZeroFields ? SASigStructNewOOL_true : SASigStructNewOOL_false)
                 .failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  TypeDefInstanceData* typeDefData =
      instance->typeDefInstanceData(typeDefIndex);
  // The new struct will be allocated in an initial heap as determined by
  // pretenuring logic as set up in `Instance::init`.
  return WasmStructObject::createStructOOL<ZeroFields>(
      cx, typeDefData, allocSite, allocSite->initialHeap());
}

template void* Instance::structNewOOL<true>(Instance* instance,
                                            uint32_t typeDefIndex,
                                            gc::AllocSite* allocSite);
template void* Instance::structNewOOL<false>(Instance* instance,
                                             uint32_t typeDefIndex,
                                             gc::AllocSite* allocSite);

/* static */
template <bool ZeroFields>
void* Instance::arrayNew(Instance* instance, uint32_t numElements,
                         uint32_t typeDefIndex, gc::AllocSite* allocSite) {
  MOZ_ASSERT(
      (ZeroFields ? SASigArrayNew_true : SASigArrayNew_false).failureMode ==
      FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  TypeDefInstanceData* typeDefData =
      instance->typeDefInstanceData(typeDefIndex);
  // The new array will be allocated in an initial heap as determined by
  // pretenuring logic as set up in `Instance::init`.
  return WasmArrayObject::createArray<ZeroFields>(
      cx, typeDefData, allocSite, allocSite->initialHeap(), numElements);
}

template void* Instance::arrayNew<true>(Instance* instance,
                                        uint32_t numElements,
                                        uint32_t typeDefIndex,
                                        gc::AllocSite* allocSite);
template void* Instance::arrayNew<false>(Instance* instance,
                                         uint32_t numElements,
                                         uint32_t typeDefIndex,
                                         gc::AllocSite* allocSite);

// Copies from a data segment into a wasm GC array. Performs the necessary
// bounds checks, accounting for the array's element size. If this function
// returns false, it has already reported a trap error. Null arrays should
// be handled in the caller.
static bool ArrayCopyFromData(JSContext* cx, Handle<WasmArrayObject*> arrayObj,
                              uint32_t arrayIndex, const DataSegment* seg,
                              uint32_t segByteOffset, uint32_t numElements) {
  uint32_t elemSize = arrayObj->typeDef().arrayType().elementType().size();

  // Compute the number of bytes to copy, ensuring it's below 2^32.
  CheckedUint32 numBytesToCopy =
      CheckedUint32(numElements) * CheckedUint32(elemSize);
  if (!numBytesToCopy.isValid()) {
    // Because the request implies that 2^32 or more bytes are to be copied.
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Range-check the copy.  The obvious thing to do is to compute the offset
  // of the last byte to copy, but that would cause underflow in the
  // zero-length-and-zero-offset case.  Instead, compute that value plus one;
  // in other words the offset of the first byte *not* to copy.
  CheckedUint32 lastByteOffsetPlus1 =
      CheckedUint32(segByteOffset) + numBytesToCopy;

  CheckedUint32 numBytesAvailable(seg->bytes.length());
  if (!lastByteOffsetPlus1.isValid() || !numBytesAvailable.isValid() ||
      lastByteOffsetPlus1.value() > numBytesAvailable.value()) {
    // Because the last byte to copy doesn't exist inside `seg->bytes`.
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Range check the destination array.
  uint64_t dstNumElements = uint64_t(arrayObj->numElements_);
  if (uint64_t(arrayIndex) + uint64_t(numElements) > dstNumElements) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // This value is safe due to the previous range check on number of elements.
  // (We know the full result fits in the array, and we can't overflow uint64_t
  // since elemSize caps out at 16.)
  uint64_t dstByteOffset = uint64_t(arrayIndex) * uint64_t(elemSize);

  // Because `numBytesToCopy` is an in-range `CheckedUint32`, the cast to
  // `size_t` is safe even on a 32-bit target.
  if (numElements != 0) {
    memcpy(&arrayObj->data_[dstByteOffset], &seg->bytes[segByteOffset],
           size_t(numBytesToCopy.value()));
  }

  return true;
}

// Copies from an element segment into a wasm GC array. Performs the necessary
// bounds checks, accounting for the array's element size. If this function
// returns false, it has already reported a trap error.
static bool ArrayCopyFromElem(JSContext* cx, Handle<WasmArrayObject*> arrayObj,
                              uint32_t arrayIndex,
                              const InstanceElemSegment& seg,
                              uint32_t segOffset, uint32_t numElements) {
  // Range-check the copy. As in ArrayCopyFromData, compute the index of the
  // last element to copy, plus one.
  CheckedUint32 lastIndexPlus1 =
      CheckedUint32(segOffset) + CheckedUint32(numElements);
  CheckedUint32 numElemsAvailable(seg.length());
  if (!lastIndexPlus1.isValid() || !numElemsAvailable.isValid() ||
      lastIndexPlus1.value() > numElemsAvailable.value()) {
    // Because the last element to copy doesn't exist inside the segment.
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  // Range check the destination array.
  uint64_t dstNumElements = uint64_t(arrayObj->numElements_);
  if (uint64_t(arrayIndex) + uint64_t(numElements) > dstNumElements) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return false;
  }

  GCPtr<AnyRef>* dst = reinterpret_cast<GCPtr<AnyRef>*>(arrayObj->data_);
  for (uint32_t i = 0; i < numElements; i++) {
    dst[arrayIndex + i] = seg[segOffset + i];
  }

  return true;
}

// Creates an array (WasmArrayObject) containing `numElements` of type
// described by `typeDef`.  Initialises it with data copied from the data
// segment whose index is `segIndex`, starting at byte offset `segByteOffset`
// in the segment.  Traps if the segment doesn't hold enough bytes to fill the
// array.
/* static */ void* Instance::arrayNewData(
    Instance* instance, uint32_t segByteOffset, uint32_t numElements,
    uint32_t typeDefIndex, gc::AllocSite* allocSite, uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayNewData.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  TypeDefInstanceData* typeDefData =
      instance->typeDefInstanceData(typeDefIndex);

  // Check that the data segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");
  const DataSegment* seg = instance->passiveDataSegments_[segIndex];

  // `seg` will be nullptr if the segment has already been 'data.drop'ed
  // (either implicitly in the case of 'active' segments during instantiation,
  // or explicitly by the data.drop instruction.)  In that case we can
  // continue only if there's no need to copy any data out of it.
  if (!seg && (numElements != 0 || segByteOffset != 0)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return nullptr;
  }
  // At this point, if `seg` is null then `numElements` and `segByteOffset`
  // are both zero.

  Rooted<WasmArrayObject*> arrayObj(
      cx,
      WasmArrayObject::createArray<true>(
          cx, typeDefData, allocSite, allocSite->initialHeap(), numElements));
  if (!arrayObj) {
    // WasmArrayObject::createArray will have reported OOM.
    return nullptr;
  }
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!seg) {
    // A zero-length array was requested and has been created, so we're done.
    return arrayObj;
  }

  if (!ArrayCopyFromData(cx, arrayObj, 0, seg, segByteOffset, numElements)) {
    // Trap errors will be reported by ArrayCopyFromData.
    return nullptr;
  }

  return arrayObj;
}

// This is almost identical to ::arrayNewData, apart from the final part that
// actually copies the data.  It creates an array (WasmArrayObject)
// containing `numElements` of type described by `typeDef`.  Initialises it
// with data copied from the element segment whose index is `segIndex`,
// starting at element number `srcOffset` in the segment.  Traps if the
// segment doesn't hold enough elements to fill the array.
/* static */ void* Instance::arrayNewElem(
    Instance* instance, uint32_t srcOffset, uint32_t numElements,
    uint32_t typeDefIndex, gc::AllocSite* allocSite, uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayNewElem.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  TypeDefInstanceData* typeDefData =
      instance->typeDefInstanceData(typeDefIndex);

  // Check that the element segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");
  const InstanceElemSegment& seg = instance->passiveElemSegments_[segIndex];

  const TypeDef* typeDef = typeDefData->typeDef;

  // Any data coming from an element segment will be an AnyRef. Writes into
  // array memory are done with raw pointers, so we must ensure here that the
  // destination size is correct.
  MOZ_RELEASE_ASSERT(typeDef->arrayType().elementType().size() ==
                     sizeof(AnyRef));

  Rooted<WasmArrayObject*> arrayObj(
      cx,
      WasmArrayObject::createArray<true>(
          cx, typeDefData, allocSite, allocSite->initialHeap(), numElements));
  if (!arrayObj) {
    // WasmArrayObject::createArray will have reported OOM.
    return nullptr;
  }
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!ArrayCopyFromElem(cx, arrayObj, 0, seg, srcOffset, numElements)) {
    // Trap errors will be reported by ArrayCopyFromElems.
    return nullptr;
  }

  return arrayObj;
}

// Copies a range of the data segment `segIndex` into an array
// (WasmArrayObject), starting at offset `segByteOffset` in the data segment and
// index `index` in the array. `numElements` is the length of the copy in array
// elements, NOT bytes - the number of bytes will be computed based on the type
// of the array.
//
// Traps if accesses are out of bounds for either the data segment or the array,
// or if the array object is null.
/* static */ int32_t Instance::arrayInitData(Instance* instance, void* array,
                                             uint32_t index,
                                             uint32_t segByteOffset,
                                             uint32_t numElements,
                                             uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayInitData.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Check that the data segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveDataSegments_.length(),
                     "ensured by validation");
  const DataSegment* seg = instance->passiveDataSegments_[segIndex];

  // `seg` will be nullptr if the segment has already been 'data.drop'ed
  // (either implicitly in the case of 'active' segments during instantiation,
  // or explicitly by the data.drop instruction.)  In that case we can
  // continue only if there's no need to copy any data out of it.
  if (!seg && (numElements != 0 || segByteOffset != 0)) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }
  // At this point, if `seg` is null then `numElements` and `segByteOffset`
  // are both zero.

  // Trap if the array is null.
  if (!array) {
    ReportTrapError(cx, JSMSG_WASM_DEREF_NULL);
    return -1;
  }

  if (!seg) {
    // The segment was dropped, therefore a zero-length init was requested, so
    // we're done.
    return 0;
  }

  // Get hold of the array.
  Rooted<WasmArrayObject*> arrayObj(cx, static_cast<WasmArrayObject*>(array));
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!ArrayCopyFromData(cx, arrayObj, index, seg, segByteOffset,
                         numElements)) {
    // Trap errors will be reported by ArrayCopyFromData.
    return -1;
  }

  return 0;
}

// Copies a range of the element segment `segIndex` into an array
// (WasmArrayObject), starting at offset `segOffset` in the elem segment and
// index `index` in the array. `numElements` is the length of the copy.
//
// Traps if accesses are out of bounds for either the elem segment or the array,
// or if the array object is null.
/* static */ int32_t Instance::arrayInitElem(Instance* instance, void* array,
                                             uint32_t index, uint32_t segOffset,
                                             uint32_t numElements,
                                             uint32_t typeDefIndex,
                                             uint32_t segIndex) {
  MOZ_ASSERT(SASigArrayInitElem.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  // Check that the element segment is valid for use.
  MOZ_RELEASE_ASSERT(size_t(segIndex) < instance->passiveElemSegments_.length(),
                     "ensured by validation");
  const InstanceElemSegment& seg = instance->passiveElemSegments_[segIndex];

  // Trap if the array is null.
  if (!array) {
    ReportTrapError(cx, JSMSG_WASM_DEREF_NULL);
    return -1;
  }

  // Any data coming from an element segment will be an AnyRef. Writes into
  // array memory are done with raw pointers, so we must ensure here that the
  // destination size is correct.
  DebugOnly<const TypeDef*> typeDef =
      &instance->codeMeta().types->type(typeDefIndex);
  MOZ_ASSERT(typeDef->arrayType().elementType().size() == sizeof(AnyRef));

  // Get hold of the array.
  Rooted<WasmArrayObject*> arrayObj(cx, static_cast<WasmArrayObject*>(array));
  MOZ_RELEASE_ASSERT(arrayObj->is<WasmArrayObject>());

  if (!ArrayCopyFromElem(cx, arrayObj, index, seg, segOffset, numElements)) {
    // Trap errors will be reported by ArrayCopyFromElems.
    return -1;
  }

  return 0;
}

// Copies range of elements between two arrays.
//
// Traps if accesses are out of bounds for the arrays, or either array
// object is null.
//
// This function is only used by baseline, Ion emits inline code using
// WasmArrayMemMove and WasmArrayRefsMove builtins instead.
/* static */ int32_t Instance::arrayCopy(Instance* instance, void* dstArray,
                                         uint32_t dstIndex, void* srcArray,
                                         uint32_t srcIndex,
                                         uint32_t numElements,
                                         uint32_t elementSize) {
  MOZ_ASSERT(SASigArrayCopy.failureMode == FailureMode::FailOnNegI32);

  // At the entry point, `elementSize` may be negative to indicate
  // reftyped-ness of array elements.  That is done in order to avoid having
  // to pass yet another (boolean) parameter here.

  // "traps if either array is null"
  if (!srcArray || !dstArray) {
    ReportTrapError(instance->cx(), JSMSG_WASM_DEREF_NULL);
    return -1;
  }

  bool elemsAreRefTyped = false;
  if (int32_t(elementSize) < 0) {
    elemsAreRefTyped = true;
    elementSize = uint32_t(-int32_t(elementSize));
  }
  MOZ_ASSERT(elementSize >= 1 && elementSize <= 16);

  // Get hold of the two arrays.
  WasmArrayObject* dstArrayObj = static_cast<WasmArrayObject*>(dstArray);
  WasmArrayObject* srcArrayObj = static_cast<WasmArrayObject*>(srcArray);
  MOZ_ASSERT(dstArrayObj->is<WasmArrayObject>() &&
             srcArrayObj->is<WasmArrayObject>());

  // If WasmArrayObject::numElements() is changed to return 64 bits, the
  // following checking logic will be incorrect.
  STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32;

  // "traps if destination + length > len(array1)"
  uint64_t dstNumElements = uint64_t(dstArrayObj->numElements_);
  if (uint64_t(dstIndex) + uint64_t(numElements) > dstNumElements) {
    // Potential GC hazard: srcArrayObj and dstArrayObj are invalidated by
    // reporting an error, do no use them after this point.
    ReportTrapError(instance->cx(), JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  // "traps if source + length > len(array2)"
  uint64_t srcNumElements = uint64_t(srcArrayObj->numElements_);
  if (uint64_t(srcIndex) + uint64_t(numElements) > srcNumElements) {
    // Potential GC hazard: srcArrayObj and dstArrayObj are invalidated by
    // reporting an error, do no use them after this point.
    ReportTrapError(instance->cx(), JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  if (numElements == 0) {
    // Early exit if there's no work to do.
    return 0;
  }

  // Actually do the copy, taking care to handle cases where the src and dst
  // areas overlap.
  uint8_t* srcBase = srcArrayObj->data_;
  uint8_t* dstBase = dstArrayObj->data_;
  srcBase += size_t(srcIndex) * size_t(elementSize);
  dstBase += size_t(dstIndex) * size_t(elementSize);
  if (srcBase == dstBase) {
    // Early exit if there's no work to do.
    return 0;
  }

  if (!elemsAreRefTyped) {
    // Hand off to memmove, which is presumably highly optimized.
    memmove(dstBase, srcBase, size_t(numElements) * size_t(elementSize));
    return 0;
  }

  GCPtr<AnyRef>* dst = (GCPtr<AnyRef>*)dstBase;
  AnyRef* src = (AnyRef*)srcBase;
  // The std::copy performs GCPtr::set() operation under the hood.
  if (uintptr_t(dstBase) < uintptr_t(srcBase)) {
    std::copy(src, src + numElements, dst);
  } else {
    std::copy_backward(src, src + numElements, dst + numElements);
  }
  return 0;
}

/* static */ void* Instance::exceptionNew(Instance* instance, void* tagArg) {
  MOZ_ASSERT(SASigExceptionNew.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  AnyRef tag = AnyRef::fromCompiledCode(tagArg);
  Rooted<WasmTagObject*> tagObj(cx, &tag.toJSObject().as<WasmTagObject>());
  RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmException));
  RootedObject stack(cx, nullptr);

  // We don't create the .stack property by default, unless the pref is set for
  // debugging.
  if (JS::Prefs::wasm_exception_force_stack_trace() &&
      !CaptureStack(cx, &stack)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  // An OOM will result in null which will be caught on the wasm side.
  return AnyRef::fromJSObjectOrNull(
             WasmExceptionObject::create(cx, tagObj, stack, proto))
      .forCompiledCode();
}

/* static */ int32_t Instance::throwException(Instance* instance,
                                              void* exceptionArg) {
  MOZ_ASSERT(SASigThrowException.failureMode == FailureMode::FailOnNegI32);

  JSContext* cx = instance->cx();
  AnyRef exception = AnyRef::fromCompiledCode(exceptionArg);
  RootedValue exnVal(cx, exception.toJSValue());
  cx->setPendingException(exnVal, nullptr);

  // By always returning -1, we trigger a wasmTrap(Trap::ThrowReported),
  // and use that to trigger the stack walking for this exception.
  return -1;
}

/* static */ int32_t Instance::intrI8VecMul(Instance* instance, uint32_t dest,
                                            uint32_t src1, uint32_t src2,
                                            uint32_t len, uint8_t* memBase) {
  MOZ_ASSERT(SASigIntrI8VecMul.failureMode == FailureMode::FailOnNegI32);
  MOZ_ASSERT(SASigIntrI8VecMul.failureTrap == Trap::OutOfBounds);
  AutoUnsafeCallWithABI unsafe;

  const WasmArrayRawBuffer* rawBuf = WasmArrayRawBuffer::fromDataPtr(memBase);
  size_t memLen = rawBuf->byteLength();

  // Bounds check and deal with arithmetic overflow.
  uint64_t destLimit = uint64_t(dest) + uint64_t(len);
  uint64_t src1Limit = uint64_t(src1) + uint64_t(len);
  uint64_t src2Limit = uint64_t(src2) + uint64_t(len);
  if (destLimit > memLen || src1Limit > memLen || src2Limit > memLen) {
    return -1;
  }

  // Basic dot product
  uint8_t* destPtr = &memBase[dest];
  uint8_t* src1Ptr = &memBase[src1];
  uint8_t* src2Ptr = &memBase[src2];
  while (len > 0) {
    *destPtr = (*src1Ptr) * (*src2Ptr);

    destPtr++;
    src1Ptr++;
    src2Ptr++;
    len--;
  }
  return 0;
}

template <bool isMutable>
static WasmArrayObject* UncheckedCastToArrayI16(HandleAnyRef ref) {
  JSObject& object = ref.toJSObject();
  WasmArrayObject& array = object.as<WasmArrayObject>();
  DebugOnly<const ArrayType*> type(&array.typeDef().arrayType());
  MOZ_ASSERT(type->elementType() == StorageType::I16);
  MOZ_ASSERT(type->isMutable() == isMutable);
  return &array;
}

/* static */
int32_t Instance::stringTest(Instance* instance, void* stringArg) {
  MOZ_ASSERT(SASigStringTest.failureMode == FailureMode::Infallible);
  AnyRef string = AnyRef::fromCompiledCode(stringArg);
  if (string.isNull() || !string.isJSString()) {
    return 0;
  }
  return 1;
}

/* static */
void* Instance::stringCast(Instance* instance, void* stringArg) {
  MOZ_ASSERT(SASigStringCast.failureMode == FailureMode::FailOnNullPtr);
  AnyRef string = AnyRef::fromCompiledCode(stringArg);
  if (string.isNull() || !string.isJSString()) {
    ReportTrapError(instance->cx(), JSMSG_WASM_BAD_CAST);
    return nullptr;
  }
  return string.forCompiledCode();
}

/* static */
void* Instance::stringFromCharCodeArray(Instance* instance, void* arrayArg,
                                        uint32_t arrayStart,
                                        uint32_t arrayEnd) {
  MOZ_ASSERT(SASigStringFromCharCodeArray.failureMode ==
             FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();
  RootedAnyRef arrayRef(cx, AnyRef::fromCompiledCode(arrayArg));
  if (arrayRef.isNull()) {
    ReportTrapError(instance->cx(), JSMSG_WASM_BAD_CAST);
    return nullptr;
  }
  Rooted<WasmArrayObject*> array(cx, UncheckedCastToArrayI16<true>(arrayRef));

  if (arrayStart > arrayEnd || arrayEnd > array->numElements_) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return nullptr;
  }
  uint32_t arrayCount = arrayEnd - arrayStart;

  // GC is disabled on this call since it can cause the array to move,
  // invalidating the data pointer we pass as a parameter
  JSLinearString* string = NewStringCopyN<NoGC, char16_t>(
      cx, (char16_t*)array->data_ + arrayStart, arrayCount);
  if (!string) {
    // If the first attempt failed, we need to try again with a potential GC.
    // Acquire a stable version of the array that we can use. This may copy
    // inline data to the stack, so we avoid doing it unless we must.
    StableWasmArrayObjectElements<uint16_t> stableElements(cx, array);
    string = NewStringCopyN<CanGC, char16_t>(
        cx, (char16_t*)stableElements.elements() + arrayStart, arrayCount);
    if (!string) {
      MOZ_ASSERT(cx->isThrowingOutOfMemory());
      return nullptr;
    }
  }
  return AnyRef::fromJSString(string).forCompiledCode();
}

/* static */
int32_t Instance::stringIntoCharCodeArray(Instance* instance, void* stringArg,
                                          void* arrayArg, uint32_t arrayStart) {
  MOZ_ASSERT(SASigStringIntoCharCodeArray.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }
  Rooted<JSString*> string(cx, stringRef.toJSString());
  size_t stringLength = string->length();

  RootedAnyRef arrayRef(cx, AnyRef::fromCompiledCode(arrayArg));
  if (arrayRef.isNull()) {
    ReportTrapError(instance->cx(), JSMSG_WASM_BAD_CAST);
    return -1;
  }
  Rooted<WasmArrayObject*> array(cx, UncheckedCastToArrayI16<true>(arrayRef));

  CheckedUint32 lastIndexPlus1 = CheckedUint32(arrayStart) + stringLength;
  if (!lastIndexPlus1.isValid() ||
      lastIndexPlus1.value() > array->numElements_) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  JSLinearString* linearStr = string->ensureLinear(cx);
  if (!linearStr) {
    return -1;
  }
  char16_t* arrayData = reinterpret_cast<char16_t*>(array->data_);
  CopyChars(arrayData + arrayStart, *linearStr);
  return stringLength;
}

void* Instance::stringFromCharCode(Instance* instance, uint32_t charCode) {
  MOZ_ASSERT(SASigStringFromCharCode.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();

  JSString* str = StringFromCharCode(cx, int32_t(charCode));
  if (!str) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }

  return AnyRef::fromJSString(str).forCompiledCode();
}

void* Instance::stringFromCodePoint(Instance* instance, uint32_t codePoint) {
  MOZ_ASSERT(SASigStringFromCodePoint.failureMode ==
             FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();

  // Check for any error conditions before calling fromCodePoint so we report
  // the correct error
  if (codePoint > unicode::NonBMPMax) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CODEPOINT);
    return nullptr;
  }

  JSString* str = StringFromCodePoint(cx, char32_t(codePoint));
  if (!str) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }

  return AnyRef::fromJSString(str).forCompiledCode();
}

int32_t Instance::stringCharCodeAt(Instance* instance, void* stringArg,
                                   uint32_t index) {
  MOZ_ASSERT(SASigStringCharCodeAt.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  Rooted<JSString*> string(cx, stringRef.toJSString());
  if (index >= string->length()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  char16_t c;
  if (!string->getChar(cx, index, &c)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }
  return c;
}

int32_t Instance::stringCodePointAt(Instance* instance, void* stringArg,
                                    uint32_t index) {
  MOZ_ASSERT(SASigStringCodePointAt.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  Rooted<JSString*> string(cx, stringRef.toJSString());
  if (index >= string->length()) {
    ReportTrapError(cx, JSMSG_WASM_OUT_OF_BOUNDS);
    return -1;
  }

  char32_t c;
  if (!string->getCodePoint(cx, index, &c)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return false;
  }
  return c;
}

int32_t Instance::stringLength(Instance* instance, void* stringArg) {
  MOZ_ASSERT(SASigStringLength.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  static_assert(JS::MaxStringLength <= INT32_MAX);
  return (int32_t)stringRef.toJSString()->length();
}

void* Instance::stringConcat(Instance* instance, void* firstStringArg,
                             void* secondStringArg) {
  MOZ_ASSERT(SASigStringConcat.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();

  AnyRef firstStringRef = AnyRef::fromCompiledCode(firstStringArg);
  AnyRef secondStringRef = AnyRef::fromCompiledCode(secondStringArg);
  if (!firstStringRef.isJSString() || !secondStringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return nullptr;
  }

  Rooted<JSString*> firstString(cx, firstStringRef.toJSString());
  Rooted<JSString*> secondString(cx, secondStringRef.toJSString());
  JSString* result = ConcatStrings<CanGC>(cx, firstString, secondString);
  if (!result) {
    MOZ_ASSERT(cx->isExceptionPending());
    return nullptr;
  }
  return AnyRef::fromJSString(result).forCompiledCode();
}

void* Instance::stringSubstring(Instance* instance, void* stringArg,
                                uint32_t startIndex, uint32_t endIndex) {
  MOZ_ASSERT(SASigStringSubstring.failureMode == FailureMode::FailOnNullPtr);
  JSContext* cx = instance->cx();

  AnyRef stringRef = AnyRef::fromCompiledCode(stringArg);
  if (!stringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return nullptr;
  }

  static_assert(JS::MaxStringLength <= INT32_MAX);
  RootedString string(cx, stringRef.toJSString());
  uint32_t stringLength = string->length();
  if (startIndex > stringLength || startIndex > endIndex) {
    return AnyRef::fromJSString(cx->names().empty_).forCompiledCode();
  }

  if (endIndex > stringLength) {
    endIndex = stringLength;
  }

  JSString* result =
      SubstringKernel(cx, string, startIndex, endIndex - startIndex);
  if (!result) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return nullptr;
  }
  return AnyRef::fromJSString(result).forCompiledCode();
}

int32_t Instance::stringEquals(Instance* instance, void* firstStringArg,
                               void* secondStringArg) {
  MOZ_ASSERT(SASigStringEquals.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();

  AnyRef firstStringRef = AnyRef::fromCompiledCode(firstStringArg);
  AnyRef secondStringRef = AnyRef::fromCompiledCode(secondStringArg);

  // Null strings are considered equals
  if (firstStringRef.isNull() || secondStringRef.isNull()) {
    return firstStringRef.isNull() == secondStringRef.isNull();
  }

  // Otherwise, rule out any other kind of reference value
  if (!firstStringRef.isJSString() || !secondStringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return -1;
  }

  bool equals;
  if (!EqualStrings(cx, firstStringRef.toJSString(),
                    secondStringRef.toJSString(), &equals)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return -1;
  }
  return equals ? 1 : 0;
}

int32_t Instance::stringCompare(Instance* instance, void* firstStringArg,
                                void* secondStringArg) {
  MOZ_ASSERT(SASigStringCompare.failureMode == FailureMode::FailOnMaxI32);
  JSContext* cx = instance->cx();

  AnyRef firstStringRef = AnyRef::fromCompiledCode(firstStringArg);
  AnyRef secondStringRef = AnyRef::fromCompiledCode(secondStringArg);
  if (!firstStringRef.isJSString() || !secondStringRef.isJSString()) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CAST);
    return INT32_MAX;
  }

  int32_t result;
  if (!CompareStrings(cx, firstStringRef.toJSString(),
                      secondStringRef.toJSString(), &result)) {
    MOZ_ASSERT(cx->isThrowingOutOfMemory());
    return INT32_MAX;
  }

  if (result < 0) {
    return -1;
  }
  if (result > 0) {
    return 1;
  }
  return result;
}

// [SMDOC] Wasm Function.prototype.call.bind optimization
//
// Check if our import is of the form `Function.prototype.call.bind(targetFunc)`
// and optimize it so that we call `targetFunc` directly and pass the
// first wasm function parameter as the 'this' value.
//
// Breaking it down:
//   1. `Function.prototype.call` invokes the function given by `this`
//       and passes the first argument as the `this` value, then the
//       remaining arguments as the natural arguments.
//   2. `Function.prototype.bind` creates a new bound function that will
//      always pass a chosen value as the `this` value.
//   3. Binding 'targetFunc' to `Function.prototype.call` is equivalent to
//      `(thisValue, ...args) => targetFunc.call(thisValue, ...args)`;
//      but in a form the VM can pattern match on easily.
//
// When all of these conditions match, we set the `isFunctionCallBind` flag on
// FuncImportInstanceData and set callable to `targetFunc`. Then
// Instance::callImport reads the flag to figure out if the first parameter
// should be stored in invokeArgs.thisv() or in normal arguments.
//
// JIT exits do not support this flag yet, and so we don't use them on the
// targetFunc. This is okay because we couldn't use them on BoundFunctionObject
// anyways, and so this is strictly faster. Eventually we can add JIT exit
// support here.
JSObject* MaybeOptimizeFunctionCallBind(const wasm::FuncType& funcType,
                                        JSObject* f) {
  // Skip this for functions with no args. This is useless as it would result
  // in `this` always being undefined. Skipping this simplifies the logic in
  // Instance::callImport.
  if (funcType.args().length() == 0) {
    return nullptr;
  }

  if (!f->is<BoundFunctionObject>()) {
    return nullptr;
  }

  BoundFunctionObject* boundFun = &f->as<BoundFunctionObject>();
  JSObject* boundTarget = boundFun->getTarget();
  Value boundThis = boundFun->getBoundThis();

  // There cannot be any extra bound args in addition to the 'this'.
  if (boundFun->numBoundArgs() != 0) {
    return nullptr;
  }

  // The bound `target` must be the Function.prototype.call builtin
  if (!IsNativeFunction(boundTarget, fun_call)) {
    return nullptr;
  }

  // The bound `this` must be a callable object
  if (!boundThis.isObject() || !boundThis.toObject().isCallable() ||
      IsCrossCompartmentWrapper(boundThis.toObjectOrNull())) {
    return nullptr;
  }

  return boundThis.toObjectOrNull();
}

//////////////////////////////////////////////////////////////////////////////
//
// Instance creation and related.

Instance::Instance(JSContext* cx, Handle<WasmInstanceObject*> object,
                   const SharedCode& code, SharedTableVector&& tables,
                   UniqueDebugState maybeDebug)
    : realm_(cx->realm()),
      onSuspendableStack_(false),
      allocSites_(nullptr),
      jsJitArgsRectifier_(
          cx->runtime()->jitRuntime()->getArgumentsRectifier().value),
      jsJitExceptionHandler_(
          cx->runtime()->jitRuntime()->getExceptionTail().value),
      preBarrierCode_(
          cx->runtime()->jitRuntime()->preBarrier(MIRType::WasmAnyRef).value),
      storeBuffer_(&cx->runtime()->gc.storeBuffer()),
      object_(object),
      code_(std::move(code)),
      tables_(std::move(tables)),
      maybeDebug_(std::move(maybeDebug)),
      debugFilter_(nullptr),
      callRefMetrics_(nullptr),
      maxInitializedGlobalsIndexPlus1_(0),
      allocationMetadataBuilder_(nullptr),
      addressOfLastBufferedWholeCell_(
          cx->runtime()->gc.addressOfLastBufferedWholeCell()) {
  for (size_t i = 0; i < N_BASELINE_SCRATCH_WORDS; i++) {
    baselineScratchWords_[i] = 0;
  }
}

Instance* Instance::create(JSContext* cx, Handle<WasmInstanceObject*> object,
                           const SharedCode& code, uint32_t instanceDataLength,
                           SharedTableVector&& tables,
                           UniqueDebugState maybeDebug) {
  void* base = js_calloc(alignof(Instance) + offsetof(Instance, data_) +
                         instanceDataLength);
  if (!base) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  void* aligned = (void*)AlignBytes(uintptr_t(base), alignof(Instance));

  auto* instance = new (aligned)
      Instance(cx, object, code, std::move(tables), std::move(maybeDebug));
  instance->allocatedBase_ = base;
  return instance;
}

void Instance::destroy(Instance* instance) {
  instance->~Instance();
  js_free(instance->allocatedBase_);
}

bool Instance::init(JSContext* cx, const JSObjectVector& funcImports,
                    const ValVector& globalImportValues,
                    Handle<WasmMemoryObjectVector> memories,
                    const WasmGlobalObjectVector& globalObjs,
                    const WasmTagObjectVector& tagObjs,
                    const DataSegmentVector& dataSegments,
                    const ModuleElemSegmentVector& elemSegments) {
  MOZ_ASSERT(!!maybeDebug_ == code().debugEnabled());

  MOZ_ASSERT(funcImports.length() == code().funcImports().length());
  MOZ_ASSERT(tables_.length() == codeMeta().tables.length());

  cx_ = cx;
  valueBoxClass_ = AnyRef::valueBoxClass();
  resetInterrupt(cx);
  jumpTable_ = code_->tieringJumpTable();
  debugFilter_ = nullptr;
  callRefMetrics_ = nullptr;
  addressOfNeedsIncrementalBarrier_ =
      cx->compartment()->zone()->addressOfNeedsIncrementalBarrier();
  addressOfNurseryPosition_ = cx->nursery().addressOfPosition();
#ifdef JS_GC_ZEAL
  addressOfGCZealModeBits_ = cx->runtime()->gc.addressOfZealModeBits();
#endif

  // Initialize the request-tier-up stub pointer, if relevant
  if (code().mode() == CompileMode::LazyTiering) {
    setRequestTierUpStub(code().sharedStubs().base() +
                         code().requestTierUpStubOffset());
    setUpdateCallRefMetricsStub(code().sharedStubs().base() +
                                code().updateCallRefMetricsStubOffset());
  } else {
    setRequestTierUpStub(nullptr);
    setUpdateCallRefMetricsStub(nullptr);
  }

  // Initialize the hotness counters, if relevant.
  if (code().mode() == CompileMode::LazyTiering) {
    // Computing the initial hotness counters requires the code section size.
    const size_t codeSectionSize = codeMeta().codeSectionSize();
    for (uint32_t funcIndex = codeMeta().numFuncImports;
         funcIndex < codeMeta().numFuncs(); funcIndex++) {
      funcDefInstanceData(funcIndex)->hotnessCounter =
          computeInitialHotnessCounter(funcIndex, codeSectionSize);
    }
  }

  // Initialize type definitions in the instance data.
  const SharedTypeContext& types = codeMeta().types;
  Zone* zone = realm()->zone();
  for (uint32_t typeIndex = 0; typeIndex < types->length(); typeIndex++) {
    const TypeDef& typeDef = types->type(typeIndex);
    TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);

    // Set default field values.
    new (typeDefData) TypeDefInstanceData();

    // Store the runtime type for this type index
    typeDefData->typeDef = &typeDef;
    typeDefData->superTypeVector = typeDef.superTypeVector();

    if (typeDef.kind() == TypeDefKind::Struct ||
        typeDef.kind() == TypeDefKind::Array) {
      // Compute the parameters that allocation will use.  First, the class
      // and alloc kind for the type definition.
      const JSClass* clasp;
      gc::AllocKind allocKind;

      if (typeDef.kind() == TypeDefKind::Struct) {
        clasp = WasmStructObject::classForTypeDef(&typeDef);
        allocKind = WasmStructObject::allocKindForTypeDef(&typeDef);
        allocKind = gc::GetFinalizedAllocKindForClass(allocKind, clasp);
      } else {
        clasp = &WasmArrayObject::class_;
        allocKind = gc::AllocKind::INVALID;
      }

      // Find the shape using the class and recursion group
      const ObjectFlags objectFlags = {ObjectFlag::NotExtensible};
      typeDefData->shape =
          WasmGCShape::getShape(cx, clasp, cx->realm(), TaggedProto(),
                                &typeDef.recGroup(), objectFlags);
      if (!typeDefData->shape) {
        return false;
      }

      typeDefData->clasp = clasp;
      typeDefData->allocKind = allocKind;

      // If `typeDef` is a struct, cache its size here, so that allocators
      // don't have to chase back through `typeDef` to determine that.
      // Similarly, if `typeDef` is an array, cache its array element size
      // here.
      MOZ_ASSERT(typeDefData->unused == 0);
      if (typeDef.kind() == TypeDefKind::Struct) {
        typeDefData->structTypeSize = typeDef.structType().size_;
        // StructLayout::close ensures this is an integral number of words.
        MOZ_ASSERT((typeDefData->structTypeSize % sizeof(uintptr_t)) == 0);
      } else {
        uint32_t arrayElemSize = typeDef.arrayType().elementType().size();
        typeDefData->arrayElemSize = arrayElemSize;
        MOZ_ASSERT(arrayElemSize == 16 || arrayElemSize == 8 ||
                   arrayElemSize == 4 || arrayElemSize == 2 ||
                   arrayElemSize == 1);
      }
    } else if (typeDef.kind() == TypeDefKind::Func) {
      // Nothing to do; the default values are OK.
    } else {
      MOZ_ASSERT(typeDef.kind() == TypeDefKind::None);
      MOZ_CRASH();
    }
  }

  // Create and initialize alloc sites, they are all the same for Wasm.
  uint32_t allocSitesCount = codeTailMeta().numAllocSites;
  if (allocSitesCount > 0) {
    allocSites_ =
        (gc::AllocSite*)js_malloc(sizeof(gc::AllocSite) * allocSitesCount);
    if (!allocSites_) {
      ReportOutOfMemory(cx);
      return false;
    }
    for (uint32_t i = 0; i < allocSitesCount; ++i) {
      new (&allocSites_[i]) gc::AllocSite();
      allocSites_[i].initWasm(zone);
    }
  }

  // Initialize function imports in the instance data
  for (size_t i = 0; i < code().funcImports().length(); i++) {
    JSObject* f = funcImports[i];

#ifdef ENABLE_WASM_JSPI
    if (JSObject* suspendingObject = MaybeUnwrapSuspendingObject(f)) {
      // Compile suspending function Wasm wrapper.
      const FuncType& funcType = codeMeta().getFuncType(i);
      RootedObject wrapped(cx, suspendingObject);
      RootedFunction wrapper(
          cx, WasmSuspendingFunctionCreate(cx, wrapped, funcType));
      if (!wrapper) {
        return false;
      }
      MOZ_ASSERT(wrapper->isWasm());
      f = wrapper;
    }
#endif

    MOZ_ASSERT(f->isCallable());
    const FuncImport& fi = code().funcImport(i);
    const FuncType& funcType = codeMeta().getFuncType(i);
    FuncImportInstanceData& import = funcImportInstanceData(i);
    import.callable = f;
    import.isFunctionCallBind = false;
    if (f->is<JSFunction>()) {
      JSFunction* fun = &f->as<JSFunction>();
      if (!isAsmJS() && !codeMeta().funcImportsAreJS && fun->isWasm()) {
        import.instance = &fun->wasmInstance();
        import.realm = fun->realm();
        import.code = fun->wasmUncheckedCallEntry();
      } else if (void* thunk = MaybeGetTypedNative(fun, funcType)) {
        import.instance = this;
        import.realm = fun->realm();
        import.code = thunk;
      } else {
        import.instance = this;
        import.realm = fun->realm();
        import.code = code().sharedStubs().base() + fi.interpExitCodeOffset();
      }
    } else if (JSObject* callable =
                   MaybeOptimizeFunctionCallBind(funcType, f)) {
      import.instance = this;
      import.callable = callable;
      import.realm = import.callable->nonCCWRealm();
      import.code = code().sharedStubs().base() + fi.interpExitCodeOffset();
      import.isFunctionCallBind = true;
    } else {
      import.instance = this;
      import.realm = import.callable->nonCCWRealm();
      import.code = code().sharedStubs().base() + fi.interpExitCodeOffset();
    }
  }

#ifdef DEBUG
  for (size_t i = 0; i < codeMeta().numExportedFuncs(); i++) {
    MOZ_ASSERT(!funcExportInstanceData(i).func);
  }
#endif

  // Initialize globals in the instance data.
  //
  // This must be performed after we have initialized runtime types as a global
  // initializer may reference them.
  //
  // We increment `maxInitializedGlobalsIndexPlus1_` every iteration of the
  // loop, as we call out to `InitExpr::evaluate` which may call
  // `constantGlobalGet` which uses this value to assert we're never accessing
  // uninitialized globals.
  maxInitializedGlobalsIndexPlus1_ = 0;
  for (size_t i = 0; i < codeMeta().globals.length();
       i++, maxInitializedGlobalsIndexPlus1_ = i) {
    const GlobalDesc& global = codeMeta().globals[i];

    // Constants are baked into the code, never stored in the global area.
    if (global.isConstant()) {
      continue;
    }

    uint8_t* globalAddr = data() + global.offset();
    switch (global.kind()) {
      case GlobalKind::Import: {
        size_t imported = global.importIndex();
        if (global.isIndirect()) {
          *(void**)globalAddr =
              (void*)&globalObjs[imported]->val().get().cell();
        } else {
          globalImportValues[imported].writeToHeapLocation(globalAddr);
        }
        break;
      }
      case GlobalKind::Variable: {
        RootedVal val(cx);
        const InitExpr& init = global.initExpr();
        Rooted<WasmInstanceObject*> instanceObj(cx, object());
        if (!init.evaluate(cx, instanceObj, &val)) {
          return false;
        }

        if (global.isIndirect()) {
          // Initialize the cell
          globalObjs[i]->setVal(val);

          // Link to the cell
          *(void**)globalAddr = globalObjs[i]->addressOfCell();
        } else {
          val.get().writeToHeapLocation(globalAddr);
        }
        break;
      }
      case GlobalKind::Constant: {
        MOZ_CRASH("skipped at the top");
      }
    }
  }

  // All globals were initialized
  MOZ_ASSERT(maxInitializedGlobalsIndexPlus1_ == codeMeta().globals.length());

  // Initialize memories in the instance data
  for (size_t i = 0; i < memories.length(); i++) {
    const MemoryDesc& md = codeMeta().memories[i];
    MemoryInstanceData& data = memoryInstanceData(i);
    WasmMemoryObject* memory = memories.get()[i];

    data.memory = memory;
    data.base = memory->buffer().dataPointerEither().unwrap();
    size_t limit = memory->boundsCheckLimit();
#if !defined(JS_64BIT)
    // We assume that the limit is a 32-bit quantity
    MOZ_ASSERT(limit <= UINT32_MAX);
#endif
    data.boundsCheckLimit = limit;
    data.isShared = md.isShared();

    // Add observer if our memory base may grow
    if (memory && memory->movingGrowable() &&
        !memory->addMovingGrowObserver(cx, object_)) {
      return false;
    }
  }

  // Cache the default memory's values
  if (memories.length() > 0) {
    MemoryInstanceData& data = memoryInstanceData(0);
    memory0Base_ = data.base;
    memory0BoundsCheckLimit_ = data.boundsCheckLimit;
  } else {
    memory0Base_ = nullptr;
    memory0BoundsCheckLimit_ = 0;
  }

  // Initialize tables in the instance data
  for (size_t i = 0; i < tables_.length(); i++) {
    const TableDesc& td = codeMeta().tables[i];
    TableInstanceData& table = tableInstanceData(i);
    table.length = tables_[i]->length();
    table.elements = tables_[i]->instanceElements();
    // Non-imported tables, with init_expr, has to be initialized with
    // the evaluated value.
    if (!td.isImported && td.initExpr) {
      Rooted<WasmInstanceObject*> instanceObj(cx, object());
      RootedVal val(cx);
      if (!td.initExpr->evaluate(cx, instanceObj, &val)) {
        return false;
      }
      RootedAnyRef ref(cx, val.get().ref());
      tables_[i]->fillUninitialized(0, tables_[i]->length(), ref, cx);
    }
  }

#ifdef DEBUG
  // All (linked) tables with non-nullable types must be initialized.
  for (size_t i = 0; i < tables_.length(); i++) {
    const TableDesc& td = codeMeta().tables[i];
    if (!td.elemType.isNullable()) {
      tables_[i]->assertRangeNotNull(0, tables_[i]->length());
    }
  }
#endif  // DEBUG

  // Initialize tags in the instance data
  for (size_t i = 0; i < codeMeta().tags.length(); i++) {
    MOZ_ASSERT(tagObjs[i] != nullptr);
    tagInstanceData(i).object = tagObjs[i];
  }
  pendingException_ = nullptr;
  pendingExceptionTag_ = nullptr;

  // Add debug filtering table.
  if (code().debugEnabled()) {
    size_t numFuncs = codeMeta().numFuncs();
    size_t numWords = std::max<size_t>((numFuncs + 31) / 32, 1);
    debugFilter_ = (uint32_t*)js_calloc(numWords, sizeof(uint32_t));
    if (!debugFilter_) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  if (code().mode() == CompileMode::LazyTiering) {
    callRefMetrics_ = (CallRefMetrics*)js_calloc(
        codeTailMeta().numCallRefMetrics, sizeof(CallRefMetrics));
    if (!callRefMetrics_) {
      ReportOutOfMemory(cx);
      return false;
    }
    // A zeroed-out CallRefMetrics should satisfy
    // CallRefMetrics::checkInvariants.
    MOZ_ASSERT_IF(codeTailMeta().numCallRefMetrics > 0,
                  callRefMetrics_[0].checkInvariants());
  } else {
    MOZ_ASSERT(codeTailMeta().numCallRefMetrics == 0);
  }

  // Add observers if our tables may grow
  for (const SharedTable& table : tables_) {
    if (table->movingGrowable() && !table->addMovingGrowObserver(cx, object_)) {
      return false;
    }
  }

  // Take references to the passive data segments
  if (!passiveDataSegments_.resize(dataSegments.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < dataSegments.length(); i++) {
    if (!dataSegments[i]->active()) {
      passiveDataSegments_[i] = dataSegments[i];
    }
  }

  // Create InstanceElemSegments for any passive element segments, since these
  // are the ones available at runtime.
  if (!passiveElemSegments_.resize(elemSegments.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < elemSegments.length(); i++) {
    const ModuleElemSegment& seg = elemSegments[i];
    if (seg.kind == ModuleElemSegment::Kind::Passive) {
      passiveElemSegments_[i] = InstanceElemSegment();
      InstanceElemSegment& instanceSeg = passiveElemSegments_[i];
      if (!instanceSeg.reserve(seg.numElements())) {
        ReportOutOfMemory(cx);
        return false;
      }

      bool ok = iterElemsAnyrefs(cx, seg, [&](uint32_t _, AnyRef ref) -> bool {
        instanceSeg.infallibleAppend(ref);
        return true;
      });
      if (!ok) {
        return false;
      }
    }
  }

  return true;
}

Instance::~Instance() {
  realm_->wasm.unregisterInstance(*this);

  if (debugFilter_) {
    js_free(debugFilter_);
  }
  if (callRefMetrics_) {
    js_free(callRefMetrics_);
  }
  if (allocSites_) {
    js_free(allocSites_);
  }

  // Any pending exceptions should have been consumed.
  MOZ_ASSERT(pendingException_.isNull());
}

void Instance::setInterrupt() {
  interrupt_ = true;
  stackLimit_ = JS::NativeStackLimitMin;
}

bool Instance::isInterrupted() const {
  return interrupt_ || stackLimit_ == JS::NativeStackLimitMin;
}

void Instance::resetInterrupt(JSContext* cx) {
  interrupt_ = false;
#ifdef ENABLE_WASM_JSPI
  if (cx->wasm().suspendableStackLimit != JS::NativeStackLimitMin) {
    stackLimit_ = cx->wasm().suspendableStackLimit;
    return;
  }
#endif
  stackLimit_ = cx->stackLimitForJitCode(JS::StackForUntrustedScript);
}

void Instance::setTemporaryStackLimit(JS::NativeStackLimit limit) {
  if (!isInterrupted()) {
    stackLimit_ = limit;
  }
  onSuspendableStack_ = true;
}

void Instance::resetTemporaryStackLimit(JSContext* cx) {
  if (!isInterrupted()) {
    stackLimit_ = cx->stackLimitForJitCode(JS::StackForUntrustedScript);
  }
  onSuspendableStack_ = false;
}

int32_t Instance::computeInitialHotnessCounter(uint32_t funcIndex,
                                               size_t codeSectionSize) {
  MOZ_ASSERT(code().mode() == CompileMode::LazyTiering);
  MOZ_ASSERT(codeSectionSize > 0);
  uint32_t bodyLength = codeTailMeta().funcDefRange(funcIndex).size();
  return LazyTieringHeuristics::estimateIonCompilationCost(bodyLength,
                                                           codeSectionSize);
}

void Instance::resetHotnessCounter(uint32_t funcIndex) {
  funcDefInstanceData(funcIndex)->hotnessCounter = INT32_MAX;
}

int32_t Instance::readHotnessCounter(uint32_t funcIndex) const {
  return funcDefInstanceData(funcIndex)->hotnessCounter;
}

void Instance::submitCallRefHints(uint32_t funcIndex) {
#ifdef JS_JITSPEW
  bool headerShown = false;
#endif

  float requiredHotnessFraction =
      float(InliningHeuristics::rawCallRefPercent()) / 100.0;

  // Limits as set by InliningHeuristics::InliningHeuristics().
  const DebugOnly<float> epsilon = 0.000001;
  MOZ_ASSERT(requiredHotnessFraction >= 0.1 - epsilon);
  MOZ_ASSERT(requiredHotnessFraction <= 1.0 + epsilon);

  CallRefMetricsRange range = codeTailMeta().getFuncDefCallRefs(funcIndex);
  for (uint32_t callRefIndex = range.begin;
       callRefIndex < range.begin + range.length; callRefIndex++) {
    MOZ_RELEASE_ASSERT(callRefIndex < codeTailMeta().numCallRefMetrics);

    // In this loop, for each CallRefMetrics, we create a corresponding
    // CallRefHint.  The CallRefHint is a recommendation of which function(s)
    // to inline into the associated call site.  It is based on call target
    // counts at the call site and incorporates other heuristics as implemented
    // by the code below.
    //
    // Later, when compiling the call site with Ion, the CallRefHint created
    // here is consulted.  That may or may not result in inlining actually
    // taking place, since it depends also on context known only at
    // Ion-compilation time -- inlining depth, inlining budgets, etc.  In
    // particular, if the call site is itself within a function that got
    // inlined multiple times, the call site may be compiled multiple times,
    // with inlining happening in some cases and not in others.
    //
    // The logic below tries to find reasons not to inline into this call site,
    // and if none are found, creates and stores a CallRefHint specifying the
    // recommended targets.
    //
    // The core criterion is that the set of targets that eventually get chosen
    // must together make up at least `requiredHotnessFraction` of all calls
    // made by this call site.

    CallRefMetrics& metrics = callRefMetrics_[callRefIndex];
    MOZ_RELEASE_ASSERT(metrics.checkInvariants());

    // For convenience, work with a copy of the candidates, not directly with
    // `metrics`.
    struct Candidate {
      uint32_t funcIndex = 0;
      uint32_t count = 0;
      Candidate() = default;
      Candidate(const Candidate&) = default;
      Candidate(uint32_t funcIndex, uint32_t count)
          : funcIndex(funcIndex), count(count) {}
    };
    Candidate candidates[CallRefMetrics::NUM_SLOTS];
    size_t numCandidates = 0;

    // If we're going to recommend no inlining here, specify a reason.
    const char* skipReason = nullptr;

    // The total count for targets that are individually tracked.
    uint64_t totalTrackedCount = 0;
    bool allCandidatesAreImports = true;

    // Make a first pass over the candidates, skipping imports.
    for (size_t i = 0; i < CallRefMetrics::NUM_SLOTS; i++) {
      if (!metrics.targets[i]) {
        break;
      }
      uint32_t targetCount = metrics.counts[i];
      if (targetCount == 0) {
        continue;
      }
      totalTrackedCount += uint64_t(targetCount);

      // We can't inline a call to a function which is in this module but has a
      // different Instance, since the potential callees of any function depend
      // on the instance it is associated with.  Cross-instance calls should
      // have already been excluded from consideration by the code generated by
      // BaseCompiler::updateCallRefMetrics, but given that this is critical,
      // assert it here.
      const DebugOnly<Instance*> targetFuncInstance =
          static_cast<wasm::Instance*>(
              metrics.targets[i]
                  ->getExtendedSlot(FunctionExtended::WASM_INSTANCE_SLOT)
                  .toPrivate());
      MOZ_ASSERT(targetFuncInstance == this);

      uint32_t targetFuncIndex = metrics.targets[i]->wasmFuncIndex();
      if (codeMeta().funcIsImport(targetFuncIndex)) {
        continue;
      }
      allCandidatesAreImports = false;
      candidates[numCandidates] = Candidate(targetFuncIndex, targetCount);
      numCandidates++;
    }
    MOZ_RELEASE_ASSERT(numCandidates <= CallRefMetrics::NUM_SLOTS);

    // The total count of all calls made by this call site.
    uint64_t totalCount = totalTrackedCount + uint64_t(metrics.countOther);

    // Throw out some obvious cases.
    if (totalCount == 0) {
      // See comments on definition of CallRefMetrics regarding overflow.
      skipReason = "(callsite unused)";
    } else if (metrics.targets[0] == nullptr) {
      // None of the calls made by this call site could be attributed to
      // specific callees; they all got lumped into CallRefMetrics::countOther.
      // See GenerateUpdateCallRefMetricsStub for possible reasons why.
      skipReason = "(no individually tracked targets)";
    } else if (numCandidates > 0 && allCandidatesAreImports) {
      // Imported functions can't be inlined.
      skipReason = "(all targets are imports)";
    }

    // We want to avoid inlining large functions into cold(ish) call sites.
    if (!skipReason) {
      uint32_t totalTargetBodySize = 0;
      for (size_t i = 0; i < numCandidates; i++) {
        totalTargetBodySize +=
            codeTailMeta().funcDefRange(candidates[i].funcIndex).size();
      }
      if (totalCount < 2 * totalTargetBodySize) {
        skipReason = "(callsite too cold)";
      }
    }

    // The final check is the most important.  We need to choose some subset of
    // the candidates which together make up at least `requiredHotnessFraction`
    // of the calls made by this call site.  However, to avoid generated code
    // wasting time on checking guards for relatively unlikely targets, we
    // ignore any candidate that does not achieve at least 10% of
    // `requiredHotnessFraction`.  Also make up a CallRefHints in anticipation
    // of finding a usable set of candidates.
    CallRefHint hints;
    if (!skipReason) {
      MOZ_RELEASE_ASSERT(totalCount > 0);  // Be sure to avoid NaN/Inf problems
      float usableFraction = 0.0;
      uint32_t numUsableCandidates = 0;
      for (size_t i = 0; i < numCandidates; i++) {
        float candidateFraction =
            float(candidates[i].count) / float(totalCount);
        if (candidateFraction >= 0.1 * requiredHotnessFraction) {
          usableFraction += candidateFraction;
          numUsableCandidates++;
          if (!hints.full()) {
            // Add this candidate to `hints`.  This assumes that we
            // (more-or-less) encounter candidates in declining order of
            // hotness.  See block comment on `struct CallRefMetrics`.
            hints.append(candidates[i].funcIndex);
          }
        }
      }
      if (numUsableCandidates == 0) {
        skipReason = "(no target is hot enough)";
      } else if (usableFraction < requiredHotnessFraction) {
        skipReason = "(collectively not hot enough)";
      }
    }

    if (!skipReason) {
      // Success!
      MOZ_ASSERT(hints.length() > 0);
      codeTailMeta().setCallRefHint(callRefIndex, hints);
    } else {
      CallRefHint empty;
      codeTailMeta().setCallRefHint(callRefIndex, empty);
    }

#ifdef JS_JITSPEW
    if (!headerShown) {
      JS_LOG(wasmPerf, Info, "CM=..%06lx  CallRefMetrics for I=..%06lx fI=%-4u",
             (unsigned long)(uintptr_t(&codeMeta()) & 0xFFFFFFL),
             (unsigned long)(uintptr_t(this) & 0xFFFFFFL), funcIndex);
      headerShown = true;
    }

    JS::UniqueChars countsStr;
    for (size_t i = 0; i < CallRefMetrics::NUM_SLOTS; i++) {
      countsStr =
          JS_sprintf_append(std::move(countsStr), "%u ", metrics.counts[i]);
    }
    JS::UniqueChars targetStr;
    if (skipReason) {
      targetStr = JS_smprintf("%s", skipReason);
    } else {
      targetStr = JS_smprintf("%s", "fI ");
      for (size_t i = 0; i < hints.length(); i++) {
        targetStr =
            JS_sprintf_append(std::move(targetStr), "%u%s", hints.get(i),
                              i + 1 < hints.length() ? ", " : "");
      }
    }
    JS_LOG(wasmPerf, Info, "CM=..%06lx    %sother:%u --> %s",
           (unsigned long)(uintptr_t(&codeMeta()) & 0xFFFFFFL), countsStr.get(),
           metrics.countOther, targetStr.get());
#endif
  }
}

bool Instance::debugFilter(uint32_t funcIndex) const {
  return (debugFilter_[funcIndex / 32] >> funcIndex % 32) & 1;
}

void Instance::setDebugFilter(uint32_t funcIndex, bool value) {
  if (value) {
    debugFilter_[funcIndex / 32] |= (1 << funcIndex % 32);
  } else {
    debugFilter_[funcIndex / 32] &= ~(1 << funcIndex % 32);
  }
}

bool Instance::memoryAccessInGuardRegion(const uint8_t* addr,
                                         unsigned numBytes) const {
  MOZ_ASSERT(numBytes > 0);

  for (uint32_t memoryIndex = 0; memoryIndex < codeMeta().memories.length();
       memoryIndex++) {
    uint8_t* base = memoryBase(memoryIndex).unwrap(/* comparison */);
    if (addr < base) {
      continue;
    }

    WasmMemoryObject* mem = memory(memoryIndex);
    size_t lastByteOffset = addr - base + (numBytes - 1);
    if (lastByteOffset >= mem->volatileMemoryLength() &&
        lastByteOffset < mem->buffer().wasmMappedSize()) {
      return true;
    }
  }
  return false;
}

void Instance::tracePrivate(JSTracer* trc) {
  // This method is only called from WasmInstanceObject so the only reason why
  // TraceEdge is called is so that the pointer can be updated during a moving
  // GC.
  MOZ_ASSERT_IF(trc->isMarkingTracer(), gc::IsMarked(trc->runtime(), object_));
  TraceEdge(trc, &object_, "wasm instance object");

  // OK to just do one tier here; though the tiers have different funcImports
  // tables, they share the instance object.
  for (uint32_t funcIndex = 0; funcIndex < codeMeta().numFuncImports;
       funcIndex++) {
    TraceNullableEdge(trc, &funcImportInstanceData(funcIndex).callable,
                      "wasm import");
  }

  for (uint32_t funcExportIndex = 0;
       funcExportIndex < codeMeta().numExportedFuncs(); funcExportIndex++) {
    TraceNullableEdge(trc, &funcExportInstanceData(funcExportIndex).func,
                      "wasm func export");
  }

  for (uint32_t memoryIndex = 0;
       memoryIndex < code().codeMeta().memories.length(); memoryIndex++) {
    MemoryInstanceData& memoryData = memoryInstanceData(memoryIndex);
    TraceNullableEdge(trc, &memoryData.memory, "wasm memory object");
  }

  for (const SharedTable& table : tables_) {
    table->trace(trc);
  }

  for (const GlobalDesc& global : code().codeMeta().globals) {
    // Indirect reference globals get traced by the owning WebAssembly.Global.
    if (!global.type().isRefRepr() || global.isConstant() ||
        global.isIndirect()) {
      continue;
    }
    GCPtr<AnyRef>* obj = (GCPtr<AnyRef>*)(data() + global.offset());
    TraceNullableEdge(trc, obj, "wasm reference-typed global");
  }

  for (uint32_t tagIndex = 0; tagIndex < code().codeMeta().tags.length();
       tagIndex++) {
    TraceNullableEdge(trc, &tagInstanceData(tagIndex).object, "wasm tag");
  }

  const SharedTypeContext& types = codeMeta().types;
  for (uint32_t typeIndex = 0; typeIndex < types->length(); typeIndex++) {
    TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);
    TraceNullableEdge(trc, &typeDefData->shape, "wasm shape");
  }

  if (callRefMetrics_) {
    for (uint32_t i = 0; i < codeTailMeta().numCallRefMetrics; i++) {
      CallRefMetrics* metrics = &callRefMetrics_[i];
      MOZ_ASSERT(metrics->checkInvariants());
      for (size_t j = 0; j < CallRefMetrics::NUM_SLOTS; j++) {
        TraceNullableEdge(trc, &metrics->targets[j], "indirect call target");
      }
    }
  }

  TraceNullableEdge(trc, &pendingException_, "wasm pending exception value");
  TraceNullableEdge(trc, &pendingExceptionTag_, "wasm pending exception tag");

  passiveElemSegments_.trace(trc);

  if (maybeDebug_) {
    maybeDebug_->trace(trc);
  }
}

void js::wasm::TraceInstanceEdge(JSTracer* trc, Instance* instance,
                                 const char* name) {
  if (IsTracerKind(trc, JS::TracerKind::Moving)) {
    // Compacting GC: The Instance does not move so there is nothing to do here.
    // Reading the object from the instance below would be a data race during
    // multi-threaded updates. Compacting GC does not rely on graph traversal
    // to find all edges that need to be updated.
    return;
  }

  // Instance fields are traced by the owning WasmInstanceObject's trace
  // hook. Tracing this ensures they are traced once.
  JSObject* object = instance->objectUnbarriered();
  TraceManuallyBarrieredEdge(trc, &object, name);
}

static uintptr_t* GetFrameScanStartForStackMap(
    const Frame* frame, const StackMap* map,
    uintptr_t* highestByteVisitedInPrevFrame) {
  // |frame| points somewhere in the middle of the area described by |map|.
  // We have to calculate |scanStart|, the lowest address that is described by
  // |map|, by consulting |map->frameOffsetFromTop|.

  const size_t numMappedBytes = map->header.numMappedWords * sizeof(void*);
  const uintptr_t scanStart = uintptr_t(frame) +
                              (map->header.frameOffsetFromTop * sizeof(void*)) -
                              numMappedBytes;
  MOZ_ASSERT(0 == scanStart % sizeof(void*));

  // Do what we can to assert that, for consecutive wasm frames, their stack
  // maps also abut exactly.  This is a useful sanity check on the sizing of
  // stackmaps.
  //
  // In debug builds, the stackmap construction machinery goes to considerable
  // efforts to ensure that the stackmaps for consecutive frames abut exactly.
  // This is so as to ensure there are no areas of stack inadvertently ignored
  // by a stackmap, nor covered by two stackmaps.  Hence any failure of this
  // assertion is serious and should be investigated.
#ifndef JS_CODEGEN_ARM64
  MOZ_ASSERT_IF(
      highestByteVisitedInPrevFrame && *highestByteVisitedInPrevFrame != 0,
      *highestByteVisitedInPrevFrame + 1 == scanStart);
#endif

  if (highestByteVisitedInPrevFrame) {
    *highestByteVisitedInPrevFrame = scanStart + numMappedBytes - 1;
  }

  // If we have some exit stub words, this means the map also covers an area
  // created by a exit stub, and so the highest word of that should be a
  // constant created by (code created by) GenerateTrapExit.
  MOZ_ASSERT_IF(map->header.numExitStubWords > 0,
                ((uintptr_t*)scanStart)[map->header.numExitStubWords - 1 -
                                        TrapExitDummyValueOffsetFromTop] ==
                    TrapExitDummyValue);

  return (uintptr_t*)scanStart;
}

uintptr_t Instance::traceFrame(JSTracer* trc, const wasm::WasmFrameIter& wfi,
                               uint8_t* nextPC,
                               uintptr_t highestByteVisitedInPrevFrame) {
  const StackMap* map = code().lookupStackMap(nextPC);
  if (!map) {
    return 0;
  }
  Frame* frame = wfi.frame();
  uintptr_t* stackWords =
      GetFrameScanStartForStackMap(frame, map, &highestByteVisitedInPrevFrame);

  // Hand refs off to the GC.
  for (uint32_t i = 0; i < map->header.numMappedWords; i++) {
    if (map->get(i) != StackMap::Kind::AnyRef) {
      continue;
    }

    TraceManuallyBarrieredNullableEdge(trc, (AnyRef*)&stackWords[i],
                                       "Instance::traceWasmFrame: normal word");
  }

  // Deal with any GC-managed fields in the DebugFrame, if it is
  // present and those fields may be live.
  if (map->header.hasDebugFrameWithLiveRefs) {
    DebugFrame* debugFrame = DebugFrame::from(frame);
    char* debugFrameP = (char*)debugFrame;

    for (size_t i = 0; i < MaxRegisterResults; i++) {
      if (debugFrame->hasSpilledRegisterRefResult(i)) {
        char* resultRefP = debugFrameP + DebugFrame::offsetOfRegisterResult(i);
        TraceManuallyBarrieredNullableEdge(
            trc, (AnyRef*)resultRefP,
            "Instance::traceWasmFrame: DebugFrame::resultResults_");
      }
    }

    if (debugFrame->hasCachedReturnJSValue()) {
      char* cachedReturnJSValueP =
          debugFrameP + DebugFrame::offsetOfCachedReturnJSValue();
      TraceManuallyBarrieredEdge(
          trc, (js::Value*)cachedReturnJSValueP,
          "Instance::traceWasmFrame: DebugFrame::cachedReturnJSValue_");
    }
  }

  return highestByteVisitedInPrevFrame;
}

void Instance::updateFrameForMovingGC(const wasm::WasmFrameIter& wfi,
                                      uint8_t* nextPC) {
  const StackMap* map = code().lookupStackMap(nextPC);
  if (!map) {
    return;
  }
  Frame* frame = wfi.frame();
  uintptr_t* stackWords = GetFrameScanStartForStackMap(frame, map, nullptr);

  // Update interior array data pointers for any inline-storage arrays that
  // moved.
  for (uint32_t i = 0; i < map->header.numMappedWords; i++) {
    if (map->get(i) != StackMap::Kind::ArrayDataPointer) {
      continue;
    }

    uint8_t** addressOfArrayDataPointer = (uint8_t**)&stackWords[i];
    if (WasmArrayObject::isDataInline(*addressOfArrayDataPointer)) {
      WasmArrayObject* oldArray =
          WasmArrayObject::fromInlineDataPointer(*addressOfArrayDataPointer);
      WasmArrayObject* newArray =
          (WasmArrayObject*)gc::MaybeForwarded(oldArray);
      *addressOfArrayDataPointer =
          WasmArrayObject::addressOfInlineData(newArray);
    }
  }
}

WasmMemoryObject* Instance::memory(uint32_t memoryIndex) const {
  return memoryInstanceData(memoryIndex).memory;
}

SharedMem<uint8_t*> Instance::memoryBase(uint32_t memoryIndex) const {
  MOZ_ASSERT_IF(
      memoryIndex == 0,
      memory0Base_ == memory(memoryIndex)->buffer().dataPointerEither());
  return memory(memoryIndex)->buffer().dataPointerEither();
}

SharedArrayRawBuffer* Instance::sharedMemoryBuffer(uint32_t memoryIndex) const {
  MOZ_ASSERT(memory(memoryIndex)->isShared());
  return memory(memoryIndex)->sharedArrayRawBuffer();
}

WasmInstanceObject* Instance::objectUnbarriered() const {
  return object_.unbarrieredGet();
}

WasmInstanceObject* Instance::object() const { return object_; }

static bool GetInterpEntryAndEnsureStubs(JSContext* cx, Instance& instance,
                                         uint32_t funcIndex,
                                         const CallArgs& args,
                                         void** interpEntry,
                                         const FuncType** funcType) {
  const FuncExport* funcExport;
  if (!instance.code().getOrCreateInterpEntry(funcIndex, &funcExport,
                                              interpEntry)) {
    ReportOutOfMemory(cx);
    return false;
  }

  *funcType = &instance.codeMeta().getFuncType(funcIndex);

#ifdef DEBUG
  // EnsureEntryStubs() has ensured proper jit-entry stubs have been created and
  // installed in funcIndex's JumpTable entry, so check against the presence of
  // the provisional lazy stub.  See also
  // WasmInstanceObject::getExportedFunction().
  if (!funcExport->hasEagerStubs() && (*funcType)->canHaveJitEntry()) {
    if (!EnsureBuiltinThunksInitialized()) {
      ReportOutOfMemory(cx);
      return false;
    }
    JSFunction& callee = args.callee().as<JSFunction>();
    void* provisionalLazyJitEntryStub = ProvisionalLazyJitEntryStub();
    MOZ_ASSERT(provisionalLazyJitEntryStub);
    MOZ_ASSERT(callee.isWasmWithJitEntry());
    MOZ_ASSERT(*callee.wasmJitEntry() != provisionalLazyJitEntryStub);
  }
#endif
  return true;
}

bool wasm::ResultsToJSValue(JSContext* cx, ResultType type,
                            void* registerResultLoc,
                            Maybe<char*> stackResultsLoc,
                            MutableHandleValue rval, CoercionLevel level) {
  if (type.empty()) {
    // No results: set to undefined, and we're done.
    rval.setUndefined();
    return true;
  }

  // If we added support for multiple register results, we'd need to establish a
  // convention for how to store them to memory in registerResultLoc.  For now
  // we can punt.
  static_assert(MaxRegisterResults == 1);

  // Stack results written to stackResultsLoc; register result written
  // to registerResultLoc.

  // First, convert the register return value, and prepare to iterate in
  // push order.  Note that if the register result is a reference type,
  // it may be unrooted, so ToJSValue_anyref must not GC in that case.
  ABIResultIter iter(type);
  DebugOnly<bool> usedRegisterResult = false;
  for (; !iter.done(); iter.next()) {
    if (iter.cur().inRegister()) {
      MOZ_ASSERT(!usedRegisterResult);
      if (!ToJSValue<DebugCodegenVal>(cx, registerResultLoc, iter.cur().type(),
                                      rval, level)) {
        return false;
      }
      usedRegisterResult = true;
    }
  }
  MOZ_ASSERT(usedRegisterResult);

  MOZ_ASSERT((stackResultsLoc.isSome()) == (iter.count() > 1));
  if (!stackResultsLoc) {
    // A single result: we're done.
    return true;
  }

  // Otherwise, collect results in an array, in push order.
  Rooted<ArrayObject*> array(cx, NewDenseEmptyArray(cx));
  if (!array) {
    return false;
  }
  RootedValue tmp(cx);
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    if (result.onStack()) {
      char* loc = stackResultsLoc.value() + result.stackOffset();
      if (!ToJSValue<DebugCodegenVal>(cx, loc, result.type(), &tmp, level)) {
        return false;
      }
      if (!NewbornArrayPush(cx, array, tmp)) {
        return false;
      }
    } else {
      if (!NewbornArrayPush(cx, array, rval)) {
        return false;
      }
    }
  }
  rval.set(ObjectValue(*array));
  return true;
}

class MOZ_RAII ReturnToJSResultCollector {
  class MOZ_RAII StackResultsRooter : public JS::CustomAutoRooter {
    ReturnToJSResultCollector& collector_;

   public:
    StackResultsRooter(JSContext* cx, ReturnToJSResultCollector& collector)
        : JS::CustomAutoRooter(cx), collector_(collector) {}

    void trace(JSTracer* trc) final {
      for (ABIResultIter iter(collector_.type_); !iter.done(); iter.next()) {
        const ABIResult& result = iter.cur();
        if (result.onStack() && result.type().isRefRepr()) {
          char* loc = collector_.stackResultsArea_.get() + result.stackOffset();
          AnyRef* refLoc = reinterpret_cast<AnyRef*>(loc);
          TraceNullableRoot(trc, refLoc, "StackResultsRooter::trace");
        }
      }
    }
  };
  friend class StackResultsRooter;

  ResultType type_;
  UniquePtr<char[], JS::FreePolicy> stackResultsArea_;
  Maybe<StackResultsRooter> rooter_;

 public:
  explicit ReturnToJSResultCollector(const ResultType& type) : type_(type) {};
  bool init(JSContext* cx) {
    bool needRooter = false;
    ABIResultIter iter(type_);
    for (; !iter.done(); iter.next()) {
      const ABIResult& result = iter.cur();
      if (result.onStack() && result.type().isRefRepr()) {
        needRooter = true;
      }
    }
    uint32_t areaBytes = iter.stackBytesConsumedSoFar();
    MOZ_ASSERT_IF(needRooter, areaBytes > 0);
    if (areaBytes > 0) {
      // It is necessary to zero storage for ref results, and it doesn't
      // hurt to do so for other POD results.
      stackResultsArea_ = cx->make_zeroed_pod_array<char>(areaBytes);
      if (!stackResultsArea_) {
        return false;
      }
      if (needRooter) {
        rooter_.emplace(cx, *this);
      }
    }
    return true;
  }

  void* stackResultsArea() {
    MOZ_ASSERT(stackResultsArea_);
    return stackResultsArea_.get();
  }

  bool collect(JSContext* cx, void* registerResultLoc, MutableHandleValue rval,
               CoercionLevel level) {
    Maybe<char*> stackResultsLoc =
        stackResultsArea_ ? Some(stackResultsArea_.get()) : Nothing();
    return ResultsToJSValue(cx, type_, registerResultLoc, stackResultsLoc, rval,
                            level);
  }
};

/*
 * [SMDOC] Exported wasm functions and the jit-entry stubs
 *
 * ## The kinds of exported functions
 *
 * There are several kinds of exported wasm functions.  /Explicitly/ exported
 * functions are:
 *
 *  - any wasm function exported via the export section
 *  - any asm.js export
 *  - the module start function
 *
 * There are also /implicitly/ exported functions, these are the functions whose
 * indices in the module are referenced outside the code segment, eg, in element
 * segments and in global initializers.
 *
 * ## Wasm functions as JSFunctions
 *
 * Any exported function can be manipulated by JS and wasm code, and to both the
 * exported function is represented as a JSFunction.  To JS, that means that the
 * function can be called in the same way as any other JSFunction.  To Wasm, it
 * means that the function is a reference with the same representation as
 * externref.
 *
 * However, the JSFunction object is created only when the function value is
 * actually exposed to JS the first time.  The creation is performed by
 * getExportedFunction(), below, as follows:
 *
 *  - A function exported via the export section (or from asm.js) is created
 *    when the export object is created, which happens at instantiation time.
 *
 *  - A function implicitly exported via a table is created when the table
 *    element is read (by JS or wasm) and a function value is needed to
 *    represent that value.  Functions stored in tables by initializers have a
 *    special representation that does not require the function object to be
 *    created, as long as the initializing element segment uses the more
 *    efficient index encoding instead of the more general expression encoding.
 *
 *  - A function implicitly exported via a global initializer is created when
 *    the global is initialized.
 *
 *  - A function referenced from a ref.func instruction in code is created when
 *    that instruction is executed the first time.
 *
 * The JSFunction representing a wasm function never changes: every reference to
 * the wasm function that exposes the JSFunction gets the same JSFunction.  In
 * particular, imported functions already have a JSFunction representation (from
 * JS or from their home module), and will be exposed using that representation.
 *
 * The mapping from a wasm function to its JSFunction is instance-specific, and
 * held in a hashmap in the instance.  If a module is shared across multiple
 * instances, possibly in multiple threads, each instance will have its own
 * JSFunction representing the wasm function.
 *
 * ## Stubs -- interpreter, eager, lazy, provisional, and absent
 *
 * While a Wasm exported function is just a JSFunction, the internal wasm ABI is
 * neither the C++ ABI nor the JS JIT ABI, so there needs to be an extra step
 * when C++ or JS JIT code calls wasm code.  For this, execution passes through
 * a stub that is adapted to both the JS caller and the wasm callee.
 *
 * ### Interpreter stubs and jit-entry stubs
 *
 * When JS interpreted code calls a wasm function, we end up in
 * Instance::callExport() to execute the call.  This function must enter wasm,
 * and to do this it uses a stub that is specific to the wasm function (see
 * GenerateInterpEntry) that is callable with the C++ interpreter ABI and which
 * will convert arguments as necessary and enter compiled wasm code.
 *
 * The interpreter stub is created eagerly, when the module is compiled.
 *
 * However, the interpreter call path is slow, and when JS jitted code calls
 * wasm we want to do better.  In this case, there is a different, optimized
 * stub that is to be invoked, and it uses the JIT ABI.  This is the jit-entry
 * stub for the function.  Jitted code will call a wasm function's jit-entry
 * stub to invoke the function with the JIT ABI.  The stub will adapt the call
 * to the wasm ABI.
 *
 * Some jit-entry stubs are created eagerly and some are created lazily.
 *
 * ### Eager jit-entry stubs
 *
 * The explicitly exported functions have stubs created for them eagerly.  Eager
 * stubs are created with their tier when the module is compiled, see
 * ModuleGenerator::finishCodeBlock(), which calls wasm::GenerateStubs(), which
 * generates stubs for functions with eager stubs.
 *
 * An eager stub for tier-1 is upgraded to tier-2 if the module tiers up, see
 * below.
 *
 * ### Lazy jit-entry stubs
 *
 * Stubs are created lazily for all implicitly exported functions.  These
 * functions may flow out to JS, but will only need a stub if they are ever
 * called from jitted code.  (That's true for explicitly exported functions too,
 * but for them the presumption is that they will be called.)
 *
 * Lazy stubs are created only when they are needed, and they are /doubly/ lazy,
 * see getExportedFunction(), below: A function implicitly exported via a table
 * or global may be manipulated eagerly by host code without actually being
 * called (maybe ever), so we do not generate a lazy stub when the function
 * object escapes to JS, but instead delay stub generation until the function is
 * actually called.
 *
 * ### The provisional lazy jit-entry stub
 *
 * However, JS baseline compilation needs to have a stub to start with in order
 * to allow it to attach CacheIR data to the call (or it deoptimizes the call as
 * a C++ call).  Thus when the JSFunction for the wasm export is retrieved by JS
 * code, a /provisional/ lazy jit-entry stub is associated with the function.
 * The stub will invoke the wasm function on the slow interpreter path via
 * callExport - if the function is ever called - and will cause a fast jit-entry
 * stub to be created at the time of the call.  The provisional lazy stub is
 * shared globally, it contains no function-specific or context-specific data.
 *
 * Thus, the final lazy jit-entry stubs are eventually created by
 * Instance::callExport, when a call is routed through it on the slow path for
 * any of the reasons given above.
 *
 * ### Absent jit-entry stubs
 *
 * Some functions never get jit-entry stubs.  The predicate canHaveJitEntry()
 * determines if a wasm function gets a stub, and it will deny this if the
 * function's signature exposes non-JS-compatible types (such as v128) or if
 * stub optimization has been disabled by a jit option.  Calls to these
 * functions will continue to go via callExport and use the slow interpreter
 * stub.
 *
 * ## The jit-entry jump table
 *
 * The mapping from the exported function to its jit-entry stub is implemented
 * by the jit-entry jump table in the JumpTables object (see WasmCode.h).  The
 * jit-entry jump table entry for a function holds a stub that the jit can call
 * to perform fast calls.
 *
 * While there is a single contiguous jump table, it has two logical sections:
 * one for eager stubs, and one for lazy stubs.  These sections are initialized
 * and updated separately, using logic that is specific to each section.
 *
 * The value of the table element for an eager stub is a pointer to the stub
 * code in the current tier.  The pointer is installed just after the creation
 * of the stub, before any code in the module is executed.  If the module later
 * tiers up, the eager jit-entry stub for tier-1 code is replaced by one for
 * tier-2 code, see the next section.
 *
 * Initially the value of the jump table element for a lazy stub is null.
 *
 * If the function is retrieved by JS (by getExportedFunction()) and is not
 * barred from having a jit-entry, then the stub is upgraded to the shared
 * provisional lazy jit-entry stub.  This upgrade happens to be racy if the
 * module is shared, and so the update is atomic and only happens if the entry
 * is already null.  Since the provisional lazy stub is shared, this is fine; if
 * several threads try to upgrade at the same time, it is to the same shared
 * value.
 *
 * If the retrieved function is later invoked (via callExport()), the stub is
 * upgraded to an actual jit-entry stub for the current code tier, again if the
 * function is allowed to have a jit-entry.  This is not racy -- though multiple
 * threads can be trying to create a jit-entry stub at the same time, they do so
 * under a lock and only the first to take the lock will be allowed to create a
 * stub, the others will reuse the first-installed stub.
 *
 * If the module later tiers up, the lazy jit-entry stub for tier-1 code (if it
 * exists) is replaced by one for tier-2 code, see the next section.
 *
 * (Note, the InterpEntry stub is never stored in the jit-entry table, as it
 * uses the C++ ABI, not the JIT ABI.  It is accessible through the
 * FunctionEntry.)
 *
 * ### Interaction of the jit-entry jump table and tiering
 *
 * (For general info about tiering, see the comment in WasmCompile.cpp.)
 *
 * The jit-entry stub, whether eager or lazy, is specific to a code tier - a
 * stub will invoke the code for its function for the tier.  When we tier up,
 * new jit-entry stubs must be created that reference tier-2 code, and must then
 * be patched into the jit-entry table.  The complication here is that, since
 * the jump table is shared with its code between instances on multiple threads,
 * tier-1 code is running on other threads and new tier-1 specific jit-entry
 * stubs may be created concurrently with trying to create the tier-2 stubs on
 * the thread that performs the tiering-up.  Indeed, there may also be
 * concurrent attempts to upgrade null jit-entries to the provisional lazy stub.
 *
 * Eager stubs:
 *
 *  - Eager stubs for tier-2 code are patched in racily by Module::finishTier2()
 *    along with code pointers for tiering; nothing conflicts with these writes.
 *
 * Lazy stubs:
 *
 *  - An upgrade from a null entry to a lazy provisional stub is atomic and can
 *    only happen if the entry is null, and it only happens in
 *    getExportedFunction().  No lazy provisional stub will be installed if
 *    there's another stub present.
 *
 *  - The lazy tier-appropriate stub is installed by callExport() (really by
 *    EnsureEntryStubs()) during the first invocation of the exported function
 *    that reaches callExport().  That invocation must be from within JS, and so
 *    the jit-entry element can't be null, because a prior getExportedFunction()
 *    will have ensured that it is not: the lazy provisional stub will have been
 *    installed.  Hence the installing of the lazy tier-appropriate stub does
 *    not race with the installing of the lazy provisional stub.
 *
 *  - A lazy tier-1 stub is upgraded to a lazy tier-2 stub by
 *    Module::finishTier2().  The upgrade needs to ensure that all tier-1 stubs
 *    are upgraded, and that once the upgrade is finished, callExport() will
 *    only create tier-2 lazy stubs.  (This upgrading does not upgrade lazy
 *    provisional stubs or absent stubs.)
 *
 *    The locking protocol ensuring that all stubs are upgraded properly and
 *    that the system switches to creating tier-2 stubs is implemented in
 *    Module::finishTier2() and EnsureEntryStubs().
 *
 * ## Stub lifetimes and serialization
 *
 * Eager jit-entry stub code, along with stub code for import functions, is
 * serialized along with the tier-2 code for the module.
 *
 * Lazy stub code and thunks for builtin functions (including the provisional
 * lazy jit-entry stub) are never serialized.
 */

static bool WasmCall(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedFunction callee(cx, &args.callee().as<JSFunction>());

  Instance& instance = callee->wasmInstance();
  uint32_t funcIndex = callee->wasmFuncIndex();
  return instance.callExport(cx, funcIndex, args);
}

bool Instance::getExportedFunction(JSContext* cx, uint32_t funcIndex,
                                   MutableHandleFunction result) {
  uint32_t funcExportIndex = codeMeta().findFuncExportIndex(funcIndex);
  FuncExportInstanceData& instanceData =
      funcExportInstanceData(funcExportIndex);

  // Early exit if we've already found or created this exported function
  if (instanceData.func) {
    result.set(instanceData.func);
    return true;
  }

  // If this is an import, we need to recover the original function to maintain
  // reference equality between a re-exported function and 'ref.func'. The
  // identity of the imported function object is stable across tiers, which is
  // what we want.
  //
  // Use the imported function only if it is an exported function, otherwise
  // fall through to get a (possibly new) exported function.
  if (funcIndex < codeMeta().numFuncImports) {
    FuncImportInstanceData& import = funcImportInstanceData(funcIndex);
    if (import.callable->is<JSFunction>()) {
      JSFunction* fun = &import.callable->as<JSFunction>();
      if (!codeMeta().funcImportsAreJS && fun->isWasm()) {
        instanceData.func = fun;
        result.set(fun);
        return true;
      }
    }
  }

  // Otherwise this is a locally defined function which we've never created a
  // function object for yet.
  const CodeBlock& codeBlock = code().funcCodeBlock(funcIndex);
  const CodeRange& codeRange = codeBlock.codeRange(funcIndex);
  const TypeDef& funcTypeDef = codeMeta().getFuncTypeDef(funcIndex);
  unsigned numArgs = funcTypeDef.funcType().args().length();
  Instance* instance = const_cast<Instance*>(this);
  const SuperTypeVector* superTypeVector = funcTypeDef.superTypeVector();
  void* uncheckedCallEntry =
      codeBlock.base() + codeRange.funcUncheckedCallEntry();

  if (isAsmJS()) {
    // asm.js needs to act like a normal JS function which means having the
    // name from the original source and being callable as a constructor.
    Rooted<JSAtom*> name(cx, getFuncDisplayAtom(cx, funcIndex));
    if (!name) {
      return false;
    }
    result.set(NewNativeConstructor(cx, WasmCall, numArgs, name,
                                    gc::AllocKind::FUNCTION_EXTENDED,
                                    TenuredObject, FunctionFlags::ASMJS_CTOR));
    if (!result) {
      return false;
    }
    MOZ_ASSERT(result->isTenured());
    STATIC_ASSERT_WASM_FUNCTIONS_TENURED;

    // asm.js does not support jit entries.
    result->initWasm(funcIndex, instance, superTypeVector, uncheckedCallEntry);
  } else {
    Rooted<JSAtom*> name(cx, NumberToAtom(cx, funcIndex));
    if (!name) {
      return false;
    }
    RootedObject proto(cx);
#ifdef ENABLE_WASM_TYPE_REFLECTIONS
    proto = GlobalObject::getOrCreatePrototype(cx, JSProto_WasmFunction);
    if (!proto) {
      return false;
    }
#endif
    result.set(NewFunctionWithProto(
        cx, WasmCall, numArgs, FunctionFlags::WASM, nullptr, name, proto,
        gc::AllocKind::FUNCTION_EXTENDED, TenuredObject));
    if (!result) {
      return false;
    }
    MOZ_ASSERT(result->isTenured());
    STATIC_ASSERT_WASM_FUNCTIONS_TENURED;

    // Some applications eagerly access all table elements which currently
    // triggers worst-case behavior for lazy stubs, since each will allocate a
    // separate 4kb code page. Most eagerly-accessed functions are not called,
    // so use a shared, provisional (and slow) lazy stub as JitEntry and wait
    // until Instance::callExport() to create the fast entry stubs.
    if (funcTypeDef.funcType().canHaveJitEntry()) {
      const FuncExport& funcExport = codeBlock.lookupFuncExport(funcIndex);
      if (!funcExport.hasEagerStubs()) {
        if (!EnsureBuiltinThunksInitialized()) {
          return false;
        }
        void* provisionalLazyJitEntryStub = ProvisionalLazyJitEntryStub();
        MOZ_ASSERT(provisionalLazyJitEntryStub);
        code().setJitEntryIfNull(funcIndex, provisionalLazyJitEntryStub);
      }
      result->initWasmWithJitEntry(code().getAddressOfJitEntry(funcIndex),
                                   instance, superTypeVector,
                                   uncheckedCallEntry);
    } else {
      result->initWasm(funcIndex, instance, superTypeVector,
                       uncheckedCallEntry);
    }
  }

  instanceData.func = result;
  return true;
}

bool Instance::callExport(JSContext* cx, uint32_t funcIndex,
                          const CallArgs& args, CoercionLevel level) {
  if (memory0Base_) {
    // If there has been a moving grow, this Instance should have been notified.
    MOZ_RELEASE_ASSERT(memoryBase(0).unwrap() == memory0Base_);
  }

  void* interpEntry;
  const FuncType* funcType;
  if (!GetInterpEntryAndEnsureStubs(cx, *this, funcIndex, args, &interpEntry,
                                    &funcType)) {
    return false;
  }

  // Lossless coercions can handle unexposable arguments or returns. This is
  // only available in testing code.
  if (level != CoercionLevel::Lossless && funcType->hasUnexposableArgOrRet()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }

  ArgTypeVector argTypes(*funcType);
  ResultType resultType(ResultType::Vector(funcType->results()));
  ReturnToJSResultCollector results(resultType);
  if (!results.init(cx)) {
    return false;
  }

  // The calling convention for an external call into wasm is to pass an
  // array of 16-byte values where each value contains either a coerced int32
  // (in the low word), or a double value (in the low dword) value, with the
  // coercions specified by the wasm signature. The external entry point
  // unpacks this array into the system-ABI-specified registers and stack
  // memory and then calls into the internal entry point. The return value is
  // stored in the first element of the array (which, therefore, must have
  // length >= 1).
  Vector<ExportArg, 8> exportArgs(cx);
  if (!exportArgs.resize(
          std::max<size_t>(1, argTypes.lengthWithStackResults()))) {
    return false;
  }

  Rooted<GCVector<AnyRef, 8, SystemAllocPolicy>> refs(cx);

  DebugCodegen(DebugChannel::Function, "wasm-function[%d] arguments [",
               funcIndex);
  RootedValue v(cx);
  for (size_t i = 0; i < argTypes.lengthWithStackResults(); ++i) {
    void* rawArgLoc = &exportArgs[i];
    if (argTypes.isSyntheticStackResultPointerArg(i)) {
      *reinterpret_cast<void**>(rawArgLoc) = results.stackResultsArea();
      continue;
    }
    size_t naturalIdx = argTypes.naturalIndex(i);
    v = naturalIdx < args.length() ? args[naturalIdx] : UndefinedValue();
    ValType type = funcType->arg(naturalIdx);
    if (!ToWebAssemblyValue<DebugCodegenVal>(cx, v, type, rawArgLoc, true,
                                             level)) {
      return false;
    }
    if (type.isRefRepr()) {
      void* ptr = *reinterpret_cast<void**>(rawArgLoc);
      // Store in rooted array until no more GC is possible.
      RootedAnyRef ref(cx, AnyRef::fromCompiledCode(ptr));
      if (!refs.emplaceBack(ref.get())) {
        return false;
      }
      DebugCodegen(DebugChannel::Function, "/(#%d)", int(refs.length() - 1));
    }
  }

  // Copy over reference values from the rooted array, if any.
  if (refs.length() > 0) {
    DebugCodegen(DebugChannel::Function, "; ");
    size_t nextRef = 0;
    for (size_t i = 0; i < argTypes.lengthWithStackResults(); ++i) {
      if (argTypes.isSyntheticStackResultPointerArg(i)) {
        continue;
      }
      size_t naturalIdx = argTypes.naturalIndex(i);
      ValType type = funcType->arg(naturalIdx);
      if (type.isRefRepr()) {
        AnyRef* rawArgLoc = (AnyRef*)&exportArgs[i];
        *rawArgLoc = refs[nextRef++];
        DebugCodegen(DebugChannel::Function, " ref(#%d) := %p ",
                     int(nextRef - 1), *(void**)rawArgLoc);
      }
    }
    refs.clear();
  }

  DebugCodegen(DebugChannel::Function, "]\n");

  // Ensure pending exception is cleared before and after (below) call.
  MOZ_ASSERT(pendingException_.isNull());

  {
    JitActivation activation(cx);

    // Call the per-exported-function trampoline created by GenerateEntry.
    auto funcPtr = JS_DATA_TO_FUNC_PTR(ExportFuncPtr, interpEntry);
    if (!CALL_GENERATED_2(funcPtr, exportArgs.begin(), this)) {
      return false;
    }
  }

  MOZ_ASSERT(pendingException_.isNull());

  if (isAsmJS() && args.isConstructing()) {
    // By spec, when a JS function is called as a constructor and this
    // function returns a primary type, which is the case for all asm.js
    // exported functions, the returned value is discarded and an empty
    // object is returned instead.
    PlainObject* obj = NewPlainObject(cx);
    if (!obj) {
      return false;
    }
    args.rval().set(ObjectValue(*obj));
    return true;
  }

  // Note that we're not rooting the register result, if any; we depend
  // on ResultsCollector::collect to root the value on our behalf,
  // before causing any GC.
  void* registerResultLoc = &exportArgs[0];
  DebugCodegen(DebugChannel::Function, "wasm-function[%d]; results [",
               funcIndex);
  if (!results.collect(cx, registerResultLoc, args.rval(), level)) {
    return false;
  }
  DebugCodegen(DebugChannel::Function, "]\n");

  return true;
}

void Instance::setPendingException(Handle<WasmExceptionObject*> exn) {
  pendingException_ = AnyRef::fromJSObject(*exn.get());
  pendingExceptionTag_ =
      AnyRef::fromJSObject(exn->as<WasmExceptionObject>().tag());
}

void Instance::constantGlobalGet(uint32_t globalIndex,
                                 MutableHandleVal result) {
  MOZ_RELEASE_ASSERT(globalIndex < maxInitializedGlobalsIndexPlus1_);
  const GlobalDesc& global = codeMeta().globals[globalIndex];

  // Constant globals are baked into the code and never stored in global data.
  if (global.isConstant()) {
    // We can just re-evaluate the global initializer to get the value.
    result.set(Val(global.constantValue()));
    return;
  }

  // Otherwise, we need to load the initialized value from its cell.
  const void* cell = addressOfGlobalCell(global);
  result.address()->initFromHeapLocation(global.type(), cell);
}

WasmStructObject* Instance::constantStructNewDefault(JSContext* cx,
                                                     uint32_t typeIndex) {
  // We assume that constant structs will have a long lifetime and hence
  // allocate them directly in the tenured heap.  Also, we have to dynamically
  // decide whether an OOL storage area is required.  This is slow(er); do not
  // call here from generated code.
  TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);
  const wasm::TypeDef* typeDef = typeDefData->typeDef;
  MOZ_ASSERT(typeDef->kind() == wasm::TypeDefKind::Struct);
  uint32_t totalBytes = typeDef->structType().size_;

  bool needsOOL = WasmStructObject::requiresOutlineBytes(totalBytes);
  return needsOOL ? WasmStructObject::createStructOOL<true>(
                        cx, typeDefData, nullptr, gc::Heap::Tenured)
                  : WasmStructObject::createStructIL<true>(
                        cx, typeDefData, nullptr, gc::Heap::Tenured);
}

WasmArrayObject* Instance::constantArrayNewDefault(JSContext* cx,
                                                   uint32_t typeIndex,
                                                   uint32_t numElements) {
  TypeDefInstanceData* typeDefData = typeDefInstanceData(typeIndex);
  // We assume that constant arrays will have a long lifetime and hence
  // allocate them directly in the tenured heap.
  return WasmArrayObject::createArray<true>(cx, typeDefData, nullptr,
                                            gc::Heap::Tenured, numElements);
}

JSAtom* Instance::getFuncDisplayAtom(JSContext* cx, uint32_t funcIndex) const {
  // The "display name" of a function is primarily shown in Error.stack which
  // also includes location, so use getFuncNameBeforeLocation.
  UTF8Bytes name;
  bool ok;
  if (codeMetaForAsmJS()) {
    ok = codeMetaForAsmJS()->getFuncNameForAsmJS(funcIndex, &name);
  } else {
    ok = codeMeta().getFuncNameForWasm(NameContext::BeforeLocation, funcIndex,
                                       codeTailMeta().nameSectionPayload.get(),
                                       &name);
  }
  if (!ok) {
    return nullptr;
  }

  return AtomizeUTF8Chars(cx, name.begin(), name.length());
}

void Instance::ensureProfilingLabels(bool profilingEnabled) const {
  return code_->ensureProfilingLabels(profilingEnabled);
}

void Instance::onMovingGrowMemory(const WasmMemoryObject* memory) {
  MOZ_ASSERT(!isAsmJS());
  MOZ_ASSERT(!memory->isShared());

  for (uint32_t i = 0; i < codeMeta().memories.length(); i++) {
    MemoryInstanceData& md = memoryInstanceData(i);
    if (memory != md.memory) {
      continue;
    }
    ArrayBufferObject& buffer = md.memory->buffer().as<ArrayBufferObject>();

    md.base = buffer.dataPointer();
    size_t limit = md.memory->boundsCheckLimit();
#if !defined(JS_64BIT)
    // We assume that the limit is a 32-bit quantity
    MOZ_ASSERT(limit <= UINT32_MAX);
#endif
    md.boundsCheckLimit = limit;

    if (i == 0) {
      memory0Base_ = md.base;
      memory0BoundsCheckLimit_ = md.boundsCheckLimit;
    }
  }
}

void Instance::onMovingGrowTable(const Table* table) {
  MOZ_ASSERT(!isAsmJS());

  // `table` has grown and we must update cached data for it.  Importantly,
  // we can have cached those data in more than one location: we'll have
  // cached them once for each time the table was imported into this instance.
  //
  // When an instance is registered as an observer of a table it is only
  // registered once, regardless of how many times the table was imported.
  // Thus when a table is grown, onMovingGrowTable() is only invoked once for
  // the table.
  //
  // Ergo we must go through the entire list of tables in the instance here
  // and check for the table in all the cached-data slots; we can't exit after
  // the first hit.

  for (uint32_t i = 0; i < tables_.length(); i++) {
    if (tables_[i] != table) {
      continue;
    }
    TableInstanceData& table = tableInstanceData(i);
    table.length = tables_[i]->length();
    table.elements = tables_[i]->instanceElements();
  }
}

JSString* Instance::createDisplayURL(JSContext* cx) {
  // In the best case, we simply have a URL, from a streaming compilation of a
  // fetched Response.

  if (codeMeta().scriptedCaller().filenameIsURL) {
    const char* filename = codeMeta().scriptedCaller().filename.get();
    return NewStringCopyUTF8N(cx, JS::UTF8Chars(filename, strlen(filename)));
  }

  // Otherwise, build wasm module URL from following parts:
  // - "wasm:" as protocol;
  // - URI encoded filename from metadata (if can be encoded), plus ":";
  // - 64-bit hash of the module bytes (as hex dump).

  JSStringBuilder result(cx);
  if (!result.append("wasm:")) {
    return nullptr;
  }

  if (const char* filename = codeMeta().scriptedCaller().filename.get()) {
    // EncodeURI returns false due to invalid chars or OOM -- fail only
    // during OOM.
    JSString* filenamePrefix = EncodeURI(cx, filename, strlen(filename));
    if (!filenamePrefix) {
      if (cx->isThrowingOutOfMemory()) {
        return nullptr;
      }

      MOZ_ASSERT(!cx->isThrowingOverRecursed());
      cx->clearPendingException();
      return nullptr;
    }

    if (!result.append(filenamePrefix)) {
      return nullptr;
    }
  }

  if (code().debugEnabled()) {
    if (!result.append(":")) {
      return nullptr;
    }

    const ModuleHash& hash = codeTailMeta().debugHash;
    for (unsigned char byte : hash) {
      unsigned char digit1 = byte / 16, digit2 = byte % 16;
      if (!result.append(
              (char)(digit1 < 10 ? digit1 + '0' : digit1 + 'a' - 10))) {
        return nullptr;
      }
      if (!result.append(
              (char)(digit2 < 10 ? digit2 + '0' : digit2 + 'a' - 10))) {
        return nullptr;
      }
    }
  }

  return result.finishString();
}

WasmBreakpointSite* Instance::getOrCreateBreakpointSite(JSContext* cx,
                                                        uint32_t offset) {
  MOZ_ASSERT(debugEnabled());
  return debug().getOrCreateBreakpointSite(cx, this, offset);
}

void Instance::destroyBreakpointSite(JS::GCContext* gcx, uint32_t offset) {
  MOZ_ASSERT(debugEnabled());
  return debug().destroyBreakpointSite(gcx, this, offset);
}

void Instance::disassembleExport(JSContext* cx, uint32_t funcIndex, Tier tier,
                                 PrintCallback printString) const {
  const CodeBlock& codeBlock = code().funcCodeBlock(funcIndex);
  const FuncExport& funcExport = codeBlock.lookupFuncExport(funcIndex);
  const CodeRange& range = codeBlock.codeRange(funcExport);

  MOZ_ASSERT(range.begin() < codeBlock.length());
  MOZ_ASSERT(range.end() < codeBlock.length());

  uint8_t* functionCode = codeBlock.base() + range.begin();
  jit::Disassemble(functionCode, range.end() - range.begin(), printString);
}

void Instance::addSizeOfMisc(
    mozilla::MallocSizeOf mallocSizeOf, CodeMetadata::SeenSet* seenCodeMeta,
    CodeMetadataForAsmJS::SeenSet* seenCodeMetaForAsmJS,
    Code::SeenSet* seenCode, Table::SeenSet* seenTables, size_t* code,
    size_t* data) const {
  *data += mallocSizeOf(this);
  for (const SharedTable& table : tables_) {
    *data += table->sizeOfIncludingThisIfNotSeen(mallocSizeOf, seenTables);
  }

  if (maybeDebug_) {
    maybeDebug_->addSizeOfMisc(mallocSizeOf, seenCodeMeta, seenCodeMetaForAsmJS,
                               seenCode, code, data);
  }

  code_->addSizeOfMiscIfNotSeen(mallocSizeOf, seenCodeMeta,
                                seenCodeMetaForAsmJS, seenCode, code, data);
}

//////////////////////////////////////////////////////////////////////////////
//
// Reporting of errors that are traps.

void wasm::MarkPendingExceptionAsTrap(JSContext* cx) {
  RootedValue exn(cx);
  if (!cx->getPendingException(&exn)) {
    return;
  }

  if (cx->isThrowingOutOfMemory()) {
    return;
  }

  MOZ_RELEASE_ASSERT(exn.isObject() && exn.toObject().is<ErrorObject>());
  exn.toObject().as<ErrorObject>().setFromWasmTrap();
}

void wasm::ReportTrapError(JSContext* cx, unsigned errorNumber) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber);

  if (cx->isThrowingOutOfMemory()) {
    return;
  }

  // Mark the exception as thrown from a trap to prevent if from being handled
  // by wasm exception handlers.
  MarkPendingExceptionAsTrap(cx);
}
