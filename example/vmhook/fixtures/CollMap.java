package vmhook.fixtures;

import vmhook.Harness;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

/**
 * Fixture for the collection_map feature (area: collections).
 *
 * Exercises field_proxy::value_t::to_entries&lt;K,V&gt;() / vmhook::map::to_entries
 * and the two underlying walkers, on a live JVM, across every container shape and
 * boundary the audit flagged:
 *
 *   - HashMap&lt;String,Box&gt;          empty / small / MANY (forces resize +
 *                                    a treeified bin) / one-null-key /
 *                                    one-null-value / empty-string key+value.
 *   - LinkedHashMap&lt;String,Box&gt;    small + MANY (the HashMap "table" fast path
 *                                    is taken, so iteration is BUCKET order, NOT
 *                                    insertion order — pinned as a known quirk).
 *   - TreeMap&lt;String,Box&gt;          empty / small / MANY (red-black in-order
 *                                    walk; in-order == sorted key order).
 *   - a Map field left NULL          and a field that does not exist at all
 *                                    (to_entries must return empty, never throw).
 *
 * The VALUE type is the nested {@code CollMap.Box} (compiled to
 * {@code vmhook/fixtures/CollMap$Box}); its {@code $} keeps the Harness fixture
 * loader from trying to Class.forName it as a probe.  The KEY type is plain
 * java.lang.String — the native key wrapper decodes the String OOP directly via
 * vmhook::read_java_string.
 *
 * Canonical go/done handshake with a `mode` selector (mirrors HookBasic): the
 * native module sets `mode`, clears `done`, raises `go`; the Harness loop runs
 * the matching scenario on the Java thread; the module polls `done`.  Because
 * the maps are plain object fields, the native side reads them directly without
 * needing a hooked dispatch — but a hookable touch() is provided so the module
 * still proves an interpreter hook fires through the modular path, exactly like
 * the pilot.
 */
