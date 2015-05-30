/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 EcmaXp
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/** ref: ../java/org/micropython/jnupy/PythonState.java **/
// TODO: Support JSR 223? (not in jnupy.c)

/** JNLUA-LICENSE
Copyright (C) 2008,2012 Andre Naef

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
**/

#include <jni.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/runtime0.h"
#include "py/objstr.h"
#include "py/builtin.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/bc.h"
#include "py/cpuctrl.h"
#include "py/stackctrl.h"
#include "py/statectrl.h"
#include "py/objmodule.h"
#include "modmicrothread.h"
#include "genhdr/mpversion.h"

/** BUILD LIMITER **/
#if !MICROPY_MULTI_STATE_CONTEXT
#error jnupy require MICROPY_MULTI_STATE_CONTEXT
#endif

#if !MICROPY_ALLOW_PAUSE_VM
#error jnupy require MICROPY_ALLOW_PAUSE_VM
#endif

#if !MICROPY_OVERRIDE_ASSERT_FAIL
#error jnupy require MICROPY_OVERRIDE_ASSERT_FAIL
#endif

#if !MICROPY_LIMIT_CPU
#error jnupy require MICROPY_LIMIT_CPU
#endif

#if !MICROPY_ENABLE_GC
#error jnupy require MICROPY_ENABLE_GC
#endif

// and other limiter require for building.

/** Legacy Code **/ // TODO: remove this block
/*
TODO: should i make many functions for
    - make / control state
    - convert value (msgpack method or other method)
      (and it should support userdata, or native micropython ptr?)
    - execute source from file or buffer
    - handle assert failure or nlr_raise
    - use coffeecatch library or other signal capture handler?
    - safe warpper or sandbox for something.
*/
/*

STATIC mp_obj_t get_executor(mp_obj_t module_fun) {
    mp_obj_t module_mt = mp_module_get(MP_QSTR_umicrothread);
    mp_obj_t thread = mp_call_function_2(mp_load_attr(module_mt, MP_QSTR_MicroThread), MP_OBJ_NEW_QSTR(MP_QSTR_module), module_fun);
    return thread;
}

STATIC bool execute(mp_state_ctx_t *state, mp_obj_t thread) {
    // TODO: handle error?

    mp_state_load(state);

    bool continue_execute = true;
    mp_obj_t result;
    mp_mrk_t kind = microthread_resume(thread, mp_const_none, &result);

    switch (kind) {
        case MP_MRK_STOP:
            continue_execute = false;
            break;
        case MP_MRK_EXCEPTION:
            handle_uncaught_exception(result);
        default:
            continue_execute = true;
    }

    mp_state_store(state);
    return continue_execute;
}
*/

/** JNUPY INFO **/
#define JNUPY_JNIVERSION JNI_VERSION_1_6

/** JNUPY INTERNAL VALUE **/
STATIC int initialized = 0;

typedef struct _nlr_gk_buf_t {
    bool is_working;
    struct _nlr_gk_buf_t *prev;
    nlr_buf_t buf;
} nlr_gk_buf_t;

typedef struct _jnupy_current_state_t {
    // JNUPY_MP_STATE
    // micropython state context
    mp_state_ctx_t *mp_state;
    
    // JNUPY_STACK_TOP
    // micropython stack top
    // TODO: use this?
    mp_uint_t *stack_top;
    
    // NLR_GK_TOP
    // nlr goalkeeper top (for JNI function call warpper)
    nlr_gk_buf_t *nlr_gk_top;

    // JNUPY_ENV
    // java env (vaild only current thread)
    JNIEnv *java_env;

    // JNUPY_SELF
    // java self (vaild only current thread)
    jobject java_self;

    // JNUPY_PY_STATE
    // java PythonState (vaild only current thread)
    jobject java_pystate;
} jnupy_current_state_t;

// _JNUPY_CUR_STATE(x)
STATIC MP_THREAD jnupy_current_state_t jnupy_cur_state;

// JNUPY_G_VM
STATIC JavaVM *jnupy_glob_java_vm;

/** JNUPY MECRO **/
#if DEBUG
#define _D(x) printf(#x "\n")
#else
#define _D(x) (void)0
#endif

#define _JNUPY_CUR_STATE(x) (jnupy_cur_state.x)
#define JNUPY_G_VM jnupy_glob_java_vm
#define JNUPY_ENV _JNUPY_CUR_STATE(java_env)
#define JNUPY_SELF _JNUPY_CUR_STATE(java_self)
#define JNUPY_MP_STATE _JNUPY_CUR_STATE(mp_state)
#define JNUPY_STACK_TOP _JNUPY_CUR_STATE(stack_top)
#define JNUPY_PY_JSTATE _JNUPY_CUR_STATE(java_pystate)

/** JNUPY CALL MECRO **/
#define JNUPY_RAW_CALL_WITH(env, func, ...) (*env)->func(env, __VA_ARGS__)
#define JNUPY_RAW_CALL(func, ...) (*JNUPY_ENV)->func(JNUPY_ENV, __VA_ARGS__)
#define JNUPY_IS_RAW_CALL_HAS_ERROR() (*JNUPY_ENV)->ExceptionOccurred(JNUPY_ENV)
#define JNUPY_RAW_AUTO_THROW (JNUPY_IS_RAW_CALL_HAS_ERROR()? nlr_gk_jump(NULL): (void)0)
#define JNUPY_CALL(func, ...) (*JNUPY_ENV)->func(JNUPY_ENV, __VA_ARGS__); JNUPY_RAW_AUTO_THROW
/*
JNUPY_CALL usage:

// With getting value
jobject msg = JNUPY_CALL(GetLongField, JNUPY_SELF, JFIELD(PythonState, mpState));

// With ignore value
JNUPY_CALL(NewGlobalRef, jfunc);

// It can't mix with if, for, while, return, or etc control block.
// But you can use JNUPY_RAW_CALL, and after call, must check by JNUPY_IS_RAW_CALL_HAS_ERROR()
if (JNUPY_RAW_CALL(ThrowNew, JNUPY_CLASS("java/lang/AssertionError", CM4H2), "There is no state") == 0) {
    if (JNUPY_IS_RAW_CALL_HAS_ERROR()) {
        return;
    }
}
assret(! "throwing is failed.");
*/

#define NLR_GK_TOP _JNUPY_CUR_STATE(nlr_gk_top)

/** JNUPY NLR GOAL KEEPER **/
// nlr goal keeper are wapper for java throw in nested function call.
// like assert fail.
// TODO: nlr_gk are currently getting heavy bug... fix them......

#define nlr_gk_set_buf(gk_buf) nlr_gk_set_buf_raw(gk_buf)
#define nlr_gk_new() {false}
#define nlr_gk_push(gk_buf) (nlr_gk_push_raw(gk_buf), nlr_push(&(gk_buf)->buf))
#define nlr_gk_pop(gk_buf) (nlr_gk_pop_raw(gk_buf))
#define nlr_gk_jump(val) (nlr_gk_jump_raw(val))

void nlr_gk_set_buf_raw(nlr_gk_buf_t *gk_buf) {
    if (gk_buf == NULL) {
        // mp_nlr_top = NULL;
    } else {
        mp_nlr_top = &gk_buf->buf;
    }
}

void nlr_gk_push_raw(nlr_gk_buf_t *gk_buf) {
    gk_buf->is_working = true;
    
    gk_buf->prev = NLR_GK_TOP;
    NLR_GK_TOP = gk_buf;
}

void nlr_gk_pop_raw(nlr_gk_buf_t *gk_buf) {
    if (gk_buf->is_working) {
        NLR_GK_TOP = NLR_GK_TOP->prev;
        nlr_gk_set_buf(NLR_GK_TOP);
    }
}

NORETURN void nlr_gk_jump_raw(void *val) {
    NLR_GK_TOP->is_working = false;
    
    nlr_gk_set_buf(NLR_GK_TOP);
    NLR_GK_TOP = NLR_GK_TOP->prev;
    
    nlr_jump(val);
}

