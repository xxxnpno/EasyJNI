package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for {@code return_value::set_arg(index, value)} with OBJECT-WRAPPER
 * and NULL reference arguments (area: return_value / argument mutation).
 *
 * <p>The companion native module ({@code tests/jvm/modules/return_set_wrapper_null.cpp})
 * installs an interpreter hook on each method below.  Inside the hook it calls
 * {@code retval.set_arg(slot, <wrapper-or-null>)} to overwrite a <em>reference</em>
 * argument slot <em>before</em> the original body runs, exercising the object/null
 * branches of {@code set_arg}:</p>
 * <ul>
 *   <li>{@code is_unique_object_ptr} branch — inject a {@code unique_ptr<wrapper>}
 *       built from a live OOP (a published donor or a {@code make_unique}-allocated
 *       object); the encoded compressed OOP lands in the slot,</li>
 *   <li>the same branch with an <em>empty</em> {@code unique_ptr} — writes a null
 *       OOP (the body must see Java {@code null}),</li>
 *   <li>the {@code object_base}-by-value branch — inject a wrapper passed by value,</li>
 *   <li>cross-type injection — a wrapper of an UNRELATED Java class injected into a
 *       differently-typed slot (no klass check; characterized, not asserted-correct).</li>
 * </ul>
 *
 * <p>Every method is deliberately tiny: it copies the reference argument it
 * actually receives into observable {@code static} fields (its identity hash, a
 * field read through it, and a null flag).  Because the hook fires first and
 * mutates the interpreter local slot, what the body observes — and therefore what
 * it publishes — reflects the injection.  The native side then reads the fields
 * back and asserts the post-injection observation.</p>
 *
 * <p>Slot model (HotSpot x64 interpreter; every local is an 8-byte slot):</p>
 * <ul>
 *   <li>instance method: slot 0 = {@code this}, slot 1 = first arg, slot 2 =
 *       second arg;</li>
 *   <li>static method: slot 0 = first arg (the SLOT-0 object-injection case —
 *       you cannot overwrite {@code this} on an instance method without breaking
 *       the dispatch, so slot 0 is reached through a static method);</li>
 *   <li>{@code takeMixedObject(int, Donor)}: this=0, n=1, x=2 — an object arg that
 *       follows a primitive, proving the slot index (not the argument index) is
 *       what {@code set_arg} targets.</li>
 * </ul>
 *
 * <p>The whole suite runs in a single {@code go}/{@code done} handshake: the
 * probe's {@code run()} calls every method once (one real bytecode dispatch each,
 * the only thing that makes an interpreter hook fire), then raises {@code done}.
 * The native module installs all of its scoped hooks up front and fires the probe
 * once.</p>
 *
 * <p>Target Java 8 syntax only (no var / records / switch-expressions).</p>
 */
public final class ReturnSetWrapperNull
{
    /** Native raises this to request the probe action; lowers it afterwards. */
    public static volatile boolean go;

    /** The probe action raises this when it has run; native polls it. */
    public static volatile boolean done;

    // A singleton instance the probe uses for the instance-method dispatches.
    public static final ReturnSetWrapperNull instance = new ReturnSetWrapperNull();

    // ── The reference type the wrappers walk ──────────────────────────────────
    /**
     * Small standalone reference type the native side wraps and injects.  Has a
     * primitive field (tag) and a method (getTag) so the native side can prove a
     * wrapper-injected argument is the real, dispatch-capable object it expects
     * (field read + identity), not a truncated/garbage pointer.
     */
    public static final class Donor
    {
        public int tag;

        public Donor(final int tag)
        {
            this.tag = tag;
        }

        public int getTag()
        {
            return this.tag;
        }
    }

    /**
     * An UNRELATED reference type, used for the cross-type-injection
     * characterization: a Decoy wrapper injected into a Donor-typed slot is
     * silently accepted by set_arg (no klass-match check).  Distinct field layout
     * from Donor so a mis-typed read is observably different.
     */
    public static final class Decoy
    {
        public int poison;
        public long extra;

        public Decoy(final int poison)
        {
            this.poison = poison;
            this.extra = 0xABCDEF0123456789L;
        }
    }

    // ── Deterministic constants the native side mirrors ───────────────────────
    /** tag of the published DONOR injected through the unique_ptr branch. */
    public static final int DONOR_TAG = 0x0D04;          // 3332

    /** tag of the published DONOR injected through the by-value branch. */
    public static final int BYVAL_DONOR_TAG = 0x0BABE;   // 48318

    /** tag the native make_unique-allocated Donor is constructed with. */
    public static final int FRESH_DONOR_TAG = 0x7E57;    // 32343

    /** poison value of the published DECOY used for cross-type injection. */
    public static final int DECOY_POISON = 0x6A11;       // 27153

