package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_overload_java_dispatch feature (area: methods).
 *
 * This is the JAVA-SIDE READBACK companion to MethodOverload (the selection-logic
 * fixture).  MethodOverload proves WHICH overload the resolver picks (each overload
 * returns a distinct sentinel, captured inside the detour).  THIS fixture proves
 * the picked overload's REAL EFFECT — its computed return value AND a per-overload
 * Java-recorded side effect — flows back correctly through method_proxy::call(),
 * mirroring the legacy example.cpp test_overloaded_methods (overloadProbe*):
 *
 *      f(int 30)      -> 130       (x + 100)
 *      f(String foo)  -> "[foo]"   ("[" + s + "]")
 *      f(2, 3)        -> 5         (a + b)
 *
 * The native module dispatches each overload TWO independent ways and asserts the
 * SAME result + side effect each way:
 *   (1) the C++-typed call():            get_method("f")->call(<typed arg>)
 *       — resolution follows the C++ argument TYPE (int->I, std::string->
 *         Ljava/lang/String;, (int,int)->(II)), proving descriptor-aware
 *         overload resolution reaches the right Java body for primitive,
 *         String, and multi-arg forms;
 *   (2) the explicit-signature call():   get_method("f","(I)I")->call(...)
 *       — resolution is pinned to the exact descriptor.
 * Both must compute the legacy value AND fire the matching side-effect recorder
 * (lastIntResult / lastStrArg / lastDualSum + per-overload hit counters) so the
 * native side can confirm, from Java's own state, that the intended body ran and
 * no sibling overload did.
 *
 * It ALSO carries a primitive-only family `h` (h(int) and h(long), NO reference
 * overload) so the native side can exercise the documented no-match fallback
 * (call with a C++ type matching NO overload -> resolve_compatible_method walks
 * the hierarchy, finds no descriptor match, and returns the FIRST-by-name overload
 * — NOT monostate; vmhook.hpp resolve_compatible_method final `return this->method`)
 * WITHOUT ever blasting a primitive into a reference slot (every `h` overload has a
 * primitive parameter, so whichever one HotSpot orders first is a safe primitive
 * dispatch — no reference-slot access violation).
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks);
 * the Harness.Probe is registered as an ANONYMOUS inner class in the static
 * initializer, exactly like the other fixtures.
 */
public final class OverloadDispatch
{
    /** Native sets this true to request the action; the probe clears it via done. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ── Legacy-mirrored arguments + expected results (single source of truth) ──
    // Kept in lockstep with the native module's constants.
    public static final int    F_INT_ARG      = 30;          // f(int)    -> 130
    public static final int    F_INT_EXPECT   = 130;         // 30 + 100
    public static final String F_STR_ARG      = "foo";       // f(String) -> "[foo]"
    public static final String F_STR_EXPECT   = "[foo]";     // "[" + "foo" + "]"
    public static final int    F_DUAL_A       = 2;           // f(int,int) -> 5
    public static final int    F_DUAL_B       = 3;
    public static final int    F_DUAL_EXPECT  = 5;           // 2 + 3

    // The primitive-only no-match family arguments + results.
    public static final int    H_INT_ARG      = 4;           // h(int)  -> 44
    public static final int    H_INT_EXPECT   = 44;          // x + 40
    public static final long   H_LONG_ARG     = 7L;          // h(long) -> 7007
    public static final long   H_LONG_EXPECT  = 7007L;       // x + 7000

    // ── Per-overload Java-recorded side effects (proof of WHICH body ran) ──────
    // Each overload records its own argument(s)/result + bumps its hit counter, so
    // the native side can confirm — purely from Java's observable state — that the
    // intended overload executed and the siblings did not.
    public static volatile int     lastIntArg;       // last f(int) argument
    public static volatile int     lastIntResult;    // last f(int) result (x + 100)
    public static volatile String  lastStrArg;       // last f(String) argument
    public static volatile String  lastStrResult;    // last f(String) result ("[s]")
    public static volatile int     lastDualA;        // last f(int,int) first arg
    public static volatile int     lastDualB;        // last f(int,int) second arg
    public static volatile int     lastDualSum;      // last f(int,int) result (a + b)

    public static volatile int     fIntHits;         // f(int)        invocation count
    public static volatile int     fStrHits;         // f(String)     invocation count
    public static volatile int     fDualHits;        // f(int,int)    invocation count

    // no-match family echoes
    public static volatile long    lastHArg;         // last h(*) argument (widened)
    public static volatile long    lastHResult;      // last h(*) result (widened)
    public static volatile int     hIntHits;         // h(int)  invocation count
    public static volatile int     hLongHits;        // h(long) invocation count

    /** tick() invocation count — handshake proof the detour fired. */
    public static volatile int     tickCount;

    // ── Hook site ─────────────────────────────────────────────────────────────
    /**
     * The native module hooks this; inside the detour current_java_thread is live,
     * which is the only context in which method_proxy::call() may invoke the call
     * gate.  The detour performs every f(...) / h(...) call on `self`.
     */
    public int tick(final int nonce)
    {
        tickCount++;
        return nonce + 1;
    }

    // ── The overloaded family `f` (legacy semantics) ──────────────────────────
    // Declaration order is intentionally scrambled (String first) so the resolver
    // must not depend on source order.

    /** f(Ljava/lang/String;)Ljava/lang/String; -> "[" + s + "]" */
    public String f(final String s)
    {
        final String r = "[" + s + "]";
        lastStrArg = s;
        lastStrResult = r;
        fStrHits++;
        return r;
    }

    /** f(I)I -> x + 100 */
    public int f(final int x)
    {
        final int r = x + 100;
        lastIntArg = x;
        lastIntResult = r;
        fIntHits++;
        return r;
    }

    /** f(II)I -> a + b */
    public int f(final int a, final int b)
    {
        final int r = a + b;
        lastDualA = a;
        lastDualB = b;
        lastDualSum = r;
        fDualHits++;
        return r;
    }

    // ── The primitive-only no-match family `h` ────────────────────────────────
    // BOTH overloads take a primitive — no reference parameter exists — so the
    // documented no-match fallback (which dispatches the first-by-name overload)
    // can be exercised with a non-matching C++ primitive type (e.g. double) with
    // zero risk of a primitive-into-reference-slot access violation.

    /** h(I)I -> x + 40 */
    public int h(final int x)
    {
        lastHArg = x;
        lastHResult = x + 40;
        hIntHits++;
        return x + 40;
    }

    /** h(J)J -> x + 7000 */
    public long h(final long x)
    {
        lastHArg = x;
        lastHResult = x + 7000L;
        hLongHits++;
        return x + 7000L;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return OverloadDispatch.go && !OverloadDispatch.done;
            }

            @Override
            public void run()
            {
                // Drive tick() on the shared SINGLETON so the native detour's
                // `self` is exactly this instance.  Every f(...)/h(...) call the
                // module performs happens inside that detour where a JavaThread
                // is live.
                SINGLETON.tick(11);
                OverloadDispatch.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps.  Created eagerly so the
     * detour's `self` OOP is deterministic and matches what the probe drove.
     */
    public static final OverloadDispatch SINGLETON = new OverloadDispatch();
}
