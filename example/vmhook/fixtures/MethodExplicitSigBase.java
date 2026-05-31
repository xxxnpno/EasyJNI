package vmhook.fixtures;

/**
 * Superclass of {@link MethodExplicitSig}, used by the method_explicit_signature
 * test module to exercise the HIERARCHY WALK inside both explicit-signature
 * {@code get_method(name, signature)} overloads (instance + static-by-type_index):
 * the two {@code base(...)} overloads live here, NOT on the leaf class, so a
 * lookup that matched only the leaf class's own _methods array would miss them.
 *
 * This is a package-private (non-public) top-level class on purpose: the harness
 * fixture loader ({@code Main.loadFixtures}) will {@code Class.forName} it because
 * it sits in vmhook/fixtures, which is harmless — it registers no probe and has
 * no static state.  It must only compile and be a valid superclass.
 *
 * Java 8 syntax only.
 */
class MethodExplicitSigBase
{
    /** base(I)I executed; observable proof the inherited overload ran. */
    public static volatile int baseIntSeen;
    /** base(II)I executed; observable proof. */
    public static volatile int baseIntIntSeen;

    /** base(I)I — distinct descriptor from base(II)I. */
    public int base(final int a)
    {
        baseIntSeen = a;
        return a + 7;
    }

    /** base(II)I — same name, different descriptor. */
    public int base(final int a, final int b)
    {
        baseIntIntSeen = a * 1000 + b;
        return a - b;
    }
}
