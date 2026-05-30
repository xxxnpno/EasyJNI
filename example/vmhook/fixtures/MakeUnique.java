package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the vmhook::make_unique&lt;T&gt; feature (area: object).
 *
 * make_unique allocates a fresh Java object from native code.  It prefers the
 * JNI NewObjectA path (which runs the REAL Java &lt;init&gt; chain), and only
 * falls back to a raw TLAB allocation + the C++ wrapper's construct(...) hook
 * when no matching Java constructor exists / NewObjectA is unavailable.  This
 * fixture exposes one constructor per JVM descriptor the native module wants to
 * drive, plus enough fields to read every constructed value back through the
 * wrapper and prove the constructor body actually executed.
 *
 * Why a hook + handshake?  make_unique is HotSpot-only: it needs a live
 * JavaThread (captured by an interpreter hook or discovered from VM metadata).
 * The native module installs a scoped_hook on {@link #trigger()} and performs
 * all its make_unique calls from inside that detour, exactly the way the
 * canonical example.cpp make_unique test does.  The probe below also allocates
 * a throwaway Object first so the current thread's TLAB is freshly refilled,
 * keeping the native allocation path warm.
 *
 * Every constructor and the trigger bump observable static/instance counters so
 * the native side can assert the constructor body (not just the allocation)
 * ran.  Each constructor records its own descriptor into {@link #lastCtor} so a
 * test can confirm which &lt;init&gt; the JVM dispatched.
 */
public final class MakeUnique
{
    // ── go / done handshake driven by the native module via run_probe ──────────
    /** Native raises this to request the probe action; clears it afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true once it has run; native polls it. */
    public static volatile boolean done;

    // ── Static observers ───────────────────────────────────────────────────────
    /** Incremented by EVERY constructor below (incl. the trigger's own new). */
    public static volatile int instanceCount;

    /** The JVM descriptor of the most-recently dispatched constructor. */
    public static volatile String lastCtor = "<none>";

    /** Set true by the trigger() body so the hook has a real dispatch to ride. */
    public static volatile boolean triggerRan;

    // ── Instance fields the native side reads back through the wrapper ──────────
    public int    intField;       // set by (I...) constructors
    public long   longField;      // set by (...J...) constructors
    public double doubleField;    // set by (...D) constructors
    public String stringField = "<unset>";   // set by (Ljava/lang/String;...) constructors
    public boolean boolField;     // set by construct(boolean) on the TLAB fallback path
    public int    ctorTag;        // 0 default; each ctor stamps a distinct id

    // ── Constructors: one per descriptor the native module exercises ───────────

    /** ()V — no-arg.  intField stays 0; ctorTag = 1. */
    public MakeUnique()
    {
        instanceCount++;
        this.ctorTag = 1;
        lastCtor = "()V";
    }

    /** (I)V — single int.  ctorTag = 2. */
    public MakeUnique(final int i)
    {
        instanceCount++;
        this.intField = i;
        this.ctorTag = 2;
        lastCtor = "(I)V";
    }

    /** (II)V — two ints, summed into intField.  ctorTag = 3. */
    public MakeUnique(final int a, final int b)
    {
        instanceCount++;
        this.intField = a + b;
        this.ctorTag = 3;
        lastCtor = "(II)V";
    }

    /** (IJD)V — int, long, double.  ctorTag = 4. */
    public MakeUnique(final int i, final long j, final double d)
    {
        instanceCount++;
        this.intField = i;
        this.longField = j;
        this.doubleField = d;
        this.ctorTag = 4;
        lastCtor = "(IJD)V";
    }

    /** (Ljava/lang/String;)V — single String.  ctorTag = 5. */
    public MakeUnique(final String s)
    {
        instanceCount++;
        this.stringField = (s == null) ? "<null-arg>" : s;
        this.ctorTag = 5;
        lastCtor = "(Ljava/lang/String;)V";
    }

    /** (Ljava/lang/String;I)V — String + int.  ctorTag = 6. */
    public MakeUnique(final String s, final int i)
    {
        instanceCount++;
        this.stringField = (s == null) ? "<null-arg>" : s;
        this.intField = i;
        this.ctorTag = 6;
        lastCtor = "(Ljava/lang/String;I)V";
    }

    // ── Probe self-registration ─────────────────────────────────────────────────
    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return MakeUnique.go && !MakeUnique.done;
            }

            @Override
            public void run()
            {
                // Freshly refill this thread's TLAB so the native make_unique
                // allocation (and any TLAB-fallback path) has room, mirroring
                // the canonical example.cpp make_unique probe.
                final Object tlabRefill = new Object();
                if (tlabRefill == null)
                {
                    throw new IllegalStateException("unreachable");
                }
                // Real bytecode dispatch the native scoped_hook rides; the
                // native detour performs every make_unique call from here.
                new MakeUnique().trigger();
                MakeUnique.done = true;
            }
        });
    }

    /**
     * Hookable instance method.  The native module hooks this; its detour runs
     * all the make_unique allocations while a JavaThread is guaranteed live.
     */
    public void trigger()
    {
        triggerRan = true;
        lastCtor = "trigger";
    }
}
