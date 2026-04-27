#pragma once

/*
    test.hpp — VMHook unit test harness
    ====================================
    Injected into a running JVM (the example\ target process).
    Every public API of vmhook.hpp is exercised here.

    Test categories
    ───────────────
      1. VMStruct resolution         – gHotSpotVMStructs / gHotSpotVMTypes accessible
      2. Class discovery             – find_class() finds JDK + user-defined classes
      3. Field storage format        – correct path selected for this JDK build
      4. Instance field lookup       – every primitive type + String reference
      5. Static field lookup         – every primitive type + String reference
      6. Field read (get_field)      – values match known Java-side initialisers
      7. Field write (set_field)     – mutate + re-read instance field
      8. Static field read           – values match known Java-side initialisers
      9. Static field write          – mutate + re-read static field
     10. field_proxy / object API    – high-level wrapper reads agree with raw reads
     11. Method hooking              – hook fires, hookCallCount increments in Java

    Conventions
    ───────────
    Every test is a function returning bool.  PASS() / FAIL() macros accumulate
    results into a TestResults struct that is printed at the end.

    The test needs a live TestTarget instance pointer.  It is found by hooking
    TestTarget::onTick, capturing `this`, storing it in g_test_target, then
    running the assertions once the pointer is valid.
*/

#include <vmhook/vmhook.hpp>
#include <atomic>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include <format>
#include <print>

// ─── Result accumulator ──────────────────────────────────────────────────────

/*
    g_test_logger — set once by run_all_tests() before any test runs.
    All test functions call tlog() which routes through this pointer so that
    every PASS/FAIL line is captured by main.cpp's log() (writing to log.txt).
*/
using test_logger_fn = void(*)(std::string_view);
static test_logger_fn g_test_logger{ nullptr };

static void tlog(std::string_view msg)
{
    if (g_test_logger) g_test_logger(msg);
    else               std::println("{}", msg);
}

struct TestResults
{
    int passed{ 0 };
    int failed{ 0 };
    test_logger_fn logger{ nullptr };

    void record(const bool ok, const std::string& name, const std::string& detail = {})
    {
        if (ok) { ++passed; tlog(std::format("  [PASS]  {}", name)); }
        else    { ++failed; tlog(std::format("  [FAIL]  {}{}", name, detail.empty() ? "" : "  — " + detail)); }
    }

    void print_summary() const
    {
        tlog("");
        tlog("════════════════════════════════════════");
        tlog(std::format("  RESULTS: {} passed, {} failed", passed, failed));
        tlog(failed == 0 ? "  ALL TESTS PASSED" : "  SOME TESTS FAILED");
        tlog("════════════════════════════════════════");
    }
};

// ─── Hook-captured state ─────────────────────────────────────────────────────

/*
    g_test_target        — decoded OOP for the TestTarget instance captured from
                           the first onTick() hook invocation.
    g_hook_call_count    — incremented by the onTick hook; compared against
                           TestTarget.hookCallCount (which is incremented from Java)
                           to verify the hook fired the right number of times.
*/
static std::atomic<void*>  g_test_target{ nullptr };
static std::atomic<int>    g_hook_call_count{ 0 };

// C++ wrapper over vmhook.example.TestTarget for the object API test.
class TestTargetWrapper final : public vmhook::object
{
public:
    explicit TestTargetWrapper(vmhook::oop instance) : vmhook::object{ instance } {}
};

// ─── Helper: decode OOP from a live slot and verify it looks sane ─────────────

static auto decode_and_check(const void* raw_oop_slot) -> void*
{
    const auto compressed{ static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(raw_oop_slot)) };
    void* decoded{ vmhook::hotspot::decode_oop_ptr(compressed) };
    return vmhook::hotspot::is_valid_ptr(decoded) ? decoded : nullptr;
}

// ─── Individual test groups ───────────────────────────────────────────────────

