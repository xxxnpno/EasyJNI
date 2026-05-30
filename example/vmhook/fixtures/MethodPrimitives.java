package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "method_call_primitives" feature: exercises
 * vmhook::method_proxy::call() returning every JVM primitive (Z B S C I J F D)
 * and void, at boundary values, through a real bytecode dispatch.
 *
 * How the native module drives it:
 *   - The native side hooks {@link #trigger(int)}.  Inside that detour
 *     vmhook::hotspot::current_java_thread is set, which is the ONLY context in
 *     which method_proxy::call() may invoke the interpreter / JNI call gate.
 *   - The probe's run() simply calls trigger(7) on the shared instance.  That
 *     fires the detour, and the detour calls every return-typed method below via
 *     method_proxy::call(), capturing the converted C++ value_t into atomics.
 *
 * Each primitive has an INSTANCE returner and a STATIC returner so the native
 * module exercises BOTH JNI dispatch slot sets (instance Call<T>MethodA and
 * static CallStatic<T>MethodA), as well as the call_stub fast path when the JDK
 * exposes StubRoutines::_call_stub_entry.
 *
 * Boundary values are deliberate: signed min/max for B/S/I/J, the full unsigned
 * range for C, and the IEEE-754 special values (NaN, +/-Inf, -0.0, MIN_VALUE,
 * MAX_VALUE) for F/D, so the C++ side proves sign-extension, zero-extension and
 * bit-cast fidelity rather than just "some number came back".
 *
 * Java 8 syntax only (no var / records / switch-expressions).
 */
public final class MethodPrimitives
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Incremented by every void returner so the native side can prove the
     *  void dispatch actually reached a real Java body (side effect). */
    public static volatile int voidInstanceHits;
    public static volatile int voidStaticHits;

    /** Records the argument the last (I)I echo method received, so the native
     *  side can prove arguments are passed through alongside the return. */
    public static volatile int lastEchoArg;

    // ----- the method the native module hooks to obtain a live thread -----

    /** Hookable instance method.  The native detour on this method calls every
     *  returner below via method_proxy::call(). */
    public int trigger(final int delta)
    {
        return delta + 1;
    }

    // ----------------------------------------------------------------------
    //  boolean (Z)
    // ----------------------------------------------------------------------
    public boolean retBoolTrue()         { return true; }
    public boolean retBoolFalse()        { return false; }
    public static boolean sRetBoolTrue() { return true; }
    public static boolean sRetBoolFalse(){ return false; }

    // ----------------------------------------------------------------------
    //  byte (B)   — signed 8-bit, range -128..127
    // ----------------------------------------------------------------------
    public byte retByteZero()        { return (byte) 0; }
    public byte retByteOne()         { return (byte) 1; }
    public byte retByteNegOne()      { return (byte) -1; }
    public byte retByteMax()         { return Byte.MAX_VALUE; }   // 127
    public byte retByteMin()         { return Byte.MIN_VALUE; }   // -128
    public static byte sRetByteNegOne() { return (byte) -1; }
    public static byte sRetByteMax()    { return Byte.MAX_VALUE; }
    public static byte sRetByteMin()    { return Byte.MIN_VALUE; }

    // ----------------------------------------------------------------------
    //  short (S)  — signed 16-bit, range -32768..32767
    // ----------------------------------------------------------------------
    public short retShortZero()      { return (short) 0; }
    public short retShortNegOne()    { return (short) -1; }
    public short retShortMax()       { return Short.MAX_VALUE; }  // 32767
    public short retShortMin()       { return Short.MIN_VALUE; }  // -32768
    public static short sRetShortNegOne() { return (short) -1; }
    public static short sRetShortMax()    { return Short.MAX_VALUE; }
    public static short sRetShortMin()    { return Short.MIN_VALUE; }

    // ----------------------------------------------------------------------
    //  char (C)   — UNSIGNED 16-bit, range 0..65535
    // ----------------------------------------------------------------------
    public char retCharZero()        { return (char) 0; }
    public char retCharA()           { return 'A'; }             // 65
    public char retCharMax()         { return (char) 0xFFFF; }   // 65535
    public static char sRetCharA()      { return 'A'; }
    public static char sRetCharMax()    { return (char) 0xFFFF; }

    // ----------------------------------------------------------------------
    //  int (I)    — signed 32-bit
    // ----------------------------------------------------------------------
    public int retIntZero()          { return 0; }
    public int retIntNegOne()        { return -1; }
    public int retIntMax()           { return Integer.MAX_VALUE; }   // 2147483647
    public int retIntMin()           { return Integer.MIN_VALUE; }   // -2147483648
    public int retIntFortyTwo()      { return 42; }
    public static int sRetIntMax()      { return Integer.MAX_VALUE; }
    public static int sRetIntMin()      { return Integer.MIN_VALUE; }
    public static int sRetIntFortyTwo() { return 42; }
    /** (I)I echo — proves argument passthrough together with the return. */
    public int echoInt(final int v)  { lastEchoArg = v; return v; }
    public static int sEchoInt(final int v) { lastEchoArg = v; return v; }

    // ----------------------------------------------------------------------
    //  long (J)   — signed 64-bit (occupies TWO interpreter local slots)
    // ----------------------------------------------------------------------
    public long retLongZero()        { return 0L; }
    public long retLongNegOne()      { return -1L; }
    public long retLongMax()         { return Long.MAX_VALUE; }  // 9223372036854775807
    public long retLongMin()         { return Long.MIN_VALUE; }  // -9223372036854775808
    public long retLongBig()         { return 0x0123456789ABCDEFL; }
    public static long sRetLongMax()    { return Long.MAX_VALUE; }
    public static long sRetLongMin()    { return Long.MIN_VALUE; }
    public static long sRetLongBig()    { return 0x0123456789ABCDEFL; }

    // ----------------------------------------------------------------------
    //  float (F)
    // ----------------------------------------------------------------------
    public float retFloatZero()      { return 0.0f; }
    public float retFloatOne()       { return 1.0f; }
    public float retFloatNegOne()    { return -1.0f; }
    public float retFloatHalf()      { return 0.5f; }
    public float retFloatMax()       { return Float.MAX_VALUE; }
    public float retFloatMinValue()  { return Float.MIN_VALUE; }   // smallest positive subnormal
    public float retFloatNegZero()   { return -0.0f; }
    public float retFloatNaN()       { return Float.NaN; }
    public float retFloatPosInf()    { return Float.POSITIVE_INFINITY; }
    public float retFloatNegInf()    { return Float.NEGATIVE_INFINITY; }
    public static float sRetFloatHalf()   { return 0.5f; }
    public static float sRetFloatNaN()    { return Float.NaN; }
    public static float sRetFloatPosInf() { return Float.POSITIVE_INFINITY; }
    public static float sRetFloatNegZero(){ return -0.0f; }

    // ----------------------------------------------------------------------
    //  double (D)
    // ----------------------------------------------------------------------
    public double retDoubleZero()    { return 0.0; }
    public double retDoubleOne()     { return 1.0; }
    public double retDoubleNegOne()  { return -1.0; }
    public double retDoublePi()      { return Math.PI; }
    public double retDoubleMax()     { return Double.MAX_VALUE; }
    public double retDoubleMinValue(){ return Double.MIN_VALUE; }  // smallest positive subnormal
    public double retDoubleNegZero() { return -0.0; }
    public double retDoubleNaN()     { return Double.NaN; }
    public double retDoublePosInf()  { return Double.POSITIVE_INFINITY; }
    public double retDoubleNegInf()  { return Double.NEGATIVE_INFINITY; }
    public static double sRetDoublePi()     { return Math.PI; }
    public static double sRetDoubleNaN()    { return Double.NaN; }
    public static double sRetDoubleNegInf() { return Double.NEGATIVE_INFINITY; }
    public static double sRetDoubleNegZero(){ return -0.0; }

    // ----------------------------------------------------------------------
    //  void (V)
    // ----------------------------------------------------------------------
    public void retVoidBump()        { voidInstanceHits++; }
    public static void sRetVoidBump(){ voidStaticHits++; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodPrimitives.go && !MethodPrimitives.done;
            }

            @Override
            public void run()
            {
                final MethodPrimitives instance = new MethodPrimitives();
                // Driving trigger() through normal bytecode dispatch fires the
                // native interpreter hook; that detour performs every
                // method_proxy::call() the test asserts on.
                instance.trigger(7);
                MethodPrimitives.done = true;
            }
        });
    }
}
