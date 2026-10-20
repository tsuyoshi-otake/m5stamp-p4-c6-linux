/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
 *
 * C68-CLEAN-RELEASE: bare-metal CPU1 secondary entry (IRAM).
 */
#define C68_STAGE_WAITING      0xC6810001
#define C68_STAGE_RELEASE_SEEN 0xC6810002
#define C68_RELEASE_GO         0xC68A11A5
#define KERNEL_LOAD_PA         0x48000000
#define DTB_LOAD_PA            0x48800000

/*
 * c68_prot (main.c) holds 16 triples {PMACFGn, PMAADDRn, PMPADDRn} followed
 * by PMPCFG0-3: 52 words.
 */
#define CSR_PMACFG0            0xbc0
#define CSR_PMAADDR0           0xbd0
#define CSR_PMPCFG0            0x3a0
#define CSR_PMPADDR0           0x3b0

    .extern c68_release
    .extern c68_stage
    .extern c68_prot
    .extern c68_prot_diff

    .section .iram1,"ax"

/*
 * #5: void c68_capture_prot(void), called on CPU0 before CPU1 is started.
 * Saves this CPU's PMA and PMP entries (ESP-IDF's region protection plus
 * grant_psram_to_umode()) for c68_secondary_entry to load on CPU1.
 */
    .align  4
    .global c68_capture_prot
    .type   c68_capture_prot, @function
c68_capture_prot:
    la      t0, c68_prot
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    csrr    t1, CSR_PMACFG0+\n
    sw      t1, 0(t0)
    csrr    t1, CSR_PMAADDR0+\n
    sw      t1, 4(t0)
    csrr    t1, CSR_PMPADDR0+\n
    sw      t1, 8(t0)
    addi    t0, t0, 12
    .endr
    .irp    n, 0,1,2,3
    csrr    t1, CSR_PMPCFG0+\n
    sw      t1, 0(t0)
    addi    t0, t0, 4
    .endr
    fence   rw, rw
    ret

    .size c68_capture_prot, .-c68_capture_prot

    .align  4
    .global c68_secondary_entry
    .type   c68_secondary_entry, @function
c68_secondary_entry:
    /*
     * #5: give CPU1 CPU0's region protection.  The shim is single-core, so
     * ESP-IDF's call_start_cpu1() (bootloader_init_mem() ->
     * esp_cpu_configure_region_protection()) never runs here and CPU1 keeps
     * the ROM's PMA, a single RWX entry for 0x40000000-0x4fffffff.  An
     * access to an unmapped address such as 0 then goes out on the bus and
     * comes back as an imprecise access fault, which can land inside Linux's
     * trap entry and panic the kernel.  CPU0's PMA turns it into a precise
     * fault.
     *
     * PMA entries as ESP-IDF writes them: disable, address, configuration.
     * PMP: every address first, the configuration words (with their lock
     * bits) last.  Nothing on CPU1 is locked before this.
     */
    la      t0, c68_prot
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    lw      t1, 0(t0)
    lw      t2, 4(t0)
    csrw    CSR_PMACFG0+\n, zero
    csrw    CSR_PMAADDR0+\n, t2
    csrw    CSR_PMACFG0+\n, t1
    lw      t1, 8(t0)
    csrw    CSR_PMPADDR0+\n, t1
    addi    t0, t0, 12
    .endr
    .irp    n, 0,1,2,3
    lw      t1, 0(t0)
    csrw    CSR_PMPCFG0+\n, t1
    addi    t0, t0, 4
    .endr

    /* Read everything back; c68_prot_diff is the OR of all differing bits. */
    la      t0, c68_prot
    li      t3, 0
    .irp    n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    csrr    t1, CSR_PMACFG0+\n
    lw      t2, 0(t0)
    xor     t1, t1, t2
    or      t3, t3, t1
    csrr    t1, CSR_PMAADDR0+\n
    lw      t2, 4(t0)
    xor     t1, t1, t2
    or      t3, t3, t1
    csrr    t1, CSR_PMPADDR0+\n
    lw      t2, 8(t0)
    xor     t1, t1, t2
    or      t3, t3, t1
    addi    t0, t0, 12
    .endr
    .irp    n, 0,1,2,3
    csrr    t1, CSR_PMPCFG0+\n
    lw      t2, 0(t0)
    xor     t1, t1, t2
    or      t3, t3, t1
    addi    t0, t0, 4
    .endr
    la      t0, c68_prot_diff
    sw      t3, 0(t0)
    fence   rw, rw

    la      t0, c68_stage
    li      t1, C68_STAGE_WAITING
    sw      t1, 0(t0)
    fence   rw, rw

1:
    la      t0, c68_release
    lw      t1, 0(t0)
    li      t2, C68_RELEASE_GO
    bne     t1, t2, 1b

    la      t0, c68_stage
    li      t1, C68_STAGE_RELEASE_SEEN
    sw      t1, 0(t0)
    fence   rw, rw

    csrci   mstatus, 0x8
    li      a0, 1
    li      a1, DTB_LOAD_PA
    li      t0, KERNEL_LOAD_PA
    fence.i
    jr      t0

    .size c68_secondary_entry, .-c68_secondary_entry
