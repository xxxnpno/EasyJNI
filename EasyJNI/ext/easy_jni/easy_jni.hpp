#pragma once

#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <type_traits>
#include <typeinfo>
#include <typeindex>
#include <functional>
#include <any>
#include <utility>
#include <memory>
#include <thread>

/*
	Java Native Interface and Java Virtual Machine Tool Interface includes.
*/
#include <jni/jni.h>
#include <jni/jvmti.h>

/*
	Windows API includes and definitions.
*/		
extern "C"
{
	using DWORD = unsigned long;
	using LPVOID = void*;
	using BOOL = int;
	using HANDLE = void*;

	/*
		@brief Enables execute and read access to a memory region.
	*/
	static constexpr DWORD PAGE_EXECUTE_READ{ 0x20 };

	/*
		@brief Enables execute, read, and write access to a memory region.
	*/
	static constexpr DWORD PAGE_EXECUTE_READWRITE{ 0x40 };

	/*
		@brief Commits physical memory to a reserved region.
	*/
	static constexpr DWORD MEM_COMMIT{ 0x1000 };

	/*
		@brief Reserves a range of the process's virtual address space.
	*/
	static constexpr DWORD MEM_RESERVE{ 0x2000 };

	/*
		@brief Releases a region of memory back to the system.
	*/
	static constexpr DWORD MEM_RELEASE{ 0x8000 };

	/*
		@brief Allocates a region of memory in the virtual address space of the calling process.

		@param lpAddress Desired starting address (nullptr = system chooses)
		@param dwSize Size of the allocation in bytes
		@param flAllocationType Type of allocation (MEM_COMMIT, MEM_RESERVE, etc.)
		@param flProtect Memory protection flags (PAGE_READWRITE, PAGE_EXECUTE_READ, etc.)

		@return Pointer to allocated memory, or nullptr on failure
	*/
	__declspec(dllimport) LPVOID __stdcall VirtualAlloc(LPVOID lpAddress, std::size_t dwSize, DWORD flAllocationType, DWORD flProtect);

	/*
		@brief Releases or decommits a region of memory.

		@param lpAddress Address of the memory to free
		@param dwSize Size of the region (0 if MEM_RELEASE)
		@param dwFreeType Type of free (MEM_RELEASE, MEM_DECOMMIT)

		@return TRUE on success, FALSE on failure
	*/
	__declspec(dllimport) BOOL __stdcall VirtualFree(LPVOID lpAddress, std::size_t dwSize, DWORD dwFreeType);

	/*
		@brief Changes the protection of a region of memory.

		@param lpAddress Address of the memory region
		@param dwSize Size of the region
		@param flNewProtect New protection flags
		@param lpflOldProtect Receives the old protection value

		@return TRUE on success, FALSE on failure
	*/
	__declspec(dllimport) BOOL __stdcall VirtualProtect(LPVOID lpAddress, std::size_t dwSize, DWORD flNewProtect, DWORD* lpflOldProtect);
}

// Everything about EasyJNI is in this namespace.
namespace jni
{
	/*
		Forward Declarations.
	*/

	class string;

	class jni_exception;

	class object;

	auto get_env()
		-> JNIEnv*;

	auto shutdown_hooks()
		-> void;

	namespace hotspot
	{
		struct method;
	}

	/*
		@brief Helpers and concepts for compile-time type checking in EasyJNI.
		@details
		These templates and concepts are used to constrain template parameters
		for methods, fields, and arguments, ensuring only supported JNI/C++ types
		are allowed.
	*/

	/*
		@brief Helper to unwrap unique_ptr to underlying JNI object type.
		@tparam T The type to unwrap
		@note If T is a std::unique_ptr to a class derived from object, extracts the underlying type.
	*/
	template<typename T>
	struct unwrap_object_ptr { using type = T; };

	template<typename T>
		requires (std::is_base_of_v<object, T>)
	struct unwrap_object_ptr<std::unique_ptr<T>> { using type = T; };

	/*
		@brief Concept for standard C++ types supported by JNI.
		@tparam T The type to check
	*/
	template<typename T>
	concept std_type =
		std::is_same_v<T, short> ||
		std::is_same_v<T, int> ||
		std::is_same_v<T, long long> ||
		std::is_same_v<T, float> ||
		std::is_same_v<T, double> ||
		std::is_same_v<T, bool> ||
		std::is_same_v<T, char> ||
		std::is_same_v<T, std::byte> ||
		std::is_same_v<T, std::string> ||
		std::is_same_v<T, void>;

	/*
		@brief Concept for JNI objects.
		@tparam T The type to check
		@note T must derive from object
	*/
	template<typename T>
	concept jni_object = std::is_base_of_v<object, T>;

	/*
		@brief Concept for pointers to JNI objects.
		@tparam T The type to check
		@note Unwraps unique_ptr and checks if underlying type is a JNI object
	*/
	template<typename T>
	concept jni_object_ptr = jni_object<typename unwrap_object_ptr<T>::type>
		&& !jni_object<T>;

	/*
		@brief Concept for valid method return types.
		@tparam T The type to check
		@note Must be a standard type, JNI object, or jobjectArray
	*/
	template<typename T>
	concept method_return = std_type<T> || jni_object<T> || std::is_same_v<T, jobjectArray>;

	/*
		@brief Concept for valid field types.
		@tparam T The type to check
		@note Must be a standard type or JNI object
	*/
	template<typename T>
	concept field_type_c = std_type<T> || jni_object<T>;

	/*
		@brief Concept for valid callable argument types.
		@tparam T The type to check
		@note Can be std_type, JNI object, pointer to JNI object, or convertible to std::string
	*/
	template<typename T>
	concept callable_arg = std_type<std::remove_cvref_t<T>>
		|| jni_object<std::remove_cvref_t<T>>
		|| jni_object_ptr<std::remove_cvref_t<T>>
		|| std::is_convertible_v<std::remove_cvref_t<T>, std::string>;

	/*
		Forward Declarations.
	*/

	template <field_type_c type>
	class field;

	template <method_return return_type>
	class method;

	/*
		@brief Custom exception for JNI errors.
	*/
	class jni_exception final : public std::exception
	{
	public:
		/*
			@brief Constructor with a message describing the error
			@param msg Error message
		*/
		explicit jni_exception(const std::string_view msg)
			: message{ msg }
		{

		}

		/*
			@brief Returns the error message
			@return C-style string containing the error description
		*/
		auto what() const noexcept 
			-> const char* override
		{
			return this->message.c_str();
		}

	private:
		std::string message;
	};

	/*
		@brief Helper class for managing JNI strings safely.

		This class wraps both a std::string and a jstring global reference.
		It handles conversion between the two and ensures proper allocation
		and deallocation of JNI global references.

		@note This class is intended for internal use within this header only.
	*/
	class string final
	{
	public:
		/*
			@brief Constructs from a std::string.

			@param std_string The standard string to wrap
			@note Creates a global jstring reference for JNI use
		*/
		explicit string(const std::string& std_string = "")
			: jni_string{ nullptr }
			, std_string{ std_string }
		{
			const jstring local{ jni::get_env()->NewStringUTF(std_string.c_str()) };

			if (local)
			{
				this->jni_string = static_cast<jstring>(jni::get_env()->NewGlobalRef(local));
				jni::get_env()->DeleteLocalRef(local);
			}
		}

		/*
			@brief Constructs from a const char*.

			@param cstr The C-style string to wrap
			@note Converts to std::string and creates a global jstring reference
		*/
		explicit string(const char* cstr)
			: string{ std::string{cstr} } // delegate to std::string constructor
		{

		}

		/*
			@brief Constructs from a std::string_view.

			@param sv The string_view to wrap
			@note Converts to std::string and creates a global jstring reference
		*/
		explicit string(std::string_view sv)
			: string{ std::string{sv} } // delegate to std::string constructor
		{

		}

		/*
			@brief Constructs from a jstring.

			@param jni_string The JNI string to wrap
			@note Creates a global reference and caches its std::string equivalent
		*/
		explicit string(const jstring jni_string = nullptr)
			: jni_string{ nullptr }
		{
			if (jni_string)
			{
				this->jni_string = static_cast<jstring>(jni::get_env()->NewGlobalRef(jni_string));

				const char* chars{ jni::get_env()->GetStringUTFChars(this->jni_string, nullptr) };
				this->std_string = std::string{ chars };
				jni::get_env()->ReleaseStringUTFChars(this->jni_string, chars);
			}
		}

		/*
			@brief Constructs from a generic jobject.

			@param jni_string The jobject representing a Java string
			@note Converts and stores both jstring global ref and std::string
		*/
		explicit string(const jobject jni_string = nullptr)
			: string{ static_cast<jstring>(jni_string) }
		{

		}

		/*
			@brief Copy constructor.

			@param other Another string instance
			@note Creates a new global reference for the copy
		*/
		string(const string& other)
			: jni_string{ nullptr }
			, std_string{ other.std_string }
		{
			if (other.jni_string)
			{
				this->jni_string = static_cast<jstring>(jni::get_env()->NewGlobalRef(other.jni_string));
			}
		}

		/*
			@brief Copy assignment operator.

			@param other Another string instance
			@return Reference to *this
			@note Properly deletes existing global ref and creates a new one
		*/
		auto operator=(const string& other) -> string&
		{
			if (this == &other)
			{
				return *this;
			}

			if (this->jni_string)
			{
				jni::get_env()->DeleteGlobalRef(this->jni_string);
			}

			this->std_string = other.std_string;
			this->jni_string = other.jni_string
				? static_cast<jstring>(jni::get_env()->NewGlobalRef(other.jni_string))
				: nullptr;

			return *this;
		}

		/*
			@brief Move constructor.

			@param other Another string instance
			@note Transfers ownership of the global reference
		*/
		string(string&& other) noexcept
			: jni_string{ other.jni_string }
			, std_string{ std::move(other.std_string) }
		{
			other.jni_string = nullptr;
		}

		/*
			@brief Move assignment operator.

			@param other Another string instance
			@return Reference to *this
			@note Transfers ownership and cleans up existing global ref
		*/
		auto operator=(string&& other) noexcept -> string&
		{
			if (this == &other)
			{
				return *this;
			}

			if (this->jni_string)
			{
				jni::get_env()->DeleteGlobalRef(this->jni_string);
			}

			this->jni_string = other.jni_string;
			this->std_string = std::move(other.std_string);
			other.jni_string = nullptr;

			return *this;
		}

