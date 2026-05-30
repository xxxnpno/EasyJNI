package vmhook.fixtures;

import vmhook.Harness;

/**
 * Exhaustive fixture for the {@code field_string} feature (String field get AND
 * set through vmhook's zero-JNI field_proxy / read_java_string / write_java_string).
 *
 * The native module (tests/jvm/modules/field_string.cpp) drives this fixture in
 * two phases around a single go/done handshake:
 *
 *   1. BEFORE raising go, native performs every {@code field_proxy::set(...)}
 *      write it wants to test.  set() mutates the backing byte[]/char[] of an
 *      existing String directly in heap memory, so no Java bytecode needs to run
 *      for the write itself.
 *   2. When go is raised, this fixture's probe runs on the Java thread.  It:
 *        - calls a hooked instance method (touchString) so an interpreter hook
 *          fires on a real bytecode dispatch (mirrors the pilot contract), and
 *        - reads every just-mutated field *through Java* and publishes the
 *          observations into the volatile result fields below, so the native
 *          side can prove the writes are visible to Java itself (not just to
 *          vmhook's own read path).
 *
 * GET fields are deliberately constructed to span every decode path of
 * read_java_string on a modern (JDK 9+) compact-string VM:
 *   - pure ASCII             -> LATIN1 (coder 0), byte-verbatim
 *   - Latin-1 (cp <= 0xFF)   -> LATIN1 (coder 0), raw high bytes preserved
 *   - supplementary / CJK    -> UTF-16 (coder 1), the lossy '?'-substitution path
 *   - empty / null           -> the length<=0 and null-oop guard paths
 *   - >4096 chars            -> the length>4096 rejection path
 *
 * Every SET-target String is allocated with {@code new String(...)} (or built
 * char-by-char) so it owns a PRIVATE, non-interned backing array — mutating it
 * in place cannot corrupt a shared string-pool literal elsewhere in the JVM.
 *
 * Target Java 8 syntax: no var / records / switch-expressions.
 */
public final class FieldString
{
    // ----- go/done handshake the native run_probe drives -------------------
    public static volatile boolean go;
    public static volatile boolean done;

    // ================= GET targets (static) ================================
    // ASCII only -> stored LATIN1 (coder 0) on JDK 9+.
    public static String getAscii      = "hello world";
    // Single char, ASCII.
    public static String getOneChar    = "Z";
    // All code points <= 0xFF (Latin-1) -> still LATIN1 coder 0, raw bytes >=0x80.
    public static String getLatin1      = makeLatin1();      // "héllo èéÿ"
    // Contains code points > 0xFF -> forced to UTF-16 coder 1 -> '?'-substitution.
    public static String getCjk         = "日本語";          // 日本語
    // Mixed ASCII + a >0xFF char -> whole string promoted to UTF-16 coder 1.
    public static String getMixed       = "A日BéC";             // A 日 B é C
    // Empty string -> length 0 -> read_java_string length<=0 guard.
    public static String getEmpty       = "";
    // Explicit null reference -> field compressed-OOP is 0 -> null-oop guard.
    public static String getNull        = null;
    // Interned literal (identity-shared); reading must not mutate it.
    public static String getInterned    = "INTERNED_LITERAL";
    // Exactly 4096 ASCII chars -> LATIN1 byte length 4096 -> the inclusive cap.
    public static String getLen4096     = repeat('x', 4096);
    // 4097 ASCII chars -> LATIN1 byte length 4097 -> rejected (returns "").
    public static String getLen4097     = repeat('y', 4097);
    // 5000 ASCII chars -> well past the cap -> rejected (returns "").
    public static String getLen5000     = repeat('z', 5000);
    // 2048 CJK chars -> UTF-16 byte length 4096 -> passes cap, all '?'.
    public static String getCjk2048     = repeat('日', 2048);
    // 2049 CJK chars -> UTF-16 byte length 4098 -> rejected (returns "").
    public static String getCjk2049     = repeat('日', 2049);
    // Embedded NUL: ASCII bytes with a 0x00 in the middle (LATIN1, length 5).
    public static String getEmbeddedNul = makeEmbeddedNul();             // a\0b\0c

    // Java-published facts about the GET targets (native cross-checks these).
    public static volatile int     jAsciiLen;
    public static volatile int     jLatin1Len;
    public static volatile int     jLatin1Cp1;          // codePointAt(1) == 0xE9
    public static volatile int     jCjkLen;
    public static volatile int     jCjkCp0;             // codePointAt(0) == 0x65E5
    public static volatile boolean jNullIsNull;
    public static volatile int     jLen4096Len;
    public static volatile int     jLen4097Len;