public final class CollMap
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect of the hookable method (pilot-style proof). */
    public static volatile int observed;

    /**
     * Scenario selector.  The native module sets this BEFORE raising `go`.
     *   0  = build/refresh every map field (default; runs once at startup too)
     *   1  = call touch() so the interpreter hook fires on real dispatch
     */
    public static volatile int mode;

    /**
     * Immutable value object stored in every map.  Two fields so the native
     * value wrapper can verify BOTH a primitive (id) and a reference (name)
     * round-trip through the decoded value OOP.
     */
    public static final class Box
    {
        public final int id;
        public final String name;

        public Box(final int id, final String name)
        {
            this.id = id;
            this.name = name;
        }
    }

    // ---- The map fields the native module reads via to_entries -------------

    /** Empty HashMap (size 0). */
    public static HashMap<String, Box> hashEmpty = new HashMap<String, Box>();

    /** Small HashMap: exactly SMALL_N deterministic entries. */
    public static HashMap<String, Box> hashSmall = new HashMap<String, Box>();

    /** Large HashMap: MANY_N entries — forces several table resizes. */
    public static HashMap<String, Box> hashMany = new HashMap<String, Box>();

    /**
     * HashMap with a SINGLE null key (legal: HashMap permits one null key,
     * stored in bucket 0).  Holds NULL_KEY_N entries, one of which has a null key.
     */
    public static HashMap<String, Box> hashNullKey = new HashMap<String, Box>();

    /** HashMap where exactly one entry has a non-null key but a NULL value. */
    public static HashMap<String, Box> hashNullValue = new HashMap<String, Box>();

    /**
     * HashMap whose keys/values are the EMPTY string "" and a Box whose name is
     * "" — exercises the read_java_string length&lt;=0 → "" boundary on both ends.
     */
    public static HashMap<String, Box> hashEmptyStr = new HashMap<String, Box>();

    /**
     * HashMap engineered so at least one bucket TREEIFIES: &gt;=8 keys that all
     * hash to the same bucket at the current capacity.  Holds TREEIFY_N entries.
     */
    public static HashMap<String, Box> hashTreeified = new HashMap<String, Box>();

    /** Small LinkedHashMap (insertion-ordered in Java; bucket-ordered via vmhook). */
    public static LinkedHashMap<String, Box> linkedSmall = new LinkedHashMap<String, Box>();

    /** Large LinkedHashMap. */
    public static LinkedHashMap<String, Box> linkedMany = new LinkedHashMap<String, Box>();

    /** Empty TreeMap. */
    public static TreeMap<String, Box> treeEmpty = new TreeMap<String, Box>();

    /** Small TreeMap: SMALL_N entries; in-order walk == sorted key order. */
    public static TreeMap<String, Box> treeSmall = new TreeMap<String, Box>();

    /** Large TreeMap: MANY_N entries; deep red-black tree. */
    public static TreeMap<String, Box> treeMany = new TreeMap<String, Box>();

    /** A declared Map field that is deliberately left NULL. */
    public static Map<String, Box> nullMap = null;

    /** A plain (non-Map) reference field — to_entries on it must stay empty. */
    public static String notAMap = "not a map";

    // ---- Scenario sizes (mirrored on the native side) ----------------------

    public static final int SMALL_N = 3;
    public static final int MANY_N = 1000;
    public static final int NULL_KEY_N = 3;     // includes the one null-key entry
    public static final int TREEIFY_N = 12;     // > 8 => bucket treeifies

    // ---- Published checksums so native can verify content without ordering --
    //
    // For the unordered maps the native walker visits entries in bucket order,
    // which is not the Java insertion order, so the module verifies CONTENT via
    // order-independent aggregates that Java computes the same way.  Each key is
    // "k" + i and each value is new Box(i, "v" + i), so:
    //   keyCharSum   = sum over entries of (sum of UTF-16 code units of the key)
    //   idSum        = sum over entries of value.id
    //   idXor        = xor over entries of value.id
    // These are published per-map so the native side can cross-check exactly.

    public static volatile long hashSmallKeyCharSum;
    public static volatile long hashSmallIdSum;
    public static volatile long hashSmallIdXor;

    public static volatile long hashManyKeyCharSum;
    public static volatile long hashManyIdSum;
    public static volatile long hashManyIdXor;

    public static volatile long treeSmallIdSum;
    public static volatile long treeManyIdSum;

    /** Java's own view of each map's size(), for a native cross-check. */
    public static volatile int hashEmptySize;
    public static volatile int hashSmallSize;
    public static volatile int hashManySize;
    public static volatile int hashNullKeySize;
    public static volatile int hashNullValueSize;
    public static volatile int hashEmptyStrSize;
    public static volatile int hashTreeifiedSize;
    public static volatile int linkedSmallSize;
    public static volatile int linkedManySize;
    public static volatile int treeEmptySize;
    public static volatile int treeSmallSize;
    public static volatile int treeManySize;

    /** TreeMap's first/last keys (sorted), so native can pin the in-order walk. */
    public static volatile String treeSmallFirstKey;
    public static volatile String treeSmallLastKey;

    /** Whether the treeified HashMap actually treeified at least one bin. */
    public static volatile boolean treeifiedHasTreeBin;

    // ---- Hookable method (pilot-style interpreter-hook proof) --------------

    private int seed = 7000;

    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    // ---- Builders ----------------------------------------------------------

    private static long codeUnitSum(final String s)
    {
        long sum = 0;
        for (int i = 0; i < s.length(); ++i)
        {
            sum += s.charAt(i);
        }
        return sum;
    }

    private static void buildAll()
    {
        hashEmpty = new HashMap<String, Box>();

        hashSmall = new HashMap<String, Box>();
        long hsKey = 0, hsId = 0, hsXor = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            final String k = "k" + i;
            hashSmall.put(k, new Box(i, "v" + i));
            hsKey += codeUnitSum(k);
            hsId += i;
            hsXor ^= i;
        }
        hashSmallKeyCharSum = hsKey;
        hashSmallIdSum = hsId;
        hashSmallIdXor = hsXor;

        hashMany = new HashMap<String, Box>();
        long hmKey = 0, hmId = 0, hmXor = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            final String k = "k" + i;
            hashMany.put(k, new Box(i, "v" + i));
            hmKey += codeUnitSum(k);
            hmId += i;
            hmXor ^= i;
        }
        hashManyKeyCharSum = hmKey;
        hashManyIdSum = hmId;
        hashManyIdXor = hmXor;

        hashNullKey = new HashMap<String, Box>();
        hashNullKey.put(null, new Box(-1, "nullkey"));   // the legal single null key
        hashNullKey.put("a", new Box(1, "va"));
        hashNullKey.put("b", new Box(2, "vb"));

        hashNullValue = new HashMap<String, Box>();
        hashNullValue.put("present", null);              // null value, non-null key
        hashNullValue.put("alsohere", new Box(9, "v9"));

        hashEmptyStr = new HashMap<String, Box>();
        hashEmptyStr.put("", new Box(0, ""));            // empty key AND empty value name

        // Build a HashMap that treeifies a bin: Strings sharing a hashCode would
        // be ideal, but engineering String hash collisions is brittle.  Instead
        // we exploit that HashMap.hash spreads h ^ (h>>>16); at capacity 16 (the
        // default) keys whose spread hash lands in the same low-4-bits bucket
        // collide.  We just insert TREEIFY_N keys; with MANY in one map the odds
        // of a treeified bin are non-trivial, but to GUARANTEE coverage we force
        // every key into bucket 0 by using keys whose String.hashCode is a
        // multiple of a large power of two is also brittle — so we instead build
        // a dedicated map with a tiny initial capacity and a load factor that
        // keeps capacity small while we cram many colliding-by-construction keys.
        // Simplest robust approach: keys "AaAa..." patterns are classic Java
        // hashCode collisions ("Aa".hashCode()=="BB".hashCode()).  We generate a
        // family of length-matched colliding strings.
        hashTreeified = new HashMap<String, Box>();
        final String[] coll = collidingKeys(TREEIFY_N);
        for (int i = 0; i < coll.length; ++i)
        {
            hashTreeified.put(coll[i], new Box(1000 + i, "t" + i));
        }
        treeifiedHasTreeBin = hasTreeNodeBin(hashTreeified);

        linkedSmall = new LinkedHashMap<String, Box>();
        for (int i = 0; i < SMALL_N; ++i)
        {
            linkedSmall.put("k" + i, new Box(i, "v" + i));
        }

        linkedMany = new LinkedHashMap<String, Box>();
        for (int i = 0; i < MANY_N; ++i)
        {
            linkedMany.put("k" + i, new Box(i, "v" + i));
        }

        treeEmpty = new TreeMap<String, Box>();

        treeSmall = new TreeMap<String, Box>();
        long tsId = 0;
        for (int i = 0; i < SMALL_N; ++i)
        {
            treeSmall.put("k" + i, new Box(i, "v" + i));
            tsId += i;
        }
        treeSmallIdSum = tsId;
        treeSmallFirstKey = treeSmall.firstKey();
        treeSmallLastKey = treeSmall.lastKey();

        treeMany = new TreeMap<String, Box>();
        long tmId = 0;
        for (int i = 0; i < MANY_N; ++i)
        {
            treeMany.put("k" + i, new Box(i, "v" + i));
            tmId += i;
        }
        treeManyIdSum = tmId;

        nullMap = null;

        hashEmptySize = hashEmpty.size();
        hashSmallSize = hashSmall.size();
        hashManySize = hashMany.size();
        hashNullKeySize = hashNullKey.size();
        hashNullValueSize = hashNullValue.size();
        hashEmptyStrSize = hashEmptyStr.size();
        hashTreeifiedSize = hashTreeified.size();
        linkedSmallSize = linkedSmall.size();
        linkedManySize = linkedMany.size();
        treeEmptySize = treeEmpty.size();
        treeSmallSize = treeSmall.size();
        treeManySize = treeMany.size();
    }

    /**
     * Generates n strings that all share the SAME String.hashCode by chaining
     * the classic "Aa"/"BB" collision (both hash to 2112).  Concatenating any
     * sequence of these 2-char blocks preserves the equal-hashCode property as
     * long as every generated string has the same length, so all n keys collide
     * into one bucket and the bin treeifies once it exceeds 8 entries.
     */
    private static String[] collidingKeys(final int n)
    {
        // 2-char blocks that pairwise collide: "Aa" and "BB" both == 2112.
        final String[] blocks = { "Aa", "BB" };
        // Use enough blocks per key to make 2^blocks >= n distinct combinations.
        int blockCount = 1;
        while ((1 << blockCount) < n)
        {
            ++blockCount;
        }
        final String[] out = new String[n];
        for (int i = 0; i < n; ++i)
        {
            final StringBuilder sb = new StringBuilder(blockCount * 2);
            for (int b = 0; b < blockCount; ++b)
            {
                sb.append(blocks[(i >> b) & 1]);
            }
            out[i] = sb.toString();
        }
        return out;
    }

    /**
     * Reflectively inspects the HashMap.table to report whether any bucket head
     * is a TreeNode (i.e. the bin treeified).  Returns false if reflection is
     * blocked (e.g. a strict module system) — the native check tolerates that.
     */
    private static boolean hasTreeNodeBin(final HashMap<String, Box> map)
    {
        try
        {
            final java.lang.reflect.Field tableField = HashMap.class.getDeclaredField("table");
            tableField.setAccessible(true);
            final Object table = tableField.get(map);
            if (table == null)
            {
                return false;
            }
            final int len = java.lang.reflect.Array.getLength(table);
            for (int i = 0; i < len; ++i)
            {
                final Object head = java.lang.reflect.Array.get(table, i);
                if (head != null && head.getClass().getSimpleName().equals("TreeNode"))
                {
                    return true;
                }
            }
            return false;
        }
        catch (final Throwable t)
        {
            return false;
        }
    }

    static
    {
        // Build once at class-init so the maps are populated even before the
        // first probe (the native module also re-requests a build via mode 0).
        buildAll();

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return CollMap.go && !CollMap.done;
            }

            @Override
            public void run()
            {
                if (CollMap.mode == 1)
                {
                    final CollMap instance = new CollMap();
                    CollMap.observed = instance.touch(42);
                }
                else
                {
                    // mode 0: (re)build all maps so the native reads see a fresh,
                    // deterministic population on this exact thread.
                    buildAll();
                }
                CollMap.done = true;
            }
        });
    }
}
