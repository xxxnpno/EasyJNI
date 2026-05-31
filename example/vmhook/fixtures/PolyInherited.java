package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the "poly_inherited_oop" feature (area: fields + methods).
 *
 * The live-JVM counterpart of the legacy example.cpp test_poly_probe: a
 * B-extends-A object exercised through vmhook so the native module can prove
 * vmhook::find_field's Klass::get_super() super-chain walk resolves an INHERITED
 * INSTANCE field, and that an inherited method is found (and, when the call gate
 * is exported, called) through the same walk.
 *
 *     A                         (super — declares the protected instance field
 *        ^  extends              protectedInt = 1337 and the protected instance
 *     B                          method protectedAdd(int) = protectedInt + x)
 *                               (sub — declares its OWN field bInt = 42)
 *
 * A and B are NESTED static classes of this fixture, so javac emits
 * PolyInherited$A and PolyInherited$B; the native module registers wrappers on
 * "vmhook/fixtures/PolyInherited$A" and "vmhook/fixtures/PolyInherited$B".
 *
 * Mirrors the legacy A/B exactly (same field name protectedInt, same value 1337,
 * same own field bInt = 42, same method protectedAdd whose body is
 * protectedInt + x) so the native constants match the canonical values and the
 * inherited-method call returns protectedAdd(3) == 1340.
 *
 * What the native module drives through this fixture:
 *   - get the live B instance (exposed as a static field),
 *   - read B's OWN bInt (super walk depth 0) and the INHERITED protected
 *     A.protectedInt (super walk depth 1) THROUGH the B klass — the offset must
 *     resolve to A's declared field,
 *   - find + (best-effort, gated on the call gate) call the inherited
 *     protectedAdd(3) and read back 1340.
 *
 * The go/done handshake lets the native side run the Java witness (Java reads
 * the same three quantities through real getfield / invokevirtual bytecode), so
 * the module can cross-check that the JVM itself sees identical values.  This
 * fixture is the only test thread for read-only ops, matching vmhook's
 * documented single-reader contract.  Java 8 syntax only (anonymous Probe; no
 * var / lambdas / records / switch-expressions).
 */
public final class PolyInherited
{
    // -- go / done handshake (native raises go; the probe latches done) --------
    public static volatile boolean go;
    public static volatile boolean done;

    // ---- Java-side witnesses: the probe sets these by reading the SAME three
    //      quantities through genuine bytecode, so the native module can confirm
    //      the JVM observes identical values to vmhook's offset reads. ----------
    public static volatile boolean sawOwnField;          // bInt == 42
    public static volatile boolean sawInheritedField;    // protectedInt == 1337
    public static volatile boolean sawInheritedMethod;   // protectedAdd(3) == 1340

    // ---- Canonical values (mirrored on the native side) --------------------
    public static final int PROTECTED_INT     = 1337;    // A.protectedInt init
    public static final int B_INT             = 42;      // B.bInt init
    public static final int ADD_ARG           = 3;       // protectedAdd argument
    public static final int ADD_RESULT        = 1340;    // protectedAdd(3) result

    /**
     * Super class — declares the inherited protected instance field and the
     * inherited protected instance method.  Mirrors legacy vmhook.A exactly.
     */
    static class A
    {
        protected int protectedInt = PROTECTED_INT;

        protected int protectedAdd(final int x)
        {
            return this.protectedInt + x;
        }
    }

    /**
     * Sub class — adds its OWN instance field.  Mirrors legacy vmhook.B (the
     * own int field bInt = 42 alongside the inherited protectedInt).
     */
    static class B extends A
    {
        int bInt = B_INT;
    }

    // ---- The live B instance the native side wraps (as a B-typed wrapper) ----
    public static final B bInstance = new B();

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return PolyInherited.go && !PolyInherited.done;
            }

            @Override
            public void run()
            {
                // Java reads the same three quantities through real getfield /
                // invokevirtual bytecode, so the native module can prove the JVM
                // itself agrees with vmhook's offset-based reads.
                PolyInherited.sawOwnField        = (PolyInherited.bInstance.bInt == B_INT);
                PolyInherited.sawInheritedField  = (PolyInherited.bInstance.protectedInt == PROTECTED_INT);
                PolyInherited.sawInheritedMethod = (PolyInherited.bInstance.protectedAdd(ADD_ARG) == ADD_RESULT);
                PolyInherited.done = true;
            }
        });
    }
}
