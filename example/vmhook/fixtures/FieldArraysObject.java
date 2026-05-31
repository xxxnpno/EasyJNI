package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_arrays_object feature (area: fields).
 *
 * Exercises READING Java reference arrays out of object / static fields:
 *
 *   - String[]                       (signature "[Ljava/lang/String;")
 *   - Object[] of a registered wrapper type Item
 *                                    (signature "[Lvmhook/fixtures/FieldArraysObject$Item;")
 *
 * through the two C++ read paths the native module drives:
 *
 *   (a) the field_proxy implicit-conversion operator into std::vector<std::string>
 *       for String[] (lands in read_array_value -> append_array_value(vector<string>)),
 *   (b) field_proxy::value_t::to_vector<Item>() for the Object[] wrapper path
 *       (the documented entry point, which currently mis-routes through
 *       collection::to_vector — see the native module's [INFO] flaw notes),
 *   (c) a manual decode_array_oop + per-element get_array_element walk that proves
 *       the Object[] DATA is reachable and that inner nulls are distinguishable,
 *       independent of (b).
 *
 * Coverage shapes, for BOTH String[] and Item[]:
 *   - canonical multi-element (all non-null),
 *   - EMPTY array (length 0),
 *   - SINGLE element,
 *   - ALL-null elements (every slot null),
 *   - MIXED null / non-null (interleaved),
 *   - leading-null and trailing-null edge layouts,
 *   - a genuinely null field (the array reference itself is null),
 *   - instance (non-static) variants of the canonical + mixed cases.
 *
 * Every element's value (String contents, or Item.tag + identityHashCode) and
 * null-ness is published so the native side can verify element COUNT and each
 * element's value / null-ness exactly.  Java 8 syntax only.
 */
public final class FieldArraysObject
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect proving a real bytecode dispatch fired. */
    public static volatile int observed;

    /**
     * Registered-wrapper element type for the Object[] arrays.  Mirrors the
     * MethodObject$Child shape: a readable int field plus a callable method, so
     * the native side can prove a decoded element is a real, usable object (read
     * tag through the wrapper AND call getTag() through it).
     */
    public static final class Item
    {
        public int tag;

        public Item(final int tag)
        {
            this.tag = tag;
        }

        public int getTag()
        {
            return this.tag;
        }
    }

    // ---- String[] fields --------------------------------------------------

    /** Canonical 3-element String[], all non-null. */
    public static volatile String[] staticStrings = { "alpha", "beta", "gamma" };

    /** Empty String[] (length 0). */
    public static volatile String[] emptyStrings = new String[0];

    /** Single-element String[]. */
    public static volatile String[] singleString = { "solo" };

    /** All-null String[] (3 slots, every one null). */
    public static volatile String[] allNullStrings = new String[3];

    /** Mixed null / non-null String[]: { "x", null, "z" }. */
    public static volatile String[] mixedStrings = { "x", null, "z" };

    /** Leading-null layout: { null, "b", "c" }. */
    public static volatile String[] leadingNullStrings = { null, "b", "c" };

    /** Trailing-null layout: { "a", "b", null }. */
    public static volatile String[] trailingNullStrings = { "a", "b", null };

    /** Larger mixed array to exercise the per-element loop at length 6. */
    public static volatile String[] bigMixedStrings =
        { "one", null, "three", null, "five", null };

    /** A null String[] reference (the array itself is null, not its elements). */
    public static volatile String[] nullStringArray = null;

    /** Instance String[] (non-static read path), all non-null. */
    public volatile String[] instStrings = { "inst0", "inst1" };

    /** Instance mixed String[]: { null, "mid", null }. */
    public volatile String[] instMixedStrings = { null, "mid", null };

    // ---- Item[] (Object[] registered-wrapper) fields ----------------------

    /** Canonical 3-element Item[], all non-null. */
    public static volatile Item[] staticItems =
        { new Item(10), new Item(20), new Item(30) };

    /** Empty Item[] (length 0). */
    public static volatile Item[] emptyItems = new Item[0];

    /** Single-element Item[]. */
    public static volatile Item[] singleItem = { new Item(99) };

    /** All-null Item[] (3 slots, every one null). */
    public static volatile Item[] allNullItems = new Item[3];

    /** Mixed null / non-null Item[]: { Item(1), null, Item(3) }. */
    public static volatile Item[] mixedItems = { new Item(1), null, new Item(3) };

    /** Leading-null Item[]: { null, Item(5), Item(6) }. */
    public static volatile Item[] leadingNullItems = { null, new Item(5), new Item(6) };

    /** Trailing-null Item[]: { Item(7), Item(8), null }. */
    public static volatile Item[] trailingNullItems = { new Item(7), new Item(8), null };

    /** A null Item[] reference (the array itself is null). */
    public static volatile Item[] nullItemArray = null;

    /** Instance Item[] (non-static), all non-null. */
    public volatile Item[] instItems = { new Item(41), new Item(42) };

    /** Instance mixed Item[]: { Item(51), null }. */
    public volatile Item[] instMixedItems = { new Item(51), null };

    // ---- Published values for native cross-checks -------------------------
    // Each Item carries a UNIQUE tag, so the native side proves it decoded the
    // right object by reading tag through the wrapper (field AND getTag method).
    // No identityHashCode is published because the zero-JNI native layer has no
    // primitive to recompute it; tag-uniqueness + re-read determinism are the
    // identity oracle instead.

    /** Self-reference so the native side can read the INSTANCE fields. */
    public static volatile FieldArraysObject self;

    /** Lengths re-published from Java so native count checks have a Java oracle. */
    public static volatile int staticStringsLen;
    public static volatile int mixedStringsLen;
    public static volatile int staticItemsLen;
    public static volatile int mixedItemsLen;

    /** Hookable instance method — the native module hooks this to prove the
        fixture is live and to obtain a `self` that can read instance fields. */
    public int touch(final int delta)
    {
        return this.instItems.length + delta;
    }

    private static void publishLengths()
    {
        staticStringsLen = staticStrings.length;
        mixedStringsLen  = mixedStrings.length;
        staticItemsLen   = staticItems.length;
        mixedItemsLen    = mixedItems.length;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldArraysObject.go && !FieldArraysObject.done;
            }

            @Override
            public void run()
            {
                final FieldArraysObject instance = new FieldArraysObject();
                FieldArraysObject.self = instance;
                publishLengths();
                // Drive a real bytecode dispatch so the interpreter hook fires.
                FieldArraysObject.observed = instance.touch(1000);
                FieldArraysObject.done = true;
            }
        });
    }
}
