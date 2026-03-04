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
	class string;

	// global vm and jvmti
	JavaVM* vm{ nullptr };
	jvmtiEnv* jvmti{ nullptr };

	// the maximum amount of envs we will store before clearing the map, to prevent memory leaks
	std::uint8_t max_stored_envs;

	// associate the thread id with the env of that thread
	std::unordered_map<std::thread::id, JNIEnv*> envs;

	// get the env of the current thread, if the env is not found, attach the thread to the JVM and store the env in the map
	auto get_env() -> JNIEnv*
	{
		manage_envs();

		return envs.at(std::this_thread::get_id());
	}

	// manage the envs of the threads, if the current thread is not in the map, 
	// attach it to the JVM and store the env in the map, if the map is full, clear it to prevent memory leaks
	auto manage_envs() -> void
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
	auto exit_thread() -> void
	{
		vm->DetachCurrentThread();

		envs.erase(std::this_thread::get_id());
	}

	// associate the class name with the global reference of the class
	std::unordered_map<std::string, jclass> classes;

	// take a class name a create a global reference to the class
	auto load_class (const std::string& name) -> jclass
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
	auto get_class(std::string name) -> jclass
	{
		std::replace(name.begin(), name.end(), '/', '.');

		if (const auto it{ classes.find(name) }; it != classes.end())
		{
			return it->second;
		}

		return load_class(name);
	}

	using convert_function = std::function<std::any(const std::any&)>;

	// associate a jni type to a cpp one
	const std::unordered_map<std::type_index, convert_function> type_map =
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
		{ std::type_index{ typeid(std::string) }, "Ljava/lang/String;" }
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

	// associate the type index of a cpp type to the signature of the corresponding java type, used to get the signature of a method or a field from the cpp type
	std::unordered_map< std::type_index, std::string> class_map;

	// associate your cpp class to a java class
	template <typename type>
		requires (std::is_base_of_v<object, type>)
	auto register_class (const std::string& class_name) -> void
	{
		class_map.emplace({ std::type_index{ type(Type) }, class_name });
	}

	template <typename type>
		requires (is_std_type<type>::value)
	auto get_signature() -> std::string
	{
		if constexpr (not is_base_of_v<object, type>)
		{
			signature_map.at(std::type_index{ typeid(type) });
		}

		return std::format("L{};", class_map.at(std::type_index{ typeid(type) }));
	}

	// handle strings
	class string final
	{
	public:
		explicit string(const std::string& std_string)
			: std_string{ std_string }
		{
			const jstring local{ get_env()->NewStringUTF(std_string.c_str()) };

			if (this->jni_string)
			{
				this->jni_string = static_cast<jstring>(get_env()->NewGlobalRef(local));
				get_env()->DeleteLocalRef(local);
			}
		}

		explicit string(const jstring jni_string)
		{
			this->jni_string = static_cast<jstring>(get_env()->NewGlobalRef(jni_string));

			if (this->jni_string)
			{
				const char* chars{ get_env()->GetStringUTFChars(this->jni_string, nullptr) };

				this->std_string = std::string(chars);
				get_env()->ReleaseStringUTFChars(this->jni_string, chars);
			}
		}

		~string()
		{
			if (this->jni_string)
			{
				get_env()->DeleteGlobalRef(this->std_string);
			}
		}

		auto get_jni_string() const -> jstring
		{
			return this->jni_string;
		}

		auto get_std_string() const -> std::string
		{
			return this->std_string;
		}

	private:
		jstring jni_string;

		std::string std_string;
	};

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
		field(const void* class_or_instance, const std::string& name, const field_type field_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, field_type{ field_type }
			, index{ index }
		{

		}

		// get the field value, if the field is static, classOrInstance will be a jclass, otherwise it will be a jobject
		auto get() const -> std::conditional_t<std::is_base_of_v<object, type>, std::unique_ptr<type>, type>
		{
			if constexpr (jobject local{}, std::is_base_of_v<object, type>)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					local = get_env()->GetObjectField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					local = get_env()->GetStaticObjectField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}

				const std::unique_ptr<type> result{ std::make_unique<type>(local) };

				if (local)
				{
					get_env()->DeleteLocalRef(local);
				}

				return result;
			}

			std::any result{};
			if constexpr (std::is_same<type, I8>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetShortField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticShortField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, I32>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetIntField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticIntField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, I64>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetLongField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticLongField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, F32>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetFloatField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticFloatField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, F64>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetDoubleField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticDoubleField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, bool>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetBooleanField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticBooleanField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, char>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = get_env()->GetCharField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
				else
				{
					result = get_env()->GetStaticCharField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name)
					);
				}
			}
			else if constexpr (std::is_same<type, std::string>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					result = string(
						static_cast<jstring>(get_env()->GetObjectField(const_cast<jobject>(this->class_or_instance),
							field_ids.at(this->index).find(this->name)))
					).get_std_string();
				}
				else
				{
					result = string(
						static_cast<jstring>(get_env()->GetStaticObjectField(const_cast<jclass>(this->class_or_instance),
							field_ids.at(this->index).find(this->name)))
					).get_std_string();
				}
			}

			auto it = type_map.find(jvalue.type());
			if (it != type_map.end())
			{
				const std::any cppValue{ it->second(jvalue) };
				
				return std::any_cast<type>(cppValue);
			}

			return type{};
		}

		// set the field value, if the field is static, classOrInstance will be a jclass, otherwise it will be a jobject
		auto Set(const type& value) const -> void
		{
			if constexpr (std::is_base_of_v<object, type>)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetObjectField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name), value
					);
				}
				else
				{
					get_env()->SetStaticObjectField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}

			if constexpr (std::is_same<type, short>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetShortField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticShortField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, int>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetIntField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticIntField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, long long>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetLongField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticLongField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, float>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetFloatField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticFloatField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, double>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetDoubleField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticDoubleField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, bool>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetBooleanField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticBooleanField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, char>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetCharField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticCharField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
			}
			else if constexpr (std::is_same<type, std::string>::value)
			{
				if (this->field_type == field_type::NOT_STATIC)
				{
					get_env()->SetObjectField(
						const_cast<jobject>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
				else
				{
					get_env()->SetStaticObjectField(
						const_cast<jclass>(this->class_or_instance),
						field_ids.at(this->index).find(this->name),
						value
					);
				}
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
		method(const void* class_or_instance, const std::string& name, const method_type method_type, const std::type_index index)
			: class_or_instance{ class_or_instance }
			, name{ name }
			, method_type{ method_type }
			, index{ index }
		{

		}

		// call the method with the given arguments, if the method is static, classOrInstance will be a jclass, otherwise it will be a jobject
		template<typename... args>
			requires (is_std_type<args>::value or std::is_base_of_v<object, args> and ...)
		auto call(args&&... args) const -> std::conditional_t<std::is_base_of_v<object, type>, std::unique_ptr<type>, type>
		{

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
		auto get_field(const std::string& field_name, const field_type field_type = field_type::NOT_STATIC) const -> std::unique_ptr<field<type>>
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
				return std::make_unique<field<type>>(nullptr, field_name, field_type);
			}
		}

		// obtain a wrapped method that you can call
		template<typename return_type>
			requires (is_std_type<return_type>::value or std::is_base_of_v<object, return_type>)
		auto get_method(const std::string& method_name, const method_type method_type = method_type::NOT_STATIC) const -> std::unique_ptr<method<return_type>>
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

				this->register_method_id<return_type>(clazz, method_name, method_type);

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
				return std::make_unique<method<return_type>>(nullptr, method_name, method_type);
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
		auto regsiter_field_id(const jclass clazz, const std::string& field_name, const field_type field_type) -> void
		{
			if (field_ids[std::type_index{ typeid(*this) }].find(field_name) != field_ids[std::type_index{ typeid(*this) }].end())
			{
				return;
			}

			const std::string& signature{ get_signature<type>() };

			if (field_type == field_type::NOT_STATIC)
			{
				field_ids[std::type_index{ typeid(*this) }].insert(
					{ field_name, get_env()->GetFieldID(clazz, field_name.c_str(), signature.c_str()) }
				);
			}
			else
			{
				field_ids[std::type_index{ typeid(*this) }].insert(
					{ field_name, get_env()->GetStaticFieldID(clazz, field_name.c_str(), signature.c_str()) }
				);
			}
		}

		// obtain the methodID of a method
		template<typename return_type>
			requires (is_std_type<return_type>::value or std::is_base_of_v<object, return_type>)
		auto RegisterMethodID(const jclass clazz, const std::string& method_name, const method_type method_type) -> void
		{
			if (method_ids[std::type_index{ typeid(*this) }].find(method_name) != method_ids[std::type_index{ typeid(*this) }].end())
			{
				return;
			}

			const std::string& signature{ get_signature<return_type>() };

			if (method_type == method_type::NOT_STATIC)
			{
				method_ids[std::type_index{ typeid(*this) }].insert(
					{ method_name, get_env()->GetMethodID(clazz, method_name.c_str(), signature.c_str()) }
				);
			}
			else
			{
				method_ids[std::type_index{ typeid(*this) }].insert(
					{ method_name, get_env()->GetStaticMethodID(clazz, method_name.c_str(), signature.c_str()) }
				);
			}
		}
	};

	// initialize EasyJI, take the maximum amount of envs to store before clearing the map as an argument
	auto init(const std::uint8_t maxEnvs = 10) -> bool
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
	auto shutdown() -> void
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