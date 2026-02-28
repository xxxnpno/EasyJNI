### API
```cpp
auto EasyJNI::Init(const U8 maxEnvs = 10) -> bool
auto EasyJNI::Shutdown() -> void

// use this function before closing a thread that uses EasyJNI
auto EasyJNI::ExitThread() -> void

class Object
{
public:
  template<typename Type, class Caller>
  requires ((IsJNIType<Type>::value or std::is_base_of_v<Object, Type>) and std::is_base_of_v<Object, Caller>)
  auto GetField(const std::string& fieldName) -> std::conditional_t<std::is_base_of_v<Object, Type>, std::unique_ptr<Type>, Type>

  template<typename Type, class Caller>
  requires ((IsJNIType<Type>::value or std::is_base_of_v<Object, Type>) and std::is_base_of_v<Object, Caller>)
  auto SetField(const std::string& fieldName, const Type& value) -> void

  template<typename Type, class Caller>
  requires ((IsJNIType<Type>::value or std::is_base_of_v<Object, Type>) and std::is_base_of_v<Object, Caller>)
  static auto GetStaticField(const std::string& fieldName) -> std::conditional_t<std::is_base_of_v<Object, Type>, std::unique_ptr<Type>, Type>

  template<typename Type, class Caller>
  requires ((IsJNIType<Type>::value or std::is_base_of_v<Object, Type>) and std::is_base_of_v<Object, Caller>)
  static auto SetStaticField(const std::string& fieldName, const Type& value) -> void
}
```

I'm going to create an example based on Minecraft 1.8.9 source code.  
The Minecraft class is the main class of the game and stores a static field with its instance.

```cpp
// you need to register your classes
EasyJNI::RegisterClass<Minecraft>("net/minecraft/client/Minecraft");
EasyJNI::RegisterClass<EntityPlayerSP>("net/minecraft/client/entity/EntityPlayerSP");
```

### Usage
```cpp
// recreate the java class in cpp and derive it from EasyJNI::Object
class Minecraft final : public EasyJNI::Object
{
public:
    Minecraft(const jobject instance)
        : EasyJNI::Object{ instance } // don't forget to give the jobject
    {

    }

    // for getters jobject are placed in unique ptrs
    static auto GetMinecraft() -> std::unique_ptr<Minecraft>
    {
        // the template takes <type of the field>, <current class>
        return GetStaticField<Minecraft, Minecraft>("theMinecraft");
    }

    auto GetThePlayer() -> std::unique_ptr<EntityPlayerSP>
    {
        // the template takes <type of the field>, <current class>
        return GetField<EntityPlayerSP, Minecraft>("thePlayer");
    }
};
```
```cpp
class EntityPlayerSP final : public EasyJNI::Object
{
public:
    EntityPlayerSP(const jobject instance)
        : EasyJNI::Object{ instance }
    {

    }

    // primitive types are not unique ptrs
    auto GetServerSprintState() -> jboolean
    {
        return GetField<jboolean, EntityPlayerSP>("serverSprintState");
    }

    // if value is a jobject give the unique ptr not the jobject 
    auto SetServerSprintState(const jboolean value) -> void
    {
        SetField<jboolean, EntityPlayerSP>("serverSprintState", value);
    }
};
```

EasyJNI currently only support getters and setters for normal classes  
See example: [main.cpp](./EasyJNI/src/main.cpp)
