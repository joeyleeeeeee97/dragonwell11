/*
 * Copyright (c) 1999, 2022, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "ci/ciUtilities.inline.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/compileLog.hpp"
#include "gc/shared/barrierSet.hpp"
#include "jfr/support/jfrIntrinsics.hpp"
#include "memory/resourceArea.hpp"
#include "oops/objArrayKlass.hpp"
#include "opto/addnode.hpp"
#include "opto/arraycopynode.hpp"
#include "opto/c2compiler.hpp"
#include "opto/castnode.hpp"
#include "opto/cfgnode.hpp"
#include "opto/convertnode.hpp"
#include "opto/countbitsnode.hpp"
#include "opto/idealKit.hpp"
#include "opto/library_call.hpp"
#include "opto/mathexactnode.hpp"
#include "opto/mulnode.hpp"
#include "opto/narrowptrnode.hpp"
#include "opto/opaquenode.hpp"
#include "opto/parse.hpp"
#include "opto/runtime.hpp"
#include "opto/rootnode.hpp"
#include "opto/subnode.hpp"
#include "prims/nativeLookup.hpp"
#include "prims/unsafe.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/macros.hpp"


//---------------------------make_vm_intrinsic----------------------------
CallGenerator* Compile::make_vm_intrinsic(ciMethod* m, bool is_virtual) {
  vmIntrinsics::ID id = m->intrinsic_id();
  assert(id != vmIntrinsics::_none, "must be a VM intrinsic");

  if (!m->is_loaded()) {
    // Do not attempt to inline unloaded methods.
    return NULL;
  }

  C2Compiler* compiler = (C2Compiler*)CompileBroker::compiler(CompLevel_full_optimization);
  bool is_available = false;

  {
    // For calling is_intrinsic_supported and is_intrinsic_disabled_by_flag
    // the compiler must transition to '_thread_in_vm' state because both
    // methods access VM-internal data.
    VM_ENTRY_MARK;
    methodHandle mh(THREAD, m->get_Method());
    is_available = compiler != NULL && compiler->is_intrinsic_supported(mh, is_virtual) &&
                   !C->directive()->is_intrinsic_disabled(mh) &&
                   !vmIntrinsics::is_disabled_by_flags(mh);

  }

  if (is_available) {
    assert(id <= vmIntrinsics::LAST_COMPILER_INLINE, "caller responsibility");
    assert(id != vmIntrinsics::_Object_init && id != vmIntrinsics::_invoke, "enum out of order?");
    return new LibraryIntrinsic(m, is_virtual,
                                vmIntrinsics::predicates_needed(id),
                                vmIntrinsics::does_virtual_dispatch(id),
                                (vmIntrinsics::ID) id);
  } else {
    return NULL;
  }
}

//----------------------register_library_intrinsics-----------------------
// Initialize this file's data structures, for each Compile instance.
void Compile::register_library_intrinsics() {
  // Nothing to do here.
}

JVMState* LibraryIntrinsic::generate(JVMState* jvms) {
  LibraryCallKit kit(jvms, this);
  Compile* C = kit.C;
  int nodes = C->unique();
#ifndef PRODUCT
  if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
    char buf[1000];
    const char* str = vmIntrinsics::short_name_as_C_string(intrinsic_id(), buf, sizeof(buf));
    tty->print_cr("Intrinsic %s", str);
  }
#endif
  ciMethod* callee = kit.callee();
  const int bci    = kit.bci();

  // Try to inline the intrinsic.
  if ((CheckIntrinsics ? callee->intrinsic_candidate() : true) &&
      kit.try_to_inline(_last_predicate)) {
    const char *inline_msg = is_virtual() ? "(intrinsic, virtual)"
                                          : "(intrinsic)";
    CompileTask::print_inlining_ul(callee, jvms->depth() - 1, bci, inline_msg);
    if (C->print_intrinsics() || C->print_inlining()) {
      C->print_inlining(callee, jvms->depth() - 1, bci, inline_msg);
    }
    C->gather_intrinsic_statistics(intrinsic_id(), is_virtual(), Compile::_intrinsic_worked);
    if (C->log()) {
      C->log()->elem("intrinsic id='%s'%s nodes='%d'",
                     vmIntrinsics::name_at(intrinsic_id()),
                     (is_virtual() ? " virtual='1'" : ""),
                     C->unique() - nodes);
    }
    // Push the result from the inlined method onto the stack.
    kit.push_result();
    C->print_inlining_update(this);
    return kit.transfer_exceptions_into_jvms();
  }

  // The intrinsic bailed out
  if (jvms->has_method()) {
    // Not a root compile.
    const char* msg;
    if (callee->intrinsic_candidate()) {
      msg = is_virtual() ? "failed to inline (intrinsic, virtual)" : "failed to inline (intrinsic)";
    } else {
      msg = is_virtual() ? "failed to inline (intrinsic, virtual), method not annotated"
                         : "failed to inline (intrinsic), method not annotated";
    }
    CompileTask::print_inlining_ul(callee, jvms->depth() - 1, bci, msg);
    if (C->print_intrinsics() || C->print_inlining()) {
      C->print_inlining(callee, jvms->depth() - 1, bci, msg);
    }
  } else {
    // Root compile
    ResourceMark rm;
    stringStream msg_stream;
    msg_stream.print("Did not generate intrinsic %s%s at bci:%d in",
                     vmIntrinsics::name_at(intrinsic_id()),
                     is_virtual() ? " (virtual)" : "", bci);
    const char *msg = msg_stream.as_string();
    log_debug(jit, inlining)("%s", msg);
    if (C->print_intrinsics() || C->print_inlining()) {
      tty->print("%s", msg);
    }
  }
  C->gather_intrinsic_statistics(intrinsic_id(), is_virtual(), Compile::_intrinsic_failed);
  C->print_inlining_update(this);

  return NULL;
}

Node* LibraryIntrinsic::generate_predicate(JVMState* jvms, int predicate) {
  LibraryCallKit kit(jvms, this);
  Compile* C = kit.C;
  int nodes = C->unique();
  _last_predicate = predicate;
#ifndef PRODUCT
  assert(is_predicated() && predicate < predicates_count(), "sanity");
  if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
    char buf[1000];
    const char* str = vmIntrinsics::short_name_as_C_string(intrinsic_id(), buf, sizeof(buf));
    tty->print_cr("Predicate for intrinsic %s", str);
  }
#endif
  ciMethod* callee = kit.callee();
  const int bci    = kit.bci();

  Node* slow_ctl = kit.try_to_predicate(predicate);
  if (!kit.failing()) {
    const char *inline_msg = is_virtual() ? "(intrinsic, virtual, predicate)"
                                          : "(intrinsic, predicate)";
    CompileTask::print_inlining_ul(callee, jvms->depth() - 1, bci, inline_msg);
    if (C->print_intrinsics() || C->print_inlining()) {
      C->print_inlining(callee, jvms->depth() - 1, bci, inline_msg);
    }
    C->gather_intrinsic_statistics(intrinsic_id(), is_virtual(), Compile::_intrinsic_worked);
    if (C->log()) {
      C->log()->elem("predicate_intrinsic id='%s'%s nodes='%d'",
                     vmIntrinsics::name_at(intrinsic_id()),
                     (is_virtual() ? " virtual='1'" : ""),
                     C->unique() - nodes);
    }
    return slow_ctl; // Could be NULL if the check folds.
  }

  // The intrinsic bailed out
  if (jvms->has_method()) {
    // Not a root compile.
    const char* msg = "failed to generate predicate for intrinsic";
    CompileTask::print_inlining_ul(kit.callee(), jvms->depth() - 1, bci, msg);
    if (C->print_intrinsics() || C->print_inlining()) {
      C->print_inlining(kit.callee(), jvms->depth() - 1, bci, msg);
    }
  } else {
    // Root compile
    ResourceMark rm;
    stringStream msg_stream;
    msg_stream.print("Did not generate intrinsic %s%s at bci:%d in",
                     vmIntrinsics::name_at(intrinsic_id()),
                     is_virtual() ? " (virtual)" : "", bci);
    const char *msg = msg_stream.as_string();
    log_debug(jit, inlining)("%s", msg);
    if (C->print_intrinsics() || C->print_inlining()) {
      C->print_inlining_stream()->print("%s", msg);
    }
  }
  C->gather_intrinsic_statistics(intrinsic_id(), is_virtual(), Compile::_intrinsic_failed);
  return NULL;
}

bool LibraryCallKit::try_to_inline(int predicate) {
  // Handle symbolic names for otherwise undistinguished boolean switches:
  const bool is_store       = true;
  const bool is_compress    = true;
  const bool is_static      = true;
  const bool is_volatile    = true;

  if (!jvms()->has_method()) {
    // Root JVMState has a null method.
    assert(map()->memory()->Opcode() == Op_Parm, "");
    // Insert the memory aliasing node
    set_all_memory(reset_memory());
  }
  assert(merged_memory(), "");

  switch (intrinsic_id()) {
  case vmIntrinsics::_hashCode:                 return inline_native_hashcode(intrinsic()->is_virtual(), !is_static);
  case vmIntrinsics::_identityHashCode:         return inline_native_hashcode(/*!virtual*/ false,         is_static);
  case vmIntrinsics::_getClass:                 return inline_native_getClass();

  case vmIntrinsics::_ceil:
  case vmIntrinsics::_floor:
  case vmIntrinsics::_rint:
  case vmIntrinsics::_dsin:
  case vmIntrinsics::_dcos:
  case vmIntrinsics::_dtan:
  case vmIntrinsics::_dabs:
  case vmIntrinsics::_fabs:
  case vmIntrinsics::_iabs:
  case vmIntrinsics::_labs:
  case vmIntrinsics::_datan2:
  case vmIntrinsics::_dsqrt:
  case vmIntrinsics::_dexp:
  case vmIntrinsics::_dlog:
  case vmIntrinsics::_dlog10:
  case vmIntrinsics::_dpow:
  case vmIntrinsics::_dcopySign:
  case vmIntrinsics::_fcopySign:
  case vmIntrinsics::_dsignum:
  case vmIntrinsics::_fsignum:                  return inline_math_native(intrinsic_id());

  case vmIntrinsics::_min:
  case vmIntrinsics::_max:                      return inline_min_max(intrinsic_id());

  case vmIntrinsics::_notify:
  case vmIntrinsics::_notifyAll:
    if (ObjectMonitor::Knob_InlineNotify) {
      return inline_notify(intrinsic_id());
    }
    return false;

  case vmIntrinsics::_addExactI:                return inline_math_addExactI(false /* add */);
  case vmIntrinsics::_addExactL:                return inline_math_addExactL(false /* add */);
  case vmIntrinsics::_decrementExactI:          return inline_math_subtractExactI(true /* decrement */);
  case vmIntrinsics::_decrementExactL:          return inline_math_subtractExactL(true /* decrement */);
  case vmIntrinsics::_incrementExactI:          return inline_math_addExactI(true /* increment */);
  case vmIntrinsics::_incrementExactL:          return inline_math_addExactL(true /* increment */);
  case vmIntrinsics::_multiplyExactI:           return inline_math_multiplyExactI();
  case vmIntrinsics::_multiplyExactL:           return inline_math_multiplyExactL();
  case vmIntrinsics::_multiplyHigh:             return inline_math_multiplyHigh();
  case vmIntrinsics::_negateExactI:             return inline_math_negateExactI();
  case vmIntrinsics::_negateExactL:             return inline_math_negateExactL();
  case vmIntrinsics::_subtractExactI:           return inline_math_subtractExactI(false /* subtract */);
  case vmIntrinsics::_subtractExactL:           return inline_math_subtractExactL(false /* subtract */);

  case vmIntrinsics::_arraycopy:                return inline_arraycopy();

  case vmIntrinsics::_compareToL:               return inline_string_compareTo(StrIntrinsicNode::LL);
  case vmIntrinsics::_compareToU:               return inline_string_compareTo(StrIntrinsicNode::UU);
  case vmIntrinsics::_compareToLU:              return inline_string_compareTo(StrIntrinsicNode::LU);
  case vmIntrinsics::_compareToUL:              return inline_string_compareTo(StrIntrinsicNode::UL);

  case vmIntrinsics::_indexOfL:                 return inline_string_indexOf(StrIntrinsicNode::LL);
  case vmIntrinsics::_indexOfU:                 return inline_string_indexOf(StrIntrinsicNode::UU);
  case vmIntrinsics::_indexOfUL:                return inline_string_indexOf(StrIntrinsicNode::UL);
  case vmIntrinsics::_indexOfIL:                return inline_string_indexOfI(StrIntrinsicNode::LL);
  case vmIntrinsics::_indexOfIU:                return inline_string_indexOfI(StrIntrinsicNode::UU);
  case vmIntrinsics::_indexOfIUL:               return inline_string_indexOfI(StrIntrinsicNode::UL);
  case vmIntrinsics::_indexOfU_char:            return inline_string_indexOfChar();

  case vmIntrinsics::_equalsL:                  return inline_string_equals(StrIntrinsicNode::LL);
  case vmIntrinsics::_equalsU:                  return inline_string_equals(StrIntrinsicNode::UU);

  case vmIntrinsics::_toBytesStringU:           return inline_string_toBytesU();
  case vmIntrinsics::_getCharsStringU:          return inline_string_getCharsU();
  case vmIntrinsics::_getCharStringU:           return inline_string_char_access(!is_store);
  case vmIntrinsics::_putCharStringU:           return inline_string_char_access( is_store);

  case vmIntrinsics::_compressStringC:
  case vmIntrinsics::_compressStringB:          return inline_string_copy( is_compress);
  case vmIntrinsics::_inflateStringC:
  case vmIntrinsics::_inflateStringB:           return inline_string_copy(!is_compress);

  case vmIntrinsics::_getObject:                return inline_unsafe_access(!is_store, T_OBJECT,   Relaxed, false);
  case vmIntrinsics::_getBoolean:               return inline_unsafe_access(!is_store, T_BOOLEAN,  Relaxed, false);
  case vmIntrinsics::_getByte:                  return inline_unsafe_access(!is_store, T_BYTE,     Relaxed, false);
  case vmIntrinsics::_getShort:                 return inline_unsafe_access(!is_store, T_SHORT,    Relaxed, false);
  case vmIntrinsics::_getChar:                  return inline_unsafe_access(!is_store, T_CHAR,     Relaxed, false);
  case vmIntrinsics::_getInt:                   return inline_unsafe_access(!is_store, T_INT,      Relaxed, false);
  case vmIntrinsics::_getLong:                  return inline_unsafe_access(!is_store, T_LONG,     Relaxed, false);
  case vmIntrinsics::_getFloat:                 return inline_unsafe_access(!is_store, T_FLOAT,    Relaxed, false);
  case vmIntrinsics::_getDouble:                return inline_unsafe_access(!is_store, T_DOUBLE,   Relaxed, false);

  case vmIntrinsics::_putObject:                return inline_unsafe_access( is_store, T_OBJECT,   Relaxed, false);
  case vmIntrinsics::_putBoolean:               return inline_unsafe_access( is_store, T_BOOLEAN,  Relaxed, false);
  case vmIntrinsics::_putByte:                  return inline_unsafe_access( is_store, T_BYTE,     Relaxed, false);
  case vmIntrinsics::_putShort:                 return inline_unsafe_access( is_store, T_SHORT,    Relaxed, false);
  case vmIntrinsics::_putChar:                  return inline_unsafe_access( is_store, T_CHAR,     Relaxed, false);
  case vmIntrinsics::_putInt:                   return inline_unsafe_access( is_store, T_INT,      Relaxed, false);
  case vmIntrinsics::_putLong:                  return inline_unsafe_access( is_store, T_LONG,     Relaxed, false);
  case vmIntrinsics::_putFloat:                 return inline_unsafe_access( is_store, T_FLOAT,    Relaxed, false);
  case vmIntrinsics::_putDouble:                return inline_unsafe_access( is_store, T_DOUBLE,   Relaxed, false);

  case vmIntrinsics::_getObjectVolatile:        return inline_unsafe_access(!is_store, T_OBJECT,   Volatile, false);
  case vmIntrinsics::_getBooleanVolatile:       return inline_unsafe_access(!is_store, T_BOOLEAN,  Volatile, false);
  case vmIntrinsics::_getByteVolatile:          return inline_unsafe_access(!is_store, T_BYTE,     Volatile, false);
  case vmIntrinsics::_getShortVolatile:         return inline_unsafe_access(!is_store, T_SHORT,    Volatile, false);
  case vmIntrinsics::_getCharVolatile:          return inline_unsafe_access(!is_store, T_CHAR,     Volatile, false);
  case vmIntrinsics::_getIntVolatile:           return inline_unsafe_access(!is_store, T_INT,      Volatile, false);
  case vmIntrinsics::_getLongVolatile:          return inline_unsafe_access(!is_store, T_LONG,     Volatile, false);
  case vmIntrinsics::_getFloatVolatile:         return inline_unsafe_access(!is_store, T_FLOAT,    Volatile, false);
  case vmIntrinsics::_getDoubleVolatile:        return inline_unsafe_access(!is_store, T_DOUBLE,   Volatile, false);

  case vmIntrinsics::_putObjectVolatile:        return inline_unsafe_access( is_store, T_OBJECT,   Volatile, false);
  case vmIntrinsics::_putBooleanVolatile:       return inline_unsafe_access( is_store, T_BOOLEAN,  Volatile, false);
  case vmIntrinsics::_putByteVolatile:          return inline_unsafe_access( is_store, T_BYTE,     Volatile, false);
  case vmIntrinsics::_putShortVolatile:         return inline_unsafe_access( is_store, T_SHORT,    Volatile, false);
  case vmIntrinsics::_putCharVolatile:          return inline_unsafe_access( is_store, T_CHAR,     Volatile, false);
  case vmIntrinsics::_putIntVolatile:           return inline_unsafe_access( is_store, T_INT,      Volatile, false);
  case vmIntrinsics::_putLongVolatile:          return inline_unsafe_access( is_store, T_LONG,     Volatile, false);
  case vmIntrinsics::_putFloatVolatile:         return inline_unsafe_access( is_store, T_FLOAT,    Volatile, false);
  case vmIntrinsics::_putDoubleVolatile:        return inline_unsafe_access( is_store, T_DOUBLE,   Volatile, false);

  case vmIntrinsics::_getShortUnaligned:        return inline_unsafe_access(!is_store, T_SHORT,    Relaxed, true);
  case vmIntrinsics::_getCharUnaligned:         return inline_unsafe_access(!is_store, T_CHAR,     Relaxed, true);
  case vmIntrinsics::_getIntUnaligned:          return inline_unsafe_access(!is_store, T_INT,      Relaxed, true);
  case vmIntrinsics::_getLongUnaligned:         return inline_unsafe_access(!is_store, T_LONG,     Relaxed, true);

  case vmIntrinsics::_putShortUnaligned:        return inline_unsafe_access( is_store, T_SHORT,    Relaxed, true);
  case vmIntrinsics::_putCharUnaligned:         return inline_unsafe_access( is_store, T_CHAR,     Relaxed, true);
  case vmIntrinsics::_putIntUnaligned:          return inline_unsafe_access( is_store, T_INT,      Relaxed, true);
  case vmIntrinsics::_putLongUnaligned:         return inline_unsafe_access( is_store, T_LONG,     Relaxed, true);

  case vmIntrinsics::_getObjectAcquire:         return inline_unsafe_access(!is_store, T_OBJECT,   Acquire, false);
  case vmIntrinsics::_getBooleanAcquire:        return inline_unsafe_access(!is_store, T_BOOLEAN,  Acquire, false);
  case vmIntrinsics::_getByteAcquire:           return inline_unsafe_access(!is_store, T_BYTE,     Acquire, false);
  case vmIntrinsics::_getShortAcquire:          return inline_unsafe_access(!is_store, T_SHORT,    Acquire, false);
  case vmIntrinsics::_getCharAcquire:           return inline_unsafe_access(!is_store, T_CHAR,     Acquire, false);
  case vmIntrinsics::_getIntAcquire:            return inline_unsafe_access(!is_store, T_INT,      Acquire, false);
  case vmIntrinsics::_getLongAcquire:           return inline_unsafe_access(!is_store, T_LONG,     Acquire, false);
  case vmIntrinsics::_getFloatAcquire:          return inline_unsafe_access(!is_store, T_FLOAT,    Acquire, false);
  case vmIntrinsics::_getDoubleAcquire:         return inline_unsafe_access(!is_store, T_DOUBLE,   Acquire, false);

  case vmIntrinsics::_putObjectRelease:         return inline_unsafe_access( is_store, T_OBJECT,   Release, false);
  case vmIntrinsics::_putBooleanRelease:        return inline_unsafe_access( is_store, T_BOOLEAN,  Release, false);
  case vmIntrinsics::_putByteRelease:           return inline_unsafe_access( is_store, T_BYTE,     Release, false);
  case vmIntrinsics::_putShortRelease:          return inline_unsafe_access( is_store, T_SHORT,    Release, false);
  case vmIntrinsics::_putCharRelease:           return inline_unsafe_access( is_store, T_CHAR,     Release, false);
  case vmIntrinsics::_putIntRelease:            return inline_unsafe_access( is_store, T_INT,      Release, false);
  case vmIntrinsics::_putLongRelease:           return inline_unsafe_access( is_store, T_LONG,     Release, false);
  case vmIntrinsics::_putFloatRelease:          return inline_unsafe_access( is_store, T_FLOAT,    Release, false);
  case vmIntrinsics::_putDoubleRelease:         return inline_unsafe_access( is_store, T_DOUBLE,   Release, false);

  case vmIntrinsics::_getObjectOpaque:          return inline_unsafe_access(!is_store, T_OBJECT,   Opaque, false);
  case vmIntrinsics::_getBooleanOpaque:         return inline_unsafe_access(!is_store, T_BOOLEAN,  Opaque, false);
  case vmIntrinsics::_getByteOpaque:            return inline_unsafe_access(!is_store, T_BYTE,     Opaque, false);
  case vmIntrinsics::_getShortOpaque:           return inline_unsafe_access(!is_store, T_SHORT,    Opaque, false);
  case vmIntrinsics::_getCharOpaque:            return inline_unsafe_access(!is_store, T_CHAR,     Opaque, false);
  case vmIntrinsics::_getIntOpaque:             return inline_unsafe_access(!is_store, T_INT,      Opaque, false);
  case vmIntrinsics::_getLongOpaque:            return inline_unsafe_access(!is_store, T_LONG,     Opaque, false);
  case vmIntrinsics::_getFloatOpaque:           return inline_unsafe_access(!is_store, T_FLOAT,    Opaque, false);
  case vmIntrinsics::_getDoubleOpaque:          return inline_unsafe_access(!is_store, T_DOUBLE,   Opaque, false);

  case vmIntrinsics::_putObjectOpaque:          return inline_unsafe_access( is_store, T_OBJECT,   Opaque, false);
  case vmIntrinsics::_putBooleanOpaque:         return inline_unsafe_access( is_store, T_BOOLEAN,  Opaque, false);
  case vmIntrinsics::_putByteOpaque:            return inline_unsafe_access( is_store, T_BYTE,     Opaque, false);
  case vmIntrinsics::_putShortOpaque:           return inline_unsafe_access( is_store, T_SHORT,    Opaque, false);
  case vmIntrinsics::_putCharOpaque:            return inline_unsafe_access( is_store, T_CHAR,     Opaque, false);
  case vmIntrinsics::_putIntOpaque:             return inline_unsafe_access( is_store, T_INT,      Opaque, false);
  case vmIntrinsics::_putLongOpaque:            return inline_unsafe_access( is_store, T_LONG,     Opaque, false);
  case vmIntrinsics::_putFloatOpaque:           return inline_unsafe_access( is_store, T_FLOAT,    Opaque, false);
  case vmIntrinsics::_putDoubleOpaque:          return inline_unsafe_access( is_store, T_DOUBLE,   Opaque, false);

  case vmIntrinsics::_compareAndSetObject:              return inline_unsafe_load_store(T_OBJECT, LS_cmp_swap,      Volatile);
  case vmIntrinsics::_compareAndSetByte:                return inline_unsafe_load_store(T_BYTE,   LS_cmp_swap,      Volatile);
  case vmIntrinsics::_compareAndSetShort:               return inline_unsafe_load_store(T_SHORT,  LS_cmp_swap,      Volatile);
  case vmIntrinsics::_compareAndSetInt:                 return inline_unsafe_load_store(T_INT,    LS_cmp_swap,      Volatile);
  case vmIntrinsics::_compareAndSetLong:                return inline_unsafe_load_store(T_LONG,   LS_cmp_swap,      Volatile);

  case vmIntrinsics::_weakCompareAndSetObjectPlain:     return inline_unsafe_load_store(T_OBJECT, LS_cmp_swap_weak, Relaxed);
  case vmIntrinsics::_weakCompareAndSetObjectAcquire:   return inline_unsafe_load_store(T_OBJECT, LS_cmp_swap_weak, Acquire);
  case vmIntrinsics::_weakCompareAndSetObjectRelease:   return inline_unsafe_load_store(T_OBJECT, LS_cmp_swap_weak, Release);
  case vmIntrinsics::_weakCompareAndSetObject:          return inline_unsafe_load_store(T_OBJECT, LS_cmp_swap_weak, Volatile);
  case vmIntrinsics::_weakCompareAndSetBytePlain:       return inline_unsafe_load_store(T_BYTE,   LS_cmp_swap_weak, Relaxed);
  case vmIntrinsics::_weakCompareAndSetByteAcquire:     return inline_unsafe_load_store(T_BYTE,   LS_cmp_swap_weak, Acquire);
  case vmIntrinsics::_weakCompareAndSetByteRelease:     return inline_unsafe_load_store(T_BYTE,   LS_cmp_swap_weak, Release);
  case vmIntrinsics::_weakCompareAndSetByte:            return inline_unsafe_load_store(T_BYTE,   LS_cmp_swap_weak, Volatile);
  case vmIntrinsics::_weakCompareAndSetShortPlain:      return inline_unsafe_load_store(T_SHORT,  LS_cmp_swap_weak, Relaxed);
  case vmIntrinsics::_weakCompareAndSetShortAcquire:    return inline_unsafe_load_store(T_SHORT,  LS_cmp_swap_weak, Acquire);
  case vmIntrinsics::_weakCompareAndSetShortRelease:    return inline_unsafe_load_store(T_SHORT,  LS_cmp_swap_weak, Release);
  case vmIntrinsics::_weakCompareAndSetShort:           return inline_unsafe_load_store(T_SHORT,  LS_cmp_swap_weak, Volatile);
  case vmIntrinsics::_weakCompareAndSetIntPlain:        return inline_unsafe_load_store(T_INT,    LS_cmp_swap_weak, Relaxed);
  case vmIntrinsics::_weakCompareAndSetIntAcquire:      return inline_unsafe_load_store(T_INT,    LS_cmp_swap_weak, Acquire);
  case vmIntrinsics::_weakCompareAndSetIntRelease:      return inline_unsafe_load_store(T_INT,    LS_cmp_swap_weak, Release);
  case vmIntrinsics::_weakCompareAndSetInt:             return inline_unsafe_load_store(T_INT,    LS_cmp_swap_weak, Volatile);
  case vmIntrinsics::_weakCompareAndSetLongPlain:       return inline_unsafe_load_store(T_LONG,   LS_cmp_swap_weak, Relaxed);
  case vmIntrinsics::_weakCompareAndSetLongAcquire:     return inline_unsafe_load_store(T_LONG,   LS_cmp_swap_weak, Acquire);
  case vmIntrinsics::_weakCompareAndSetLongRelease:     return inline_unsafe_load_store(T_LONG,   LS_cmp_swap_weak, Release);
  case vmIntrinsics::_weakCompareAndSetLong:            return inline_unsafe_load_store(T_LONG,   LS_cmp_swap_weak, Volatile);

  case vmIntrinsics::_compareAndExchangeObject:         return inline_unsafe_load_store(T_OBJECT, LS_cmp_exchange,  Volatile);
  case vmIntrinsics::_compareAndExchangeObjectAcquire:  return inline_unsafe_load_store(T_OBJECT, LS_cmp_exchange,  Acquire);
  case vmIntrinsics::_compareAndExchangeObjectRelease:  return inline_unsafe_load_store(T_OBJECT, LS_cmp_exchange,  Release);
  case vmIntrinsics::_compareAndExchangeByte:           return inline_unsafe_load_store(T_BYTE,   LS_cmp_exchange,  Volatile);
  case vmIntrinsics::_compareAndExchangeByteAcquire:    return inline_unsafe_load_store(T_BYTE,   LS_cmp_exchange,  Acquire);
  case vmIntrinsics::_compareAndExchangeByteRelease:    return inline_unsafe_load_store(T_BYTE,   LS_cmp_exchange,  Release);
  case vmIntrinsics::_compareAndExchangeShort:          return inline_unsafe_load_store(T_SHORT,  LS_cmp_exchange,  Volatile);
  case vmIntrinsics::_compareAndExchangeShortAcquire:   return inline_unsafe_load_store(T_SHORT,  LS_cmp_exchange,  Acquire);
  case vmIntrinsics::_compareAndExchangeShortRelease:   return inline_unsafe_load_store(T_SHORT,  LS_cmp_exchange,  Release);
  case vmIntrinsics::_compareAndExchangeInt:            return inline_unsafe_load_store(T_INT,    LS_cmp_exchange,  Volatile);
  case vmIntrinsics::_compareAndExchangeIntAcquire:     return inline_unsafe_load_store(T_INT,    LS_cmp_exchange,  Acquire);
  case vmIntrinsics::_compareAndExchangeIntRelease:     return inline_unsafe_load_store(T_INT,    LS_cmp_exchange,  Release);
  case vmIntrinsics::_compareAndExchangeLong:           return inline_unsafe_load_store(T_LONG,   LS_cmp_exchange,  Volatile);
  case vmIntrinsics::_compareAndExchangeLongAcquire:    return inline_unsafe_load_store(T_LONG,   LS_cmp_exchange,  Acquire);
  case vmIntrinsics::_compareAndExchangeLongRelease:    return inline_unsafe_load_store(T_LONG,   LS_cmp_exchange,  Release);

  case vmIntrinsics::_getAndAddByte:                    return inline_unsafe_load_store(T_BYTE,   LS_get_add,       Volatile);
  case vmIntrinsics::_getAndAddShort:                   return inline_unsafe_load_store(T_SHORT,  LS_get_add,       Volatile);
  case vmIntrinsics::_getAndAddInt:                     return inline_unsafe_load_store(T_INT,    LS_get_add,       Volatile);
  case vmIntrinsics::_getAndAddLong:                    return inline_unsafe_load_store(T_LONG,   LS_get_add,       Volatile);

  case vmIntrinsics::_getAndSetByte:                    return inline_unsafe_load_store(T_BYTE,   LS_get_set,       Volatile);
  case vmIntrinsics::_getAndSetShort:                   return inline_unsafe_load_store(T_SHORT,  LS_get_set,       Volatile);
  case vmIntrinsics::_getAndSetInt:                     return inline_unsafe_load_store(T_INT,    LS_get_set,       Volatile);
  case vmIntrinsics::_getAndSetLong:                    return inline_unsafe_load_store(T_LONG,   LS_get_set,       Volatile);
  case vmIntrinsics::_getAndSetObject:                  return inline_unsafe_load_store(T_OBJECT, LS_get_set,       Volatile);

  case vmIntrinsics::_loadFence:
  case vmIntrinsics::_storeFence:
  case vmIntrinsics::_fullFence:                return inline_unsafe_fence(intrinsic_id());

  case vmIntrinsics::_onSpinWait:               return inline_onspinwait();

  case vmIntrinsics::_currentThread:            return inline_native_currentThread();
  case vmIntrinsics::_isInterrupted:            return inline_native_isInterrupted();

#ifdef JFR_HAVE_INTRINSICS
  case vmIntrinsics::_counterTime:              return inline_native_time_funcs(CAST_FROM_FN_PTR(address, JFR_TIME_FUNCTION), "counterTime");
  case vmIntrinsics::_getClassId:               return inline_native_classID();
  case vmIntrinsics::_getEventWriter:           return inline_native_getEventWriter();
#endif
  case vmIntrinsics::_currentTimeMillis:        return inline_native_time_funcs(CAST_FROM_FN_PTR(address, os::javaTimeMillis), "currentTimeMillis");
  case vmIntrinsics::_nanoTime:                 return inline_native_time_funcs(CAST_FROM_FN_PTR(address, os::javaTimeNanos), "nanoTime");
  case vmIntrinsics::_allocateInstance:         return inline_unsafe_allocate();
  case vmIntrinsics::_copyMemory:               return inline_unsafe_copyMemory();
  case vmIntrinsics::_getLength:                return inline_native_getLength();
  case vmIntrinsics::_copyOf:                   return inline_array_copyOf(false);
  case vmIntrinsics::_copyOfRange:              return inline_array_copyOf(true);
  case vmIntrinsics::_equalsB:                  return inline_array_equals(StrIntrinsicNode::LL);
  case vmIntrinsics::_equalsC:                  return inline_array_equals(StrIntrinsicNode::UU);
  case vmIntrinsics::_Preconditions_checkIndex: return inline_preconditions_checkIndex();
  case vmIntrinsics::_clone:                    return inline_native_clone(intrinsic()->is_virtual());

  case vmIntrinsics::_allocateUninitializedArray: return inline_unsafe_newArray(true);
  case vmIntrinsics::_newArray:                   return inline_unsafe_newArray(false);

  case vmIntrinsics::_isAssignableFrom:         return inline_native_subtype_check();

  case vmIntrinsics::_isInstance:
  case vmIntrinsics::_getModifiers:
  case vmIntrinsics::_isInterface:
  case vmIntrinsics::_isArray:
  case vmIntrinsics::_isPrimitive:
  case vmIntrinsics::_getSuperclass:
  case vmIntrinsics::_getClassAccessFlags:      return inline_native_Class_query(intrinsic_id());

  case vmIntrinsics::_floatToRawIntBits:
  case vmIntrinsics::_floatToIntBits:
  case vmIntrinsics::_intBitsToFloat:
  case vmIntrinsics::_doubleToRawLongBits:
  case vmIntrinsics::_doubleToLongBits:
  case vmIntrinsics::_longBitsToDouble:         return inline_fp_conversions(intrinsic_id());

  case vmIntrinsics::_numberOfLeadingZeros_i:
  case vmIntrinsics::_numberOfLeadingZeros_l:
  case vmIntrinsics::_numberOfTrailingZeros_i:
  case vmIntrinsics::_numberOfTrailingZeros_l:
  case vmIntrinsics::_bitCount_i:
  case vmIntrinsics::_bitCount_l:
  case vmIntrinsics::_reverseBytes_i:
  case vmIntrinsics::_reverseBytes_l:
  case vmIntrinsics::_reverseBytes_s:
  case vmIntrinsics::_reverseBytes_c:           return inline_number_methods(intrinsic_id());

  case vmIntrinsics::_getCallerClass:           return inline_native_Reflection_getCallerClass();

  case vmIntrinsics::_Reference_get:            return inline_reference_get();

  case vmIntrinsics::_Class_cast:               return inline_Class_cast();

  case vmIntrinsics::_aescrypt_encryptBlock:
  case vmIntrinsics::_aescrypt_decryptBlock:    return inline_aescrypt_Block(intrinsic_id());

  case vmIntrinsics::_cipherBlockChaining_encryptAESCrypt:
  case vmIntrinsics::_cipherBlockChaining_decryptAESCrypt:
    return inline_cipherBlockChaining_AESCrypt(intrinsic_id());

  case vmIntrinsics::_electronicCodeBook_encryptAESCrypt:
  case vmIntrinsics::_electronicCodeBook_decryptAESCrypt:
    return inline_electronicCodeBook_AESCrypt(intrinsic_id());

  case vmIntrinsics::_counterMode_AESCrypt:
    return inline_counterMode_AESCrypt(intrinsic_id());

  case vmIntrinsics::_sha_implCompress:
  case vmIntrinsics::_sha2_implCompress:
  case vmIntrinsics::_sha5_implCompress:
    return inline_sha_implCompress(intrinsic_id());

  case vmIntrinsics::_digestBase_implCompressMB:
    return inline_digestBase_implCompressMB(predicate);

  case vmIntrinsics::_multiplyToLen:
    return inline_multiplyToLen();

  case vmIntrinsics::_squareToLen:
    return inline_squareToLen();

  case vmIntrinsics::_mulAdd:
    return inline_mulAdd();

  case vmIntrinsics::_montgomeryMultiply:
    return inline_montgomeryMultiply();
  case vmIntrinsics::_montgomerySquare:
    return inline_montgomerySquare();

  case vmIntrinsics::_vectorizedMismatch:
    return inline_vectorizedMismatch();

  case vmIntrinsics::_ghash_processBlocks:
    return inline_ghash_processBlocks();
  case vmIntrinsics::_base64_encodeBlock:
    return inline_base64_encodeBlock();

  case vmIntrinsics::_encodeISOArray:
  case vmIntrinsics::_encodeByteISOArray:
    return inline_encodeISOArray();

  case vmIntrinsics::_updateCRC32:
    return inline_updateCRC32();
  case vmIntrinsics::_updateBytesCRC32:
    return inline_updateBytesCRC32();
  case vmIntrinsics::_updateByteBufferCRC32:
    return inline_updateByteBufferCRC32();

  case vmIntrinsics::_updateBytesCRC32C:
    return inline_updateBytesCRC32C();
  case vmIntrinsics::_updateDirectByteBufferCRC32C:
    return inline_updateDirectByteBufferCRC32C();

  case vmIntrinsics::_updateBytesAdler32:
    return inline_updateBytesAdler32();
  case vmIntrinsics::_updateByteBufferAdler32:
    return inline_updateByteBufferAdler32();

  case vmIntrinsics::_profileBoolean:
    return inline_profileBoolean();
  case vmIntrinsics::_isCompileConstant:
    return inline_isCompileConstant();

  case vmIntrinsics::_hasNegatives:
    return inline_hasNegatives();

  case vmIntrinsics::_fmaD:
  case vmIntrinsics::_fmaF:
    return inline_fma(intrinsic_id());

  case vmIntrinsics::_isDigit:
  case vmIntrinsics::_isLowerCase:
  case vmIntrinsics::_isUpperCase:
  case vmIntrinsics::_isWhitespace:
    return inline_character_compare(intrinsic_id());

  case vmIntrinsics::_maxF:
  case vmIntrinsics::_minF:
  case vmIntrinsics::_maxD:
  case vmIntrinsics::_minD:
    return inline_fp_min_max(intrinsic_id());

  case vmIntrinsics::_VectorUnaryOp:
    return inline_vector_nary_operation(1);
  case vmIntrinsics::_VectorBinaryOp:
    return inline_vector_nary_operation(2);
  case vmIntrinsics::_VectorTernaryOp:
    return inline_vector_nary_operation(3);
  case vmIntrinsics::_VectorBroadcastCoerced:
    return inline_vector_broadcast_coerced();
  case vmIntrinsics::_VectorShuffleIota:
    return inline_vector_shuffle_iota();
  case vmIntrinsics::_VectorShuffleToVector:
    return inline_vector_shuffle_to_vector();
  case vmIntrinsics::_VectorLoadOp:
    return inline_vector_mem_operation(/*is_store=*/false);
  case vmIntrinsics::_VectorStoreOp:
    return inline_vector_mem_operation(/*is_store=*/true);
  case vmIntrinsics::_VectorGatherOp:
    return inline_vector_gather_scatter(/*is_scatter*/ false);
  case vmIntrinsics::_VectorScatterOp:
    return inline_vector_gather_scatter(/*is_scatter*/ true);
  case vmIntrinsics::_VectorReductionCoerced:
    return inline_vector_reduction();
  case vmIntrinsics::_VectorTest:
    return inline_vector_test();
  case vmIntrinsics::_VectorBlend:
    return inline_vector_blend();
  case vmIntrinsics::_VectorRearrange:
    return inline_vector_rearrange();
  case vmIntrinsics::_VectorCompare:
    return inline_vector_compare();
  case vmIntrinsics::_VectorBroadcastInt:
    return inline_vector_broadcast_int();
  case vmIntrinsics::_VectorConvert:
    return inline_vector_convert();
  case vmIntrinsics::_VectorInsert:
    return inline_vector_insert();
  case vmIntrinsics::_VectorExtract:
    return inline_vector_extract();

  default:
    // If you get here, it may be that someone has added a new intrinsic
    // to the list in vmSymbols.hpp without implementing it here.
