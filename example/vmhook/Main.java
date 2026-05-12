package vmhook;

import java.lang.management.ManagementFactory;
import java.util.function.BooleanSupplier;

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
        Class.forName("vmhook.B");

        // we let vmhook.dll do its things in the jvm
        // vmhook has to get_field("stopJVM")->set(true) to stop the JVM
        while(!stopJVM)
        {
            runProbe(() -> Example.hookProbeRequested && !Example.hookProbeDone,
                () -> Example.instance.nonStaticCallMe(77),
                () -> Example.hookProbeDone = true);

            runProbe(() -> Example.forceReturnProbeRequested && !Example.forceReturnProbeDone,
                () -> Example.forceReturnProbeValue = Example.instance.nonStaticReturnMe(77),
                () -> Example.forceReturnProbeDone = true);

            runProbe(() -> Example.cancelProbeRequested && !Example.cancelProbeDone,
                () -> Example.instance.nonStaticCancelMe(9),
                () -> Example.cancelProbeDone = true);

            runProbe(() -> Example.staticForceReturnProbeRequested && !Example.staticForceReturnProbeDone,
                () -> Example.staticForceReturnProbeValue = Example.staticReturnMe(77),
                () -> Example.staticForceReturnProbeDone = true);

            runProbe(() -> Example.makeUniqueProbeRequested && !Example.makeUniqueProbeDone,
                () ->
                {
                    final Object tlabRefill = new Object();
                    Example.instance.nonStaticCallMe(88);
                    if (tlabRefill == null)
                    {
                        throw new IllegalStateException("unreachable");
                    }
                },
                () -> Example.makeUniqueProbeDone = true);

            runProbe(() -> Example.listProbeRequested && !Example.listProbeDone,
                () -> Example.listProbeSize = Example.instance.listOfAs.size(),
                () -> Example.listProbeDone = true);

            runProbe(() -> Example.polyProbeRequested && !Example.polyProbeDone,
                () ->
                {
                    Example.polyProbeInheritedField = (Example.instance.bInstance.protectedInt == 1337);
                    Example.polyProbeInheritedMethod = (Example.instance.bInstance.protectedAdd(3) == 1340);
                    Example.polyProbeOwnField = (Example.instance.bInstance.bInt == 42);
                },
                () -> Example.polyProbeDone = true);

            runProbe(() -> Example.methodCallReturnProbeRequested && !Example.methodCallReturnProbeDone,
                () -> Example.instance.nonStaticCallMe(99),
                () -> Example.methodCallReturnProbeDone = true);

            Thread.sleep(1);
        }

        // Phase 3: Java-side verification that C++ setter effects are visible.
        System.out.println("[INFO] Running Java-side setter verification...");

        if (!Example.verifySetterEffects())
        {
            System.out.println("[JAVA FAIL] One or more setter verifications failed.");
            System.exit(1);
        }

        System.out.println("[JAVA PASS] All setter verifications passed.");
    }

    private static void runProbe(final BooleanSupplier shouldRun, final Runnable action, final Runnable markDone)
    {
        if (!shouldRun.getAsBoolean())
        {
            return;
        }

        action.run();
        markDone.run();
    }
}
