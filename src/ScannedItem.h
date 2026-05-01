#pragma once

#include <string_view>
#include "Define.h"
class ScannedItem
{
    uint8 factionNum;
    uint32 itemID;
    uint32 suffixID;
    uint32 meanPrice;
    uint32 minPrice;
    uint32 maxPrice;

public:
    uint8 GetFactionNum();
    uint32 GetItemID();
    uint32 GetSuffixID();
    uint32 GetMeanPrice();
    uint32 GetMinPrice();
    uint32 GetMaxPrice();

    ScannedItem(std::string_view _dataLine);
};
