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
}