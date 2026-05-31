package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_explicit_signature feature (area: methods).
 *
 * The feature under test is {@code object::get_method(name, signature)} selecting
 * ONE overload out of several same-named methods by EXACT JVM descriptor, and
 * yielding no method (safe no-op call) for a wrong/absent signature.
 *
 * To exercise that exhaustively this fixture defines deliberately HEAVILY
 * overloaded method families, both instance and static:
 *
 *   process(...)        — 6 instance overloads sharing the name "process":
 *        (I)I                                returns arg + 1
 *        (II)I                               returns a*100 + b
 *        (J)J                                returns arg + 1000
 *        (Ljava/lang/String;)Ljava/lang/String;   returns "S:" + arg
 *        (I)Ljava/lang/String;  ?  NOT ALLOWED in Java (overload by return type
 *        only is illegal), so instead we add a structurally-distinct one:
 *        (Ljava/lang/String;I)Ljava/lang/String;  returns arg + "#" + n
 *        ()V                                 increments processVoidHits
 *
 *   combo(...)          — TWO instance overloads whose PARAMETER LISTS are the
 *        same shape to a naive matcher but whose descriptors differ by the
 *        reference type, so only an EXACT descriptor compare disambiguates:
 *        (Ljava/lang/CharSequence;)Ljava/lang/String;   returns "CS:" + s
 *        (Ljava/lang/String;)Ljava/lang/String;         returns "ST:" + s
 *        Both can be invoked with the SAME Java String argument, so the explicit
 *        signature is the ONLY way to choose between them.
 *
 *   smap(...)           — TWO STATIC overloads (exercises the static
 *        type_index-based explicit-signature overload + static_method(name,sig)):
 *        (I)I                                returns arg * 2
 *        (Ljava/lang/String;)Ljava/lang/String;  returns "M:" + arg
 *
 *   inherited overloads — declared on a SUPERCLASS (MethodExplicitSigBase) so the
 *        hierarchy-walk in both overloads is exercised:
 *        base(I)I        returns arg + 7
 *        base(II)I       returns a - b
 *
 * The native module hooks {@code trigger()} (a no-arg instance method the probe
 * calls on a real bytecode dispatch); inside that detour {@code current_java_thread}
 * is live, so every {@code get_method(name,sig)->call(...)} actually dispatches a
 * real Java method.  Each overload writes a distinct, observable side effect so
 * the native side can prove which overload ran, independent of the returned value.
 *
 * Java 8 syntax only (no var / records / switch-expr / text-blocks).
 */
public final class MethodExplicitSig extends MethodExplicitSigBase
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ---- Per-overload side-effect tallies (proof of WHICH overload ran) ----

    /** process(I)I executed; records last int arg seen. */
    public static volatile int processIntArg;
    /** process(II)I executed; records last (a,b). */
    public static volatile int processIntIntA;
    public static volatile int processIntIntB;
    /** process(J)J executed; records last long arg. */
    public static volatile long processLongArg;
    /** process(String)String executed; records last arg. */
    public static volatile String processStrArg;
    /** process(String,int)String executed; records last args. */
    public static volatile String processStrIntS;
    public static volatile int processStrIntN;
    /** process()V executed; counts invocations. */
    public static volatile int processVoidHits;

    /** combo(CharSequence) executed. */
    public static volatile int comboCsHits;
    /** combo(String) executed. */
    public static volatile int comboStHits;

    /** smap(int) static executed. */
    public static volatile int smapIntHits;
    /** smap(String) static executed. */
    public static volatile int smapStrHits;

    // The inherited base(I)I / base(II)I overloads record their side effects in
    // MethodExplicitSigBase.baseIntSeen / baseIntIntSeen (proof of hierarchy walk).

    /** trigger() invocation count (handshake proof). */
    public static volatile int triggerCount;

    // ---- Constants mirrored on the native side -----------------------------

    public static final int    PROC_I_ARG      = 41;   // process(I)I  -> 42
    public static final int    PROC_II_A       = 3;
    public static final int    PROC_II_B       = 9;     // process(II)I -> 309
    public static final long   PROC_J_ARG      = 5L;    // process(J)J  -> 1005
    public static final String PROC_S_ARG      = "abc"; // process(String) -> "S:abc"
    public static final String PROC_SI_ARG     = "k";   // process(String,int) -> "k#7"
    public static final int    PROC_SI_N       = 7;
    public static final String COMBO_ARG       = "Z";   // combo(*) arg
    public static final int    SMAP_I_ARG      = 21;    // smap(int) -> 42
    public static final String SMAP_S_ARG      = "qq";  // smap(String) -> "M:qq"
    public static final int    BASE_I_ARG      = 100;   // base(I)I -> 107
    public static final int    BASE_II_A       = 50;
    public static final int    BASE_II_B       = 8;     // base(II)I -> 42

    // ===================== Overload family: process =========================

    /** process(I)I */
    public int process(final int a)
    {
        processIntArg = a;
        return a + 1;
    }

    /** process(II)I */
    public int process(final int a, final int b)
    {
        processIntIntA = a;
        processIntIntB = b;
        return a * 100 + b;
    }

    /** process(J)J */
    public long process(final long a)
    {
        processLongArg = a;
        return a + 1000L;
    }

    /** process(Ljava/lang/String;)Ljava/lang/String; */
    public String process(final String s)
    {
        processStrArg = s;
        return "S:" + s;
    }

    /** process(Ljava/lang/String;I)Ljava/lang/String; */
    public String process(final String s, final int n)
    {
        processStrIntS = s;
        processStrIntN = n;
        return s + "#" + n;
    }

    /** process()V */
    public void process()
    {
        processVoidHits++;
    }

    // ===================== Overload family: combo ==========================
    // Same arg COUNT, different reference descriptor.  The SAME Java String can
    // be passed to both, so only an exact-descriptor lookup disambiguates.

    /** combo(Ljava/lang/CharSequence;)Ljava/lang/String; */
    public String combo(final CharSequence s)
    {
        comboCsHits++;
        return "CS:" + s;
    }

    /** combo(Ljava/lang/String;)Ljava/lang/String; */
    public String combo(final String s)
    {
        comboStHits++;
        return "ST:" + s;
    }

    // ===================== Static overload family: smap ====================

    /** smap(I)I (static) */
    public static int smap(final int a)
    {
        smapIntHits++;
        return a * 2;
    }

    /** smap(Ljava/lang/String;)Ljava/lang/String; (static) */
    public static String smap(final String s)
    {
        smapStrHits++;
        return "M:" + s;
    }

    // ===================== Hook target =====================================

    /** No-arg instance method the probe calls so the native detour fires. */
    public void trigger()
    {
        triggerCount++;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodExplicitSig.go && !MethodExplicitSig.done;
            }

            @Override
            public void run()
            {
                final MethodExplicitSig instance = new MethodExplicitSig();
                // A single real bytecode dispatch into trigger() fires the native
                // interpreter hook; ALL get_method(name,sig)->call() work happens
                // inside that detour where current_java_thread is set.
                instance.trigger();
                MethodExplicitSig.done = true;
            }
        });
    }
}