		/*
			@brief Destructor.

			@note Deletes the JNI global reference if it exists
		*/
		~string() noexcept
		{
			if (this->jni_string)
			{
				jni::get_env()->DeleteGlobalRef(this->jni_string);
			}
		}

		/*
			@brief Returns the JNI global reference.

			@return jstring reference
		*/
		inline auto get_jni_string() const noexcept
			-> jstring
		{
			return this->jni_string;
		}

		/*
			@brief Returns the std::string representation.

			@return std::string copy
		*/
		inline auto get_std_string() const noexcept
			-> std::string
		{
			return this->std_string;
		}

	private:
		jstring jni_string;

		std::string std_string;
	};

	inline const constexpr std::string_view easy_jni_error{ "[EasyJNI ERROR]" };
	inline const constexpr std::string_view easy_jni_warning{ "[EasyJNI WARNING]" };
	inline const constexpr std::string_view easy_jni_info{ "[EasyJNI INFO]" };

	// Global vm and jvmti pointers.
	inline constinit JavaVM* vm{ nullptr };
	inline constinit jvmtiEnv* jvmti{ nullptr };

	/*
		@brief Retrieves the JNIEnv for the current thread.

		If the current thread is not already attached to the JVM, this function
		will automatically attach it and cache the JNIEnv pointer using thread-local storage.

		@return Pointer to JNIEnv on success, nullptr on failure

		@note Requires jni::vm to be initialized before calling this function
		@warning Attaching threads has a cost; avoid calling excessively in performance-critical paths
	*/
	static auto get_env()
		-> JNIEnv*
	{
		thread_local JNIEnv* scoped_env{ nullptr };

		try
		{
			if (!jni::vm)
			{
				throw jni::jni_exception{ "JavaVM* is not initialized." };
				return nullptr;
			}

			if (!scoped_env)
			{
				if (const jint get_env_result{ jni::vm->GetEnv(reinterpret_cast<void**>(&scoped_env), JNI_VERSION_1_8) };
					get_env_result == JNI_EDETACHED)
				{
					if (const jint attach_result{ jni::vm->AttachCurrentThread(reinterpret_cast<void**>(&scoped_env), nullptr) };
						attach_result != JNI_OK)
					{
						throw jni::jni_exception{ 
							std::format("AttachCurrentThread failed with result {}.", static_cast<std::int32_t>(attach_result))
						};
					}
				}
				else if (get_env_result != JNI_OK)
				{
					throw jni::jni_exception{ 
						std::format("GetEnv failed with result {}.", static_cast<std::int32_t>(get_env_result)) 
					};
				}
			}

			return scoped_env;
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::get_env() {}", jni::easy_jni_error, e.what());
			return nullptr;
		}
	}

	/*
		@brief Detaches the current thread from the JVM.

		This function should be called before a thread exits to ensure that
		the JVM cleans up its internal thread resources.

		@note Uses thread-local storage to track the JNIEnv for the current thread.
		@warning If detachment fails, a warning message is printed but no exception is thrown.

		@return void
	*/
	static auto exit_thread()
		-> void
	{
		thread_local JNIEnv* scoped_env{ nullptr };

		try
		{
			if (scoped_env && jni::vm)
			{
				if (const jint detach_result{ jni::vm->DetachCurrentThread() }; detach_result != JNI_OK)
				{
					throw jni::jni_exception{
						std::format("DetachCurrentThread failed with result {}.", static_cast<std::int32_t>(detach_result))
					};
				}

				scoped_env = nullptr;
			}
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::exit_thread() {}", jni::easy_jni_error, e.what());
			return;
		}
	}

	/*
		@brief Maps Java class names to their corresponding global references.

		This unordered_map stores a global reference (`jclass`) for each
		Java class name to allow fast lookup and prevent repeated class
		lookups in the JVM.

		@note The global references must be properly managed to avoid
		memory leaks (deleted when no longer needed).

		@type std::unordered_map<std::string, jclass>
	*/
	inline std::unordered_map<std::string, jclass> classes{};

	/*
		@brief Loads a Java class by name and creates a global reference.

		This function searches all currently loaded Java classes using JVMTI,
		compares their names with the provided `name`, and creates a global
		reference (`jclass`) for the matching class.

		@param name The fully qualified Java class name (e.g., "java/lang/String")
		@return Global jclass reference if found, nullptr on failure

		@note Only creates global references for classes actually used, reducing
			  overhead compared to creating global refs for all loaded classes.
		@warning The first call may have slight performance cost due to the search.
	*/
	static auto load_class(const std::string_view class_name)
		-> jclass
	{
		// Get java/lang/Class and its getName() method.
		static constinit jclass permanent_clazz{ nullptr };
		static constinit jmethodID permanent_get_name_method_id{ nullptr };

		if (!permanent_clazz)
		{
			permanent_clazz = jni::get_env()->FindClass("java/lang/Class");
		}

		if (permanent_clazz && !permanent_get_name_method_id)
		{
			permanent_get_name_method_id = jni::get_env()->GetMethodID(permanent_clazz, "getName", "()Ljava/lang/String;");
		}

		try
		{
			if (!permanent_clazz)
			{
				throw jni::jni_exception{ std::format("Class is nullptr.") };
			}

			if (!permanent_get_name_method_id)
			{
				throw jni::jni_exception{ std::format("getName jmethodID is nullptr.") };
			}

			jni::get_env()->DeleteLocalRef(permanent_clazz);

			jint amount{ 0 };
			jclass* classes_ptr{ nullptr };

			if (!jni::jvmti)
			{
				throw jni::jni_exception{ "JVMTI environment is not initialized." };
			}

			// Retrieve all loaded classes via JVMTI.
			if (const jint loaded_classes_result{ jni::jvmti->GetLoadedClasses(&amount, &classes_ptr) }; 
				loaded_classes_result != JVMTI_ERROR_NONE)
			{
				throw std::runtime_error{ 
					std::format("GetLoadedClasses failed with result {}.", static_cast<std::int32_t>(loaded_classes_result)) 
				};
			}

			// Search for the class and create a global reference for this class.
			jclass found{ nullptr };
			for (jint i{ 0 }; i < amount; ++i)
			{
				const jclass current_class{ classes_ptr[i] };

				if (!current_class)
				{
					throw std::runtime_error{ std::format("Found a nullptr class.") };
				}

				const jni::string _string{ jni::get_env()->CallObjectMethod(current_class, permanent_get_name_method_id) };

				const bool match{ !std::strcmp(_string.get_std_string().c_str(), class_name.data())};

				if (match)
				{
					found = static_cast<jclass>(jni::get_env()->NewGlobalRef(current_class));
					jni::get_env()->DeleteLocalRef(current_class);

					// Store the global reference in a global map for future lookups without needing to search again.
					jni::classes.insert({ _string.get_std_string(), found });
					break;
				}

				jni::get_env()->DeleteLocalRef(current_class);
			}

			jni::jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes_ptr));

