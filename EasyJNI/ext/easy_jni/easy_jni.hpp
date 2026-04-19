#pragma once

#include <print>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>

#include <windows.h>

namespace jni
{
    /*
        @brief Maps C++ wrapper types to their corresponding internal Java class names.
        @details
        Populated by register_class() when a C++ wrapper type is associated with
        a Java class name. Used by install_hook() and other APIs that need to look
        up the Java class corresponding to a given C++ wrapper type at runtime.
        Keys are std::type_index values derived from typeid() of the C++ wrapper type.
        Values are the internal JVM class names using '/' separators.
        @see register_class, install_hook
    */
    inline std::unordered_map<std::type_index, std::string> class_map{};

    template<class wrapped_class>
    static auto register_class(const std::string_view class_name) noexcept -> bool;

    namespace util
    {
        inline static auto is_valid_ptr(const void* const ptr) noexcept -> bool;
        inline static auto untag_ptr(const void* const ptr) noexcept -> const void*;
        static auto safe_read_ptr(const void* const ptr) noexcept -> const void*;
    }

    namespace hotspot
    {
        struct VMStructEntry;
        struct VMTypeEntry;

        inline auto get_jvm_module() noexcept -> HMODULE;
        inline auto get_vm_types() noexcept-> jni::hotspot::VMTypeEntry*;
        inline auto get_vm_structs() noexcept -> jni::hotspot::VMStructEntry*;
        static auto iterate_type_entries(const char* const type_name) noexcept -> jni::hotspot::VMTypeEntry*;
        static auto iterate_struct_entries(const char* const type_name, const char* const field_name) noexcept -> jni::hotspot::VMStructEntry*;

		struct symbol;
        struct klass;
        struct dictionary;
        struct class_loader_data;
		struct class_loader_data_graph;

        /*
            @brief Cache of klass pointers keyed by their internal class name.
            @details
            Populated by find_class() on first lookup of each class name.
            Subsequent calls for the same name return the cached klass* directly
            without repeating the ClassLoaderDataGraph walk, making repeated
            lookups of the same class effectively free.
            Keys use the internal JVM '/' separator format (e.g. "java/lang/String")
            consistent with the format accepted by find_class().
            @note Entries remain valid as long as the corresponding class is loaded.
                  Classes loaded by short-lived classloaders may be unloaded and
                  their cached klass* will become dangling. For stable classes such
                  as JDK classes and main application classes this is never a concern.
            @see find_class
        */
        inline std::unordered_map<std::string, jni::hotspot::klass*> loaded_classes{};

        static auto find_class(const std::string_view class_name) noexcept -> jni::hotspot::klass*;
	}

    namespace util
    {
        /*
            @brief Checks whether a pointer is likely valid for dereferencing.
            @param ptr The pointer to validate.
            @return true if the pointer is within the valid user-space range, false otherwise.
            @details
            Filters out null pointers, low sentinel values used by HotSpot to mark
            the end of linked lists, and kernel-space addresses above 0x00007FFFFFFFFFFF.
            This check is intentionally lightweight as it is called on every node
            during dictionary and class loader data walks.
            @note This is not a guarantee of validity. Always follow with safe_read_ptr
                  when dereferencing unknown pointer chains.
        */
        inline static auto is_valid_ptr(const void* const ptr) noexcept
            -> bool
        {
            const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(ptr) };

            return addr > 0xFFFF && addr < 0x00007FFFFFFFFFFF;
        }

