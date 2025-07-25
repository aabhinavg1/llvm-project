//===-- aeabi_uidivmod.S - EABI uidivmod implementation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../assembly.h"

// struct { unsigned quot, unsigned rem}
//        __aeabi_uidivmod(unsigned numerator, unsigned denominator) {
//   unsigned rem, quot;
//   quot = __udivmodsi4(numerator, denominator, &rem);
//   return {quot, rem};
// }

#if defined(__MINGW32__)
#define __aeabi_uidivmod __rt_udiv
#endif

        .syntax unified
        .text
        DEFINE_CODE_STATE
        .p2align 2
DEFINE_COMPILERRT_FUNCTION(__aeabi_uidivmod)
#if defined(USE_THUMB_1)
        cmp     r0, r1
        bcc     LOCAL_LABEL(case_denom_larger)
        push    {r0, r1, lr}
        bl      SYMBOL_NAME(__aeabi_uidiv)
        pop     {r1, r2, r3}
        muls    r2, r0, r2 // r2 = quot * denom
        subs    r1, r1, r2
        JMP     (r3)
LOCAL_LABEL(case_denom_larger):
        movs    r1, r0
        movs    r0, #0
        JMP     (lr)
#else // defined(USE_THUMB_1)
        PACBTI_LANDING
#if defined(__ARM_FEATURE_PAC_DEFAULT)
        push    { r12, lr }
#else
        push    { lr }
#endif
        sub     sp, sp, #4
        mov     r2, sp
#if defined(__MINGW32__)
        mov     r3, r0
        mov     r0, r1
        mov     r1, r3
#endif
        bl      SYMBOL_NAME(__udivmodsi4)
        ldr     r1, [sp]
        add     sp, sp, #4
#if defined(__ARM_FEATURE_PAC_DEFAULT)
        pop     { r12, lr }
        PAC_RETURN
#else
        pop     { pc }
#endif
#endif
END_COMPILERRT_FUNCTION(__aeabi_uidivmod)

NO_EXEC_STACK_DIRECTIVE

