// Standalone (no-JVM) tests for vmhook's diagnostic logging layer:
// detail::format_log formatting, the VMHOOK_LOG macro, the *_tag literals,
// detail::emit_log_line null/edge tolerance, and the never-throw guarantee.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

int main()
{
    // ---------------------------------------------------------------------
    // error_tag / warning_tag / info_tag — stable, non-empty string literals.
    // The header declares them as `inline constexpr std::string_view`.
    // ---------------------------------------------------------------------
    check("error_tag_non_empty", !vmhook::error_tag.empty());
    check("error_tag_exact_value", vmhook::error_tag == "[VMHook ERROR]");
    check("warning_tag_non_empty", !vmhook::warning_tag.empty());
    check("warning_tag_exact_value", vmhook::warning_tag == "[VMHook WARNING]");
    check("info_tag_non_empty", !vmhook::info_tag.empty());
    check("info_tag_exact_value", vmhook::info_tag == "[VMHook INFO]");
    // Tags are distinct from one another.
    check("tags_distinct",
        vmhook::error_tag != vmhook::warning_tag &&
        vmhook::warning_tag != vmhook::info_tag &&
        vmhook::error_tag != vmhook::info_tag);
    // It is a std::string_view (its element type is char), so this comparison
    // is meaningful and the literal carries the documented bracket prefix.
    check("error_tag_starts_with_bracket",
        !vmhook::error_tag.empty() && vmhook::error_tag.front() == '[');

    // ---------------------------------------------------------------------
    // detail::format_log positive path.  These produce a formatted string only
    // when the toolchain ships std::format (VMHOOK_HAS_STD_FORMAT); otherwise
    // format_log returns the format string verbatim, so the expectations are
    // gated on that macro to stay correct on every supported compiler.
    // ---------------------------------------------------------------------
#if VMHOOK_HAS_STD_FORMAT
    // int argument.
    check("format_int",
        vmhook::detail::format_log("{}", 42) == "42");
    // string argument (std::string).
    check("format_string",
        vmhook::detail::format_log("{}", std::string{ "hello" }) == "hello");
    // string-literal argument.
    check("format_cstring",
        vmhook::detail::format_log("v={}", "x") == "v=x");
    // pointer argument — std::format renders pointers as 0x.. ; a null pointer
    // is well defined.  We only assert the value is non-empty and that a
    // non-null pointer yields a different rendering than nullptr.
    {
        int local{ 0 };
        const std::string p_null{ vmhook::detail::format_log("{}", static_cast<void*>(nullptr)) };
        const std::string p_some{ vmhook::detail::format_log("{}", static_cast<void*>(&local)) };
        check("format_pointer_non_empty", !p_null.empty() && !p_some.empty());
        check("format_pointer_distinct", p_null != p_some);
    }
    // float / double argument — exact integral-valued double is portable.
    check("format_double",
        vmhook::detail::format_log("{}", 1.5) == "1.5");
    // Multiple heterogeneous args in one call (int, string, pointer-ish).
    check("format_multi_args",
        vmhook::detail::format_log("{}-{}-{}", 7, std::string{ "ab" }, 3) == "7-ab-3");
    // Positional / reordered indices.
    check("format_indexed",
        vmhook::detail::format_log("{1}{0}", 1, 2) == "21");
    // Width / fill spec is honoured.
    check("format_width_spec",
        vmhook::detail::format_log("{:03}", 5) == "005");
    // A format call that embeds error_tag, exactly as the library does
    // internally (e.g. VMHOOK_LOG("{} ...", vmhook::error_tag, ...)).
    check("format_with_error_tag",
        vmhook::detail::format_log("{} boom", vmhook::error_tag) == "[VMHook ERROR] boom");
    // A plain format string with no replacement fields round-trips unchanged.
    check("format_no_fields",
        vmhook::detail::format_log("literal text") == "literal text");
    // Escaped braces collapse to single braces.
    check("format_escaped_braces",
        vmhook::detail::format_log("{{{}}}", 9) == "{9}");
#else
    // Without std::format, format_log ignores specifiers and returns the
    // format string verbatim (documented fallback behaviour).
    check("format_fallback_verbatim",
        vmhook::detail::format_log("{}", 42) == "{}");
    check("format_fallback_plain",
        vmhook::detail::format_log("literal text") == "literal text");
    check("format_fallback_with_tag",
        vmhook::detail::format_log("{} boom", vmhook::error_tag) == "{} boom");
#endif

    // ---------------------------------------------------------------------
    // format_log return type is std::string, and a zero-argument call (just a
    // format string) is always valid and yields exactly that string.
    // ---------------------------------------------------------------------
    {
        auto produced = vmhook::detail::format_log("plain");
        check("format_returns_std_string",
            std::is_same_v<decltype(produced), std::string>);
        check("format_zero_args_is_fmt", produced == "plain");
    }

    // Empty format string yields an empty result on every toolchain.
    check("format_empty_fmt_empty_result",
        vmhook::detail::format_log("").empty());

    // ---------------------------------------------------------------------
    // Never-throw guarantee.  format_log is implemented with an internal
    // try/catch (NOTE: it is *not* marked `noexcept` in the header, so we do
    // not assert noexcept(...) here — that would be a false claim — we assert
    // the observable behaviour instead): a malformed-but-syntactically-valid
    // std::vformat call (here, a replacement field with no matching argument)
    // throws std::format_error internally, which format_log swallows and
    // returns the raw format string instead of propagating.
    // ---------------------------------------------------------------------
    {
        bool threw{ false };
        std::string result;
        try
        {
            // "{}" with no argument: std::vformat throws std::format_error at
            // runtime; format_log must catch it and hand back the fmt string.
            result = vmhook::detail::format_log("{}");
        }
        catch (...)
        {
            threw = true;
        }
        check("format_mismatched_field_does_not_throw", !threw);
#if VMHOOK_HAS_STD_FORMAT
        // On the catch path the verbatim format string is returned.
        check("format_mismatched_returns_fmt", result == "{}");
#else
        check("format_mismatched_returns_fmt", result == "{}");
#endif
    }
    {
        // A second malformed case: too few args for two fields.
        bool threw{ false };
        try
        {
            (void)vmhook::detail::format_log("{} {}", 1);
        }
        catch (...)
        {
            threw = true;
        }
        check("format_too_few_args_does_not_throw", !threw);
    }

    // ---------------------------------------------------------------------
    // detail::emit_log_line is noexcept and must tolerate any std::string,
    // including empty and very long inputs, without crashing or throwing.
    // (Output goes to std::cout / VMHOOK_LOG_FILE; we only assert it returns
    // normally.)
    // ---------------------------------------------------------------------
    check("emit_log_line_is_noexcept",
        noexcept(vmhook::detail::emit_log_line(std::string{})));
    {
        bool threw{ false };
        try
        {
            vmhook::detail::emit_log_line(std::string{});            // empty
        }
        catch (...) { threw = true; }
        check("emit_empty_string_ok", !threw);
    }
    {
        bool threw{ false };
        try
        {
            vmhook::detail::emit_log_line(std::string{ "a short line" });
        }
        catch (...) { threw = true; }
        check("emit_short_string_ok", !threw);
    }
    {
        bool threw{ false };
        try
        {
            const std::string long_line(64u * 1024u, 'x');          // 64 KiB
            vmhook::detail::emit_log_line(long_line);
        }
        catch (...) { threw = true; }
        check("emit_long_string_ok", !threw);
    }
    {
        bool threw{ false };
        try
        {
            // Embedded newlines / NUL bytes must not break emission.
            std::string weird{ "line1\nline2" };
            weird.push_back('\0');
            weird += "after-nul";
            vmhook::detail::emit_log_line(weird);
        }
        catch (...) { threw = true; }
        check("emit_embedded_control_chars_ok", !threw);
    }

    // ---------------------------------------------------------------------
    // VMHOOK_LOG must compile and run without crashing in whatever mode this
    // translation unit is built (no-op when VMHOOK_DEBUG_LOGS == 0, active
    // otherwise).  Exercise it with the int/string/pointer/float arg mix and
    // with the library's own error_tag, mirroring real internal usage.
    // ---------------------------------------------------------------------
    {
        bool threw{ false };
        try
        {
            int sentinel{ 123 };
            VMHOOK_LOG("plain message");
            VMHOOK_LOG("{} int={}", vmhook::error_tag, 7);
            VMHOOK_LOG("{} str={} ptr={} flt={}",
                vmhook::warning_tag,
                std::string{ "s" },
                static_cast<void*>(&sentinel),
                2.25);
        }
        catch (...) { threw = true; }
        check("vmhook_log_macro_runs_without_throwing", !threw);
    }
    // VMHOOK_DEBUG_LOGS is always defined (to 0 or 1) by the header.
#if defined(VMHOOK_DEBUG_LOGS)
    check("vmhook_debug_logs_macro_defined",
        (VMHOOK_DEBUG_LOGS == 0) || (VMHOOK_DEBUG_LOGS == 1));
#else
    check("vmhook_debug_logs_macro_defined", false);
#endif

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
