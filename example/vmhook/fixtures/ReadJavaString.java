package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture making vmhook's free helper {@code read_java_string(oop)} the SUBJECT
 * under test (migrates the legacy example.cpp test_read_java_string).
 *
 * The native module (tests/jvm/modules/read_java_string.cpp) obtains each static
 * String field's backing OOP exactly the way the library does internally --
 * {@code static_field(name)->get_compressed_oop()} then
 * {@code vmhook::hotspot::decode_oop_pointer(...)} -- and feeds it straight into
 * {@code vmhook::read_java_string}, asserting the returned std::string is
 * BYTE-EXACT UTF-8 for every case.
 *
 * The static String fields below span every decode path of read_java_string and
 * span BOTH backing-storage coders on a modern (JDK 9+) compact-string VM:
 *
 *   LATIN1 (coder 0, every char {@code <= 0xFF} -- one byte per char):
 *     - ascii    "hello"      -> pure ASCII; UTF-8 == backing bytes verbatim.
 *     - cafe     "cafe"  -> U+00E9; LATIN1 backing byte 0xE9 but the UTF-8
 *                                OUTPUT is MULTIBYTE (C3 A9), proving the LATIN1
 *                                path UTF-8-encodes high bytes (not raw-copies).
 *     - latin1Hi "y"     -> the LATIN1 ceiling char; UTF-8 C3 BF.
 *
 *   UTF16 (coder 1, at least one char {@code > 0xFF} -- two bytes per char):
 *     - nihongo  "U+65E5U+672CU+8A9E" -> three BMP CJK code points.
 *     - mixed    "AU+65E5B"           -> ASCII + a >0xFF char promotes the WHOLE
 *                                        string to UTF-16, so even the ASCII
 *                                        chars go through the UTF-16 decode path.
 *     - emoji    "U+1F600"       -> a SURROGATE PAIR -> one astral code
 *                                        point U+1F600 -> 4-byte UTF-8
 *                                        (F0 9F 98 80), exercising
 *                                        read_java_string's surrogate-combining
 *                                        branch.
 *
 *   Guard paths:
 *     - empty    ""    -> backing array length 0 -> the length<=0 guard -> "".
 *     - nullRef  null  -> field compressed-OOP is 0 -> the null-oop guard -> "".
 *
 * On Java 8 String.value is a char[] (always UTF-16, no {@code coder} field); on
 * Java 9+ it is a byte[] + a {@code coder} field (LATIN1/UTF16).  read_java_string
 * must yield the IDENTICAL UTF-8 bytes on both -- the native module asserts that
 * invariant by comparing against fixed expected byte sequences regardless of JDK.
 *
 * Every field is built with {@code new String(char[])} so it owns a PRIVATE,
 * non-interned backing array; read_java_string is a PURE READER (it never
 * mutates), but private backing keeps this fixture self-contained and mirrors
 * the sibling string fixtures.  ALL non-ASCII chars are {@code \\uXXXX} escapes
 * (lexer-level), so javac agrees on Cp1252 (Windows) and UTF-8 alike.
 *
 * Java 8 syntax only: anonymous Harness.Probe, no var / records / lambdas.
 */
public final class ReadJavaString
{
    // ----- go/done handshake the native run_probe drives -------------------
    public static volatile boolean go;
    public static volatile boolean done;

    // ================= SUBJECT fields (static String) ======================
    // LATIN1 (coder 0) targets ------------------------------------------------
    public static String ascii    = fresh(new char[] { 'h', 'e', 'l', 'l', 'o' });
    // "cafe" with an acute e (U+00E9).  All chars <= 0xFF -> LATIN1 coder 0,
    // backing byte 0xE9, but UTF-8 output is the two bytes C3 A9.
    public static String cafe     = fresh(new char[] { 'c', 'a', 'f', '\u00E9' });
    // The LATIN1 ceiling: U+00FF -> still coder 0, UTF-8 C3 BF.
    public static String latin1Hi = fresh(new char[] { '\u00FF' });

