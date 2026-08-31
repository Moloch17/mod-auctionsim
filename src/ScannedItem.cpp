#include "ScannedItem.h"
#include <charconv>
#include <cstdint>
#include <optional>
#include "Tokenize.h"

namespace
{
    template <typename T>
    bool ParseField(std::string_view field, T& out)
    {
        auto result = std::from_chars(field.data(), field.data() + field.size(), out);
        return result.ec == std::errc() && result.ptr == field.data() + field.size();
    }

    // Stat fields come from an int64 pipeline; a raw troll price can exceed uint32.
    // Clamp rather than reject the whole row -- the module only makes decisions off
    // the trimmed/percentile fields anyway, which are never extreme.
    bool ParseStat(std::string_view field, uint32& out)
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
}

std::optional<ScannedItem> ScannedItem::TryParse(std::string_view dataLine)
{
    std::vector<std::string_view> f = Acore::Tokenize(dataLine, ':', false);
    // 16 fields = price stats only (equippable gear, stack size always 1).
    // 28 fields = price stats + 12 trailing stack-size stat fields.
    if (f.size() != 16 && f.size() != 28)
    {
        return std::nullopt;
    }

    ScannedItem item;
    uint32 factionNum = 0;
    if (!ParseField(f[0], factionNum) || factionNum > UINT8_MAX) return std::nullopt;
    item.factionNum = static_cast<uint8>(factionNum);
    if (!ParseField(f[1], item.itemID)) return std::nullopt;
    if (!ParseField(f[2], item.suffixID)) return std::nullopt;
    if (!ParseField(f[3], item.sampleCount)) return std::nullopt;
    if (!ParseStat(f[4], item.rawLow)) return std::nullopt;
    if (!ParseStat(f[5], item.rawHigh)) return std::nullopt;
    if (!ParseStat(f[6], item.rawMean)) return std::nullopt;
    if (!ParseStat(f[7], item.rawMedian)) return std::nullopt;
    if (!ParseStat(f[8], item.rawMode)) return std::nullopt;
    if (!ParseStat(f[9], item.q1)) return std::nullopt;
    if (!ParseStat(f[10], item.q3)) return std::nullopt;
    if (!ParseStat(f[11], item.adjLow)) return std::nullopt;
    if (!ParseStat(f[12], item.adjHigh)) return std::nullopt;
    if (!ParseStat(f[13], item.adjMean)) return std::nullopt;
    if (!ParseStat(f[14], item.adjMedian)) return std::nullopt;
    if (!ParseStat(f[15], item.adjMode)) return std::nullopt;

    if (f.size() == 28)
    {
        if (!ParseStat(f[16], item.stackLow)) return std::nullopt;
        if (!ParseStat(f[17], item.stackHigh)) return std::nullopt;
        if (!ParseStat(f[18], item.stackMean)) return std::nullopt;
        if (!ParseStat(f[19], item.stackMedian)) return std::nullopt;
        if (!ParseStat(f[20], item.stackMode)) return std::nullopt;
        if (!ParseStat(f[21], item.stackQ1)) return std::nullopt;
        if (!ParseStat(f[22], item.stackQ3)) return std::nullopt;
        if (!ParseStat(f[23], item.stackAdjLow)) return std::nullopt;
        if (!ParseStat(f[24], item.stackAdjHigh)) return std::nullopt;
        if (!ParseStat(f[25], item.stackAdjMean)) return std::nullopt;
        if (!ParseStat(f[26], item.stackAdjMedian)) return std::nullopt;
        if (!ParseStat(f[27], item.stackAdjMode)) return std::nullopt;
    }

    return item;
}

uint32 ScannedItem::GetMarketPrice() const
{
    if (adjMedian > 0) return adjMedian;
    if (adjMean > 0) return adjMean;
    if (rawMedian > 0) return rawMedian;
    return rawMean;
}

uint32 ScannedItem::GetListLow() const { return adjLow > 0 ? adjLow : GetMarketPrice(); }
uint32 ScannedItem::GetListHigh() const { return adjHigh > 0 ? adjHigh : GetMarketPrice(); }

uint32 ScannedItem::GetBuyCeiling() const
{
    uint32 market = GetMarketPrice();
    return q3 > market ? q3 : market;
}

uint32 ScannedItem::GetTypicalStackSize() const
{
    if (stackAdjMode > 0) return stackAdjMode;
    if (stackAdjMedian > 0) return stackAdjMedian;
    if (stackMode > 0) return stackMode;
    if (stackMedian > 0) return stackMedian;
    return 1;
}

uint32 ScannedItem::GetStackLow() const { return stackAdjLow > 0 ? stackAdjLow : 1; }
uint32 ScannedItem::GetStackHigh() const { return stackAdjHigh > 0 ? stackAdjHigh : GetTypicalStackSize(); }
