package vmhook.example;

import java.lang.management.ManagementFactory;

/**
 * Entry point for the VMHook example / test target process.
 *
 * Compatible with JDK 8 through the latest release.
 *
 * Inject VMHook.dll while this process is running.  The DLL console shows
 * PASS / FAIL for every assertion.  Press DELETE to unload the DLL.
 *
 * Run with:
 *   java [-Xint] -cp out vmhook.example.Main
 *
 * The -Xint flag disables JIT so interpreter hooks always fire regardless of
 * how long the process has been running.  Required for automated testing via
 * example\test_all_jdks.ps1; optional for manual experimentation.
 */
public class Main
{
    // Stable handle for native-side tests to retrieve the live TestTarget instance.
    public static volatile TestTarget testTargetRef;

    public static void main(final String[] args) throws InterruptedException
    {
        System.out.println("=== VMHook example / test target ===");

        // ManagementFactory.getRuntimeMXBean().getName() returns "PID@hostname"
        // on every HotSpot version from JDK 8 to JDK 26+.
        final String runtime_name = ManagementFactory.getRuntimeMXBean().getName();
        final String pid_string   = runtime_name.contains("@")
            ? runtime_name.substring(0, runtime_name.indexOf('@'))
            : runtime_name;

        System.out.println("PID : " + pid_string);
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
        testTargetRef = target;

        long tick_count = 0;

        while (true)
        {
            for (final Player player : players)
            {
                player.tick();
            }

            // Call the hookable method every iteration.
            target.onTick((int) tick_count);
            testTargetRef = target;
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
