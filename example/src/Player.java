package vmhook.example;

/**
 * A simple class with both instance fields and a static field.
 * VMHook reads these at runtime to demonstrate field discovery via
 * InstanceKlass._fields (JDK 8-20) or _fieldinfo_stream (JDK 21+).
 *
 * Expected field offsets (printed by VMHook.dll on injection):
 *   health   float  instance
 *   x        double instance
 *   y        double instance
 *   z        double instance
 *   name     String instance (compressed OOP)
 *   count    int    static   (lives in java.lang.Class mirror)
 */
public class Player
{
    // Static field — stored in the java.lang.Class mirror object.
    public static int count = 0;

    // Instance fields — stored at fixed byte offsets within each Player object.
    public float  health;
    public double x;
    public double y;
    public double z;
    public String name;

    public Player(final String name, final float health, final double x, final double y, final double z)
    {
        this.name   = name;
        this.health = health;
        this.x      = x;
        this.y      = y;
        this.z      = z;
        ++Player.count;
    }

    public void tick()
    {
        // Simulates lightweight per-frame work so this method stays interpreted
        // long enough for VMHook to install a hook before JIT kicks in.
        this.x += 0.001;
        this.y  = Math.sin(this.x) * 10.0;
    }

    @Override
    public String toString()
    {
        return String.format("Player{name=%s, health=%.1f, x=%.3f, y=%.3f, z=%.3f}",
            this.name, this.health, this.x, this.y, this.z);
    }
}
