package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_static feature (area: fields).
 *
 * Focus: STATIC field GET and (above all) SET through vmhook's zero-JNI
 * static_field("name") accessor, for EVERY JVM primitive (Z B C S I J F D),
 * java.lang.String, and an object reference -- with the written value proven
 * VISIBLE TO JAVA ITSELF (the contract: set-then-read-back through Java).
 *
 * Division of labour vs the sibling field modules:
 *   - field_primitives_get already covers static GET decoding/variant-index.
 *   - field_string already covers String GET/SET decode corner cases.
 *   - THIS fixture is the SET + Java-readback + GCC-portability authority:
 *       (a) the native side writes each static field via static_field(name)->set(v)
 *           BEFORE raising `go` (field_proxy::set mutates heap memory directly,
 *           no bytecode needed),
 *       (b) the probe (mode 1) then snapshots every static field into a parallel
 *           "seen*" witness field using GENUINE putstatic / getstatic bytecode,
 *           so the native side reads back what the *JVM* observed, not what C++
 *           thinks it wrote, and
 *       (c) Java getter methods (getX()) return each field so the native side can
 *           additionally pull the value back through static_method("getX")->call()
 *           -- which also exercises that static_field/static_method work when
 *           CALLED FROM A STATIC C++ WRAPPER METHOD on every compiler incl. GCC.
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = snapshot every settable static field into its seen* witness (putstatic),
 *       AND publish each value via the getX() getters' results into seen* too.
 *   2 = writeRuntime(): putstatic fresh boundary values so native GET sees live,
 *       post-dispatch state (proves get() is not reading stale init constants).
 *   3 = resetTargets(): restore the set* targets to known initial values so the
 *       module could (in principle) re-run; also re-publishes objA into objRef.
 *   4 = objA.touch(0x5A): a real bytecode dispatch so the native interpreter
 *       hook on touch() fires; INSIDE that detour the module pulls every static
 *       field back through the getX() getters (method_proxy::call needs a live
 *       current_java_thread, which only exists on the Java thread in a detour).
 *
 * PORTABILITY/ENCODING NOTES:
 *   - All char values are numeric / \\uXXXX escapes (lexer-level, encoding
 *     independent) so javac on Cp1252 (Windows) and UTF-8 (Linux/macOS) agree.
 *   - String SET targets are ASCII and are only ever overwritten with an
 *     equal-or-shorter ASCII value, so the in-place backing-array write is
 *     deterministic across JDK 8 (char[]) and JDK 9+ (compact LATIN1/UTF-16),
 *     where write_java_string keeps the existing length and never resizes.
 *
 * Java 8 syntax only (anonymous class, no var/lambda-in-field/switch-expr).
 */
