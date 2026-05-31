package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the vmhook::for_each_instance&lt;T&gt; feature (area: heap scan).
 *
 * vmhook::for_each_instance&lt;T&gt;() walks the JVM's collected-heap reservation
 * (Universe::_collectedHeap::_reserved) linearly in 4&nbsp;KiB chunks via a safe
 * read, decodes each candidate oop's narrow-klass pointer at +8, and hands the
 * native visitor a freshly-allocated {@code std::unique_ptr<T>} for every header
 * whose klass matches the registered wrapper.  It is a CONSERVATIVE, best-effort
 * raw-memory scan: it is NOT GC-cooperative and runs without a safepoint, so a
 * concurrent GC, a region-based heap (G1) with unmapped pages, or a coloured-
 * pointer collector (ZGC/Shenandoah) can legitimately make it MISS any given
 * object.  Critically, though, every object it DOES report is a real one — it
 * never fabricates a false instance.
 *
 * Design — this class IS the dedicated, count-controlled instance type the
 * native side scans for.  The native module asks the JVM (via the standard
 * go/done probe) to populate {@link #PINNED}, a static array that holds exactly
 * {@link #PIN_COUNT} freshly-allocated ForEachInstance objects strongly
 * reachable for the entire scan, then performs the heap walk.  Because:
 *   - ONLY the probe ever constructs a ForEachInstance, and
 *   - it constructs exactly {@code PIN_COUNT} of them into {@code PINNED},
 * the JVM heap contains at most {@code PIN_COUNT} live instances of this class.
 * That gives the native side two RELIABLE bounds to assert (visited &gt; 0 and
 * visited &le; PIN_COUNT — the scanner can never see more instances than exist,
 * because it reports only real headers) while leaving "found a SPECIFIC pinned
 * instance" / "found ALL of them" as best-effort observations the conservative
 * scan does not guarantee.
 *
 * Each pinned instance carries a unique {@link #id} (0..PIN_COUNT-1) and a
 * constant {@link #marker} sentinel so the native side can, best-effort, read a
 * matched object's fields back through the wrapper and confirm it is genuinely
 * one of ours (marker matches, id in range) rather than merely a klass-pointer
 * coincidence.
 *
 * No hook is involved: for_each_instance is a pure VMStructs heap read the
 * native module calls straight from its worker thread (exactly like the legacy
 * inline example.cpp test_for_each_instance), so this fixture needs no trigger()
 * and no JavaThread handshake beyond the go/done allocate-and-pin step.
 *
 * Java 8 syntax only (no var / lambda-in-field / records / switch-expr).
 */
public final class ForEachInstance
{
    // ── go / done handshake driven by the native module via run_probe ──────────
    /** Native raises this to request the allocate-and-pin action; clears it after. */
    public static volatile boolean go;

    /** The probe action sets this true once the array is fully populated. */
    public static volatile boolean done;

    /** The exact number of live instances the probe pins for the scan. */
    public static final int PIN_COUNT = 100;

    /** A distinctive constant every pinned instance stamps into {@link #marker}. */
    public static final int MARKER = 0xFE10FE10;

    /**
     * Strong references that keep every constructed instance alive across the
     * native heap walk.  Populated exactly once by the probe; never cleared while
     * the scan runs, so the GC cannot reclaim (or, on a moving collector, is far
     * less likely to relocate) the objects mid-scan.
     */
    public static volatile ForEachInstance[] PINNED;

    /** Set true by the probe once PINNED is fully built (native reads it as a sanity flag). */
    public static volatile int pinnedCount;

    // ── Instance fields the native side reads back, best-effort, through the wrapper ──
    /** Unique 0..PIN_COUNT-1 index of this instance within {@link #PINNED}. */
    public final int id;

    /** Constant sentinel = {@link #MARKER}; lets native confirm a match is really ours. */
    public final int marker;

    /**
     * The ONLY constructor.  Kept private so nothing outside this fixture can
     * create a ForEachInstance, guaranteeing the live-instance count equals the
     * number the probe pins (the native side's reliable upper bound).
     */
    private ForEachInstance(final int id)
    {
        this.id = id;
        this.marker = MARKER;
    }

    /**
     * Allocate exactly {@link #PIN_COUNT} instances into {@link #PINNED}, each
     * with a unique id, and publish them.  Idempotent: a second call with the
     * array already built is a no-op (so a re-driven probe never inflates the
     * live count).  Runs on the Harness tick (Java) thread.
     */
    private static void allocateAndPin()
    {
        if (PINNED != null)
        {
            return; // already pinned — keep the live count at exactly PIN_COUNT
        }
        final ForEachInstance[] arr = new ForEachInstance[PIN_COUNT];
        for (int i = 0; i < PIN_COUNT; i++)
        {
            arr[i] = new ForEachInstance(i);
        }
        // Publish last so the volatile write makes every element visible together.
        PINNED = arr;
        pinnedCount = PIN_COUNT;
    }

    // ── Probe self-registration ─────────────────────────────────────────────────
    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ForEachInstance.go && !ForEachInstance.done;
            }

            @Override
            public void run()
            {
                allocateAndPin();
                ForEachInstance.done = true;
            }
        });
    }
}
