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
        Class.forName("vmhook.B");

        // we let vmhook.dll do its things in the jvm
        // vmhook has to get_field("stopJVM")->set(true) to stop the JVM
        while(!stopJVM)
        {
            if (Example.hookProbeRequested && !Example.hookProbeDone)
            {
                Example.instance.nonStaticCallMe(77);
                Example.hookProbeDone = true;
            }

            if (Example.forceReturnProbeRequested && !Example.forceReturnProbeDone)
            {
                Example.forceReturnProbeValue = Example.instance.nonStaticReturnMe(77);
                Example.forceReturnProbeDone = true;
            }

            if (Example.cancelProbeRequested && !Example.cancelProbeDone)
            {
                Example.instance.nonStaticCancelMe(9);
                Example.cancelProbeDone = true;
            }

            if (Example.staticForceReturnProbeRequested && !Example.staticForceReturnProbeDone)
            {
                Example.staticForceReturnProbeValue = Example.staticReturnMe(77);
                Example.staticForceReturnProbeDone = true;
            }

            if (Example.makeUniqueProbeRequested && !Example.makeUniqueProbeDone)
            {
                final Object tlabRefill = new Object();
                Example.instance.nonStaticCallMe(88);
                if (tlabRefill == null)
                {
                    throw new IllegalStateException("unreachable");
                }
                Example.makeUniqueProbeDone = true;
            }

            if (Example.listProbeRequested && !Example.listProbeDone)
            {
                Example.listProbeSize = Example.instance.listOfAs.size();
                Example.listProbeDone = true;
            }

            if (Example.polyProbeRequested && !Example.polyProbeDone)
            {
                Example.polyProbeInheritedField = (Example.instance.bInstance.protectedInt == 1337);
                Example.polyProbeInheritedMethod = (Example.instance.bInstance.protectedAdd(3) == 1340);
                Example.polyProbeOwnField = (Example.instance.bInstance.bInt == 42);
                Example.polyProbeDone = true;
            }

            if (Example.methodCallReturnProbeRequested && !Example.methodCallReturnProbeDone)
            {
                // The C++ hook on nonStaticCallMe will call nonStaticReturnMe(5) inside
                // the detour and store 6 back through an atomic; we just trigger the call.
                Example.instance.nonStaticCallMe(99);
                Example.methodCallReturnProbeDone = true;
            }

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
}
