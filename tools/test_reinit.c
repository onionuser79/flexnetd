/*
 * test_reinit.c — pins ce_reinit_answer(): when a CE type-0 INIT on a
 * running session is answered with ours.
 *
 * Build + run (no libax25, no root):
 *     make test_reinit
 */
#include <stdio.h>

#include "flexnetd.h"

static int failures = 0, checks = 0;

static void ok(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

int main(void)
{
    /* The peer's first INIT is the other half of the session handshake. */
    ok(!ce_reinit_answer(false, 1000, 1000), "first INIT, same second");
    ok(!ce_reinit_answer(false, 1000, 9000), "first INIT, late");
    ok(!ce_reinit_answer(false, 0, 9000), "first INIT, ours never stamped");

    /* 2026-10-12: -14 re-INITs a session that has run for days. */
    ok(ce_reinit_answer(true, 1000, 1000 + 4 * 86400), "re-INIT days later");
    ok(ce_reinit_answer(true, 0, 9000), "re-INIT, ours never stamped");
    ok(ce_reinit_answer(true, 1000, 1000 + CE_REINIT_GUARD_S),
       "re-INIT exactly at the guard");

    /* Guard: no answer to the answer, nor to the peer's quick repeat. */
    ok(!ce_reinit_answer(true, 1000, 1000), "answered this second");
    ok(!ce_reinit_answer(true, 1000, 1000 + CE_REINIT_GUARD_S - 1),
       "one second inside the guard");
    ok(!ce_reinit_answer(true, 1000, 1016), "peer's 16 s repeat");

    /* Wall clock stepped backwards: never wedge the answer off. */
    ok(ce_reinit_answer(true, 5000, 1000), "now before last_init_tx");

    printf("test_reinit: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