/** JNI CLASS/VALUE REFERENCE MECRO **/
#define _JNUPY_REF_ID(hash) _jnupy_REF_##hash

#define JNUPY_CLASS(name, id) _JNUPY_REF_ID(id)
#define JNUPY_METHOD(class_, name, type, id) _JNUPY_REF_ID(id)
#define JNUPY_FIELD(class_, name, type, id) _JNUPY_REF_ID(id)
#define JNUPY_STATICMETHOD(class_, name, type, id) _JNUPY_REF_ID(id)
#define JNUPY_STATICFIELD(class_, name, type, id) _JNUPY_REF_ID(id)
#define JNUPY_ANY(id) _JNUPY_REF_ID(id)

#define _JNUPY_REF(vtype, id, default) STATIC vtype _JNUPY_REF_ID(id) = default;
#define JNUPY_REF_CLASS(id) _JNUPY_REF(jclass, id, NULL)
#define JNUPY_REF_METHOD(id) _JNUPY_REF(jmethodID, id, 0)
#define JNUPY_REF_FIELD(id) _JNUPY_REF(jfieldID, id, 0)
#define JNUPY_REF_STATICMETHOD(id) JNUPY_REF_METHOD(id)
#define JNUPY_REF_STATICFIELD(id) JNUPY_REF_FIELD(id)
#define JNUPY_REF_ANY(type, id, default) _JNUPY_REF(type, id, default)

#define _JNUPY_LOAD(id, value) if (!(_JNUPY_REF_ID(id) = (value))) break;
#define JNUPY_LOAD_CLASS(name, id) \
    _JNUPY_LOAD(id, jnupy_refclass(name))
#define JNUPY_LOAD_METHOD(clsname, name, type, clsid, id) \
    _JNUPY_LOAD(id, JNUPY_RAW_CALL(GetMethodID, _JNUPY_REF_ID(clsid), name, type))
#define JNUPY_LOAD_FIELD(clsname, name, type, clsid, id) \
    _JNUPY_LOAD(id, JNUPY_RAW_CALL(GetFieldID, _JNUPY_REF_ID(clsid), name, type))
#define JNUPY_LOAD_STATICMETHOD(clsname, name, type, clsid, id) \
    _JNUPY_LOAD(id, JNUPY_RAW_CALL(GetStaticMethodID, _JNUPY_REF_ID(clsid), name, type))
#define JNUPY_LOAD_STATICFIELD(clsname, name, type, clsid, id) \
    _JNUPY_LOAD(id, JNUPY_RAW_CALL(GetStaticFieldID, _JNUPY_REF_ID(clsid), name, type))
#define JNUPY_LOAD_ANY(id, stmt) \
    _JNUPY_LOAD(id, stmt)

#define _JNUPY_UNLOAD(id, value) _JNUPY_REF_ID(id) = value;
#define JNUPY_UNLOAD_CLASS(name, id) \
    _JNUPY_UNLOAD(id, ((_JNUPY_REF_ID(id) != NULL? JNUPY_RAW_CALL(DeleteGlobalRef, _JNUPY_REF_ID(id)): (void)0), NULL))
#define JNUPY_UNLOAD_METHOD(clsname, name, type, clsid, id) \
    _JNUPY_UNLOAD(id, 0)
#define JNUPY_UNLOAD_FIELD(clsname, name, type, clsid, id) \
    _JNUPY_UNLOAD(id, 0)
#define JNUPY_UNLOAD_STATICMETHOD(clsname, name, type, clsid, id) \
    _JNUPY_UNLOAD(id, 0)
#define JNUPY_UNLOAD_STATICFIELD(clsname, name, type, clsid, id) \
    _JNUPY_UNLOAD(id, 0)
#define JNUPY_UNLOAD_ANY(id, stmt, value) \
    stmt; _JNUPY_UNLOAD(id, value)

/** JNUPY AUTO PARSER MECRO **/
#define JNUPY_AP(...)

/** JNI CLASS/VALUE AUTO REFERENCE **/
JNUPY_AP(REF, START)
// CLASS: java/io/ByteArrayInputStream
JNUPY_REF_CLASS(CPYWG)
// CLASS: java/io/ByteArrayOutputStream
JNUPY_REF_CLASS(CPITF)
// CLASS: java/lang/AssertionError
JNUPY_REF_CLASS(CM4H2)
// CLASS: java/lang/Boolean
JNUPY_REF_CLASS(CDKHI)
// CLASS: java/lang/Double
JNUPY_REF_CLASS(CMCKJ)
// CLASS: java/lang/Float
JNUPY_REF_CLASS(CLJBD)
// CLASS: java/lang/Integer
JNUPY_REF_CLASS(CTOBT)
// CLASS: java/lang/Long
JNUPY_REF_CLASS(CJACF)
// CLASS: java/lang/Object
JNUPY_REF_CLASS(CVNFN)
// CLASS: java/lang/String
JNUPY_REF_CLASS(CCHCW)
// CLASS: org/micropython/jnupy/JavaFunction
JNUPY_REF_CLASS(CRBZE)
// CLASS: org/micropython/jnupy/PythonObject
JNUPY_REF_CLASS(CKZG7)
// CLASS: org/micropython/jnupy/PythonState
JNUPY_REF_CLASS(C4SDY)
// FIELD: org/micropython/jnupy/PythonObject->mpObject[J]
JNUPY_REF_FIELD(FG57O)
// FIELD: org/micropython/jnupy/PythonObject->pythonState[Lorg/micropython/jnupy/PythonState;]
JNUPY_REF_FIELD(F4HBX)
// FIELD: org/micropython/jnupy/PythonState->mpState[J]
JNUPY_REF_FIELD(F3VA2)
// METHOD: java/io/ByteArrayInputStream-><init>[([B)V]
JNUPY_REF_METHOD(MUZ6M)
// METHOD: java/io/ByteArrayOutputStream->toByteArray[()[B]
JNUPY_REF_METHOD(MHUAS)
// METHOD: java/lang/Boolean->booleanValue[()Z]
JNUPY_REF_METHOD(ME2HS)
// METHOD: java/lang/Double-><init>[(D)V]
JNUPY_REF_METHOD(MONM4)
// METHOD: java/lang/Double->doubleValue[()D]
JNUPY_REF_METHOD(MRBT7)
// METHOD: java/lang/Double->floatValue[()F]
JNUPY_REF_METHOD(MTEKP)
// METHOD: java/lang/Float-><init>[(F)V]
JNUPY_REF_METHOD(MT3CM)
// METHOD: java/lang/Float->floatValue[()F]
JNUPY_REF_METHOD(MAHUY)
// METHOD: java/lang/Integer-><init>[(I)V]
JNUPY_REF_METHOD(MMSNU)
// METHOD: java/lang/Integer->intValue[()I]
JNUPY_REF_METHOD(MIDRV)
// METHOD: java/lang/Long->longValue[()J]
JNUPY_REF_METHOD(ME7YL)
// METHOD: java/lang/String-><init>[([BIILjava/lang/String;)V]
JNUPY_REF_METHOD(MT7JN)
// METHOD: java/lang/String->getBytes[(Ljava/lang/String;)[B]
JNUPY_REF_METHOD(MNONY)
// METHOD: org/micropython/jnupy/JavaFunction->invoke[(Lorg/micropython/jnupy/PythonState;[Ljava/lang/Object;)Ljava/lang/Object;]
JNUPY_REF_METHOD(MEFVT)
// METHOD: org/micropython/jnupy/PythonObject-><init>[(Lorg/micropython/jnupy/PythonState;JJ)V]
JNUPY_REF_METHOD(MHTAY)
// STATICFIELD: java/lang/Boolean->FALSE[Ljava/lang/Boolean;]
JNUPY_REF_STATICFIELD(SYCJ2)
// STATICFIELD: java/lang/Boolean->TRUE[Ljava/lang/Boolean;]
JNUPY_REF_STATICFIELD(S3RTH)
JNUPY_AP(REF, END)

