/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
 *
 * C68-CLEAN-RELEASE: bare-metal CPU1 secondary entry (IRAM).
 */
#define C68_STAGE_WAITING      0xC6810001
#define C68_STAGE_RELEASE_SEEN 0xC6810002
#define C68_RELEASE_GO         0xC68A11A5
#define KERNEL_LOAD_PA         0x48000000
#define DTB_LOAD_PA            0x48800000

    .extern c68_release
    .extern c68_stage

    .section .iram1,"ax"
    .align  4
    .global c68_secondary_entry
    .type   c68_secondary_entry, @function
c68_secondary_entry:
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
