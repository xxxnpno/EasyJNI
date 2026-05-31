package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the hook_signature feature (area: hooks).
 *
 * The whole point of vmhook::hook&lt;T&gt;(name, signature, detour) is OVERLOAD
 * SELECTION: a class has SEVERAL methods sharing one name but with different JVM
 * descriptors, and the caller installs the detour on EXACTLY ONE of them by
 * descriptor.  The contract this fixture proves on a real JVM:
 *
 *   - When a probe calls EVERY overload of a name, ONLY the descriptor-selected
 *     overload fires the detour; the sibling overloads run their original body
 *     and do NOT fire it (no cross-fire).
 *   - The selected overload's arguments decode correctly (right slot offsets for
 *     the chosen descriptor — int, long(2 slots), double(2 slots), boolean,
 *     String reference, object reference, int[] array, and trailing-int-after-
 *     long ordering).
 *   - Static overloads are selectable by descriptor with NO implicit `this`.
 *   - The empty-signature overload hook&lt;T&gt;(name, detour) picks the FIRST
 *     declared same-name overload (documented foot-gun) — we record declaration
 *     order so the native side can lock that contract.
 *   - A descriptor that matches NO overload fails the install (returns false),
 *     while a different valid descriptor on the same name still installs.
 *   - Duplicate install on the same name+signature: second call returns true but
 *     the FIRST detour stays active (second is silently dropped) — current,
 *     documented behaviour, locked here.
 *   - scoped_hook variant: a per-descriptor handle uninstalls only its own
 *     overload on scope exit; a sibling overload's hook keeps firing.
 *   - Force-return on the selected overload (retval.set) replaces ONLY that
 *     overload's return; siblings return their unmodified original.
 *
 * Single-run()-per-cycle constraint: `done` latches, so every Java call the
 * module wants observed in one assertion batch happens inside ONE run().  The
 * native module selects the scenario via `mode` and resets `done` on the rising
 * edge of `go` (canonical HookBasic pattern).
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks).
 */
