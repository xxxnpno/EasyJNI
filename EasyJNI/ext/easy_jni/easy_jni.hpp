#pragma once

#include <cstdint>
#include <cstdlib>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <type_traits>
#include <typeinfo>
#include <typeindex>
#include <functional>
#include <any>
#include <utility>
#include <memory>
#include <thread>

#include <jni/jni.h>
#include <jni/jvmti.h>

namespace jni
{
	class object;

	auto manage_envs() -> void;

	// global vm and jvmti
	JavaVM* vm{ nullptr };
	jvmtiEnv* jvmti{ nullptr };

	// the maximum amount of envs we will store before clearing the map, to prevent memory leaks
	std::uint8_t max_stored_envs;

	// associate the thread id with the env of that thread
	std::unordered_map<std::thread::id, JNIEnv*> envs;

	// get the env of the current thread, if the env is not found, attach the thread to the JVM and store the env in the map
	auto get_env() 
		-> JNIEnv*
	{
		manage_envs();

		return envs.at(std::this_thread::get_id());
	}

	// manage the envs of the threads, if the current thread is not in the map, 
	// attach it to the JVM and store the env in the map, if the map is full, clear it to prevent memory leaks
	auto manage_envs() 
		-> void
	{
		try
		{
			if (envs.size() >= max_stored_envs)
			{
				envs.clear();
			}

			const std::thread::id id{ std::this_thread::get_id() };
			const auto it{ envs.find(id) };

			if (it == envs.end())
			{
				JNIEnv* env{ nullptr };
				const jint result{ vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) };

				if (result == JNI_EDETACHED)
				{
					if (vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK)
					{
						throw std::runtime_error{ "Failed to attach current thread to JVM." };
					}
				}

				envs.insert({ id, env });
			}
		}
		catch (const std::runtime_error& e)
		{
			std::println("[ERROR] {}", e.what());
		}
	}

	// detach the current thread from the JVM, should be called when the thread is done using the JVM to prevent memory leaks
	auto exit_thread() 
		-> void
	{
		vm->DetachCurrentThread();

		envs.erase(std::this_thread::get_id());
	}

	// associate the class name with the global reference of the class
	std::unordered_map<std::string, jclass> classes;

	// take a class name a create a global reference to the class
	auto load_class (const std::string& name) 
		-> jclass
	{
		try
		{
			const jclass clazz{ get_env()->FindClass("java/lang/Class") };
			const jmethodID get_name_id{ get_env()->GetMethodID(clazz, "getName", "()Ljava/lang/String;") };

			jint amount{};
			jclass* classes_ptr{};
			if (jvmti->GetLoadedClasses(&amount, &classes_ptr) != JVMTI_ERROR_NONE)
			{
				throw std::runtime_error{ "Failed to get loaded classes." };
			}

			for (jint i{ 0 }; i < amount; ++i)
			{
				const jclass current_class{ classes_ptr[i] };

				const jstring class_name{ static_cast<jstring>(get_env()->CallObjectMethod(current_class, get_name_id)) };
				const char* const class_nams_c_str{ get_env()->GetStringUTFChars(class_name, nullptr) };

				if (not std::strcmp(class_nams_c_str, name.c_str()))
				{
					classes.insert({ name, static_cast<jclass>(get_env()->NewGlobalRef(current_class)) });
					return classes.at(name);
				}
			}

			return nullptr;
		}
		catch (const std::runtime_error& e)
		{
			std::println("[ERROR] {}", e.what());
			return nullptr;
		}
	}

	// get the global reference of a class from its name, if the class is not found, return nullptr
	auto get_class(std::string name) 
		-> jclass
	{
		std::replace(name.begin(), name.end(), '/', '.');

		if (const auto it{ classes.find(name) }; it != classes.end())
		{
			return it->second;
		}

		return load_class(name);
	}

	// handle strings
	class string final
	{
	public:
		explicit string(const std::string& std_string = "")
			: jni_string{ nullptr }
			, std_string{ std_string }
		{
			const jstring local{ get_env()->NewStringUTF(std_string.c_str()) };

			if (local)
			{
				this->jni_string = static_cast<jstring>(get_env()->NewGlobalRef(local));
				get_env()->DeleteLocalRef(local);
			}
		}

		explicit string(const jstring jni_string = nullptr)
			: jni_string{ nullptr }
		{
			if (jni_string)
			{
				this->jni_string = static_cast<jstring>(get_env()->NewGlobalRef(jni_string));

				const char* chars{ get_env()->GetStringUTFChars(this->jni_string, nullptr) };
				this->std_string = std::string{ chars };
				get_env()->ReleaseStringUTFChars(this->jni_string, chars);
			}
		}

		~string()
		{
			if (this->jni_string)
			{
				get_env()->DeleteGlobalRef(this->jni_string);
			}
		}

		auto get_jni_string() const
			-> jstring
		{
			return this->jni_string;
		}

		auto get_std_string() const
			-> std::string
		{
			return this->std_string;
		}

	private:
		jstring jni_string;

		std::string std_string;
	};

	using convert_function = std::function<std::any(const std::any&)>;

	// associate a jni type to a cpp one
	const std::unordered_map<std::type_index, convert_function> jni_to_cpp =
	{
		{ typeid(jshort),   [](const std::any& v) -> std::any { return static_cast<short>(std::any_cast<jshort>(v)); } },
		{ typeid(jint),     [](const std::any& v) -> std::any { return static_cast<int>(std::any_cast<jint>(v)); } },
		{ typeid(jlong),    [](const std::any& v) -> std::any { return static_cast<long long>(std::any_cast<jlong>(v)); } },
		{ typeid(jfloat),   [](const std::any& v) -> std::any { return static_cast<float>(std::any_cast<jfloat>(v)); } },
		{ typeid(jdouble),  [](const std::any& v) -> std::any { return static_cast<double>(std::any_cast<jdouble>(v)); } },
		{ typeid(jboolean), [](const std::any& v) -> std::any { return static_cast<bool>(std::any_cast<jboolean>(v)); } },
		{ typeid(jchar),    [](const std::any& v) -> std::any { return static_cast<char>(std::any_cast<jchar>(v)); } },
		{ typeid(jstring),  [](const std::any& v) -> std::any { return string(std::any_cast<jstring>(v)).get_std_string(); } },
	};

	// associate a cpp type to a jni one
	const std::unordered_map<std::type_index, convert_function> cpp_to_jni =
	{
		{ typeid(short),       [](const std::any& v) -> std::any { return static_cast<jshort>(std::any_cast<short>(v)); } },
		{ typeid(int),         [](const std::any& v) -> std::any { return static_cast<jint>(std::any_cast<int>(v)); } },
		{ typeid(long long),   [](const std::any& v) -> std::any { return static_cast<jlong>(std::any_cast<long long>(v)); } },
		{ typeid(float),       [](const std::any& v) -> std::any { return static_cast<jfloat>(std::any_cast<float>(v)); } },
		{ typeid(double),      [](const std::any& v) -> std::any { return static_cast<jdouble>(std::any_cast<double>(v)); } },
		{ typeid(bool),        [](const std::any& v) -> std::any { return static_cast<jboolean>(std::any_cast<bool>(v)); } },
		{ typeid(char),        [](const std::any& v) -> std::any { return static_cast<jchar>(std::any_cast<char>(v)); } },
		{ typeid(std::string), [](const std::any& v) -> std::any { return string(std::any_cast<std::string>(v)).get_jni_string(); } },
	};

	using jvalue_setter = std::function<jvalue(const std::any&)>;

	// associate a jni type to a function that takes a cpp type and returns a jvalue with the corresponding field set, 
	// used to call methods with arguments or to set fields
	const std::unordered_map<std::type_index, jvalue_setter> jni_to_jvalue =
	{
		{ typeid(jshort),   [](const std::any& v) -> jvalue { jvalue j{}; j.s = std::any_cast<jshort>(v);   return j; } },
		{ typeid(jint),     [](const std::any& v) -> jvalue { jvalue j{}; j.i = std::any_cast<jint>(v);     return j; } },
		{ typeid(jlong),    [](const std::any& v) -> jvalue { jvalue j{}; j.j = std::any_cast<jlong>(v);    return j; } },
		{ typeid(jfloat),   [](const std::any& v) -> jvalue { jvalue j{}; j.f = std::any_cast<jfloat>(v);   return j; } },
		{ typeid(jdouble),  [](const std::any& v) -> jvalue { jvalue j{}; j.d = std::any_cast<jdouble>(v);  return j; } },
		{ typeid(jboolean), [](const std::any& v) -> jvalue { jvalue j{}; j.z = std::any_cast<jboolean>(v); return j; } },
		{ typeid(jchar),    [](const std::any& v) -> jvalue { jvalue j{}; j.c = std::any_cast<jchar>(v);    return j; } },
		{ typeid(jstring),  [](const std::any& v) -> jvalue { jvalue j{}; j.l = std::any_cast<jstring>(v);  return j; } },
		{ typeid(jobject),  [](const std::any& v) -> jvalue { jvalue j{}; j.l = std::any_cast<jobject>(v);  return j; } },
	};

	// associate a cpp type to a jni signature
	const std::unordered_map<std::type_index, std::string> signature_map
	{
		{ std::type_index{ typeid(short) }, "S" },
		{ std::type_index{ typeid(int) }, "I" },
		{ std::type_index{ typeid(long long) }, "J" },
		{ std::type_index{ typeid(float) }, "F" },
		{ std::type_index{ typeid(double) }, "D" },
		{ std::type_index{ typeid(bool) }, "Z" },
		{ std::type_index{ typeid(char) }, "C" },
		{ std::type_index{ typeid(std::string) }, "Ljava/lang/String;" },
		{ std::type_index{ typeid(void) }, "V" },
	};

	template<typename type>
	struct is_std_type : public std::false_type {};

	template<> struct is_std_type<short> : public std::true_type {};
	template<> struct is_std_type<int> : public std::true_type {};
	template<> struct is_std_type<long long> : public std::true_type {};

	template<> struct is_std_type<float> : public std::true_type {};
	template<> struct is_std_type<double> : public std::true_type {};

	template<> struct is_std_type<bool> : public std::true_type {};
	template<> struct is_std_type<char> : public std::true_type {};

	template<> struct is_std_type<std::string> : public std::true_type {};

	template<> struct is_std_type<void> : public std::true_type {};

	template<typename type>
	struct is_jni_type : public std::false_type {};

	template<> struct is_jni_type<jshort> : public std::true_type {};
	template<> struct is_jni_type<jint> : public std::true_type {};
	template<> struct is_jni_type<jlong> : public std::true_type {};

	template<> struct is_jni_type<jfloat> : public std::true_type {};
	template<> struct is_jni_type<jdouble> : public std::true_type {};

	template<> struct is_jni_type<jboolean> : public std::true_type {};
	template<> struct is_jni_type<jchar> : public std::true_type {};
	template<> struct is_jni_type<jbyte> : public std::true_type {};

	template<> struct is_jni_type<jstring> : public std::true_type {};
	template<> struct is_jni_type<jobject> : public std::true_type {};

	template<typename T>
	struct is_object_ptr : std::false_type {};

	template<typename T>
		requires (std::is_base_of_v<object, T>)
	struct is_object_ptr<std::unique_ptr<T>> : std::true_type {};

	// associate the type index of a cpp type to the signature of the corresponding java type, used to get the signature of a method or a field from the cpp type
	std::unordered_map< std::type_index, std::string> class_map;

	// associate your cpp class to a java class
	template <typename type>
		requires (std::is_base_of_v<object, type>)
	auto register_class (const std::string& class_name) 
		-> void
	{
		class_map.insert_or_assign(std::type_index{ typeid(type) }, class_name);
	}

	template <typename type>
		requires (is_std_type<type>::value or std::is_base_of_v<object, type>)
	auto get_signature() 
		-> std::string
	{
		if constexpr (std::is_base_of_v<object, type>)
		{
			return std::format("L{};", class_map.at(std::type_index{ typeid(type) }));
		}
		else
		{
			return signature_map.at(std::type_index{ typeid(type) });
		}
	}

	// associate typeid(*this).name() with the map of its fieldIDs
	std::unordered_map<std::type_index, std::unordered_map<std::string, jfieldID>> field_ids;

	// used to know if a field is static or not
	enum class field_type : std::int8_t
	{
		STATIC,
		NOT_STATIC
	};

	// used to represent a field and to get and set the field
	template <typename type>
		requires (is_std_type<type>::value or std::is_base_of_v<object, type>)
	class field final
	{
	public:
		field(void* class_or_instance, const std::string& name, const field_type field_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, field_type{ field_type }
			, index{ index }
		{

		}

		// get the field value, if the field is static, class_or_instance will be a jclass, otherwise it will be a jobject
		auto get() const 
			-> std::conditional_t<std::is_base_of_v<object, type>, std::unique_ptr<type>, type>
		{
			try
			{
				const jfieldID field_id{ field_ids.at(this->index).find(this->name)->second };

				if (not field_id)
				{
					throw std::runtime_error{ "Failed to get method id." };
				}

				jobject local{};
				if constexpr (std::is_base_of_v<object, type>)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						local = get_env()->GetObjectField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						local = get_env()->GetStaticObjectField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}

					std::unique_ptr<type> result{ std::make_unique<type>(local) };

					if (local)
					{
						get_env()->DeleteLocalRef(local);
					}

					return result;
				}

				std::any result{};
				if constexpr (std::is_same<type, short>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetShortField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticShortField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, int>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetIntField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticIntField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, long long>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetLongField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticLongField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, float>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetFloatField(
							const_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticFloatField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, double>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetDoubleField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticDoubleField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, bool>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetBooleanField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticBooleanField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, char>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = get_env()->GetCharField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id
						);
					}
					else
					{
						result = get_env()->GetStaticCharField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id
						);
					}
				}
				else if constexpr (std::is_same<type, std::string>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						result = string(
							static_cast<jstring>(get_env()->GetObjectField(
								reinterpret_cast<jobject>(this->class_or_instance),
								field_id)
								)
							).get_std_string();
					}
					else
					{
						result = string(
							static_cast<jstring>(get_env()->GetStaticObjectField(
								reinterpret_cast<jclass>(this->class_or_instance),
								field_id)
								)
							).get_std_string();
					}
				}

				if constexpr (not std::is_base_of_v<object, type>)
				{
					auto it = jni_to_cpp.find(result.type());
					if (it != jni_to_cpp.end())
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
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());

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

		// set the field value, if the field is static, class_or_instance will be a jclass, otherwise it will be a jobject
		auto set(const type& value) const 
			-> void
		{
			try
			{
				const jfieldID field_id{ field_ids.at(this->index).find(this->name)->second };

				if (not field_id)
				{
					throw std::runtime_error{ "Failed to get method id." };
				}

				if constexpr (std::is_base_of_v<object, type>)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetObjectField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							value->get_instance()
						);
					}
					else
					{
						get_env()->SetStaticObjectField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							value->get_instance()
						);
					}
				}

				if constexpr (std::is_same<type, short>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetShortField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jshort>(value)
						);
					}
					else
					{
						get_env()->SetStaticShortField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jshort>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, int>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetIntField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jint>(value)
						);
					}
					else
					{
						get_env()->SetStaticIntField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jint>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, long long>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetLongField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jlong>(value)
						);
					}
					else
					{
						get_env()->SetStaticLongField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jlong>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, float>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetFloatField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jfloat>(value)
						);
					}
					else
					{
						get_env()->SetStaticFloatField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jfloat>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, double>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetDoubleField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jdouble>(value)
						);
					}
					else
					{
						get_env()->SetStaticDoubleField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jdouble>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, bool>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetBooleanField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jboolean>(value)
						);
					}
					else
					{
						get_env()->SetStaticBooleanField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jboolean>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, char>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetCharField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							static_cast<jchar>(value)
						);
					}
					else
					{
						get_env()->SetStaticCharField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							static_cast<jchar>(value)
						);
					}
				}
				else if constexpr (std::is_same<type, std::string>::value)
				{
					if (this->field_type == field_type::NOT_STATIC)
					{
						get_env()->SetObjectField(
							reinterpret_cast<jobject>(this->class_or_instance),
							field_id,
							string(value).get_jni_string()
						);
					}
					else
					{
						get_env()->SetStaticObjectField(
							reinterpret_cast<jclass>(this->class_or_instance),
							field_id,
							string(value).get_jni_string()
						);
					}
				}
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
			}
		}

	private:
		// store the class or instance depending on if the field is static or not, to prevent having to pass it every time we want to get or set the field
		void* class_or_instance;

		std::string name;

		field_type field_type;

		std::type_index index;
	};

	// associate typeid(*this).name() with the map of its methodIDs
	std::unordered_map<std::type_index, std::unordered_map<std::string, jmethodID>> method_ids;

	// used to know if a method is static or not
	enum class method_type : std::uint8_t
	{
		STATIC,
		NOT_STATIC
	};

	// used to represent a method and to call the method
	template <typename return_type>
		requires (is_std_type<return_type>::value or std::is_base_of_v<object, return_type>)
	class method final
	{
	public:
		method(void* class_or_instance, const std::string& name, const method_type method_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, method_type{ method_type }
			, index{ index }
		{

		}

		// call the method with the given arguments, if the method is static, class_or_instance will be a jclass, otherwise it will be a jobject
		template<typename... args_t>
			requires (
				(
					is_std_type<std::remove_cvref_t<args_t>>::value
					or std::is_base_of_v<object, std::remove_cvref_t<args_t>>
					or is_object_ptr<std::remove_cvref_t<args_t>>::value
				) and ...
			)
		auto call(args_t&&... args) const 
			-> std::conditional_t<std::is_base_of_v<object, return_type>, std::unique_ptr<return_type>, return_type>
		{
			try
			{
				const jmethodID method_id{ method_ids.at(this->index).find(this->name)->second };

				if (not method_id)
				{
					throw std::runtime_error{ "Failed to get method id." };
				}

				auto convert_arg = [](auto&& arg) -> jvalue
				{
					using raw = std::remove_cvref_t<decltype(arg)>;

					std::any jni_val{};
					if constexpr (std::is_base_of_v<object, raw>)
					{
						jni_val = arg.get_instance();
					}
					else if constexpr (is_object_ptr<raw>::value)
					{
						jni_val = arg->get_instance();
					}
					else
					{
						jni_val = cpp_to_jni.at(std::type_index{ typeid(raw) })(arg);
					}

					return jni_to_jvalue.at(std::type_index{ jni_val.type() })(jni_val);
				};

				const jvalue* jargs_ptr{ nullptr };
				std::vector<jvalue> jargs{};

				if constexpr (sizeof...(args_t) > 0)
				{
					jargs = { convert_arg(std::forward<args_t>(args))... };
					jargs_ptr = jargs.data();
				}

				if constexpr (std::is_base_of_v<object, return_type>)
				{
					jobject local{};
					if (this->method_type == method_type::NOT_STATIC)
					{
						local = get_env()->CallObjectMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						local = get_env()->CallStaticObjectMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}

					std::unique_ptr<return_type> result{ std::make_unique<return_type>(local) };

					if (local)
					{
						get_env()->DeleteLocalRef(local);
					}

					return result;
				}

				std::any result{};
				if constexpr (std::is_same_v<return_type, void>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						get_env()->CallVoidMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						get_env()->CallStaticVoidMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					return;
				}
				if constexpr (std::is_same_v<return_type, short>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallShortMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticShortMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, int>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallIntMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticIntMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, long long>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallLongMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticLongMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, float>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallFloatMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticFloatMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, double>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallDoubleMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticDoubleMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, bool>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallBooleanMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticBooleanMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}
				else if constexpr (std::is_same_v<return_type, char>)
				{
					if (this->method_type == method_type::NOT_STATIC)
					{
						result = get_env()->CallCharMethodA(
							reinterpret_cast<jobject>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
					else
					{
						result = get_env()->CallStaticCharMethodA(
							reinterpret_cast<jclass>(this->class_or_instance),
							method_id,
							jargs_ptr
						);
					}
				}

				if constexpr (not std::is_base_of_v<object, return_type> and not std::is_same_v<return_type, void>)
				{
					auto it = jni_to_cpp.find(result.type());
					if (it != jni_to_cpp.end())
					{
						return std::any_cast<return_type>(it->second(result));
					}
					return return_type{};
				}
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());

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
		// store the class or instance depending on if the method is static or not, to prevent having to pass it every time we want to call the method
		void* class_or_instance;

		std::string name;

		method_type method_type;

		std::type_index index;
	};

	// used to represent a java object and to get and set its fields and call its methods
	// store the jobject as a global reference to prevent it from being garbage collected while we are using it
	// inherit from this class to create a wrapper for a java class
	class object
	{
	public:
		explicit object(const jobject instance = nullptr)
			: instance{ instance ? get_env()->NewGlobalRef(instance) : nullptr }
		{

		}

		virtual ~object()
		{
			if (this->instance)
			{
				get_env()->DeleteGlobalRef(this->instance);
			}
		}

		// obtain a wrapped field that you can get or set
		template<typename type>
			requires (is_std_type<type>::value or std::is_base_of_v<object, type>)
		auto get_field(const std::string& field_name, const field_type field_type = field_type::NOT_STATIC) const
			-> std::unique_ptr<field<type>>
		{
			try
			{
				const jclass clazz{ get_class(class_map.at(std::type_index{ typeid(*this) })) };

				if (not clazz)
				{
					throw std::runtime_error{ "Class not found." };
				}

				if (not this->instance and field_type == field_type::NOT_STATIC)
				{
					throw std::runtime_error{ "Instance is null." };
				}

				this->register_field_id<type>(clazz, field_name, field_type);

				return std::make_unique<field<type>>(
					field_type == field_type::NOT_STATIC ? static_cast<void*>(this->instance) : static_cast<void*>(clazz),
					field_name,
					field_type,
					std::type_index{ typeid(*this) }
				);
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
				return std::make_unique<field<type>>(nullptr, field_name, field_type, std::type_index{ typeid(*this) });
			}
		}

		// obtain a wrapped method that you can call
		template<typename return_type, typename... args_t>
			requires (
				(is_std_type<return_type>::value or std::is_base_of_v<object, return_type>)
				and
				((is_std_type<std::remove_cvref_t<args_t>>::value or std::is_base_of_v<object, std::remove_cvref_t<args_t>>) and ...)
			)
			auto get_method(const std::string& method_name, const method_type method_type = method_type::NOT_STATIC) const
			-> std::unique_ptr<method<return_type>>
		{
			try
			{
				const jclass clazz{ get_class(class_map.at(std::type_index{ typeid(*this) })) };

				if (not clazz)
				{
					throw std::runtime_error{ "Class not found." };
				}

				if (not this->instance and method_type == method_type::NOT_STATIC)
				{
					throw std::runtime_error{ "Instance is null." };
				}

				this->register_method_id<return_type, args_t...>(clazz, method_name, method_type);

				return std::make_unique<method<return_type>>(
					method_type == method_type::NOT_STATIC ? static_cast<void*>(this->instance) : static_cast<void*>(clazz),
					method_name,
					method_type,
					std::type_index{ typeid(*this) }
				);
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
				return std::make_unique<method<return_type>>(nullptr, method_name, method_type, std::type_index{ typeid(*this) });
			}
		}

		auto get_instance() const -> jobject
		{
			return this->instance;
		}

	protected:
		jobject instance;

	private:
		// obtain the fieldID of a field
		template<typename type>
			requires (is_std_type<type>::value or std::is_base_of_v<object, type>)
		auto register_field_id(const jclass clazz, const std::string& field_name, const field_type field_type) const
			-> void
		{
			try
			{
				if (field_ids[std::type_index{ typeid(*this) }].find(field_name) != field_ids[std::type_index{ typeid(*this) }].end())
				{
					return;
				}

				const std::string& signature{ get_signature<type>() };

				jfieldID field_id{};
				if (field_type == field_type::NOT_STATIC)
				{
					field_id = get_env()->GetFieldID(clazz, field_name.c_str(), signature.c_str());
				}
				else
				{
					field_id = get_env()->GetStaticFieldID(clazz, field_name.c_str(), signature.c_str());
				}

				if (not field_id)
				{
					throw std::runtime_error{ "Failed to get field id." };
				}

				field_ids[std::type_index{ typeid(*this) }].insert(
					{ field_name, field_id }
				);
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
			}
		}

		// obtain the methodID of a method
		template<typename return_type, typename... args_t>
			requires (
				(is_std_type<return_type>::value or std::is_base_of_v<object, return_type>)
				and
				((is_std_type<std::remove_cvref_t<args_t>>::value or std::is_base_of_v<object, std::remove_cvref_t<args_t>>) and ...)
			)
			auto register_method_id(const jclass clazz, const std::string& method_name, const method_type method_type) const
			-> void
		{
			try
			{
				if (method_ids[std::type_index{ typeid(*this) }].find(method_name) != method_ids[std::type_index{ typeid(*this) }].end())
				{
					return;
				}

				std::string params_sig{};
				((params_sig += get_signature<std::remove_cvref_t<args_t>>()), ...);

				const std::string signature{ std::format("({}){}", params_sig, get_signature<return_type>()) };

				jmethodID method_id{};
				if (method_type == method_type::NOT_STATIC)
				{
					method_id = get_env()->GetMethodID(clazz, method_name.c_str(), signature.c_str());
				}
				else
				{
					method_id = get_env()->GetStaticMethodID(clazz, method_name.c_str(), signature.c_str());
				}

				if (not method_id)
				{
					throw std::runtime_error{ "Failed to get method id." };
				}

				method_ids[std::type_index{ typeid(*this) }].insert(
					{ method_name, method_id }
				);
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
			}
		}
	};

	// initialize EasyJI, take the maximum amount of envs to store before clearing the map as an argument
	auto init(const std::uint8_t maxEnvs = 10) 
		-> bool
	{
		max_stored_envs = maxEnvs;

		try
		{
			jsize count{};
			if (JNI_GetCreatedJavaVMs(&vm, 1, &count) != JNI_OK)
			{
				throw std::runtime_error{ "Failed to get created Java VMs." };
			}

			if (not vm)
			{
				throw std::runtime_error{ "No Java VM found." };
			}

			manage_envs();

			if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK)
			{
				throw std::runtime_error{ "Failed to get JVMTI environment." };
			}

			return true;
		}
		catch (const std::runtime_error& e)
		{
			std::println("[ERROR] {}", e.what());
			return false;
		}
	}

	// shutdown EasyJNI, detach the current thread from the JVM and clear the map of envs and classes to prevent memory leaks
	auto shutdown() 
		-> void
	{
		std::println("[INFO] Shutdown");

		for (const auto& [_, clazz] : classes)
		{
			if (clazz)
			{
				get_env()->DeleteGlobalRef(clazz);
			}
		}

		exit_thread();
	}
}