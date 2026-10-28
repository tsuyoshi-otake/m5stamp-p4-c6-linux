// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
/*
 * Example LP core program (#59).  main() runs once every lp_period (1 s by
 * default) while Linux runs (lp_core=always) or while the board sleeps
 * (lp_core=always or sleep).  It counts its runs, and with wake_every set it
 * wakes the board every that many runs (wake_reason=lp_core).
 *
 * From Linux, with the addresses in build/esp-idf/main/ulp_main/ulp_main.ld:
 *   python: import lpcore; lpcore.read(ADDR_OF_runs)
 *   shell:  od -An -tu4 -j $((ADDR - 0x50108100)) -N4 /sys/kernel/board/lp_core/memory
 */
#include <stdint.h>
#include "ulp_lp_core_utils.h"

volatile uint32_t runs;         /* main() calls since the program was loaded */
volatile uint32_t wake_every;   /* 0: never wake the board */
volatile uint32_t wakes;        /* wake requests made */

int main(void)
{
	runs++;
	if (wake_every && runs % wake_every == 0) {
		wakes++;
		/* While Linux runs this only sets a flag nobody waits for. */
		ulp_lp_core_wakeup_main_processor();
	}
	return 0;
}
