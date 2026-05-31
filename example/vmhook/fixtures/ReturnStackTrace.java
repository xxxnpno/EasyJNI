package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the return_value::stack_trace() DEPTH feature (area: hooks).
 *
 * Sibling to {@link ReturnCaller}: that fixture proves caller() identifies the
 * IMMEDIATE caller; this one stresses the MULTI-FRAME walk that
 * {@code return_value::stack_trace(max_depth)} performs from inside a detour,
 * specifically:
 *   - a known-DEPTH interpreter chain outer(int) -> mid(int) -> inner(int) where
 *     inner() is the hooked leaf, so the trace has a KNOWN length and KNOWN
 *     per-frame method/class names in a KNOWN order (immediate caller first):
 *     index 0 == mid, index 1 == outer, and outer is the same frame caller()
 *     would NOT report (caller() only sees mid),
 *   - the {@code max_depth} contract: an explicit small cap truncates the trace
 *     to exactly that many frames (mid-then-outer ordering preserved), and the
 *     documented "pass 0 for the default" promotion returns the default cap
 *     rather than zero frames,
 *   - a DEEP self-recursion deeper than the default 64-frame cap, proving the
 *     walk (a) terminates cleanly at the cap without spinning / AV-ing on the
 *     saved-rbp chain, and (b) the deep portion of the trace is uniform (every
 *     recursive frame shares one name+descriptor), and that an explicit cap
 *     below the real depth truncates exactly,
 *   - per-fire freshness: TWO different chains in one probe cycle (a 2-frame
 *     chain then the 3-frame chain) so the native side proves the second
 *     trace is recomputed live, not a stale copy of the first.
 *
 * Because stack_trace() walks the LIVE interpreter saved-rbp chain at the moment
 * the detour fires, the entire call chain must execute as real Java bytecode on
 * the Java thread.  The native module never builds frames itself: it hooks the
 * fixed leaf {@link #inner(int)} (always (I)I) and drives this fixture to build
 * different chains above the leaf via the {@code mode} selector; the detour
 * records what stack_trace() reported and the module reads it back.
 *
 * The leaf {@code inner(int)} keeps a fixed name+descriptor on purpose, and the
 * intermediate frames {@code mid}/{@code outer} are deliberately TINY so HotSpot
 * keeps the whole chain interpreted long enough to observe the relationship.
 *
 * Scenario selector (native sets {@code mode} + clears {@code done} on the
 * rising edge of {@code go}; each scenario is one probe cycle):
 *   1 = known-depth chain  outer -> mid -> inner   (depth/order/names headline)
 *   2 = same chain, used by the native side to exercise the max_depth CONTRACT
 *       (the chain is identical to mode 1; the native side just calls
 *        stack_trace(1), stack_trace(2), stack_trace(0) inside the detour)
 *   3 = deep self-recursion recurse(N) calling inner at the bottom, N well past
 *       the default 64 cap (truncation + "no spin" + uniform deep frames)
 *   4 = TWO chains in one cycle: a 2-frame shallow(int)->inner THEN the
 *       3-frame outer->mid->inner, so the native side proves the trace for the
 *       second fire is fresh and deeper than the first
 *
 * Java 8 syntax only (no var / records / switch-expr / text-blocks).
 */
public final class ReturnStackTrace
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
     * Recursion depth for mode 3.  Chosen comfortably greater than the default
     * stack_trace cap of 64 so the cap-and-truncate behaviour is exercised, but
     * small enough to stay well within the Java thread stack.  The native side
     * mirrors only the relationship "DEEP_RECURSION > 64", never the exact
     * frame count of the trace (which also includes the run()/probe frames).
     */
    public static final int DEEP_RECURSION = 120;

    /** Constant args used by the chains (mirrored on the native side). */
    public static final int ARG_OUTER   = 100;
    public static final int ARG_SHALLOW = 200;
    public static final int ARG_RECURSE_LEAF = 1;

    // ---- The fixed leaf -----------------------------------------------------

    /**
     * The single hooked leaf.  Name + descriptor are FIXED at {@code (I)I} so
     * only the chain ABOVE it varies between scenarios.  Kept tiny so HotSpot
     * leaves it interpreted (the native detour replaces its entry anyway).
     */
    public int inner(final int x)
    {
        innerCalls++;
        observed += x;
        return x * 2;
    }

    // ---- mode 1 / 2 / 4: the named depth-3 chain outer -> mid -> inner -------

    /** Outermost named frame of the 3-deep chain (index 1 in the trace). */
    public int outer(final int x)
    {
        return this.mid(x + 1) + 1;
    }

    /** Middle named frame: the IMMEDIATE caller of inner (index 0 in trace). */
    public int mid(final int x)
    {
        return this.inner(x + 1) + 1;
    }

    // ---- mode 4: a shallow depth-2 chain shallow -> inner -------------------

    /** Immediate caller for the 2-frame chain (mode 4, first fire). */
    public int shallow(final int x)
    {
        return this.inner(x + 1) + 1;
    }

    // ---- mode 3: deep recursion --------------------------------------------

    /**
     * Self-recursion: descends {@code depth} frames, then calls inner at the
     * bottom.  Every recursive frame has the SAME name+descriptor {@code (I)I},
     * so the native side can assert the deep portion of the trace is uniform and
     * that the walk stops cleanly at the default max_depth without spinning.
     */
    public int recurse(final int depth)
    {
        if (depth <= 0)
        {
            return this.inner(ARG_RECURSE_LEAF);
        }
        return this.recurse(depth - 1) + 1;
    }

    // ---- scenario runners ---------------------------------------------------

    private void runKnownDepth()
    {
        observed = 0;
        innerCalls = 0;
        this.outer(ARG_OUTER);
    }

    private void runDeepRecursion()
    {
        observed = 0;
        innerCalls = 0;
        this.recurse(DEEP_RECURSION);
    }

    private void runTwoChains()
    {
        observed = 0;
        innerCalls = 0;
        // First fire: shallow 2-frame chain.  Second fire: deeper 3-frame chain.
        this.shallow(ARG_SHALLOW);
        this.outer(ARG_OUTER);
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnStackTrace.go && !ReturnStackTrace.done;
            }

            @Override
            public void run()
            {
                final ReturnStackTrace probe = new ReturnStackTrace();
                switch (ReturnStackTrace.mode)
                {
                    case 1:
                        probe.runKnownDepth();
                        break;
                    case 2:
                        // Same chain as mode 1; the native detour varies max_depth.
                        probe.runKnownDepth();
                        break;
                    case 3:
                        probe.runDeepRecursion();
                        break;
                    case 4:
                        probe.runTwoChains();
                        break;
                    default:
                        break;
                }
                ReturnStackTrace.done = true;
            }
        });
    }
}
