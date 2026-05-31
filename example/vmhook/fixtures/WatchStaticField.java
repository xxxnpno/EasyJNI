package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the watch_static_field feature (area: watchers / hardware
 * debug registers).
 *
 * watch_static_field installs a hardware data breakpoint (one of the CPU's
 * four DR0-DR3 slots) on a Java static field's backing storage.  The trap
 * fires SYNCHRONOUSLY on whichever thread executes the write, *during* the
 * write instruction, and the native callback runs inside a vectored
 * exception handler on that same thread.  vmhook arms the trap on every
 * thread that exists at install time -- which includes THIS Harness loop
 * thread -- so a putstatic executed inside run() below traps immediately.
 *
 * The native module:
 *   (a) installs a watch on a watched static int BEFORE raising `go`,
 *   (b) raises `go`; the Harness loop (on the Java thread) runs run(), which
 *       does N genuine putstatic writes to the watched field -- each write
 *       trapping and invoking the native callback synchronously,
 *   (c) polls `done`, then reads back its atomic fire-counters and the last
 *       value the callback observed, asserting the callback saw the field's
 *       NEW value.
 *
 * Because the DR trap is synchronous on the writing thread, every write the
 * module wants observed must happen inside a SINGLE run() invocation (the
 * `done` flag latches).  The module selects which field(s) to drive via the
 * `mode` selector; it sets `mode` and clears `done` on the rising edge of
 * `go`.
 *
 * The fixture exposes FIVE independent watched int fields (counterA..counterE)
 * so the module can characterise the four-hardware-slot limit: watch four of
 * them at once (slots fill), prove a fifth watch is refused, then prove that
 * dropping a watch frees its slot.  Each mode writes ONE field a known number
 * of times so the module can attribute fired callbacks to a specific watch.
 *
 * Java 8 syntax only (anonymous Probe class; no var / lambda / switch-expr).
 */
public final class WatchStaticField
{
    // -- go / done handshake driven by the native module via run_probe ------
    public static volatile boolean go;
    public static volatile boolean done;

    /** Scenario selector; native sets it (and clears done) before raising go. */
    public static volatile int mode;

    // =====================================================================
    //  WATCHED static int fields.  Each is written ONLY by writeField(...)
    //  below, via genuine putstatic bytecode, so the native hardware-DR
    //  watchpoint traps on every increment.  They start at 0; the module
    //  resets them through mode 0 before each scenario so fire-counts and
    //  last-observed values are deterministic across scenarios.
    //
    //  int (4 bytes) is the canonical watched width: it exercises the DR
    //  LEN=four_bytes path and a putstatic is a single aligned 4-byte store.
    // =====================================================================
    public static volatile int counterA;
    public static volatile int counterB;
    public static volatile int counterC;
    public static volatile int counterD;
    public static volatile int counterE;

    /**
     * How many increments each "write" mode performs in one run().  The module
     * mirrors this constant and asserts the callback fired exactly this many
     * times (the trap is synchronous and one-per-write).
     */
    public static final int WRITE_COUNT = 12;

    /**
     * The exact value the LAST increment leaves in the driven field, so the
     * native callback's "new value" argument can be asserted precisely.  The
     * field is reset to 0 (mode 0) before each scenario, so after WRITE_COUNT
     * unit increments the final value is WRITE_COUNT.
     */
    public static final int FINAL_VALUE = WRITE_COUNT;

    /** Records, per scenario, how many putstatic writes run() actually did. */
    public static volatile int writesMade;

    /** True after loadFixtures()+static-init, so a native readiness check works. */
    public static volatile boolean ready = true;

    private static void resetCounters()
    {
        counterA = 0;
        counterB = 0;
        counterC = 0;
        counterD = 0;
        counterE = 0;
        writesMade = 0;
    }

    /**
     * Increments the selected watched field WRITE_COUNT times with a genuine
     * putstatic each iteration (read-modify-write of a volatile int compiles
     * to getstatic/iadd/putstatic, so the store the DR watches really runs).
     * A tiny sleep between writes spaces the traps out and keeps the loop from
     * being optimised into a single store, so each increment is an independent
     * 4-byte store the hardware breakpoint fires on.
     */
    private static void writeField(final int which)
    {
        int made = 0;
        for (int i = 0; i < WRITE_COUNT; i++)
        {
            switch (which)
            {
                case 0: counterA = counterA + 1; break;
                case 1: counterB = counterB + 1; break;
                case 2: counterC = counterC + 1; break;
                case 3: counterD = counterD + 1; break;
                case 4: counterE = counterE + 1; break;
                default: break;
            }
            made++;
            try { Thread.sleep(1); } catch (final InterruptedException ie) { /* ignore */ }
        }
        writesMade = made;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return WatchStaticField.go && !WatchStaticField.done;
            }

            @Override
            public void run()
            {
                switch (WatchStaticField.mode)
                {
                    case 0:
                        // Reset only: clears every watched field to 0 so the
                        // next scenario's fire-count / last-value is clean.
                        resetCounters();
                        break;
                    case 1:
                        writeField(0); // drive counterA
                        break;
                    case 2:
                        writeField(1); // drive counterB
                        break;
                    case 3:
                        writeField(2); // drive counterC
                        break;
                    case 4:
                        writeField(3); // drive counterD
                        break;
                    case 5:
                        writeField(4); // drive counterE
                        break;
                    default:
                        break;
                }
                WatchStaticField.done = true;
            }
        });
    }
}
