package vmhook.fixtures;

import vmhook.Harness;

/**
 * Fixture for the {@code jni_local_ref_hygiene} feature: it proves that vmhook
 * does NOT leak JNI <em>local</em> references on the four code paths that create
 * them, when those paths are driven from a long-lived attached detour thread in
 * a tight loop well past HotSpot's default 16-entry local-reference table.
 *
 * <p>Each of the following vmhook operations internally allocates one (or more)
 * JNI local reference that vmhook is responsible for releasing via
 * {@code JNIEnv::DeleteLocalRef} (vmhook.hpp {@code jni_delete_local_ref}, JNI
 * table slot 23):</p>
 * <ul>
 *   <li>{@code method_proxy::call()} returning a {@code String} -&gt; the JNI
 *       fallback path ({@code call_jni}) obtains the returned {@code jstring}
 *       via {@code Call(Static)?ObjectMethodA}, decodes it, and must release the
 *       local ref;</li>
 *   <li>{@code method_proxy::call()} returning a non-String reference
 *       ({@code Object}/array) -&gt; same {@code CallObjectMethodA} local ref,
 *       released in the {@code 'L'/'['} arm;</li>
 *   <li>{@code method_proxy::call(java.lang.String arg)} -&gt; the String arg is
 *       marshalled through {@code NewStringUTF}, which returns a local ref that
 *       the arg-cleanup RAII must release;</li>
 *   <li>STATIC dispatch -&gt; resolves the declaring {@code jclass} via
 *       {@code FindClass} (a local ref); instance dispatch resolves it via
 *       {@code GetObjectClass} (also a local ref);</li>
 *   <li>{@code return_value::set_arg(index, String)} -&gt; injects a Java String
 *       argument; vmhook calls {@code NewStringUTF} (local ref) then
 *       {@code DeleteLocalRef} after extracting the OOP (vmhook.hpp
 *       {@code return_value::set_arg}, the documented v0.4.x leak fix).</li>
 * </ul>
 *
 * <p>If any of these refs were leaked, the local-ref table (default capacity 16)
 * would overflow within ~16 iterations; HotSpot then emits a
 * <em>"JNI local reference table overflow"</em> warning and later calls degrade
 * (a starved {@code CallObjectMethodA} returns null, so the decoded String comes
 * back empty / the returned wrapper becomes null, and an injected arg stops
 * reaching the body).  The native module therefore drives every path
 * <strong>100+ times</strong> and asserts the observable result stays correct on
 * every iteration -- the behavioural proof that the refs are released.  Crucially
 * the loops are BOUNDED so that even if a leak existed it would surface as the
 * benign table-overflow warning, never an access violation that takes the JVM
 * down.</p>
 *
 * <p>How the native module drives it: it hooks {@link #trigger()} (the only
 * context where {@code vmhook::hotspot::current_java_thread} is established,
 * which {@code call()} requires) and ALSO hooks {@link #inject(String)} (so the
 * detour can call {@code set_arg(0, String)} on a real String argument slot in a
 * loop).  The probe's {@code run()} calls {@code trigger()} once -- the detour
 * runs the call()/return loops -- then calls {@code inject(...)} in its OWN loop
 * so each dispatch fires the {@code inject} hook and exercises one
 * {@code set_arg(String)} on a fresh interpreter frame.  The body of
 * {@code inject} copies the (mutated) argument into observable static fields so
 * the native side can prove the injected String reached the body unstarved.</p>
 *
 * <p>Target Java 8 syntax only (no var / records / switch-expressions / lambdas
 * outside the anonymous Probe).  All strings are pure ASCII so javac decodes the
 * source identically on every CI host regardless of file encoding.</p>
 */
public final class JniLocalRef
{
    /** Native sets this true to request the probe action; cleared afterwards. */
    public static volatile boolean go;

    /** The probe action sets this true when it has run; native polls it. */
    public static volatile boolean done;

    /** Bumped every time {@link #trigger()} actually runs (hook liveness). */
    public static volatile int triggerCount;

    /** Bumped every time {@link #inject(String)} runs (set_arg loop liveness). */
    public static volatile int injectCount;

    // ── set_arg(String) loop observations ──────────────────────────────────
    // The native detour mutates inject()'s String argument via set_arg(0, ...)
    // before the body runs; the body records exactly what it observed so the
    // native side proves the injected value reached an unstarved local slot.

    /** The String value the LAST inject() body observed (post-mutation). */
    public static volatile String  injectSeen = "<unset>";

    /** Length of the last observed injected String (-1 if the body saw null). */
    public static volatile int     injectLenSeen = -1;

    /** True once at least one inject() body has run. */
    public static volatile boolean injectBodyRan;