    /** content of the published donor String injected into takeString. */
    public static final String STRING_DONOR = "injected-object-string";

    // ── Published donor objects (stable identities the native side reads) ─────
    /** Donor the native side reads into a unique_ptr<wrapper> and injects. */
    public static final Donor DONOR = new Donor(DONOR_TAG);

    /** Donor injected through the object_base-by-value set_arg branch. */
    public static final Donor BYVAL_DONOR = new Donor(BYVAL_DONOR_TAG);

    /** Decoy injected (wrong type) into a Donor-typed slot. */
    public static final Decoy DECOY = new Decoy(DECOY_POISON);

    /** A String donor the native side injects into takeString as a real object. */
    public static final String STRING_DONOR_REF = STRING_DONOR;

    /** identityHashCode of DONOR, published so native checks identity exactly. */
    public static volatile int donorIdentity;
    /** identityHashCode of BYVAL_DONOR. */
    public static volatile int byvalDonorIdentity;
    /** identityHashCode of DECOY. */
    public static volatile int decoyIdentity;

    // ── takeObject(Donor): instance, slot 1.  unique_ptr injection. ───────────
    // The body publishes everything the native side cross-checks: whether the
    // arg was null, its identityHashCode, and its tag read through the object.
    public static volatile boolean objWasNull   = true;
    public static volatile int     objIdentity  = 0;
    public static volatile int     objTag       = -1;
    public void takeObject(final Donor value)
    {
        objWasNull  = (value == null);
        objIdentity = (value == null) ? 0 : System.identityHashCode(value);
        objTag      = (value == null) ? -1 : value.getTag();
    }

    // ── takeObjectStatic(Donor): static, slot 0.  unique_ptr injection. ───────
    public static volatile boolean sObjWasNull  = true;
    public static volatile int     sObjIdentity = 0;
    public static volatile int     sObjTag      = -1;
    public static void takeObjectStatic(final Donor value)
    {
        sObjWasNull  = (value == null);
        sObjIdentity = (value == null) ? 0 : System.identityHashCode(value);
        sObjTag      = (value == null) ? -1 : value.getTag();
    }

    // ── takeObjectNull(Donor): instance, slot 1.  empty-unique_ptr -> null. ───
    public static volatile boolean nullObjWasNull = false;   // body must flip true
    public static volatile int     nullObjTag     = -2;
    public void takeObjectNull(final Donor value)
    {
        nullObjWasNull = (value == null);
        nullObjTag     = (value == null) ? -1 : value.getTag();
    }

    // ── takeObjectNullStatic(Donor): static, slot 0.  null injection. ─────────
    public static volatile boolean sNullObjWasNull = false;
    public static void takeObjectNullStatic(final Donor value)
    {
        sNullObjWasNull = (value == null);
    }

    // ── takeByVal(Donor): instance, slot 1.  object_base-by-value branch. ─────
    public static volatile boolean byvalWasNull  = true;
    public static volatile int     byvalIdentity = 0;
    public static volatile int     byvalTag      = -1;
    public void takeByVal(final Donor value)
    {
        byvalWasNull  = (value == null);
        byvalIdentity = (value == null) ? 0 : System.identityHashCode(value);
        byvalTag      = (value == null) ? -1 : value.getTag();
    }

    // ── takeFresh(Donor): instance, slot 1.  make_unique-allocated injection. ─
    public static volatile boolean freshWasNull = true;
    public static volatile int     freshTag     = -1;
    public void takeFresh(final Donor value)
    {
        freshWasNull = (value == null);
        freshTag     = (value == null) ? -1 : value.getTag();
    }

    // ── takeTwoObjects(Donor a, Donor b): instance, slots 1 and 2. ────────────
    // Native injects the published DONOR at slot 2 (b) only; a must survive as
    // the original argument the probe passed.  Proves object injection targets
    // the requested slot among consecutive single-slot reference args.
    public static volatile int twoFirstTag  = -1;   // a's tag (untouched original)
    public static volatile int twoSecondTag = -1;   // b's tag (injected DONOR)
    public static volatile boolean twoFirstWasNull  = true;
    public static volatile boolean twoSecondWasNull = true;
    public void takeTwoObjects(final Donor a, final Donor b)
    {
        twoFirstWasNull  = (a == null);
        twoSecondWasNull = (b == null);
        twoFirstTag  = (a == null) ? -1 : a.getTag();
        twoSecondTag = (b == null) ? -1 : b.getTag();
    }

