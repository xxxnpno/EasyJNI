package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_basic feature (area: hooks).
 *
 * Exercises vmhook::hook<T> / scoped_hook installed on BOTH an instance method
 * and a static method, and proves on a real JVM bytecode dispatch that:
 *   - the detour fires exactly once per Java call (the probe makes a known,
 *     deterministic number of calls; the native module asserts the fire count),
 *   - the detour sees the correct receiver (`self`) — verified by reading the
 *     instance's own `seed` field, and by calling on two DIFFERENT instances
 *     with different seeds,
 *   - the detour decodes every argument correctly across primitive widths and
 *     across the J/D 2-slot boundary (int, long, double, boolean, String),
 *   - the ORIGINAL method body still runs after a non-cancelling detour
 *     (allow-through): the returned/observed values are the unmodified results.
 *
 * The fixture follows the canonical go/done handshake.  The native module
 * raises `go`, the Harness loop runs `run()` on the Java thread (so the
 * interpreter hook fires on genuine bytecode dispatch), then the module polls
 * `done`.  Because `done` latches, every Java call the module wants observed
 * must happen inside a SINGLE `run()` invocation; the module selects which
 * scenario to run via the `mode` selector below and reads back the recorded
 * observations from the public fields.
 */
public final class HookBasic
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this
     * BEFORE raising `go` so a single probe cycle drives exactly the calls the
     * module is about to assert on.
     *   1 = instance touch() called INSTANCE_CALLS times (exactly-once + self + arg + allow-through)
     *   2 = static staticTouch() called STATIC_CALLS times
     *   3 = instance combine(int,long,int)        (multi-slot arg decode + self)
     *   4 = static  staticCombine(int,long,int)   (multi-slot arg decode, no self)
     *   5 = instance two-different-instances touch (self is the CORRECT instance)
     *   6 = instance wideArgs(boolean,double,String,int) (boolean/double/String decode + self)
     *   7 = instance touch() ONE call, used AFTER the module dropped its handle
     *       (proves the scoped_hook uninstalled — detour must NOT fire)
     */
    public static volatile int mode;

    // ---- Instance scenario data -------------------------------------------

    /** Seed for the primary instance; touch(delta) returns seed + delta. */
    private int seed = 1000;

    /** Last value the original touch() body computed (allow-through proof). */
    public static volatile int lastTouchResult;

    /** Running sum of every touch() return across one probe cycle. */
    public static volatile long touchResultSum;

    /** Number of times run() actually invoked the instance touch(). */
    public static volatile int instanceCallsMade;

    /** Number of times run() actually invoked the static staticTouch(). */
    public static volatile int staticCallsMade;

    /** combine()'s last original return (a + b + c). */
    public static volatile long combineResult;

    /** staticCombine()'s last original return. */
    public static volatile long staticCombineResult;

    /** wideArgs()'s last original return. */
    public static volatile double wideResult;

    /** Seeds of the two instances used by mode 5 (so native can cross-check self). */
    public static volatile int instanceASeed;
    public static volatile int instanceBSeed;

    /** Original results from the two-instance scenario. */
    public static volatile int twoInstanceResultA;
    public static volatile int twoInstanceResultB;

    /** How many calls each mode is expected to drive (mirrored on native side). */
    public static final int INSTANCE_CALLS = 3;
    public static final int STATIC_CALLS = 4;

    /** The exact delta values mode 1 feeds touch(), in order. */
    public static final int TOUCH_DELTA_0 = 7;
    public static final int TOUCH_DELTA_1 = 11;
    public static final int TOUCH_DELTA_2 = 42;

    /** The exact arg mode 2 feeds staticTouch(). */
    public static final int STATIC_DELTA = 99;

    /** Multi-slot scenario constants (modes 3/4). */
    public static final int COMBINE_A = 5;
    public static final long COMBINE_B = 0x1122334455667788L;
    public static final int COMBINE_C = -13;

    /** Two-instance scenario constants (mode 5). */
    public static final int SEED_A = 2000;
    public static final int SEED_B = 30000;
    public static final int DELTA_A = 3;
    public static final int DELTA_B = 4;

    /** Wide-arg scenario constants (mode 6). */
    public static final boolean WIDE_FLAG = true;
    public static final double WIDE_D = 2.5;
    public static final String WIDE_S = "vmhook";
    public static final int WIDE_I = 77;

    // ---- Hookable methods -------------------------------------------------

    /** Hookable instance method.  Returns seed + delta. */
    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    /** Hookable static method.  Returns delta * 2. */
    public static int staticTouch(final int delta)
    {
        return delta * 2;
    }

    /**
     * Multi-slot instance method: a long sits between two ints, so a correct
     * decoder must widen the long across two interpreter slots and still read
     * the trailing int from the right slot.
     */
    public long combine(final int a, final long b, final int c)
    {
        return this.seed + a + b + c;
    }

    /** Static twin of combine(): no `this`, so the first int is at slot 0. */
    public static long staticCombine(final int a, final long b, final int c)
    {
        return a + b + c;
    }

    /**
     * Wide-argument instance method exercising boolean / double / String /
     * int decode together (double consumes two slots; String is a reference).
     */
    public double wideArgs(final boolean flag, final double d, final String s, final int i)
    {
        final int slen = (s == null) ? -1 : s.length();
        return (flag ? 1.0 : 0.0) + d + slen + i;
    }

    private static void runInstanceTouch()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        int made = 0;
        long sum = 0;
        int r0 = obj.touch(TOUCH_DELTA_0);
        sum += r0; ++made;
        int r1 = obj.touch(TOUCH_DELTA_1);
        sum += r1; ++made;
        int r2 = obj.touch(TOUCH_DELTA_2);
        sum += r2; ++made;
        lastTouchResult = r2;
        touchResultSum = sum;
        instanceCallsMade = made;
    }

    private static void runStaticTouch()
    {
        int made = 0;
        int last = 0;
        for (int n = 0; n < STATIC_CALLS; ++n)
        {
            last = staticTouch(STATIC_DELTA);
            ++made;
        }
        lastTouchResult = last;
        staticCallsMade = made;
    }

    private static void runCombine()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 1000;
        combineResult = obj.combine(COMBINE_A, COMBINE_B, COMBINE_C);
    }

    private static void runStaticCombine()
    {
        staticCombineResult = staticCombine(COMBINE_A, COMBINE_B, COMBINE_C);
    }

    private static void runTwoInstances()
    {
        final HookBasic a = new HookBasic();
        a.seed = SEED_A;
        final HookBasic b = new HookBasic();
        b.seed = SEED_B;
        instanceASeed = a.seed;
        instanceBSeed = b.seed;
        twoInstanceResultA = a.touch(DELTA_A);
        twoInstanceResultB = b.touch(DELTA_B);
    }

    private static void runWideArgs()
    {
        final HookBasic obj = new HookBasic();
        obj.seed = 0;
        wideResult = obj.wideArgs(WIDE_FLAG, WIDE_D, WIDE_S, WIDE_I);
    }

    private static void runUninstallProbe()
    {
        // One plain touch() call.  By the time the module raises mode 7 it has
        // already dropped the scoped_hook handle, so the detour must NOT fire,
        // yet the original body must still run normally.
        final HookBasic obj = new HookBasic();
        obj.seed = 500;
        lastTouchResult = obj.touch(1);
        instanceCallsMade = 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookBasic.go && !HookBasic.done;
            }

            @Override
            public void run()
            {
                switch (HookBasic.mode)
                {
                    case 1:
                        runInstanceTouch();
                        break;
                    case 2:
                        runStaticTouch();
                        break;
                    case 3:
                        runCombine();
                        break;
                    case 4:
                        runStaticCombine();
                        break;
                    case 5:
                        runTwoInstances();
                        break;
                    case 6:
                        runWideArgs();
                        break;
                    case 7:
                        runUninstallProbe();
                        break;
                    default:
                        break;
                }
                HookBasic.done = true;
            }
        });
    }
}
