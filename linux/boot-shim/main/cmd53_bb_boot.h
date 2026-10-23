#ifndef CMD53_BB_BOOT_H
#define CMD53_BB_BOOT_H

void cmd53_bb_dump_early(void);
void cmd53_bb_release_after_dump(void);
void crash_capsule_dump_early(void);
void crash_capsule_flush(void);
#ifdef CMD53_BB_SELFTEST
void cmd53_bb_selftest_maybe(void);
#endif
#ifdef CMD53_BB_SELFTEST_TORN
void cmd53_bb_selftest_torn_maybe(void);
#endif

#endif