#ifndef PRODUCT
    if ((PrintMiscellaneous && (Verbose || WizardMode)) || PrintOpto) {
      tty->print_cr("*** Warning: Unimplemented intrinsic %s(%d)",
                    vmIntrinsics::name_at(intrinsic_id()), intrinsic_id());
    }
#endif
    return false;
  }
}

Node* LibraryCallKit::try_to_predicate(int predicate) {
  if (!jvms()->has_method()) {
    // Root JVMState has a null method.
    assert(map()->memory()->Opcode() == Op_Parm, "");
    // Insert the memory aliasing node
    set_all_memory(reset_memory());
  }
  assert(merged_memory(), "");

  switch (intrinsic_id()) {
  case vmIntrinsics::_cipherBlockChaining_encryptAESCrypt:
    return inline_cipherBlockChaining_AESCrypt_predicate(false);
  case vmIntrinsics::_cipherBlockChaining_decryptAESCrypt:
    return inline_cipherBlockChaining_AESCrypt_predicate(true);
  case vmIntrinsics::_electronicCodeBook_encryptAESCrypt:
    return inline_electronicCodeBook_AESCrypt_predicate(false);
  case vmIntrinsics::_electronicCodeBook_decryptAESCrypt:
    return inline_electronicCodeBook_AESCrypt_predicate(true);
  case vmIntrinsics::_counterMode_AESCrypt:
    return inline_counterMode_AESCrypt_predicate();
  case vmIntrinsics::_digestBase_implCompressMB:
    return inline_digestBase_implCompressMB_predicate(predicate);

  default:
    // If you get here, it may be that someone has added a new intrinsic
    // to the list in vmSymbols.hpp without implementing it here.
#ifndef PRODUCT
    if ((PrintMiscellaneous && (Verbose || WizardMode)) || PrintOpto) {
      tty->print_cr("*** Warning: Unimplemented predicate for intrinsic %s(%d)",
                    vmIntrinsics::name_at(intrinsic_id()), intrinsic_id());
    }
#endif
    Node* slow_ctl = control();
    set_control(top()); // No fast path instrinsic
    return slow_ctl;
  }
}

//------------------------------set_result-------------------------------
// Helper function for finishing intrinsics.
void LibraryCallKit::set_result(RegionNode* region, PhiNode* value) {
  record_for_igvn(region);
  set_control(_gvn.transform(region));
  set_result( _gvn.transform(value));
  assert(value->type()->basic_type() == result()->bottom_type()->basic_type(), "sanity");
}

//------------------------------generate_guard---------------------------
// Helper function for generating guarded fast-slow graph structures.
// The given 'test', if true, guards a slow path.  If the test fails
// then a fast path can be taken.  (We generally hope it fails.)
// In all cases, GraphKit::control() is updated to the fast path.
// The returned value represents the control for the slow path.
// The return value is never 'top'; it is either a valid control
// or NULL if it is obvious that the slow path can never be taken.
// Also, if region and the slow control are not NULL, the slow edge
// is appended to the region.
Node* LibraryCallKit::generate_guard(Node* test, RegionNode* region, float true_prob) {
  if (stopped()) {
    // Already short circuited.
    return NULL;
  }

  // Build an if node and its projections.
  // If test is true we take the slow path, which we assume is uncommon.
  if (_gvn.type(test) == TypeInt::ZERO) {
    // The slow branch is never taken.  No need to build this guard.
    return NULL;
  }

  IfNode* iff = create_and_map_if(control(), test, true_prob, COUNT_UNKNOWN);

  Node* if_slow = _gvn.transform(new IfTrueNode(iff));
  if (if_slow == top()) {
    // The slow branch is never taken.  No need to build this guard.
    return NULL;
  }

  if (region != NULL)
    region->add_req(if_slow);

  Node* if_fast = _gvn.transform(new IfFalseNode(iff));
  set_control(if_fast);

  return if_slow;
}

inline Node* LibraryCallKit::generate_slow_guard(Node* test, RegionNode* region) {
  return generate_guard(test, region, PROB_UNLIKELY_MAG(3));
}
inline Node* LibraryCallKit::generate_fair_guard(Node* test, RegionNode* region) {
  return generate_guard(test, region, PROB_FAIR);
}

inline Node* LibraryCallKit::generate_negative_guard(Node* index, RegionNode* region,
                                                     Node* *pos_index) {
  if (stopped())
    return NULL;                // already stopped
  if (_gvn.type(index)->higher_equal(TypeInt::POS)) // [0,maxint]
    return NULL;                // index is already adequately typed
  Node* cmp_lt = _gvn.transform(new CmpINode(index, intcon(0)));
  Node* bol_lt = _gvn.transform(new BoolNode(cmp_lt, BoolTest::lt));
  Node* is_neg = generate_guard(bol_lt, region, PROB_MIN);
  if (is_neg != NULL && pos_index != NULL) {
    // Emulate effect of Parse::adjust_map_after_if.
    Node* ccast = new CastIINode(index, TypeInt::POS);
    ccast->set_req(0, control());
    (*pos_index) = _gvn.transform(ccast);
  }
  return is_neg;
}

// Make sure that 'position' is a valid limit index, in [0..length].
// There are two equivalent plans for checking this:
//   A. (offset + copyLength)  unsigned<=  arrayLength
//   B. offset  <=  (arrayLength - copyLength)
// We require that all of the values above, except for the sum and
// difference, are already known to be non-negative.
// Plan A is robust in the face of overflow, if offset and copyLength
// are both hugely positive.
//
// Plan B is less direct and intuitive, but it does not overflow at
// all, since the difference of two non-negatives is always
// representable.  Whenever Java methods must perform the equivalent
// check they generally use Plan B instead of Plan A.
// For the moment we use Plan A.
inline Node* LibraryCallKit::generate_limit_guard(Node* offset,
                                                  Node* subseq_length,
                                                  Node* array_length,
                                                  RegionNode* region) {
  if (stopped())
    return NULL;                // already stopped
  bool zero_offset = _gvn.type(offset) == TypeInt::ZERO;
  if (zero_offset && subseq_length->eqv_uncast(array_length))
    return NULL;                // common case of whole-array copy
  Node* last = subseq_length;
  if (!zero_offset)             // last += offset
    last = _gvn.transform(new AddINode(last, offset));
  Node* cmp_lt = _gvn.transform(new CmpUNode(array_length, last));
  Node* bol_lt = _gvn.transform(new BoolNode(cmp_lt, BoolTest::lt));
  Node* is_over = generate_guard(bol_lt, region, PROB_MIN);
  return is_over;
}

// Emit range checks for the given String.value byte array
void LibraryCallKit::generate_string_range_check(Node* array, Node* offset, Node* count, bool char_count) {
  if (stopped()) {
    return; // already stopped
  }
  RegionNode* bailout = new RegionNode(1);
  record_for_igvn(bailout);
  if (char_count) {
    // Convert char count to byte count
    count = _gvn.transform(new LShiftINode(count, intcon(1)));
  }

  // Offset and count must not be negative
  generate_negative_guard(offset, bailout);
  generate_negative_guard(count, bailout);
  // Offset + count must not exceed length of array
  generate_limit_guard(offset, count, load_array_length(array), bailout);

  if (bailout->req() > 1) {
    PreserveJVMState pjvms(this);
    set_control(_gvn.transform(bailout));
    uncommon_trap(Deoptimization::Reason_intrinsic,
                  Deoptimization::Action_maybe_recompile);
  }
}

//--------------------------generate_current_thread--------------------
Node* LibraryCallKit::generate_current_thread(Node* &tls_output) {
  ciKlass*    thread_klass = env()->Thread_klass();
  const Type* thread_type  = TypeOopPtr::make_from_klass(thread_klass)->cast_to_ptr_type(TypePtr::NotNull);
  Node* thread = _gvn.transform(new ThreadLocalNode());
  Node* p = basic_plus_adr(top()/*!oop*/, thread, in_bytes(JavaThread::threadObj_offset()));
  Node* threadObj = make_load(NULL, p, thread_type, T_OBJECT, MemNode::unordered);
  tls_output = thread;
  return threadObj;
}


//------------------------------make_string_method_node------------------------
// Helper method for String intrinsic functions. This version is called with
// str1 and str2 pointing to byte[] nodes containing Latin1 or UTF16 encoded
// characters (depending on 'is_byte'). cnt1 and cnt2 are pointing to Int nodes
// containing the lengths of str1 and str2.
Node* LibraryCallKit::make_string_method_node(int opcode, Node* str1_start, Node* cnt1, Node* str2_start, Node* cnt2, StrIntrinsicNode::ArgEnc ae) {
  Node* result = NULL;
  switch (opcode) {
  case Op_StrIndexOf:
    result = new StrIndexOfNode(control(), memory(TypeAryPtr::BYTES),
                                str1_start, cnt1, str2_start, cnt2, ae);
    break;
  case Op_StrComp:
    result = new StrCompNode(control(), memory(TypeAryPtr::BYTES),
                             str1_start, cnt1, str2_start, cnt2, ae);
    break;
  case Op_StrEquals:
    // We already know that cnt1 == cnt2 here (checked in 'inline_string_equals').
    // Use the constant length if there is one because optimized match rule may exist.
    result = new StrEqualsNode(control(), memory(TypeAryPtr::BYTES),
                               str1_start, str2_start, cnt2->is_Con() ? cnt2 : cnt1, ae);
    break;
  default:
    ShouldNotReachHere();
    return NULL;
  }

  // All these intrinsics have checks.
  C->set_has_split_ifs(true); // Has chance for split-if optimization
  clear_upper_avx();

  return _gvn.transform(result);
}

//------------------------------inline_string_compareTo------------------------
bool LibraryCallKit::inline_string_compareTo(StrIntrinsicNode::ArgEnc ae) {
  Node* arg1 = argument(0);
  Node* arg2 = argument(1);

  arg1 = must_be_not_null(arg1, true);
  arg2 = must_be_not_null(arg2, true);

  // Get start addr and length of first argument
  Node* arg1_start  = array_element_address(arg1, intcon(0), T_BYTE);
  Node* arg1_cnt    = load_array_length(arg1);

  // Get start addr and length of second argument
  Node* arg2_start  = array_element_address(arg2, intcon(0), T_BYTE);
  Node* arg2_cnt    = load_array_length(arg2);

  Node* result = make_string_method_node(Op_StrComp, arg1_start, arg1_cnt, arg2_start, arg2_cnt, ae);
  set_result(result);
  return true;
}

//------------------------------inline_string_equals------------------------
bool LibraryCallKit::inline_string_equals(StrIntrinsicNode::ArgEnc ae) {
  Node* arg1 = argument(0);
  Node* arg2 = argument(1);

  // paths (plus control) merge
  RegionNode* region = new RegionNode(3);
  Node* phi = new PhiNode(region, TypeInt::BOOL);

  if (!stopped()) {

    arg1 = must_be_not_null(arg1, true);
    arg2 = must_be_not_null(arg2, true);

    // Get start addr and length of first argument
    Node* arg1_start  = array_element_address(arg1, intcon(0), T_BYTE);
    Node* arg1_cnt    = load_array_length(arg1);

    // Get start addr and length of second argument
    Node* arg2_start  = array_element_address(arg2, intcon(0), T_BYTE);
    Node* arg2_cnt    = load_array_length(arg2);

    // Check for arg1_cnt != arg2_cnt
    Node* cmp = _gvn.transform(new CmpINode(arg1_cnt, arg2_cnt));
    Node* bol = _gvn.transform(new BoolNode(cmp, BoolTest::ne));
    Node* if_ne = generate_slow_guard(bol, NULL);
    if (if_ne != NULL) {
      phi->init_req(2, intcon(0));
      region->init_req(2, if_ne);
    }

    // Check for count == 0 is done by assembler code for StrEquals.

    if (!stopped()) {
      Node* equals = make_string_method_node(Op_StrEquals, arg1_start, arg1_cnt, arg2_start, arg2_cnt, ae);
      phi->init_req(1, equals);
      region->init_req(1, control());
    }
  }

  // post merge
  set_control(_gvn.transform(region));
  record_for_igvn(region);

  set_result(_gvn.transform(phi));
  return true;
}

//------------------------------inline_array_equals----------------------------
bool LibraryCallKit::inline_array_equals(StrIntrinsicNode::ArgEnc ae) {
  assert(ae == StrIntrinsicNode::UU || ae == StrIntrinsicNode::LL, "unsupported array types");
  Node* arg1 = argument(0);
  Node* arg2 = argument(1);

  const TypeAryPtr* mtype = (ae == StrIntrinsicNode::UU) ? TypeAryPtr::CHARS : TypeAryPtr::BYTES;
  set_result(_gvn.transform(new AryEqNode(control(), memory(mtype), arg1, arg2, ae)));
  clear_upper_avx();

  return true;
}

//------------------------------inline_hasNegatives------------------------------
bool LibraryCallKit::inline_hasNegatives() {
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }

  assert(callee()->signature()->size() == 3, "hasNegatives has 3 parameters");
  // no receiver since it is static method
  Node* ba         = argument(0);
  Node* offset     = argument(1);
  Node* len        = argument(2);

  ba = must_be_not_null(ba, true);

  // Range checks
  generate_string_range_check(ba, offset, len, false);
  if (stopped()) {
    return true;
  }
  Node* ba_start = array_element_address(ba, offset, T_BYTE);
  Node* result = new HasNegativesNode(control(), memory(TypeAryPtr::BYTES), ba_start, len);
  set_result(_gvn.transform(result));
  return true;
}

bool LibraryCallKit::inline_preconditions_checkIndex() {
  Node* index = argument(0);
  Node* length = argument(1);
  if (too_many_traps(Deoptimization::Reason_intrinsic) || too_many_traps(Deoptimization::Reason_range_check)) {
    return false;
  }

  Node* len_pos_cmp = _gvn.transform(new CmpINode(length, intcon(0)));
  Node* len_pos_bol = _gvn.transform(new BoolNode(len_pos_cmp, BoolTest::ge));

  {
    BuildCutout unless(this, len_pos_bol, PROB_MAX);
    uncommon_trap(Deoptimization::Reason_intrinsic,
                  Deoptimization::Action_make_not_entrant);
  }

  if (stopped()) {
    return false;
  }

  Node* rc_cmp = _gvn.transform(new CmpUNode(index, length));
  BoolTest::mask btest = BoolTest::lt;
  Node* rc_bool = _gvn.transform(new BoolNode(rc_cmp, btest));
  RangeCheckNode* rc = new RangeCheckNode(control(), rc_bool, PROB_MAX, COUNT_UNKNOWN);
  _gvn.set_type(rc, rc->Value(&_gvn));
  if (!rc_bool->is_Con()) {
    record_for_igvn(rc);
  }
  set_control(_gvn.transform(new IfTrueNode(rc)));
  {
    PreserveJVMState pjvms(this);
    set_control(_gvn.transform(new IfFalseNode(rc)));
    uncommon_trap(Deoptimization::Reason_range_check,
                  Deoptimization::Action_make_not_entrant);
  }

  if (stopped()) {
    return false;
  }

  Node* result = new CastIINode(index, TypeInt::make(0, _gvn.type(length)->is_int()->_hi, Type::WidenMax));
  result->set_req(0, control());
  result = _gvn.transform(result);
  set_result(result);
  replace_in_map(index, result);
  return true;
}

//------------------------------inline_string_indexOf------------------------
bool LibraryCallKit::inline_string_indexOf(StrIntrinsicNode::ArgEnc ae) {
  if (!Matcher::match_rule_supported(Op_StrIndexOf)) {
    return false;
  }
  Node* src = argument(0);
  Node* tgt = argument(1);

  // Make the merge point
  RegionNode* result_rgn = new RegionNode(4);
  Node*       result_phi = new PhiNode(result_rgn, TypeInt::INT);

  src = must_be_not_null(src, true);
  tgt = must_be_not_null(tgt, true);

  // Get start addr and length of source string
  Node* src_start = array_element_address(src, intcon(0), T_BYTE);
  Node* src_count = load_array_length(src);

  // Get start addr and length of substring
  Node* tgt_start = array_element_address(tgt, intcon(0), T_BYTE);
  Node* tgt_count = load_array_length(tgt);

  if (ae == StrIntrinsicNode::UU || ae == StrIntrinsicNode::UL) {
    // Divide src size by 2 if String is UTF16 encoded
    src_count = _gvn.transform(new RShiftINode(src_count, intcon(1)));
  }
  if (ae == StrIntrinsicNode::UU) {
    // Divide substring size by 2 if String is UTF16 encoded
    tgt_count = _gvn.transform(new RShiftINode(tgt_count, intcon(1)));
  }

  Node* result = make_indexOf_node(src_start, src_count, tgt_start, tgt_count, result_rgn, result_phi, ae);
  if (result != NULL) {
    result_phi->init_req(3, result);
    result_rgn->init_req(3, control());
  }
  set_control(_gvn.transform(result_rgn));
  record_for_igvn(result_rgn);
  set_result(_gvn.transform(result_phi));

  return true;
}

//-----------------------------inline_string_indexOf-----------------------
bool LibraryCallKit::inline_string_indexOfI(StrIntrinsicNode::ArgEnc ae) {
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }
  if (!Matcher::match_rule_supported(Op_StrIndexOf)) {
    return false;
  }
  assert(callee()->signature()->size() == 5, "String.indexOf() has 5 arguments");
  Node* src         = argument(0); // byte[]
  Node* src_count   = argument(1); // char count
  Node* tgt         = argument(2); // byte[]
  Node* tgt_count   = argument(3); // char count
  Node* from_index  = argument(4); // char index

  src = must_be_not_null(src, true);
  tgt = must_be_not_null(tgt, true);

  // Multiply byte array index by 2 if String is UTF16 encoded
  Node* src_offset = (ae == StrIntrinsicNode::LL) ? from_index : _gvn.transform(new LShiftINode(from_index, intcon(1)));
  src_count = _gvn.transform(new SubINode(src_count, from_index));
  Node* src_start = array_element_address(src, src_offset, T_BYTE);
  Node* tgt_start = array_element_address(tgt, intcon(0), T_BYTE);

  // Range checks
  generate_string_range_check(src, src_offset, src_count, ae != StrIntrinsicNode::LL);
  generate_string_range_check(tgt, intcon(0), tgt_count, ae == StrIntrinsicNode::UU);
  if (stopped()) {
    return true;
  }

  RegionNode* region = new RegionNode(5);
  Node* phi = new PhiNode(region, TypeInt::INT);

  Node* result = make_indexOf_node(src_start, src_count, tgt_start, tgt_count, region, phi, ae);
  if (result != NULL) {
    // The result is index relative to from_index if substring was found, -1 otherwise.
    // Generate code which will fold into cmove.
    Node* cmp = _gvn.transform(new CmpINode(result, intcon(0)));
    Node* bol = _gvn.transform(new BoolNode(cmp, BoolTest::lt));

    Node* if_lt = generate_slow_guard(bol, NULL);
    if (if_lt != NULL) {
      // result == -1
      phi->init_req(3, result);
      region->init_req(3, if_lt);
    }
    if (!stopped()) {
      result = _gvn.transform(new AddINode(result, from_index));
      phi->init_req(4, result);
      region->init_req(4, control());
    }
  }

  set_control(_gvn.transform(region));
  record_for_igvn(region);
  set_result(_gvn.transform(phi));
  clear_upper_avx();

  return true;
}

// Create StrIndexOfNode with fast path checks
Node* LibraryCallKit::make_indexOf_node(Node* src_start, Node* src_count, Node* tgt_start, Node* tgt_count,
                                        RegionNode* region, Node* phi, StrIntrinsicNode::ArgEnc ae) {
  // Check for substr count > string count
  Node* cmp = _gvn.transform(new CmpINode(tgt_count, src_count));
  Node* bol = _gvn.transform(new BoolNode(cmp, BoolTest::gt));
  Node* if_gt = generate_slow_guard(bol, NULL);
  if (if_gt != NULL) {
    phi->init_req(1, intcon(-1));
    region->init_req(1, if_gt);
  }
  if (!stopped()) {
    // Check for substr count == 0
    cmp = _gvn.transform(new CmpINode(tgt_count, intcon(0)));
    bol = _gvn.transform(new BoolNode(cmp, BoolTest::eq));
    Node* if_zero = generate_slow_guard(bol, NULL);
    if (if_zero != NULL) {
      phi->init_req(2, intcon(0));
      region->init_req(2, if_zero);
    }
  }
  if (!stopped()) {
    return make_string_method_node(Op_StrIndexOf, src_start, src_count, tgt_start, tgt_count, ae);
  }
  return NULL;
}

//-----------------------------inline_string_indexOfChar-----------------------
bool LibraryCallKit::inline_string_indexOfChar() {
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }
  if (!Matcher::match_rule_supported(Op_StrIndexOfChar)) {
    return false;
  }
  assert(callee()->signature()->size() == 4, "String.indexOfChar() has 4 arguments");
  Node* src         = argument(0); // byte[]
  Node* int_ch      = argument(1);
  Node* from_index  = argument(2);
  Node* max         = argument(3);

  src = must_be_not_null(src, true);

  Node* src_offset = _gvn.transform(new LShiftINode(from_index, intcon(1)));
  Node* src_start = array_element_address(src, src_offset, T_BYTE);
  Node* src_count = _gvn.transform(new SubINode(max, from_index));

  // Range checks
  generate_string_range_check(src, src_offset, src_count, true);

  // Check for int_ch >= 0
  Node* int_ch_cmp = _gvn.transform(new CmpINode(int_ch, intcon(0)));
  Node* int_ch_bol = _gvn.transform(new BoolNode(int_ch_cmp, BoolTest::ge));
  {
    BuildCutout unless(this, int_ch_bol, PROB_MAX);
    uncommon_trap(Deoptimization::Reason_intrinsic,
                  Deoptimization::Action_maybe_recompile);
  }
  if (stopped()) {
    return true;
  }

  RegionNode* region = new RegionNode(3);
  Node* phi = new PhiNode(region, TypeInt::INT);

  Node* result = new StrIndexOfCharNode(control(), memory(TypeAryPtr::BYTES), src_start, src_count, int_ch, StrIntrinsicNode::none);
  C->set_has_split_ifs(true); // Has chance for split-if optimization
  _gvn.transform(result);

  Node* cmp = _gvn.transform(new CmpINode(result, intcon(0)));
  Node* bol = _gvn.transform(new BoolNode(cmp, BoolTest::lt));

  Node* if_lt = generate_slow_guard(bol, NULL);
  if (if_lt != NULL) {
    // result == -1
    phi->init_req(2, result);
    region->init_req(2, if_lt);
  }
  if (!stopped()) {
    result = _gvn.transform(new AddINode(result, from_index));
    phi->init_req(1, result);
    region->init_req(1, control());
  }
  set_control(_gvn.transform(region));
  record_for_igvn(region);
  set_result(_gvn.transform(phi));

  return true;
}
//---------------------------inline_string_copy---------------------
// compressIt == true --> generate a compressed copy operation (compress char[]/byte[] to byte[])
//   int StringUTF16.compress(char[] src, int srcOff, byte[] dst, int dstOff, int len)
//   int StringUTF16.compress(byte[] src, int srcOff, byte[] dst, int dstOff, int len)
// compressIt == false --> generate an inflated copy operation (inflate byte[] to char[]/byte[])
//   void StringLatin1.inflate(byte[] src, int srcOff, char[] dst, int dstOff, int len)
//   void StringLatin1.inflate(byte[] src, int srcOff, byte[] dst, int dstOff, int len)
bool LibraryCallKit::inline_string_copy(bool compress) {
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }
  int nargs = 5;  // 2 oops, 3 ints
  assert(callee()->signature()->size() == nargs, "string copy has 5 arguments");

  Node* src         = argument(0);
  Node* src_offset  = argument(1);
  Node* dst         = argument(2);
  Node* dst_offset  = argument(3);
  Node* length      = argument(4);

  // Check for allocation before we add nodes that would confuse
  // tightly_coupled_allocation()
  AllocateArrayNode* alloc = tightly_coupled_allocation(dst, NULL);

  // Figure out the size and type of the elements we will be copying.
  const TypeAryPtr* src_type = src->Value(&_gvn)->isa_aryptr();
  const TypeAryPtr* dst_type = dst->Value(&_gvn)->isa_aryptr();
  if (src_type == NULL || dst_type == NULL) {
    return false;
  }
  BasicType src_elem = src_type->klass()->as_array_klass()->element_type()->basic_type();
  BasicType dst_elem = dst_type->klass()->as_array_klass()->element_type()->basic_type();
  assert((compress && dst_elem == T_BYTE && (src_elem == T_BYTE || src_elem == T_CHAR)) ||
         (!compress && src_elem == T_BYTE && (dst_elem == T_BYTE || dst_elem == T_CHAR)),
         "Unsupported array types for inline_string_copy");

  src = must_be_not_null(src, true);
  dst = must_be_not_null(dst, true);

  // Convert char[] offsets to byte[] offsets
  bool convert_src = (compress && src_elem == T_BYTE);
  bool convert_dst = (!compress && dst_elem == T_BYTE);
  if (convert_src) {
    src_offset = _gvn.transform(new LShiftINode(src_offset, intcon(1)));
  } else if (convert_dst) {
    dst_offset = _gvn.transform(new LShiftINode(dst_offset, intcon(1)));
  }

  // Range checks
  generate_string_range_check(src, src_offset, length, convert_src);
  generate_string_range_check(dst, dst_offset, length, convert_dst);
  if (stopped()) {
    return true;
  }

  Node* src_start = array_element_address(src, src_offset, src_elem);
  Node* dst_start = array_element_address(dst, dst_offset, dst_elem);
  // 'src_start' points to src array + scaled offset
  // 'dst_start' points to dst array + scaled offset
  Node* count = NULL;
  if (compress) {
    count = compress_string(src_start, TypeAryPtr::get_array_body_type(src_elem), dst_start, length);
  } else {
    inflate_string(src_start, dst_start, TypeAryPtr::get_array_body_type(dst_elem), length);
  }

  if (alloc != NULL) {
    if (alloc->maybe_set_complete(&_gvn)) {
      // "You break it, you buy it."
      InitializeNode* init = alloc->initialization();
      assert(init->is_complete(), "we just did this");
      init->set_complete_with_arraycopy();
      assert(dst->is_CheckCastPP(), "sanity");
      assert(dst->in(0)->in(0) == init, "dest pinned");
    }
    // Do not let stores that initialize this object be reordered with
    // a subsequent store that would make this object accessible by
    // other threads.
    // Record what AllocateNode this StoreStore protects so that
    // escape analysis can go from the MemBarStoreStoreNode to the
    // AllocateNode and eliminate the MemBarStoreStoreNode if possible
    // based on the escape status of the AllocateNode.
    insert_mem_bar(Op_MemBarStoreStore, alloc->proj_out_or_null(AllocateNode::RawAddress));
  }
  if (compress) {
    set_result(_gvn.transform(count));
  }
  clear_upper_avx();

  return true;
}

#ifdef _LP64
#define XTOP ,top() /*additional argument*/
#else  //_LP64
#define XTOP        /*no additional argument*/
#endif //_LP64

//------------------------inline_string_toBytesU--------------------------
// public static byte[] StringUTF16.toBytes(char[] value, int off, int len)
bool LibraryCallKit::inline_string_toBytesU() {
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }
  // Get the arguments.
  Node* value     = argument(0);
  Node* offset    = argument(1);
  Node* length    = argument(2);

  Node* newcopy = NULL;

  // Set the original stack and the reexecute bit for the interpreter to reexecute
  // the bytecode that invokes StringUTF16.toBytes() if deoptimization happens.
  { PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);

    // Check if a null path was taken unconditionally.
    value = null_check(value);

    RegionNode* bailout = new RegionNode(1);
    record_for_igvn(bailout);

    // Range checks
    generate_negative_guard(offset, bailout);
    generate_negative_guard(length, bailout);
    generate_limit_guard(offset, length, load_array_length(value), bailout);
    // Make sure that resulting byte[] length does not overflow Integer.MAX_VALUE
    generate_limit_guard(length, intcon(0), intcon(max_jint/2), bailout);

    if (bailout->req() > 1) {
      PreserveJVMState pjvms(this);
      set_control(_gvn.transform(bailout));
      uncommon_trap(Deoptimization::Reason_intrinsic,
                    Deoptimization::Action_maybe_recompile);
    }
    if (stopped()) {
      return true;
    }

    Node* size = _gvn.transform(new LShiftINode(length, intcon(1)));
    Node* klass_node = makecon(TypeKlassPtr::make(ciTypeArrayKlass::make(T_BYTE)));
    newcopy = new_array(klass_node, size, 0);  // no arguments to push
    AllocateArrayNode* alloc = tightly_coupled_allocation(newcopy, NULL);

    // Calculate starting addresses.
    Node* src_start = array_element_address(value, offset, T_CHAR);
    Node* dst_start = basic_plus_adr(newcopy, arrayOopDesc::base_offset_in_bytes(T_BYTE));

    // Check if src array address is aligned to HeapWordSize (dst is always aligned)
    const TypeInt* toffset = gvn().type(offset)->is_int();
    bool aligned = toffset->is_con() && ((toffset->get_con() * type2aelembytes(T_CHAR)) % HeapWordSize == 0);

    // Figure out which arraycopy runtime method to call (disjoint, uninitialized).
    const char* copyfunc_name = "arraycopy";
    address     copyfunc_addr = StubRoutines::select_arraycopy_function(T_CHAR, aligned, true, copyfunc_name, true);
    Node* call = make_runtime_call(RC_LEAF|RC_NO_FP,
                      OptoRuntime::fast_arraycopy_Type(),
                      copyfunc_addr, copyfunc_name, TypeRawPtr::BOTTOM,
                      src_start, dst_start, ConvI2X(length) XTOP);
    // Do not let reads from the cloned object float above the arraycopy.
    if (alloc != NULL) {
      if (alloc->maybe_set_complete(&_gvn)) {
        // "You break it, you buy it."
        InitializeNode* init = alloc->initialization();
        assert(init->is_complete(), "we just did this");
        init->set_complete_with_arraycopy();
        assert(newcopy->is_CheckCastPP(), "sanity");
        assert(newcopy->in(0)->in(0) == init, "dest pinned");
      }
      // Do not let stores that initialize this object be reordered with
      // a subsequent store that would make this object accessible by
      // other threads.
      // Record what AllocateNode this StoreStore protects so that
      // escape analysis can go from the MemBarStoreStoreNode to the
      // AllocateNode and eliminate the MemBarStoreStoreNode if possible
      // based on the escape status of the AllocateNode.
      insert_mem_bar(Op_MemBarStoreStore, alloc->proj_out_or_null(AllocateNode::RawAddress));
    } else {
      insert_mem_bar(Op_MemBarCPUOrder);
    }
  } // original reexecute is set back here

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  if (!stopped()) {
    set_result(newcopy);
  }
  clear_upper_avx();

  return true;
}

//------------------------inline_string_getCharsU--------------------------
// public void StringUTF16.getChars(byte[] src, int srcBegin, int srcEnd, char dst[], int dstBegin)
bool LibraryCallKit::inline_string_getCharsU() {
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }

  // Get the arguments.
  Node* src       = argument(0);
  Node* src_begin = argument(1);
  Node* src_end   = argument(2); // exclusive offset (i < src_end)
  Node* dst       = argument(3);
  Node* dst_begin = argument(4);

  // Check for allocation before we add nodes that would confuse
  // tightly_coupled_allocation()
  AllocateArrayNode* alloc = tightly_coupled_allocation(dst, NULL);

  // Check if a null path was taken unconditionally.
  src = null_check(src);
  dst = null_check(dst);
  if (stopped()) {
    return true;
  }

  // Get length and convert char[] offset to byte[] offset
  Node* length = _gvn.transform(new SubINode(src_end, src_begin));
  src_begin = _gvn.transform(new LShiftINode(src_begin, intcon(1)));

  // Range checks
  generate_string_range_check(src, src_begin, length, true);
  generate_string_range_check(dst, dst_begin, length, false);
  if (stopped()) {
    return true;
  }

  if (!stopped()) {
    // Calculate starting addresses.
    Node* src_start = array_element_address(src, src_begin, T_BYTE);
    Node* dst_start = array_element_address(dst, dst_begin, T_CHAR);

    // Check if array addresses are aligned to HeapWordSize
    const TypeInt* tsrc = gvn().type(src_begin)->is_int();
    const TypeInt* tdst = gvn().type(dst_begin)->is_int();
    bool aligned = tsrc->is_con() && ((tsrc->get_con() * type2aelembytes(T_BYTE)) % HeapWordSize == 0) &&
                   tdst->is_con() && ((tdst->get_con() * type2aelembytes(T_CHAR)) % HeapWordSize == 0);

    // Figure out which arraycopy runtime method to call (disjoint, uninitialized).
    const char* copyfunc_name = "arraycopy";
    address     copyfunc_addr = StubRoutines::select_arraycopy_function(T_CHAR, aligned, true, copyfunc_name, true);
    Node* call = make_runtime_call(RC_LEAF|RC_NO_FP,
                      OptoRuntime::fast_arraycopy_Type(),
                      copyfunc_addr, copyfunc_name, TypeRawPtr::BOTTOM,
                      src_start, dst_start, ConvI2X(length) XTOP);
    // Do not let reads from the cloned object float above the arraycopy.
    if (alloc != NULL) {
      if (alloc->maybe_set_complete(&_gvn)) {
        // "You break it, you buy it."
        InitializeNode* init = alloc->initialization();
        assert(init->is_complete(), "we just did this");
        init->set_complete_with_arraycopy();
        assert(dst->is_CheckCastPP(), "sanity");
        assert(dst->in(0)->in(0) == init, "dest pinned");
      }
      // Do not let stores that initialize this object be reordered with
      // a subsequent store that would make this object accessible by
      // other threads.
      // Record what AllocateNode this StoreStore protects so that
      // escape analysis can go from the MemBarStoreStoreNode to the
      // AllocateNode and eliminate the MemBarStoreStoreNode if possible
      // based on the escape status of the AllocateNode.
      insert_mem_bar(Op_MemBarStoreStore, alloc->proj_out_or_null(AllocateNode::RawAddress));
    } else {
      insert_mem_bar(Op_MemBarCPUOrder);
    }
  }

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  return true;
}

