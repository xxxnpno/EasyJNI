package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_call_jni_fallback" feature: exercises
 * vmhook::method_proxy::call() over EVERY return type and argument shape, with
 * the deliberate intent of driving the JNI invocation FALLBACK path
 * (method_proxy::call_jni, vmhook.hpp ~12488-13064).
 *
 * WHY THIS HITS call_jni: method_proxy::call() first probes
 * detail::find_call_stub_entry() (StubRoutines::_call_stub_entry).  When that
 * VMStruct is present (typically JDK 8..20) call() dispatches through the
 * interpreter call-stub fast path; when it is ABSENT (JDK 21+, and in fact on
 * every JDK the CI exercises, where the entry is not exported via VMStructs)
 * call() short-circuits into call_jni(), which marshals args into a jvalue[]
 * and dispatches via Call(Static)?<Type>MethodA.  The native module records
 * which path is live (find_call_stub_entry) so the same assertions are valid on
 * either dispatcher — the converted value_t must be identical.
 *
 * Coverage shape (every return type x arg shape the audit's scenario list
 * enumerates):
 *   - return types : void / boolean(Z) / byte(B) / char(C) / short(S) / int(I)
 *                    / long(J) / float(F) / double(D) / String / Object,
 *   - arg shapes   : no-arg, single primitive, String arg, Object arg,
 *                    MULTI-ARG including long + double (each occupies TWO local
 *                    slots on the interpreter path; ONE jvalue cell on the JNI
 *                    path — the marshaller must agree either way),
 *   - dispatch kind: INSTANCE (Call<Type>MethodA) AND STATIC
 *                    (CallStatic<Type>MethodA — static path resolves the jclass
 *                    via the declaring class name through FindClass),
 *   - JNI-fallback stress:
 *       * a TIGHT LOOP of String-returning calls (the audit flags a local-ref
 *         leak: every String return / String arg creates a JNI local ref that
 *         must be released or HotSpot's default 16-entry local-ref table
 *         overflows; once starved, later calls return "" — the native side
 *         asserts the result is stable across the loop),
 *       * a TIGHT LOOP of String-ARG calls (NewStringUTF local ref per call),
 *       * a TIGHT LOOP of long+double MULTI-ARG primitive calls (the
 *         union-aliasing footgun: a primitive jvalue cell must NEVER be handed
 *         to DeleteLocalRef — the loop must not corrupt state),
 *       * REPEATED calls on the SAME proxy (cache warm-up: cached_method_id /
 *         cached_class_handle reused — no state corruption across iterations),
 *       * INSTANCE vs STATIC interleaving.
 *
 * Every value-returning method here uses a recognizable boundary value so the
 * native side can pin the exact decode (sign-extension for B/S/I/J,
 * zero-extension for C, IEEE-754 fidelity for F/D, modified-UTF-8 for String on
 * the JNI path).  Every void / arg-consuming method records its invocation and
 * arguments into observable static fields so the native side can prove the body
 * actually ran with the right arguments even when nothing is returned.
 *
 * How the native module drives it: it hooks {@link #trigger(int)} (the only
 * context where vmhook::hotspot::current_java_thread is set, which call()
 * requires).  The probe's run() calls trigger() through a real bytecode
 * dispatch; the detour performs every call() below and records observations.
 *
 * Java 8 syntax only (no var / records / switch-expressions / lambdas).
 */
