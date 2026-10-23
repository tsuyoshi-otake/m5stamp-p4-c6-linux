/* EasyStick M2.5 post-exec BusyBox ledger (test-only).
 * Dual sink: /dev/kmsg + /dev/ttyGS1, once-open, write-only, errno save/restore.
 * BusyBox child cannot inherit Dropbear fds after exec — open its own sinks.
 * Tags reuse ES_SSH prefix for one passive serial grep.
 */
#ifndef EASYSTICK_BB_LEDGER_H
#define EASYSTICK_BB_LEDGER_H

void es_bb_ledger_open(void);
void es_bb_mark(const char *msg);

#endif