//----------------------inline_string_char_access----------------------------
// Store/Load char to/from byte[] array.
// static void StringUTF16.putChar(byte[] val, int index, int c)
// static char StringUTF16.getChar(byte[] val, int index)
bool LibraryCallKit::inline_string_char_access(bool is_store) {
  Node* value  = argument(0);
  Node* index  = argument(1);
  Node* ch = is_store ? argument(2) : NULL;

  // This intrinsic accesses byte[] array as char[] array. Computing the offsets
  // correctly requires matched array shapes.
  assert (arrayOopDesc::base_offset_in_bytes(T_CHAR) == arrayOopDesc::base_offset_in_bytes(T_BYTE),
          "sanity: byte[] and char[] bases agree");
  assert (type2aelembytes(T_CHAR) == type2aelembytes(T_BYTE)*2,
          "sanity: byte[] and char[] scales agree");

  // Bail when getChar over constants is requested: constant folding would
  // reject folding mismatched char access over byte[]. A normal inlining for getChar
  // Java method would constant fold nicely instead.
  if (!is_store && value->is_Con() && index->is_Con()) {
    return false;
  }

  value = must_be_not_null(value, true);

  Node* adr = array_element_address(value, index, T_CHAR);
  if (adr->is_top()) {
    return false;
  }
  if (is_store) {
    (void) store_to_memory(control(), adr, ch, T_CHAR, TypeAryPtr::BYTES, MemNode::unordered,
                           false, false, true /* mismatched */);
  } else {
    ch = make_load(control(), adr, TypeInt::CHAR, T_CHAR, TypeAryPtr::BYTES, MemNode::unordered,
                   LoadNode::Pinned, false, false, true /* mismatched */);
    set_result(ch);
  }
  return true;
}

//--------------------------round_double_node--------------------------------
// Round a double node if necessary.
Node* LibraryCallKit::round_double_node(Node* n) {
  if (Matcher::strict_fp_requires_explicit_rounding && UseSSE <= 1)
    n = _gvn.transform(new RoundDoubleNode(0, n));
  return n;
}

//------------------------------inline_math-----------------------------------
// public static double Math.abs(double)
// public static double Math.sqrt(double)
// public static double Math.log(double)
// public static double Math.log10(double)
bool LibraryCallKit::inline_double_math(vmIntrinsics::ID id) {
  Node* arg = round_double_node(argument(0));
  Node* n = NULL;
  switch (id) {
  case vmIntrinsics::_dabs:   n = new AbsDNode(                arg);  break;
  case vmIntrinsics::_dsqrt:  n = new SqrtDNode(C, control(),  arg);  break;
  case vmIntrinsics::_dcopySign: n = CopySignDNode::make(_gvn, arg, round_double_node(argument(2))); break;
  case vmIntrinsics::_dsignum: n = SignumDNode::make(_gvn, arg); break;
  case vmIntrinsics::_ceil:   n = new RoundDoubleModeNode(arg, makecon(TypeInt::make(2))); break;
  case vmIntrinsics::_floor:  n = new RoundDoubleModeNode(arg, makecon(TypeInt::make(1))); break;
  case vmIntrinsics::_rint:   n = new RoundDoubleModeNode(arg, makecon(TypeInt::make(0))); break;
  default:  fatal_unexpected_iid(id);  break;
  }
  set_result(_gvn.transform(n));
  return true;
}

//------------------------------inline_math-----------------------------------
// public static float Math.abs(float)
// public static int Math.abs(int)
// public static long Math.abs(long)
bool LibraryCallKit::inline_math(vmIntrinsics::ID id) {
  Node* arg = argument(0);
  Node* n = NULL;
  switch (id) {
  case vmIntrinsics::_fabs:   n = new AbsFNode(                arg);  break;
  case vmIntrinsics::_iabs:   n = new AbsINode(                arg);  break;
  case vmIntrinsics::_labs:   n = new AbsLNode(                arg);  break;
  case vmIntrinsics::_fcopySign: n = new CopySignFNode(arg, argument(1)); break;
  case vmIntrinsics::_fsignum: n = SignumFNode::make(_gvn, arg); break;
  default:  fatal_unexpected_iid(id);  break;
  }
  set_result(_gvn.transform(n));
  return true;
}

//------------------------------runtime_math-----------------------------
bool LibraryCallKit::runtime_math(const TypeFunc* call_type, address funcAddr, const char* funcName) {
  assert(call_type == OptoRuntime::Math_DD_D_Type() || call_type == OptoRuntime::Math_D_D_Type(),
         "must be (DD)D or (D)D type");

  // Inputs
  Node* a = round_double_node(argument(0));
  Node* b = (call_type == OptoRuntime::Math_DD_D_Type()) ? round_double_node(argument(2)) : NULL;

  const TypePtr* no_memory_effects = NULL;
  Node* trig = make_runtime_call(RC_LEAF, call_type, funcAddr, funcName,
                                 no_memory_effects,
                                 a, top(), b, b ? top() : NULL);
  Node* value = _gvn.transform(new ProjNode(trig, TypeFunc::Parms+0));
#ifdef ASSERT
  Node* value_top = _gvn.transform(new ProjNode(trig, TypeFunc::Parms+1));
  assert(value_top == top(), "second value must be top");
#endif

  set_result(value);
  return true;
}

//------------------------------inline_math_native-----------------------------
bool LibraryCallKit::inline_math_native(vmIntrinsics::ID id) {
#define FN_PTR(f) CAST_FROM_FN_PTR(address, f)
  switch (id) {
    // These intrinsics are not properly supported on all hardware
  case vmIntrinsics::_dsin:
    return StubRoutines::dsin() != NULL ?
      runtime_math(OptoRuntime::Math_D_D_Type(), StubRoutines::dsin(), "dsin") :
      runtime_math(OptoRuntime::Math_D_D_Type(), FN_PTR(SharedRuntime::dsin),   "SIN");
  case vmIntrinsics::_dcos:
    return StubRoutines::dcos() != NULL ?
      runtime_math(OptoRuntime::Math_D_D_Type(), StubRoutines::dcos(), "dcos") :
      runtime_math(OptoRuntime::Math_D_D_Type(), FN_PTR(SharedRuntime::dcos),   "COS");
  case vmIntrinsics::_dtan:
    return StubRoutines::dtan() != NULL ?
      runtime_math(OptoRuntime::Math_D_D_Type(), StubRoutines::dtan(), "dtan") :
      runtime_math(OptoRuntime::Math_D_D_Type(), FN_PTR(SharedRuntime::dtan), "TAN");
  case vmIntrinsics::_dlog:
    return StubRoutines::dlog() != NULL ?
      runtime_math(OptoRuntime::Math_D_D_Type(), StubRoutines::dlog(), "dlog") :
      runtime_math(OptoRuntime::Math_D_D_Type(), FN_PTR(SharedRuntime::dlog),   "LOG");
  case vmIntrinsics::_dlog10:
    return StubRoutines::dlog10() != NULL ?
      runtime_math(OptoRuntime::Math_D_D_Type(), StubRoutines::dlog10(), "dlog10") :
      runtime_math(OptoRuntime::Math_D_D_Type(), FN_PTR(SharedRuntime::dlog10), "LOG10");

    // These intrinsics are supported on all hardware
  case vmIntrinsics::_ceil:
  case vmIntrinsics::_floor:
  case vmIntrinsics::_rint:   return Matcher::match_rule_supported(Op_RoundDoubleMode) ? inline_double_math(id) : false;
  case vmIntrinsics::_dsqrt:  return Matcher::match_rule_supported(Op_SqrtD) ? inline_double_math(id) : false;
  case vmIntrinsics::_dabs:   return Matcher::has_match_rule(Op_AbsD)   ? inline_double_math(id) : false;
  case vmIntrinsics::_fabs:   return Matcher::match_rule_supported(Op_AbsF)   ? inline_math(id) : false;
  case vmIntrinsics::_iabs:   return Matcher::match_rule_supported(Op_AbsI)   ? inline_math(id) : false;
  case vmIntrinsics::_labs:   return Matcher::match_rule_supported(Op_AbsL)   ? inline_math(id) : false;

  case vmIntrinsics::_dexp:
    return StubRoutines::dexp() != NULL ?
      runtime_math(OptoRuntime::Math_D_D_Type(), StubRoutines::dexp(),  "dexp") :
      runtime_math(OptoRuntime::Math_D_D_Type(), FN_PTR(SharedRuntime::dexp),  "EXP");
  case vmIntrinsics::_dpow: {
    Node* exp = round_double_node(argument(2));
    const TypeD* d = _gvn.type(exp)->isa_double_constant();
    if (d != NULL && d->getd() == 2.0) {
      // Special case: pow(x, 2.0) => x * x
      Node* base = round_double_node(argument(0));
      set_result(_gvn.transform(new MulDNode(base, base)));
      return true;
    }
    return StubRoutines::dpow() != NULL ?
      runtime_math(OptoRuntime::Math_DD_D_Type(), StubRoutines::dpow(),  "dpow") :
      runtime_math(OptoRuntime::Math_DD_D_Type(), FN_PTR(SharedRuntime::dpow),  "POW");
  }
#undef FN_PTR

  case vmIntrinsics::_dcopySign: return inline_double_math(id);
  case vmIntrinsics::_fcopySign: return inline_math(id);
  case vmIntrinsics::_dsignum: return inline_double_math(id);
  case vmIntrinsics::_fsignum: return inline_math(id);

   // These intrinsics are not yet correctly implemented
  case vmIntrinsics::_datan2:
    return false;

  default:
    fatal_unexpected_iid(id);
    return false;
  }
}

static bool is_simple_name(Node* n) {
  return (n->req() == 1         // constant
          || (n->is_Type() && n->as_Type()->type()->singleton())
          || n->is_Proj()       // parameter or return value
          || n->is_Phi()        // local of some sort
          );
}

//----------------------------inline_notify-----------------------------------*
bool LibraryCallKit::inline_notify(vmIntrinsics::ID id) {
  const TypeFunc* ftype = OptoRuntime::monitor_notify_Type();
  address func;
  if (id == vmIntrinsics::_notify) {
    func = OptoRuntime::monitor_notify_Java();
  } else {
    func = OptoRuntime::monitor_notifyAll_Java();
  }
  Node* call = make_runtime_call(RC_NO_LEAF, ftype, func, NULL, TypeRawPtr::BOTTOM, argument(0));
  make_slow_call_ex(call, env()->Throwable_klass(), false);
  return true;
}


//----------------------------inline_min_max-----------------------------------
bool LibraryCallKit::inline_min_max(vmIntrinsics::ID id) {
  set_result(generate_min_max(id, argument(0), argument(1)));
  return true;
}

void LibraryCallKit::inline_math_mathExact(Node* math, Node *test) {
  Node* bol = _gvn.transform( new BoolNode(test, BoolTest::overflow) );
  IfNode* check = create_and_map_if(control(), bol, PROB_UNLIKELY_MAG(3), COUNT_UNKNOWN);
  Node* fast_path = _gvn.transform( new IfFalseNode(check));
  Node* slow_path = _gvn.transform( new IfTrueNode(check) );

  {
    PreserveJVMState pjvms(this);
    PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);

    set_control(slow_path);
    set_i_o(i_o());

    uncommon_trap(Deoptimization::Reason_intrinsic,
                  Deoptimization::Action_none);
  }

  set_control(fast_path);
  set_result(math);
}

template <typename OverflowOp>
bool LibraryCallKit::inline_math_overflow(Node* arg1, Node* arg2) {
  typedef typename OverflowOp::MathOp MathOp;

  MathOp* mathOp = new MathOp(arg1, arg2);
  Node* operation = _gvn.transform( mathOp );
  Node* ofcheck = _gvn.transform( new OverflowOp(arg1, arg2) );
  inline_math_mathExact(operation, ofcheck);
  return true;
}

bool LibraryCallKit::inline_math_addExactI(bool is_increment) {
  return inline_math_overflow<OverflowAddINode>(argument(0), is_increment ? intcon(1) : argument(1));
}

bool LibraryCallKit::inline_math_addExactL(bool is_increment) {
  return inline_math_overflow<OverflowAddLNode>(argument(0), is_increment ? longcon(1) : argument(2));
}

bool LibraryCallKit::inline_math_subtractExactI(bool is_decrement) {
  return inline_math_overflow<OverflowSubINode>(argument(0), is_decrement ? intcon(1) : argument(1));
}

bool LibraryCallKit::inline_math_subtractExactL(bool is_decrement) {
  return inline_math_overflow<OverflowSubLNode>(argument(0), is_decrement ? longcon(1) : argument(2));
}

bool LibraryCallKit::inline_math_negateExactI() {
  return inline_math_overflow<OverflowSubINode>(intcon(0), argument(0));
}

bool LibraryCallKit::inline_math_negateExactL() {
  return inline_math_overflow<OverflowSubLNode>(longcon(0), argument(0));
}

bool LibraryCallKit::inline_math_multiplyExactI() {
  return inline_math_overflow<OverflowMulINode>(argument(0), argument(1));
}

bool LibraryCallKit::inline_math_multiplyExactL() {
  return inline_math_overflow<OverflowMulLNode>(argument(0), argument(2));
}

bool LibraryCallKit::inline_math_multiplyHigh() {
  set_result(_gvn.transform(new MulHiLNode(argument(0), argument(2))));
  return true;
}

Node*
LibraryCallKit::generate_min_max(vmIntrinsics::ID id, Node* x0, Node* y0) {
  // These are the candidate return value:
  Node* xvalue = x0;
  Node* yvalue = y0;

  if (xvalue == yvalue) {
    return xvalue;
  }

  bool want_max = (id == vmIntrinsics::_max);

  const TypeInt* txvalue = _gvn.type(xvalue)->isa_int();
  const TypeInt* tyvalue = _gvn.type(yvalue)->isa_int();
  if (txvalue == NULL || tyvalue == NULL)  return top();
  // This is not really necessary, but it is consistent with a
  // hypothetical MaxINode::Value method:
  int widen = MAX2(txvalue->_widen, tyvalue->_widen);

  // %%% This folding logic should (ideally) be in a different place.
  // Some should be inside IfNode, and there to be a more reliable
  // transformation of ?: style patterns into cmoves.  We also want
  // more powerful optimizations around cmove and min/max.

  // Try to find a dominating comparison of these guys.
  // It can simplify the index computation for Arrays.copyOf
  // and similar uses of System.arraycopy.
  // First, compute the normalized version of CmpI(x, y).
  int   cmp_op = Op_CmpI;
  Node* xkey = xvalue;
  Node* ykey = yvalue;
  Node* ideal_cmpxy = _gvn.transform(new CmpINode(xkey, ykey));
  if (ideal_cmpxy->is_Cmp()) {
    // E.g., if we have CmpI(length - offset, count),
    // it might idealize to CmpI(length, count + offset)
    cmp_op = ideal_cmpxy->Opcode();
    xkey = ideal_cmpxy->in(1);
    ykey = ideal_cmpxy->in(2);
  }

  // Start by locating any relevant comparisons.
  Node* start_from = (xkey->outcnt() < ykey->outcnt()) ? xkey : ykey;
  Node* cmpxy = NULL;
  Node* cmpyx = NULL;
  for (DUIterator_Fast kmax, k = start_from->fast_outs(kmax); k < kmax; k++) {
    Node* cmp = start_from->fast_out(k);
    if (cmp->outcnt() > 0 &&            // must have prior uses
        cmp->in(0) == NULL &&           // must be context-independent
        cmp->Opcode() == cmp_op) {      // right kind of compare
      if (cmp->in(1) == xkey && cmp->in(2) == ykey)  cmpxy = cmp;
      if (cmp->in(1) == ykey && cmp->in(2) == xkey)  cmpyx = cmp;
    }
  }

  const int NCMPS = 2;
  Node* cmps[NCMPS] = { cmpxy, cmpyx };
  int cmpn;
  for (cmpn = 0; cmpn < NCMPS; cmpn++) {
    if (cmps[cmpn] != NULL)  break;     // find a result
  }
  if (cmpn < NCMPS) {
    // Look for a dominating test that tells us the min and max.
    int depth = 0;                // Limit search depth for speed
    Node* dom = control();
    for (; dom != NULL; dom = IfNode::up_one_dom(dom, true)) {
      if (++depth >= 100)  break;
      Node* ifproj = dom;
      if (!ifproj->is_Proj())  continue;
      Node* iff = ifproj->in(0);
      if (!iff->is_If())  continue;
      Node* bol = iff->in(1);
      if (!bol->is_Bool())  continue;
      Node* cmp = bol->in(1);
      if (cmp == NULL)  continue;
      for (cmpn = 0; cmpn < NCMPS; cmpn++)
        if (cmps[cmpn] == cmp)  break;
      if (cmpn == NCMPS)  continue;
      BoolTest::mask btest = bol->as_Bool()->_test._test;
      if (ifproj->is_IfFalse())  btest = BoolTest(btest).negate();
      if (cmp->in(1) == ykey)    btest = BoolTest(btest).commute();
      // At this point, we know that 'x btest y' is true.
      switch (btest) {
      case BoolTest::eq:
        // They are proven equal, so we can collapse the min/max.
        // Either value is the answer.  Choose the simpler.
        if (is_simple_name(yvalue) && !is_simple_name(xvalue))
          return yvalue;
        return xvalue;
      case BoolTest::lt:          // x < y
      case BoolTest::le:          // x <= y
        return (want_max ? yvalue : xvalue);
      case BoolTest::gt:          // x > y
      case BoolTest::ge:          // x >= y
        return (want_max ? xvalue : yvalue);
      default:
        break;
      }
    }
  }

  // We failed to find a dominating test.
  // Let's pick a test that might GVN with prior tests.
  Node*          best_bol   = NULL;
  BoolTest::mask best_btest = BoolTest::illegal;
  for (cmpn = 0; cmpn < NCMPS; cmpn++) {
    Node* cmp = cmps[cmpn];
    if (cmp == NULL)  continue;
    for (DUIterator_Fast jmax, j = cmp->fast_outs(jmax); j < jmax; j++) {
      Node* bol = cmp->fast_out(j);
      if (!bol->is_Bool())  continue;
      BoolTest::mask btest = bol->as_Bool()->_test._test;
      if (btest == BoolTest::eq || btest == BoolTest::ne)  continue;
      if (cmp->in(1) == ykey)   btest = BoolTest(btest).commute();
      if (bol->outcnt() > (best_bol == NULL ? 0 : best_bol->outcnt())) {
        best_bol   = bol->as_Bool();
        best_btest = btest;
      }
    }
  }

  Node* answer_if_true  = NULL;
  Node* answer_if_false = NULL;
  switch (best_btest) {
  default:
    if (cmpxy == NULL)
      cmpxy = ideal_cmpxy;
    best_bol = _gvn.transform(new BoolNode(cmpxy, BoolTest::lt));
    // and fall through:
  case BoolTest::lt:          // x < y
  case BoolTest::le:          // x <= y
    answer_if_true  = (want_max ? yvalue : xvalue);
    answer_if_false = (want_max ? xvalue : yvalue);
    break;
  case BoolTest::gt:          // x > y
  case BoolTest::ge:          // x >= y
    answer_if_true  = (want_max ? xvalue : yvalue);
    answer_if_false = (want_max ? yvalue : xvalue);
    break;
  }

  jint hi, lo;
  if (want_max) {
    // We can sharpen the minimum.
    hi = MAX2(txvalue->_hi, tyvalue->_hi);
    lo = MAX2(txvalue->_lo, tyvalue->_lo);
  } else {
    // We can sharpen the maximum.
    hi = MIN2(txvalue->_hi, tyvalue->_hi);
    lo = MIN2(txvalue->_lo, tyvalue->_lo);
  }

  // Use a flow-free graph structure, to avoid creating excess control edges
  // which could hinder other optimizations.
  // Since Math.min/max is often used with arraycopy, we want
  // tightly_coupled_allocation to be able to see beyond min/max expressions.
  Node* cmov = CMoveNode::make(NULL, best_bol,
                               answer_if_false, answer_if_true,
                               TypeInt::make(lo, hi, widen));

  return _gvn.transform(cmov);

  /*
  // This is not as desirable as it may seem, since Min and Max
  // nodes do not have a full set of optimizations.
  // And they would interfere, anyway, with 'if' optimizations
  // and with CMoveI canonical forms.
  switch (id) {
  case vmIntrinsics::_min:
    result_val = _gvn.transform(new (C, 3) MinINode(x,y)); break;
  case vmIntrinsics::_max:
    result_val = _gvn.transform(new (C, 3) MaxINode(x,y)); break;
  default:
    ShouldNotReachHere();
  }
  */
}

inline int
LibraryCallKit::classify_unsafe_addr(Node* &base, Node* &offset, BasicType type) {
  const TypePtr* base_type = TypePtr::NULL_PTR;
  if (base != NULL)  base_type = _gvn.type(base)->isa_ptr();
  if (base_type == NULL) {
    // Unknown type.
    return Type::AnyPtr;
  } else if (base_type == TypePtr::NULL_PTR) {
    // Since this is a NULL+long form, we have to switch to a rawptr.
    base   = _gvn.transform(new CastX2PNode(offset));
    offset = MakeConX(0);
    return Type::RawPtr;
  } else if (base_type->base() == Type::RawPtr) {
    return Type::RawPtr;
  } else if (base_type->isa_oopptr()) {
    // Base is never null => always a heap address.
    if (!TypePtr::NULL_PTR->higher_equal(base_type)) {
      return Type::OopPtr;
    }
    // Offset is small => always a heap address.
    const TypeX* offset_type = _gvn.type(offset)->isa_intptr_t();
    if (offset_type != NULL &&
        base_type->offset() == 0 &&     // (should always be?)
        offset_type->_lo >= 0 &&
        !MacroAssembler::needs_explicit_null_check(offset_type->_hi)) {
      return Type::OopPtr;
    } else if (type == T_OBJECT) {
      // off heap access to an oop doesn't make any sense. Has to be on
      // heap.
      return Type::OopPtr;
    }
    // Otherwise, it might either be oop+off or NULL+addr.
    return Type::AnyPtr;
  } else {
    // No information:
    return Type::AnyPtr;
  }
}

inline Node* LibraryCallKit::make_unsafe_address(Node*& base, Node* offset, BasicType type, bool can_cast) {
  Node* uncasted_base = base;
  int kind = classify_unsafe_addr(uncasted_base, offset, type);
  if (kind == Type::RawPtr) {
    return basic_plus_adr(top(), uncasted_base, offset);
  } else if (kind == Type::AnyPtr) {
    assert(base == uncasted_base, "unexpected base change");
    if (can_cast) {
      if (!_gvn.type(base)->speculative_maybe_null() &&
          !too_many_traps(Deoptimization::Reason_speculate_null_check)) {
        // According to profiling, this access is always on
        // heap. Casting the base to not null and thus avoiding membars
        // around the access should allow better optimizations
        Node* null_ctl = top();
        base = null_check_oop(base, &null_ctl, true, true, true);
        assert(null_ctl->is_top(), "no null control here");
        return basic_plus_adr(base, offset);
      } else if (_gvn.type(base)->speculative_always_null() &&
                 !too_many_traps(Deoptimization::Reason_speculate_null_assert)) {
        // According to profiling, this access is always off
        // heap.
        base = null_assert(base);
        Node* raw_base = _gvn.transform(new CastX2PNode(offset));
        offset = MakeConX(0);
        return basic_plus_adr(top(), raw_base, offset);
      }
    }
    // We don't know if it's an on heap or off heap access. Fall back
    // to raw memory access.
    Node* raw = _gvn.transform(new CheckCastPPNode(control(), base, TypeRawPtr::BOTTOM));
    return basic_plus_adr(top(), raw, offset);
  } else {
    assert(base == uncasted_base, "unexpected base change");
    // We know it's an on heap access so base can't be null
    if (TypePtr::NULL_PTR->higher_equal(_gvn.type(base))) {
      base = must_be_not_null(base, true);
    }
    return basic_plus_adr(base, offset);
  }
}

//--------------------------inline_number_methods-----------------------------
// inline int     Integer.numberOfLeadingZeros(int)
// inline int        Long.numberOfLeadingZeros(long)
//
// inline int     Integer.numberOfTrailingZeros(int)
// inline int        Long.numberOfTrailingZeros(long)
//
// inline int     Integer.bitCount(int)
// inline int        Long.bitCount(long)
//
// inline char  Character.reverseBytes(char)
// inline short     Short.reverseBytes(short)
// inline int     Integer.reverseBytes(int)
// inline long       Long.reverseBytes(long)
bool LibraryCallKit::inline_number_methods(vmIntrinsics::ID id) {
  Node* arg = argument(0);
  Node* n = NULL;
  switch (id) {
  case vmIntrinsics::_numberOfLeadingZeros_i:   n = new CountLeadingZerosINode( arg);  break;
  case vmIntrinsics::_numberOfLeadingZeros_l:   n = new CountLeadingZerosLNode( arg);  break;
  case vmIntrinsics::_numberOfTrailingZeros_i:  n = new CountTrailingZerosINode(arg);  break;
  case vmIntrinsics::_numberOfTrailingZeros_l:  n = new CountTrailingZerosLNode(arg);  break;
  case vmIntrinsics::_bitCount_i:               n = new PopCountINode(          arg);  break;
  case vmIntrinsics::_bitCount_l:               n = new PopCountLNode(          arg);  break;
  case vmIntrinsics::_reverseBytes_c:           n = new ReverseBytesUSNode(0,   arg);  break;
  case vmIntrinsics::_reverseBytes_s:           n = new ReverseBytesSNode( 0,   arg);  break;
  case vmIntrinsics::_reverseBytes_i:           n = new ReverseBytesINode( 0,   arg);  break;
  case vmIntrinsics::_reverseBytes_l:           n = new ReverseBytesLNode( 0,   arg);  break;
  default:  fatal_unexpected_iid(id);  break;
  }
  set_result(_gvn.transform(n));
  return true;
}

//----------------------------inline_unsafe_access----------------------------

const TypeOopPtr* LibraryCallKit::sharpen_unsafe_type(Compile::AliasType* alias_type, const TypePtr *adr_type) {
  // Attempt to infer a sharper value type from the offset and base type.
  ciKlass* sharpened_klass = NULL;

  // See if it is an instance field, with an object type.
  if (alias_type->field() != NULL) {
    if (alias_type->field()->type()->is_klass()) {
      sharpened_klass = alias_type->field()->type()->as_klass();
    }
  }

  // See if it is a narrow oop array.
  if (adr_type->isa_aryptr()) {
    if (adr_type->offset() >= objArrayOopDesc::base_offset_in_bytes()) {
      const TypeOopPtr *elem_type = adr_type->is_aryptr()->elem()->isa_oopptr();
      if (elem_type != NULL) {
        sharpened_klass = elem_type->klass();
      }
    }
  }

  // The sharpened class might be unloaded if there is no class loader
  // contraint in place.
  if (sharpened_klass != NULL && sharpened_klass->is_loaded()) {
    const TypeOopPtr* tjp = TypeOopPtr::make_from_klass(sharpened_klass);

#ifndef PRODUCT
    if (C->print_intrinsics() || C->print_inlining()) {
      tty->print("  from base type:  ");  adr_type->dump(); tty->cr();
      tty->print("  sharpened value: ");  tjp->dump();      tty->cr();
    }
#endif
    // Sharpen the value type.
    return tjp;
  }
  return NULL;
}

DecoratorSet LibraryCallKit::mo_decorator_for_access_kind(AccessKind kind) {
  switch (kind) {
      case Relaxed:
        return MO_UNORDERED;
      case Opaque:
        return MO_RELAXED;
      case Acquire:
        return MO_ACQUIRE;
      case Release:
        return MO_RELEASE;
      case Volatile:
        return MO_SEQ_CST;
      default:
        ShouldNotReachHere();
        return 0;
  }
}

bool LibraryCallKit::inline_unsafe_access(bool is_store, const BasicType type, const AccessKind kind, const bool unaligned) {
  if (callee()->is_static())  return false;  // caller must have the capability!
  DecoratorSet decorators = C2_UNSAFE_ACCESS;
  guarantee(!is_store || kind != Acquire, "Acquire accesses can be produced only for loads");
  guarantee( is_store || kind != Release, "Release accesses can be produced only for stores");
  assert(type != T_OBJECT || !unaligned, "unaligned access not supported with object type");

  if (type == T_OBJECT || type == T_ARRAY) {
    decorators |= ON_UNKNOWN_OOP_REF;
  }

  if (unaligned) {
    decorators |= C2_UNALIGNED;
  }

#ifndef PRODUCT
  {
    ResourceMark rm;
    // Check the signatures.
    ciSignature* sig = callee()->signature();
#ifdef ASSERT
    if (!is_store) {
      // Object getObject(Object base, int/long offset), etc.
      BasicType rtype = sig->return_type()->basic_type();
      assert(rtype == type, "getter must return the expected value");
      assert(sig->count() == 2, "oop getter has 2 arguments");
      assert(sig->type_at(0)->basic_type() == T_OBJECT, "getter base is object");
      assert(sig->type_at(1)->basic_type() == T_LONG, "getter offset is correct");
    } else {
      // void putObject(Object base, int/long offset, Object x), etc.
      assert(sig->return_type()->basic_type() == T_VOID, "putter must not return a value");
      assert(sig->count() == 3, "oop putter has 3 arguments");
      assert(sig->type_at(0)->basic_type() == T_OBJECT, "putter base is object");
      assert(sig->type_at(1)->basic_type() == T_LONG, "putter offset is correct");
      BasicType vtype = sig->type_at(sig->count()-1)->basic_type();
      assert(vtype == type, "putter must accept the expected value");
    }
#endif // ASSERT
 }
#endif //PRODUCT

  C->set_has_unsafe_access(true);  // Mark eventual nmethod as "unsafe".

  Node* receiver = argument(0);  // type: oop

  // Build address expression.
  Node* adr;
  Node* heap_base_oop = top();
  Node* offset = top();
  Node* val;

  // The base is either a Java object or a value produced by Unsafe.staticFieldBase
  Node* base = argument(1);  // type: oop
  // The offset is a value produced by Unsafe.staticFieldOffset or Unsafe.objectFieldOffset
  offset = argument(2);  // type: long
  // We currently rely on the cookies produced by Unsafe.xxxFieldOffset
  // to be plain byte offsets, which are also the same as those accepted
  // by oopDesc::field_addr.
  assert(Unsafe_field_offset_to_byte_offset(11) == 11,
         "fieldOffset must be byte-scaled");
  // 32-bit machines ignore the high half!
  offset = ConvL2X(offset);
  adr = make_unsafe_address(base, offset, type, kind == Relaxed);

  if (_gvn.type(base)->isa_ptr() == TypePtr::NULL_PTR) {
    if (type != T_OBJECT) {
      decorators |= IN_NATIVE; // off-heap primitive access
    } else {
      return false; // off-heap oop accesses are not supported
    }
  } else {
    heap_base_oop = base; // on-heap or mixed access
  }

  // Can base be NULL? Otherwise, always on-heap access.
  bool can_access_non_heap = TypePtr::NULL_PTR->higher_equal(_gvn.type(base));

  if (!can_access_non_heap) {
    decorators |= IN_HEAP;
  }

  val = is_store ? argument(4) : NULL;

  const TypePtr* adr_type = _gvn.type(adr)->isa_ptr();
  if (adr_type == TypePtr::NULL_PTR) {
    return false; // off-heap access with zero address
  }

  // Try to categorize the address.
  Compile::AliasType* alias_type = C->alias_type(adr_type);
  assert(alias_type->index() != Compile::AliasIdxBot, "no bare pointers here");

  if (alias_type->adr_type() == TypeInstPtr::KLASS ||
      alias_type->adr_type() == TypeAryPtr::RANGE) {
    return false; // not supported
  }

  bool mismatched = false;
  BasicType bt = alias_type->basic_type();
  if (bt != T_ILLEGAL) {
    assert(alias_type->adr_type()->is_oopptr(), "should be on-heap access");
    if (bt == T_BYTE && adr_type->isa_aryptr()) {
      // Alias type doesn't differentiate between byte[] and boolean[]).
      // Use address type to get the element type.
      bt = adr_type->is_aryptr()->elem()->array_element_basic_type();
    }
    if (bt == T_ARRAY || bt == T_NARROWOOP) {
      // accessing an array field with getObject is not a mismatch
      bt = T_OBJECT;
    }
    if ((bt == T_OBJECT) != (type == T_OBJECT)) {
      // Don't intrinsify mismatched object accesses
      return false;
    }
    mismatched = (bt != type);
  } else if (alias_type->adr_type()->isa_oopptr()) {
    mismatched = true; // conservatively mark all "wide" on-heap accesses as mismatched
  }

  assert(!mismatched || alias_type->adr_type()->is_oopptr(), "off-heap access can't be mismatched");

  if (mismatched) {
    decorators |= C2_MISMATCHED;
  }

  // First guess at the value type.
  const Type *value_type = Type::get_const_basic_type(type);

  // Figure out the memory ordering.
  decorators |= mo_decorator_for_access_kind(kind);

  if (!is_store && type == T_OBJECT) {
    const TypeOopPtr* tjp = sharpen_unsafe_type(alias_type, adr_type);
    if (tjp != NULL) {
      value_type = tjp;
    }
  }

  receiver = null_check(receiver);
  if (stopped()) {
    return true;
  }
  // Heap pointers get a null-check from the interpreter,
  // as a courtesy.  However, this is not guaranteed by Unsafe,
  // and it is not possible to fully distinguish unintended nulls
  // from intended ones in this API.

  if (!is_store) {
    Node* p = NULL;
    // Try to constant fold a load from a constant field
    ciField* field = alias_type->field();
    if (heap_base_oop != top() && field != NULL && field->is_constant() && !mismatched) {
      // final or stable field
      p = make_constant_from_field(field, heap_base_oop);
    }

    if (p == NULL) { // Could not constant fold the load
      p = access_load_at(heap_base_oop, adr, adr_type, value_type, type, decorators);
      // Normalize the value returned by getBoolean in the following cases
      if (type == T_BOOLEAN &&
          (mismatched ||
           heap_base_oop == top() ||                  // - heap_base_oop is NULL or
           (can_access_non_heap && field == NULL))    // - heap_base_oop is potentially NULL
                                                      //   and the unsafe access is made to large offset
                                                      //   (i.e., larger than the maximum offset necessary for any
                                                      //   field access)
            ) {
          IdealKit ideal = IdealKit(this);
#define __ ideal.
          IdealVariable normalized_result(ideal);
          __ declarations_done();
          __ set(normalized_result, p);
          __ if_then(p, BoolTest::ne, ideal.ConI(0));
          __ set(normalized_result, ideal.ConI(1));
          ideal.end_if();
          final_sync(ideal);
          p = __ value(normalized_result);
#undef __
      }
    }
    if (type == T_ADDRESS) {
      p = gvn().transform(new CastP2XNode(NULL, p));
      p = ConvX2UL(p);
    }
    // The load node has the control of the preceding MemBarCPUOrder.  All
    // following nodes will have the control of the MemBarCPUOrder inserted at
    // the end of this method.  So, pushing the load onto the stack at a later
    // point is fine.
    set_result(p);
  } else {
    if (bt == T_ADDRESS) {
      // Repackage the long as a pointer.
      val = ConvL2X(val);
      val = gvn().transform(new CastX2PNode(val));
    }
    access_store_at(control(), heap_base_oop, adr, adr_type, val, value_type, type, decorators);
  }

  return true;
}

