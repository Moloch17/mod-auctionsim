#pragma once
#include <string_view>
#include <vector>
namespace Acore
{
    inline std::vector<std::string_view> Tokenize(std::string_view str, char sep, bool keepEmpty)
    {
        std::vector<std::string_view> tokens;
        size_t start = 0;
        for (size_t end = str.find(sep); end != std::string_view::npos; end = str.find(sep, start))
        {
            if (keepEmpty || start < end)
                tokens.push_back(str.substr(start, end - start));
            start = end + 1;
        }
        if (keepEmpty || start < str.length())
            tokens.push_back(str.substr(start));
        return tokens;
    }
}