			return found;
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::load_class() for {}: {}", jni::easy_jni_error, class_name, e.what());
			return nullptr;
		}
	}

	/*
		@brief Retrieves the global reference of a Java class by its name.

		This function normalizes the class name (replacing '/' with '.'),
		then checks if a global reference already exists in the cache (`jni::classes`).
		If not, it tries to load the class via JVMTI (`load_class`) and,
		as a fallback, via JNI's `FindClass`.

		@param name Fully qualified Java class name (e.g., "java/lang/String")
		@return Global jclass reference if found, nullptr otherwise

		@note The first lookup may be slower due to JVMTI scanning all loaded classes.
		@note Global references are cached to avoid repeated lookups.
	*/
	static auto get_class(const std::string_view qualified_name)
		-> jclass
	{
		// Copy to modify for normalization.
		std::string class_name{ qualified_name };
		std::replace(class_name.begin(), class_name.end(), '/', '.');

		if (const auto it{ jni::classes.find(class_name) }; it != jni::classes.end())
		{
			return it->second;
		}

		// Try to load class via JVMTI
		jclass found{ jni::load_class(class_name) };

		try
		{
			// Fallback to JNI FindClass if JVMTI failed
			if (!found)
			{
				const jclass local = jni::get_env()->FindClass(qualified_name.data());
				if (local)
				{
					found = static_cast<jclass>(jni::get_env()->NewGlobalRef(local));
					jni::get_env()->DeleteLocalRef(local);

					// Store in cache
					jni::classes.insert({ class_name, found });
				}
				else
				{
					throw std::runtime_error{ std::format("Couldn't find the class.") };
				}
			}

			return found;
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::load_class() for {}: {}", jni::easy_jni_error, qualified_name, e.what());
			return nullptr;
		}
	}

	/*
		@brief Type cast helpers for JNI <-> C++ conversions.
		@details
		These maps and functions allow converting between JNI types (jshort, jint, jlong, jfloat, jdouble, jboolean, jchar, jbyte, jstring)
		and their corresponding C++ types (short, int, long long, float, double, bool, char, std::byte, std::string).
		They also provide helpers to convert to jvalue structs when calling JNI methods or setting fields.
	*/

	using convert_function = std::function<std::any(const std::any&)>;

	/*
		@brief Maps JNI types to corresponding C++ types.
		@note Returns a std::any containing the converted value.
	*/
	inline const std::unordered_map<std::type_index, convert_function> jni_to_cpp
	{
		{ typeid(jshort),   [](const std::any& v) -> std::any { return static_cast<short>(std::any_cast<jshort>(v)); } },
		{ typeid(jint),     [](const std::any& v) -> std::any { return static_cast<int>(std::any_cast<jint>(v)); } },
		{ typeid(jlong),    [](const std::any& v) -> std::any { return static_cast<long long>(std::any_cast<jlong>(v)); } },
		{ typeid(jfloat),   [](const std::any& v) -> std::any { return static_cast<float>(std::any_cast<jfloat>(v)); } },
		{ typeid(jdouble),  [](const std::any& v) -> std::any { return static_cast<double>(std::any_cast<jdouble>(v)); } },
		{ typeid(jboolean), [](const std::any& v) -> std::any { return static_cast<bool>(std::any_cast<jboolean>(v)); } },
		{ typeid(jchar),    [](const std::any& v) -> std::any { return static_cast<char>(std::any_cast<jchar>(v)); } },
		{ typeid(jbyte),    [](const std::any& v) -> std::any { return static_cast<std::byte>(std::any_cast<jbyte>(v)); } },
		{ typeid(jstring),  [](const std::any& v) -> std::any { return jni::string(std::any_cast<jstring>(v)).get_std_string(); } }
	};

	/*
		@brief Maps C++ types to corresponding JNI types.
		@note Returns a std::any containing the converted JNI value.
	*/
	inline const std::unordered_map<std::type_index, convert_function> cpp_to_jni
	{
		{ typeid(short),       [](const std::any& v) -> std::any { return static_cast<jshort>(std::any_cast<short>(v)); } },
		{ typeid(int),         [](const std::any& v) -> std::any { return static_cast<jint>(std::any_cast<int>(v)); } },
		{ typeid(long long),   [](const std::any& v) -> std::any { return static_cast<jlong>(std::any_cast<long long>(v)); } },
		{ typeid(float),       [](const std::any& v) -> std::any { return static_cast<jfloat>(std::any_cast<float>(v)); } },
		{ typeid(double),      [](const std::any& v) -> std::any { return static_cast<jdouble>(std::any_cast<double>(v)); } },
		{ typeid(bool),        [](const std::any& v) -> std::any { return static_cast<jboolean>(std::any_cast<bool>(v)); } },
		{ typeid(char),        [](const std::any& v) -> std::any { return static_cast<jchar>(std::any_cast<char>(v)); } },
		{ typeid(std::byte),   [](const std::any& v) -> std::any { return static_cast<jbyte>(std::any_cast<std::byte>(v)); } },
		{ typeid(std::string), [](const std::any& v) -> std::any { return jni::string(std::any_cast<std::string>(v)).get_jni_string(); } }
	};

	using jvalue_setter = std::function<jvalue(const std::any&)>;

	/*
		@brief Maps JNI types to a jvalue setter.
		@details
		Each entry returns a jvalue struct with the corresponding field set.
		Useful for calling JNI methods with arguments or setting object fields.
	*/
	inline const std::unordered_map<std::type_index, jvalue_setter> jni_to_jvalue
	{
		{ typeid(jshort),   [](const std::any& v) -> jvalue { jvalue j{}; j.s = std::any_cast<jshort>(v);   return j; } },
		{ typeid(jint),     [](const std::any& v) -> jvalue { jvalue j{}; j.i = std::any_cast<jint>(v);     return j; } },
		{ typeid(jlong),    [](const std::any& v) -> jvalue { jvalue j{}; j.j = std::any_cast<jlong>(v);    return j; } },
		{ typeid(jfloat),   [](const std::any& v) -> jvalue { jvalue j{}; j.f = std::any_cast<jfloat>(v);   return j; } },
		{ typeid(jdouble),  [](const std::any& v) -> jvalue { jvalue j{}; j.d = std::any_cast<jdouble>(v);  return j; } },
		{ typeid(jboolean), [](const std::any& v) -> jvalue { jvalue j{}; j.z = std::any_cast<jboolean>(v); return j; } },
		{ typeid(jchar),    [](const std::any& v) -> jvalue { jvalue j{}; j.c = std::any_cast<jchar>(v);    return j; } },
		{ typeid(jbyte),    [](const std::any& v) -> jvalue { jvalue j{}; j.b = std::any_cast<jbyte>(v);    return j; } },
		{ typeid(jstring),  [](const std::any& v) -> jvalue { jvalue j{}; j.l = std::any_cast<jstring>(v);  return j; } },
		{ typeid(jobject),  [](const std::any& v) -> jvalue { jvalue j{}; j.l = std::any_cast<jobject>(v);  return j; } }
	};

	/*
		@brief Maps C++ types to their corresponding JNI type signatures.
		@details
		This map is used to generate method signatures for JNI calls.
		Each C++ type is associated with a string representing the JNI signature.
		For objects and arrays, the JNI signature follows the standard Java notation.
	*/
	inline const std::unordered_map<std::type_index, std::string> signature_map
	{
		{ std::type_index{ typeid(void) },          "V" },
		{ std::type_index{ typeid(short) },         "S" },
		{ std::type_index{ typeid(int) },           "I" },
		{ std::type_index{ typeid(long long) },     "J" },
		{ std::type_index{ typeid(float) },         "F" },
		{ std::type_index{ typeid(double) },        "D" },
		{ std::type_index{ typeid(bool) },          "Z" },
		{ std::type_index{ typeid(char) },          "C" },
		{ std::type_index{ typeid(std::byte) },     "B" },
		{ std::type_index{ typeid(std::string) },   "Ljava/lang/String;" },
		{ std::type_index{ typeid(jobjectArray) },  "[Ljava/lang/Object;" }
	};

	/*
		@brief Maps C++ type_index to the JNI class signature.
		@details
		Used to determine the Java type signature corresponding to a given C++ type.
		This is useful when generating method or field signatures dynamically.
		@note The map is initially empty and populated at runtime as needed.
	*/
	inline std::unordered_map<std::type_index, std::string> class_map{};

	/*
		@brief Registers a C++ class with a corresponding Java class.
		@tparam type The C++ class type (must satisfy jni_object)
		@param class_name The fully qualified Java class name (e.g., "com/example/MyClass")
		@return true if the class was successfully found in the JVM and registered; false otherwise
		@details
		Stores the mapping from C++ type_index to the Java class name in `class_map`.
		If the class cannot be found in the JVM, it removes the mapping and logs an error.
	*/
	template <jni_object type>
	static auto register_class(const std::string_view class_name)
		-> bool
	{
		// Insert or update the mapping from C++ type to Java class name
		jni::class_map.insert_or_assign(std::type_index{ typeid(type) }, class_name);

		try
		{
			// Try to obtain the global reference to the Java class
			if (!jni::get_class(class_name))
			{
				throw jni::jni_exception{ "Class not found in JVM." };
			}

			return true;
		}
		catch (const std::exception& e)
		{
			std::println("{} register_class() for {}: {}", jni::easy_jni_error, class_name, e.what());

			jni::class_map.erase(std::type_index{ typeid(type) });
			return false;
		}
	}

	/*
		@brief Returns the JNI signature for a given C++ type.
		@tparam type The C++ type (must satisfy method_return)
		@return The JNI type signature as a string. Returns empty string on error.
		@details
		Handles primitive types, JNI objects, and jobjectArray.
		- For objects derived from `object`, looks up `class_map` and returns "L<classname>;"
		- For jobjectArray, returns the array signature "[Ljava/lang/Object;"
		- For primitive types, looks up `signature_map`
		Logs an error if the type is unregistered or not found.
	*/
	template <method_return type>
	static auto get_signature()
		-> std::string
	{
		try
		{
			if constexpr (std::is_base_of_v<object, type>)
			{
				// Look for the registered C++ -> Java class mapping
				const auto it{ jni::class_map.find(std::type_index{ typeid(type) }) };
				if (it == jni::class_map.end())
				{
					throw std::runtime_error{ std::format("Class not registered for type {}.", typeid(type).name()) };
				}

				// Return JNI object signature format
				return std::format("L{};", it->second);
			}
			else if constexpr (std::is_same_v<type, jobjectArray>)
			{
				// Special case for object arrays
				return signature_map.at(std::type_index{ typeid(jobjectArray) });
			}
			else
			{
				// Primitive or std::string types
				return jni::signature_map.at(std::type_index{ typeid(type) });
			}
		}
		catch (const std::exception& e)
		{
			std::println("{} get_signature() {}", jni::easy_jni_error, e.what());
			return std::string{};
		}
	}

	/*
		@brief Converts a pack of C++ arguments into JNI jvalues for method calls.
		@tparam args_t Variadic template arguments constrained by `callable_arg`
		@param args The arguments to convert
		@return A pair containing:
			- std::vector<jvalue>: converted JNI arguments ready for method calls
			- std::vector<jni::string>: temporary string holders to keep JNI strings alive
		@details
		- Handles standard C++ types, JNI objects, pointers to JNI objects, and types convertible to std::string.
		- Keeps jni::string instances alive in `string_keeper` to prevent dangling jstring references.
		- Uses `cpp_to_jni` and `jni_to_jvalue` maps for primitive type conversions.
	*/
	template<callable_arg... args_t>
	static auto build_jargs(args_t&&... args)
		-> std::pair<std::vector<jvalue>, std::vector<jni::string>>
	{
		std::vector<jni::string> string_keeper{};
		string_keeper.reserve(sizeof...(args_t));

		auto convert_arg = [&string_keeper](auto&& arg) 
			-> jvalue
		{
			using raw = std::remove_cvref_t<decltype(arg)>;

			std::any jni_val{};
			if constexpr (std::is_base_of_v<object, raw>)
			{
				jni_val = arg.get_instance(); // JNI object instance
			}
			else if constexpr (jni_object_ptr<raw>)
			{
				jni_val = arg->get_instance(); // pointer to JNI object instance
			}
			else if constexpr (std::is_convertible_v<raw, std::string>)
			{
				string_keeper.emplace_back(std::string{ arg }); // store string to keep jstring alive
				jni_val = string_keeper.back().get_jni_string();
			}
			else
			{
				// convert C++ primitive type to JNI type
				jni_val = jni::cpp_to_jni.at(std::type_index{ typeid(raw) })(arg);
			}

			// convert std::any JNI value to jvalue struct
			return jni::jni_to_jvalue.at(std::type_index{ jni_val.type() })(jni_val);
		};

		std::vector<jvalue> jargs{};
		if constexpr (sizeof...(args_t) > 0)
		{
			jargs = { convert_arg(std::forward<args_t>(args))... };
		}

		return { std::move(jargs), std::move(string_keeper) };
	}

	/*
		@brief Maps a C++ class type to its Java field IDs.
		@details
		The outer map keys are C++ type_index values (unique per type),
		the inner map associates field names (std::string) to their corresponding JNI jfieldID.
		This allows quick lookup of field IDs for both instance and static fields.
	*/
	inline std::unordered_map<std::type_index, std::unordered_map<std::string, jfieldID>> field_ids{};

	/*
		@brief Enum to indicate whether a Java field is static or not.
	*/
	enum class field_type : std::int8_t
	{
		STATIC,
		NOT_STATIC
	};

	/*
		@brief Represents a Java field and provides get/set access from C++.
		@tparam type The C++ type corresponding to the Java field type (primitive, object, string, etc.)
		@details
		- Supports static and instance fields.
		- Supports primitive types (short, int, long long, float, double, bool, char, std::byte) and std::string.
		- Supports JNI objects derived from `jni::object` (wrapped in std::unique_ptr when returned).
		- Uses `jni::field_ids` to cache jfieldID for fast access.
		- Should not be used outside this header.
	*/
	template <field_type_c type>
	class field final
	{
	public:
		/*
			@brief Constructs a field object.
			@param class_or_instance Pointer to the Java class (for static) or instance (for instance fields)
			@param name Field name as a string
			@param field_type Indicates whether the field is static or instance
			@param index type_index of the C++ class owning the field (used for lookup in field_ids)
		*/
		field(void* class_or_instance, const std::string_view name, const jni::field_type field_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, index{ index }
			, field_type{ field_type }
		{

		}

		/*
			@brief Gets the value of the field.
			@return For primitive types, returns the value.
					For JNI objects, returns std::unique_ptr<type>.
			@throws std::runtime_error if the field ID cannot be found.
			@details Handles static and instance fields appropriately.
		*/
		auto get() const
			-> std::conditional_t<std::is_base_of_v<object, type>, std::unique_ptr<type>, type>
		{
			try
			{
				const auto& inner_map{ jni::field_ids.at(this->index) };
				const auto it{ inner_map.find(this->name) };

				if (it == inner_map.end())
				{
					throw jni::jni_exception{ "Field ID not found." };
				}

				const jfieldID field_id{ it->second };

				jobject local{ nullptr };
				if constexpr (std::is_base_of_v<object, type>)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						local = jni::get_env()->GetObjectField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						local = jni::get_env()->GetStaticObjectField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}

					std::unique_ptr<type> result{ std::make_unique<type>(local) };

					if (local)
					{
						jni::get_env()->DeleteLocalRef(local);
					}

					return result;
				}

				std::any result{};
				if constexpr (std::is_same<type, short>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetShortField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticShortField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, int>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetIntField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticIntField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, long long>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetLongField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticLongField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, float>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetFloatField(
							const_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticFloatField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, double>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetDoubleField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticDoubleField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, bool>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetBooleanField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticBooleanField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, char>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetCharField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticCharField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, std::byte>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::get_env()->GetByteField(
							reinterpret_cast<jbyte>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = jni::get_env()->GetStaticByteField(
							reinterpret_cast<jbyte>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, std::string>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						result = jni::string(
							static_cast<jstring>(jni::get_env()->GetObjectField(
								reinterpret_cast<jobject>(this->class_or_instance),
								field_id)
								)
						).get_std_string();
					}
					else
					{
						result = jni::string(
							static_cast<jstring>(jni::get_env()->GetStaticObjectField(
								reinterpret_cast<jclass>(this->class_or_instance),
								field_id)
								)
						).get_std_string();
					}
				}

				if constexpr (!std::is_base_of_v<object, type>)
				{
					auto it = jni::jni_to_cpp.find(result.type());
					if (it != jni::jni_to_cpp.end())
					{
						return std::any_cast<type>(it->second(result));
					}
					return type{};
				}
				else
				{
					return std::make_unique<type>(nullptr);
				}
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::field.get() for {}: {}", jni::easy_jni_error, this->name, e.what());

				if constexpr (std::is_base_of_v<object, type>)
				{
					return std::make_unique<type>(nullptr);
				}
				else
				{
					return type{};
				}
			}
		}

		/*
			@brief Sets the value of the field.
			@param value Value to set (primitive or unique_ptr for objects)
			@details Handles static and instance fields appropriately.
					 Converts C++ types to JNI types where necessary.
		*/
		auto set(const type& value) const
			-> void
		{
			try
			{
				const auto& inner_map{ jni::field_ids.at(this->index) };
				const auto it{ inner_map.find(this->name) };

				if (it == inner_map.end())
				{
					throw jni::jni_exception{ "Field ID not found." };
				}

				const jfieldID field_id{ it->second };

				if constexpr (std::is_base_of_v<object, type>)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetObjectField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							value->get_instance()
						);
					}
					else
					{
						jni::get_env()->SetStaticObjectField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							value->get_instance()
						);
					}
				}

				if constexpr (std::is_same<type, short>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetShortField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jshort>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticShortField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jshort>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, int>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetIntField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jint>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticIntField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jint>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, long long>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetLongField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jlong>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticLongField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jlong>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, float>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetFloatField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jfloat>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticFloatField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jfloat>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, double>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetDoubleField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jdouble>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticDoubleField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jdouble>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, bool>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetBooleanField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jboolean>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticBooleanField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jboolean>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, char>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetCharField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jchar>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticCharField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jchar>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, std::byte>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetByteField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jbyte>(value)
						);
					}
					else
					{
						jni::get_env()->SetStaticByteField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jbyte>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, std::string>::value)
				{
					if (this->field_type == jni::field_type::NOT_STATIC)
					{
						jni::get_env()->SetObjectField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							jni::string(value).get_jni_string()
						);
					}
					else
					{
						jni::get_env()->SetStaticObjectField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							jni::string(value).get_jni_string()
						);
					}
				}
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::field.set() for {}: {}", jni::easy_jni_error, this->name, e.what());
				return;
			}
		}

	private:
		/*
			@brief Stores the class pointer (for static) or instance pointer (for instance fields)
			to avoid passing it for every get/set call.
		*/
		void* class_or_instance;

		/*
			@brief Name of the Java field
		*/
		std::string name;

		/*
			@brief type_index of the C++ class owning this field
		*/
		std::type_index index;

		/*
			@brief Indicates if the field is static or instance
		*/
		field_type field_type;
	};

	/*
		@brief Stores a cached method ID and its optional JVMTI method pointer.
	*/
	struct cached_method
	{
		/*
			@brief JNI method ID
		*/
		jmethodID id;

		/*
			@brief Optional pointer to the JVMTI method structure
			@details Used for HotSpot-specific introspection or profiling
		*/
		jni::hotspot::method* ptr;
	};

	/*
		@brief Maps C++ class types to their Java method IDs.
		@details
		- First key: std::type_index of the C++ class
		- Second key: method name as string
		- Value: cached_method containing JNI method ID and optional JVMTI pointer
		- Speeds up repeated calls by avoiding repeated GetMethodID/GetStaticMethodID calls
	*/
	inline std::unordered_map<std::type_index, std::unordered_map<std::string, jni::cached_method>> method_ids{};

	/*
		@brief Specifies whether a Java method is static or an instance method.
	*/
	enum class method_type : std::uint8_t
	{
		STATIC,
		NOT_STATIC
	};

	/*
		@brief Represents a Java method and provides call access from C++.
		@tparam return_type The C++ type corresponding to the Java method's return type (primitive, object, string, void, etc.)
		@details
		- Supports static and instance methods.
		- Supports all primitive return types (short, int, long long, float, double, bool, char, std::byte),
		  std::string, jobjectArray, and JNI objects derived from `jni::object` (wrapped in std::unique_ptr when returned).
		- Uses `jni::method_ids` to cache jmethodID for fast repeated calls.
		- Arguments are passed as C++ types and converted to JNI jvalue array via `jni::build_jargs`.
		- Should not be used outside this header.
	*/
	template <method_return return_type>
	class method final
	{
	public:
		/*
			@brief Constructs a method object.
			@param class_or_instance Pointer to the Java class (for static) or instance (for instance methods)
			@param name Method name as a string
			@param method_type Indicates whether the method is static or instance
			@param index type_index of the C++ class owning the method (used for lookup in method_ids)
		*/
		method(void* class_or_instance, const std::string_view name, const jni::method_type method_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, index{ index }
			, method_type{ method_type }
		{

		}

		/*
			@brief Calls the Java method with the given arguments.
			@tparam args_t Variadic template arguments constrained by `callable_arg`
			@param args Arguments to pass to the method (primitives or unique_ptr to JNI objects)
			@return For primitive types, returns the value directly.
					For JNI objects, returns std::unique_ptr<return_type>.
					For void methods, returns nothing.
			@throws std::runtime_error if the method ID cannot be found.
			@details
			- Converts C++ arguments to JNI jvalue array via `jni::build_jargs`.
			- Dispatches to the appropriate JNI call (CallXxxMethodA or CallStaticXxxMethodA)
			  depending on the return type and whether the method is static.
			- On exception, returns a default-constructed value or nullptr unique_ptr.
			@note Do not pass jobject directly, pass primitive types or unique_ptr to JNI objects.
		*/
		template<callable_arg... args_t>
		auto call(args_t&&... args) const
			-> std::conditional_t<std::is_base_of_v<object, return_type>, std::unique_ptr<return_type>, return_type>
		{
			try
			{
				const auto& inner_map{ jni::method_ids.at(this->index) };
				const auto it{ inner_map.find(this->name) };

				if (it == inner_map.end())
				{
					throw jni::jni_exception{ "Method ID not found." };
				}

				const jmethodID method_id{ it->second.id };

				auto [jargs, string_keeper] = jni::build_jargs(std::forward<args_t>(args)...);
				const jvalue* jargs_ptr{ jargs.empty() ? nullptr : jargs.data() };

				if constexpr (std::is_base_of_v<object, return_type>)
				{
					jobject local{};
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						local = jni::get_env()->CallObjectMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						local = jni::get_env()->CallStaticObjectMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}

					std::unique_ptr<return_type> result{ std::make_unique<return_type>(local) };

					if (local)
					{
						jni::get_env()->DeleteLocalRef(local);
					}

					return result;
				}

				std::any result{};
				if constexpr (std::is_same_v<return_type, void>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						jni::get_env()->CallVoidMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						jni::get_env()->CallStaticVoidMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					return;
				}
				if constexpr (std::is_same_v<return_type, short>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallShortMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticShortMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, int>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallIntMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticIntMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, long long>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallLongMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticLongMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, float>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallFloatMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticFloatMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, double>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallDoubleMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticDoubleMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, bool>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallBooleanMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticBooleanMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, char>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallCharMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticCharMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, std::byte>)
				{
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						result = jni::get_env()->CallByteMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = jni::get_env()->CallStaticByteMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, std::string>)
				{
					jobject local{};
					if (this->method_type == jni::method_type::NOT_STATIC)
					{
						local = jni::get_env()->CallObjectMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						local = jni::get_env()->CallStaticObjectMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}

					if (!local)
					{
						return std::string{};
					}

					const jni::string str{ static_cast<jstring>(local) };
					jni::get_env()->DeleteLocalRef(local);

					return str.get_std_string();
				}

				if constexpr (std::is_same_v<return_type, jobjectArray>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						return static_cast<jobjectArray>(jni::get_env()->CallObjectMethodA(
							reinterpret_cast<jobject>(this->class_or_instance), method_id, jargs_ptr)
							);
					}
					else
					{
						return static_cast<jobjectArray>(jni::get_env()->CallStaticObjectMethodA(
							reinterpret_cast<jclass>(this->class_or_instance), method_id, jargs_ptr)
							);
					}
				}

				if constexpr (!std::is_base_of_v<object, return_type> && !std::is_same_v<return_type, void>)
				{
					auto it = jni::jni_to_cpp.find(result.type());
					if (it != jni::jni_to_cpp.end())
					{
						return std::any_cast<return_type>(it->second(result));
					}
					return return_type{};
				}
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::method.call() for {}: {}", jni::easy_jni_error, this->name, e.what());

				if constexpr (std::is_same_v<return_type, void>)
				{
					return;
				}
				else if constexpr (std::is_base_of_v<object, return_type>)
				{
					return std::make_unique<return_type>(nullptr);
				}
				else
				{
					return return_type{};
				}
			}
		}

	private:
		/*
			@brief Stores the class pointer (for static) or instance pointer (for instance methods)
			to avoid passing it on every call.
		*/
		void* class_or_instance;

		/*
			@brief Name of the Java method.
		*/
		std::string name;

		/*
			@brief type_index of the C++ class owning this method.
		*/
		std::type_index index;

		/*
			@brief Indicates if the method is static or instance.
		*/
		method_type method_type;
	};

	/*
		@brief Base class representing a Java object accessible from C++.
		@details
		- Wraps a jobject as a global JNI reference to prevent it from being
		  garbage collected while the C++ object is alive.
		- Inherit from this class to create a typed wrapper for a Java class.
		- Provides access to fields via `get_field` and methods via `get_method`,
		  which cache their respective IDs for fast repeated use.
		- The wrapped instance may be nullptr if the Java object is also null in the JVM.
		- All copies create a new independent global reference; moves transfer ownership.
	*/
	class object
	{
	public:
		/*
			@brief Constructs an object from a jobject.
			@param instance The Java object to wrap (may be nullptr)
			@note Creates a global JNI reference to prevent garbage collection.
				  If nullptr is passed, no global reference is created.
		*/
		explicit object(const jobject instance = nullptr)
			// Create a global reference of the jobject that is how every jobjects are managed
			: instance{ instance ? jni::get_env()->NewGlobalRef(instance) : nullptr }
		{

		}

		/*
			@brief Copy constructor.
			@param other Another object instance to copy from
			@note Creates a new independent global reference for the copy.
		*/
		object(const object& other)
			: instance{ other.instance ? jni::get_env()->NewGlobalRef(other.instance) : nullptr }
		{

		}

		/*
			@brief Copy assignment operator.
			@param other Another object instance to copy from
			@return Reference to *this
			@note Deletes the existing global reference before creating a new one.
		*/
		auto operator=(const object& other) 
			-> object&
		{
			if (this == &other)
			{
				return *this;
			}

			if (this->instance)
			{
				jni::get_env()->DeleteGlobalRef(this->instance);
			}

			this->instance = other.instance ? jni::get_env()->NewGlobalRef(other.instance) : nullptr;

			return *this;
		}

		/*
			@brief Move constructor.
			@param other Another object instance to move from
			@note Transfers ownership of the global reference without creating a new one.
		*/
		object(object&& other) noexcept
			: instance{ other.instance }
		{
			other.instance = nullptr;
		}

		/*
			@brief Move assignment operator.
			@param other Another object instance to move from
			@return Reference to *this
			@note Transfers ownership and deletes the existing global reference if present.
		*/
		auto operator=(object&& other) noexcept 
			-> object&
		{
			if (this == &other)
			{
				return *this;
			}

			if (this->instance)
			{
				jni::get_env()->DeleteGlobalRef(this->instance);
			}

			this->instance = other.instance;
			other.instance = nullptr;

			return *this;
		}

		/*
			@brief Destructor.
			@note Deletes the JNI global reference if one was created.
		*/
		virtual ~object() noexcept
		{
			if (this->instance)
			{
				// delete the global reference created
				jni::get_env()->DeleteGlobalRef(this->instance);
			}
		}

		/*
			@brief Retrieves a wrapped field that can be read or written.
			@tparam type The C++ type corresponding to the Java field type (must satisfy field_type_c)
			@param field_name Name of the Java field
			@param field_type Indicates whether the field is static or instance (default: NOT_STATIC)
			@return A unique_ptr to a field<type> allowing get/set access.
					On failure, returns a field wrapping a nullptr instance.
			@throws std::runtime_error if the class is not registered or the instance is null
				  for a non-static field.
			@note Caches the jfieldID on first call for fast repeated access.
		*/
		template<field_type_c type>
		auto get_field(const std::string& field_name, const jni::field_type field_type = jni::field_type::NOT_STATIC) const
			-> std::unique_ptr<field<type>>
		{
			const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(*this) })) };

			try
			{
				if (!clazz)
				{
					throw jni::jni_exception{ "Class not found." };
				}

				if (!this->instance && field_type == jni::field_type::NOT_STATIC)
				{
					throw jni::jni_exception{ "Instance is null." };
				}

				this->register_field_id<type>(clazz, field_name, field_type, std::type_index{ typeid(*this) });

				return std::make_unique<field<type>>(
					field_type == field_type::NOT_STATIC ? static_cast<void*>(this->instance) : static_cast<void*>(clazz),
					field_name,
					field_type,
					std::type_index{ typeid(*this) }
				);
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::object.get_field() for {}: {}", jni::easy_jni_error, field_name, e.what());

				return std::make_unique<field<type>>(nullptr, field_name, field_type, std::type_index{ typeid(*this) });
			}
		}

		/*
			@brief Retrieves a wrapped method that can be called.
			@tparam return_type The C++ return type of the Java method (must satisfy method_return)
			@tparam args_t The C++ types of the method's parameters (must satisfy callable_arg)
			@param method_name Name of the Java method
			@param method_type Indicates whether the method is static or instance (default: NOT_STATIC)
			@return A unique_ptr to a method<return_type> allowing call access.
					On failure, returns a method wrapping a nullptr instance.
			@throws std::runtime_error if the class is not registered or the instance is null
					for a non-static method.
			@note Caches the jmethodID on first call for fast repeated access.
				  The method signature is derived automatically from the template parameters.
		*/
		template<method_return return_type, callable_arg... args_t>
		auto get_method(const std::string& method_name, const jni::method_type method_type = jni::method_type::NOT_STATIC) const
			-> std::unique_ptr<method<return_type>>
		{
			const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(*this) })) };

			try
			{
				if (!clazz)
				{
					throw jni::jni_exception{ "Class not found." };
				}

				if (!this->instance && method_type == jni::method_type::NOT_STATIC)
				{
					throw jni::jni_exception{ "Instance is null." };
				}

				this->register_method_id<return_type, args_t...>(clazz, method_name, method_type, std::type_index{ typeid(*this) });

				return std::make_unique<method<return_type>>(
					method_type == jni::method_type::NOT_STATIC ? static_cast<void*>(this->instance) : static_cast<void*>(clazz),
					method_name,
					method_type,
					std::type_index{ typeid(*this) }
				);
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::object.get_method() for {}: {}", jni::easy_jni_error, method_name, e.what());

				return std::make_unique<method<return_type>>(nullptr, method_name, method_type, std::type_index{ typeid(*this) });
			}
		}

		/*
			@brief Returns the underlying JNI global reference.
			@return The wrapped jobject, or nullptr if the Java object is null.
			@note The returned jobject remains valid as long as this object is alive.
				Unique pointers to object are never nullptr, but the instance they
				wrap may be nullptr if the Java object is null in the JVM.
		*/
		inline auto get_instance() const noexcept
			-> jobject
		{
			return this->instance;
		}

	protected:
		/*
			@brief The JNI global reference to the wrapped Java object.
			@note Managed exclusively by this class — do not delete manually.
		*/
		jobject instance;

	private:
		/*
			@brief Looks up or registers the jfieldID for a given field.
			@tparam type The C++ type of the field (must satisfy field_type_c)
			@param clazz The jclass of the owning Java class
			@param field_name Name of the Java field
			@param field_type Indicates whether the field is static or instance
			@param index type_index of the C++ class owning the field
			@note Skips registration if the ID is already cached in jni::field_ids.
				  The JNI signature is derived automatically from the type template parameter.
		*/
		template<field_type_c type>
		auto register_field_id(const jclass clazz, const std::string& field_name, const jni::field_type field_type, const std::type_index index) const
			-> void
		{
			if (jni::field_ids[std::type_index{ index }].find(field_name)
				!= jni::field_ids[std::type_index{ index }].end())
			{
				return;
			}

			const std::string& signature{ jni::get_signature<type>() };

			try
			{
				if (signature.empty())
				{
					throw jni::jni_exception{ "Failed to get signature." };
				}

				jfieldID field_id{};
				if (field_type == jni::field_type::NOT_STATIC)
				{
					field_id = jni::get_env()->GetFieldID(clazz, field_name.c_str(), signature.c_str());
				}
				else
				{
					field_id = jni::get_env()->GetStaticFieldID(clazz, field_name.c_str(), signature.c_str());
				}

				if (!field_id)
				{
					throw jni::jni_exception{ "Failed to get field id." };
				}

				jni::field_ids[std::type_index{ index }].insert(
					{ field_name, field_id }
				);
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::object.register_field_id() for {}: {}", jni::easy_jni_error, field_name, e.what());
				return;
			}
		}

		/*
			@brief Looks up or registers the jmethodID for a given method.
			@tparam return_type The C++ return type of the method (must satisfy method_return)
			@tparam args_t The C++ parameter types of the method (must satisfy callable_arg)
			@param clazz The jclass of the owning Java class
			@param method_name Name of the Java method
			@param method_type Indicates whether the method is static or instance
			@param index type_index of the C++ class owning the method
			@note Skips registration if the ID is already cached in jni::method_ids,
				  unless the underlying HotSpot method pointer has changed (e.g. after
				  class redefinition), in which case the cached ID is invalidated and refreshed.
				  The JNI signature is built automatically from the template parameters.
		*/
		template<method_return return_type, callable_arg... args_t>
		auto register_method_id(const jclass clazz, const std::string& method_name, const jni::method_type method_type, const std::type_index index = typeid(*this)) const
			-> void
		{
			auto& inner{ jni::method_ids[std::type_index{ index }] };
			if (auto it{ inner.find(method_name) }; it != inner.end())
			{
				hotspot::method* current = *(hotspot::method**)it->second.id;
				if (current == it->second.ptr)
				{
					return;
				}
				inner.erase(it);
			}

			std::string params_sig{};
			((params_sig += jni::get_signature<std::remove_cvref_t<args_t>>()), ...);

			const std::string signature{ std::format("({}){}", params_sig, jni::get_signature<return_type>()) };

			try
			{
				if (signature.empty())
				{
					throw jni::jni_exception{ "Failed to get signature." };
				}

				jmethodID method_id{};
				if (method_type == jni::method_type::NOT_STATIC)
				{
					method_id = jni::get_env()->GetMethodID(clazz, method_name.c_str(), signature.c_str());
				}
				else
				{
					method_id = jni::get_env()->GetStaticMethodID(clazz, method_name.c_str(), signature.c_str());
				}

				if (!method_id)
				{
					throw jni::jni_exception{ "Failed to get method id." };
				}

				inner.insert({
						method_name,
						{ method_id, *(hotspot::method**)method_id }
					});
			}
			catch (const std::exception& e)
			{
				std::println("{} jni::object.register_field_id() for {}: {}", jni::easy_jni_error, method_name, e.what());
				return;
			}
		}

		/*
			@brief Grants make_unique<> access to NewObjectA for constructor calls.
			@see jni::make_unique
		*/
		template<jni_object type, callable_arg... args_t>
		friend auto make_unique(args_t&&... args)
			-> std::unique_ptr<type>;
	};

	/*
		Java data structure helpers.
	*/

	/*
		@brief Wrapper for the Java Collection interface.
		@details
		- Inherits from jni::object and adds a helper to convert the collection
		  to a C++ std::vector.
		- Can be inherited to add more specific methods for a given collection type.
		- The wrapped instance may be nullptr if the Java object is also null in the JVM.
	*/
	class collection : public jni::object
	{
	public:
		/*
			@brief Constructs a collection from a jobject.
			@param instance The Java Collection object to wrap (may be nullptr)
		*/
		explicit collection(const jobject instance = nullptr)
			: jni::object{ instance }
		{

		}

		/*
			@brief Converts the Java Collection to a C++ std::vector.
			@tparam type The C++ type of the collection's elements.
					Must be either a class derived from jni::object or std::string.
			@return For jni::object-derived types, returns std::vector<std::unique_ptr<type>>.
					For std::string, returns std::vector<std::string>.
					Returns an empty vector if the instance is nullptr, the underlying
					toArray() call fails, or the collection is empty.
			@details
			- Calls Java's Collection::toArray() internally to retrieve elements.
			- Each element is wrapped in std::unique_ptr<type> (or std::string) and
			  its local JNI reference is deleted after wrapping.
			- Null elements in the Java collection produce nullptr unique_ptrs
			  or empty strings respectively.
		*/
		template<typename type>
			requires (std::is_base_of_v<object, type> || std::is_same_v<type, std::string>)
		auto to_vector() const
			-> std::conditional_t<std::is_same_v<type, std::string>, std::vector<std::string>, std::vector<std::unique_ptr<type>>>
		{
			using result_t = std::conditional_t<std::is_same_v<type, std::string>, std::vector<std::string>, std::vector<std::unique_ptr<type>>>;

			try
			{
				if (!this->instance)
				{
					return result_t{};
				}

				const jobjectArray array{ this->get_method<jobjectArray>("toArray")->call() };

				if (!array)
				{
					return result_t{};
				}

				const jsize length{ jni::get_env()->GetArrayLength(array) };

				if (length <= 0)
				{
					jni::get_env()->DeleteLocalRef(array);
					return result_t{};
				}

				result_t result{};
				result.reserve(static_cast<std::size_t>(length));

				for (jsize i{ 0 }; i < length; ++i)
				{
					const jobject element{ jni::get_env()->GetObjectArrayElement(array, i) };

					if constexpr (std::is_same_v<type, std::string>)
					{
						if (element)
						{
							result.emplace_back(jni::string(static_cast<jstring>(element)).get_std_string());
							jni::get_env()->DeleteLocalRef(element);
						}
						else
						{
							result.emplace_back(std::string{});
						}
					}
					else
					{
						result.emplace_back(std::make_unique<type>(element));

						if (element)
						{
							jni::get_env()->DeleteLocalRef(element);
						}
					}
				}

				jni::get_env()->DeleteLocalRef(array);

				return result;
			}
			catch (...)
			{
				return result_t{};
			}
		}
	};

	/*
		@brief Wrapper for the Java List interface.
		@details
		- Inherits from jni::collection, providing the same to_vector() helper.
		- No additional methods are defined beyond those inherited from collection.
	*/	
	class list final : public jni::collection
	{
	public:
		/*
			@brief Constructs a list from a jobject.
			@param instance The Java List object to wrap (may be nullptr)
		*/
		explicit list(const jobject instance = nullptr)
			: jni::collection{ instance }
		{

		}
	};

	/*
		@brief Wrapper for the Java UUID class.
		@details
		- Inherits from jni::object and exposes the version() method.
	*/
	class uuid final : public jni::object
	{
	public:
		/*
			@brief Constructs a uuid from a jobject.
			@param instance The Java UUID object to wrap (may be nullptr)
		*/
		explicit uuid(const jobject instance = nullptr)
			: jni::object{ instance }
		{

		}

		/*
			@brief Returns the version number of this UUID.
			@return The UUID version as an int (e.g. 1, 4).
			@details Calls Java's UUID::version() method internally.
		*/
		auto version() const
			-> int
		{
			return get_method<int>("version")->call();
		}
	};

	/*
		@brief Creates a new Java object using its constructor and wraps it in a std::unique_ptr.
		@tparam type The C++ wrapper type to instantiate (must satisfy jni_object)
		@tparam args_t The C++ types of the constructor's parameters (must satisfy callable_arg)
		@param args Arguments to pass to the Java constructor
		@return A std::unique_ptr<type> wrapping the newly created Java object.
				On failure, returns a unique_ptr wrapping a nullptr instance.
		@details
		- Looks up the registered Java class for the given C++ type via jni::class_map.
		- Resolves and caches the constructor jmethodID (registered as "<init>").
		- Converts C++ arguments to a JNI jvalue array via jni::build_jargs.
		- Calls JNI's NewObjectA and wraps the resulting jobject in a std::unique_ptr<type>.
		- The local reference returned by NewObjectA is deleted after wrapping.
		@note The class must have been registered via jni::register_class<type>() beforehand.
			  This function is a friend of jni::object to access register_method_id internally.
	*/
	template<jni_object type, callable_arg... args_t>
	auto make_unique(args_t&&... args)
		-> std::unique_ptr<type>
	{
		const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(type) })) };

		try
		{
			if (!clazz)
			{
				throw std::runtime_error{ std::format("Class not found for constructor.") };
			}

			jni::object temp{ nullptr };
			temp.register_method_id<void, typename unwrap_object_ptr<std::remove_cvref_t<args_t>>::type...>(
				clazz, "<init>", jni::method_type::NOT_STATIC, std::type_index{ typeid(type) }
			);

			const jmethodID constructor_id{ jni::method_ids.at(typeid(type)).at("<init>").id };

			auto [jargs, string_keeper] = jni::build_jargs(std::forward<args_t>(args)...);
			const jvalue* jargs_ptr{ jargs.empty() ? nullptr : jargs.data() };

			const jobject local{ jni::get_env()->NewObjectA(clazz, constructor_id, jargs_ptr) };

			std::unique_ptr<type> result{ std::make_unique<type>(local) };
			jni::get_env()->DeleteLocalRef(local);

			return result;
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::make_unique() {}", jni::easy_jni_error, e.what());

			return std::make_unique<type>(nullptr);
		}
	}

	/*
		@brief List of JVMTI versions to attempt when acquiring the JVMTI environment.
		@details
		- Versions are tried in descending order to obtain the most recent
		  available JVMTI environment supported by the running JVM.
	*/
	inline const constexpr DWORD versions[]
	{
		JVMTI_VERSION_1_2,
		JVMTI_VERSION_1_1,
		JVMTI_VERSION_1_0,
		JVMTI_VERSION
	};

	/*
		@brief Initializes EasyJNI.
		@return true on success, false if any step of the initialization fails.
		@details
		- Retrieves the running JavaVM via JNI_GetCreatedJavaVMs.
		- Attaches the current thread to the JVM if not already attached.
		- Acquires a JVMTI environment by trying each version in jni::versions
		  in order, stopping at the first success.
		- Registers built-in Java data structure wrappers:
		  jni::collection, jni::list, and jni::uuid.
		@note Must be called before any other EasyJNI function.
			  Requires a JVM to already be running in the process.
	*/
	static auto init()
		-> bool
	{
		try
		{
			jsize count{ 0 };
			if (JNI_GetCreatedJavaVMs(&jni::vm, 1, &count) != JNI_OK)
			{
				throw std::runtime_error{ "Failed to get created Java VMs." };
			}

			if (!jni::vm)
			{
				throw std::runtime_error{ "No Java VM found." };
			}

			JNIEnv* env{ nullptr };
			if (jni::vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) == JNI_EDETACHED)
			{
				if (jni::vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK)
				{
					throw std::runtime_error{ "Failed to attach init thread." };
				}
			}

			if (!env)
			{
				throw std::runtime_error{ "JNIEnv is null after attach." };
			}

			for (const DWORD version : jni::versions)
			{
				if (jni::vm->GetEnv(reinterpret_cast<void**>(&jni::jvmti), version) == JNI_OK && jni::jvmti)
				{
					break;
				}
			}

			// Already register some java datastructures that have helper methods
			jni::register_class<jni::collection>("java/util/Collection");
			jni::register_class<jni::list>("java/util/List");
			jni::register_class<jni::uuid>("java/util/UUID");

			return true;
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::init() {}", jni::easy_jni_error, e.what());
			return false;
		}
	}

	/*
		@brief Shuts down EasyJNI.
		@details
		- Runs any registered shutdown hooks via jni::shutdown_hooks().
		- Deletes all cached global jclass references stored in jni::classes
		  to prevent memory leaks.
		- Detaches the current thread from the JVM via jni::exit_thread().
		@note Should be called before the consuming thread exits to ensure
			  proper cleanup of JNI global references.
		@warning Any use of EasyJNI after this call results in undefined behaviour.
	*/
	static auto shutdown() noexcept
		-> void
	{
		std::println("{} shutdown() Shutdown", jni::easy_jni_info);

		jni::shutdown_hooks();

		for (const auto& [_, clazz] : jni::classes)
		{
			if (clazz)
			{
				jni::get_env()->DeleteGlobalRef(clazz);
			}
		}

		jni::exit_thread();
	}
}