//----------------------------inline_unsafe_load_store----------------------------
// This method serves a couple of different customers (depending on LoadStoreKind):
//
// LS_cmp_swap:
//
//   boolean compareAndSetObject(Object o, long offset, Object expected, Object x);
//   boolean compareAndSetInt(   Object o, long offset, int    expected, int    x);
//   boolean compareAndSetLong(  Object o, long offset, long   expected, long   x);
//
// LS_cmp_swap_weak:
//
//   boolean weakCompareAndSetObject(       Object o, long offset, Object expected, Object x);
//   boolean weakCompareAndSetObjectPlain(  Object o, long offset, Object expected, Object x);
//   boolean weakCompareAndSetObjectAcquire(Object o, long offset, Object expected, Object x);
//   boolean weakCompareAndSetObjectRelease(Object o, long offset, Object expected, Object x);
//
//   boolean weakCompareAndSetInt(          Object o, long offset, int    expected, int    x);
//   boolean weakCompareAndSetIntPlain(     Object o, long offset, int    expected, int    x);
//   boolean weakCompareAndSetIntAcquire(   Object o, long offset, int    expected, int    x);
//   boolean weakCompareAndSetIntRelease(   Object o, long offset, int    expected, int    x);
//
//   boolean weakCompareAndSetLong(         Object o, long offset, long   expected, long   x);
//   boolean weakCompareAndSetLongPlain(    Object o, long offset, long   expected, long   x);
//   boolean weakCompareAndSetLongAcquire(  Object o, long offset, long   expected, long   x);
//   boolean weakCompareAndSetLongRelease(  Object o, long offset, long   expected, long   x);
//
// LS_cmp_exchange:
//
//   Object compareAndExchangeObjectVolatile(Object o, long offset, Object expected, Object x);
//   Object compareAndExchangeObjectAcquire( Object o, long offset, Object expected, Object x);
//   Object compareAndExchangeObjectRelease( Object o, long offset, Object expected, Object x);
//
//   Object compareAndExchangeIntVolatile(   Object o, long offset, Object expected, Object x);
//   Object compareAndExchangeIntAcquire(    Object o, long offset, Object expected, Object x);
//   Object compareAndExchangeIntRelease(    Object o, long offset, Object expected, Object x);
//
//   Object compareAndExchangeLongVolatile(  Object o, long offset, Object expected, Object x);
//   Object compareAndExchangeLongAcquire(   Object o, long offset, Object expected, Object x);
//   Object compareAndExchangeLongRelease(   Object o, long offset, Object expected, Object x);
//
// LS_get_add:
//
//   int  getAndAddInt( Object o, long offset, int  delta)
//   long getAndAddLong(Object o, long offset, long delta)
//
// LS_get_set:
//
//   int    getAndSet(Object o, long offset, int    newValue)
//   long   getAndSet(Object o, long offset, long   newValue)
//   Object getAndSet(Object o, long offset, Object newValue)
//
bool LibraryCallKit::inline_unsafe_load_store(const BasicType type, const LoadStoreKind kind, const AccessKind access_kind) {
  // This basic scheme here is the same as inline_unsafe_access, but
  // differs in enough details that combining them would make the code
  // overly confusing.  (This is a true fact! I originally combined
  // them, but even I was confused by it!) As much code/comments as
  // possible are retained from inline_unsafe_access though to make
  // the correspondences clearer. - dl

  if (callee()->is_static())  return false;  // caller must have the capability!

  DecoratorSet decorators = C2_UNSAFE_ACCESS;
  decorators |= mo_decorator_for_access_kind(access_kind);

#ifndef PRODUCT
  BasicType rtype;
  {
    ResourceMark rm;
    // Check the signatures.
    ciSignature* sig = callee()->signature();
    rtype = sig->return_type()->basic_type();
    switch(kind) {
      case LS_get_add:
      case LS_get_set: {
      // Check the signatures.
#ifdef ASSERT
      assert(rtype == type, "get and set must return the expected type");
      assert(sig->count() == 3, "get and set has 3 arguments");
      assert(sig->type_at(0)->basic_type() == T_OBJECT, "get and set base is object");
      assert(sig->type_at(1)->basic_type() == T_LONG, "get and set offset is long");
      assert(sig->type_at(2)->basic_type() == type, "get and set must take expected type as new value/delta");
      assert(access_kind == Volatile, "mo is not passed to intrinsic nodes in current implementation");
#endif // ASSERT
        break;
      }
      case LS_cmp_swap:
      case LS_cmp_swap_weak: {
      // Check the signatures.
#ifdef ASSERT
      assert(rtype == T_BOOLEAN, "CAS must return boolean");
      assert(sig->count() == 4, "CAS has 4 arguments");
      assert(sig->type_at(0)->basic_type() == T_OBJECT, "CAS base is object");
      assert(sig->type_at(1)->basic_type() == T_LONG, "CAS offset is long");
#endif // ASSERT
        break;
      }
      case LS_cmp_exchange: {
      // Check the signatures.
#ifdef ASSERT
      assert(rtype == type, "CAS must return the expected type");
      assert(sig->count() == 4, "CAS has 4 arguments");
      assert(sig->type_at(0)->basic_type() == T_OBJECT, "CAS base is object");
      assert(sig->type_at(1)->basic_type() == T_LONG, "CAS offset is long");
#endif // ASSERT
        break;
      }
      default:
        ShouldNotReachHere();
    }
  }
#endif //PRODUCT

  C->set_has_unsafe_access(true);  // Mark eventual nmethod as "unsafe".

  // Get arguments:
  Node* receiver = NULL;
  Node* base     = NULL;
  Node* offset   = NULL;
  Node* oldval   = NULL;
  Node* newval   = NULL;
  switch(kind) {
    case LS_cmp_swap:
    case LS_cmp_swap_weak:
    case LS_cmp_exchange: {
      const bool two_slot_type = type2size[type] == 2;
      receiver = argument(0);  // type: oop
      base     = argument(1);  // type: oop
      offset   = argument(2);  // type: long
      oldval   = argument(4);  // type: oop, int, or long
      newval   = argument(two_slot_type ? 6 : 5);  // type: oop, int, or long
      break;
    }
    case LS_get_add:
    case LS_get_set: {
      receiver = argument(0);  // type: oop
      base     = argument(1);  // type: oop
      offset   = argument(2);  // type: long
      oldval   = NULL;
      newval   = argument(4);  // type: oop, int, or long
      break;
    }
    default:
      ShouldNotReachHere();
  }

  // Build field offset expression.
  // We currently rely on the cookies produced by Unsafe.xxxFieldOffset
  // to be plain byte offsets, which are also the same as those accepted
  // by oopDesc::field_addr.
  assert(Unsafe_field_offset_to_byte_offset(11) == 11, "fieldOffset must be byte-scaled");
  // 32-bit machines ignore the high half of long offsets
  offset = ConvL2X(offset);
  Node* adr = make_unsafe_address(base, offset, type, false);
  const TypePtr *adr_type = _gvn.type(adr)->isa_ptr();

  Compile::AliasType* alias_type = C->alias_type(adr_type);
  BasicType bt = alias_type->basic_type();
  if (bt != T_ILLEGAL &&
      ((bt == T_OBJECT || bt == T_ARRAY) != (type == T_OBJECT))) {
    // Don't intrinsify mismatched object accesses.
    return false;
  }

  // For CAS, unlike inline_unsafe_access, there seems no point in
  // trying to refine types. Just use the coarse types here.
  assert(alias_type->index() != Compile::AliasIdxBot, "no bare pointers here");
  const Type *value_type = Type::get_const_basic_type(type);

  switch (kind) {
    case LS_get_set:
    case LS_cmp_exchange: {
      if (type == T_OBJECT) {
        const TypeOopPtr* tjp = sharpen_unsafe_type(alias_type, adr_type);
        if (tjp != NULL) {
          value_type = tjp;
        }
      }
      break;
    }
    case LS_cmp_swap:
    case LS_cmp_swap_weak:
    case LS_get_add:
      break;
    default:
      ShouldNotReachHere();
  }

  // Null check receiver.
  receiver = null_check(receiver);
  if (stopped()) {
    return true;
  }

  int alias_idx = C->get_alias_index(adr_type);

  if (type == T_OBJECT || type == T_ARRAY) {
    decorators |= IN_HEAP | ON_UNKNOWN_OOP_REF;

    // Transformation of a value which could be NULL pointer (CastPP #NULL)
    // could be delayed during Parse (for example, in adjust_map_after_if()).
    // Execute transformation here to avoid barrier generation in such case.
    if (_gvn.type(newval) == TypePtr::NULL_PTR)
      newval = _gvn.makecon(TypePtr::NULL_PTR);

    if (oldval != NULL && _gvn.type(oldval) == TypePtr::NULL_PTR) {
      // Refine the value to a null constant, when it is known to be null
      oldval = _gvn.makecon(TypePtr::NULL_PTR);
    }
  }

  Node* result = NULL;
  switch (kind) {
    case LS_cmp_exchange: {
      result = access_atomic_cmpxchg_val_at(control(), base, adr, adr_type, alias_idx,
                                            oldval, newval, value_type, type, decorators);
      break;
    }
    case LS_cmp_swap_weak:
      decorators |= C2_WEAK_CMPXCHG;
    case LS_cmp_swap: {
      result = access_atomic_cmpxchg_bool_at(control(), base, adr, adr_type, alias_idx,
                                             oldval, newval, value_type, type, decorators);
      break;
    }
    case LS_get_set: {
      result = access_atomic_xchg_at(control(), base, adr, adr_type, alias_idx,
                                     newval, value_type, type, decorators);
      break;
    }
    case LS_get_add: {
      result = access_atomic_add_at(control(), base, adr, adr_type, alias_idx,
                                    newval, value_type, type, decorators);
      break;
    }
    default:
      ShouldNotReachHere();
  }

  assert(type2size[result->bottom_type()->basic_type()] == type2size[rtype], "result type should match");
  set_result(result);
  return true;
}

bool LibraryCallKit::inline_unsafe_fence(vmIntrinsics::ID id) {
  // Regardless of form, don't allow previous ld/st to move down,
  // then issue acquire, release, or volatile mem_bar.
  insert_mem_bar(Op_MemBarCPUOrder);
  switch(id) {
    case vmIntrinsics::_loadFence:
      insert_mem_bar(Op_LoadFence);
      return true;
    case vmIntrinsics::_storeFence:
      insert_mem_bar(Op_StoreFence);
      return true;
    case vmIntrinsics::_fullFence:
      insert_mem_bar(Op_MemBarVolatile);
      return true;
    default:
      fatal_unexpected_iid(id);
      return false;
  }
}

bool LibraryCallKit::inline_onspinwait() {
  insert_mem_bar(Op_OnSpinWait);
  return true;
}

bool LibraryCallKit::klass_needs_init_guard(Node* kls) {
  if (!kls->is_Con()) {
    return true;
  }
  const TypeKlassPtr* klsptr = kls->bottom_type()->isa_klassptr();
  if (klsptr == NULL) {
    return true;
  }
  ciInstanceKlass* ik = klsptr->klass()->as_instance_klass();
  // don't need a guard for a klass that is already initialized
  return !ik->is_initialized();
}

//----------------------------inline_unsafe_allocate---------------------------
// public native Object Unsafe.allocateInstance(Class<?> cls);
bool LibraryCallKit::inline_unsafe_allocate() {
  if (callee()->is_static())  return false;  // caller must have the capability!

  null_check_receiver();  // null-check, then ignore
  Node* cls = null_check(argument(1));
  if (stopped())  return true;

  Node* kls = load_klass_from_mirror(cls, false, NULL, 0);
  kls = null_check(kls);
  if (stopped())  return true;  // argument was like int.class

  Node* test = NULL;
  if (LibraryCallKit::klass_needs_init_guard(kls)) {
    // Note:  The argument might still be an illegal value like
    // Serializable.class or Object[].class.   The runtime will handle it.
    // But we must make an explicit check for initialization.
    Node* insp = basic_plus_adr(kls, in_bytes(InstanceKlass::init_state_offset()));
    // Use T_BOOLEAN for InstanceKlass::_init_state so the compiler
    // can generate code to load it as unsigned byte.
    Node* inst = make_load(NULL, insp, TypeInt::UBYTE, T_BOOLEAN, MemNode::unordered);
    Node* bits = intcon(InstanceKlass::fully_initialized);
    test = _gvn.transform(new SubINode(inst, bits));
    // The 'test' is non-zero if we need to take a slow path.
  }

  Node* obj = new_instance(kls, test);
  set_result(obj);
  return true;
}

//------------------------inline_native_time_funcs--------------
// inline code for System.currentTimeMillis() and System.nanoTime()
// these have the same type and signature
bool LibraryCallKit::inline_native_time_funcs(address funcAddr, const char* funcName) {
  const TypeFunc* tf = OptoRuntime::void_long_Type();
  const TypePtr* no_memory_effects = NULL;
  Node* time = make_runtime_call(RC_LEAF, tf, funcAddr, funcName, no_memory_effects);
  Node* value = _gvn.transform(new ProjNode(time, TypeFunc::Parms+0));
#ifdef ASSERT
  Node* value_top = _gvn.transform(new ProjNode(time, TypeFunc::Parms+1));
  assert(value_top == top(), "second value must be top");
#endif
  set_result(value);
  return true;
}

#ifdef JFR_HAVE_INTRINSICS

/*
* oop -> myklass
* myklass->trace_id |= USED
* return myklass->trace_id & ~0x3
*/
bool LibraryCallKit::inline_native_classID() {
  Node* cls = null_check(argument(0), T_OBJECT);
  Node* kls = load_klass_from_mirror(cls, false, NULL, 0);
  kls = null_check(kls, T_OBJECT);

  ByteSize offset = KLASS_TRACE_ID_OFFSET;
  Node* insp = basic_plus_adr(kls, in_bytes(offset));
  Node* tvalue = make_load(NULL, insp, TypeLong::LONG, T_LONG, MemNode::unordered);

  Node* clsused = longcon(0x01l); // set the class bit
  Node* orl = _gvn.transform(new OrLNode(tvalue, clsused));
  const TypePtr *adr_type = _gvn.type(insp)->isa_ptr();
  store_to_memory(control(), insp, orl, T_LONG, adr_type, MemNode::unordered);

#ifdef TRACE_ID_META_BITS
  Node* mbits = longcon(~TRACE_ID_META_BITS);
  tvalue = _gvn.transform(new AndLNode(tvalue, mbits));
#endif
#ifdef TRACE_ID_SHIFT
  Node* cbits = intcon(TRACE_ID_SHIFT);
  tvalue = _gvn.transform(new URShiftLNode(tvalue, cbits));
#endif

  set_result(tvalue);
  return true;

}

bool LibraryCallKit::inline_native_getEventWriter() {
  Node* tls_ptr = _gvn.transform(new ThreadLocalNode());

  Node* jobj_ptr = basic_plus_adr(top(), tls_ptr,
                                  in_bytes(THREAD_LOCAL_WRITER_OFFSET_JFR));

  Node* jobj = make_load(control(), jobj_ptr, TypeRawPtr::BOTTOM, T_ADDRESS, MemNode::unordered);

  Node* jobj_cmp_null = _gvn.transform( new CmpPNode(jobj, null()) );
  Node* test_jobj_eq_null  = _gvn.transform( new BoolNode(jobj_cmp_null, BoolTest::eq) );

  IfNode* iff_jobj_null =
    create_and_map_if(control(), test_jobj_eq_null, PROB_MIN, COUNT_UNKNOWN);

  enum { _normal_path = 1,
         _null_path = 2,
         PATH_LIMIT };

  RegionNode* result_rgn = new RegionNode(PATH_LIMIT);
  PhiNode*    result_val = new PhiNode(result_rgn, TypeInstPtr::BOTTOM);

  Node* jobj_is_null = _gvn.transform(new IfTrueNode(iff_jobj_null));
  result_rgn->init_req(_null_path, jobj_is_null);
  result_val->init_req(_null_path, null());

  Node* jobj_is_not_null = _gvn.transform(new IfFalseNode(iff_jobj_null));
  set_control(jobj_is_not_null);
  Node* res = access_load(jobj, TypeInstPtr::NOTNULL, T_OBJECT,
                          IN_NATIVE | C2_CONTROL_DEPENDENT_LOAD);
  result_rgn->init_req(_normal_path, control());
  result_val->init_req(_normal_path, res);

  set_result(result_rgn, result_val);

  return true;
}

#endif // JFR_HAVE_INTRINSICS

//------------------------inline_native_currentThread------------------
bool LibraryCallKit::inline_native_currentThread() {
  Node* junk = NULL;
  set_result(generate_current_thread(junk));
  return true;
}

//------------------------inline_native_isInterrupted------------------
// private native boolean java.lang.Thread.isInterrupted(boolean ClearInterrupted);
bool LibraryCallKit::inline_native_isInterrupted() {
  // Add a fast path to t.isInterrupted(clear_int):
  //   (t == Thread.current() &&
  //    (!TLS._osthread._interrupted || WINDOWS_ONLY(false) NOT_WINDOWS(!clear_int)))
  //   ? TLS._osthread._interrupted : /*slow path:*/ t.isInterrupted(clear_int)
  // So, in the common case that the interrupt bit is false,
  // we avoid making a call into the VM.  Even if the interrupt bit
  // is true, if the clear_int argument is false, we avoid the VM call.
  // However, if the receiver is not currentThread, we must call the VM,
  // because there must be some locking done around the operation.

  // We only go to the fast case code if we pass two guards.
  // Paths which do not pass are accumulated in the slow_region.

  enum {
    no_int_result_path   = 1, // t == Thread.current() && !TLS._osthread._interrupted
    no_clear_result_path = 2, // t == Thread.current() &&  TLS._osthread._interrupted && !clear_int
    slow_result_path     = 3, // slow path: t.isInterrupted(clear_int)
    PATH_LIMIT
  };

  // Ensure that it's not possible to move the load of TLS._osthread._interrupted flag
  // out of the function.
  insert_mem_bar(Op_MemBarCPUOrder);

  RegionNode* result_rgn = new RegionNode(PATH_LIMIT);
  PhiNode*    result_val = new PhiNode(result_rgn, TypeInt::BOOL);

  RegionNode* slow_region = new RegionNode(1);
  record_for_igvn(slow_region);

  // (a) Receiving thread must be the current thread.
  Node* rec_thr = argument(0);
  Node* tls_ptr = NULL;
  Node* cur_thr = generate_current_thread(tls_ptr);
  Node* cmp_thr = _gvn.transform(new CmpPNode(cur_thr, rec_thr));
  Node* bol_thr = _gvn.transform(new BoolNode(cmp_thr, BoolTest::ne));

  generate_slow_guard(bol_thr, slow_region);

  // (b) Interrupt bit on TLS must be false.
  Node* p = basic_plus_adr(top()/*!oop*/, tls_ptr, in_bytes(JavaThread::osthread_offset()));
  Node* osthread = make_load(NULL, p, TypeRawPtr::NOTNULL, T_ADDRESS, MemNode::unordered);
  p = basic_plus_adr(top()/*!oop*/, osthread, in_bytes(OSThread::interrupted_offset()));

  // Set the control input on the field _interrupted read to prevent it floating up.
  Node* int_bit = make_load(control(), p, TypeInt::BOOL, T_INT, MemNode::unordered);
  Node* cmp_bit = _gvn.transform(new CmpINode(int_bit, intcon(0)));
  Node* bol_bit = _gvn.transform(new BoolNode(cmp_bit, BoolTest::ne));

  IfNode* iff_bit = create_and_map_if(control(), bol_bit, PROB_UNLIKELY_MAG(3), COUNT_UNKNOWN);

  // First fast path:  if (!TLS._interrupted) return false;
  Node* false_bit = _gvn.transform(new IfFalseNode(iff_bit));
  result_rgn->init_req(no_int_result_path, false_bit);
  result_val->init_req(no_int_result_path, intcon(0));

  // drop through to next case
  set_control( _gvn.transform(new IfTrueNode(iff_bit)));

#ifndef _WINDOWS
  // (c) Or, if interrupt bit is set and clear_int is false, use 2nd fast path.
  Node* clr_arg = argument(1);
  Node* cmp_arg = _gvn.transform(new CmpINode(clr_arg, intcon(0)));
  Node* bol_arg = _gvn.transform(new BoolNode(cmp_arg, BoolTest::ne));
  IfNode* iff_arg = create_and_map_if(control(), bol_arg, PROB_FAIR, COUNT_UNKNOWN);

  // Second fast path:  ... else if (!clear_int) return true;
  Node* false_arg = _gvn.transform(new IfFalseNode(iff_arg));
  result_rgn->init_req(no_clear_result_path, false_arg);
  result_val->init_req(no_clear_result_path, intcon(1));

  // drop through to next case
  set_control( _gvn.transform(new IfTrueNode(iff_arg)));
#else
  // To return true on Windows you must read the _interrupted field
  // and check the event state i.e. take the slow path.
#endif // _WINDOWS

  // (d) Otherwise, go to the slow path.
  slow_region->add_req(control());
  set_control( _gvn.transform(slow_region));

  if (stopped()) {
    // There is no slow path.
    result_rgn->init_req(slow_result_path, top());
    result_val->init_req(slow_result_path, top());
  } else {
    // non-virtual because it is a private non-static
    CallJavaNode* slow_call = generate_method_call(vmIntrinsics::_isInterrupted);

    Node* slow_val = set_results_for_java_call(slow_call);
    // this->control() comes from set_results_for_java_call

    Node* fast_io  = slow_call->in(TypeFunc::I_O);
    Node* fast_mem = slow_call->in(TypeFunc::Memory);

    // These two phis are pre-filled with copies of of the fast IO and Memory
    PhiNode* result_mem  = PhiNode::make(result_rgn, fast_mem, Type::MEMORY, TypePtr::BOTTOM);
    PhiNode* result_io   = PhiNode::make(result_rgn, fast_io,  Type::ABIO);

    result_rgn->init_req(slow_result_path, control());
    result_io ->init_req(slow_result_path, i_o());
    result_mem->init_req(slow_result_path, reset_memory());
    result_val->init_req(slow_result_path, slow_val);

    set_all_memory(_gvn.transform(result_mem));
    set_i_o(       _gvn.transform(result_io));
  }

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  set_result(result_rgn, result_val);
  return true;
}

//---------------------------load_mirror_from_klass----------------------------
// Given a klass oop, load its java mirror (a java.lang.Class oop).
Node* LibraryCallKit::load_mirror_from_klass(Node* klass) {
  Node* p = basic_plus_adr(klass, in_bytes(Klass::java_mirror_offset()));
  Node* load = make_load(NULL, p, TypeRawPtr::NOTNULL, T_ADDRESS, MemNode::unordered);
  // mirror = ((OopHandle)mirror)->resolve();
  return access_load(load, TypeInstPtr::MIRROR, T_OBJECT, IN_NATIVE);
}

//-----------------------load_klass_from_mirror_common-------------------------
// Given a java mirror (a java.lang.Class oop), load its corresponding klass oop.
// Test the klass oop for null (signifying a primitive Class like Integer.TYPE),
// and branch to the given path on the region.
// If never_see_null, take an uncommon trap on null, so we can optimistically
// compile for the non-null case.
// If the region is NULL, force never_see_null = true.
Node* LibraryCallKit::load_klass_from_mirror_common(Node* mirror,
                                                    bool never_see_null,
                                                    RegionNode* region,
                                                    int null_path,
                                                    int offset) {
  if (region == NULL)  never_see_null = true;
  Node* p = basic_plus_adr(mirror, offset);
  const TypeKlassPtr*  kls_type = TypeKlassPtr::OBJECT_OR_NULL;
  Node* kls = _gvn.transform(LoadKlassNode::make(_gvn, NULL, immutable_memory(), p, TypeRawPtr::BOTTOM, kls_type));
  Node* null_ctl = top();
  kls = null_check_oop(kls, &null_ctl, never_see_null);
  if (region != NULL) {
    // Set region->in(null_path) if the mirror is a primitive (e.g, int.class).
    region->init_req(null_path, null_ctl);
  } else {
    assert(null_ctl == top(), "no loose ends");
  }
  return kls;
}

//--------------------(inline_native_Class_query helpers)---------------------
// Use this for JVM_ACC_INTERFACE, JVM_ACC_IS_CLONEABLE_FAST, JVM_ACC_HAS_FINALIZER.
// Fall through if (mods & mask) == bits, take the guard otherwise.
Node* LibraryCallKit::generate_access_flags_guard(Node* kls, int modifier_mask, int modifier_bits, RegionNode* region) {
  // Branch around if the given klass has the given modifier bit set.
  // Like generate_guard, adds a new path onto the region.
  Node* modp = basic_plus_adr(kls, in_bytes(Klass::access_flags_offset()));
  Node* mods = make_load(NULL, modp, TypeInt::INT, T_INT, MemNode::unordered);
  Node* mask = intcon(modifier_mask);
  Node* bits = intcon(modifier_bits);
  Node* mbit = _gvn.transform(new AndINode(mods, mask));
  Node* cmp  = _gvn.transform(new CmpINode(mbit, bits));
  Node* bol  = _gvn.transform(new BoolNode(cmp, BoolTest::ne));
  return generate_fair_guard(bol, region);
}
Node* LibraryCallKit::generate_interface_guard(Node* kls, RegionNode* region) {
  return generate_access_flags_guard(kls, JVM_ACC_INTERFACE, 0, region);
}

//-------------------------inline_native_Class_query-------------------
bool LibraryCallKit::inline_native_Class_query(vmIntrinsics::ID id) {
  const Type* return_type = TypeInt::BOOL;
  Node* prim_return_value = top();  // what happens if it's a primitive class?
  bool never_see_null = !too_many_traps(Deoptimization::Reason_null_check);
  bool expect_prim = false;     // most of these guys expect to work on refs

  enum { _normal_path = 1, _prim_path = 2, PATH_LIMIT };

  Node* mirror = argument(0);
  Node* obj    = top();

  switch (id) {
  case vmIntrinsics::_isInstance:
    // nothing is an instance of a primitive type
    prim_return_value = intcon(0);
    obj = argument(1);
    break;
  case vmIntrinsics::_getModifiers:
    prim_return_value = intcon(JVM_ACC_ABSTRACT | JVM_ACC_FINAL | JVM_ACC_PUBLIC);
    assert(is_power_of_2((int)JVM_ACC_WRITTEN_FLAGS+1), "change next line");
    return_type = TypeInt::make(0, JVM_ACC_WRITTEN_FLAGS, Type::WidenMin);
    break;
  case vmIntrinsics::_isInterface:
    prim_return_value = intcon(0);
    break;
  case vmIntrinsics::_isArray:
    prim_return_value = intcon(0);
    expect_prim = true;  // cf. ObjectStreamClass.getClassSignature
    break;
  case vmIntrinsics::_isPrimitive:
    prim_return_value = intcon(1);
    expect_prim = true;  // obviously
    break;
  case vmIntrinsics::_getSuperclass:
    prim_return_value = null();
    return_type = TypeInstPtr::MIRROR->cast_to_ptr_type(TypePtr::BotPTR);
    break;
  case vmIntrinsics::_getClassAccessFlags:
    prim_return_value = intcon(JVM_ACC_ABSTRACT | JVM_ACC_FINAL | JVM_ACC_PUBLIC);
    return_type = TypeInt::INT;  // not bool!  6297094
    break;
  default:
    fatal_unexpected_iid(id);
    break;
  }

  const TypeInstPtr* mirror_con = _gvn.type(mirror)->isa_instptr();
  if (mirror_con == NULL)  return false;  // cannot happen?

#ifndef PRODUCT
  if (C->print_intrinsics() || C->print_inlining()) {
    ciType* k = mirror_con->java_mirror_type();
    if (k) {
      tty->print("Inlining %s on constant Class ", vmIntrinsics::name_at(intrinsic_id()));
      k->print_name();
      tty->cr();
    }
  }
#endif

  // Null-check the mirror, and the mirror's klass ptr (in case it is a primitive).
  RegionNode* region = new RegionNode(PATH_LIMIT);
  record_for_igvn(region);
  PhiNode* phi = new PhiNode(region, return_type);

  // The mirror will never be null of Reflection.getClassAccessFlags, however
  // it may be null for Class.isInstance or Class.getModifiers. Throw a NPE
  // if it is. See bug 4774291.

  // For Reflection.getClassAccessFlags(), the null check occurs in
  // the wrong place; see inline_unsafe_access(), above, for a similar
  // situation.
  mirror = null_check(mirror);
  // If mirror or obj is dead, only null-path is taken.
  if (stopped())  return true;

  if (expect_prim)  never_see_null = false;  // expect nulls (meaning prims)

  // Now load the mirror's klass metaobject, and null-check it.
  // Side-effects region with the control path if the klass is null.
  Node* kls = load_klass_from_mirror(mirror, never_see_null, region, _prim_path);
  // If kls is null, we have a primitive mirror.
  phi->init_req(_prim_path, prim_return_value);
  if (stopped()) { set_result(region, phi); return true; }
  bool safe_for_replace = (region->in(_prim_path) == top());

  Node* p;  // handy temp
  Node* null_ctl;

  // Now that we have the non-null klass, we can perform the real query.
  // For constant classes, the query will constant-fold in LoadNode::Value.
  Node* query_value = top();
  switch (id) {
  case vmIntrinsics::_isInstance:
    // nothing is an instance of a primitive type
    query_value = gen_instanceof(obj, kls, safe_for_replace);
    break;

  case vmIntrinsics::_getModifiers:
    p = basic_plus_adr(kls, in_bytes(Klass::modifier_flags_offset()));
    query_value = make_load(NULL, p, TypeInt::INT, T_INT, MemNode::unordered);
    break;

  case vmIntrinsics::_isInterface:
    // (To verify this code sequence, check the asserts in JVM_IsInterface.)
    if (generate_interface_guard(kls, region) != NULL)
      // A guard was added.  If the guard is taken, it was an interface.
      phi->add_req(intcon(1));
    // If we fall through, it's a plain class.
    query_value = intcon(0);
    break;

  case vmIntrinsics::_isArray:
    // (To verify this code sequence, check the asserts in JVM_IsArrayClass.)
    if (generate_array_guard(kls, region) != NULL)
      // A guard was added.  If the guard is taken, it was an array.
      phi->add_req(intcon(1));
    // If we fall through, it's a plain class.
    query_value = intcon(0);
    break;

  case vmIntrinsics::_isPrimitive:
    query_value = intcon(0); // "normal" path produces false
    break;

  case vmIntrinsics::_getSuperclass:
    // The rules here are somewhat unfortunate, but we can still do better
    // with random logic than with a JNI call.
    // Interfaces store null or Object as _super, but must report null.
    // Arrays store an intermediate super as _super, but must report Object.
    // Other types can report the actual _super.
    // (To verify this code sequence, check the asserts in JVM_IsInterface.)
    if (generate_interface_guard(kls, region) != NULL)
      // A guard was added.  If the guard is taken, it was an interface.
      phi->add_req(null());
    if (generate_array_guard(kls, region) != NULL)
      // A guard was added.  If the guard is taken, it was an array.
      phi->add_req(makecon(TypeInstPtr::make(env()->Object_klass()->java_mirror())));
    // If we fall through, it's a plain class.  Get its _super.
    p = basic_plus_adr(kls, in_bytes(Klass::super_offset()));
    kls = _gvn.transform(LoadKlassNode::make(_gvn, NULL, immutable_memory(), p, TypeRawPtr::BOTTOM, TypeKlassPtr::OBJECT_OR_NULL));
    null_ctl = top();
    kls = null_check_oop(kls, &null_ctl);
    if (null_ctl != top()) {
      // If the guard is taken, Object.superClass is null (both klass and mirror).
      region->add_req(null_ctl);
      phi   ->add_req(null());
    }
    if (!stopped()) {
      query_value = load_mirror_from_klass(kls);
    }
    break;

  case vmIntrinsics::_getClassAccessFlags:
    p = basic_plus_adr(kls, in_bytes(Klass::access_flags_offset()));
    query_value = make_load(NULL, p, TypeInt::INT, T_INT, MemNode::unordered);
    break;

  default:
    fatal_unexpected_iid(id);
    break;
  }

  // Fall-through is the normal case of a query to a real class.
  phi->init_req(1, query_value);
  region->init_req(1, control());

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  set_result(region, phi);
  return true;
}

//-------------------------inline_Class_cast-------------------
bool LibraryCallKit::inline_Class_cast() {
  Node* mirror = argument(0); // Class
  Node* obj    = argument(1);
  const TypeInstPtr* mirror_con = _gvn.type(mirror)->isa_instptr();
  if (mirror_con == NULL) {
    return false;  // dead path (mirror->is_top()).
  }
  if (obj == NULL || obj->is_top()) {
    return false;  // dead path
  }
  const TypeOopPtr* tp = _gvn.type(obj)->isa_oopptr();

  // First, see if Class.cast() can be folded statically.
  // java_mirror_type() returns non-null for compile-time Class constants.
  ciType* tm = mirror_con->java_mirror_type();
  if (tm != NULL && tm->is_klass() &&
      tp != NULL && tp->klass() != NULL) {
    if (!tp->klass()->is_loaded()) {
      // Don't use intrinsic when class is not loaded.
      return false;
    } else {
      int static_res = C->static_subtype_check(tm->as_klass(), tp->klass());
      if (static_res == Compile::SSC_always_true) {
        // isInstance() is true - fold the code.
        set_result(obj);
        return true;
      } else if (static_res == Compile::SSC_always_false) {
        // Don't use intrinsic, have to throw ClassCastException.
        // If the reference is null, the non-intrinsic bytecode will
        // be optimized appropriately.
        return false;
      }
    }
  }

  // Bailout intrinsic and do normal inlining if exception path is frequent.
  if (too_many_traps(Deoptimization::Reason_intrinsic)) {
    return false;
  }

  // Generate dynamic checks.
  // Class.cast() is java implementation of _checkcast bytecode.
  // Do checkcast (Parse::do_checkcast()) optimizations here.

  mirror = null_check(mirror);
  // If mirror is dead, only null-path is taken.
  if (stopped()) {
    return true;
  }

  // Not-subtype or the mirror's klass ptr is NULL (in case it is a primitive).
  enum { _bad_type_path = 1, _prim_path = 2, PATH_LIMIT };
  RegionNode* region = new RegionNode(PATH_LIMIT);
  record_for_igvn(region);

  // Now load the mirror's klass metaobject, and null-check it.
  // If kls is null, we have a primitive mirror and
  // nothing is an instance of a primitive type.
  Node* kls = load_klass_from_mirror(mirror, false, region, _prim_path);

  Node* res = top();
  if (!stopped()) {
    Node* bad_type_ctrl = top();
    // Do checkcast optimizations.
    res = gen_checkcast(obj, kls, &bad_type_ctrl);
    region->init_req(_bad_type_path, bad_type_ctrl);
  }
  if (region->in(_prim_path) != top() ||
      region->in(_bad_type_path) != top()) {
    // Let Interpreter throw ClassCastException.
    PreserveJVMState pjvms(this);
    set_control(_gvn.transform(region));
    uncommon_trap(Deoptimization::Reason_intrinsic,
                  Deoptimization::Action_maybe_recompile);
  }
  if (!stopped()) {
    set_result(res);
  }
  return true;
}


