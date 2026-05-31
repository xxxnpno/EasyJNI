package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the return_caller feature (area: hooks).
 *
 * The feature under test is, FROM INSIDE A HOOK on a leaf method:
 *   - {@code return_value::caller()} reports the IMMEDIATE calling method
 *     (class_name / method_name / signature) when that caller is an
 *     interpreted HotSpot frame, and {@code valid()==false} when the
 *     immediate caller is compiled (JIT) or native, and
 *   - {@code return_value::stack_trace()} returns the interpreted frames in
 *     order, with the immediate caller at index 0 and progressively older
 *     interpreter frames after it.
 *
 * Because {@code caller()} / {@code stack_trace()} walk the LIVE interpreter
 * saved-rbp chain at the moment the detour fires, the entire call chain must
 * execute as real Java bytecode on the Java thread.  The native side therefore
 * never builds frames itself: it hooks {@link #inner(int)} (always the same
 * leaf, signature {@code (I)I}) and then drives this fixture to build different
 * caller chains above that leaf via the {@code mode} selector.  The detour
 * records, for each invocation, what {@code caller()} and {@code stack_trace()}
 * reported; the native module reads those recordings back and asserts on them.
 *
 * The leaf {@code inner(int)} keeps a fixed name+descriptor on purpose: only
 * the CALLER varies between scenarios, so every assertion about caller-identity
 * is attributable to the walk and not to which method was hooked.
 *
 * Scenario selector (native sets {@code mode} + clears {@code done} on the
 * rising edge of {@code go}; each scenario is one probe cycle):
 *   1  = depth-2 chain  outerA(int) -> inner(int)
 *        (immediate caller is outerA, interpreted)
 *   2  = depth-3 chain  outerB(int) -> middle(int) -> inner(int)
 *        (immediate caller is middle; outerB sits one frame deeper)
 *   3  = deep self-recursion recurse(int) calling inner at the leaf, depth
 *        well beyond the default stack_trace cap (so truncation + the
 *        max_depth contract + "no infinite loop" can be exercised)
 *   4  = caller with a LONG reference-heavy descriptor longSig(...)->inner
 *        (proves signature is not truncated)
 *   5  = caller forced to JIT-compile: warmCaller(int) is invoked in a hot
 *        loop ABOVE the C2 threshold first, THEN once more to fire the leaf;
 *        the immediate caller frame is then (very likely) compiled, so
 *        caller().valid() must be false / the trace must omit it
 *   6  = TWO DIFFERENT interpreted callers in one cycle: alpha(int)->inner
 *        then beta(int)->inner, so the native side proves caller() reports
 *        the correct DISTINCT caller each fire (not a stale cache)
 *   7  = caller with a primitive-but-non-(I)I descriptor: longArgCaller(long)
 *        -> inner, so the reported signature is "(J)I" not "(I)I"
 *
 * Java 8 syntax only (no var / records / switch-expr / text-blocks).
 */