// this part will handle method hooking with the HotSpot
namespace jni
{
	namespace hotspot
	{
		// https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
		typedef struct vm_type_entry
		{
			const char* type_name;
			const char* superclass_name;

			std::int32_t is_oop_type;
			std::int32_t is_integer_type;
			std::int32_t is_unsigned;

			std::uint64_t size;
		} vm_type_entry_t, * vm_type_entry_ptr_t;

		// https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
		typedef struct vm_struct_entry
		{
			const char* type_name;
			const char* field_name;
			const char* type_string;

			std::int32_t is_static;

			std::uint64_t offset;

			void* address;
		} vm_struct_entry_t, * vm_struct_entry_ptr_t;

		extern "C" JNIIMPORT vm_type_entry_ptr_t gHotSpotVMTypes;
		extern "C" JNIIMPORT vm_struct_entry_ptr_t gHotSpotVMStructs;

		// find offsets based on the jvm
		static auto find_VMStructEntry(const char* type_mame, const char* field_name)
			-> vm_struct_entry_ptr_t
		{
			for (vm_struct_entry_ptr_t entry{ gHotSpotVMStructs }; entry->type_name; ++entry)
			{
				if (std::strcmp(entry->type_name, type_mame))
				{
					continue;
				}

				if (std::strcmp(entry->field_name, field_name))
				{
					continue;
				}

				return entry;
			}

			return nullptr;
		}

