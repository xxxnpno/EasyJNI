package vmhook;

// here we store every java types to handle the unit tests
public class Example
{
    public static boolean staticBool = true;
    public static byte staticByte = 0xf;
    public static short staticShort = 0xff;
    public static int staticInt = 0xffff;
    public static long staticLong = 0xffffffffL;
    public static float staticFloat = Float.intBitsToFloat(0x7f7fffff);
    public static double staticDouble = Double.longBitsToDouble(0x7fefffffffffffffL);
    public static char staticChar = 0xffff;
    public static String staticString = "fortnite";

    public boolean notStaticBool = true;
    public byte notStaticByte = 0xf;
    public short notStaticShort = 0xff;
    public int notStaticInt = 0xffff;
    public long notStaticLong = 0xffffffffL;
    public float notStaticFloat = Float.intBitsToFloat(0x7f7fffff);
    public double notStaticDouble = Double.longBitsToDouble(0x7fefffffffffffffL);
    public char notStaticChar = 0xffff;
    public String notStaticString = "big yahu";

    public static boolean[] staticBoolArray = { true, false, true };
    public static byte[] staticByteArray = { 0x1, 0x2, 0x3 };
    public static short[] staticShortArray = { 0x10, 0x20, 0x30 };
    public static int[] staticIntArray = { 0x100, 0x200, 0x300 };
    public static long[] staticLongArray = { 0x1000L, 0x2000L, 0x3000L };
    public static float[] staticFloatArray = 
    {
        Float.intBitsToFloat(0x3f800000),
        Float.intBitsToFloat(0x40000000),
        Float.intBitsToFloat(0x40400000)
    };
    public static double[] staticDoubleArray = 
    {
        Double.longBitsToDouble(0x3ff0000000000000L),
        Double.longBitsToDouble(0x4000000000000000L),
        Double.longBitsToDouble(0x4008000000000000L)
    };
    public static char[] staticCharArray = { 'A', 'B', 'C' };
    public static String[] staticStringArray = { "hello", "world", "!" };

    public boolean[] notStaticBoolArray = { true, false, true };
    public byte[] notStaticByteArray = { 0x1, 0x2, 0x3 };
    public short[] notStaticShortArray = { 0x10, 0x20, 0x30 };
    public int[] notStaticIntArray = { 0x100, 0x200, 0x300 };
    public long[] notStaticLongArray = { 0x1000L, 0x2000L, 0x3000L };
    public float[] notStaticFloatArray = 
    {
        Float.intBitsToFloat(0x3f800000),
        Float.intBitsToFloat(0x40000000),
        Float.intBitsToFloat(0x40400000)
    };
    public double[] notStaticDoubleArray = 
    {
        Double.longBitsToDouble(0x3ff0000000000000L),
        Double.longBitsToDouble(0x4000000000000000L),
        Double.longBitsToDouble(0x4008000000000000L)
    };
    public char[] notStaticCharArray = { 'X', 'Y', 'Z' };
    public String[] notStaticStringArray = { "we", "like", "vmhook" };

    // vmhook.dll needs an instance to access non static fields and methods
    // since it can access static fields withour an instance it wiil get this one and then it will be able to obtain non static fields and methods
    public static Example instance = new Example();

    // this one will store how many times did staticCallMe got called
    public static int staticCalled = 0;

    // this one will store how many times did nonStaticCallMe got called
    public int nonStaticCalled = 0;

    // these fields coordinate the native hook unit test
    public static volatile boolean hookProbeRequested = false;
    public static volatile boolean hookProbeDone = false;

    public static void staticCallMe(final int value)
    {
        staticCalled++;
    }

    public void nonStaticCallMe(final int value)
    {
        this.nonStaticCalled++;
    }

    // this part's goal is just to check if we can work with Objects correcly
    private A a = new A();

    public void useA(final A a)
    {
        a.field++;
    }

