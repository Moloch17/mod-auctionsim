#pragma once
#include <vector>
#include "ItemTemplate.h"
#include "ScannedItem.h"

class ASConfig
{
public:
    int updateInterval;
    ASConfig(std::string _filepath, bool& _isEnabled);
    std::vector<ScannedItem*> ItemSelectionTable[2][MAX_ITEM_CLASS][MAX_ITEM_QUALITY];
    float ItemSelectionMask[MAX_ITEM_CLASS][MAX_ITEM_QUALITY];
    std::vector<ScannedItem> ScanData;

    private:
    void UnpackQualityString(std::string _qualityString, int _itemClass);
};
