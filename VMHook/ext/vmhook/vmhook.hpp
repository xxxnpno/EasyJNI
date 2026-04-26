#pragma once

#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <thread>
#include <chrono>
#include <algorithm>
#include <type_traits>
#include <tuple>
#include <cstring>
#include <optional>
#include <variant>

#include <windows.h>

namespace jni
{
    inline constexpr std::string_view easy_jni_error{ "[EasyJNI ERROR]" };
    inline constexpr std::string_view easy_jni_warning{ "[EasyJNI WARNING]" };
    inline constexpr std::string_view easy_jni_info{ "[EasyJNI INFO]" };

    /*
        @brief Custom exception for EasyJNI errors.
    */
    class jni_exception final : public std::exception
    {
    public:
        explicit jni_exception(const std::string_view msg)
            : message{ msg }
        {

        }

        auto what() const noexcept -> const char* override
        {
            return this->message.c_str();
        }

    private:
        std::string message;
    };

    namespace hotspot
    {
        struct VMStructEntry;
        struct VMTypeEntry;
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
        struct field_entry;

        static auto decode_oop_ptr(std::uint32_t compressed) noexcept -> void*;
    }

    /*
        @brief Maps C++ wrapper types to their corresponding internal Java class names.
        @details
        Populated by register_class() when a C++ wrapper type is associated with
        a Java class name. Used by hook() and other APIs that need to look up the
        Java class corresponding to a given C++ wrapper type at runtime.
        Keys are std::type_index values derived from typeid() of the C++ wrapper type.
        Values are the internal JVM class names using '/' separators.
        @see register_class, hook
    */
    inline std::unordered_map<std::type_index, std::string> class_map{};

    template<class T>
    static auto register_class(std::string_view class_name) noexcept -> bool;

    // ─── HotSpot internals ────────────────────────────────────────────────────

    namespace hotspot
    {
        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct VMTypeEntry
        {
            const char*   type_name;
            const char*   superclass_name;
            std::int32_t  is_oop_type;
            std::int32_t  is_integer_type;
            std::int32_t  is_unsigned;
            std::uint64_t size;
        };

        // https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
        struct VMStructEntry
        {
            const char*   type_name;
            const char*   field_name;
            const char*   type_string;
            std::int32_t  is_static;
            std::uint64_t offset;
            void*         address;
        };

        /*
            @brief Returns the module handle for jvm.dll loaded in the current process.
        */
        inline auto get_jvm_module() noexcept -> HMODULE
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
        inline auto get_vm_types() noexcept -> VMTypeEntry*
        {
            static FARPROC proc{ GetProcAddress(get_jvm_module(), "gHotSpotVMTypes") };
            static VMTypeEntry* ptr{ *reinterpret_cast<VMTypeEntry**>(proc) };
            return ptr;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM struct entries.
            @details
            Resolves gHotSpotVMStructs from jvm.dll via GetProcAddress on first call
            and caches the typed pointer so subsequent calls are free.
        */
        inline auto get_vm_structs() noexcept -> VMStructEntry*
        {
            static FARPROC proc{ GetProcAddress(get_jvm_module(), "gHotSpotVMStructs") };
            static VMStructEntry* ptr{ *reinterpret_cast<VMStructEntry**>(proc) };
            return ptr;
        }

        /*
            @brief Searches the gHotSpotVMTypes array for a type entry by name.
        */
        static auto iterate_type_entries(const char* const type_name) noexcept -> VMTypeEntry*
        {
            for (VMTypeEntry* entry{ get_vm_types() }; entry && entry->type_name; ++entry)
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
        static auto iterate_struct_entries(const char* const type_name, const char* const field_name) noexcept -> VMStructEntry*
        {
            for (VMStructEntry* entry{ get_vm_structs() }; entry && entry->type_name; ++entry)
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
            Extends is_valid_ptr with a VirtualQuery call to verify that the memory
            region containing ptr is actually committed and readable.
        */
        static auto is_readable_ptr(const void* const ptr) noexcept -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(ptr) };

            if (addr <= 0xFFFF || addr >= 0x00007FFFFFFFFFFF || (addr & 0x7) != 0)
            {
                return false;
            }

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
            {
                return false;
            }

            return mbi.State == MEM_COMMIT
                && (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_WRITECOPY)) != 0
                && (mbi.Protect & PAGE_GUARD) == 0;
        }