public final class FieldStatic
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    /** Scenario selector; native sets it before raising go. */
    public static volatile int mode;

    // =====================================================================
    //  SET TARGETS -- the native side writes these via static_field(name)->set().
    //  Initial values are deliberately DISTINCT from the values the native
    //  side writes, so a no-op write (e.g. a silently-dropped set) is caught:
    //  the seen* witness would still equal the initial value.
    // =====================================================================
    public static boolean setZ = false;           // native writes true
    public static byte    setB = 0;               // native writes Byte.MIN_VALUE (-128)
    public static char    setC = 0x0000;          // native writes 0xFFFF
    public static short   setS = 0;               // native writes Short.MIN_VALUE
    public static int     setI = 0;               // native writes Integer.MIN_VALUE
    public static long    setJ = 0L;              // native writes Long.MAX_VALUE
    public static float   setF = 0.0f;            // native writes a known bit pattern
    public static double  setD = 0.0;             // native writes a known bit pattern

    // A second battery of SET targets so native can write boundary/edge values
    // independent of the "primary" battery above (more angles, no aliasing).
    public static boolean setZ2 = true;           // native writes false
    public static byte    setB2 = 0;              // native writes (byte)0xFF == -1
    public static char    setC2 = 0x0041;         // native writes 0x20AC (euro)
    public static short   setS2 = 0;              // native writes (short)0xBEEF
    public static int     setI2 = 0;              // native writes 0xDEADBEEF
    public static long    setJ2 = 0L;             // native writes Long.MIN_VALUE
    public static float   setF2 = 0.0f;           // native writes Float.NEGATIVE_INFINITY
    public static double  setD2 = 0.0;            // native writes Double.NaN (canonical)

    // Mid-range "ordinary" values, to prove the common case (not just extremes).
    public static int     setIOrd = -1;           // native writes 123456789
    public static long    setJOrd = -1L;          // native writes 0x0123456789ABCDEF
    public static double  setDOrd = -1.0;         // native writes Math.PI
    public static float   setFOrd = -1.0f;        // native writes 1.5f (exact in binary)

    // String SET target: ASCII, length 5, overwritten with an equal-length
    // ASCII value ("world").  In-place write keeps length on all JDKs.
    public static String  setStr = "AAAAA";       // native writes "world"

    // String SET target overwritten with a SHORTER value ("hi") -> partial
    // overwrite leaves the tail: Java should see "hirld" (length unchanged).
    public static String  setStrShort = "world";  // native writes "hi" -> "hirld"

    // =====================================================================
    //  PRIMITIVE-vs-PRIMITIVE TYPE/SIZE GUARD targets (audit:
    //  field_proxy_set_size_guard.md).  The native side attempts MISTYPED
    //  writes and we assert (Java-side) the value is UNCHANGED:
    //   - guardInt: native tries set(int64) and set(std::string) -> refused.
    //   - guardLong: native tries set(int32) -> refused (too narrow).
    //   - guardChar: native writes a 1-byte C++ char -> widening shortcut
    //                must land the full 2-byte char (0x00NN), not clobber.
    // =====================================================================
    public static int     guardInt  = 0x11223344; // must remain unchanged
    public static long    guardLong  = 0x1122334455667788L; // must remain unchanged
    public static char    guardChar = 0x0000;      // native writes 'Z'(0x5A) via 1-byte char

    // =====================================================================
    //  OBJECT-REFERENCE SET targets.  objA / objB are two distinct published
    //  instances.  The native side rewrites objRef (initially objA) to point
    //  at objB via set(unique_ptr<wrapper>), then to null via an empty
    //  unique_ptr.  The probe records identity comparisons Java-side.
    // =====================================================================
    public static FieldStatic objA = new FieldStatic();
    public static FieldStatic objB = new FieldStatic();
    public static FieldStatic objRef = objA;       // native rewrites to objB, then null

    // A live instance the native side can wrap for instance-side cross-checks
    // (and to disambiguate static vs instance handling).  Carries a tag so the
    // two published instances are distinguishable when read back.
    public int tag = 0;

    // =====================================================================
    //  GET-only static fields with boundary values, so the native GET path is
    //  re-proven here under the field_static module too (independent of
    //  field_primitives_get).  These are never written by native.
    // =====================================================================
    public static boolean gZTrue  = true;
    public static boolean gZFalse = false;
    public static byte    gBMin   = Byte.MIN_VALUE;
    public static byte    gBMax   = Byte.MAX_VALUE;
    public static short   gSMin   = Short.MIN_VALUE;
    public static short   gSMax   = Short.MAX_VALUE;
    public static char    gCMax   = 0xFFFF;
    public static int     gIMin   = Integer.MIN_VALUE;
    public static int     gIMax   = Integer.MAX_VALUE;
    public static long    gJMin   = Long.MIN_VALUE;
    public static long    gJMax   = Long.MAX_VALUE;
    public static float   gFOne   = Float.intBitsToFloat(0x3F800000); // +1.0
    public static double  gDOne   = Double.longBitsToDouble(0x3FF0000000000000L); // +1.0
    public static String  gStr    = "field_static";

    /** An instance (non-static) int field, to drive the "needs an object" path. */
    public int instanceOnlyInt = 4242;

    /**
     * Hookable instance method.  The native module hooks this and, from INSIDE
     * the detour (where HotSpot's current_java_thread is set, the precondition
     * for method_proxy::call()), pulls each native-written static field back
     * through the Java getX() getters via static_method("getX")->call().  The
     * probe (mode 4) calls this once on objA so the detour fires on a real
     * bytecode dispatch.  Returns delta unchanged so the caller can sanity it.
     */
    public int touch(final int delta)
    {
        return delta;
    }

    // =====================================================================
    //  WITNESS fields ("seen*") -- the probe writes these from the set*
    //  targets using genuine getstatic/putstatic bytecode in run() (mode 1).
    //  The native side reads these back to confirm Java observed the writes.
    // =====================================================================
    public static boolean seenZ;
    public static byte    seenB;
    public static char    seenC;
    public static short   seenS;
    public static int     seenI;
    public static long    seenJ;
    public static int     seenFBits;   // Float.floatToRawIntBits(setF)
    public static long    seenDBits;   // Double.doubleToRawLongBits(setD)

    public static boolean seenZ2;
    public static byte    seenB2;
    public static char    seenC2;
    public static short   seenS2;
    public static int     seenI2;
    public static long    seenJ2;
    public static int     seenF2Bits;
    public static long    seenD2Bits;

    public static int     seenIOrd;
    public static long    seenJOrd;
    public static long    seenDOrdBits;
    public static int     seenFOrdBits;

    public static String  seenStr;        // copy of setStr seen by Java
    public static int      seenStrLen;     // setStr.length() seen by Java
    public static boolean  seenStrEqWorld; // setStr.equals("world")
    public static String   seenStrShort;   // copy of setStrShort
    public static int       seenStrShortLen;

    public static int      seenGuardInt;
    public static long      seenGuardLong;
    public static char      seenGuardChar;

    public static boolean  seenObjRefIsA;  // objRef == objA at snapshot time
    public static boolean  seenObjRefIsB;  // objRef == objB at snapshot time
    public static boolean  seenObjRefIsNull;
    public static int       seenObjRefTag;  // objRef == null ? -1 : objRef.tag

    // =====================================================================
    //  RUNTIME GET targets (mode 2) -- written by writeRuntime() via putstatic
    //  so the native GET path reads live, post-dispatch JVM state.
    // =====================================================================
    public static boolean rZ;
    public static int     rI;
    public static long    rJ;
    public static double  rD;
    public static char    rC;

    // ---- Java getter methods (called natively via static_method to prove the
    //      static-wrapper-method path + that Java bytecode reads the native
    //      write).  Each returns the current field value. ----
    public static boolean getZ() { return setZ; }
    public static byte    getB() { return setB; }
    public static char    getC() { return setC; }
    public static short   getS() { return setS; }
    public static int     getI() { return setI; }
    public static long    getJ() { return setJ; }
    public static float   getF() { return setF; }
    public static double  getD() { return setD; }
    public static int     getIOrd() { return setIOrd; }
    public static String  getStr() { return setStr; }
    public static int     getStrLen() { return setStr == null ? -1 : setStr.length(); }
    public static int     getGuardInt() { return guardInt; }
    public static long    getGuardLong() { return guardLong; }
    public static char    getGuardChar() { return guardChar; }
    public static int     getObjRefTag() { return objRef == null ? -1 : objRef.tag; }
    public static boolean objRefIsB() { return objRef == objB; }
    public static boolean objRefIsNull() { return objRef == null; }

    private static void snapshot()
    {
        // getstatic each set* target, putstatic into the witness -- genuine
        // bytecode so the JVM's own view of the field is captured.
        seenZ = setZ;
        seenB = setB;
        seenC = setC;
        seenS = setS;
        seenI = setI;
        seenJ = setJ;
        seenFBits = Float.floatToRawIntBits(setF);
        seenDBits = Double.doubleToRawLongBits(setD);

        seenZ2 = setZ2;
        seenB2 = setB2;
        seenC2 = setC2;
        seenS2 = setS2;
        seenI2 = setI2;
        seenJ2 = setJ2;
        seenF2Bits = Float.floatToRawIntBits(setF2);
        seenD2Bits = Double.doubleToRawLongBits(setD2);

        seenIOrd = setIOrd;
        seenJOrd = setJOrd;
        seenDOrdBits = Double.doubleToRawLongBits(setDOrd);
        seenFOrdBits = Float.floatToRawIntBits(setFOrd);

        seenStr = setStr;
        seenStrLen = (setStr == null) ? -1 : setStr.length();
        seenStrEqWorld = "world".equals(setStr);
        seenStrShort = setStrShort;
        seenStrShortLen = (setStrShort == null) ? -1 : setStrShort.length();

        seenGuardInt = guardInt;
        seenGuardLong = guardLong;
        seenGuardChar = guardChar;

        seenObjRefIsA = (objRef == objA);
        seenObjRefIsB = (objRef == objB);
        seenObjRefIsNull = (objRef == null);
        seenObjRefTag = (objRef == null) ? -1 : objRef.tag;
    }

    private static void writeRuntime()
    {
        rZ = true;
        rI = Integer.MIN_VALUE;
        rJ = Long.MAX_VALUE;
        rD = Double.longBitsToDouble(0x7FF8000000000000L); // canonical NaN
        rC = 0xFFFF;
    }

    private static void resetTargets()
    {
        setStr = "AAAAA";
        setStrShort = "world";
        guardInt = 0x11223344;
        guardLong = 0x1122334455667788L;
        guardChar = 0x0000;
        objRef = objA;
    }

    static
    {
        objA.tag = 0xA;
        objB.tag = 0xB;

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldStatic.go && !FieldStatic.done;
            }

            @Override
            public void run()
            {
                switch (FieldStatic.mode)
                {
                    case 1:
                        snapshot();
                        break;
                    case 2:
                        writeRuntime();
                        break;
                    case 3:
                        resetTargets();
                        break;
                    case 4:
                        // Real bytecode dispatch so the native interpreter hook
                        // on touch() fires; the detour does the getX() call-backs.
                        objA.touch(0x5A);
                        break;
                    default:
                        break;
                }
                FieldStatic.done = true;
            }
        });
    }
}
