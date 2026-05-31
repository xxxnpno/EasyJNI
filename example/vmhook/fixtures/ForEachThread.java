package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the for_each_thread feature (area: threads / HotSpot thread list).
 *
 * vmhook::for_each_thread() walks the JVM's live JavaThread list (Path 1: the
 * classic intrusive Threads::_thread_list; Path 2: the JDK 10+ SMR ThreadsList
 * snapshot) and hands the native visitor a thread_info per live Java thread.
 * The thread_info carries only {JavaThread*, state, os_thread_id} -- there is no
 * thread NAME, so the module cannot match the spawned thread by name.  Instead
 * this fixture lets the native side prove enumeration TRACKS a newly-created
 * Java thread by an exact LIVE-COUNT DELTA: enumerate a baseline, start an extra
 * named daemon thread that parks itself alive, enumerate again (count must rise
 * by exactly one), then release the thread and enumerate a third time (count
 * must fall back).  This is JDK-portable across Path 1 and Path 2 and needs no
 * native OS-TID-from-Java trick.
 *
 * Coordination is the standard go/done handshake plus a `mode` selector.  Two
 * extra volatile flags bridge the spawned thread's lifecycle to the native side:
 *   - `threadUp`  : the spawned worker sets this true once it is running and
 *                   parked (so the native side can WAIT for the worker to become
 *                   a live JavaThread before re-enumerating).  Native reads it
 *                   directly via static_field("threadUp")->get() -- a plain heap
 *                   read, no bytecode dispatch, so it works off the Java thread.
 *   - `stop`      : native sets this true to ask the parked worker to exit; the
 *                   worker observes it and returns, after which the JVM reclaims
 *                   its JavaThread and the live count drops back.
 *
 * mode selector (native sets `mode` + clears `done` on the rising edge of go):
 *   1 = startWorker(): create + start the named daemon worker, then return
 *       (done=true) WITHOUT joining -- the worker stays parked & alive so the
 *       native side can enumerate it.  Idempotent: a second mode-1 with the
 *       worker already up is a no-op.
 *   2 = (no-op tick) -- a plain done=true so the native side has a cheap probe
 *       cycle available if it needs one; the actual worker release is driven by
 *       the `stop` flag, which native can set directly (no probe required).
 *
 * The worker is a DAEMON so it can never wedge JVM shutdown even if the native
 * side fails before setting `stop`; it also self-times-out after a generous
 * bound so a crashed/aborted native run cannot leak a busy thread.
 *
 * Java 8 syntax only (anonymous Runnable + anonymous Probe; no var/lambda).
 */
public final class ForEachThread
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Scenario selector; native sets it before raising go. */
    public static volatile int mode;

    /** The exact name the spawned worker thread is given. */
    public static final String WORKER_NAME = "vmhook-fet-probe";

    /** Set true by the worker once it is running and parked. */
    public static volatile boolean threadUp;

    /** Native sets this true to ask the parked worker to exit. */
    public static volatile boolean stop;

    /** The live worker (null until started). */
    private static volatile Thread worker;

    /**
     * Creates and starts the named daemon worker if it is not already running.
     * Returns immediately; the worker parks itself alive until {@code stop} is
     * set (or a self-timeout fires).  Called on the Harness tick thread.
     */
    private static void startWorker()
    {
        if (worker != null && worker.isAlive())
        {
            // Already up -- mode 1 is idempotent.
            return;
        }
        stop = false;
        threadUp = false;

        final Thread t = new Thread(new Runnable()
        {
            @Override
            public void run()
            {
                // Announce we are alive and parked so the native side can
                // re-enumerate and see the count rise by one.
                ForEachThread.threadUp = true;

                // Park alive until native asks us to stop, with a generous
                // self-timeout so an aborted native run cannot leak this thread.
                final long deadlineNanos = System.nanoTime() + 30_000_000_000L; // 30s
                while (!ForEachThread.stop && System.nanoTime() < deadlineNanos)
                {
                    try
                    {
                        Thread.sleep(5);
                    }
                    catch (final InterruptedException ignored)
                    {
                        // Treat an interrupt as a stop request.
                        break;
                    }
                }

                ForEachThread.threadUp = false;
            }
        }, WORKER_NAME);

        t.setDaemon(true);
        worker = t;
        t.start();
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return ForEachThread.go && !ForEachThread.done;
            }

            @Override
            public void run()
            {
                switch (ForEachThread.mode)
                {
                    case 1:
                        startWorker();
                        break;
                    case 2:
                    default:
                        // Plain tick: nothing to do but acknowledge.
                        break;
                }
                ForEachThread.done = true;
            }
        });
    }
}
