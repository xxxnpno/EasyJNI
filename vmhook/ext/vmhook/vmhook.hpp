#pragma once

/*
    VMHook - Single-header, header-only C++23 library for hooking Java methods
    and accessing fields at runtime via HotSpot VMStructs.

    Purpose:
        Provides a zero-JNI, zero-JVMTI API to intercept Java method calls
        and read/write instance/static fields by walking HotSpot's internal
        metadata tables (gHotSpotVMStructs / gHotSpotVMTypes) exported by jvm.dll.

    Dependencies:
        - C++23 compiler (MSVC recommended; uses <print>, std::format, concepts)
        - Windows (VirtualAlloc, VirtualProtect, ReadProcessMemory)
        - A running HotSpot JVM (Temurin, Corretto, Liberica, Microsoft, etc.)
        - jvm.dll loaded in the current process with gHotSpotVMStructs exported

    Thread safety:
        - read-only operations (find_class, find_field, get_field)
          are NOT thread-safe; the caller must synchronise access.
        - hook installation and shutdown_hooks() MUST be called from a single thread.
        - The internal caches (klass_lookup_cache, g_field_cache, type_to_class_map)
          use std::unordered_map which is NOT thread-safe for concurrent mutation.

    Template requirements:
        - get_field<T>, set_field<T>:
          T must be std::is_trivially_copyable_v<T> == true.
        - register_class<T>: T must be a complete class type with a virtual table
          (typeid(*this) is used to resolve the class name).
        - hook<T>: T must have been registered via register_class<T>() beforehand.

    Complexity guarantees:
        - find_class(class_name):       O(N*M) worst case, where N = number of loaded
                                        classloaders, M = average classes per classloader.
                                        O(1) after first lookup (cached in klass_lookup_cache).
        - find_field(klass, name):      O(F) where F = number of fields on the klass.
                                        O(1) after first lookup (cached in g_field_cache).
        - get_field<T> / set_field<T>:  O(1) after field is cached; O(F) on first call.
        - hook<T>(method_name, detour): O(M) where M = number of methods on the klass.
        - shutdown_hooks():             O(H) where H = number of active hooks.
        - klass.get_methods_count():    O(1).
        - klass.find_field(name):       O(F) where F = number of fields on the klass.
        - dictionary.find_klass(name):  O(B*E) where B = bucket count, E = entries per bucket.
        - class_loader_data_graph.find_klass(name): O(N*M) full graph walk.

    Exception safety:
        - All public API functions catch vmhook::exception internally and log the
          message via std::println before returning a safe default value (nullptr,
          std::nullopt, false, or T{}). Callers never see uncaught exceptions
          escaping the library boundary.
        - Internal helpers (iterate_struct_entries, iterate_type_entries, etc.)
          are noexcept and return nullptr on failure.
        - midi2i_hook constructor may throw vmhook::exception on allocation failure;
          the caller (hook<T>) catches it and returns false.
*/

#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <thread>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <type_traits>
#include <tuple>
#include <cstring>
#include <optional>
#include <variant>
#include <functional>
#include <limits>

#include <windows.h>

#ifndef VMHOOK_DEBUG_LOGS
#ifdef NDEBUG
#define VMHOOK_DEBUG_LOGS 0
#else
#define VMHOOK_DEBUG_LOGS 1
#endif
#endif

#if VMHOOK_DEBUG_LOGS
#define VMHOOK_LOG(...) ::std::println(__VA_ARGS__)
#else
#define VMHOOK_LOG(...) do { if constexpr (false) { ::std::println(__VA_ARGS__); } } while (false)
#endif

namespace vmhook
{
    /*
        @brief Log-prefix tags used in all diagnostic messages printed by this library.
        @details  These are prepended to every VMHOOK_LOG() call so that output from
        VMHook can be filtered in the host process console.
    */
    inline constexpr std::string_view error_tag{ "[VMHook ERROR]" };
    inline constexpr std::string_view warning_tag{ "[VMHook WARNING]" };
    inline constexpr std::string_view info_tag{ "[VMHook INFO]" };

    /*
        @brief Exception type thrown internally by VMHook to report unrecoverable errors.
        @details  All public API functions catch vmhook::exception and log the message
        via std::println before returning a safe default value, so callers never see
        uncaught exceptions escaping the library boundary.
    */
    class exception final : public std::exception
    {
    public:
        explicit exception(const std::string_view msg)
            : message{ msg }
        {

        }

        auto what() const noexcept
            -> const char* override
        {
            return this->message.c_str();
        }

    private:
        std::string message;
    };

    namespace hotspot
    {
        struct frame;

        struct return_slot
        {
            bool         cancel{ false };
            std::int64_t retval{ 0 };
        };
    }

    class object_base;

    /*
        @brief Handle passed as the first argument to every hook callback.
        @details
        Call set(value) to suppress the original Java method body and return a
        custom value to the caller.  Call cancel() to suppress without a return
        value (for void methods).  If neither is called the original method runs
        normally.

        Example:
            vmhook::hook<MyClass>("getScore",
                [](vmhook::return_value& ret, const std::unique_ptr<MyClass>& self) {
                    ret.set(std::int32_t{ 9999 });   // always return 9999
                });
    */
    class return_value
    {
    public:
        explicit return_value(vmhook::hotspot::return_slot* slot, vmhook::hotspot::frame* frame = nullptr) noexcept
            : slot_{ slot }, frame_{ frame }
        {

        }

        template<typename T>
        auto set(const T value) noexcept -> void
        {
            static_assert(sizeof(T) <= sizeof(std::int64_t), "return type too large for hook slot");
            slot_->cancel = true;
            slot_->retval = 0;
            std::memcpy(&slot_->retval, &value, sizeof(T));
        }

        auto cancel() noexcept -> void
        {
            slot_->cancel = true;
        }

        template<typename T>
        auto set_arg(std::int32_t index, T&& value) noexcept
            -> bool;

    private:
        vmhook::hotspot::return_slot* slot_{ nullptr };
        vmhook::hotspot::frame* frame_{ nullptr };
    };

    namespace hotspot
    {
        struct vm_struct_entry_t;
        struct vm_type_entry_t;
        struct symbol;
        struct constant_pool;
        struct const_method;
        struct method;
        struct klass;
        struct class_loader_data;
        struct class_loader_data_graph;
        struct dictionary;
        struct java_thread;
        struct frame;
        class  midi2i_hook;
        struct hooked_method;
        struct i2i_hook_data;
        struct field_entry_t;

        static auto decode_oop_pointer(std::uint32_t compressed) noexcept
            -> void*;

        static auto encode_oop_pointer(void* decoded) noexcept
            -> std::uint32_t;

        static auto ensure_current_java_thread() noexcept
            -> bool;
    }

    namespace detail
    {
        inline auto find_call_stub_entry() noexcept -> void*;
    }

    /*
        @brief Maps C++ wrapper types to their corresponding internal Java class names.
        @details
        Populated by vmhook::register_class() when a C++ wrapper type is associated
        with a Java class name.  Used by vmhook::hook() and other APIs that need to
        look up the Java class corresponding to a given C++ wrapper type at runtime.
        Keys   - std::type_index values derived from typeid() of the C++ wrapper type.
        Values - internal JVM class names using '/' separators (e.g. "java/lang/String").
        @see vmhook::register_class, vmhook::hook
    */
    inline std::unordered_map<std::type_index, std::string> type_to_class_map{};

    /*
        @brief Factory function type that creates a std::unique_ptr<T> from a raw OOP.
        @details
        Populated by vmhook::register_class() alongside type_to_class_map.
        Used by field_proxy::get_as<T>() and frame::get_arguments() to construct
        C++ wrapper objects from decoded Java object references.
        Keys   - internal JVM class names (e.g. "java/lang/String").
        Values - lambda functions: +[](void* oop) { return std::make_unique<T>(oop); }
    */
    using type_factory_function_t = std::unique_ptr<class object_base>(*)(void* instance);
    inline std::unordered_map<std::string, type_factory_function_t> g_type_factory_map{};

    template<class wrapper_type>
    static auto register_class(std::string_view class_name) noexcept
        -> bool;

    class object_base;
    template<typename derived = void> class object;
    class field_proxy;
    class collection;
    class list;

    inline auto read_java_string(void* string_oop)
        -> std::string;

    inline auto make_java_string(std::string_view value) noexcept
        -> void*;

    inline auto write_java_string(void* string_oop, std::string_view value) noexcept
        -> void;

    inline auto decode_array_oop(std::uint32_t compressed)
        -> void*;

    inline auto set_str_field(const field_proxy& field, std::string_view value) noexcept
        -> void;

    inline auto field_oop(const field_proxy& field) noexcept
        -> void*;

    inline auto set_bool_array(const field_proxy& field, const std::vector<bool>& values) noexcept
        -> void;

    inline auto set_str_array(const field_proxy& field, const std::vector<std::string>& values) noexcept
        -> void;

    template<typename element_type>
    inline auto set_prim_array(const field_proxy& field, const std::vector<element_type>& values) noexcept
        -> void;

    namespace detail
    {
        template<typename type>
        struct is_vector : std::false_type {};

        template<typename value_type, typename allocator_type>
        struct is_vector<std::vector<value_type, allocator_type>> : std::true_type
        {
            using value_type_t = value_type;
        };

        template<typename type>
        inline constexpr bool is_vector_v{ is_vector<std::remove_cvref_t<type>>::value };

        template<typename type>
        struct is_unique_ptr : std::false_type {};

        template<typename value_type, typename deleter_type>
        struct is_unique_ptr<std::unique_ptr<value_type, deleter_type>> : std::true_type
        {
            using value_type_t = value_type;
        };

        template<typename type>
        inline constexpr bool is_unique_ptr_v{ is_unique_ptr<std::remove_cvref_t<type>>::value };

    }

    // --- HotSpot internals ----------------------------------------------------

    namespace hotspot
    {
        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct vm_type_entry_t
        {
            const char* type_name;
            const char* superclass_name;
            std::int32_t  is_oop_type_type;
            std::int32_t  is_integer_type;
            std::int32_t  is_unsigned;
            std::uint64_t size;
        };

        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct vm_struct_entry_t
        {
            const char* type_name;
            const char* field_name;
            const char* type_string;
            std::int32_t  is_static;
            std::uint64_t offset;
            void* address;
        };

        /*
            @brief Returns the module handle for jvm.dll loaded in the current process.
        */
        inline auto get_jvm_module() noexcept
            -> HMODULE
        {
            static HMODULE module{ GetModuleHandleA("jvm.dll") };
            return module;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM type entries.
            @details
            Resolves gHotSpotVMTypes from jvm.dll via GetProcAddress on first call
            and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_types() noexcept
            -> vmhook::hotspot::vm_type_entry_t*
        {
            static FARPROC procedure_address{ GetProcAddress(vmhook::hotspot::get_jvm_module(), "gHotSpotVMTypes") };
            static vmhook::hotspot::vm_type_entry_t* pointer{ *reinterpret_cast<vmhook::hotspot::vm_type_entry_t**>(procedure_address) };
            return pointer;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM struct entries.
            @details
            Resolves gHotSpotVMStructs from jvm.dll via GetProcAddress on first call
            and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_structs() noexcept
            -> vmhook::hotspot::vm_struct_entry_t*
        {
            static FARPROC procedure_address{ GetProcAddress(vmhook::hotspot::get_jvm_module(), "gHotSpotVMStructs") };
            static vmhook::hotspot::vm_struct_entry_t* pointer{ *reinterpret_cast<vmhook::hotspot::vm_struct_entry_t**>(procedure_address) };
            return pointer;
        }

        /*
            @brief Searches the gHotSpotVMTypes array for a type entry by name.
        */
        static auto iterate_type_entries(const char* const type_name) noexcept
            -> vmhook::hotspot::vm_type_entry_t*
        {
            for (vmhook::hotspot::vm_type_entry_t* entry{ vmhook::hotspot::get_vm_types() }; entry && entry->type_name; ++entry)
            {
                if (!std::strcmp(entry->type_name, type_name))
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /*
            @brief Searches the gHotSpotVMStructs array for a field entry by type and field name.
        */
        static auto iterate_struct_entries(const char* const type_name, const char* const field_name) noexcept
            -> vmhook::hotspot::vm_struct_entry_t*
        {
            for (vmhook::hotspot::vm_struct_entry_t* entry{ vmhook::hotspot::get_vm_structs() }; entry && entry->type_name; ++entry)
            {
                if (!std::strcmp(entry->type_name, type_name) && !std::strcmp(entry->field_name, field_name))
                {
                    return entry;
                }
            }
            return nullptr;
        }

        /*
            @brief Checks whether a pointer refers to committed readable memory.
            @details
            Extends is_valid_pointer with a VirtualQuery call to verify that the memory
            region containing pointer is actually committed and readable.
        */
        static auto is_readable_pointer(const void* const pointer) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };

            if (addr <= 0xFFFF || addr >= 0x00007FFFFFFFFFFF || (addr & 0x7) != 0)
            {
                return false;
            }

            MEMORY_BASIC_INFORMATION memory_basic_info{};
            if (VirtualQuery(pointer, &memory_basic_info, sizeof(memory_basic_info)) == 0)
            {
                return false;
            }

            return memory_basic_info.State == MEM_COMMIT && (memory_basic_info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY)) != 0 && (memory_basic_info.Protect & PAGE_GUARD) == 0;
        }

        /*
            @brief Checks whether a pointer is likely valid for dereferencing.
            @details
            Filters out null pointers, low sentinel values used by HotSpot to mark
            the end of linked lists, and kernel-space addresses above 0x00007FFFFFFFFFFF.
        */
        inline static auto is_valid_pointer(const void* const pointer) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };
            return addr > 0xFFFF && addr < 0x00007FFFFFFFFFFF;
        }