/** JNI CLASS/VALUE NON-AUTO ANY REFERENCE **/
JNUPY_REF_ANY(jstring, RUTF8, NULL)

JNUPY_AP(EXPORT)

/** JNI CLASS/VALUE MANUAL REFERENCE MECRO **/
#define JCLASS(x) JCLASS_##x
#define JFIELD(x, y) ((void)JCLASS(x), JFIELD_##x##_##y)
#define JMETHOD(x, y) ((void)JCLASS(x), JMETHOD_##x##_##y)
#define JMETHODV(x, y, z) ((void)JCLASS(x), JMETHOD_##x##_##y##_##z)
#define JSTATICFIELD(x, y) ((void)JCLASS(x), JSTATICFIELD_##x##_##y)
#define JSTATICMETHOD(x, y) ((void)JCLASS(x), JSTATICMETHOD_##x##_##y)
#define JOBJECT(x) JOBJECT_##x
#define JANY(x) JNUPY_ANY(x)

/** JNI CLASS/VALUE MANUAL REFERENCE **/
#define JCLASS_Boolean JNUPY_CLASS("java/lang/Boolean", CDKHI)
#define JMETHOD_Boolean_booleanValue JNUPY_METHOD("java/lang/Boolean", "booleanValue", "()Z", ME2HS)
#define JSTATICFIELD_Boolean_TRUE JNUPY_STATICFIELD("java/lang/Boolean", "TRUE", "Ljava/lang/Boolean;", S3RTH)
#define JSTATICFIELD_Boolean_FALSE JNUPY_STATICFIELD("java/lang/Boolean", "FALSE", "Ljava/lang/Boolean;", SYCJ2)

#define JCLASS_PythonState JNUPY_CLASS("org/micropython/jnupy/PythonState", C4SDY)
#define JFIELD_PythonState_mpState JNUPY_FIELD("org/micropython/jnupy/PythonState", "mpState", "J", F3VA2)

#define JCLASS_PythonObject JNUPY_CLASS("org/micropython/jnupy/PythonObject", CKZG7)
#define JMETHOD_PythonObject_INIT JNUPY_METHOD("org/micropython/jnupy/PythonObject", "<init>", "(Lorg/micropython/jnupy/PythonState;JJ)V", MHTAY)
#define JFIELD_PythonObject_mpObject JNUPY_FIELD("org/micropython/jnupy/PythonObject", "mpObject", "J", FG57O)
#define JFIELD_PythonObject_pythonState JNUPY_FIELD("org/micropython/jnupy/PythonObject", "pythonState", "Lorg/micropython/jnupy/PythonState;", F4HBX)

#define JCLASS_JavaFunction JNUPY_CLASS("org/micropython/jnupy/JavaFunction", CRBZE)
#define JMETHOD_JavaFunction_invoke JNUPY_METHOD("org/micropython/jnupy/JavaFunction", "invoke", "(Lorg/micropython/jnupy/PythonState;[Ljava/lang/Object;)Ljava/lang/Object;", MEFVT)

#define JCLASS_Integer JNUPY_CLASS("java/lang/Integer", CTOBT)
#define JMETHOD_Integer_INIT JNUPY_METHOD("java/lang/Integer", "<init>", "(I)V", MMSNU)
#define JMETHOD_Integer_intValue JNUPY_METHOD("java/lang/Integer", "intValue", "()I", MIDRV)

#define JCLASS_Long JNUPY_CLASS("java/lang/Long", CJACF)
#define JMETHOD_Long_longValue JNUPY_METHOD("java/lang/Long", "longValue", "()J", ME7YL)

#define JCLASS_Float JNUPY_CLASS("java/lang/Float", CLJBD)
#define JMETHOD_Float_INIT JNUPY_METHOD("java/lang/Float", "<init>", "(F)V", MT3CM)
#define JMETHOD_Float_floatValue JNUPY_METHOD("java/lang/Float", "floatValue", "()F", MAHUY)

#define JCLASS_Double JNUPY_CLASS("java/lang/Double", CMCKJ)
// java.lang.Double can't convert from float... WHAT??
#define JMETHOD_Double_INIT JNUPY_METHOD("java/lang/Double", "<init>", "(D)V", MONM4)
#define JMETHOD_Double_doubleValue JNUPY_METHOD("java/lang/Double", "doubleValue", "()D", MRBT7)
#define JMETHOD_Double_floatValue JNUPY_METHOD("java/lang/Double", "floatValue", "()F", MTEKP)

#define JCLASS_String JNUPY_CLASS("java/lang/String", CCHCW)
#define JMETHOD_String_INIT_str JNUPY_METHOD("java/lang/String", "<init>", "([BIILjava/lang/String;)V", MT7JN)
#define JMETHOD_String_getBytes JNUPY_METHOD("java/lang/String", "getBytes", "(Ljava/lang/String;)[B", MNONY)

#define JCLASS_ByteArrayInputStream JNUPY_CLASS("java/io/ByteArrayInputStream", CPYWG)
#define JMETHOD_ByteArrayInputStream_INIT JNUPY_METHOD("java/io/ByteArrayInputStream", "<init>", "([B)V", MUZ6M)

#define JCLASS_ByteArrayOutputStream JNUPY_CLASS("java/io/ByteArrayOutputStream", CPITF)
#define JMETHOD_ByteArrayOutputStream_toByteArray JNUPY_METHOD("java/io/ByteArrayOutputStream", "toByteArray", "()[B", MHUAS)

#define JOBJECT_TRUE JNUPY_CALL(GetStaticObjectField, JCLASS(Boolean), JSTATICFIELD(Boolean, TRUE))
#define JOBJECT_FALSE JNUPY_CALL(GetStaticObjectField, JCLASS(Boolean), JSTATICFIELD(Boolean, FALSE))

