// REQUIRES: arm
// RUN: llvm-mc -filetype=obj -triple=armv7a-none-linux-gnueabi %s -o %t
// RUN: ld.lld %t -no-combine-eh-sections -o %t2 2>&1

// First llvm-readobj emits .ARM.exidx sections,
// then it emits .ARM.extab sections.
// If we want to check first all sections for f1, and then for f2,
// we should run checks twice.

// RUN: llvm-readobj --sections %t2 | FileCheck %s --check-prefix=CHECK-EXIDX
// RUN: llvm-readobj --sections %t2 | FileCheck %s --check-prefix=CHECK-EXTAB

 .syntax unified
 .section .text, "ax",%progbits

// Create fake personality routine (it should be present somewhere in c++ library)
 .globl __gxx_personality_v0
__gxx_personality_v0:
 .fnstart
 bx lr
 .cantunwind
 .fnend

 .globl _start
_start:
 .fnstart
 bx lr
 .cantunwind
 .fnend

// CHECK-EXIDX:         Section {
// CHECK-EXIDX:         Name: .ARM.exidx.text.f1
// CHECK-EXIDX-NEXT:    Type: SHT_ARM_EXIDX (0x70000001)
// CHECK-EXIDX-NEXT:    Flags [
// CHECK-EXIDX-NEXT:      SHF_ALLOC
// CHECK-EXIDX-NEXT:      SHF_LINK_ORDER
// CHECK-EXIDX-NEXT:    ]
 .section .text.f1, "ax", %progbits
 .globl f1
f1:
 .fnstart
 bx lr


// CHECK-EXTAB: Section {
// CHECK-EXTAB:     Name: .ARM.extab.text.f1
// CHECK-EXTAB-NEXT:     Type: SHT_PROGBITS
// CHECK-EXTAB-NEXT:     Flags [
// CHECK-EXTAB-NEXT:       SHF_ALLOC
// CHECK-EXTAB-NEXT:     ]
 .personality __gxx_personality_v0
 .handlerdata
// Empty handler data

 .fnend

// CHECK-EXIDX:         Section {
// CHECK-EXIDX:         Name: .ARM.exidx.text.f2
// CHECK-EXIDX-NEXT:    Type: SHT_ARM_EXIDX (0x70000001)
// CHECK-EXIDX-NEXT:    Flags [
// CHECK-EXIDX-NEXT:      SHF_ALLOC
// CHECK-EXIDX-NEXT:      SHF_LINK_ORDER
// CHECK-EXIDX-NEXT:    ]
 .section .text.f2, "ax", %progbits
 .globl f2
f2:
 .fnstart
 bx lr

// CHECK-EXTAB: Section {
// CHECK-EXTAB:     Name: .ARM.extab.text.f2
// CHECK-EXTAB-NEXT:     Type: SHT_PROGBITS
// CHECK-EXTAB-NEXT:     Flags [
// CHECK-EXTAB-NEXT:       SHF_ALLOC
// CHECK-EXTAB-NEXT:     ]
 .globl  __gxx_personality_v0
 .personality __gxx_personality_v0
 .handlerdata
// Empty handler data

 .fnend
