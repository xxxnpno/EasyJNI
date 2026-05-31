package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code interface_polymorphism} feature (area: fields + methods).
 *
 * Mirrors the legacy {@code test_interface_and_polymorphism} from
 * {@code vmhook/src/example.cpp} (the Animal / Dog / Pet shapes) as a
 * self-contained modular fixture, but folds the interface, the concrete
 * implementation, and the holder all INTO this one top-level fixture class so a
 * single {@code Class.forName("vmhook.fixtures.InterfacePoly")} (the only thing
 * {@code Main.loadFixtures()} does for a fixture) transitively loads everything
 * the native module touches.
 *
 * <p>The shape under test is the canonical interface-polymorphism case: a field
 * whose STATIC (declared) type is an interface but whose RUNTIME type is a
 * concrete subclass.</p>
 *
 * <pre>
 *     interface Animal  { String speak(); default String defaultGreet(); }
 *     class     Dog implements Animal { ... String speak() -> "...woof"; ... }
 *     holder    InterfacePoly { Animal pet = new Dog(...);  // declared Animal, IS a Dog }
 * </pre>
 *
 * Because {@code Animal} and {@code Dog} are nested inside this class, javac
 * emits them as {@code vmhook/fixtures/InterfacePoly$Animal} and
 * {@code vmhook/fixtures/InterfacePoly$Dog} -- the exact internal names the C++
 * module asserts the runtime klass resolves to.
 *
 * <p>What the native module proves on a live JVM (Java 8..25 x MSVC/Clang/GCC),
 * all from native code against the published {@link #SINGLETON}:</p>
 * <ul>
 *   <li>reading the {@code Animal}-typed {@code pet} field yields a usable wrapper
 *       whose decoded OOP's RUNTIME klass is {@code ...$Dog} (internal name ends
 *       with {@code "Dog"}), i.e. vmhook sees the concrete type, not the declared
 *       interface;</li>
 *   <li>the overridden {@code speak()} dispatched through the concrete-type
 *       wrapper reaches Dog's override and returns a {@code "woof"}-containing
 *       String (virtual dispatch);</li>
 *   <li>Dog-specific fields ({@code name}/{@code age}/{@code breed}) read back
 *       their deterministic values through the same wrapper;</li>
 *   <li>reading the slot as the DECLARED interface type and as the CONCRETE type
 *       yields the SAME decoded OOP (the field decode is type-agnostic);</li>
 *   <li>the interface DEFAULT method {@code defaultGreet()} -- declared on
 *       {@code Animal}, NOT overridden by {@code Dog} -- characterises whether
 *       vmhook walks the interface chain (it walks only the superclass chain), so
 *       the module records [INFO] rather than failing when it is unreachable.</li>
 * </ul>
 *
 * <p>The Java-side probe runs the SAME polymorphism observations and publishes a
 * single boolean witness ({@link #petIsDogSeen}) plus the Dog's speak() result,
 * so the native side can confirm the JVM itself agrees. Canonical go/done
 * handshake with a {@code mode}/done-reset selector drives exactly one dispatch.
 * Java 8 syntax only (anonymous {@code Harness.Probe}, no lambdas / var /
 * switch-expressions).</p>
 */
public final class InterfacePoly
{
    // ── go / done / mode handshake (native sets mode + clears done first) ───
    /** Native sets this true to request the action; clears it after. */
    public static volatile boolean go;

    /** The action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /**
     * Scenario selector. The native module sets this BEFORE raising {@code go}.
     *   0 = publish the Java-side polymorphism witnesses and call {@code tick()}
     *       once (the bytecode dispatch that fires the native interpreter hook).
     * One scenario suffices: every native observation is a side-effect-free read
     * against the published SINGLETON, so the probe exists only to fire the hook
     * and to (re)publish witnesses on the Java thread.
     */
    public static volatile int mode;

    // ── The nested interface (emitted as InterfacePoly$Animal) ─────────────
    /**
     * The declared (static) type of the holder's {@code pet} field. Declares an
     * abstract {@code speak()} every implementation must override, plus a DEFAULT
     * method {@code defaultGreet()} that exists ONLY on the interface (no Dog
     * override) -- the probe target for the interface-default-method limitation.
     */
    public interface Animal
    {
        /** Abstract: every implementation overrides this. */
        String speak();

        /**
         * Interface DEFAULT method (Java 8+). NOT overridden by Dog, so it is
         * reachable only by walking the interface chain. vmhook's
         * object::get_method walks the SUPERCLASS chain (Dog -> Object) but not
         * the interface chain, so the native module treats reaching this as a
         * best-effort [INFO], never a failure.
         */
        default String defaultGreet()
        {
            return "interface-default-greet:" + this.speak();
        }
    }

    // ── The nested concrete implementation (emitted as InterfacePoly$Dog) ──
    /**
     * Concrete {@code Animal}. Overrides {@code speak()} (the virtual-dispatch
     * target), carries Dog-specific fields the native side reads, and adds a
     * Dog-only method {@code fetch()}.
     */
    public static final class Dog implements Animal
    {
        public String name;
        public int    age;
        public String breed;

        public Dog(final String name, final int age, final String breed)
        {
            this.name  = name;
            this.age   = age;
            this.breed = breed;
        }

        /** Overrides Animal.speak(); the string the native side matches on. */
        @Override
        public String speak()
        {
            return this.name + " says woof";
        }

        /** Dog-only method (not on the Animal interface). */
        public String fetch()
        {
            return this.name + " fetches the " + this.breed;
        }
    }

    // ── Deterministic constants the native side mirrors ────────────────────
    public static final String PET_NAME   = "Rex";
    public static final int    PET_AGE    = 5;
    public static final String PET_BREED  = "labrador";

    // ── The holder field: declared Animal, runtime Dog ─────────────────────
    /**
     * The headline shape: declared type is the interface {@code Animal}, the
     * runtime object is a concrete {@code Dog}. Eagerly initialised so the Dog
     * klass is loaded (and the OOP exists) by the time the native module runs --
     * {@code Main.loadFixtures()} only forName's the top-level fixture, so the
     * nested {@code Dog} would otherwise stay unloaded.
     */
    public Animal pet = new Dog(PET_NAME, PET_AGE, PET_BREED);

    /**
     * A SECOND field that holds the SAME object as {@code pet} (shared-ref
     * angle): proves reading the slot through either declared type yields the
     * same decoded OOP. Declared as the concrete type here purely to vary the
     * declared signature; the native side compares decoded identities, not types.
     */
    public Dog petAsDog = (Dog) this.pet;

    // ── Java-side witnesses (so native checks the JVM agrees) ──────────────
    /** True after the probe confirms, Java-side, that pet instanceof Dog and speak() contains "woof". */
    public static volatile boolean petIsDogSeen;

    /** The Dog's speak() result, published so native can compare byte-for-byte. */
    public static volatile String petSpeakSeen = "";

    /** System.identityHashCode of the pet object, so native identity checks are exact. */
    public static volatile int petIdentity;

    // ── Hook site ──────────────────────────────────────────────────────────
    /**
     * The native module hooks this; calling it through real bytecode dispatch is
     * what makes the interpreter hook fire. All the polymorphism reads in the
     * module happen from native code against the published SINGLETON, so the
     * detour body itself need do nothing but exist.
     */
    public int tick(final int nonce)
    {
        return nonce + 1;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return InterfacePoly.go && !InterfacePoly.done;
            }

            @Override
            public void run()
            {
                final InterfacePoly s = SINGLETON;

                // Java-side polymorphism observations: the runtime type IS a Dog,
                // and the overridden speak() reaches Dog's body ("...woof").
                final boolean isDog = (s.pet instanceof Dog);
                final String  spoke = s.pet.speak();
                InterfacePoly.petSpeakSeen = spoke;
                InterfacePoly.petIsDogSeen = isDog && spoke.contains("woof");
                InterfacePoly.petIdentity  = System.identityHashCode(s.pet);

                // Real bytecode dispatch -> native interpreter hook fires.
                s.tick(7);

                InterfacePoly.done = true;
            }
        });
    }

    /**
     * The single instance the native module wraps and drives. Created eagerly so
     * the native side can fetch it through a static field and so the published
     * identity matches exactly the OOP the module decodes from the pet slot.
     */
    public static final InterfacePoly SINGLETON = new InterfacePoly();
}
