package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code return_value::set_arg(index, value)} feature.
 *
 * <p>The native module ({@code tests/jvm/modules/return_set_arg.cpp}) installs an
 * interpreter hook on each of the methods below.  Inside the hook it calls
 * {@code retval.set_arg(slot, replacement)} to mutate an argument <em>before</em>
 * the original method body runs.  Every method here is deliberately tiny: it just
 * copies the argument(s) it actually receives into an observable {@code static}
 * field.  Because the hook fires first and mutates the interpreter local slot,
 * the value the method body sees — and therefore the value it stores — reflects
 * the mutation.  The native side then reads the field back and asserts the
 * post-mutation value.</p>
 *
 * <p>The whole suite is driven by a single {@code go}/{@code done} handshake: the
 * registered probe's {@code run()} calls every test method once (a real bytecode
 * dispatch, which is the only thing that makes an interpreter hook fire), then
 * raises {@code done}.  The native module installs all of its scoped hooks up
 * front, fires the probe once, and checks every observation.</p>
 *
 * <p>Slot model exercised (HotSpot x64 interpreter, where every local is an
 * 8-byte slot):</p>
 * <ul>
 *   <li>instance method: slot 0 = {@code this}, slot 1 = first arg, slot 2 =
 *       second arg (or the reserved second half of a preceding long/double);</li>
 *   <li>static method: slot 0 = first arg;</li>
 *   <li>a {@code long}/{@code double} argument reserves two slot <em>offsets</em>
 *       for the purposes of locating the <em>next</em> argument, but its 64-bit
 *       value is read and written at its single base slot.</li>
 * </ul>
 *
 * Target Java 8 syntax only (no var / records / switch-expressions).
 */
public final class ReturnSetArg
{
    /** Native raises this to request the probe action; lowers it afterwards. */
    public static volatile boolean go;

    /** The probe action raises this when it has run; native polls it. */
    public static volatile boolean done;

    // A singleton instance the probe uses for the instance-method dispatches.
    public static final ReturnSetArg instance = new ReturnSetArg();

    // ── int (instance, slot 1) ────────────────────────────────────────────
    // intSeen records exactly what the body received for `value`.
    public static volatile int intSeen = 0;
    public void takeInt(final int value) { intSeen = value; }

    // ── int (static, slot 0) ──────────────────────────────────────────────
    public static volatile int staticIntSeen = 0;
    public static void takeStaticInt(final int value) { staticIntSeen = value; }

    // ── boolean (instance, slot 1) ────────────────────────────────────────
    // Recorded into an int (0/1) so the native side can compare without
    // relying on field-proxy boolean decoding.
    public static volatile int boolSeen = -1;
    public void takeBool(final boolean value) { boolSeen = value ? 1 : 0; }

    // ── byte (instance, slot 1) ───────────────────────────────────────────
    // Recorded as int WITHOUT an explicit (byte) cast on read, so the value
    // stored is exactly what `iload` produced from the slot.  This is the
    // sign-extension probe: a correct set_arg(-1) yields -1; a non-sign-
    // extending one yields 255.
    public static volatile int byteSeen = 0;
    public void takeByte(final byte value) { byteSeen = value; }

    // ── short (instance, slot 1) ──────────────────────────────────────────
    // Same sign-extension probe as byte, for the 16-bit case.
    public static volatile int shortSeen = 0;
    public void takeShort(final short value) { shortSeen = value; }

    // ── char (instance, slot 1) ───────────────────────────────────────────
    // char is unsigned 16-bit; recorded as int.
    public static volatile int charSeen = 0;
    public void takeChar(final char value) { charSeen = value; }

    // ── long (instance, slots 1..2; value at base slot 1) ─────────────────
    public static volatile long longSeen = 0L;
    public void takeLong(final long value) { longSeen = value; }

    // ── double (instance, slots 1..2; value at base slot 1) ───────────────
    // Recorded both as the double itself and as its raw IEEE-754 bits so the
    // native side can compare exactly without float formatting ambiguity.
    public static volatile double doubleSeen = 0.0;
    public static volatile long   doubleBitsSeen = 0L;
    public void takeDouble(final double value)
    {
        doubleSeen = value;
        doubleBitsSeen = Double.doubleToRawLongBits(value);
    }

