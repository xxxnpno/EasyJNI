package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the on_exception feature (area: hooks / exception-construction
 * watcher).  Migrates the legacy vmhook.Example throwProbe / test_on_exception.
 *
 * Drives vmhook::on_exception(callback) — the callback that fires whenever a
 * java.lang.Throwable (or any subclass) is constructed, because every public
 * Throwable constructor calls Throwable.fillInStackTrace() (the hooked method)
 * before returning.  This fixture proves, on a LIVE JVM with a GENUINE `athrow`,
 * that:
 *   - constructing + throwing + catching a java.lang.IllegalStateException makes
 *     the native callback observe the JVM-internal class name
 *     "java/lang/IllegalStateException" (when the underlying hook is armed),
 *   - a DIFFERENT genuine throw (java.lang.NumberFormatException) is reported
 *     under its own internal name, so the callback can discriminate by type,
 *   - several throws in one cycle each fire the callback (fan-out / counting),
 *   - a NO-THROW cycle constructs no Throwable, so the callback must NOT fire
 *     (the control that distinguishes "armed but silent" from "armed + firing").
 *
 * EVERY throw here is a real `athrow` of a freshly-constructed exception whose
 * public constructor runs fillInStackTrace on THIS Java thread, immediately
 * before the throw — so the interpreter hook (if armed) fires synchronously,
 * exactly like the throw the legacy probe performed.
 *
 * Canonical go/done handshake: the native module sets `mode`, raises `go`, the
 * Harness loop runs run() on the Java thread (genuine bytecode dispatch, which
 * is what makes the interpreter hook fire), then the module polls the latched
 * `done`.  Because `done` latches, every throw a single assertion needs happens
 * inside one run() invocation; the module selects the scenario via `mode`.
 *
 * Crucially, run() ALSO records Java-observable witnesses — `throwsObserved`
 * (how many exceptions this cycle constructed + threw + caught) and
 * `lastThrowKind` (a stable tag for the last type thrown).  The native side
 * reads these back to prove the Java throw genuinely happened, INDEPENDENTLY of
 * whether the (possibly silently-dead) on_exception callback fired.  That lets
 * the module distinguish "the callback did not fire" (the known flag-reset flaw)
 * from "the throw never ran".
 */
public final class OnException
{
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Selects which scenario run() executes.  Set by the native module BEFORE it
     * raises `go` so a single probe cycle drives exactly the throws it asserts on.
     *   1 = throw+catch ONE IllegalStateException        (primary trap trigger)
     *   2 = throw+catch MANY IllegalStateException        (fan-out / per-watcher count)
     *   3 = throw+catch ONE NumberFormatException         (type discrimination)
     *   4 = NO throw at all                               (control: no Throwable built)
     */
    public static volatile int mode;

    // ---- Java-observable witnesses (prove the throw genuinely ran) ---------

    /** How many exceptions run() constructed + threw + caught this cycle. */
    public static volatile int throwsObserved;

    /**
     * Stable tag for the LAST exception kind thrown this cycle, so the native
     * side can confirm the Java action without resolving a class name itself:
     *   0 = none thrown this cycle (mode 4)
     *   1 = IllegalStateException
     *   2 = NumberFormatException
     */
    public static volatile int lastThrowKind;

    /** The detail message carried by the last thrown exception (round-trip aid). */
    public static volatile String lastThrowMsg = "";

    // ---- Constants mirrored on the native side ----------------------------

    /** How many ISEs mode 2 throws in one cycle (fan-out counting target). */
    public static final int MANY_THROWS = 4;

    /** The detail message every IllegalStateException carries. */
    public static final String ISE_MESSAGE = "vmhook-on-exception-ISE";

    /** The internal ('/'-separated) name the native callback expects for mode 1/2. */
    public static final String ISE_INTERNAL_NAME = "java/lang/IllegalStateException";

    /** The internal name expected for the mode-3 NumberFormatException. */
    public static final String NFE_INTERNAL_NAME = "java/lang/NumberFormatException";

    // ---- Throwing actions (each is a genuine construct + athrow + catch) ---

    /**
     * Construct, throw, and catch ONE IllegalStateException.  `throw` is a real
     * athrow of a freshly-built exception, so its constructor's fillInStackTrace
     * runs on this thread right before the throw — the hook (if armed) fires.
     */
    private static void throwOneIse()
    {
        try
        {
            throw new IllegalStateException(ISE_MESSAGE);
        }
        catch (final IllegalStateException ex)
        {
            throwsObserved++;
            lastThrowKind = 1;
            lastThrowMsg = ex.getMessage();
        }
    }

    /** Construct + throw + catch MANY IllegalStateExceptions (distinct athrows). */
    private static void throwManyIse()
    {
        for (int n = 0; n < MANY_THROWS; ++n)
        {
            try
            {
                throw new IllegalStateException(ISE_MESSAGE);
            }
            catch (final IllegalStateException ex)
            {
                throwsObserved++;
                lastThrowKind = 1;
                lastThrowMsg = ex.getMessage();
            }
        }
    }

    /**
     * Construct + throw + catch ONE NumberFormatException.  Built directly (not
     * via Integer.parseInt) so the type is unambiguous and the only Throwable
     * this cycle constructs is the NFE — letting the native side assert the
     * callback reports THIS type and not a stray internal exception.
     */
    private static void throwOneNfe()
    {
        try
        {
            throw new NumberFormatException("vmhook-on-exception-NFE");
        }
        catch (final NumberFormatException ex)
        {
            throwsObserved++;
            lastThrowKind = 2;
            lastThrowMsg = ex.getMessage();
        }
    }

    private static void runScenario()
    {
        // Reset witnesses at the start of every cycle so each drive() reads only
        // what THIS cycle produced.
        throwsObserved = 0;
        lastThrowKind = 0;
        lastThrowMsg = "";
        switch (mode)
        {
            case 1:
                throwOneIse();
                break;
            case 2:
                throwManyIse();
                break;
            case 3:
                throwOneNfe();
                break;
            case 4:
                // Deliberately construct NO Throwable: the control cycle.  The
                // native callback (if it were firing) must observe nothing new.
                break;
            default:
                break;
        }
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return OnException.go && !OnException.done;
            }

            @Override
            public void run()
            {
                runScenario();
                OnException.done = true;
            }
        });
    }
}
