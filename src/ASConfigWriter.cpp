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

    // Rewrites "AuctionSim.<key> = <old>" -> "... = <value>" in `line` in place.
    // Returns true if `line` was that key's line (and was rewritten).
    bool RewriteScalarLine(std::string& line, std::string const& fullKey, std::string const& value)
    {
        size_t eqPos = FindKeyEquals(line, fullKey);
        if (eqPos == std::string::npos)
        {
            return false;
        }
        line = line.substr(0, eqPos + 1) + " " + value;
        return true;
    }

    // Rewrites the "<qualityLabel>: <num>" cell inside the "AuctionSim.<fullKey> = ..."
    // line in place. Returns true if `line` was that key's line; on that line, if the
    // label or its number can't be located it logs and sets `outCellOk` false but
    // still returns true (the line was matched, the cell just wasn't).
    bool RewriteMaskLine(
        std::string& line,
        std::string const& fullKey,
        std::string const& qualityLabel,
        std::string const& value,
        bool& outCellOk)
    {
        size_t eqPos = FindKeyEquals(line, fullKey);
        if (eqPos == std::string::npos)
        {
            return false;
        }

        std::string labelPrefix = qualityLabel + ": ";
        size_t labelPos = line.find(labelPrefix, eqPos);
        if (labelPos == std::string::npos)
        {
            LOG_ERROR(
                "module", "AuctionSim: config writer couldn't find '{}' on the {} line", qualityLabel, fullKey);
            outCellOk = false;
            return true;
        }

        size_t valueStart = labelPos + labelPrefix.size();
        size_t valueEnd = valueStart;
        while (valueEnd < line.size() &&
               (std::isdigit(static_cast<unsigned char>(line[valueEnd])) || line[valueEnd] == '.'))
        {
            valueEnd++;
        }
        if (valueEnd == valueStart)
        {
            LOG_ERROR(
                "module", "AuctionSim: config writer found no number after '{}' on the {} line", labelPrefix,
                fullKey);
            outCellOk = false;
            return true;
        }

        line = line.substr(0, valueStart) + value + line.substr(valueEnd);
        return true;
    }
}

namespace ASConfigWriter
{
    bool SetMany(std::string const& filepath, std::vector<Edit> const& edits)
    {
        if (edits.empty())
        {
            return true;
        }

        std::vector<std::string> lines = ReadLines(filepath);
        if (lines.empty())
        {
            return false;
        }

        bool allOk = true;
        for (Edit const& edit : edits)
        {
            std::string fullKey = "AuctionSim." + edit.key;
            bool applied = false;
            bool lineMatched = false;

            for (std::string& line : lines)
            {
                if (edit.qualityLabel.empty())
                {
                    if (RewriteScalarLine(line, fullKey, edit.value))
                    {
                        applied = true;
                        lineMatched = true;
                        break;
                    }
                }
                else
                {
                    bool cellOk = true;
                    if (RewriteMaskLine(line, fullKey, edit.qualityLabel, edit.value, cellOk))
                    {
                        applied = cellOk;
                        lineMatched = true;
                        break;
                    }
                }
            }

            if (!applied)
            {
                // A matched-line-but-bad-cell mask edit already logged its specifics.
                if (!lineMatched)
                {
                    LOG_ERROR("module", "AuctionSim: config writer couldn't find '{}' in {}", fullKey, filepath);
                }
                allOk = false;
            }
        }

        return WriteLinesAtomic(filepath, lines) && allOk;
    }

    bool SetScalarValue(std::string const& filepath, std::string const& key, std::string const& value)
    {
        return SetMany(filepath, {{key, "", value}});
    }

    bool SetMaskValue(
        std::string const& filepath, std::string const& percentConfigKey, std::string const& qualityLabel,
        float value)
    {
        return SetMany(filepath, {{percentConfigKey, qualityLabel, Acore::StringFormat("{:g}", value)}});
    }
}
