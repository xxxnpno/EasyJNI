package vmhook.example;

/**
 * Test target for VMHook.  Every field here is verified by the C++ test harness
 * inside VMHook.dll.  Add new cases here, then add the corresponding assertion
 * in VMHook/src/test.hpp.
 *
 * Layout intent (JVM x64 with compressed OOPs):
 *   header     +0   mark word (8 B)
 *   header     +8   compressed klass ptr (4 B)
 *   padding    +12  alignment
 *   fieldBool  +12  Z  (1 B, packed with others)
 *   fieldByte  +13  B  (1 B)
 *   fieldShort +14  S  (2 B)
 *   fieldInt   +16  I  (4 B)
 *   fieldFloat +20  F  (4 B)
 *   fieldLong  +24  J  (8 B, 8-byte aligned)
 *   fieldDouble+32  D  (8 B)
 *   fieldChar  +40  C  (2 B)
 *   padding    +42
 *   fieldRef   +44  Ljava/lang/String;  (4 B compressed OOP)
 *
 *   Static fields live in the java.lang.Class mirror — offset values
 *   are distinct from instance offsets.
 */
public class TestTarget
{
    // ---- static fields -------------------------------------------------------
    public static int    staticInt    = 42;
    public static long   staticLong   = 0xDEADBEEFCAFEL;
    public static float  staticFloat  = 3.14f;
    public static double staticDouble = 2.718281828;
    public static String staticString = "hello";

    // ---- instance fields of every primitive type ----------------------------
    public boolean fieldBool   = true;
    public byte    fieldByte   = 127;
    public short   fieldShort  = 32767;
    public int     fieldInt    = 100;
    public float   fieldFloat  = 1.5f;
    public long    fieldLong   = 9876543210L;
    public double  fieldDouble = 6.283185307;
    public char    fieldChar   = 'X';
    public String  fieldRef    = "world";

    // ---- method to be hooked ------------------------------------------------
    /** Called every tick; VMHook installs a hook here and increments hookCallCount. */
    public void onTick(final int tick)
    {
        // Light computation so HotSpot doesn't inline-eliminate the call.
        this.fieldInt = tick % 10000;
    }

    // Counter incremented by the hook detour, NOT by Java — lets the test
    // verify the hook actually fired without modifying the Java side.
    public static volatile int hookCallCount = 0;
}