public final class MethodCallJni
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time the hooked trigger() runs (handshake sanity). */
    public static volatile int triggerCount;

    // ── void side-effect counters (prove a void body executed) ──────────────

    /** Bumped by the instance void method. */
    public static volatile int voidInstanceHits;

    /** Bumped by the static void method. */
    public static volatile int voidStaticHits;

    // ── recorded multi-arg primitives (prove the arg block was marshalled) ──

    /** Set true once sumILD(int,long,double) has run at least once. */
    public static volatile boolean multiPrimCalled;
    public static volatile int     multiArgInt;
    public static volatile long    multiArgLong;
    public static volatile double  multiArgDouble;

    /** Set true once the long+double two-slot loop body has run. */
    public static volatile boolean twoSlotLoopCalled;
    public static volatile long    twoSlotLastLong;
    public static volatile double  twoSlotLastDouble;

    // ── recorded String arg (prove a String reached the body) ───────────────

    /** Set true once consumeString(String) has run. */
    public static volatile boolean stringArgCalled;
    public static volatile String  stringArgValue;
    public static volatile int     stringArgLen;

    // ── recorded Object arg (prove a reference reached the body) ────────────

    /** Set true once consumeObject(Object) has run. */
    public static volatile boolean objectArgCalled;
    public static volatile boolean objectArgNonNull;
    public static volatile int     objectArgIdentity;

    /** identityHashCode of SINGLETON, so the native side can pass SINGLETON as
     *  the Object arg and prove byte-for-byte the body got the exact object. */
    public static volatile int     selfIdentity;

    // ── non-corruption breadcrumb ───────────────────────────────────────────

    /** The arg the LAST echoInt(int) received — proves a value-returning call
     *  performed after the stress loops still delivers its argument intact. */
    public static volatile int     lastEchoArg;

    // ── the method the native module hooks to obtain a live thread ──────────

    /** Hookable instance method.  The native detour on this method performs
     *  every method_proxy::call() the test asserts on. */
    public int trigger(final int delta)
    {
        triggerCount++;
        return delta + 1;
    }

    // ════════════════════════════════════════════════════════════════════════
    //  INSTANCE value returners — one per primitive return type, no args.
    //  Boundary values chosen so the native decode is unambiguous.
    // ════════════════════════════════════════════════════════════════════════

    public void    retVoid()        { voidInstanceHits++; }
    public boolean retBoolTrue()    { return true; }
    public boolean retBoolFalse()   { return false; }
    public byte    retByte()        { return (byte) -7; }          // sign-extends to -7
    public char    retChar()        { return 'Z'; }                // 90, zero-extends
    public char    retCharMax()     { return (char) 0xFFFF; }      // 65535, NOT -1
    public short   retShort()       { return (short) -12345; }     // sign-extends
    public int     retInt()         { return 0x0BADF00D; }         // 195948557
    public long    retLong()        { return 0x0123456789ABCDEFL; }
    public float   retFloat()       { return 3.5f; }               // exact in binary
    public double  retDouble()      { return 2.718281828459045; }
    public String  retString()      { return "jni-instance-hello"; }

    // Single-primitive ARG echoes (prove a primitive arg is marshalled).
    public int  echoInt(final int v)   { lastEchoArg = v; return v; }
    public long echoLong(final long v) { return v; }

    // Multi-arg primitive with a long (2 slots) and a double (2 slots) between
    // single-slot ints.  Returns a derived long so the native side can verify
    // every argument arrived in the correct slot.
    //   result = i + j + (long) d
    public long sumILD(final int i, final long j, final double d)
    {
        multiArgInt    = i;
        multiArgLong   = j;
        multiArgDouble = d;
        multiPrimCalled = true;
        return ((long) i) + j + (long) d;
    }

    // Two-slot-heavy multi-arg used by the tight loop: long, double, long.
    // Returns long+long+(long)double so a single returned value pins all three.
    public long twoSlot(final long a, final double b, final long c)
    {
        twoSlotLastLong   = a;
        twoSlotLastDouble = b;
        twoSlotLoopCalled = true;
        return a + c + (long) b;
    }

    // String ARG -> String return (round-trip through the JNI String paths).
    public String echoString(final String s)
    {
        return s;
    }

    // String ARG -> void (prove a String reaches a no-return body).
    public void consumeString(final String s)
    {
        stringArgValue  = s;
        stringArgLen    = (s == null) ? -1 : s.length();
        stringArgCalled = true;
    }

    // Object ARG -> void (prove a reference reaches a no-return body).
    public void consumeObject(final Object o)
    {
        objectArgNonNull  = (o != null);
        objectArgIdentity = (o == null) ? 0 : System.identityHashCode(o);
        objectArgCalled   = true;
    }

    // Object return (non-String reference): returns SINGLETON itself, so the
    // native side can prove the returned wrapper's OOP == the receiver OOP.
    public MethodCallJni retSelf()
    {
        return this;
    }

    // Object return that is null (null reference contract -> null unique_ptr).
    public MethodCallJni retNullObject()
    {
        return null;
    }

    // Array reference return ('[' descriptor) — a non-null int[].
    public int[] retIntArray()
    {
        return new int[] { 11, 22, 33 };
    }

    // String return used by the tight leak loop: a fresh constant each call.
    public String loopString()
    {
        return "loop-stable-value";
    }

    // ════════════════════════════════════════════════════════════════════════
    //  STATIC value returners — mirror the instance set (CallStatic*MethodA).
    //  The static path resolves the jclass from the declaring class name; this
    //  fixture lives on the application/system classloader, reachable via
    //  FindClass from the detour thread, so the static branch succeeds.
    // ════════════════════════════════════════════════════════════════════════

    public static void    sRetVoid()      { voidStaticHits++; }
    public static boolean sRetBoolTrue()  { return true; }
    public static byte    sRetByte()      { return (byte) 99; }
    public static char    sRetChar()      { return 'k'; }            // 107
    public static short   sRetShort()     { return (short) 20000; }
    public static int     sRetInt()       { return -2147483648; }    // Integer.MIN_VALUE
    public static long    sRetLong()      { return Long.MAX_VALUE; }
    public static float   sRetFloat()     { return -0.5f; }
    public static double  sRetDouble()    { return -1.5; }
    public static String  sRetString()    { return "jni-static-hello"; }

    public static int     sEchoInt(final int v) { return v; }

    public static long    sSumILD(final int i, final long j, final double d)
    {
        return ((long) i) + j + (long) d;
    }

    // Static Object return (non-String reference): the published SINGLETON.
    public static MethodCallJni sRetSingleton()
    {
        return SINGLETON;
    }

    public static MethodCallJni sRetNullObject()
    {
        return null;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodCallJni.go && !MethodCallJni.done;
            }

            @Override
            public void run()
            {
                // Publish the receiver identity so the Object-arg check is exact.
                MethodCallJni.selfIdentity = System.identityHashCode(SINGLETON);

                // Drive trigger() through normal bytecode dispatch -> fires the
                // native interpreter hook; that detour performs every
                // method_proxy::call() the test asserts on.
                SINGLETON.trigger(11);

                MethodCallJni.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side reaches the instance methods on a stable OOP and so the
     * published identity matches the receiver the detour sees as `self`.
     */
    public static final MethodCallJni SINGLETON = new MethodCallJni();
}
