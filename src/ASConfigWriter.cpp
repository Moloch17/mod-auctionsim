#include "ASConfigWriter.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>
#include "Log.h"
#include "StringFormat.h"

namespace
{
    std::vector<std::string> ReadLines(std::string const& filepath)
    {
        std::vector<std::string> lines;
        std::ifstream stream(filepath, std::ios::in);
        if (!stream.is_open())
        {
            LOG_ERROR("module", "AuctionSim: config writer couldn't open {} for reading", filepath);
            return lines;
        }

        std::string line;
        while (std::getline(stream, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

    bool WriteLinesAtomic(std::string const& filepath, std::vector<std::string> const& lines)
    {
        std::string tempPath = filepath + ".tmp";

        {
            std::ofstream stream(tempPath, std::ios::out | std::ios::trunc);
            if (!stream.is_open())
            {
                LOG_ERROR("module", "AuctionSim: config writer couldn't open {} for writing", tempPath);
                return false;
            }
            for (std::string const& line : lines)
            {
                stream << line << "\n";
            }
        }

        std::error_code ec;
        std::filesystem::rename(tempPath, filepath, ec);
        if (ec)
        {
            LOG_ERROR(
                "module", "AuctionSim: config writer couldn't replace {} with {}: {}", filepath, tempPath,
                ec.message());
            return false;
        }
        return true;
    }

    // Finds the "AuctionSim.<key>" line and returns the position of its '=', or
    // std::string::npos if not found. Requires only whitespace between the key and '='
    // so e.g. "MaxItemLevel" doesn't match a line for a "MaxItemLevelFoo" key.
    size_t FindKeyEquals(std::string const& line, std::string const& fullKey)
    {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, fullKey.size(), fullKey) != 0)
        {
            return std::string::npos;
        }

        size_t afterKey = start + fullKey.size();
        size_t eqPos = line.find('=', afterKey);
        if (eqPos == std::string::npos)
        {
            return std::string::npos;
        }

        std::string between = line.substr(afterKey, eqPos - afterKey);
        if (between.find_first_not_of(" \t") != std::string::npos)
        {
            return std::string::npos;
        }
        return eqPos;
    }
}

namespace ASConfigWriter
{
    bool SetScalarValue(std::string const& filepath, std::string const& key, std::string const& value)
    {
        std::vector<std::string> lines = ReadLines(filepath);
        if (lines.empty())
        {
            return false;
        }

        std::string fullKey = "AuctionSim." + key;
        for (std::string& line : lines)
        {
            size_t eqPos = FindKeyEquals(line, fullKey);
            if (eqPos == std::string::npos)
            {
                continue;
            }

            line = line.substr(0, eqPos + 1) + " " + value;
            return WriteLinesAtomic(filepath, lines);
        }

        LOG_ERROR("module", "AuctionSim: config writer couldn't find '{}' in {}", fullKey, filepath);
        return false;
    }

    bool SetMaskValue(
        std::string const& filepath, std::string const& percentConfigKey, std::string const& qualityLabel,
        uint32 value)
    {
        std::vector<std::string> lines = ReadLines(filepath);
        if (lines.empty())
        {
            return false;
        }

        std::string fullKey = "AuctionSim." + percentConfigKey;
        for (std::string& line : lines)
        {
            size_t eqPos = FindKeyEquals(line, fullKey);
            if (eqPos == std::string::npos)
            {
                continue;
            }

            std::string labelPrefix = qualityLabel + ": ";
            size_t labelPos = line.find(labelPrefix, eqPos);
            if (labelPos == std::string::npos)
            {
                LOG_ERROR(
                    "module", "AuctionSim: config writer couldn't find '{}' on the {} line", qualityLabel, fullKey);
                return false;
            }

            size_t valueStart = labelPos + labelPrefix.size();
            size_t valueEnd = valueStart;
            while (valueEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[valueEnd])))
            {
                valueEnd++;
            }
            if (valueEnd == valueStart)
            {
                LOG_ERROR("module", "AuctionSim: config writer found no digits after '{}' on the {} line",
                    labelPrefix, fullKey);
                return false;
            }

            line = line.substr(0, valueStart) + Acore::StringFormat("{:04}", value) + line.substr(valueEnd);
            return WriteLinesAtomic(filepath, lines);
        }

        LOG_ERROR("module", "AuctionSim: config writer couldn't find '{}' in {}", fullKey, filepath);
        return false;
    }
}
