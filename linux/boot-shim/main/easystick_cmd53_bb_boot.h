#ifndef EASYSTICK_CMD53_BB_BOOT_H
#define EASYSTICK_CMD53_BB_BOOT_H

void easystick_cmd53_bb_dump_early(void);
void easystick_cmd53_bb_release_after_dump(void);
void easystick_crash_capsule_dump_early(void);
void easystick_crash_capsule_flush(void);
#ifdef EASYSTICK_CMD53_BB_SELFTEST
void easystick_cmd53_bb_selftest_maybe(void);
#endif
#ifdef EASYSTICK_CMD53_BB_SELFTEST_TORN
void easystick_cmd53_bb_selftest_torn_maybe(void);
#endif

#endif