public final class ReturnCaller
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector (native programs this before raising go). */
    public static volatile int mode;

    /** Observable leaf side effect (sum of inner() arguments this cycle). */
    public static volatile int observed;

    /** How many times inner() actually ran this cycle (handshake proof). */
    public static volatile int innerCalls;

    /**
     * Iteration count for the JIT-warmup scenario (mode 5).  Large enough to
     * push warmCaller well past the default tiered C1/C2 compile thresholds
     * (C2 default ~10000 invocations) so the method is compiled by the time
     * the LAST call drives the leaf.  The native side does NOT depend on the
     * exact value — only that it is "hot enough".
     */
    public static final int WARMUP_ITERATIONS = 200000;

    /** Expected number of distinct leaf calls for the recursion scenario. */
    public static final int RECURSION_DEPTH = 90;

    /** Constant args used by the various callers (mirrored on native side). */
    public static final int  ARG_OUTER_A   = 11;
    public static final int  ARG_OUTER_B   = 12;
    public static final int  ARG_LONGSIG   = 13;
    public static final int  ARG_WARM      = 14;
    public static final int  ARG_ALPHA     = 15;
    public static final int  ARG_BETA      = 16;
    public static final long ARG_LONG      = 17L;

    // ---- The fixed leaf -----------------------------------------------------

    /**
     * The single hooked leaf.  Name + descriptor are FIXED at {@code (I)I} so
     * only the caller above it varies between scenarios.  Kept tiny so HotSpot
     * leaves it interpreted (the native detour replaces its entry anyway).
     */
    public int inner(final int x)
    {
        innerCalls++;
        observed += x;
        return x * 2;
    }

    // ---- mode 1: depth-2 interpreted caller ---------------------------------

    /** Immediate interpreted caller for mode 1. */
    public int outerA(final int x)
    {
        return this.inner(x + 1) + 1;
    }

    // ---- mode 2: depth-3 interpreted chain ----------------------------------

    /** Outer frame for mode 2 (one level deeper than the immediate caller). */
    public int outerB(final int x)
    {
        return this.middle(x + 1) + 1;
    }

    /** Immediate interpreted caller for mode 2 (sits between outerB and inner). */
    public int middle(final int x)
    {
        return this.inner(x + 1) + 1;
    }

    // ---- mode 3: deep recursion ---------------------------------------------

    /**
     * Self-recursion: descends {@code depth} frames, then calls inner at the
     * bottom.  Every recursive frame has the SAME name+descriptor, so the
     * native side can assert that the deep portion of the trace is uniform and
     * that the walk stops cleanly at max_depth without spinning.
     */
    public int recurse(final int depth)
    {
        if (depth <= 0)
        {
            return this.inner(1);
        }
        return this.recurse(depth - 1) + 1;
    }

    // ---- mode 4: long reference-heavy descriptor ----------------------------

    /**
     * Caller whose JVM descriptor is deliberately long: eight Object params
     * plus a trailing int yields
     *   (Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;
     *    Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;I)I
     * which is > 140 chars — far past anything a fixed buffer would hold.  The
     * native side mirrors the exact expected descriptor string.
     */
    public int longSig(final Object a, final Object b, final Object c, final Object d,
                       final Object e, final Object f, final Object g, final Object h,
                       final int x)
    {
        // touch every ref so javac cannot elide the params
        final int n = (a != null ? 1 : 0) + (b != null ? 1 : 0) + (c != null ? 1 : 0)
                    + (d != null ? 1 : 0) + (e != null ? 1 : 0) + (f != null ? 1 : 0)
                    + (g != null ? 1 : 0) + (h != null ? 1 : 0);
        return this.inner(x + n);
    }

    // ---- mode 5: JIT-compiled immediate caller ------------------------------

    /**
     * Caller that the probe drives in a hot loop so HotSpot compiles it.  The
     * loop body returns a value the JIT cannot constant-fold away (depends on
     * the loop counter), and the FINAL call (the one that actually reaches the
     * leaf) happens after the method is hot — so the immediate caller frame on
     * the stack at the moment the detour fires is a COMPILED frame.  The
     * feature contract: caller() is invalid for a compiled immediate caller.
     */
    public int warmCaller(final int x)
    {
        // Only the sentinel value drives the leaf; all warmup iterations do
        // pure arithmetic so they stay cheap but still trip the counters.
        if (x < 0)
        {
            return this.inner(ARG_WARM);
        }
        return (x * 31) ^ (x + 7);
    }

    // ---- mode 6: two distinct interpreted callers ---------------------------

    /** First distinct interpreted caller for mode 6. */
    public int alpha(final int x)
    {
        return this.inner(x) + 1;
    }

    /** Second distinct interpreted caller for mode 6. */
    public int beta(final int x)
    {
        return this.inner(x) + 2;
    }

    // ---- mode 7: caller with a (J)I descriptor ------------------------------

    /** Caller whose descriptor is (J)I, so caller().signature must reflect J. */
    public int longArgCaller(final long v)
    {
        return this.inner((int) v);
    }

    // ---- scenario runners ---------------------------------------------------

    private void runOuterA()
    {
        observed = 0;
        innerCalls = 0;
        this.outerA(ARG_OUTER_A);
    }

    private void runOuterBMiddle()
    {
        observed = 0;
        innerCalls = 0;
        this.outerB(ARG_OUTER_B);
    }

    private void runRecursion()
    {
        observed = 0;
        innerCalls = 0;
        this.recurse(RECURSION_DEPTH);
    }

    private void runLongSig()
    {
        observed = 0;
        innerCalls = 0;
        final Object o = new Object();
        this.longSig(o, o, o, o, o, o, o, o, ARG_LONGSIG);
    }

    private void runWarmCaller()
    {
        observed = 0;
        innerCalls = 0;
        // Heat warmCaller well past the C2 threshold with non-folding inputs.
        int sink = 0;
        for (int i = 0; i < WARMUP_ITERATIONS; i++)
        {
            sink += this.warmCaller(i);
        }
        // Publish the sink so the JIT cannot dead-code the whole loop.
        ReturnCaller.warmSink = sink;
        // Now the one call that actually reaches the leaf, from the (now
        // compiled) warmCaller frame.
        this.warmCaller(-1);
    }

    private void runTwoCallers()
    {
        observed = 0;
        innerCalls = 0;
        this.alpha(ARG_ALPHA);
        this.beta(ARG_BETA);
    }

    private void runLongArgCaller()
    {
        observed = 0;
        innerCalls = 0;
        this.longArgCaller(ARG_LONG);
    }

    /** Keeps the warmup loop from being optimised away. */
    public static volatile int warmSink;

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnCaller.go && !ReturnCaller.done;
            }

            @Override
            public void run()
            {
                final ReturnCaller probe = new ReturnCaller();
                switch (ReturnCaller.mode)
                {
                    case 1:
                        probe.runOuterA();
                        break;
                    case 2:
                        probe.runOuterBMiddle();
                        break;
                    case 3:
                        probe.runRecursion();
                        break;
                    case 4:
                        probe.runLongSig();
                        break;
                    case 5:
                        probe.runWarmCaller();
                        break;
                    case 6:
                        probe.runTwoCallers();
                        break;
                    case 7:
                        probe.runLongArgCaller();
                        break;
                    default:
                        break;
                }
                ReturnCaller.done = true;
            }
        });
    }
}
