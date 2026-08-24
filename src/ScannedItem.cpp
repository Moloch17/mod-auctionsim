#include "ScannedItem.h"
#include <charconv>
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
}

std::optional<ScannedItem> ScannedItem::TryParse(std::string_view dataLine)
{
    std::vector<std::string_view> fields = Acore::Tokenize(dataLine, ':', false);
    if (fields.size() != 6)
    {
        return std::nullopt;
    }

    ScannedItem item;
    uint32 factionNum = 0;
    if (!ParseField(fields[0], factionNum) || factionNum > UINT8_MAX) return std::nullopt;
    item.factionNum = static_cast<uint8>(factionNum);
    if (!ParseField(fields[1], item.itemID)) return std::nullopt;
    if (!ParseField(fields[2], item.suffixID)) return std::nullopt;
    if (!ParseField(fields[3], item.meanPrice)) return std::nullopt;
    if (!ParseField(fields[4], item.minPrice)) return std::nullopt;
    if (!ParseField(fields[5], item.maxPrice)) return std::nullopt;
    return item;
}

uint8 ScannedItem::GetFactionNum() const { return this->factionNum; }
uint32 ScannedItem::GetItemID() const { return this->itemID; }
int32 ScannedItem::GetSuffixID() const { return this->suffixID; }
uint32 ScannedItem::GetMeanPrice() const { return this->meanPrice; }
uint32 ScannedItem::GetMinPrice() const { return this->minPrice; }
uint32 ScannedItem::GetMaxPrice() const { return this->maxPrice; }
