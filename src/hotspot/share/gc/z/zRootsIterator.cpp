/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "classfile/classLoaderData.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "compiler/oopMap.hpp"
#include "gc/shared/oopStorageParState.inline.hpp"
#include "gc/z/zNMethodTable.hpp"
#include "gc/z/zOopClosures.inline.hpp"
#include "gc/z/zRootsIterator.hpp"
#include "gc/z/zStat.hpp"
#include "memory/universe.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/atomic.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/thread.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/synchronizer.hpp"
#include "services/management.hpp"
#include "trace/traceMacros.hpp"
#include "utilities/debug.hpp"

static const ZStatSubPhase ZSubPhasePauseRootsSetup("Pause Roots Setup");
static const ZStatSubPhase ZSubPhasePauseRoots("Pause Roots");
static const ZStatSubPhase ZSubPhasePauseRootsTeardown("Pause Roots Teardown");
static const ZStatSubPhase ZSubPhasePauseRootsUniverse("Pause Roots Universe");
static const ZStatSubPhase ZSubPhasePauseRootsJNIHandles("Pause Roots JNIHandles");
static const ZStatSubPhase ZSubPhasePauseRootsJNIWeakHandles("Pause Roots JNIWeakHandles");
static const ZStatSubPhase ZSubPhasePauseRootsObjectSynchronizer("Pause Roots ObjectSynchronizer");
static const ZStatSubPhase ZSubPhasePauseRootsManagement("Pause Roots Management");
static const ZStatSubPhase ZSubPhasePauseRootsJVMTIExport("Pause Roots JVMTIExport");
static const ZStatSubPhase ZSubPhasePauseRootsJVMTIWeakExport("Pause Roots JVMTIWeakExport");
static const ZStatSubPhase ZSubPhasePauseRootsTrace("Pause Roots Trace");
static const ZStatSubPhase ZSubPhasePauseRootsSystemDictionary("Pause Roots SystemDictionary");
static const ZStatSubPhase ZSubPhasePauseRootsClassLoaderDataGraph("Pause Roots ClassLoaderDataGraph");
static const ZStatSubPhase ZSubPhasePauseRootsThreads("Pause Roots Threads");
static const ZStatSubPhase ZSubPhasePauseRootsCodeCache("Pause Roots CodeCache");
static const ZStatSubPhase ZSubPhasePauseRootsStringTable("Pause Roots StringTable");

static const ZStatSubPhase ZSubPhasePauseWeakRootsSetup("Pause Weak Roots Setup");
static const ZStatSubPhase ZSubPhasePauseWeakRoots("Pause Weak Roots");
static const ZStatSubPhase ZSubPhasePauseWeakRootsTeardown("Pause Weak Roots Teardown");
static const ZStatSubPhase ZSubPhasePauseWeakRootsJNIWeakHandles("Pause Weak Roots JNIWeakHandles");
static const ZStatSubPhase ZSubPhasePauseWeakRootsJVMTIWeakExport("Pause Weak Roots JVMTIWeakExport");
static const ZStatSubPhase ZSubPhasePauseWeakRootsTrace("Pause Weak Roots Trace");
static const ZStatSubPhase ZSubPhasePauseWeakRootsSymbolTable("Pause Weak Roots SymbolTable");
static const ZStatSubPhase ZSubPhasePauseWeakRootsStringTable("Pause Weak Roots StringTable");

static const ZStatSubPhase ZSubPhaseConcurrentWeakRoots("Concurrent Weak Roots");
static const ZStatSubPhase ZSubPhaseConcurrentWeakRootsJNIWeakHandles("Concurrent Weak Roots JNIWeakHandles");

template <typename T, void (T::*F)(OopClosure*)>
ZSerialOopsDo<T, F>::ZSerialOopsDo(T* iter) :
    _iter(iter),
    _claimed(false) {}

template <typename T, void (T::*F)(OopClosure*)>
void ZSerialOopsDo<T, F>::oops_do(OopClosure* cl) {
  if (!_claimed && Atomic::cmpxchg(true, &_claimed, false) == false) {
    (_iter->*F)(cl);
  }
}

template <typename T, void (T::*F)(OopClosure*)>
ZParallelOopsDo<T, F>::ZParallelOopsDo(T* iter) :
    _iter(iter),
    _completed(false) {}

template <typename T, void (T::*F)(OopClosure*)>
void ZParallelOopsDo<T, F>::oops_do(OopClosure* cl) {
  if (!_completed) {
    (_iter->*F)(cl);
    if (!_completed) {
      _completed = true;
    }
  }
}

