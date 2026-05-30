package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the method_call_object feature (area: methods).
 *
 * Exercises the ONE thing the method-vs-field parity path promises:
 *
 *     std::unique_ptr&lt;wrapper&gt; obj = proxy-&gt;get_method("foo")-&gt;call(args...);
 *
 * i.e. method_proxy::call() that returns a Java reference type ('L' / '[') and
 * whose value_t implicitly converts to a std::unique_ptr&lt;wrapper&gt;.  The native
 * module asserts the full contract:
 *
 *   - a NON-NULL object return yields a USABLE wrapper: the native side reads a
 *     field through it (Child.tag / Child.label) AND calls a method through it
 *     (Child.getTag()), so it proves the decoded OOP is a real, walkable heap
 *     object — not a truncated handle or a garbage pointer,
 *   - a NULL object return yields a NULL unique_ptr (monostate -&gt; nullptr), on
 *     the SAME method that can also return non-null (maybeChild(false)) and on a
 *     method that is unconditionally null (nullChild()),
 *   - method-vs-field PARITY: the SAME Child reachable via the `child` field
 *     (field_proxy -&gt; unique_ptr) and via getChild() (method_proxy -&gt; unique_ptr)
 *     decode to the SAME heap object — identityHashCode is published so the
 *     native side can cross-check the field read against the method return,
 *   - SELF identity: self() returns `this`, so the returned wrapper's instance
 *     must equal the receiver's instance,
 *   - STATIC object returns: staticMakeChild() / staticNullChild() drive the
 *     static-call path of method_proxy::call() returning an object,
 *   - ARRAY reference return: childArray() returns Child[] ('[' descriptor),
 *     the other reference-return branch of the value_t,
 *   - a String-returning method (childLabel()) — the eager-decode reference
 *     return that lands in the std::string variant alternative, NOT the
 *     uint32 OOP alternative; included so the module proves the value_t routes
 *     String vs Object to different alternatives.
 *
 * Every object the native side inspects is published with a deterministic field
 * value AND its System.identityHashCode so the C++ checks are exact, never
 * "non-null and hope".  The fixture follows the canonical go/done handshake; a
 * `mode` selector lets one probe cycle drive exactly the call the module is
 * about to assert on (because `done` latches, every observed call must happen
 * inside a single run()).
 */
