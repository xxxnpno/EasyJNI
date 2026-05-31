package vmhook.fixtures;

/**
 * Middle class of the field_inherited hierarchy (area: fields).
 *
 *     FieldInheritedBase   <-  FieldInheritedMid   <-  FieldInherited
 *
 * Deliberately declares NO field whose name collides with a Base or child
 * field.  Its single own instance field (midOwnInt) and own static (sMid) are
 * found by walking exactly ONE Klass::get_super() link up from a FieldInherited
 * instance — the depth-1 case between the depth-0 (own) and depth-2 (grandparent)
 * cases the native module pins.  By contributing no shadowing slot, Mid also
 * guarantees that the Base shadow slots are reachable only by the FULL two-link
 * walk, so a regression that stopped the walk early would surface here.
 *
 * Package-private top-level class (its public-named file matches the type); it
 * carries no Harness.Probe, so Main.loadFixtures() merely loads it.  Java 8.
 */
class FieldInheritedMid extends FieldInheritedBase
{
    static final int  MID_INT_INIT    = 0x00C0FFEE;
    static final int  MID_INT_RUNTIME = 0x77777777;
    static final int  STAT_MID_INIT    = 400;
    static final int  STAT_MID_RUNTIME = 0x7373;

    // Own instance field — inherited by the child via a single super link.
    public int midOwnInt = MID_INT_INIT;

    // Own static — inherited static reached by a single super link.
    public static int sMid = STAT_MID_INIT;
}
