@ sigsetjmp/siglongjmp: the signal mask is meaningless here, so these are
@ setjmp/longjmp. Tail calls keep the caller's frame for setjmp.
  .syntax unified
  .arm
  .text
  .global b_sigsetjmp
  .type b_sigsetjmp, %function
b_sigsetjmp:
  b setjmp

  .global b_siglongjmp
  .type b_siglongjmp, %function
b_siglongjmp:
  b longjmp
