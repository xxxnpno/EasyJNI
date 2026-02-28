#pragma once

#include <print>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <type_traits>
#include <typeinfo>
#include <memory>
#include <thread>

#include <Type/Type.hpp>

#include <jni/jni.h>
#include <jni/jvmti.h>

namespace EasyJNI
{
	class Object;

	auto ManageEnvs() -> void;
	auto GetClass(std::string name) -> jclass;
	auto InitOwnWrapper() -> void;

	// the maximum amount of envs we will store before clearing the map, to prevent memory leaks
	U8 maxStoredEnvs;

	JavaVM* vm{ nullptr };
	jvmtiEnv* jvmti{ nullptr };

	// associate the thread id with the env of that thread
	std::unordered_map<std::thread::id, JNIEnv*> envs;

	// associate the class name with the global reference of the class
	std::unordered_map<std::string, jclass> classes;

	// associate typeid(*this).name() with the className and signature of the class
	std::unordered_map<const char*, std::pair<std::string, std::string>> signatures;

	// associate typeid(*this).name() with the map of its fieldIDs
	std::unordered_map<const char*, std::unordered_map<std::string, jfieldID>> fieldIDs;

	// associate typeid(*this).name() with the map of its methodIDs
	std::unordered_map<const char*, std::unordered_map<std::string, jfieldID>> methodIDs;

	auto ExitThread() -> void
	{
		vm->DetachCurrentThread();
	}

	auto GetEnv() -> JNIEnv*
	{
		ManageEnvs();

		return envs.at(std::this_thread::get_id());
	}

	auto ManageEnvs() -> void
	{
		try
		{
			if (envs.size() >= maxStoredEnvs)
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

	auto Init(const U8 maxEnvs = 10) -> bool
	{
		maxStoredEnvs = maxEnvs;

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

			ManageEnvs();

			if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK)
			{
				throw std::runtime_error{ "Failed to get JVMTI environment." };
			}

			InitOwnWrapper();

			return true;
		}
		catch (const std::runtime_error& e)
		{
			std::println("[ERROR] {}", e.what());
			return false;
		}
	}

	auto Shutdown() -> void
	{
		std::println("[INFO] Shutdown");

		for (const auto& [_, clazz] : classes)
		{
			if (clazz)
			{
				GetEnv()->DeleteGlobalRef(clazz);
			}
		}

		ExitThread();
	}

