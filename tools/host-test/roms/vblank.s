@ Tiny GBA test ROM that behaves like a real game: waits for the VBlank
@ interrupt every frame (BIOS VBlankIntrWait, the CPU halted in between) and
@ changes the backdrop colour from an interrupt-driven counter. If a save
@ state loses or repeats an interrupt, the colours stop matching a normal run.
@ Build: arm-none-eabi-as vblank.s -o vblank.o && arm-none-eabi-ld -Ttext=0x08000000 vblank.o -o vblank.elf && arm-none-eabi-objcopy -O binary vblank.elf vblank.gba
    .arm
    .section .text
    .global _start
_start:
    b       start
    .space  0xBC                    @ rest of the cartridge header (unchecked)
start:
    mov     r0, #0x04000000
    mov     r1, #0
    strh    r1, [r0]                @ DISPCNT: mode 0, no layers -> backdrop
    mov     r1, #8
    strh    r1, [r0, #4]            @ DISPSTAT: VBlank IRQ on
    add     r2, r0, #0x200
    mov     r1, #1
    strh    r1, [r2]                @ IE = VBlank
    strh    r1, [r2, #8]            @ IME = 1
    ldr     r3, =0x03007FFC
    ldr     r4, =irq
    str     r4, [r3]                @ IRQ vector used by the BIOS
loop:
    swi     0x050000                @ VBlankIntrWait
    ldr     r3, =0x03000000
    ldr     r1, [r3]
    @ Add the keys held, so input changes the picture too.
    ldr     r5, =0x04000130
    ldrh    r6, [r5]
    eor     r6, r6, #0x300
    eor     r6, r6, #0xFF
    add     r1, r1, r6, lsl #5
    ldr     r5, =0x05000000
    strh    r1, [r5]                @ backdrop colour
    b       loop
irq:
    mov     r0, #0x04000000
    add     r0, r0, #0x200
    mov     r1, #1
    strh    r1, [r0, #2]            @ acknowledge IF
    ldr     r2, =0x03007FF8
    ldrh    r3, [r2]
    orr     r3, r3, #1
    strh    r3, [r2]                @ tell VBlankIntrWait it happened
    ldr     r2, =0x03000000
    ldr     r3, [r2]
    add     r3, r3, #37
    str     r3, [r2]
    bx      lr
    .ltorg
    .balign 4
    .space  1024                    @ the core expects a few KB at least
