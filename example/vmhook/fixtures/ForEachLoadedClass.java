package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the for_each_loaded_class feature (area: class enumeration /
 * SystemDictionary + ClassLoaderDataGraph walk).
 *
 * vmhook::for_each_loaded_class(visitor) takes a SNAPSHOT of every Java class
 * currently reachable through the global ClassLoaderDataGraph and invokes
 * `visitor(internalName, klass*)` once per Klass.  Enumeration is a pure
 * HotSpot-internal read — it needs NO bytecode dispatch — so unlike the
 * method-hook fixtures this one does not have to drive a hooked Java method on
 * the Harness tick thread.  Its real contribution is simply EXISTING:
 *
 *   - It is a top-level, non-'$' class under vmhook/fixtures, so Main.loadFixtures()
 *     Class.forName's it at startup.  That places vmhook/fixtures/ForEachLoadedClass
 *     into the ClassLoaderData graph under the APPLICATION class loader, giving the
 *     native module a known APP-LOADED class to find in the enumeration (proving the
 *     walk reaches more than just bootstrap classes).
 *
 * The native side asserts only PORTABLE invariants (a sane class count, the
 * universal bootstrap classes java/lang/Object + java/lang/String, this OWN
 * fixture class, all klass pointers valid, and that the walk terminates).
 * Launcher-entry classes (vmhook/Main) are treated as BEST-EFFORT: HotSpot's
 * JDK 8 SystemDictionary walk omits the launcher main class, so the module
 * records vmhook/Main's presence as [INFO] and never hard-fails on it.
 *
 * To give the native module additional, richer (still portable) targets without
 * relying on the launcher class, this fixture force-loads a couple of anchors in
 * its static initializer:
 *   - a NESTED class vmhook/fixtures/ForEachLoadedClass$Inner (instantiated +
 *     statically referenced, since Main.loadFixtures SKIPS '$' files and HotSpot
 *     would otherwise never load it), so the module can confirm a nested app
 *     class also appears in the snapshot;
 *   - the primitive-array klass [I and the object-array klass
 *     [Ljava/lang/String; (anchored so the graph holds them) — array klasses are
 *     a separate Klass family the walk should still surface.
 *
 * A trivial go/done probe is registered purely so the fixture is a well-formed
 * Harness participant (and so a future revision could ride a real dispatch if
 * ever needed); its run() is a no-op acknowledge.
 *
 * Java 8 syntax only (anonymous Probe; no var / lambda-in-field / records).
 */
public final class ForEachLoadedClass
{
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    /**
     * A distinctive static sentinel.  The native module is free to read this back
     * through the resolved klass (via the registered wrapper) to confirm the klass
     * the enumeration surfaced for THIS class is genuinely usable — not merely a
     * non-null pointer.
     */
    public static int sentinel = 0x10ADC1A5;

    // ── Forced-load anchors for the nested + array classes ─────────────────────
    //
    // Main.loadFixtures() skips '$' files and HotSpot never loads a class until it
    // is referenced, so these static references force Inner, [I and
    // [Ljava/lang/String; into the ClassLoaderData graph so the native module can
    // find them in the enumeration snapshot.

    /** Keeps the nested klass loaded + reachable in the graph. */
    public static Inner innerAnchor;

    /** Keeps the [I primitive-array klass loaded + reachable. */
    public static int[] primIntArrayAnchor;

    /** Keeps the [Ljava/lang/String; object-array klass loaded + reachable. */
    public static String[] strArrayAnchor;

    /**
     * A nested (inner, non-static) class.  Its internal name is
     * vmhook/fixtures/ForEachLoadedClass$Inner.  Force-loaded below so the native
     * module can confirm a nested application class is enumerated too.
     */
    public final class Inner
    {
        public int innerTag = 0xC0DE;

        public int tag()
        {
            return this.innerTag;
        }
    }

    static
    {
        // Force-load Inner (needs an enclosing instance) and anchor it.
        final ForEachLoadedClass outer = new ForEachLoadedClass();
        innerAnchor = outer.new Inner();

        // Force-load + anchor the array klasses.
        primIntArrayAnchor = new int[] { 1, 2, 3 };
        strArrayAnchor = new String[] { "alpha", "omega" };

        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ForEachLoadedClass.go && !ForEachLoadedClass.done;
            }

            @Override
            public void run()
            {
                // Enumeration is a pure native read; nothing to dispatch here.
                // Acknowledge so any native probe cycle completes cleanly.
                ForEachLoadedClass.done = true;
            }
        });
    }
}
