package vmhook;

public class A
{
    private String string = "test";

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

    public static int counter = 0;

    // used by Example.useA to verify object access through vmhook
    public int field = 0;

    private int val = 0;
}