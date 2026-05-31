package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_is_reference feature (area: methods).
 *
 * The ONE thing under test: {@code vmhook::method_proxy::is_reference()} — the
 * O(1) introspection accessor that reports whether a method's RETURN type is a
 * Java reference (object / array, descriptor char 'L' or '[') as opposed to a
 * primitive (Z B S C I J F D) or void (V).  It reads the character that follows
 * the closing ')' of the JVM descriptor; it requires NO live bytecode dispatch
 * (it never calls the method), so the native module asserts everything straight
 * from the resolved proxies — no call() and therefore no current_java_thread is
 * needed for the core coverage.
 *
 * This fixture supplies a method per return KIND, in INSTANCE and STATIC flavours
 * so the native side exercises both the instance {@code get_method("name")} path
 * and the static {@code static_method("name")} path:
 *
 *   primitive returns (is_reference() must be FALSE):
 *     retBool   ()Z      retByte ()B      retShort()S      retChar ()C
 *     retInt    ()I      retLong ()J      retFloat()F      retDouble()D
 *     retVoid   ()V      (V is "not a reference" — the close+1 == 'V' branch)
 *
 *   reference returns (is_reference() must be TRUE):
 *     retString ()Ljava/lang/String;   retObject ()Ljava/lang/Object;
 *     retIntArray ()[I                 retStringArray ()[Ljava/lang/String;
 *
 * Each of the above has an {@code s}-prefixed STATIC twin with the IDENTICAL
 * return descriptor, so is_reference() is proven independent of static-ness.
 *
 * Overload disambiguation: {@code dual(...)} is a single NAME carrying two
 * overloads that differ ONLY in return type-class, selectable purely by explicit
 * JVM descriptor:
 *     dual(I)I                            -> primitive return  (is_reference FALSE)
 *     dual(Ljava/lang/String;)Ljava/lang/Object;  -> reference return (TRUE)
 * (Java forbids overloading by return type alone, so the two overloads also
 * differ in their parameter list; the native side resolves each by its EXACT
 * descriptor and proves is_reference() tracks the RESOLVED overload, not the bare
 * name.)  A static twin {@code sdual(...)} mirrors this for the static path.
 *
 * NOTE: the method bodies here are never executed by the test — is_reference() is
 * pure metadata.  They return harmless constants only so the class verifies and
 * loads.  The {@code SINGLETON} field exists so the native side can fetch an
 * instance (the standard "get an instance to reach the instance get_method path"
 * trick) without driving any Java code.  The Harness.Probe below is a trivial
 * handshake required by the modular-harness contract; the module does not need it
 * for is_reference(), so its run() is a pure no-op flag flip.
 *
 * Java 8 syntax only (no var / records / switch-expressions / text-blocks); only
 * java.* + vmhook.Harness are referenced.
 */
public final class IsReference
{
    /** Native sets this true to request the (no-op) action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    // ======================================================================
    //  INSTANCE primitive returns — is_reference() must be FALSE for each.
    // ======================================================================
    public boolean retBool()   { return true; }
    public byte    retByte()   { return (byte) 1; }
    public short   retShort()  { return (short) 2; }
    public char    retChar()   { return 'A'; }
    public int     retInt()    { return 3; }
    public long    retLong()   { return 4L; }
    public float   retFloat()  { return 0.5f; }
    public double  retDouble() { return 6.0; }
    public void    retVoid()   { /* V: not a reference */ }

    // ======================================================================
    //  INSTANCE reference returns — is_reference() must be TRUE for each.
    // ======================================================================
    public String   retString()      { return "ref"; }
    public Object   retObject()       { return this; }
    public int[]    retIntArray()     { return INT_ARRAY; }
    public String[] retStringArray()  { return STRING_ARRAY; }

    // ======================================================================
    //  STATIC primitive returns (identical descriptors to the instance ones).
    // ======================================================================
    public static boolean sRetBool()   { return false; }
    public static byte     sRetByte()   { return (byte) -1; }
    public static short    sRetShort()  { return (short) -2; }
    public static char     sRetChar()   { return 'B'; }
    public static int      sRetInt()    { return -3; }
    public static long     sRetLong()   { return -4L; }
    public static float    sRetFloat()  { return -0.5f; }
    public static double   sRetDouble() { return -6.0; }
    public static void     sRetVoid()   { /* V: not a reference */ }

    // ======================================================================
    //  STATIC reference returns (identical descriptors to the instance ones).
    // ======================================================================
    public static String   sRetString()     { return "sref"; }
    public static Object    sRetObject()      { return STATIC_SELF; }
    public static int[]     sRetIntArray()    { return INT_ARRAY; }
    public static String[]  sRetStringArray() { return STRING_ARRAY; }

    // ======================================================================
    //  Overloaded pair: ONE name, primitive vs reference return, told apart
    //  ONLY by explicit JVM descriptor.  Proves is_reference() tracks the
    //  specific RESOLVED overload, not merely the method name.
    // ======================================================================

    /** dual(I)I — primitive return. */
    public int dual(final int a) { return a; }

    /** dual(Ljava/lang/String;)Ljava/lang/Object; — reference return. */
    public Object dual(final String s) { return s; }

    /** sdual(I)I — static primitive return. */
    public static int sdual(final int a) { return a; }

    /** sdual(Ljava/lang/String;)Ljava/lang/Object; — static reference return. */
    public static Object sdual(final String s) { return s; }

    // ======================================================================
    //  Fixed reference payloads (so the array returners are well-defined).
    // ======================================================================
    private static final int[]    INT_ARRAY    = { 10, 20, 30 };
    private static final String[] STRING_ARRAY = { "x", "y" };

    /** A stable object the static Object returner hands back. */
    private static final Object STATIC_SELF = new Object();

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return IsReference.go && !IsReference.done;
            }

            @Override
            public void run()
            {
                // is_reference() is pure metadata — nothing to dispatch.  The
                // handshake exists only to satisfy the harness contract; flip
                // done so any native run_probe() resolves cleanly.
                IsReference.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps so it can drive the INSTANCE
     * {@code get_method("name")} resolution path.  Created eagerly so the native
     * side can fetch it via the {@code SINGLETON} static reference field without
     * executing any Java code.
     */
    public static final IsReference SINGLETON = new IsReference();
}