//--------------------------inline_native_subtype_check------------------------
// This intrinsic takes the JNI calls out of the heart of
// UnsafeFieldAccessorImpl.set, which improves Field.set, readObject, etc.
bool LibraryCallKit::inline_native_subtype_check() {
  // Pull both arguments off the stack.
  Node* args[2];                // two java.lang.Class mirrors: superc, subc
  args[0] = argument(0);
  args[1] = argument(1);
  Node* klasses[2];             // corresponding Klasses: superk, subk
  klasses[0] = klasses[1] = top();

  enum {
    // A full decision tree on {superc is prim, subc is prim}:
    _prim_0_path = 1,           // {P,N} => false
                                // {P,P} & superc!=subc => false
    _prim_same_path,            // {P,P} & superc==subc => true
    _prim_1_path,               // {N,P} => false
    _ref_subtype_path,          // {N,N} & subtype check wins => true
    _both_ref_path,             // {N,N} & subtype check loses => false
    PATH_LIMIT
  };

  RegionNode* region = new RegionNode(PATH_LIMIT);
  Node*       phi    = new PhiNode(region, TypeInt::BOOL);
  record_for_igvn(region);

  const TypePtr* adr_type = TypeRawPtr::BOTTOM;   // memory type of loads
  const TypeKlassPtr* kls_type = TypeKlassPtr::OBJECT_OR_NULL;
  int class_klass_offset = java_lang_Class::klass_offset_in_bytes();

  // First null-check both mirrors and load each mirror's klass metaobject.
  int which_arg;
  for (which_arg = 0; which_arg <= 1; which_arg++) {
    Node* arg = args[which_arg];
    arg = null_check(arg);
    if (stopped())  break;
    args[which_arg] = arg;

    Node* p = basic_plus_adr(arg, class_klass_offset);
    Node* kls = LoadKlassNode::make(_gvn, NULL, immutable_memory(), p, adr_type, kls_type);
    klasses[which_arg] = _gvn.transform(kls);
  }

  // Having loaded both klasses, test each for null.
  bool never_see_null = !too_many_traps(Deoptimization::Reason_null_check);
  for (which_arg = 0; which_arg <= 1; which_arg++) {
    Node* kls = klasses[which_arg];
    Node* null_ctl = top();
    kls = null_check_oop(kls, &null_ctl, never_see_null);
    int prim_path = (which_arg == 0 ? _prim_0_path : _prim_1_path);
    region->init_req(prim_path, null_ctl);
    if (stopped())  break;
    klasses[which_arg] = kls;
  }

  if (!stopped()) {
    // now we have two reference types, in klasses[0..1]
    Node* subk   = klasses[1];  // the argument to isAssignableFrom
    Node* superk = klasses[0];  // the receiver
    region->set_req(_both_ref_path, gen_subtype_check(subk, superk));
    // now we have a successful reference subtype check
    region->set_req(_ref_subtype_path, control());
  }

  // If both operands are primitive (both klasses null), then
  // we must return true when they are identical primitives.
  // It is convenient to test this after the first null klass check.
  set_control(region->in(_prim_0_path)); // go back to first null check
  if (!stopped()) {
    // Since superc is primitive, make a guard for the superc==subc case.
    Node* cmp_eq = _gvn.transform(new CmpPNode(args[0], args[1]));
    Node* bol_eq = _gvn.transform(new BoolNode(cmp_eq, BoolTest::eq));
    generate_guard(bol_eq, region, PROB_FAIR);
    if (region->req() == PATH_LIMIT+1) {
      // A guard was added.  If the added guard is taken, superc==subc.
      region->swap_edges(PATH_LIMIT, _prim_same_path);
      region->del_req(PATH_LIMIT);
    }
    region->set_req(_prim_0_path, control()); // Not equal after all.
  }

  // these are the only paths that produce 'true':
  phi->set_req(_prim_same_path,   intcon(1));
  phi->set_req(_ref_subtype_path, intcon(1));

  // pull together the cases:
  assert(region->req() == PATH_LIMIT, "sane region");
  for (uint i = 1; i < region->req(); i++) {
    Node* ctl = region->in(i);
    if (ctl == NULL || ctl == top()) {
      region->set_req(i, top());
      phi   ->set_req(i, top());
    } else if (phi->in(i) == NULL) {
      phi->set_req(i, intcon(0)); // all other paths produce 'false'
    }
  }

  set_control(_gvn.transform(region));
  set_result(_gvn.transform(phi));
  return true;
}

//---------------------generate_array_guard_common------------------------
Node* LibraryCallKit::generate_array_guard_common(Node* kls, RegionNode* region,
                                                  bool obj_array, bool not_array) {

  if (stopped()) {
    return NULL;
  }

  // If obj_array/non_array==false/false:
  // Branch around if the given klass is in fact an array (either obj or prim).
  // If obj_array/non_array==false/true:
  // Branch around if the given klass is not an array klass of any kind.
  // If obj_array/non_array==true/true:
  // Branch around if the kls is not an oop array (kls is int[], String, etc.)
  // If obj_array/non_array==true/false:
  // Branch around if the kls is an oop array (Object[] or subtype)
  //
  // Like generate_guard, adds a new path onto the region.
  jint  layout_con = 0;
  Node* layout_val = get_layout_helper(kls, layout_con);
  if (layout_val == NULL) {
    bool query = (obj_array
                  ? Klass::layout_helper_is_objArray(layout_con)
                  : Klass::layout_helper_is_array(layout_con));
    if (query == not_array) {
      return NULL;                       // never a branch
    } else {                             // always a branch
      Node* always_branch = control();
      if (region != NULL)
        region->add_req(always_branch);
      set_control(top());
      return always_branch;
    }
  }
  // Now test the correct condition.
  jint  nval = (obj_array
                ? (jint)(Klass::_lh_array_tag_type_value
                   <<    Klass::_lh_array_tag_shift)
                : Klass::_lh_neutral_value);
  Node* cmp = _gvn.transform(new CmpINode(layout_val, intcon(nval)));
  BoolTest::mask btest = BoolTest::lt;  // correct for testing is_[obj]array
  // invert the test if we are looking for a non-array
  if (not_array)  btest = BoolTest(btest).negate();
  Node* bol = _gvn.transform(new BoolNode(cmp, btest));
  return generate_fair_guard(bol, region);
}


//-----------------------inline_native_newArray--------------------------
// private static native Object java.lang.reflect.newArray(Class<?> componentType, int length);
// private        native Object Unsafe.allocateUninitializedArray0(Class<?> cls, int size);
bool LibraryCallKit::inline_unsafe_newArray(bool uninitialized) {
  Node* mirror;
  Node* count_val;
  if (uninitialized) {
    mirror    = argument(1);
    count_val = argument(2);
  } else {
    mirror    = argument(0);
    count_val = argument(1);
  }

  mirror = null_check(mirror);
  // If mirror or obj is dead, only null-path is taken.
  if (stopped())  return true;

  enum { _normal_path = 1, _slow_path = 2, PATH_LIMIT };
  RegionNode* result_reg = new RegionNode(PATH_LIMIT);
  PhiNode*    result_val = new PhiNode(result_reg, TypeInstPtr::NOTNULL);
  PhiNode*    result_io  = new PhiNode(result_reg, Type::ABIO);
  PhiNode*    result_mem = new PhiNode(result_reg, Type::MEMORY, TypePtr::BOTTOM);

  bool never_see_null = !too_many_traps(Deoptimization::Reason_null_check);
  Node* klass_node = load_array_klass_from_mirror(mirror, never_see_null,
                                                  result_reg, _slow_path);
  Node* normal_ctl   = control();
  Node* no_array_ctl = result_reg->in(_slow_path);

  // Generate code for the slow case.  We make a call to newArray().
  set_control(no_array_ctl);
  if (!stopped()) {
    // Either the input type is void.class, or else the
    // array klass has not yet been cached.  Either the
    // ensuing call will throw an exception, or else it
    // will cache the array klass for next time.
    PreserveJVMState pjvms(this);
    CallJavaNode* slow_call = NULL;
    if (uninitialized) {
      // Generate optimized virtual call (holder class 'Unsafe' is final)
      slow_call = generate_method_call(vmIntrinsics::_allocateUninitializedArray, false, false);
    } else {
      slow_call = generate_method_call_static(vmIntrinsics::_newArray);
    }
    Node* slow_result = set_results_for_java_call(slow_call);
    // this->control() comes from set_results_for_java_call
    result_reg->set_req(_slow_path, control());
    result_val->set_req(_slow_path, slow_result);
    result_io ->set_req(_slow_path, i_o());
    result_mem->set_req(_slow_path, reset_memory());
  }

  set_control(normal_ctl);
  if (!stopped()) {
    // Normal case:  The array type has been cached in the java.lang.Class.
    // The following call works fine even if the array type is polymorphic.
    // It could be a dynamic mix of int[], boolean[], Object[], etc.
    Node* obj = new_array(klass_node, count_val, 0);  // no arguments to push
    result_reg->init_req(_normal_path, control());
    result_val->init_req(_normal_path, obj);
    result_io ->init_req(_normal_path, i_o());
    result_mem->init_req(_normal_path, reset_memory());

    if (uninitialized) {
      // Mark the allocation so that zeroing is skipped
      AllocateArrayNode* alloc = AllocateArrayNode::Ideal_array_allocation(obj, &_gvn);
      alloc->maybe_set_complete(&_gvn);
    }
  }

  // Return the combined state.
  set_i_o(        _gvn.transform(result_io)  );
  set_all_memory( _gvn.transform(result_mem));

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  set_result(result_reg, result_val);
  return true;
}

//----------------------inline_native_getLength--------------------------
// public static native int java.lang.reflect.Array.getLength(Object array);
bool LibraryCallKit::inline_native_getLength() {
  if (too_many_traps(Deoptimization::Reason_intrinsic))  return false;

  Node* array = null_check(argument(0));
  // If array is dead, only null-path is taken.
  if (stopped())  return true;

  // Deoptimize if it is a non-array.
  Node* non_array = generate_non_array_guard(load_object_klass(array), NULL);

  if (non_array != NULL) {
    PreserveJVMState pjvms(this);
    set_control(non_array);
    uncommon_trap(Deoptimization::Reason_intrinsic,
                  Deoptimization::Action_maybe_recompile);
  }

  // If control is dead, only non-array-path is taken.
  if (stopped())  return true;

  // The works fine even if the array type is polymorphic.
  // It could be a dynamic mix of int[], boolean[], Object[], etc.
  Node* result = load_array_length(array);

  C->set_has_split_ifs(true);  // Has chance for split-if optimization
  set_result(result);
  return true;
}

//------------------------inline_array_copyOf----------------------------
// public static <T,U> T[] java.util.Arrays.copyOf(     U[] original, int newLength,         Class<? extends T[]> newType);
// public static <T,U> T[] java.util.Arrays.copyOfRange(U[] original, int from,      int to, Class<? extends T[]> newType);
bool LibraryCallKit::inline_array_copyOf(bool is_copyOfRange) {
  if (too_many_traps(Deoptimization::Reason_intrinsic))  return false;

  // Get the arguments.
  Node* original          = argument(0);
  Node* start             = is_copyOfRange? argument(1): intcon(0);
  Node* end               = is_copyOfRange? argument(2): argument(1);
  Node* array_type_mirror = is_copyOfRange? argument(3): argument(2);

  Node* newcopy = NULL;

  // Set the original stack and the reexecute bit for the interpreter to reexecute
  // the bytecode that invokes Arrays.copyOf if deoptimization happens.
  { PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);

    array_type_mirror = null_check(array_type_mirror);
    original          = null_check(original);

    // Check if a null path was taken unconditionally.
    if (stopped())  return true;

    Node* orig_length = load_array_length(original);

    Node* klass_node = load_klass_from_mirror(array_type_mirror, false, NULL, 0);
    klass_node = null_check(klass_node);

    RegionNode* bailout = new RegionNode(1);
    record_for_igvn(bailout);

    // Despite the generic type of Arrays.copyOf, the mirror might be int, int[], etc.
    // Bail out if that is so.
    Node* not_objArray = generate_non_objArray_guard(klass_node, bailout);
    if (not_objArray != NULL) {
      // Improve the klass node's type from the new optimistic assumption:
      ciKlass* ak = ciArrayKlass::make(env()->Object_klass());
      const Type* akls = TypeKlassPtr::make(TypePtr::NotNull, ak, 0/*offset*/);
      Node* cast = new CastPPNode(klass_node, akls);
      cast->init_req(0, control());
      klass_node = _gvn.transform(cast);
    }

    // Bail out if either start or end is negative.
    generate_negative_guard(start, bailout, &start);
    generate_negative_guard(end,   bailout, &end);

    Node* length = end;
    if (_gvn.type(start) != TypeInt::ZERO) {
      length = _gvn.transform(new SubINode(end, start));
    }

    // Bail out if length is negative.
    // Without this the new_array would throw
    // NegativeArraySizeException but IllegalArgumentException is what
    // should be thrown
    generate_negative_guard(length, bailout, &length);

    if (bailout->req() > 1) {
      PreserveJVMState pjvms(this);
      set_control(_gvn.transform(bailout));
      uncommon_trap(Deoptimization::Reason_intrinsic,
                    Deoptimization::Action_maybe_recompile);
    }

    if (!stopped()) {
      // How many elements will we copy from the original?
      // The answer is MinI(orig_length - start, length).
      Node* orig_tail = _gvn.transform(new SubINode(orig_length, start));
      Node* moved = generate_min_max(vmIntrinsics::_min, orig_tail, length);

      // Generate a direct call to the right arraycopy function(s).
      // We know the copy is disjoint but we might not know if the
      // oop stores need checking.
      // Extreme case:  Arrays.copyOf((Integer[])x, 10, String[].class).
      // This will fail a store-check if x contains any non-nulls.

      // ArrayCopyNode:Ideal may transform the ArrayCopyNode to
      // loads/stores but it is legal only if we're sure the
      // Arrays.copyOf would succeed. So we need all input arguments
      // to the copyOf to be validated, including that the copy to the
      // new array won't trigger an ArrayStoreException. That subtype
      // check can be optimized if we know something on the type of
      // the input array from type speculation.
      if (_gvn.type(klass_node)->singleton()) {
        ciKlass* subk   = _gvn.type(load_object_klass(original))->is_klassptr()->klass();
        ciKlass* superk = _gvn.type(klass_node)->is_klassptr()->klass();

        int test = C->static_subtype_check(superk, subk);
        if (test != Compile::SSC_always_true && test != Compile::SSC_always_false) {
          const TypeOopPtr* t_original = _gvn.type(original)->is_oopptr();
          if (t_original->speculative_type() != NULL) {
            original = maybe_cast_profiled_obj(original, t_original->speculative_type(), true);
          }
        }
      }

      bool validated = false;
      // Reason_class_check rather than Reason_intrinsic because we
      // want to intrinsify even if this traps.
      if (!too_many_traps(Deoptimization::Reason_class_check)) {
        Node* not_subtype_ctrl = gen_subtype_check(load_object_klass(original),
                                                   klass_node);

        if (not_subtype_ctrl != top()) {
          PreserveJVMState pjvms(this);
          set_control(not_subtype_ctrl);
          uncommon_trap(Deoptimization::Reason_class_check,
                        Deoptimization::Action_make_not_entrant);
          assert(stopped(), "Should be stopped");
        }
        validated = true;
      }

      if (!stopped()) {
        newcopy = new_array(klass_node, length, 0);  // no arguments to push

        ArrayCopyNode* ac = ArrayCopyNode::make(this, true, original, start, newcopy, intcon(0), moved, true, false,
                                                load_object_klass(original), klass_node);
        if (!is_copyOfRange) {
          ac->set_copyof(validated);
        } else {
          ac->set_copyofrange(validated);
        }
        Node* n = _gvn.transform(ac);
        if (n == ac) {
          ac->connect_outputs(this);
        } else {
          assert(validated, "shouldn't transform if all arguments not validated");
          set_all_memory(n);
        }
      }
    }
  } // original reexecute is set back here

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  if (!stopped()) {
    set_result(newcopy);
  }
  return true;
}


//----------------------generate_virtual_guard---------------------------
// Helper for hashCode and clone.  Peeks inside the vtable to avoid a call.
Node* LibraryCallKit::generate_virtual_guard(Node* obj_klass,
                                             RegionNode* slow_region) {
  ciMethod* method = callee();
  int vtable_index = method->vtable_index();
  assert(vtable_index >= 0 || vtable_index == Method::nonvirtual_vtable_index,
         "bad index %d", vtable_index);
  // Get the Method* out of the appropriate vtable entry.
  int entry_offset  = in_bytes(Klass::vtable_start_offset()) +
                     vtable_index*vtableEntry::size_in_bytes() +
                     vtableEntry::method_offset_in_bytes();
  Node* entry_addr  = basic_plus_adr(obj_klass, entry_offset);
  Node* target_call = make_load(NULL, entry_addr, TypePtr::NOTNULL, T_ADDRESS, MemNode::unordered);

  // Compare the target method with the expected method (e.g., Object.hashCode).
  const TypePtr* native_call_addr = TypeMetadataPtr::make(method);

  Node* native_call = makecon(native_call_addr);
  Node* chk_native  = _gvn.transform(new CmpPNode(target_call, native_call));
  Node* test_native = _gvn.transform(new BoolNode(chk_native, BoolTest::ne));

  return generate_slow_guard(test_native, slow_region);
}

//-----------------------generate_method_call----------------------------
// Use generate_method_call to make a slow-call to the real
// method if the fast path fails.  An alternative would be to
// use a stub like OptoRuntime::slow_arraycopy_Java.
// This only works for expanding the current library call,
// not another intrinsic.  (E.g., don't use this for making an
// arraycopy call inside of the copyOf intrinsic.)
CallJavaNode*
LibraryCallKit::generate_method_call(vmIntrinsics::ID method_id, bool is_virtual, bool is_static) {
  // When compiling the intrinsic method itself, do not use this technique.
  guarantee(callee() != C->method(), "cannot make slow-call to self");

  ciMethod* method = callee();
  // ensure the JVMS we have will be correct for this call
  guarantee(method_id == method->intrinsic_id(), "must match");

  const TypeFunc* tf = TypeFunc::make(method);
  CallJavaNode* slow_call;
  if (is_static) {
    assert(!is_virtual, "");
    slow_call = new CallStaticJavaNode(C, tf,
                           SharedRuntime::get_resolve_static_call_stub(),
                           method, bci());
  } else if (is_virtual) {
    null_check_receiver();
    int vtable_index = Method::invalid_vtable_index;
    if (UseInlineCaches) {
      // Suppress the vtable call
    } else {
      // hashCode and clone are not a miranda methods,
      // so the vtable index is fixed.
      // No need to use the linkResolver to get it.
       vtable_index = method->vtable_index();
       assert(vtable_index >= 0 || vtable_index == Method::nonvirtual_vtable_index,
              "bad index %d", vtable_index);
    }
    slow_call = new CallDynamicJavaNode(tf,
                          SharedRuntime::get_resolve_virtual_call_stub(),
                          method, vtable_index, bci());
  } else {  // neither virtual nor static:  opt_virtual
    null_check_receiver();
    slow_call = new CallStaticJavaNode(C, tf,
                                SharedRuntime::get_resolve_opt_virtual_call_stub(),
                                method, bci());
    slow_call->set_optimized_virtual(true);
  }
  if (CallGenerator::is_inlined_method_handle_intrinsic(this->method(), bci(), callee())) {
    // To be able to issue a direct call (optimized virtual or virtual)
    // and skip a call to MH.linkTo*/invokeBasic adapter, additional information
    // about the method being invoked should be attached to the call site to
    // make resolution logic work (see SharedRuntime::resolve_{virtual,opt_virtual}_call_C).
    slow_call->set_override_symbolic_info(true);
  }
  set_arguments_for_java_call(slow_call);
  set_edges_for_java_call(slow_call);
  return slow_call;
}


/**
 * Build special case code for calls to hashCode on an object. This call may
 * be virtual (invokevirtual) or bound (invokespecial). For each case we generate
 * slightly different code.
 */
bool LibraryCallKit::inline_native_hashcode(bool is_virtual, bool is_static) {
  assert(is_static == callee()->is_static(), "correct intrinsic selection");
  assert(!(is_virtual && is_static), "either virtual, special, or static");

  enum { _slow_path = 1, _fast_path, _null_path, PATH_LIMIT };

  RegionNode* result_reg = new RegionNode(PATH_LIMIT);
  PhiNode*    result_val = new PhiNode(result_reg, TypeInt::INT);
  PhiNode*    result_io  = new PhiNode(result_reg, Type::ABIO);
  PhiNode*    result_mem = new PhiNode(result_reg, Type::MEMORY, TypePtr::BOTTOM);
  Node* obj = NULL;
  if (!is_static) {
    // Check for hashing null object
    obj = null_check_receiver();
    if (stopped())  return true;        // unconditionally null
    result_reg->init_req(_null_path, top());
    result_val->init_req(_null_path, top());
  } else {
    // Do a null check, and return zero if null.
    // System.identityHashCode(null) == 0
    obj = argument(0);
    Node* null_ctl = top();
    obj = null_check_oop(obj, &null_ctl);
    result_reg->init_req(_null_path, null_ctl);
    result_val->init_req(_null_path, _gvn.intcon(0));
  }

  // Unconditionally null?  Then return right away.
  if (stopped()) {
    set_control( result_reg->in(_null_path));
    if (!stopped())
      set_result(result_val->in(_null_path));
    return true;
  }

  // We only go to the fast case code if we pass a number of guards.  The
  // paths which do not pass are accumulated in the slow_region.
  RegionNode* slow_region = new RegionNode(1);
  record_for_igvn(slow_region);

  // If this is a virtual call, we generate a funny guard.  We pull out
  // the vtable entry corresponding to hashCode() from the target object.
  // If the target method which we are calling happens to be the native
  // Object hashCode() method, we pass the guard.  We do not need this
  // guard for non-virtual calls -- the caller is known to be the native
  // Object hashCode().
  if (is_virtual) {
    // After null check, get the object's klass.
    Node* obj_klass = load_object_klass(obj);
    generate_virtual_guard(obj_klass, slow_region);
  }

  // Get the header out of the object, use LoadMarkNode when available
  Node* header_addr = basic_plus_adr(obj, oopDesc::mark_offset_in_bytes());
  // The control of the load must be NULL. Otherwise, the load can move before
  // the null check after castPP removal.
  Node* no_ctrl = NULL;
  Node* header = make_load(no_ctrl, header_addr, TypeX_X, TypeX_X->basic_type(), MemNode::unordered);

  // Test the header to see if it is unlocked.
  Node *lock_mask      = _gvn.MakeConX(markOopDesc::biased_lock_mask_in_place);
  Node *lmasked_header = _gvn.transform(new AndXNode(header, lock_mask));
  Node *unlocked_val   = _gvn.MakeConX(markOopDesc::unlocked_value);
  Node *chk_unlocked   = _gvn.transform(new CmpXNode( lmasked_header, unlocked_val));
  Node *test_unlocked  = _gvn.transform(new BoolNode( chk_unlocked, BoolTest::ne));

  generate_slow_guard(test_unlocked, slow_region);

  // Get the hash value and check to see that it has been properly assigned.
  // We depend on hash_mask being at most 32 bits and avoid the use of
  // hash_mask_in_place because it could be larger than 32 bits in a 64-bit
  // vm: see markOop.hpp.
  Node *hash_mask      = _gvn.intcon(UseCompactObjectHeaders ? markOopDesc::hash_mask_compact : markOopDesc::hash_mask);
  Node *hash_shift     = _gvn.intcon(UseCompactObjectHeaders ? markOopDesc::hash_shift_compact : markOopDesc::hash_shift);
  Node *hshifted_header= _gvn.transform(new URShiftXNode(header, hash_shift));
  // This hack lets the hash bits live anywhere in the mark object now, as long
  // as the shift drops the relevant bits into the low 32 bits.  Note that
  // Java spec says that HashCode is an int so there's no point in capturing
  // an 'X'-sized hashcode (32 in 32-bit build or 64 in 64-bit build).
  hshifted_header      = ConvX2I(hshifted_header);
  Node *hash_val       = _gvn.transform(new AndINode(hshifted_header, hash_mask));

  Node *no_hash_val    = _gvn.intcon(markOopDesc::no_hash);
  Node *chk_assigned   = _gvn.transform(new CmpINode( hash_val, no_hash_val));
  Node *test_assigned  = _gvn.transform(new BoolNode( chk_assigned, BoolTest::eq));

  generate_slow_guard(test_assigned, slow_region);

  Node* init_mem = reset_memory();
  // fill in the rest of the null path:
  result_io ->init_req(_null_path, i_o());
  result_mem->init_req(_null_path, init_mem);

  result_val->init_req(_fast_path, hash_val);
  result_reg->init_req(_fast_path, control());
  result_io ->init_req(_fast_path, i_o());
  result_mem->init_req(_fast_path, init_mem);

  // Generate code for the slow case.  We make a call to hashCode().
  set_control(_gvn.transform(slow_region));
  if (!stopped()) {
    // No need for PreserveJVMState, because we're using up the present state.
    set_all_memory(init_mem);
    vmIntrinsics::ID hashCode_id = is_static ? vmIntrinsics::_identityHashCode : vmIntrinsics::_hashCode;
    CallJavaNode* slow_call = generate_method_call(hashCode_id, is_virtual, is_static);
    Node* slow_result = set_results_for_java_call(slow_call);
    // this->control() comes from set_results_for_java_call
    result_reg->init_req(_slow_path, control());
    result_val->init_req(_slow_path, slow_result);
    result_io  ->set_req(_slow_path, i_o());
    result_mem ->set_req(_slow_path, reset_memory());
  }

  // Return the combined state.
  set_i_o(        _gvn.transform(result_io)  );
  set_all_memory( _gvn.transform(result_mem));

  set_result(result_reg, result_val);
  return true;
}

//---------------------------inline_native_getClass----------------------------
// public final native Class<?> java.lang.Object.getClass();
//
// Build special case code for calls to getClass on an object.
bool LibraryCallKit::inline_native_getClass() {
  Node* obj = null_check_receiver();
  if (stopped())  return true;
  set_result(load_mirror_from_klass(load_object_klass(obj)));
  return true;
}

//-----------------inline_native_Reflection_getCallerClass---------------------
// public static native Class<?> sun.reflect.Reflection.getCallerClass();
//
// In the presence of deep enough inlining, getCallerClass() becomes a no-op.
//
// NOTE: This code must perform the same logic as JVM_GetCallerClass
// in that it must skip particular security frames and checks for
// caller sensitive methods.
bool LibraryCallKit::inline_native_Reflection_getCallerClass() {
#ifndef PRODUCT
  if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
    tty->print_cr("Attempting to inline sun.reflect.Reflection.getCallerClass");
  }
#endif

  if (!jvms()->has_method()) {
#ifndef PRODUCT
    if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
      tty->print_cr("  Bailing out because intrinsic was inlined at top level");
    }
#endif
    return false;
  }

  // Walk back up the JVM state to find the caller at the required
  // depth.
  JVMState* caller_jvms = jvms();

  // Cf. JVM_GetCallerClass
  // NOTE: Start the loop at depth 1 because the current JVM state does
  // not include the Reflection.getCallerClass() frame.
  for (int n = 1; caller_jvms != NULL; caller_jvms = caller_jvms->caller(), n++) {
    ciMethod* m = caller_jvms->method();
    switch (n) {
    case 0:
      fatal("current JVM state does not include the Reflection.getCallerClass frame");
      break;
    case 1:
      // Frame 0 and 1 must be caller sensitive (see JVM_GetCallerClass).
      if (!m->caller_sensitive()) {
#ifndef PRODUCT
        if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
          tty->print_cr("  Bailing out: CallerSensitive annotation expected at frame %d", n);
        }
#endif
        return false;  // bail-out; let JVM_GetCallerClass do the work
      }
      break;
    default:
      if (!m->is_ignored_by_security_stack_walk()) {
        // We have reached the desired frame; return the holder class.
        // Acquire method holder as java.lang.Class and push as constant.
        ciInstanceKlass* caller_klass = caller_jvms->method()->holder();
        ciInstance* caller_mirror = caller_klass->java_mirror();
        set_result(makecon(TypeInstPtr::make(caller_mirror)));

#ifndef PRODUCT
        if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
          tty->print_cr("  Succeeded: caller = %d) %s.%s, JVMS depth = %d", n, caller_klass->name()->as_utf8(), caller_jvms->method()->name()->as_utf8(), jvms()->depth());
          tty->print_cr("  JVM state at this point:");
          for (int i = jvms()->depth(), n = 1; i >= 1; i--, n++) {
            ciMethod* m = jvms()->of_depth(i)->method();
            tty->print_cr("   %d) %s.%s", n, m->holder()->name()->as_utf8(), m->name()->as_utf8());
          }
        }
#endif
        return true;
      }
      break;
    }
  }

#ifndef PRODUCT
  if ((C->print_intrinsics() || C->print_inlining()) && Verbose) {
    tty->print_cr("  Bailing out because caller depth exceeded inlining depth = %d", jvms()->depth());
    tty->print_cr("  JVM state at this point:");
    for (int i = jvms()->depth(), n = 1; i >= 1; i--, n++) {
      ciMethod* m = jvms()->of_depth(i)->method();
      tty->print_cr("   %d) %s.%s", n, m->holder()->name()->as_utf8(), m->name()->as_utf8());
    }
  }
#endif

  return false;  // bail-out; let JVM_GetCallerClass do the work
}

bool LibraryCallKit::inline_fp_conversions(vmIntrinsics::ID id) {
  Node* arg = argument(0);
  Node* result = NULL;

  switch (id) {
  case vmIntrinsics::_floatToRawIntBits:    result = new MoveF2INode(arg);  break;
  case vmIntrinsics::_intBitsToFloat:       result = new MoveI2FNode(arg);  break;
  case vmIntrinsics::_doubleToRawLongBits:  result = new MoveD2LNode(arg);  break;
  case vmIntrinsics::_longBitsToDouble:     result = new MoveL2DNode(arg);  break;

  case vmIntrinsics::_doubleToLongBits: {
    // two paths (plus control) merge in a wood
    RegionNode *r = new RegionNode(3);
    Node *phi = new PhiNode(r, TypeLong::LONG);

    Node *cmpisnan = _gvn.transform(new CmpDNode(arg, arg));
    // Build the boolean node
    Node *bolisnan = _gvn.transform(new BoolNode(cmpisnan, BoolTest::ne));

    // Branch either way.
    // NaN case is less traveled, which makes all the difference.
    IfNode *ifisnan = create_and_xform_if(control(), bolisnan, PROB_STATIC_FREQUENT, COUNT_UNKNOWN);
    Node *opt_isnan = _gvn.transform(ifisnan);
    assert( opt_isnan->is_If(), "Expect an IfNode");
    IfNode *opt_ifisnan = (IfNode*)opt_isnan;
    Node *iftrue = _gvn.transform(new IfTrueNode(opt_ifisnan));

    set_control(iftrue);

    static const jlong nan_bits = CONST64(0x7ff8000000000000);
    Node *slow_result = longcon(nan_bits); // return NaN
    phi->init_req(1, _gvn.transform( slow_result ));
    r->init_req(1, iftrue);

    // Else fall through
    Node *iffalse = _gvn.transform(new IfFalseNode(opt_ifisnan));
    set_control(iffalse);

    phi->init_req(2, _gvn.transform(new MoveD2LNode(arg)));
    r->init_req(2, iffalse);

    // Post merge
    set_control(_gvn.transform(r));
    record_for_igvn(r);

    C->set_has_split_ifs(true); // Has chance for split-if optimization
    result = phi;
    assert(result->bottom_type()->isa_long(), "must be");
    break;
  }

  case vmIntrinsics::_floatToIntBits: {
    // two paths (plus control) merge in a wood
    RegionNode *r = new RegionNode(3);
    Node *phi = new PhiNode(r, TypeInt::INT);

    Node *cmpisnan = _gvn.transform(new CmpFNode(arg, arg));
    // Build the boolean node
    Node *bolisnan = _gvn.transform(new BoolNode(cmpisnan, BoolTest::ne));

    // Branch either way.
    // NaN case is less traveled, which makes all the difference.
    IfNode *ifisnan = create_and_xform_if(control(), bolisnan, PROB_STATIC_FREQUENT, COUNT_UNKNOWN);
    Node *opt_isnan = _gvn.transform(ifisnan);
    assert( opt_isnan->is_If(), "Expect an IfNode");
    IfNode *opt_ifisnan = (IfNode*)opt_isnan;
    Node *iftrue = _gvn.transform(new IfTrueNode(opt_ifisnan));

    set_control(iftrue);

    static const jint nan_bits = 0x7fc00000;
    Node *slow_result = makecon(TypeInt::make(nan_bits)); // return NaN
    phi->init_req(1, _gvn.transform( slow_result ));
    r->init_req(1, iftrue);

    // Else fall through
    Node *iffalse = _gvn.transform(new IfFalseNode(opt_ifisnan));
    set_control(iffalse);

    phi->init_req(2, _gvn.transform(new MoveF2INode(arg)));
    r->init_req(2, iffalse);

    // Post merge
    set_control(_gvn.transform(r));
    record_for_igvn(r);

    C->set_has_split_ifs(true); // Has chance for split-if optimization
    result = phi;
    assert(result->bottom_type()->isa_int(), "must be");
    break;
  }

  default:
    fatal_unexpected_iid(id);
    break;
  }
  set_result(_gvn.transform(result));
  return true;
}

//----------------------inline_unsafe_copyMemory-------------------------
// public native void Unsafe.copyMemory0(Object srcBase, long srcOffset, Object destBase, long destOffset, long bytes);
bool LibraryCallKit::inline_unsafe_copyMemory() {
  if (callee()->is_static())  return false;  // caller must have the capability!
  null_check_receiver();  // null-check receiver
  if (stopped())  return true;

  C->set_has_unsafe_access(true);  // Mark eventual nmethod as "unsafe".

  Node* src_ptr =         argument(1);   // type: oop
  Node* src_off = ConvL2X(argument(2));  // type: long
  Node* dst_ptr =         argument(4);   // type: oop
  Node* dst_off = ConvL2X(argument(5));  // type: long
  Node* size    = ConvL2X(argument(7));  // type: long

  assert(Unsafe_field_offset_to_byte_offset(11) == 11,
         "fieldOffset must be byte-scaled");

  Node* src = make_unsafe_address(src_ptr, src_off);
  Node* dst = make_unsafe_address(dst_ptr, dst_off);

  // Conservatively insert a memory barrier on all memory slices.
  // Do not let writes of the copy source or destination float below the copy.
  insert_mem_bar(Op_MemBarCPUOrder);

  // Call it.  Note that the length argument is not scaled.
  make_runtime_call(RC_LEAF|RC_NO_FP,
                    OptoRuntime::fast_arraycopy_Type(),
                    StubRoutines::unsafe_arraycopy(),
                    "unsafe_arraycopy",
                    TypeRawPtr::BOTTOM,
                    src, dst, size XTOP);

  // Do not let reads of the copy destination float above the copy.
  insert_mem_bar(Op_MemBarCPUOrder);

  return true;
}

//------------------------clone_coping-----------------------------------
// Helper function for inline_native_clone.
void LibraryCallKit::copy_to_clone(Node* obj, Node* alloc_obj, Node* obj_size, bool is_array) {
  assert(obj_size != NULL, "");
  Node* raw_obj = alloc_obj->in(1);
  assert(alloc_obj->is_CheckCastPP() && raw_obj->is_Proj() && raw_obj->in(0)->is_Allocate(), "");

  AllocateNode* alloc = NULL;
  if (ReduceBulkZeroing) {
    // We will be completely responsible for initializing this object -
    // mark Initialize node as complete.
    alloc = AllocateNode::Ideal_allocation(alloc_obj, &_gvn);
    // The object was just allocated - there should be no any stores!
    guarantee(alloc != NULL && alloc->maybe_set_complete(&_gvn), "");
    // Mark as complete_with_arraycopy so that on AllocateNode
    // expansion, we know this AllocateNode is initialized by an array
    // copy and a StoreStore barrier exists after the array copy.
    alloc->initialization()->set_complete_with_arraycopy();
  }

  // Copy the fastest available way.
  // TODO: generate fields copies for small objects instead.
  Node* size = _gvn.transform(obj_size);

  access_clone(control(), obj, alloc_obj, size, is_array);

  // Do not let reads from the cloned object float above the arraycopy.
  if (alloc != NULL) {
    // Do not let stores that initialize this object be reordered with
    // a subsequent store that would make this object accessible by
    // other threads.
    // Record what AllocateNode this StoreStore protects so that
    // escape analysis can go from the MemBarStoreStoreNode to the
    // AllocateNode and eliminate the MemBarStoreStoreNode if possible
    // based on the escape status of the AllocateNode.
    insert_mem_bar(Op_MemBarStoreStore, alloc->proj_out_or_null(AllocateNode::RawAddress));
  } else {
    insert_mem_bar(Op_MemBarCPUOrder);
  }
}

