package vmhook.fixtures;

import vmhook.Harness;

/**
 * Pilot fixture that validates the modular JVM harness end-to-end:
 *  - it self-registers a probe (proves Main's fixture scan + Harness wiring),
 *  - it exposes a `go`/`done` handshake (proves the native run_probe path),
 *  - it has a hookable instance method `touch(int)` that the native pilot
 *    module hooks, so the run proves an interpreter hook fires through the new
 *    modular path on a real Java bytecode dispatch.
 *
 * Every feature fixture follows this exact shape: a `go`/`done` pair, some
 * data/methods to exercise, and a static-block self-registration.
 */
public final class Pilot
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Observable side effect the native module asserts on. */
    public static volatile int observed;

    private int seed = 1000;

    /** Hookable instance method — the native pilot module hooks this. */
    public int touch(final int delta)
    {
        return this.seed + delta;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return Pilot.go && !Pilot.done;
            }

            @Override
            public void run()
            {
                final Pilot instance = new Pilot();
                // Calling touch() through normal bytecode dispatch is what makes
                // the native interpreter hook fire.
                Pilot.observed = instance.touch(42);
                Pilot.done = true;
            }
        });
    }
}
