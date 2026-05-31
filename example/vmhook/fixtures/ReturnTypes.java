package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code method_return_types} feature: exercises
 * {@code vmhook::method_proxy::call()} / {@code static_method(...)->call()}
 * DECODING every Java return type back into a C++ {@code value_t} -- one method
 * per {@code BasicType} ({@code Z B S C I J F D}), {@code java.lang.String}, and
 * an {@code Object}-typed method that returns {@code null}.
 *
 * <p>This is the modular-harness sibling of the legacy
 * {@code example.cpp::test_return_types}: the legacy probe made Java itself
 * accumulate the returns into one int; here the NATIVE side calls each method
 * through {@code method_proxy::call()} and asserts the decoded C++ value equals a
 * fixed sentinel, so the DECODE path (variant alternative + conversion operator)
 * is what is under test, not Java arithmetic.</p>
 *
 * <p>How the native module drives it: it hooks {@link #trigger(int)} (the only
 * context in which {@code vmhook::hotspot::current_java_thread} is established,
 * which {@code call()} requires).  The probe's {@code run()} calls
 * {@code trigger(7)} on {@link #SINGLETON} through a real bytecode dispatch; that
 * fires the interpreter detour, and the detour performs every {@code call()} the
 * native module asserts on against this very instance (and the static methods).</p>
 *
 * <p>Sentinel values (the native module hard-codes the matching expectations).
 * The headline value of each primitive mirrors the legacy {@code Example.returnsX}
 * so the two suites agree; additional boundary returners cover sign-extension,
 * zero-extension, IEEE special bit patterns and signed min/max:</p>
 * <ul>
 *   <li>{@code boolean returnsBool}    -> {@code true}        (and {@code returnsBoolFalse} -> {@code false})</li>
 *   <li>{@code byte    returnsByte}    -> {@code (byte)126}   (0x7E; + Max 127, Min -128, NegOne -1)</li>
 *   <li>{@code short   returnsShort}   -> {@code (short)12345}(+ Max 32767, Min -32768, NegOne -1)</li>
 *   <li>{@code char    returnsChar}    -> {@code '?'} (63)    (+ Max 0xFFFF=65535 unsigned)</li>
 *   <li>{@code int     returnsInt}     -> {@code 0x12345678}  (305419896; + Max, Min)</li>
 *   <li>{@code long    returnsLong}    -> {@code 0x123456789ABCDEF0L} (+ Min, a negative -9876543210)</li>
 *   <li>{@code float   returnsFloat}   -> {@code 3.1415926f}  (bits 0x40490FDA; + NaN, a fixed bit pattern)</li>
 *   <li>{@code double  returnsDouble}  -> {@code 2.718281828459045} (bits 0x4005BF0A8B145769; + NaN, fixed bits)</li>
 *   <li>{@code String  returnsString}  -> {@code "hello-from-jvm"} (+ empty, + a multibyte unicode string)</li>
 *   <li>{@code Object  returnsNull}    -> {@code null}        (the null-reference boundary)</li>
 * </ul>
 *
 * <p>The non-ASCII String constant is written with {@code \\uXXXX} escapes so the
 * source is pure ASCII and javac decodes it identically on every CI host
 * regardless of file encoding.  Java 8 syntax only (no var / records /
 * switch-expressions / lambdas): the probe is an anonymous {@link Harness.Probe}.</p>
 */
public final class ReturnTypes
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time {@link #trigger(int)} actually runs (hook liveness). */
    public static volatile int triggerCount;

    // -- Canonical multibyte unicode return (ASCII-safe \\uXXXX source) ----------
    // "caf" + e-acute (U+00E9) + space + nihongo (U+65E5 U+672C U+8A9E): a mix of
    // a Latin-1 high char (2-byte modified-UTF-8) and CJK chars (3-byte each), so
    // the String decode is proven on more than pure ASCII.  Written purely with
    // \\uXXXX escapes (like MethodString) so javac decodes it identically on every
    // CI host no matter the file encoding; the native module hard-codes the
    // matching modified-UTF-8 bytes.  Logical text: "cafe<acute> <nihongo>".
    public static final String UNICODE = "caf\u00e9 \u65e5\u672c\u8a9e";  // cafe-acute + nihongo

    // -- the method the native module hooks to obtain a live thread --------------

    /** Hookable instance method.  The native detour on this method performs every
     *  {@code method_proxy::call()} the test asserts on. */
    public int trigger(final int delta)
    {
        triggerCount++;
        return delta + 1;
    }

    // ========================================================================
    //  boolean (Z)
    // ========================================================================
    public boolean returnsBool()      { return true; }
    public boolean returnsBoolFalse() { return false; }

    // ========================================================================
    //  byte (B) -- signed 8-bit
    // ========================================================================
    public byte returnsByte()       { return (byte) 0x7e; }   // 126 (mirrors Example)
    public byte returnsByteMax()    { return Byte.MAX_VALUE; }  // 127
    public byte returnsByteMin()    { return Byte.MIN_VALUE; }  // -128
    public byte returnsByteNegOne() { return (byte) -1; }       // sign-extension probe

    // ========================================================================
    //  short (S) -- signed 16-bit
    // ========================================================================
    public short returnsShort()       { return (short) 12345; } // mirrors Example
    public short returnsShortMax()    { return Short.MAX_VALUE; }  // 32767
    public short returnsShortMin()    { return Short.MIN_VALUE; }  // -32768
    public short returnsShortNegOne() { return (short) -1; }

    // ========================================================================
    //  char (C) -- UNSIGNED 16-bit
    // ========================================================================
    public char returnsChar()    { return '?'; }          // '?' == 63 (mirrors Example)
    public char returnsCharMax() { return (char) 0xFFFF; }      // 65535, zero-extension probe

    // ========================================================================
    //  int (I) -- signed 32-bit
    // ========================================================================
    public int returnsInt()    { return 0x12345678; }          // 305419896 (mirrors Example)
    public int returnsIntMax() { return Integer.MAX_VALUE; }   // 2147483647
    public int returnsIntMin() { return Integer.MIN_VALUE; }   // -2147483648

    // ========================================================================
    //  long (J) -- signed 64-bit (two interpreter slots)
    // ========================================================================
    public long returnsLong()    { return 0x123456789ABCDEF0L; } // mirrors Example
    public long returnsLongMin() { return Long.MIN_VALUE; }
    public long returnsLongNeg() { return -9876543210L; }        // a negative > 32-bit magnitude

    // ========================================================================
    //  float (F)
    // ========================================================================
    public float returnsFloat()    { return 3.1415926f; }                   // bits 0x40490FDA (mirrors Example)
    public float returnsFloatNaN() { return Float.NaN; }
    public float returnsFloatBits(){ return Float.intBitsToFloat(0x7f7fffff); } // FLT_MAX exact bit pattern

    // ========================================================================
    //  double (D)
    // ========================================================================
    public double returnsDouble()    { return 2.718281828459045; }                 // bits 0x4005BF0A8B145769 (mirrors Example)
    public double returnsDoubleNaN() { return Double.NaN; }
    public double returnsDoubleBits(){ return Double.longBitsToDouble(0x7fefffffffffffffL); } // DBL_MAX exact bits

    // ========================================================================
    //  java.lang.String
    // ========================================================================
    public String returnsString()        { return "hello-from-jvm"; }   // mirrors Example
    public String returnsStringEmpty()   { return ""; }                 // empty != null boundary
    public String returnsStringUnicode() { return UNICODE; }            // multibyte modified-UTF-8

    // ========================================================================
    //  Object / null-returning method
    // ========================================================================
    /** Returns Java {@code null} through an Object-typed signature -- the
     *  null/empty-wrapper boundary the native side characterizes. */
    public Object returnsNull() { return null; }

    /** Returns a non-null Object (a fresh java.lang.Object) -- the native side
     *  characterizes whether the reference decode yields a non-empty wrapper. */
    public Object returnsObject() { return new Object(); }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnTypes.go && !ReturnTypes.done;
            }

            @Override
            public void run()
            {
                // Real bytecode dispatch -> the native hook on trigger() fires,
                // and the detour exercises every return-typed method on this very
                // SINGLETON instance.
                SINGLETON.trigger(7);
                ReturnTypes.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly so
     * the native side reaches the instance methods on a stable OOP and so the
     * receiver the detour sees as {@code self} is this very object.
     */
    public static final ReturnTypes SINGLETON = new ReturnTypes();
}
