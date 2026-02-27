#pragma once

#include <cstdint>

namespace Type
{
	using U8 = std::int8_t;
	using U16 = std::int16_t;
	using U32 = std::int32_t;
	using U64 = std::int64_t;

	using I8 = std::uint8_t;
	using I16 = std::uint16_t;
	using I32 = std::uint32_t;
	using I64 = std::uint64_t;

	using F32 = float;
	using F64 = double;

	using Char = char;

	using Size = size_t;
}

using namespace Type;