/** JNI LOAD/UNLOAD FUNCTIONS **/
STATIC jclass jnupy_refclass(const char *className) {
	jclass class_ = JNUPY_RAW_CALL(FindClass, className);
	if (!class_) {
		return NULL;
	}

	return JNUPY_RAW_CALL(NewGlobalRef, class_);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNUPY_G_VM = vm;

    if (JNUPY_RAW_CALL_WITH(JNUPY_G_VM, GetEnv, (void **) &JNUPY_ENV, JNUPY_JNIVERSION) != JNI_OK) {
		return JNUPY_JNIVERSION;
	}

	do {
    // section for load (DO NOT MODIFY)
    JNUPY_AP(LOAD, START)
    JNUPY_LOAD_CLASS("java/io/ByteArrayInputStream", CPYWG)
    JNUPY_LOAD_CLASS("java/io/ByteArrayOutputStream", CPITF)
    JNUPY_LOAD_CLASS("java/lang/AssertionError", CM4H2)
    JNUPY_LOAD_CLASS("java/lang/Boolean", CDKHI)
    JNUPY_LOAD_CLASS("java/lang/Double", CMCKJ)
    JNUPY_LOAD_CLASS("java/lang/Float", CLJBD)
    JNUPY_LOAD_CLASS("java/lang/Integer", CTOBT)
    JNUPY_LOAD_CLASS("java/lang/Long", CJACF)
    JNUPY_LOAD_CLASS("java/lang/Object", CVNFN)
    JNUPY_LOAD_CLASS("java/lang/String", CCHCW)
    JNUPY_LOAD_CLASS("org/micropython/jnupy/JavaFunction", CRBZE)
    JNUPY_LOAD_CLASS("org/micropython/jnupy/PythonObject", CKZG7)
    JNUPY_LOAD_CLASS("org/micropython/jnupy/PythonState", C4SDY)
    JNUPY_LOAD_FIELD("org/micropython/jnupy/PythonObject", "mpObject", "J", CKZG7, FG57O)
    JNUPY_LOAD_FIELD("org/micropython/jnupy/PythonObject", "pythonState", "Lorg/micropython/jnupy/PythonState;", CKZG7, F4HBX)
    JNUPY_LOAD_FIELD("org/micropython/jnupy/PythonState", "mpState", "J", C4SDY, F3VA2)
    JNUPY_LOAD_METHOD("java/io/ByteArrayInputStream", "<init>", "([B)V", CPYWG, MUZ6M)
    JNUPY_LOAD_METHOD("java/io/ByteArrayOutputStream", "toByteArray", "()[B", CPITF, MHUAS)
    JNUPY_LOAD_METHOD("java/lang/Boolean", "booleanValue", "()Z", CDKHI, ME2HS)
    JNUPY_LOAD_METHOD("java/lang/Double", "<init>", "(D)V", CMCKJ, MONM4)
    JNUPY_LOAD_METHOD("java/lang/Double", "doubleValue", "()D", CMCKJ, MRBT7)
    JNUPY_LOAD_METHOD("java/lang/Double", "floatValue", "()F", CMCKJ, MTEKP)
    JNUPY_LOAD_METHOD("java/lang/Float", "<init>", "(F)V", CLJBD, MT3CM)
    JNUPY_LOAD_METHOD("java/lang/Float", "floatValue", "()F", CLJBD, MAHUY)
    JNUPY_LOAD_METHOD("java/lang/Integer", "<init>", "(I)V", CTOBT, MMSNU)
    JNUPY_LOAD_METHOD("java/lang/Integer", "intValue", "()I", CTOBT, MIDRV)
    JNUPY_LOAD_METHOD("java/lang/Long", "longValue", "()J", CJACF, ME7YL)
    JNUPY_LOAD_METHOD("java/lang/String", "<init>", "([BIILjava/lang/String;)V", CCHCW, MT7JN)
    JNUPY_LOAD_METHOD("java/lang/String", "getBytes", "(Ljava/lang/String;)[B", CCHCW, MNONY)
    JNUPY_LOAD_METHOD("org/micropython/jnupy/JavaFunction", "invoke", "(Lorg/micropython/jnupy/PythonState;[Ljava/lang/Object;)Ljava/lang/Object;", CRBZE, MEFVT)
    JNUPY_LOAD_METHOD("org/micropython/jnupy/PythonObject", "<init>", "(Lorg/micropython/jnupy/PythonState;JJ)V", CKZG7, MHTAY)
    JNUPY_LOAD_STATICFIELD("java/lang/Boolean", "FALSE", "Ljava/lang/Boolean;", CDKHI, SYCJ2)
    JNUPY_LOAD_STATICFIELD("java/lang/Boolean", "TRUE", "Ljava/lang/Boolean;", CDKHI, S3RTH)
	JNUPY_AP(LOAD, END)

	// section for load jany
	JNUPY_LOAD_ANY(RUTF8, JNUPY_RAW_CALL(NewStringUTF, "utf-8"))
	// Question: NewStringUTF require NewGlobalRef?

	initialized = 1;
	} while (false);

	return JNUPY_JNIVERSION;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    JNUPY_G_VM = vm;

	if (JNUPY_RAW_CALL_WITH(JNUPY_G_VM, GetEnv, (void **) &JNUPY_ENV, JNUPY_JNIVERSION) != JNI_OK) {
		return;
	}

	initialized = 0;

	do {
    // section for unload (DO NOT MODIFY)
    JNUPY_AP(UNLOAD, START)
    JNUPY_UNLOAD_CLASS("java/io/ByteArrayInputStream", CPYWG)
    JNUPY_UNLOAD_CLASS("java/io/ByteArrayOutputStream", CPITF)
    JNUPY_UNLOAD_CLASS("java/lang/AssertionError", CM4H2)
    JNUPY_UNLOAD_CLASS("java/lang/Boolean", CDKHI)
    JNUPY_UNLOAD_CLASS("java/lang/Double", CMCKJ)
    JNUPY_UNLOAD_CLASS("java/lang/Float", CLJBD)
    JNUPY_UNLOAD_CLASS("java/lang/Integer", CTOBT)
    JNUPY_UNLOAD_CLASS("java/lang/Long", CJACF)
    JNUPY_UNLOAD_CLASS("java/lang/Object", CVNFN)
    JNUPY_UNLOAD_CLASS("java/lang/String", CCHCW)
    JNUPY_UNLOAD_CLASS("org/micropython/jnupy/JavaFunction", CRBZE)
    JNUPY_UNLOAD_CLASS("org/micropython/jnupy/PythonObject", CKZG7)
    JNUPY_UNLOAD_CLASS("org/micropython/jnupy/PythonState", C4SDY)
    JNUPY_UNLOAD_FIELD("org/micropython/jnupy/PythonObject", "mpObject", "J", CKZG7, FG57O)
    JNUPY_UNLOAD_FIELD("org/micropython/jnupy/PythonObject", "pythonState", "Lorg/micropython/jnupy/PythonState;", CKZG7, F4HBX)
    JNUPY_UNLOAD_FIELD("org/micropython/jnupy/PythonState", "mpState", "J", C4SDY, F3VA2)
    JNUPY_UNLOAD_METHOD("java/io/ByteArrayInputStream", "<init>", "([B)V", CPYWG, MUZ6M)
    JNUPY_UNLOAD_METHOD("java/io/ByteArrayOutputStream", "toByteArray", "()[B", CPITF, MHUAS)
    JNUPY_UNLOAD_METHOD("java/lang/Boolean", "booleanValue", "()Z", CDKHI, ME2HS)
    JNUPY_UNLOAD_METHOD("java/lang/Double", "<init>", "(D)V", CMCKJ, MONM4)
    JNUPY_UNLOAD_METHOD("java/lang/Double", "doubleValue", "()D", CMCKJ, MRBT7)
    JNUPY_UNLOAD_METHOD("java/lang/Double", "floatValue", "()F", CMCKJ, MTEKP)
    JNUPY_UNLOAD_METHOD("java/lang/Float", "<init>", "(F)V", CLJBD, MT3CM)
    JNUPY_UNLOAD_METHOD("java/lang/Float", "floatValue", "()F", CLJBD, MAHUY)
    JNUPY_UNLOAD_METHOD("java/lang/Integer", "<init>", "(I)V", CTOBT, MMSNU)
    JNUPY_UNLOAD_METHOD("java/lang/Integer", "intValue", "()I", CTOBT, MIDRV)
    JNUPY_UNLOAD_METHOD("java/lang/Long", "longValue", "()J", CJACF, ME7YL)
    JNUPY_UNLOAD_METHOD("java/lang/String", "<init>", "([BIILjava/lang/String;)V", CCHCW, MT7JN)
    JNUPY_UNLOAD_METHOD("java/lang/String", "getBytes", "(Ljava/lang/String;)[B", CCHCW, MNONY)
    JNUPY_UNLOAD_METHOD("org/micropython/jnupy/JavaFunction", "invoke", "(Lorg/micropython/jnupy/PythonState;[Ljava/lang/Object;)Ljava/lang/Object;", CRBZE, MEFVT)
    JNUPY_UNLOAD_METHOD("org/micropython/jnupy/PythonObject", "<init>", "(Lorg/micropython/jnupy/PythonState;JJ)V", CKZG7, MHTAY)
    JNUPY_UNLOAD_STATICFIELD("java/lang/Boolean", "FALSE", "Ljava/lang/Boolean;", CDKHI, SYCJ2)
    JNUPY_UNLOAD_STATICFIELD("java/lang/Boolean", "TRUE", "Ljava/lang/Boolean;", CDKHI, S3RTH)
	JNUPY_AP(UNLOAD, END)

	// section for unload jany
	JNUPY_UNLOAD_ANY(RUTF8, JNUPY_RAW_CALL(ReleaseStringUTFChars, JANY(RUTF8), NULL), 0)

	} while (false);

	return;
}

/** JNUPY INTERNAL FUNCTION **/
bool jnupy_load_state(mp_state_ctx_t *state) {
    if (state == NULL) {
        JNUPY_RAW_CALL(ThrowNew, JNUPY_CLASS("java/lang/AssertionError", CM4H2), "There is no state");

        // just return, it will throw error.
        return false;
    }

    mp_state_force_load(state);
    if (state != mp_state_ctx) {
        JNUPY_RAW_CALL(ThrowNew, JNUPY_CLASS("java/lang/AssertionError", CM4H2), "Invaild Load");
        return false;
    }

    JNUPY_MP_STATE = state;
    return true;
}

