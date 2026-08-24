#pragma once

#include <optional>
#include <string_view>
#include "Define.h"

class ScannedItem
{
    uint8 factionNum = 0;
    uint32 itemID = 0;
    int32 suffixID = 0;  // signed: negative = enchantment/suffix table, positive = random property table
    uint32 meanPrice = 0;
    uint32 minPrice = 0;
    uint32 maxPrice = 0;

    ScannedItem() = default;

public:
    uint8 GetFactionNum() const;
    uint32 GetItemID() const;
    int32 GetSuffixID() const;
    uint32 GetMeanPrice() const;
    uint32 GetMinPrice() const;
    uint32 GetMaxPrice() const;

    // Parses a "faction:itemID:suffixID:meanPrice:minPrice:maxPrice" line.
    // Returns std::nullopt on malformed input instead of throwing.
    static std::optional<ScannedItem> TryParse(std::string_view dataLine);
};
