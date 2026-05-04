package vmhook;

import java.lang.management.ManagementFactory;

public class Main
{
    // this boolean must be called by vmhook.dll
    static boolean stopJVM = false;

    public static void main(final String[] args) throws InterruptedException, ClassNotFoundException
    {
        final String runtimeName = ManagementFactory.getRuntimeMXBean().getName();

        final String pidString = runtimeName.contains("@") ? runtimeName.substring(0, runtimeName.indexOf('@')) : runtimeName;

        System.out.println("[INFO] JVM PID : " + pidString);

        System.out.println("[INFO] JVM : " + System.getProperty("java.vm.name") + " " + System.getProperty("java.version"));

        System.out.println("Waiting for vmhook.dll injection...");

        // to make sure Example class exists in the JVM
        Class.forName("vmhook.Example");

        // we let vmhook.dll do its things in the jvm
        // vmhook has to get_field("stopJVM")->set(true) to stop the JVM
        while(!stopJVM)
        {
            Thread.sleep(1);
        }
    }
}