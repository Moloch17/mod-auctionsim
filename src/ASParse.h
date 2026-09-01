#pragma once
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include "Define.h"

// Small shared parsers for the module's text formats -- auctionsim.dat rows and
// the addon wire protocol. Every parse requires the whole field to be consumed:
// trailing junk is a failure, never silently ignored.
namespace ASParse
{
    // Parses a whole integer field. T may be signed or unsigned.
    template <typename T>
    bool Integer(std::string_view field, T& out)
    {
        auto result = std::from_chars(field.data(), field.data() + field.size(), out);
        return result.ec == std::errc() && result.ptr == field.data() + field.size();
    }

    // Parses an unsigned value, clamping to UINT32_MAX instead of failing when the
    // number overflows -- auctionsim.dat stat columns come from an int64 pipeline
    // and a raw troll price can exceed uint32. The module only ever acts on the
    // trimmed / percentile columns, which are never extreme.
    inline bool ClampedU32(std::string_view field, uint32& out)
    {
        uint64_t value = 0;
        auto result = std::from_chars(field.data(), field.data() + field.size(), value);
        if (result.ptr != field.data() + field.size())
        {
            return false;
        }
        if (result.ec == std::errc::result_out_of_range)
        {
            out = UINT32_MAX;
            return true;
        }
        if (result.ec != std::errc())
        {
            return false;
        }
        out = value > UINT32_MAX ? UINT32_MAX : static_cast<uint32>(value);
        return true;
    }

    // Parses a plain decimal ("1", "1.5", "0.25") via strtof. False on an empty
    // field, trailing junk, or an out-of-range magnitude. Sign is NOT checked here;
    // callers that need non-negative apply their own `< 0` guard, so this stays
    // usable for any float field.
    inline bool Float(std::string_view field, float& out)
    {
        if (field.empty())
        {
            return false;
        }
        std::string buf(field);
        char* end = nullptr;
        errno = 0;
        float value = std::strtof(buf.c_str(), &end);
        if (end != buf.c_str() + buf.size() || errno == ERANGE)
        {
            return false;
        }
        out = value;
        return true;
    }
}