mp_state_ctx_t *jnupy_get_state_from_pythonstate(jobject pythonState) {
    jlong pythonStateId = JNUPY_CALL(GetLongField, pythonState, JFIELD(PythonState, mpState));
    return (mp_state_ctx_t *)pythonStateId;
}

bool jnupy_load_state_from_pythonstate() {
    JNUPY_PY_JSTATE = JNUPY_SELF;
    mp_state_ctx_t *state = jnupy_get_state_from_pythonstate(JNUPY_SELF);
    return jnupy_load_state(state);
}

bool jnupy_load_state_from_pythonobject() {
    jobject pythonState = JNUPY_CALL(GetObjectField, JNUPY_SELF, JFIELD(PythonObject, pythonState));
    JNUPY_PY_JSTATE = pythonState;
    mp_state_ctx_t *state = jnupy_get_state_from_pythonstate(pythonState);
    return jnupy_load_state(state);
}

void jnupy_setup_env(JNIEnv *env, jobject self) {
    JNUPY_ENV = env;
    JNUPY_SELF = self;
}

/** UPY INTERNAL MECRO **/
#define _MEM_SIZE_B  (1)
#define _MEM_SIZE_KB (1024)
#define _MEM_SIZE_MB (1024 * 1024)

#define MEM_SIZE(x, y) ((x) * _MEM_SIZE_##y * (BYTES_PER_WORD / 4))

/** UPY INTERNAL VALUE **/
// STATIC uint emit_opt = MP_EMIT_OPT_NONE;

/** PORT IMPL VALUE/FUNCTIONS **/
MP_THREAD mp_uint_t mp_verbose_flag = 0;

uint mp_import_stat(const char *path) {
    // TODO: limit path to internal only?
    // (but custom importer maybe don't this this?)

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return MP_IMPORT_STAT_DIR;
        } else if (S_ISREG(st.st_mode)) {
            return MP_IMPORT_STAT_FILE;
        }
    }
    return MP_IMPORT_STAT_NO_EXIST;
}

int DEBUG_printf(const char *fmt, ...) {
    // TODO: use java output?
    va_list ap;
    va_start(ap, fmt);
    int ret = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return ret;
}

NORETURN void mp_assert_fail(const char *assertion, const char *file,
                             unsigned int line, const char *function) {

    const char *fmt = "<JNUPY>: %s:%u %s: Assertion '%s' failed.";
    size_t buf_size = strlen(fmt) + strlen(file) + strlen(function) + strlen(assertion) + 64;
    char *buf = malloc(buf_size);

    if (buf != NULL) {
        snprintf(buf, buf_size, fmt, file, line, function, assertion);
    } else {
        buf = "<JNUPY> Allocation buffer failed.";
    }

    if (JNUPY_ENV != NULL) {
        JNUPY_RAW_CALL_WITH(JNUPY_G_VM, AttachCurrentThread, (void **) &JNUPY_ENV, NULL);
        JNUPY_RAW_CALL(ThrowNew, JNUPY_CLASS("java/lang/AssertionError", CM4H2), buf);
    } else {
        printf("%s\n", buf);
    }

    nlr_gk_jump(NULL);
}

void nlr_jump_fail(void *val) {
    char buf[128];
    snprintf(buf, sizeof(buf), "<JNUPY>: FATAL: uncaught NLR %p (mp_state_ctx=%p)", val, mp_state_ctx);

    if (JNUPY_ENV != NULL) {
        JNUPY_RAW_CALL_WITH(JNUPY_G_VM, AttachCurrentThread, (void **) &JNUPY_ENV, NULL);
        JNUPY_RAW_CALL(FatalError, buf);
    } else {
        printf("%s\n", buf);
    }

    abort();
}

/** JNUPY INTERNAL MODULE **/

const mp_obj_type_t mp_type_jobject;
mp_obj_t jnupy_jobj_new(jobject jobj);
jobject jnupy_jobj_get(mp_obj_t self_in);

const mp_obj_type_t mp_type_jfunc;
mp_obj_t mp_obj_jfunc_new(jobject jstate, mp_obj_t name, jobject jfunc);
jobject mp_obj_jfunc_get(mp_obj_t self_in);

/* in li.cil.oc.server.machine.Machine.scala ...
TODO: support convert it (jnupy_obj_j2py, jnupy_obj_py2j)
- NULL = None [OK]
- java.lang.Boolean = bool [OK]
- java.lang.Byte = ? => Double.box(arg.doubleValue) [-]
- java.lang.Character = ? => Double.box(arg.toDouble) [-]
- java.lang.Short = int => Double.box(arg.doubleValue) [-]
- java.lang.Integer = int => Double.box(arg.doubleValue) [py2j only]
- java.lang.Long = int => Double.box(arg.doubleValue) [-]
- java.lang.Float = float => Double.box(arg.doubleValue) [OK]
- java.lang.Double = float [invaild convert...]
- java.lang.String = str [OK]
- Array<Byte> = bytes [?: ByteArrayInput/ByteArrayOutputStream only]
- Map<String, String> = dict? [-]
- NBTTagCompound = JObject => arg [-]
*/

jobject jnupy_pyobj_new(jobject pythonState, mp_obj_t pyobj) {
    jobject pyState = JNUPY_CALL(NewGlobalRef, pythonState);
    jobject jobj = JNUPY_CALL(NewObject, JCLASS(PythonObject), JMETHOD(PythonObject, INIT), pyState, (jlong)(void *)JNUPY_MP_STATE, (jlong)(void *)pyobj);

    return jobj;
}

mp_obj_t jnupy_pyobj_get(jobject jobj) {
    assert(JNUPY_RAW_CALL(IsInstanceOf, jobj, JCLASS(PythonObject)) == JNI_TRUE);

    mp_obj_t pyobj = (mp_obj_t)JNUPY_CALL(GetLongField, jobj, JFIELD(PythonObject, mpObject));
    return pyobj;
}

// TODO: check modmsgpack.c will help coding!
#define IsInstanceOf(obj, class_) (JNUPY_RAW_CALL(IsInstanceOf, obj, class_) == JNI_TRUE)
mp_obj_t jnupy_obj_j2py(jobject obj) {
    // TODO: warp handler for java exception
    bool is_str = false, is_bytes = false;

    if (0) {
    } else if (obj == NULL) {
        return mp_const_none;
    } else if (IsInstanceOf(obj, JCLASS(Boolean))) {
        jboolean val = JNUPY_CALL(CallBooleanMethod, obj, JMETHOD(Boolean, booleanValue));

        if (val == JNI_TRUE) {
            return mp_const_true;
        } else if (val == JNI_FALSE) {
            return mp_const_false;
        }

        assert(! "invaild control flow");
    } else if (IsInstanceOf(obj, JCLASS(Integer))) {
        jint val = JNUPY_CALL(CallIntMethod, obj, JMETHOD(Integer, intValue));

        mp_obj_t obj = MP_OBJ_NEW_SMALL_INT(val);
        if (MP_OBJ_SMALL_INT_VALUE(obj) == val) {
            return obj;
        } else {
            return mp_obj_new_int(val);
        }
    } else if (IsInstanceOf(obj, JCLASS(Long))) {
        jlong val = JNUPY_CALL(CallLongMethod, obj, JMETHOD(Long, longValue));

        return mp_obj_new_int_from_ll(val);
    } else if (IsInstanceOf(obj, JCLASS(Float))) {
        jfloat val = JNUPY_CALL(CallFloatMethod, obj, JMETHOD(Float, floatValue));

        return mp_obj_new_float(val);
    } else if (IsInstanceOf(obj, JCLASS(Double))) {
        // TODO: handle Double correct... (invaild value when calling double.doubleValue()...)
        jfloat val = JNUPY_CALL(CallFloatMethod, obj, JMETHOD(Double, floatValue));

        return mp_obj_new_float(val);
    } else if ((is_str = IsInstanceOf(obj, JCLASS(String))) || \
               (is_bytes = IsInstanceOf(obj, JCLASS(ByteArrayOutputStream)))) {
        jarray bytearr = NULL;
        if (is_str) {
            bytearr = (jarray)JNUPY_CALL(CallObjectMethod, obj, JMETHOD(String, getBytes), JANY(RUTF8));
        } else {
            bytearr = (jarray)JNUPY_CALL(CallObjectMethod, obj, JMETHOD(ByteArrayOutputStream, toByteArray));
        }

        jsize arrsize = JNUPY_CALL(GetArrayLength, bytearr);
        jbyte *buf = malloc(arrsize);

        JNUPY_CALL(GetByteArrayRegion, bytearr, 0, arrsize, buf);

        mp_obj_t pobj = MP_OBJ_NULL;
        if (is_str) {
            pobj = mp_obj_new_str((char *)buf, arrsize, false);
        } else {
            pobj = mp_obj_new_bytes((byte *)buf, arrsize);
        }

        JNUPY_CALL(ReleaseByteArrayElements, bytearr, buf, 0);

        return pobj;
    } else if (0) {
        // TODO: handle array
        // (Entry? no way...)
    } else if (0) {
        // TODO: handle dictionary
    } else if (0) {
        // TODO: handle set?
    } else if (IsInstanceOf(obj, JCLASS(PythonObject))) {
        return jnupy_pyobj_get(obj);
    } else {
        return jnupy_jobj_new(obj);
    }

    return mp_const_none;
}
#undef IsInstanceOf

