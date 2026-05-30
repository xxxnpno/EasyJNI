package vmhook;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Generic JVM-side coordination for the modular test harness.
 *
 * Each feature fixture under vmhook.fixtures.* self-registers a {@link Probe}
 * in its static initializer.  Main loads every fixture (so the static blocks
 * run), then ticks all registered probes in its loop.  A probe runs its action
 * exactly when the native side has raised its "go" flag and not yet seen
 * "done" — that action calls the Java methods the C++ module hooked, so the
 * interpreter hooks fire on a real Java bytecode dispatch.
 *
 * This keeps adding a feature conflict-free: a new fixture is a new class that
 * registers itself; no edit to Main or to this file.
 */
public final class Harness
{
    private Harness() { }

    /** A unit of Java-side work a native module can request on demand. */
    public interface Probe
    {
        /** True when the native side wants this probe's action to run now. */
        boolean pending();

        /** Run the Java action (call hooked methods, mutate fields, ...). */
        void run();
    }

    private static final List<Probe> PROBES = new CopyOnWriteArrayList<>();

    /** Called from a fixture's static initializer. */
    public static void register(final Probe probe)
    {
        if (probe != null)
        {
            PROBES.add(probe);
        }
    }

    /**
     * Run every pending probe once.  Called from Main's main loop each tick.
     * A probe that throws is isolated so one bad fixture can't wedge the loop.
     */
    public static void tickAll()
    {
        for (final Probe probe : PROBES)
        {
            try
            {
                if (probe.pending())
                {
                    probe.run();
                }
            }
            catch (final Throwable ignored)
            {
                // The native side observes a missing "done" flag and fails the
                // corresponding check; never let one fixture kill the loop.
            }
        }
    }
}
