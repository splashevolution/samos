/*
 * SAM OS — kernel/pit.h
 * Sprint 20: 8254 PIT timer @ 100 Hz on IRQ0 — the preemption clock.
 *
 * Requires the PIC remap from ps2kbd.h (IRQ0 → vector 0x20) and the
 * _irq0 handler installed in idt.h. Unmasks IRQ0 at the master PIC.
 */

#ifndef SAM_PIT_H
#define SAM_PIT_H

#include <stdint.h>

#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43
#define PIT_FREQ_HZ  100
#define PIT_DIVISOR  (1193182 / PIT_FREQ_HZ)   /* ~11932 */

static void pit_init_100hz(void)
{
    /* Channel 0, lo/hi byte, mode 3 (square wave) */
    __asm__ volatile ("outb %0,%1" :: "a"((uint8_t)0x36), "Nd"(PIT_CMD));
    __asm__ volatile ("outb %0,%1" :: "a"((uint8_t)(PIT_DIVISOR & 0xFF)), "Nd"(PIT_CH0_DATA));
    __asm__ volatile ("outb %0,%1" :: "a"((uint8_t)(PIT_DIVISOR >> 8)),    "Nd"(PIT_CH0_DATA));

    /* Unmask IRQ0 at the master PIC (ps2kbd.h left only IRQ1 unmasked) */
    uint8_t mask;
    __asm__ volatile ("inb %1,%0" : "=a"(mask) : "Nd"(0x21));
    mask &= (uint8_t)~0x01;
    __asm__ volatile ("outb %0,%1" :: "a"(mask), "Nd"(0x21));
}

#endif /* SAM_PIT_H */
