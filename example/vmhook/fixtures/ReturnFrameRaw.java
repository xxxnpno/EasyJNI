package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the return_value_frame_raw_access feature (area: hooks).
 *
 * The feature under test is, FROM INSIDE A HOOK detour, raw access to the live
 * HotSpot interpreter frame the trampoline stashed in the {@code return_value}:
 *   - {@code ret.frame()} returns a non-null {@code hotspot::frame*} for a real
 *     interpreted dispatch;
 *   - {@code ret.frame()->get_method()} is the SAME method the hook was installed
 *     on (name + descriptor match);
 *   - {@code ret.frame()->get_locals()} is the live local-variable array — its
 *     slot 0 holds {@code this} (the receiver oop) for an instance method, and
 *     holds the FIRST primitive arg (NOT an oop) for a static method;
 *   - reading raw primitive arg slots off {@code get_locals()} reproduces the
 *     Java argument values, respecting the HotSpot slot model: a {@code long} /
 *     {@code double} occupies TWO slots and its 64-bit value is stored at the
 *     LOWER address {@code locals[-(slot+1)]}, while the NEXT argument shifts by
 *     two slot offsets;
 *   - the same locals array that {@code frame()} exposes is the one
 *     {@code ret.set_arg(...)} mutates (raw read and typed write agree);
 *   - out-of-range slot reads return a default and never crash the JVM.
 *
 * Because the raw frame is the live {@code rbp} of the intercepted interpreter
 * activation, every method below must execute as genuine Java bytecode on the
 * Java thread (the only thing that makes the interpreter hook fire).  Each method
 * is hooked INDEPENDENTLY on the native side, so the single {@code go}/{@code done}
 * probe runs every method exactly once and the native module gathers all of its
 * raw-frame observations from that one cycle.
 *
 * Every method just echoes the argument(s) it actually received into observable
 * {@code static} fields so the native side can cross-check its raw-frame reads
 * against ground truth (and so allow-through is provable: the body still ran).
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks).
 */
public final class ReturnFrameRaw
{
    /** Native raises this to request the probe action; lowers it afterwards. */
    public static volatile boolean go;

    /** The probe action raises this when it has run; native polls it. */
    public static volatile boolean done;

    /** How many times the probe body has run (handshake proof). */
    public static volatile int probeTicks = 0;

    // A singleton receiver the probe uses for the instance-method dispatches.
    // `tag` is a per-instance fingerprint the native side reads THROUGH the oop
    // it recovers from local slot 0 — proving slot 0 really is this receiver.
    public static final int INSTANCE_TAG = 0x5A11AB1E;   // arbitrary recognisable
    public final int tag = INSTANCE_TAG;
    public static final ReturnFrameRaw instance = new ReturnFrameRaw();

    // ---- Constant args (mirrored on the native side) -----------------------
    // Chosen so each fits its width and no two collide, and so the long/double
    // halves are non-trivial (catch a halves-swap on the two-slot read).
    public static final int    SIMPLE_X = 0x1234;
    public static final int    WIDE_A   = 0x0A0B0C0D;            // int  -> slot 1
    public static final long   WIDE_B   = 0x1122334455667788L;   // long -> slots 2..3
    public static final double WIDE_C   = 3.141592653589793;     // double -> slots 4..5
    public static final int    WIDE_D   = -0x0708090A;           // int  -> slot 6 (after two wides)
    public static final int    STATIC_A = 0x00C0FFEE;            // static int -> slot 0
    public static final long   STATIC_B = 0x7EDCBA9876543210L;   // static long -> slots 1..2
    public static final int    STATIC_C = 0x1BADD00D;            // static int -> slot 3 (after long)

    // ---- Echoed observations (what each body actually received) -------------
    public static volatile int    simpleSeen   = 0;
    public static volatile int    wideASeen    = 0;
    public static volatile long   wideBSeen    = 0L;
    public static volatile long   wideCBitsSeen = 0L;            // raw IEEE-754 bits
    public static volatile int    wideDSeen    = 0;
    public static volatile int    staticASeen  = 0;
    public static volatile long   staticBSeen  = 0L;
    public static volatile int    staticCSeen  = 0;

    // set_arg round-trip target: the body echoes what it actually observed AFTER
    // any in-hook mutation, so the native side can confirm frame()'s locals
    // alias the array set_arg writes.
    public static volatile int    roundTripSeen = 0;

    // ---- Hookable methods ---------------------------------------------------

    /**
     * Minimal instance method.  Used for receiver / method-identity / bounds:
     *   slot 0 = this, slot 1 = x.
     */
    public int instanceSimple(final int x)
    {
        simpleSeen = x;
        return this.tag + x;
    }

    /**
     * Wide instance method exercising the full slot model:
     *   slot 0 = this, slot 1 = a (int), slots 2..3 = b (long, value at base
     *   slot 2), slots 4..5 = c (double, value at base slot 4), slot 6 = d (int).
     */
    public long instanceWide(final int a, final long b, final double c, final int d)
    {
        wideASeen     = a;
        wideBSeen     = b;
        wideCBitsSeen = Double.doubleToRawLongBits(c);
        wideDSeen     = d;
        return this.tag + a + b + (long) c + d;
    }

    /**
     * Static twin: NO {@code this}, so the first primitive arg sits at slot 0.
     *   slot 0 = a (int), slots 1..2 = b (long, value at base slot 1),
     *   slot 3 = c (int).
     */
    public static long staticWide(final int a, final long b, final int c)
    {
        staticASeen = a;
        staticBSeen = b;
        staticCSeen = c;
        return (long) a + b + c;
    }

    /**
     * set_arg round-trip target: slot 0 = this, slot 1 = value.  The native hook
     * mutates slot 1 via set_arg, then reads it back through frame()->get_locals()
     * to prove both reach the same array; the body records what it finally saw.
     */
    public int roundTrip(final int value)
    {
        roundTripSeen = value;
        return value;
    }

    private void runAll()
    {
        // Each call is one real bytecode dispatch -> the matching interpreter
        // hook fires and reads (and for roundTrip, mutates) the live frame.
        this.instanceSimple(SIMPLE_X);
        this.instanceWide(WIDE_A, WIDE_B, WIDE_C, WIDE_D);
        staticWide(STATIC_A, STATIC_B, STATIC_C);
        this.roundTrip(7);
        probeTicks++;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnFrameRaw.go && !ReturnFrameRaw.done;
            }

            @Override
            public void run()
            {
                ReturnFrameRaw.instance.runAll();
                ReturnFrameRaw.done = true;
            }
        });
    }
}