//------------------------inline_native_clone----------------------------
// protected native Object java.lang.Object.clone();
//
// Here are the simple edge cases:
//  null receiver => normal trap
//  virtual and clone was overridden => slow path to out-of-line clone
//  not cloneable or finalizer => slow path to out-of-line Object.clone
//
// The general case has two steps, allocation and copying.
// Allocation has two cases, and uses GraphKit::new_instance or new_array.
//
// Copying also has two cases, oop arrays and everything else.
// Oop arrays use arrayof_oop_arraycopy (same as System.arraycopy).
// Everything else uses the tight inline loop supplied by CopyArrayNode.
//
// These steps fold up nicely if and when the cloned object's klass
// can be sharply typed as an object array, a type array, or an instance.
//
bool LibraryCallKit::inline_native_clone(bool is_virtual) {
  PhiNode* result_val;

  // Set the reexecute bit for the interpreter to reexecute
  // the bytecode that invokes Object.clone if deoptimization happens.
  { PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);

    Node* obj = null_check_receiver();
    if (stopped())  return true;

    const TypeOopPtr* obj_type = _gvn.type(obj)->is_oopptr();

    // If we are going to clone an instance, we need its exact type to
    // know the number and types of fields to convert the clone to
    // loads/stores. Maybe a speculative type can help us.
    if (!obj_type->klass_is_exact() &&
        obj_type->speculative_type() != NULL &&
        obj_type->speculative_type()->is_instance_klass()) {
      ciInstanceKlass* spec_ik = obj_type->speculative_type()->as_instance_klass();
      if (spec_ik->nof_nonstatic_fields() <= ArrayCopyLoadStoreMaxElem &&
          !spec_ik->has_injected_fields()) {
        ciKlass* k = obj_type->klass();
        if (!k->is_instance_klass() ||
            k->as_instance_klass()->is_interface() ||
            k->as_instance_klass()->has_subklass()) {
          obj = maybe_cast_profiled_obj(obj, obj_type->speculative_type(), false);
        }
      }
    }

    Node* obj_klass = load_object_klass(obj);
    const TypeKlassPtr* tklass = _gvn.type(obj_klass)->isa_klassptr();
    const TypeOopPtr*   toop   = ((tklass != NULL)
                                ? tklass->as_instance_type()
                                : TypeInstPtr::NOTNULL);

    // Conservatively insert a memory barrier on all memory slices.
    // Do not let writes into the original float below the clone.
    insert_mem_bar(Op_MemBarCPUOrder);

    // paths into result_reg:
    enum {
      _slow_path = 1,     // out-of-line call to clone method (virtual or not)
      _objArray_path,     // plain array allocation, plus arrayof_oop_arraycopy
      _array_path,        // plain array allocation, plus arrayof_long_arraycopy
      _instance_path,     // plain instance allocation, plus arrayof_long_arraycopy
      PATH_LIMIT
    };
    RegionNode* result_reg = new RegionNode(PATH_LIMIT);
    result_val             = new PhiNode(result_reg, TypeInstPtr::NOTNULL);
    PhiNode*    result_i_o = new PhiNode(result_reg, Type::ABIO);
    PhiNode*    result_mem = new PhiNode(result_reg, Type::MEMORY, TypePtr::BOTTOM);
    record_for_igvn(result_reg);

    Node* array_ctl = generate_array_guard(obj_klass, (RegionNode*)NULL);
    if (array_ctl != NULL) {
      // It's an array.
      PreserveJVMState pjvms(this);
      set_control(array_ctl);
      Node* obj_length = load_array_length(obj);
      Node* obj_size  = NULL;
      Node* alloc_obj = new_array(obj_klass, obj_length, 0, &obj_size, /*deoptimize_on_exception=*/true);

      BarrierSetC2* bs = BarrierSet::barrier_set()->barrier_set_c2();
      if (bs->array_copy_requires_gc_barriers(T_OBJECT)) {
        // If it is an oop array, it requires very special treatment,
        // because gc barriers are required when accessing the array.
        Node* is_obja = generate_objArray_guard(obj_klass, (RegionNode*)NULL);
        if (is_obja != NULL) {
          PreserveJVMState pjvms2(this);
          set_control(is_obja);
          // Generate a direct call to the right arraycopy function(s).
          // Clones are always tightly coupled.
          ArrayCopyNode* ac = ArrayCopyNode::make(this, true, obj, intcon(0), alloc_obj, intcon(0), obj_length, true, false);
          ac->set_cloneoop();
          Node* n = _gvn.transform(ac);
          assert(n == ac, "cannot disappear");
          ac->connect_outputs(this, /*deoptimize_on_exception=*/true);

          result_reg->init_req(_objArray_path, control());
          result_val->init_req(_objArray_path, alloc_obj);
          result_i_o ->set_req(_objArray_path, i_o());
          result_mem ->set_req(_objArray_path, reset_memory());
        }
      }
      // Otherwise, there are no barriers to worry about.
      // (We can dispense with card marks if we know the allocation
      //  comes out of eden (TLAB)...  In fact, ReduceInitialCardMarks
      //  causes the non-eden paths to take compensating steps to
      //  simulate a fresh allocation, so that no further
      //  card marks are required in compiled code to initialize
      //  the object.)

      if (!stopped()) {
        copy_to_clone(obj, alloc_obj, obj_size, true);

        // Present the results of the copy.
        result_reg->init_req(_array_path, control());
        result_val->init_req(_array_path, alloc_obj);
        result_i_o ->set_req(_array_path, i_o());
        result_mem ->set_req(_array_path, reset_memory());
      }
    }

    // We only go to the instance fast case code if we pass a number of guards.
    // The paths which do not pass are accumulated in the slow_region.
    RegionNode* slow_region = new RegionNode(1);
    record_for_igvn(slow_region);
    if (!stopped()) {
      // It's an instance (we did array above).  Make the slow-path tests.
      // If this is a virtual call, we generate a funny guard.  We grab
      // the vtable entry corresponding to clone() from the target object.
      // If the target method which we are calling happens to be the
      // Object clone() method, we pass the guard.  We do not need this
      // guard for non-virtual calls; the caller is known to be the native
      // Object clone().
      if (is_virtual) {
        generate_virtual_guard(obj_klass, slow_region);
      }

      // The object must be easily cloneable and must not have a finalizer.
      // Both of these conditions may be checked in a single test.
      // We could optimize the test further, but we don't care.
      generate_access_flags_guard(obj_klass,
                                  // Test both conditions:
                                  JVM_ACC_IS_CLONEABLE_FAST | JVM_ACC_HAS_FINALIZER,
                                  // Must be cloneable but not finalizer:
                                  JVM_ACC_IS_CLONEABLE_FAST,
                                  slow_region);
    }

    if (!stopped()) {
      // It's an instance, and it passed the slow-path tests.
      PreserveJVMState pjvms(this);
      Node* obj_size  = NULL;
      // Need to deoptimize on exception from allocation since Object.clone intrinsic
      // is reexecuted if deoptimization occurs and there could be problems when merging
      // exception state between multiple Object.clone versions (reexecute=true vs reexecute=false).
      Node* alloc_obj = new_instance(obj_klass, NULL, &obj_size, /*deoptimize_on_exception=*/true);

      copy_to_clone(obj, alloc_obj, obj_size, false);

      // Present the results of the slow call.
      result_reg->init_req(_instance_path, control());
      result_val->init_req(_instance_path, alloc_obj);
      result_i_o ->set_req(_instance_path, i_o());
      result_mem ->set_req(_instance_path, reset_memory());
    }

    // Generate code for the slow case.  We make a call to clone().
    set_control(_gvn.transform(slow_region));
    if (!stopped()) {
      PreserveJVMState pjvms(this);
      CallJavaNode* slow_call = generate_method_call(vmIntrinsics::_clone, is_virtual);
      // We need to deoptimize on exception (see comment above)
      Node* slow_result = set_results_for_java_call(slow_call, false, /* deoptimize */ true);
      // this->control() comes from set_results_for_java_call
      result_reg->init_req(_slow_path, control());
      result_val->init_req(_slow_path, slow_result);
      result_i_o ->set_req(_slow_path, i_o());
      result_mem ->set_req(_slow_path, reset_memory());
    }

    // Return the combined state.
    set_control(    _gvn.transform(result_reg));
    set_i_o(        _gvn.transform(result_i_o));
    set_all_memory( _gvn.transform(result_mem));
  } // original reexecute is set back here

  set_result(_gvn.transform(result_val));
  return true;
}

// If we have a tighly coupled allocation, the arraycopy may take care
// of the array initialization. If one of the guards we insert between
// the allocation and the arraycopy causes a deoptimization, an
// unitialized array will escape the compiled method. To prevent that
// we set the JVM state for uncommon traps between the allocation and
// the arraycopy to the state before the allocation so, in case of
// deoptimization, we'll reexecute the allocation and the
// initialization.
JVMState* LibraryCallKit::arraycopy_restore_alloc_state(AllocateArrayNode* alloc, int& saved_reexecute_sp) {
  if (alloc != NULL) {
    ciMethod* trap_method = alloc->jvms()->method();
    int trap_bci = alloc->jvms()->bci();

    if (!C->too_many_traps(trap_method, trap_bci, Deoptimization::Reason_intrinsic) &&
        !C->too_many_traps(trap_method, trap_bci, Deoptimization::Reason_null_check)) {
      // Make sure there's no store between the allocation and the
      // arraycopy otherwise visible side effects could be rexecuted
      // in case of deoptimization and cause incorrect execution.
      bool no_interfering_store = true;
      Node* mem = alloc->in(TypeFunc::Memory);
      if (mem->is_MergeMem()) {
        for (MergeMemStream mms(merged_memory(), mem->as_MergeMem()); mms.next_non_empty2(); ) {
          Node* n = mms.memory();
          if (n != mms.memory2() && !(n->is_Proj() && n->in(0) == alloc->initialization())) {
            assert(n->is_Store(), "what else?");
            no_interfering_store = false;
            break;
          }
        }
      } else {
        for (MergeMemStream mms(merged_memory()); mms.next_non_empty(); ) {
          Node* n = mms.memory();
          if (n != mem && !(n->is_Proj() && n->in(0) == alloc->initialization())) {
            assert(n->is_Store(), "what else?");
            no_interfering_store = false;
            break;
          }
        }
      }

      if (no_interfering_store) {
        SafePointNode* sfpt = create_safepoint_with_state_before_array_allocation(alloc);

        JVMState* saved_jvms = jvms();
        saved_reexecute_sp = _reexecute_sp;

        set_jvms(sfpt->jvms());
        _reexecute_sp = jvms()->sp();

        return saved_jvms;
      }
    }
  }
  return NULL;
}

// Clone the JVMState of the array allocation and create a new safepoint with it. Re-push the array length to the stack
// such that uncommon traps can be emitted to re-execute the array allocation in the interpreter.
SafePointNode* LibraryCallKit::create_safepoint_with_state_before_array_allocation(const AllocateArrayNode* alloc) const {
  JVMState* old_jvms = alloc->jvms()->clone_shallow(C);
  uint size = alloc->req();
  SafePointNode* sfpt = new SafePointNode(size, old_jvms);
  old_jvms->set_map(sfpt);
  for (uint i = 0; i < size; i++) {
    sfpt->init_req(i, alloc->in(i));
  }
  // re-push array length for deoptimization
  sfpt->ins_req(old_jvms->stkoff() + old_jvms->sp(), alloc->in(AllocateNode::ALength));
  old_jvms->set_sp(old_jvms->sp()+1);
  old_jvms->set_monoff(old_jvms->monoff()+1);
  old_jvms->set_scloff(old_jvms->scloff()+1);
  old_jvms->set_endoff(old_jvms->endoff()+1);
  old_jvms->set_should_reexecute(true);

  sfpt->set_i_o(map()->i_o());
  sfpt->set_memory(map()->memory());
  sfpt->set_control(map()->control());
  return sfpt;
}

// In case of a deoptimization, we restart execution at the
// allocation, allocating a new array. We would leave an uninitialized
// array in the heap that GCs wouldn't expect. Move the allocation
// after the traps so we don't allocate the array if we
// deoptimize. This is possible because tightly_coupled_allocation()
// guarantees there's no observer of the allocated array at this point
// and the control flow is simple enough.
void LibraryCallKit::arraycopy_move_allocation_here(AllocateArrayNode* alloc, Node* dest, JVMState* saved_jvms_before_guards,
                                                    int saved_reexecute_sp, uint new_idx) {
  if (saved_jvms_before_guards != NULL && !stopped()) {
    replace_unrelated_uncommon_traps_with_alloc_state(alloc, saved_jvms_before_guards);

    assert(alloc != NULL, "only with a tightly coupled allocation");
    // restore JVM state to the state at the arraycopy
    saved_jvms_before_guards->map()->set_control(map()->control());
    assert(saved_jvms_before_guards->map()->memory() == map()->memory(), "memory state changed?");
    assert(saved_jvms_before_guards->map()->i_o() == map()->i_o(), "IO state changed?");
    // If we've improved the types of some nodes (null check) while
    // emitting the guards, propagate them to the current state
    map()->replaced_nodes().apply(saved_jvms_before_guards->map(), new_idx);
    set_jvms(saved_jvms_before_guards);
    _reexecute_sp = saved_reexecute_sp;

    // Remove the allocation from above the guards
    CallProjections callprojs;
    alloc->extract_projections(&callprojs, true);
    InitializeNode* init = alloc->initialization();
    Node* alloc_mem = alloc->in(TypeFunc::Memory);
    C->gvn_replace_by(callprojs.fallthrough_ioproj, alloc->in(TypeFunc::I_O));
    C->gvn_replace_by(init->proj_out(TypeFunc::Memory), alloc_mem);

    // The CastIINode created in GraphKit::new_array (in AllocateArrayNode::make_ideal_length) must stay below
    // the allocation (i.e. is only valid if the allocation succeeds):
    // 1) replace CastIINode with AllocateArrayNode's length here
    // 2) Create CastIINode again once allocation has moved (see below) at the end of this method
    //
    // Multiple identical CastIINodes might exist here. Each GraphKit::load_array_length() call will generate
    // new separate CastIINode (arraycopy guard checks or any array length use between array allocation and ararycopy)
    Node* init_control = init->proj_out(TypeFunc::Control);
    Node* alloc_length = alloc->Ideal_length();
#ifdef ASSERT
    Node* prev_cast = NULL;
#endif
    for (uint i = 0; i < init_control->outcnt(); i++) {
      Node* init_out = init_control->raw_out(i);
      if (init_out->is_CastII() && init_out->in(TypeFunc::Control) == init_control && init_out->in(1) == alloc_length) {
#ifdef ASSERT
        if (prev_cast == NULL) {
          prev_cast = init_out;
        } else {
          if (prev_cast->cmp(*init_out) == false) {
            prev_cast->dump();
            init_out->dump();
            assert(false, "not equal CastIINode");
          }
        }
#endif
        C->gvn_replace_by(init_out, alloc_length);
      }
    }
    C->gvn_replace_by(init->proj_out(TypeFunc::Control), alloc->in(0));

    // move the allocation here (after the guards)
    _gvn.hash_delete(alloc);
    alloc->set_req(TypeFunc::Control, control());
    alloc->set_req(TypeFunc::I_O, i_o());
    Node *mem = reset_memory();
    set_all_memory(mem);
    alloc->set_req(TypeFunc::Memory, mem);
    set_control(init->proj_out_or_null(TypeFunc::Control));
    set_i_o(callprojs.fallthrough_ioproj);

    // Update memory as done in GraphKit::set_output_for_allocation()
    const TypeInt* length_type = _gvn.find_int_type(alloc->in(AllocateNode::ALength));
    const TypeOopPtr* ary_type = _gvn.type(alloc->in(AllocateNode::KlassNode))->is_klassptr()->as_instance_type();
    if (ary_type->isa_aryptr() && length_type != NULL) {
      ary_type = ary_type->is_aryptr()->cast_to_size(length_type);
    }
    const TypePtr* telemref = ary_type->add_offset(Type::OffsetBot);
    int            elemidx  = C->get_alias_index(telemref);
    set_memory(init->proj_out_or_null(TypeFunc::Memory), Compile::AliasIdxRaw);
    set_memory(init->proj_out_or_null(TypeFunc::Memory), elemidx);

    Node* allocx = _gvn.transform(alloc);
    assert(allocx == alloc, "where has the allocation gone?");
    assert(dest->is_CheckCastPP(), "not an allocation result?");

    _gvn.hash_delete(dest);
    dest->set_req(0, control());
    Node* destx = _gvn.transform(dest);
    assert(destx == dest, "where has the allocation result gone?");

    array_ideal_length(alloc, ary_type, true);
  }
}

// Unrelated UCTs between the array allocation and the array copy, which are considered safe by tightly_coupled_allocation(),
// need to be replaced by an UCT with a state before the array allocation (including the array length). This is necessary
// because we could hit one of these UCTs (which are executed before the emitted array copy guards and the actual array
// allocation which is moved down in arraycopy_move_allocation_here()). When later resuming execution in the interpreter,
// we would have wrongly skipped the array allocation. To prevent this, we resume execution at the array allocation in
// the interpreter similar to what we are doing for the newly emitted guards for the array copy.
void LibraryCallKit::replace_unrelated_uncommon_traps_with_alloc_state(AllocateArrayNode* alloc,
                                                                       JVMState* saved_jvms_before_guards) {
  if (saved_jvms_before_guards->map()->control()->is_IfProj()) {
    // There is at least one unrelated uncommon trap which needs to be replaced.
    SafePointNode* sfpt = create_safepoint_with_state_before_array_allocation(alloc);

    JVMState* saved_jvms = jvms();
    const int saved_reexecute_sp = _reexecute_sp;
    set_jvms(sfpt->jvms());
    _reexecute_sp = jvms()->sp();

    replace_unrelated_uncommon_traps_with_alloc_state(saved_jvms_before_guards);

    // Restore state
    set_jvms(saved_jvms);
    _reexecute_sp = saved_reexecute_sp;
  }
}

// Replace the unrelated uncommon traps with new uncommon trap nodes by reusing the action and reason. The new uncommon
// traps will have the state of the array allocation. Let the old uncommon trap nodes die.
void LibraryCallKit::replace_unrelated_uncommon_traps_with_alloc_state(JVMState* saved_jvms_before_guards) {
  Node* if_proj = saved_jvms_before_guards->map()->control(); // Start the search right before the newly emitted guards
  while (if_proj->is_IfProj()) {
    CallStaticJavaNode* uncommon_trap = get_uncommon_trap_from_success_proj(if_proj);
    if (uncommon_trap != NULL) {
      create_new_uncommon_trap(uncommon_trap);
    }
    assert(if_proj->in(0)->is_If(), "must be If");
    if_proj = if_proj->in(0)->in(0);
  }
  assert(if_proj->is_Proj() && if_proj->in(0)->is_Initialize(),
         "must have reached control projection of init node");
}

void LibraryCallKit::create_new_uncommon_trap(CallStaticJavaNode* uncommon_trap_call) {
  const int trap_request = uncommon_trap_call->uncommon_trap_request();
  assert(trap_request != 0, "no valid UCT trap request");
  PreserveJVMState pjvms(this);
  set_control(uncommon_trap_call->in(0));
  uncommon_trap(Deoptimization::trap_request_reason(trap_request),
                Deoptimization::trap_request_action(trap_request));
  assert(stopped(), "Should be stopped");
  _gvn.hash_delete(uncommon_trap_call);
  uncommon_trap_call->set_req(0, top()); // not used anymore, kill it
}

//------------------------------inline_arraycopy-----------------------
// public static native void java.lang.System.arraycopy(Object src,  int  srcPos,
//                                                      Object dest, int destPos,
//                                                      int length);
bool LibraryCallKit::inline_arraycopy() {
  // Get the arguments.
  Node* src         = argument(0);  // type: oop
  Node* src_offset  = argument(1);  // type: int
  Node* dest        = argument(2);  // type: oop
  Node* dest_offset = argument(3);  // type: int
  Node* length      = argument(4);  // type: int

  uint new_idx = C->unique();

  // Check for allocation before we add nodes that would confuse
  // tightly_coupled_allocation()
  AllocateArrayNode* alloc = tightly_coupled_allocation(dest, NULL);

  int saved_reexecute_sp = -1;
  JVMState* saved_jvms_before_guards = arraycopy_restore_alloc_state(alloc, saved_reexecute_sp);
  // See arraycopy_restore_alloc_state() comment
  // if alloc == NULL we don't have to worry about a tightly coupled allocation so we can emit all needed guards
  // if saved_jvms_before_guards != NULL (then alloc != NULL) then we can handle guards and a tightly coupled allocation
  // if saved_jvms_before_guards == NULL and alloc != NULL, we can't emit any guards
  bool can_emit_guards = (alloc == NULL || saved_jvms_before_guards != NULL);

  // The following tests must be performed
  // (1) src and dest are arrays.
  // (2) src and dest arrays must have elements of the same BasicType
  // (3) src and dest must not be null.
  // (4) src_offset must not be negative.
  // (5) dest_offset must not be negative.
  // (6) length must not be negative.
  // (7) src_offset + length must not exceed length of src.
  // (8) dest_offset + length must not exceed length of dest.
  // (9) each element of an oop array must be assignable

  // (3) src and dest must not be null.
  // always do this here because we need the JVM state for uncommon traps
  Node* null_ctl = top();
  src  = saved_jvms_before_guards != NULL ? null_check_oop(src, &null_ctl, true, true) : null_check(src, T_ARRAY);
  assert(null_ctl->is_top(), "no null control here");
  dest = null_check(dest, T_ARRAY);

  if (!can_emit_guards) {
    // if saved_jvms_before_guards == NULL and alloc != NULL, we don't emit any
    // guards but the arraycopy node could still take advantage of a
    // tightly allocated allocation. tightly_coupled_allocation() is
    // called again to make sure it takes the null check above into
    // account: the null check is mandatory and if it caused an
    // uncommon trap to be emitted then the allocation can't be
    // considered tightly coupled in this context.
    alloc = tightly_coupled_allocation(dest, NULL);
  }

  bool validated = false;

  const Type* src_type  = _gvn.type(src);
  const Type* dest_type = _gvn.type(dest);
  const TypeAryPtr* top_src  = src_type->isa_aryptr();
  const TypeAryPtr* top_dest = dest_type->isa_aryptr();

  // Do we have the type of src?
  bool has_src = (top_src != NULL && top_src->klass() != NULL);
  // Do we have the type of dest?
  bool has_dest = (top_dest != NULL && top_dest->klass() != NULL);
  // Is the type for src from speculation?
  bool src_spec = false;
  // Is the type for dest from speculation?
  bool dest_spec = false;

  if ((!has_src || !has_dest) && can_emit_guards) {
    // We don't have sufficient type information, let's see if
    // speculative types can help. We need to have types for both src
    // and dest so that it pays off.

    // Do we already have or could we have type information for src
    bool could_have_src = has_src;
    // Do we already have or could we have type information for dest
    bool could_have_dest = has_dest;

    ciKlass* src_k = NULL;
    if (!has_src) {
      src_k = src_type->speculative_type_not_null();
      if (src_k != NULL && src_k->is_array_klass()) {
        could_have_src = true;
      }
    }

    ciKlass* dest_k = NULL;
    if (!has_dest) {
      dest_k = dest_type->speculative_type_not_null();
      if (dest_k != NULL && dest_k->is_array_klass()) {
        could_have_dest = true;
      }
    }

    if (could_have_src && could_have_dest) {
      // This is going to pay off so emit the required guards
      if (!has_src) {
        src = maybe_cast_profiled_obj(src, src_k, true);
        src_type  = _gvn.type(src);
        top_src  = src_type->isa_aryptr();
        has_src = (top_src != NULL && top_src->klass() != NULL);
        src_spec = true;
      }
      if (!has_dest) {
        dest = maybe_cast_profiled_obj(dest, dest_k, true);
        dest_type  = _gvn.type(dest);
        top_dest  = dest_type->isa_aryptr();
        has_dest = (top_dest != NULL && top_dest->klass() != NULL);
        dest_spec = true;
      }
    }
  }

  if (has_src && has_dest && can_emit_guards) {
    BasicType src_elem  = top_src->klass()->as_array_klass()->element_type()->basic_type();
    BasicType dest_elem = top_dest->klass()->as_array_klass()->element_type()->basic_type();
    if (src_elem  == T_ARRAY)  src_elem  = T_OBJECT;
    if (dest_elem == T_ARRAY)  dest_elem = T_OBJECT;

    if (src_elem == dest_elem && src_elem == T_OBJECT) {
      // If both arrays are object arrays then having the exact types
      // for both will remove the need for a subtype check at runtime
      // before the call and may make it possible to pick a faster copy
      // routine (without a subtype check on every element)
      // Do we have the exact type of src?
      bool could_have_src = src_spec;
      // Do we have the exact type of dest?
      bool could_have_dest = dest_spec;
      ciKlass* src_k = top_src->klass();
      ciKlass* dest_k = top_dest->klass();
      if (!src_spec) {
        src_k = src_type->speculative_type_not_null();
        if (src_k != NULL && src_k->is_array_klass()) {
          could_have_src = true;
        }
      }
      if (!dest_spec) {
        dest_k = dest_type->speculative_type_not_null();
        if (dest_k != NULL && dest_k->is_array_klass()) {
          could_have_dest = true;
        }
      }
      if (could_have_src && could_have_dest) {
        // If we can have both exact types, emit the missing guards
        if (could_have_src && !src_spec) {
          src = maybe_cast_profiled_obj(src, src_k, true);
        }
        if (could_have_dest && !dest_spec) {
          dest = maybe_cast_profiled_obj(dest, dest_k, true);
        }
      }
    }
  }

  ciMethod* trap_method = method();
  int trap_bci = bci();
  if (saved_jvms_before_guards != NULL) {
    trap_method = alloc->jvms()->method();
    trap_bci = alloc->jvms()->bci();
  }

  bool negative_length_guard_generated = false;

  if (!C->too_many_traps(trap_method, trap_bci, Deoptimization::Reason_intrinsic) &&
      can_emit_guards &&
      !src->is_top() && !dest->is_top()) {
    // validate arguments: enables transformation the ArrayCopyNode
    validated = true;

    RegionNode* slow_region = new RegionNode(1);
    record_for_igvn(slow_region);

    // (1) src and dest are arrays.
    generate_non_array_guard(load_object_klass(src), slow_region);
    generate_non_array_guard(load_object_klass(dest), slow_region);

    // (2) src and dest arrays must have elements of the same BasicType
    // done at macro expansion or at Ideal transformation time

    // (4) src_offset must not be negative.
    generate_negative_guard(src_offset, slow_region);

    // (5) dest_offset must not be negative.
    generate_negative_guard(dest_offset, slow_region);

    // (7) src_offset + length must not exceed length of src.
    generate_limit_guard(src_offset, length,
                         load_array_length(src),
                         slow_region);

    // (8) dest_offset + length must not exceed length of dest.
    generate_limit_guard(dest_offset, length,
                         load_array_length(dest),
                         slow_region);

    // (6) length must not be negative.
    // This is also checked in generate_arraycopy() during macro expansion, but
    // we also have to check it here for the case where the ArrayCopyNode will
    // be eliminated by Escape Analysis.
    if (EliminateAllocations) {
      generate_negative_guard(length, slow_region);
      negative_length_guard_generated = true;
    }

    // (9) each element of an oop array must be assignable
    Node* src_klass  = load_object_klass(src);
    Node* dest_klass = load_object_klass(dest);
    Node* not_subtype_ctrl = gen_subtype_check(src_klass, dest_klass);

    if (not_subtype_ctrl != top()) {
      PreserveJVMState pjvms(this);
      set_control(not_subtype_ctrl);
      uncommon_trap(Deoptimization::Reason_intrinsic,
                    Deoptimization::Action_make_not_entrant);
      assert(stopped(), "Should be stopped");
    }
    {
      PreserveJVMState pjvms(this);
      set_control(_gvn.transform(slow_region));
      uncommon_trap(Deoptimization::Reason_intrinsic,
                    Deoptimization::Action_make_not_entrant);
      assert(stopped(), "Should be stopped");
    }

    const TypeKlassPtr* dest_klass_t = _gvn.type(dest_klass)->is_klassptr();
    const Type *toop = TypeOopPtr::make_from_klass(dest_klass_t->klass());
    src = _gvn.transform(new CheckCastPPNode(control(), src, toop));
    arraycopy_move_allocation_here(alloc, dest, saved_jvms_before_guards, saved_reexecute_sp, new_idx);
  }

  if (stopped()) {
    return true;
  }

  ArrayCopyNode* ac = ArrayCopyNode::make(this, true, src, src_offset, dest, dest_offset, length, alloc != NULL, negative_length_guard_generated,
                                          // Create LoadRange and LoadKlass nodes for use during macro expansion here
                                          // so the compiler has a chance to eliminate them: during macro expansion,
                                          // we have to set their control (CastPP nodes are eliminated).
                                          load_object_klass(src), load_object_klass(dest),
                                          load_array_length(src), load_array_length(dest));

  ac->set_arraycopy(validated);

  Node* n = _gvn.transform(ac);
  if (n == ac) {
    ac->connect_outputs(this);
  } else {
    assert(validated, "shouldn't transform if all arguments not validated");
    set_all_memory(n);
  }
  clear_upper_avx();


  return true;
}


// Helper function which determines if an arraycopy immediately follows
// an allocation, with no intervening tests or other escapes for the object.
AllocateArrayNode*
LibraryCallKit::tightly_coupled_allocation(Node* ptr,
                                           RegionNode* slow_region) {
  if (stopped())             return NULL;  // no fast path
  if (C->AliasLevel() == 0)  return NULL;  // no MergeMems around

  AllocateArrayNode* alloc = AllocateArrayNode::Ideal_array_allocation(ptr, &_gvn);
  if (alloc == NULL)  return NULL;

  Node* rawmem = memory(Compile::AliasIdxRaw);
  // Is the allocation's memory state untouched?
  if (!(rawmem->is_Proj() && rawmem->in(0)->is_Initialize())) {
    // Bail out if there have been raw-memory effects since the allocation.
    // (Example:  There might have been a call or safepoint.)
    return NULL;
  }
  rawmem = rawmem->in(0)->as_Initialize()->memory(Compile::AliasIdxRaw);
  if (!(rawmem->is_Proj() && rawmem->in(0) == alloc)) {
    return NULL;
  }

  // There must be no unexpected observers of this allocation.
  for (DUIterator_Fast imax, i = ptr->fast_outs(imax); i < imax; i++) {
    Node* obs = ptr->fast_out(i);
    if (obs != this->map()) {
      return NULL;
    }
  }

  // This arraycopy must unconditionally follow the allocation of the ptr.
  Node* alloc_ctl = ptr->in(0);
  Node* ctl = control();
  while (ctl != alloc_ctl) {
    // There may be guards which feed into the slow_region.
    // Any other control flow means that we might not get a chance
    // to finish initializing the allocated object.
    // This code is removed by "8263615: Cleanup tightly_coupled_allocation"
    // I had to adapt it when backporting "8297730: C2: Arraycopy intrinsic throws incorrect exception"
    if (slow_region != NULL) {
      if ((ctl->is_IfFalse() || ctl->is_IfTrue()) && ctl->in(0)->is_If()) {
        IfNode* iff = ctl->in(0)->as_If();
        Node* not_ctl = iff->proj_out_or_null(1 - ctl->as_Proj()->_con);
        if (slow_region->find_edge(not_ctl) >= 1) {
          ctl = iff->in(0);       // This test feeds the known slow_region.
          continue;
        }
      }
    }
    // Various low-level checks bottom out in uncommon traps. These
    // are considered safe since we've already checked above that
    // there is no unexpected observer of this allocation.
    if (get_uncommon_trap_from_success_proj(ctl) != NULL) {
      assert(ctl->in(0)->is_If(), "must be If");
      ctl = ctl->in(0)->in(0);
    } else {
      return NULL;
    }
  }

  // If we get this far, we have an allocation which immediately
  // precedes the arraycopy, and we can take over zeroing the new object.
  // The arraycopy will finish the initialization, and provide
  // a new control state to which we will anchor the destination pointer.

  return alloc;
}

CallStaticJavaNode* LibraryCallKit::get_uncommon_trap_from_success_proj(Node* node) {
  if (node->is_IfProj()) {
    Node* other_proj = node->as_IfProj()->other_if_proj();
    for (DUIterator_Fast jmax, j = other_proj->fast_outs(jmax); j < jmax; j++) {
      Node* obs = other_proj->fast_out(j);
      if (obs->in(0) == other_proj && obs->is_CallStaticJava() &&
          (obs->as_CallStaticJava()->entry_point() == SharedRuntime::uncommon_trap_blob()->entry_point())) {
        return obs->as_CallStaticJava();
      }
    }
  }
  return NULL;
}

//-------------inline_encodeISOArray-----------------------------------
// encode char[] to byte[] in ISO_8859_1
bool LibraryCallKit::inline_encodeISOArray() {
  assert(callee()->signature()->size() == 5, "encodeISOArray has 5 parameters");
  // no receiver since it is static method
  Node *src         = argument(0);
  Node *src_offset  = argument(1);
  Node *dst         = argument(2);
  Node *dst_offset  = argument(3);
  Node *length      = argument(4);

  src = must_be_not_null(src, true);
  dst = must_be_not_null(dst, true);

  const Type* src_type = src->Value(&_gvn);
  const Type* dst_type = dst->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  const TypeAryPtr* top_dest = dst_type->isa_aryptr();
  if (top_src  == NULL || top_src->klass()  == NULL ||
      top_dest == NULL || top_dest->klass() == NULL) {
    // failed array check
    return false;
  }

  // Figure out the size and type of the elements we will be copying.
  BasicType src_elem = top_src->klass()->as_array_klass()->element_type()->basic_type();
  BasicType dst_elem = top_dest->klass()->as_array_klass()->element_type()->basic_type();
  if (!((src_elem == T_CHAR) || (src_elem== T_BYTE)) || dst_elem != T_BYTE) {
    return false;
  }

  Node* src_start = array_element_address(src, src_offset, T_CHAR);
  Node* dst_start = array_element_address(dst, dst_offset, dst_elem);
  // 'src_start' points to src array + scaled offset
  // 'dst_start' points to dst array + scaled offset

  const TypeAryPtr* mtype = TypeAryPtr::BYTES;
  Node* enc = new EncodeISOArrayNode(control(), memory(mtype), src_start, dst_start, length);
  enc = _gvn.transform(enc);
  Node* res_mem = _gvn.transform(new SCMemProjNode(enc));
  set_memory(res_mem, mtype);
  set_result(enc);
  clear_upper_avx();

  return true;
}

//-------------inline_multiplyToLen-----------------------------------
bool LibraryCallKit::inline_multiplyToLen() {
  assert(UseMultiplyToLenIntrinsic, "not implemented on this platform");

  address stubAddr = StubRoutines::multiplyToLen();
  if (stubAddr == NULL) {
    return false; // Intrinsic's stub is not implemented on this platform
  }
  const char* stubName = "multiplyToLen";

  assert(callee()->signature()->size() == 5, "multiplyToLen has 5 parameters");

  // no receiver because it is a static method
  Node* x    = argument(0);
  Node* xlen = argument(1);
  Node* y    = argument(2);
  Node* ylen = argument(3);
  Node* z    = argument(4);

  x = must_be_not_null(x, true);
  y = must_be_not_null(y, true);

  const Type* x_type = x->Value(&_gvn);
  const Type* y_type = y->Value(&_gvn);
  const TypeAryPtr* top_x = x_type->isa_aryptr();
  const TypeAryPtr* top_y = y_type->isa_aryptr();
  if (top_x  == NULL || top_x->klass()  == NULL ||
      top_y == NULL || top_y->klass() == NULL) {
    // failed array check
    return false;
  }

  BasicType x_elem = top_x->klass()->as_array_klass()->element_type()->basic_type();
  BasicType y_elem = top_y->klass()->as_array_klass()->element_type()->basic_type();
  if (x_elem != T_INT || y_elem != T_INT) {
    return false;
  }

  // Set the original stack and the reexecute bit for the interpreter to reexecute
  // the bytecode that invokes BigInteger.multiplyToLen() if deoptimization happens
  // on the return from z array allocation in runtime.
  { PreserveReexecuteState preexecs(this);
    jvms()->set_should_reexecute(true);

    Node* x_start = array_element_address(x, intcon(0), x_elem);
    Node* y_start = array_element_address(y, intcon(0), y_elem);
    // 'x_start' points to x array + scaled xlen
    // 'y_start' points to y array + scaled ylen

    // Allocate the result array
    Node* zlen = _gvn.transform(new AddINode(xlen, ylen));
    ciKlass* klass = ciTypeArrayKlass::make(T_INT);
    Node* klass_node = makecon(TypeKlassPtr::make(klass));

    IdealKit ideal(this);

#define __ ideal.
     Node* one = __ ConI(1);
     Node* zero = __ ConI(0);
     IdealVariable need_alloc(ideal), z_alloc(ideal);  __ declarations_done();
     __ set(need_alloc, zero);
     __ set(z_alloc, z);
     __ if_then(z, BoolTest::eq, null()); {
       __ increment (need_alloc, one);
     } __ else_(); {
       // Update graphKit memory and control from IdealKit.
       sync_kit(ideal);
       Node *cast = new CastPPNode(z, TypePtr::NOTNULL);
       cast->init_req(0, control());
       _gvn.set_type(cast, cast->bottom_type());
       C->record_for_igvn(cast);

       Node* zlen_arg = load_array_length(cast);
       // Update IdealKit memory and control from graphKit.
       __ sync_kit(this);
       __ if_then(zlen_arg, BoolTest::lt, zlen); {
         __ increment (need_alloc, one);
       } __ end_if();
     } __ end_if();

     __ if_then(__ value(need_alloc), BoolTest::ne, zero); {
       // Update graphKit memory and control from IdealKit.
       sync_kit(ideal);
       Node * narr = new_array(klass_node, zlen, 1);
       // Update IdealKit memory and control from graphKit.
       __ sync_kit(this);
       __ set(z_alloc, narr);
     } __ end_if();

     sync_kit(ideal);
     z = __ value(z_alloc);
     // Can't use TypeAryPtr::INTS which uses Bottom offset.
     _gvn.set_type(z, TypeOopPtr::make_from_klass(klass));
     // Final sync IdealKit and GraphKit.
     final_sync(ideal);
#undef __

    Node* z_start = array_element_address(z, intcon(0), T_INT);

    Node* call = make_runtime_call(RC_LEAF|RC_NO_FP,
                                   OptoRuntime::multiplyToLen_Type(),
                                   stubAddr, stubName, TypePtr::BOTTOM,
                                   x_start, xlen, y_start, ylen, z_start, zlen);
  } // original reexecute is set back here

  C->set_has_split_ifs(true); // Has chance for split-if optimization
  set_result(z);
  return true;
}

