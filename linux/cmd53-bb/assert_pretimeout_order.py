#!/usr/bin/env python3
"""Assert the WDT pretimeout path writes its crash capsule BEFORE it prints.

Why this checker exists
-----------------------
The 2026-08-28 S0 shot reproduced the SSH wedge and recovered a full retention
black box, but reported CRASH_CAPSULE empty/invalid.  That silence was
uninterpretable, because the instrument meant to survive the stall was gated
behind the very subsystem the stall might have taken down:

    pr_emerg("EASYSTICK_WDT PRETIMEOUT ...");  /* console: may block forever */
    easystick_capture_crash(...);              /* never reached if it does   */

and the whole handler had only 1.000 s before the stage-1 hardware reset.  An
empty capsule therefore could not distinguish an IRQ-off hard lock from a
printk/console deadlock.  The repair reorders the handler capsule-first.

build-m1.sh already gates on the PRESENCE of easystick_capture_crash, the
PRETIMEOUT pr_emerg, and dump_stack() in the built source.  Presence was never
the problem: every defective ordering above satisfies all three markers.  ORDER
is the invariant, and nothing checked it.  (CLAUDE.md 14.2: a checker only
proves what it looks at.)

The grace cap is compared against a value supplied by the CALLER, not read from
the source under test, so that design and requirement stay two independent
statements this tool compares (CLAUDE.md 14.13, 14.16).

Extended 2026-08-29 after a properly gated S0b re-shot (preflight-checked
reachability, so the trigger genuinely fired against a live DUT) captured a
real WDT reset inside the same window as an independent console beacon, with
the handler's own ordering already correct (this checker passed on it) --
and the capsule was STILL empty/invalid.  The cause was not ordering: the arm
routine wrote `grace_ticks` into TIMG_WDTCONFIG3 (the stage-1 / hardware-reset
threshold) instead of `timeout_ticks`.  Every wdt_hal_config_stage() call site
across every SoC in vendor/esp-idf (int_wdt.c, task_wdt_impl_timergroup.c,
and the rtc_wdt call sites) configures stage N's register as an ABSOLUTE tick
count from the last feed -- stage 1 is consistently `2 * stage0_period`, not
`1 * stage0_period` -- because all stages compare against the same free-
running counter. Writing `grace_ticks` (<= timeout_ticks/2) into stage 1 when
stage 0 already holds `pretimeout_ticks` (> timeout_ticks/2 in general) makes
stage 1's threshold SMALLER than stage 0's, so the hardware reset can fire
before the counter ever reaches stage 0's threshold: the pretimeout interrupt,
and therefore easystick_capture_crash(), may never run at all, independent of
whether the CPU is otherwise healthy. This is a second, independent way to
reach the same symptom item 3 below already named ("no source-level check can
see" an IRQ never taken) -- except this one IS visible at the source level,
because it is a register-value error, not a runtime IRQ-masking state. C8
below checks it.

What this checker CANNOT see
----------------------------
1.  Whether the capsule store actually reaches retention RAM before the
    stage-1 reset.  It reads C text; it cannot see cacheability, write
    buffering, or compiler store reordering.  Only the boot shim's post-reset
    readback proves retention, and it does: the S0 capture read back
    CMD53_BB VALID gen=91086 seq=15 from the same physical base after
    rst:0x7, so that base demonstrably retains through a WDT reset.
2.  Any TIME dimension (CLAUDE.md 14.19).  It cannot prove the grace window is
    long enough for the handler to finish, only that correctness no longer
    depends on the window being long enough for a printk.
3.  Whether easystick_capture_crash is reached at all.  An IRQ-off hard lock
    never takes the interrupt, and no source-level check can see that.
4.  Anything the preprocessor decides.  dump_stack() sits under
    #ifdef CONFIG_STACKTRACE; this tool checks the text is present and
    correctly ordered, not that the kernel config selects it.
5.  Callees.  It checks ordering inside the pretimeout handler and inside
    easystick_capture_crash only.  A console call reached indirectly through
    some other callee of the handler is invisible to it.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

TAB = chr(9)
NL = chr(10)
BS = chr(92)

HANDLER = "esp32p4_wdt_pretimeout"
CAPTURE = "easystick_capture_crash"
CAPSULE_CALL = CAPTURE + "("
PRETIMEOUT_PRINT = 'pr_emerg("EASYSTICK_WDT PRETIMEOUT'
COMMIT_STORE = "WRITE_ONCE(capsule->commit,"
DUMP_STACK = "dump_stack()"

# Calls that can block on a lock, a console, or an unbounded loop.  Any of
# these before the capsule store re-creates the S0 defect.
BLOCKING_CALLS = (
    "printk(",
    "pr_emerg(",
    "pr_alert(",
    "pr_crit(",
    "pr_err(",
    "pr_warn(",
    "pr_notice(",
    "pr_info(",
    "pr_debug(",
    "pr_cont(",
    "dev_emerg(",
    "dev_err(",
    "dev_warn(",
    "dev_info(",
    "dump_stack(",
    "show_regs(",
    "panic(",
    "console_lock(",
    "console_flush",
    "WARN(",
    "WARN_ON(",
    "BUG(",
    "BUG_ON(",
)

GRACE_RE = re.compile(
    r"grace_ticks\s*=\s*min_t\(\s*u32\s*,\s*([0-9]+)u\s*,\s*"
    r"timeout_ticks\s*/\s*2u\s*\)\s*;"
)

# TIMG_WDTCONFIG3 is the stage-1 (hardware reset) threshold. It MUST hold the
# full timeout_ticks (absolute, from the last feed), not grace_ticks alone --
# see the 2026-08-29 addition to the module docstring.
CONFIG3_WRITE_RE = re.compile(
    r"writel\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"wdt->base\s*\+\s*TIMG_WDTCONFIG3\s*\)\s*;"
)


def function_body(source, name):
    """Return the brace-balanced body of the definition of name, or None.

    A definition is an occurrence of name( whose argument list is followed by
    { rather than ; (which would be a call or a prototype).
    """
    for match in re.finditer(re.escape(name) + r"\s*\(", source):
        i = match.end() - 1
        depth = 0
        while i < len(source):
            ch = source[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            i += 1
        while i < len(source) and source[i] in " " + TAB + chr(13) + NL:
            i += 1
        if i >= len(source) or source[i] != "{":
            continue  # a call or a prototype, not a definition
        depth = 0
        start = i
        while i < len(source):
            if source[i] == "{":
                depth += 1
            elif source[i] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1]
            i += 1
    return None


def check(source, stacktrace, expect_grace_cap):
    """Return a list of failure strings; empty means PASS."""
    fails = []

    body = function_body(source, HANDLER)
    if body is None:
        fails.append("C1: no definition of " + HANDLER + "() found")
    else:
        cap = body.find(CAPSULE_CALL)
        if cap < 0:
            fails.append("C2: " + HANDLER + "() does not call " + CAPSULE_CALL)
        else:
            for call in BLOCKING_CALLS:
                at = body.find(call)
                if 0 <= at < cap:
                    fails.append(
                        "C3: " + call + " at offset " + str(at) + " precedes "
                        "the capsule write at offset " + str(cap) + " in "
                        + HANDLER + "() -- this is the S0 defect: the "
                        "survivability instrument is gated behind a call that "
                        "may never return"
                    )
            printed = body.find(PRETIMEOUT_PRINT)
            if printed < 0:
                fails.append(
                    "C4: " + HANDLER + "() lost its PRETIMEOUT marker print"
                )
            elif printed < cap:
                fails.append("C4: PRETIMEOUT print precedes the capsule write")
            if stacktrace:
                dumped = body.find(DUMP_STACK)
                if dumped < 0:
                    fails.append(
                        "C5: stacktrace requested but dump_stack() is absent "
                        "from " + HANDLER + "()"
                    )
                elif dumped < cap:
                    fails.append("C5: dump_stack() precedes the capsule write")

    capture_body = function_body(source, CAPTURE)
    if capture_body is None:
        fails.append("C6: no definition of " + CAPTURE + "() found")
    else:
        commit = capture_body.find(COMMIT_STORE)
        if commit < 0:
            fails.append("C6: " + CAPTURE + "() has no commit store")
        else:
            for call in BLOCKING_CALLS:
                at = capture_body.find(call)
                if 0 <= at < commit:
                    fails.append(
                        "C6: " + call + " at offset " + str(at) + " precedes "
                        "the commit store at offset " + str(commit) + " inside "
                        + CAPTURE + "() -- the capsule is not committed before "
                        "the path can block"
                    )

    graces = GRACE_RE.findall(source)
    if len(graces) != 1:
        fails.append(
            "C7: expected exactly 1 grace_ticks min_t expression, found "
            + str(len(graces))
        )
    elif int(graces[0]) != expect_grace_cap:
        fails.append(
            "C7: grace cap is " + graces[0] + "u, caller requires "
            + str(expect_grace_cap) + "u"
        )

    config3 = CONFIG3_WRITE_RE.findall(source)
    if len(config3) != 1:
        fails.append(
            "C8: expected exactly 1 write to TIMG_WDTCONFIG3, found "
            + str(len(config3))
        )
    elif config3[0] != "timeout_ticks":
        fails.append(
            "C8: TIMG_WDTCONFIG3 (stage 1 / hardware-reset threshold) is "
            "written with '" + config3[0] + "', not 'timeout_ticks' -- this "
            "register holds an ABSOLUTE tick count from the last feed, not a "
            "per-stage duration (every wdt_hal_config_stage() call site in "
            "vendor/esp-idf sets stage N's value to N times the base period). "
            "Writing 'grace_ticks' makes stage 1's threshold smaller than "
            "stage 0's, so the hardware reset can fire before the pretimeout "
            "interrupt ever can -- the capsule stays empty regardless of "
            "handler ordering."
        )

    return fails


def patch_postimage(text):
    """Reconstruct the post-image of a unified diff (context + added lines)."""
    out = []
    in_hunk = False
    for line in text.splitlines():
        if line.startswith("@@"):
            in_hunk = True
            continue
        if not in_hunk:
            continue
        if line.startswith(("--- ", "+++ ", "diff --git ", "index ")):
            in_hunk = False
            continue
        if line.startswith("-"):
            continue
        if line.startswith("+") or line.startswith(" "):
            out.append(line[1:])
        elif line == "":
            out.append("")
        else:
            in_hunk = False
    return NL.join(out)


# A minimal source that mirrors the repaired 0054 + 0056 shape.  Built with
# chr() for TAB and backslash so no escape has to survive a shell boundary
# (CLAUDE.md 14.21).
GOOD = (
    "static void " + CAPTURE + "(struct esp32p4_wdt *wdt," + NL
    + TAB * 5 + "    struct pt_regs *regs, u32 reason)" + NL
    + "{" + NL
    + TAB + "WRITE_ONCE(capsule->magic, EASYSTICK_CRASH_CAPSULE_MAGIC);" + NL
    + TAB + "wmb();" + NL
    + TAB + COMMIT_STORE + NL
    + TAB * 3 + "   sequence ^ EASYSTICK_CRASH_CAPSULE_COMMIT_XOR);" + NL
    + TAB + 'pr_emerg("EASYSTICK_WDT CAPSULE_COMMIT seq=%u' + BS + 'n", seq);'
    + NL
    + "}" + NL
    + NL
    + "static irqreturn_t " + HANDLER + "(int irq, void *data)" + NL
    + "{" + NL
    + TAB + "struct esp32p4_wdt *wdt = data;" + NL
    + NL
    + TAB + CAPSULE_CALL + "wdt, get_irq_regs()," + NL
    + TAB * 4 + "EASYSTICK_CRASH_CAPSULE_REASON_WDT);" + NL
    + NL
    + TAB + PRETIMEOUT_PRINT + BS + 'n");' + NL
    + "#ifdef CONFIG_STACKTRACE" + NL
    + TAB + "dump_stack();" + NL
    + "#endif" + NL
    + TAB + "esp32p4_wdt_unlock(wdt);" + NL
    + NL
    + TAB + "return IRQ_HANDLED;" + NL
    + "}" + NL
    + NL
    + TAB + "grace_ticks = min_t(u32, 30000u, timeout_ticks / 2u);" + NL
    + TAB + "writel(pretimeout_ticks, wdt->base + TIMG_WDTCONFIG2);" + NL
    + TAB + "writel(timeout_ticks, wdt->base + TIMG_WDTCONFIG3);" + NL
)

CAPSULE_STMT = (
    TAB + CAPSULE_CALL + "wdt, get_irq_regs()," + NL
    + TAB * 4 + "EASYSTICK_CRASH_CAPSULE_REASON_WDT);"
)
PRINT_STMT = TAB + PRETIMEOUT_PRINT + BS + 'n");'


def selftest():
    """Positive case plus at least one negative case per check.

    CLAUDE.md 14.2: a checker never observed failing is not evidence.
    """
    cases = [
        ("positive: repaired handler", GOOD, True, 30000, None),
        (
            "C1: handler definition renamed away",
            GOOD.replace(HANDLER, "some_other_handler"),
            True, 30000, "C1",
        ),
        (
            "C2: capsule call removed from handler",
            GOOD.replace(CAPSULE_STMT, TAB + "(void)wdt;"),
            True, 30000, "C2",
        ),
        (
            "C3: the S0 defect -- print before capsule",
            GOOD.replace(
                CAPSULE_STMT + NL + NL + PRINT_STMT,
                PRINT_STMT + NL + CAPSULE_STMT,
            ),
            True, 30000, "C3",
        ),
        (
            "C3: dump_stack hoisted above the capsule",
            GOOD.replace(
                CAPSULE_STMT,
                TAB + "dump_stack();" + NL + CAPSULE_STMT,
            ),
            True, 30000, "C3",
        ),
        (
            "C4: PRETIMEOUT marker print deleted",
            GOOD.replace(PRINT_STMT + NL, ""),
            True, 30000, "C4",
        ),
        (
            "C5: dump_stack absent while stacktrace requested",
            GOOD.replace(TAB + "dump_stack();" + NL, ""),
            True, 30000, "C5",
        ),
        (
            "C6: printk before the commit store inside capture",
            GOOD.replace(
                TAB + "WRITE_ONCE(capsule->magic, "
                "EASYSTICK_CRASH_CAPSULE_MAGIC);",
                TAB + 'pr_info("capturing' + BS + 'n");',
            ),
            True, 30000, "C6",
        ),
        (
            "C7: grace cap regressed to the 1 s window",
            GOOD.replace("30000u", "1000u"),
            True, 30000, "C7",
        ),
        (
            "C7: caller requires a cap the source does not state",
            GOOD, True, 1000, "C7",
        ),
        (
            "C8: the 2026-08-29 defect -- stage 1 armed with grace_ticks",
            GOOD.replace(
                "writel(timeout_ticks, wdt->base + TIMG_WDTCONFIG3);",
                "writel(grace_ticks, wdt->base + TIMG_WDTCONFIG3);",
            ),
            True, 30000, "C8",
        ),
        (
            "C8: TIMG_WDTCONFIG3 write missing entirely",
            GOOD.replace(
                TAB + "writel(timeout_ticks, wdt->base + TIMG_WDTCONFIG3);"
                + NL, "",
            ),
            True, 30000, "C8",
        ),
    ]
    rc = 0
    for name, src, st, cap, expect in cases:
        fails = check(src, st, cap)
        if expect is None:
            ok = not fails
        else:
            ok = any(f.startswith(expect + ":") for f in fails)
        print(("PASS  " if ok else "FAIL  ") + name)
        if not ok:
            rc = 1
            if not fails:
                print("        got: no failure at all, expected " + str(expect))
            for f in fails:
                print("        got: " + f)
    print("SELFTEST " + ("OK" if rc == 0 else "FAILED"))
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", type=Path, help="post-apply C source file")
    ap.add_argument("--patch", type=Path,
                    help="unified diff; its post-image is checked")
    ap.add_argument("--stacktrace", type=int, choices=(0, 1), default=0)
    ap.add_argument("--expect-grace-cap", type=int, default=None,
                    help="required grace_ticks cap, stated by the caller")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.expect_grace_cap is None:
        print("assert_pretimeout_order: --expect-grace-cap is required",
              file=sys.stderr)
        return 2
    if bool(args.source) == bool(args.patch):
        print("assert_pretimeout_order: give exactly one of --source / --patch",
              file=sys.stderr)
        return 2

    if args.source:
        text = args.source.read_text(encoding="utf-8", errors="replace")
        label = str(args.source)
    else:
        text = patch_postimage(
            args.patch.read_text(encoding="utf-8", errors="replace")
        )
        label = str(args.patch) + " (post-image)"

    fails = check(text, bool(args.stacktrace), args.expect_grace_cap)
    if fails:
        print("PRETIMEOUT_ORDER_FAIL " + label, file=sys.stderr)
        for f in fails:
            print("  " + f, file=sys.stderr)
        return 1
    print("PRETIMEOUT_ORDER_OK " + label
          + " stacktrace=" + str(args.stacktrace)
          + " grace_cap=" + str(args.expect_grace_cap))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
