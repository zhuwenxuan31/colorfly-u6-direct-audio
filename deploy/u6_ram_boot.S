/* U6 v2.0 fastboot 'go' entry. Dispatch only to the matched vendor boota. */
.syntax unified
.arch armv7-a
.arm
.text
.balign 4
.global u6_ram_boot
.type u6_ram_boot, %function
u6_ram_boot:
.Lentry:
    cmp     r0, #1
    bne     reject
    ldr     r2, [lr, #-4]
    ldr     r3, =0xe12fff33
    cmp     r2, r3
    bne     reject
    ldr     r2, [lr]
    ldr     r3, =0xe8bd8008
    cmp     r2, r3
    bne     reject

    adr     r2, .Lentry
    ldr     r3, =0x41000800
    cmp     r2, r3
    bne     reject
    add     r2, r2, #2048
    ldr     r3, [r2]
    ldr     ip, =0x52444e41
    cmp     r3, ip
    bne     reject
    ldr     r3, [r2, #4]
    ldr     ip, =0x2144494f
    cmp     r3, ip
    bne     reject

    push    {r4-r8, lr}
    mov     r4, r2
    movw    r6, #0x13dc            /* boota entry minus go return address */
    add     r6, lr, r6
    adr     r5, stock_prologue
    mov     r7, r6
    mov     r8, #6
match_prologue:
    ldr     r2, [r5], #4
    ldr     r3, [r7], #4
    cmp     r2, r3
    bne     restore_reject
    subs    r8, r8, #1
    bne     match_prologue

    /* Vendor fastboot downloads at 0x41000000, the ramdisk destination.
       Relocate the image before boota copies over that address. */
    ldr     r2, [r4, #36]
    cmp     r2, #2048
    bne     restore_reject
    ldr     r2, [r4, #8]
    ldr     r3, [r4, #16]
    cmp     r2, #0x1000000
    bhs     restore_reject
    cmp     r3, #0x1000000
    bhs     restore_reject
    movw    r5, #2047
    add     r2, r2, r5
    add     r3, r3, r5
    bic     r2, r2, r5
    bic     r3, r3, r5
    add     r2, r2, r3
    add     r2, r2, #2048
    mov     r3, #0x1000000
    sub     r3, r3, #4096
    cmp     r2, r3
    bhi     restore_reject
    mov     r5, #0x42000000
copy_image:
    ldmia   r4!, {r7, r8}
    stmia   r5!, {r7, r8}
    subs    r2, r2, #8
    bne     copy_image
    mov     r4, #0x42000000

    mov     r7, #0x43000000
    add     r2, r7, #12
    str     r2, [r7]
    ldr     r2, =0x746f6f62
    str     r2, [r7, #12]
    mov     r2, #'a'
    str     r2, [r7, #16]
    add     r2, r7, #24
    str     r2, [r7, #4]
    mov     r3, #0
    str     r3, [r7, #8]
    mov     r5, #28
write_address:
    lsr     r3, r4, r5
    and     r3, r3, #15
    cmp     r3, #9
    addls   r3, r3, #'0'
    addhi   r3, r3, #'a' - 10
    strb    r3, [r2], #1
    subs    r5, r5, #4
    bpl     write_address
    mov     r3, #0
    strb    r3, [r2]
    mov     r0, #0
    mov     r1, #0
    mov     r2, #2
    mov     r3, r7
    mov     ip, r6
    pop     {r4-r8, lr}
    bx      ip                  /* Tail call: the ramdisk may overwrite this stub. */

restore_reject:
    pop     {r4-r8, lr}
reject:
    mov     r0, #65
    bx      lr

.balign 4
stock_prologue:
    .word 0xe3520001, 0xe92d41f0, 0xe3a01000
    .word 0xe24dd050, 0xe1a05003, 0xe58d1040
.ltorg
.size u6_ram_boot, .-u6_ram_boot
