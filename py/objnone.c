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

#include <stdlib.h>

#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime0.h"

typedef struct _mp_obj_none_t {
    mp_obj_base_t base;
} mp_obj_none_t;

STATIC void none_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)self_in;
    if (MICROPY_PY_UJSON && kind == PRINT_JSON) {
        mp_print_str(print, "null");
    } else {
        mp_print_str(print, "None");
    }

    #if MICROPY_ALLOW_PAUSE_VM
    if (self_in == &mp_const__vm_pause_obj){
        mp_print_str(print, "! (MICROPY_ALLOW_PAUSE_VM)");
    }
    #endif
}

STATIC mp_obj_t none_unary_op(mp_uint_t op, mp_obj_t o_in) {
    (void)o_in;
    switch (op) {
        case MP_UNARY_OP_BOOL: return mp_const_false;
        default: return MP_OBJ_NULL; // op not supported
    }
}

const mp_obj_type_t mp_type_NoneType = {
    { &mp_type_type },
    .name = MP_QSTR_NoneType,
    .print = none_print,
    .unary_op = none_unary_op,
};

const mp_obj_none_t mp_const_none_obj = {{&mp_type_NoneType}};

#if MICROPY_ALLOW_PAUSE_VM
// TODO: move this.
const mp_obj_type_t mp_type_VMPauseNoneType = {
    { &mp_type_type },
    .name = MP_QSTR_NoneType,
    .print = none_print,
    .unary_op = none_unary_op,
};

const mp_obj_none_t mp_const__vm_pause_obj = {{&mp_type_VMPauseNoneType}};
#endif
