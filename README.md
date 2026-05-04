# vmhook

## This project's goal is to allow clean and fast interactions between a Java Virtual Machine and native C++ code

- _Is this a copy of the Java Native Interface?_
Everything that the Java Native Interface C++ library offers can be done using vmhook, but vmhook is way more powerful.  
First, vmhook's API is way easier to use: no more local or global references; with vmhook, you associate a C++ class with a Java class, and you work directly on the Java Virtual Machine with your C++ code. Each Java field or method usage is one C++ function call with **no Java signature and no C++ type templates**.  
Secondly, vmhook works directly with Java HotSpot. That means insanely fast interactions and no Java Native Interface overhead.  
Finally, hooking is an important part of vmhook. With vmhook, you can hook method calls, intercept arguments, modify them, totally stop Java methods from being called, or return your own values. You can also intercept when a Java class is being loaded in the Java Virtual Machine. You work directly with the Just-In-Time compiler's Assembly instructions; retransform classes, add your own Assembly instructions. And the best part is: you don't have to have any Java Virtual Machine Tool Interface permissions since it doesn't use them. **Vmhook works on any Java Virtual Machine from Java 8 to the latest and doesn't care about the Java Virtual Machine's opinion.**

## Currently only Windows support

- Compile with MSVC

```bash
msbuild vmhook.slnx /p:Configuration=Release /p:Platform=x64
```