        /*
            @brief Checks whether a pointer is likely valid for dereferencing.
            @details
            Filters out null pointers, low sentinel values used by HotSpot to mark
            the end of linked lists, and kernel-space addresses above 0x00007FFFFFFFFFFF.
        */
        inline static auto is_valid_ptr(const void* const ptr) noexcept -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(ptr) };
            return addr > 0xFFFF && addr < 0x00007FFFFFFFFFFF;
        }

        /*
            @brief Removes GC tag bits from a HotSpot pointer to recover the real address.
            @details
            Masking with 0x00007FFFFFFFFFFF strips high GC tag bits and recovers
            the underlying canonical user-space address.
        */
        inline static auto untag_ptr(const void* const ptr) noexcept -> const void*
        {
            return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(ptr) & 0x00007FFFFFFFFFFF);
        }

        /*
            @brief Reads a 32-bit pointer field from a JVM structure and zero-extends it.
        */
        template<typename _struct>
        inline static auto read_ptr(const void* const base, const std::uint64_t offset) noexcept -> _struct*
        {
            const std::uint32_t raw{ *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(base) + offset) };
            return reinterpret_cast<_struct*>(static_cast<std::uintptr_t>(raw));
        }

        /*
            @brief Safely reads a pointer value from a memory address using ReadProcessMemory.
            @details
            Uses ReadProcessMemory to safely dereference a pointer without risking an
            access violation. Pre-checks filter out null, low, non-canonical, and unaligned
            addresses before calling ReadProcessMemory.
        */
        static auto safe_read_ptr(const void* const ptr) noexcept -> const void*
        {
            if (!ptr)
            {
                return nullptr;
            }

            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(ptr) };

            if (addr <= 0xFFFF || addr >= 0x00007FFFFFFFFFFF || (addr & 0x7) != 0)
            {
                return nullptr;
            }

            const void* result{ nullptr };
            SIZE_T bytes_read{ 0 };

            if (!ReadProcessMemory(
                GetCurrentProcess(),
                ptr,
                &result,
                sizeof(result),
                &bytes_read
            ) || bytes_read != sizeof(result))
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
            auto to_string() const -> std::string
            {
                static const VMStructEntry* const length_entry{ iterate_struct_entries("Symbol", "_length") };
                static const VMStructEntry* const body_entry{ iterate_struct_entries("Symbol", "_body") };

                try
                {
                    if (!length_entry)
                    {
                        throw jni_exception{ "Failed to find Symbol._length entry." };
                    }

                    if (!body_entry)
                    {
                        throw jni_exception{ "Failed to find Symbol._body entry." };
                    }

                    if (!safe_read_ptr(this))
                    {
                        return std::string{};
                    }

                    const std::uint16_t length{ *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(this) + length_entry->offset) };
                    const char* const body{ reinterpret_cast<const char*>(reinterpret_cast<const std::uint8_t*>(this) + body_entry->offset) };

                    if (!is_valid_ptr(body) || length == 0 || length > 0x1000)
                    {
                        return std::string{};
                    }

                    return std::string{ body, length };
                }
                catch (const std::exception& e)
                {
                    std::println("{} symbol.to_string() {}", easy_jni_error, e.what());
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
                The entries are stored immediately after the ConstantPool header.
                The base is computed by adding the size of the ConstantPool type to this.
            */
            auto get_base() const -> void**
            {
                static const VMTypeEntry* const entry{ iterate_type_entries("ConstantPool") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ConstantPool entry." };
                    }

                    return reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<constant_pool*>(this)) + entry->size);
                }
                catch (const std::exception& e)
                {
                    std::println("{} constant_pool.get_base() {}", easy_jni_error, e.what());
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
            auto get_constants() const -> constant_pool*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ConstMethod", "_constants") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ConstMethod._constants entry." };
                    }

                    return *reinterpret_cast<constant_pool**>(reinterpret_cast<std::uint8_t*>(const_cast<const_method*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} const_method.get_constants() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the name of this method.
            */
            auto get_name() const -> symbol*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ConstMethod", "_name_index") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ConstMethod._name_index entry." };
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<const_method*>(this)) + entry->offset) };
                    return reinterpret_cast<symbol*>(get_constants()->get_base()[index]);
                }
                catch (const std::exception& e)
                {
                    std::println("{} const_method.get_name() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the symbol representing the signature of this method.
            */
            auto get_signature() const -> symbol*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ConstMethod", "_signature_index") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ConstMethod._signature_index entry." };
                    }

                    const std::uint16_t index{ *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<const_method*>(this)) + entry->offset) };
                    return reinterpret_cast<symbol*>(get_constants()->get_base()[index]);
                }
                catch (const std::exception& e)
                {
                    std::println("{} const_method.get_signature() {}", easy_jni_error, e.what());
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
            auto get_i2i_entry() const -> void*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Method", "_i2i_entry") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Method._i2i_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<method*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_i2i_entry() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the from-interpreted entry point of this method.
            */
            auto get_from_interpreted_entry() const -> void*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Method", "_from_interpreted_entry") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Method._from_interpreted_entry entry." };
                    }

                    return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<method*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_from_interpreted_entry() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the access flags of this method.
                @details
                Access flags encode Java visibility modifiers (public, private, static, etc.)
                as well as JVM-internal flags such as NO_COMPILE.
            */
            auto get_access_flags() const -> std::uint32_t*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Method", "_access_flags") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Method._access_flags entry." };
                    }

                    return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<method*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_access_flags() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the internal method flags of this method.
                @details
                These flags encode HotSpot-internal method properties such as _dont_inline.
            */
            auto get_flags() const -> std::uint16_t*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Method", "_flags") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Method._flags entry." };
                    }

                    return reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(const_cast<method*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_flags() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns a pointer to the ConstMethod of this method.
            */
            auto get_const_method() const -> const_method*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Method", "_constMethod") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Method._constMethod entry." };
                    }

                    return *reinterpret_cast<const_method**>(reinterpret_cast<std::uint8_t*>(const_cast<method*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_const_method() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the name of this method as a std::string.
            */
            auto get_name() const -> std::string
            {
                const const_method* const cm{ this->get_const_method() };

                try
                {
                    if (!cm)
                    {
                        throw jni_exception{ "ConstMethod is nullptr." };
                    }

                    const symbol* const sym{ cm->get_name() };
                    if (!sym)
                    {
                        throw jni_exception{ "Symbol is nullptr." };
                    }

                    return sym->to_string();
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_name() {}", easy_jni_error, e.what());
                    return std::string{};
                }
            }

            /*
                @brief Returns the signature of this method as a std::string.
            */
            auto get_signature() const -> std::string
            {
                const const_method* const cm{ this->get_const_method() };

                try
                {
                    if (!cm)
                    {
                        throw jni_exception{ "ConstMethod is nullptr." };
                    }

                    const symbol* const sym{ cm->get_signature() };
                    if (!sym)
                    {
                        throw jni_exception{ "Symbol is nullptr." };
                    }

                    return sym->to_string();
                }
                catch (const std::exception& e)
                {
                    std::println("{} method.get_signature() {}", easy_jni_error, e.what());
                    return std::string{};
                }
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
        struct field_entry
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
            auto get_name() const -> symbol*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Klass", "_name") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Klass._name entry." };
                    }

                    if (!is_valid_ptr(this))
                    {
                        return nullptr;
                    }

                    const void* const raw{ safe_read_ptr(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };
                    symbol* const sym{ reinterpret_cast<symbol*>(const_cast<void*>(untag_ptr(raw))) };

                    return is_valid_ptr(sym) ? sym : nullptr;
                }
                catch (const std::exception& e)
                {
                    std::println("{} klass.get_name() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next sibling klass in the ClassLoaderData klass linked list.
            */
            auto get_next_link() const -> klass*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Klass", "_next_link") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find Klass._next_link entry." };
                    }

                    if (!is_valid_ptr(this))
                    {
                        return nullptr;
                    }

                    klass* const next{ *reinterpret_cast<klass**>(reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + entry->offset) };

                    return is_valid_ptr(next) ? next : nullptr;
                }
                catch (const std::exception& e)
                {
                    std::println("{} klass.get_next_link() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the number of methods declared by this InstanceKlass.
                @details
                Reads InstanceKlass._methods via its VMStruct offset, then reads
                the Array<Method*>::_length field at offset 0 of the array.
                @note This klass* must be an InstanceKlass* (i.e. a regular Java class).
            */
            auto get_methods_count() const noexcept -> std::int32_t
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !is_valid_ptr(this))
                {
                    return 0;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + entry->offset) };

                if (!is_valid_ptr(array))
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
            auto get_methods_ptr() const noexcept -> method**
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("InstanceKlass", "_methods") };

                if (!entry || !is_valid_ptr(this))
                {
                    return nullptr;
                }

                void* const array{ *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + entry->offset) };

                if (!is_valid_ptr(array))
                {
                    return nullptr;
                }

                return reinterpret_cast<method**>(reinterpret_cast<std::uint8_t*>(array) + 8);
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
            auto get_java_mirror() const noexcept -> void*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("Klass", "_java_mirror") };

                if (!entry || !is_valid_ptr(this))
                {
                    return nullptr;
                }

                // At entry->offset within the klass there is an OopHandle, which is
                // simply { oop* _obj; }.  Read the oop* stored there, then dereference
                // it to get the actual oop (full 64-bit pointer — OopStorage is not compressed).
                const void* const oop_handle_addr{ reinterpret_cast<const std::uint8_t*>(this) + entry->offset };
                const void* const oop_storage_slot{ safe_read_ptr(oop_handle_addr) };

                if (!is_valid_ptr(oop_storage_slot))
                {
                    return nullptr;
                }

                return const_cast<void*>(safe_read_ptr(oop_storage_slot));
            }

            /*
                @brief Searches the InstanceKlass _fields array for a field by name.
                @param name The exact Java field name (e.g. "health", "x", "INSTANCE").
                @return A field_entry with offset, static flag, and type descriptor,
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
            auto find_field(const std::string_view name) const noexcept -> std::optional<field_entry>
            {
                static const VMStructEntry* const fields_entry{ iterate_struct_entries("InstanceKlass", "_fields") };
                static const VMStructEntry* const constants_entry{ iterate_struct_entries("InstanceKlass", "_constants") };

                if (!fields_entry || !constants_entry || !is_valid_ptr(this))
                {
                    return std::nullopt;
                }

                void* const fields_array{
                    *reinterpret_cast<void**>(
                        reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + fields_entry->offset)
                };

                if (!is_valid_ptr(fields_array))
                {
                    return std::nullopt;
                }

                // Array<u2> layout: int32_t _length at +0, 4 bytes padding, data at +8
                const std::int32_t array_length{ *reinterpret_cast<const std::int32_t*>(fields_array) };

                static const std::int32_t field_slots{ 6 };

                if (array_length <= 0 || array_length % field_slots != 0)
                {
                    return std::nullopt;
                }

                const std::uint16_t* const data{
                    reinterpret_cast<const std::uint16_t*>(
                        reinterpret_cast<const std::uint8_t*>(fields_array) + 8)
                };

                constant_pool* const cp{
                    *reinterpret_cast<constant_pool**>(
                        reinterpret_cast<std::uint8_t*>(const_cast<klass*>(this)) + constants_entry->offset)
                };

                if (!is_valid_ptr(cp))
                {
                    return std::nullopt;
                }

                void** const cp_base{ cp->get_base() };

                if (!is_valid_ptr(cp_base))
                {
                    return std::nullopt;
                }

                const std::int32_t field_count{ array_length / field_slots };

                for (std::int32_t i{ 0 }; i < field_count; ++i)
                {
                    const std::uint16_t name_index{ data[i * field_slots + 1] };

                    if (!name_index)
                    {
                        continue;
                    }

                    const symbol* const name_sym{
                        reinterpret_cast<const symbol*>(cp_base[name_index])
                    };

                    if (!is_valid_ptr(name_sym) || name_sym->to_string() != name)
                    {
                        continue;
                    }

                    const std::uint16_t access_flags{ data[i * field_slots + 0] };
                    const std::uint16_t sig_index  { data[i * field_slots + 2] };
                    const std::uint16_t low_packed { data[i * field_slots + 4] };
                    const std::uint16_t high_packed{ data[i * field_slots + 5] };

                    // Strip the 2-bit FIELDINFO_TAG to recover the byte offset
                    const std::uint32_t packed{ (static_cast<std::uint32_t>(high_packed) << 16) | low_packed };
                    const std::uint32_t offset{ packed >> 2 };

                    const bool is_static{ (access_flags & 0x0008u) != 0u };  // JVM_ACC_STATIC

                    const symbol* const sig_sym{ reinterpret_cast<const symbol*>(cp_base[sig_index]) };
                    const std::string  signature{ is_valid_ptr(sig_sym) ? sig_sym->to_string() : std::string{} };

                    return field_entry{ offset, is_static, signature };
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
            auto get_klasses() const -> klass*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ClassLoaderData", "_klasses") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ClassLoaderData._klasses entry." };
                    }

                    if (!is_valid_ptr(this))
                    {
                        return nullptr;
                    }

                    // _klasses is a Klass* (full 8-byte native pointer), not a compressed OOP.
                    klass* const result{ *reinterpret_cast<klass**>(
                        reinterpret_cast<std::uint8_t*>(const_cast<class_loader_data*>(this)) + entry->offset
                    ) };

                    return is_valid_ptr(result) ? result : nullptr;
                }
                catch (const std::exception& e)
                {
                    std::println("{} class_loader_data.get_klasses() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the next ClassLoaderData node in the global linked list.
            */
            auto get_next() const -> class_loader_data*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ClassLoaderData", "_next") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ClassLoaderData._next entry." };
                    }

                    if (!is_valid_ptr(this))
                    {
                        return nullptr;
                    }

                    class_loader_data* const next{ reinterpret_cast<class_loader_data*>(const_cast<void*>(safe_read_ptr(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return is_valid_ptr(next) ? next : nullptr;
                }
                catch (const std::exception& e)
                {
                    std::println("{} class_loader_data.get_next() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }

            /*
                @brief Returns the Dictionary associated with this classloader.
            */
            auto get_dictionary() const -> dictionary*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ClassLoaderData", "_dictionary") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ClassLoaderData._dictionary entry." };
                    }

                    if (!is_valid_ptr(this))
                    {
                        return nullptr;
                    }

                    dictionary* const dict{ reinterpret_cast<dictionary*>(const_cast<void*>(safe_read_ptr(reinterpret_cast<const std::uint8_t*>(this) + entry->offset))) };

                    return is_valid_ptr(dict) ? dict : nullptr;
                }
                catch (const std::exception& e)
                {
                    std::println("{} class_loader_data.get_dictionary() {}", easy_jni_error, e.what());
                    return nullptr;
                }
            }
        };

        /*
            @brief Represents a HotSpot Dictionary — the per-classloader class registry.
            @details
            Each ClassLoaderData owns a Dictionary which is a hashtable mapping class
            names to their corresponding Klass* objects. It inherits from
            BasicHashtable<mtInternal> whose layout is:
            - offset 0: _table_size (int32) — number of buckets
            - offset 8: _buckets (HashtableBucket*) — pointer to the bucket array
            Each bucket slot holds a pointer to the head of a linked list of
            DictionaryEntry nodes. Each DictionaryEntry has:
            - offset 0:  _next (DictionaryEntry*) — next entry in the chain
            - offset 8:  _hash (uint32)
            - offset 16: _literal (Klass*) — the actual class
        */
        struct dictionary
        {
            inline auto get_table_size() const noexcept -> std::int32_t
            {
                return *reinterpret_cast<const std::int32_t*>(this);
            }

            inline auto get_buckets() const noexcept -> const std::uint8_t*
            {
                return *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(this) + 8);
            }

            /*
                @brief Searches this dictionary for a klass by its internal name.
            */
            auto find_klass(const std::string_view class_name) const -> klass*
            {
                const std::int32_t table_size{ get_table_size() };
                const std::uint8_t* const buckets{ get_buckets() };

                if (!is_valid_ptr(buckets) || table_size <= 0 || table_size > 0x186A0)
                {
                    return nullptr;
                }

                for (std::int32_t i{ 0 }; i < table_size; ++i)
                {
                    const std::uint8_t* entry{ reinterpret_cast<const std::uint8_t*>(untag_ptr(safe_read_ptr(buckets + i * 8))) };

                    while (is_valid_ptr(entry))
                    {
                        const void* const raw_klass{ safe_read_ptr(entry + 16) };
                        const klass* const k{ reinterpret_cast<const klass*>(untag_ptr(raw_klass)) };

                        if (is_valid_ptr(k))
                        {
                            const symbol* const sym{ k->get_name() };
                            if (is_valid_ptr(sym) && sym->to_string() == class_name)
                            {
                                return const_cast<klass*>(k);
                            }
                        }

                        entry = reinterpret_cast<const std::uint8_t*>(untag_ptr(safe_read_ptr(entry)));
                    }
                }

                return nullptr;
            }
        };

        /*
            @brief Represents the HotSpot ClassLoaderDataGraph — the global registry of all classloaders.
            @details
            ClassLoaderDataGraph::_head is a static field holding the head of a global
            linked list of ClassLoaderData nodes, one per classloader registered in the JVM.
        */
        struct class_loader_data_graph
        {
            /*
                @brief Returns the head of the global ClassLoaderData linked list.
            */
            auto get_head() const -> class_loader_data*
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("ClassLoaderDataGraph", "_head") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find ClassLoaderDataGraph._head entry." };
                    }

                    class_loader_data* const head{ *reinterpret_cast<class_loader_data* const*>(entry->address) };

                    return is_valid_ptr(head) ? head : nullptr;
                }
                catch (const std::exception& e)
                {
                    std::println("{} class_loader_data_graph.get_head() {}", easy_jni_error, e.what());
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
            auto find_klass(const std::string_view class_name) const -> klass*
            {
                // Walk every CLD's _klasses linked list.
                // The Dictionary-based approach is not used because
                // ClassLoaderData::_dictionary is absent from VMStructs in JDK 21+.
                class_loader_data* cld{ this->get_head() };

                while (is_valid_ptr(cld) && cld)
                {
                    klass* k{ cld->get_klasses() };
                    while (k && is_valid_ptr(k))
                    {
                        const symbol* const sym{ k->get_name() };
                        if (is_valid_ptr(sym) && sym->to_string() == class_name)
                        {
                            return k;
                        }
                        k = k->get_next_link();
                    }

                    class_loader_data* const next{ cld->get_next() };
                    cld = is_valid_ptr(next) ? next : nullptr;
                }

                return nullptr;
            }
        };

        /*
            @brief Represents the possible execution states of a HotSpot JavaThread.
        */
        enum class java_thread_state : std::int8_t
        {
            _thread_uninitialized  = 0,
            _thread_new            = 2,
            _thread_new_trans      = 3,
            _thread_in_native      = 4,
            _thread_in_native_trans = 5,
            _thread_in_vm          = 6,
            _thread_in_vm_trans    = 7,
            /*
                @brief The thread is currently executing Java bytecode in the interpreter.
                @note This is the only state in which method hooks are safely intercepted.
            */
            _thread_in_Java        = 8,
            _thread_in_Java_trans  = 9,
            _thread_blocked        = 10,
            _thread_blocked_trans  = 11,
            _thread_max_state      = 12
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
            auto get_thread_state() const -> java_thread_state
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    return *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} java_thread.get_thread_state() {}", easy_jni_error, e.what());
                    return java_thread_state::_thread_uninitialized;
                }
            }

            /*
                @brief Sets the execution state of this thread.
                @warning Incorrect use of this function can corrupt the JVM thread state machine.
            */
            auto set_thread_state(const java_thread_state state) const -> void
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("JavaThread", "_thread_state") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find JavaThread._thread_state entry." };
                    }

                    *reinterpret_cast<java_thread_state*>(reinterpret_cast<std::uint8_t*>(const_cast<java_thread*>(this)) + entry->offset) = state;
                }
                catch (const std::exception& e)
                {
                    std::println("{} java_thread.set_thread_state() {}", easy_jni_error, e.what());
                }
            }

            /*
                @brief Returns the current suspension flags of this thread.
            */
            auto get_suspend_flags() const -> std::uint32_t
            {
                static const VMStructEntry* const entry{ iterate_struct_entries("JavaThread", "_suspend_flags") };

                try
                {
                    if (!entry)
                    {
                        throw jni_exception{ "Failed to find JavaThread._suspend_flags entry." };
                    }

                    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(const_cast<java_thread*>(this)) + entry->offset);
                }
                catch (const std::exception& e)
                {
                    std::println("{} java_thread.get_suspend_flags() {}", easy_jni_error, e.what());
                    return 0;
                }
            }
        };

        /*
            @brief Decodes a compressed OOP to a real 64-bit pointer.
            @param compressed The 32-bit compressed OOP value read from a JVM structure.
            @return The decoded real pointer, or nullptr if compressed is 0.
            @details
            HotSpot compresses heap pointers to 32 bits using a base+shift scheme.
            The real address is recovered as: base + (compressed << shift).
            Both base and shift are read from CompressedOops::_narrow_oop via VMStructs.
        */
        static auto decode_oop_ptr(const std::uint32_t compressed) noexcept -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            static const VMStructEntry* const base_entry{ iterate_struct_entries("CompressedOops", "_narrow_oop._base") };
            static const VMStructEntry* const shift_entry{ iterate_struct_entries("CompressedOops", "_narrow_oop._shift") };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            return reinterpret_cast<void*>(base + (static_cast<std::uint64_t>(compressed) << shift));
        }

        /*
            @brief Decodes a compressed Klass pointer to a real 64-bit pointer.
            @details
            HotSpot compresses Klass pointers separately from OOPs using their own
            base+shift scheme stored in CompressedKlassPointers::_narrow_klass.
        */
        static auto decode_klass_ptr(const std::uint32_t compressed) noexcept -> void*
        {
            if (!compressed)
            {
                return nullptr;
            }

            static const VMStructEntry* const base_entry{ iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._base") };
            static const VMStructEntry* const shift_entry{ iterate_struct_entries("CompressedKlassPointers", "_narrow_klass._shift") };

            if (!base_entry || !shift_entry)
            {
                return nullptr;
            }

            const std::uint64_t base{ *reinterpret_cast<const std::uint64_t*>(base_entry->address) };
            const std::uint32_t shift{ *reinterpret_cast<const std::uint32_t*>(shift_entry->address) };

            return reinterpret_cast<void*>(base + (static_cast<std::uint64_t>(compressed) << shift));
        }

        /*
            @brief Checks whether a memory region matches a given byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto match_pattern(const std::uint8_t* const address, const std::uint8_t* const pattern, const std::size_t size) -> bool
        {
            for (std::size_t i{ 0 }; i < size; ++i)
            {
                if (pattern[i] == 0x00) continue;
                if (address[i] != pattern[i]) return false;
            }
            return true;
        }

        /*
            @brief Scans a memory region for the first occurrence of a byte pattern.
            @details
            A pattern byte of 0x00 acts as a wildcard and always matches.
        */
        inline static auto scan(
            const std::uint8_t* const start,
            const std::size_t range,
            const std::uint8_t* pattern,
            const std::size_t size
        ) -> std::uint8_t*
        {
            for (std::size_t i{ 0 }; i < range; ++i)
            {
                if (match_pattern(start + i, pattern, size))
                {
                    return const_cast<std::uint8_t*>(start + i);
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
        static auto find_stub_size(const std::uint8_t* start) -> std::size_t
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
            @note The hook is installed 8 bytes before the end of the matched pattern,
                  at the position of the mov BYTE PTR [r15+??], 0x0 instruction.
        */
        static auto find_hook_location(const void* i2i_entry) -> void*
        {
            static const constexpr std::uint8_t pattern[]
            {
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00, // mov [rsp+??], eax
                0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 // mov BYTE PTR [r15+??], 0x0
            };

            static const constexpr std::uint8_t locals_pattern[]
            {
                0x4C, 0x8B, 0x75, 0x00, 0xC3 // mov r14, QWORD PTR [rbp+??] ; ret
            };

            const std::uint8_t* const current{ reinterpret_cast<const std::uint8_t*>(i2i_entry) };
            const std::size_t scan_size{ find_stub_size(current) };

            std::uint8_t* const hook_location{ scan(current, scan_size, pattern, sizeof(pattern)) };

            try
            {
                if (!hook_location)
                {
                    throw jni_exception{ "Failed to find hook pattern." };
                }

                for (std::uint8_t* p{ hook_location }; p > current; --p)
                {
                    if (match_pattern(p, locals_pattern, sizeof(locals_pattern)))
                    {
                        locals_offset = static_cast<std::int8_t>(p[3]);
                        break;
                    }
                }

                return hook_location + sizeof(pattern) - 8;
            }
            catch (const std::exception& e)
            {
                std::println("{} find_hook_location() {}", easy_jni_error, e.what());
                return nullptr;
            }
        }

        /*
            @brief Allocates a block of executable memory within a 32-bit relative jump range
                   of a given address.
            @details
            Iterates outward from nearby_addr in steps of 65536 bytes (Windows allocation
            granularity) up to +/- 0x7FFFFFFF bytes, attempting VirtualAlloc at each step.
            This ensures the allocated block can be reached by a 5-byte relative JMP.
        */
        static auto allocate_nearby_memory(std::uint8_t* nearby_addr, const std::size_t size, const DWORD protect) noexcept -> std::uint8_t*
        {
            for (std::int64_t i{ 65536 }; i < 0x7FFFFFFF; i += 65536)
            {
                std::uint8_t* allocated{ reinterpret_cast<std::uint8_t*>(VirtualAlloc(nearby_addr + i, size, MEM_COMMIT | MEM_RESERVE, protect)) };
                if (allocated) return allocated;

                allocated = reinterpret_cast<std::uint8_t*>(VirtualAlloc(nearby_addr - i, size, MEM_COMMIT | MEM_RESERVE, protect));
                if (allocated) return allocated;
            }

            return nullptr;
        }

        using detour_t = void(*)(frame*, java_thread*, bool*);

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
            */
            inline auto get_method() const noexcept -> method*
            {
                return *reinterpret_cast<method**>(reinterpret_cast<std::uint8_t*>(const_cast<frame*>(this)) - 24);
            }

            /*
                @brief Returns a pointer to the local variables array of the currently executing method.
            */
            inline auto get_locals() const noexcept -> void**
            {
                return *reinterpret_cast<void***>(reinterpret_cast<std::uint8_t*>(const_cast<frame*>(this)) + locals_offset);
            }

            /*
                @brief Retrieves all method arguments as a typed tuple.
                @tparam types The C++ types of the method arguments in declaration order.
                @return A std::tuple containing all arguments converted to their C++ types.
                @details
                For primitive types, the raw slot value is reinterpreted directly.
                For pointer types, the compressed OOP is decoded via decode_oop_ptr().
                For index 0 on instance methods, this holds the implicit `this` reference.
            */
            template<typename... types>
            auto get_arguments() const noexcept -> std::tuple<types...>
            {
                std::int32_t index{ 0 };
                return std::tuple<types...>{ get_argument<types>(index++)... };
            }

        private:
            /*
                @brief Retrieves a single method argument at the given index.
                @tparam type The C++ type to interpret the argument as.
                @param index Zero-based index into the local variables array.
                @return The argument value, or a default-constructed value on failure.
                @details
                Arguments are stored at locals[-index].
                - Pointer types: the slot value is treated as a compressed OOP and decoded
                  via decode_oop_ptr(), then cast to the requested pointer type.
                - Primitive types (sizeof <= sizeof(void*)): bit-copied via memcpy.
            */
            template<typename type>
            auto get_argument(const std::int32_t index) const noexcept -> type
            {
                void** const locals{ get_locals() };
                if (!locals) return type{};

                void* raw{ locals[-index] };

                if constexpr (std::is_pointer_v<type>)
                {
                    if (!raw) return nullptr;
                    return reinterpret_cast<type>(decode_oop_ptr(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(raw))));
                }
                else if constexpr (sizeof(type) <= sizeof(void*))
                {
                    type result{};
                    std::memcpy(&result, &raw, sizeof(type));
                    return result;
                }
                else
                {
                    return type{};
                }
            }
        };

        /*
            @brief Installs a low-level hook on a HotSpot interpreter-to-interpreter (i2i) stub.
            @details
            midi2i_hook patches the i2i interpreter stub of a Java method at the injection
            point found by find_hook_location() with a 5-byte relative JMP instruction that
            redirects execution to an allocated trampoline stub. The trampoline saves all
            volatile registers, calls common_detour with (frame*, java_thread*, bool*), and
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
            midi2i_hook(std::uint8_t* const target, const detour_t detour)
                : target{ target }
                , allocated{ nullptr }
                , error{ true }
            {
                static const constexpr std::int32_t HOOK_SIZE{ 8 };
                static const constexpr std::int32_t JMP_SIZE{ 5 };
                static const constexpr std::int32_t JE_OFFSET{ 0x3d };
                static const constexpr std::int32_t JE_SIZE{ 6 };
                static const constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x56 };
                static const constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

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
                    0x6A, 0x00,                                     // push 0x0  ; cancel flag

                    0x48, 0x89, 0xE9,                               // mov rcx, rbp  ; frame*
                    0x4C, 0x89, 0xFA,                               // mov rdx, r15  ; java_thread*
                    0x4C, 0x8D, 0x04, 0x24,                         // lea r8,  [rsp] ; bool* cancel

                    0x48, 0x89, 0xE5,                               // mov rbp, rsp
                    0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16
                    0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 0x20

                    0xFF, 0x15, 0x2D, 0x00, 0x00, 0x00,             // call [rip+0x2D]

                    0x48, 0x89, 0xEC,                               // mov rsp, rbp

                    0x58,                                           // pop rax  ; cancel flag value
                    0x48, 0x83, 0xF8, 0x00,                         // cmp rax, 0x0

                    0x5D,                                           // pop rbp
                    0x41, 0x5B,                                     // pop r11
                    0x41, 0x5A,                                     // pop r10
                    0x41, 0x59,                                     // pop r9
                    0x41, 0x58,                                     // pop r8
                    0x5A,                                           // pop rdx
                    0x59,                                           // pop rcx
                    0x58,                                           // pop rax

                    0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je ????????  ; cancel path

                    // resume path: fall through to original code (JE not taken)

                    // cancel path
                    0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax
                    0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8]
                    0x48, 0x89, 0xEC,                               // mov rsp, rbp
                    0x5D,                                           // pop rbp
                    0x5E,                                           // pop rsi
                    0x48, 0x89, 0xDC,                               // mov rsp, rbx
                    0xFF, 0xE6,                                     // jmp rsi

                    // data slot: detour function pointer
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
                };

                this->allocated = allocate_nearby_memory(target, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READWRITE);

                try
                {
                    if (!this->allocated)
                    {
                        throw jni_exception{ "Failed to allocate memory for hook." };
                    }
                }
                catch (const std::exception& e)
                {
                    std::println("{} midi2i_hook::midi2i_hook {}", easy_jni_error, e.what());
                    return;
                }

                const std::int32_t je_delta{ static_cast<std::int32_t>(target + HOOK_SIZE - (this->allocated + HOOK_SIZE + JE_OFFSET + JE_SIZE)) };
                *reinterpret_cast<std::int32_t*>(assembly + JE_OFFSET + 2) = je_delta;

                *reinterpret_cast<detour_t*>(assembly + DETOUR_ADDRESS_OFFSET) = detour;

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
                if (this->error) return;

                static const constexpr std::uint8_t JMP_OPCODE{ 0xE9 };

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
        */
        struct hooked_method
        {
            method*  method{ nullptr };
            detour_t detour{ nullptr };
        };

        /*
            @brief Stores the association between an i2i entry point and its installed midi2i_hook.
            @details
            Since multiple Java methods can share the same i2i stub, only one trampoline
            is allocated per unique i2i entry point.
        */
        struct i2i_hook_data
        {
            void*        i2i_entry{ nullptr };
            midi2i_hook* hook{ nullptr };
        };

        /*
            @brief Global list of all currently hooked Java methods and their detour functions.
        */
        inline std::vector<hooked_method> hooked_methods{};

        /*
            @brief Global list of all i2i entry points that have been patched and their hooks.
        */
        inline std::vector<i2i_hook_data> hooked_i2i_entries{};

        /*
            @brief Common detour function invoked by the trampoline stub for every intercepted method call.
            @param f      Pointer to the current HotSpot interpreter frame.
            @param thread Pointer to the current HotSpot JavaThread.
            @param cancel Pointer to the cancel flag on the trampoline stack.
            @details
            Single entry point for all midi2i_hook trampolines. Verifies thread state,
            finds the matching per-method detour in hooked_methods, dispatches it, then
            restores thread state to _thread_in_Java.
        */
        static auto common_detour(frame* const f, java_thread* const thread, bool* const cancel) -> void
        {
            try
            {
                if (!thread || !is_valid_ptr(thread))
                {
                    throw jni_exception{ "JavaThread pointer is null or invalid." };
                }

                if (thread->get_thread_state() != java_thread_state::_thread_in_Java)
                {
                    throw jni_exception{ "JavaThread is not in _thread_in_Java state." };
                }

                const method* const current_method{ f->get_method() };

                for (const hooked_method& hook : hooked_methods)
                {
                    if (hook.method == current_method)
                    {
                        hook.detour(f, thread, cancel);
                        thread->set_thread_state(java_thread_state::_thread_in_Java);
                        return;
                    }
                }
            }
            catch (const std::exception& e)
            {
                std::println("{} common_detour() {}", easy_jni_error, e.what());
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
        inline const constexpr std::int32_t NO_COMPILE =
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
        static auto set_dont_inline(const method* const m, bool enabled) noexcept -> void
        {
            std::uint16_t* const flags{ m->get_flags() };
            if (!flags) return;

            if (enabled)
            {
                *flags |= (1 << 2);
            }
            else
            {
                *flags &= static_cast<std::uint16_t>(~(1 << 2));
            }
        }
    }

    // ─── Cache and class lookup ───────────────────────────────────────────────

    /*
        @brief Cache of klass pointers keyed by their internal class name.
        @details
        Populated by find_class() on first lookup of each class name.
        Subsequent calls return the cached klass* directly without repeating
        the full ClassLoaderDataGraph walk.
    */
    inline std::unordered_map<std::string, hotspot::klass*> classes_hs{};

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
    static auto find_class(const std::string_view class_name) -> hotspot::klass*
    {
        if (const auto it{ classes_hs.find(std::string{ class_name }) }; it != classes_hs.end())
        {
            return it->second;
        }

        try
        {
            const hotspot::class_loader_data_graph graph{};
            hotspot::klass* const k{ graph.find_klass(class_name) };

            if (!k)
            {
                return nullptr;
            }

            classes_hs.insert({ std::string{ class_name }, k });
            return k;
        }
        catch (const std::exception& e)
        {
            std::println("{} jni::find_class() for {}: {}", easy_jni_error, class_name, e.what());
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
        Stores the mapping from the C++ type_index of T to class_name in class_map,
        then verifies the class exists in the JVM by calling find_class().
        Registration is required before calling hook<T>().
    */
    template<class T>
    static auto register_class(const std::string_view class_name) noexcept -> bool
    {
        hotspot::klass* const k{ find_class(class_name) };

        if (!k)
        {
            std::println("{} register_class() for {}: class not found in JVM.", easy_jni_error, class_name);
            return false;
        }

        class_map.insert_or_assign(std::type_index{ typeid(T) }, std::string{ class_name });
        return true;
    }

    // ─── Hooking ──────────────────────────────────────────────────────────────

    /*
        @brief Installs a low-level interpreter hook on a Java method.
        @tparam T The C++ type registered for the Java class that owns the method.
                  Must have been registered via register_class<T>() beforehand.
        @param method_name The name of the Java method to hook (e.g., "toString", "update").
        @param detour      The C++ function to invoke when the hooked method is intercepted.
                           Must match: void(*)(hotspot::frame*, hotspot::java_thread*, bool*)
        @return true if the hook was successfully installed or was already active, false on failure.
        @details
        The hook installation process:
        1. Retrieve the klass for the registered Java class via find_class().
        2. Walk the InstanceKlass::_methods array to locate the target method by name.
        3. Disable JIT compilation by setting NO_COMPILE in Method._access_flags
           and _dont_inline in Method._flags.
        4. Register the method and its detour in hooked_methods for dispatch by common_detour.
        5. Check whether the i2i entry point has already been patched; if so, reuse it.
        6. If the i2i entry is new, locate the injection point via find_hook_location(),
           allocate a trampoline via midi2i_hook, and register it in hooked_i2i_entries.

        @note Unlike the JNI/JVMTI version, this implementation does not force a class
              retransformation to flush existing JIT-compiled code. Hooking a method that
              has already been JIT-compiled by the time hook() is called will not intercept
              calls that execute through compiled code. Hook early, before the method is called.
        @see midi2i_hook, common_detour, set_dont_inline, NO_COMPILE, shutdown_hooks
    */
    template<class T>
    static auto hook(const std::string_view method_name, const hotspot::detour_t detour) -> bool
    {
        try
        {
            if (!detour)
            {
                throw jni_exception{ "Detour function pointer is null." };
            }

            const auto it{ class_map.find(std::type_index{ typeid(T) }) };
            if (it == class_map.end())
            {
                throw jni_exception{ std::format("Class not registered for type {}. Did you call register_class<T>()?", typeid(T).name()) };
            }

            hotspot::klass* const k{ find_class(it->second) };
            if (!k)
            {
                throw jni_exception{ std::format("Class '{}' not found in JVM.", it->second) };
            }

            const std::int32_t count{ k->get_methods_count() };
            hotspot::method** const methods_ptr{ k->get_methods_ptr() };

            if (!methods_ptr || count <= 0)
            {
                throw jni_exception{ std::format("No methods found on class '{}'.", it->second) };
            }

            hotspot::method* found_method{ nullptr };
            for (std::int32_t i{ 0 }; i < count; ++i)
            {
                hotspot::method* const m{ methods_ptr[i] };
                if (m && hotspot::is_valid_ptr(m) && m->get_name() == method_name)
                {
                    found_method = m;
                    break;
                }
            }

            if (!found_method)
            {
                throw jni_exception{ std::format("Method '{}' not found in class '{}'.", method_name, it->second) };
            }

            for (const hotspot::hooked_method& hm : hotspot::hooked_methods)
            {
                if (hm.method == found_method)
                {
                    return true;
                }
            }

            hotspot::set_dont_inline(found_method, true);

            std::uint32_t* const flags{ found_method->get_access_flags() };
            if (!flags)
            {
                throw jni_exception{ "Failed to retrieve access flags." };
            }
            *flags |= hotspot::NO_COMPILE;

            hotspot::hooked_methods.push_back({ found_method, detour });

            void* const i2i{ found_method->get_i2i_entry() };
            if (!i2i)
            {
                throw jni_exception{ "Failed to retrieve i2i entry." };
            }

            for (const hotspot::i2i_hook_data& hd : hotspot::hooked_i2i_entries)
            {
                if (hd.i2i_entry == i2i)
                {
                    return true;
                }
            }

            std::uint8_t* const target{ reinterpret_cast<std::uint8_t*>(hotspot::find_hook_location(i2i)) };
            if (!target)
            {
                throw jni_exception{ "Failed to find hook location in i2i stub." };
            }

            hotspot::midi2i_hook* const hook_instance{ new hotspot::midi2i_hook(target, hotspot::common_detour) };
            if (hook_instance->has_error())
            {
                delete hook_instance;
                throw jni_exception{ "midi2i_hook installation failed." };
            }

            hotspot::hooked_i2i_entries.push_back({ i2i, hook_instance });
            return true;
        }
        catch (const std::exception& e)
        {
            std::println("{} jni::hook() for {}: {}", easy_jni_error, method_name, e.what());
            return false;
        }
    }

    /*
        @brief Removes all active interpreter hooks and restores the JVM to its original state.
        @details
        Phase 1: Deletes each midi2i_hook instance, restoring original bytes and freeing memory.
        Phase 2: Clears _dont_inline and NO_COMPILE flags on all hooked methods.
        Both vectors are cleared after cleanup.
    */
    static auto shutdown_hooks() noexcept -> void
    {
        for (const hotspot::i2i_hook_data& hd : hotspot::hooked_i2i_entries)
        {
            delete hd.hook;
        }

        for (const hotspot::hooked_method& hm : hotspot::hooked_methods)
        {
            hotspot::set_dont_inline(hm.method, false);

            std::uint32_t* const flags{ hm.method->get_access_flags() };
            if (flags)
            {
                *flags &= static_cast<std::uint32_t>(~hotspot::NO_COMPILE);
            }
        }

        hotspot::hooked_methods.clear();
        hotspot::hooked_i2i_entries.clear();
    }

    // ─── Field access ─────────────────────────────────────────────────────────

    /*
        @brief Cache of field entries keyed by klass* then field name.
        @details
        Populated lazily by find_field() on the first lookup of each (klass, name) pair.
        Raw klass pointers are safe as keys for process-lifetime injection; class unloading
        would invalidate them, but that is not a concern for the typical use case here.
    */
    inline std::unordered_map<hotspot::klass*, std::unordered_map<std::string, hotspot::field_entry>> field_cache{};

    /*
        @brief Looks up and caches a field entry for a named field on a klass.
        @param k     The klass that declares the field (obtain via find_class()).
                     Only the declaring class is searched — not superclasses.
        @param name  The exact Java field name.
        @return The cached field_entry, or std::nullopt if the field is not found.
        @details
        On the first call for a given (k, name) pair the full InstanceKlass._fields
        array is walked; subsequent calls return the cached result directly.
    */
    static auto find_field(hotspot::klass* const k, const std::string_view name) -> std::optional<hotspot::field_entry>
    {
        if (!k || !hotspot::is_valid_ptr(k))
        {
            std::println("{} jni::find_field() for '{}': klass pointer is null.", easy_jni_error, name);
            return std::nullopt;
        }

        auto& class_fields{ field_cache[k] };
        const std::string name_str{ name };

        if (const auto it{ class_fields.find(name_str) }; it != class_fields.end())
        {
            return it->second;
        }

        const auto entry{ k->find_field(name) };

        if (!entry)
        {
            std::println("{} jni::find_field() for '{}': field not declared on klass.", easy_jni_error, name);
            return std::nullopt;
        }

        class_fields.emplace(name_str, *entry);
        return entry;
    }

    /*
        @brief Reads an instance field from a Java object by name.
        @tparam T      The C++ scalar type to read the field as (int, float, bool,
                       double, long long, short, char, std::byte, uint32_t, etc.).
        @param object  Decoded pointer to the Java object (not a compressed OOP).
                       Obtain it from frame->get_arguments<void*>() which already
                       calls decode_oop_ptr() internally.
        @param k       The klass that declares the field (from find_class()).
        @param name    The exact Java field name.
        @return The field value as T, or a default-constructed T on failure.
        @note For reference-type fields specify T = uint32_t to receive the raw
              compressed OOP, then pass it to hotspot::decode_oop_ptr() yourself.
    */
    template<typename T>
    static auto get_field(void* const object, hotspot::klass* const k, const std::string_view name) -> T
    {
        static_assert(std::is_trivially_copyable_v<T>, "get_field<T>: T must be trivially copyable.");

        try
        {
            if (!object || !hotspot::is_valid_ptr(object))
            {
                throw jni_exception{ "Object pointer is null or invalid." };
            }

            const auto entry{ find_field(k, name) };

            if (!entry)
            {
                throw jni_exception{ std::format("Instance field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                throw jni_exception{ std::format("'{}' is a static field; use jni::get_static_field<T>().", name) };
            }

            T result{};
            std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(object) + entry->offset, sizeof(T));
            return result;
        }
        catch (const std::exception& e)
        {
            std::println("{} jni::get_field<{}>('{}') {}", easy_jni_error, typeid(T).name(), name, e.what());
            return T{};
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
    template<typename T>
    static auto set_field(void* const object, hotspot::klass* const k, const std::string_view name, const T value) -> void
    {
        static_assert(std::is_trivially_copyable_v<T>, "set_field<T>: T must be trivially copyable.");

        try
        {
            if (!object || !hotspot::is_valid_ptr(object))
            {
                throw jni_exception{ "Object pointer is null or invalid." };
            }

            const auto entry{ find_field(k, name) };

            if (!entry)
            {
                throw jni_exception{ std::format("Instance field '{}' not found.", name) };
            }

            if (entry->is_static)
            {
                throw jni_exception{ std::format("'{}' is a static field; use set_static_field().", name) };
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(object) + entry->offset, &value, sizeof(T));
        }
        catch (const std::exception& e)
        {
            std::println("{} jni::set_field<{}>('{}') {}", easy_jni_error, typeid(T).name(), name, e.what());
        }
    }

    /*
        @brief Reads a static field from the java.lang.Class mirror of a klass.
        @tparam T    The C++ scalar type to read the field as.
        @param k     The klass that declares the static field.
        @param name  The exact Java field name.
        @return The field value as T, or a default-constructed T on failure.
        @details
        Static field values live inside the java.lang.Class mirror object at the
        byte offset stored in InstanceKlass._fields. The mirror is obtained via
        Klass::_java_mirror (an OopHandle — JDK 17+).
    */
    template<typename T>
    static auto get_static_field(hotspot::klass* const k, const std::string_view name) -> T
    {
        static_assert(std::is_trivially_copyable_v<T>, "get_static_field<T>: T must be trivially copyable.");

        try
        {
            const auto entry{ find_field(k, name) };

            if (!entry)
            {
                throw jni_exception{ std::format("Static field '{}' not found.", name) };
            }

            if (!entry->is_static)
            {
                throw jni_exception{ std::format("'{}' is an instance field; use get_field().", name) };
            }

            void* const mirror{ k->get_java_mirror() };

            if (!mirror || !hotspot::is_valid_ptr(mirror))
            {
                throw jni_exception{ "Failed to retrieve java.lang.Class mirror." };
            }

            T result{};
            std::memcpy(&result, reinterpret_cast<const std::uint8_t*>(mirror) + entry->offset, sizeof(T));
            return result;
        }
        catch (const std::exception& e)
        {
            std::println("{} jni::get_static_field<{}>('{}') {}", easy_jni_error, typeid(T).name(), name, e.what());
            return T{};
        }
    }

    /*
        @brief Writes a value to a static field in the java.lang.Class mirror of a klass.
        @tparam T    The C++ scalar type to write.
        @param k     The klass that declares the static field.
        @param name  The exact Java field name.
        @param value The value to write.
    */
    template<typename T>
    static auto set_static_field(hotspot::klass* const k, const std::string_view name, const T value) -> void
    {
        static_assert(std::is_trivially_copyable_v<T>, "set_static_field<T>: T must be trivially copyable.");

        try
        {
            const auto entry{ find_field(k, name) };

            if (!entry)
            {
                throw jni_exception{ std::format("Static field '{}' not found.", name) };
            }

            if (!entry->is_static)
            {
                throw jni_exception{ std::format("'{}' is an instance field; use set_field().", name) };
            }

            void* const mirror{ k->get_java_mirror() };

            if (!mirror || !hotspot::is_valid_ptr(mirror))
            {
                throw jni_exception{ "Failed to retrieve java.lang.Class mirror." };
            }

            std::memcpy(reinterpret_cast<std::uint8_t*>(mirror) + entry->offset, &value, sizeof(T));
        }
        catch (const std::exception& e)
        {
            std::println("{} jni::set_static_field<{}>('{}') {}", easy_jni_error, typeid(T).name(), name, e.what());
        }
    }

    // ─── Field proxy ──────────────────────────────────────────────────────────

    /*
        @brief Lightweight proxy to a single Java field value in memory.
        @details
        Returned by jni::object::get_field().  Reads the field value on demand,
        dispatches the correct C++ type from the JVM type descriptor (signature),
        and returns a typed copy — not a raw-pointer alias.

        Usage inside a wrapper class:

            // No trailing return type needed — auto deduces field_proxy::value,
            // which converts implicitly to the target type at the call site.
            auto is_connected() { return get_field("isConnected")->get(); }
            auto get_health()   { return get_field("health")->get(); }

            // If you want a concrete type inside the method, cast before returning:
            auto get_max_hp()   { return static_cast<int>(get_field("maxHp")->get()); }

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
                auto  v = proxy->get();   // type is field_proxy::value; converts lazily

            For reference-type fields (signature starts with 'L' or '[') the stored
            alternative is uint32_t (the raw compressed OOP).  Pass it to
            hotspot::decode_oop_ptr() to recover the real 64-bit address.
        */
        struct value
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

            /*
                @brief Converts the stored value to T via static_cast.
                Works for any combination of the nine stored types and any
                numeric or bool target type.
            */
            template<typename T>
            operator T() const noexcept
            {
                return std::visit([](auto v) noexcept -> T {
                    return static_cast<T>(v);
                }, data);
            }
        };

        /*
            @param ptr       Direct pointer to the field's bytes in JVM memory
                             (decoded object address + offset for instance fields;
                              java.lang.Class mirror + offset for static fields).
            @param signature JVM type descriptor, e.g. "I", "Z", "Ljava/lang/String;"
            @param is_static true when JVM_ACC_STATIC is set on the field.
        */
        field_proxy(void* ptr, std::string signature, const bool is_static) noexcept
            : ptr_{ ptr }
            , signature_{ std::move(signature) }
            , is_static_{ is_static }
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
            The returned value is a copy — safe to store and return from methods.
        */
        auto get() const noexcept -> value
        {
            if (!ptr_) return value{ std::int32_t{} };

            if (signature_ == "Z") { bool           v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "B") { std::int8_t    v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "S") { std::int16_t   v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "I") { std::int32_t   v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "J") { std::int64_t   v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "F") { float          v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "D") { double         v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }
            if (signature_ == "C") { std::uint16_t  v{}; std::memcpy(&v, ptr_, sizeof(v)); return value{ v }; }

            // Reference or array type — store compressed OOP
            std::uint32_t v{};
            std::memcpy(&v, ptr_, sizeof(v));
            return value{ v };
        }

        /*
            @brief Writes val into the field's storage.
            @tparam T Must be trivially copyable; sizeof(T) must match the field width.
            @note For reference-type fields T should be uint32_t (compressed OOP).
        */
        template<typename T>
        auto set(const T val) const noexcept -> void
        {
            static_assert(std::is_trivially_copyable_v<T>,
                "field_proxy::set: value type must be trivially copyable.");
            if (ptr_) std::memcpy(ptr_, &val, sizeof(T));
        }

        /*
            @brief Returns the JVM type descriptor of this field (e.g. "I", "Z", "J").
        */
        auto signature() const noexcept -> std::string_view
        {
            return signature_;
        }

        auto is_static() const noexcept -> bool
        {
            return is_static_;
        }

    private:
        void*       ptr_;
        std::string signature_;
        bool        is_static_;
    };

    // ─── Object base class ────────────────────────────────────────────────────

    /*
        @brief Alias for a decoded (uncompressed) Java object pointer.
        @details
        This is the type passed to jni::object constructors. Obtain it from a
        hook frame via frame->get_arguments<jni::oop>(), which internally calls
        hotspot::decode_oop_ptr() to convert the 32-bit compressed OOP to a full
        64-bit address.  It is NOT a JNI global reference — no GC handles are
        created and the pointer remains valid only for the duration of the hook.
    */
    using oop = void*;

    /*
        @brief Base class for C++ wrappers around live Java objects.
        @details
        Derive from this class to create a typed façade for a Java class.
        get_field() handles both instance and static fields automatically —
        the library reads JVM_ACC_STATIC from the InstanceKlass._fields array
        so there is no need for a separate get_static_field().

        Example:

            class http_client : public jni::object
            {
            public:
                explicit http_client(jni::oop instance)
                    : jni::object{ instance }
                {}

                // auto return — field_proxy::value converts implicitly at the call site
                auto is_connected() { return get_field("isConnected")->get(); }
                auto get_health()   { return get_field("health")->get(); }

                // Cast inside the method if you want a concrete return type
                auto get_timeout()  { return static_cast<int>(get_field("timeout")->get()); }

                // Static field — get_field detects JVM_ACC_STATIC automatically
                auto get_version()  { return get_field("VERSION")->get(); }

                // Writing a field
                auto set_health(int hp) { get_field("health")->set(hp); }
            };

            jni::register_class<http_client>("com/example/HttpClient");

            // Inside a hook detour:
            auto [self] = frame->get_arguments<jni::oop>();
            http_client client{ self };
            bool ok  = client.is_connected(); // operator bool()  fires
            int  hp  = client.get_health();   // operator int()   fires
            client.set_health(100);

        @note The wrapped pointer is a raw decoded OOP, not a JNI global reference.
              It is valid for the duration of the hook invocation only.
    */
    class object
    {
    public:
        /*
            @brief Wraps a decoded Java object pointer.
            @param instance Decoded OOP from frame->get_arguments<jni::oop>().
                            May be nullptr if the Java reference is null.
        */
        explicit object(oop instance = nullptr) noexcept
            : instance_{ instance }
        {
        }

        virtual ~object() = default;

        object(const object&) = default;
        auto operator=(const object&) -> object& = default;

        object(object&& other) noexcept
            : instance_{ other.instance_ }
        {
            other.instance_ = nullptr;
        }

        auto operator=(object&& other) noexcept -> object&
        {
            if (this != &other)
            {
                instance_     = other.instance_;
                other.instance_ = nullptr;
            }
            return *this;
        }

        /*
            @brief Returns the raw decoded OOP pointer held by this wrapper.
        */
        auto get_instance() const noexcept -> oop
        {
            return instance_;
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

            The klass is resolved from class_map via typeid(*this) (dynamic type),
            so the derived C++ class must have been registered with register_class<T>()
            before this is called.  Field entries are cached after the first lookup.

            @note Dereferencing a nullopt with -> is undefined behaviour.
                  In production code always check the optional, or assert field names
                  are correct at development time.
        */
        auto get_field(const std::string_view name) const -> std::optional<field_proxy>
        {
            hotspot::klass* const k{ resolve_klass() };
            if (!k)
            {
                return std::nullopt;
            }

            const auto entry{ jni::find_field(k, name) };
            if (!entry)
            {
                return std::nullopt;
            }

            if (entry->is_static)
            {
                void* const mirror{ k->get_java_mirror() };
                if (!mirror || !hotspot::is_valid_ptr(mirror))
                {
                    std::println("{} object::get_field('{}') failed to get java.lang.Class mirror.", easy_jni_error, name);
                    return std::nullopt;
                }
                void* const ptr{ reinterpret_cast<std::uint8_t*>(mirror) + entry->offset };
                return field_proxy{ ptr, entry->signature, true };
            }

            if (!instance_)
            {
                std::println("{} object::get_field('{}') instance pointer is null.", easy_jni_error, name);
                return std::nullopt;
            }

            void* const ptr{ reinterpret_cast<std::uint8_t*>(instance_) + entry->offset };
            return field_proxy{ ptr, entry->signature, false };
        }

    protected:
        /*
            @brief The raw decoded OOP pointer to the wrapped Java object.
        */
        oop instance_{ nullptr };

    private:
        /*
            @brief Resolves the HotSpot klass for the dynamic type of this object.
            @details
            Uses typeid(*this) to look up the class name in class_map, then
            calls find_class() which walks the ClassLoaderDataGraph (cached after
            the first call).
            @return Pointer to the klass, or nullptr if the type is not registered.
        */
        auto resolve_klass() const -> hotspot::klass*
        {
            const auto it{ class_map.find(std::type_index{ typeid(*this) }) };
            if (it == class_map.end())
            {
                std::println("{} object::resolve_klass() type '{}' not registered via register_class<T>().",
                    easy_jni_error, typeid(*this).name());
                return nullptr;
            }

            hotspot::klass* const k{ find_class(it->second) };
            if (!k)
            {
                std::println("{} object::resolve_klass() class '{}' not found in JVM.", easy_jni_error, it->second);
            }

            return k;
        }
    };
}