    // ── takeMixedObject(int n, Donor x): instance.  this=0, n=1, x=2. ─────────
    // An object arg that FOLLOWS a primitive int.  Native injects the DONOR at
    // slot 2 (x) and leaves n (slot 1) alone — proving the slot index, not the
    // argument ordinal, is what set_arg targets.
    public static volatile int     mixedN       = -1;   // n's value (untouched)
    public static volatile int     mixedObjTag  = -1;   // x's tag (injected DONOR)
    public static volatile boolean mixedObjWasNull = true;
    public void takeMixedObject(final int n, final Donor x)
    {
        mixedN          = n;
        mixedObjWasNull = (x == null);
        mixedObjTag     = (x == null) ? -1 : x.getTag();
    }

    // ── takeString(String): instance, slot 1.  real Java String object inject. ─
    // Two scenarios reuse this single method across re-fired probes (the native
    // side resets `done` between them): (1) inject a published String object,
    // (2) inject explicit null.
    public static volatile boolean strWasNull = true;
    public static volatile int     strLen     = -2;
    public static volatile String  strSeen    = "<unset>";
    public void takeString(final String value)
    {
        strWasNull = (value == null);
        strLen     = (value == null) ? -1 : value.length();
        strSeen    = value;
    }

    // ── takeWrongType(Donor): instance, slot 1.  cross-type characterization. ─
    // Native injects a DECOY (unrelated class) into this Donor-typed slot.
    // set_arg performs NO klass check, so the slot accepts the Decoy oop.  The
    // body reads it back AS a Donor; since Donor.tag and Decoy.poison share the
    // first instance-field offset, the .tag FIELD read returns the Decoy's poison.
    //
    // We read the .tag FIELD (a `getfield Donor.tag`, resolved to a fixed offset
    // at link time — a raw, offset-based byte read that cannot throw) and the
    // identityHashCode (a JVM intrinsic that works for ANY oop).  We deliberately
    // do NOT call value.getTag(): that `invokevirtual Donor.getTag` on a Decoy
    // oop would resolve through the Decoy's (Donor-incompatible) method table and
    // could throw / destabilise the JVM — exactly the kind of wreckage the HARD
    // RULE forbids.  Field read + identity is the safe, characterizable surface.
    public static volatile boolean wrongWasNull = true;
    public static volatile int     wrongTagRead = -1;
    public static volatile int     wrongIdentity = 0;
    public void takeWrongType(final Donor value)
    {
        wrongWasNull  = (value == null);
        wrongIdentity = (value == null) ? 0 : System.identityHashCode(value);
        // Read .tag (field, not method) against a Decoy oop: lands at the offset
        // Donor.tag occupies, which Decoy.poison shares as its first int field.
        wrongTagRead  = (value == null) ? -1 : value.tag;
    }

    // Tiny no-arg dispatch so even a fully-failed feature still flips probeTicks
    // (lets the native side tell "probe ran" apart from "observation failed").
    public static volatile int probeTicks = 0;
    public void tick() { probeTicks++; }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ReturnSetWrapperNull.go && !ReturnSetWrapperNull.done;
            }

            @Override
            public void run()
            {
                final ReturnSetWrapperNull self = ReturnSetWrapperNull.instance;

                // Publish donor identities the native side cross-checks.
                ReturnSetWrapperNull.donorIdentity      = System.identityHashCode(DONOR);
                ReturnSetWrapperNull.byvalDonorIdentity = System.identityHashCode(BYVAL_DONOR);
                ReturnSetWrapperNull.decoyIdentity      = System.identityHashCode(DECOY);

                // One real bytecode dispatch per method -> the matching hook
                // fires and injects the reference BEFORE the body publishes.
                // Sentinel "original" args are chosen so a no-op hook leaves a
                // recognisably different observation than a successful injection.
                self.tick();

                self.takeObject(null);                 // hook -> DONOR (unique_ptr)
                ReturnSetWrapperNull.takeObjectStatic(null); // hook -> DONOR (slot 0)
                self.takeObjectNull(DONOR);            // hook -> null (empty uptr)
                ReturnSetWrapperNull.takeObjectNullStatic(DONOR); // hook -> null (slot 0)
                self.takeByVal(null);                  // hook -> BYVAL_DONOR (by value)
                self.takeFresh(null);                  // hook -> make_unique Donor

                // Two consecutive object args; native injects DONOR into b (slot 2).
                self.takeTwoObjects(new Donor(11), null);
                // Object arg after a primitive; native injects DONOR into x (slot 2).
                self.takeMixedObject(4242, null);

                // Cross-type: native injects a DECOY into this Donor slot.
                self.takeWrongType(null);

                // takeString is driven by the native side across SEPARATE re-fired
                // probes (string-object inject, then null inject); the single call
                // here covers whichever scenario the native module armed this run.
                self.takeString("before");

                ReturnSetWrapperNull.done = true;
            }
        });
    }
}
