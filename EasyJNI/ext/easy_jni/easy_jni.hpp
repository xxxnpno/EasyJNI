#pragma once

#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <stdexcept>
#include <string>
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

#include <jni/jni.h>
#include <jni/jvmti.h>

// everything about EasyJNI is in this namespace
namespace jni
{
	/*
		declarations
	*/

	class object;

	static auto shutdown_hooks() -> void;

	/*
		declarations
	*/

	// global vm and jvmti pointers
	inline constinit JavaVM* vm{ nullptr };
	inline constinit jvmtiEnv* jvmti{ nullptr };

	// gets the env of the current thread, if the env is not found, attaches the thread to the JVM and stores it locally
	static auto get_env() noexcept 
		-> JNIEnv*
	{
		thread_local JNIEnv* env{ nullptr };

		if (!jni::vm)
		{
			std::println("[ERROR] get_env() called before jni::vm is initialized.");
			return nullptr;
		}

		if (!env)
		{
			const jint result{ jni::vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) };

			if (result == JNI_EDETACHED)
			{
				if (jni::vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK)
				{
					std::println("[ERROR] get_env() Failed to attach current thread to JVM.");
					return nullptr;
				}
			}
			else if (result != JNI_OK)
			{
				std::println("[ERROR] get_env() GetEnv failed with result {}.", static_cast<std::int32_t>(result));
				return nullptr;
			}
		}

