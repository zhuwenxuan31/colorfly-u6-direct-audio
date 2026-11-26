#if defined(__aarch64__)
.section .text.u6_call,"ax",%progbits
    bl u6_stub

.section .text.u6_stub,"ax",%progbits
.p2align 2
.type u6_stub,%function
u6_stub:
    stp x0, x1, [sp, #-48]!
    stp x2, x3, [sp, #16]
    str x30, [sp, #32]

    adr x0, .Llibrary
    mov w1, #2 // Bionic RTLD_NOW on LP64.
    adr x16, .Ldlopen
    ldr w17, [x16]
    ldr x16, [x16, w17, sxtw]
    blr x16
    cbz x0, .Ldispatch
    adr x1, .Lentry
    adr x16, .Ldlsym
    ldr w17, [x16]
    ldr x16, [x16, w17, sxtw]
    blr x16
.Ldispatch:
    mov x16, x0
    ldr x30, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #48
    cbz x16, .Lstock
    br x16
.Lstock:
    b u6_stock_register

#else
.syntax unified
.section .text.u6_call,"ax",%progbits
.thumb
    blx u6_stub

.section .text.u6_stub,"ax",%progbits
.arm
.p2align 2
.type u6_stub,%function
u6_stub:
    push {r0-r4, lr}

    adr r0, .Llibrary
    mov r1, #0 // Bionic RTLD_NOW on LP32.
    adr r4, .Ldlopen
    ldr ip, [r4]
    ldr ip, [r4, ip]
    blx ip
    cmp r0, #0
    beq .Ldispatch
    adr r1, .Lentry
    adr r4, .Ldlsym
    ldr ip, [r4]
    ldr ip, [r4, ip]
    blx ip
.Ldispatch:
    mov ip, r0
    pop {r0-r4, lr}
    cmp ip, #0
    bxne ip
    b u6_stock_register
#endif

.p2align 2
.Ldlopen:
    .word u6_dlopen_slot - .
.Ldlsym:
    .word u6_dlsym_slot - .
.Llibrary:
    .asciz "libu6_direct_client.so"
.Lentry:
    .asciz "u6_register_audiotrack"
.size u6_stub, . - u6_stub
.section .note.GNU-stack,"",%progbits
