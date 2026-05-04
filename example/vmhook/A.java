package vmhook;

public class A
{
    private String string = "test";

    // we're testing vmhook::make_unique
    A(final String a)
    {
        this.string = a;

        this.counter++;
    }

    public static int counter = 0;

    private int val = 0;
}