template <typename T, void (T::*F)(BoolObjectClosure*, OopClosure*)>
ZSerialUnlinkOrOopsDo<T, F>::ZSerialUnlinkOrOopsDo(T* iter) :
    _iter(iter),
    _claimed(false) {}

template <typename T, void (T::*F)(BoolObjectClosure*, OopClosure*)>
void ZSerialUnlinkOrOopsDo<T, F>::unlink_or_oops_do(BoolObjectClosure* is_alive, OopClosure* cl) {
  if (!_claimed && Atomic::cmpxchg(true, &_claimed, false) == false) {
    (_iter->*F)(is_alive, cl);
  }
}

template <typename T, void (T::*F)(BoolObjectClosure*, OopClosure*)>
ZParallelUnlinkOrOopsDo<T, F>::ZParallelUnlinkOrOopsDo(T* iter) :
    _iter(iter),
    _completed(false) {}

template <typename T, void (T::*F)(BoolObjectClosure*, OopClosure*)>
void ZParallelUnlinkOrOopsDo<T, F>::unlink_or_oops_do(BoolObjectClosure* is_alive, OopClosure* cl) {
  if (!_completed) {
    (_iter->*F)(is_alive, cl);
    if (!_completed) {
      _completed = true;
    }
  }
}

ZRootsIterator::ZRootsIterator() :
    _universe(this),
    _jni_handles(this),
    _jni_weak_handles(this),
    _object_synchronizer(this),
    _management(this),
    _jvmti_export(this),
    _jvmti_weak_export(this),
    _trace(this),
    _system_dictionary(this),
    _class_loader_data_graph(this),
    _threads(this),
    _code_cache(this),
    _string_table(this) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");
  ZStatTimer timer(ZSubPhasePauseRootsSetup);
  Threads::change_thread_claim_parity();
  StringTable::clear_parallel_claimed_index();
  ClassLoaderDataGraph::clear_claimed_marks();
  COMPILER2_PRESENT(DerivedPointerTable::clear());
  CodeCache::gc_prologue();
  ZNMethodTable::gc_prologue();
}

ZRootsIterator::~ZRootsIterator() {
  ZStatTimer timer(ZSubPhasePauseRootsTeardown);
  ResourceMark rm;
  ZNMethodTable::gc_epilogue();
  CodeCache::gc_epilogue();
  JvmtiExport::gc_epilogue();
  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
  Threads::assert_all_threads_claimed();
}

void ZRootsIterator::do_universe(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsUniverse);
  Universe::oops_do(cl);
}

void ZRootsIterator::do_jni_handles(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsJNIHandles);
  JNIHandles::oops_do(cl);
}

void ZRootsIterator::do_jni_weak_handles(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsJNIWeakHandles);
  JNIHandles::weak_oops_do(cl);
}

void ZRootsIterator::do_object_synchronizer(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsObjectSynchronizer);
  ObjectSynchronizer::oops_do(cl);
}

void ZRootsIterator::do_management(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsManagement);
  Management::oops_do(cl);
}

void ZRootsIterator::do_jvmti_export(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsJVMTIExport);
  JvmtiExport::oops_do(cl);
}

void ZRootsIterator::do_jvmti_weak_export(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsJVMTIWeakExport);
  AlwaysTrueClosure always_alive;
  JvmtiExport::weak_oops_do(&always_alive, cl);
}

void ZRootsIterator::do_trace(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsTrace);
  AlwaysTrueClosure always_alive;
  TRACE_WEAK_OOPS_DO(&always_alive, cl);
}

void ZRootsIterator::do_system_dictionary(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsSystemDictionary);
  SystemDictionary::oops_do(cl);
}

void ZRootsIterator::do_class_loader_data_graph(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsClassLoaderDataGraph);
  CLDToOopClosure cld_cl(cl);
  ClassLoaderDataGraph::cld_do(&cld_cl);
}

void ZRootsIterator::do_threads(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsThreads);
  ResourceMark rm;
  Threads::possibly_parallel_oops_do(true, cl, NULL);
}

void ZRootsIterator::do_code_cache(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsCodeCache);
  ZNMethodTable::oops_do(cl);
}

void ZRootsIterator::do_string_table(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsStringTable);
  StringTable::possibly_parallel_oops_do(cl);
}

