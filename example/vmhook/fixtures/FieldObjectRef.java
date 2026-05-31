package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the field_object_ref feature (area: fields).
 *
 * Exercises the ONE promise of the object-reference field path:
 *
 *     std::unique_ptr&lt;wrapper&gt; obj = holder-&gt;get_field("ref")-&gt;get();
 *
 * i.e. field_proxy::get() on a field whose JVM descriptor starts with 'L'
 * yields a compressed OOP that value_t::cast_for_variant decodes into a
 * std::unique_ptr&lt;wrapper&gt; (vmhook.hpp ~11433-11449).  Unlike the method-return
 * twin (method_proxy::call, which truncates/free's a JNI handle on JDK 21+),
 * the FIELD path reads a real compressed OOP straight from the object slot, so
 * the "non-null ref -&gt; usable wrapper" contract holds on EVERY JDK.  That makes
 * this fixture the canonical, JDK-independent proof of the decode pipeline.
 *
 * The native module asserts, on a live JVM:
 *
 *   - NON-NULL instance ref field   -&gt; usable wrapper (read its int / String /
 *     nested-ref fields, AND call a method through it),
 *   - NON-NULL static ref field     -&gt; usable wrapper (the mirror+offset path),
 *   - NULL ref field                -&gt; null unique_ptr (the most important
 *     invariant: a null slot must never fabricate a wrapper),
 *   - FINAL object field            -&gt; decodes identically to a non-final one
 *     (final-ness is compile-time only; the slot is an ordinary oop),
 *   - VOLATILE object field         -&gt; decodes correctly (no fences needed for a
 *     plain read of the slot),
 *   - SELF ref (a field that holds `this`) -&gt; decoded instance == the receiver,
 *   - IDENTITY: the decoded OOP, re-encoded, round-trips; and the field read and
 *     a direct decode_oop of the same slot agree (compressed-oop decode
 *     correctness),
 *   - SHARED ref: two different fields that hold the SAME Java object decode to
 *     the SAME heap address,
 *   - WRONG-WRAPPER-TYPE read: reading a Holder-typed field through a wrapper
 *     registered for an UNRELATED Java class (Decoy) is NOT rejected by the
 *     library (a documented flaw: no klass-match check) — the fixture lays out
 *     Decoy so its field offsets differ from Holder's, so the wrong-typed read
 *     returns a DIFFERENT value than the correct-typed read, proving the silent
 *     mis-decode,
 *   - ARRAY-vs-object: a field whose descriptor is '[' (Ref[]) decoded as a
 *     unique_ptr&lt;wrapper&gt; is ALSO not rejected (no signature-shape check) — the
 *     wrapper ends up pointing at the array oop; the fixture publishes the array
 *     identity so native can show the wrapper wrapped the array, not an element.
 *
 * Every object the native side inspects carries deterministic field values AND
 * its System.identityHashCode, so the C++ checks are exact (never "non-null and
 * hope").  Canonical go/done handshake; `mode` selector + done-reset let one
 * probe cycle drive exactly the dispatch the module asserts on.
 */
public final class FieldObjectRef
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this
     * BEFORE raising `go`.  Only one trivial scenario is needed because every
     * object-reference read is side-effect free and happens from native code;
     * the probe exists solely to fire the interpreter hook on tick() and to
     * (re)publish identities on the Java thread.
     *   0 = publish identities + call tick() once (drives the native hook).
     */
    public static volatile int mode;

    // ── The reference type the wrappers walk ───────────────────────────────
    /**
     * The object a Holder field points at.  Has a primitive int (val), a String
     * (label), a nested self-reference capability, and a method (compute) so the
     * native side can prove a field-decoded wrapper is fully usable: read a
     * primitive, read a String, AND dispatch a real virtual call through it.
     */
    public static final class Ref
    {
        public int val;
        public String label;
        public Ref next;        // nested object reference (a Ref-typed field on a Ref)

        public Ref(final int val, final String label)
        {
            this.val = val;
            this.label = label;
            this.next = null;
        }

        /** Method dispatched through a field-decoded wrapper (usability proof). */
        public int compute()
        {
            return this.val * 2 + 1;
        }
    }

    /**
     * An UNRELATED reference type used for the wrong-wrapper-type angle.  Its
     * field layout deliberately differs from Ref: the first declared int sits at
     * a name ("poison") that Ref does not have, and its value is distinctive.
     * When native reads a Ref-typed slot through a Decoy wrapper, the library
     * does NOT reject it (no klass-match check), and Decoy.get_field("poison")
     * reads at refOop + Decoy's poison-offset — i.e. garbage relative to Ref —
     * which the module shows differs from the correct Ref read.
     */
    public static final class Decoy
    {
        public long pad0;       // shift subsequent fields so offsets differ from Ref
        public long pad1;
        public int poison;      // a field name Ref does NOT declare
        public int poison2;

        public Decoy()
        {
            this.pad0 = 0x1111_1111_1111_1111L;
            this.pad1 = 0x2222_2222_2222_2222L;
            this.poison = 0xDEAD;
            this.poison2 = 0xBEEF;
        }
    }

    // ── Deterministic constants the native side mirrors ────────────────────
    public static final int    REF_VAL          = 0x0BADF00D >>> 8;   // 0x000BADF0 -> positive
    public static final String REF_LABEL        = "ref-of-field";
    public static final int    STATIC_REF_VAL   = 0x5151;
    public static final String STATIC_REF_LABEL = "static-ref";
    public static final int    NESTED_REF_VAL   = 0x2222;
    public static final String NESTED_REF_LABEL = "nested-ref";
    public static final int    FINAL_REF_VAL    = 0x3333;
    public static final int    VOLATILE_REF_VAL = 0x4444;
    public static final int    ARRAY_ELEM0_VAL  = 700;
    public static final int    ARRAY_ELEM1_VAL  = 800;
    public static final int    ARRAY_LEN        = 2;

    // ── Instance reference fields (read through an INSTANCE field_proxy) ────

    /** Non-null instance object reference: the primary usable-wrapper target. */
    public Ref ref = makeRef(REF_VAL, REF_LABEL);

    /** A second field pointing at the SAME object as `ref` (shared-ref angle). */
    public Ref refAlias = this.ref;

    /** Null object reference: must decode to a null unique_ptr. */
    public Ref nullRef = null;

    /** FINAL object reference: final is compile-time; the slot is a plain oop. */
    public final Ref finalRef = makeRef(FINAL_REF_VAL, "final-ref");

    /** VOLATILE object reference: a plain slot read must still decode correctly. */
    public volatile Ref volatileRef = makeRef(VOLATILE_REF_VAL, "volatile-ref");

    /** A field that holds `this` (self-reference identity angle). */
    public FieldObjectRef self;

    /** An object-ARRAY field ('[' descriptor) — the signature-shape angle. */
    public Ref[] refArray =
    {
        makeRef(ARRAY_ELEM0_VAL, "a0"),
        makeRef(ARRAY_ELEM1_VAL, "a1"),
    };

    /**
     * A plain (non-final, mutable) primitive int instance field with an ordinary
     * object slot.  Target for the get_compressed_oop()-on-a-primitive flaw: it
     * has a real 4-byte 'I' slot (no ConstantValue inlining), so reading its
     * compressed-oop returns exactly these 4 bytes.
     */
    public int primitiveInt = PRIMITIVE_INT_VALUE;

    /** The exact value of primitiveInt (native mirrors it). */
    public static final int PRIMITIVE_INT_VALUE = 0x04D2;   // 1234

    // ── Static reference fields (read through the mirror+offset path) ───────

    /** Non-null STATIC object reference. */
    public static Ref staticRef = makeRef(STATIC_REF_VAL, STATIC_REF_LABEL);

    /** Null STATIC object reference. */
    public static Ref staticNullRef = null;

    // ── Identity publication (so native checks are exact) ──────────────────
    public static volatile int refIdentity;
    public static volatile int refAliasIdentity;
    public static volatile int finalRefIdentity;
    public static volatile int volatileRefIdentity;
    public static volatile int selfIdentity;
    public static volatile int staticRefIdentity;
    public static volatile int nestedRefIdentity;
    public static volatile int refArrayIdentity;
    public static volatile int refArrayElem0Identity;

    /** Helper so the field initialisers and run() build Refs identically. */
    private static Ref makeRef(final int val, final String label)
    {
        return new Ref(val, label);
    }

    // ── Hook site ──────────────────────────────────────────────────────────
    /**
     * The native module hooks this; calling it through real bytecode dispatch is
     * what makes the interpreter hook fire.  All the object-reference reads in
     * the module happen from native code against the published SINGLETON, so the
     * detour body itself need do nothing but exist.
     */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return FieldObjectRef.go && !FieldObjectRef.done;
            }

            @Override
            public void run()
            {
                final FieldObjectRef s = SINGLETON;

                // Wire the nested + self references on the published instance.
                if (s.ref.next == null)
                {
                    s.ref.next = makeRef(NESTED_REF_VAL, NESTED_REF_LABEL);
                }
                s.self = s;

                // Publish identities the native side cross-checks against the
                // OOPs it decodes from each field slot.
                FieldObjectRef.refIdentity            = System.identityHashCode(s.ref);
                FieldObjectRef.refAliasIdentity       = System.identityHashCode(s.refAlias);
                FieldObjectRef.finalRefIdentity       = System.identityHashCode(s.finalRef);
                FieldObjectRef.volatileRefIdentity    = System.identityHashCode(s.volatileRef);
                FieldObjectRef.selfIdentity           = System.identityHashCode(s.self);
                FieldObjectRef.staticRefIdentity      = System.identityHashCode(FieldObjectRef.staticRef);
                FieldObjectRef.nestedRefIdentity      = System.identityHashCode(s.ref.next);
                FieldObjectRef.refArrayIdentity       = System.identityHashCode(s.refArray);
                FieldObjectRef.refArrayElem0Identity  = System.identityHashCode(s.refArray[0]);

                // Real bytecode dispatch -> native interpreter hook fires.
                s.tick(7);

                FieldObjectRef.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side can fetch it through a static field and so the
     * identities published above match exactly the OOPs the module decodes.
     */
    public static final FieldObjectRef SINGLETON = new FieldObjectRef();
}