    // ── Java-side verification of C++ setter effects ──────────────────────────
    // Called by Main after stopJVM is set to true.
    // Returns true only if every field holds the value written by C++ Phase 2.
    public static boolean verifySetterEffects()
    {
        boolean ok = true;

        // Static scalars
        ok = jcheck("staticBool",   staticBool   == false)        & ok;
        ok = jcheck("staticByte",   staticByte   == 7)             & ok;
        ok = jcheck("staticShort",  staticShort  == 127)           & ok;
        ok = jcheck("staticInt",    staticInt    == 0x7fff)        & ok;
        ok = jcheck("staticLong",   staticLong   == 0x7fffffffL)   & ok;
        ok = jcheck("staticFloat",  staticFloat  == 1.0f)          & ok;
        ok = jcheck("staticDouble", staticDouble == 2.0)           & ok;
        ok = jcheck("staticChar",   staticChar   == 'A')           & ok;
        ok = jcheck("staticString", "java_ftw".equals(staticString)) & ok;

        // Non-static scalars (on Example.instance)
        ok = jcheck("notStaticBool",   instance.notStaticBool   == false)       & ok;
        ok = jcheck("notStaticByte",   instance.notStaticByte   == 7)           & ok;
        ok = jcheck("notStaticShort",  instance.notStaticShort  == 127)         & ok;
        ok = jcheck("notStaticInt",    instance.notStaticInt    == 0x7fff)      & ok;
        ok = jcheck("notStaticLong",   instance.notStaticLong   == 0x7fffffffL) & ok;
        ok = jcheck("notStaticFloat",  instance.notStaticFloat  == 1.0f)        & ok;
        ok = jcheck("notStaticDouble", instance.notStaticDouble == 2.0)         & ok;
        ok = jcheck("notStaticChar",   instance.notStaticChar   == 'B')         & ok;
        ok = jcheck("notStaticString", "cppwins!".equals(instance.notStaticString)) & ok;

        // Static arrays
        ok = jcheck("staticBoolArray",
                !staticBoolArray[0] && staticBoolArray[1] && !staticBoolArray[2]) & ok;
        ok = jcheck("staticByteArray",
                staticByteArray[0] == 10 && staticByteArray[1] == 20 && staticByteArray[2] == 30) & ok;
        ok = jcheck("staticShortArray",
                staticShortArray[0] == 256 && staticShortArray[1] == 512 && staticShortArray[2] == 768) & ok;
        ok = jcheck("staticIntArray",
                staticIntArray[0] == 4096 && staticIntArray[1] == 8192 && staticIntArray[2] == 12288) & ok;
        ok = jcheck("staticLongArray",
                staticLongArray[0] == 65536L && staticLongArray[1] == 131072L && staticLongArray[2] == 196608L) & ok;
        ok = jcheck("staticFloatArray",
                staticFloatArray[0] == 4.0f && staticFloatArray[1] == 5.0f && staticFloatArray[2] == 6.0f) & ok;
        ok = jcheck("staticDoubleArray",
                staticDoubleArray[0] == 4.0 && staticDoubleArray[1] == 5.0 && staticDoubleArray[2] == 6.0) & ok;
        ok = jcheck("staticCharArray",
                staticCharArray[0] == 'X' && staticCharArray[1] == 'Y' && staticCharArray[2] == 'Z') & ok;
        // Note: "alpha"/"omega"/"?" are used (not "world"/"hello") to avoid
        // interning collisions — modifying "hello"->"world" and "world"->"hello"
        // would corrupt the JVM string pool for those literals.
        ok = jcheck("staticStringArray",
                "alpha".equals(staticStringArray[0]) &&
                "omega".equals(staticStringArray[1]) &&
                "?".equals(staticStringArray[2])) & ok;

        // Non-static arrays
        ok = jcheck("notStaticBoolArray",
                !instance.notStaticBoolArray[0] && instance.notStaticBoolArray[1] && !instance.notStaticBoolArray[2]) & ok;
        ok = jcheck("notStaticByteArray",
                instance.notStaticByteArray[0] == 10 && instance.notStaticByteArray[1] == 20 && instance.notStaticByteArray[2] == 30) & ok;
        ok = jcheck("notStaticShortArray",
                instance.notStaticShortArray[0] == 256 && instance.notStaticShortArray[1] == 512 && instance.notStaticShortArray[2] == 768) & ok;
        ok = jcheck("notStaticIntArray",
                instance.notStaticIntArray[0] == 4096 && instance.notStaticIntArray[1] == 8192 && instance.notStaticIntArray[2] == 12288) & ok;
        ok = jcheck("notStaticLongArray",
                instance.notStaticLongArray[0] == 65536L && instance.notStaticLongArray[1] == 131072L && instance.notStaticLongArray[2] == 196608L) & ok;
        ok = jcheck("notStaticFloatArray",
                instance.notStaticFloatArray[0] == 4.0f && instance.notStaticFloatArray[1] == 5.0f && instance.notStaticFloatArray[2] == 6.0f) & ok;
        ok = jcheck("notStaticDoubleArray",
                instance.notStaticDoubleArray[0] == 4.0 && instance.notStaticDoubleArray[1] == 5.0 && instance.notStaticDoubleArray[2] == 6.0) & ok;
        ok = jcheck("notStaticCharArray",
                instance.notStaticCharArray[0] == 'D' && instance.notStaticCharArray[1] == 'E' && instance.notStaticCharArray[2] == 'F') & ok;
        ok = jcheck("notStaticStringArray",
                "ab".equals(instance.notStaticStringArray[0]) &&
                "love".equals(instance.notStaticStringArray[1]) &&
                "coding".equals(instance.notStaticStringArray[2])) & ok;

        return ok;
    }

    private static boolean jcheck(final String name, final boolean condition)
    {
        System.out.println(condition ? "[JAVA PASS] " + name : "[JAVA FAIL] " + name);
        return condition;
    }
}
