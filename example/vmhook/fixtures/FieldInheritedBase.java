package vmhook.fixtures;

/**
 * Grandparent of the field_inherited hierarchy (area: fields).
 *
 *     FieldInheritedBase   <-  FieldInheritedMid   <-  FieldInherited
 *
 * This class declares the fields that the native field_inherited module reaches
 * by walking TWO Klass::get_super() links up from a FieldInherited instance:
 *   - protected / public / package-private / private INSTANCE fields, so the
 *     test proves vmhook::find_field ignores Java access control (it reads by
 *     raw offset) for every access level,
 *   - protected / public / private STATIC fields, exercising the same super
 *     walk on the java.lang.Class mirror,
 *   - shadow slots (shadowedInt / shadowedStr / sShadow) whose NAMES are
 *     re-declared on the child, so a base-typed read of a child object must see
 *     THESE (grandparent) slots while a child-typed read sees the child's.
 *
 * Package-private top-level class (its public-named file matches the type); it
 * carries no Harness.Probe, so Main.loadFixtures() merely loads it.  Java 8.
 */
class FieldInheritedBase
{
    // ---- Init + runtime constants the child's mutators write ---------------
    static final int PROT_INT_INIT    = 1337;        // canonical inherited value
    static final int PROT_INT_RUNTIME = 0xABCD;
    static final int PUB_INT_INIT     = 2674;
    static final int PUB_INT_RUNTIME  = 0x1234;
    static final int PKG_INT_INIT     = 0x0BADCAFE;
    static final int PRV_INT_INIT     = 0x0DEFACED;

    static final int  STAT_PROT_INIT    = 100;
    static final int  STAT_PROT_RUNTIME = 0x5151;
    static final int  STAT_PUB_INIT     = 200;
    static final int  STAT_PUB_RUNTIME  = 0x6262;
    static final int  STAT_PRV_INIT     = 300;

    // ---- INSTANCE fields at every access level (find_field ignores access) -
    protected int    protectedInt = PROT_INT_INIT;   // inherited PROTECTED
    public    int    publicInt    = PUB_INT_INIT;    // inherited PUBLIC
              int    packageInt   = PKG_INT_INIT;    // inherited PACKAGE-PRIVATE
    private   int    privateInt   = PRV_INT_INIT;    // inherited PRIVATE (base-only)

    // A representative non-int inherited primitive + an inherited reference,
    // so the super walk is covered for a wide primitive AND a compressed-OOP
    // reference decode (not just I-typed slots).
    protected long   baseLong     = 0x00BA5E_0000BA5EL;
    public    String baseStr      = "base-str";

    // ---- STATIC fields at every access level -------------------------------
    protected static int sProtected = STAT_PROT_INIT;
    public    static int sPublic    = STAT_PUB_INIT;
    private   static int sPrivate   = STAT_PRV_INIT;

    // ---- Shadow slots: the child RE-DECLARES these same names --------------
    public  int    shadowedInt = 1111;      // FieldInherited.BASE_SHADOW_INT
    public  String shadowedStr = "base";    // base copy of the shadowed String
    public  static int sShadow  = 555;      // FieldInherited.STATIC_SHADOW_BASE

    // Touch private members so javac does not warn them unused under -Werror-y
    // builds and so they are guaranteed present in the layout.
    int sumPrivates()
    {
        return this.privateInt + FieldInheritedBase.sPrivate;
    }
}