    /** How many inject() bodies observed a non-null, non-empty injected value
     *  -- a starved local-ref table would make set_arg silently inject "" (or
     *  fail), so this must equal injectCount after the loop. */
    public static volatile int     injectNonEmptyCount;

    // ════════════════════════════════════════════════════════════════════════
    //  Instance reference-returning methods (each call() allocates a JNI local
    //  ref for the returned object that vmhook must DeleteLocalRef).
    // ════════════════════════════════════════════════════════════════════════

    /** Stable String return used by the String-return leak loop. */
    public String makeString()
    {
        return "local-ref-stable";
    }

    /** A FRESH (non-interned) String each call, so the returned jstring is a
     *  brand-new heap object / local ref every iteration -- the harshest case
     *  for a leak (no constant-pool reuse to mask a missing release). Always
     *  evaluates to the same content "fresh-77" so the native side can assert
     *  stability. */
    public String freshString()
    {
        StringBuilder sb = new StringBuilder();
        sb.append("fresh");
        sb.append('-');
        sb.append(7 * 11);
        return sb.toString();
    }

    /** Echoes a native-supplied String argument back: the call marshals a
     *  NewStringUTF local ref (arg) AND returns a String local ref (result) --
     *  TWO local refs per call, so the table starves twice as fast if either
     *  release is missing. */
    public String echo(final String value)
    {
        return value;
    }

    /** A non-String reference return (the SINGLETON itself): exercises the
     *  CallObjectMethodA local-ref release on the 'L' arm without involving the
     *  String decoder. */
    public JniLocalRef self()
    {
        return this;
    }

    /** An array reference return ('[' descriptor): a fresh int[] each call,
     *  another CallObjectMethodA local ref to release. */
    public int[] makeArray()
    {
        return new int[] { 3, 1, 4, 1, 5 };
    }

    /** Instance method the native detour hooks; calling it on a real bytecode
     *  dispatch establishes current_java_thread so the detour's call() loops can
     *  dispatch.  The detour does ALL the call()/return leak-loop work here. */
    public void trigger()
    {
        triggerCount++;
    }

    /** The String-argument method whose argument the native detour mutates via
     *  set_arg(0, "..."): each dispatch fires the inject() hook, the detour
     *  injects a fresh Java String (NewStringUTF local ref + DeleteLocalRef),
     *  and this body records what it actually received. */
    public void inject(final String value)
    {
        injectCount++;
        injectSeen = value;
        injectLenSeen = (value == null) ? -1 : value.length();
        injectBodyRan = true;
        if (value != null && value.length() > 0)
        {
            injectNonEmptyCount++;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    //  Static reference-returning methods (the static branch resolves the
    //  declaring jclass via FindClass -- itself a JNI local ref to release).
    // ════════════════════════════════════════════════════════════════════════

    /** Stable String return for the static String-return leak loop. */
    public static String staticMakeString()
    {
        return "static-local-ref-stable";
    }

    /** Static non-String reference return (the SINGLETON): the static-dispatch
     *  CallStaticObjectMethodA local ref AND the FindClass jclass local ref. */
    public static JniLocalRef staticSelf()
    {
        return SINGLETON;
    }

    static
    {
        Harness.register(new Harness.Probe()
        {
            @Override
            public boolean pending()
            {
                return JniLocalRef.go && !JniLocalRef.done;
            }

            @Override
            public void run()
            {
                // (1) One real bytecode dispatch of trigger() -> fires the native
                //     interpreter hook; that detour runs every call()/return leak
                //     loop (String return, fresh String, echo, Object return,
                //     array return, static return) far past 16 iterations.
                SINGLETON.trigger();

                // (2) A loop of inject(...) dispatches.  Each call is a real
                //     bytecode dispatch so the inject() hook fires; the detour
                //     calls set_arg(0, <fresh String>) before each body runs.
                //     Driving this hundreds of times exercises the NewStringUTF +
                //     DeleteLocalRef path of set_arg well past the 16-slot table.
                for (int i = 0; i < INJECT_ITERATIONS; i++)
                {
                    // The literal here is irrelevant: the native hook overwrites
                    // slot 0 with its own fresh String before the body reads it.
                    SINGLETON.inject("original");
                }

                JniLocalRef.done = true;
            }
        });
    }

    /** Iterations of the set_arg(String) loop -- well over the 16-slot default
     *  local-ref table so a leak would overflow it many times over. */
    public static final int INJECT_ITERATIONS = 120;

    /**
     * The single instance the native module wraps and drives.  Created eagerly
     * so the detour reaches the instance methods on a stable OOP.
     */
    public static final JniLocalRef SINGLETON = new JniLocalRef();
}