jobject jnupy_obj_py2j(mp_obj_t obj) {
    if (0) {
    } else if (obj == mp_const_none) {
        return NULL;
    } else if (obj == mp_const_true) {
        return JOBJECT(TRUE);
    } else if (obj == mp_const_false) {
        return JOBJECT(FALSE);
    } else if (MP_OBJ_IS_SMALL_INT(obj)) {
        mp_int_t val = MP_OBJ_SMALL_INT_VALUE(obj);

        jobject jobj = JNUPY_CALL(NewObject, JCLASS(Integer), JMETHOD(Integer, INIT), val);
        return jobj;
    } else if (MP_OBJ_IS_INT(obj)) {
        // TODO: handle big num
        mp_int_t val = mp_obj_int_get_truncated(obj);

        jobject jobj = JNUPY_CALL(NewObject, JCLASS(Integer), JMETHOD(Integer, INIT), val);
        return jobj;
    #if MICROPY_PY_BUILTINS_FLOAT
    } else if (MP_OBJ_IS_TYPE(obj, &mp_type_float)) {
        mp_float_t val = mp_obj_get_float(obj);

        jobject jobj = JNUPY_CALL(NewObject, JCLASS(Float), JMETHOD(Float, INIT), (jfloat)val);
        return jobj;
    #endif
    } else if (MP_OBJ_IS_STR_OR_BYTES(obj)) {
        mp_buffer_info_t objbuf;
        mp_get_buffer_raise(obj, &objbuf, MP_BUFFER_READ);

        jbyteArray bytearr = JNUPY_CALL(NewByteArray, objbuf.len);
        JNUPY_CALL(SetByteArrayRegion, bytearr, 0, objbuf.len, objbuf.buf);

        jobject jobj;
        if (MP_OBJ_IS_STR(obj)) {
            jobj = JNUPY_CALL(NewObject, JCLASS(String), JMETHODV(String, INIT, str), bytearr, 0, objbuf.len, JANY(RUTF8));
        } else {
            jobj = JNUPY_CALL(NewObject, JCLASS(ByteArrayInputStream), JMETHOD(ByteArrayInputStream, INIT), bytearr);
        }

        JNUPY_CALL(ReleaseByteArrayElements, bytearr, NULL, JNI_ABORT);
        return jobj;
    } else if (0) {
        // TODO: handle tuple, list
    } else if (0) {
        // TODO: handle dict
    } else if (0) {
        // TODO: handle set?
    } else if (MP_OBJ_IS_TYPE(obj, &mp_type_jobject)) {
        return jnupy_jobj_get(obj);
    } else {
        return jnupy_pyobj_new(JNUPY_PY_JSTATE, obj);
    }

    return NULL;
}

typedef struct _jnupy_jobj_t {
    mp_obj_base_t base;
    jobject jobj;
} jnupy_jobj_t;

mp_obj_t jnupy_jobj_new(jobject jobj) {
    jnupy_jobj_t *o = m_new_obj_with_finaliser(jnupy_jobj_t);
    o->base.type = &mp_type_jobject;
    o->jobj = JNUPY_CALL(NewGlobalRef, jobj);

    return (mp_obj_t)o;
}

jobject jnupy_jobj_get(mp_obj_t self_in) {
    assert(MP_OBJ_IS_TYPE(self_in, &mp_type_jobject));
    jnupy_jobj_t *self = self_in;
    return self->jobj;
}

STATIC void jobject_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind) {
    jobject jobj = jnupy_jobj_get(o_in);

    jclass class_ = JNUPY_CALL(GetObjectClass, jobj);
    jmethodID mid = JNUPY_CALL(GetMethodID, class_, "toString", "()Ljava/lang/String;");
    jstring val = JNUPY_CALL(CallObjectMethod, jobj, mid);
    const char *buf = JNUPY_CALL(GetStringUTFChars, val, NULL);

    if (kind == PRINT_REPR) {
        mp_printf(print, "<JObject %s>", buf);
    } else if (kind == PRINT_STR) {
        mp_print_str(print, buf);
    } else {
        // TODO: replace exception type/message.
        nlr_raise(mp_obj_new_exception_msg(&mp_type_TypeError, "invaild print (jobject_print)"));
    }
}

STATIC mp_obj_t jobject_del(mp_obj_t self_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    jnupy_jobj_t *self = self_in;
    JNUPY_CALL(DeleteGlobalRef, self->jobj);

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(jobject_del_obj, jobject_del);

STATIC const mp_map_elem_t jobject_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___del__), (mp_obj_t)&jobject_del_obj },
};

STATIC MP_DEFINE_CONST_DICT(jobject_locals_dict, jobject_locals_dict_table);

const mp_obj_type_t mp_type_jobject = {
    { &mp_type_type },
    .name = MP_QSTR_JObject,
    .print = jobject_print,
    .locals_dict = (mp_obj_t)&jobject_locals_dict,
};

typedef struct _mp_obj_jfunc_t {
    mp_obj_base_t base;
    mp_obj_t name;
    jobject jstate;
    jobject jfunc;
} mp_obj_jfunc_t;

mp_obj_t mp_obj_jfunc_new(jobject jstate, mp_obj_t name, jobject jfunc) {
    mp_obj_jfunc_t *o = m_new_obj_with_finaliser(mp_obj_jfunc_t);
    o->base.type = &mp_type_jfunc;
    o->name = name;
    o->jstate = JNUPY_CALL(NewGlobalRef, jstate);
    o->jfunc = JNUPY_CALL(NewGlobalRef, jfunc);

    return (mp_obj_t)o;
}

jobject mp_obj_jfunc_get(mp_obj_t self_in) {
    assert(MP_OBJ_IS_TYPE(self_in, &mp_type_jfunc));
    mp_obj_jfunc_t *self = self_in;
    return self->jfunc;
}

