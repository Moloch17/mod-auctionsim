#include "ScannedItem.h"
#include <string>
#include "Define.h"

ScannedItem::ScannedItem(std::string_view _dataLine)
{
    std::string subStr = "";
    int delimCount = 0;

    for (size_t i = 0; i < _dataLine.size(); i++)
    {
        char c = _dataLine[i];
        if (c != ':')
        {
            subStr.push_back(c);
        }
        else
        {
            int num = std::stoi(subStr);
            switch (delimCount)
            {
                case 0:
                    this->factionNum = num;
                    break;
                case 1:
                    this->itemID = num;
                    break;
                case 2:
                    this->suffixID = num;
                    break;
                case 3:
                    this->meanPrice = num;
                    break;
                case 4:
                    this->minPrice = num;
                    break;
                default:
                    this->maxPrice = num;
            }
            subStr = "";
            delimCount++;
        }
    }
}

uint8 ScannedItem::GetFactionNum() { return this->factionNum; }
uint32 ScannedItem::GetItemID() { return this->itemID; }
uint32 ScannedItem::GetSuffixID() { return this->suffixID; }
uint32 ScannedItem::GetMeanPrice() { return this->meanPrice; }
uint32 ScannedItem::GetMinPrice() { return this->minPrice; }
uint32 ScannedItem::GetMaxPrice() { return this->maxPrice; }