        /*
            @brief Removes GC tag bits from a HotSpot pointer to recover the real address.
            @details
            Masking with 0x00007FFFFFFFFFFF strips high GC tag bits and recovers
            the underlying canonical user-space address.
        */
        inline static auto untag_pointer(const void* const pointer) noexcept
            -> const void*
        {
            return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(pointer) & 0x00007FFFFFFFFFFF);
        }

        /*
            @brief Reads a 32-bit pointer field from a JVM structure and zero-extends it.
        */
        template<typename structure_type>
        inline static auto read_pointer(const void* const base, const std::uint64_t offset) noexcept
            -> structure_type*
        {
            const std::uint32_t raw{ *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(base) + offset) };
            return reinterpret_cast<structure_type*>(static_cast<std::uintptr_t>(raw));
        }

        /*
            @brief Safely reads a pointer value from a memory address using ReadProcessMemory.
            @details
            Uses ReadProcessMemory to safely dereference a pointer without risking an
            access violation. Pre-checks filter out null, low, non-canonical, and unaligned
            addresses before calling ReadProcessMemory.
        */
        static auto safe_read_pointer(const void* const pointer) noexcept
            -> const void*
        {
            if (!pointer)
            {
                return nullptr;
            }

            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(pointer) };

            if (addr <= 0xFFFF || addr >= 0x00007FFFFFFFFFFF || (addr & 0x7) != 0)
            {
                return nullptr;
            }

            const void* result{ nullptr };
            SIZE_T bytes_read{ 0 };

            if (!ReadProcessMemory(GetCurrentProcess(), pointer, &result, sizeof(result), &bytes_read) || bytes_read != sizeof(result))
            {
                return nullptr;
            }

            return result;
        }

        /*
            @brief Represents a HotSpot internal Symbol object.
            @details
            Symbols are interned strings used throughout the JVM to represent class names,
            method names, field names, and type signatures. The layout is resolved at
            runtime via gHotSpotVMStructs using the offsets of Symbol._length and Symbol._body.
        */
        struct symbol
        {
            /*
                @brief Converts this HotSpot Symbol to a std::string.
                @return A std::string containing a copy of the symbol's character data,
                        or an empty string on failure.
            */
            auto to_string() const
                -> std::string
            {
                static const vmhook::hotspot::vm_struct_entry_t* const length_entry{ vmhook::hotspot::iterate_struct_entries("Symbol", "_length") };
                static const vmhook::hotspot::vm_struct_entry_t* const body_entry{ vmhook::hotspot::iterate_struct_entries("Symbol", "_body") };

                try
                {
                    if (!length_entry)
                    {
                        throw vmhook::exception{ "Failed to find Symbol._length entry." };
                    }

                    if (!body_entry)
                    {
                        throw vmhook::exception{ "Failed to find Symbol._body entry." };
                    }

                    if (!vmhook::hotspot::safe_read_pointer(this))
                    {
                        return std::string{};
                    }

                    const std::uint16_t length{ *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(this) + length_entry->offset) };
                    const char* const body{ reinterpret_cast<const char*>(reinterpret_cast<const std::uint8_t*>(this) + body_entry->offset) };

                    if (!vmhook::hotspot::is_valid_pointer(body) || length == 0 || length > 0x1000)
                    {
                        return std::string{};
                    }

                    return std::string{ body, length };
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} symbol.to_string() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }
        };

        /*
            @brief Represents a HotSpot internal ConstantPool object.
            @details
            The constant pool holds all constants referenced by a Java class.
            The layout is resolved at runtime via gHotSpotVMStructs using the size
            of the ConstantPool type to locate the base of the pool entries array.
        */
        struct constant_pool
        {
            /*
                @brief Returns a pointer to the base of the constant pool entries array.
                @details
                ConstantPool entries are stored immediately after the fixed-size ConstantPool
                header in memory (no separate heap allocation):
                  [ ConstantPool header (size from gHotSpotVMTypes) ][ entry[0] ][ entry[1] ] ...
                Entry indices are 1-based: index 0 is unused, valid indices start at 1.
                Each entry is pointer-sized (8 bytes on x64): it may hold a Symbol*, a primitive
                constant, or other data depending on the tag byte in _tags.
                The size of the header is read at runtime from gHotSpotVMTypes so this works
                across all JDK versions regardless of header size changes.
            */
            auto get_base() const
                -> void**
            {
                static const vmhook::hotspot::vm_type_entry_t* const entry{ vmhook::hotspot::iterate_type_entries("ConstantPool") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstantPool entry." };
                    }

                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::constant_pool*>(this)) + entry->size);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} constant_pool.get_base() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot internal ConstMethod object.
            @details
            ConstMethod holds the immutable metadata of a Java method, including its name,
            signature, and a reference to the owning class constant pool.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct const_method
        {
            /*
                @brief Returns a pointer to the constant pool of the owning class.
            */
            auto get_constants() const
                -> vmhook::hotspot::constant_pool*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_constants") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._constants entry." };
                    }

                    return *reinterpret_cast<constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_constants() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the name of this method.
            */
            auto get_name() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_name_index") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._name_index entry." };
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset) };
                    return reinterpret_cast<vmhook::hotspot::symbol*>(get_constants()->get_base()[index]);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_name() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the signature of this method.
            */
            auto get_signature() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ConstMethod", "_signature_index") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ConstMethod._signature_index entry." };
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::const_method*>(this)) + entry->offset) };
                    return reinterpret_cast<vmhook::hotspot::symbol*>(get_constants()->get_base()[index]);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} const_method.get_signature() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot internal Method object.
            @details
            The Method object is the JVM's internal representation of a Java method.
            It holds all runtime metadata needed to invoke, compile, and profile a method,
            including its entry points, access flags, compilation flags, and a pointer
            to its immutable ConstMethod.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct method
        {
            /*
                @brief Returns the interpreter-to-interpreter entry point of this method.
                @details
                The i2i entry is the native code stub invoked when an interpreted method
                calls another interpreted method. It is used as the hook location target
                in midi2i_hook.
            */
            auto get_i2i_entry() const
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_i2i_entry") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._i2i_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_i2i_entry() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the from-interpreted entry point of this method.
            */
            auto get_from_interpreted_entry() const
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._from_interpreted_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_from_interpreted_entry() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the access flags of this method.
                @details
                Access flags encode Java visibility modifiers (public, private, static, etc.)
                as well as JVM-internal flags such as NO_COMPILE.
            */
            auto get_access_flags() const
                -> std::uint32_t*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_access_flags") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._access_flags entry." };
                    }

                    return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_access_flags() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the internal method flags of this method.
                @details
                These flags encode HotSpot-internal method properties such as _dont_inline.
            */
            auto get_flags() const
                -> std::uint16_t*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_flags") };

                if (!entry)
                {
                    return nullptr;
                }

                return reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Returns a pointer to the ConstMethod of this method.
            */
            auto get_const_method() const
                -> vmhook::hotspot::const_method*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_constMethod") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Method._constMethod entry." };
                    }

                    return *reinterpret_cast<vmhook::hotspot::const_method**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_const_method() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the name of this method as a std::string.
            */
            auto get_name() const
                -> std::string
            {
                const vmhook::hotspot::const_method* const const_method_pointer{ this->get_const_method() };

                try
                {
                    if (!const_method_pointer)
                    {
                        throw vmhook::exception{ "ConstMethod is nullptr." };
                    }

                    const vmhook::hotspot::symbol* const symbol_pointer{ const_method_pointer->get_name() };
                    if (!symbol_pointer)
                    {
                        throw vmhook::exception{ "Symbol is nullptr." };
                    }

                    return symbol_pointer->to_string();
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_name() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the signature of this method as a std::string.
            */
            auto get_signature() const
                -> std::string
            {
                const vmhook::hotspot::const_method* const const_method_pointer{ this->get_const_method() };

                try
                {
                    if (!const_method_pointer)
                    {
                        throw vmhook::exception{ "ConstMethod is nullptr." };
                    }

                    const vmhook::hotspot::symbol* const symbol_pointer{ const_method_pointer->get_signature() };
                    if (!symbol_pointer)
                    {
                        throw vmhook::exception{ "Symbol is nullptr." };
                    }

                    return symbol_pointer->to_string();
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} method.get_signature() {}", vmhook::error_tag, exception.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the current nmethod pointer (_code field).
                @details Non-null when the method has been JIT-compiled; null when interpreted.
                         Writing null forces HotSpot dispatch to treat the method as uncompiled
                         without freeing the compiled code.
            */
            auto get_code() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_code") };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            auto set_code(void* const code) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_code") };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = code;
            }

            /*
                @brief Overwrites _from_interpreted_entry.
                @details Reset to _i2i_entry during deoptimisation so interpreted callers
                         route through the interpreter entry stub (which we have patched).
            */
            auto set_from_interpreted_entry(void* const entry_point) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = entry_point;
            }

            /*
                @brief Returns the from-compiled entry point.
                @details The VMStruct field name changed across JDK versions:
                         JDK ≤ 20  → "_from_compiled_code_entry_point"
                         JDK 21+   → "_from_compiled_entry"
                         Both names are tried at first call; the result is cached.
            */
            auto get_from_compiled_entry() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []() noexcept
                    -> const vmhook::hotspot::vm_struct_entry_t*
                        {
                            const vmhook::hotspot::vm_struct_entry_t* found_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
                            if (!found_entry)
                            {
                                found_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
                            }

                            return found_entry;
                        }()
                };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }

            /*
                @brief Overwrites the from-compiled entry point.
                @details Set to the c2i adapter during deoptimisation so compiled callers
                         transition to the interpreter (and reach our patched i2i stub).
            */
            auto set_from_compiled_entry(void* const entry_point) noexcept
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []() noexcept
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        const vmhook::hotspot::vm_struct_entry_t* found_entry{ vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point") };
                        if (!found_entry)
                        {
                            found_entry = vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry");
                        }

                        return found_entry;
                    }()
                };
                if (!entry)
                {
                    return;
                }

                *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(this) + entry->offset) = entry_point;
            }

            /*
                @brief Returns the AdapterHandlerEntry* (_adapter field).
                @details Stores the calling-convention adapters (i2c / c2i) for this method.
                         Used to obtain the c2i entry when deoptimising a compiled method.
            */
            auto get_adapter() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Method", "_adapter") };
                if (!entry)
                {
                    return nullptr;
                }

                return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::method*>(this)) + entry->offset);
            }
        };

        /*
            @brief Describes a Java field discovered from InstanceKlass._fields.
            @details
            - offset     Byte offset of the field's value within the object (instance fields)
                         or within the java.lang.Class mirror object (static fields).
            - is_static  true when JVM_ACC_STATIC (0x0008) is set on the field.
            - signature  JVM type descriptor, e.g. "I", "J", "Z", "Ljava/lang/String;"
        */
        struct field_entry_t
        {
            std::uint32_t offset;
            bool          is_static;
            std::string   signature;
        };

        /*
            @brief Represents a HotSpot internal Klass object.
            @details
            Klass is the JVM's internal representation of a Java class or interface.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct klass
        {
            /*
                @brief Returns the symbol representing the internal name of this class.
                @return Pointer to the symbol containing the class name using '/' separators,
                        or nullptr on failure.
            */
            auto get_name() const
                -> vmhook::hotspot::symbol*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_name") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find Klass._name entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    const void* const raw{ vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                    vmhook::hotspot::symbol* const symbol_pointer{ reinterpret_cast<vmhook::hotspot::symbol*>(const_cast<void*>(vmhook::hotspot::untag_pointer(raw))) };

                    return vmhook::hotspot::is_valid_pointer(symbol_pointer) ? symbol_pointer : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} klass.get_name() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next sibling klass in the ClassLoaderData klass linked list.
            */
            auto get_next_link() const
                -> vmhook::hotspot::klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_next_link") };

                if (!entry)
                {
                    throw vmhook::exception{ "Failed to find Klass._next_link entry." };
                }

                if (!vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                vmhook::hotspot::klass* const next{ *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + entry->offset) };

                return vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
            }

            /*
                @brief Returns the number of methods declared by this InstanceKlass.
                @details
                Reads InstanceKlass._methods via its VMStruct offset, then reads
                the Array<Method*>::_length field at offset 0 of the array.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_count() const noexcept
                -> std::int32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(array))
                {
                    return 0;
                }

                return *reinterpret_cast<std::int32_t*>(array);
            }

            /*
                @brief Returns a pointer to the first Method* in this InstanceKlass's methods array.
                @details
                Reads InstanceKlass._methods via its VMStruct offset. The Array<Method*> layout
                in HotSpot is: [int _length (4 bytes)] [4 bytes padding] [Method* _data[...]].
                Data starts at offset 8 from the array base pointer.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_ptr() const noexcept
                -> vmhook::hotspot::method**
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(array))
                {
                    return nullptr;
                }

                // HotSpot Array<T> layout on x64:
                //   +0  int32_t _length   (4 bytes)
                //   +4  int32_t _padding  (4 bytes alignment padding)
                //   +8  T       _data[0]  ← first element starts here
                return reinterpret_cast<vmhook::hotspot::method**>(reinterpret_cast<std::uint8_t*>(array) + 8);
            }

            /*
                @brief Returns the java.lang.Class mirror object associated with this klass.
                @details
                Reads Klass::_java_mirror, which is an OopHandle (introduced as such in JDK 17).
                An OopHandle wraps an oop* that points into an OopStorage slot.
                Dereferencing that slot yields the full 64-bit address of the
                java.lang.Class instance. Static field values are stored inside this mirror.
                @return Pointer to the java.lang.Class object, or nullptr on failure.
            */
            auto get_java_mirror() const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_java_mirror") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                /*
                    The type and indirection level of _java_mirror changed across JDK versions.

                    JDK 8 – JDK 16:
                      _java_mirror is a plain `oop` (a direct 64-bit pointer to the
                      java.lang.Class object). One read is enough.
                      VMStructs type_string: "oop"

                    JDK 17+:
                      _java_mirror was changed to an OopHandle:
                        struct OopHandle { oop* _obj; };
                      The value at klass+offset is an oop* pointing into an OopStorage
                      arena. Dereferencing that slot yields the actual java.lang.Class oop.
                      VMStructs type_string: "OopHandle"

                    Detection: inspect entry->type_string at runtime so we stay
                    version-agnostic without any hardcoded JDK version numbers.
                */
                static const bool is_oop_handle{ entry->type_string && std::strcmp(entry->type_string, "OopHandle") == 0 };

                const void* const field_addr{ reinterpret_cast<const std::uint8_t*>(this) + entry->offset };

                if (is_oop_handle)
                {
                    // JDK 17+: two-level indirection through OopStorage.
                    //   klass + offset  →  OopHandle._obj  (oop* into OopStorage)
                    //   *OopHandle._obj →  java.lang.Class oop  (full 64-bit, not compressed)
                    const void* const oop_storage_slot{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    if (!vmhook::hotspot::is_valid_pointer(oop_storage_slot))
                    {
                        return nullptr;
                    }

                    return const_cast<void*>(vmhook::hotspot::safe_read_pointer(oop_storage_slot));
                }
                else
                {
                    // JDK 8 – JDK 16: the field is a direct full-width oop pointer.
                    // Read it as a 64-bit value; no further dereference needed.
                    const void* const mirror{ vmhook::hotspot::safe_read_pointer(field_addr) };
                    return vmhook::hotspot::is_valid_pointer(mirror) ? const_cast<void*>(mirror) : nullptr;
                }
            }

            /*
                @brief Returns the super-klass of this klass, or nullptr for java.lang.Object.
            */
            auto get_super() const noexcept -> klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{
                    vmhook::hotspot::iterate_struct_entries("Klass", "_super") };
                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }
                auto* const super{
                    *reinterpret_cast<klass**>(reinterpret_cast<std::uint8_t*>(
                        const_cast<klass*>(this)) + entry->offset) };
                return vmhook::hotspot::is_valid_pointer(super) ? super : nullptr;
            }

            auto get_instance_size() const noexcept
                -> std::size_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_layout_helper") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                const std::int32_t layout_helper{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                if (layout_helper <= 0)
                {
                    return 0;
                }

                return static_cast<std::size_t>(layout_helper & ~1);
            }

            auto get_prototype_header() const noexcept
                -> std::uintptr_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("Klass", "_prototype_header") };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 1;
                }

                return *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);
            }

            /*
                @brief Searches the InstanceKlass _fields array for a field by name.
                @param name The exact Java field name (e.g. "health", "x", "INSTANCE").
                @return A field_entry_t with offset, static flag, and type descriptor,
                        or std::nullopt when the field is not declared directly by this class.
                @details
                Parses InstanceKlass._fields, which is an Array<u2>.  Each field occupies
                exactly 6 consecutive u2 slots (FieldInfo::field_slots):
                  [0] access_flags  [1] name_index  [2] signature_index
                  [3] initval_index [4] low_packed   [5] high_packed
                The byte offset is recovered as: ((high_packed << 16) | low_packed) >> 2
                (FIELDINFO_TAG_SIZE = 2).
                Name and signature are resolved from the class constant pool.
                @note Searches only this class, not superclasses.  Walk the superclass
                      chain manually to locate inherited fields.
                @note Covers JDK 8–21.  JDK 22+ uses a different FieldInfoStream encoding.
            */
            /*
                @brief Decodes one UNSIGNED5 value from a byte stream (JDK 21+ FieldInfoStream).
                @details
                Algorithm (from src/hotspot/share/utilities/unsigned5.hpp):
                  sum = sum of (b_i - 1) * 64^i   for i = 0, 1, 2, ...
                  Stop after the first byte b_i where b_i < 192 (a "low byte").
                  Byte 0 is never emitted; it marks the stream End and returns ~0u.
            */
            inline static auto decode_u5(const std::uint8_t* data, int& stream_pos) noexcept
                -> std::uint32_t
            {
                std::uint32_t sum{ 0 };
                for (int byte_position{ 0 }; byte_position < 5; ++byte_position)
                {
                    const std::uint8_t current_byte{ data[stream_pos++] };
                    if (current_byte == 0)
                    {
                        --stream_pos;
                        return ~0u;  // End marker - never a valid value
                    }
                    sum += static_cast<std::uint32_t>(current_byte - 1) << (6 * byte_position);
                    if (current_byte < 192)
                    {
                        return sum;  // Low byte - sequence complete
                    }
                }
                return sum;
            }

            /*
                @brief Looks up a field by name from an InstanceKlass._fieldinfo_stream
                       (JDK 21+ FieldInfoStream format, Array<u1>).
                @details
                Stream grammar (from fieldInfo.hpp):
                  FieldInfoStream := j(num_java) k(num_injected) Field[j+k] End(0)
                  Field := name_idx sig_idx offset access_flags field_flags
                           [initval_idx  if field_flags & 0x01]
                           [gsig_idx     if field_flags & 0x04]
                           [group        if field_flags & 0x10]
                All integers encoded with UNSIGNED5 (see decode_u5).
            */
            auto find_field_in_stream(const std::string_view name, void** constant_pool_base) const noexcept
                -> std::optional<vmhook::hotspot::field_entry_t>
            {
                static const vmhook::hotspot::vm_struct_entry_t* const fis_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") };

                if (!fis_entry || !vmhook::hotspot::is_valid_pointer(this) || !vmhook::hotspot::is_valid_pointer(constant_pool_base))
                {
                    return std::nullopt;
                }

                // Read Array<u1>* pointer from InstanceKlass
                const void* const arr_ptr{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + fis_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(arr_ptr))
                {
                    return std::nullopt;
                }

                // Array<u1> layout on x64 HotSpot:
                //   +0  int32_t _length   (4 bytes)
                //   +4  u1      _data[0]  ← data starts here (no padding: u1 has alignment 1)
                // Note: Array<Method*> uses +8 because 8-byte pointers need 8-byte alignment,
                // requiring 4 bytes of padding after _length.  u1 and u2 arrays do NOT need this.
                const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(arr_ptr) };
                if (length <= 0 || length > 0x4000)
                {
                    return std::nullopt;
                }

                const std::uint8_t* const data{ reinterpret_cast<const std::uint8_t*>(arr_ptr) + 4 };

                std::int32_t stream_pos{ 0 };

                // Header: number of Java-declared fields, then number of VM-injected fields.
                const std::uint32_t num_java_fields{ decode_u5(data, stream_pos) };
                const std::uint32_t num_injected_fields{ decode_u5(data, stream_pos) };
                if (num_java_fields == ~0u || num_injected_fields == ~0u || num_java_fields + num_injected_fields > 4096u)
                {
                    return std::nullopt;
                }

                for (std::uint32_t field_index{ 0 }; field_index < num_java_fields + num_injected_fields && stream_pos < length; ++field_index)
                {
                    const std::uint32_t name_index{ decode_u5(data, stream_pos) };
                    if (name_index == ~0u)
                    {
                        break;
                    }

                    const std::uint32_t sig_index{ decode_u5(data, stream_pos) };
                    const std::uint32_t field_offset{ decode_u5(data, stream_pos) };
                    const std::uint32_t access_flags{ decode_u5(data, stream_pos) };
                    const std::uint32_t field_flags{ decode_u5(data, stream_pos) };

                    // Consume optional trailing entries whose presence is signalled by field_flags bits:
                    //   bit 0 (0x01): initval_index  - compile-time constant initialiser value
                    //   bit 2 (0x04): generic_sig_index - generic type signature (e.g. List<T>)
                    //   bit 4 (0x10): contended_group - @Contended padding group id
                    if (field_flags & 0x01u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }
                    if (field_flags & 0x04u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }
                    if (field_flags & 0x10u)
                    {
                        vmhook::hotspot::klass::decode_u5(data, stream_pos);
                    }

                    // Resolve the field name from the constant pool and compare.
                    if (name_index && vmhook::hotspot::is_valid_pointer(constant_pool_base[name_index]))
                    {
                        const vmhook::hotspot::symbol* const name_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[name_index]) };
                        if (vmhook::hotspot::is_valid_pointer(name_symbol) && name_symbol->to_string() == name)
                        {
                            const bool is_static{ (access_flags & 0x0008u) != 0u };
                            std::string signature;
                            if (sig_index && vmhook::hotspot::is_valid_pointer(constant_pool_base[sig_index]))
                            {
                                const vmhook::hotspot::symbol* const signature_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[sig_index]) };
                                if (vmhook::hotspot::is_valid_pointer(signature_symbol))
                                {
                                    signature = signature_symbol->to_string();
                                }
                            }
                            return vmhook::hotspot::field_entry_t{ field_offset, is_static, signature };
                        }
                    }
                }
                return std::nullopt;
            }

            /*
                @brief Searches the InstanceKlass field metadata for a field by name.
                @details
                Automatically selects the correct field storage format based on what
                is exported in gHotSpotVMStructs for this JDK build:

                JDK 8 – early JDK 21:
                  InstanceKlass._fields  →  Array<u2>, 6 slots per field
                  Byte offset = ((high_packed << 16) | low_packed) >> FIELDINFO_TAG_SIZE(2)

                JDK 21.0.x+ and JDK 22+:
                  InstanceKlass._fieldinfo_stream  →  Array<u1>, UNSIGNED5 compressed
                  Stream grammar: j(num_java) k(num_injected) Field[j+k] End(0)
                  Per field: name_idx sig_idx offset access_flags field_flags [optionals]

                @note Searches only fields declared directly on this class.
                      Walk the superclass chain to find inherited fields.
            */
            auto find_field(const std::string_view name) const noexcept
                -> std::optional<vmhook::hotspot::field_entry_t>
            {
                static const vmhook::hotspot::vm_struct_entry_t* const fields_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fields") };
                static const vmhook::hotspot::vm_struct_entry_t* const fis_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_fieldinfo_stream") };
                static const vmhook::hotspot::vm_struct_entry_t* const constants_entry{ vmhook::hotspot::iterate_struct_entries("InstanceKlass", "_constants") };

                if (!vmhook::hotspot::is_valid_pointer(this) || !constants_entry)
                {
                    return std::nullopt;
                }

                // Resolve constant pool (needed by both paths)
                vmhook::hotspot::constant_pool* const constant_pool_ptr{ *reinterpret_cast<vmhook::hotspot::constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + constants_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(constant_pool_ptr))
                {
                    return std::nullopt;
                }

                void** const constant_pool_base{ constant_pool_ptr->get_base() };
                if (!vmhook::hotspot::is_valid_pointer(constant_pool_base))
                {
                    return std::nullopt;
                }

                // -- JDK 21+ path: FieldInfoStream ----------------------------
                if (fis_entry)
                {
                    return vmhook::hotspot::klass::find_field_in_stream(name, constant_pool_base);
                }

                // -- JDK 8–17 path: Array<u2> with 6-slot FieldInfo records --
                if (!fields_entry)
                {
                    return std::nullopt;
                }

                void* const fields_array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::klass*>(this)) + fields_entry->offset) };

                if (!vmhook::hotspot::is_valid_pointer(fields_array))
                {
                    return std::nullopt;
                }

                // Array<u2>: int32_t _length at +0, data at +8
                const std::int32_t array_length{ *reinterpret_cast<const std::int32_t*>(fields_array) };

                static const std::int32_t field_slots{ 6 };

                // In JDK 8, the _fields Array<u2> may include a trailing u2
                // storing _java_fields_count after all the 6-slot field records.
                // Use integer division to safely ignore any trailing partial slot.
                if (array_length <= 0 || array_length < field_slots)
                {
                    return std::nullopt;
                }

                // Array<u2> layout on x64 HotSpot:
                //   +0  int32_t _length   (4 bytes)
                //   +4  u2      _data[0]  ← data starts here (u2 needs 2-byte alignment;
                //                           offset 4 is already 2-byte aligned, no padding needed)
                const std::uint16_t* const data{ reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(fields_array) + 4) };

                // FieldInfo::field_slots layout for each field record (JDK 8–20):
                //   slot 0  u16  access_flags   - JVM_ACC_* flags (static flag = bit 3 / 0x0008)
                //   slot 1  u16  name_index     - constant-pool index of the field's name Symbol
                //   slot 2  u16  signature_index- constant-pool index of the type descriptor Symbol
                //   slot 3  u16  initval_index  - cp index of compile-time constant value (or 0)
                //   slot 4  u16  low_packed     - bits [1:0] FIELDINFO_TAG, bits [15:2] offset_low
                //   slot 5  u16  high_packed    - offset_high (upper 16 bits of the packed offset)
                for (std::int32_t field_slot_index{ 0 }; field_slot_index < array_length / field_slots; ++field_slot_index)
                {
                    const std::uint16_t name_index{ data[field_slot_index * field_slots + 1] };
                    if (!name_index)
                    {
                        continue;  // slot 1: name_index == 0 means VM-injected field, skip
                    }

                    const vmhook::hotspot::symbol* const name_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[name_index]) };
                    if (!vmhook::hotspot::is_valid_pointer(name_symbol) || name_symbol->to_string() != name)
                    {
                        continue;
                    }

                    const std::uint16_t access_flags{ data[field_slot_index * field_slots + 0] };  // slot 0
                    const std::uint16_t sig_index{ data[field_slot_index * field_slots + 2] };  // slot 2
                    const std::uint16_t low_packed{ data[field_slot_index * field_slots + 4] };  // slot 4
                    const std::uint16_t high_packed{ data[field_slot_index * field_slots + 5] };  // slot 5

                    // Reconstruct the byte offset from the packed representation:
                    //   packed = (high_packed << 16) | low_packed
                    //   offset = packed >> FIELDINFO_TAG_SIZE   (FIELDINFO_TAG_SIZE = 2)
                    // The 2 lowest bits of packed are the FIELDINFO_TAG and carry no offset data.
                    const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed };
                    const std::uint32_t offset{ packed >> 2 };

                    const bool is_static{ (access_flags & 0x0008u) != 0u };

                    const vmhook::hotspot::symbol* const signature_symbol{ reinterpret_cast<const vmhook::hotspot::symbol*>(constant_pool_base[sig_index]) };
                    const std::string signature{ vmhook::hotspot::is_valid_pointer(signature_symbol) ? signature_symbol->to_string() : std::string{} };

                    return vmhook::hotspot::field_entry_t{ offset, is_static, signature };
                }

                return std::nullopt;
            }
        };

        /*
            @brief Represents a HotSpot ClassLoaderData node.
            @details
            Each ClassLoader in the JVM has a corresponding ClassLoaderData that
            tracks every Klass it has loaded. The ClassLoaderData nodes are chained
            together via _next into a global linked list whose head is held by
            ClassLoaderDataGraph::_head.
        */
        struct class_loader_data
        {
            /*
                @brief Returns the head of the klass linked list for this classloader.
                @details
                Reads ClassLoaderData::_klasses using its offset from gHotSpotVMStructs.
            */
            auto get_klasses() const
                -> vmhook::hotspot::klass*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderData._klasses entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    // _klasses is a Klass* (full 8-byte native pointer), not a compressed OOP.
                    vmhook::hotspot::klass* const result{ *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::class_loader_data*>(this)) + entry->offset) };

                    return vmhook::hotspot::is_valid_pointer(result) ? result : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_klasses() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next ClassLoaderData node in the global linked list.
            */
            auto get_next() const
                -> vmhook::hotspot::class_loader_data*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ iterate_struct_entries("ClassLoaderData", "_next") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderData._next entry." };
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    vmhook::hotspot::class_loader_data* const next{ reinterpret_cast<vmhook::hotspot::class_loader_data*>(const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_next() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the Dictionary associated with this classloader.
            */
            auto get_dictionary() const
                -> vmhook::hotspot::dictionary*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_dictionary") };

                try
                {
                    if (!entry)
                    {
                        return nullptr;
                    }

                    if (!vmhook::hotspot::is_valid_pointer(this))
                    {
                        return nullptr;
                    }

                    vmhook::hotspot::dictionary* const dict{ reinterpret_cast<vmhook::hotspot::dictionary*>(const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return vmhook::hotspot::is_valid_pointer(dict) ? dict : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data.get_dictionary() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot Dictionary - the per-classloader class registry.
            @details
            Each ClassLoaderData owns a Dictionary which is a hashtable mapping class
            names to their corresponding Klass* objects. It inherits from
            BasicHashtable<mtInternal> whose layout is:
            - offset 0: _table_size (int32) - number of buckets
            - offset 8: _buckets (HashtableBucket*) - pointer to the bucket array
            Each bucket slot holds a pointer to the head of a linked list of
            DictionaryEntry nodes. Each DictionaryEntry has:
            - offset 0:  _next (DictionaryEntry*) - next entry in the chain
            - offset 8:  _hash (uint32)
            - offset 16: _literal (Klass*) - the actual class
        */
        struct dictionary
        {
            /*
                @brief Returns the number of hash buckets in this dictionary.
                @details
                BasicHashtable<mtInternal> layout (x64):
                  +0  int32_t _table_size  - number of buckets  ← this offset
                  +4  (4 bytes alignment padding)
                  +8  HashtableBucket* _buckets
            */
            inline auto get_table_size() const noexcept
                -> std::int32_t
            {
                // _table_size is the first field of BasicHashtable at offset 0.
                return *reinterpret_cast<const std::int32_t*>(this);
            }

            /*
                @brief Returns a pointer to the bucket array of this dictionary.
                @details
                _buckets is the second field of BasicHashtable at offset +8 (after
                _table_size int32 + 4 bytes padding on x64).
                Each element is a pointer to the head of a singly-linked DictionaryEntry chain.
            */
            inline auto get_buckets() const noexcept
                -> const std::uint8_t*
            {
                // +8 bytes = sizeof(int32_t _table_size) + 4 bytes alignment padding.
                return *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(this) + 8);
            }

            /*
                @brief Searches this dictionary for a klass by its internal name.
            */
            auto find_klass(const std::string_view class_name) const
                -> vmhook::hotspot::klass*
            {
                const std::int32_t table_size{ this->get_table_size() };
                const std::uint8_t* const buckets{ this->get_buckets() };

                if (!vmhook::hotspot::is_valid_pointer(buckets) || table_size <= 0 || table_size > 0x186A0)
                {
                    return nullptr;
                }

                for (std::int32_t bucket_index{ 0 }; bucket_index < table_size; ++bucket_index)
                {
                    const std::uint8_t* dict_entry{ reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(buckets + bucket_index * 8))) };

                    while (vmhook::hotspot::is_valid_pointer(dict_entry))
                    {
                        // DictionaryEntry (extends HashtableEntry<InstanceKlass*, mtClass>) layout:
                        //   +0   void*       _next    (next entry in the chain)
                        //   +8   uint32_t    _hash    (pre-computed hash, 4 bytes + 4 padding)
                        //   +16  Klass*      _literal ← the actual class pointer
                        const void* const raw_klass{ vmhook::hotspot::safe_read_pointer(dict_entry + 16) };
                        const vmhook::hotspot::klass* const candidate_klass{ reinterpret_cast<const vmhook::hotspot::klass*>(vmhook::hotspot::untag_pointer(raw_klass)) };

                        if (vmhook::hotspot::is_valid_pointer(candidate_klass))
                        {
                            const vmhook::hotspot::symbol* const sym{ candidate_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym) && sym->to_string() == class_name)
                            {
                                return const_cast<vmhook::hotspot::klass*>(candidate_klass);
                            }
                        }

                        dict_entry = reinterpret_cast<const std::uint8_t*>(vmhook::hotspot::untag_pointer(vmhook::hotspot::safe_read_pointer(dict_entry)));
                    }
                }

                return nullptr;
            }
        };

        /*
            @brief Represents the HotSpot ClassLoaderDataGraph - the global registry of all classloaders.
            @details
            ClassLoaderDataGraph::_head is a static field holding the head of a global
            linked list of ClassLoaderData nodes, one per classloader registered in the JVM.
        */
        struct class_loader_data_graph
        {
            /*
                @brief Returns the head of the global ClassLoaderData linked list.
            */
            auto get_head() const
                -> vmhook::hotspot::class_loader_data*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("ClassLoaderDataGraph", "_head") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find ClassLoaderDataGraph._head entry." };
                    }

                    vmhook::hotspot::class_loader_data* const head{ *reinterpret_cast<vmhook::hotspot::class_loader_data* const*>(entry->address) };

                    return vmhook::hotspot::is_valid_pointer(head) ? head : nullptr;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} class_loader_data_graph.get_head() {}", vmhook::error_tag, exception.what());
                    return nullptr;
                }
            }

            /*
                @brief Searches all loaded classloaders for a klass by its internal name.
                @param class_name The internal JVM class name using '/' separators
                                  (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
                @return Pointer to the matching klass if found, nullptr otherwise.
                @details
                Walks the full ClassLoaderDataGraph using only HotSpot internal structures
                resolved via gHotSpotVMStructs, without any JNI or JVMTI calls.
            */
            auto find_klass(const std::string_view class_name) const
                -> vmhook::hotspot::klass*
            {
                // Adaptive strategy - detected once at startup via VMStructs presence:
                //   JDK 21+  exports _klasses  but not _dictionary
                //   JDK 8-17 exports _dictionary but not _klasses
                static const bool use_klasses{ vmhook::hotspot::iterate_struct_entries("ClassLoaderData", "_klasses") != nullptr };

                vmhook::hotspot::class_loader_data* class_loader_data{ this->get_head() };

                while (vmhook::hotspot::is_valid_pointer(class_loader_data) && class_loader_data)
                {
                    if (use_klasses)
                    {
                        // JDK 21+: walk the _klasses linked list
                        vmhook::hotspot::klass* current_klass{ class_loader_data->get_klasses() };
                        while (current_klass && vmhook::hotspot::is_valid_pointer(current_klass))
                        {
                            const vmhook::hotspot::symbol* const sym{ current_klass->get_name() };
                            if (vmhook::hotspot::is_valid_pointer(sym) && sym->to_string() == class_name)
                            {
                                return current_klass;
                            }

                            current_klass = current_klass->get_next_link();
                        }
                    }
                    else
                    {
                        // JDK 8-17: search via per-CLD Dictionary hashtable
                        // (may return null if _dictionary not in VMStructs for this JDK build)
                        vmhook::hotspot::dictionary* const dict{ class_loader_data->get_dictionary() };
                        if (vmhook::hotspot::is_valid_pointer(dict))
                        {
                            vmhook::hotspot::klass* const found_klass{ dict->find_klass(class_name) };
                            if (found_klass)
                            {
                                return found_klass;
                            }
                        }
                    }

                    vmhook::hotspot::class_loader_data* const next{ class_loader_data->get_next() };
                    class_loader_data = vmhook::hotspot::is_valid_pointer(next) ? next : nullptr;
                }

                // JDK 8 fallback: ClassLoaderData._dictionary not in VMStructs,
                // but SystemDictionary._dictionary (bootstrap CL) and
                // SystemDictionary._shared_dictionary (CDS) ARE exported as statics.
                // These cover all bootstrap-loaded classes (java.*, javax.*, sun.*, etc.)
                // and most Minecraft client classes (loaded by the same classloader chain).
                static const vmhook::hotspot::vm_struct_entry_t* const sd_main{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_dictionary") };
                static const vmhook::hotspot::vm_struct_entry_t* const sd_shared{ vmhook::hotspot::iterate_struct_entries("SystemDictionary", "_shared_dictionary") };

                for (const vmhook::hotspot::vm_struct_entry_t* system_dictionary_entry : { sd_main, sd_shared })
                {
                    if (!system_dictionary_entry || !system_dictionary_entry->address)
                    {
                        continue;
                    }
                    vmhook::hotspot::dictionary* const dictionary_pointer{ *reinterpret_cast<vmhook::hotspot::dictionary**>(system_dictionary_entry->address) };
                    if (!vmhook::hotspot::is_valid_pointer(dictionary_pointer))
                    {
                        continue;
                    }
                    klass* const found_klass{ dictionary_pointer->find_klass(class_name) };
                    if (found_klass)
                    {
                        return found_klass;
                    }
                }

                return nullptr;
            }
        };

        /*
            @brief Represents the possible execution states of a HotSpot JavaThread.
        */
        enum class java_thread_state : std::int8_t
        {
            _thread_uninitialized = 0,
            _thread_new = 2,
            _thread_new_trans = 3,
            _thread_in_native = 4,
            _thread_in_native_trans = 5,
            _thread_in_vm = 6,
            _thread_in_vm_trans = 7,
            /*
                @brief The thread is currently executing Java bytecode in the interpreter.
                @note This is the only state in which method hooks are safely intercepted.
            */
            _thread_in_Java = 8,
            _thread_in_Java_trans = 9,
            _thread_blocked = 10,
            _thread_blocked_trans = 11,
            _thread_max_state = 12
        };

        /*
            @brief Represents a HotSpot internal JavaThread object.
            @details
            JavaThread is the JVM's internal representation of a Java thread.
            On x64 HotSpot the current JavaThread pointer is always held in register r15,
            making it directly accessible from low-level hook stubs.
            The layout is resolved at runtime via gHotSpotVMStructs.
        */
        struct java_thread
        {
            /*
                @brief Returns the current execution state of this thread.
            */
            auto get_thread_state() const
                -> vmhook::hotspot::java_thread_state
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    return *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.get_thread_state() {}", vmhook::error_tag, exception.what());
                    return vmhook::hotspot::java_thread_state::_thread_uninitialized;
                }
            }

            /*
                @brief Sets the execution state of this thread.
                @warning Incorrect use of this function can corrupt the JVM thread state machine.
            */
            auto set_thread_state(const vmhook::hotspot::java_thread_state state) const
                -> void
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset) = state;
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.set_thread_state() {}", vmhook::error_tag, exception.what());
                }
            }

            /*
                @brief Returns the current suspension flags of this thread.
            */
            auto get_suspend_flags() const
                -> std::uint32_t
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_suspend_flags") };

                try
                {
                    if (!entry)
                    {
                        throw vmhook::exception{ "Failed to find JavaThread._suspend_flags entry." };
                    }

                    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} java_thread.get_suspend_flags() {}", vmhook::error_tag, exception.what());
                    return 0;
                }
            }

            auto get_next() const noexcept
                -> vmhook::hotspot::java_thread*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        {
                            auto* found_entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_next") };
                            if (found_entry)
                            {
                                return found_entry;
                            }
                        }
                        return vmhook::hotspot::iterate_struct_entries("Thread", "_next");
                    }()
                };

                if (!entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return nullptr;
                }

                vmhook::hotspot::java_thread* const next_thread{ *reinterpret_cast<vmhook::hotspot::java_thread* const*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                return vmhook::hotspot::is_valid_pointer(next_thread) ? next_thread : nullptr;
            }

            auto get_os_thread_id() const noexcept
                -> DWORD
            {
                static const vmhook::hotspot::vm_struct_entry_t* const osthread_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        if (auto* const entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_osthread") })
                        {
                            return entry;
                        }

                        return vmhook::hotspot::iterate_struct_entries("Thread", "_osthread");
                    }()
                };
                static const vmhook::hotspot::vm_struct_entry_t* const thread_id_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        return vmhook::hotspot::iterate_struct_entries("OSThread", "_thread_id");
                    }()
                };

                if (!osthread_entry || !thread_id_entry || !vmhook::hotspot::is_valid_pointer(this))
                {
                    return 0;
                }

                void* const os_thread{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + osthread_entry->offset) };
                if (!os_thread || !vmhook::hotspot::is_valid_pointer(os_thread))
                {
                    return 0;
                }

                return *reinterpret_cast<const DWORD*>(reinterpret_cast<const std::uint8_t*>(os_thread) + thread_id_entry->offset);
            }

            auto allocate_tlab(const std::size_t byte_size) const noexcept
                -> void*
            {
                static const vmhook::hotspot::vm_struct_entry_t* const tlab_entry{ []()
                    -> const vmhook::hotspot::vm_struct_entry_t*
                    {
                        {
                            auto* entry{ vmhook::hotspot::iterate_struct_entries("JavaThread", "_tlab") };
                            if (entry)
                            {
                                return entry;
                            }
                        }
                        return vmhook::hotspot::iterate_struct_entries("Thread", "_tlab");
                    }()
                };
                static const vmhook::hotspot::vm_struct_entry_t* const top_entry{ vmhook::hotspot::iterate_struct_entries("ThreadLocalAllocBuffer", "_top") };
                static const vmhook::hotspot::vm_struct_entry_t* const end_entry{ vmhook::hotspot::iterate_struct_entries("ThreadLocalAllocBuffer", "_end") };

                if (!tlab_entry || !top_entry || !end_entry || byte_size == 0)
                {
                    return nullptr;
                }

                std::uint8_t* const tlab{ reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::java_thread*>(this)) + tlab_entry->offset };
                std::uint8_t** const top_address{ reinterpret_cast<std::uint8_t**>(tlab + top_entry->offset) };
                std::uint8_t** const end_address{ reinterpret_cast<std::uint8_t**>(tlab + end_entry->offset) };

                std::uint8_t* const top{ *top_address };
                std::uint8_t* const end{ *end_address };

                if (!vmhook::hotspot::is_valid_pointer(top) || !vmhook::hotspot::is_valid_pointer(end) || top > end)
                {
                    return nullptr;
                }

                if (static_cast<std::size_t>(end - top) < byte_size)
                {
                    return nullptr;
                }

                *top_address = top + byte_size;
                return top;
            }
        };

        inline thread_local vmhook::hotspot::java_thread* current_java_thread{ nullptr };
        inline thread_local void* current_jni_env{ nullptr };
        inline std::atomic<vmhook::hotspot::java_thread*> last_java_thread{ nullptr };

        static auto find_any_java_thread() noexcept
            -> vmhook::hotspot::java_thread*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const thread_list_entry{
                vmhook::hotspot::iterate_struct_entries("Threads", "_thread_list") };

            if (!thread_list_entry || !thread_list_entry->address)
            {
                return nullptr;
            }

            vmhook::hotspot::java_thread* const head{ *reinterpret_cast<vmhook::hotspot::java_thread**>(thread_list_entry->address) };
            return vmhook::hotspot::is_valid_pointer(head) ? head : nullptr;
        }

        static auto find_java_thread_by_os_thread_id(const DWORD os_thread_id) noexcept
            -> vmhook::hotspot::java_thread*
        {
            if (os_thread_id == 0)
            {
                return nullptr;
            }

            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* thread{ vmhook::hotspot::find_any_java_thread() };
                thread && vmhook::hotspot::is_valid_pointer(thread) && visited_threads < 4096;
                thread = thread->get_next(), ++visited_threads)
            {
                if (thread->get_os_thread_id() == os_thread_id)
                {
                    return thread;
                }
            }

            static const vmhook::hotspot::vm_struct_entry_t* const list_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
            static const vmhook::hotspot::vm_struct_entry_t* const length_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
            static const vmhook::hotspot::vm_struct_entry_t* const threads_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

            if (!list_entry || !length_entry || !threads_entry || !list_entry->address)
            {
                return nullptr;
            }

            void* const list{ *reinterpret_cast<void**>(list_entry->address) };
            if (!list || !vmhook::hotspot::is_valid_pointer(list))
            {
                return nullptr;
            }

            const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(list) + length_entry->offset) };
            if (length <= 0 || length > 4096)
            {
                return nullptr;
            }

            auto** const threads{ *reinterpret_cast<vmhook::hotspot::java_thread***>(reinterpret_cast<std::uint8_t*>(list) + threads_entry->offset) };
            if (!threads || !vmhook::hotspot::is_valid_pointer(threads))
            {
                return nullptr;
            }

            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                vmhook::hotspot::java_thread* const thread{ threads[index] };
                if (thread && vmhook::hotspot::is_valid_pointer(thread) && thread->get_os_thread_id() == os_thread_id)
                {
                    return thread;
                }
            }

            return nullptr;
        }

        static auto attach_current_native_thread() noexcept
            -> bool
        {
            HMODULE const jvm_module{ GetModuleHandleA("jvm.dll") };
            if (!jvm_module)
            {
                return false;
            }

            using jint = int;
            struct JNIEnv__;
            struct JavaVM__;
            using JNIEnv = JNIEnv__;
            using JavaVM = JavaVM__;

            struct JNIInvokeInterface_
            {
                void* reserved0;
                void* reserved1;
                void* reserved2;
                jint (*DestroyJavaVM)(JavaVM*);
                jint (*AttachCurrentThread)(JavaVM*, void**, void*);
                jint (*DetachCurrentThread)(JavaVM*);
                jint (*GetEnv)(JavaVM*, void**, jint);
                jint (*AttachCurrentThreadAsDaemon)(JavaVM*, void**, void*);
            };

            struct JavaVM__
            {
                const JNIInvokeInterface_* functions;
            };

            using get_created_java_vms_t = jint (*)(JavaVM**, jint, jint*);
            auto* const get_created_java_vms{ reinterpret_cast<get_created_java_vms_t>(GetProcAddress(jvm_module, "JNI_GetCreatedJavaVMs")) };
            if (!get_created_java_vms)
            {
                return false;
            }

            JavaVM* vm{};
            jint vm_count{};
            if (get_created_java_vms(&vm, 1, &vm_count) != 0 || vm_count <= 0 || !vm || !vm->functions)
            {
                return false;
            }

            constexpr jint jni_version_1_8{ 0x00010008 };
            void* env{};
            if (vm->functions->GetEnv && vm->functions->GetEnv(vm, &env, jni_version_1_8) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                return true;
            }

            if (vm->functions->AttachCurrentThreadAsDaemon && vm->functions->AttachCurrentThreadAsDaemon(vm, &env, nullptr) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                return true;
            }

            if (vm->functions->AttachCurrentThread && vm->functions->AttachCurrentThread(vm, &env, nullptr) == 0)
            {
                vmhook::hotspot::current_jni_env = env;
                return true;
            }

            return false;
        }

        static auto ensure_current_java_thread() noexcept
            -> bool
        {
            if (vmhook::hotspot::current_java_thread && vmhook::hotspot::is_valid_pointer(vmhook::hotspot::current_java_thread))
            {
                if (!vmhook::hotspot::current_jni_env)
                {
                    vmhook::hotspot::attach_current_native_thread();
                }

                return true;
            }

            const DWORD current_os_thread_id{ GetCurrentThreadId() };
            if (vmhook::hotspot::java_thread* const existing_thread{ vmhook::hotspot::find_java_thread_by_os_thread_id(current_os_thread_id) })
            {
                vmhook::hotspot::current_java_thread = existing_thread;
                vmhook::hotspot::last_java_thread.store(existing_thread, std::memory_order_relaxed);
                VMHOOK_LOG("{} ensure_current_java_thread(): adopted JavaThread 0x{:016X} for OS thread {}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(existing_thread), current_os_thread_id);
                return true;
            }

            if (!vmhook::hotspot::attach_current_native_thread())
            {
                VMHOOK_LOG("{} ensure_current_java_thread(): AttachCurrentThread failed for OS thread {}", vmhook::error_tag, current_os_thread_id);
                return false;
            }

            for (std::int32_t attempt{ 0 }; attempt < 64; ++attempt)
            {
                if (vmhook::hotspot::java_thread* const attached_thread{ vmhook::hotspot::find_java_thread_by_os_thread_id(current_os_thread_id) })
                {
                    vmhook::hotspot::current_java_thread = attached_thread;
                    vmhook::hotspot::last_java_thread.store(attached_thread, std::memory_order_relaxed);
                    VMHOOK_LOG("{} ensure_current_java_thread(): attached JavaThread 0x{:016X} for OS thread {}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(attached_thread), current_os_thread_id);
                    return true;
                }

                std::this_thread::yield();
            }

            VMHOOK_LOG("{} ensure_current_java_thread(): attached thread was not found in HotSpot thread list for OS thread {}", vmhook::error_tag, current_os_thread_id);
            return false;
        }

        static auto find_allocation_thread() noexcept
            -> vmhook::hotspot::java_thread*
        {
            if (vmhook::hotspot::current_java_thread && vmhook::hotspot::is_valid_pointer(vmhook::hotspot::current_java_thread))
            {
                vmhook::hotspot::last_java_thread.store(vmhook::hotspot::current_java_thread, std::memory_order_relaxed);
                return vmhook::hotspot::current_java_thread;
            }

            vmhook::hotspot::java_thread* const cached_thread{ vmhook::hotspot::last_java_thread.load(std::memory_order_relaxed) };
            if (cached_thread && vmhook::hotspot::is_valid_pointer(cached_thread))
            {
                return cached_thread;
            }

            vmhook::hotspot::java_thread* const discovered_thread{ vmhook::hotspot::find_any_java_thread() };
            if (discovered_thread)
            {
                vmhook::hotspot::last_java_thread.store(discovered_thread, std::memory_order_relaxed);
            }
            return discovered_thread;
        }

        static auto allocate_from_threads_list(const std::size_t byte_size) noexcept
            -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const list_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsSMRSupport", "_java_thread_list") };
            static const vmhook::hotspot::vm_struct_entry_t* const length_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_length") };
            static const vmhook::hotspot::vm_struct_entry_t* const threads_entry{
                vmhook::hotspot::iterate_struct_entries("ThreadsList", "_threads") };

            if (!list_entry || !length_entry || !threads_entry || !list_entry->address)
            {
                return nullptr;
            }

            void* const list{ *reinterpret_cast<void**>(list_entry->address) };
            if (!list || !vmhook::hotspot::is_valid_pointer(list))
            {
                return nullptr;
            }

            const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(list) + length_entry->offset) };
            if (length <= 0 || length > 4096)
            {
                return nullptr;
            }

            auto** const threads{ *reinterpret_cast<vmhook::hotspot::java_thread***>(reinterpret_cast<std::uint8_t*>(list) + threads_entry->offset) };
            if (!threads || !vmhook::hotspot::is_valid_pointer(threads))
            {
                return nullptr;
            }

            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                vmhook::hotspot::java_thread* const thread{ threads[index] };
                if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
                {
                    continue;
                }

                void* const object{ thread->allocate_tlab(byte_size) };
                if (object)
                {
                    vmhook::hotspot::last_java_thread.store(thread, std::memory_order_relaxed);
                    return object;
                }
            }

            return nullptr;
        }

        /*
            @brief Decodes a compressed OOP to a real 64-bit pointer.
            @param compressed The 32-bit compressed OOP value read from a JVM structure.
            @return The decoded real pointer, or nullptr if compressed is 0.
            @details
            HotSpot stores heap object pointers in 32-bit "compressed OOP" form to reduce
            memory usage. The formula to recover the real 64-bit address is:
              real_address = narrow_oop_base + (compressed << narrow_oop_shift)

            - narrow_oop_base  - base address of the Java heap (0 when -Xmx < 4 GB and
                                  heap starts at address 0, otherwise the heap start).
            - narrow_oop_shift - how many bits to left-shift the compressed value (typically
                                  0 for heap < 4 GB, 3 for heap up to 32 GB with 8-byte aligned oops).

            Both values are read from CompressedOops::_narrow_oop.{_base,_shift} via
            gHotSpotVMStructs so this works across all JDK versions.
        */
        static auto decode_oop_pointer(const std::uint32_t compressed) noexcept
            -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            // VMStruct field names for the narrow OOP base/shift changed across versions:
            //   JDK  8-16: Universe::_narrow_oop._base/shift
            //   JDK 17-24: CompressedOops::_narrow_oop._base/shift
            //   JDK 25+  : CompressedOops::_base/shift  (_narrow_oop. prefix dropped)
            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._base");
                }()
            };

            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t narrow_oop_base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t narrow_oop_shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            // real_address = narrow_oop_base + (compressed << narrow_oop_shift)
            return reinterpret_cast<void*>(narrow_oop_base + (static_cast<std::uint64_t>(compressed) << narrow_oop_shift));
        }

        /*
            @brief Compresses a decoded OOP pointer back into HotSpot's narrow OOP form.
            @details
            This is the inverse of decode_oop_pointer() and is used when assigning
            object wrapper fields through field_proxy::set().
        */
        static auto encode_oop_pointer(void* const decoded) noexcept
            -> std::uint32_t
        {
            if (!decoded)
            {
                return 0;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._base");
                }()
            };

            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedOops", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_oop._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return 0;
            }

            const std::uint64_t narrow_oop_base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t narrow_oop_shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };
            const std::uint64_t decoded_address{ reinterpret_cast<std::uint64_t>(decoded) };
            if (decoded_address < narrow_oop_base)
            {
                return 0;
            }

            return static_cast<std::uint32_t>((decoded_address - narrow_oop_base) >> narrow_oop_shift);
        }

        /*
            @brief Decodes a compressed Klass pointer to a real 64-bit pointer.
            @details
            Klass pointers use a separate compressed-pointer scheme from object OOPs,
            stored in CompressedKlassPointers::_narrow_klass.{_base,_shift}.
            The decoding formula is identical: real_address = base + (compressed << shift).
        */
        static auto decode_klass_pointer(const std::uint32_t compressed) noexcept
            -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            // VMStruct field names changed the same way as for CompressedOops:
            //   JDK  8-16: Universe::_narrow_klass._base/shift
            //   JDK 17-24: CompressedKlassPointers::_narrow_klass._base/shift
            //   JDK 25+  : CompressedKlassPointers::_base/shift
            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._base");
                }()
            };
            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            return reinterpret_cast<void*>(base + (static_cast<std::uint64_t>(compressed) << shift));
        }

        static auto encode_klass_pointer(void* const decoded) noexcept
            -> std::uint32_t
        {
            if (!decoded)
            {
                return 0;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const base_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_base") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._base");
                }()
            };
            static const vmhook::hotspot::vm_struct_entry_t* const shift_entry{ []()
                -> const vmhook::hotspot::vm_struct_entry_t*
                {
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    {
                        auto* entry{ vmhook::hotspot::iterate_struct_entries("CompressedKlassPointers", "_shift") };
                        if (entry)
                        {
                            return entry;
                        }
                    }
                    return vmhook::hotspot::iterate_struct_entries("Universe", "_narrow_klass._shift");
                }()
            };

            if (!base_entry || !shift_entry)
            {
                return 0;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };
            const std::uint64_t decoded_address{ reinterpret_cast<std::uint64_t>(decoded) };

            if (decoded_address < base)
            {
                return 0;
            }

            return static_cast<std::uint32_t>((decoded_address - base) >> shift);
        }

        /*
            @brief Checks whether a memory region matches a given byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto match_pattern(const std::uint8_t* const address, const std::uint8_t* const pattern, const std::size_t size)
            -> bool
        {
            for (std::size_t byte_index{ 0 }; byte_index < size; ++byte_index)
            {
                if (pattern[byte_index] == 0x00)
                {
                    continue;
                }
                if (address[byte_index] != pattern[byte_index])
                {
                    return false;
                }
            }
            return true;
        }

        /*
            @brief Scans a memory region for the first occurrence of a byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto scan(const std::uint8_t* const start, const std::size_t range, const std::uint8_t* pattern, const std::size_t size)
            -> std::uint8_t*
        {
            for (std::size_t scan_offset{ 0 }; scan_offset < range; ++scan_offset)
            {
                if (vmhook::hotspot::match_pattern(start + scan_offset, pattern, size))
                {
                    return const_cast<std::uint8_t*>(start + scan_offset);
                }
            }

            return nullptr;
        }

        /*
            @brief Determines the safe scannable size of a JVM generated code stub.
            @details
            Uses VirtualQuery to retrieve the memory region information and computes
            how many bytes remain from start to the end of the region, capped at 0x2000.
        */
        static auto find_stub_size(const std::uint8_t* start)
            -> std::size_t
        {
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(start, &mbi, sizeof(mbi));

            const std::size_t region_size{ static_cast<std::size_t>(reinterpret_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - start) };
            return (std::min)(region_size, static_cast<std::size_t>(0x2000));
        }

        /*
            @brief Byte offset used to retrieve the local variables pointer from the interpreter frame.
            @details
            Determined at runtime by find_hook_location(). Defaults to -56.
        */
        inline constinit std::int8_t locals_offset{ -56 };

        /*
            @brief Locates the optimal injection point within a HotSpot i2i interpreter stub.
            @param i2i_entry Pointer to the beginning of the i2i interpreter stub to scan.
            @return Pointer to the injection point within the stub, or nullptr on failure.
            @details
            Scans the i2i stub for a known sequence of instructions that appears at a stable
            location across JVM builds, immediately before the interpreter begins executing
            the actual Java bytecode. All method arguments are fully set up at this point.
            Also scans backwards to find and cache the locals_offset value.
            @note Two patterns are tried in order:
                  1. Full pattern (JDK 8 – early JDK 21): 4×mov-spill + mov BYTE PTR [r15+??],??
                     Hook injected at the thread-state-write instruction (last 8 bytes).
                  2. Fallback (JDK 21 release / JDK 22+): just mov BYTE PTR [r15+??],??
                     Hook injected at the start of that instruction directly.
        */
        static auto find_hook_location(const void* i2i_entry)
            -> void*
        {
            /*
                Primary pattern (JDK 8 – early JDK 21 builds):
                  Four consecutive `mov [rsp+imm32], eax` instructions that spill the first
                  four Windows x64 integer arguments to the shadow area, followed by
                  `mov BYTE PTR [r15+imm32], imm8` which writes a thread-status byte.
                  Wildcard bytes (0x00) match any value.
            */
            static constexpr std::uint8_t pattern_full[]
            {
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], ??
            };

            /*
                Fallback pattern (JDK 21 release builds, JDK 22+):
                  The 4×mov spill block is absent or different, but the
                  `mov BYTE PTR [r15+imm32], imm8` thread-status write is always present.
                  0x41 0xC6 0x87 = REX.B MOV r/m8,imm8 with ModRM selecting [r15+disp32].
                  All four displacement bytes and the imm8 are wildcards.
            */
            static constexpr std::uint8_t pattern_fallback[]
            {
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], ??
            };

            static constexpr std::uint8_t locals_pattern[]
            {
                0x4C, 0x8B, 0x75, 0x00, 0xC3 // mov r14, QWORD PTR [rbp+??] ; ret
            };

            const std::uint8_t* const current{ reinterpret_cast<const std::uint8_t*>(i2i_entry) };
            const std::size_t scan_size{ vmhook::hotspot::find_stub_size(current) };

            // Try the full 4×spill + thread-state-write pattern first (JDK 8 – early JDK 21).
            // The injection point is at the START of the thread-state-write instruction,
            // which sits at the END of the full pattern (offset = sizeof - 8).
            std::uint8_t* injection_point{ nullptr };
            std::uint8_t* const full_match{ vmhook::hotspot::scan(current, scan_size, pattern_full, sizeof(pattern_full)) };
            if (full_match)
            {
                // The thread-state instruction is the last 8 bytes of the full match.
                injection_point = full_match + sizeof(pattern_full) - 8;
            }
            else
            {
                // Fallback: scan directly for `mov BYTE PTR [r15+??], ??` (JDK 21+).
                // The injection point is at the START of the matched instruction.
                std::uint8_t* const fallback_match{ vmhook::hotspot::scan(current, scan_size, pattern_fallback, sizeof(pattern_fallback)) };
                if (fallback_match)
                {
                    injection_point = fallback_match;
                }
            }

            try
            {
                if (!injection_point)
                {
                    throw vmhook::exception{ "Failed to find hook pattern (tried full and fallback)." };
                }

                // Scan backwards from the injection point to find the locals pointer load:
                //   mov r14, QWORD PTR [rbp+disp8]  ; ret
                // The displacement byte is the rbp-relative offset of the locals pointer.
                for (std::uint8_t* scan_ptr{ injection_point }; scan_ptr > current; --scan_ptr)
                {
                    if (vmhook::hotspot::match_pattern(scan_ptr, locals_pattern, sizeof(locals_pattern)))
                    {
                        // Byte [3] of `4C 8B 75 ??` is the signed 8-bit displacement.
                        locals_offset = static_cast<std::int8_t>(scan_ptr[3]);
                        break;
                    }
                }

                return injection_point;
            }
            catch (const std::exception& exception)
            {
                VMHOOK_LOG("{} find_hook_location() {}", vmhook::error_tag, exception.what());
                return nullptr;
            }
        }

        /*
            @brief Allocates a block of executable memory within a 32-bit relative jump range
                   of a given address.
            @details
            Walks the process address space with VirtualQuery and allocates inside a free
            region that is reachable by a 5-byte relative JMP. HotSpot often reserves dense
            areas around code stubs, so blindly probing exact offsets with VirtualAlloc can
            fail even when a usable nearby free region exists.
        */
        static auto allocate_nearby_memory(std::uint8_t* nearby_addr, const std::size_t size, const DWORD protect) noexcept
            -> std::uint8_t*
        {
            if (!nearby_addr || size == 0)
            {
                return nullptr;
            }

            SYSTEM_INFO system_info{};
            GetSystemInfo(&system_info);

            const std::uintptr_t minimum_application_address{ reinterpret_cast<std::uintptr_t>(system_info.lpMinimumApplicationAddress) };
            const std::uintptr_t maximum_application_address{ reinterpret_cast<std::uintptr_t>(system_info.lpMaximumApplicationAddress) };
            const std::uintptr_t allocation_granularity{ static_cast<std::uintptr_t>(system_info.dwAllocationGranularity) };
            const std::uintptr_t target_address{ reinterpret_cast<std::uintptr_t>(nearby_addr) };
            const std::uintptr_t relative_limit{ static_cast<std::uintptr_t>((std::numeric_limits<std::int32_t>::max)()) };

            const auto align_up = [](const std::uintptr_t value, const std::uintptr_t alignment) noexcept
                -> std::uintptr_t
            {
                return (value + alignment - 1) & ~(alignment - 1);
            };

            const auto align_down = [](const std::uintptr_t value, const std::uintptr_t alignment) noexcept
                -> std::uintptr_t
            {
                return value & ~(alignment - 1);
            };

            const std::uintptr_t search_min{
                (std::max)(minimum_application_address, target_address > relative_limit ? target_address - relative_limit : minimum_application_address)
            };
            const std::uintptr_t search_max{
                target_address > maximum_application_address - relative_limit ? maximum_application_address : (std::min)(maximum_application_address, target_address + relative_limit)
            };

            auto try_allocate_in_region = [&](const std::uintptr_t region_base, const std::uintptr_t region_end) noexcept
                -> std::uint8_t*
            {
                if (region_end <= region_base || region_end - region_base < size)
                {
                    return nullptr;
                }

                const std::uintptr_t usable_begin{ (std::max)(region_base, search_min) };
                const std::uintptr_t usable_end{ (std::min)(region_end, search_max + 1) };
                if (usable_end <= usable_begin || usable_end - usable_begin < size)
                {
                    return nullptr;
                }

                const std::uintptr_t first_candidate{ align_up(usable_begin, allocation_granularity) };
                const std::uintptr_t last_candidate{ align_down(usable_end - size, allocation_granularity) };
                if (first_candidate > last_candidate)
                {
                    return nullptr;
                }

                const std::uintptr_t preferred_candidate{
                    target_address < first_candidate ? first_candidate :
                    target_address > last_candidate ? last_candidate :
                    align_down(target_address, allocation_granularity)
                };

                if (void* const allocated{ VirtualAlloc(reinterpret_cast<void*>(preferred_candidate), size, MEM_COMMIT | MEM_RESERVE, protect) })
                {
                    return reinterpret_cast<std::uint8_t*>(allocated);
                }

                if (preferred_candidate != first_candidate)
                {
                    if (void* const allocated{ VirtualAlloc(reinterpret_cast<void*>(first_candidate), size, MEM_COMMIT | MEM_RESERVE, protect) })
                    {
                        return reinterpret_cast<std::uint8_t*>(allocated);
                    }
                }

                if (preferred_candidate != last_candidate && first_candidate != last_candidate)
                {
                    if (void* const allocated{ VirtualAlloc(reinterpret_cast<void*>(last_candidate), size, MEM_COMMIT | MEM_RESERVE, protect) })
                    {
                        return reinterpret_cast<std::uint8_t*>(allocated);
                    }
                }

                return nullptr;
            };

            MEMORY_BASIC_INFORMATION memory_basic_info{};
            for (std::uintptr_t current{ search_min }; current < search_max; )
            {
                if (!VirtualQuery(reinterpret_cast<void*>(current), &memory_basic_info, sizeof(memory_basic_info)))
                {
                    current += system_info.dwPageSize;
                    continue;
                }

                const std::uintptr_t region_base{ reinterpret_cast<std::uintptr_t>(memory_basic_info.BaseAddress) };
                const std::uintptr_t region_size{ memory_basic_info.RegionSize };
                const std::uintptr_t region_end{ region_base + region_size };

                if (memory_basic_info.State == MEM_FREE)
                {
                    if (std::uint8_t* const allocated{ try_allocate_in_region(region_base, region_end) })
                    {
                        return allocated;
                    }
                }

                if (region_end <= current)
                {
                    break;
                }
                current = region_end;
            }

            return nullptr;
        }

        using detour_function_t = void(*)(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*);

        /*
            @brief Type-erased container for method arguments obtained via auto-detection.
            @details
            Returned by frame::get_arguments() (no template parameters). Stores raw decoded
            OOP pointers and class names. Call as<T>() to construct a wrapper on demand.
        */
        struct method_args
        {
            struct argument_entry
            {
                void* decoded_oop;
                std::string class_name;
            };

            /*
                @brief Returns the raw decoded OOP at the given index.
                @param index Zero-based argument index (0 = this / first parameter).
            */
            auto operator[](const std::size_t index) const noexcept
                -> void*
            {
                if (index >= this->arguments.size())
                {
                    return nullptr;
                }
                return this->arguments[index].decoded_oop;
            }

            /*
                @brief Returns the argument at the given index as a C++ wrapper.
                @tparam wrapper_type The C++ wrapper class (must be constructible from void* OOP).
                @param index Zero-based argument index.
                @return A new wrapper_type* constructed from the decoded OOP, or nullptr.
            */
            template<typename wrapper_type>
            auto as(const std::size_t index) const noexcept
                -> wrapper_type*
            {
                if (index >= this->arguments.size())
                {
                    return nullptr;
                }
                void* const raw{ this->arguments[index].decoded_oop };
                if (!raw)
                {
                    return nullptr;
                }
                return new wrapper_type{ raw };
            }

            auto size() const noexcept
                -> std::size_t
            {
                return this->arguments.size();
            }

            std::vector<argument_entry> arguments{};
        };


        /*
            @brief Represents a HotSpot interpreter frame on the call stack.
            @details
            In HotSpot's interpreter, each method invocation creates a frame on the
            native call stack. On x64 HotSpot, the base pointer (rbp) always points
            to the current frame. The Method pointer is at -24 bytes from rbp, and
            the local variables pointer is at locals_offset bytes from rbp.
        */
        struct frame
        {
        public:
            /*
                @brief Returns a pointer to the Method object of the currently executing method.
                @details
                On x64 HotSpot, `this` is rbp (the interpreter frame base pointer).
                The x64 interpreter frame layout relative to rbp is:
                  rbp + 0   saved caller rbp
                  rbp - 8   return address (pushed by call instruction)
                  rbp - 16  last_sp / expression stack bottom
                  rbp - 24  Method* pointer  ← this offset
                This corresponds to interpreter_frame_method_offset = -3 words = -24 bytes
                as defined in frame_x86.hpp.
            */
            inline auto get_method() const noexcept
                -> vmhook::hotspot::method*
            {
                // -24 bytes = -3 * sizeof(void*): the Method* slot in the interpreter frame.
                return *reinterpret_cast<vmhook::hotspot::method**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::frame*>(this)) - 24);
            }

            /*
                @brief Returns a pointer to the local variables array of the currently executing method.
                @details
                The frame slot at [rbp + locals_offset] encodes the locals pointer in one of two
                formats depending on the JDK era (see get_locals() implementation comment for
                the full derivation and proof from the hs_err crash dump).
                The actual offset within the frame is determined at hook time by scanning the i2i
                stub backwards for `mov r14, QWORD PTR [rbp+??]; ret`.  Defaults to -56.
            */
            inline auto get_locals() const noexcept
                -> void**
            {
                /*
                    How the locals pointer is encoded in the frame differs by JDK era.

                    JDK 8 – 20:
                      The stub spills r14 (the locals register) directly into the frame
                      via `mov r14, QWORD PTR [rbp+locals_offset]; ret` in a helper.
                      So [rbp + locals_offset] IS the locals pointer - a valid stack
                      address that passes vmhook::hotspot::is_valid_pointer().

                    JDK 21+:
                      The stub computes and spills an INDEX instead:
                        mov rax, r14
                        sub rax, rbp        ; rax = r14 - rbp  (positive, locals are above rbp)
                        shr rax, 3          ; rax = (r14 - rbp) >> 3
                        push rax            ; [rbp - 56] = index
                      The raw value at [rbp + locals_offset] is therefore a small positive
                      integer (e.g. 3), NOT a pointer.  Recover via: r14 = rbp + index * 8.

                    Detection: if the value at the frame slot is a valid user-space pointer
                    (> 0xFFFF), treat it as a direct locals pointer (JDK 8-20).
                    Otherwise treat it as a slot index (JDK 21+).

                    Proof from hs_err crash dump for JDK 21:
                      RBP = 0x243f888  →  [rbp-56] = 3
                      R14 = 0x243f8a0  →  rbp + 3*8 = 0x243f8a0  ✓
                */
                const void* const frame_slot_value{ *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(this) + locals_offset) };

                // JDK 8-20: direct pointer stored in the frame slot.
                if (vmhook::hotspot::is_valid_pointer(frame_slot_value))
                {
                    return const_cast<void**>(reinterpret_cast<const void* const*>(frame_slot_value));
                }

                // JDK 21+: the slot holds (r14 - rbp) >> 3 - a non-negative slot index.
                // Recover r14 = rbp + index * sizeof(void*).
                const std::uintptr_t slot_index{ reinterpret_cast<std::uintptr_t>(frame_slot_value) };
                if (slot_index < 0x1000u)
                {
                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<vmhook::hotspot::frame*>(this)) + slot_index * sizeof(void*));
                }

                return nullptr;
            }

            /*
                @brief Retrieves all method arguments as a typed tuple.
                @tparam types The C++ types of the method arguments in declaration order.
                @return A std::tuple containing all arguments converted to their C++ types.
                @details
                For primitive types, the raw slot value is reinterpreted directly.
                For pointer types, the compressed OOP is decoded via decode_oop_pointer().
                For index 0 on instance methods, this holds the implicit `this` reference.
            */
            template<typename... types>
            auto get_arguments() const noexcept
                -> std::tuple<types...>
            {
                std::int32_t index{ 0 };
                return std::tuple<types...>{ this->get_argument<types>(index++)... };
            }

            /*
                @brief Retrieves all method arguments by auto-detecting types from the method signature.
                @return A method_args container with one entry per argument.
                @details
                Parses the method descriptor (e.g. "(ILjava/lang/String;)V") to determine
                how many arguments there are and which slots correspond to reference types.
                For reference-type arguments the factory registered via vmhook::register_class<T>()
                is used to construct a std::unique_ptr<T>. Primitive arguments are skipped
                (only reference-type args produce entries).

                Usage:
                    auto args = frame->get_arguments();
                    auto* player = args.as<test_target>(0);  // first ref arg
            */
            auto get_arguments() const noexcept
                -> vmhook::hotspot::method_args
            {
                vmhook::hotspot::method_args result{};

                vmhook::hotspot::method* const current_method{ this->get_method() };
                if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                {
                    return result;
                }

                const std::string descriptor{ current_method->get_signature() };
                if (descriptor.empty())
                {
                    return result;
                }

                // Parse the parameter section: everything between '(' and ')'
                const std::size_t open_paren{ descriptor.find('(') };
                const std::size_t close_paren{ descriptor.find(')') };
                if (open_paren == std::string::npos || close_paren == std::string::npos || close_paren <= open_paren)
                {
                    return result;
                }

                void** const locals{ this->get_locals() };
                if (!locals)
                {
                    return result;
                }

                std::int32_t slot_index{ 0 };
                for (std::size_t pos{ open_paren + 1 }; pos < close_paren; )
                {
                    const char ch{ descriptor[pos] };

                    if (ch == 'L')
                    {
                        // Reference type: find the semicolon end, decode OOP
                        const std::size_t semi{ descriptor.find(';', pos) };
                        if (semi == std::string::npos)
                        {
                            break;
                        }
                        const std::string class_name{ descriptor.substr(pos + 1, semi - pos - 1) };

                        void* raw_value{ locals[-slot_index] };
                        void* decoded{ nullptr };
                        if (raw_value)
                        {
                            const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };
                            if (raw_bits <= 0xFFFFFFFFull)
                            {
                                decoded = vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(raw_bits));
                            }
                            else
                            {
                                decoded = raw_value;
                            }
                        }

                        if (decoded && vmhook::hotspot::is_valid_pointer(decoded))
                        {
                            result.arguments.push_back({ decoded, class_name });
                        }
                        else
                        {
                            result.arguments.push_back({ nullptr, class_name });
                        }

                        pos = semi + 1;
                    }
                    else if (ch == '[')
                    {
                        // Array type: skip array dimensions, then skip the base type
                        ++pos;
                        if (pos < close_paren && descriptor[pos] == 'L')
                        {
                            const std::size_t semi{ descriptor.find(';', pos) };
                            pos = (semi == std::string::npos) ? close_paren : semi + 1;
                        }
                        else
                        {
                            // Primitive array like [I, [Z - one char for base type
                            ++pos;
                        }
                    }
                    else
                    {
                        // Primitive: B, C, D, F, I, J, S, Z
                        ++pos;
                    }

                    // long and double occupy two local slots
                    if (ch == 'J' || ch == 'D')
                    {
                        ++slot_index;
                    }
                    ++slot_index;
                }

                return result;
            }

        private:
            /*
                @brief Retrieves a single method argument at the given index.
                @tparam type The C++ type to interpret the argument as.
                @param index Zero-based index into the local variables array.
                @return The argument value, or a default-constructed value on failure.
                @details
                HotSpot lays out the locals array in reverse order relative to the frame:
                  locals[0]  = argument 0  (this for instance methods)
                  locals[-1] = argument 1
                  locals[-2] = argument 2, ...
                So the slot for argument `index` is at locals[-index].
                - Pointer types: the slot holds a 32-bit compressed OOP (zero-extended to 64 bits
                  by the interpreter). vmhook::hotspot::decode_oop_pointer() reconstructs the real
                  address using narrow_oop_base + (compressed << narrow_oop_shift).
                - Primitive types (sizeof <= 8): the bits are copied verbatim via std::memcpy.
            */
            template<typename argument_type>
            auto get_argument(const std::int32_t index) const noexcept
                -> argument_type
            {
                void** const locals{ this->get_locals() };
                if (!locals)
                {
                    return argument_type{};
                }

                // locals[-index]: arguments are stored in descending slot order.
                void* raw_value{ locals[-index] };

                if constexpr (std::is_pointer_v<argument_type>)
                {
                    if (!raw_value)
                    {
                        return nullptr;
                    }
                    const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };

                    // HotSpot can expose object arguments either as:
                    //  - compressed oops (32-bit narrow value in the slot), or
                    //  - direct oop pointers (already decoded 64-bit address).
                    // Prefer decode only for narrow-looking values; otherwise use the
                    // direct pointer as-is.
                    if (raw_bits <= 0xFFFFFFFFull)
                    {
                        return reinterpret_cast<argument_type>(vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(raw_bits)));
                    }

                    return reinterpret_cast<argument_type>(raw_value);
                }
                else if constexpr (sizeof(argument_type) <= sizeof(void*))
                {
                    argument_type result{};
                    std::memcpy(&result, &raw_value, sizeof(argument_type));
                    return result;
                }
                else
                {
                    return argument_type{};
                }
            }
        };

        /*
            @brief Installs a low-level hook on a HotSpot interpreter-to-interpreter (i2i) stub.
            @details
            midi2i_hook patches the i2i interpreter stub of a Java method at the injection
            point found by find_hook_location() with a 5-byte relative JMP instruction that
            redirects execution to an allocated trampoline stub. The trampoline saves all
            volatile registers, calls common_detour with (frame*, java_thread*, return_slot*), and
            then either returns the custom value or resumes normal execution depending on
            whether the detour set the cancel flag.

            The trampoline stub is allocated within 32-bit relative JMP range of the target.
            The stub layout is: [original 8 bytes] [assembly trampoline].
            Only one trampoline is allocated per unique i2i entry point, even if multiple
            methods share the same stub.
        */
        class midi2i_hook final
        {
        public:
            /*
                @brief Installs the hook on the given target address.
                @param target  Pointer to the injection point within the i2i stub.
                @param detour  The C++ function to call when the hook fires.
            */
            midi2i_hook(std::uint8_t* const target, const vmhook::hotspot::detour_function_t detour)
                : target{ target }
                , allocated{ nullptr }
                , error{ true }
            {
                static constexpr std::int32_t HOOK_SIZE{ 8 };
                static constexpr std::int32_t JMP_SIZE{ 5 };
                static constexpr std::int32_t JE_OFFSET{ 0x32 };   // offset of je in assembly
                static constexpr std::int32_t JE_SIZE{ 6 };
                static constexpr std::int32_t RESUME_OFFSET{ 0x63 };
                static constexpr std::int32_t RESUME_JMP_OFFSET{ 0x73 };
                static constexpr std::int32_t RESUME_JMP_SIZE{ 5 };
                static constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x78 }; // offset of data slot
                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

                // Stack layout after the two pushes:
                //   [rsp+0]  return_slot::cancel  (bool, 1 byte; rest zeroed)
                //   [rsp+8]  return_slot::retval  (int64_t)
                std::uint8_t assembly[]
                {
                    0x50,                                           // push rax
                    0x51,                                           // push rcx
                    0x52,                                           // push rdx
                    0x41, 0x50,                                     // push r8
                    0x41, 0x51,                                     // push r9
                    0x41, 0x52,                                     // push r10
                    0x41, 0x53,                                     // push r11
                    0x55,                                           // push rbp
                    0x6A, 0x00,                                     // push 0x0  ; return_slot::retval  (slot +8)
                    0x6A, 0x00,                                     // push 0x0  ; return_slot::cancel  (slot +0)

                    0x48, 0x89, 0xE9,                               // mov rcx, rbp   ; frame*
                    0x4C, 0x89, 0xFA,                               // mov rdx, r15   ; java_thread*
                    0x4C, 0x8D, 0x04, 0x24,                         // lea r8, [rsp]  ; return_slot*

                    0x48, 0x89, 0xE5,                               // mov rbp, rsp
                    0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16
                    0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 0x20

                    0xFF, 0x15, 0x4D, 0x00, 0x00, 0x00,             // call [rip+0x4D]

                    0x48, 0x89, 0xEC,                               // mov rsp, rbp

                    0x80, 0x3C, 0x24, 0x00,                         // cmp byte ptr [rsp], 0
                    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je resume  ; cancel==false

                    // cancel path (cancel==true, falls through):
                    0x48, 0x8B, 0x44, 0x24, 0x08,                   // mov rax, [rsp+8]    ; return_slot::retval
                    0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax      ; float/double return
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10       ; discard return_slot
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x48, 0x83, 0xC4, 0x08,                         // add rsp, 0x8        ; discard saved original rax
                    0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8]
                    0x48, 0x89, 0xEC,                               // mov rsp, rbp
                    0x5D,                                           // pop rbp
                    0x5E,                                           // pop rsi
                    0x48, 0x89, 0xDC,                               // mov rsp, rbx
                    0xFF, 0xE6,                                     // jmp rsi

                    // resume path (cancel==false):
                    0x48, 0x83, 0xC4, 0x10,                         // add rsp, 0x10       ; discard return_slot
                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x58,                                           // pop rax
                    0xE9, 0x00, 0x00, 0x00, 0x00,                   // jmp target+HOOK_SIZE

                    // data slot: detour function pointer
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };

                this->allocated = vmhook::hotspot::allocate_nearby_memory(target, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READWRITE);

                try
                {
                    if (!this->allocated)
                    {
                        throw vmhook::exception{ "Failed to allocate memory for hook." };
                    }
                }
                catch (const std::exception& exception)
                {
                    VMHOOK_LOG("{} midi2i_hook::midi2i_hook {}", vmhook::error_tag, exception.what());
                    return;
                }

                const std::int32_t je_delta{ RESUME_OFFSET - (JE_OFFSET + JE_SIZE) };
                *reinterpret_cast<std::int32_t*>(assembly + JE_OFFSET + 2) = je_delta;

                const std::int32_t resume_jmp_delta{ static_cast<std::int32_t>(target + HOOK_SIZE - (this->allocated + HOOK_SIZE + RESUME_JMP_OFFSET + RESUME_JMP_SIZE)) };
                *reinterpret_cast<std::int32_t*>(assembly + RESUME_JMP_OFFSET + 1) = resume_jmp_delta;

                *reinterpret_cast<vmhook::hotspot::detour_function_t*>(assembly + DETOUR_ADDRESS_OFFSET) = detour;

                std::memcpy(this->allocated, target, HOOK_SIZE);
                std::memcpy(this->allocated + HOOK_SIZE, assembly, sizeof(assembly));

                DWORD old_protect{};
                VirtualProtect(this->allocated, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READ, &old_protect);
                VirtualProtect(target, JMP_SIZE, PAGE_EXECUTE_READWRITE, &old_protect);

                target[0] = JMP_OPCODE;
                const std::int32_t jmp_delta{ static_cast<std::int32_t>(this->allocated - (target + JMP_SIZE)) };
                *reinterpret_cast<std::int32_t*>(target + 1) = jmp_delta;

                VirtualProtect(target, JMP_SIZE, old_protect, &old_protect);

                this->error = false;
            }

            /*
                @brief Removes the hook and restores the original code at the target.
            */
            ~midi2i_hook()
            {
                if (this->error)
                {
                    return;
                }

                static constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

                DWORD old_protect{};
                if (this->target[0] == JMP_OPCODE && VirtualProtect(this->target, 5, PAGE_EXECUTE_READWRITE, &old_protect))
                {
                    std::memcpy(this->target, this->allocated, 5);
                    VirtualProtect(this->target, 5, old_protect, &old_protect);
                }

                VirtualFree(this->allocated, 0, MEM_RELEASE);
            }

            inline auto has_error() const noexcept -> bool
            {
                return this->error;
            }

        private:
            std::uint8_t* target{ nullptr };
            std::uint8_t* allocated{ nullptr };
            bool          error{ true };
        };

        /*
            @brief Stores the association between a HotSpot Method object and its C++ detour function.
            @details  Also holds the original entry points and _code pointer so shutdown_hooks()
                      can fully restore the method's state, including re-linking the nmethod if the
                      method was JIT-compiled when the hook was installed.
        */
        struct hooked_method
        {
            vmhook::hotspot::method* method{ nullptr };
            std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot*)> detour;
            void* original_code{ nullptr };
            void* original_from_interpreted_entry{ nullptr };
            void* original_from_compiled_entry{ nullptr };
            bool     was_compiled{ false };
        };

        /*
            @brief Stores the association between an i2i entry point and its installed midi2i_hook.
            @details
            Since multiple Java methods can share the same i2i stub, only one trampoline
            is allocated per unique i2i entry point.
        */
        struct i2i_hook_data
        {
            void* i2i_entry{ nullptr };
            vmhook::hotspot::midi2i_hook* hook{ nullptr };
        };

        /*
            @brief Global list of all currently hooked Java methods and their detour functions.
        */
        inline std::vector<vmhook::hotspot::hooked_method> g_hooked_methods{};

        /*
            @brief Global list of all i2i entry points that have been patched and their hooks.
        */
        inline std::vector<vmhook::hotspot::i2i_hook_data> g_hooked_i2i_entries{};

        /*
            @brief Common detour function invoked by the trampoline stub for every intercepted method call.
            @param f      Pointer to the current HotSpot interpreter frame (rbp at hook site).
            @param thread Pointer to the current HotSpot JavaThread (r15).
            @param slot Pointer to the return slot on the trampoline stack.
            @details
            Single entry point for all midi2i_hook trampolines.  Finds the matching
            per-method detour in g_hooked_methods and dispatches it.

            The thread-state precondition check was removed because the injection point
            (the `mov BYTE PTR [r15+X], Y` instruction) is reached while the thread may
            still be in a transition state on JDK 21+ builds - the state is not
            necessarily _thread_in_Java yet at that exact instruction.  After the user
            detour returns we force the state to _thread_in_Java so the bytecode
            dispatch that follows finds the correct state.
        */
        static auto common_detour(vmhook::hotspot::frame* const frame_pointer, vmhook::hotspot::java_thread* const thread, vmhook::hotspot::return_slot* const slot)
            -> void
        {
            try
            {
                if (!thread || !vmhook::hotspot::is_valid_pointer(thread))
                {
                    throw vmhook::exception{ "JavaThread pointer is null or invalid." };
                }

                const method* const current_method{ frame_pointer->get_method() };
                struct current_thread_guard
                {
                    vmhook::hotspot::java_thread* previous;

                    explicit current_thread_guard(vmhook::hotspot::java_thread* const thread_pointer) noexcept
                        : previous{ vmhook::hotspot::current_java_thread }
                    {
                        vmhook::hotspot::current_java_thread = thread_pointer;
                        vmhook::hotspot::last_java_thread.store(thread_pointer, std::memory_order_relaxed);
                    }

                    ~current_thread_guard()
                    {
                        vmhook::hotspot::current_java_thread = this->previous;
                    }
                } guard{ thread };

                for (const vmhook::hotspot::hooked_method& hook : vmhook::hotspot::g_hooked_methods)
                {
                    if (hook.method == current_method)
                    {
                        hook.detour(frame_pointer, thread, slot);
                        // Ensure the thread state is _thread_in_Java after the detour
                        // so the bytecode dispatcher finds a consistent state.
                        thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);
                        return;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                VMHOOK_LOG("{} common_detour() {}", vmhook::error_tag, exception.what());
            }
        }

        /*
            @brief Bitmask of HotSpot access flags used to disable JIT compilation on hooked methods.
            @details
            OR'd into Method._access_flags to prevent C1, C2, and OSR compilation.
            - JVM_ACC_NOT_C2_COMPILABLE   (0x02000000)
            - JVM_ACC_NOT_C1_COMPILABLE   (0x04000000)
            - JVM_ACC_NOT_C2_OSR_COMPILABLE (0x08000000)
            - JVM_ACC_QUEUED              (0x01000000)
        */
        inline constexpr std::int32_t NO_COMPILE =
            0x02000000 |
            0x04000000 |
            0x08000000 |
            0x01000000;

        /*
            @brief Enables or disables the _dont_inline flag on a HotSpot Method object.
            @details
            The _dont_inline flag is stored in Method._flags at bit position 2.
            When set, it prevents the JIT from inlining this method at any call site.
        */
        static auto set_dont_inline(const vmhook::hotspot::method* const method_pointer, const bool enabled) noexcept
            -> void
        {
            std::uint16_t* const flags{ method_pointer->get_flags() };
            if (!flags)
            {
                return;
            }

            if (enabled)
            {
                *flags |= (1 << 2);
            }
            else
            {
                *flags &= static_cast<std::uint16_t>(~(1 << 2));
            }
        }

        /*
            @brief Reads the c2i (compiled-to-interpreter) adapter entry from an AdapterHandlerEntry.
            @param adapter  AdapterHandlerEntry* stored in Method._adapter.
            @return The c2i entry address, or nullptr if not available.
            @details
            Used when deoptimising a hook to redirect Method._from_compiled_entry to the
            c2i adapter, so compiled callers that miss their inline cache re-enter the
            interpreter and reach our patched i2i stub.
            AdapterHandlerEntry._c2i_entry is exported via gHotSpotVMStructs on all
            supported JDK versions (8 – 26).
        */
        static auto get_c2i_entry_from_adapter(void* const adapter) noexcept
            -> void*
        {
            if (!adapter || !vmhook::hotspot::is_valid_pointer(adapter))
            {
                return nullptr;
            }
            static const vmhook::hotspot::vm_struct_entry_t* const entry{ vmhook::hotspot::iterate_struct_entries("AdapterHandlerEntry", "_c2i_entry") };
            if (!entry)
            {
                return nullptr;
            }

            return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(adapter) + entry->offset);
        }
    }

    namespace detail
    {
        inline auto jni_find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*;
    }

    // --- Cache and class lookup -----------------------------------------------

    /*
        @brief Cache of klass pointers keyed by their internal class name.
        @details
        Populated by find_class() on first lookup of each class name.
        Subsequent calls return the cached klass* directly without repeating
        the full ClassLoaderDataGraph walk.
    */
    inline std::unordered_map<std::string, vmhook::hotspot::klass*> klass_lookup_cache{};

    /*
        @brief Finds a loaded Java class by its internal name using HotSpot internals only.
        @param class_name The internal JVM class name using '/' separators
                          (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
        @return Pointer to the matching klass if found, nullptr otherwise.
        @details
        Searches all loaded classloaders for a class matching the given name by walking
        the HotSpot ClassLoaderDataGraph entirely via gHotSpotVMStructs, without any
        JNI or JVMTI calls. Results are cached on first lookup.
    */
    static auto find_class(const std::string_view class_name)
        -> vmhook::hotspot::klass*
    {
        const auto cache_entry{ vmhook::klass_lookup_cache.find(std::string{ class_name }) };
        if (cache_entry != vmhook::klass_lookup_cache.end())
        {
            return cache_entry->second;
        }

        try
        {
            const vmhook::hotspot::class_loader_data_graph graph{};
            vmhook::hotspot::klass* found_klass{ graph.find_klass(class_name) };

            if (!found_klass)
            {
                found_klass = vmhook::detail::jni_find_class_with_context_loader(class_name);
                if (!found_klass)
                {
                    return nullptr;
                }
            }

            vmhook::klass_lookup_cache.insert({ std::string{ class_name }, found_klass });
            return found_klass;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::find_class() for {}: {}", vmhook::error_tag, class_name, exception.what());
            return nullptr;
        }
    }

    /*
        @brief Associates a C++ type with its corresponding Java class name.
        @tparam T The C++ type to register.
        @param class_name The internal JVM class name using '/' separators.
        @return true if the class was found in the JVM and successfully registered,
                false if the class could not be found.
        @details
        Stores the mapping from the C++ type_index of T to class_name in type_to_class_map,
        then verifies the class exists in the JVM by calling find_class().
        Registration is required before calling hook<T>().
    */
    template<class wrapper_type>
    static auto register_class(const std::string_view class_name) noexcept
        -> bool
    {
        vmhook::hotspot::klass* const verified_klass{ vmhook::find_class(class_name) };

        if (!verified_klass)
        {
            VMHOOK_LOG("{} register_class() for {}: class not found in JVM.", vmhook::error_tag, class_name);
            return false;
        }

        vmhook::type_to_class_map.insert_or_assign(std::type_index{ typeid(wrapper_type) }, std::string{ class_name });

        // Store a factory function so field_proxy::get_as<T>() and frame::get_arguments()
        // can construct C++ wrapper objects from decoded Java object references.
        vmhook::g_type_factory_map.emplace(std::string{ class_name }, +[](void* instance)
            -> std::unique_ptr<object_base>
            {
                return std::make_unique<wrapper_type>(instance);
            }
        );

        return true;
    }

    // --- Internal helpers for typed hook API ---------------------------------

    namespace detail
    {
        /*
            Function trait helpers — extract the argument list from any callable so the
            typed hook() overload can deduce Java parameter types at compile time.
        */
        template<typename F, typename = void>
        struct function_traits;

        template<typename Ret, typename... Args>
        struct function_traits<Ret(*)(Args...)>
        {
            using args_tuple = std::tuple<Args...>;
        };

        template<typename Ret, typename... Args>
        struct function_traits<std::function<Ret(Args...)>>
        {
            using args_tuple = std::tuple<Args...>;
        };

        template<typename F>
        struct function_traits<F, std::void_t<decltype(&F::operator())>>
            : function_traits<decltype(&F::operator())> {
        };

        template<typename C, typename Ret, typename... Args>
        struct function_traits<Ret(C::*)(Args...) const>
        {
            using args_tuple = std::tuple<Args...>;
        };

        template<typename C, typename Ret, typename... Args>
        struct function_traits<Ret(C::*)(Args...)>
        {
            using args_tuple = std::tuple<Args...>;
        };

        // Drop the leading element (bool*) from a tuple type.
        template<typename Tuple>
        struct tuple_tail;

        template<typename First, typename... Rest>
        struct tuple_tail<std::tuple<First, Rest...>>
        {
            using type = std::tuple<Rest...>;
        };

        /*
            Extract a single Java method argument from the interpreter frame, converting
            the raw slot value to the requested C++ type T.
            Uses the public get_locals() path so it does not require friendship with frame.
              - std::string              → decoded OOP fed into read_java_string()
              - std::unique_ptr<U>       → decoded OOP fed into the registered factory for U
              - pointer types            → decoded compressed OOP
              - primitive / trivial types → raw slot bits via memcpy
        */
        template<typename T>
        auto extract_frame_arg(vmhook::hotspot::frame* const frame, const std::int32_t index)
            -> std::remove_cvref_t<T>
        {
            using base_t = std::remove_cvref_t<T>;

            void** const locals{ frame->get_locals() };
            if (!locals)
            {
                return base_t{};
            }

            void* const raw_value{ locals[-index] };
            const std::uintptr_t raw_bits{ reinterpret_cast<std::uintptr_t>(raw_value) };

            // Decode a compressed OOP (32-bit narrow value) to a full 64-bit pointer.
            auto decode_oop = [](void* rv) -> void*
                {
                    if (!rv) return nullptr;
                    const std::uintptr_t bits{ reinterpret_cast<std::uintptr_t>(rv) };
                    return (bits <= 0xFFFFFFFFull)
                        ? vmhook::hotspot::decode_oop_pointer(static_cast<std::uint32_t>(bits))
                        : rv;
                };

            if constexpr (std::is_same_v<base_t, std::string>)
            {
                return vmhook::read_java_string(decode_oop(raw_value));
            }
            else if constexpr (is_unique_ptr_v<base_t>)
            {
                using element_t = typename base_t::element_type;
                void* const oop{ decode_oop(raw_value) };
                if (!oop)
                {
                    return nullptr;
                }
                const auto type_it{ vmhook::type_to_class_map.find(std::type_index{ typeid(element_t) }) };
                if (type_it == vmhook::type_to_class_map.end())
                {
                    return nullptr;
                }
                const auto factory_it{ vmhook::g_type_factory_map.find(type_it->second) };
                if (factory_it == vmhook::g_type_factory_map.end())
                {
                    return nullptr;
                }
                return base_t{ static_cast<element_t*>(factory_it->second(oop).release()) };
            }
            else if constexpr (std::is_pointer_v<base_t>)
            {
                return reinterpret_cast<base_t>(decode_oop(raw_value));
            }
            else if constexpr (sizeof(base_t) <= sizeof(void*))
            {
                base_t result{};
                std::memcpy(&result, &raw_value, sizeof(base_t));
                return result;
            }
            else
            {
                return base_t{};
            }
        }

        template<typename T>
        struct is_unique_object_ptr : std::false_type {};

        template<typename value_type, typename deleter_type>
        struct is_unique_object_ptr<std::unique_ptr<value_type, deleter_type>>
            : std::bool_constant<std::is_base_of_v<vmhook::object_base, value_type>> {};

        inline auto jni_decode_object(void* object_handle) noexcept
            -> void*;

        inline auto jni_new_string_utf(std::string_view value) noexcept
            -> void*;
    } // namespace detail

    template<typename T>
    auto return_value::set_arg(const std::int32_t index, T&& value) noexcept
        -> bool
    {
        if (!this->frame_ || index < 0)
        {
            return false;
        }

        void** const locals{ this->frame_->get_locals() };
        if (!locals)
        {
            return false;
        }

        using value_type = std::remove_cvref_t<T>;

        auto store_oop = [&](void* const oop)
            -> bool
        {
            void* const previous_value{ locals[-index] };
            const std::uintptr_t previous_bits{ reinterpret_cast<std::uintptr_t>(previous_value) };

            if (!oop)
            {
                locals[-index] = nullptr;
                return true;
            }

            if (previous_bits > 0xFFFFFFFFull)
            {
                locals[-index] = oop;
                return true;
            }

            const std::uint32_t compressed{ vmhook::hotspot::encode_oop_pointer(oop) };
            locals[-index] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(compressed));
            return true;
        };

        if constexpr (vmhook::detail::is_unique_object_ptr<value_type>::value)
        {
            return store_oop(value.get()
                ? static_cast<const vmhook::object_base*>(value.get())->get_instance()
                : nullptr);
        }
        else if constexpr (std::is_base_of_v<vmhook::object_base, value_type>)
        {
            return store_oop(value.get_instance());
        }
        else if constexpr (std::is_same_v<value_type, std::string> || std::is_same_v<value_type, std::string_view>)
        {
            void* const string_handle{ vmhook::detail::jni_new_string_utf(value) };
            void* const string_oop{ string_handle
                ? vmhook::detail::jni_decode_object(string_handle)
                : vmhook::make_java_string(value) };
            if (!string_oop)
            {
                return false;
            }

            return store_oop(string_oop);
        }
        else if constexpr (std::is_same_v<value_type, const char*> || std::is_same_v<value_type, char*>)
        {
            const std::string_view text{ value ? std::string_view{ value } : std::string_view{} };
            void* const string_handle{ vmhook::detail::jni_new_string_utf(text) };
            void* const string_oop{ string_handle
                ? vmhook::detail::jni_decode_object(string_handle)
                : vmhook::make_java_string(text) };
            if (!string_oop)
            {
                return false;
            }

            return store_oop(string_oop);
        }
        else if constexpr (std::is_trivially_copyable_v<value_type> && sizeof(value_type) <= sizeof(void*))
        {
            void* raw{};
            std::memcpy(&raw, &value, sizeof(value_type));
            locals[-index] = raw;
            return true;
        }
        else
        {
            return false;
        }
    }

    // --- Hooking --------------------------------------------------------------

    /*
        @brief Installs an interpreter hook on a Java method with typed C++ parameters.
        @tparam wrapper_type The C++ wrapper class registered for the Java class that owns the method.
                             Must have been registered via register_class<T>() beforehand.
        @param method_name  The name of the Java method to hook (e.g., "toString", "update").
        @param user_detour  A callable with signature:
                              void(vmhook::return_value& retval, T1 arg1, T2 arg2, ...)
                            where T1, T2, ... correspond to the Java method's explicit parameters:
                              - int / boolean / byte / short / char / float  → std::int32_t / bool / ...
                              - long                                         → std::int64_t
                              - double                                       → double
                              - String                                       → std::string (or const std::string&)
                              - Object reference                             → std::unique_ptr<WrapperClass>
                                                                               (or const std::unique_ptr<...>&)
                            For instance methods the implicit Java 'this' occupies slot 0, so it must
                            appear as the first argument (typically std::unique_ptr<wrapper_type>).
                            For static methods there is no 'this'; parameters begin at slot 0.
                            Call retval.cancel() to suppress a void method body, or
                            retval.set(value) to suppress the original body and return value to Java.
        @return true if the hook was successfully installed or was already active, false on failure.
        @details
        The hook installation process:
        1. Retrieve the klass for the registered Java class via find_class().
        2. Walk the InstanceKlass::_methods array to locate the target method by name.
        3. Disable JIT compilation by setting NO_COMPILE in Method._access_flags
           and _dont_inline in Method._flags.
        4. Register the method and its detour in g_hooked_methods for dispatch by common_detour.
        5. If the method is already compiled, clear Method._code and restore the
           interpreted entry so future dispatch reaches the interpreter hook.
        6. Check whether the i2i entry point has already been patched; if so, reuse it.
        7. If the i2i entry is new, locate the injection point via find_hook_location(),
           allocate a trampoline via midi2i_hook, and register it in g_hooked_i2i_entries.

        @note Unlike the JNI/JVMTI version, this implementation does not force a class
              retransformation to flush existing inline caches. Hooking early is still best:
              compiled callers that already cached an nmethod can keep bypassing the hook
              until HotSpot repairs that call site at a safepoint.
        @see midi2i_hook, common_detour, set_dont_inline, NO_COMPILE, shutdown_hooks
    */
    template<class wrapper_type>
    static auto hook(const std::string_view method_name, auto&& user_detour)
        -> bool
    {
        try
        {
            using traits = vmhook::detail::function_traits<std::remove_cvref_t<decltype(user_detour)>>;
            using all_args_tuple = typename traits::args_tuple;
            using method_arg_tuple = typename vmhook::detail::tuple_tail<all_args_tuple>::type;

            const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                throw vmhook::exception{ std::format("Class not registered for type {}. Did you call register_class<wrapper_type>()?", typeid(wrapper_type).name()) };
            }

            vmhook::hotspot::klass* const target_klass{ vmhook::find_class(type_map_entry->second) };
            if (!target_klass)
            {
                throw vmhook::exception{ std::format("Class '{}' not found in JVM.", type_map_entry->second) };
            }

            const std::int32_t method_count{ target_klass->get_methods_count() };
            vmhook::hotspot::method** const methods_array{ target_klass->get_methods_ptr() };

            if (!methods_array || method_count <= 0)
            {
                throw vmhook::exception{ std::format("No methods found on class '{}'.", type_map_entry->second) };
            }

            vmhook::hotspot::method* found_method{ nullptr };
            for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
            {
                vmhook::hotspot::method* const method_ptr{ methods_array[method_index] };
                if (method_ptr && vmhook::hotspot::is_valid_pointer(method_ptr) && method_ptr->get_name() == method_name)
                {
                    found_method = method_ptr;
                    break;
                }
            }

            if (!found_method)
            {
                throw vmhook::exception{ std::format("Method '{}' not found in class '{}'.", method_name, type_map_entry->second) };
            }

            for (const vmhook::hotspot::hooked_method& hooked_method_entry : vmhook::hotspot::g_hooked_methods)
            {
                if (hooked_method_entry.method == found_method)
                {
                    return true;
                }
            }

            vmhook::hotspot::set_dont_inline(found_method, true);

            std::uint32_t* const flags{ found_method->get_access_flags() };
            if (!flags)
            {
                throw vmhook::exception{ "Failed to retrieve access flags." };
            }
            *flags |= vmhook::hotspot::NO_COMPILE;

            // -- Snapshot original entry points before any modification ----------
            void* const i2i{ found_method->get_i2i_entry() };
            if (!i2i)
            {
                throw vmhook::exception{ "Failed to retrieve i2i entry." };
            }

            void* const original_code{ found_method->get_code() };
            void* const original_from_interpreted{ found_method->get_from_interpreted_entry() };
            void* const original_from_compiled{ found_method->get_from_compiled_entry() };
            const bool was_compiled{ original_code != nullptr && vmhook::hotspot::is_valid_pointer(original_code) };

            if (was_compiled)
            {
                VMHOOK_LOG("{} hook(): '{}' is JIT-compiled (_code=0x{:016X}) - deoptimising.", vmhook::info_tag, method_name, reinterpret_cast<std::uintptr_t>(original_code));
            }
            else
            {
                VMHOOK_LOG("{} hook(): '{}' is interpreted - patching i2i stub.", vmhook::info_tag, method_name);
            }

            // Wrap the user callable: extract typed Java args from the frame and forward them.
            auto wrapper_detour = [detour = std::forward<decltype(user_detour)>(user_detour)]
            (vmhook::hotspot::frame* const frame_pointer, vmhook::hotspot::java_thread*, vmhook::hotspot::return_slot* const slot)
                {
                    vmhook::return_value retval{ slot, frame_pointer };
                    // Slots indexed from 0: instance methods have 'this' at slot 0.
                    auto invoke = [&]<std::size_t... Is>(std::index_sequence<Is...>)
                    {
                        detour(retval,
                            vmhook::detail::extract_frame_arg<std::tuple_element_t<Is, method_arg_tuple>>(
                                frame_pointer, static_cast<std::int32_t>(Is))...);
                    };
                    invoke(std::make_index_sequence<std::tuple_size_v<method_arg_tuple>>{});
                };

            vmhook::hotspot::g_hooked_methods.push_back({ found_method, std::move(wrapper_detour), original_code, original_from_interpreted, original_from_compiled, was_compiled });

            // -- Install (or reuse) the i2i stub patch ---------------------------
            bool i2i_already_patched{ false };
            for (const vmhook::hotspot::i2i_hook_data& hook_data_entry : vmhook::hotspot::g_hooked_i2i_entries)
            {
                if (hook_data_entry.i2i_entry == i2i)
                {
                    i2i_already_patched = true;
                    break;
                }
            }

            if (!i2i_already_patched)
            {
                std::uint8_t* const target{ reinterpret_cast<std::uint8_t*>(vmhook::hotspot::find_hook_location(i2i)) };
                if (!target)
                {
                    throw vmhook::exception{ "Failed to find hook location in i2i stub." };
                }

                vmhook::hotspot::midi2i_hook* const hook_instance{ new vmhook::hotspot::midi2i_hook(target, vmhook::hotspot::common_detour) };
                if (hook_instance->has_error())
                {
                    delete hook_instance;
                    throw vmhook::exception{ "midi2i_hook installation failed." };
                }

                vmhook::hotspot::g_hooked_i2i_entries.push_back({ i2i, hook_instance });
            }

            // -- Deoptimise JIT-compiled methods ---------------------------------
            // Problem:  when _code != nullptr, _from_interpreted_entry points to the i2c
            //           adapter (not the i2i stub), so calls bypass our patch entirely.
            // Fix:      null _code and reset the interpreted entry so the JVM dispatches
            //           through the interpreter - and therefore through our patched i2i stub.
            // Limitation: compiled callers with stale monomorphic inline caches still call
            //             the old nmethod directly.  Those caches will be repaired the next
            //             time HotSpot reaches a safe point and re-evaluates the IC.
            if (was_compiled)
            {
                void* const adapter{ found_method->get_adapter() };
                void* const c2i_entry{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };
                // 1. Redirect interpreted callers to the (now-patched) i2i stub.
                found_method->set_from_interpreted_entry(i2i);

                // 2. Redirect compiled callers through the c2i adapter → interpreter → i2i stub.
                if (c2i_entry && vmhook::hotspot::is_valid_pointer(c2i_entry))
                {
                    found_method->set_from_compiled_entry(c2i_entry);
                    VMHOOK_LOG("{} hook():   _from_compiled_entry → c2i @ 0x{:016X}", vmhook::info_tag, reinterpret_cast<std::uintptr_t>(c2i_entry));
                }
                else
                {
                    // Do not point compiled callers directly at i2i: the compiled-call ABI
                    // expects a c2i adapter. Leaving this entry unchanged is safer; once
                    // _code is cleared, normal interpreted dispatch reaches the hook.
                    VMHOOK_LOG("{} hook():   c2i adapter unavailable; leaving _from_compiled_entry unchanged.", vmhook::info_tag);
                }

                // 3. Clear _code last so the above entry-point writes are visible first.
                found_method->set_code(nullptr);
                VMHOOK_LOG("{} hook():   _code cleared - method running via interpreter.", vmhook::info_tag);
            }

            return true;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::hook() for {}: {}", vmhook::error_tag, method_name, exception.what());
            return false;
        }
    }

    /*
        @brief Removes all active interpreter hooks and restores the JVM to its original state.
        @details
        Phase 1: Deletes each midi2i_hook instance, restoring original bytes and freeing memory.
        Phase 2: Clears _dont_inline and NO_COMPILE flags.
        Phase 3: For methods that were JIT-compiled at hook-install time, restores the original
                 entry points and re-links _code so the nmethod is active again.
        Order within phase 3:  entry points first, then _code - this ensures callers have a valid
        destination before the JVM re-enables compiled dispatch.
    */
    static auto shutdown_hooks() noexcept
        -> void
    {
        for (const vmhook::hotspot::i2i_hook_data& hook_data_entry : vmhook::hotspot::g_hooked_i2i_entries)
        {
            delete hook_data_entry.hook;
        }

        for (const vmhook::hotspot::hooked_method& hooked_method_entry : vmhook::hotspot::g_hooked_methods)
        {
            vmhook::hotspot::set_dont_inline(hooked_method_entry.method, false);

            std::uint32_t* const flags{ hooked_method_entry.method->get_access_flags() };
            if (flags)
            {
                *flags &= static_cast<std::uint32_t>(~vmhook::hotspot::NO_COMPILE);
            }

            if (hooked_method_entry.was_compiled)
            {
                if (hooked_method_entry.original_from_compiled_entry)
                {
                    hooked_method_entry.method->set_from_compiled_entry(hooked_method_entry.original_from_compiled_entry);
                }
                if (hooked_method_entry.original_from_interpreted_entry)
                {
                    hooked_method_entry.method->set_from_interpreted_entry(hooked_method_entry.original_from_interpreted_entry);
                }
                if (hooked_method_entry.original_code)
                {
                    hooked_method_entry.method->set_code(hooked_method_entry.original_code);
                }
            }
        }

        vmhook::hotspot::g_hooked_methods.clear();
        vmhook::hotspot::g_hooked_i2i_entries.clear();
    }

    namespace detail
    {
        union jni_value
        {
            bool z;
            std::int8_t b;
            std::uint16_t c;
            std::int16_t s;
            std::int32_t i;
            std::int64_t j;
            float f;
            double d;
            void* l;
        };

        template<std::size_t index, typename function_type>
        inline auto jni_function(void* const env) noexcept
            -> function_type
        {
            if (!env)
            {
                return nullptr;
            }

            void** const table{ *reinterpret_cast<void***>(env) };
            if (!table)
            {
                return nullptr;
            }

            return reinterpret_cast<function_type>(table[index]);
        }

        inline auto jni_decode_object(void* const object_handle) noexcept
            -> void*
        {
            if (!object_handle)
            {
                return nullptr;
            }

            void* const oop{ *reinterpret_cast<void**>(object_handle) };
            return vmhook::hotspot::is_valid_pointer(oop) ? oop : nullptr;
        }

        inline auto jni_oop_handle(void* const oop, void*& handle_storage) noexcept
            -> void*
        {
            handle_storage = oop;
            return &handle_storage;
        }

        inline auto jni_find_class(const std::string_view class_name) noexcept
            -> void*
        {
            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                return nullptr;
            }

            using find_class_t = void* (*)(void*, const char*);
            find_class_t const find_class{ vmhook::detail::jni_function<6, find_class_t>(vmhook::hotspot::current_jni_env) };
            if (!find_class)
            {
                return nullptr;
            }

            const std::string name{ class_name };
            return find_class(vmhook::hotspot::current_jni_env, name.c_str());
        }

        inline auto jni_exception_clear() noexcept
            -> void
        {
            using exception_check_t = bool (*)(void*);
            using exception_clear_t = void (*)(void*);
            exception_check_t const exception_check{ vmhook::detail::jni_function<228, exception_check_t>(vmhook::hotspot::current_jni_env) };
            exception_clear_t const exception_clear{ vmhook::detail::jni_function<17, exception_clear_t>(vmhook::hotspot::current_jni_env) };
            if (exception_check && exception_clear && exception_check(vmhook::hotspot::current_jni_env))
            {
                exception_clear(vmhook::hotspot::current_jni_env);
            }
        }

        inline auto jni_get_object_class(void* const object_handle) noexcept
            -> void*
        {
            using get_object_class_t = void* (*)(void*, void*);
            get_object_class_t const get_object_class{ vmhook::detail::jni_function<31, get_object_class_t>(vmhook::hotspot::current_jni_env) };
            return get_object_class ? get_object_class(vmhook::hotspot::current_jni_env, object_handle) : nullptr;
        }

        inline auto jni_get_method_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            vmhook::detail::jni_exception_clear();
            using get_method_id_t = void* (*)(void*, void*, const char*, const char*);
            get_method_id_t const get_method_id{ vmhook::detail::jni_function<33, get_method_id_t>(vmhook::hotspot::current_jni_env) };
            void* const method_id{ get_method_id ? get_method_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr };
            if (!method_id)
            {
                vmhook::detail::jni_exception_clear();
            }
            return method_id;
        }

        inline auto jni_new_string_utf(const std::string_view value) noexcept
            -> void*;

        inline auto jni_get_static_method_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            using get_static_method_id_t = void* (*)(void*, void*, const char*, const char*);
            get_static_method_id_t const get_static_method_id{ vmhook::detail::jni_function<113, get_static_method_id_t>(vmhook::hotspot::current_jni_env) };
            return get_static_method_id ? get_static_method_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr;
        }

        inline auto jni_get_static_field_id(void* const klass, const std::string& name, const std::string& signature) noexcept
            -> void*
        {
            using get_static_field_id_t = void* (*)(void*, void*, const char*, const char*);
            get_static_field_id_t const get_static_field_id{ vmhook::detail::jni_function<144, get_static_field_id_t>(vmhook::hotspot::current_jni_env) };
            return get_static_field_id ? get_static_field_id(vmhook::hotspot::current_jni_env, klass, name.c_str(), signature.c_str()) : nullptr;
        }

        inline auto jni_get_static_object_field(void* const klass, void* const field_id) noexcept
            -> void*
        {
            using get_static_object_field_t = void* (*)(void*, void*, void*);
            get_static_object_field_t const get_static_object_field{ vmhook::detail::jni_function<145, get_static_object_field_t>(vmhook::hotspot::current_jni_env) };
            return get_static_object_field ? get_static_object_field(vmhook::hotspot::current_jni_env, klass, field_id) : nullptr;
        }

        inline auto jni_call_object_method(void* const object, void* const method_id, const vmhook::detail::jni_value* const args = nullptr) noexcept
            -> void*
        {
            using call_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_object_method_a_t const call_object_method_a{ vmhook::detail::jni_function<36, call_object_method_a_t>(vmhook::hotspot::current_jni_env) };
            return call_object_method_a ? call_object_method_a(vmhook::hotspot::current_jni_env, object, method_id, args) : nullptr;
        }

        inline auto jni_call_static_object_method(void* const klass, void* const method_id, const vmhook::detail::jni_value* const args = nullptr) noexcept
            -> void*
        {
            using call_static_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_static_object_method_a_t const call_static_object_method_a{ vmhook::detail::jni_function<116, call_static_object_method_a_t>(vmhook::hotspot::current_jni_env) };
            return call_static_object_method_a ? call_static_object_method_a(vmhook::hotspot::current_jni_env, klass, method_id, args) : nullptr;
        }

        inline auto jni_klass_from_class_mirror(void* const class_handle) noexcept
            -> vmhook::hotspot::klass*
        {
            void* const class_oop{ vmhook::detail::jni_decode_object(class_handle) };
            if (!class_oop || !vmhook::hotspot::is_valid_pointer(class_oop))
            {
                return nullptr;
            }

            static const vmhook::hotspot::vm_struct_entry_t* const klass_offset{ vmhook::hotspot::iterate_struct_entries("java_lang_Class", "_klass_offset") };
            if (!klass_offset || !klass_offset->address)
            {
                return nullptr;
            }

            const int offset{ *reinterpret_cast<const int*>(klass_offset->address) };
            void* const raw_klass{ const_cast<void*>(vmhook::hotspot::safe_read_pointer(reinterpret_cast<const std::uint8_t*>(class_oop) + offset)) };
            return vmhook::hotspot::is_valid_pointer(raw_klass) ? reinterpret_cast<vmhook::hotspot::klass*>(const_cast<void*>(vmhook::hotspot::untag_pointer(raw_klass))) : nullptr;
        }

        inline auto jni_find_class_with_context_loader(const std::string_view class_name) noexcept
            -> vmhook::hotspot::klass*
        {
            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                return nullptr;
            }

            auto load_with_loader = [&](void* const class_loader) noexcept
                -> vmhook::hotspot::klass*
            {
                if (!class_loader)
                {
                    return nullptr;
                }

                void* const class_loader_class{ vmhook::detail::jni_find_class("java/lang/ClassLoader") };
                if (!class_loader_class)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                void* const load_class_id{ vmhook::detail::jni_get_method_id(class_loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") };
                if (!load_class_id)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                std::string dotted_name{ class_name };
                std::replace(dotted_name.begin(), dotted_name.end(), '/', '.');
                void* const name_string{ vmhook::detail::jni_new_string_utf(dotted_name) };
                if (!name_string)
                {
                    vmhook::detail::jni_exception_clear();
                    return nullptr;
                }

                vmhook::detail::jni_value args[1]{};
                args[0].l = name_string;

                void* const class_mirror{ vmhook::detail::jni_call_object_method(class_loader, load_class_id, args) };
                vmhook::hotspot::klass* const klass{ vmhook::detail::jni_klass_from_class_mirror(class_mirror) };
                vmhook::detail::jni_exception_clear();
                return klass;
            };

            void* const thread_class{ vmhook::detail::jni_find_class("java/lang/Thread") };
            if (thread_class)
            {
                void* const current_thread_id{ vmhook::detail::jni_get_static_method_id(thread_class, "currentThread", "()Ljava/lang/Thread;") };
                void* const get_context_loader_id{ vmhook::detail::jni_get_method_id(thread_class, "getContextClassLoader", "()Ljava/lang/ClassLoader;") };
                if (current_thread_id && get_context_loader_id)
                {
                    void* const current_thread{ vmhook::detail::jni_call_static_object_method(thread_class, current_thread_id) };
                    void* const context_loader{ current_thread ? vmhook::detail::jni_call_object_method(current_thread, get_context_loader_id) : nullptr };
                    if (vmhook::hotspot::klass* const klass{ load_with_loader(context_loader) })
                    {
                        return klass;
                    }
                }
            }
            vmhook::detail::jni_exception_clear();

            void* const class_loader_class{ vmhook::detail::jni_find_class("java/lang/ClassLoader") };
            if (class_loader_class)
            {
                void* const get_system_loader_id{ vmhook::detail::jni_get_static_method_id(class_loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;") };
                void* const system_loader{ get_system_loader_id ? vmhook::detail::jni_call_static_object_method(class_loader_class, get_system_loader_id) : nullptr };
                if (vmhook::hotspot::klass* const klass{ load_with_loader(system_loader) })
                {
                    return klass;
                }
            }
            vmhook::detail::jni_exception_clear();

            void* const launch_class{ vmhook::detail::jni_find_class("net/minecraft/launchwrapper/Launch") };
            if (!launch_class)
            {
                vmhook::detail::jni_exception_clear();
                return nullptr;
            }

            void* const class_loader_field{ vmhook::detail::jni_get_static_field_id(launch_class, "classLoader", "Lnet/minecraft/launchwrapper/LaunchClassLoader;") };
            void* const launch_loader{ class_loader_field ? vmhook::detail::jni_get_static_object_field(launch_class, class_loader_field) : nullptr };
            if (vmhook::hotspot::klass* const klass{ load_with_loader(launch_loader) })
            {
                return klass;
            }

            vmhook::detail::jni_exception_clear();
            return nullptr;
        }

        inline auto jni_new_string_utf(const std::string_view value) noexcept
            -> void*
        {
            using new_string_utf_t = void* (*)(void*, const char*);
            new_string_utf_t const new_string_utf{ vmhook::detail::jni_function<167, new_string_utf_t>(vmhook::hotspot::current_jni_env) };
            if (!new_string_utf)
            {
                return nullptr;
            }

            const std::string text{ value };
            return new_string_utf(vmhook::hotspot::current_jni_env, text.c_str());
        }

        inline auto jni_get_string_utf(void* const string_handle) noexcept
            -> std::string
        {
            if (!string_handle)
            {
                return {};
            }

            using get_string_utf_chars_t = const char* (*)(void*, void*, bool*);
            using release_string_utf_chars_t = void (*)(void*, void*, const char*);
            get_string_utf_chars_t const get_string_utf_chars{ vmhook::detail::jni_function<169, get_string_utf_chars_t>(vmhook::hotspot::current_jni_env) };
            release_string_utf_chars_t const release_string_utf_chars{ vmhook::detail::jni_function<170, release_string_utf_chars_t>(vmhook::hotspot::current_jni_env) };
            if (!get_string_utf_chars)
            {
                return {};
            }

            bool is_copy{};
            const char* const chars{ get_string_utf_chars(vmhook::hotspot::current_jni_env, string_handle, &is_copy) };
            if (!chars)
            {
                return {};
            }

            std::string result{ chars };
            if (release_string_utf_chars)
            {
                release_string_utf_chars(vmhook::hotspot::current_jni_env, string_handle, chars);
            }
            return result;
        }

        template<typename arg_type>
        inline auto jni_signature_for_arg() noexcept
            -> std::string
        {
            using clean_t = std::decay_t<arg_type>;

            if constexpr (std::is_same_v<clean_t, std::string> || std::is_same_v<clean_t, std::string_view> || std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
            {
                return "Ljava/lang/String;";
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                return "Z";
            }
            else if constexpr (std::is_same_v<clean_t, std::int8_t> || std::is_same_v<clean_t, std::uint8_t>)
            {
                return "B";
            }
            else if constexpr (std::is_same_v<clean_t, std::int16_t>)
            {
                return "S";
            }
            else if constexpr (std::is_same_v<clean_t, std::uint16_t>)
            {
                return "C";
            }
            else if constexpr (std::is_same_v<clean_t, std::int64_t> || std::is_same_v<clean_t, std::uint64_t>)
            {
                return "J";
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                return "F";
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                return "D";
            }
            else
            {
                return "I";
            }
        }

        template<typename arg_type>
        inline auto append_jni_arg(std::vector<vmhook::detail::jni_value>& values, std::vector<void*>& object_handles, arg_type&& arg) noexcept
            -> void
        {
            using clean_t = std::decay_t<arg_type>;
            vmhook::detail::jni_value value{};

            if constexpr (std::is_same_v<clean_t, std::string>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
            }
            else if constexpr (std::is_same_v<clean_t, std::string_view>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg);
            }
            else if constexpr (std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
            {
                value.l = vmhook::detail::jni_new_string_utf(arg ? std::string_view{ arg } : std::string_view{});
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                {
                    object_handles.push_back(arg ? arg->get_instance() : nullptr);
                    value.l = object_handles.empty() ? nullptr : &object_handles.back();
                }
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
            {
                object_handles.push_back(arg.get_instance());
                value.l = &object_handles.back();
            }
            else if constexpr (std::is_same_v<clean_t, bool>)
            {
                value.z = arg;
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) <= sizeof(std::int32_t))
            {
                value.i = static_cast<std::int32_t>(arg);
            }
            else if constexpr (std::is_integral_v<clean_t> && sizeof(clean_t) == sizeof(std::int64_t))
            {
                value.j = static_cast<std::int64_t>(arg);
            }
            else if constexpr (std::is_same_v<clean_t, float>)
            {
                value.f = arg;
            }
            else if constexpr (std::is_same_v<clean_t, double>)
            {
                value.d = arg;
            }

            values.push_back(value);
        }

        template<typename... args_t>
        inline auto make_jni_args(std::vector<void*>& object_handles, args_t&&... args) noexcept
            -> std::vector<vmhook::detail::jni_value>
        {
            std::vector<vmhook::detail::jni_value> values{};
            values.reserve(sizeof...(args_t));
            object_handles.reserve(sizeof...(args_t));
            (vmhook::detail::append_jni_arg(values, object_handles, std::forward<args_t>(args)), ...);
            return values;
        }

        template<typename wrapper_type, typename... args_t>
        inline auto jni_make_unique(const std::string& class_name, args_t&&... args) noexcept
            -> std::unique_ptr<wrapper_type>
        {
            vmhook::hotspot::klass* const hotspot_klass{ vmhook::find_class(class_name) };
            if (!hotspot_klass)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): HotSpot class '{}' not found.", vmhook::error_tag, typeid(wrapper_type).name(), class_name);
                return nullptr;
            }

            void* const class_mirror{ hotspot_klass->get_java_mirror() };
            if (!class_mirror || !vmhook::hotspot::is_valid_pointer(class_mirror))
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): java.lang.Class mirror for '{}' is invalid.", vmhook::error_tag, typeid(wrapper_type).name(), class_name);
                return nullptr;
            }

            void* class_handle_storage{};
            void* const klass{ vmhook::detail::jni_oop_handle(class_mirror, class_handle_storage) };

            std::vector<void*> object_handles{};
            std::vector<vmhook::detail::jni_value> values{ vmhook::detail::make_jni_args(object_handles, std::forward<args_t>(args)...) };

            std::string signature{ "(" };
            ((signature += vmhook::detail::jni_signature_for_arg<std::remove_cvref_t<args_t>>()), ...);
            signature += ")V";

            void* const method_id{ vmhook::detail::jni_get_method_id(klass, "<init>", signature) };
            if (!method_id)
            {
                VMHOOK_LOG("{} jni_make_unique<{}>(): GetMethodID('<init>', '{}') failed.", vmhook::error_tag, typeid(wrapper_type).name(), signature);
                return nullptr;
            }

            using new_object_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            new_object_a_t const new_object_a{ vmhook::detail::jni_function<30, new_object_a_t>(vmhook::hotspot::current_jni_env) };
            if (!new_object_a)
            {
                return nullptr;
            }

            void* const object_handle{ new_object_a(vmhook::hotspot::current_jni_env, klass, method_id, values.data()) };
            void* const oop{ vmhook::detail::jni_decode_object(object_handle) };
            return oop ? std::make_unique<wrapper_type>(oop) : nullptr;
        }
    }

    /*
        @brief Constructs a new Java object and returns a C++ wrapper.
        @tparam T The C++ wrapper class (must derive from vmhook::object).
        @param args Arguments to pass to the Java constructor.
        @return A std::unique_ptr<T> wrapping the new Java object, or nullptr on failure.
        @details
        Looks up the Java class for type T (via register_class<T>()), allocates a new
        instance, and calls the appropriate constructor with the provided arguments.

        Usage:
            vmhook::register_class<player>("com/example/Player");
            auto p = vmhook::make_unique<player>("Bob", 12);

        @note This is a minimal implementation. Full constructor dispatch requires
              parsing method descriptors and setting up interpreter frames.
    */
    template<typename wrapper_type, typename... args_t>
    static auto make_unique(args_t&&... args)
        -> std::unique_ptr<wrapper_type>
    {
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): failed to attach current native thread to the JVM.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        auto map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
        if (map_entry == vmhook::type_to_class_map.end())
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): type not registered.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        if (!vmhook::detail::find_call_stub_entry())
        {
            if (std::unique_ptr<wrapper_type> jni_result{ vmhook::detail::jni_make_unique<wrapper_type>(map_entry->second, std::forward<args_t>(args)...) })
            {
                return jni_result;
            }
        }

        vmhook::hotspot::klass* const klass{ vmhook::find_class(map_entry->second) };
        if (!klass)
        {
            return nullptr;
        }

        const std::size_t raw_size{ klass->get_instance_size() };
        const std::size_t object_size{ (raw_size + 7u) & ~static_cast<std::size_t>(7u) };
        if (object_size == 0)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): failed to read HotSpot instance size.", vmhook::error_tag, typeid(wrapper_type).name());
            return nullptr;
        }

        vmhook::hotspot::java_thread* const thread{ vmhook::hotspot::find_allocation_thread() };
        void* object_pointer{};
        if (thread && vmhook::hotspot::is_valid_pointer(thread))
        {
            object_pointer = thread->allocate_tlab(object_size);
        }

        if (!object_pointer)
        {
            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* candidate{ vmhook::hotspot::find_any_java_thread() };
                candidate && vmhook::hotspot::is_valid_pointer(candidate) && visited_threads < 256;
                candidate = candidate->get_next(), ++visited_threads)
            {
                object_pointer = candidate->allocate_tlab(object_size);
                if (object_pointer)
                {
                    vmhook::hotspot::last_java_thread.store(candidate, std::memory_order_relaxed);
                    break;
                }
            }
        }

        if (!object_pointer)
        {
            object_pointer = vmhook::hotspot::allocate_from_threads_list(object_size);
        }

        if (!object_pointer)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): current JavaThread TLAB has no room for {} bytes.", vmhook::warning_tag, typeid(wrapper_type).name(), object_size);
            return nullptr;
        }

        std::memset(object_pointer, 0, object_size);

        static const vmhook::hotspot::vm_struct_entry_t* const mark_entry{ []()
            -> const vmhook::hotspot::vm_struct_entry_t*
            {
                {
                    auto* entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_mark") };
                    if (entry)
                    {
                        return entry;
                    }
                }
                return vmhook::hotspot::iterate_struct_entries("oopDesc", "_markWord");
            }()
        };
        static const vmhook::hotspot::vm_struct_entry_t* const compressed_klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._compressed_klass") };
        static const vmhook::hotspot::vm_struct_entry_t* const klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._klass") };

        const std::size_t mark_offset{ mark_entry ? static_cast<std::size_t>(mark_entry->offset) : 0u };
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + mark_offset) = klass->get_prototype_header();

        if (compressed_klass_entry)
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + compressed_klass_entry->offset) = vmhook::hotspot::encode_klass_pointer(klass);
        }
        else if (klass_entry)
        {
            *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(object_pointer) + klass_entry->offset) = klass;
        }
        else
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + 8u) = vmhook::hotspot::encode_klass_pointer(klass);
        }

        auto result{ std::make_unique<wrapper_type>(object_pointer) };

        if constexpr (requires(wrapper_type & wrapper, args_t&&... construct_args)
        {
            wrapper.construct(std::forward<args_t>(construct_args)...);
        })
        {
            result->construct(std::forward<args_t>(args)...);
        }
        else if constexpr (sizeof...(args_t) > 0)
        {
            VMHOOK_LOG("{} vmhook::make_unique<{}>(): object allocated, but wrapper has no matching construct(...) method for the provided arguments.", vmhook::warning_tag, typeid(wrapper_type).name());
        }

        return result;
    }

    // --- Field access ---------------------------------------------------------

    /*
        @brief Cache of field entries keyed by klass* then field name.
        @details
        Populated lazily by find_field() on the first lookup of each (klass, name) pair.
        Raw klass pointers are safe as keys for process-lifetime injection; class unloading
        would invalidate them, but that is not a concern for the typical use case here.
    */
    inline std::unordered_map<vmhook::hotspot::klass*, std::unordered_map<std::string, vmhook::hotspot::field_entry_t>> g_field_cache{};

    /*
        @brief Looks up and caches a field entry for a named field on a klass.
        @param target_klass  The klass that declares the field (obtain via find_class()).
                             Only the declaring class is searched - not superclasses.
        @param name          The exact Java field name.
        @return The cached field_entry_t, or std::nullopt if the field is not found.
        @details
        On the first call for a given (target_klass, name) pair the full InstanceKlass._fields
        array is walked; subsequent calls return the cached result directly.
    */
    static auto find_field(vmhook::hotspot::klass* const target_klass, const std::string_view name)
        -> std::optional<vmhook::hotspot::field_entry_t>
    {
        if (!target_klass || !vmhook::hotspot::is_valid_pointer(target_klass))
        {
            VMHOOK_LOG("{} vmhook::find_field() for '{}': klass pointer is null.", vmhook::error_tag, name);
            return std::nullopt;
        }

        auto& class_fields{ vmhook::g_field_cache[target_klass] };
        const std::string name_str{ name };

        if (const auto field_cache_entry{ class_fields.find(name_str) }; field_cache_entry != class_fields.end())
        {
            return field_cache_entry->second;
        }

        // Walk the superclass chain so that inherited fields are found.
        for (vmhook::hotspot::klass* k{ target_klass }; k != nullptr; k = k->get_super())
        {
            const auto entry{ k->find_field(name) };
            if (entry)
            {
                class_fields.emplace(name_str, *entry);
                return entry;
            }
        }

        VMHOOK_LOG("{} vmhook::find_field() for '{}': field not found in class hierarchy.", vmhook::error_tag, name);
        return std::nullopt;
    }

    /*
        @brief Reads an instance field from a Java object by name.
        @tparam T      The C++ scalar type to read the field as (int, float, bool,
                       double, long long, short, char, std::byte, uint32_t, etc.).
        @param object  Decoded pointer to the Java object (not a compressed OOP).
                       Obtain it from frame->get_arguments<void*>() which already
                       calls decode_oop_pointer() internally.
        @param k       The klass that declares the field (from find_class()).
        @param name    The exact Java field name.
        @return The field value as T, or a default-constructed T on failure.
        @note For reference-type fields specify T = uint32_t to receive the raw
              compressed OOP, then pass it to vmhook::hotspot::decode_oop_pointer() yourself.
    */
    template<typename value_type>
    static auto get_field(void* const object, vmhook::hotspot::klass* const target_klass, const std::string_view name)
        -> value_type
    {
        static_assert(std::is_trivially_copyable_v<value_type>, "get_field<value_type>: value_type must be trivially copyable.");

        try
        {
            const auto entry{ vmhook::find_field(target_klass, name) };

            if (!entry)
            {
                throw vmhook::exception{ std::format("Field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                void* const mirror{ target_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    throw vmhook::exception{ "Failed to retrieve java.lang.Class mirror." };
                }

                value_type result{};
                std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(mirror) + entry->offset, sizeof(value_type));
                return result;
            }

            if (!object || !vmhook::hotspot::is_valid_pointer(object))
            {
                throw vmhook::exception{ "Object pointer is null or invalid." };
            }

            value_type result{};
            std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(object) + entry->offset, sizeof(value_type));
            return result;
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::get_field<{}>('{}') {}", vmhook::error_tag, typeid(value_type).name(), name, exception.what());
            return value_type{};
        }
    }

    /*
        @brief Writes a value to an instance field of a Java object.
        @tparam T      The C++ scalar type to write.
        @param object  Decoded pointer to the Java object instance.
        @param k       The klass that declares the field.
        @param name    The exact Java field name.
        @param value   The value to write.
        @note Writing a reference-type field requires a properly encoded compressed OOP.
              Encoding is: (real_address - narrow_oop_base) >> narrow_oop_shift.
    */
    template<typename value_type>
    static auto set_field(void* const object, vmhook::hotspot::klass* const target_klass, const std::string_view name, const value_type value)
        -> void
    {
        static_assert(std::is_trivially_copyable_v<value_type>, "set_field<value_type>: value_type must be trivially copyable.");

        try
        {
            const auto entry{ vmhook::find_field(target_klass, name) };

            if (!entry)
            {
                throw vmhook::exception{ std::format("Field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                void* const mirror{ target_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    throw vmhook::exception{ "Failed to retrieve java.lang.Class mirror." };
                }

                std::memcpy(reinterpret_cast<std::uint8_t*>(mirror) + entry->offset, &value, sizeof(value_type));
                return;
            }

            if (!object || !vmhook::hotspot::is_valid_pointer(object))
            {
                throw vmhook::exception{ "Object pointer is null or invalid." };
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(object) + entry->offset, &value, sizeof(value_type));
        }
        catch (const std::exception& exception)
        {
            VMHOOK_LOG("{} vmhook::set_field<{}>('{}') {}", vmhook::error_tag, typeid(value_type).name(), name, exception.what());
        }
    }

    inline auto make_java_object(vmhook::hotspot::klass* const klass, const std::size_t requested_size) noexcept
        -> void*
    {
        if (!vmhook::hotspot::ensure_current_java_thread())
        {
            return nullptr;
        }

        if (!klass || requested_size == 0)
        {
            return nullptr;
        }

        const std::size_t object_size{ (requested_size + 7u) & ~static_cast<std::size_t>(7u) };
        vmhook::hotspot::java_thread* const thread{ vmhook::hotspot::find_allocation_thread() };

        void* object_pointer{};
        if (thread && vmhook::hotspot::is_valid_pointer(thread))
        {
            object_pointer = thread->allocate_tlab(object_size);
        }

        if (!object_pointer)
        {
            std::int32_t visited_threads{ 0 };
            for (vmhook::hotspot::java_thread* candidate{ vmhook::hotspot::find_any_java_thread() };
                candidate && vmhook::hotspot::is_valid_pointer(candidate) && visited_threads < 256;
                candidate = candidate->get_next(), ++visited_threads)
            {
                object_pointer = candidate->allocate_tlab(object_size);
                if (object_pointer)
                {
                    vmhook::hotspot::last_java_thread.store(candidate, std::memory_order_relaxed);
                    break;
                }
            }
        }

        if (!object_pointer)
        {
            object_pointer = vmhook::hotspot::allocate_from_threads_list(object_size);
        }

        if (!object_pointer)
        {
            return nullptr;
        }

        std::memset(object_pointer, 0, object_size);

        static const vmhook::hotspot::vm_struct_entry_t* const mark_entry{ []()
            -> const vmhook::hotspot::vm_struct_entry_t*
            {
                if (auto* const entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_mark") })
                {
                    return entry;
                }

                return vmhook::hotspot::iterate_struct_entries("oopDesc", "_markWord");
            }()
        };
        static const vmhook::hotspot::vm_struct_entry_t* const compressed_klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._compressed_klass") };
        static const vmhook::hotspot::vm_struct_entry_t* const klass_entry{ vmhook::hotspot::iterate_struct_entries("oopDesc", "_metadata._klass") };

        const std::size_t mark_offset{ mark_entry ? static_cast<std::size_t>(mark_entry->offset) : 0u };
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + mark_offset) = klass->get_prototype_header();

        if (compressed_klass_entry)
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + compressed_klass_entry->offset) = vmhook::hotspot::encode_klass_pointer(klass);
        }
        else if (klass_entry)
        {
            *reinterpret_cast<vmhook::hotspot::klass**>(reinterpret_cast<std::uint8_t*>(object_pointer) + klass_entry->offset) = klass;
        }
        else
        {
            *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(object_pointer) + 8u) = vmhook::hotspot::encode_klass_pointer(klass);
        }

        return object_pointer;
    }

    inline auto make_java_array(const std::string_view class_name, const std::int32_t length, const std::size_t element_size) noexcept
        -> void*
    {
        if (length < 0)
        {
            return nullptr;
        }

        vmhook::hotspot::klass* const array_klass{ vmhook::find_class(class_name) };
        if (!array_klass)
        {
            return nullptr;
        }

        constexpr std::size_t array_header_size{ 16u };
        void* const array_oop{ vmhook::make_java_object(array_klass, array_header_size + static_cast<std::size_t>(length) * element_size) };
        if (!array_oop)
        {
            return nullptr;
        }

        *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uint8_t*>(array_oop) + 12u) = length;
        return array_oop;
    }

    inline auto make_java_string(const std::string_view value) noexcept
        -> void*
    {
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return nullptr;
        }

        void* const string_oop{ vmhook::make_java_object(string_klass, string_klass->get_instance_size()) };
        if (!string_oop)
        {
            return nullptr;
        }

        const bool compact_string{ string_klass->find_field("coder").has_value() };
        const std::int32_t length{ static_cast<std::int32_t>(std::min<std::size_t>(value.size(), 4096u)) };

        if (compact_string)
        {
            void* const value_array{ vmhook::make_java_array("[B", length, sizeof(std::uint8_t)) };
            if (!value_array)
            {
                return nullptr;
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(value_array) + 16u, value.data(), static_cast<std::size_t>(length));
            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));
            vmhook::set_field<std::uint8_t>(string_oop, string_klass, "coder", 0u);
        }
        else
        {
            void* const value_array{ vmhook::make_java_array("[C", length, sizeof(std::uint16_t)) };
            if (!value_array)
            {
                return nullptr;
            }

            auto* const chars{ reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(value_array) + 16u) };
            for (std::int32_t index{ 0 }; index < length; ++index)
            {
                chars[index] = static_cast<std::uint8_t>(value[static_cast<std::size_t>(index)]);
            }

            vmhook::set_field(string_oop, string_klass, "value", vmhook::hotspot::encode_oop_pointer(value_array));

            if (string_klass->find_field("offset").has_value())
            {
                vmhook::set_field<std::int32_t>(string_oop, string_klass, "offset", 0);
            }

            if (string_klass->find_field("count").has_value())
            {
                vmhook::set_field<std::int32_t>(string_oop, string_klass, "count", length);
            }
        }

        if (string_klass->find_field("hash").has_value())
        {
            vmhook::set_field<std::int32_t>(string_oop, string_klass, "hash", 0);
        }

        return string_oop;
    }

    // --- Array element access -------------------------------------------------

    /*
        @brief Reads the length of a Java primitive array.
        @param array_oop Decoded pointer to the Java array object (not compressed).
        @return Element count, or 0 if the pointer is invalid.
        @details
        HotSpot Array object layout (x64, compressed OOPs):
          +0  mark word (8 B)
          +8  klass pointer (4 B)
          +12 _length   (int)
          +16 _data[0]
    */
    inline static auto array_length(void* const array_oop) noexcept
        -> std::int32_t
    {
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return 0;
        }

        return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(array_oop) + 12);
    }

    /*
        @brief Reads the element at the given index from a Java primitive array.
        @tparam T    The C++ element type (int32_t, double, int8_t, bool, etc.).
        @param array_oop Decoded pointer to the Java array object.
        @param index Zero-based element index.
        @return The element value, or T{} if the pointer/index is invalid.
        @details
        Data starts at offset +16 from the array oop.  Element stride is sizeof(T).
        Bounds checking is performed against the array length.
    */
    template<typename element_type>
    static auto get_array_element(void* const array_oop, const std::int32_t index)
        -> element_type
    {
        static_assert(std::is_trivially_copyable_v<element_type>, "get_array_element<element_type>: element_type must be trivially copyable.");
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return element_type{};
        }
        const std::int32_t length{ array_length(array_oop) };
        if (index < 0 || index >= length)
        {
            return element_type{};
        }

        element_type result{};
        std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(array_oop) + 16 + index * static_cast<std::int32_t>(sizeof(element_type)), sizeof(element_type));
        return result;
    }

    /*
        @brief Writes a value into a Java primitive array at the given index.
        @tparam T    The C++ element type.
        @param array_oop Decoded pointer to the Java array object.
        @param index Zero-based element index.
        @param value The value to write.
    */
    template<typename element_type>
    static auto set_array_element(void* const array_oop, const std::int32_t index, const element_type value)
        -> void
    {
        static_assert(std::is_trivially_copyable_v<element_type>, "set_array_element<element_type>: element_type must be trivially copyable.");
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return;
        }
        const std::int32_t length{ array_length(array_oop) };
        if (index < 0 || index >= length)
        {
            return;
        }
        std::memcpy(reinterpret_cast<std::uint8_t*>(array_oop) + 16 + index * static_cast<std::int32_t>(sizeof(element_type)), &value, sizeof(element_type));
    }

    // --- Field proxy ----------------------------------------------------------

        /*
            @brief Lightweight proxy to a single Java field value in memory.
            @details
            Returned by vmhook::object::get_field().  Reads the field value on demand,
            dispatches the correct C++ type from the JVM type descriptor (signature),
            and returns a typed copy - not a raw-pointer alias.

            Usage inside a wrapper class:

                // No trailing return type needed - auto deduces field_proxy::value_t,
                // which converts implicitly to the target type at the call site.
                 auto is_connected() -> bool { return get_field("isConnected")->get(); }
                 auto get_health()   -> int { return get_field("health")->get(); }

                 // If you want a concrete type inside the method, cast before returning:
                 auto get_max_hp()   -> int { return static_cast<int>(get_field("maxHp")->get()); }

            Usage at the call site:
                bool ok  = client.is_connected();   // operator bool()  fires
                int  hp  = client.get_health();     // operator int()   fires
                if  (client.is_connected()) { ... } // contextual bool conversion

            Writing:
                obj.get_field("health")->set(100);  // T = int, deduced
                obj.get_field("flag"  )->set(true); // T = bool, deduced
        */
    class field_proxy final
    {
    public:
        /*
            @brief A typed copy of the field's value.
            @details
            Holds one alternative from a variant whose alternatives cover every JVM
            primitive type and compressed-OOP references.  get() selects the
            alternative that matches the field's JVM type descriptor before returning,
            so the value is already correctly cast.

            Implicitly converts to any type T via std::visit + static_cast, which
            means you can write:

                bool b  = proxy->get();   // contextual / assignment conversion
                int  i  = proxy->get();
                float f = proxy->get();
                auto  v = proxy->get();   // type is field_proxy::value_t; converts lazily

            For reference-type fields (signature starts with 'L' or '[') the stored
            alternative is uint32_t (the raw compressed OOP).  Pass it to
            vmhook::hotspot::decode_oop_pointer() to recover the real 64-bit address.
        */
        struct value_t
        {
            std::variant<
                bool,
                std::int8_t,
                std::int16_t,
                std::int32_t,
                std::int64_t,
                float,
                double,
                std::uint16_t,
                std::uint32_t   // reference / array (compressed OOP)
            > data;
            std::string signature{};

            static auto append_array_value(std::vector<bool>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                result.push_back(vmhook::get_array_element<std::uint8_t>(array_oop, index) != 0);
            }

            static auto append_array_value(std::vector<std::string>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                const std::uint32_t element_compressed{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
                result.push_back(vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(element_compressed)));
            }

            static auto append_array_value(std::vector<char>& result, void* const array_oop, const std::int32_t index, const std::string_view signature) noexcept
                -> void
            {
                if (signature == "[C")
                {
                    result.push_back(static_cast<char>(vmhook::get_array_element<std::uint16_t>(array_oop, index)));
                }
                else
                {
                    result.push_back(vmhook::get_array_element<char>(array_oop, index));
                }
            }

            template<typename element_type>
            static auto append_array_value(std::vector<element_type>& result, void* const array_oop, const std::int32_t index, std::string_view) noexcept
                -> void
            {
                result.push_back(vmhook::get_array_element<element_type>(array_oop, index));
            }

            /*
               @brief Converts the stored value to target_type via static_cast.
               Works for any combination of the nine stored types and any
               numeric or bool target type.
           */
            template<typename target_type>
            static auto read_array_value(const std::uint32_t compressed, const std::string_view signature) noexcept
                -> target_type
            {
                target_type result{};
                void* const array_oop{ vmhook::decode_array_oop(compressed) };
                if (!array_oop)
                {
                    return result;
                }

                const std::int32_t length{ vmhook::array_length(array_oop) };
                if (length <= 0)
                {
                    return result;
                }

                result.reserve(static_cast<std::size_t>(length));
                for (std::int32_t index{ 0 }; index < length; ++index)
                {
                    append_array_value(result, array_oop, index, signature);
                }

                return result;
            }

            template<typename target_type, typename source_type>
            auto cast_for_variant(source_type value) const noexcept
                -> target_type
            {
                using clean_target_type = std::remove_cvref_t<target_type>;
                using clean_source_type = std::remove_cvref_t<source_type>;

                if constexpr (std::is_same_v<clean_target_type, std::string>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(value));
                    }
                    else
                    {
                        return {};
                    }
                }
                else if constexpr (vmhook::detail::is_vector_v<clean_target_type>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return read_array_value<clean_target_type>(value, this->signature);
                    }
                    else
                    {
                        return {};
                    }
                }
                else if constexpr (vmhook::detail::is_unique_ptr_v<clean_target_type>)
                {
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        using wrapper_type = typename clean_target_type::element_type;
                        void* const decoded{ vmhook::hotspot::decode_oop_pointer(value) };
                        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
                        {
                            return nullptr;
                        }
                        return clean_target_type{ new wrapper_type{ decoded } };
                    }
                    else
                    {
                        return nullptr;
                    }
                }
                else if constexpr (std::is_same_v<target_type, void*>)
                {
                    // For void*, only convert from uint32_t (compressed OOP)
                    if constexpr (std::is_same_v<clean_source_type, std::uint32_t>)
                    {
                        return vmhook::hotspot::decode_oop_pointer(value);
                    }
                    else
                    {
                        return static_cast<target_type>(nullptr);
                    }
                }
                else if constexpr (requires { static_cast<target_type>(value); })
                {
                    return static_cast<target_type>(value);
                }
                else
                {
                    return target_type{};
                }
            }

            template<typename target_type>
            operator target_type() const noexcept
            {
                return std::visit([this](auto value) noexcept
                    -> target_type
                    {
                        return this->cast_for_variant<target_type>(value);
                    }, data);
            }

            template<typename element_type>
            auto to_vector() const
                -> std::vector<std::unique_ptr<element_type>>;
        };

        /*
            @param field_pointer Direct pointer to the field's bytes in JVM memory
                                 (decoded object address + offset for instance fields;
                                  java.lang.Class mirror + offset for static fields).
            @param signature     JVM type descriptor, e.g. "I", "Z", "Ljava/lang/String;"
            @param is_static     true when JVM_ACC_STATIC is set on the field.
        */
        field_proxy(void* field_pointer, std::string sig, const bool is_static_flag) noexcept
            : field_pointer{ field_pointer }
            , m_signature{ std::move(sig) }
            , m_is_static{ is_static_flag }
        {
        }

        /*
            @brief Reads the field and returns a typed copy.
            @details
            Dispatches on the JVM type descriptor to determine how many bytes to
            read and which variant alternative to populate:
              "Z" → bool       "B" → int8_t    "S" → int16_t   "I" → int32_t
              "J" → int64_t    "F" → float     "D" → double    "C" → uint16_t
              "L…"/"[…" → uint32_t (compressed OOP)
            The returned value is a copy - safe to store and return from methods.
        */
        auto get() const noexcept
            -> value_t
        {
            if (!this->field_pointer)
            {
                return value_t{ std::int32_t{}, this->m_signature };
            }

            if (this->m_signature == "Z")
            {
                bool value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "B")
            {
                std::int8_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "S")
            {
                std::int16_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "I")
            {
                std::int32_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "J")
            {
                std::int64_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "F")
            {
                float value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "D")
            {
                double value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }
            if (this->m_signature == "C")
            {
                std::uint16_t value{};
                std::memcpy(&value, this->field_pointer, sizeof(value));
                return value_t{ value, this->m_signature };
            }

            // Reference or array type - store compressed OOP
            std::uint32_t value{};
            std::memcpy(&value, this->field_pointer, sizeof(value));
            return value_t{ value, this->m_signature };
        }

        /*
            @brief Writes value into the field's storage.
            @details
            This is the only field setter exposed by field_proxy. It accepts JVM
            primitives, std::string for java.lang.String, and std::vector<T> for
            Java arrays. String and array writes update the existing Java object
            in place because this zero-JNI layer cannot resize Java heap objects.
        */
        template<typename value_type>
        auto set(const value_type& value) const noexcept
            -> void
        {
            using clean_value_type = std::remove_cvref_t<value_type>;

            if constexpr (std::is_same_v<clean_value_type, std::string>)
            {
                vmhook::set_str_field(*this, value);
            }
            else if constexpr (std::is_convertible_v<value_type, std::string_view> && !std::is_same_v<clean_value_type, std::string>)
            {
                vmhook::set_str_field(*this, std::string_view{ value });
            }
            else if constexpr (vmhook::detail::is_vector_v<clean_value_type>)
            {
                if constexpr (std::is_same_v<clean_value_type, std::vector<bool>>)
                {
                    vmhook::set_bool_array(*this, value);
                }
                else if constexpr (std::is_same_v<clean_value_type, std::vector<std::string>>)
                {
                    vmhook::set_str_array(*this, value);
                }
                else
                {
                    vmhook::set_prim_array(*this, value);
                }
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_value_type>)
            {
                if (this->field_pointer)
                {
                    const std::uint32_t compressed{ value.get() ? vmhook::hotspot::encode_oop_pointer(static_cast<const vmhook::object_base*>(value.get())->get_instance()) : 0 };
                    std::memcpy(this->field_pointer, &compressed, sizeof(compressed));
                }
            }
            else if constexpr (std::is_trivially_copyable_v<clean_value_type>)
            {
                if (this->field_pointer)
                {
                    if (this->m_signature == "C" && sizeof(clean_value_type) == sizeof(char))
                    {
                        const std::uint16_t wide_value{ static_cast<std::uint16_t>(static_cast<unsigned char>(value)) };
                        std::memcpy(this->field_pointer, &wide_value, sizeof(wide_value));
                    }
                    else
                    {
                        std::memcpy(this->field_pointer, &value, sizeof(clean_value_type));
                    }
                }
            }
        }

        /*
            @brief Returns the JVM type descriptor of this field (e.g. "I", "Z", "J").
        */
        auto signature() const noexcept
            -> std::string_view
        {
            return this->m_signature;
        }

        auto is_static() const noexcept
            -> bool
        {
            return this->m_is_static;
        }

        /*
            @brief Returns the compressed OOP stored in this field (for reference/array types).
            @return The compressed OOP as uint32_t, or 0 if field_pointer is null.
        */
        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
            if (!this->field_pointer)
            {
                return 0;
            }
            std::uint32_t value{};
            std::memcpy(&value, this->field_pointer, sizeof(value));
            return value;
        }

    private:
        void* field_pointer;
        std::string m_signature;
        bool        m_is_static;
    };

    // --- detail helpers that depend on hotspot types -------------------------
    // (reopened here because find_call_stub_entry references hotspot types
    //  that are not yet defined at the earlier detail block position)
    namespace detail
    {
        /*
            @brief Locates StubRoutines::_call_stub_entry via VMStructs.

            HotSpot's official C++→Java call gate.  Creates a properly-typed
            "entry frame" so the GC, exception handler, and frame-walker all
            recognise the native→Java boundary.  Unlike jumping directly to a
            c2i adapter, this prevents JVM fatal errors when the frame-walker
            encounters an unexpected native frame above the interpreter frame.

            Stub signature (Windows x64):
              rcx  = link (−1 sentinel)
              rdx  = intptr_t* result holder
              r8   = BasicType (T_INT=10, T_VOID=14, …)
              r9   = Method*
              stk  = entry_point, parameters*, param_count, JavaThread*
        */
        inline auto find_call_stub_entry() noexcept -> void*
        {
            static const vmhook::hotspot::vm_struct_entry_t* const entry{
                vmhook::hotspot::iterate_struct_entries("StubRoutines", "_call_stub_entry") };
            if (!entry || !entry->address) return nullptr;
            void* const stub{ *reinterpret_cast<void**>(entry->address) };
            return vmhook::hotspot::is_valid_pointer(stub) ? stub : nullptr;
        }

        /*
            @brief Maps a JVM type-descriptor character to a HotSpot BasicType int.
            Values are stable across all JDK versions 8–24+.
        */
        inline auto sig_char_to_basic_type(const char c) noexcept -> int
        {
            switch (c)
            {
            case 'Z': return 4;   // T_BOOLEAN
            case 'C': return 5;   // T_CHAR
            case 'F': return 6;   // T_FLOAT
            case 'D': return 7;   // T_DOUBLE
            case 'B': return 8;   // T_BYTE
            case 'S': return 9;   // T_SHORT
            case 'I': return 10;  // T_INT
            case 'J': return 11;  // T_LONG
            case 'L': return 12;  // T_OBJECT
            case '[': return 13;  // T_ARRAY
            case 'V': return 14;  // T_VOID
            default:  return 12;  // T_OBJECT (fallback)
            }
        }
    }

    // --- Method proxy ------------------------------------------------------

    /*
        @brief Lightweight proxy to a Java method, supporting typed argument calls.
        @details
        Returned by vmhook::object::get_method().  Handles argument conversion
        and dispatches to the JVM method via the interpreter entry point.

        Usage inside a wrapper class:
             auto say_hi() -> void { get_method("sayHi")->call(); }

             auto say_string(const std::string& value) -> void {
                get_method("sayString")->call(value);
            }

        Returns a value_t that implicitly converts to the expected C++ type.
    */
    class method_proxy final
    {
    public:
        /*
            @brief Return type from method_proxy::call().
            @details
            Holds the result value as a variant, supporting implicit conversion
            to common C++ types via the conversion operator.
        */
        struct value_t
        {
            std::variant<
                std::monostate,
                bool,
                std::int8_t,
                std::int16_t,
                std::int32_t,
                std::int64_t,
                float,
                double,
                std::uint16_t,
                std::uint32_t,   // reference / array (compressed OOP)
                std::string      // for String objects
            > data;

            /*
                @brief Converts the stored value to T via static_cast.
                Falls back to a default-constructed T for variant alternatives
                that cannot be cast to the target type (std::monostate, std::string).
            */
            template<typename target_type>
            operator target_type() const noexcept
            {
                return std::visit([](auto v) noexcept
                    -> target_type
                    {
                        if constexpr (requires { static_cast<target_type>(v); })
                        {
                            return static_cast<target_type>(v);
                        }
                        else
                        {
                            return target_type{};
                        }
                    }
                , data);
            }
        };

        /*
            @param owning_object The object whose method will be called (may be null for static).
            @param method_pointer Pointer to the HotSpot Method object.
            @param signature     JVM method descriptor, e.g. "(I)Ljava/lang/String;"
        */
        method_proxy(void* owning_object, vmhook::hotspot::method* method_ptr, std::string sig) noexcept
            : object{ owning_object }
            , method{ method_ptr }
            , m_signature{ std::move(sig) }
            , m_is_static{ false }
        {
        }

        template<typename... args_t>
        auto call_jni(args_t&&... args) const noexcept
            -> value_t
        {
            if (!this->object)
            {
                return value_t{ std::monostate{} };
            }

            void* object_handle_storage{};
            void* const object_handle{ vmhook::detail::jni_oop_handle(this->object, object_handle_storage) };
            void* const klass{ vmhook::detail::jni_get_object_class(object_handle) };
            if (!klass)
            {
                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): GetObjectClass failed.", vmhook::error_tag, this->name(), this->m_signature);
                return value_t{ std::monostate{} };
            }

            void* const method_id{ vmhook::detail::jni_get_method_id(klass, this->name(), this->m_signature) };
            if (!method_id)
            {
                vmhook::detail::jni_exception_clear();

                if (!this->method || !vmhook::hotspot::is_valid_pointer(this->method))
                {
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): GetMethodID failed.", vmhook::error_tag, this->name(), this->m_signature);
                    return value_t{ std::monostate{} };
                }
            }
            void* const resolved_method_id{ method_id ? method_id : reinterpret_cast<void*>(this->method) };

            std::vector<void*> object_handles{};
            std::vector<vmhook::detail::jni_value> values{ vmhook::detail::make_jni_args(object_handles, std::forward<args_t>(args)...) };

            const std::size_t rparen{ this->m_signature.rfind(')') };
            const std::string_view return_signature{ rparen != std::string::npos ? std::string_view{ this->m_signature }.substr(rparen + 1) : std::string_view{ "V" } };

            if (return_signature == "Ljava/lang/String;")
            {
                using call_object_method_a_t = void* (*)(void*, void*, void*, const vmhook::detail::jni_value*);
                call_object_method_a_t const call_object_method_a{ vmhook::detail::jni_function<36, call_object_method_a_t>(vmhook::hotspot::current_jni_env) };
                if (!call_object_method_a)
                {
                    VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): CallObjectMethodA unavailable.", vmhook::error_tag, this->name(), this->m_signature);
                    return value_t{ std::monostate{} };
                }

                void* const result_handle{ call_object_method_a(vmhook::hotspot::current_jni_env, object_handle, resolved_method_id, values.data()) };
                return value_t{ vmhook::detail::jni_get_string_utf(result_handle) };
            }

            using call_void_method_a_t = void (*)(void*, void*, void*, const vmhook::detail::jni_value*);
            call_void_method_a_t const call_void_method_a{ vmhook::detail::jni_function<63, call_void_method_a_t>(vmhook::hotspot::current_jni_env) };
            if (!call_void_method_a)
            {
                VMHOOK_LOG("{} method_proxy::call_jni('{}{}'): CallVoidMethodA unavailable.", vmhook::error_tag, this->name(), this->m_signature);
                return value_t{ std::monostate{} };
            }

            call_void_method_a(vmhook::hotspot::current_jni_env, object_handle, resolved_method_id, values.data());
            return value_t{ std::monostate{} };
        }

        /*
            @brief Calls the method with the given arguments.
            @param args Arguments to pass (C++ types: int, std::string, etc.).
            @return value_t containing the result, or empty value_t for void methods.
            @details
            Converts C++ arguments to JVM types and invokes the method through
            the interpreter entry point. Handles both instance and static methods.

            For reference-type arguments (strings, objects), pass std::string for class name
            or use an object wrapper.
        */
        /*
            @brief Invokes the Java method and returns its result.
            @param args  C++ values forwarded as the method's Java arguments.
                         For instance methods the receiver is added automatically
                         from the proxy's stored object pointer.
            @return value_t holding the Java return value, or monostate if the
                    call gate is unavailable.

            @note  Calling Java methods from native C++ requires HotSpot's
                   StubRoutines::_call_stub_entry, which sets up a properly-typed
                   "entry frame" that the GC and frame-walker understand.
                   This address is located via VMStructs at runtime; if the
                   VMStruct entry is absent (removed in some JDK releases), call()
                   falls back to returning monostate without invoking the method.

                   call() must be invoked from inside a vmhook::hook() detour
                   where vmhook::hotspot::current_java_thread is set.
        */
        template<typename... args_t>
        auto call(args_t&&... args) const noexcept
            -> value_t
        {
            if (!this->method || !vmhook::hotspot::is_valid_pointer(this->method))
            {
                VMHOOK_LOG("{} method_proxy::call(): method pointer is null or invalid.", vmhook::error_tag);
                return value_t{ std::monostate{} };
            }

            vmhook::hotspot::method* const selected_method{ this->resolve_compatible_method<std::remove_cvref_t<args_t>...>() };
            const std::string selected_signature{ selected_method ? selected_method->get_signature() : this->m_signature };

            if (!vmhook::hotspot::ensure_current_java_thread())
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): no current JavaThread.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            auto* const thread{ vmhook::hotspot::current_java_thread };
            if (!thread)
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): current JavaThread is null after attach.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            // Locate HotSpot's C++→Java call gate via VMStructs.
            // StubRoutines::_call_stub_entry was removed from the VMStruct
            // export table in some JDK releases; check availability at runtime.
            void* const call_stub{ vmhook::detail::find_call_stub_entry() };
            if (!call_stub)
            {
                return method_proxy{ this->object, selected_method, selected_signature }.call_jni(std::forward<args_t>(args)...);
            }

            void* const entry{ selected_method->get_from_interpreted_entry() };
            if (!entry || !vmhook::hotspot::is_valid_pointer(entry))
            {
                VMHOOK_LOG("{} method_proxy::call('{}{}'): interpreted entry is null or invalid.", vmhook::error_tag, this->name(), selected_signature);
                return value_t{ std::monostate{} };
            }

            // ── Return type ───────────────────────────────────────────────────
            const std::string_view sig{ selected_signature };
            const std::size_t rparen{ sig.rfind(')') };
            const char ret_char{
                rparen != std::string_view::npos ? sig[rparen + 1] : 'V' };
            const int result_type{ vmhook::detail::sig_char_to_basic_type(ret_char) };

            // ── Parameter slot array ──────────────────────────────────────────
            // The call_stub passes parameters[] to the interpreter as locals[].
            // Each slot is an intptr_t: primitives are zero-extended, object
            // references are uncompressed decoded OOP pointers.
            std::intptr_t params[8]{};
            std::size_t   param_idx{ 0 };

            if (this->object && !this->m_is_static)
            {
                params[param_idx++] = reinterpret_cast<std::intptr_t>(this->object);
            }

            auto pack = [&](auto&& a) noexcept
                {
                    if (param_idx >= 8) return;
                    using clean_t = std::remove_cvref_t<decltype(a)>;
                    if constexpr (std::is_same_v<clean_t, std::string>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (std::is_same_v<clean_t, std::string_view>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (std::is_same_v<clean_t, const char*> || std::is_same_v<clean_t, char*>)
                    {
                        void* const string_oop{ vmhook::make_java_string(a ? std::string_view{ a } : std::string_view{}) };
                        if (!string_oop)
                        {
                            VMHOOK_LOG("{} method_proxy::call('{}{}'): failed to allocate Java String argument.", vmhook::error_tag, this->name(), selected_signature);
                        }
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(string_oop);
                    }
                    else if constexpr (vmhook::detail::is_unique_ptr_v<clean_t>)
                    {
                        using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_t>::value_type_t;
                        if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                        {
                            params[param_idx++] = reinterpret_cast<std::intptr_t>(a ? a->get_instance() : nullptr);
                        }
                    }
                    else if constexpr (std::is_base_of_v<vmhook::object_base, clean_t>)
                    {
                        params[param_idx++] = reinterpret_cast<std::intptr_t>(a.get_instance());
                    }
                    else
                    {
                        static_assert(sizeof(clean_t) <= 8);
                        std::intptr_t v{};
                        std::memcpy(&v, &a, sizeof(clean_t));
                        params[param_idx++] = v;
                    }
                };
            (pack(std::forward<args_t>(args)), ...);

            // ── Call the stub ─────────────────────────────────────────────────
            // Windows x64 calling convention — 8 arguments:
            //   rcx = link  (return address for stub frame, -1 sentinel)
            //   rdx = result* (where the Java return value is written)
            //   r8  = result_type (BasicType enum)
            //   r9  = Method*
            //   stk = entry_point, parameters*, param_count, JavaThread*
            using call_stub_fn_t = void (*)(
                void*,          // link
                std::intptr_t*, // result
                int,            // result_type
                void*,          // method
                void*,          // entry_point
                std::intptr_t*, // parameters
                int,            // size_of_parameters
                void*           // thread
                );

            std::intptr_t result_holder{ 0 };
            const vmhook::hotspot::java_thread_state previous_state{ thread->get_thread_state() };
            thread->set_thread_state(vmhook::hotspot::java_thread_state::_thread_in_Java);

            reinterpret_cast<call_stub_fn_t>(call_stub)(
                reinterpret_cast<void*>(static_cast<std::intptr_t>(-1)),
                &result_holder,
                result_type,
                reinterpret_cast<void*>(selected_method),
                entry,
                params,
                static_cast<int>(param_idx),
                reinterpret_cast<void*>(thread)
                );

            // ── Decode result ─────────────────────────────────────────────────
            thread->set_thread_state(previous_state);

            switch (ret_char)
            {
            case 'Z': return value_t{ (result_holder & 1) != 0 };
            case 'B': return value_t{ static_cast<std::int8_t> (result_holder) };
            case 'S': return value_t{ static_cast<std::int16_t>(result_holder) };
            case 'I': return value_t{ static_cast<std::int32_t>(result_holder) };
            case 'J': return value_t{ static_cast<std::int64_t>(result_holder) };
            case 'C': return value_t{ static_cast<std::uint16_t>(result_holder) };
            case 'F':
            {
                float f{};
                const std::int32_t bits{ static_cast<std::int32_t>(result_holder) };
                std::memcpy(&f, &bits, sizeof(f));
                return value_t{ f };
            }
            case 'D':
            {
                double d{};
                std::memcpy(&d, &result_holder, sizeof(d));
                return value_t{ d };
            }
            case 'V': return value_t{ std::monostate{} };
            default:  return value_t{ static_cast<std::uint32_t>(result_holder) };
            }
        }

        /*
            @brief Returns the method name.
        */
        auto name() const noexcept
            -> std::string
        {
            if (!this->method)
            {
                return {};
            }
            return this->method->get_name();
        }

        /*
            @brief Returns the JVM method signature.
        */
        auto signature() const noexcept
            -> std::string_view
        {
            return this->m_signature;
        }

        auto is_static() const noexcept
            -> bool
        {
            return this->m_is_static;
        }

        auto get_compressed_oop() const noexcept
            -> std::uint32_t
        {
            if (!this->object)
            {
                return 0;
            }
            std::uint32_t value{};
            std::memcpy(&value, this->object, sizeof(value));
            return value;
        }

    private:
        static auto klass_from_object_header(void* const oop) noexcept
            -> vmhook::hotspot::klass*
        {
            if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
            {
                return nullptr;
            }

            const std::uint32_t narrow_klass{ *reinterpret_cast<const std::uint32_t*>(
                reinterpret_cast<const std::uint8_t*>(oop) + 8) };
            void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow_klass) };
            if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
            {
                return nullptr;
            }

            return reinterpret_cast<vmhook::hotspot::klass*>(decoded);
        }

        template<typename argument_type>
        static auto argument_matches_descriptor(const std::string_view descriptor) noexcept
            -> bool
        {
            using clean_type = std::remove_cvref_t<argument_type>;

            if constexpr (std::is_same_v<clean_type, std::string> || std::is_same_v<clean_type, std::string_view> || std::is_same_v<clean_type, const char*> || std::is_same_v<clean_type, char*>)
            {
                return descriptor == "Ljava/lang/String;";
            }
            else if constexpr (vmhook::detail::is_unique_ptr_v<clean_type>)
            {
                using wrapper_type = typename vmhook::detail::is_unique_ptr<clean_type>::value_type_t;
                if constexpr (std::is_base_of_v<vmhook::object_base, wrapper_type>)
                {
                    const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(wrapper_type) }) };
                    return descriptor.size() >= 3
                        && descriptor.front() == 'L'
                        && descriptor.back() == ';'
                        && (type_map_entry == vmhook::type_to_class_map.end() || descriptor.substr(1, descriptor.size() - 2) == type_map_entry->second);
                }
                else
                {
                    return false;
                }
            }
            else if constexpr (std::is_base_of_v<vmhook::object_base, clean_type>)
            {
                const auto type_map_entry{ vmhook::type_to_class_map.find(std::type_index{ typeid(clean_type) }) };
                return descriptor.size() >= 3
                    && descriptor.front() == 'L'
                    && descriptor.back() == ';'
                    && (type_map_entry == vmhook::type_to_class_map.end() || descriptor.substr(1, descriptor.size() - 2) == type_map_entry->second);
            }
            else if constexpr (std::is_same_v<clean_type, bool>)
            {
                return descriptor == "Z";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 1)
            {
                return descriptor == "B" || descriptor == "Z";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 2)
            {
                return descriptor == "S" || descriptor == "C";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 4)
            {
                return descriptor == "I";
            }
            else if constexpr (std::is_integral_v<clean_type> && sizeof(clean_type) == 8)
            {
                return descriptor == "J";
            }
            else if constexpr (std::is_same_v<clean_type, float>)
            {
                return descriptor == "F";
            }
            else if constexpr (std::is_same_v<clean_type, double>)
            {
                return descriptor == "D";
            }
            else
            {
                return false;
            }
        }

        static auto next_argument_descriptor(const std::string_view signature, std::size_t& position, const std::size_t close_paren) noexcept
            -> std::string_view
        {
            const std::size_t start{ position };
            while (position < close_paren && signature[position] == '[')
            {
                ++position;
            }

            if (position >= close_paren)
            {
                return {};
            }

            if (signature[position] == 'L')
            {
                const std::size_t semicolon{ signature.find(';', position) };
                if (semicolon == std::string_view::npos || semicolon > close_paren)
                {
                    return {};
                }
                position = semicolon + 1;
                return signature.substr(start, position - start);
            }

            ++position;
            return signature.substr(start, position - start);
        }

        template<typename... args_t>
        static auto signature_matches_arguments(const std::string_view signature) noexcept
            -> bool
        {
            const std::size_t open_paren{ signature.find('(') };
            const std::size_t close_paren{ signature.find(')') };
            if (open_paren == std::string_view::npos || close_paren == std::string_view::npos || close_paren < open_paren)
            {
                return false;
            }

            std::size_t position{ open_paren + 1 };
            bool matches{ true };
            ([&]
                {
                    if (!matches)
                    {
                        return;
                    }
                    const std::string_view descriptor{ next_argument_descriptor(signature, position, close_paren) };
                    matches = !descriptor.empty() && argument_matches_descriptor<args_t>(descriptor);
                }(), ...);

            return matches && position == close_paren;
        }

        template<typename... args_t>
        auto resolve_compatible_method() const noexcept
            -> vmhook::hotspot::method*
        {
            if (signature_matches_arguments<args_t...>(this->m_signature))
            {
                return this->method;
            }

            vmhook::hotspot::klass* const resolved_klass{ this->object ? klass_from_object_header(this->object) : nullptr };
            if (!resolved_klass)
            {
                return this->method;
            }

            const std::string method_name{ this->name() };
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method) || current_method->get_name() != method_name)
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (signature_matches_arguments<args_t...>(current_signature))
                    {
                        return current_method;
                    }
                }
            }

            return this->method;
        }

        void* object;
        vmhook::hotspot::method* method;
        std::string m_signature;
        bool        m_is_static;
    };

    // --- Object base class ----------------------------------------------------

    /*
        @brief Alias for a decoded (uncompressed) Java object pointer.
        @details
        This is the type passed to vmhook::object constructors. Obtain it from a
        hook frame via frame->get_arguments<vmhook::oop_type_t>(), which internally calls
        vmhook::hotspot::decode_oop_pointer() to convert the 32-bit compressed OOP to a full
        64-bit address.  It is NOT a JNI global reference - no GC handles are
        created and the pointer remains valid only for the duration of the hook.
    */
    using oop_type_t = void*;
    using oop_t = oop_type_t;

    /*
        @brief Base class for C++ wrappers around live Java objects.
        @details
        Derive from this class to create a typed façade for a Java class.
        get_field() handles both instance and static fields automatically -
        the library reads JVM_ACC_STATIC from the InstanceKlass._fields array
        so there is no separate static-field accessor.

        Example:

            class http_client : public vmhook::object
            {
            public:
                explicit http_client(vmhook::oop_type_t instance)
                    : vmhook::object{ instance }
                {}

                // auto return - field_proxy::value_t converts implicitly at the call site
                auto is_connected() -> bool { return get_field("isConnected")->get(); }
                auto get_health()   -> int { return get_field("health")->get(); }

                // Cast inside the method if you want a concrete return type
                auto get_timeout()  -> int { return static_cast<int>(get_field("timeout")->get()); }

                // Static field - get_field resolves through the registered class.
                static auto get_version() -> std::string { return get_field("VERSION")->get(); }

                // Writing a field
                auto set_health(int hp) -> void { get_field("health")->set(hp); }
            };

            vmhook::register_class<http_client>("com/example/HttpClient");

            // Inside a hook detour:
            auto [self] = frame->get_arguments<vmhook::oop_type_t>();
            http_client client{ self };
            bool ok  = client.is_connected(); // operator bool()  fires
            int  hp  = client.get_health();   // operator int()   fires
            client.set_health(100);

        @note The wrapped pointer is a raw decoded OOP, not a JNI global reference.
              It is valid for the duration of the hook invocation only.
    */
    class object_base
    {
    public:
        /*
            @brief Wraps a decoded Java object pointer.
            @param instance Decoded OOP from frame->get_arguments<vmhook::oop_type_t>().
                            May be nullptr if the Java reference is null.
        */
        explicit object_base(oop_type_t instance = nullptr) noexcept
            : instance{ instance }
        {
        }

        virtual ~object_base() = default;

        object_base(const object_base&) = default;
        auto operator=(const object_base&)
            -> object_base & = default;

        object_base(object_base&& other) noexcept
            : instance{ other.instance }
        {
            other.instance = nullptr;
        }

        auto operator=(object_base&& other) noexcept
            -> object_base&
        {
            if (this != &other)
            {
                this->instance = other.instance;
                other.instance = nullptr;
            }
            return *this;
        }

        /*
            @brief Returns the raw decoded OOP pointer held by this wrapper.
        */
        auto get_instance() const noexcept
            -> oop_type_t
        {
            return this->instance;
        }

        /*
            @brief Returns a field proxy for any field declared on this class.
            @param name Exact Java field name.
            @return Optional holding the field proxy, or nullopt on failure.
            @details
            Works for both instance and static fields.  The JVM_ACC_STATIC flag is
            read from InstanceKlass._fields so no static/instance distinction is
            needed at the call site.

            Instance fields:  value lives at decoded_object_ptr + field_offset.
            Static fields:    value lives at java.lang.Class mirror + field_offset.

            The klass is resolved from type_to_class_map via typeid(*this) (dynamic type),
            so the derived C++ class must have been registered with register_class<T>()
            before this is called.  Field entries are cached after the first lookup.

            @note Dereferencing a nullopt with -> is undefined behaviour.
                  In production code always check the optional, or assert field names
                  are correct at development time.
        */
        auto get_field(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            const auto entry{ vmhook::find_field(resolved_klass, name) };
            if (!entry)
            {
                return std::nullopt;
            }

            if (entry->is_static)
            {
                void* const mirror{ resolved_klass->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    VMHOOK_LOG("{} object::get_field('{}') failed to get java.lang.Class mirror.", vmhook::error_tag, name);
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }

            if (!this->instance)
            {
                VMHOOK_LOG("{} object::get_field('{}') instance pointer is null.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        static auto get_field(const std::type_index wrapper_type, const std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            const auto entry{ vmhook::find_field(resolved_klass, name) };
            if (!entry)
            {
                return std::nullopt;
            }

            if (!entry->is_static)
            {
                VMHOOK_LOG("{} object::get_field('{}') needs an object instance.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const mirror{ resolved_klass->get_java_mirror() };
            if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
            {
                VMHOOK_LOG("{} object::get_field('{}') failed to get java.lang.Class mirror.", vmhook::error_tag, name);
                return std::nullopt;
            }

            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, true };
        }

        /*
            @brief Returns a method proxy for a method declared on this class.
            @param method_name Exact Java method name.
            @return Optional holding the method proxy, or nullopt on failure.
            @details
            Looks up the method in the InstanceKlass._methods array by name.
            The returned proxy can be used to call the method with arguments.

            Usage:
                auto say_hi() -> void { return get_method("sayHi")->call(); }
                auto take_values(int a, const std::string& b) -> void {
                    return get_method("takeValues")->call(a, b);
                }
        */
        auto get_method(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (current_method && vmhook::hotspot::is_valid_pointer(current_method)
                        && current_method->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, current_method, current_method->get_signature() };
                    }
                }
            }

            return std::nullopt;
        }

        auto get_method(const std::string_view method_name, const std::string_view method_signature) const
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ this->resolve_klass() };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (current_method->get_name() == method_name && current_signature == method_signature)
                    {
                        return vmhook::method_proxy{ this->instance, current_method, current_signature };
                    }
                }
            }

            return std::nullopt;
        }

        static auto get_method(const std::type_index wrapper_type, const std::string_view method_name)
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (current_method && vmhook::hotspot::is_valid_pointer(current_method)
                        && current_method->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ nullptr, current_method, current_method->get_signature() };
                    }
                }
            }

            return std::nullopt;
        }

        static auto get_method(const std::type_index wrapper_type, const std::string_view method_name, const std::string_view method_signature)
            -> std::optional<vmhook::method_proxy>
        {
            vmhook::hotspot::klass* const resolved_klass{ resolve_klass(wrapper_type) };
            if (!resolved_klass)
            {
                return std::nullopt;
            }

            // Walk the superclass chain so inherited methods are found.
            for (vmhook::hotspot::klass* k{ resolved_klass }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };

                if (!methods_array || method_count <= 0)
                {
                    continue;
                }

                for (std::int32_t method_index{ 0 }; method_index < method_count; ++method_index)
                {
                    vmhook::hotspot::method* const current_method{ methods_array[method_index] };
                    if (!current_method || !vmhook::hotspot::is_valid_pointer(current_method))
                    {
                        continue;
                    }

                    const std::string current_signature{ current_method->get_signature() };
                    if (current_method->get_name() == method_name && current_signature == method_signature)
                    {
                        return vmhook::method_proxy{ nullptr, current_method, current_signature };
                    }
                }
            }

            return std::nullopt;
        }

    protected:
        /*
            @brief The raw decoded OOP pointer to the wrapped Java object.
        */
        oop_type_t instance{ nullptr };

    private:
        /*
            @brief Resolves the HotSpot klass for the dynamic type of this object.
            @details
            Uses typeid(*this) to look up the class name in type_to_class_map, then
            calls find_class() which walks the ClassLoaderDataGraph (cached after
            the first call).
            @return Pointer to the klass, or nullptr if the type is not registered.
        */
        auto resolve_klass() const
            -> vmhook::hotspot::klass*
        {
            return resolve_klass(std::type_index{ typeid(*this) });
        }

        static auto resolve_klass(const std::type_index wrapper_type)
            -> vmhook::hotspot::klass*
        {
            const auto type_map_entry{ vmhook::type_to_class_map.find(wrapper_type) };
            if (type_map_entry == vmhook::type_to_class_map.end())
            {
                VMHOOK_LOG("{} object::resolve_klass() type '{}' not registered via register_class<T>().", vmhook::error_tag, wrapper_type.name());
                return nullptr;
            }

            vmhook::hotspot::klass* const found_klass{ vmhook::find_class(type_map_entry->second) };
            if (!found_klass)
            {
                VMHOOK_LOG("{} object::resolve_klass() class '{}' not found in JVM.", vmhook::error_tag, type_map_entry->second);
            }

            return found_klass;
        }
    };

    template<typename derived>
    class object : public object_base
    {
    public:
        using object_base::object_base;

        /*
            @brief Instance get_field / get_method — C++23 deducing-this overloads.

            A deducing-this function requires an explicit object to be called.
            MSVC (and all conformant compilers) correctly exclude it from the
            overload set when there is no 'this' in scope (i.e. inside a static
            C++ method).  This avoids the C2352 "non-static member requires object"
            that MSVC erroneously fires when a regular non-static overload is the
            best type match from static context.

            From instance context, string literals are an exact match for
            const char*, so these deducing-this overloads win over the static
            string_view overloads below.  The call is forwarded to
            object_base::get_field / get_method, which carries the live OOP
            pointer needed for instance Java fields.

            Usage:
                auto get_health()  -> int  { return get_field("health")->get(); }
                auto set_health(int v)     { get_field("health")->set(v); }
        */
        auto get_field(this const object_base& self, const char* const name)
            -> std::optional<vmhook::field_proxy>
        {
            return self.object_base::get_field(name);
        }

        auto get_method(this const object_base& self, const char* const name)
            -> std::optional<vmhook::method_proxy>
        {
            return self.object_base::get_method(name);
        }

        auto get_method(this const object_base& self, const char* const name, const char* const signature)
            -> std::optional<vmhook::method_proxy>
        {
            return self.object_base::get_method(name, signature);
        }

        /*
            @brief Static get_field / get_method — for static Java fields called
            from static C++ methods.

            From a static C++ method the deducing-this overloads above are not
            in the candidate set (no object), so these static overloads are the
            only viable candidates.  They resolve the class via type_index (no
            OOP needed for the Java mirror that backs static fields).

            Usage:
                static auto get_version() -> std::string { return get_field("version")->get(); }
                static auto reset()        -> void        { get_method("reset")->call(); }
        */
        static auto get_field(const std::string_view name)
            -> std::optional<vmhook::field_proxy>
        {
            return object_base::get_field(std::type_index{ typeid(derived) }, name);
        }

        static auto get_method(const std::string_view name)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name);
        }

        static auto get_method(const std::string_view name, const std::string_view signature)
            -> std::optional<vmhook::method_proxy>
        {
            return object_base::get_method(std::type_index{ typeid(derived) }, name, signature);
        }
    };

    // --- Built-in Java collection wrappers ------------------------------------

    /*
        @brief Resolves the HotSpot klass* directly from a decoded OOP header.

        On x64 HotSpot (compressed class pointers enabled, the default):
          offset 0  : mark word (8 bytes)
          offset 8  : narrow klass (uint32_t, 4 bytes)
        The narrow klass is decoded the same way as regular narrow oops but using
        the klass base/shift instead of the oop base/shift.

        Returns nullptr if the pointer is invalid or decoding fails.
    */
    inline auto klass_from_oop(void* const oop) noexcept -> vmhook::hotspot::klass*
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return nullptr;
        }
        const std::uint32_t narrow{ *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<const std::uint8_t*>(oop) + 8) };
        void* const decoded{ vmhook::hotspot::decode_klass_pointer(narrow) };
        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return nullptr;
        }
        return reinterpret_cast<vmhook::hotspot::klass*>(decoded);
    }

    /*
        @brief C++ wrapper for java.util.Collection objects.

        Uses the live OOP's klass pointer (read from its object header) to
        resolve fields and methods, so no register_class<T>() call is needed
        and it works with any concrete Collection implementation
        (ArrayList, LinkedList, HashSet, etc.).

        Typical usage (from a hook detour where you received a List OOP):

            collection col{ list_oop };
            std::int32_t n = col.size();
    */
    class collection : public vmhook::object_base
    {
    public:
        explicit collection(vmhook::oop_t oop) noexcept
            : vmhook::object_base{ oop }
        {
        }

    protected:
        /*
            @brief Returns the klass by reading the narrow klass slot in the OOP header.
            Falls back to nullptr if the OOP is invalid.
        */
        auto oop_klass() const noexcept -> vmhook::hotspot::klass*
        {
            return vmhook::klass_from_oop(this->instance);
        }

        /*
            @brief get_field variant that uses the live OOP's klass, not the C++ type registry.
        */
        auto get_field_by_oop_klass(const std::string_view name) const
            -> std::optional<vmhook::field_proxy>
        {
            vmhook::hotspot::klass* const k{ oop_klass() };
            if (!k)
            {
                return std::nullopt;
            }
            const auto entry{ vmhook::find_field(k, name) };
            if (!entry)
            {
                return std::nullopt;
            }
            if (entry->is_static)
            {
                void* const mirror{ k->get_java_mirror() };
                if (!mirror || !vmhook::hotspot::is_valid_pointer(mirror))
                {
                    return std::nullopt;
                }
                void* const field_pointer{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return vmhook::field_proxy{ field_pointer, entry->signature, true };
            }
            if (!this->instance)
            {
                return std::nullopt;
            }
            void* const field_pointer{ reinterpret_cast<std::uint8_t*>(this->instance) + entry->offset };
            return vmhook::field_proxy{ field_pointer, entry->signature, false };
        }

        /*
            @brief get_method variant that searches the live OOP's klass hierarchy.
        */
        auto get_method_by_oop_klass(const std::string_view method_name) const
            -> std::optional<vmhook::method_proxy>
        {
            for (vmhook::hotspot::klass* k{ oop_klass() }; k != nullptr; k = k->get_super())
            {
                const std::int32_t method_count{ k->get_methods_count() };
                vmhook::hotspot::method** const methods_array{ k->get_methods_ptr() };
                if (!methods_array || method_count <= 0)
                {
                    continue;
                }
                for (std::int32_t i{ 0 }; i < method_count; ++i)
                {
                    vmhook::hotspot::method* const m{ methods_array[i] };
                    if (m && vmhook::hotspot::is_valid_pointer(m) && m->get_name() == method_name)
                    {
                        return vmhook::method_proxy{ this->instance, m, m->get_signature() };
                    }
                }
            }
            return std::nullopt;
        }

    public:
        /*
            @brief Returns the number of elements via the Java size() method.
            Resolves through the concrete class's virtual dispatch table,
            so it works for ArrayList, LinkedList, HashSet, etc.
        */
        auto size() const noexcept -> std::int32_t
        {
            const auto proxy{ get_method_by_oop_klass("size") };
            if (!proxy)
            {
                return 0;
            }
            return proxy->call();
        }

        auto is_empty() const noexcept -> bool
        {
            return size() == 0;
        }

        template<typename element_type>
        auto to_vector() const -> std::vector<std::unique_ptr<element_type>>
        {
            std::vector<std::unique_ptr<element_type>> result;

            if (!instance || !vmhook::hotspot::is_valid_pointer(instance))
            {
                return result;
            }

            const auto size_opt{ get_field_by_oop_klass("size") };
            const auto data_opt{ get_field_by_oop_klass("elementData") };

            if (size_opt && data_opt)
            {
                const std::int32_t n{ size_opt->get() };
                if (n <= 0)
                {
                    return result;
                }

                const std::uint32_t compressed_array{ static_cast<std::uint32_t>(data_opt->get()) };
                void* const array_oop{ vmhook::decode_array_oop(compressed_array) };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    result.reserve(static_cast<std::size_t>(n));
                    for (std::int32_t index{ 0 }; index < n; ++index)
                    {
                        const std::uint32_t compressed_element{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
                        void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed_element) };
                        if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                        {
                            result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                        }
                        else
                        {
                            result.push_back(nullptr);
                        }
                    }
                    return result;
                }
            }

            const std::int32_t n{ size() };
            if (n <= 0)
            {
                return result;
            }

            const auto get_method_opt{ get_method_by_oop_klass("get") };
            if (!get_method_opt)
            {
                return result;
            }

            result.reserve(static_cast<std::size_t>(n));
            for (std::int32_t index{ 0 }; index < n; ++index)
            {
                const auto element_value{ get_method_opt->call<std::uint32_t>(index) };
                void* const element_oop{ vmhook::hotspot::decode_oop_pointer(element_value) };
                if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
                {
                    result.push_back(std::make_unique<element_type>(static_cast<vmhook::oop_t>(element_oop)));
                }
                else
                {
                    result.push_back(nullptr);
                }
            }

            return result;
        }
    };

    /*
        @brief C++ wrapper for java.util.List objects.

        Provides to_vector<T>() which reads an ArrayList's backing array
        directly from the JVM heap and returns a std::vector of unique_ptr<T>.

        Usage:

            // Java: public List<A> listOfAs;
            auto vec = get_field("listOfAs")->get<std::unique_ptr<vmhook::list>>()
                           ->to_vector<a_class>();
            // vec is std::vector<std::unique_ptr<a_class>>
    */
    class list : public vmhook::collection
    {
    public:
        explicit list(vmhook::oop_t oop) noexcept
            : vmhook::collection{ oop }
        {
        }

        /*
            @brief Converts this Java List to a std::vector<std::unique_ptr<T>>.
            @tparam element_type  C++ wrapper class whose constructor accepts a
                                  vmhook::oop_t (decoded Java object pointer).

            Reads the ArrayList backing array directly from the JVM heap.
            Null Java elements become nullptr entries in the returned vector.
            Works on all Java versions supported by vmhook (8–24+).

            For non-ArrayList implementations the method falls back to calling
            the Java get(int) method for each index.
        */
        template<typename element_type>
        auto to_vector() const -> std::vector<std::unique_ptr<element_type>>
        {
            std::vector<std::unique_ptr<element_type>> result;

            if (!instance || !vmhook::hotspot::is_valid_pointer(instance))
            {
                return result;
            }

            // ── ArrayList fast path ─────────────────────────────────────────
            // ArrayList stores its live element count in a field named "size"
            // and its backing Object[] in a field named "elementData".
            const auto size_opt{ get_field_by_oop_klass("size") };
            const auto data_opt{ get_field_by_oop_klass("elementData") };

            if (size_opt && data_opt)
            {
                const std::int32_t n{ size_opt->get() };
                if (n > 0)
                {
                    // elementData is stored as a compressed OOP (uint32_t)
                    const std::uint32_t compressed_array{
                        static_cast<std::uint32_t>(data_opt->get()) };
                    void* const array_oop{ vmhook::decode_array_oop(compressed_array) };

                    if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                    {
                        result.reserve(static_cast<std::size_t>(n));
                        for (std::int32_t i{ 0 }; i < n; ++i)
                        {
                            const std::uint32_t compressed_elem{
                                vmhook::get_array_element<std::uint32_t>(array_oop, i) };
                            void* const elem_oop{
                                vmhook::hotspot::decode_oop_pointer(compressed_elem) };

                            if (elem_oop && vmhook::hotspot::is_valid_pointer(elem_oop))
                            {
                                result.push_back(std::make_unique<element_type>(
                                    static_cast<vmhook::oop_t>(elem_oop)));
                            }
                            else
                            {
                                result.push_back(nullptr);
                            }
                        }
                        return result;
                    }
                }
                else
                {
                    return result;  // empty list
                }
            }

            // ── Generic fallback: call Java List.get(int) ───────────────────
            const std::int32_t n{ size() };
            if (n <= 0)
            {
                return result;
            }
            result.reserve(static_cast<std::size_t>(n));

            const auto get_method_opt{ get_method_by_oop_klass("get") };
            if (!get_method_opt)
            {
                return result;
            }

            for (std::int32_t i{ 0 }; i < n; ++i)
            {
                const auto elem_val{ get_method_opt->call<std::uint32_t>(i) };
                void* const elem_oop{ vmhook::hotspot::decode_oop_pointer(elem_val) };
                if (elem_oop && vmhook::hotspot::is_valid_pointer(elem_oop))
                {
                    result.push_back(std::make_unique<element_type>(
                        static_cast<vmhook::oop_t>(elem_oop)));
                }
                else
                {
                    result.push_back(nullptr);
                }
            }
            return result;
        }
    };

    template<typename element_type>
    auto field_proxy::value_t::to_vector() const
        -> std::vector<std::unique_ptr<element_type>>
    {
        const std::uint32_t compressed_collection{ static_cast<std::uint32_t>(*this) };
        void* const collection_oop{ vmhook::hotspot::decode_oop_pointer(compressed_collection) };
        if (!collection_oop || !vmhook::hotspot::is_valid_pointer(collection_oop))
        {
            return {};
        }

        return vmhook::collection{ collection_oop }.to_vector<element_type>();
    }

    // --- Helper: read a Java String OOP to std::string ------------------------

    /*
        @brief Decodes a Java String object into a std::string.
        @param string_oop  A decoded OOP pointing to a java.lang.String instance.
        @return A std::string containing the string contents, or empty on failure.
        @details
        Handles both pre-Java-9 (char[] value) and Java-9+ (byte[] value + coder) layouts.
        Truncates strings longer than 4096 characters as a sanity check.
    */
    inline auto read_java_string(void* const string_oop)
        -> std::string
    {
        if (!string_oop || !vmhook::hotspot::is_valid_pointer(string_oop))
        {
            return {};
        }

        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return {};
        }

        const std::uint32_t arr_compressed{ vmhook::get_field<std::uint32_t>(string_oop, string_klass, "value") };
        if (!arr_compressed)
        {
            return {};
        }

        void* const arr_oop{ vmhook::hotspot::decode_oop_pointer(arr_compressed) };
        if (!arr_oop || !vmhook::hotspot::is_valid_pointer(arr_oop))
        {
            return {};
        }

        const auto* const arr{ reinterpret_cast<const std::uint8_t*>(arr_oop) };
        const std::int32_t length{ *reinterpret_cast<const std::int32_t*>(arr + 12) };
        if (length <= 0 || length > 4096)
        {
            return {};
        }

        const std::uint8_t* const data{ arr + 16 };
        const bool has_coder{ string_klass->find_field("coder").has_value() };

        std::string result;
        if (!has_coder)
        {
            const auto* const chars{ reinterpret_cast<const std::uint16_t*>(data) };
            result.reserve(static_cast<std::size_t>(length));
            for (std::int32_t i{ 0 }; i < length; ++i)
            {
                result += (chars[i] < 0x80) ? static_cast<char>(chars[i]) : '?';
            }
        }
        else
        {
            const std::uint8_t coder{ vmhook::get_field<std::uint8_t>(string_oop, string_klass, "coder") };
            if (coder == 0)
            {
                result.assign(reinterpret_cast<const char*>(data), static_cast<std::size_t>(length));
            }
            else
            {
                const auto* const chars{ reinterpret_cast<const std::uint16_t*>(data) };
                const std::int32_t char_count{ length / 2 };
                result.reserve(static_cast<std::size_t>(char_count));
                for (std::int32_t i{ 0 }; i < char_count; ++i)
                {
                    result += (chars[i] < 0x80) ? static_cast<char>(chars[i]) : '?';
                }
            }
        }
        return result;
    }

    inline auto field_oop(const vmhook::field_proxy& field) noexcept
        -> void*
    {
        return vmhook::decode_array_oop(field.get_compressed_oop());
    }

    inline auto write_java_string(void* const string_oop, const std::string_view value) noexcept
        -> void
    {
        if (!string_oop || !vmhook::hotspot::is_valid_pointer(string_oop))
        {
            return;
        }

        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        if (!string_klass)
        {
            return;
        }

        const auto value_field{ vmhook::find_field(string_klass, "value") };
        if (!value_field)
        {
            return;
        }

        std::uint32_t compressed{};
        std::memcpy(&compressed, reinterpret_cast<const std::uint8_t*>(string_oop) + value_field->offset, sizeof(compressed));
        void* const array_oop{ vmhook::decode_array_oop(compressed) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ vmhook::array_length(array_oop) };
        const std::int32_t writable_length{ (std::min)(length, static_cast<std::int32_t>(value.size())) };
        if (writable_length <= 0)
        {
            return;
        }

        if (value_field->signature == "[C")
        {
            for (std::int32_t index{ 0 }; index < writable_length; ++index)
            {
                vmhook::set_array_element<std::uint16_t>(array_oop, index, static_cast<std::uint16_t>(static_cast<unsigned char>(value[static_cast<std::size_t>(index)])));
            }
        }
        else
        {
            for (std::int32_t index{ 0 }; index < writable_length; ++index)
            {
                vmhook::set_array_element<std::uint8_t>(array_oop, index, static_cast<std::uint8_t>(value[static_cast<std::size_t>(index)]));
            }
        }
    }

    inline auto set_str_field(const vmhook::field_proxy& field, const std::string_view value) noexcept
        -> void
    {
        vmhook::write_java_string(vmhook::field_oop(field), value);
    }

    inline auto set_bool_array(const vmhook::field_proxy& field, const std::vector<bool>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            vmhook::set_array_element<std::uint8_t>(array_oop, index, values[static_cast<std::size_t>(index)] ? 1u : 0u);
        }
    }

    template<typename element_type>
    inline auto set_prim_array(const vmhook::field_proxy& field, const std::vector<element_type>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            const element_type& value{ values[static_cast<std::size_t>(index)] };
            if constexpr (std::is_same_v<element_type, char>)
            {
                if (field.signature() == "[C")
                {
                    vmhook::set_array_element<std::uint16_t>(array_oop, index, static_cast<std::uint16_t>(static_cast<unsigned char>(value)));
                }
                else
                {
                    vmhook::set_array_element<char>(array_oop, index, value);
                }
            }
            else
            {
                vmhook::set_array_element<element_type>(array_oop, index, value);
            }
        }
    }

    inline auto set_str_array(const vmhook::field_proxy& field, const std::vector<std::string>& values) noexcept
        -> void
    {
        void* const array_oop{ vmhook::field_oop(field) };
        if (!array_oop)
        {
            return;
        }

        const std::int32_t length{ (std::min)(vmhook::array_length(array_oop), static_cast<std::int32_t>(values.size())) };
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            const std::uint32_t compressed{ vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            vmhook::write_java_string(vmhook::hotspot::decode_oop_pointer(compressed), values[static_cast<std::size_t>(index)]);
        }
    }

    // --- Helper: decode compressed array OOP ---------------------------------

    /*
        @brief Decodes a 32-bit compressed array OOP into a raw pointer.
        @param compressed  A 32-bit compressed OOP value.
        @return A valid pointer if decoding succeeded, nullptr otherwise.
    */
    auto decode_array_oop(const std::uint32_t compressed)
        -> void*
    {
        if (!compressed)
        {
            return nullptr;
        }
        void* const decoded_pointer{ vmhook::hotspot::decode_oop_pointer(compressed) };
        return (decoded_pointer && vmhook::hotspot::is_valid_pointer(decoded_pointer)) ? decoded_pointer : nullptr;
    }
}