STATIC mp_obj_t jfunc_call(mp_obj_t self_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, n_args, false);
    mp_obj_jfunc_t *o = self_in;

    // TODO: if argument convert provided by keyword value, convert by custom converter...
    // TODO: more detail value convert required.
    JNUPY_PY_JSTATE = o->jstate;

    jobjectArray jargs = JNUPY_CALL(NewObjectArray, n_args, JNUPY_CLASS("java/lang/Object", CVNFN), NULL);

    for (int i = 0; i < n_args; i++) {
        JNUPY_CALL(SetObjectArrayElement, jargs, i, jnupy_obj_py2j(args[i]));
    }

	jobject jresult = JNUPY_RAW_CALL(CallObjectMethod, o->jfunc, JMETHOD(JavaFunction, invoke), JNUPY_SELF, jargs);
    jthrowable error = JNUPY_IS_RAW_CALL_HAS_ERROR();
	if (error) {
	    // just throw java error, export to jnupy.
	    // and if error are not captured by python, raise it to java...?
	    // TODO: how to handle JavaError? ...
		nlr_raise(mp_obj_new_exception_msg(&mp_type_RuntimeError, "error raised from java"));
	}

    mp_obj_t pyresult = jnupy_obj_j2py(jresult);

	return pyresult;
}

STATIC void jfunc_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_jfunc_t *self = self_in;

    if (dest[0] != MP_OBJ_NULL) {
        // not load attribute
        return;
    }
    if (attr == MP_QSTR___name__) {
        dest[0] = self->name;
    }
}

#if MICROPY_CPYTHON_COMPAT
STATIC void jfunc_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind) {
    mp_obj_jfunc_t *o = o_in;

    mp_print_str(print, "<JFunction ");
    mp_obj_print_helper(print, o->name, kind);
    mp_print_str(print, ">");
}
#endif

STATIC mp_obj_t jfunc_del(mp_obj_t self_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    mp_obj_jfunc_t *o = self_in;
    JNUPY_CALL(DeleteGlobalRef, o->jstate);
    JNUPY_CALL(DeleteGlobalRef, o->jfunc);

    return mp_const_none;
}

STATIC MP_DEFINE_CONST_FUN_OBJ_1(jfunc_del_obj, jfunc_del);

STATIC const mp_map_elem_t jfunc_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___del__), (mp_obj_t)&jfunc_del_obj },
};

STATIC MP_DEFINE_CONST_DICT(jfunc_locals_dict, jfunc_locals_dict_table);

const mp_obj_type_t mp_type_jfunc = {
    { &mp_type_type },
    .name = MP_QSTR_JFunction,
    .call = jfunc_call,
    .attr = jfunc_attr,
    #if MICROPY_CPYTHON_COMPAT
    .print = jfunc_print,
    #endif
    .locals_dict = (mp_obj_t)&jfunc_locals_dict,
};

STATIC const mp_map_elem_t mp_module_ujnupy_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_micropython) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_JObject), (mp_obj_t)&mp_type_jobject },
    { MP_OBJ_NEW_QSTR(MP_QSTR_JFunction), (mp_obj_t)&mp_type_jfunc },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_ujnupy_globals, mp_module_ujnupy_globals_table);

const mp_obj_module_t mp_module_jnupy = {
    .base = { &mp_type_module },
    .name = MP_QSTR_ujnupy,
    .globals = (mp_obj_dict_t*)&mp_module_ujnupy_globals,
};

/** JNI EXPORT FUNCTION MECRO **/
#define JNUPY_FUNC(name) // Java_org_micropython_jnupy_xxx_##name
#define JNUPY_FUNC_DEF(ret, name) \
    JNIEXPORT ret JNICALL JNUPY_FUNC(name)

// TODO: cleanup body... (nlr_gk, nlr_top, stack_top, etc...)

#define _JNUPY_FUNC_BODY_START(_with_state, init_expr) \
    jnupy_setup_env(env, self); \
    bool with_state = _with_state; \
    void *stack_top = NULL; \
    nlr_buf_t *nlr_ptr = mp_nlr_top; \
    nlr_gk_buf_t _nlr_gk = nlr_gk_new(); \
    if (init_expr) { \
        if (with_state) { \
            stack_top = MP_STATE_VM(stack_top); \
            mp_stack_ctrl_init(); \
        } \
        do { \
            if (nlr_gk_push(&_nlr_gk) != 0) { \
                break; \
            } \

            /* body */

#define _JNUPY_FUNC_BODY_END(ret_stmt) \
        } while(0); \
    } \
    ret_stmt;

#define return \
    if (with_state) { \
        MP_STATE_VM(stack_top) = stack_top; \
    } \
    nlr_gk_pop(&_nlr_gk); \
    mp_nlr_top = nlr_ptr; \
    return

#define JNUPY_FUNC_STATE_LOADER
#define JNUPY_FUNC_START_WITH_STATE _JNUPY_FUNC_BODY_START(true, JNUPY_FUNC_STATE_LOADER())
#define JNUPY_FUNC_START _JNUPY_FUNC_BODY_START(false, true)
#define JNUPY_FUNC_END_VOID _JNUPY_FUNC_BODY_END(return)
#define JNUPY_FUNC_END_VALUE(value) _JNUPY_FUNC_BODY_END(return (value))
#define JNUPY_FUNC_END _JNUPY_FUNC_BODY_END(return 0)

/** JNI EXPORT FUNCTIONS **/

#undef JNUPY_FUNC
#undef JNUPY_FUNC_STATE_LOADER
// org.micropython.jnupy.PythonState
#define JNUPY_FUNC(name) Java_org_micropython_jnupy_PythonState_##name
#define JNUPY_FUNC_STATE_LOADER jnupy_load_state_from_pythonstate

// http://cafe.daum.net/oddtip/JxlJ/27?docid=1CBe5|JxlJ|27|20080424210900&q=java%20jni&srchid=CCB1CBe5|JxlJ|27|20080424210900

JNUPY_FUNC_DEF(void, mp_1test_1jni)
    (JNIEnv *env, jobject self) {
    JNUPY_FUNC_START;

    printf("Welcome to java native micropython! (env=%p; obj=%p;)\n", JNUPY_ENV, JNUPY_SELF);

    JNUPY_FUNC_END_VOID;
}

JNUPY_FUNC_DEF(void, mp_1test_1jni_1fail)
    (JNIEnv *env, jobject self) {
    JNUPY_FUNC_START;

	assert(! "just checking assert work?");

    JNUPY_FUNC_END_VOID;
}

JNUPY_FUNC_DEF(void, mp_1test_1jni_1state)
    (JNIEnv *env, jobject self) {
    JNUPY_FUNC_START_WITH_STATE;

    JNUPY_FUNC_END_VOID;
}

JNUPY_FUNC_DEF(jboolean, mp_1state_1new)
    (JNIEnv *env, jobject self) {
    JNUPY_FUNC_START;

    if (!initialized) {
        return JNI_FALSE;
    }

	if (JNUPY_MP_STATE != NULL) {
	    // TODO: raise error?
	    return JNI_FALSE;
	}

	mp_state_ctx_t *state = mp_state_new();
	if (state == NULL) {
	    return JNI_FALSE;
	}

    char *heap = NULL;

    nlr_gk_buf_t nlr_gk = nlr_gk_new();
    if (nlr_gk_push(&nlr_gk) == 0) {
        mp_uint_t stack_size = MEM_SIZE(40, KB);
        mp_uint_t heap_size = MEM_SIZE(256, KB);

        mp_state_force_load(state);
        mp_stack_set_limit(stack_size);

        heap = malloc(heap_size);
        assert(heap != NULL);

        gc_init(heap, heap + heap_size);

        assert((char *)MP_STATE_MEM(gc_alloc_table_start) == heap);

        mp_init();

        mp_obj_list_init(mp_sys_path, 0);
        mp_obj_list_init(mp_sys_argv, 0);

        mp_obj_module_t *module_jnupy = mp_obj_new_module(MP_QSTR_jnupy);
        mp_obj_dict_t *module_jnupy_dict = mp_call_function_0(mp_load_attr(mp_module_jnupy.globals, MP_QSTR_copy));
        mp_obj_dict_store(module_jnupy_dict, MP_OBJ_NEW_QSTR(MP_QSTR_jfuncs), mp_obj_new_dict(0));
        mp_obj_dict_store(module_jnupy_dict, MP_OBJ_NEW_QSTR(MP_QSTR_pyrefs), mp_obj_new_dict(0));
        module_jnupy->globals = module_jnupy_dict;

        mp_state_store(state);
        JNUPY_CALL(SetLongField, JNUPY_SELF, JFIELD(PythonState, mpState), (mp_uint_t)state);
        JNUPY_MP_STATE = state;

        nlr_gk_pop(&nlr_gk);
        return JNI_TRUE;
    } else {
        if (state != NULL) {
            free(state);
        }

        if (heap != NULL) {
            free(heap);
        }

        return JNI_FALSE;
    }

    JNUPY_FUNC_END;
}


