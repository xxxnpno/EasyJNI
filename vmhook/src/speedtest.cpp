// speedtest.cpp — pure-JNI vs vmhook microbench.
//
// Built only when CMake's find_package(JNI) succeeded.  vmhook.hpp itself
// stays jni-header-free; the JNI side lives here in its own translation
// unit and reuses the JNIEnv* that vmhook::hotspot::current_jni_env
// stores after attach_current_native_thread().
//
// The bench calls vmhook/Example.staticReturnMe(int) in a tight loop
// through three paths:
//   1. vmhook method_proxy::call() (the everyday API)
//   2. pure JNI CallStaticIntMethodA via jni.h
// and prints ns/call for each so the gap is visible.

#include <vmhook/vmhook.hpp>

#ifdef VMHOOK_BENCH_USE_JNI

#include <jni.h>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <variant>

namespace
{
    // Minimal wrapper used by vmhook on its side of the bench.  Static
    // methods don't need a live instance, just the registered class.
    class example_wrapper : public vmhook::object<example_wrapper>
    {
    public:
        explicit example_wrapper(vmhook::oop_t oop) noexcept
            : vmhook::object<example_wrapper>{ oop }
        {
        }
    };
}

extern "C" auto run_vmhook_vs_jni_speedtest() -> void
{
    using clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    // Make sure the calling thread is attached so current_jni_env is
    // populated.  ensure_current_java_thread() also calls
    // attach_current_native_thread() as a side effect.
    if (!vmhook::hotspot::ensure_current_java_thread())
    {
        std::fprintf(stdout, "[BENCH] cannot attach to JavaThread; speedtest skipped\n");
        std::fflush(stdout);
        return;
    }
    auto* const env_void{ vmhook::hotspot::current_jni_env };
    if (!env_void)
    {
        std::fprintf(stdout, "[BENCH] no JNIEnv; speedtest skipped\n");
        std::fflush(stdout);
        return;
    }
    JNIEnv* const env{ reinterpret_cast<JNIEnv*>(env_void) };

    // -------- vmhook side --------
    if (vmhook::type_to_class_map.find(std::type_index{ typeid(example_wrapper) })
        == vmhook::type_to_class_map.end())
    {
        if (!vmhook::register_class<example_wrapper>("vmhook/Example"))
        {
            std::fprintf(stdout, "[BENCH] vmhook: register_class failed\n");
            std::fflush(stdout);
            return;
        }
    }

    example_wrapper empty{ nullptr };
    auto method_opt{ empty.get_method("staticReturnMe") };
    if (!method_opt.has_value())
    {
        std::fprintf(stdout, "[BENCH] vmhook: get_method('staticReturnMe') failed\n");
        std::fflush(stdout);
        return;
    }

    // Quick sanity probe: call once and make sure the value comes
    // back correctly before timing the loop.  staticReturnMe(40)
    // returns 40 + 2 = 42.
    {
        const auto sanity{ method_opt->call(static_cast<std::int32_t>(40)) };
        const auto* const p{ std::get_if<std::int32_t>(&sanity.data) };
        if (!p || *p != 42)
        {
            std::fprintf(stdout,
                         "[BENCH] vmhook sanity check failed (variant index %zu, expected int32=42); "
                         "skipping speedtest\n",
                         sanity.data.index());
            std::fflush(stdout);
            return;
        }
    }

    constexpr std::size_t iterations{ 50'000 };

    // Warm-up so the interpreter and any cached lookups are settled.
    for (std::size_t i{ 0 }; i < 1024; ++i)
    {
        (void)method_opt->call(static_cast<std::int32_t>(i));
    }

    long long vmhook_acc{ 0 };
    const auto t_vmhook_begin{ clock::now() };
    for (std::size_t i{ 0 }; i < iterations; ++i)
    {
        const auto v{ method_opt->call(static_cast<std::int32_t>(i)) };
        if (const auto* const p{ std::get_if<std::int32_t>(&v.data) })
        {
            vmhook_acc += *p;
        }
    }
    const auto t_vmhook_end{ clock::now() };

    // -------- pure JNI side --------
    jclass cls{ env->FindClass("vmhook/Example") };
    if (!cls)
    {
        env->ExceptionClear();
        std::fprintf(stdout, "[BENCH] JNI: FindClass('vmhook/Example') failed\n");
        std::fflush(stdout);
        return;
    }
    jmethodID mid{ env->GetStaticMethodID(cls, "staticReturnMe", "(I)I") };
    if (!mid)
    {
        env->ExceptionClear();
        std::fprintf(stdout, "[BENCH] JNI: GetStaticMethodID failed\n");
        env->DeleteLocalRef(cls);
        std::fflush(stdout);
        return;
    }

    for (std::size_t i{ 0 }; i < 1024; ++i)
    {
        (void)env->CallStaticIntMethod(cls, mid, static_cast<jint>(i));
    }

    long long jni_acc{ 0 };
    const auto t_jni_begin{ clock::now() };
    for (std::size_t i{ 0 }; i < iterations; ++i)
    {
        jni_acc += env->CallStaticIntMethod(cls, mid, static_cast<jint>(i));
    }
    const auto t_jni_end{ clock::now() };

    env->DeleteLocalRef(cls);

    const long long vmhook_ns{ std::chrono::duration_cast<ns>(t_vmhook_end - t_vmhook_begin).count() };
    const long long jni_ns{    std::chrono::duration_cast<ns>(t_jni_end    - t_jni_begin   ).count() };

    std::fprintf(stdout,
                 "[BENCH] iterations: %zu (staticReturnMe(int) -> int)\n",
                 iterations);
    std::fprintf(stdout,
                 "[BENCH] vmhook : total %lld ns, per-call %.0f ns, acc=%lld\n",
                 vmhook_ns, double(vmhook_ns) / double(iterations), vmhook_acc);
    std::fprintf(stdout,
                 "[BENCH] JNI    : total %lld ns, per-call %.0f ns, acc=%lld\n",
                 jni_ns,    double(jni_ns)    / double(iterations), jni_acc);
    if (jni_ns > 0)
    {
        std::fprintf(stdout,
                     "[BENCH] vmhook / JNI ratio: %.2fx (lower is better for vmhook)\n",
                     double(vmhook_ns) / double(jni_ns));
    }
    std::fflush(stdout);
}

#else  // !VMHOOK_BENCH_USE_JNI

extern "C" auto run_vmhook_vs_jni_speedtest() -> void
{
    std::fprintf(stdout, "[BENCH] speedtest: jni.h not present at build time, skipped\n");
    std::fflush(stdout);
}

#endif
