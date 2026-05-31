package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_static feature (area: methods).
 *
 * Exercises {@code static_method("name")->call(args)} for STATIC Java methods
 * that return every JVM primitive (Z B S C I J F D), {@code java.lang.String},
 * and an object reference, and proves on a real JVM (genuine bytecode dispatch
 * via the Harness go/done handshake) that:
 *
 *   - the returned value_t decodes to the EXACT Java return value for each
 *     primitive width and boundary (min/max/-1/NaN/Inf/-0.0/65535 ...),
 *   - the String return decodes to the exact UTF-8 text (eager std::string
 *     alternative on both call paths),
 *   - the object return routes through the uint32/oop alternative (its full
 *     usability is JDK/call-path dependent and recorded as INFO, mirroring
 *     method_call_object),
 *   - NO RECEIVER is passed to a static method: the first declared argument
 *     occupies slot 0 (a {@code this} would have pushed it to slot 1), the
 *     proxy's receiver OOP is null, and a static method observably cannot and
 *     does not see any instance state,
 *   - {@code method_proxy::is_static()} returns TRUE for every static method and
 *     FALSE for every instance method (the recently-fixed accessor that reads
 *     JVM_ACC_STATIC straight from the live Method's _access_flags),
 *   - the audit's still-open flaw is exercised: {@code static_method("inst")}
 *     wrongly returns a proxy for an INSTANCE method (no JVM_ACC_STATIC filter
 *     on the static get_method path); the fixed is_static() accessor is what
 *     lets the native side DETECT that the wrongly-accepted proxy is non-static.
 *
 * Java 8 syntax only (no var / records / switch-expression / text blocks).
 * Only java.* + vmhook.Harness are referenced.
 *
 * Everything the native module asserts happens inside ONE run() invocation,
 * because method_proxy::call() requires the current JavaThread to be set, which
 * only holds on the Java thread while it executes inside the interpreter detour.
 * The native module hooks {@link #trigger(int)} and performs every static call
 * from within that detour.
 */
public final class MethodStatic
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ---- "no receiver" instrumentation -----------------------------------

    /**
     * A static method body cannot reference {@code this}.  To give the native
     * side a hard, observable proof that no receiver was injected, the static
     * recorder methods below stamp the arguments they ACTUALLY received into
     * these fields.  If a stray receiver had been pushed into slot 0, the
     * interpreter would have shifted every declared argument by one slot and
     * the recorded values would be wrong (or the call would mis-dispatch).
     */
    public static volatile int   recordedIntArg;
    public static volatile long  recordedLongArg;
    public static volatile int   recordedFirstOfThree;
    public static volatile long  recordedSecondOfThree;
    public static volatile int   recordedThirdOfThree;

    /** Counts how many times the static recorder ran (allow-through proof). */
    public static volatile int   staticRecorderHits;

    /**
     * An INSTANCE field with a poison value.  A correctly-dispatched STATIC
     * method has no access to it; this exists only so the wrapper's
     * static-vs-instance story is concrete and to seed instance scenarios.
     */
    private int instancePoison = 0xBADF00D;

    /** Seed used by the instance returners (parity / is_static()==false set). */
    private int seed = 4242;

    // ---- Trigger (hooked; the detour runs every native static call) -------

    /** Hookable instance method; the native module hooks this to get a frame. */
    public int trigger(final int delta)
    {
        return this.seed + delta;
    }

    // ======================================================================
    //  STATIC primitive returners — one per type, at representative
    //  boundaries.  Names are unique so static_method("name") is unambiguous.
    // ======================================================================

    public static boolean sBoolTrue()   { return true; }
    public static boolean sBoolFalse()  { return false; }

    public static byte sByteMax()       { return Byte.MAX_VALUE; }   // 127
    public static byte sByteMin()       { return Byte.MIN_VALUE; }   // -128
    public static byte sByteNegOne()    { return (byte) -1; }

    public static short sShortMax()     { return Short.MAX_VALUE; }  // 32767
    public static short sShortMin()     { return Short.MIN_VALUE; }  // -32768
    public static short sShortNegOne()  { return (short) -1; }

    public static char sCharA()         { return 'A'; }             // 65
    public static char sCharMax()       { return Character.MAX_VALUE; } // 65535

    public static int sIntMax()         { return Integer.MAX_VALUE; }
    public static int sIntMin()         { return Integer.MIN_VALUE; }
    public static int sIntFortyTwo()    { return 42; }
    public static int sIntNegOne()      { return -1; }

    public static long sLongMax()       { return Long.MAX_VALUE; }
    public static long sLongMin()       { return Long.MIN_VALUE; }
    public static long sLongBig()       { return 0x0123456789ABCDEFL; }

    public static float sFloatHalf()    { return 0.5f; }
    public static float sFloatNegZero() { return -0.0f; }
    public static float sFloatNaN()     { return Float.NaN; }
    public static float sFloatPosInf()  { return Float.POSITIVE_INFINITY; }
    public static float sFloatMax()     { return Float.MAX_VALUE; }

    public static double sDoublePi()    { return 3.141592653589793; }
    public static double sDoubleNegZero() { return -0.0d; }
    public static double sDoubleNaN()   { return Double.NaN; }
    public static double sDoubleNegInf() { return Double.NEGATIVE_INFINITY; }
    public static double sDoubleMax()   { return Double.MAX_VALUE; }

    public static void sVoidBump()      { ++staticRecorderHits; }

    // ======================================================================
    //  STATIC String + object returners.
    // ======================================================================

    /** Static String return; exact ASCII payload the native side pins. */
    public static String sStringHello() { return "hello-static"; }

    /** Static String return with non-ASCII payload (UTF-8: café). */
    public static String sStringUnicode() { return "café"; }

    /** Static String return that is empty (distinct from null / void). */
    public static String sStringEmpty() { return ""; }

    /** Static String return that is null (decodes to empty, never crashes). */
    public static String sStringNull()  { return null; }

    /** Static object return: a fresh tagged child. */
    public static MethodStatic sMakeChild()
    {
        final MethodStatic child = new MethodStatic();
        child.seed = STATIC_CHILD_SEED;
        return child;
    }

    /** Static object return that is null (must yield a null unique_ptr). */
    public static MethodStatic sNullChild() { return null; }

    public static final int STATIC_CHILD_SEED = 9090;

    // ======================================================================
    //  STATIC argument-passing methods — these PROVE no receiver is passed.
    // ======================================================================

    /**
     * Echoes its only argument.  If a {@code this} had been injected into slot
     * 0, the interpreter would read the receiver as the int and return garbage
     * (or the call_stub param_count would be off by one).  A correct static
     * dispatch returns exactly {@code v}.
     */
    public static int sEchoInt(final int v) { return v; }

    /**
     * Records its single int argument into a static field, so the native side
     * confirms the value arrived intact at parameter slot 0 (no leading
     * receiver slot).  Also bumps the allow-through hit counter.
     */
    public static void sRecordInt(final int v)
    {
        recordedIntArg = v;
        ++staticRecorderHits;
    }

    /** Records a long arg (2 interpreter slots) at slot 0 — no receiver shift. */
    public static long sRecordLong(final long v)
    {
        recordedLongArg = v;
        ++staticRecorderHits;
        return v;
    }

    /**
     * Multi-arg static recorder: first int at slot 0, long across slots 1-2,
     * trailing int at slot 3.  With a phantom receiver every one of these would
     * be shifted by a slot.  Returns the sum so the native side double-checks.
     */
    public static long sRecordThree(final int a, final long b, final int c)
    {
        recordedFirstOfThree  = a;
        recordedSecondOfThree = b;
        recordedThirdOfThree  = c;
        ++staticRecorderHits;
        return (long) a + b + c;
    }

    /**
     * Returns the identity hash of the single argument it receives.  Used so
     * the native side can confirm the object argument (not a receiver) reached
     * the static method as parameter zero.
     */
    public static int sArgIdentity(final MethodStatic arg)
    {
        return (arg == null) ? 0 : System.identityHashCode(arg);
    }

    // ======================================================================
    //  INSTANCE methods — used to prove is_static() == FALSE for non-static,
    //  and to exercise the audit's "static_method wrongly accepts an instance
    //  method" flaw (Bug #2).  Names are deliberately distinct from the static
    //  ones so the native side can target each precisely.
    // ======================================================================

    /** Instance int returner (is_static() must be false). */
    public int iGetSeed() { return this.seed; }

    /** Instance String returner (is_static() must be false). */
    public String iLabel() { return "instance-label"; }

    /** Instance method that takes an int (is_static() must be false). */
    public int iEcho(final int v) { return this.seed + v; }

    /** Instance void method (is_static() must be false). */
    public void iTouch() { this.instancePoison = 0; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodStatic.go && !MethodStatic.done;
            }

            @Override
            public void run()
            {
                // Reset allow-through counters so the per-cycle assertions are
                // deterministic, THEN drive trigger() through normal bytecode
                // dispatch.  That fires the native interpreter detour, and the
                // detour performs every static_method(...).call() the module
                // asserts on (the only context where current_java_thread is set,
                // which method_proxy::call() requires).
                staticRecorderHits = 0;
                recordedIntArg = 0;
                recordedLongArg = 0L;
                recordedFirstOfThree = 0;
                recordedSecondOfThree = 0L;
                recordedThirdOfThree = 0;

                final MethodStatic instance = new MethodStatic();
                instance.trigger(7);
                MethodStatic.done = true;
            }
        });
    }
}