		struct symbol
		{
			auto to_string()
				-> std::string
			{
				static vm_struct_entry_ptr_t length_entry{ find_VMStructEntry("Symbol", "_length") };
				static vm_struct_entry_ptr_t body_entry{ find_VMStructEntry("Symbol", "_body") };
				if (!length_entry || !body_entry)
				{
					return "";
				}

				const std::uint16_t length{ *(std::uint16_t*)((std::uint8_t*)this + length_entry->offset) };
				const char* body{ (const char*)((std::uint8_t*)this + body_entry->offset) };

				return std::string{ body, length };
			}
		};

		struct constant_pool
		{
			auto get_base()
				-> void**
			{
				static vm_type_entry_ptr_t entry = []() -> vm_type_entry_ptr_t
				{
					for (vm_type_entry_ptr_t _entry{ gHotSpotVMTypes }; _entry->type_name; ++_entry)
					{
						if (!std::strcmp(_entry->type_name, "ConstantPool"))
						{
							return _entry;
						}
					}

					return nullptr;
				}();

				if (!entry)
				{
					return nullptr;
				}

				return (void**)((std::uint8_t*)this + entry->size);
			}
		};

		struct const_method
		{
			auto get_constants()
				-> constant_pool*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("ConstMethod", "_constants") };
				if (!entry)
				{
					return nullptr;
				}