        /*
            @brief Removes GC tag bits from a HotSpot pointer to recover the real address.
            @param ptr The potentially tagged pointer to untag.
            @return The untagged pointer with only the lower 47 bits preserved.
            @details
            HotSpot's garbage collectors use the low and high bits of OOP and pointer
            values to store metadata such as mark words and forwarding pointers during
            GC cycles. Masking with 0x00007FFFFFFFFFFF strips these high bits and
            recovers the underlying canonical user-space address.
            @note Always follow with is_valid_ptr on the result before dereferencing.
        */
        inline static auto untag_ptr(const void* const ptr) noexcept
            -> const void*
        {
            return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(ptr) & 0x00007FFFFFFFFFFF);
        }

        /*
            @brief Safely reads a pointer value from a memory address using ReadProcessMemory.
            @param ptr The address to read a pointer from.
            @return The pointer value stored at ptr, or nullptr if the read fails.
            @details
            Uses ReadProcessMemory to safely dereference a pointer without risking an
            access violation. Before attempting the read, a fast pre-check filters out
            obviously invalid addresses: null, low addresses below 0xFFFF, non-canonical
            addresses above 0x00007FFFFFFFFFFF, and unaligned addresses not on an 8-byte
            boundary. These cases are rejected immediately without calling ReadProcessMemory.
            @note ReadProcessMemory has a higher per-call cost than a direct dereference.
                  Use only on entry and next pointers during dictionary walks, not on
                  every field read inside a validated entry.
        */
        static auto safe_read_ptr(const void* const ptr) noexcept
            -> const void*
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
            std::size_t bytes_read{ 0 };

            if (!ReadProcessMemory(GetCurrentProcess(),ptr,&result,sizeof(result), &bytes_read) || bytes_read != sizeof(result))
            {
                return nullptr;
            }

            return result;
        }
    }

    namespace hotspot
    {
        /*
            @brief Describes a single field of a HotSpot internal type as exported by the JVM.
            @details
            Each entry in the gHotSpotVMStructs array describes one field of one HotSpot
            internal type. The array is exported by jvm.dll and populated at JVM startup.
            It is the canonical source of truth for field offsets and static addresses
            within the JVM process, allowing native code to access HotSpot internals
            without hardcoding offsets that change between JVM versions or builds.
            The array is terminated by an entry whose type_name is nullptr.
            @see gHotSpotVMStructs, iterate_struct_entries, VMTypeEntry
        */
        struct VMStructEntry
        {
            /*
                @brief Fully qualified name of the HotSpot type that owns this field.
                @details
                For example "Method", "ConstantPool", "JavaThread", "Klass".
                Used as the primary key when searching the array via iterate_struct_entries.
                nullptr on the sentinel terminator entry.
            */
            const char* type_name;

            /*
                @brief Name of the field within the owning type.
                @details
                For example "_constMethod", "_i2i_entry", "_thread_state".
                Used as the secondary key when searching the array via iterate_struct_entries.
            */
            const char* field_name;

            /*
                @brief JVM type string describing the field's declared type.
                @details
                For example "void*", "int", "bool", "ConstMethod*".
                Informational only — not used for offset or address resolution.
            */
            const char* type_string;

            /*
                @brief Non-zero if this field is a static (class-level) field, zero if instance.
                @details
                Static fields have a meaningful address value and an offset of zero.
                Instance fields have a meaningful offset value and an address of nullptr.
                Always check this flag before deciding whether to use offset or address.
            */
            std::int32_t is_static;

            /*
                @brief Byte offset of this field from the start of an instance of the owning type.
                @details
                Only meaningful when is_static is zero.
                Apply this offset to a pointer to an instance of type_name to reach the field:
                reinterpret_cast<const std::uint8_t*>(instance) + entry->offset
                Zero for static fields.
            */
            std::uint64_t offset;

            /*
                @brief Absolute address of this field in the JVM process memory.
                @details
                Only meaningful when is_static is non-zero.
                Points directly to the storage location of the static field value.
                For pointer fields, dereference once to obtain the value:
                *reinterpret_cast<T*>(entry->address)
                nullptr for instance fields.
            */
            void* address;
        };

        /*
            @brief Describes a single HotSpot internal type as exported by the JVM.
            @details
            Each entry in the gHotSpotVMTypes array describes one HotSpot internal type.
            The array is exported by jvm.dll and populated at JVM startup.
            It provides metadata about each type including its inheritance hierarchy,
            classification, and size in bytes, allowing native code to correctly
            interpret and navigate HotSpot internal structures without hardcoding
            type sizes that change between JVM versions or builds.
            The array is terminated by an entry whose type_name is nullptr.
            @see gHotSpotVMTypes, iterate_type_entries, VMStructEntry
        */
        struct VMTypeEntry
        {
            /*
                @brief Fully qualified name of this HotSpot type.
                @details
                For example "Method", "ConstantPool", "JavaThread", "Klass", "Symbol".
                Used as the primary key when searching the array via iterate_type_entries.
                nullptr on the sentinel terminator entry.
            */
            const char* type_name;

            /*
                @brief Name of the immediate superclass of this type, or nullptr if none.
                @details
                For example "Klass" is the superclass of "InstanceKlass".
                nullptr for root types such as "Thread" or types with no superclass
                in the HotSpot type hierarchy.
                Informational only — not used for size or offset resolution.
            */
            const char* superclass_name;

            /*
                @brief Non-zero if this type is an OOP (ordinary object pointer) type.
                @details
                OOP types represent Java heap objects and are subject to garbage
                collection. Their pointers may be compressed on 64-bit JVMs with
                compressed OOPs enabled and must be decoded via decode_oop_ptr
                before dereferencing.
                Zero for non-OOP types such as metadata and native structs.
            */
            std::int32_t is_oop_type;

            /*
                @brief Non-zero if this type is an integer type.
                @details
                Integer types include primitive integral types used internally
                by HotSpot such as int, jint, intptr_t, and similar.
                Zero for pointer, OOP, and compound struct types.
            */
            std::int32_t is_integer_type;

            /*
                @brief Non-zero if this integer type is unsigned.
                @details
                Only meaningful when is_integer_type is non-zero.
                Indicates that the integer type should be interpreted as unsigned
                when reading or printing its value.
                Zero for signed integer types and all non-integer types.
            */
            std::int32_t is_unsigned;

            /*
                @brief Size in bytes of this type in the JVM process memory.
                @details
                The primary use of VMTypeEntry is to retrieve this size value.
                It is used to compute the base address of variable-length structures
                that store their data immediately after the fixed-size header, such
                as ConstantPool whose entry array begins at:
                reinterpret_cast<const std::uint8_t*>(constant_pool_ptr) + entry->size
                The size reflects the actual in-process layout including padding and
                alignment, not the logical or source-level sizeof the type.
            */
            std::uint64_t size;
        };

        /*
            @brief Returns the module handle for jvm.dll loaded in the current process.
            @return Handle to jvm.dll, or nullptr if jvm.dll is not loaded.
            @details
            Retrieves and caches the HMODULE for jvm.dll on first call using
            GetModuleHandleA. The result is stored in a static local variable
            so subsequent calls return immediately without repeated system calls.
            This handle is used as the base for resolving gHotSpotVMTypes and
            gHotSpotVMStructs via GetProcAddress, and for computing the scan
            range when searching jvm.dll for internal function patterns such as
            JavaCalls::call_virtual.
            @note jvm.dll must already be loaded in the process before this
                  function is called for the first time. Since EasyJNI is injected
                  into a running JVM process, this is always guaranteed to be the
                  case by the time any EasyJNI code executes.
            @note The returned handle is not reference counted and must not be
                  passed to FreeLibrary.
            @see get_vm_types, get_vm_structs
        */
        inline auto get_jvm_module() noexcept
            -> HMODULE
        {
            static HMODULE module{ GetModuleHandleA("jvm.dll") };

            return module;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM type entries.
            @return Pointer to the first entry in the gHotSpotVMTypes array exported
                    by jvm.dll, or nullptr if the symbol could not be resolved.
            @details
            Resolves gHotSpotVMTypes from jvm.dll via GetProcAddress on first call
            and caches both the raw FARPROC and the typed pointer in static local
            variables so subsequent calls return immediately without repeated
            symbol resolution.
            The returned array describes every HotSpot internal type known to the
            running JVM, including its name, superclass, classification flags, and
            size in bytes. It is terminated by an entry whose type_name is nullptr.
            This array is the sole source used by iterate_type_entries to look up
            type metadata such as the size of ConstantPool at runtime.
            @note The array is populated by the JVM at startup and remains valid
                  and immutable for the lifetime of the JVM process. The returned
                  pointer may be used freely without synchronization.
            @note get_jvm_module() must return a valid handle for this function
                  to succeed. Since EasyJNI executes inside a running JVM process,
                  jvm.dll is always loaded before any EasyJNI code runs.
            @see VMTypeEntry, iterate_type_entries, get_vm_structs, get_jvm_module
        */
        inline auto get_vm_types() noexcept
            -> jni::hotspot::VMTypeEntry*
        {
            static FARPROC proc{ GetProcAddress(get_jvm_module(), "gHotSpotVMTypes") };
            
            static VMTypeEntry* ptr{ *reinterpret_cast<VMTypeEntry**>(proc) };
            
            return ptr;
        }

        /*
            @brief Returns a pointer to the global array of HotSpot VM struct entries.
            @return Pointer to the first entry in the gHotSpotVMStructs array exported
                    by jvm.dll, or nullptr if the symbol could not be resolved.
            @details
            Resolves gHotSpotVMStructs from jvm.dll via GetProcAddress on first call
            and caches both the raw FARPROC and the typed pointer in static local
            variables so subsequent calls return immediately without repeated
            symbol resolution.
            The returned array describes every field of every HotSpot internal type
            known to the running JVM, including its name, type string, static flag,
            byte offset within the owning type, and absolute address for static fields.
            It is terminated by an entry whose type_name is nullptr.
            This array is the sole source used by iterate_struct_entries to resolve
            field offsets and static addresses at runtime without hardcoding values
            that change between JVM versions or builds.
            @note The array is populated by the JVM at startup and remains valid
                  and immutable for the lifetime of the JVM process. The returned
                  pointer may be used freely without synchronization.
            @note get_jvm_module() must return a valid handle for this function
                  to succeed. Since EasyJNI executes inside a running JVM process,
                  jvm.dll is always loaded before any EasyJNI code runs.
            @see VMStructEntry, iterate_struct_entries, get_vm_types, get_jvm_module
        */
        inline auto get_vm_structs() noexcept
            -> jni::hotspot::VMStructEntry*
        {
            static FARPROC proc{ GetProcAddress(get_jvm_module(), "gHotSpotVMStructs") };
            
            static VMStructEntry* ptr{ *reinterpret_cast<VMStructEntry**>(proc) };
            
            return ptr;
        }

        /*
            @brief Searches the gHotSpotVMTypes array for a type entry by name.
            @param type_name The name of the HotSpot type to find (e.g. "ConstantPool", "Method").
            @return Pointer to the matching VMTypeEntry if found, nullptr otherwise.
            @details
            Walks the gHotSpotVMTypes array linearly from the first entry until it
            finds an entry whose type_name matches the provided string, or until the
            null terminator entry is reached.
            The returned pointer points directly into the gHotSpotVMTypes array and
            remains valid for the lifetime of the JVM process.
            The primary use of this function is to retrieve the size of a HotSpot type
            in order to locate variable-length data stored immediately after its header,
            such as the ConstantPool entry array.
            @note Linear scan cost is paid only once per unique type_name since callers
                  store the result in a static local variable.
            @see VMTypeEntry, get_vm_types, iterate_struct_entries
        */
        static auto iterate_type_entries(const char* const type_name) noexcept
            -> jni::hotspot::VMTypeEntry*
        {
            for (jni::hotspot::VMTypeEntry* entry{ jni::hotspot::get_vm_types() }; entry->type_name; ++entry)
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
            @param type_name  The name of the HotSpot type that owns the field (e.g. "Method").
            @param field_name The name of the field to find (e.g. "_constMethod", "_i2i_entry").
            @return Pointer to the matching VMStructEntry if found, nullptr otherwise.
            @details
            Walks the gHotSpotVMStructs array linearly from the first entry until it
            finds an entry whose type_name and field_name both match the provided strings,
            or until the null terminator entry is reached.
            The returned pointer points directly into the gHotSpotVMStructs array and
            remains valid for the lifetime of the JVM process.
            For instance fields, use entry->offset to locate the field within an object:
                reinterpret_cast<const std::uint8_t*>(instance) + entry->offset
            For static fields, use entry->address to access the field directly:
                *reinterpret_cast<T*>(entry->address)
            @note Linear scan cost is paid only once per unique (type_name, field_name) pair
                  since callers store the result in a static local variable.
            @see VMStructEntry, get_vm_structs, iterate_type_entries
        */
        static auto iterate_struct_entries(const char* const type_name, const char* const field_name) noexcept
            -> jni::hotspot::VMStructEntry*
        {
            for (jni::hotspot::VMStructEntry* entry{ jni::hotspot::get_vm_structs() }; entry->type_name; ++entry)
            {
                if (!std::strcmp(entry->type_name, type_name) && !std::strcmp(entry->field_name, field_name))
                {
                    return entry;
                }
            }

            return nullptr;
        }

        /*
            @brief Represents a HotSpot internal Symbol object.
            @details
            Symbols are interned strings used throughout the JVM to represent class names,
            method names, field names, and type signatures. They are stored in a shared
            symbol table and reused across the JVM. The layout is resolved at runtime
            via gHotSpotVMStructs using the offsets of Symbol._length and Symbol._body.
        */
        struct symbol
        {
            /*
                @brief Returns the length of this symbol in characters.
                @return The number of characters in the symbol's body,
                        or 0 if the entry could not be found or the pointer is invalid.
                @details
                Reads the _length field using its offset retrieved from gHotSpotVMStructs.
                The length is stored as an unsigned 16-bit integer and represents the
                number of bytes in the symbol body, not including any null terminator.
            */
            auto get_length() const noexcept
                -> std::uint16_t
            {
                static const jni::hotspot::VMStructEntry* const entry{ jni::hotspot::iterate_struct_entries("Symbol", "_length") };

                if (!entry || !jni::util::is_valid_ptr(this))
                {
                    return 0;
                }

                return *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);
            }

            /*
                @brief Returns a pointer to the raw character data of this symbol.
                @return Pointer to the first character of the symbol body,
                        or nullptr if the entry could not be found or the pointer is invalid.
                @details
                Reads the _body field using its offset retrieved from gHotSpotVMStructs.
                The body is a sequence of bytes of length get_length(), not null terminated.
                The returned pointer points directly into JVM metaspace and remains valid
                as long as the owning symbol is alive in the JVM symbol table.
            */
            auto get_body() const noexcept
                -> const char*
            {
                static const jni::hotspot::VMStructEntry* const entry{ jni::hotspot::iterate_struct_entries("Symbol", "_body") };

                if (!entry || !jni::util::is_valid_ptr(this))
                {
                    return nullptr;
                }

                return reinterpret_cast<const char*>(reinterpret_cast<const std::uint8_t*>(this) + entry->offset);
            }

            /*
                @brief Converts this HotSpot Symbol to a std::string.
                @return A std::string containing a copy of the symbol's character data,
                        or an empty string if the symbol pointer is invalid, the length
                        is zero, or the length exceeds the sanity threshold.
                @details
                Validates the symbol pointer via safe_read_ptr before touching any fields
                to handle partially initialized or GC-tagged symbol pointers gracefully.
                Reads the length via get_length() and the body via get_body(), both of
                which resolve their offsets from gHotSpotVMStructs on first call.
                The length is checked against a sanity threshold of 0x1000 (4096) characters.
                Legitimate Java class, method, and field names never approach this limit
                in practice — exceeding it indicates that this pointer does not refer to
                a valid Symbol and the memory contents are garbage, likely due to GC
                relocation or memory reuse. Reading past this threshold risks accessing
                gigabytes of arbitrary memory and is rejected immediately.
                @note The returned std::string owns its data and is safe to use
                      independently of JVM memory after construction.
            */
            auto to_string() const noexcept
                -> std::string
            {
                if (!jni::util::safe_read_ptr(this))
                {
                    return std::string{};
                }

                const std::uint16_t length{ this->get_length() };

                if (!length || length > 0x1000)
                {
                    return std::string{};
                }

                const char* const body{ this->get_body() };

                if (!jni::util::is_valid_ptr(body))
                {
                    return std::string{};
                }

                return std::string{ body, length };
            }
        };

        /*
            @brief Represents a HotSpot internal Klass object.
            @details
            Klass is the JVM's internal representation of a Java class or interface.
            It holds all metadata needed to describe a type at runtime including its
            name and superclass. The layout is resolved at runtime via gHotSpotVMStructs
            using the offset of Klass._name.
            @note A pointer to this struct is obtained by walking the ClassLoaderDataGraph
                  via class_loader_data_graph::get_all_classes().
            @see symbol, class_loader_data_graph
        */
        struct klass
        {
            /*
                @brief Returns the symbol representing the internal name of this class.
                @return Pointer to the symbol containing the class name using '/' separators
                        (e.g. "java/lang/String"), or nullptr if the name cannot be read.
                @details
                Reads the _name field using its offset retrieved from gHotSpotVMStructs.
                The raw pointer is untagged via untag_ptr to strip any GC tag bits before
                casting to symbol*, and validated via is_valid_ptr before returning.
                @note The returned pointer points directly into JVM metaspace and remains
                      valid as long as the owning class is loaded in the JVM.
            */
            auto get_name() const noexcept
                -> symbol*
            {
                static const jni::hotspot::VMStructEntry* const entry{ jni::hotspot::iterate_struct_entries("Klass", "_name") };

                if (!entry || !jni::util::is_valid_ptr(this))
                {
                    return nullptr;
                }

                const void* const raw{ jni::util::safe_read_ptr(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };

                jni::hotspot::symbol* const symbol{ reinterpret_cast<jni::hotspot::symbol*>(const_cast<void*>(jni::util::untag_ptr(raw))) };

                return jni::util::is_valid_ptr(symbol) ? symbol : nullptr;
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
            - offset 16: _literal (Klass*) — the actual class
            @see class_loader_data, klass
        */
        struct dictionary
        {
            /*
                @brief Returns the number of buckets in this dictionary.
                @return The bucket count as a signed 32-bit integer.
                @details
                Reads _table_size at offset 0 per BasicHashtable VMStruct layout.
            */
            inline auto get_table_size() const noexcept
                -> std::int32_t
            {
                return *reinterpret_cast<const std::int32_t*>(this);
            }

            /*
                @brief Returns a pointer to the bucket array.
                @return Pointer to the first bucket slot, or nullptr if invalid.
                @details
                Reads _buckets at offset 8 per BasicHashtable VMStruct layout.
                Each bucket slot is a pointer to the head DictionaryEntry of that
                bucket's chain.
            */
            inline auto get_buckets() const noexcept
                -> const std::uint8_t*
            {
                return *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(this) + 8);
            }

            /*
                @brief Collects all klass pointers stored in this dictionary.
                @param result The vector to append found klass pointers into.
                @details
                Iterates over every bucket and follows each entry chain via the
                _next pointer at offset 0, reading the Klass* from _literal at
                offset 16 of each DictionaryEntry. Both entry and klass pointers
                are validated via is_valid_ptr and untag_ptr before use to handle
                GC-tagged values and partially initialized entries safely.
            */
            auto collect_classes(std::vector<jni::hotspot::klass*>& result) const noexcept
                -> void
            {
                const std::int32_t table_size{ this->get_table_size() };
                const std::uint8_t* const buckets{ this->get_buckets() };

                if (!jni::util::is_valid_ptr(buckets) || table_size <= 0 || table_size > 0x186A0)
                {
                    return;
                }

                for (std::int32_t i{ 0 }; i < table_size; ++i)
                {
                    const std::uint8_t* entry{ reinterpret_cast<const std::uint8_t*>(jni::util::untag_ptr(jni::util::safe_read_ptr(buckets + i * 8))) };

                    while (jni::util::is_valid_ptr(entry))
                    {
                        const void* const raw_klass{ jni::util::safe_read_ptr(entry + 16) };

                        jni::hotspot::klass* const klass{ reinterpret_cast<jni::hotspot::klass*>(const_cast<void*>(jni::util::untag_ptr(raw_klass))) };

                        if (jni::util::is_valid_ptr(klass))
                        {
                            result.push_back(klass);
                        }

                        entry = reinterpret_cast<const std::uint8_t*>(jni::util::untag_ptr(jni::util::safe_read_ptr(entry)));
                    }
                }
            }
        };

        /*
            @brief Represents a HotSpot ClassLoaderData node.
            @details
            Each ClassLoader in the JVM has a corresponding ClassLoaderData that
            tracks every Klass it has loaded. The ClassLoaderData nodes are chained
            together via _next into a global linked list whose head is held by
            ClassLoaderDataGraph::_head.
            @see dictionary, class_loader_data_graph, klass
        */
        struct class_loader_data
        {
            /*
                @brief Returns the next ClassLoaderData node in the global linked list.
                @return Pointer to the next class_loader_data, or nullptr if this is the last.
                @details
                Reads ClassLoaderData::_next using its offset from gHotSpotVMStructs.
                Uses safe_read_ptr to handle partially initialized or GC-tagged pointers.
            */
            auto get_next() const noexcept
                -> jni::hotspot::class_loader_data*
            {
                static const jni::hotspot::VMStructEntry* const entry{ jni::hotspot::iterate_struct_entries("ClassLoaderData", "_next") };

                if (!entry || !jni::util::is_valid_ptr(this))
                {
                    return nullptr;
                }

                const void* const raw{ jni::util::safe_read_ptr(reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };

                return 
                    jni::util::is_valid_ptr(raw)
                    ? const_cast<jni::hotspot::class_loader_data*>(reinterpret_cast<const jni::hotspot::class_loader_data*>(raw))
                    : nullptr;
            }

            /*
                @brief Returns the Dictionary associated with this classloader.
                @return Pointer to the dictionary for this classloader, or nullptr if this
                        classloader has no dictionary or on failure.
                @details
                Reads ClassLoaderData::_dictionary using its offset from gHotSpotVMStructs.
                The bootstrap classloader typically has no dictionary and returns nullptr.
                Uses safe_read_ptr to handle partially initialized or GC-tagged pointers.
            */
            auto get_dictionary() const noexcept
                -> jni::hotspot::dictionary*
            {
                static const jni::hotspot::VMStructEntry* const entry{ jni::hotspot::iterate_struct_entries("ClassLoaderData", "_dictionary") };

                if (!entry || !jni::util::is_valid_ptr(this))
                {
                    return nullptr;
                }

                const void* const raw{ jni::util::safe_read_ptr( reinterpret_cast<const std::uint8_t*>(this) + entry->offset) };

                return 
                    jni::util::is_valid_ptr(raw)
                    ? const_cast<jni::hotspot::dictionary*>(reinterpret_cast<const jni::hotspot::dictionary*>(raw))
                    : nullptr;
            }
        };

        /*
            @brief Represents the HotSpot ClassLoaderDataGraph — the global registry of all classloaders.
            @details
            ClassLoaderDataGraph::_head is a static field holding the head of a global
            linked list of ClassLoaderData nodes, one per classloader registered in the JVM.
            Walking this list gives access to every class loaded by every classloader,
            including the bootstrap loader, the platform loader, the app loader, and all
            custom classloaders such as Minecraft's own classloader.
            @see class_loader_data, dictionary, klass
        */
        struct class_loader_data_graph
        {
            /*
                @brief Returns the head of the global ClassLoaderData linked list.
                @return Pointer to the first class_loader_data node, or nullptr on failure.
                @details
                Reads ClassLoaderDataGraph::_head via its absolute VMStruct address since
                _head is a static field. The returned node is the entry point for walking
                every classloader registered in the JVM. Subsequent nodes are accessed via
                class_loader_data::get_next().
            */
            auto get_head() const noexcept
                -> jni::hotspot::class_loader_data*
            {
                static const jni::hotspot::VMStructEntry* const entry{ jni::hotspot::iterate_struct_entries("ClassLoaderDataGraph", "_head") };

                if (!entry)
                {
                    return nullptr;
                }

                jni::hotspot::class_loader_data* const head{ *reinterpret_cast<jni::hotspot::class_loader_data* const*>(entry->address) };

                return jni::util::is_valid_ptr(head) ? head : nullptr;
            }

            /*
                @brief Returns a vector of pointers to all klass objects loaded in the JVM.
                @return A std::vector containing a raw pointer to every klass found across
                        all classloader dictionaries. May be empty if no classes are loaded yet.
                @details
                Walks the full ClassLoaderDataGraph by starting at get_head() and following
                each ClassLoaderData::_next pointer until the end of the list. For each
                ClassLoaderData node, retrieves its Dictionary and calls collect_classes()
                to gather every klass pointer stored in that dictionary's hashtable.
                The returned pointers point directly into JVM metaspace and remain valid
                as long as the corresponding classes remain loaded. Classes loaded by
                short-lived classloaders may be unloaded, invalidating their klass pointers.
                @note The returned vector may contain duplicate pointers if the same class
                      is somehow registered in multiple dictionaries, though this is rare.
                @note Call this after the JVM has finished loading the classes you need.
                      Classes not yet loaded will not appear in the result.
            */
            auto get_all_classes() const noexcept
                -> std::vector<jni::hotspot::klass*>
            {
                std::vector<jni::hotspot::klass*> result{};

                jni::hotspot::class_loader_data* class_loader_data{ this->get_head() };

                while (jni::util::is_valid_ptr(class_loader_data))
                {
                    jni::hotspot::dictionary* const dict{ class_loader_data->get_dictionary() };

                    if (jni::util::is_valid_ptr(dict))
                    {
                        dict->collect_classes(result);
                    }

                    class_loader_data = class_loader_data->get_next();
                }

                return result;
            }
        };

        /*
            @brief Finds a loaded Java class by its internal name using HotSpot internals.
            @param class_name The internal JVM class name using '/' separators
                              (e.g. "java/lang/String", "net/minecraft/client/Minecraft").
            @return Pointer to the matching klass if found, nullptr otherwise.
            @details
            Checks the loaded_classes cache first to avoid repeating the full
            ClassLoaderDataGraph walk on subsequent calls for the same class name.
            On cache miss, walks the full ClassLoaderDataGraph by calling
            get_all_classes() and comparing each klass name symbol against the
            requested class_name. On a match the result is inserted into the cache
            before returning.
            This function is the HotSpot-native replacement for JNI's FindClass and
            JVMTI's GetLoadedClasses. It covers all loaded classes across all
            classloaders including the bootstrap loader, JDK classes, and any
            application classloader such as Minecraft's own classloader.
            @note The returned klass* points directly into JVM metaspace and remains
                  valid as long as the class remains loaded in the JVM.
            @note The class name must use '/' as the package separator, not '.'.
            @see loaded_classes, class_loader_data_graph::get_all_classes
        */
        static auto find_class(const std::string_view class_name) noexcept
            -> jni::hotspot::klass*
        {
            if (const auto it{ jni::hotspot::loaded_classes.find(std::string{ class_name }) }; it != jni::hotspot::loaded_classes.end())
            {
                return it->second;
            }

            const jni::hotspot::class_loader_data_graph graph{};
            const std::vector<jni::hotspot::klass*> classes{ graph.get_all_classes() };

            for (jni::hotspot::klass* const klass : classes)
            {
                const jni::hotspot::symbol* const symbol{ klass->get_name() };
                if (!symbol) continue;

                const std::string name{ symbol->to_string() };
                if (name.empty()) continue;

                jni::hotspot::loaded_classes.emplace(name, klass);

                if (name == class_name)
                {
                    return klass;
                }
            }

            return nullptr;
        }
    }

    /*
        @brief Associates a C++ wrapper type with its corresponding Java class name.
        @tparam wrapped_class The C++ wrapper type to register. Must derive from jni::object.
        @param class_name The internal JVM class name using '/' separators
                          (e.g. "java/lang/String").
        @return true if the class was found in the JVM and successfully registered,
                false if the class could not be found.
        @details
        Inserts or updates the mapping from the C++ type_index of wrapped_class to
        class_name in jni::class_map, then verifies that the class actually exists
        in the JVM by calling jni::hotspot::find_class(). If the class is not found
        the mapping is removed and false is returned.
        This registration is required before calling install_hook<wrapped_class>()
        or any other API that dispatches through the C++ type system to locate the
        corresponding Java class at runtime.
        @note The class name must use '/' as the package separator, not '.'.
        @note If the class is not yet loaded at the time of registration, the call
              will fail. Call register_class() after the class has been loaded by
              the JVM.
        @see class_map, jni::hotspot::find_class
    */
    template<class wrapped_class>
    static auto register_class(const std::string_view class_name) noexcept
        -> bool
    {
        jni::hotspot::klass* const klass{ jni::hotspot::find_class(class_name) };

        if (!klass)
        {
            return false;
        }

        jni::class_map.insert_or_assign(std::type_index{ typeid(wrapped_class) }, std::string{ class_name });

        return true;
    }
}