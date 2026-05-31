package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_install_after_jit feature (area: hooks / deopt-on-install).
 *
 * Proves the headline behaviour the README promises for the
 * "install a hook on an ALREADY-JIT-compiled method" path on a LIVE JVM:
 *
 *   - a method (hot(int)) is first warmed to JIT compilation BEFORE any hook is
 *     installed (a tight hot loop drives HotSpot to publish Method::_code != null),
 *   - the native module then installs a vmhook::hook<T> on that warm method.  At
 *     install time vmhook observes _code != null ("is JIT-compiled") and must
 *     DEOPTIMISE the method back to the interpreter — clear Method::_code and
 *     redirect the entry points to the c2i adapter / i2i stub — so the patched
 *     i2i stub actually takes effect,
 *   - driving hot() once more afterward FIRES the detour (the deopt routed the
 *     freshly-resolving dispatch through the interpreter and our patch), the
 *     detour sees the correct receiver + decoded arg, and (non-cancelling)
 *     allow-through leaves the original body result intact,
 *   - a CANCELLING detour can force a return value even on the formerly-compiled
 *     method (proves we own the dispatch, not just observe it),
 *   - after the hook is removed (shutdown_hooks), hot() runs normally again and
 *     the detour does NOT fire.
 *
 * Canonical go/done handshake.  `done` LATCHES, so each scenario the native side
 * wants observed must complete inside ONE run() invocation; the module selects
 * the scenario via `mode` before raising `go`, then reads back the recorded
 * counters.  The native side ORDERS the phases: warm (mode 1/3) happens with NO
 * hook armed, install happens on the native thread BETWEEN probe cycles, then the
 * single-call probes (mode 2/4) drive the post-install / post-removal dispatch.
 *
 * The hot-loop count is intentionally large but bounded so a single run()
 * completes well within the native probe's poll window, and so HotSpot has ample
 * budget to compile hot() before the native side installs the hook.
 */
public final class HookAfterJit
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Native sets this BEFORE raising
     * `go` so one probe cycle drives exactly the calls about to be asserted on.
     *   1 = WARM hot() WARM_CALLS times in a tight loop, NO hook installed yet
     *       (drives HotSpot to JIT-compile hot() so Method::_code becomes non-null
     *        BEFORE the native side installs its hook),
     *   2 = call hot() ONCE with HOT_DELTA (post-install: detour must fire once,
     *       correct self + arg; allow-through OR forced-return per native control),
     *   3 = WARM hot() WARM_CALLS times again (a second JIT window; used by native
     *       to re-establish _code != null if the first warm did not stick, still
     *       BEFORE the hook is installed),
     *   4 = call hot() ONCE with HOT_DELTA (post-removal: detour must NOT fire,
     *       original body still runs).
     */
    public static volatile int mode;

    /** Seed for the hot() instance method; hot(delta) returns seed + delta. */
    private int seed = SEED;

    /** Last value the original hot() body computed (allow-through proof). */
    public static volatile int lastHotResult;

    /** XOR accumulator of every hot() return inside one probe (defeats DCE). */
    public static volatile long hotResultXor;

    /** Number of times run() actually invoked hot() in the last probe cycle. */
    public static volatile int hotCallsMade;

    // ---- Constants mirrored on the native side ----------------------------

    /** Instance seed; hot(delta) returns seed + delta. */
    public static final int SEED = 1000;

    /** The delta fed to hot() on the single-call probes (modes 2 / 4). */
    public static final int HOT_DELTA = 7;

    /**
     * Iterations for the JIT-warming hot loop (modes 1 / 3).  Comfortably above
     * the default C1/C2 thresholds (~1500-10000 depending on JDK / tiered) so
     * HotSpot compiles hot() and publishes Method::_code — the precondition for
     * the "install on an already-JIT'd method" scenario.  Because NO hook is
     * armed during these warm loops, nothing inhibits the compilation.
     */
    public static final int WARM_CALLS = 200000;

    // ---- Hookable method --------------------------------------------------

    /**
     * Hookable instance method.  Deliberately tiny and side-effect-light so
     * HotSpot is eager to compile it once it goes hot.  Returns seed + delta.
     */
    public int hot(final int delta)
    {
        return this.seed + delta;
    }

    private static void runHotOnce(final int delta)
    {
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        final int r = obj.hot(delta);
        lastHotResult = r;
        hotResultXor = r;
        hotCallsMade = 1;
    }

    private static void runHotLoop(final int iterations)
    {
        final HookAfterJit obj = new HookAfterJit();
        obj.seed = SEED;
        int made = 0;
        long acc = 0;
        int last = 0;
        for (int i = 0; i < iterations; ++i)
        {
            // Vary the delta a little so the JIT can't fold the whole loop to a
            // constant, but keep it cheap.  The low 8 bits keep the result
            // bounded and the XOR meaningful.
            final int d = i & 0xFF;
            last = obj.hot(d);
            acc ^= last;
            ++made;
        }
        lastHotResult = last;
        hotResultXor = acc;
        hotCallsMade = made;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookAfterJit.go && !HookAfterJit.done;
            }

            @Override
            public void run()
            {
                switch (HookAfterJit.mode)
                {
                    case 1:
                        runHotLoop(WARM_CALLS);
                        break;
                    case 2:
                        runHotOnce(HOT_DELTA);
                        break;
                    case 3:
                        runHotLoop(WARM_CALLS);
                        break;
                    case 4:
                        runHotOnce(HOT_DELTA);
                        break;
                    default:
                        break;
                }
                HookAfterJit.done = true;
            }
        });
    }
}