void ZRootsIterator::oops_do(OopClosure* cl, bool visit_jvmti_weak_export) {
  ZStatTimer timer(ZSubPhasePauseRoots);
  _universe.oops_do(cl);
  _jni_handles.oops_do(cl);
  _object_synchronizer.oops_do(cl);
  _management.oops_do(cl);
  _jvmti_export.oops_do(cl);
  _system_dictionary.oops_do(cl);
  _class_loader_data_graph.oops_do(cl);
  _threads.oops_do(cl);
  _code_cache.oops_do(cl);
  if (!ZWeakRoots) {
    _jni_weak_handles.oops_do(cl);
    _jvmti_weak_export.oops_do(cl);
    _trace.oops_do(cl);
    _string_table.oops_do(cl);
  } else {
    if (visit_jvmti_weak_export) {
      _jvmti_weak_export.oops_do(cl);
    }
  }
}

ZWeakRootsIterator::ZWeakRootsIterator() :
    _jni_weak_handles(this),
    _jvmti_weak_export(this),
    _trace(this),
    _symbol_table(this),
    _string_table(this) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");
  ZStatTimer timer(ZSubPhasePauseWeakRootsSetup);
  SymbolTable::clear_parallel_claimed_index();
  StringTable::clear_parallel_claimed_index();
}

ZWeakRootsIterator::~ZWeakRootsIterator() {
  ZStatTimer timer(ZSubPhasePauseWeakRootsTeardown);
}

void ZWeakRootsIterator::do_jni_weak_handles(BoolObjectClosure* is_alive, OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseWeakRootsJNIWeakHandles);
  JNIHandles::weak_oops_do(is_alive, cl);
}

void ZWeakRootsIterator::do_jvmti_weak_export(BoolObjectClosure* is_alive, OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseWeakRootsJVMTIWeakExport);
  JvmtiExport::weak_oops_do(is_alive, cl);
}

void ZWeakRootsIterator::do_trace(BoolObjectClosure* is_alive, OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseWeakRootsTrace);
  TRACE_WEAK_OOPS_DO(is_alive, cl);
}

void ZWeakRootsIterator::do_symbol_table(BoolObjectClosure* is_alive, OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseWeakRootsSymbolTable);
  int dummy;
  SymbolTable::possibly_parallel_unlink(&dummy, &dummy);
}

void ZWeakRootsIterator::do_string_table(BoolObjectClosure* is_alive, OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseWeakRootsStringTable);
  int dummy;
  StringTable::possibly_parallel_unlink_or_oops_do(is_alive, cl, &dummy, &dummy);
}

void ZWeakRootsIterator::unlink_or_oops_do(BoolObjectClosure* is_alive, OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseWeakRoots);
  _symbol_table.unlink_or_oops_do(is_alive, cl);
  if (ZWeakRoots) {
    if (!ZConcurrentJNIWeakGlobalHandles) {
      _jni_weak_handles.unlink_or_oops_do(is_alive, cl);
    }
    _jvmti_weak_export.unlink_or_oops_do(is_alive, cl);
    _trace.unlink_or_oops_do(is_alive, cl);
    _string_table.unlink_or_oops_do(is_alive, cl);
  }
}

void ZWeakRootsIterator::oops_do(OopClosure* cl) {
  AlwaysTrueClosure always_alive;
  unlink_or_oops_do(&always_alive, cl);
}

void ZConcurrentWeakRootsIterator::do_jni_weak_handles(OopClosure* cl) {
  ZStatTimer timer(ZSubPhaseConcurrentWeakRootsJNIWeakHandles);
  _par_state.oops_do(cl);
}

ZConcurrentWeakRootsIterator::ZConcurrentWeakRootsIterator() :
    _par_state(JNIHandles::weak_global_handles()),
    _jni_weak_handles(this) {}

void ZConcurrentWeakRootsIterator::oops_do(OopClosure* cl) {
  ZStatTimer timer(ZSubPhaseConcurrentWeakRoots);
  if (ZWeakRoots) {
    if (ZConcurrentJNIWeakGlobalHandles) {
      _jni_weak_handles.oops_do(cl);
    }
  }
}

ZThreadRootsIterator::ZThreadRootsIterator() :
    _threads(this) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");
  ZStatTimer timer(ZSubPhasePauseRootsSetup);
  Threads::change_thread_claim_parity();
}

ZThreadRootsIterator::~ZThreadRootsIterator() {
  ZStatTimer timer(ZSubPhasePauseRootsTeardown);
  Threads::assert_all_threads_claimed();
}

void ZThreadRootsIterator::do_threads(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRootsThreads);
  ResourceMark rm;
  Threads::possibly_parallel_oops_do(true, cl, NULL);
}

void ZThreadRootsIterator::oops_do(OopClosure* cl) {
  ZStatTimer timer(ZSubPhasePauseRoots);
  _threads.oops_do(cl);
}