    // UTF16 (coder 1) targets -------------------------------------------------
    // U+65E5 U+672C U+8A9E -> three BMP code points all > 0xFF.
    public static String nihongo  = fresh(new char[] { '\u65E5', '\u672C', '\u8A9E' });
    // ASCII + a >0xFF char -> whole string promoted to UTF-16 coder 1.
    public static String mixed    = fresh(new char[] { 'A', '\u65E5', 'B' });
    // U+1F600 as a UTF-16 surrogate pair -> one astral code point.
    public static String emoji    = fresh(new char[] { '\uD83D', '\uDE00' });

    // Guard targets -----------------------------------------------------------
    public static String empty    = fresh(new char[0]);   // length 0
    public static String nullRef  = null;                  // null reference

    // ================= Java-published cross-check facts ====================
    // The native side reads these back to PROVE its byte-exact decodes match
    // what Java itself computes for the same fields (length, code points, coder).
    public static volatile int     jAsciiLen;       // 5
    public static volatile int     jCafeLen;        // 4
    public static volatile int     jCafeCp3;        // 0x00E9
    public static volatile int     jLatin1HiCp0;    // 0x00FF
    public static volatile int     jNihongoLen;     // 3
    public static volatile int     jNihongoCp0;     // 0x65E5
    public static volatile int     jNihongoCp1;     // 0x672C
    public static volatile int     jNihongoCp2;     // 0x8A9E
    public static volatile int     jMixedLen;       // 3 (chars: A, U+65E5, B)
    public static volatile int     jEmojiCpCount;   // 1 (one code point)
    public static volatile int     jEmojiCp0;       // 0x1F600
    public static volatile int     jEmptyLen;       // 0
    public static volatile boolean jNullIsNull;     // true

    // String coder byte for each subject (JDK 9+); -1 on JDK 8 (no field).
    // Lets the native side label which physical coder each case exercised.
    public static volatile int     jCoderAscii;     // 0 (LATIN1) / -1 (JDK8)
    public static volatile int     jCoderCafe;      // 0 (LATIN1) / -1
    public static volatile int     jCoderNihongo;   // 1 (UTF16)  / -1
    public static volatile int     jCoderEmoji;     // 1 (UTF16)  / -1
    public static volatile boolean jHasCoderField;  // true on JDK 9+, false on 8

    // ---------------------- helpers ----------------------------------------
    /** new String(char[]) -> a PRIVATE backing array (never an interned alias). */
    private static String fresh(final char[] chars)
    {
        return new String(chars);
    }

    /** Reflective coder() probe; returns -1 when the field is absent (JDK 8). */
    private static int coderOf(final String s)
    {
        try
        {
            final java.lang.reflect.Field cf = String.class.getDeclaredField("coder");
            cf.setAccessible(true);
            return cf.getByte(s);
        }
        catch (final Throwable t)
        {
            return -1;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReadJavaString.go && !ReadJavaString.done;
            }

            @Override
            public void run()
            {
                jAsciiLen     = ascii.length();
                jCafeLen      = cafe.length();
                jCafeCp3      = cafe.codePointAt(3);
                jLatin1HiCp0  = latin1Hi.codePointAt(0);
                jNihongoLen   = nihongo.length();
                jNihongoCp0   = nihongo.codePointAt(0);
                jNihongoCp1   = nihongo.codePointAt(1);
                jNihongoCp2   = nihongo.codePointAt(2);
                jMixedLen     = mixed.length();
                jEmojiCpCount = emoji.codePointCount(0, emoji.length());
                jEmojiCp0     = emoji.codePointAt(0);
                jEmptyLen     = empty.length();
                jNullIsNull   = (nullRef == null);

                jHasCoderField = false;
                try
                {
                    String.class.getDeclaredField("coder");
                    jHasCoderField = true;
                }
                catch (final Throwable t)
                {
                    jHasCoderField = false;
                }
                jCoderAscii   = coderOf(ascii);
                jCoderCafe    = coderOf(cafe);
                jCoderNihongo = coderOf(nihongo);
                jCoderEmoji   = coderOf(emoji);

                ReadJavaString.done = true;
            }
        });
    }
}