	// take a class name a create a global reference to the class
	auto LoadClass(const std::string& name) -> jclass
	{
		try
		{
			const jclass clazz{ GetEnv()->FindClass("java/lang/Class") };
			const jmethodID getNameID{ GetEnv()->GetMethodID(clazz, "getName", "()Ljava/lang/String;") };

			jint amount{};
			jclass* classesPtr{};
			if (jvmti->GetLoadedClasses(&amount, &classesPtr) != JVMTI_ERROR_NONE)
			{
				throw std::runtime_error{ "Failed to get loaded classes." };
			}

			for (jint i{ 0 }; i < amount; ++i)
			{
				const jclass currentClazz{ classesPtr[i] };

				const jstring className{ static_cast<jstring>(GetEnv()->CallObjectMethod(currentClazz, getNameID)) };
				const char* const classNameCStr{ GetEnv()->GetStringUTFChars(className, nullptr) };

				if (not std::strcmp(classNameCStr, name.c_str()))
				{
					classes.insert({ name, static_cast<jclass>(GetEnv()->NewGlobalRef(currentClazz)) });
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

	auto GetClass(std::string name) -> jclass
	{
		std::replace(name.begin(), name.end(), '/', '.');

		if (const auto it{ classes.find(name) }; it != classes.end())
		{
			return it->second;
		}

		return LoadClass(name);
	}

	enum class FieldType : I8
	{
		Static,
		NotStatic
	};

	template<typename T>
	struct IsJNIType : std::false_type {};

	template<> struct IsJNIType<jint> : std::true_type {};
	template<> struct IsJNIType<jfloat> : std::true_type {};
	template<> struct IsJNIType<jdouble> : std::true_type {};
	template<> struct IsJNIType<jlong> : std::true_type {};
	template<> struct IsJNIType<jboolean> : std::true_type {};
	template<> struct IsJNIType<jchar> : std::true_type {};
	template<> struct IsJNIType<jshort> : std::true_type {};
	template<> struct IsJNIType<jbyte> : std::true_type {};
	template<> struct IsJNIType<jstring> : std::true_type {};
	template<> struct IsJNIType<jobject> : std::true_type {};

	class String final
	{
	public:
		explicit String(const std::string string = "")
			: string{ string }
		{
			this->jstring_ = static_cast<jstring>(GetEnv()->NewGlobalRef(GetEnv()->NewStringUTF(string.c_str())));
		}

		~String()
		{
			GetEnv()->DeleteGlobalRef(this->jstring_);
		}

		auto GetJString() const -> jstring
		{
			return this->jstring_;
		}

		auto GetString() const -> std::string
		{
			return this->string;
		}

	private:
		jstring jstring_;

		std::string string;
	};

	template<typename InputType, typename OutputType>
	auto PrimitiveTypeManager(const InputType& value) -> OutputType
	{
		if constexpr (IsJNIType<InputType>::value)
		{
			if constexpr (std::is_same<InputType, jstring>::value)
			{
				const char* chars{ GetEnv()->GetStringUTFChars(value, nullptr) };
				const std::string output{ chars };

				GetEnv()->ReleaseStringUTFChars(value, chars);

				return output;
			}
			else
			{
				return static_cast<OutputType>(value);
			}
		}
		else
		{
			if constexpr (std::is_same<InputType, std::string>::value)
			{
				return String{ value };
			}
			else
			{
				return static_cast<OutputType>(value);
			}
		}
	}
	
	template<typename Type, class Caller>
	requires (std::is_base_of_v<Object, Caller>)
	auto AddFieldID(const jclass clazz, const std::string& fieldName, const char* objectTypeid = "primitiveType", EasyJNI::FieldType fieldType = EasyJNI::FieldType::NotStatic) -> void
	{
		if (fieldIDs[typeid(Caller).name()].find(fieldName) != fieldIDs[typeid(Caller).name()].end())
		{
			return;
		}
		
		std::string signature{};
		if (std::strcmp(objectTypeid, "primitiveType"))
		{
			signature = signatures.at(objectTypeid).second;
		}
		else
		{
			if constexpr (std::is_same<Type, I32>::value)
			{
				signature = "I";
			}
			else if constexpr (std::is_same<Type, F32>::value)
			{
				signature = "F";
			}
			else if constexpr (std::is_same<Type, F64>::value)
			{
				signature = "D";
			}
			else if constexpr (std::is_same<Type, I64>::value)
			{
				signature = "J";
			}
			else if constexpr (std::is_same<Type, bool>::value)
			{
				signature = "Z";
			}
			else if constexpr (std::is_same<Type, char>::value)
			{
				signature = "C";
			}
			else if constexpr (std::is_same<Type, I8>::value)
			{
				signature = "S";
			}
			else if constexpr (std::is_same<Type, char>::value)
			{
				signature = "B";
			}
			else if constexpr (std::is_same<Type, std::string>::value)
			{
				signature = "Ljava/lang/String;";
			}
		}

		if (fieldType == EasyJNI::FieldType::Static)
		{
			fieldIDs[typeid(Caller).name()].insert({fieldName, GetEnv()->GetStaticFieldID(clazz, fieldName.c_str(), signature.c_str())});
		}
		else
		{
			fieldIDs[typeid(Caller).name()].insert({ fieldName, GetEnv()->GetFieldID(clazz, fieldName.c_str(), signature.c_str()) });
		}
	}

	auto AddMethodID() -> void
	{

	}

	template<typename Type, class Caller>
	requires (std::is_base_of_v<Object, Caller>)
	auto GetField_(const std::string& fieldName, const void* classOrInstance, EasyJNI::FieldType fieldType = EasyJNI::FieldType::NotStatic) -> std::conditional_t<std::is_base_of_v<Object, Type>, std::unique_ptr<Type>, Type>
	{
		const jfieldID fieldID{ fieldIDs[typeid(Caller).name()].at(fieldName) };

		if (fieldType == EasyJNI::FieldType::NotStatic)
		{
			const jobject instance{ static_cast<jobject>(const_cast<void*>(classOrInstance)) };

			if constexpr (std::is_same<Type, I32>::value)
			{
				return PrimitiveTypeManager<jint, I32>(GetEnv()->GetIntField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, F32>::value)
			{
				return PrimitiveTypeManager<jfloat, F32>(GetEnv()->GetFloatField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, F64>::value)
			{
				return PrimitiveTypeManager<jdouble, F64>(GetEnv()->GetDoubleField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, I64>::value)
			{
				return PrimitiveTypeManager<jlong, I64>(GetEnv()->GetLongField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, bool>::value)
			{
				return PrimitiveTypeManager<jboolean, bool>(GetEnv()->GetBooleanField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, char>::value)
			{
				return PrimitiveTypeManager<jchar, char>(GetEnv()->GetCharField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, I8>::value)
			{
				return PrimitiveTypeManager<jshort, I8>(GetEnv()->GetShortField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, char>::value)
			{
				return PrimitiveTypeManager<jbyte, char>(GetEnv()->GetByteField(instance, fieldID));
			}
			else if constexpr (std::is_same<Type, std::string>::value)
			{
				return PrimitiveTypeManager<jstring, std::string>(GetEnv()->GetObjectField(instance, fieldID));
			}
			else if constexpr (std::is_base_of_v<Object, Type>)
			{
				return std::make_unique<Type>(GetEnv()->GetObjectField(instance, fieldID));
			}
		}
		else
		{
			const jclass clazz{ static_cast<jclass>(const_cast<void*>(classOrInstance)) };

			if constexpr (std::is_same<Type, I32>::value)
			{
				return PrimitiveTypeManager<jint, I32>(GetEnv()->GetStaticIntField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, F32>::value)
			{
				return PrimitiveTypeManager<jfloat, F32>(GetEnv()->GetStaticFloatField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, F64>::value)
			{
				return PrimitiveTypeManager<jdouble, F64>(GetEnv()->GetStaticDoubleField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, I64>::value)
			{
				return PrimitiveTypeManager<jlong, I64>(GetEnv()->GetStaticLongField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, bool>::value)
			{
				return PrimitiveTypeManager<jboolean, bool>(GetEnv()->GetStaticBooleanField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, char>::value)
			{
				return PrimitiveTypeManager<jchar, char> (GetEnv()->GetStaticCharField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, I8>::value)
			{
				return PrimitiveTypeManager<jshort, I8>(GetEnv()->GetStaticShortField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, char>::value)
			{
				return PrimitiveTypeManager<jbyte, char>(GetEnv()->GetStaticByteField(clazz, fieldID));
			}
			else if constexpr (std::is_same<Type, std::string>::value)
			{
				return PrimitiveTypeManager<jstring, std::string>(GetEnv()->GetStaticObjectField(clazz, fieldID));
			}
			else if constexpr (std::is_base_of_v<Object, Type>)
			{
				return std::make_unique<Type>(GetEnv()->GetStaticObjectField(clazz, fieldID));
			}
		}

		if constexpr (std::is_base_of_v<Object, Type>)
		{
			return std::make_unique<Type>(nullptr);
		}
		else
		{
			return Type{};
		}
	}

	template<typename Type, class Caller>
	requires (std::is_base_of_v<Object, Caller>)
	auto SetField_(const std::string& fieldName, const Type& value, const void* classOrInstance, EasyJNI::FieldType fieldType = EasyJNI::FieldType::NotStatic) -> void
	{
		const jfieldID fieldID{ fieldIDs[typeid(Caller).name()].at(fieldName) };

		if (fieldType == EasyJNI::FieldType::NotStatic)
		{
			const jobject instance{ static_cast<jobject>(const_cast<void*>(classOrInstance)) };

			if constexpr (std::is_same<Type, jint>::value)
			{
				GetEnv()->SetIntField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jfloat>::value)
			{
				GetEnv()->SetFloatField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jdouble>::value)
			{
				GetEnv()->SetDoubleField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jlong>::value)
			{
				GetEnv()->SetLongField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jboolean>::value)
			{
				GetEnv()->SetBooleanField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jchar>::value)
			{
				GetEnv()->SetCharField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jshort>::value)
			{
				GetEnv()->SetShortField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jbyte>::value)
			{
				GetEnv()->SetByteField(instance, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jstring>::value)
			{
				GetEnv()->SetObjectField(instance, fieldID, value);
			}
			else if constexpr (std::is_base_of_v<Object, Type>)
			{
				std::make_unique<Type>(GetEnv()->SetObjectField(instance, fieldID, value));
			}
		}
		else
		{
			const jclass clazz{ static_cast<jclass>(const_cast<void*>(classOrInstance)) };

			if constexpr (std::is_same<Type, jint>::value)
			{
				GetEnv()->SetStaticIntField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jfloat>::value)
			{
				GetEnv()->SetStaticFloatField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jdouble>::value)
			{
				GetEnv()->SetStaticDoubleField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jlong>::value)
			{
				GetEnv()->SetStaticLongField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jboolean>::value)
			{
				GetEnv()->SetStaticBooleanField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jchar>::value)
			{
				GetEnv()->SetStaticCharField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jshort>::value)
			{
				GetEnv()->SetStaticShortField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jbyte>::value)
			{
				GetEnv()->SetStaticByteField(clazz, fieldID, value);
			}
			else if constexpr (std::is_same<Type, jstring>::value)
			{
				GetEnv()->SetStaticObjectField(clazz, fieldID, value);
			}
			else if constexpr (std::is_base_of_v<Object, Type>)
			{
				std::make_unique<Type>(GetEnv()->SetStaticObjectField(clazz, fieldID, value));
			}
		}
	}

	template<class Caller>
	requires (std::is_base_of_v<Object, Caller>)
	auto RegisterClass(const std::string& className) -> void
	{
		signatures.insert({ typeid(Caller).name(), { className, std::format("L{};", className) } });
	}

	class Object
	{
	public:
		explicit Object(const jobject instance = nullptr)
			: instance{ instance ? GetEnv()->NewGlobalRef(instance) : nullptr }
		{
			
		}

		virtual ~Object()
		{
			if (this->instance)
			{
				GetEnv()->DeleteGlobalRef(this->instance);
			}
		}

		template<typename Type, class Caller>
		requires (std::is_base_of_v<Object, Caller>)
		auto GetField(const std::string& fieldName) -> std::conditional_t<std::is_base_of_v<Object, Type>, std::unique_ptr<Type>, Type>
		{
			try
			{
				const jclass clazz{ GetClass(signatures.at(typeid(Caller).name()).first) };
				if (not clazz)
				{
					throw std::runtime_error{ "Class not found." };
				}

				if (not this->instance)
				{
					throw std::runtime_error{ "Instance is null." };
				}

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					AddFieldID<Type, Caller>(clazz, fieldName, typeid(Type).name(), EasyJNI::FieldType::NotStatic);
				}
				else
				{
					AddFieldID<Type, Caller>(clazz, fieldName, "primitiveType", EasyJNI::FieldType::NotStatic);
				}

				return GetField_<Type, Caller>(fieldName, this->instance, EasyJNI::FieldType::NotStatic);
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					return std::make_unique<Type>(nullptr);
				}
				else
				{
					return Type{};
				}
			}
		}

		template<typename Type, class Caller>
		requires (std::is_base_of_v<Object, Caller>)
		auto SetField(const std::string& fieldName, const Type& value) -> void
		{
			try
			{
				const jclass clazz{ GetClass(signatures.at(typeid(Caller).name()).first) };
				if (not clazz)
				{
					throw std::runtime_error{ "Class not found." };
				}

				if (not this->instance)
				{
					throw std::runtime_error{ "Instance is null." };
				}

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					AddFieldID<Type, Caller>(clazz, fieldName, typeid(Type).name(), EasyJNI::FieldType::NotStatic);
				}
				else
				{
					AddFieldID<Type, Caller>(clazz, fieldName, "primitiveType", EasyJNI::FieldType::NotStatic);
				}

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					SetField_<Type, Caller>(fieldName, value->GetInstance(), this->instance, EasyJNI::FieldType::NotStatic);
				}
				else
				{
					SetField_<Type, Caller>(fieldName, value, this->instance, EasyJNI::FieldType::NotStatic);
				}
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
			}
		}

		template<typename Type, class Caller>
		requires (std::is_base_of_v<Object, Caller>)
		static auto GetStaticField(const std::string& fieldName) -> std::conditional_t<std::is_base_of_v<Object, Type>, std::unique_ptr<Type>, Type>
		{
			try
			{
				const jclass clazz{ GetClass(signatures.at(typeid(Caller).name()).first) };
				if (not clazz)
				{
					throw std::runtime_error{ "Class not found." };
				}

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					AddFieldID<Type, Caller>(clazz, fieldName, typeid(Type).name(), EasyJNI::FieldType::Static);
				}
				else
				{
					AddFieldID<Type, Caller>(clazz, fieldName, "primitiveType", EasyJNI::FieldType::Static);
				}

				return GetField_<Type, Caller>(fieldName, clazz, EasyJNI::FieldType::Static);
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
				return nullptr;
			}
		}

		template<typename Type, class Caller>
		requires (std::is_base_of_v<Object, Caller>)
		static auto SetStaticField(const std::string& fieldName, const Type& value) -> void
		{
			try
			{
				const jclass clazz{ GetClass(signatures.at(typeid(Caller).name()).first) };
				if (not clazz)
				{
					throw std::runtime_error{ "Class not found." };
				}

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					AddFieldID<Type, Caller>(clazz, fieldName, typeid(Type).name(), EasyJNI::FieldType::Static);
				}
				else
				{
					AddFieldID<Type, Caller>(clazz, fieldName, "primitiveType", EasyJNI::FieldType::Static);
				}

				if constexpr (std::is_base_of_v<Object, Type>)
				{
					SetField_<Type, Caller>(fieldName, value->GetInstance(), clazz, EasyJNI::FieldType::Static);
				}
				else
				{
					SetField_<Type, Caller>(fieldName, value, clazz, EasyJNI::FieldType::Static);
				}
				
			}
			catch (const std::runtime_error& e)
			{
				std::println("[ERROR] {}", e.what());
			}
		}

		template<typename Type, class Caller, typename... Args>
		requires (std::is_base_of_v<Object, Caller>)
		auto CallMethod(const std::string& methodName, Args&&... args) -> std::conditional_t<std::is_base_of_v<Object, Type>, std::unique_ptr<Type>, Type>
		{

		}

		auto GetInstance() const -> jobject
		{
			return this->instance;
		}

	protected:
		jobject instance;
	};

	namespace Wrapper
	{
		template<typename Type>
		class Collection : public Object
		{
		public:
			Collection(const jobject instance = nullptr)
				: Object{ instance }
			{

			}
		};
	}

	auto InitOwnWrapper() -> void
	{
		EasyJNI::RegisterClass<Wrapper::Collection<void>>("java/util/Collection");
	}
}