		return env;
	}

	// detaches the current thread from the JVM, should be called before thread exit
	static auto exit_thread() noexcept
		-> void
	{
		thread_local JNIEnv* env{ nullptr };

		if (env && jni::vm)
		{
			if (jni::vm->DetachCurrentThread() != JNI_OK)
			{
				std::println("[WARNING] exit_thread() Failed to detach current thread from JVM.");
			}

			env = nullptr;
		}
	}

	// associates the class name with the global reference of the class
	inline std::unordered_map<std::string, jclass> classes{};

	// takes a class name a creates a global reference to the class
	static auto load_class(const std::string& name)
		-> jclass
	{
		try
		{
			const jclass clazz{ jni::get_env()->FindClass("java/lang/Class") };
			const jmethodID get_name_id{ jni::get_env()->GetMethodID(clazz, "getName", "()Ljava/lang/String;") };

			if (!clazz)
			{
				throw std::runtime_error{ std::format("Class is nullptr for {}.", name) };
			}

			if (!get_name_id)
			{
				throw std::runtime_error{ std::format("getName jmethodID is nullptr for {}.", name) };
			}

			jni::get_env()->DeleteLocalRef(clazz);

			jint amount{};
			jclass* classes_ptr{};

			// classes_ptr will become an array of all the classes that jvmti found
			// since all the the java classes are java/lang/Class we can use the getName method
			if (jni::jvmti->GetLoadedClasses(&amount, &classes_ptr) != JVMTI_ERROR_NONE)
			{
				throw std::runtime_error{ std::format("Failed to get loaded classes for {}.", name) };
			}

			// each time you want to get a new class it looks for it in the array and creates a global reference for it
			// doing so we don't create a global ref for every classes in the jvm but only for the ones you are using
			// it creates a bit of overhead the first time you use a method
			jclass found{ nullptr };
			for (jint i{ 0 }; i < amount; ++i)
			{
				const jclass current_class{ classes_ptr[i] };

				if (!current_class)
				{
					throw std::runtime_error{ std::format("Found a null class during {}'s search.", name) };
				}

				const jstring class_name{ static_cast<jstring>(jni::get_env()->CallObjectMethod(current_class, get_name_id)) };

				const char* const class_name_c_str{ jni::get_env()->GetStringUTFChars(class_name, nullptr) };
				const bool match{ !std::strcmp(class_name_c_str, name.c_str()) };

				jni::get_env()->ReleaseStringUTFChars(class_name, class_name_c_str);
				jni::get_env()->DeleteLocalRef(class_name);

				if (match)
				{
					found = static_cast<jclass>(jni::get_env()->NewGlobalRef(current_class));
					jni::get_env()->DeleteLocalRef(current_class);

					jni::classes.insert({ name, found });
					break;
				}

				jni::get_env()->DeleteLocalRef(current_class);
			}

			jni::jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes_ptr));

			return found;
		}
		catch (const std::exception& e)
		{
			std::println("[ERROR] manage_envs() {}", e.what());
			return nullptr;
		}
	}

	// gets the global reference of a class from its name, if the class is not found, return nullptr
	static auto get_class(const std::string& name)
		-> jclass
	{
		std::string class_name{ name };

		// I prefer using my/Class instead of my.Class
		// maybe I we really care we can create a preprocessor flag for "." or "/"
		std::replace(class_name.begin(), class_name.end(), '/', '.');

		// if the class already has a global ref we just return it
		if (const auto it{ jni::classes.find(class_name) }; it != jni::classes.end())
		{
			return it->second;
		}

		// if not we ask jvmti to get it
		jclass found{ jni::load_class(class_name) };

		// if jvmti fails to obtain it we try FindClass from the jni api
		if (!found)
		{
			const jclass local{ jni::get_env()->FindClass(name.c_str()) };

			if (local)
			{
				found = static_cast<jclass>(jni::get_env()->NewGlobalRef(local));
				jni::get_env()->DeleteLocalRef(local);
				jni::classes.insert({ class_name, found });
			}
			else
			{
				std::println("[ERROR] get_class() Couldn't find the class {}.", name);
			}
		}

		return found;
	}

	// handles strings, shoud not be used outside of this header
	// constructor takes std::string or jstring and creates the other type and stores it
	class string final
	{
	public:
		explicit string(const std::string& std_string = "")
			: jni_string{ nullptr }
			, std_string{ std_string }
		{
			const jstring local{ jni::get_env()->NewStringUTF(std_string.c_str()) };

			if (local)
			{
				// create a global ref of the string just like we handle jobject
				this->jni_string = static_cast<jstring>(jni::get_env()->NewGlobalRef(local));
				jni::get_env()->DeleteLocalRef(local);
			}
		}

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

		string(const string& other)
			: jni_string{ nullptr }
			, std_string{ other.std_string }
		{
			if (other.jni_string)
			{
				this->jni_string = static_cast<jstring>(jni::get_env()->NewGlobalRef(other.jni_string));
			}
		}

		auto operator=(const string& other) 
			-> string&
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

		string(string&& other) noexcept
			: jni_string{ other.jni_string }
			, std_string{ std::move(other.std_string) }
		{
			other.jni_string = nullptr;
		}

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

		~string() noexcept
		{
			if (this->jni_string)
			{
				jni::get_env()->DeleteGlobalRef(this->jni_string);
			}
		}

		inline auto get_jni_string() const noexcept
			-> jstring
		{
			return this->jni_string;
		}

		inline auto get_std_string() const noexcept
			-> std::string
		{
			return this->std_string;
		}

	private:
		jstring jni_string;

		std::string std_string;
	};

	/*
		type cast helper
	*/

	using convert_function = std::function<std::any(const std::any&)>;

	// associates a jni type to a cpp one
	inline const std::unordered_map<std::type_index, convert_function> jni_to_cpp =
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

	// associates a cpp type to a jni one
	inline const std::unordered_map<std::type_index, convert_function> cpp_to_jni =
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

	// associates a jni type to a function that takes a cpp type and returns a jvalue with the corresponding field set, 
	// used to call methods with arguments or to set fields
	inline const std::unordered_map<std::type_index, jvalue_setter> jni_to_jvalue =
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
		type cast helper
	*/

	// associates a cpp type to a jni signature
	inline const std::unordered_map<std::type_index, std::string> signature_map
	{
		{ std::type_index{ typeid(void) }, "V" },
		{ std::type_index{ typeid(short) }, "S" },
		{ std::type_index{ typeid(int) }, "I" },
		{ std::type_index{ typeid(long long) }, "J" },
		{ std::type_index{ typeid(float) }, "F" },
		{ std::type_index{ typeid(double) }, "D" },
		{ std::type_index{ typeid(bool) }, "Z" },
		{ std::type_index{ typeid(char) }, "C" },
		{ std::type_index{ typeid(std::byte) }, "B" },
		{ std::type_index{ typeid(std::string) }, "Ljava/lang/String;" },
		{ std::type_index{ typeid(jobjectArray) }, "[Ljava/lang/Object;" }
	};

	/*
		helpers for requires
	*/

	template<typename T>
	struct unwrap_object_ptr { using type = T; };

	template<typename T>
		requires (std::is_base_of_v<object, T>)
	struct unwrap_object_ptr<std::unique_ptr<T>> { using type = T; };

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

	template<typename T>
	concept jni_object = std::is_base_of_v<object, T>;

	template<typename T>
	concept jni_object_ptr = jni_object<typename unwrap_object_ptr<T>::type>
		&& !jni_object<T>;

	template<typename T>
	concept method_return = std_type<T> || jni_object<T> || std::is_same_v<T, jobjectArray>;

	template<typename T>
	concept field_type_c = std_type<T> || jni_object<T>;

	template<typename T>
	concept callable_arg = std_type<std::remove_cvref_t<T>>
		|| jni_object<std::remove_cvref_t<T>>
		|| jni_object_ptr<std::remove_cvref_t<T>>
		|| std::is_convertible_v<std::remove_cvref_t<T>, std::string>;

	/*
		helpers for requires
	*/

	// associates the type index of a cpp type to the signature of the corresponding java type,
	// used to get the signature of a method or a field from the cpp type
	inline std::unordered_map<std::type_index, std::string> class_map{};

	// associates your cpp class to a java class
	template <jni_object type>
	static auto register_class(const std::string& class_name)
		-> bool
	{
		jni::class_map.insert_or_assign(std::type_index{ typeid(type) }, class_name);

		if (!jni::get_class(class_name))
		{
			std::println("[ERROR] register_class() class not found in JVM: {}", class_name);
			jni::class_map.erase(std::type_index{ typeid(type) });
			return false;
		}

		return true;
	}

	// returns the signature of a type primitive or not
	template <method_return type>
	static auto get_signature()
		-> std::string
	{
		try
		{
			if constexpr (std::is_base_of_v<object, type>)
			{
				const auto it{ jni::class_map.find(std::type_index{ typeid(type) }) };
				if (it == jni::class_map.end())
				{
					throw std::runtime_error{ std::format("Class not registered for type {}.", typeid(type).name()) };
				}

				return std::format("L{};", it->second);
			}
			else if constexpr (std::is_same_v<type, jobjectArray>)
			{
				return signature_map.at(std::type_index{ typeid(jobjectArray) });
			}
			else
			{
				return jni::signature_map.at(std::type_index{ typeid(type) });
			}
		}
		catch (const std::exception& e)
		{
			std::println("[ERROR] get_signature() {}", e.what());
			return std::string{};
		}
	}

	// helper function to transform args_t&&... args into a vector of jvalues
	template<callable_arg... args_t>
	static auto build_jargs(args_t&&... args)
		-> std::pair<std::vector<jvalue>, std::vector<jni::string>>
	{
		std::vector<jni::string> string_keeper{};
		string_keeper.reserve(sizeof...(args_t));

		auto convert_arg = [&string_keeper](auto&& arg) -> jvalue
		{
			using raw = std::remove_cvref_t<decltype(arg)>;

			std::any jni_val{};
			if constexpr (std::is_base_of_v<object, raw>)
			{
				jni_val = arg.get_instance();
			}
			else if constexpr (jni_object_ptr<raw>)
			{
				jni_val = arg->get_instance();
			}
			else if constexpr (std::is_convertible_v<raw, std::string>)
			{
				string_keeper.emplace_back(std::string{ arg });
				jni_val = string_keeper.back().get_jni_string();
			}
			else
			{
				jni_val = jni::cpp_to_jni.at(std::type_index{ typeid(raw) })(arg);
			}

			return jni::jni_to_jvalue.at(std::type_index{ jni_val.type() })(jni_val);
		};

		std::vector<jvalue> jargs{};
		if constexpr (sizeof...(args_t) > 0)
		{
			jargs = { convert_arg(std::forward<args_t>(args))... };
		}

		return { std::move(jargs), std::move(string_keeper) };
	}

	// associates type_index of a cpp class with the map of its fieldIDs
	inline std::unordered_map<std::type_index, std::unordered_map<std::string, jfieldID>> field_ids{};

	// used to know if a field is static or not
	enum class field_type : std::int8_t
	{
		STATIC,
		NOT_STATIC
	};

	// used to represent a field and to get and set the field
	// should not be used outside of this header
	template <field_type_c type>
	class field final
	{
	public:
		field(void* class_or_instance, const std::string& name, const jni::field_type field_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, index{ index }
			, field_type{ field_type }
		{

		}

		// gets the field value
		auto get() const
			-> std::conditional_t<std::is_base_of_v<object, type>, std::unique_ptr<type>, type>
		{
			try
			{
				const auto& inner_map{ jni::field_ids.at(this->index) };
				const auto it{ inner_map.find(this->name) };

				if (it == inner_map.end())
				{
					throw std::runtime_error{ std::format("Field ID not found for {}.", this->name) };
				}

				const jfieldID field_id{ it->second };

				jobject local{};
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
				std::println("[ERROR] get() {}", e.what());

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

		// set the field value
		// pass primitive types and unique ptrs, do not pass the jobject directly
		auto set(const type& value) const
			-> void
		{
			try
			{
				const auto& inner_map{ jni::field_ids.at(this->index) };
				const auto it{ inner_map.find(this->name) };

				if (it == inner_map.end())
				{
					throw std::runtime_error{ std::format("Field ID not found for {}.", this->name) };
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
				std::println("[ERROR] set() {}", e.what());
			}
		}

	private:
		// stores the class or instance pointer depending on if the field is static or not,
		// to prevent having to pass it every time we want to get or set the field
		void* class_or_instance;

		std::string name;

		std::type_index index;

		field_type field_type;
	};

	// associates type_index of a cpp class with the map of its methodIDs
	inline std::unordered_map<std::type_index, std::unordered_map<std::string, jmethodID>> method_ids{};

	// used to know if a method is static or not
	enum class method_type : std::uint8_t
	{
		STATIC,
		NOT_STATIC
	};

	// used to represent a method and to call the method
	// should not be used outside of this header
	template <method_return return_type>
	class method final
	{
	public:
		method(void* class_or_instance, const std::string& name, const jni::method_type method_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, index{ index }
			, method_type{ method_type }
		{

		}

		// call the method with the given arguments
		// pass primitive types and unique ptrs, do not pass the jobject directly
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
					throw std::runtime_error{ std::format("Method ID not found for {}.", this->name) };
				}

				const jmethodID method_id{ it->second };

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
				std::println("[ERROR] call() {}", e.what());

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
		// store the class or instance pointer depending on if the method is static or not, to prevent having to pass it every time we want to call the method
		void* class_or_instance;

		std::string name;

		std::type_index index;

		method_type method_type;
	};

	// used to represent a java object and to get and set its fields and call its methods
	// stores the jobject as a global reference to prevent it from being garbage collected while we are using it
	// inherit from this class to create a wrapper for a java class
	class object
	{
	public:
		// you can pass a nullptr and it will store it
		// used for constructors or for object that are null in the jvm
		explicit object(const jobject instance = nullptr)
			// create a global reference of the jobject that is how every jobjects are managed 
			: instance{ instance ? jni::get_env()->NewGlobalRef(instance) : nullptr }
		{

		}

		object(const object& other)
			: instance{ other.instance ? jni::get_env()->NewGlobalRef(other.instance) : nullptr }
		{

		}

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

		object(object&& other) noexcept
			: instance{ other.instance }
		{
			other.instance = nullptr;
		}

		// move assignment
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

		virtual ~object() noexcept
		{
			if (this->instance)
			{
				// delete the global reference created
				jni::get_env()->DeleteGlobalRef(this->instance);
			}
		}

		// obtains a wrapped field that you can get or set
		template<field_type_c type>
		auto get_field(const std::string& field_name, const jni::field_type field_type = jni::field_type::NOT_STATIC) const
			-> std::unique_ptr<field<type>>
		{
			try
			{
				const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(*this) })) };

				if (!clazz)
				{
					throw std::runtime_error{ std::format("Class not found for {}.", field_name) };
				}

				if (!this->instance && field_type == jni::field_type::NOT_STATIC)
				{
					throw std::runtime_error{ std::format("Instance is null for {}.", field_name) };
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
				std::println("[ERROR] get_field() {}", e.what());
				return std::make_unique<field<type>>(nullptr, field_name, field_type, std::type_index{ typeid(*this) });
			}
		}

		// obtains a wrapped method that you can call
		template<method_return return_type, callable_arg... args_t>
		auto get_method(const std::string& method_name, const jni::method_type method_type = jni::method_type::NOT_STATIC) const
			-> std::unique_ptr<method<return_type>>
		{
			try
			{
				const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(*this) })) };

				if (!clazz)
				{
					throw std::runtime_error{ std::format("Class not found for {}.", method_name) };
				}

				if (!this->instance && method_type == jni::method_type::NOT_STATIC)
				{
					throw std::runtime_error{ std::format("Instance is null for {}.", method_name) };
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
				std::println("[ERROR] get_method() {}", e.what());
				return std::make_unique<method<return_type>>(nullptr, method_name, method_type, std::type_index{ typeid(*this) });
			}
		}

		// unique pointers are never nullptr but the instance might be nullptr if the object is also null in the jvm
		inline auto get_instance() const noexcept
			-> jobject
		{
			return this->instance;
		}

	protected:
		jobject instance;

	private:
		// obtains the jfieldID of a field
		template<field_type_c type>
		auto register_field_id(const jclass clazz, const std::string& field_name, const jni::field_type field_type, const std::type_index index) const
			-> void
		{
			try
			{
				if (jni::field_ids[std::type_index{ index }].find(field_name)
					!= jni::field_ids[std::type_index{ index }].end())
				{
					return;
				}

				const std::string& signature{ jni::get_signature<type>() };

				if (signature.empty())
				{
					throw std::runtime_error{ std::format("Failed to get signature for {}.", field_name) };
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
					throw std::runtime_error{ std::format("Failed to get field id for {}.", field_name) };
				}

				jni::field_ids[std::type_index{ index }].insert(
					{ field_name, field_id }
				);
			}
			catch (const std::exception& e)
			{
				std::println("[ERROR] register_field_id() {}", e.what());
			}
		}

		// obtains the jmethodID of a method
		template<method_return return_type, callable_arg... args_t>
		auto register_method_id(const jclass clazz, const std::string& method_name, const jni::method_type method_type, const std::type_index index = typeid(*this)) const
			-> void
		{
			try
			{
				if (jni::method_ids[std::type_index{ index }].find(method_name)
					!= jni::method_ids[std::type_index{ index }].end())
				{
					return;
				}

				std::string params_sig{};
				((params_sig += jni::get_signature<std::remove_cvref_t<args_t>>()), ...);

				const std::string signature{ std::format("({}){}", params_sig, jni::get_signature<return_type>()) };

				if (signature.empty())
				{
					throw std::runtime_error{ std::format("Failed to get signature for {}.", method_name) };
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
					throw std::runtime_error{ std::format("Failed to get method id for {}.", method_name) };
				}

				jni::method_ids[std::type_index{ index }].insert(
					{ method_name, method_id }
				);
			}
			catch (const std::exception& e)
			{
				std::println("[ERROR] register_method_id() {}", e.what());
			}
		}

		// friend function to use NewObject from the jni api
		template<jni_object type, callable_arg... args_t>
		friend auto make_unique(args_t&&... args)
			-> std::unique_ptr<type>;
	};

	/*
		java data structure helpers
	*/

	// own wrapper of java Collection, you can override it if you really need
	class collection : public jni::object
	{
	public:
		explicit collection(const jobject instance = nullptr)
			: jni::object{ instance }
		{

		}

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

	// wrapper for java List, inherite to_vector
	// same as collection but no new methods for now
	class list final : public jni::collection
	{
	public:
		explicit list(const jobject instance = nullptr)
			: jni::collection{ instance }
		{

		}
	};

	// I needed uuid for an other projet, not that useful
	class uuid final : public jni::object
	{
	public:
		explicit uuid(const jobject instance = nullptr)
			: jni::object{ instance }
		{

		}

		auto version() const
			-> int
		{
			return get_method<int>("version")->call();
		}
	};

	/*
		java data structure helpers
	*/

	// friend function of jni::object 
	// allows you to create jobject using java constructors
	template<jni_object type, callable_arg... args_t>
	auto make_unique(args_t&&... args)
		-> std::unique_ptr<type>
	{
		try
		{
			const jclass clazz{ jni::get_class(jni::class_map.at(std::type_index{ typeid(type) })) };

			if (!clazz)
			{
				throw std::runtime_error{ std::format("Class not found for constructor.") };
			}

			jni::object temp{ nullptr };
			temp.register_method_id<void, typename unwrap_object_ptr<std::remove_cvref_t<args_t>>::type...>(
				clazz, "<init>", jni::method_type::NOT_STATIC, std::type_index{ typeid(type) }
			);

			const jmethodID constructor_id{ jni::method_ids.at(typeid(type)).at("<init>") };

			auto [jargs, string_keeper] = jni::build_jargs(std::forward<args_t>(args)...);
			const jvalue* jargs_ptr{ jargs.empty() ? nullptr : jargs.data() };

			const jobject local{ jni::get_env()->NewObjectA(clazz, constructor_id, jargs_ptr) };

			std::unique_ptr<type> result{ std::make_unique<type>(local) };
			jni::get_env()->DeleteLocalRef(local);

			return result;
		}
		catch (const std::exception& e)
		{
			std::println("[ERROR] make_unique() {}", e.what());

			return std::make_unique<type>(nullptr);
		}
	}

	inline const constexpr DWORD versions[] = 
	{
		JVMTI_VERSION_1_2,
		JVMTI_VERSION_1_1,
		JVMTI_VERSION_1_0,
		JVMTI_VERSION
	};

	// initializes EasyJNI
	static auto init()
		-> bool
	{
		try
		{
			jsize count{};
			if (JNI_GetCreatedJavaVMs(&jni::vm, 1, &count) != JNI_OK)
			{
				throw std::runtime_error{ "Failed to get created Java VMs." };
			}

			if (!jni::vm)
			{
				throw std::runtime_error{ "No Java VM found." };
			}

			JNIEnv* env{};
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

			// already register some java datastructures that have helper methods
			jni::register_class<jni::collection>("java/util/Collection");
			jni::register_class<jni::list>("java/util/List");
			jni::register_class<jni::uuid>("java/util/UUID");

			return true;
		}
		catch (const std::exception& e)
		{
			std::println("[ERROR] init() {}", e.what());
			return false;
		}
	}

	// shutdown EasyJNI, detach the current thread from the JVM and clear the map of envs and classes to prevent memory leaks
	static auto shutdown()
		-> void
	{
		std::println("[INFO] shutdown() Shutdown");

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