public final class MethodObject
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  The native module sets this
     * BEFORE raising `go` so a single probe cycle drives exactly the calls the
     * module is about to assert on.
     *   0  = trigger only: call tick() once (the native hook lives on tick();
     *        the detour does all the object-returning calls on `self`).
     */
    public static volatile int mode;

    // ── The child object the wrappers walk ─────────────────────────────────
    /**
     * Small standalone reference type the native side wraps.  Has a primitive
     * field (tag), a String field (label), and a method (getTag) so the native
     * side can prove a method-returned wrapper is fully usable (field read +
     * method call through it).
     */
    public static final class Child
    {
        public int tag;
        public String label;

        public Child(final int tag, final String label)
        {
            this.tag = tag;
            this.label = label;
        }

        public int getTag()
        {
            return this.tag;
        }

        public String getLabel()
        {
            return this.label;
        }
    }

    // ── Deterministic constants the native side mirrors ────────────────────
    /** tag of the Child returned by makeChild() / held by getChild(). */
    public static final int CHILD_TAG = 0x5EED;          // 24301

    /** label of that Child. */
    public static final String CHILD_LABEL = "child-of-method";

    /** tag of the Child returned by maybeChild(true). */
    public static final int MAYBE_TAG = 0x1234;

    /** tag of the Child returned by staticMakeChild(). */
    public static final int STATIC_TAG = 0x7AC0;          // 31424

    /** label of the static Child. */
    public static final String STATIC_LABEL = "static-child";

    /** tags of the three Child elements in childArray(), in order. */
    public static final int ARRAY_TAG_0 = 100;
    public static final int ARRAY_TAG_1 = 200;
    public static final int ARRAY_TAG_2 = 300;

    /** Length of childArray(). */
    public static final int ARRAY_LEN = 3;

    /** Return value of childLabel() — a String, the eager-decode reference. */
    public static final String LABEL_STRING = "label-via-method";

    // ── Stored instance state ──────────────────────────────────────────────
    /**
     * The Child instance getChild() returns and the `child` field exposes.
     * Published identity (below) lets the native side prove the method return
     * and the field read decode to the SAME heap object.
     */
    public Child child = new Child(CHILD_TAG, CHILD_LABEL);

    // ── Identity publication (so native checks are exact) ──────────────────
    /** identityHashCode of `this` (the receiver), for the self() parity check. */
    public static volatile int selfIdentity;

    /** identityHashCode of `child`, for the method-vs-field parity check. */
    public static volatile int childIdentity;

    /** identityHashCode of the Child staticMakeChild() returns. */
    public static volatile int staticChildIdentity;

    /** The singleton static Child, so its identity is stable across calls. */
    private static final Child STATIC_CHILD = new Child(STATIC_TAG, STATIC_LABEL);

    /** The fixed Child[] childArray() returns, so element identities are stable. */
    private final Child[] childArray =
    {
        new Child(ARRAY_TAG_0, "a0"),
        new Child(ARRAY_TAG_1, "a1"),
        new Child(ARRAY_TAG_2, "a2"),
    };

    // ── Object-returning probe targets ─────────────────────────────────────

    /** Hook site.  The native module hooks this; the detour drives every call
     *  below on `self` so they run through method_proxy::call() on a live OOP. */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    /** Returns a freshly-allocated non-null Child with a known tag/label. */
    public Child makeChild()
    {
        return new Child(CHILD_TAG, CHILD_LABEL);
    }

    /** Returns the stored `child` field (same object the field read sees). */
    public Child getChild()
    {
        return this.child;
    }

    /** Returns `this` — identity / self parity probe. */
    public MethodObject self()
    {
        return this;
    }

    /** Returns a non-null Child when present==true, else null (same method, both paths). */
    public Child maybeChild(final boolean present)
    {
        return present ? new Child(MAYBE_TAG, "maybe") : null;
    }

    /** Always returns null (the unconditional null-reference return). */
    public Child nullChild()
    {
        return null;
    }

    /** Static object-returning call: a stable non-null Child. */
    public static Child staticMakeChild()
    {
        return STATIC_CHILD;
    }

    /** Static null-returning object call. */
    public static Child staticNullChild()
    {
        return null;
    }

    /** Array reference return ('[' descriptor) — the other reference branch. */
    public Child[] childArray()
    {
        return this.childArray;
    }

    /** String reference return — lands in the std::string variant alternative. */
    public String childLabel()
    {
        return LABEL_STRING;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MethodObject.go && !MethodObject.done;
            }

            @Override
            public void run()
            {
                // Publish identities the native side cross-checks.  Use the
                // SAME instance the native module wrapped: the module's hook
                // fires on tick() with `self` == this instance, so identity of
                // `this` and `this.child` here are exactly what native decodes.
                final MethodObject self = SINGLETON;
                MethodObject.selfIdentity = System.identityHashCode(self);
                MethodObject.childIdentity = System.identityHashCode(self.child);
                MethodObject.staticChildIdentity = System.identityHashCode(STATIC_CHILD);

                // Calling tick() through normal bytecode dispatch is what makes
                // the native interpreter hook fire; the detour then performs the
                // object-returning calls on `self` via method_proxy::call().
                self.tick(7);

                MethodObject.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the native side can fetch it via a static field (the standard "get an
     * instance to reach non-static methods" pattern) and so the identities
     * published above match the OOP the detour sees as `self`.
     */
    public static final MethodObject SINGLETON = new MethodObject();
}
