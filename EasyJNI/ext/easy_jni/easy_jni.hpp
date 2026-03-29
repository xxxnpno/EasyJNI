#pragma once

#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <cmath>
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

#include <windows.h>

/*
	Java Native Interface and Java Virtual Machine Tool Interface includes.
*/
#include <jni/jni.h>
#include <jni/jvmti.h>

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

	auto shutdown_hooks() noexcept
		-> void;

	namespace hotspot
	{
		struct VMStructEntry;

		struct VMTypeEntry;

		struct symbol;

		struct constant_pool;

		struct const_method;

		struct method;

		struct java_thread;

		struct frame;

		class midi2i_hook;

		struct hooked_method;

		struct i2i_hook_data;
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

				const bool match{ !std::strcmp(_string.get_std_string().c_str(), class_name.data()) };

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

	// This part will handle hooks with the HotSpot.
	namespace hotspot
	{
		// https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
		struct VMTypeEntry
		{
			/*
				@brief Fully qualified name of the HotSpot type (e.g., "Method", "ConstantPool")
			*/
			const char* type_name;

			/*
				@brief Name of the superclass, or nullptr if none
			*/
			const char* superclass_name;

			/*
				@brief Non-zero if this type is an OOP (ordinary object pointer) type
			*/
			std::int32_t is_oop_type;

			/*
				@brief Non-zero if this type is an integer type
			*/
			std::int32_t is_integer_type;

			/*
				@brief Non-zero if this integer type is unsigned
			*/
			std::int32_t is_unsigned;

			/*
				@brief Size in bytes of this type in the JVM
			*/
			std::uint64_t size;
		};

		// https://github.com/openjdk/jdk/blob/master/src/hotspot/share/runtime/vmStructs.hpp
		struct VMStructEntry
		{
			/*
				@brief Name of the HotSpot type that owns this field (e.g., "Method", "ConstMethod")
			*/
			const char* type_name;

			/*
				@brief Name of the field within that type (e.g., "_i2i_entry", "_constMethod")
			*/
			const char* field_name;

			/*
				@brief JVM type string of the field (e.g., "void*", "int")
			*/
			const char* type_string;

			/*
				@brief Non-zero if the field is static (class-level), zero if it is an instance field
			*/
			std::int32_t is_static;

			/*
				@brief Byte offset of the field from the start of the owning type.
					   Used to access the field at runtime via pointer arithmetic.
					   Only meaningful for non-static fields.
			*/
			std::uint64_t offset;

			/*
				@brief Absolute address of the field in the JVM process memory.
					   Only meaningful for static fields (is_static != 0).
			*/
			void* address;
		};

		/*
			@brief Pointer to the global array of HotSpot VM type entries.
			@details
			Exported by the JVM (jvm.dll) and populated at JVM startup.
			Each entry describes a HotSpot internal type (name, superclass, size, etc.).
			The array is terminated by an entry with a nullptr type_name.
			@note Used by iterate_type_entries() to look up type metadata at runtime.
			@see VMTypeEntry
		*/
		extern "C" __declspec(dllimport) jni::hotspot::VMTypeEntry* gHotSpotVMTypes;

		/*
			@brief Pointer to the global array of HotSpot VM struct entries.
			@details
			Exported by the JVM (jvm.dll) and populated at JVM startup.
			Each entry describes a field of a HotSpot internal type (name, offset, address, etc.).
			The array is terminated by an entry with a nullptr type_name.
			@note Used by iterate_struct_entries() to look up field offsets and addresses at runtime.
			@see VMStructEntry
		*/
		extern "C" __declspec(dllimport) jni::hotspot::VMStructEntry* gHotSpotVMStructs;

		/*
			@brief Iterates over the gHotSpotVMTypes array to find a type entry by name.
			@param type_name The name of the HotSpot type to look for (e.g., "Method", "ConstantPool")
			@return Pointer to the matching VMTypeEntry if found, nullptr otherwise.
			@details
			Walks the gHotSpotVMTypes array linearly until it finds an entry whose
			type_name matches the provided string, or until the null terminator entry is reached.
			@note The returned pointer points directly into the gHotSpotVMTypes array
				  and remains valid for the lifetime of the JVM process.
			@see VMTypeEntry, gHotSpotVMTypes
		*/
		static auto iterate_type_entries(const char* type_name) noexcept
			-> jni::hotspot::VMTypeEntry*
		{
			for (jni::hotspot::VMTypeEntry* entry{ jni::hotspot::gHotSpotVMTypes }; entry->type_name; ++entry)
			{
				if (std::strcmp(entry->type_name, type_name))
				{
					continue;
				}

				return entry;
			}

			return nullptr;
		}

		/*
			@brief Iterates over the gHotSpotVMStructs array to find a field entry by type and field name.
			@param type_name The name of the HotSpot type that owns the field (e.g., "Method", "ConstMethod")
			@param field_name The name of the field to look for (e.g., "_i2i_entry", "_constMethod")
			@return Pointer to the matching VMStructEntry if found, nullptr otherwise.
			@details
			Walks the gHotSpotVMStructs array linearly until it finds an entry whose
			type_name and field_name both match the provided strings,
			or until the null terminator entry is reached.
			@note The returned pointer points directly into the gHotSpotVMStructs array
				  and remains valid for the lifetime of the JVM process.
			@see VMStructEntry, gHotSpotVMStructs
		*/
		static auto iterate_struct_entries(const char* type_mame, const char* field_name) noexcept
			-> jni::hotspot::VMStructEntry*
		{
			for (jni::hotspot::VMStructEntry* entry{ jni::hotspot::gHotSpotVMStructs }; entry->type_name; ++entry)
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

		/*
			@brief Represents a HotSpot internal Symbol object.
			@details
			Symbols are interned strings used throughout the JVM to represent
			class names, method names, field names, and type signatures.
			They are stored in a shared symbol table and reused across the JVM.
			@note The layout of this struct is resolved at runtime via gHotSpotVMStructs,
				  using the offsets of Symbol._length and Symbol._body.
			@see const_method, gHotSpotVMStructs
		*/
		struct symbol
		{
			/*
				@brief Converts this HotSpot Symbol to a std::string_view.
				@return A string_view over the raw character body of the symbol.
						Returns an empty string_view on failure.
				@details
				Reads the symbol length from the _length field and the character
				data from the _body field using offsets retrieved from gHotSpotVMStructs.
				The returned string_view points directly into JVM memory and remains
				valid as long as the symbol is alive in the JVM symbol table.
				@note Do not store the returned string_view beyond the scope of the call
					  without copying it into a std::string first.
			*/
			auto to_string() const
				-> std::string
			{
				static jni::hotspot::VMStructEntry* length_entry{ jni::hotspot::iterate_struct_entries("Symbol", "_length") };
				static jni::hotspot::VMStructEntry* body_entry{ jni::hotspot::iterate_struct_entries("Symbol", "_body") };

				try
				{
					if (!length_entry)
					{
						throw jni::jni_exception{ "Failed to find Symbol._length entry." };
					}

					if (!body_entry)
					{
						throw jni::jni_exception{ "Failed to find Symbol._body entry." };
					}

					const std::uint16_t length{ *(std::uint16_t*)((std::uint8_t*)this + length_entry->offset) };
					const char* body{ (const char*)((std::uint8_t*)this + body_entry->offset) };

					return std::string{ body, length };
				}
				catch (const std::exception& e)
				{
					std::println("{} symbol.to_string() {}", jni::easy_jni_error, e.what());

					return std::string{};
				}
			}
		};

		/*
			@brief Represents a HotSpot internal ConstantPool object.
			@details
			The constant pool holds all constants referenced by a Java class,
			including class names, method names, field names, signatures, and
			literal values. Each entry is indexed and accessible via the base pointer.
			The layout of this struct is resolved at runtime via gHotSpotVMStructs,
			using the size of the ConstantPool type to locate the base of the pool.
			@note Every Java class has exactly one ConstantPool instance, accessible
				  via its ConstMethod through const_method::get_constants().
			@see const_method, symbol, gHotSpotVMStructs
		*/
		struct constant_pool
		{
			/*
				@brief Returns a pointer to the base of the constant pool entries array.
				@return Pointer to the first entry in the constant pool array,
						or nullptr on failure.
				@details
				The constant pool entries are stored immediately after the ConstantPool
				object in memory. The base is computed by adding the size of the
				ConstantPool type (retrieved from gHotSpotVMTypes) to the address of this.
				Each entry in the array is a pointer-sized slot that can hold a symbol*,
				a class reference, or another constant depending on its index.
				@note The returned pointer points directly into JVM memory and remains
					  valid as long as the owning class is loaded in the JVM.
			*/
			auto get_base() const
				-> void**
			{
				static jni::hotspot::VMTypeEntry* entry{ jni::hotspot::iterate_type_entries("ConstantPool") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find ConstantPool entry." };
					}

					return (void**)((std::uint8_t*)this + entry->size);
				}
				catch (const std::exception& e)
				{
					std::println("{} constant_pool.get_base() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}
		};

		/*
			@brief Represents a HotSpot internal ConstMethod object.
			@details
			ConstMethod holds the immutable metadata of a Java method, including
			its name, signature, and a reference to the owning class constant pool.
			It is referenced by every Method object and remains unchanged for the
			lifetime of the method in the JVM.
			The layout of this struct is resolved at runtime via gHotSpotVMStructs,
			using the offsets of ConstMethod._constants, ConstMethod._name_index,
			and ConstMethod._signature_index.
			@note Every hotspot::method holds a pointer to its ConstMethod,
				  accessible via method::get_const_method().
			@see method, constant_pool, symbol, gHotSpotVMStructs
		*/
		struct const_method
		{
			/*
				@brief Returns a pointer to the constant pool of the owning class.
				@return Pointer to the constant_pool of the class that owns this method,
						or nullptr on failure.
				@details
				Reads the _constants field using its offset retrieved from gHotSpotVMStructs.
				The constant pool holds all symbolic references needed to resolve
				the method name, signature, and other class-level constants.
				@note The returned pointer points directly into JVM memory and remains
					  valid as long as the owning class is loaded in the JVM.
				@see constant_pool
			*/
			auto get_constants() const
				-> constant_pool*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("ConstMethod", "_constants") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find ConstMethod._constants entry." };
					}

					return *(constant_pool**)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} const_method.get_constants() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns the symbol representing the name of this method.
				@return Pointer to the symbol containing the method name (e.g., "toString"),
						or nullptr on failure.
				@details
				Reads the _name_index field to obtain the index into the constant pool,
				then retrieves the corresponding symbol from the pool base.
				@note The returned pointer points directly into JVM memory and remains
					  valid as long as the owning class is loaded in the JVM.
				@see symbol, constant_pool
			*/
			auto get_name() const
				-> symbol*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("ConstMethod", "_name_index") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find ConstMethod._name_index entry." };
					}

					const std::uint16_t index{ *(std::uint16_t*)((std::uint8_t*)this + entry->offset) };
					return (symbol*)get_constants()->get_base()[index];
				}
				catch (const std::exception& e)
				{
					std::println("{} const_method.get_name() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns the symbol representing the signature of this method.
				@return Pointer to the symbol containing the method signature (e.g., "(I)Ljava/lang/String;"),
						or nullptr on failure.
				@details
				Reads the _signature_index field to obtain the index into the constant pool,
				then retrieves the corresponding symbol from the pool base.
				@note The returned pointer points directly into JVM memory and remains
					  valid as long as the owning class is loaded in the JVM.
				@see symbol, constant_pool
			*/
			auto get_signature() const
				-> symbol*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("ConstMethod", "_signature_index") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find ConstMethod._signature_index entry." };
					}

					const std::uint16_t index{ *(std::uint16_t*)((std::uint8_t*)this + entry->offset) };
					return (symbol*)get_constants()->get_base()[index];
				}
				catch (const std::exception& e)
				{
					std::println("{} const_method.get_signature() {}", jni::easy_jni_error, e.what());

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
			The layout of this struct is resolved at runtime via gHotSpotVMStructs,
			using the offsets of Method._i2i_entry, Method._from_interpreted_entry,
			Method._access_flags, Method._flags, and Method._constMethod.
			@note A pointer to this struct can be obtained by dereferencing a jmethodID,
				  as jmethodID is internally a pointer to a Method pointer in HotSpot.
			@see const_method, constant_pool, symbol, gHotSpotVMStructs
		*/
		struct method
		{
			/*
				@brief Returns the interpreter-to-interpreter entry point of this method.
				@return Pointer to the i2i entry stub, or nullptr on failure.
				@details
				Reads the _i2i_entry field using its offset retrieved from gHotSpotVMStructs.
				The i2i entry is the native code stub invoked when an interpreted method
				calls another interpreted method. It is used as the hook location target
				in midi2i_hook to intercept method calls at the interpreter level.
				@note The returned pointer points directly into JVM generated code memory.
				@see midi2i_hook, find_hook_location
			*/
			auto get_i2i_entry() const
				-> void*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("Method", "_i2i_entry") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find Method._i2i_entry entry." };
					}

					return *(void**)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_i2i_entry() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns the from-interpreted entry point of this method.
				@return Pointer to the from-interpreted entry stub, or nullptr on failure.
				@details
				Reads the _from_interpreted_entry field using its offset retrieved from gHotSpotVMStructs.
				This entry point is used when the interpreter dispatches a call to this method,
				whether it is interpreted or compiled. It may point to the i2i stub or to
				an adapter stub depending on the current compilation state of the method.
				@note The returned pointer points directly into JVM generated code memory.
			*/
			auto get_from_interpreted_entry() const
				-> void*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("Method", "_from_interpreted_entry") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find Method._from_interpreted_entry entry." };
					}

					return *(void**)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_from_interpreted_entry() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns a pointer to the access flags of this method.
				@return Pointer to the _access_flags field, or nullptr on failure.
				@details
				Reads the _access_flags field using its offset retrieved from gHotSpotVMStructs.
				Access flags encode Java visibility modifiers (public, private, static, etc.)
				as well as JVM-internal flags such as NO_COMPILE, which are used to prevent
				the JIT compiler from compiling hooked methods.
				@note Modifying the returned value directly affects JVM behaviour for this method.
				@see NO_COMPILE
			*/
			auto get_access_flags() const
				-> std::uint32_t*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("Method", "_access_flags") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find Method._access_flags entry." };
					}

					return (uint32_t*)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_access_flags() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns a pointer to the internal method flags of this method.
				@return Pointer to the _flags field, or nullptr on failure.
				@details
				Reads the _flags field using its offset retrieved from gHotSpotVMStructs.
				These flags encode HotSpot-internal method properties such as _dont_inline,
				which prevents the JIT compiler from inlining the method at call sites.
				@note Modifying the returned value directly affects JVM behaviour for this method.
				@see set_dont_inline
			*/
			auto get_flags() const
				-> std::uint16_t*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("Method", "_flags") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find Method._flags entry." };
					}

					return (std::uint16_t*)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_flags() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns a pointer to the ConstMethod of this method.
				@return Pointer to the const_method holding the immutable metadata,
						or nullptr on failure.
				@details
				Reads the _constMethod field using its offset retrieved from gHotSpotVMStructs.
				The ConstMethod provides access to the method name, signature, and
				the owning class constant pool.
				@note The returned pointer points directly into JVM memory and remains
					  valid as long as the owning class is loaded in the JVM.
				@see const_method
			*/
			auto get_const_method() const
				-> const_method*
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("Method", "_constMethod") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find Method._constMethod entry." };
					}

					return *(const_method**)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_const_method() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns the name of this method as a string_view.
				@return A string_view over the method name symbol (e.g., "toString"),
						or an empty string_view on failure.
				@details
				Retrieves the ConstMethod via get_const_method(), then reads the name
				symbol via const_method::get_name() and converts it to a string_view.
				@note The returned string_view points directly into JVM symbol table memory.
					  Do not store it beyond the scope of the call without copying it first.
				@see const_method, symbol
			*/
			auto get_name() const
				-> std::string
			{
				const const_method* const_method{ this->get_const_method() };

				try
				{
					if (!const_method)
					{
						throw jni::jni_exception{ "ConstMethod is nullptr." };
					}

					const symbol* symbol{ const_method->get_name() };
					if (!symbol)
					{
						throw jni::jni_exception{ "Symbol is nullptr." };
					}

					return symbol->to_string();
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_name() {}", jni::easy_jni_error, e.what());

					return std::string{};
				}
			}

			/*
				@brief Returns the signature of this method as a string_view.
				@return A string_view over the method signature symbol
						(e.g., "(I)Ljava/lang/String;"), or an empty string_view on failure.
				@details
				Retrieves the ConstMethod via get_const_method(), then reads the signature
				symbol via const_method::get_signature() and converts it to a string_view.
				@note The returned string_view points directly into JVM symbol table memory.
					  Do not store it beyond the scope of the call without copying it first.
				@see const_method, symbol
			*/
			auto get_signature() const
				-> std::string
			{
				const const_method* const_method{ this->get_const_method() };

				try
				{
					if (!const_method)
					{
						throw jni::jni_exception{ "ConstMethod is nullptr." };
					}

					const symbol* symbol{ const_method->get_signature() };
					if (!symbol)
					{
						throw jni::jni_exception{ "Symbol is nullptr." };
					}

					return symbol->to_string();
				}
				catch (const std::exception& e)
				{
					std::println("{} method.get_signature() {}", jni::easy_jni_error, e.what());

					return std::string{};
				}
			}
		};

		/*
			@brief Represents the possible execution states of a HotSpot JavaThread.
			@details
			Each value corresponds to a distinct state in the HotSpot thread state machine,
			describing where the thread is currently executing and whether it is in a
			transition between states. Transition states (suffixed with _trans) indicate
			that the thread is in the process of moving from one state to another.
			The thread state is stored in JavaThread._thread_state and can be read or
			written via java_thread::get_thread_state() and java_thread::set_thread_state().
			@note In the context of hooking, _thread_in_Java is the only state in which
				  it is safe to intercept and interact with a Java method call.
				  After executing a detour, the thread state must be restored to
				  _thread_in_Java to ensure correct JVM behaviour.
			@see java_thread, common_detour
		*/
		enum class java_thread_state : std::int8_t
		{
			/*
				@brief The thread has not been initialized yet.
			*/
			_thread_uninitialized = 0,

			/*
				@brief The thread has been created but has not yet started executing Java code.
			*/
			_thread_new = 2,

			/*
				@brief The thread is transitioning from the new state to another state.
			*/
			_thread_new_trans = 3,

			/*
				@brief The thread is currently executing native (non-Java) code.
			*/
			_thread_in_native = 4,

			/*
				@brief The thread is transitioning from native code back into the JVM.
			*/
			_thread_in_native_trans = 5,

			/*
				@brief The thread is currently executing inside the JVM runtime (e.g. GC, class loading).
			*/
			_thread_in_vm = 6,

			/*
				@brief The thread is transitioning from JVM runtime execution to another state.
			*/
			_thread_in_vm_trans = 7,

			/*
				@brief The thread is currently executing Java bytecode in the interpreter or compiled code.
				@note This is the only state in which method hooks are safely intercepted.
			*/
			_thread_in_Java = 8,

			/*
				@brief The thread is transitioning from Java execution to another state.
			*/
			_thread_in_Java_trans = 9,

			/*
				@brief The thread is blocked, waiting on a monitor or lock.
			*/
			_thread_blocked = 10,

			/*
				@brief The thread is transitioning out of the blocked state.
			*/
			_thread_blocked_trans = 11,

			/*
				@brief Sentinel value representing the total number of defined thread states.
			*/
			_thread_max_state = 12
		};

		/*
			@brief Represents a HotSpot internal JavaThread object.
			@details
			JavaThread is the JVM's internal representation of a Java thread.
			It holds all runtime state needed to manage thread execution, including
			the JNI environment, the current thread state, and suspension flags.
			A pointer to the current JavaThread is always held in the r15 register
			on x64 HotSpot, making it directly accessible from low-level hook stubs.
			The layout of this struct is resolved at runtime via gHotSpotVMStructs,
			using the offsets of JavaThread._jni_environment, JavaThread._thread_state,
			and JavaThread._suspend_flags.
			@note In the context of hooking, the JavaThread pointer is passed as the
				  second argument to every detour function via the common_detour stub.
			@see java_thread_state, frame, common_detour, gHotSpotVMStructs
		*/
		struct java_thread
		{
			/*
				@brief Returns the JNIEnv associated with this thread.
				@return Pointer to the JNIEnv for this thread, or nullptr on failure.
				@details
				_jni_environment is not exported in gHotSpotVMStructs on this JDK version,
				so its address cannot be retrieved directly via iterate_struct_entries.
				However, _jni_environment is always laid out immediately after _anchor in the
				JavaThread memory layout, separated by exactly sizeof(JavaFrameAnchor) = 32 bytes.
				Its address is therefore computed as: &_anchor + sizeof(JavaFrameAnchor).
				_anchor itself is exported in gHotSpotVMStructs and used as the stable anchor point
				for this calculation, making it resilient to future shifts in the JavaThread layout
				as long as the relative ordering of _anchor and _jni_environment is preserved.
				@note The returned JNIEnv is only valid while this thread is attached to the JVM.
					  It should not be used after the thread has detached or exited.
				@note sizeof(JavaFrameAnchor) = 32 bytes on x64 HotSpot:
					  _last_Java_sp (8) + _last_Java_pc (8) + _last_Java_fp (8) + padding (8) = 32.
			*/
			auto get_env() const
				-> JNIEnv*
			{
				// _anchor is used as a stable reference point since _jni_environment is not
				// exported in gHotSpotVMStructs on this JDK version.
				static jni::hotspot::VMStructEntry* anchor_entry{ jni::hotspot::iterate_struct_entries("JavaThread", "_anchor") };

				try
				{
					if (!anchor_entry)
					{
						throw jni::jni_exception{ "Failed to find JavaThread._anchor entry." };
					}

					// _jni_environment immediately follows _anchor in the JavaThread layout.
					// The offset between them is exactly sizeof(JavaFrameAnchor) = 32 bytes,
					// which is stable across all x64 HotSpot builds.
					static const constexpr std::ptrdiff_t SIZEOF_JAVA_FRAME_ANCHOR{ 32 };
					return (JNIEnv*)((std::uint8_t*)this + anchor_entry->offset + SIZEOF_JAVA_FRAME_ANCHOR);
				}
				catch (const std::exception& e)
				{
					std::println("{} java_thread.get_env() {}", jni::easy_jni_error, e.what());

					return nullptr;
				}
			}

			/*
				@brief Returns the current execution state of this thread.
				@return The current java_thread_state value of this thread.
						Returns _thread_uninitialized on failure.
				@details
				Reads the _thread_state field using its offset retrieved from gHotSpotVMStructs.
				The thread state describes where the thread is currently executing
				(e.g. in Java bytecode, in native code, in the JVM runtime, blocked, etc.).
				@note This value may change at any time as the thread transitions between states.
				@see java_thread_state
			*/
			auto get_thread_state() const
				-> jni::hotspot::java_thread_state
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find JavaThread._thread_state entry." };
					}

					return *(java_thread_state*)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} java_thread.get_thread_state() {}", jni::easy_jni_error, e.what());

					return jni::hotspot::java_thread_state::_thread_uninitialized;
				}
			}

			/*
				@brief Sets the execution state of this thread.
				@param state The java_thread_state value to assign to this thread.
				@details
				Writes directly to the _thread_state field using its offset retrieved
				from gHotSpotVMStructs. This is used in common_detour to restore the
				thread state to _thread_in_Java after executing a detour, ensuring
				the JVM continues to operate correctly after the hook returns.
				@warning Incorrect use of this function can corrupt the JVM thread state
						 machine and lead to undefined behaviour or JVM crashes.
				@see java_thread_state, common_detour
			*/
			auto set_thread_state(const jni::hotspot::java_thread_state state) const
				-> void
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("JavaThread", "_thread_state") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find JavaThread._thread_state entry." };
					}

					*(jni::hotspot::java_thread_state*)((std::uint8_t*)this + entry->offset) = state;
				}
				catch (const std::exception& e)
				{
					std::println("{} java_thread.set_thread_state() {}", jni::easy_jni_error, e.what());
				}
			}

			/*
				@brief Returns the current suspension flags of this thread.
				@return The current value of the _suspend_flags field, or 0 on failure.
				@details
				Reads the _suspend_flags field using its offset retrieved from gHotSpotVMStructs.
				Suspension flags are used by the JVM to coordinate safe point operations,
				thread suspension, and asynchronous exceptions across threads.
				@note This value may change at any time as the JVM modifies it internally.
			*/
			auto get_suspend_flags() const
				-> std::uint32_t
			{
				static jni::hotspot::VMStructEntry* entry{ jni::hotspot::iterate_struct_entries("JavaThread", "_suspend_flags") };

				try
				{
					if (!entry)
					{
						throw jni::jni_exception{ "Failed to find JavaThread._suspend_flags entry." };
					}

					return *(std::uint32_t*)((std::uint8_t*)this + entry->offset);
				}
				catch (const std::exception& e)
				{
					std::println("{} java_thread.get_suspend_flags() {}", jni::easy_jni_error, e.what());

					return 0;
				}
			}
		};

		/*
			@brief Checks whether a memory region matches a given byte pattern.
			@param address Pointer to the start of the memory region to check.
			@param pattern Pointer to the byte pattern to match against.
			@param size    Number of bytes to compare.
			@return true if the memory region matches the pattern, false otherwise.
			@details
			Compares each byte of the memory region at address against the corresponding
			byte in pattern. A pattern byte of 0x00 acts as a wildcard and always matches,
			allowing partial patterns to be used when some bytes are unknown or variable
			(e.g. immediate values in instructions).
			@note This function does not perform any bounds checking on the provided
				  pointers. The caller is responsible for ensuring that both address
				  and pattern point to valid readable memory of at least size bytes.
			@see scan, find_hook_location
		*/
		inline static auto match_pattern(const std::uint8_t* address, const std::uint8_t* pattern, const std::size_t size)
			-> bool
		{
			for (std::size_t i{ 0 }; i < size; ++i)
			{
				if (pattern[i] == 0x00)
				{
					continue;
				}

				if (address[i] != pattern[i])
				{
					return false;
				}
			}

			return true;
		}

		/*
			@brief Scans a memory region for the first occurrence of a byte pattern.
			@param start   Pointer to the beginning of the memory region to scan.
			@param range   Number of bytes to scan from start.
			@param pattern Pointer to the byte pattern to search for.
			@param size    Number of bytes in the pattern.
			@return Pointer to the first matching location in the scanned region,
					or nullptr if no match was found.
			@details
			Iterates over each byte in the memory region starting at start and checks
			whether the pattern matches at each position using match_pattern().
			A pattern byte of 0x00 acts as a wildcard and always matches, allowing
			partial patterns to be used when some bytes are unknown or variable
			(e.g. immediate values or offsets embedded in instructions).
			@note This function does not perform any bounds checking on the provided
				  pointers. The caller is responsible for ensuring that start points
				  to valid readable memory of at least range bytes, and that pattern
				  points to valid readable memory of at least size bytes.
			@see match_pattern, find_hook_location
		*/
		inline static auto scan(
			const std::uint8_t* start,
			const std::size_t range,
			const std::uint8_t* pattern,
			const std::size_t size
		)
			-> std::uint8_t*
		{
			for (std::size_t i{ 0 }; i < range; ++i)
			{
				if (jni::hotspot::match_pattern(start + i, pattern, size))
				{
					return (std::uint8_t*)(start + i);
				}
			}

			return nullptr;
		}

		/*
			@brief Determines the safe scannable size of a JVM generated code stub.
			@param start Pointer to the beginning of the stub to measure.
			@return The number of bytes that can be safely scanned from start,
					capped at 0x2000 bytes.
			@details
			Uses VirtualQuery to retrieve the memory region information of the page
			containing start, then computes how many bytes remain in that region
			from start to the end of the page. The result is capped at 0x2000 bytes
			to avoid scanning excessively large regions and reduce the risk of
			reading into unrelated or unmapped memory.
			This is used by find_hook_location() to determine the scan range when
			searching for the hook pattern inside a JVM interpreter stub.
			@note The returned size is a conservative upper bound. The actual stub
				  may be smaller than the returned value.
			@see scan, find_hook_location
		*/
		static auto find_stub_size(const std::uint8_t* start)
			-> std::size_t
		{
			// Query the memory region containing start to retrieve its base address and size.
			MEMORY_BASIC_INFORMATION mbi{};
			VirtualQuery(start, &mbi, sizeof(mbi));

			// Compute the number of bytes remaining in this memory region from start
			// to the end of the page, to avoid scanning past the region boundary.
			const std::size_t region_size{ (std::size_t)((std::uint8_t*)mbi.BaseAddress + mbi.RegionSize - start) };

			// Cap the scan size to 0x2000 bytes to avoid scanning excessively large
			// regions and reduce the risk of reading into unrelated memory.
			return min(region_size, (std::size_t)0x2000);
		}

		/*
			@brief Byte offset used to retrieve the local variables pointer from the interpreter frame.
			@details
			In HotSpot's interpreter, the base pointer (rbp) of an interpreter frame holds a reference
			to the current frame. The local variables array of the executing method is located at a
			fixed negative offset from rbp, accessible via the r14 register on x64 HotSpot.
			This offset is determined at runtime by find_hook_location(), which scans the i2i stub
			for the instruction that loads r14 from rbp (mov r14, QWORD PTR [rbp + ??]) and extracts
			the immediate offset from it.
			The default value of -56 corresponds to the offset observed on the latest versions of
			the JVM at the time of writing, and is used as a fallback if the pattern scan fails.
			@note This value is updated automatically by find_hook_location() on first hook installation
				  and shared across all subsequent frame::get_locals() calls.
			@see find_hook_location, frame::get_locals
		*/
		inline constinit std::int8_t locals_offset{ -56 };

		/*
			@brief Locates the optimal injection point within a HotSpot i2i interpreter stub.
			@param i2i_entry Pointer to the beginning of the i2i interpreter stub to scan.
			@return Pointer to the injection point within the stub where the hook can be safely
					installed, or nullptr if the pattern could not be found.
			@details
			Scans the i2i stub for a known sequence of instructions that appears at a stable
			location across JVM builds, immediately before the interpreter begins executing
			the actual Java bytecode. This location is chosen because all method arguments
			are fully set up and accessible via the local variables array at this point.
			Additionally scans backwards from the hook location to find and cache the
			locals_offset value used by frame::get_locals() to retrieve method arguments.
			@note The hook is installed 8 bytes before the end of the matched pattern,
				  at the position of the mov BYTE PTR [r15+??], 0x0 instruction, which
				  is the last setup instruction before bytecode dispatch begins.
			@see scan, match_pattern, find_stub_size, locals_offset, midi2i_hook
		*/
		static auto find_hook_location(const void* i2i_entry)
			-> void*
		{
			// Byte pattern matching the sequence of instructions found at the end of the
			// interpreter frame setup, just before bytecode dispatch begins.
			// 0x00 bytes act as wildcards for variable immediates (offsets, addresses).
			const constexpr std::uint8_t pattern[]
			{
				// mov [rsp+??], eax
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,

				// mov [rsp+??], eax
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,

				// mov [rsp+??], eax
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,

				// mov [rsp+??], eax
				0x89, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,

				// mov BYTE PTR [r15+??], 0x0
				0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00
			};

			// Byte pattern matching the instruction that loads the local variables pointer
			// into r14 from rbp, used to determine the locals_offset value at runtime.
			const constexpr std::uint8_t locals_pattern[]
			{
				// mov r14, QWORD PTR [rbp + ??] ; ret
				0x4C, 0x8B, 0x75, 0x00, 0xC3
			};

			// Cast i2i_entry to a byte pointer to allow pointer arithmetic during scanning.
			const std::uint8_t* current{ (std::uint8_t*)i2i_entry };

			// Determine the safe scan range for this stub using VirtualQuery.
			const std::size_t scan_size{ jni::hotspot::find_stub_size((std::uint8_t*)i2i_entry) };

			// Scan the stub for the main hook pattern to locate the injection point.
			std::uint8_t* hook_location{ jni::hotspot::scan(current, scan_size, pattern, sizeof(pattern)) };

			try
			{
				if (!hook_location)
				{
					throw jni::jni_exception{ "Failed to find hook pattern." };
				}

				// Scan backwards from the hook location towards the start of the stub
				// to find the locals_pattern and extract the locals_offset value.
				for (std::uint8_t* p{ hook_location }; p > (std::uint8_t*)i2i_entry; --p)
				{
					if (jni::hotspot::match_pattern(p, locals_pattern, sizeof(locals_pattern)))
					{
						// Extract the signed byte offset from the mov r14, [rbp+??] instruction
						// (third byte of the instruction encoding) and cache it in locals_offset.
						jni::hotspot::locals_offset = (std::int8_t)p[3];
						break;
					}
				}

				// The actual injection point is 8 bytes before the end of the matched pattern,
				// at the position of the mov BYTE PTR [r15+??], 0x0 instruction.
				// sizeof(pattern) - 8 gives the offset of that instruction within the pattern.
				return hook_location + sizeof(pattern) - 8;
			}
			catch (const std::exception& e)
			{
				std::println("{} find_hook_location() {}", jni::easy_jni_error, e.what());

				return nullptr;
			}
		}

		template<typename type>
		using argument_return_t = std::conditional_t<std::is_base_of_v<jni::object, type>, std::unique_ptr<type>, type>;

		/*
			@brief Represents a HotSpot interpreter frame on the call stack.
			@details
			In HotSpot's interpreter, each method invocation creates a frame on the
			native call stack. The frame holds the execution context of the currently
			executing Java method, including a pointer to the Method object and a
			pointer to the local variables array containing the method arguments.
			On x64 HotSpot, the base pointer (rbp) of the interpreter always points
			to the current frame, which is why it is passed directly as the first
			argument to every detour function in common_detour.
			The Method pointer is located at a fixed negative offset of -24 bytes
			from rbp, and the local variables pointer is located at locals_offset
			bytes from rbp, both of which are stable across JVM builds.
			@note A pointer to the current frame is passed as the first argument
				  to every detour function registered via jni::hook().
			@see java_thread, locals_offset, common_detour, midi2i_hook
		*/
		struct frame
		{
		public:
			/*
				@brief Returns a pointer to the Method object of the currently executing method.
				@return Pointer to the hotspot::method of the intercepted call, or nullptr if
						the frame pointer is invalid.
				@details
				On x64 HotSpot, the Method pointer is stored at a fixed offset of -24 bytes
				from rbp (the frame base pointer). The value at that address is itself a pointer
				to the Method object, so a double dereference is required to obtain it.
				This offset is stable across all HotSpot builds on x64 and does not need
				to be resolved via gHotSpotVMStructs.
				@note The returned pointer points directly into JVM memory and remains valid
					  for the duration of the intercepted method call only.
				@see hotspot::method, common_detour
			*/
			auto get_method() const noexcept
				-> method*
			{
				// The Method pointer is stored 24 bytes before the frame base pointer (rbp).
				// Casting this to a uint8_t* allows the byte-level subtraction,
				// then we dereference twice: once to get the Method**, once to get the Method*.
				return *(method**)((std::uint8_t*)this - 24);
			}

			/*
				@brief Returns a pointer to the local variables array of the currently executing method.
				@return Pointer to the base of the local variables array, or nullptr if
						the frame pointer is invalid.
				@details
				On x64 HotSpot, the local variables pointer is stored at locals_offset bytes
				from rbp (the frame base pointer). This offset is determined at runtime by
				find_hook_location() by scanning the i2i stub for the instruction that loads
				r14 from rbp, and defaults to -56 if the scan fails.
				The local variables array holds all method arguments and local variables,
				laid out contiguously in memory. Arguments are accessed at negative indices
				relative to the base pointer, with index 0 being the first argument.
				@note The returned pointer points directly into JVM stack memory and remains
					  valid for the duration of the intercepted method call only.
				@see locals_offset, find_hook_location, get_arguments
			*/
			auto get_locals() const noexcept
				-> void**
			{
				// locals_offset is a signed byte offset from rbp to the local variables pointer.
				// Casting this to a uint8_t* allows the byte-level offset to be applied,
				// then we dereference twice: once to get the void***, once to get the void**.
				return *(void***)((std::uint8_t*)this + jni::hotspot::locals_offset);
			}

			/*
				@brief Retrieves all method arguments as a typed tuple.
				@tparam types The C++ types of the method arguments in declaration order.
							  Primitive types are returned by value, JNI objects are returned
							  as std::unique_ptr<type>.
				@return A std::tuple containing all arguments converted to their C++ types.
						Default-constructed values are returned for arguments that could not
						be retrieved.
				@details
				Calls get_argument() for each type in the parameter pack, passing an
				incrementing index to access each slot in the local variables array.
				The index starts at 0 for the first argument and increments by 1 for
				each subsequent argument, regardless of the argument type.
				@note For instance methods, index 0 holds the implicit `this` reference.
					  For static methods, index 0 holds the first explicit argument.
				@see get_argument, get_locals
			*/
			template<typename... types>
			auto get_arguments() const noexcept
				-> std::tuple<argument_return_t<types>...>
			{
				// Initialize a counter to track the current argument index.
				std::int32_t index{ 0 };

				// Expand the parameter pack, calling get_argument<T>(index++) for each type.
				// The post-increment ensures each argument receives a unique sequential index.
				return std::tuple<argument_return_t<types>...>
				{
					this->get_argument<types>(index++)...
				};
			}

		private:
			/*
				@brief Retrieves a single method argument at the given index from the local variables array.
				@tparam type The C++ type to interpret the argument as. Primitive types are returned
							 by value, JNI objects are returned as std::unique_ptr<type>.
				@param index Zero-based index of the argument in the local variables array.
				@return The argument value converted to the requested C++ type, or a
						default-constructed value if the argument could not be retrieved.
				@details
				Reads the raw pointer-sized slot at locals[-index] from the local variables array.
				The slot is then interpreted differently depending on the requested type:
				- For std::string: the raw pointer is treated as a compressed OOP reference,
				  a local JNI reference is created from it, and the string characters are
				  extracted via GetStringUTFChars.
				- For JNI objects derived from jni::object: the raw pointer is treated as a
				  compressed OOP reference, a local JNI reference is created from it, and the
				  result is wrapped in a std::unique_ptr<type>.
				- For primitive types up to pointer size: the raw value is reinterpreted
				  directly via memcpy to avoid strict aliasing violations.
				@note Arguments are stored at negative indices in the local variables array,
					  so locals[-index] is used to access them. This is an implementation
					  detail of the HotSpot interpreter's frame layout.
				@see get_locals, get_arguments
			*/
			template<typename type>
			auto get_argument(const std::int32_t index) const noexcept
				-> std::conditional_t<std::is_base_of_v<jni::object, type>, std::unique_ptr<type>, type>
			{
				using return_type = std::conditional_t<std::is_base_of_v<jni::object, type>, std::unique_ptr<type>, type>;

				// Retrieve the base of the local variables array from the frame.
				void** locals{ this->get_locals() };
				if (!locals)
				{
					return return_type{};
				}

				// Arguments are stored at negative indices relative to the locals base pointer.
				// locals[-index] gives the raw pointer-sized slot for the requested argument.
				void* raw{ locals[-index] };

				if constexpr (std::is_same_v<type, std::string>)
				{
					if (!raw)
					{
						return std::string{};
					}

					// The raw value is a compressed OOP (ordinary object pointer) to a Java String.
					// Taking its address and casting to jobject* gives us a pointer to the OOP slot,
					// which NewLocalRef can resolve into a proper JNI local reference.
					jobject obj = jni::get_env()->NewLocalRef(reinterpret_cast<jobject>(&raw));
					jstring str = reinterpret_cast<jstring>(obj);

					// Extract the UTF-8 characters from the JNI string reference,
					// copy them into a std::string, then release the JNI resources.
					const char* chars = jni::get_env()->GetStringUTFChars(str, nullptr);
					std::string result{ chars };
					jni::get_env()->ReleaseStringUTFChars(str, chars);
					jni::get_env()->DeleteLocalRef(obj);

					return result;
				}
				else if constexpr (std::is_base_of_v<jni::object, type>)
				{
					if (!raw)
					{
						return return_type{};
					}

					// The raw value is a compressed OOP to a Java object.
					// Taking its address and casting to jobject* gives us a pointer to the OOP slot,
					// which NewLocalRef can resolve into a proper JNI local reference.
					jobject obj{ jni::get_env()->NewLocalRef(reinterpret_cast<jobject>(&raw)) };

					// Wrap the resolved JNI reference in a unique_ptr of the requested type.
					return return_type{ std::make_unique<type>(obj) };
				}
				else if constexpr (sizeof(type) <= sizeof(void*))
				{
					// For primitive types that fit within a pointer-sized slot,
					// reinterpret the raw bits directly via memcpy to avoid
					// strict aliasing violations and correctly handle all primitive sizes.
					type result{};
					std::memcpy(&result, &raw, sizeof(type));
					return result;
				}
				else
				{
					// Types larger than a pointer cannot be stored in a single local variable slot.
					// Return a default-constructed value as a safe fallback.
					return return_type{};
				}
			}
		};

		/*
			@brief Allocates a block of executable memory within a 32-bit relative jump range
				   of a given address.
			@param nearby_addr The address near which the memory must be allocated.
							   The allocated block will be within +/- 2GB of this address.
			@param size        The number of bytes to allocate.
			@param protect     The memory protection flags to apply to the allocated block
							   (e.g. PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE).
			@return Pointer to the allocated memory block on success, or nullptr if no
					suitable block could be found within the search range.
			@details
			Iterates outward from nearby_addr in both directions (above and below) in steps
			of 65536 bytes (one memory page granularity unit on Windows), attempting to
			allocate a committed memory block at each candidate address via VirtualAlloc.
			The search range is limited to +/- 0x7FFFFFFF bytes (the maximum reach of a
			signed 32-bit relative offset) to ensure that a 5-byte relative JMP instruction
			(0xE9 + 4-byte signed offset) can reach the allocated stub from nearby_addr.
			This constraint is critical for the midi2i_hook, which patches the target
			with a 5-byte relative JMP to the allocated stub. If the stub were allocated
			further than 2GB away, the relative offset would overflow a signed 32-bit integer
			and the JMP would branch to an incorrect address, causing a crash.
			@note 65536 is used as the step size because it is the minimum allocation
				  granularity on Windows (SYSTEM_INFO.dwAllocationGranularity), meaning
				  VirtualAlloc will silently round down any address not aligned to this
				  boundary, potentially causing repeated failed allocations if a smaller
				  step were used.
			@see midi2i_hook, VirtualAlloc, VirtualFree
		*/
		static auto allocate_nearby_memory(std::uint8_t* nearby_addr, const std::size_t size, const DWORD protect) noexcept
			-> std::uint8_t*
		{
			// Iterate outward from nearby_addr in steps of 65536 bytes (Windows allocation granularity),
			// up to a maximum offset of 0x7FFFFFFF bytes to stay within 32-bit relative JMP range.
			for (std::int64_t i{ 65536 }; i < 0x7FFFFFFF; i += 65536)
			{
				// First attempt: try to allocate above nearby_addr at nearby_addr + i.
				// MEM_COMMIT | MEM_RESERVE both reserves and commits the pages in one call,
				// avoiding a separate VirtualAlloc(MEM_RESERVE) + VirtualAlloc(MEM_COMMIT) sequence.
				std::uint8_t* allocated{ (std::uint8_t*)VirtualAlloc(
					nearby_addr + i, size, MEM_COMMIT | MEM_RESERVE, protect
				) };

				// If the allocation above succeeded, return the allocated address immediately.
				if (allocated)
				{
					return allocated;
				}

				// Second attempt: try to allocate below nearby_addr at nearby_addr - i.
				// This handles cases where the address space above nearby_addr is exhausted
				// or already reserved by other modules or the JVM itself.
				allocated = (std::uint8_t*)VirtualAlloc(
					nearby_addr - i, size, MEM_COMMIT | MEM_RESERVE, protect
				);

				// If the allocation below succeeded, return the allocated address immediately.
				if (allocated)
				{
					return allocated;
				}

				// Both attempts failed at this offset, continue to the next step outward.
			}

			// No suitable block was found within the entire +/- 2GB search range.
			return nullptr;
		}

		using detour_t = void(*)(frame*, java_thread*, bool*);

		/*
			@brief Installs a low-level hook on a HotSpot interpreter-to-interpreter (i2i) stub.
			@details
			midi2i_hook patches the i2i interpreter stub of a Java method at the injection
			point found by find_hook_location() with a 5-byte relative JMP instruction that
			redirects execution to an allocated trampoline stub. The trampoline saves all
			volatile registers, sets up the arguments for the detour function (frame*,
			java_thread*, bool*), calls the detour, and then either returns the custom
			value set by the detour or resumes normal interpreter execution depending on
			whether the detour set the cancel flag.

			The hook works at the interpreter level, meaning it only fires when the method
			is being executed by the HotSpot interpreter. JIT compilation of hooked methods
			is explicitly disabled via NO_COMPILE and _dont_inline flags to ensure the hook
			always fires regardless of how many times the method has been called.

			The trampoline stub is allocated in memory within 32-bit relative JMP range of
			the target via allocate_nearby_memory() to ensure the 5-byte relative JMP patch
			can reach it. The stub is laid out as follows:
				[original 8 bytes] [assembly trampoline]
			The original bytes are preserved at the start of the stub so that the destructor
			can restore them when the hook is removed.

			@note Only one trampoline is allocated per unique i2i entry point, even if
				  multiple methods share the same stub. Method dispatch is handled by
				  common_detour, which iterates over hooked_methods to find the correct
				  detour for the currently executing method.
			@see find_hook_location, allocate_nearby_memory, common_detour, midi2i_hook::~midi2i_hook
		*/
		class midi2i_hook final
		{
		public:
			/*
				@brief Installs the hook on the given target address.
				@param target  Pointer to the injection point within the i2i stub,
							   as returned by find_hook_location().
				@param detour  The C++ function to call when the hook fires.
							   Must match the detour_t signature: void(*)(frame*, java_thread*, bool*).
				@details
				Allocates a trampoline stub near the target, patches the assembly to set up
				the correct JMP back to the original code and the correct detour address,
				copies the original bytes followed by the trampoline assembly into the stub,
				makes the stub executable, then patches the target with a 5-byte relative JMP
				to the stub. Sets error to false only if all steps succeed.
			*/
			midi2i_hook(std::uint8_t* target, jni::hotspot::detour_t detour)
				: target{ target }
				, allocated{ nullptr }
				, error{ true }
			{
				// Size of the region patched at the target with the relative JMP instruction.
				// A relative JMP (0xE9) requires exactly 5 bytes: 1 opcode + 4 byte offset.
				// We patch 8 bytes to overwrite the full mov BYTE PTR [r15+??], 0x0 instruction.
				const constexpr std::int32_t HOOK_SIZE{ 8 };

				// Size of the relative JMP instruction patched at the target.
				const constexpr std::int32_t JMP_SIZE{ 5 };

				// Byte offset of the JE instruction within the assembly trampoline,
				// used to patch the conditional jump destination after allocation.
				const constexpr std::int32_t JE_OFFSET{ 0x3d };

				// Size of the JE instruction in bytes (0F 84 + 4 byte signed offset).
				const constexpr std::int32_t JE_SIZE{ 6 };

				// Byte offset of the detour function pointer slot within the assembly trampoline,
				// used to write the address of the C++ detour function into the stub.
				const constexpr std::int32_t DETOUR_ADDRESS_OFFSET{ 0x56 };

				// The trampoline assembly stub. See the block comment above the class for
				// a full annotated breakdown of each instruction and its purpose.
				std::uint8_t assembly[]
				{
					0x50,                                           // push rax         ; save rax (will hold the custom return value)
					0x51,                                           // push rcx         ; save rcx (first argument register)
					0x52,                                           // push rdx         ; save rdx (second argument register)
					0x41, 0x50,                                     // push r8          ; save r8  (third argument register)
					0x41, 0x51,                                     // push r9          ; save r9  (fourth argument register)
					0x41, 0x52,                                     // push r10         ; save r10 (volatile caller-saved register)
					0x41, 0x53,                                     // push r11         ; save r11 (volatile caller-saved register)
					0x55,                                           // push rbp         ; save rbp (current frame base pointer)
					0x6A, 0x00,                                     // push 0x0         ; push cancel flag (bool* = false by default)

					0x48, 0x89, 0xE9,                               // mov rcx, rbp     ; first argument  
																	//					; -> frame* (rbp = current interpreter frame)
					0x4C, 0x89, 0xFA,                               // mov rdx, r15     ; second argument 
																	//					; -> java_thread* (r15 = current JavaThread*)
					0x4C, 0x8D, 0x04, 0x24,                         // lea r8,  [rsp]   ; third argument  
																	//					; -> bool* cancel (points to the 0x0 we pushed)
					
					0x48, 0x89, 0xE5,                               // mov rbp, rsp     ; save rsp into rbp for 
																	//					; stack restoration after the call
					0x48, 0x83, 0xE4, 0xF0,                         // and rsp, -16     ; align rsp to 16 bytes as required 
																	//					; by the Windows x64 ABI
					0x48, 0x83, 0xEC, 0x20,                         // sub rsp, 0x20    ; allocate 32 bytes of shadow space 
																	//					; as required by the Windows x64 ABI
					
					0xFF, 0x15, 0x2D, 0x00, 0x00, 0x00,             // call [rip+0x2D]  ; call the C++ detour function via 
																	//					; RIP-relative indirect call
					
					0x48, 0x89, 0xEC,                               // mov rsp, rbp     ; restore rsp from rbp 
																	//					; (undo alignment and shadow space)
					
					0x58,                                           // pop rax          ; rax = cancel flag value 
																	//					; (0x0 = do not cancel, non-zero = cancel)
					0x48, 0x83, 0xF8, 0x00,                         // cmp rax, 0x0     ; check if the detour requested cancellation
					
					0x5D,                                           // pop rbp          ; restore rbp (frame base pointer)
					0x41, 0x5B,                                     // pop r11          ; restore r11
					0x41, 0x5A,                                     // pop r10          ; restore r10
					0x41, 0x59,                                     // pop r9           ; restore r9
					0x41, 0x58,                                     // pop r8           ; restore r8
					0x5A,                                           // pop rdx          ; restore rdx
					0x59,                                           // pop rcx          ; restore rcx
					0x58,                                           // pop rax          ; restore rax
					
					0x0F, 0x84, 0x00, 0x00, 0x00, 0x00,             // je 0x????????    ; if cancel flag was set -> jump to the 
																	//					; return path (offset patched at runtime)
					
					// cancel path: the detour requested early return, rax holds the custom return value
					0x66, 0x48, 0x0F, 0x6E, 0xC0,                   // movq xmm0, rax   ; move rax into xmm0 for floating point 
																	//					; return value compatibility
					0x48, 0x8B, 0x5D, 0xF8,                         // mov rbx, [rbp-8] ; restore rbx from the interpreter frame
					0x48, 0x89, 0xEC,                               // mov rsp, rbp     ; restore rsp to the interpreter frame base
					0x5D,                                           // pop rbp          ; restore rbp (caller frame base pointer)
					0x5E,                                           // pop rsi          ; restore rsi (interpreter state pointer)
					0x48, 0x89, 0xDC,                               // mov rsp, rbx     ; restore rsp to the caller stack pointer
					0xFF, 0xE6,                                     // jmp rsi          ; jump to the return address saved in rsi
					
					// data slot: 8-byte address of the C++ detour function, written at construction time
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // .quad 0x0        ; detour function pointer (patched at runtime)
				};

				// Allocate a block of memory near the target within 32-bit relative JMP range,
				// large enough to hold both the saved original bytes and the trampoline assembly.
				// PAGE_EXECUTE_READWRITE is required at this stage to allow writing the stub contents.
				this->allocated = allocate_nearby_memory(target, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READWRITE);

				try
				{
					if (!this->allocated)
					{
						throw jni::jni_exception{ "Failed to allocate memory for hook." };
					}
				}
				catch (const std::exception& e)
				{
					std::println("{} midi2i_hook::midi2i_hook {}", jni::easy_jni_error, e.what());

					return;
				}

				// Compute the signed 32-bit relative offset for the JE instruction in the trampoline.
				// The JE must jump back to the original code immediately after the patched HOOK_SIZE bytes,
				// so the destination is target + HOOK_SIZE. The JE itself is at allocated + HOOK_SIZE + JE_OFFSET
				// and is JE_SIZE bytes long, so the relative offset is:
				// delta = destination - (JE address + JE_SIZE)
				const std::int32_t je_delta{ (std::int32_t)(target + HOOK_SIZE - (this->allocated + HOOK_SIZE + JE_OFFSET + JE_SIZE)) };

				// Patch the 4-byte signed offset field of the JE instruction in the assembly buffer.
				*(std::int32_t*)(assembly + JE_OFFSET + 2) = je_delta;

				// Write the address of the C++ detour function into the stub at DETOUR_ADDRESS_OFFSET.
				// The trampoline uses a RIP-relative indirect CALL (FF 15) to invoke the detour,
				// reading the target address from this 8-byte slot at runtime.
				*(jni::hotspot::detour_t*)(assembly + DETOUR_ADDRESS_OFFSET) = detour;

				// Copy the original HOOK_SIZE bytes from the target to the start of the allocated stub.
				// These are preserved so that the destructor can restore the original code on unhook.
				std::memcpy(this->allocated, target, HOOK_SIZE);

				// Copy the fully patched trampoline assembly immediately after the saved original bytes.
				std::memcpy(this->allocated + HOOK_SIZE, assembly, sizeof(assembly));

				// Downgrade the stub memory protection from EXECUTE_READWRITE to EXECUTE_READ
				// now that all writes are complete, reducing the attack surface.
				DWORD old_protect{};
				VirtualProtect(this->allocated, HOOK_SIZE + sizeof(assembly), PAGE_EXECUTE_READ, &old_protect);

				// Temporarily make the target memory writable so we can patch it with the JMP.
				VirtualProtect(target, JMP_SIZE, PAGE_EXECUTE_READWRITE, &old_protect);

				// Write the relative JMP opcode (0xE9) at the target injection point.
				target[0] = 0xE9;

				// Compute the signed 32-bit relative offset for the JMP from the target to the stub.
				// The JMP is JMP_SIZE bytes long, so the offset is:
				// delta = destination - (JMP address + JMP_SIZE)
				const std::int32_t jmp_delta{ (std::int32_t)(this->allocated - (target + JMP_SIZE)) };

				// Write the 4-byte signed offset immediately after the JMP opcode.
				*(std::int32_t*)(target + 1) = jmp_delta;

				// Restore the original memory protection on the target after patching.
				VirtualProtect(target, JMP_SIZE, old_protect, &old_protect);

				// Mark the hook as successfully installed.
				this->error = false;
			}

			/*
				@brief Removes the hook and restores the original code at the target.
				@details
				Checks that the hook was successfully installed and that the target still
				begins with the JMP instruction we wrote (0xE9), then restores the original
				5 bytes from the saved copy at the start of the allocated stub. Finally,
				releases the allocated trampoline memory back to the system via VirtualFree.
				@note If the hook was never successfully installed (error == true), or if
					  the target has already been restored by another party, the destructor
					  does nothing to avoid corrupting memory.
			*/
			~midi2i_hook()
			{
				if (this->error)
				{
					return;
				}

				// Verify that the target still starts with our JMP patch (0xE9) before restoring,
				// to avoid overwriting code that may have been modified by another hook or the JVM.
				DWORD old_protect{};
				if (this->target[0] == 0xE9 && VirtualProtect(this->target, 5, PAGE_EXECUTE_READWRITE, &old_protect))
				{
					// Restore the original bytes from the saved copy at the start of the allocated stub.
					std::memcpy(this->target, this->allocated, 5);

					// Restore the original memory protection on the target after unpatching.
					VirtualProtect(this->target, 5, old_protect, &old_protect);
				}

				// Release the allocated trampoline stub back to the system.
				// MEM_RELEASE must be used with dwSize = 0 to release the entire region at once.
				VirtualFree(this->allocated, 0, MEM_RELEASE);
			}

			/*
				@brief Returns whether the hook installation encountered an error.
				@return true if an error occurred during construction and the hook is not active,
						false if the hook was successfully installed.
			*/
			inline auto has_error() const noexcept
				-> bool
			{
				return this->error;
			}

		private:
			/*
				@brief Pointer to the injection point within the i2i stub that was patched
					   with the relative JMP instruction.
				@note Used by the destructor to restore the original bytes on unhook.
			*/
			std::uint8_t* target;

			/*
				@brief Pointer to the allocated trampoline stub.
				@details
				The first HOOK_SIZE bytes hold the saved original bytes from the target.
				The remaining bytes hold the trampoline assembly.
				@note Released by VirtualFree in the destructor.
			*/
			std::uint8_t* allocated;

			/*
				@brief Indicates whether the hook installation encountered an error.
				@details
				Set to false only if all steps of the constructor complete successfully.
				Checked by the destructor to avoid attempting cleanup of a partially
				installed hook.
			*/
			bool error;
		};

		/*
			@brief Stores the association between a HotSpot Method object and its C++ detour function.
			@details
			Each entry in hooked_methods maps a hotspot::method pointer to the detour function
			that should be called when that method is intercepted by the common_detour stub.
			The method pointer is used at hook time to identify which method is currently
			executing inside the interpreter, allowing multiple methods sharing the same
			i2i stub to each have their own independent detour function.
			@see common_detour, hooked_methods, jni::hook
		*/
		struct hooked_method
		{
			/*
				@brief Pointer to the HotSpot Method object of the hooked Java method.
				@note Used by common_detour to identify the currently executing method
					  and dispatch to the correct detour function.
			*/
			jni::hotspot::method* m{ nullptr };

			/*
				@brief The C++ detour function to invoke when this method is intercepted.
				@note Must match the detour_t signature: void(*)(frame*, java_thread*, bool*).
			*/
			jni::hotspot::detour_t detour{ nullptr };
		};

		/*
			@brief Stores the association between an i2i entry point and its installed midi2i_hook.
			@details
			Each entry in hooked_i2i_entries tracks a unique i2i stub entry point and the
			midi2i_hook instance that patched it. Since multiple Java methods can share the
			same i2i stub (e.g. all non-synchronized instance methods of the same type use
			the same interpreter entry), only one trampoline is allocated and installed per
			unique i2i entry point. The common_detour then dispatches to the correct per-method
			detour by looking up the currently executing method in hooked_methods.
			@see midi2i_hook, hooked_i2i_entries, jni::hook
		*/
		struct i2i_hook_data
		{
			/*
				@brief Pointer to the i2i stub entry point that was patched by the hook.
				@note Used to detect whether a new midi2i_hook needs to be installed
					  when hooking a method whose i2i entry has already been patched.
			*/
			void* i2i_entry{ nullptr };

			/*
				@brief Pointer to the midi2i_hook instance that manages the trampoline
					   installed at this i2i entry point.
				@note Owned by hooked_i2i_entries. Deleted by shutdown_hooks() on cleanup.
				@see shutdown_hooks
			*/
			jni::hotspot::midi2i_hook* hook{ nullptr };
		};

		/*
			@brief Global list of all currently hooked Java methods and their detour functions.
			@details
			Populated by jni::hook() when a new method is hooked, and cleared by shutdown_hooks().
			Iterated by common_detour on every intercepted interpreter call to dispatch
			to the correct per-method detour function.
			@see hooked_method, common_detour, jni::hook, shutdown_hooks
		*/
		inline std::vector<jni::hotspot::hooked_method> hooked_methods{};

		/*
			@brief Global list of all i2i entry points that have been patched and their hooks.
			@details
			Populated by jni::hook() when a new i2i stub is patched for the first time,
			and cleared by shutdown_hooks(). Used to avoid installing multiple trampolines
			on the same i2i entry point when multiple methods share the same stub.
			@see i2i_hook_data, jni::hook, shutdown_hooks
		*/
		inline std::vector<jni::hotspot::i2i_hook_data> hooked_i2i_entries{};

		/*
			@brief Common detour function invoked by the trampoline stub for every intercepted method call.
			@param f      Pointer to the current HotSpot interpreter frame, passed via rcx by the trampoline.
						  Provides access to the currently executing method and its arguments.
			@param thread Pointer to the current HotSpot JavaThread, passed via rdx by the trampoline.
						  Provides access to the thread state and JNI environment.
			@param cancel Pointer to the cancel flag on the trampoline stack, passed via r8 by the trampoline.
						  Setting *cancel to true instructs the trampoline to skip the original method
						  and return the value currently held in rax to the caller instead.
			@details
			This function is the single entry point called by every midi2i_hook trampoline regardless
			of which Java method was intercepted. It performs a series of safety checks on the thread
			and frame before iterating over hooked_methods to find the entry whose method pointer
			matches the currently executing method, then dispatches to the registered per-method detour.
			After the per-method detour returns, the thread state is explicitly restored to
			_thread_in_Java to ensure the JVM continues to operate correctly, since the detour
			may have triggered JNI calls that left the thread in a different state.
			@note This function must execute as quickly as possible as it is called on every
				  intercepted interpreter dispatch, potentially thousands of times per second.
			@see midi2i_hook, hooked_methods, hooked_method, java_thread_state
		*/
		static auto common_detour(jni::hotspot::frame* f, jni::hotspot::java_thread* thread, bool* cancel)
			-> void
		{
			try
			{
				// Verify that the JavaThread pointer itself is valid before dereferencing any of its fields.
				// A null thread pointer indicates the trampoline fired in an unexpected context.
				if (!thread)
				{
					throw jni::jni_exception{ "JavaThread pointer is null." };
				}

				// Verify that the JNI environment embedded in the JavaThread is valid before proceeding.
				// A null JNI environment means the thread is not properly attached to the JVM,
				// making it unsafe to perform any JNI calls inside the per-method detour.
				if (!thread->get_env())
				{
					throw jni::jni_exception{ "JavaThread JNI environment is null." };
				}

				// Verify that the thread is currently executing Java bytecode before dispatching.
				// The hook can theoretically fire in other thread states (e.g. during class loading
				// or JVM internal operations), where it would be unsafe to interact with Java objects
				// or call JNI functions. Only _thread_in_Java guarantees a safe interception context.
				if (thread->get_thread_state() != jni::hotspot::java_thread_state::_thread_in_Java)
				{
					throw jni::jni_exception{ "JavaThread is not in _thread_in_Java state." };
				}

				// Retrieve the Method pointer of the currently executing method from the interpreter frame.
				// This is used to look up the correct per-method detour in hooked_methods.
				jni::hotspot::method* current_method{ f->get_method() };

				// Iterate over all registered hooked methods to find the one matching the current method.
				for (jni::hotspot::hooked_method& hk : hooked_methods)
				{
					// Compare the Method pointer of this entry against the currently executing method.
					// Pointer equality is sufficient here since Method objects are unique per method in HotSpot.
					if (hk.m == current_method)
					{
						// Dispatch to the registered per-method detour, passing the frame, thread,
						// and cancel flag so the detour can read arguments and optionally cancel the call.
						hk.detour(f, thread, cancel);

						// Restore the thread state to _thread_in_Java after the detour returns.
						// The detour may have performed JNI calls that transitioned the thread to
						// _thread_in_native or another state, which would cause the JVM to behave
						// incorrectly if not corrected before returning to the interpreter.
						thread->set_thread_state(jni::hotspot::java_thread_state::_thread_in_Java);
						return;
					}
				}
			}
			catch (const std::exception& e)
			{
				std::println("{} common_detour() {}", jni::easy_jni_error, e.what());
				return;
			}
		}

		/*
			@brief Bitmask of HotSpot access flags used to disable JIT compilation on hooked methods.
			@details
			When a Java method is hooked via jni::hook(), its JIT compilation must be permanently
			disabled to ensure the hook always fires through the interpreter and its i2i stub,
			regardless of how many times the method has been called. If the JIT were allowed to
			compile the method, HotSpot would replace the interpreter dispatch with a direct call
			to the compiled native code, completely bypassing the i2i stub and the installed hook.

			This bitmask is OR'd into the Method._access_flags field of the hooked method to set
			all four relevant flags simultaneously. It is applied both before and after a forced
			class retransformation via JVMTI to ensure the flags are preserved even if HotSpot
			resets them during retransformation.

			The individual flags and their effects are:
			- JVM_ACC_NOT_C2_COMPILABLE  (0x02000000): Prevents the C2 (server) JIT compiler
			  from compiling this method. C2 produces highly optimized native code and is the
			  primary compilation tier that would otherwise replace the interpreter dispatch.
			- JVM_ACC_NOT_C1_COMPILABLE  (0x04000000): Prevents the C1 (client) JIT compiler
			  from compiling this method. C1 produces lightly optimized native code and is used
			  as an intermediate compilation tier before C2 in tiered compilation mode.
			- JVM_ACC_NOT_C2_OSR_COMPILABLE (0x08000000): Prevents C2 from performing
			  On-Stack Replacement (OSR) compilation of this method. OSR allows the JVM to
			  transition a method from interpreted to compiled execution mid-execution, inside
			  a running loop, which would also bypass the hook if not disabled.
			- JVM_ACC_QUEUED (0x01000000): Marks the method as already queued for compilation,
			  preventing the JIT compiler from re-queuing it for compilation in the future.

			@note These flags are internal HotSpot implementation details and are not part of
				  the public JVM specification. Their values may differ across JVM versions.
			@see jni::hook, set_dont_inline, hotspot::method::get_access_flags
		*/
		inline const constexpr std::int32_t NO_COMPILE =
			0x02000000 | // JVM_ACC_NOT_C2_COMPILABLE    : disable C2 (server) JIT compilation
			0x04000000 | // JVM_ACC_NOT_C1_COMPILABLE    : disable C1 (client) JIT compilation
			0x08000000 | // JVM_ACC_NOT_C2_OSR_COMPILABLE: disable C2 on-stack replacement compilation
			0x01000000;  // JVM_ACC_QUEUED               : mark as already queued to prevent re-queuing

		/*
			@brief Enables or disables the _dont_inline flag on a HotSpot Method object.
			@param m       Pointer to the hotspot::method to modify.
			@param enabled If true, sets the _dont_inline flag to prevent the JIT compiler
						   from inlining this method at call sites.
						   If false, clears the flag to restore normal inlining behaviour.
			@details
			The _dont_inline flag is a HotSpot-internal method flag stored in Method._flags
			at bit position 2. When set, it instructs both the C1 and C2 JIT compilers to
			never inline this method at any call site, regardless of how small or frequently
			called the method is.

			This flag is set on every hooked method in addition to the NO_COMPILE flags
			in Method._access_flags, as an extra safety measure. Even if the NO_COMPILE
			flags were somehow bypassed or reset by the JVM, preventing inlining ensures
			that the method always has its own distinct call site in compiled code, making
			it harder for the JIT to optimize away the interpreter dispatch that the hook
			relies on.

			The flag is cleared by shutdown_hooks() when the hook is removed, restoring
			the method to its original inlining behaviour and allowing the JIT compiler
			to inline it again if appropriate.

			@note Modifying Method._flags directly bypasses all JVM safepoints and
				  synchronization mechanisms. This is safe here because the flag is
				  only read by the JIT compiler during compilation, which is already
				  prevented from occurring by the NO_COMPILE flags set beforehand.
			@see NO_COMPILE, hotspot::method::get_flags, jni::hook, shutdown_hooks
		*/
		static auto set_dont_inline(jni::hotspot::method* m, bool enabled) noexcept
			-> void
		{
			// Retrieve a pointer to the Method._flags field via the cached VMStructEntry offset.
			// Returns nullptr if the entry could not be found in gHotSpotVMStructs.
			std::uint16_t* flags{ m->get_flags() };
			if (!flags)
			{
				return;
			}

			if (enabled)
			{
				// Set bit 2 of Method._flags to enable the _dont_inline flag.
				// The bitwise OR ensures all other flags are preserved unchanged.
				*flags |= (1 << 2); // _dont_inline
			}
			else
			{
				// Clear bit 2 of Method._flags to disable the _dont_inline flag.
				// The bitwise AND with the inverted mask ensures all other flags are preserved unchanged.
				*flags &= ~(1 << 2);
			}
		}
	}

	/*
		@brief Installs a low-level interpreter hook on a Java method of the given C++ wrapper type.
		@tparam type The C++ wrapper type whose corresponding Java class owns the method to hook.
					 Must satisfy jni_object and have been registered via jni::register_class<type>().
		@param method_name The name of the Java method to hook (e.g., "toString", "update").
		@param detour      The C++ function to invoke when the hooked method is intercepted.
						   Must match the detour_t signature: void(*)(frame*, java_thread*, bool*).
		@return true if the hook was successfully installed or was already active, false on any failure.
		@details
		The hook installation process proceeds through the following steps:

		1. Retrieve the jclass for the registered Java class corresponding to type.
		2. Enumerate all methods of that class via JVMTI GetClassMethods.
		3. Locate the jmethodID whose Method name matches method_name.
		4. Disable JIT compilation on the method by setting NO_COMPILE in Method._access_flags
		   and _dont_inline in Method._flags, to ensure the method always executes through
		   the interpreter and its i2i stub where the hook is installed.
		5. Force a JVMTI class retransformation to flush any existing compiled code for the method,
		   then re-apply the NO_COMPILE and _dont_inline flags since retransformation may reset them.
		6. Register the method and its detour in hooked_methods for dispatch by common_detour.
		7. Check whether the i2i entry point of the method has already been patched by a previous
		   hook installation. If so, no new trampoline is needed since common_detour will dispatch
		   to the correct detour based on the Method pointer.
		8. If the i2i entry is new, locate the injection point via find_hook_location(), allocate
		   a trampoline via midi2i_hook, and register it in hooked_i2i_entries.

		@note The class must have been registered via jni::register_class<type>() before calling hook().
			  Hooking a method that is already hooked is a no-op and returns true immediately.
		@warning Hooked methods must not be allowed to be JIT compiled. If the JVM somehow bypasses
				 the NO_COMPILE flags, the hook will silently stop firing for compiled invocations.
		@see midi2i_hook, common_detour, set_dont_inline, NO_COMPILE, shutdown_hooks
	*/
	template <jni_object type>
	static auto hook(const std::string_view method_name, jni::hotspot::detour_t detour)
		-> bool
	{
		try
		{
			// Validate the detour function pointer before proceeding with any JVM interactions.
			if (!detour)
			{
				throw jni::jni_exception{ "Detour function pointer is null." };
			}

			// Retrieve the global jclass reference for the Java class registered for this C++ type.
			// Returns nullptr if the class was not registered via jni::register_class<type>().
			const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(type) })) };
			if (!clazz)
			{
				throw jni::jni_exception{ "Class not found in JVM. Did you call jni::register_class<type>()?" };
			}

			// Enumerate all methods declared by the class via JVMTI to locate the target method.
			jint method_count{ 0 };
			jmethodID* methods{ nullptr };
			if (jni::jvmti->GetClassMethods(clazz, &method_count, &methods) != JVMTI_ERROR_NONE)
			{
				throw jni::jni_exception{ "JVMTI GetClassMethods failed." };
			}

			// Iterate over the enumerated methods to find the one whose name matches method_name.
			// The Method pointer is obtained by dereferencing the jmethodID, which in HotSpot
			// is internally a pointer to a Method pointer (Method**).
			jmethodID method_id{ nullptr };
			for (jint i{ 0 }; i < method_count; ++i)
			{
				jni::hotspot::method* m{ *(jni::hotspot::method**)methods[i] };
				if (m && m->get_name() == method_name)
				{
					method_id = methods[i];
					break;
				}
			}

			// Release the JVMTI-allocated method array regardless of whether the method was found.
			jni::jvmti->Deallocate(reinterpret_cast<unsigned char*>(methods));

			if (!method_id)
			{
				throw jni::jni_exception{ std::format("Method not found in class.") };
			}

			// Dereference the jmethodID to obtain the raw HotSpot Method pointer.
			jni::hotspot::method* m{ *(jni::hotspot::method**)method_id };

			// Check whether this method is already hooked to avoid installing duplicate hooks.
			// If it is, return true immediately since the hook is already active.
			for (jni::hotspot::hooked_method& hk : jni::hotspot::hooked_methods)
			{
				if (hk.m == m)
				{
					return true;
				}
			}

			// Prevent the JIT compiler from inlining this method at call sites.
			// This is set before NO_COMPILE to ensure the flag is in place before
			// the compiler has any chance to process the method.
			jni::hotspot::set_dont_inline(m, true);

			// Disable all JIT compilation tiers for this method by setting the NO_COMPILE flags
			// in Method._access_flags, preventing C1, C2, and OSR compilation.
			std::uint32_t* flags{ m->get_access_flags() };
			if (!flags)
			{
				throw jni::jni_exception{ std::format("Failed to retrieve access flags.") };
			}
			*flags |= jni::hotspot::NO_COMPILE;

			// Re-dereference the jmethodID after retransformation since the Method pointer
			// may have changed if HotSpot reallocated the Method object during retransformation.
			m = *(jni::hotspot::method**)method_id;

			// Re-apply the _dont_inline and NO_COMPILE flags after retransformation,
			// since retransformation may have reset the Method._flags and Method._access_flags fields.
			jni::hotspot::set_dont_inline(m, true);

			flags = m->get_access_flags();
			if (!flags)
			{
				throw jni::jni_exception{ std::format("Failed to retrieve access flags after retransformation.") };
			}
			*flags |= jni::hotspot::NO_COMPILE;

			// Register the method and its detour in hooked_methods so that common_detour
			// can dispatch to the correct per-method detour when the hook fires.
			jni::hotspot::hooked_methods.push_back({ m, detour });

			// Retrieve the i2i entry point of the method to determine whether a new
			// trampoline needs to be installed or whether an existing one can be reused.
			void* i2i = m->get_i2i_entry();
			if (!i2i)
			{
				throw jni::jni_exception{ std::format("Failed to retrieve i2i entry.") };
			}

			// Check whether this i2i entry point has already been patched by a previous hook.
			// If so, common_detour will already dispatch correctly via hooked_methods,
			// so no new trampoline needs to be allocated or installed.
			for (jni::hotspot::i2i_hook_data& hk : jni::hotspot::hooked_i2i_entries)
			{
				if (hk.i2i_entry == i2i)
				{
					return true;
				}
			}

			// Locate the injection point within the i2i stub where the JMP patch will be installed.
			// Returns nullptr if the expected instruction pattern could not be found in the stub.
			std::uint8_t* target{ (std::uint8_t*)jni::hotspot::find_hook_location(i2i) };
			if (!target)
			{
				throw jni::jni_exception{ std::format("Failed to find hook location in i2i stub.") };
			}

			// Allocate and install the trampoline stub at the injection point via midi2i_hook.
			// The trampoline saves registers, calls common_detour, and either resumes or cancels
			// the original method execution depending on the cancel flag set by the detour.
			jni::hotspot::midi2i_hook* hook_instance{ new jni::hotspot::midi2i_hook(target, jni::hotspot::common_detour) };
			if (hook_instance->has_error())
			{
				delete hook_instance;
				throw jni::jni_exception{ std::format("midi2i_hook installation failed.") };
			}

			// Register the i2i entry and its hook instance in hooked_i2i_entries so that
			// subsequent hooks on methods sharing the same i2i stub can detect and reuse it.
			jni::hotspot::hooked_i2i_entries.push_back({ i2i, hook_instance });

			return true;
		}
		catch (const std::exception& e)
		{
			std::println("{} jni::hook() for '{}': {}", jni::easy_jni_error, method_name, e.what());
			return false;
		}
	}

	/*
		@brief Removes all active interpreter hooks and restores the JVM to its original state.
		@details
		Performs a full cleanup of all hooks installed by jni::hook() in two phases:

		Phase 1 - Trampoline removal:
		Iterates over hooked_i2i_entries and deletes each midi2i_hook instance.
		The midi2i_hook destructor is responsible for restoring the original bytes
		at the patched injection point and releasing the allocated trampoline memory
		via VirtualFree, ensuring no dangling JMP patches remain in the i2i stubs.

		Phase 2 - Method flag restoration:
		Iterates over hooked_methods and restores the _dont_inline and NO_COMPILE flags
		on each hooked Method object to their original state, allowing the JIT compiler
		to resume normal compilation and inlining of the previously hooked methods.
		This is important to avoid leaving the JVM in a degraded performance state
		after the hooks are removed.

		Both vectors are cleared after cleanup to release their memory and reset the
		hook registry to an empty state, ensuring no stale entries remain.

		@note This function is called automatically by jni::shutdown() before the
			  consuming thread detaches from the JVM. It should not be called while
			  any hooked method is currently executing on another thread, as restoring
			  the i2i stub bytes while a thread is executing through the trampoline
			  would result in undefined behaviour.
		@warning After this function returns, any previously hooked methods will execute
				 normally through the interpreter without interception. The JIT compiler
				 will also be free to compile and inline them again on subsequent calls.
		@see midi2i_hook::~midi2i_hook, set_dont_inline, NO_COMPILE, jni::shutdown
	*/
	static auto shutdown_hooks() noexcept
		-> void
	{
		// Phase 1: remove all installed trampolines by deleting each midi2i_hook instance.
		// The destructor of midi2i_hook restores the original bytes at the patched injection
		// point and releases the allocated trampoline memory back to the system via VirtualFree.
		for (jni::hotspot::i2i_hook_data& hk : jni::hotspot::hooked_i2i_entries)
		{
			delete hk.hook;
		}

		// Phase 2: restore the _dont_inline and NO_COMPILE flags on all hooked methods
		// to allow the JIT compiler to resume normal compilation and inlining behaviour.
		for (jni::hotspot::hooked_method& hm : jni::hotspot::hooked_methods)
		{
			// Clear the _dont_inline flag to allow the JIT compiler to inline this method
			// at call sites again, restoring its original inlining eligibility.
			jni::hotspot::set_dont_inline(hm.m, false);

			// Clear the NO_COMPILE flags from Method._access_flags to allow the JIT compiler
			// to compile this method again through C1, C2, and OSR compilation tiers.
			std::uint32_t* flags{ hm.m->get_access_flags() };
			if (flags)
			{
				*flags &= ~jni::hotspot::NO_COMPILE;
			}
		}

		// Clear both vectors to release their memory and reset the hook registry to an empty state.
		// After this point, no hooks are active and common_detour will find no matching methods.
		jni::hotspot::hooked_methods.clear();
		jni::hotspot::hooked_i2i_entries.clear();
	}
}