JNUPY_FUNC_DEF(jboolean, mp_1state_1check)
    (JNIEnv *env, jobject self) {
    JNUPY_FUNC_START;

	if (JNUPY_MP_STATE != NULL) {
	    return JNI_TRUE;
	} else {
	    return JNI_FALSE;
	}

    JNUPY_FUNC_END;
}

JNUPY_FUNC_DEF(jboolean, mp_1state_1free)
    (JNIEnv *env, jobject self) {
    JNUPY_FUNC_START;

	if (JNUPY_MP_STATE != NULL) {
	    if (!mp_state_is_loaded(JNUPY_MP_STATE)) {
    	    mp_state_load(JNUPY_MP_STATE);
	    }

        mp_deinit();
        free(MP_STATE_MEM(gc_alloc_table_start));
        mp_state_store(JNUPY_MP_STATE);

	    mp_state_free(JNUPY_MP_STATE);
	}

	return JNI_TRUE;

    JNUPY_FUNC_END;
}

JNUPY_FUNC_DEF(jboolean, mp_1code_1exec)
    (JNIEnv *env, jobject self, jstring code) {
    JNUPY_FUNC_START_WITH_STATE;

    nlr_gk_buf_t nlr_gk;
    if (nlr_gk_push(&nlr_gk) == 0) {
        qstr name = qstr_from_str("<CODE from JAVA>");
        const char *codebuf = JNUPY_CALL(GetStringUTFChars, code, 0);

        mp_lexer_t *lex = mp_lexer_new_from_str_len(name, codebuf, strlen(codebuf), 0);

        if (lex == NULL) {
            JNUPY_CALL(ReleaseStringUTFChars, code, codebuf);
            return JNI_FALSE;
        }

        qstr source_name = lex->source_name;
        mp_parse_node_t pn = mp_parse(lex, MP_PARSE_FILE_INPUT);

        JNUPY_CALL(ReleaseStringUTFChars, code, codebuf);

        mp_obj_t module_fun = mp_compile(pn, source_name, MP_EMIT_OPT_NONE, false);
        if (module_fun == NULL) {
            return JNI_FALSE;
        }

        mp_call_function_0(module_fun);

        nlr_gk_pop(&nlr_gk);
        return JNI_TRUE;
    } else {
        // TODO: how to handle exception on java side?
        mp_obj_print_exception(&mp_plat_print, nlr_gk.buf.ret_val);
        return JNI_FALSE;
    }

    JNUPY_FUNC_END;
}

JNUPY_FUNC_DEF(jobject, mp_1code_1eval)
    (JNIEnv *env, jobject self, jstring code) {
    JNUPY_FUNC_START_WITH_STATE;

    nlr_gk_buf_t nlr_gk;
    if (nlr_gk_push(&nlr_gk) == 0) {
        qstr name = qstr_from_str("<CODE from JAVA>");
        const char *codebuf = JNUPY_CALL(GetStringUTFChars, code, 0);

        mp_lexer_t *lex = mp_lexer_new_from_str_len(name, codebuf, strlen(codebuf), 0);

        if (lex == NULL) {
            JNUPY_CALL(ReleaseStringUTFChars, code, codebuf);
            return NULL;
        }

        qstr source_name = lex->source_name;
        mp_parse_node_t pn = mp_parse(lex, MP_PARSE_EVAL_INPUT);

        JNUPY_CALL(ReleaseStringUTFChars, code, codebuf);

        mp_obj_t module_fun = mp_compile(pn, source_name, MP_EMIT_OPT_NONE, false);
        if (module_fun == NULL) {
            return NULL;
        }

        mp_obj_t result = mp_call_function_0(module_fun);
        jobject jresult = jnupy_obj_py2j(result);

        nlr_gk_pop(&nlr_gk);
        return jresult;
    } else {
        // TODO: how to handle exception on java side?
        if (nlr_gk.buf.ret_val == NULL) {
            nlr_gk_jump(NULL);
        }

        mp_obj_print_exception(&mp_plat_print, nlr_gk.buf.ret_val);
        return NULL;
    }

    JNUPY_FUNC_END;
}

JNUPY_FUNC_DEF(jboolean, mp_1jfunc_1set)
    (JNIEnv *env, jobject self, jstring name, jobject jfunc) {
    JNUPY_FUNC_START_WITH_STATE;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        const char *namebuf = JNUPY_CALL(GetStringUTFChars, name, 0);
        mp_obj_t name_obj = mp_obj_new_str(namebuf, strlen(namebuf), true);
        JNUPY_CALL(ReleaseStringUTFChars, name, namebuf);

        mp_obj_t jfunc_obj = mp_obj_jfunc_new(JNUPY_PY_JSTATE, name_obj, jfunc);

        mp_obj_t module_jnupy = mp_module_get(MP_QSTR_jnupy);
        mp_obj_t jfuncs_dict = mp_load_attr(module_jnupy, MP_QSTR_jfuncs);
        mp_obj_subscr(jfuncs_dict, name_obj, jfunc_obj);

        nlr_pop();
        return JNI_TRUE;
    } else {
        // TODO: how to handle exception on java side?
        mp_obj_print_exception(&mp_plat_print, nlr.ret_val);
        return JNI_FALSE;
    }

    JNUPY_FUNC_END_VALUE(JNI_FALSE);
}

// TODO: add mp_jobject_set?

JNUPY_FUNC_DEF(void, mp_1ref_1incr)
    (JNIEnv *env, jobject self, jobject jobj) {
    JNUPY_FUNC_START_WITH_STATE;

    mp_obj_t obj = jnupy_pyobj_get(jobj);
    if (!MP_OBJ_IS_OBJ(obj)) {
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_int_t objid = ((mp_int_t)obj) >> 2;

        mp_obj_t module_jnupy = mp_module_get(MP_QSTR_jnupy);
        mp_obj_t pyrefs_dict = mp_load_attr(module_jnupy, MP_QSTR_pyrefs);
        mp_obj_subscr(pyrefs_dict, MP_OBJ_NEW_SMALL_INT(objid), obj);

        nlr_pop();
    } else {
        // TODO: how to handle exception on java side?
        mp_obj_print_exception(&mp_plat_print, nlr.ret_val);
    }
    JNUPY_FUNC_END_VOID;
}

JNUPY_FUNC_DEF(void, mp_1ref_1derc)
    (JNIEnv *env, jobject self, jobject jobj) {
    JNUPY_FUNC_START_WITH_STATE;

    mp_obj_t obj = jnupy_pyobj_get(jobj);
    if (!MP_OBJ_IS_OBJ(obj)) {
        return;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_int_t objid = ((mp_int_t)obj) >> 2;

        mp_obj_t module_jnupy = mp_module_get(MP_QSTR_jnupy);
        mp_obj_t pyrefs_dict = mp_load_attr(module_jnupy, MP_QSTR_pyrefs);
        mp_obj_subscr(pyrefs_dict, MP_OBJ_NEW_SMALL_INT(objid), obj); // TODO: remove safety with no setitem.
        mp_obj_subscr(pyrefs_dict, MP_OBJ_NEW_SMALL_INT(objid), MP_OBJ_NULL);

        nlr_pop();
    } else {
        // TODO: how to handle exception on java side?
        mp_obj_print_exception(&mp_plat_print, nlr.ret_val);
    }
    JNUPY_FUNC_END_VOID;
}

/** JNI EXPORT FUNCTION MECRO CLNEAUP **/
#undef return
