package vmhook.example;

/**
 * Entry point for the VMHook example / test target process.
 *
 * What this does:
 *   1. Creates Player instances (field discovery demo).
 *   2. Creates a TestTarget instance whose fields are verified by the C++ test harness.
 *   3. Calls onTick() on TestTarget every loop iteration so the hook gets exercised.
 *   4. Prints status every 5 seconds.
 *
 * Inject VMHook.dll while this process is running.  The DLL console will show
 * PASS / FAIL for every assertion.  Press DELETE to unload the DLL.
 *
 * Run with:
 *   java -cp out vmhook.example.Main
 */
public class Main
{
    public static void main(final String[] args) throws InterruptedException
    {
        System.out.println("=== VMHook example / test target ===");
        System.out.println("PID : " + ProcessHandle.current().pid());
        System.out.println("JVM : " + System.getProperty("java.vm.name")
            + " " + System.getProperty("java.version"));
        System.out.println("Inject VMHook.dll now, then press DELETE in the VMHook console.");
        System.out.println();

        // ---- Player instances (field demo) ----------------------------------
        final Player[] players = {
            new Player("Alice",   100.0f,  0.0, 64.0, 0.0),
            new Player("Bob",      75.5f, 10.0, 64.0, 5.0),
            new Player("Charlie",  50.0f, -5.0, 64.0, 3.0),
        };

        // ---- TestTarget instance (unit test subject) -----------------------
        final TestTarget target = new TestTarget();

        long tick_count = 0;

        while (true)
        {
            // Advance players
            for (final Player player : players)
            {
                player.tick();
            }

            // Call the hookable method on TestTarget
            target.onTick((int) tick_count);
            ++tick_count;

            // Status every 5 s (~100 ticks/s × 500)
            if (tick_count % 500 == 0)
            {
                System.out.printf("[tick %6d] Player.count=%d  TestTarget.hookCallCount=%d%n",
                    tick_count, Player.count, TestTarget.hookCallCount);
                for (final Player player : players)
                {
                    System.out.println("  " + player);
                }
            }

            Thread.sleep(10);
        }
    }
}
