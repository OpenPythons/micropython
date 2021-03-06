/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
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

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"
#include "py/bc.h"
#include "py/objboundmeth.h"

#if MICROPY_ERROR_REPORTING == MICROPY_ERROR_REPORTING_DETAILED
STATIC void bound_meth_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind) {
    (void)kind;
    mp_obj_bound_meth_t *o = MP_OBJ_TO_PTR(o_in);
    mp_printf(print, "<bound_method %p ", o);
    mp_obj_print_helper(print, o->self, PRINT_REPR);
    mp_print_str(print, ".");
    mp_obj_print_helper(print, o->meth, PRINT_REPR);
    mp_print_str(print, ">");
}
#endif

STATIC mp_obj_t bound_meth_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_obj_bound_meth_t *self = MP_OBJ_TO_PTR(self_in);

    // need to insert self->self before all other args and then call self->meth

    mp_uint_t n_total = n_args + 2 * n_kw;
    if (n_total <= 4) {
        // use stack to allocate temporary args array
        mp_obj_t args2[5];
        args2[0] = self->self;
        memcpy(args2 + 1, args, n_total * sizeof(mp_obj_t));
        return mp_call_function_n_kw(self->meth, n_args + 1, n_kw, &args2[0]);
    } else {
        // use heap to allocate temporary args array
        mp_obj_t *args2 = m_new(mp_obj_t, 1 + n_total);
        args2[0] = self->self;
        memcpy(args2 + 1, args, n_total * sizeof(mp_obj_t));
        mp_obj_t res = mp_call_function_n_kw(self->meth, n_args + 1, n_kw, &args2[0]);
        m_del(mp_obj_t, args2, 1 + n_total);
        return res;
    }
}

#if MICROPY_STACKLESS_EXTRA
mp_code_state_ptr bound_meth_flatcall(mp_obj_t self_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    mp_obj_bound_meth_t *self = self_in;

    mp_flatcall_fun_t flatcall = mp_obj_get_type(self->meth)->flatcall;
    if (flatcall == NULL) {
        return NULL;
    }
    // need to insert self->self before all other args and then call self->meth

    mp_uint_t n_total = n_args + 2 * n_kw;
    // use heap to allocate temporary args array
    mp_obj_t *args2 = m_new(mp_obj_t, 1 + n_total);
    args2[0] = self->self;
    memcpy(args2 + 1, args, n_total * sizeof(mp_obj_t));
    mp_code_state *res = flatcall(self->meth, n_args + 1, n_kw, &args2[0]);
    m_del(mp_obj_t, args2, 1 + n_total);
    return res;
}
#endif

#if MICROPY_PY_FUNCTION_ATTRS
STATIC void bound_meth_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        // not load attribute
        return;
    }
    if (attr == MP_QSTR___name__) {
        mp_obj_bound_meth_t *o = MP_OBJ_TO_PTR(self_in);
        dest[0] = MP_OBJ_NEW_QSTR(mp_obj_fun_get_name(o->meth));
    }
}
#endif

const mp_obj_type_t mp_type_bound_meth = {
    { &mp_type_type },
    .name = MP_QSTR_bound_method,
#if MICROPY_ERROR_REPORTING == MICROPY_ERROR_REPORTING_DETAILED
    .print = bound_meth_print,
#endif
    .call = bound_meth_call,
#if MICROPY_STACKLESS_EXTRA
    .flatcall = bound_meth_flatcall,
#endif
#if MICROPY_PY_FUNCTION_ATTRS
    .attr = bound_meth_attr,
#endif
};

mp_obj_t mp_obj_new_bound_meth(mp_obj_t meth, mp_obj_t self) {
    mp_obj_bound_meth_t *o = m_new_obj(mp_obj_bound_meth_t);
    o->base.type = &mp_type_bound_meth;
    o->meth = meth;
    o->self = self;
    return MP_OBJ_FROM_PTR(o);
}