    // ── float (instance, slot 1) ──────────────────────────────────────────
    public static volatile float floatSeen = 0.0f;
    public static volatile int   floatBitsSeen = 0;
    public void takeFloat(final float value)
    {
        floatSeen = value;
        floatBitsSeen = Float.floatToRawIntBits(value);
    }

    // ── String (instance, slot 1) ─────────────────────────────────────────
    // Records the exact reference content the body saw.  null when the body
    // received null.  length recorded separately so very long / unicode
    // strings can be asserted without shipping the whole content back.
    public static volatile String stringSeen = "<unset>";
    public static volatile int    stringLenSeen = -1;
    public static volatile boolean stringWasNull = false;
    public void takeString(final String value)
    {
        stringSeen = value;
        stringWasNull = (value == null);
        stringLenSeen = (value == null) ? -1 : value.length();
    }

    // A second String method used for the const char* / char* overload and
    // for the leak-pressure loop, kept separate so its observation does not
    // race with takeString.
    public static volatile String stringSeen2 = "<unset>";
    public static volatile int    stringLenSeen2 = -1;
    public void takeString2(final String value)
    {
        stringSeen2 = value;
        stringLenSeen2 = (value == null) ? -1 : value.length();
    }

    // ── long + int (instance): slot model with a J/D in front ─────────────
    // this=slot0, a (long)=slot1 (reserves offsets 1..2), b (int)=slot3.
    // The native side mutates `b` at slot 3 (correct) and separately probes
    // slot 2 (the reserved second half) to show it does NOT change `b`.
    public static volatile long mixLongSeen = 0L;
    public static volatile int  mixIntSeen  = 0;
    public void mixLongInt(final long a, final int b)
    {
        mixLongSeen = a;
        mixIntSeen  = b;
    }

    // ── int + int (instance): two consecutive single-slot args ────────────
    // this=slot0, a=slot1, b=slot2.  Used to prove the second arg is at
    // slot 2 and that mutating slot 1 vs slot 2 targets the intended one.
    public static volatile int twoFirstSeen  = 0;
    public static volatile int twoSecondSeen = 0;
    public void twoInts(final int a, final int b)
    {
        twoFirstSeen  = a;
        twoSecondSeen = b;
    }

    // ── int (instance): the out-of-range / bounds probe target ────────────
    // The native side attempts set_arg with out-of-range indices on this
    // method and asserts the original argument survives unscathed.
    public static volatile int boundsSeen = 0;
    public void boundsTarget(final int value) { boundsSeen = value; }

    // Tiny no-arg dispatch the probe uses so even a fully-failed feature still
    // makes `done` flip (the native side can then tell "probe ran" apart from
    // "individual method observation").
    public static volatile int probeTicks = 0;
    public void tick() { probeTicks++; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnSetArg.go && !ReturnSetArg.done;
            }

            @Override
            public void run()
            {
                final ReturnSetArg self = ReturnSetArg.instance;

                // Each call is a real bytecode dispatch -> the matching
                // interpreter hook fires and mutates the argument slot before
                // the body below stores it.  Sentinel "original" values are
                // chosen so a no-op hook would leave a recognisably different
                // observation than a successful mutation.
                self.tick();

                self.takeInt(7);                 // hook -> 42
                ReturnSetArg.takeStaticInt(7);   // hook -> 4242 (slot 0)
                self.takeBool(false);            // hook -> true
                self.takeByte((byte) 1);         // hook -> (byte)-1
                self.takeShort((short) 1);       // hook -> (short)-1
                self.takeChar('A');              // hook -> 0x2764 (heavy black heart)
                self.takeLong(1L);               // hook -> 0x0123456789ABCDEF
                self.takeDouble(1.0);            // hook -> 12.5
                self.takeFloat(1.0f);            // hook -> 3.5f

                self.takeString("before");       // hook -> "after" (string_view)
                self.takeString2("before");      // hook -> "cc" (const char*)

                self.mixLongInt(100L, 7);        // hook -> a kept, b -> 99 (slot 3)
                self.twoInts(5, 6);              // hook -> a -> 50, b -> 60

                self.boundsTarget(7);            // hook attempts OOB set_arg; arg stays 7

                ReturnSetArg.done = true;
            }
        });
    }
}
