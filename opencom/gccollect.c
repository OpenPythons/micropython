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

#include <stdio.h>

#include "py/mpstate.h"
#include "py/gc.h"

#if MICROPY_ENABLE_GC

#if !MICROPY_GCREGS_SETJMP
#error MICROPY_GCREGS_SETJMP is unset.
#endif

#include <setjmp.h>

typedef jmp_buf regs_t;

STATIC void gc_helper_get_regs(regs_t arr) {
    setjmp(arr);
}

void gc_collect(void) {
    //gc_dump_info();

    gc_collect_start();
    regs_t regs;
    
    gc_helper_get_regs(regs);
    // GC stack (and regs because we captured them)
    void **regs_ptr = (void**)(void*)&regs;
    gc_collect_root(regs_ptr, ((mp_uint_t)MP_STATE_VM(stack_top) - (mp_uint_t)&regs) / sizeof(mp_uint_t));
    
    gc_collect_end();

    //printf("-----\n");
    //gc_dump_info();
}

#endif //MICROPY_ENABLE_GC
