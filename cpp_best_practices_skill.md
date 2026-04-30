# AI Skill: Modern C++ Best Practices & Low-Level Mastery

This skill profile defines a strict, high-readability standard for modern C++ development, ranging from high-level architectural design to low-level systems programming.

## General Principles
- **Language**: Always write code and comments in English.
- **Clarity over Brevity**: Prefer verbose, descriptive code over clever or short snippets.
- **No Abbreviations**: Never shorten names (e.g., use `pointer` instead of `ptr`, `index` instead of `idx`).
- **Standard**: Use modern C++ features up to **C++23** (excluding modules).

## Naming Conventions
- **Snake Case**: Use `snake_case` for variables, functions, classes, and templates.
    ```cpp
    auto compute_value() -> std::int32_t;

    template<typename value_type>
    struct container_type;
    ```
- **Type Suffixes**: Always use the `_t` suffix for type aliases and typedefs.
    ```cpp
    using integer_value_t = std::int32_t;
    using container_t = std::vector<std::int32_t>;
    ```

## Function Syntax
- **Trailing Return Types**: Always use the trailing return type syntax.
- **Brace Placement**: Never place the opening brace `{` on the same line as the function signature.
    ```cpp
    // Good
    auto function_name() -> void
    {
    }

    // Bad
    auto function_name() -> void {
    }
    ```

## Namespace Usage
- **Full Qualification**: Always use the full namespace path, even when calling a function or using a type within the same namespace.
    ```cpp
    namespace application::math
    {
        auto add(std::int32_t a, std::int32_t b) -> std::int32_t
        {
            return a + b;
        }

        auto calculate() -> void
        {
            // Good
            auto result = application::math::add(1, 2);

            // Bad
            auto result = add(1, 2);
        }
    }
    ```

## Control Structures & Formatting
- **Explicit Scopes**: Always use braces `{}` for control structures (`if`, `else`, `for`, `while`), even for single statements.
- **One Action Per Line**: Never put multiple statements or actions on a single line.
- **Initialization**: Prefer brace initialization `{}`.
    ```cpp
    std::int32_t value{ 1 }; // Good
    ```
- **IO**: Prefer `<print>` (C++23) over `<iostream>`.

## Header-Only Library Documentation
When developing C++ header-only libraries, provide **perfect and exhaustive documentation** within the file. This includes:
1.  **Top-level Overview**: Purpose, dependencies, and thread-safety guarantees.
2.  **Template Requirements**: Explicitly document requirements for template parameters (using Concepts where applicable).
3.  **Complexity Guarantees**: Big-O notation for time and space complexity for every public method.
4.  **Exception Safety**: Specify if a function is `noexcept` or what exceptions it might throw.

## Low-Level & Systems Programming
When performing low-level operations (memory mapping, hardware abstraction, drivers), you must be hyper-explicit:
- **Address Manipulation**: Explain the rationale behind every pointer arithmetic operation.
- **Offsets**: Document why a specific offset is used (e.g., referencing a specific hardware register datasheet).
- **Bit Manipulation**: For every bitwise AND, OR, XOR, or Shift, provide a comment explaining which bit-fields are being affected and why.
    ```cpp
    // Masking bits 4-7 to extract the interrupt priority level
    // Offset 0x04 corresponds to the Interrupt Control Register (ICR)
    auto priority = (*(interrupt_register + 0x04) >> 4) & 0x0F;
    ```

## Summary
This style enforces:
- **Maximum Readability**: Names tell a story.
- **Explicit Behavior**: No hidden logic or implicit namespace lookups.
- **Strong Consistency**: A unified look across all modules.
