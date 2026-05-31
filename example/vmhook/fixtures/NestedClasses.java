package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the nested_classes feature (area: classes / klass resolution).
 *
 * The live-JVM authority for vmhook's handling of Java NESTED classes — the two
 * shapes whose javac-generated internal names are STABLE across recompiles and
 * therefore resolvable by a fixed {@code $}-name through {@link
 * vmhook::find_class}:
 *
 *   - a STATIC nested class  {@code NestedClasses$Host$StaticNested}
 *       (no synthetic outer reference; an ordinary class that merely lives in
 *        another class's namespace),
 *   - a non-static INNER class {@code NestedClasses$Host$Inner}
 *       (javac injects a synthetic {@code this$0} back-reference to the
 *        enclosing {@code Host} instance, plus a synthetic ctor parameter that
 *        wires it).
 *
 * The enclosing {@code Host} is itself a static nested class of this fixture
 * ({@code NestedClasses$Host}) so it can be force-instantiated without needing a
 * {@code NestedClasses} instance.  Anonymous / local classes are deliberately
 * NOT exercised: their generated names ({@code NestedClasses$1}, ...) are
 * unstable across recompiles, so no fixed Java name can identify them (the lone
 * anonymous class here is this fixture's own {@link Harness.Probe}, which the
 * native side never resolves by name).
 *
 * Mirrors the legacy {@code test_nested_classes} (vmhook/src/example.cpp) and
 * {@code vmhook.NestedHost} value-for-value so the documented composite holds:
 *
 *     outerField (7)  +  Inner.innerValue (99)  ==  106
 *
 * which the native module asserts both by reading the synthetic {@code this$0}
 * slot of the Inner instance back to the Host (identity proof) and — the robust,
 * JDK-independent proof — by driving {@code Inner.outerPlusInner()} through real
 * bytecode here in mode 1 and publishing the result for the module to check.
 *
 * Every object the native side inspects is force-instantiated in {@code <clinit>}
 * (so its klass is actually loaded — Main.loadFixtures only Class.forName's the
 * top-level fixture, never the {@code $}-nested klasses) and carries its
 * System.identityHashCode so the C++ checks are exact, never "non-null and hope".
 *
 * Java 8 syntax only (anonymous Probe, no lambdas / var / records).
 */
public final class NestedClasses
{
    // ── go / done / mode handshake (native sets mode + clears done first) ────
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector.  The native module sets this BEFORE raising {@code go}.
     *   1 = drive Inner.outerPlusInner() + StaticNested.doubled() through REAL
     *       bytecode and publish the results (the JDK-independent composite
     *       proof) — also fires any interpreter hook on a genuine dispatch.
     */
    public static volatile int mode;

    // ── Deterministic constants the native side mirrors (== NestedHost) ──────
    public static final int OUTER_FIELD_INIT   = 7;     // Host.outerField
    public static final int STATIC_NESTED_VAL  = 42;    // StaticNested.value
    public static final int STATIC_NESTED_DBL  = 84;    // == value * 2
    public static final int INNER_VALUE_INIT   = 99;    // Inner.innerValue
    public static final int OUTER_PLUS_INNER   = 106;   // 7 + 99 (documented)

    // ── The outer holder.  STATIC nested so it needs no NestedClasses instance,
    //    yet still produces the 3-level internal name NestedClasses$Host and is
    //    the enclosing instance whose `this$0` an Inner points back at. ────────
    public static class Host
    {
        /** Read by the native side at Host depth-0; the Inner reads it too. */
        public int outerField = OUTER_FIELD_INIT;

        // ---- STATIC nested class: NO synthetic outer reference --------------
        public static class StaticNested
        {
            public int value;

            public StaticNested(final int v)
            {
                this.value = v;
            }

            /** No-arg instance method (interpreter call-gate target). */
            public int doubled()
            {
                return this.value * 2;
            }
        }

        // ---- non-static INNER class: javac injects a synthetic this$0 -------
        public class Inner
        {
            public int innerValue = INNER_VALUE_INIT;

            /**
             * Reads the enclosing Host.outerField THROUGH the synthetic
             * {@code this$0} reference, so a correct read proves the back-link.
             * Returns 7 + 99 == 106.
             */
            public int outerPlusInner()
            {
                return outerField + this.innerValue;
            }
        }

        /** Factory so an Inner is built against THIS Host (wires this$0). */
        public Inner newInner()
        {
            return new Inner();
        }
    }

    // ── Force-instantiated singletons (so the $-nested klasses are LOADED and
    //    so the published identities match exactly the OOPs the module decodes).
    /** The enclosing Host instance; native reads outerField off it. */
    public static final Host host = new Host();

    /** A StaticNested instance; native reads value + calls doubled(). */
    public static final Host.StaticNested staticNested = new Host.StaticNested(STATIC_NESTED_VAL);

    /** An Inner instance bound to {@code host}; native reads innerValue + this$0. */
    public static final Host.Inner innerInst = host.newInner();

    // ── Identity publication (so the synthetic this$0 check is exact) ────────
    public static volatile int hostIdentity;
    public static volatile int staticNestedIdentity;
    public static volatile int innerIdentity;

    // ── Probe-published composite results (the JDK-independent proofs) ───────
    /** Set by mode 1 to innerInst.outerPlusInner(); native asserts == 106. */
    public static volatile int outerPlusInnerValue;
    /** Set by mode 1 to staticNested.doubled(); native asserts == 84. */
    public static volatile int doubledValue;

    static
    {
        // Publish identities once at load (also valid before any probe runs).
        hostIdentity         = System.identityHashCode(host);
        staticNestedIdentity = System.identityHashCode(staticNested);
        innerIdentity        = System.identityHashCode(innerInst);

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return NestedClasses.go && !NestedClasses.done;
            }

            @Override
            public void run()
            {
                if (NestedClasses.mode == 1)
                {
                    // Real bytecode dispatch: reads outerField via the synthetic
                    // this$0 (Inner) and value (StaticNested).  These are the
                    // authoritative, JDK-independent composite proofs.
                    NestedClasses.outerPlusInnerValue = NestedClasses.innerInst.outerPlusInner();
                    NestedClasses.doubledValue        = NestedClasses.staticNested.doubled();
                }
                NestedClasses.done = true;
            }
        });
    }
}
