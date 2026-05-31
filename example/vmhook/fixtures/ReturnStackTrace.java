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
 * Crash-safety: this module runs LATE, after the generic {@code Harness.tickAll}
 * dispatch frame above every probe has JIT-compiled.  Because a compiled frame
 * breaks the interpreter saved-rbp layout stack_trace() walks, the named chains
 * are reached through a deep INTERPRETED {@link #guard(int, int)} recursion
 * (GUARD_DEPTH > the 64 cap) so a default-capped walk exhausts its budget on
 * valid interpreter frames and never reaches that compiled frame.  See
 * {@link #GUARD_DEPTH}.
 *
 * Scenario selector (native sets {@code mode} + clears {@code done} on the
 * rising edge of {@code go}; each scenario is one probe cycle):
 *   1 = known-depth chain  ...guard... -> outer -> mid -> inner  (depth/order/
 *       names headline: index 0 == mid, index 1 == outer)
 *   2 = same chain, used by the native side to exercise the max_depth CONTRACT
 *       (the chain is identical to mode 1; the native side just calls
 *        stack_trace(1), stack_trace(2), stack_trace(0) inside the detour)
 *   3 = deep self-recursion recurse(N) calling inner at the bottom, N well past
 *       the default 64 cap (truncation + "no spin" + uniform deep frames)
 *   4 = TWO chains in one cycle: a guard->shallow->inner chain THEN a
 *       guard->outer->mid->inner chain, so the native side proves the trace for
 *       the second fire is recomputed live (its IMMEDIATE caller is mid, the
 *       first fire's was shallow); both are guard-deep so the freshness signal
 *       is the distinct immediate caller, not a depth difference (the 64 cap
 *       makes both traces the same length)
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

    /**
     * Depth of the interpreted "guard" recursion that wraps the named chains in
     * modes 1/2/4 (see {@link #guard(int)}).  CRITICAL for crash-safety, not just
     * coverage: stack_trace() walks the live saved-rbp chain immediate-caller
     * first, and this module runs LATE in the suite, by which point the generic
     * Harness dispatch frame ({@code Harness.tickAll}) that sits above every
     * probe has been JIT-compiled.  A compiled frame does NOT follow the
     * interpreter layout stack_trace() assumes, so a walk that reaches it must
     * rely on the library's best-effort "stop on a non-interpreter frame" logic —
     * which was observed to intermittently AV on one CI runtime when the stray
     * read landed on unmapped metaspace.  By interposing GUARD_DEPTH (> the
     * default 64-frame cap) PLAIN INTERPRETED guard frames between the named
     * chain and that compiled boundary, the default-capped walk is GUARANTEED to
     * exhaust its budget on valid interpreter frames and never reach the compiled
     * dispatch frame.  Kept comfortably below DEEP_RECURSION so it stays well
     * within the Java thread stack.  The native side mirrors only "GUARD_DEPTH >
     * 64", never the exact value.
     */
    public static final int GUARD_DEPTH = 80;

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

    // ---- guard recursion: keeps the walk inside interpreter frames ----------

    /** Tail selector for {@link #guard(int, int)}: descend into outer(). */
    public static final int TAIL_OUTER   = 0;
    /** Tail selector for {@link #guard(int, int)}: descend into shallow(). */
    public static final int TAIL_SHALLOW = 1;

    /**
     * Plain self-recursion that descends {@code depth} interpreted frames and
     * then enters the requested named chain ({@code tail}) at the bottom.  Its
     * ONLY purpose is to interpose a deep run of interpreter frames between the
     * named chain (mid/outer or shallow, which sit BELOW guard, closest to the
     * hooked leaf) and the JIT-compiled {@code Harness.tickAll} dispatch frame
     * that sits ABOVE it — so a default-capped stack_trace() walk exhausts its
     * 64-frame budget on these valid interpreter frames and never reaches (and
     * never has to safely reject) the compiled frame.  See {@link #GUARD_DEPTH}.
     *
     * Frame order at the leaf detour (immediate-caller first) is therefore:
     *   mid, outer, guard, guard, ... (GUARD_DEPTH of them), runKnownDepth, ...
     * so index 0 is still mid and index 1 is still outer exactly as before.
     */
    public int guard(final int depth, final int tail)
    {
        if (depth <= 0)
        {
            if (tail == TAIL_SHALLOW)
            {
                return this.shallow(ARG_SHALLOW);
            }
            return this.outer(ARG_OUTER);
        }
        return this.guard(depth - 1, tail) + 1;
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
        // Reach outer -> mid -> inner THROUGH the interpreted guard recursion so
        // the default-capped walk never reaches the compiled Harness.tickAll
        // frame above this probe.  guard(...) returns at TAIL_OUTER into outer().
        this.guard(GUARD_DEPTH, TAIL_OUTER);
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
        // Two DISTINCT chains in one cycle so the native side proves the trace is
        // recomputed live per fire (different IMMEDIATE caller each time).  Both
        // are reached through the interpreted guard recursion so neither walk
        // reaches the compiled Harness.tickAll frame.  First fire's immediate
        // caller is shallow; second fire's is mid — that difference (not a depth
        // difference, which the 64-cap erases) is the freshness signal.
        this.guard(GUARD_DEPTH, TAIL_SHALLOW);
        this.guard(GUARD_DEPTH, TAIL_OUTER);
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