//-------------inline_squareToLen------------------------------------
bool LibraryCallKit::inline_squareToLen() {
  assert(UseSquareToLenIntrinsic, "not implemented on this platform");

  address stubAddr = StubRoutines::squareToLen();
  if (stubAddr == NULL) {
    return false; // Intrinsic's stub is not implemented on this platform
  }
  const char* stubName = "squareToLen";

  assert(callee()->signature()->size() == 4, "implSquareToLen has 4 parameters");

  Node* x    = argument(0);
  Node* len  = argument(1);
  Node* z    = argument(2);
  Node* zlen = argument(3);

  x = must_be_not_null(x, true);
  z = must_be_not_null(z, true);

  const Type* x_type = x->Value(&_gvn);
  const Type* z_type = z->Value(&_gvn);
  const TypeAryPtr* top_x = x_type->isa_aryptr();
  const TypeAryPtr* top_z = z_type->isa_aryptr();
  if (top_x  == NULL || top_x->klass()  == NULL ||
      top_z  == NULL || top_z->klass()  == NULL) {
    // failed array check
    return false;
  }

  BasicType x_elem = top_x->klass()->as_array_klass()->element_type()->basic_type();
  BasicType z_elem = top_z->klass()->as_array_klass()->element_type()->basic_type();
  if (x_elem != T_INT || z_elem != T_INT) {
    return false;
  }


  Node* x_start = array_element_address(x, intcon(0), x_elem);
  Node* z_start = array_element_address(z, intcon(0), z_elem);

  Node*  call = make_runtime_call(RC_LEAF|RC_NO_FP,
                                  OptoRuntime::squareToLen_Type(),
                                  stubAddr, stubName, TypePtr::BOTTOM,
                                  x_start, len, z_start, zlen);

  set_result(z);
  return true;
}

//-------------inline_mulAdd------------------------------------------
bool LibraryCallKit::inline_mulAdd() {
  assert(UseMulAddIntrinsic, "not implemented on this platform");

  address stubAddr = StubRoutines::mulAdd();
  if (stubAddr == NULL) {
    return false; // Intrinsic's stub is not implemented on this platform
  }
  const char* stubName = "mulAdd";

  assert(callee()->signature()->size() == 5, "mulAdd has 5 parameters");

  Node* out      = argument(0);
  Node* in       = argument(1);
  Node* offset   = argument(2);
  Node* len      = argument(3);
  Node* k        = argument(4);

  out = must_be_not_null(out, true);

  const Type* out_type = out->Value(&_gvn);
  const Type* in_type = in->Value(&_gvn);
  const TypeAryPtr* top_out = out_type->isa_aryptr();
  const TypeAryPtr* top_in = in_type->isa_aryptr();
  if (top_out  == NULL || top_out->klass()  == NULL ||
      top_in == NULL || top_in->klass() == NULL) {
    // failed array check
    return false;
  }

  BasicType out_elem = top_out->klass()->as_array_klass()->element_type()->basic_type();
  BasicType in_elem = top_in->klass()->as_array_klass()->element_type()->basic_type();
  if (out_elem != T_INT || in_elem != T_INT) {
    return false;
  }

  Node* outlen = load_array_length(out);
  Node* new_offset = _gvn.transform(new SubINode(outlen, offset));
  Node* out_start = array_element_address(out, intcon(0), out_elem);
  Node* in_start = array_element_address(in, intcon(0), in_elem);

  Node*  call = make_runtime_call(RC_LEAF|RC_NO_FP,
                                  OptoRuntime::mulAdd_Type(),
                                  stubAddr, stubName, TypePtr::BOTTOM,
                                  out_start,in_start, new_offset, len, k);
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

//-------------inline_montgomeryMultiply-----------------------------------
bool LibraryCallKit::inline_montgomeryMultiply() {
  address stubAddr = StubRoutines::montgomeryMultiply();
  if (stubAddr == NULL) {
    return false; // Intrinsic's stub is not implemented on this platform
  }

  assert(UseMontgomeryMultiplyIntrinsic, "not implemented on this platform");
  const char* stubName = "montgomery_multiply";

  assert(callee()->signature()->size() == 7, "montgomeryMultiply has 7 parameters");

  Node* a    = argument(0);
  Node* b    = argument(1);
  Node* n    = argument(2);
  Node* len  = argument(3);
  Node* inv  = argument(4);
  Node* m    = argument(6);

  const Type* a_type = a->Value(&_gvn);
  const TypeAryPtr* top_a = a_type->isa_aryptr();
  const Type* b_type = b->Value(&_gvn);
  const TypeAryPtr* top_b = b_type->isa_aryptr();
  const Type* n_type = a->Value(&_gvn);
  const TypeAryPtr* top_n = n_type->isa_aryptr();
  const Type* m_type = a->Value(&_gvn);
  const TypeAryPtr* top_m = m_type->isa_aryptr();
  if (top_a  == NULL || top_a->klass()  == NULL ||
      top_b == NULL || top_b->klass()  == NULL ||
      top_n == NULL || top_n->klass()  == NULL ||
      top_m == NULL || top_m->klass()  == NULL) {
    // failed array check
    return false;
  }

  BasicType a_elem = top_a->klass()->as_array_klass()->element_type()->basic_type();
  BasicType b_elem = top_b->klass()->as_array_klass()->element_type()->basic_type();
  BasicType n_elem = top_n->klass()->as_array_klass()->element_type()->basic_type();
  BasicType m_elem = top_m->klass()->as_array_klass()->element_type()->basic_type();
  if (a_elem != T_INT || b_elem != T_INT || n_elem != T_INT || m_elem != T_INT) {
    return false;
  }

  // Make the call
  {
    Node* a_start = array_element_address(a, intcon(0), a_elem);
    Node* b_start = array_element_address(b, intcon(0), b_elem);
    Node* n_start = array_element_address(n, intcon(0), n_elem);
    Node* m_start = array_element_address(m, intcon(0), m_elem);

    Node* call = make_runtime_call(RC_LEAF,
                                   OptoRuntime::montgomeryMultiply_Type(),
                                   stubAddr, stubName, TypePtr::BOTTOM,
                                   a_start, b_start, n_start, len, inv, top(),
                                   m_start);
    set_result(m);
  }

  return true;
}

bool LibraryCallKit::inline_montgomerySquare() {
  address stubAddr = StubRoutines::montgomerySquare();
  if (stubAddr == NULL) {
    return false; // Intrinsic's stub is not implemented on this platform
  }

  assert(UseMontgomerySquareIntrinsic, "not implemented on this platform");
  const char* stubName = "montgomery_square";

  assert(callee()->signature()->size() == 6, "montgomerySquare has 6 parameters");

  Node* a    = argument(0);
  Node* n    = argument(1);
  Node* len  = argument(2);
  Node* inv  = argument(3);
  Node* m    = argument(5);

  const Type* a_type = a->Value(&_gvn);
  const TypeAryPtr* top_a = a_type->isa_aryptr();
  const Type* n_type = a->Value(&_gvn);
  const TypeAryPtr* top_n = n_type->isa_aryptr();
  const Type* m_type = a->Value(&_gvn);
  const TypeAryPtr* top_m = m_type->isa_aryptr();
  if (top_a  == NULL || top_a->klass()  == NULL ||
      top_n == NULL || top_n->klass()  == NULL ||
      top_m == NULL || top_m->klass()  == NULL) {
    // failed array check
    return false;
  }

  BasicType a_elem = top_a->klass()->as_array_klass()->element_type()->basic_type();
  BasicType n_elem = top_n->klass()->as_array_klass()->element_type()->basic_type();
  BasicType m_elem = top_m->klass()->as_array_klass()->element_type()->basic_type();
  if (a_elem != T_INT || n_elem != T_INT || m_elem != T_INT) {
    return false;
  }

  // Make the call
  {
    Node* a_start = array_element_address(a, intcon(0), a_elem);
    Node* n_start = array_element_address(n, intcon(0), n_elem);
    Node* m_start = array_element_address(m, intcon(0), m_elem);

    Node* call = make_runtime_call(RC_LEAF,
                                   OptoRuntime::montgomerySquare_Type(),
                                   stubAddr, stubName, TypePtr::BOTTOM,
                                   a_start, n_start, len, inv, top(),
                                   m_start);
    set_result(m);
  }

  return true;
}

//-------------inline_vectorizedMismatch------------------------------
bool LibraryCallKit::inline_vectorizedMismatch() {
  assert(UseVectorizedMismatchIntrinsic, "not implementated on this platform");

  address stubAddr = StubRoutines::vectorizedMismatch();
  if (stubAddr == NULL) {
    return false; // Intrinsic's stub is not implemented on this platform
  }
  const char* stubName = "vectorizedMismatch";
  int size_l = callee()->signature()->size();
  assert(callee()->signature()->size() == 8, "vectorizedMismatch has 6 parameters");

  Node* obja = argument(0);
  Node* aoffset = argument(1);
  Node* objb = argument(3);
  Node* boffset = argument(4);
  Node* length = argument(6);
  Node* scale = argument(7);

  const Type* a_type = obja->Value(&_gvn);
  const Type* b_type = objb->Value(&_gvn);
  const TypeAryPtr* top_a = a_type->isa_aryptr();
  const TypeAryPtr* top_b = b_type->isa_aryptr();
  if (top_a == NULL || top_a->klass() == NULL ||
    top_b == NULL || top_b->klass() == NULL) {
    // failed array check
    return false;
  }

  Node* call;
  jvms()->set_should_reexecute(true);

  Node* obja_adr = make_unsafe_address(obja, aoffset);
  Node* objb_adr = make_unsafe_address(objb, boffset);

  call = make_runtime_call(RC_LEAF,
    OptoRuntime::vectorizedMismatch_Type(),
    stubAddr, stubName, TypePtr::BOTTOM,
    obja_adr, objb_adr, length, scale);

  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

/**
 * Calculate CRC32 for byte.
 * int java.util.zip.CRC32.update(int crc, int b)
 */
bool LibraryCallKit::inline_updateCRC32() {
  assert(UseCRC32Intrinsics, "need AVX and LCMUL instructions support");
  assert(callee()->signature()->size() == 2, "update has 2 parameters");
  // no receiver since it is static method
  Node* crc  = argument(0); // type: int
  Node* b    = argument(1); // type: int

  /*
   *    int c = ~ crc;
   *    b = timesXtoThe32[(b ^ c) & 0xFF];
   *    b = b ^ (c >>> 8);
   *    crc = ~b;
   */

  Node* M1 = intcon(-1);
  crc = _gvn.transform(new XorINode(crc, M1));
  Node* result = _gvn.transform(new XorINode(crc, b));
  result = _gvn.transform(new AndINode(result, intcon(0xFF)));

  Node* base = makecon(TypeRawPtr::make(StubRoutines::crc_table_addr()));
  Node* offset = _gvn.transform(new LShiftINode(result, intcon(0x2)));
  Node* adr = basic_plus_adr(top(), base, ConvI2X(offset));
  result = make_load(control(), adr, TypeInt::INT, T_INT, MemNode::unordered);

  crc = _gvn.transform(new URShiftINode(crc, intcon(8)));
  result = _gvn.transform(new XorINode(crc, result));
  result = _gvn.transform(new XorINode(result, M1));
  set_result(result);
  return true;
}

/**
 * Calculate CRC32 for byte[] array.
 * int java.util.zip.CRC32.updateBytes(int crc, byte[] buf, int off, int len)
 */
bool LibraryCallKit::inline_updateBytesCRC32() {
  assert(UseCRC32Intrinsics, "need AVX and LCMUL instructions support");
  assert(callee()->signature()->size() == 4, "updateBytes has 4 parameters");
  // no receiver since it is static method
  Node* crc     = argument(0); // type: int
  Node* src     = argument(1); // type: oop
  Node* offset  = argument(2); // type: int
  Node* length  = argument(3); // type: int

  const Type* src_type = src->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  if (top_src  == NULL || top_src->klass()  == NULL) {
    // failed array check
    return false;
  }

  // Figure out the size and type of the elements we will be copying.
  BasicType src_elem = top_src->klass()->as_array_klass()->element_type()->basic_type();
  if (src_elem != T_BYTE) {
    return false;
  }

  // 'src_start' points to src array + scaled offset
  src = must_be_not_null(src, true);
  Node* src_start = array_element_address(src, offset, src_elem);

  // We assume that range check is done by caller.
  // TODO: generate range check (offset+length < src.length) in debug VM.

  // Call the stub.
  address stubAddr = StubRoutines::updateBytesCRC32();
  const char *stubName = "updateBytesCRC32";

  Node* call = make_runtime_call(RC_LEAF|RC_NO_FP, OptoRuntime::updateBytesCRC32_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 crc, src_start, length);
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

/**
 * Calculate CRC32 for ByteBuffer.
 * int java.util.zip.CRC32.updateByteBuffer(int crc, long buf, int off, int len)
 */
bool LibraryCallKit::inline_updateByteBufferCRC32() {
  assert(UseCRC32Intrinsics, "need AVX and LCMUL instructions support");
  assert(callee()->signature()->size() == 5, "updateByteBuffer has 4 parameters and one is long");
  // no receiver since it is static method
  Node* crc     = argument(0); // type: int
  Node* src     = argument(1); // type: long
  Node* offset  = argument(3); // type: int
  Node* length  = argument(4); // type: int

  src = ConvL2X(src);  // adjust Java long to machine word
  Node* base = _gvn.transform(new CastX2PNode(src));
  offset = ConvI2X(offset);

  // 'src_start' points to src array + scaled offset
  Node* src_start = basic_plus_adr(top(), base, offset);

  // Call the stub.
  address stubAddr = StubRoutines::updateBytesCRC32();
  const char *stubName = "updateBytesCRC32";

  Node* call = make_runtime_call(RC_LEAF|RC_NO_FP, OptoRuntime::updateBytesCRC32_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 crc, src_start, length);
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

//------------------------------get_table_from_crc32c_class-----------------------
Node * LibraryCallKit::get_table_from_crc32c_class(ciInstanceKlass *crc32c_class) {
  Node* table = load_field_from_object(NULL, "byteTable", "[I", /*is_exact*/ false, /*is_static*/ true, crc32c_class);
  assert (table != NULL, "wrong version of java.util.zip.CRC32C");

  return table;
}

//------------------------------inline_updateBytesCRC32C-----------------------
//
// Calculate CRC32C for byte[] array.
// int java.util.zip.CRC32C.updateBytes(int crc, byte[] buf, int off, int end)
//
bool LibraryCallKit::inline_updateBytesCRC32C() {
  assert(UseCRC32CIntrinsics, "need CRC32C instruction support");
  assert(callee()->signature()->size() == 4, "updateBytes has 4 parameters");
  assert(callee()->holder()->is_loaded(), "CRC32C class must be loaded");
  // no receiver since it is a static method
  Node* crc     = argument(0); // type: int
  Node* src     = argument(1); // type: oop
  Node* offset  = argument(2); // type: int
  Node* end     = argument(3); // type: int

  Node* length = _gvn.transform(new SubINode(end, offset));

  const Type* src_type = src->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  if (top_src  == NULL || top_src->klass()  == NULL) {
    // failed array check
    return false;
  }

  // Figure out the size and type of the elements we will be copying.
  BasicType src_elem = top_src->klass()->as_array_klass()->element_type()->basic_type();
  if (src_elem != T_BYTE) {
    return false;
  }

  // 'src_start' points to src array + scaled offset
  src = must_be_not_null(src, true);
  Node* src_start = array_element_address(src, offset, src_elem);

  // static final int[] byteTable in class CRC32C
  Node* table = get_table_from_crc32c_class(callee()->holder());
  table = must_be_not_null(table, true);
  Node* table_start = array_element_address(table, intcon(0), T_INT);

  // We assume that range check is done by caller.
  // TODO: generate range check (offset+length < src.length) in debug VM.

  // Call the stub.
  address stubAddr = StubRoutines::updateBytesCRC32C();
  const char *stubName = "updateBytesCRC32C";

  Node* call = make_runtime_call(RC_LEAF, OptoRuntime::updateBytesCRC32C_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 crc, src_start, length, table_start);
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

//------------------------------inline_updateDirectByteBufferCRC32C-----------------------
//
// Calculate CRC32C for DirectByteBuffer.
// int java.util.zip.CRC32C.updateDirectByteBuffer(int crc, long buf, int off, int end)
//
bool LibraryCallKit::inline_updateDirectByteBufferCRC32C() {
  assert(UseCRC32CIntrinsics, "need CRC32C instruction support");
  assert(callee()->signature()->size() == 5, "updateDirectByteBuffer has 4 parameters and one is long");
  assert(callee()->holder()->is_loaded(), "CRC32C class must be loaded");
  // no receiver since it is a static method
  Node* crc     = argument(0); // type: int
  Node* src     = argument(1); // type: long
  Node* offset  = argument(3); // type: int
  Node* end     = argument(4); // type: int

  Node* length = _gvn.transform(new SubINode(end, offset));

  src = ConvL2X(src);  // adjust Java long to machine word
  Node* base = _gvn.transform(new CastX2PNode(src));
  offset = ConvI2X(offset);

  // 'src_start' points to src array + scaled offset
  Node* src_start = basic_plus_adr(top(), base, offset);

  // static final int[] byteTable in class CRC32C
  Node* table = get_table_from_crc32c_class(callee()->holder());
  table = must_be_not_null(table, true);
  Node* table_start = array_element_address(table, intcon(0), T_INT);

  // Call the stub.
  address stubAddr = StubRoutines::updateBytesCRC32C();
  const char *stubName = "updateBytesCRC32C";

  Node* call = make_runtime_call(RC_LEAF, OptoRuntime::updateBytesCRC32C_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 crc, src_start, length, table_start);
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

//------------------------------inline_updateBytesAdler32----------------------
//
// Calculate Adler32 checksum for byte[] array.
// int java.util.zip.Adler32.updateBytes(int crc, byte[] buf, int off, int len)
//
bool LibraryCallKit::inline_updateBytesAdler32() {
  assert(UseAdler32Intrinsics, "Adler32 Instrinsic support need"); // check if we actually need to check this flag or check a different one
  assert(callee()->signature()->size() == 4, "updateBytes has 4 parameters");
  assert(callee()->holder()->is_loaded(), "Adler32 class must be loaded");
  // no receiver since it is static method
  Node* crc     = argument(0); // type: int
  Node* src     = argument(1); // type: oop
  Node* offset  = argument(2); // type: int
  Node* length  = argument(3); // type: int

  const Type* src_type = src->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  if (top_src  == NULL || top_src->klass()  == NULL) {
    // failed array check
    return false;
  }

  // Figure out the size and type of the elements we will be copying.
  BasicType src_elem = top_src->klass()->as_array_klass()->element_type()->basic_type();
  if (src_elem != T_BYTE) {
    return false;
  }

  // 'src_start' points to src array + scaled offset
  Node* src_start = array_element_address(src, offset, src_elem);

  // We assume that range check is done by caller.
  // TODO: generate range check (offset+length < src.length) in debug VM.

  // Call the stub.
  address stubAddr = StubRoutines::updateBytesAdler32();
  const char *stubName = "updateBytesAdler32";

  Node* call = make_runtime_call(RC_LEAF, OptoRuntime::updateBytesAdler32_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 crc, src_start, length);
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

//------------------------------inline_updateByteBufferAdler32---------------
//
// Calculate Adler32 checksum for DirectByteBuffer.
// int java.util.zip.Adler32.updateByteBuffer(int crc, long buf, int off, int len)
//
bool LibraryCallKit::inline_updateByteBufferAdler32() {
  assert(UseAdler32Intrinsics, "Adler32 Instrinsic support need"); // check if we actually need to check this flag or check a different one
  assert(callee()->signature()->size() == 5, "updateByteBuffer has 4 parameters and one is long");
  assert(callee()->holder()->is_loaded(), "Adler32 class must be loaded");
  // no receiver since it is static method
  Node* crc     = argument(0); // type: int
  Node* src     = argument(1); // type: long
  Node* offset  = argument(3); // type: int
  Node* length  = argument(4); // type: int

  src = ConvL2X(src);  // adjust Java long to machine word
  Node* base = _gvn.transform(new CastX2PNode(src));
  offset = ConvI2X(offset);

  // 'src_start' points to src array + scaled offset
  Node* src_start = basic_plus_adr(top(), base, offset);

  // Call the stub.
  address stubAddr = StubRoutines::updateBytesAdler32();
  const char *stubName = "updateBytesAdler32";

  Node* call = make_runtime_call(RC_LEAF, OptoRuntime::updateBytesAdler32_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 crc, src_start, length);

  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);
  return true;
}

//----------------------------inline_reference_get----------------------------
// public T java.lang.ref.Reference.get();
bool LibraryCallKit::inline_reference_get() {
  const int referent_offset = java_lang_ref_Reference::referent_offset;
  guarantee(referent_offset > 0, "should have already been set");

  // Get the argument:
  Node* reference_obj = null_check_receiver();
  if (stopped()) return true;

  const TypeInstPtr* tinst = _gvn.type(reference_obj)->isa_instptr();
  assert(tinst != NULL, "obj is null");
  assert(tinst->klass()->is_loaded(), "obj is not loaded");
  ciInstanceKlass* referenceKlass = tinst->klass()->as_instance_klass();
  ciField* field = referenceKlass->get_field_by_name(ciSymbol::make("referent"),
                                                     ciSymbol::make("Ljava/lang/Object;"),
                                                     false);
  assert (field != NULL, "undefined field");

  Node* adr = basic_plus_adr(reference_obj, reference_obj, referent_offset);
  const TypePtr* adr_type = C->alias_type(field)->adr_type();

  ciInstanceKlass* klass = env()->Object_klass();
  const TypeOopPtr* object_type = TypeOopPtr::make_from_klass(klass);

  DecoratorSet decorators = IN_HEAP | ON_WEAK_OOP_REF;
  Node* result = access_load_at(reference_obj, adr, adr_type, object_type, T_OBJECT, decorators);
  // Add memory barrier to prevent commoning reads from this field
  // across safepoint since GC can change its value.
  insert_mem_bar(Op_MemBarCPUOrder);

  set_result(result);
  return true;
}


Node * LibraryCallKit::load_field_from_object(Node * fromObj, const char * fieldName, const char * fieldTypeString,
                                              bool is_exact=true, bool is_static=false,
                                              ciInstanceKlass * fromKls=NULL) {
  if (fromKls == NULL) {
    const TypeInstPtr* tinst = _gvn.type(fromObj)->isa_instptr();
    assert(tinst != NULL, "obj is null");
    assert(tinst->klass()->is_loaded(), "obj is not loaded");
    assert(!is_exact || tinst->klass_is_exact(), "klass not exact");
    fromKls = tinst->klass()->as_instance_klass();
  } else {
    assert(is_static, "only for static field access");
  }
  ciField* field = fromKls->get_field_by_name(ciSymbol::make(fieldName),
                                              ciSymbol::make(fieldTypeString),
                                              is_static);

  assert (field != NULL, "undefined field");
  if (field == NULL) return (Node *) NULL;

  if (is_static) {
    const TypeInstPtr* tip = TypeInstPtr::make(fromKls->java_mirror());
    fromObj = makecon(tip);
  }

  // Next code  copied from Parse::do_get_xxx():

  // Compute address and memory type.
  int offset  = field->offset_in_bytes();
  bool is_vol = field->is_volatile();
  ciType* field_klass = field->type();
  assert(field_klass->is_loaded(), "should be loaded");
  const TypePtr* adr_type = C->alias_type(field)->adr_type();
  Node *adr = basic_plus_adr(fromObj, fromObj, offset);
  BasicType bt = field->layout_type();

  // Build the resultant type of the load
  const Type *type;
  if (bt == T_OBJECT) {
    type = TypeOopPtr::make_from_klass(field_klass->as_klass());
  } else {
    type = Type::get_const_basic_type(bt);
  }

  DecoratorSet decorators = IN_HEAP;

  if (is_vol) {
    decorators |= MO_SEQ_CST;
  }

  return access_load_at(fromObj, adr, adr_type, type, bt, decorators);
}

Node * LibraryCallKit::field_address_from_object(Node * fromObj, const char * fieldName, const char * fieldTypeString,
                                                 bool is_exact = true, bool is_static = false,
                                                 ciInstanceKlass * fromKls = NULL) {
  if (fromKls == NULL) {
    const TypeInstPtr* tinst = _gvn.type(fromObj)->isa_instptr();
    assert(tinst != NULL, "obj is null");
    assert(tinst->klass()->is_loaded(), "obj is not loaded");
    assert(!is_exact || tinst->klass_is_exact(), "klass not exact");
    fromKls = tinst->klass()->as_instance_klass();
  }
  else {
    assert(is_static, "only for static field access");
  }
  ciField* field = fromKls->get_field_by_name(ciSymbol::make(fieldName),
    ciSymbol::make(fieldTypeString),
    is_static);

  assert(field != NULL, "undefined field");
  assert(!field->is_volatile(), "not defined for volatile fields");

  if (is_static) {
    const TypeInstPtr* tip = TypeInstPtr::make(fromKls->java_mirror());
    fromObj = makecon(tip);
  }

  // Next code  copied from Parse::do_get_xxx():

  // Compute address and memory type.
  int offset = field->offset_in_bytes();
  Node *adr = basic_plus_adr(fromObj, fromObj, offset);

  return adr;
}

//------------------------------inline_aescrypt_Block-----------------------
bool LibraryCallKit::inline_aescrypt_Block(vmIntrinsics::ID id) {
  address stubAddr = NULL;
  const char *stubName;
  assert(UseAES, "need AES instruction support");

  switch(id) {
  case vmIntrinsics::_aescrypt_encryptBlock:
    stubAddr = StubRoutines::aescrypt_encryptBlock();
    stubName = "aescrypt_encryptBlock";
    break;
  case vmIntrinsics::_aescrypt_decryptBlock:
    stubAddr = StubRoutines::aescrypt_decryptBlock();
    stubName = "aescrypt_decryptBlock";
    break;
  default:
    break;
  }
  if (stubAddr == NULL) return false;

  Node* aescrypt_object = argument(0);
  Node* src             = argument(1);
  Node* src_offset      = argument(2);
  Node* dest            = argument(3);
  Node* dest_offset     = argument(4);

  src = must_be_not_null(src, true);
  dest = must_be_not_null(dest, true);

  // (1) src and dest are arrays.
  const Type* src_type = src->Value(&_gvn);
  const Type* dest_type = dest->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  const TypeAryPtr* top_dest = dest_type->isa_aryptr();
  assert (top_src  != NULL && top_src->klass()  != NULL &&  top_dest != NULL && top_dest->klass() != NULL, "args are strange");

  // for the quick and dirty code we will skip all the checks.
  // we are just trying to get the call to be generated.
  Node* src_start  = src;
  Node* dest_start = dest;
  if (src_offset != NULL || dest_offset != NULL) {
    assert(src_offset != NULL && dest_offset != NULL, "");
    src_start  = array_element_address(src,  src_offset,  T_BYTE);
    dest_start = array_element_address(dest, dest_offset, T_BYTE);
  }

  // now need to get the start of its expanded key array
  // this requires a newer class file that has this array as littleEndian ints, otherwise we revert to java
  Node* k_start = get_key_start_from_aescrypt_object(aescrypt_object);
  if (k_start == NULL) return false;

  if (Matcher::pass_original_key_for_aes()) {
    // on SPARC we need to pass the original key since key expansion needs to happen in intrinsics due to
    // compatibility issues between Java key expansion and SPARC crypto instructions
    Node* original_k_start = get_original_key_start_from_aescrypt_object(aescrypt_object);
    if (original_k_start == NULL) return false;

    // Call the stub.
    make_runtime_call(RC_LEAF|RC_NO_FP, OptoRuntime::aescrypt_block_Type(),
                      stubAddr, stubName, TypePtr::BOTTOM,
                      src_start, dest_start, k_start, original_k_start);
  } else {
    // Call the stub.
    make_runtime_call(RC_LEAF|RC_NO_FP, OptoRuntime::aescrypt_block_Type(),
                      stubAddr, stubName, TypePtr::BOTTOM,
                      src_start, dest_start, k_start);
  }

  return true;
}

//------------------------------inline_cipherBlockChaining_AESCrypt-----------------------
bool LibraryCallKit::inline_cipherBlockChaining_AESCrypt(vmIntrinsics::ID id) {
  address stubAddr = NULL;
  const char *stubName = NULL;

  assert(UseAES, "need AES instruction support");

  switch(id) {
  case vmIntrinsics::_cipherBlockChaining_encryptAESCrypt:
    stubAddr = StubRoutines::cipherBlockChaining_encryptAESCrypt();
    stubName = "cipherBlockChaining_encryptAESCrypt";
    break;
  case vmIntrinsics::_cipherBlockChaining_decryptAESCrypt:
    stubAddr = StubRoutines::cipherBlockChaining_decryptAESCrypt();
    stubName = "cipherBlockChaining_decryptAESCrypt";
    break;
  default:
    break;
  }
  if (stubAddr == NULL) return false;

  Node* cipherBlockChaining_object = argument(0);
  Node* src                        = argument(1);
  Node* src_offset                 = argument(2);
  Node* len                        = argument(3);
  Node* dest                       = argument(4);
  Node* dest_offset                = argument(5);

  src = must_be_not_null(src, false);
  dest = must_be_not_null(dest, false);

  // (1) src and dest are arrays.
  const Type* src_type = src->Value(&_gvn);
  const Type* dest_type = dest->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  const TypeAryPtr* top_dest = dest_type->isa_aryptr();
  assert (top_src  != NULL && top_src->klass()  != NULL
          &&  top_dest != NULL && top_dest->klass() != NULL, "args are strange");

  // checks are the responsibility of the caller
  Node* src_start  = src;
  Node* dest_start = dest;
  if (src_offset != NULL || dest_offset != NULL) {
    assert(src_offset != NULL && dest_offset != NULL, "");
    src_start  = array_element_address(src,  src_offset,  T_BYTE);
    dest_start = array_element_address(dest, dest_offset, T_BYTE);
  }

  // if we are in this set of code, we "know" the embeddedCipher is an AESCrypt object
  // (because of the predicated logic executed earlier).
  // so we cast it here safely.
  // this requires a newer class file that has this array as littleEndian ints, otherwise we revert to java

  Node* embeddedCipherObj = load_field_from_object(cipherBlockChaining_object, "embeddedCipher", "Lcom/sun/crypto/provider/SymmetricCipher;", /*is_exact*/ false);
  if (embeddedCipherObj == NULL) return false;

  // cast it to what we know it will be at runtime
  const TypeInstPtr* tinst = _gvn.type(cipherBlockChaining_object)->isa_instptr();
  assert(tinst != NULL, "CBC obj is null");
  assert(tinst->klass()->is_loaded(), "CBC obj is not loaded");
  ciKlass* klass_AESCrypt = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make("com/sun/crypto/provider/AESCrypt"));
  assert(klass_AESCrypt->is_loaded(), "predicate checks that this class is loaded");

  ciInstanceKlass* instklass_AESCrypt = klass_AESCrypt->as_instance_klass();
  const TypeKlassPtr* aklass = TypeKlassPtr::make(instklass_AESCrypt);
  const TypeOopPtr* xtype = aklass->as_instance_type();
  Node* aescrypt_object = new CheckCastPPNode(control(), embeddedCipherObj, xtype);
  aescrypt_object = _gvn.transform(aescrypt_object);

  // we need to get the start of the aescrypt_object's expanded key array
  Node* k_start = get_key_start_from_aescrypt_object(aescrypt_object);
  if (k_start == NULL) return false;

  // similarly, get the start address of the r vector
  Node* objRvec = load_field_from_object(cipherBlockChaining_object, "r", "[B", /*is_exact*/ false);
  if (objRvec == NULL) return false;
  Node* r_start = array_element_address(objRvec, intcon(0), T_BYTE);

  Node* cbcCrypt;
  if (Matcher::pass_original_key_for_aes()) {
    // on SPARC we need to pass the original key since key expansion needs to happen in intrinsics due to
    // compatibility issues between Java key expansion and SPARC crypto instructions
    Node* original_k_start = get_original_key_start_from_aescrypt_object(aescrypt_object);
    if (original_k_start == NULL) return false;

    // Call the stub, passing src_start, dest_start, k_start, r_start, src_len and original_k_start
    cbcCrypt = make_runtime_call(RC_LEAF|RC_NO_FP,
                                 OptoRuntime::cipherBlockChaining_aescrypt_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 src_start, dest_start, k_start, r_start, len, original_k_start);
  } else {
    // Call the stub, passing src_start, dest_start, k_start, r_start and src_len
    cbcCrypt = make_runtime_call(RC_LEAF|RC_NO_FP,
                                 OptoRuntime::cipherBlockChaining_aescrypt_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 src_start, dest_start, k_start, r_start, len);
  }

  // return cipher length (int)
  Node* retvalue = _gvn.transform(new ProjNode(cbcCrypt, TypeFunc::Parms));
  set_result(retvalue);
  return true;
}

//------------------------------inline_electronicCodeBook_AESCrypt-----------------------
bool LibraryCallKit::inline_electronicCodeBook_AESCrypt(vmIntrinsics::ID id) {
  address stubAddr = NULL;
  const char *stubName = NULL;

  assert(UseAES, "need AES instruction support");

  switch (id) {
  case vmIntrinsics::_electronicCodeBook_encryptAESCrypt:
    stubAddr = StubRoutines::electronicCodeBook_encryptAESCrypt();
    stubName = "electronicCodeBook_encryptAESCrypt";
    break;
  case vmIntrinsics::_electronicCodeBook_decryptAESCrypt:
    stubAddr = StubRoutines::electronicCodeBook_decryptAESCrypt();
    stubName = "electronicCodeBook_decryptAESCrypt";
    break;
  default:
    break;
  }

  if (stubAddr == NULL) return false;

  Node* electronicCodeBook_object = argument(0);
  Node* src                       = argument(1);
  Node* src_offset                = argument(2);
  Node* len                       = argument(3);
  Node* dest                      = argument(4);
  Node* dest_offset               = argument(5);

  // (1) src and dest are arrays.
  const Type* src_type = src->Value(&_gvn);
  const Type* dest_type = dest->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  const TypeAryPtr* top_dest = dest_type->isa_aryptr();
  assert(top_src != NULL && top_src->klass() != NULL
         &&  top_dest != NULL && top_dest->klass() != NULL, "args are strange");

  // checks are the responsibility of the caller
  Node* src_start = src;
  Node* dest_start = dest;
  if (src_offset != NULL || dest_offset != NULL) {
    assert(src_offset != NULL && dest_offset != NULL, "");
    src_start = array_element_address(src, src_offset, T_BYTE);
    dest_start = array_element_address(dest, dest_offset, T_BYTE);
  }

  // if we are in this set of code, we "know" the embeddedCipher is an AESCrypt object
  // (because of the predicated logic executed earlier).
  // so we cast it here safely.
  // this requires a newer class file that has this array as littleEndian ints, otherwise we revert to java

  Node* embeddedCipherObj = load_field_from_object(electronicCodeBook_object, "embeddedCipher", "Lcom/sun/crypto/provider/SymmetricCipher;", /*is_exact*/ false);
  if (embeddedCipherObj == NULL) return false;

  // cast it to what we know it will be at runtime
  const TypeInstPtr* tinst = _gvn.type(electronicCodeBook_object)->isa_instptr();
  assert(tinst != NULL, "ECB obj is null");
  assert(tinst->klass()->is_loaded(), "ECB obj is not loaded");
  ciKlass* klass_AESCrypt = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make("com/sun/crypto/provider/AESCrypt"));
  assert(klass_AESCrypt->is_loaded(), "predicate checks that this class is loaded");

  ciInstanceKlass* instklass_AESCrypt = klass_AESCrypt->as_instance_klass();
  const TypeKlassPtr* aklass = TypeKlassPtr::make(instklass_AESCrypt);
  const TypeOopPtr* xtype = aklass->as_instance_type();
  Node* aescrypt_object = new CheckCastPPNode(control(), embeddedCipherObj, xtype);
  aescrypt_object = _gvn.transform(aescrypt_object);

  // we need to get the start of the aescrypt_object's expanded key array
  Node* k_start = get_key_start_from_aescrypt_object(aescrypt_object);
  if (k_start == NULL) return false;

  Node* ecbCrypt;
  if (Matcher::pass_original_key_for_aes()) {
    // no SPARC version for AES/ECB intrinsics now.
    return false;
  }
  // Call the stub, passing src_start, dest_start, k_start, r_start and src_len
  ecbCrypt = make_runtime_call(RC_LEAF | RC_NO_FP,
                               OptoRuntime::electronicCodeBook_aescrypt_Type(),
                               stubAddr, stubName, TypePtr::BOTTOM,
                               src_start, dest_start, k_start, len);

  // return cipher length (int)
  Node* retvalue = _gvn.transform(new ProjNode(ecbCrypt, TypeFunc::Parms));
  set_result(retvalue);
  return true;
}

//------------------------------inline_counterMode_AESCrypt-----------------------
bool LibraryCallKit::inline_counterMode_AESCrypt(vmIntrinsics::ID id) {
  assert(UseAES, "need AES instruction support");
  if (!UseAESCTRIntrinsics) return false;

  address stubAddr = NULL;
  const char *stubName = NULL;
  if (id == vmIntrinsics::_counterMode_AESCrypt) {
    stubAddr = StubRoutines::counterMode_AESCrypt();
    stubName = "counterMode_AESCrypt";
  }
  if (stubAddr == NULL) return false;

  Node* counterMode_object = argument(0);
  Node* src = argument(1);
  Node* src_offset = argument(2);
  Node* len = argument(3);
  Node* dest = argument(4);
  Node* dest_offset = argument(5);

  // (1) src and dest are arrays.
  const Type* src_type = src->Value(&_gvn);
  const Type* dest_type = dest->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  const TypeAryPtr* top_dest = dest_type->isa_aryptr();
  assert(top_src != NULL && top_src->klass() != NULL &&
         top_dest != NULL && top_dest->klass() != NULL, "args are strange");

  // checks are the responsibility of the caller
  Node* src_start = src;
  Node* dest_start = dest;
  if (src_offset != NULL || dest_offset != NULL) {
    assert(src_offset != NULL && dest_offset != NULL, "");
    src_start = array_element_address(src, src_offset, T_BYTE);
    dest_start = array_element_address(dest, dest_offset, T_BYTE);
  }

  // if we are in this set of code, we "know" the embeddedCipher is an AESCrypt object
  // (because of the predicated logic executed earlier).
  // so we cast it here safely.
  // this requires a newer class file that has this array as littleEndian ints, otherwise we revert to java
  Node* embeddedCipherObj = load_field_from_object(counterMode_object, "embeddedCipher", "Lcom/sun/crypto/provider/SymmetricCipher;", /*is_exact*/ false);
  if (embeddedCipherObj == NULL) return false;
  // cast it to what we know it will be at runtime
  const TypeInstPtr* tinst = _gvn.type(counterMode_object)->isa_instptr();
  assert(tinst != NULL, "CTR obj is null");
  assert(tinst->klass()->is_loaded(), "CTR obj is not loaded");
  ciKlass* klass_AESCrypt = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make("com/sun/crypto/provider/AESCrypt"));
  assert(klass_AESCrypt->is_loaded(), "predicate checks that this class is loaded");
  ciInstanceKlass* instklass_AESCrypt = klass_AESCrypt->as_instance_klass();
  const TypeKlassPtr* aklass = TypeKlassPtr::make(instklass_AESCrypt);
  const TypeOopPtr* xtype = aklass->as_instance_type();
  Node* aescrypt_object = new CheckCastPPNode(control(), embeddedCipherObj, xtype);
  aescrypt_object = _gvn.transform(aescrypt_object);
  // we need to get the start of the aescrypt_object's expanded key array
  Node* k_start = get_key_start_from_aescrypt_object(aescrypt_object);
  if (k_start == NULL) return false;
  // similarly, get the start address of the r vector
  Node* obj_counter = load_field_from_object(counterMode_object, "counter", "[B", /*is_exact*/ false);
  if (obj_counter == NULL) return false;
  Node* cnt_start = array_element_address(obj_counter, intcon(0), T_BYTE);

  Node* saved_encCounter = load_field_from_object(counterMode_object, "encryptedCounter", "[B", /*is_exact*/ false);
  if (saved_encCounter == NULL) return false;
  Node* saved_encCounter_start = array_element_address(saved_encCounter, intcon(0), T_BYTE);
  Node* used = field_address_from_object(counterMode_object, "used", "I", /*is_exact*/ false);

  Node* ctrCrypt;
  if (Matcher::pass_original_key_for_aes()) {
    // no SPARC version for AES/CTR intrinsics now.
    return false;
  }
  // Call the stub, passing src_start, dest_start, k_start, r_start and src_len
  ctrCrypt = make_runtime_call(RC_LEAF|RC_NO_FP,
                               OptoRuntime::counterMode_aescrypt_Type(),
                               stubAddr, stubName, TypePtr::BOTTOM,
                               src_start, dest_start, k_start, cnt_start, len, saved_encCounter_start, used);

  // return cipher length (int)
  Node* retvalue = _gvn.transform(new ProjNode(ctrCrypt, TypeFunc::Parms));
  set_result(retvalue);
  return true;
}

//------------------------------get_key_start_from_aescrypt_object-----------------------
Node * LibraryCallKit::get_key_start_from_aescrypt_object(Node *aescrypt_object) {
#if defined(PPC64) || defined(S390)
  // MixColumns for decryption can be reduced by preprocessing MixColumns with round keys.
  // Intel's extention is based on this optimization and AESCrypt generates round keys by preprocessing MixColumns.
  // However, ppc64 vncipher processes MixColumns and requires the same round keys with encryption.
  // The ppc64 stubs of encryption and decryption use the same round keys (sessionK[0]).
  Node* objSessionK = load_field_from_object(aescrypt_object, "sessionK", "[[I", /*is_exact*/ false);
  assert (objSessionK != NULL, "wrong version of com.sun.crypto.provider.AESCrypt");
  if (objSessionK == NULL) {
    return (Node *) NULL;
  }
  Node* objAESCryptKey = load_array_element(control(), objSessionK, intcon(0), TypeAryPtr::OOPS);
#else
  Node* objAESCryptKey = load_field_from_object(aescrypt_object, "K", "[I", /*is_exact*/ false);
#endif // PPC64
  assert (objAESCryptKey != NULL, "wrong version of com.sun.crypto.provider.AESCrypt");
  if (objAESCryptKey == NULL) return (Node *) NULL;

  // now have the array, need to get the start address of the K array
  Node* k_start = array_element_address(objAESCryptKey, intcon(0), T_INT);
  return k_start;
}

//------------------------------get_original_key_start_from_aescrypt_object-----------------------
Node * LibraryCallKit::get_original_key_start_from_aescrypt_object(Node *aescrypt_object) {
  Node* objAESCryptKey = load_field_from_object(aescrypt_object, "lastKey", "[B", /*is_exact*/ false);
  assert (objAESCryptKey != NULL, "wrong version of com.sun.crypto.provider.AESCrypt");
  if (objAESCryptKey == NULL) return (Node *) NULL;

  // now have the array, need to get the start address of the lastKey array
  Node* original_k_start = array_element_address(objAESCryptKey, intcon(0), T_BYTE);
  return original_k_start;
}

//----------------------------inline_cipherBlockChaining_AESCrypt_predicate----------------------------
// Return node representing slow path of predicate check.
// the pseudo code we want to emulate with this predicate is:
// for encryption:
//    if (embeddedCipherObj instanceof AESCrypt) do_intrinsic, else do_javapath
// for decryption:
//    if ((embeddedCipherObj instanceof AESCrypt) && (cipher!=plain)) do_intrinsic, else do_javapath
//    note cipher==plain is more conservative than the original java code but that's OK
//
Node* LibraryCallKit::inline_cipherBlockChaining_AESCrypt_predicate(bool decrypting) {
  // The receiver was checked for NULL already.
  Node* objCBC = argument(0);

  Node* src = argument(1);
  Node* dest = argument(4);

  // Load embeddedCipher field of CipherBlockChaining object.
  Node* embeddedCipherObj = load_field_from_object(objCBC, "embeddedCipher", "Lcom/sun/crypto/provider/SymmetricCipher;", /*is_exact*/ false);

  // get AESCrypt klass for instanceOf check
  // AESCrypt might not be loaded yet if some other SymmetricCipher got us to this compile point
  // will have same classloader as CipherBlockChaining object
  const TypeInstPtr* tinst = _gvn.type(objCBC)->isa_instptr();
  assert(tinst != NULL, "CBCobj is null");
  assert(tinst->klass()->is_loaded(), "CBCobj is not loaded");

  // we want to do an instanceof comparison against the AESCrypt class
  ciKlass* klass_AESCrypt = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make("com/sun/crypto/provider/AESCrypt"));
  if (!klass_AESCrypt->is_loaded()) {
    // if AESCrypt is not even loaded, we never take the intrinsic fast path
    Node* ctrl = control();
    set_control(top()); // no regular fast path
    return ctrl;
  }

  src = must_be_not_null(src, true);
  dest = must_be_not_null(dest, true);

  ciInstanceKlass* instklass_AESCrypt = klass_AESCrypt->as_instance_klass();

  Node* instof = gen_instanceof(embeddedCipherObj, makecon(TypeKlassPtr::make(instklass_AESCrypt)));
  Node* cmp_instof  = _gvn.transform(new CmpINode(instof, intcon(1)));
  Node* bool_instof  = _gvn.transform(new BoolNode(cmp_instof, BoolTest::ne));

  Node* instof_false = generate_guard(bool_instof, NULL, PROB_MIN);

  // for encryption, we are done
  if (!decrypting)
    return instof_false;  // even if it is NULL

  // for decryption, we need to add a further check to avoid
  // taking the intrinsic path when cipher and plain are the same
  // see the original java code for why.
  RegionNode* region = new RegionNode(3);
  region->init_req(1, instof_false);

  Node* cmp_src_dest = _gvn.transform(new CmpPNode(src, dest));
  Node* bool_src_dest = _gvn.transform(new BoolNode(cmp_src_dest, BoolTest::eq));
  Node* src_dest_conjoint = generate_guard(bool_src_dest, NULL, PROB_MIN);
  region->init_req(2, src_dest_conjoint);

  record_for_igvn(region);
  return _gvn.transform(region);
}

//----------------------------inline_electronicCodeBook_AESCrypt_predicate----------------------------
// Return node representing slow path of predicate check.
// the pseudo code we want to emulate with this predicate is:
// for encryption:
//    if (embeddedCipherObj instanceof AESCrypt) do_intrinsic, else do_javapath
// for decryption:
//    if ((embeddedCipherObj instanceof AESCrypt) && (cipher!=plain)) do_intrinsic, else do_javapath
//    note cipher==plain is more conservative than the original java code but that's OK
//
Node* LibraryCallKit::inline_electronicCodeBook_AESCrypt_predicate(bool decrypting) {
  // The receiver was checked for NULL already.
  Node* objECB = argument(0);

  // Load embeddedCipher field of ElectronicCodeBook object.
  Node* embeddedCipherObj = load_field_from_object(objECB, "embeddedCipher", "Lcom/sun/crypto/provider/SymmetricCipher;", /*is_exact*/ false);

  // get AESCrypt klass for instanceOf check
  // AESCrypt might not be loaded yet if some other SymmetricCipher got us to this compile point
  // will have same classloader as ElectronicCodeBook object
  const TypeInstPtr* tinst = _gvn.type(objECB)->isa_instptr();
  assert(tinst != NULL, "ECBobj is null");
  assert(tinst->klass()->is_loaded(), "ECBobj is not loaded");

  // we want to do an instanceof comparison against the AESCrypt class
  ciKlass* klass_AESCrypt = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make("com/sun/crypto/provider/AESCrypt"));
  if (!klass_AESCrypt->is_loaded()) {
    // if AESCrypt is not even loaded, we never take the intrinsic fast path
    Node* ctrl = control();
    set_control(top()); // no regular fast path
    return ctrl;
  }
  ciInstanceKlass* instklass_AESCrypt = klass_AESCrypt->as_instance_klass();

  Node* instof = gen_instanceof(embeddedCipherObj, makecon(TypeKlassPtr::make(instklass_AESCrypt)));
  Node* cmp_instof = _gvn.transform(new CmpINode(instof, intcon(1)));
  Node* bool_instof = _gvn.transform(new BoolNode(cmp_instof, BoolTest::ne));

  Node* instof_false = generate_guard(bool_instof, NULL, PROB_MIN);

  // for encryption, we are done
  if (!decrypting)
    return instof_false;  // even if it is NULL

  // for decryption, we need to add a further check to avoid
  // taking the intrinsic path when cipher and plain are the same
  // see the original java code for why.
  RegionNode* region = new RegionNode(3);
  region->init_req(1, instof_false);
  Node* src = argument(1);
  Node* dest = argument(4);
  Node* cmp_src_dest = _gvn.transform(new CmpPNode(src, dest));
  Node* bool_src_dest = _gvn.transform(new BoolNode(cmp_src_dest, BoolTest::eq));
  Node* src_dest_conjoint = generate_guard(bool_src_dest, NULL, PROB_MIN);
  region->init_req(2, src_dest_conjoint);

  record_for_igvn(region);
  return _gvn.transform(region);
}

//----------------------------inline_counterMode_AESCrypt_predicate----------------------------
// Return node representing slow path of predicate check.
// the pseudo code we want to emulate with this predicate is:
// for encryption:
//    if (embeddedCipherObj instanceof AESCrypt) do_intrinsic, else do_javapath
// for decryption:
//    if ((embeddedCipherObj instanceof AESCrypt) && (cipher!=plain)) do_intrinsic, else do_javapath
//    note cipher==plain is more conservative than the original java code but that's OK
//

Node* LibraryCallKit::inline_counterMode_AESCrypt_predicate() {
  // The receiver was checked for NULL already.
  Node* objCTR = argument(0);

  // Load embeddedCipher field of CipherBlockChaining object.
  Node* embeddedCipherObj = load_field_from_object(objCTR, "embeddedCipher", "Lcom/sun/crypto/provider/SymmetricCipher;", /*is_exact*/ false);

  // get AESCrypt klass for instanceOf check
  // AESCrypt might not be loaded yet if some other SymmetricCipher got us to this compile point
  // will have same classloader as CipherBlockChaining object
  const TypeInstPtr* tinst = _gvn.type(objCTR)->isa_instptr();
  assert(tinst != NULL, "CTRobj is null");
  assert(tinst->klass()->is_loaded(), "CTRobj is not loaded");

  // we want to do an instanceof comparison against the AESCrypt class
  ciKlass* klass_AESCrypt = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make("com/sun/crypto/provider/AESCrypt"));
  if (!klass_AESCrypt->is_loaded()) {
    // if AESCrypt is not even loaded, we never take the intrinsic fast path
    Node* ctrl = control();
    set_control(top()); // no regular fast path
    return ctrl;
  }

  ciInstanceKlass* instklass_AESCrypt = klass_AESCrypt->as_instance_klass();
  Node* instof = gen_instanceof(embeddedCipherObj, makecon(TypeKlassPtr::make(instklass_AESCrypt)));
  Node* cmp_instof = _gvn.transform(new CmpINode(instof, intcon(1)));
  Node* bool_instof = _gvn.transform(new BoolNode(cmp_instof, BoolTest::ne));
  Node* instof_false = generate_guard(bool_instof, NULL, PROB_MIN);

  return instof_false; // even if it is NULL
}

