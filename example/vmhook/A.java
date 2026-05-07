package vmhook;

public class A
{
    private String string = "test";
    public static int counter = 0;
    public int field = 0;
    private int val = 0;

    // protected fields to test hierarchy field access from B
    protected int protectedInt = 1337;
    protected String protectedString = "from_A";

    // default constructor used by Example (private A a = new A())
    A()
    {
        this.counter++;
    }

    // we're testing vmhook::make_unique
    A(final String a)
    {
        this.string = a;

        this.counter++;
    }

    // protected method to test hierarchy method access
    protected int protectedAdd(final int x) { return x + protectedInt; }
}
