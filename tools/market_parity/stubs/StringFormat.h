#pragma once
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <string>
namespace Acore
{
    template <typename... Args>
    std::string StringFormat(fmt::format_string<Args...> f, Args&&... args)
    {
        return fmt::format(f, std::forward<Args>(args)...);
    }
}