				return *(constant_pool**)((std::uint8_t*)this + entry->offset);
			}

			auto get_name()
				-> symbol*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("ConstMethod", "_name_index") };
				if (!entry)
				{
					return nullptr;
				}

				const std::uint16_t index = *(std::uint16_t*)((std::uint8_t*)this + entry->offset);
				return (symbol*)get_constants()->get_base()[index];
			}

			auto get_signature()
				-> symbol*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("ConstMethod", "_signature_index") };
				if (!entry)
				{
					return nullptr;
				}

				const std::uint16_t index{ *(std::uint16_t*)((std::uint8_t*)this + entry->offset) };
				return (symbol*)get_constants()->get_base()[index];
			}
		};

		struct method
		{
			auto get_i2i_entry()
				-> void*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("Method", "_i2i_entry") };
				if (!entry)
				{
					return nullptr;
				}

				return *(void**)((std::uint8_t*)this + entry->offset);
			}

			auto get_from_interpreted_entry()
				-> void*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("Method", "_from_interpreted_entry") };
				if (!entry)
				{
					return nullptr;
				}

				return *(void**)((std::uint8_t*)this + entry->offset);
			}

			auto get_access_flags()
				-> std::uint32_t*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("Method", "_access_flags") };
				if (!entry)
				{
					return nullptr;
				}

				return (uint32_t*)((std::uint8_t*)this + entry->offset);
			}

			auto get_flags()
				-> std::uint16_t*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("Method", "_flags") };
				if (!entry)
				{
					return nullptr;
				}

				return (std::uint16_t*)((std::uint8_t*)this + entry->offset);
			}

			auto get_const_method()
				-> const_method*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("Method", "_constMethod") };
				if (!entry)
				{
					return nullptr;
				}

				return *(const_method**)((std::uint8_t*)this + entry->offset);
			}

			auto get_name()
				-> std::string
			{
				const_method* const_method{ get_const_method() };
				if (!const_method)
				{
					return "";
				}

				symbol* symbol{ const_method->get_name() };
				if (!symbol)
				{
					return "";
				}

				return symbol->to_string();
			}

			auto get_signature()
				-> std::string
			{
				const_method* const_method{ get_const_method() };
				if (!const_method)
				{
					return "";
				}

				symbol* symbol{ const_method->get_signature() };
				if (!symbol)
				{
					return "";
				}

				return symbol->to_string();
			}
		};

		enum java_thread_state : std::int8_t
		{
			_thread_uninitialized = 0,
			_thread_new = 2,
			_thread_new_trans = 3,
			_thread_in_native = 4,
			_thread_in_native_trans = 5,
			_thread_in_vm = 6,
			_thread_in_vm_trans = 7,
			_thread_in_Java = 8,
			_thread_in_Java_trans = 9,
			_thread_blocked = 10,
			_thread_blocked_trans = 11,
			_thread_max_state = 12
		};

		struct java_thread
		{
			auto get_env()
				-> JNIEnv*
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("JavaThread", "_anchor") };
				if (!entry)
				{
					return nullptr;
				}

				// _anchor is a JavaFrameAnchor, JNIEnv* is at +32
				return (JNIEnv*)((std::uint8_t*)this + entry->offset + 32);
			}

			auto get_thread_state()
				-> java_thread_state
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("JavaThread", "_thread_state") };
				if (!entry)
				{
					return _thread_uninitialized;
				}

				return *(java_thread_state*)((std::uint8_t*)this + entry->offset);
			}

			auto set_thread_state(const java_thread_state state)
				-> void
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("JavaThread", "_thread_state") };
				if (!entry)
				{
					return;
				}

				*(java_thread_state*)((std::uint8_t*)this + entry->offset) = state;
			}

			auto get_suspend_flags()
				-> std::uint32_t
			{
				static vm_struct_entry_ptr_t entry{ find_VMStructEntry("JavaThread", "_suspend_flags") };
				if (!entry)
				{
					return 0;
				}

				return *(std::uint32_t*)((std::uint8_t*)this + entry->offset);
			}
		};

		// 0x00 is the wildcard
		static auto match_pattern(const std::uint8_t* addr, const std::uint8_t* pattern, const std::size_t size)
			-> bool
		{
			for (std::size_t i{ 0 }; i < size; ++i)
			{
				if (pattern[i] == 0x00)
				{
					continue; // wildcard
				}

				if (addr[i] != pattern[i])
				{
					return false;
				}
			}
			return true;
		}

		static auto scan(const std::uint8_t* start, const std::size_t range, const std::uint8_t* pattern, const std::size_t size)
			-> std::uint8_t*
		{
			for (std::size_t i{ 0 }; i < range; ++i)
			{
				if (match_pattern(start + i, pattern, size))
				{
					return (std::uint8_t*)(start + i);
				}
			}
			return nullptr;
		}

		// default value for latest java
		inline std::int8_t locals_offset{ -56 };

		static auto find_hook_location(void* i2i_entry)
			-> void*
		{
			// 0x00 = wildcard
			const std::uint8_t pattern[] =
			{
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
				0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00
			};

			// mov r14, QWORD PTR [rbp + ??] ; ret
			const std::uint8_t locals_pattern[] =
			{
				0x4C, 0x8B, 0x75, 0x00, 0xC3
			};

			std::uint8_t* curr{ (std::uint8_t*)i2i_entry };

			// we scan the first 0x400 bytes from i2i_entry
			std::uint8_t* hook_location{ scan(curr, 0x400, pattern, sizeof(pattern)) };

			if (!hook_location)
			{
				std::println("[ERROR] find_hook_location() pattern not found\n");
				return nullptr;
			}

			// go up from hook_location to find locals_pattern
			// we check the last 100 bytes
			for (std::uint8_t* p = hook_location; p > hook_location - 100; --p)
			{
				if (match_pattern(p, locals_pattern, sizeof(locals_pattern)))
				{
					// p[3] is the ?? > meaning the local offset
					locals_offset = (std::int8_t)p[3];
					break;
				}
			}

			// the hook is 8 bytes before the end of the pattern
			// sizeof(pattern) - 8 is the position of mov BYTE PTR [r15+...], 0x0
			return hook_location + sizeof(pattern) - 8;
		}

		template<typename type>
		using argument_return_t = std::conditional_t<std::is_base_of_v<jni::object, type>, std::unique_ptr<type>, type>;

		struct frame
		{
		public:
			auto get_method() const
				-> method*
			{
				return *(method**)((std::uint8_t*)this - 24);
			}

			auto get_locals() const
				-> void**
			{
				return *(void***)((std::uint8_t*)this + locals_offset);
			}

			template<typename... types>
			auto get_arguments() const
				-> std::tuple<argument_return_t<types>...>
			{
				std::int32_t index{ 0 };
				return std::tuple<argument_return_t<types>...>
				{
					this->get_argument<types>(index++)...
				};
			}

		private:
			template<typename type>
			auto get_argument(const std::int32_t index) const
				-> std::conditional_t<std::is_base_of_v<jni::object, type>, std::unique_ptr<type>, type>
			{
				using return_type = std::conditional_t<std::is_base_of_v<jni::object, type>, std::unique_ptr<type>, type>;

				void** locals{ this->get_locals() };
				if (!locals)
				{
					return return_type{};
				}

				void* raw{ locals[-index] };

				if constexpr (std::is_same_v<type, std::string>)
				{
					if (!raw)
					{
						return std::string{};
					}

					std::uint8_t* char_array{ (std::uint8_t*)raw + 0x18 };

					std::int32_t length{};
					std::memcpy(&length, char_array + 12, sizeof(std::int32_t));

					if (length <= 0 || length > 512)
					{
						return std::string{};
					}

					std::string result(static_cast<std::size_t>(length), '\0');
					std::memcpy(result.data(), char_array + 16, static_cast<std::size_t>(length));

					return result;
				}
				else if constexpr (std::is_base_of_v<jni::object, type>)
				{
					if (!raw)
					{
						return return_type{};
					}

					jobject obj{ jni::get_env()->NewLocalRef(reinterpret_cast<jobject>(&raw)) };

					return return_type{ std::make_unique<type>(obj) };
				}
				else if constexpr (sizeof(type) <= sizeof(void*))
				{
					type result{};
					std::memcpy(&result, &raw, sizeof(type));
					return result;
				}
				else
				{
					return return_type{};
				}
			}
		};

		/*

			push rax         ; save all registers
			push rcx
			push rdx
			push r8
			push r9
			push r10
			push r11
			push rbp
			push 0x0         ; place for the custom return value

			mov rcx, rbp     ; first argument > frame* (rbp holds current frame)
			mov rdx, r15     ; second argument > thread* (r15 = JavaThread*)
			lea r8, [rsp]    ; third argument > bool* cancel (holds the 0x0 that we pushed)

			mov rbp, rsp     ; align the stack
			and rsp, 0xFFFFFFFFFFFFFFF0
			sub rsp, 0x20    ; shadow space windows x64

			call [rip+data]  ; our cpp detour

			mov rsp, rbp     ; restore the stack
			pop rax          ; rax = custom return value
			cmp rax, 0x0     ; cancel == true ?
			pop rbp
			pop r11          ; restore registers
			pop r10
			pop r9
			pop r8
			pop rdx
			pop rcx
			pop rax
			je 0x????????    ; if cancel -> returns the custom value
							 ; else do the normal code

		*/

		static auto allocate_nearby_memory(std::uint8_t* nearby_addr, const std::size_t size, const DWORD protect)
			-> std::uint8_t*
		{
			for (std::int64_t i{ 65536 }; i < 0x7FFFFFFF; i += 65536)
			{
				std::uint8_t* allocated = { (std::uint8_t*)VirtualAlloc(
					nearby_addr + i, size, MEM_COMMIT | MEM_RESERVE, protect
				) };
				if (allocated)
				{
					return allocated;
				}

				allocated = (std::uint8_t*)VirtualAlloc(
					nearby_addr - i, size, MEM_COMMIT | MEM_RESERVE, protect
				);
				if (allocated)
				{
					return allocated;
				}
			}

			return nullptr;
		}

		using detour_t = void(*)(frame*, java_thread*, bool*);

		class midi2i_hook final
		{
		public:
			midi2i_hook(std::uint8_t* target, detour_t detour)
				: target{ target }
				, allocated{ nullptr }
				, error{ true }
			{
				constexpr std::int32_t HOOK_SIZE = 8;
				constexpr std::int32_t JMP_SIZE = 5;
				constexpr std::int32_t JE_OFFSET = 0x3d;
				constexpr std::int32_t JE_SIZE = 6;
				constexpr std::int32_t DETOUR_ADDRESS_OFFSET = 0x56;

				std::uint8_t assembly[] =
				{
					0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x55,
					0x6A, 0x00,
					0x48, 0x89, 0xE9,
					0x4C, 0x89, 0xFA,
					0x4C, 0x8D, 0x04, 0x24,
					0x48, 0x89, 0xE5,
					0x48, 0x83, 0xE4, 0xF0,
					0x48, 0x83, 0xEC, 0x20,
					0xFF, 0x15, 0x2D, 0x00, 0x00, 0x00,
					0x48, 0x89, 0xEC,
					0x58,
					0x48, 0x83, 0xF8, 0x00,
					0x5D,
					0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
					0x5A, 0x59, 0x58,
					0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, // je - offset to patch
					0x66, 0x48, 0x0F, 0x6E, 0xC0,
					0x48, 0x8B, 0x5D, 0xF8,
					0x48, 0x89, 0xEC,
					0x5D, 0x5E,
					0x48, 0x89, 0xDC,
					0xFF, 0xE6,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // detour address
				};

				this->allocated = allocate_nearby_memory(target, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READWRITE);
				if (!this->allocated)
				{
					std::println("[ERROR] midi2i_hook failed to allocate memory");
					return;
				}

				// patch the je to return to the original code after the hook
				const std::int32_t je_delta{ (std::int32_t)(target + HOOK_SIZE - (this->allocated + HOOK_SIZE + JE_OFFSET + JE_SIZE)) };
				*(std::int32_t*)(assembly + JE_OFFSET + 2) = je_delta;

				// write at the detour adress in the stub
				*(detour_t*)(assembly + DETOUR_ADDRESS_OFFSET) = detour;

				// copy the original bytes then the stub
				std::memcpy(this->allocated, target, HOOK_SIZE);
				std::memcpy(this->allocated + HOOK_SIZE, assembly, sizeof(assembly));

				// make the stub only executable
				DWORD old_protect{};
				VirtualProtect(this->allocated, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READ, &old_protect);

				// path the target with the jmp to the stub
				VirtualProtect(target, JMP_SIZE, PAGE_EXECUTE_READWRITE, &old_protect);
				target[0] = 0xE9;

				const std::int32_t jmp_delta{ (std::int32_t)(this->allocated - (target + JMP_SIZE)) };
				*(std::int32_t*)(target + 1) = jmp_delta;
				VirtualProtect(target, JMP_SIZE, old_protect, &old_protect);

				this->error = false;
			}

			~midi2i_hook()
			{
				if (this->error)
				{
					return;
				}

				// restore original bytes
				DWORD old_protect{};
				if (this->target[0] == 0xE9 && VirtualProtect(this->target, 5, PAGE_EXECUTE_READWRITE, &old_protect))
				{
					std::memcpy(this->target, this->allocated, 5);
					VirtualProtect(this->target, 5, old_protect, &old_protect);
				}

				VirtualFree(this->allocated, 0, MEM_RELEASE);
			}

			inline auto has_error() const
				-> bool
			{
				return this->error;
			}

		private:
			std::uint8_t* target;
			std::uint8_t* allocated;
			bool error;
		};

		struct hooked_method
		{
			method* m{ nullptr };
			detour_t detour{ nullptr };
		};

		struct i2i_hook_data
		{
			void* i2i_entry{ nullptr };
			midi2i_hook* hook{ nullptr };
		};

		inline std::vector<hooked_method> hooked_methods{};
		inline std::vector<i2i_hook_data> hooked_i2i_entries{};

		static auto common_detour(frame* f, java_thread* thread, bool* cancel)
			-> void
		{
			if (!thread)
			{
				return;
			}

			if (!thread->get_env())
			{
				return;
			}

			if (thread->get_thread_state() != _thread_in_Java)
			{
				return;
			}

			method* current_method{ f->get_method() };

			for (hooked_method& hk : hooked_methods)
			{
				if (hk.m == current_method)
				{
					hk.detour(f, thread, cancel);

					thread->set_thread_state(_thread_in_Java);
					return;
				}
			}
		}

		// flags to desactivate jit compilation on hooked method
		inline constexpr std::int32_t NO_COMPILE =
			0x02000000 | // JVM_ACC_NOT_C2_COMPILABLE
			0x04000000 | // JVM_ACC_NOT_C1_COMPILABLE
			0x08000000 | // JVM_ACC_NOT_C2_OSR_COMPILABLE
			0x01000000;  // JVM_ACC_QUEUED

		static auto set_dont_inline(method* m, bool enabled)
			-> void
		{
			std::uint16_t* flags{ m->get_flags() };
			if (!flags)
			{
				return;
			}

			if (enabled)
			{
				*flags |= (1 << 2); // _dont_inline
			}
			else
			{
				*flags &= ~(1 << 2);
			}
		}
	}

	template <jni_object type>
	static auto hook(const std::string& method_name, hotspot::detour_t detour)
		-> bool
	{
		if (!detour)
		{
			return false;
		}

		const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(type) })) };
		if (!clazz)
		{
			std::println("[ERROR] hook() class not found");
			return false;
		}

		jint method_count{};
		jmethodID* methods{};
		if (jni::jvmti->GetClassMethods(clazz, &method_count, &methods) != JVMTI_ERROR_NONE)
		{
			std::println("[ERROR] hook() GetClassMethods failed");
			return false;
		}

		jmethodID method_id{ nullptr };
		for (jint i{ 0 }; i < method_count; ++i)
		{
			hotspot::method* m{ *(hotspot::method**)methods[i] };
			if (m && m->get_name() == method_name)
			{
				method_id = methods[i];
				break;
			}
		}

		jni::jvmti->Deallocate(reinterpret_cast<unsigned char*>(methods));

		if (!method_id)
		{
			std::println("[ERROR] hook: method '{}' not found", method_name);
			return false;
		}

		hotspot::method* m{ *(hotspot::method**)method_id };

		for (hotspot::hooked_method& hk : hotspot::hooked_methods)
		{
			if (hk.m == m)
			{
				return true;
			}
		}

		hotspot::set_dont_inline(m, true);

		std::uint32_t* flags{ m->get_access_flags() };
		*flags |= hotspot::NO_COMPILE;

		jclass owner{};
		jni::jvmti->GetMethodDeclaringClass(method_id, &owner);
		jni::jvmti->RetransformClasses(1, &owner);
		jni::get_env()->DeleteLocalRef(owner);

		m = *(hotspot::method**)method_id;

		hotspot::set_dont_inline(m, true);

		flags = m->get_access_flags();
		*flags |= hotspot::NO_COMPILE;

		hotspot::hooked_methods.push_back({ m, detour });

		void* i2i = m->get_i2i_entry();
		bool hook_new_i2i = true;

		for (hotspot::i2i_hook_data& hk : hotspot::hooked_i2i_entries)
		{
			if (hk.i2i_entry == i2i)
			{
				hook_new_i2i = false;
				break;
			}
		}

		if (!hook_new_i2i)
		{
			return true;
		}

		std::uint8_t* target{ (std::uint8_t*)hotspot::find_hook_location(i2i) };
		if (!target)
		{
			std::println("[ERROR] hook() failed to find hook location");
			return false;
		}

		hotspot::midi2i_hook* hook_instance{ new hotspot::midi2i_hook(target, hotspot::common_detour) };
		if (hook_instance->has_error())
		{
			delete hook_instance;
			return false;
		}

		hotspot::hooked_i2i_entries.push_back({ i2i, hook_instance });
		return true;
	}

	static auto shutdown_hooks()
		-> void
	{
		for (hotspot::i2i_hook_data& hk : hotspot::hooked_i2i_entries)
		{
			delete hk.hook;
		}

		for (hotspot::hooked_method& hm : hotspot::hooked_methods)
		{
			hotspot::set_dont_inline(hm.m, false);
			std::uint32_t* flags{ hm.m->get_access_flags() };
			*flags &= ~hotspot::NO_COMPILE;
		}

		hotspot::hooked_methods.clear();
		hotspot::hooked_i2i_entries.clear();
	}
}