public final class HookSignature
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this BEFORE
     * raising `go`.  Each scenario calls EVERY overload in the relevant family so
     * the module can prove only the selected descriptor fired.
     *   1  = call ALL FOUR process(...) overloads once each   (I / J / D / String)
     *   2  = call process(int) ONLY                            (selected-fires sanity)
     *   3  = call process(long) ONLY
     *   4  = call process(double) ONLY
     *   5  = call process(String) ONLY
     *   6  = call BOTH mix(int,long) and mix(long,int)         (arg-order overloads)
     *   7  = call BOTH combine(int) [arity1] and combine(int,int) [arity2]
     *   8  = call ALL THREE refTake overloads (Object / int[] / String)
     *   9  = call BOTH static stat(int) and stat(long)
     *   10 = call process(int) N times                         (exactly-once-per-call)
     *   11 = call process(int) once + process(long) once       (scoped teardown probe)
     *   12 = call process(int) once + process(long) once       (force-return: only int replaced)
     *   13 = call wide(boolean,double,String,int) ONLY         (full multi-slot decode)
     */
    public static volatile int mode;

    private int seed = 7000;

    // ---- Recorded observations the native side reads back ------------------

    /** Original return of the LAST process(...) overload run, per type. */
    public static volatile int    resI;     // process(int)
    public static volatile long   resJ;     // process(long)
    public static volatile double resD;     // process(double)
    public static volatile int    resStr;   // process(String) -> length-based int
    public static volatile long   resMixIL; // mix(int,long)
    public static volatile long   resMixLI; // mix(long,int)
    public static volatile int    resComb1; // combine(int)
    public static volatile int    resComb2; // combine(int,int)
    public static volatile int    resRefObj;
    public static volatile int    resRefArr;
    public static volatile int    resRefStr;
    public static volatile long   resStatI;
    public static volatile long   resStatJ;
    public static volatile double resWide;

    /** How many process(int) calls mode 10 made. */
    public static volatile int processIntCalls;

    /** Records the order index of each declared overload of `process`
     *  (native uses it only as documentation; declaration order is fixed by
     *  the source below: int, long, double, String). */
    public static final int PROC_DECL_ORDER_FIRST_IS_INT = 1;

    // ---- Scenario constants (mirrored on the native side) ------------------

    public static final int    ARG_I   = 314;
    public static final long   ARG_J   = 0x0BADC0DE0BADC0DEL;   // both 32-bit halves non-zero
    public static final double ARG_D   = 6.875;
    public static final String ARG_STR = "signature";           // length 9
    public static final int    STR_LEN = 9;

    public static final int    MIX_I   = 41;
    public static final long   MIX_J   = 0x7766554433221100L;

    public static final int    COMB_A  = 100;
    public static final int    COMB_B  = 23;

    public static final int    REF_TAG = 555;

    public static final int    STAT_I  = -17;
    public static final long   STAT_J  = 0x00000001FFFFFFFFL;   // > 2^32, needs full long

    public static final boolean WIDE_FLAG = true;
    public static final double  WIDE_D    = 1.25;
    public static final String  WIDE_S    = "wide";             // length 4
    public static final int     WIDE_I    = 88;

    public static final int    PROCESS_INT_CALLS = 5;

    // ---- Overload family A: process(...) — selected by descriptor ----------
    // Declaration order below is load-bearing for the empty-signature test:
    // (I)I is FIRST, so hook<T>("process", cb) must pick the int overload.

    /** process(int): returns seed + v.  Descriptor (I)I. */
    public int process(final int v)
    {
        return this.seed + v;
    }

    /** process(long): returns seed + v.  Descriptor (J)J. */
    public long process(final long v)
    {
        return this.seed + v;
    }

    /** process(double): returns seed + v.  Descriptor (D)D. */
    public double process(final double v)
    {
        return this.seed + v;
    }

    /** process(String): returns seed + length.  Descriptor (Ljava/lang/String;)I. */
    public int process(final String v)
    {
        return this.seed + (v == null ? -1 : v.length());
    }

    // ---- Overload family B: mix(...) — same arity, swapped slot widths -----
    // Proves the slot-offset math is descriptor-specific: (IJ) puts the long at
    // slot 1, (JI) puts the trailing int at slot 2 (long ate slots 0-1).

    /** mix(int,long): returns a + b.  Descriptor (IJ)J. */
    public long mix(final int a, final long b)
    {
        return a + b;
    }

    /** mix(long,int): returns a + b.  Descriptor (JI)J. */
    public long mix(final long a, final int b)
    {
        return a + b;
    }

    // ---- Overload family C: combine(...) — different arity ------------------

    /** combine(int): returns v.  Descriptor (I)I. */
    public int combine(final int v)
    {
        return v;
    }

    /** combine(int,int): returns a + b.  Descriptor (II)I. */
    public int combine(final int a, final int b)
    {
        return a + b;
    }

    // ---- Overload family D: refTake(...) — reference-type overloads ---------

    /** refTake(Object): returns REF_TAG.  Descriptor (Ljava/lang/Object;)I. */
    public int refTake(final Object o)
    {
        return REF_TAG + (o == null ? 0 : 1);
    }

    /** refTake(int[]): returns array length.  Descriptor ([I)I. */
    public int refTake(final int[] a)
    {
        return a == null ? -1 : a.length;
    }

    /** refTake(String): returns length.  Descriptor (Ljava/lang/String;)I. */
    public int refTake(final String s)
    {
        return s == null ? -1 : s.length();
    }

    // ---- Overload family E: static stat(...) — no implicit `this` -----------

    /** stat(int): returns v*2.  Descriptor (I)J. */
    public static long stat(final int v)
    {
        return (long) v * 2L;
    }

    /** stat(long): returns v+1.  Descriptor (J)J. */
    public static long stat(final long v)
    {
        return v + 1L;
    }

    // ---- Wide multi-slot instance method (single descriptor) ---------------

    /** wide(boolean,double,String,int).  Descriptor (ZDLjava/lang/String;I)D. */
    public double wide(final boolean flag, final double d, final String s, final int i)
    {
        final int slen = (s == null) ? -1 : s.length();
        return (flag ? 1.0 : 0.0) + d + slen + i;
    }

    // ---- Scenario drivers --------------------------------------------------

    private static HookSignature fresh()
    {
        final HookSignature obj = new HookSignature();
        obj.seed = 7000;
        return obj;
    }

    private static void runAllProcess()
    {
        final HookSignature o = fresh();
        resI   = o.process(ARG_I);
        resJ   = o.process(ARG_J);
        resD   = o.process(ARG_D);
        resStr = o.process(ARG_STR);
    }

    private static void runProcessInt()    { resI   = fresh().process(ARG_I);   }
    private static void runProcessLong()   { resJ   = fresh().process(ARG_J);   }
    private static void runProcessDouble() { resD   = fresh().process(ARG_D);   }
    private static void runProcessString() { resStr = fresh().process(ARG_STR); }

    private static void runMix()
    {
        final HookSignature o = fresh();
        resMixIL = o.mix(MIX_I, MIX_J);
        resMixLI = o.mix(MIX_J, MIX_I);
    }

    private static void runCombine()
    {
        final HookSignature o = fresh();
        resComb1 = o.combine(COMB_A);
        resComb2 = o.combine(COMB_A, COMB_B);
    }

    private static void runRefTake()
    {
        final HookSignature o = fresh();
        resRefObj = o.refTake(new Object());
        resRefArr = o.refTake(new int[] { 1, 2, 3, 4 });
        resRefStr = o.refTake(ARG_STR);
    }

    private static void runStat()
    {
        resStatI = stat(STAT_I);
        resStatJ = stat(STAT_J);
    }

    private static void runProcessIntRepeated()
    {
        final HookSignature o = fresh();
        int made = 0;
        int last = 0;
        for (int n = 0; n < PROCESS_INT_CALLS; ++n)
        {
            last = o.process(ARG_I);
            ++made;
        }
        resI = last;
        processIntCalls = made;
    }

    private static void runIntThenLong()
    {
        final HookSignature o = fresh();
        resI = o.process(ARG_I);
        resJ = o.process(ARG_J);
    }

    private static void runWide()
    {
        final HookSignature o = fresh();
        o.seed = 0;
        resWide = o.wide(WIDE_FLAG, WIDE_D, WIDE_S, WIDE_I);
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return HookSignature.go && !HookSignature.done;
            }

            @Override
            public void run()
            {
                switch (HookSignature.mode)
                {
                    case 1:  runAllProcess();         break;
                    case 2:  runProcessInt();         break;
                    case 3:  runProcessLong();        break;
                    case 4:  runProcessDouble();      break;
                    case 5:  runProcessString();      break;
                    case 6:  runMix();                break;
                    case 7:  runCombine();            break;
                    case 8:  runRefTake();            break;
                    case 9:  runStat();               break;
                    case 10: runProcessIntRepeated(); break;
                    case 11: runIntThenLong();        break;
                    case 12: runIntThenLong();        break;
                    case 13: runWide();               break;
                    default: break;
                }
                HookSignature.done = true;
            }
        });
    }
}