    // ================= SET targets (static) ================================
    // CRITICAL: every SET target is built with new String(char[]) so it owns a
    // PRIVATE backing array.  (new String(String) shares the backing byte[] with
    // the interned literal, so an in-place write_java_string would corrupt every
    // copy of that literal across the whole JVM — verified empirically on JDK 21.)
    // For an in-place write to land cleanly the replacement length must equal the
    // existing backing length; these are sized to match their test inputs.
    public static String setAsciiEq    = freshAscii("AAAAA");           // len 5, write "world"
    public static String setShorter    = freshAscii("world");           // len 5, write "hi"
    public static String setEmptyTgt   = freshAscii("");                // len 0, write -> no-op
    public static String setLatin1Tgt  = newLatin1Blank();              // LATIN1 len 5, write "abcde"
    public static String setOverlong   = freshAscii("abc");             // len 3, write "LONGER"

    // Java-published facts AFTER the native writes (read through Java).
    public static volatile String  setAsciiEqValue;
    public static volatile int     setAsciiEqLen;
    public static volatile boolean setAsciiEqMatches;   // equals("world")
    public static volatile String  setShorterValue;
    public static volatile int     setShorterLen;
    public static volatile String  setLatin1TgtValue;
    public static volatile boolean setLatin1Matches;    // equals("abcde")
    public static volatile String  setOverlongValue;
    public static volatile int     setOverlongLen;      // must stay 3 (no resize)

    // ================= SET target (instance) ===============================
    // Instance String field, mutated through an instance field_proxy.
    // Fresh char[] backing (see SET-target note above) so the write is isolated.
    public String  instAscii = freshAscii("QQQQQ");                      // len 5, write "java!"
    public static volatile String  instAsciiValue;
    public static volatile boolean instAsciiMatches;    // equals("java!")

    // A live instance the native side wraps (to reach the instance field).
    public static volatile FieldString self;

    // ================= negative / guard targets ============================
    // A primitive int field.  Native attempts field_proxy::set(std::string) on
    // it; the type guard must REFUSE the write and leave this unchanged.
    public static int notAStringInt = 12345;
    // A String the native side reads but never writes; used to prove that
    // reading an interned literal does not corrupt the shared pool.
    public static volatile boolean internedStillIntact;

    // Hookable instance method (mirrors pilot.touch) so a real interpreter hook
    // fires on bytecode dispatch.  Returns the live length of instAscii.
    public int touchString(final int delta)
    {
        return this.instAscii.length() + delta;
    }
    public static volatile int observed;

    // ---------------------- helper constructors ----------------------------
    private static String makeLatin1()
    {
        final char[] c = { 'h', (char) 0x00E9, 'l', 'l', 'o', ' ',
                           (char) 0x00E8, (char) 0x00E9, (char) 0x00FF };
        return new String(c);
    }

    private static String makeEmbeddedNul()
    {
        final char[] c = { 'a', (char) 0x0000, 'b', (char) 0x0000, 'c' };
        return new String(c);
    }

    private static String newLatin1Blank()
    {
        // Five Latin-1 'AA' so coder is 0 and backing length is 5 bytes.
        final char[] c = { 'A', 'A', 'A', 'A', 'A' };
        return new String(c);
    }

    /**
     * Builds a String from the chars of {@code text} via new String(char[]),
     * guaranteeing a PRIVATE backing array (never shared with an interned
     * literal).  Essential for SET targets that are mutated in place.
     */
    private static String freshAscii(final String text)
    {
        return new String(text.toCharArray());
    }

    private static String repeat(final char ch, final int count)
    {
        final char[] c = new char[count];
        for (int i = 0; i < count; i++)
        {
            c[i] = ch;
        }
        return new String(c);
    }

    static
    {
        self = new FieldString();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldString.go && !FieldString.done;
            }

            @Override
            public void run()
            {
                // Fire the interpreter hook through a real bytecode dispatch.
                FieldString.observed = FieldString.self.touchString(100);

                // --- publish Java-observed facts about the GET targets ------
                jAsciiLen  = getAscii.length();
                jLatin1Len = getLatin1.length();
                jLatin1Cp1 = getLatin1.codePointAt(1);
                jCjkLen    = getCjk.length();
                jCjkCp0    = getCjk.codePointAt(0);
                jNullIsNull = (getNull == null);
                jLen4096Len = getLen4096.length();
                jLen4097Len = getLen4097.length();

                // --- read the post-write SET targets THROUGH JAVA -----------
                setAsciiEqValue   = setAsciiEq;
                setAsciiEqLen     = setAsciiEq.length();
                setAsciiEqMatches = "world".equals(setAsciiEq);

                setShorterValue = setShorter;
                setShorterLen   = setShorter.length();

                setLatin1TgtValue = setLatin1Tgt;
                setLatin1Matches  = "abcde".equals(setLatin1Tgt);

                setOverlongValue = setOverlong;
                setOverlongLen   = setOverlong.length();

                instAsciiValue   = self.instAscii;
                instAsciiMatches = "java!".equals(self.instAscii);

                // The shared literal must be untouched by native reads.
                internedStillIntact = "INTERNED_LITERAL".equals(getInterned)
                        && ("INTERNED_LITERAL" == "INTERNED_LITERAL".intern());

                FieldString.done = true;
            }
        });
    }
}
