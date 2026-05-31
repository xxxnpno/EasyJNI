package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_unhook_double_free feature (area: hooks / lifecycle).
 *
 * Drives the LOW-LEVEL hook install / uninstall lifecycle on a LIVE JVM and
 * proves the "exactly-once teardown" contract has no use-after-free and no
 * double-restore corruption.  The native module exercises every transition the
 * audit/findings/hook_unhook_double_free_safety.md scenario calls out:
 *
 *   - install a hook on target()           -> it FIRES;
 *   - remove it (handle.stop())            -> the ORIGINAL body runs byte-exact;
 *   - remove AGAIN (second stop())         -> a safe no-op, no crash / double-free;
 *   - re-install on the same method        -> it FIRES again (entry fully cleared);
 *   - install the SAME method twice        -> remove must not corrupt the method;
 *   - install on target() AND other(),
 *     remove target() only                 -> other() still fires, target() does not;
 *   - after teardown the method's behaviour is EXACTLY the original.
 *
 * Every hookable method is a PURE, deterministic function of its argument and the
 * instance `seed` (no hidden state a hook could perturb), so the native side can
 * assert the result is byte-exact-original after every remove.  Each method has a
 * DISTINCT formula so the native side can tell which body actually ran:
 *   target(d)       = seed + d
 *   other(d)        = (seed * 2) + d        (different shape, different Method*)
 *   staticTarget(d) = d * 3                 (static, no `this`)
 *
 * Canonical go/done handshake: the native module sets `mode`, raises `go`, the
 * Harness loop runs run() on the Java thread (genuine bytecode dispatch — what
 * makes an interpreter hook fire), then the module polls the latched `done`.
 * Because `done` latches, every Java call a single assertion needs happens in one
 * run() invocation; the module selects the scenario via `mode`.
 */
public final class HookUnhook
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which method(s) run() drives.  The native module sets this BEFORE
     * raising go so one probe cycle drives exactly the calls about to be asserted.
     *   1 = instance target(int) called TARGET_CALLS times
     *       (the primary install/remove/re-install method; used while-hooked AND
     *        after-teardown — same code path, the native side compares fire counts
     *        and the byte-exact original result)
     *   2 = instance other(int) once          (a SECOND distinct Method* for the
     *                                           "remove A only, B still fires" test)
     *   3 = target(int) once AND other(int) once in the SAME run()
     *       (two-method install: removing target's hook must leave other's intact)
     *   4 = static staticTarget(int) once     (static-method shape variant)
     */
    public static volatile int mode;

    // ---- Instance scenario data -------------------------------------------

    /** Seed for the instance; target(d) returns seed + d, other(d) returns seed*2 + d. */
    private int seed = SEED;

    /** Last value the original target() body computed (byte-exact-original proof). */
    public static volatile int lastTargetResult;

    /** Last value the original other() body computed. */
    public static volatile int lastOtherResult;

    /** Last value the original staticTarget() body computed. */
    public static volatile int lastStaticTargetResult;

    /** How many times run() actually invoked the instance target(). */
    public static volatile int targetCallsMade;

    /** How many times run() actually invoked the instance other(). */
    public static volatile int otherCallsMade;

    /** How many times run() actually invoked the static staticTarget(). */
    public static volatile int staticTargetCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; target / other fold this into their result. */
    public static final int SEED = 7000;

    /** How many times mode 1 drives target() in a single run() (exactly-once-per-call). */
    public static final int TARGET_CALLS = 3;

    /** The exact delta each method is fed (same every call so the result is stable). */
    public static final int TARGET_DELTA = 17;
    public static final int OTHER_DELTA = 29;
    public static final int STATIC_TARGET_DELTA = 41;

    // ---- Hookable methods (pure functions; DISTINCT formula each) ----------

    /** Primary hookable instance method.  Returns seed + delta. */
    public int target(final int delta)
    {
        return this.seed + delta;
    }

    /** Second distinct instance method.  Returns (seed * 2) + delta. */
    public int other(final int delta)
    {
        return (this.seed * 2) + delta;
    }

    /** Static hookable method (no `this`).  Returns delta * 3. */
    public static int staticTarget(final int delta)
    {
        return delta * 3;
    }

    // ---- run() drivers (one per scenario) ----------------------------------

    private static void runTarget()
    {
        final HookUnhook obj = new HookUnhook();
        obj.seed = SEED;
        int made = 0;
        int last = 0;
        for (int n = 0; n < TARGET_CALLS; ++n)
        {
            last = obj.target(TARGET_DELTA);
            ++made;
        }
        lastTargetResult = last;
        targetCallsMade = made;
    }

    private static void runOther()
    {
        final HookUnhook obj = new HookUnhook();
        obj.seed = SEED;
        lastOtherResult = obj.other(OTHER_DELTA);
        otherCallsMade = 1;
    }

    private static void runBoth()
    {
        final HookUnhook obj = new HookUnhook();
        obj.seed = SEED;
        lastTargetResult = obj.target(TARGET_DELTA);
        targetCallsMade = 1;
        lastOtherResult = obj.other(OTHER_DELTA);
        otherCallsMade = 1;
    }

    private static void runStaticTarget()
    {
        lastStaticTargetResult = staticTarget(STATIC_TARGET_DELTA);
        staticTargetCallsMade = 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookUnhook.go && !HookUnhook.done;
            }

            @Override
            public void run()
            {
                switch (HookUnhook.mode)
                {
                    case 1:
                        runTarget();
                        break;
                    case 2:
                        runOther();
                        break;
                    case 3:
                        runBoth();
                        break;
                    case 4:
                        runStaticTarget();
                        break;
                    default:
                        break;
                }
                HookUnhook.done = true;
            }
        });
    }
}
