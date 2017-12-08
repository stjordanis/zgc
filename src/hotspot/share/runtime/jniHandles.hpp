/*
 * Copyright (c) 1998, 2018, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_VM_RUNTIME_JNIHANDLES_HPP
#define SHARE_VM_RUNTIME_JNIHANDLES_HPP

#include "memory/allocation.hpp"
#include "runtime/handles.hpp"

class JNIHandleBlock;
class OopStorage;


// Interface for creating and resolving local/global JNI handles

class JNIHandles : AllStatic {
  friend class VMStructs;
 private:
  static OopStorage* _global_handles;
  static OopStorage* _weak_global_handles;

  inline static bool is_jweak(jobject handle);
  inline static oop& jobject_ref(jobject handle); // NOT jweak!
  inline static oop* jweak_ref_addr(jobject handle);
  inline static oop& jweak_ref(jobject handle);

  template<bool external_guard> inline static oop resolve_impl(jobject handle);
  static oop resolve_jweak(jweak handle);

  // This method is not inlined in order to avoid circular includes between
  // this header file and thread.hpp.
  static bool current_thread_in_native();

 public:
  // Low tag bit in jobject used to distinguish a jweak.  jweak is
  // type equivalent to jobject, but there are places where we need to
  // be able to distinguish jweak values from other jobjects, and
  // is_weak_global_handle is unsuitable for performance reasons.  To
  // provide such a test we add weak_tag_value to the (aligned) byte
  // address designated by the jobject to produce the corresponding
  // jweak.  Accessing the value of a jobject must account for it
  // being a possibly offset jweak.
  static const uintptr_t weak_tag_size = 1;
  static const uintptr_t weak_tag_alignment = (1u << weak_tag_size);
  static const uintptr_t weak_tag_mask = weak_tag_alignment - 1;
  static const int weak_tag_value = 1;

  // Resolve handle into oop
  inline static oop resolve(jobject handle);
  // Resolve externally provided handle into oop with some guards
  inline static oop resolve_external_guard(jobject handle);
  // Resolve handle into oop, result guaranteed not to be null
  inline static oop resolve_non_null(jobject handle);

  // Local handles
  static jobject make_local(oop obj);
  static jobject make_local(JNIEnv* env, oop obj);    // Fast version when env is known
  static jobject make_local(Thread* thread, oop obj); // Even faster version when current thread is known
  inline static void destroy_local(jobject handle);

  // Global handles
  static jobject make_global(Handle  obj, AllocFailType alloc_failmode = AllocFailStrategy::EXIT_OOM);
  static void destroy_global(jobject handle);

  // Weak global handles
  static jobject make_weak_global(Handle obj, AllocFailType alloc_failmode = AllocFailStrategy::EXIT_OOM);
  static void destroy_weak_global(jobject handle);
  static bool is_global_weak_cleared(jweak handle); // Test jweak without resolution

  // Initialization
  static void initialize();

  // Debugging
  static void print_on(outputStream* st);
  static void print()           { print_on(tty); }
  static void verify();
  // The category predicates all require handle != NULL.
  static bool is_local_handle(Thread* thread, jobject handle);
  static bool is_frame_handle(JavaThread* thread, jobject handle);
  static bool is_global_handle(jobject handle);
  static bool is_weak_global_handle(jobject handle);
  static size_t global_handle_memory_usage();
  static size_t weak_global_handle_memory_usage();

#ifndef PRODUCT
  // Is handle from any local block of any thread?
  static bool is_local_handle(jobject handle);
#endif

  // precondition: handle != NULL.
  static jobjectRefType handle_type(Thread* thread, jobject handle);

  // Garbage collection support(global handles only, local handles are traversed from thread)
  // Traversal of regular global handles
  static void oops_do(OopClosure* f);
  // Traversal of weak global handles. Unreachable oops are cleared.
  static void weak_oops_do(BoolObjectClosure* is_alive, OopClosure* f);
  // Traversal of weak global handles.
  static void weak_oops_do(OopClosure* f);
  // Expose the _weak_globa_handles to the GCs that want to do more exotic iterations.
  static OopStorage* weak_global_handles();
};



// JNI handle blocks holding local/global JNI handles

class JNIHandleBlock : public CHeapObj<mtInternal> {
  friend class VMStructs;
  friend class CppInterpreter;

 private:
  enum SomeConstants {
    block_size_in_oops  = 32                    // Number of handles per handle block
  };

  oop             _handles[block_size_in_oops]; // The handles
  int             _top;                         // Index of next unused handle
  JNIHandleBlock* _next;                        // Link to next block

  // The following instance variables are only used by the first block in a chain.
  // Having two types of blocks complicates the code and the space overhead in negligible.
  JNIHandleBlock* _last;                        // Last block in use
  JNIHandleBlock* _pop_frame_link;              // Block to restore on PopLocalFrame call
  oop*            _free_list;                   // Handle free list
  int             _allocate_before_rebuild;     // Number of blocks to allocate before rebuilding free list

  // Check JNI, "planned capacity" for current frame (or push/ensure)
  size_t          _planned_capacity;

  #ifndef PRODUCT
  JNIHandleBlock* _block_list_link;             // Link for list below
  static JNIHandleBlock* _block_list;           // List of all allocated blocks (for debugging only)
  #endif

  static JNIHandleBlock* _block_free_list;      // Free list of currently unused blocks
  static int      _blocks_allocated;            // For debugging/printing

  // Fill block with bad_handle values
  void zap() NOT_DEBUG_RETURN;

  // Free list computation
  void rebuild_free_list();

  // No more handles in the both the current and following blocks
  void clear() { _top = 0; }

 public:
  // Handle allocation
  jobject allocate_handle(oop obj);

  // Block allocation and block free list management
  static JNIHandleBlock* allocate_block(Thread* thread = NULL);
  static void release_block(JNIHandleBlock* block, Thread* thread = NULL);

  // JNI PushLocalFrame/PopLocalFrame support
  JNIHandleBlock* pop_frame_link() const          { return _pop_frame_link; }
  void set_pop_frame_link(JNIHandleBlock* block)  { _pop_frame_link = block; }

  // Stub generator support
  static int top_offset_in_bytes()                { return offset_of(JNIHandleBlock, _top); }

  // Garbage collection support
  // Traversal of handles
  void oops_do(OopClosure* f);

  // Checked JNI support
  void set_planned_capacity(size_t planned_capacity) { _planned_capacity = planned_capacity; }
  const size_t get_planned_capacity() { return _planned_capacity; }
  const size_t get_number_of_live_handles();

  // Debugging
  bool chain_contains(jobject handle) const;    // Does this block or following blocks contain handle
  bool contains(jobject handle) const;          // Does this block contain handle
  size_t length() const;                        // Length of chain starting with this block
  size_t memory_usage() const;
  #ifndef PRODUCT
  static bool any_contains(jobject handle);     // Does any block currently in use contain handle
  static void print_statistics();
  #endif
};

inline bool JNIHandles::is_jweak(jobject handle) {
  STATIC_ASSERT(weak_tag_size == 1);
  STATIC_ASSERT(weak_tag_value == 1);
  return (reinterpret_cast<uintptr_t>(handle) & weak_tag_mask) != 0;
}

inline oop& JNIHandles::jobject_ref(jobject handle) {
  assert(!is_jweak(handle), "precondition");
  return *reinterpret_cast<oop*>(handle);
}

inline oop& JNIHandles::jweak_ref(jobject handle) {
  return *jweak_ref_addr(handle);
}

inline oop* JNIHandles::jweak_ref_addr(jobject handle) {
  assert(is_jweak(handle), "precondition");
  char* ptr = reinterpret_cast<char*>(handle) - weak_tag_value;
  return reinterpret_cast<oop*>(ptr);
}

// external_guard is true if called from resolve_external_guard.
template<bool external_guard>
inline oop JNIHandles::resolve_impl(jobject handle) {
  assert(handle != NULL, "precondition");
  assert(!current_thread_in_native(), "must not be in native");
  oop result;
  if (is_jweak(handle)) {       // Unlikely
    result = resolve_jweak(handle);
  } else {
    result = jobject_ref(handle);
    // Construction of jobjects canonicalize a null value into a null
    // jobject, so for non-jweak the pointee should never be null.
    assert(external_guard || result != NULL, "Invalid JNI handle");
  }
  return result;
}

inline oop JNIHandles::resolve(jobject handle) {
  oop result = NULL;
  if (handle != NULL) {
    result = resolve_impl<false /* external_guard */ >(handle);
  }
  return result;
}

// Resolve some erroneous cases to NULL, rather than treating them as
// possibly unchecked errors.  In particular, deleted handles are
// treated as NULL (though a deleted and later reallocated handle
// isn't detected).
inline oop JNIHandles::resolve_external_guard(jobject handle) {
  oop result = NULL;
  if (handle != NULL) {
    result = resolve_impl<true /* external_guard */ >(handle);
  }
  return result;
}

inline oop JNIHandles::resolve_non_null(jobject handle) {
  assert(handle != NULL, "JNI handle should not be null");
  oop result = resolve_impl<false /* external_guard */ >(handle);
  assert(result != NULL, "NULL read from jni handle");
  return result;
}

inline void JNIHandles::destroy_local(jobject handle) {
  if (handle != NULL) {
    assert(!is_jweak(handle), "Invalid JNI local handle");
    jobject_ref(handle) = NULL;
  }
}

#endif // SHARE_VM_RUNTIME_JNIHANDLES_HPP