//------------------------------inline_ghash_processBlocks
bool LibraryCallKit::inline_ghash_processBlocks() {
  address stubAddr;
  const char *stubName;
  assert(UseGHASHIntrinsics, "need GHASH intrinsics support");

  stubAddr = StubRoutines::ghash_processBlocks();
  stubName = "ghash_processBlocks";

  Node* data           = argument(0);
  Node* offset         = argument(1);
  Node* len            = argument(2);
  Node* state          = argument(3);
  Node* subkeyH        = argument(4);

  state = must_be_not_null(state, true);
  subkeyH = must_be_not_null(subkeyH, true);
  data = must_be_not_null(data, true);

  Node* state_start  = array_element_address(state, intcon(0), T_LONG);
  assert(state_start, "state is NULL");
  Node* subkeyH_start  = array_element_address(subkeyH, intcon(0), T_LONG);
  assert(subkeyH_start, "subkeyH is NULL");
  Node* data_start  = array_element_address(data, offset, T_BYTE);
  assert(data_start, "data is NULL");

  Node* ghash = make_runtime_call(RC_LEAF|RC_NO_FP,
                                  OptoRuntime::ghash_processBlocks_Type(),
                                  stubAddr, stubName, TypePtr::BOTTOM,
                                  state_start, subkeyH_start, data_start, len);
  return true;
}

bool LibraryCallKit::inline_base64_encodeBlock() {
  address stubAddr;
  const char *stubName;
  assert(UseBASE64Intrinsics, "need Base64 intrinsics support");
  assert(callee()->signature()->size() == 6, "base64_encodeBlock has 6 parameters");
  stubAddr = StubRoutines::base64_encodeBlock();
  stubName = "encodeBlock";

  if (!stubAddr) return false;
  Node* base64obj = argument(0);
  Node* src = argument(1);
  Node* offset = argument(2);
  Node* len = argument(3);
  Node* dest = argument(4);
  Node* dp = argument(5);
  Node* isURL = argument(6);

  Node* src_start = array_element_address(src, intcon(0), T_BYTE);
  assert(src_start, "source array is NULL");
  Node* dest_start = array_element_address(dest, intcon(0), T_BYTE);
  assert(dest_start, "destination array is NULL");

  Node* base64 = make_runtime_call(RC_LEAF,
                                   OptoRuntime::base64_encodeBlock_Type(),
                                   stubAddr, stubName, TypePtr::BOTTOM,
                                   src_start, offset, len, dest_start, dp, isURL);
  return true;
}

//------------------------------inline_sha_implCompress-----------------------
//
// Calculate SHA (i.e., SHA-1) for single-block byte[] array.
// void com.sun.security.provider.SHA.implCompress(byte[] buf, int ofs)
//
// Calculate SHA2 (i.e., SHA-244 or SHA-256) for single-block byte[] array.
// void com.sun.security.provider.SHA2.implCompress(byte[] buf, int ofs)
//
// Calculate SHA5 (i.e., SHA-384 or SHA-512) for single-block byte[] array.
// void com.sun.security.provider.SHA5.implCompress(byte[] buf, int ofs)
//
bool LibraryCallKit::inline_sha_implCompress(vmIntrinsics::ID id) {
  assert(callee()->signature()->size() == 2, "sha_implCompress has 2 parameters");

  Node* sha_obj = argument(0);
  Node* src     = argument(1); // type oop
  Node* ofs     = argument(2); // type int

  const Type* src_type = src->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  if (top_src  == NULL || top_src->klass()  == NULL) {
    // failed array check
    return false;
  }
  // Figure out the size and type of the elements we will be copying.
  BasicType src_elem = top_src->klass()->as_array_klass()->element_type()->basic_type();
  if (src_elem != T_BYTE) {
    return false;
  }
  // 'src_start' points to src array + offset
  src = must_be_not_null(src, true);
  Node* src_start = array_element_address(src, ofs, src_elem);
  Node* state = NULL;
  address stubAddr;
  const char *stubName;

  switch(id) {
  case vmIntrinsics::_sha_implCompress:
    assert(UseSHA1Intrinsics, "need SHA1 instruction support");
    state = get_state_from_sha_object(sha_obj);
    stubAddr = StubRoutines::sha1_implCompress();
    stubName = "sha1_implCompress";
    break;
  case vmIntrinsics::_sha2_implCompress:
    assert(UseSHA256Intrinsics, "need SHA256 instruction support");
    state = get_state_from_sha_object(sha_obj);
    stubAddr = StubRoutines::sha256_implCompress();
    stubName = "sha256_implCompress";
    break;
  case vmIntrinsics::_sha5_implCompress:
    assert(UseSHA512Intrinsics, "need SHA512 instruction support");
    state = get_state_from_sha5_object(sha_obj);
    stubAddr = StubRoutines::sha512_implCompress();
    stubName = "sha512_implCompress";
    break;
  default:
    fatal_unexpected_iid(id);
    return false;
  }
  if (state == NULL) return false;

  assert(stubAddr != NULL, "Stub is generated");
  if (stubAddr == NULL) return false;

  // Call the stub.
  Node* call = make_runtime_call(RC_LEAF|RC_NO_FP, OptoRuntime::sha_implCompress_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 src_start, state);

  return true;
}

//------------------------------inline_digestBase_implCompressMB-----------------------
//
// Calculate SHA/SHA2/SHA5 for multi-block byte[] array.
// int com.sun.security.provider.DigestBase.implCompressMultiBlock(byte[] b, int ofs, int limit)
//
bool LibraryCallKit::inline_digestBase_implCompressMB(int predicate) {
  assert(UseSHA1Intrinsics || UseSHA256Intrinsics || UseSHA512Intrinsics,
         "need SHA1/SHA256/SHA512 instruction support");
  assert((uint)predicate < 3, "sanity");
  assert(callee()->signature()->size() == 3, "digestBase_implCompressMB has 3 parameters");

  Node* digestBase_obj = argument(0); // The receiver was checked for NULL already.
  Node* src            = argument(1); // byte[] array
  Node* ofs            = argument(2); // type int
  Node* limit          = argument(3); // type int

  const Type* src_type = src->Value(&_gvn);
  const TypeAryPtr* top_src = src_type->isa_aryptr();
  if (top_src  == NULL || top_src->klass()  == NULL) {
    // failed array check
    return false;
  }
  // Figure out the size and type of the elements we will be copying.
  BasicType src_elem = top_src->klass()->as_array_klass()->element_type()->basic_type();
  if (src_elem != T_BYTE) {
    return false;
  }
  // 'src_start' points to src array + offset
  src = must_be_not_null(src, false);
  Node* src_start = array_element_address(src, ofs, src_elem);

  const char* klass_SHA_name = NULL;
  const char* stub_name = NULL;
  address     stub_addr = NULL;
  bool        long_state = false;

  switch (predicate) {
  case 0:
    if (UseSHA1Intrinsics) {
      klass_SHA_name = "sun/security/provider/SHA";
      stub_name = "sha1_implCompressMB";
      stub_addr = StubRoutines::sha1_implCompressMB();
    }
    break;
  case 1:
    if (UseSHA256Intrinsics) {
      klass_SHA_name = "sun/security/provider/SHA2";
      stub_name = "sha256_implCompressMB";
      stub_addr = StubRoutines::sha256_implCompressMB();
    }
    break;
  case 2:
    if (UseSHA512Intrinsics) {
      klass_SHA_name = "sun/security/provider/SHA5";
      stub_name = "sha512_implCompressMB";
      stub_addr = StubRoutines::sha512_implCompressMB();
      long_state = true;
    }
    break;
  default:
    fatal("unknown SHA intrinsic predicate: %d", predicate);
  }
  if (klass_SHA_name != NULL) {
    assert(stub_addr != NULL, "Stub is generated");
    if (stub_addr == NULL) return false;

    // get DigestBase klass to lookup for SHA klass
    const TypeInstPtr* tinst = _gvn.type(digestBase_obj)->isa_instptr();
    assert(tinst != NULL, "digestBase_obj is not instance???");
    assert(tinst->klass()->is_loaded(), "DigestBase is not loaded");

    ciKlass* klass_SHA = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make(klass_SHA_name));
    assert(klass_SHA->is_loaded(), "predicate checks that this class is loaded");
    ciInstanceKlass* instklass_SHA = klass_SHA->as_instance_klass();
    return inline_sha_implCompressMB(digestBase_obj, instklass_SHA, long_state, stub_addr, stub_name, src_start, ofs, limit);
  }
  return false;
}
//------------------------------inline_sha_implCompressMB-----------------------
bool LibraryCallKit::inline_sha_implCompressMB(Node* digestBase_obj, ciInstanceKlass* instklass_SHA,
                                               bool long_state, address stubAddr, const char *stubName,
                                               Node* src_start, Node* ofs, Node* limit) {
  const TypeKlassPtr* aklass = TypeKlassPtr::make(instklass_SHA);
  const TypeOopPtr* xtype = aklass->as_instance_type();
  Node* sha_obj = new CheckCastPPNode(control(), digestBase_obj, xtype);
  sha_obj = _gvn.transform(sha_obj);

  Node* state;
  if (long_state) {
    state = get_state_from_sha5_object(sha_obj);
  } else {
    state = get_state_from_sha_object(sha_obj);
  }
  if (state == NULL) return false;

  // Call the stub.
  Node* call = make_runtime_call(RC_LEAF|RC_NO_FP,
                                 OptoRuntime::digestBase_implCompressMB_Type(),
                                 stubAddr, stubName, TypePtr::BOTTOM,
                                 src_start, state, ofs, limit);
  // return ofs (int)
  Node* result = _gvn.transform(new ProjNode(call, TypeFunc::Parms));
  set_result(result);

  return true;
}

//------------------------------get_state_from_sha_object-----------------------
Node * LibraryCallKit::get_state_from_sha_object(Node *sha_object) {
  Node* sha_state = load_field_from_object(sha_object, "state", "[I", /*is_exact*/ false);
  assert (sha_state != NULL, "wrong version of sun.security.provider.SHA/SHA2");
  if (sha_state == NULL) return (Node *) NULL;

  // now have the array, need to get the start address of the state array
  Node* state = array_element_address(sha_state, intcon(0), T_INT);
  return state;
}

//------------------------------get_state_from_sha5_object-----------------------
Node * LibraryCallKit::get_state_from_sha5_object(Node *sha_object) {
  Node* sha_state = load_field_from_object(sha_object, "state", "[J", /*is_exact*/ false);
  assert (sha_state != NULL, "wrong version of sun.security.provider.SHA5");
  if (sha_state == NULL) return (Node *) NULL;

  // now have the array, need to get the start address of the state array
  Node* state = array_element_address(sha_state, intcon(0), T_LONG);
  return state;
}

//----------------------------inline_digestBase_implCompressMB_predicate----------------------------
// Return node representing slow path of predicate check.
// the pseudo code we want to emulate with this predicate is:
//    if (digestBaseObj instanceof SHA/SHA2/SHA5) do_intrinsic, else do_javapath
//
Node* LibraryCallKit::inline_digestBase_implCompressMB_predicate(int predicate) {
  assert(UseSHA1Intrinsics || UseSHA256Intrinsics || UseSHA512Intrinsics,
         "need SHA1/SHA256/SHA512 instruction support");
  assert((uint)predicate < 3, "sanity");

  // The receiver was checked for NULL already.
  Node* digestBaseObj = argument(0);

  // get DigestBase klass for instanceOf check
  const TypeInstPtr* tinst = _gvn.type(digestBaseObj)->isa_instptr();
  assert(tinst != NULL, "digestBaseObj is null");
  assert(tinst->klass()->is_loaded(), "DigestBase is not loaded");

  const char* klass_SHA_name = NULL;
  switch (predicate) {
  case 0:
    if (UseSHA1Intrinsics) {
      // we want to do an instanceof comparison against the SHA class
      klass_SHA_name = "sun/security/provider/SHA";
    }
    break;
  case 1:
    if (UseSHA256Intrinsics) {
      // we want to do an instanceof comparison against the SHA2 class
      klass_SHA_name = "sun/security/provider/SHA2";
    }
    break;
  case 2:
    if (UseSHA512Intrinsics) {
      // we want to do an instanceof comparison against the SHA5 class
      klass_SHA_name = "sun/security/provider/SHA5";
    }
    break;
  default:
    fatal("unknown SHA intrinsic predicate: %d", predicate);
  }

  ciKlass* klass_SHA = NULL;
  if (klass_SHA_name != NULL) {
    klass_SHA = tinst->klass()->as_instance_klass()->find_klass(ciSymbol::make(klass_SHA_name));
  }
  if ((klass_SHA == NULL) || !klass_SHA->is_loaded()) {
    // if none of SHA/SHA2/SHA5 is loaded, we never take the intrinsic fast path
    Node* ctrl = control();
    set_control(top()); // no intrinsic path
    return ctrl;
  }
  ciInstanceKlass* instklass_SHA = klass_SHA->as_instance_klass();

  Node* instofSHA = gen_instanceof(digestBaseObj, makecon(TypeKlassPtr::make(instklass_SHA)));
  Node* cmp_instof = _gvn.transform(new CmpINode(instofSHA, intcon(1)));
  Node* bool_instof = _gvn.transform(new BoolNode(cmp_instof, BoolTest::ne));
  Node* instof_false = generate_guard(bool_instof, NULL, PROB_MIN);

  return instof_false;  // even if it is NULL
}

//-------------inline_fma-----------------------------------
bool LibraryCallKit::inline_fma(vmIntrinsics::ID id) {
  Node *a = NULL;
  Node *b = NULL;
  Node *c = NULL;
  Node* result = NULL;
  switch (id) {
  case vmIntrinsics::_fmaD:
    assert(callee()->signature()->size() == 6, "fma has 3 parameters of size 2 each.");
    // no receiver since it is static method
    a = round_double_node(argument(0));
    b = round_double_node(argument(2));
    c = round_double_node(argument(4));
    result = _gvn.transform(new FmaDNode(control(), a, b, c));
    break;
  case vmIntrinsics::_fmaF:
    assert(callee()->signature()->size() == 3, "fma has 3 parameters of size 1 each.");
    a = argument(0);
    b = argument(1);
    c = argument(2);
    result = _gvn.transform(new FmaFNode(control(), a, b, c));
    break;
  default:
    fatal_unexpected_iid(id);  break;
  }
  set_result(result);
  return true;
}

bool LibraryCallKit::inline_character_compare(vmIntrinsics::ID id) {
  // argument(0) is receiver
  Node* codePoint = argument(1);
  Node* n = NULL;

  switch (id) {
    case vmIntrinsics::_isDigit :
      n = new DigitNode(control(), codePoint);
      break;
    case vmIntrinsics::_isLowerCase :
      n = new LowerCaseNode(control(), codePoint);
      break;
    case vmIntrinsics::_isUpperCase :
      n = new UpperCaseNode(control(), codePoint);
      break;
    case vmIntrinsics::_isWhitespace :
      n = new WhitespaceNode(control(), codePoint);
      break;
    default:
      fatal_unexpected_iid(id);
  }

  set_result(_gvn.transform(n));
  return true;
}

//------------------------------inline_fp_min_max------------------------------
bool LibraryCallKit::inline_fp_min_max(vmIntrinsics::ID id) {
/* DISABLED BECAUSE METHOD DATA ISN'T COLLECTED PER CALL-SITE, SEE JDK-8015416.

  // The intrinsic should be used only when the API branches aren't predictable,
  // the last one performing the most important comparison. The following heuristic
  // uses the branch statistics to eventually bail out if necessary.

  ciMethodData *md = callee()->method_data();

  if ( md != NULL && md->is_mature() && md->invocation_count() > 0 ) {
    ciCallProfile cp = caller()->call_profile_at_bci(bci());

    if ( ((double)cp.count()) / ((double)md->invocation_count()) < 0.8 ) {
      // Bail out if the call-site didn't contribute enough to the statistics.
      return false;
    }

    uint taken = 0, not_taken = 0;

    for (ciProfileData *p = md->first_data(); md->is_valid(p); p = md->next_data(p)) {
      if (p->is_BranchData()) {
        taken = ((ciBranchData*)p)->taken();
        not_taken = ((ciBranchData*)p)->not_taken();
      }
    }

    double balance = (((double)taken) - ((double)not_taken)) / ((double)md->invocation_count());
    balance = balance < 0 ? -balance : balance;
    if ( balance > 0.2 ) {
      // Bail out if the most important branch is predictable enough.
      return false;
    }
  }
*/

  Node *a = NULL;
  Node *b = NULL;
  Node *n = NULL;
  switch (id) {
  case vmIntrinsics::_maxF:
  case vmIntrinsics::_minF:
    assert(callee()->signature()->size() == 2, "minF/maxF has 2 parameters of size 1 each.");
    a = argument(0);
    b = argument(1);
    break;
  case vmIntrinsics::_maxD:
  case vmIntrinsics::_minD:
    assert(callee()->signature()->size() == 4, "minD/maxD has 2 parameters of size 2 each.");
    a = round_double_node(argument(0));
    b = round_double_node(argument(2));
    break;
  default:
    fatal_unexpected_iid(id);
    break;
  }
  switch (id) {
  case vmIntrinsics::_maxF:  n = new MaxFNode(a, b);  break;
  case vmIntrinsics::_minF:  n = new MinFNode(a, b);  break;
  case vmIntrinsics::_maxD:  n = new MaxDNode(a, b);  break;
  case vmIntrinsics::_minD:  n = new MinDNode(a, b);  break;
  default:  fatal_unexpected_iid(id);  break;
  }
  set_result(_gvn.transform(n));
  return true;
}

bool LibraryCallKit::inline_profileBoolean() {
  Node* counts = argument(1);
  const TypeAryPtr* ary = NULL;
  ciArray* aobj = NULL;
  if (counts->is_Con()
      && (ary = counts->bottom_type()->isa_aryptr()) != NULL
      && (aobj = ary->const_oop()->as_array()) != NULL
      && (aobj->length() == 2)) {
    // Profile is int[2] where [0] and [1] correspond to false and true value occurrences respectively.
    jint false_cnt = aobj->element_value(0).as_int();
    jint  true_cnt = aobj->element_value(1).as_int();

    if (C->log() != NULL) {
      C->log()->elem("observe source='profileBoolean' false='%d' true='%d'",
                     false_cnt, true_cnt);
    }

    if (false_cnt + true_cnt == 0) {
      // According to profile, never executed.
      uncommon_trap_exact(Deoptimization::Reason_intrinsic,
                          Deoptimization::Action_reinterpret);
      return true;
    }

    // result is a boolean (0 or 1) and its profile (false_cnt & true_cnt)
    // is a number of each value occurrences.
    Node* result = argument(0);
    if (false_cnt == 0 || true_cnt == 0) {
      // According to profile, one value has been never seen.
      int expected_val = (false_cnt == 0) ? 1 : 0;

      Node* cmp  = _gvn.transform(new CmpINode(result, intcon(expected_val)));
      Node* test = _gvn.transform(new BoolNode(cmp, BoolTest::eq));

      IfNode* check = create_and_map_if(control(), test, PROB_ALWAYS, COUNT_UNKNOWN);
      Node* fast_path = _gvn.transform(new IfTrueNode(check));
      Node* slow_path = _gvn.transform(new IfFalseNode(check));

      { // Slow path: uncommon trap for never seen value and then reexecute
        // MethodHandleImpl::profileBoolean() to bump the count, so JIT knows
        // the value has been seen at least once.
        PreserveJVMState pjvms(this);
        PreserveReexecuteState preexecs(this);
        jvms()->set_should_reexecute(true);

        set_control(slow_path);
        set_i_o(i_o());

        uncommon_trap_exact(Deoptimization::Reason_intrinsic,
                            Deoptimization::Action_reinterpret);
      }
      // The guard for never seen value enables sharpening of the result and
      // returning a constant. It allows to eliminate branches on the same value
      // later on.
      set_control(fast_path);
      result = intcon(expected_val);
    }
    // Stop profiling.
    // MethodHandleImpl::profileBoolean() has profiling logic in its bytecode.
    // By replacing method body with profile data (represented as ProfileBooleanNode
    // on IR level) we effectively disable profiling.
    // It enables full speed execution once optimized code is generated.
    Node* profile = _gvn.transform(new ProfileBooleanNode(result, false_cnt, true_cnt));
    C->record_for_igvn(profile);
    set_result(profile);
    return true;
  } else {
    // Continue profiling.
    // Profile data isn't available at the moment. So, execute method's bytecode version.
    // Usually, when GWT LambdaForms are profiled it means that a stand-alone nmethod
    // is compiled and counters aren't available since corresponding MethodHandle
    // isn't a compile-time constant.
    return false;
  }
}

bool LibraryCallKit::inline_isCompileConstant() {
  Node* n = argument(0);
  set_result(n->is_Con() ? intcon(1) : intcon(0));
  return true;
}
