/* SPDX-License-Identifier: CC0-1.0 */
@ setjmp/longjmp for the guest. Saves r4-r11, sp, lr and d8-d15.
    .syntax unified
    .arm
    .text

    .globl setjmp
    .type setjmp, %function
setjmp:
    stmia r0!, {r4-r11}
    str sp, [r0], #4
    str lr, [r0], #4
    vstmia r0, {d8-d15}
    mov r0, #0
    bx lr

    .globl _setjmp
    .type _setjmp, %function
_setjmp:
    b setjmp

    .globl longjmp
    .type longjmp, %function
longjmp:
    mov ip, r0
    movs r0, r1
    moveq r0, #1
    ldmia ip!, {r4-r11}
    ldr sp, [ip], #4
    ldr lr, [ip], #4
    vldmia ip, {d8-d15}
    bx lr

    .globl _longjmp
    .type _longjmp, %function
_longjmp:
    b longjmp

@ sigsetjmp/siglongjmp: the signal mask is not emulated, so they are setjmp/longjmp.
    .globl sigsetjmp
    .type sigsetjmp, %function
sigsetjmp:
    b setjmp

    .globl siglongjmp
    .type siglongjmp, %function
siglongjmp:
    b longjmp