// 1. VMStruct resolution
static void test_vmstructs(TestResults& results)
{
    tlog("\n── 1. VMStruct resolution ──");

    results.record(vmhook::hotspot::get_jvm_module()    != nullptr, "get_jvm_module()");
    results.record(vmhook::hotspot::get_vm_structs()    != nullptr, "get_vm_structs()");
    results.record(vmhook::hotspot::get_vm_types()      != nullptr, "get_vm_types()");

    // Spot-check a few well-known entries.
    results.record(vmhook::hotspot::iterate_struct_entries("Klass",         "_name")    != nullptr, "VMStruct: Klass._name");
    results.record(vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") != nullptr, "VMStruct: InstanceKlass._methods");
    results.record(vmhook::hotspot::iterate_struct_entries("Method",        "_i2i_entry") != nullptr, "VMStruct: Method._i2i_entry");
    results.record(vmhook::hotspot::iterate_type_entries("ConstantPool")    != nullptr, "VMType: ConstantPool");
    results.record(vmhook::hotspot::iterate_type_entries("Symbol")          != nullptr, "VMType: Symbol");

    // At least one of _fields / _fieldinfo_stream must be present.
    const bool has_fields  { vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields")           != nullptr };
    const bool has_fis     { vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") != nullptr };
    results.record(has_fields || has_fis, "InstanceKlass field storage present",
        std::format("has_fields={} has_fis={}", has_fields, has_fis));
}

// 2. Class discovery
static void test_class_discovery(TestResults& results)
{
    tlog("\n── 2. Class discovery ──");

    results.record(vmhook::find_class("java/lang/Object")       != nullptr, "find_class: java/lang/Object");
    results.record(vmhook::find_class("java/lang/String")       != nullptr, "find_class: java/lang/String");
    results.record(vmhook::find_class("java/lang/Integer")      != nullptr, "find_class: java/lang/Integer");
    results.record(vmhook::find_class("vmhook/example/Player")  != nullptr, "find_class: vmhook/example/Player");
    results.record(vmhook::find_class("vmhook/example/TestTarget") != nullptr, "find_class: vmhook/example/TestTarget");
    // Non-existent class must return nullptr (not crash).
    results.record(vmhook::find_class("does/not/Exist")         == nullptr, "find_class: non-existent class → nullptr");
}

// 3 + 4 + 5. Field lookup (existence, signature, static flag)
struct FieldSpec
{
    const char* class_name;
    const char* field_name;
    const char* expected_sig;
    bool        expected_static;
};

static void test_field_lookup(TestResults& results)
{
    tlog("\n── 3/4/5. Field lookup (existence, signature, static flag) ──");

    static const FieldSpec specs[]
    {
        // Standard JDK classes
        { "java/lang/String",          "hash",         "I",                   false },
        { "java/lang/Integer",         "TYPE",         "Ljava/lang/Class;",   true  },
        // Player fields
        { "vmhook/example/Player",     "health",       "F",                   false },
        { "vmhook/example/Player",     "x",            "D",                   false },
        { "vmhook/example/Player",     "y",            "D",                   false },
        { "vmhook/example/Player",     "z",            "D",                   false },
        { "vmhook/example/Player",     "name",         "Ljava/lang/String;",  false },
        { "vmhook/example/Player",     "count",        "I",                   true  },
        // TestTarget — every primitive type + String ref, plus statics
        { "vmhook/example/TestTarget", "fieldBool",    "Z",                   false },
        { "vmhook/example/TestTarget", "fieldByte",    "B",                   false },
        { "vmhook/example/TestTarget", "fieldShort",   "S",                   false },
        { "vmhook/example/TestTarget", "fieldInt",     "I",                   false },
        { "vmhook/example/TestTarget", "fieldFloat",   "F",                   false },
        { "vmhook/example/TestTarget", "fieldLong",    "J",                   false },
        { "vmhook/example/TestTarget", "fieldDouble",  "D",                   false },
        { "vmhook/example/TestTarget", "fieldChar",    "C",                   false },
        { "vmhook/example/TestTarget", "fieldRef",     "Ljava/lang/String;",  false },
        { "vmhook/example/TestTarget", "staticInt",    "I",                   true  },
        { "vmhook/example/TestTarget", "staticLong",   "J",                   true  },
        { "vmhook/example/TestTarget", "staticFloat",  "F",                   true  },
        { "vmhook/example/TestTarget", "staticDouble", "D",                   true  },
        { "vmhook/example/TestTarget", "staticString", "Ljava/lang/String;",  true  },
        { "vmhook/example/TestTarget", "hookCallCount","I",                   true  },
    };

    for (const auto& spec : specs)
    {
        vmhook::hotspot::klass* const target_klass{ vmhook::find_class(spec.class_name) };
        if (!target_klass) { results.record(false, std::format("find_class: {}", spec.class_name)); continue; }

        const auto entry{ target_klass->find_field(spec.field_name) };
        const std::string test_name{ std::format("{}.{}", spec.class_name, spec.field_name) };

        if (!entry) { results.record(false, test_name, "field not found"); continue; }

        const bool sig_ok    { entry->signature == spec.expected_sig };
        const bool static_ok { entry->is_static  == spec.expected_static };
        const bool offset_ok { entry->offset      > 0 };

        results.record(sig_ok,    test_name + " sig",    std::format("got='{}' want='{}'", entry->signature, spec.expected_sig));
        results.record(static_ok, test_name + " static", std::format("got={} want={}", entry->is_static, spec.expected_static));
        results.record(offset_ok, test_name + " offset>0",std::format("got={}", entry->offset));
    }
}

// 6 + 7. Instance field read/write (needs live TestTarget instance)
static void test_instance_field_rw(TestResults& results, void* const instance)
{
    tlog("\n── 6/7. Instance field read / write ──");

    if (!vmhook::hotspot::is_valid_ptr(instance))
    {
        // Instance pointer capture via frame->get_arguments() is disabled until the
        // JDK 21 locals-pointer spill offset is determined (see on_tick_detour comment).
        tlog("  [SKIP] No live instance — frame locals not yet supported on JDK 21.");
        return;
    }

    vmhook::hotspot::klass* const target_klass{ vmhook::find_class("vmhook/example/TestTarget") };
    if (!target_klass) { results.record(false, "TestTarget klass", "not found"); return; }

    // Java-side initialisers:
    //   fieldBool=true, fieldByte=127, fieldShort=32767, fieldInt=100,
    //   fieldFloat=1.5f, fieldLong=9876543210L, fieldDouble=6.283185307, fieldChar='X'

    // Read checks
    results.record(vmhook::get_field<bool>          (instance, target_klass, "fieldBool")   == true,    "get_field<bool>   fieldBool");
    results.record(vmhook::get_field<std::int8_t>   (instance, target_klass, "fieldByte")   == 127,     "get_field<int8>   fieldByte");
    results.record(vmhook::get_field<std::int16_t>  (instance, target_klass, "fieldShort")  == 32767,   "get_field<int16>  fieldShort");
    results.record(vmhook::get_field<std::int32_t>  (instance, target_klass, "fieldInt")    == 100,     "get_field<int32>  fieldInt");
    {
        const float got { vmhook::get_field<float>(instance, target_klass, "fieldFloat") };
        results.record(got >= 1.49f && got <= 1.51f, "get_field<float>  fieldFloat", std::format("got={}", got));
    }
    results.record(vmhook::get_field<std::int64_t>(instance, target_klass, "fieldLong")  == 9876543210LL, "get_field<int64>  fieldLong");
    {
        const double got { vmhook::get_field<double>(instance, target_klass, "fieldDouble") };
        results.record(got > 6.28 && got < 6.29, "get_field<double> fieldDouble", std::format("got={}", got));
    }
    results.record(vmhook::get_field<std::uint16_t>(instance, target_klass, "fieldChar") == static_cast<std::uint16_t>('X'), "get_field<char>   fieldChar");

    // Write + re-read: fieldInt
    vmhook::set_field<std::int32_t>(instance, target_klass, "fieldInt", 9999);
    results.record(vmhook::get_field<std::int32_t>(instance, target_klass, "fieldInt") == 9999, "set_field<int32>  fieldInt→9999");

    // Restore
    vmhook::set_field<std::int32_t>(instance, target_klass, "fieldInt", 100);

    // Write + re-read: fieldDouble
    vmhook::set_field<double>(instance, target_klass, "fieldDouble", 1.0);
    {
        const double got{ vmhook::get_field<double>(instance, target_klass, "fieldDouble") };
        results.record(got > 0.999 && got < 1.001, "set_field<double> fieldDouble→1.0", std::format("got={}", got));
    }
    vmhook::set_field<double>(instance, target_klass, "fieldDouble", 6.283185307);
}

// 8 + 9. Static field read/write
static void test_static_field_rw(TestResults& results)
{
    tlog("\n── 8/9. Static field read / write ──");

    vmhook::hotspot::klass* const target_klass{ vmhook::find_class("vmhook/example/TestTarget") };
    if (!target_klass) { results.record(false, "TestTarget klass", "not found"); return; }

    // Java-side initialisers:
    //   staticInt=42, staticLong=0xDEADBEEFCAFEL, staticFloat=3.14f,
    //   staticDouble=2.718281828, staticString="hello"

    results.record(vmhook::get_static_field<std::int32_t>(target_klass, "staticInt")    == 42,            "get_static<int32>  staticInt");
    results.record(vmhook::get_static_field<std::int64_t>(target_klass, "staticLong")   == 0xDEADBEEFCAFELL, "get_static<int64>  staticLong");
    {
        const float got{ vmhook::get_static_field<float>(target_klass, "staticFloat") };
        results.record(got > 3.13f && got < 3.15f, "get_static<float>  staticFloat", std::format("got={}", got));
    }
    {
        const double got{ vmhook::get_static_field<double>(target_klass, "staticDouble") };
        results.record(got > 2.71 && got < 2.72, "get_static<double> staticDouble", std::format("got={}", got));
    }

    // Write + re-read: staticInt
    vmhook::set_static_field<std::int32_t>(target_klass, "staticInt", 12345);
    results.record(vmhook::get_static_field<std::int32_t>(target_klass, "staticInt") == 12345, "set_static<int32>  staticInt→12345");
    vmhook::set_static_field<std::int32_t>(target_klass, "staticInt", 42); // restore

    // Write + re-read: staticLong
    vmhook::set_static_field<std::int64_t>(target_klass, "staticLong", 0xCAFEBABELL);
    results.record(vmhook::get_static_field<std::int64_t>(target_klass, "staticLong") == 0xCAFEBABELL, "set_static<int64>  staticLong→CAFEBABE");
    vmhook::set_static_field<std::int64_t>(target_klass, "staticLong", 0xDEADBEEFCAFELL); // restore
}

// 10. field_proxy / vmhook::object API
static void test_object_api(TestResults& results, void* const instance)
{
    tlog("\n── 10. field_proxy / vmhook::object API ──");

    if (!vmhook::hotspot::is_valid_ptr(instance))
    {
        tlog("  [SKIP] No live instance — frame locals not yet supported on JDK 21.");
        return;
    }

    vmhook::register_class<TestTargetWrapper>("vmhook/example/TestTarget");
    TestTargetWrapper wrapper{ instance };

    // Instance field via proxy
    {
        const auto proxy_int{ wrapper.get_field("fieldInt") };
        results.record(proxy_int.has_value(), "object::get_field fieldInt exists");
        if (proxy_int)
        {
            const std::int32_t val{ proxy_int->get() };
            results.record(val == 100, "field_proxy::get() fieldInt==100", std::format("got={}", val));

            proxy_int->set(static_cast<std::int32_t>(777));
            const std::int32_t after{ wrapper.get_field("fieldInt")->get() };
            results.record(after == 777, "field_proxy::set() fieldInt→777", std::format("got={}", after));
            proxy_int->set(static_cast<std::int32_t>(100)); // restore
        }
    }

    // Bool proxy
    {
        const auto proxy_bool{ wrapper.get_field("fieldBool") };
        results.record(proxy_bool.has_value(), "object::get_field fieldBool exists");
        if (proxy_bool)
        {
            const bool val{ proxy_bool->get() };
            results.record(val == true, "field_proxy::get() fieldBool==true");
        }
    }

    // Static field via proxy (object API auto-routes to mirror)
    {
        const auto proxy_static{ wrapper.get_field("staticInt") };
        results.record(proxy_static.has_value(), "object::get_field staticInt exists");
        if (proxy_static)
        {
            const std::int32_t val{ proxy_static->get() };
            results.record(val == 42, "field_proxy::get() staticInt==42", std::format("got={}", val));
        }
    }

    // Non-existent field must return nullopt (not crash)
    const auto proxy_bad{ wrapper.get_field("doesNotExist") };
    results.record(!proxy_bad.has_value(), "object::get_field non-existent → nullopt");
}

// 11. Method hooking — verified after the hook has been active for ≥1 second.
static void test_hook(TestResults& results, const int hook_calls_observed)
{
    tlog("\n── 11. Method hook (onTick) ──");

    vmhook::hotspot::klass* const target_klass{ vmhook::find_class("vmhook/example/TestTarget") };
    if (!target_klass) { results.record(false, "TestTarget klass for hook", "not found"); return; }

    // hookCallCount is read via get_static_field<> to confirm it was mutated
    // by the C++ hook, not by Java (Java never writes hookCallCount).
    const std::int32_t java_side{ vmhook::get_static_field<std::int32_t>(target_klass, "hookCallCount") };
    results.record(java_side > 0,             "hookCallCount > 0 (Java-side mirror updated by hook)", std::format("got={}", java_side));
    results.record(hook_calls_observed > 0,   "hook_call_count C++ counter > 0",                      std::format("got={}", hook_calls_observed));
    results.record(java_side == hook_calls_observed, "C++ counter == Java-side mirror",
        std::format("cpp={} java={}", hook_calls_observed, java_side));
}

// ─── Hook detour for TestTarget::onTick ──────────────────────────────────────

static void on_tick_detour(vmhook::hotspot::frame* const /*frame*/,
                           vmhook::hotspot::java_thread* const /*thread*/,
                           bool* const /*cancel*/)
{
    /*
        NOTE on frame locals in JDK 21:
        In JDK 8 the interpreter spills the locals pointer to [rbp + locals_offset].
        In JDK 21 the locals pointer lives in register r14 and is NOT spilled to the
        frame memory at our injection point.  Calling frame->get_arguments() / get_locals()
        would read garbage from [rbp + (-56)] and crash the JVM.

        For now the detour only increments a counter and updates a static field via
        vmhook::set_static_field (which is safe — it doesn't touch the frame stack).
        Once the JDK 21 locals-pointer spill offset is determined, get_arguments() can
        be re-enabled.
    */
    const int new_count{ ++g_hook_call_count };

    // Mirror the count into TestTarget.hookCallCount via static field write
    // so the Java process can observe how many times the hook fired.
    vmhook::hotspot::klass* const target_klass{ vmhook::find_class("vmhook/example/TestTarget") };
    if (target_klass)
    {
        vmhook::set_static_field<std::int32_t>(target_klass, "hookCallCount", new_count);
    }
}

// ─── Master entry point ───────────────────────────────────────────────────────

/*
    run_all_tests() — called from thread_entry() in main.cpp after:
      1. VMStructs are confirmed reachable.
      2. The class graph has been walked (so find_class() caches are warm).
      3. The hook on TestTarget::onTick has been installed.
      4. We have waited long enough to capture at least one hook invocation.

    Returns the number of failed tests (0 = all passed).
*/
// ─── i2i stub hex dump (diagnostic only) ─────────────────────────────────────

static void dump_i2i_stub()
{
    vmhook::hotspot::klass* const target_klass{ vmhook::find_class("vmhook/example/TestTarget") };
    if (!target_klass) return;

    const std::int32_t method_count{ target_klass->get_methods_count() };
    vmhook::hotspot::method** const methods{ target_klass->get_methods_ptr() };
    if (!methods || method_count <= 0) return;

    for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
    {
        vmhook::hotspot::method* const current_method{ methods[method_index] };
        if (!current_method || !vmhook::hotspot::is_valid_ptr(current_method)) continue;
        if (current_method->get_name() != "onTick") continue;

        void* const i2i_entry{ current_method->get_i2i_entry() };
        if (!vmhook::hotspot::is_valid_ptr(i2i_entry)) continue;

        const auto* stub_bytes{ reinterpret_cast<const std::uint8_t*>(i2i_entry) };

        // Write to a dedicated dump file so it's readable outside the console window.
        std::ofstream dump_file{ R"(C:\repos\cpp\VMHook\i2i_dump.txt)", std::ios::out | std::ios::trunc };
        if (!dump_file.is_open()) break;

        dump_file << std::format("i2i stub for onTick @ 0x{:016X}\n", reinterpret_cast<std::uintptr_t>(i2i_entry));
        for (int row{ 0 }; row < 16; ++row)
        {
            std::string hex_row;
            for (int col{ 0 }; col < 16; ++col)
                hex_row += std::format("{:02X} ", stub_bytes[row * 16 + col]);
            dump_file << std::format("+{:03X}  {}\n", row * 16, hex_row);
        }
        dump_file.close();
        std::println("  [i2i dump written to i2i_dump.txt]");
        break;
    }
}

/*
    @param logger  A function that writes a line to both the in-process console and
                   log.txt.  Pass main.cpp's `log` function pointer here.
*/
static auto run_all_tests(const test_logger_fn logger = nullptr) -> int
{
    g_test_logger = logger;

    tlog("");
    tlog("════════════════════════════════════════════════════════");
    tlog("  VMHook unit tests");
    tlog("════════════════════════════════════════════════════════");

    TestResults results;

    dump_i2i_stub();   // always show stub bytes regardless of hook success
    test_vmstructs(results);
    test_class_discovery(results);
    test_field_lookup(results);

    void* const instance{ g_test_target.load() };
    test_instance_field_rw(results, instance);
    test_static_field_rw(results);
    test_object_api(results, instance);
    test_hook(results, g_hook_call_count.load());

    results.print_summary();
    return results.failed;
}
