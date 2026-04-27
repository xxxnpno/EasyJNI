package vmhook.example;

/**
 * Entry point for the VMHook example target process.
 *
 * What this does:
 *   1. Creates several Player instances with known field values.
 *   2. Calls tick() on each player in a loop, slowly mutating their state.
 *   3. Prints a status line every 5 seconds.
 *
 * Inject VMHook.dll while this process is running to observe:
 *   - Class discovery: "vmhook/example/Player" and "vmhook/example/Main" appear in the class list.
 *   - Field lookup: health (float, instance), x/y/z (double, instance),
 *                   name (String ref, instance), count (int, static).
 *   - Field offsets printed by VMHook are correct for the running JDK.
 *
 * Run with:
 *   java -cp out vmhook.example.Main
 */
public class Main
{
    public static void main(final String[] args) throws InterruptedException
    {
        System.out.println("=== VMHook example target ===");
        System.out.println("PID: " + ProcessHandle.current().pid());
        System.out.println("JVM: " + System.getProperty("java.vm.name")
            + " " + System.getProperty("java.version"));
        System.out.println("Inject VMHook.dll now, then press DELETE inside the VMHook console to unload.");
        System.out.println();

        final Player[] players = {
            new Player("Alice",  100.0f,  0.0, 64.0, 0.0),
            new Player("Bob",     75.5f, 10.0, 64.0, 5.0),
            new Player("Charlie", 50.0f, -5.0, 64.0, 3.0),
        };

        long tick_count = 0;

        while (true)
        {
            for (final Player player : players)
            {
                player.tick();
            }

            ++tick_count;

            // Print status every 5 seconds (assuming ~100 ticks/s)
            if (tick_count % 500 == 0)
            {
                System.out.printf("[tick %6d] Player.count=%d%n", tick_count, Player.count);
                for (final Player player : players)
                {
                    System.out.println("  " + player);
                }
            }

            Thread.sleep(10);
        }
    }
}
