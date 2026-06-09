#include "Config.h"
#include <cstddef>
#include <filesystem>
#include "ASConfig.h"
#include "ObjectMgr.h"
#include "ScannedItem.h"

ASConfig::ASConfig(std::string _filepath, bool& _isEnabled)
{
    this->updateInterval = sConfigMgr->GetOption<int>("AuctionSim.UpdateInterval", 0);

    if (!std::filesystem::exists(_filepath))
    {
        LOG_ERROR("module", "AuctionSim: {} not found", _filepath);
        _isEnabled = false;
        return;
    }

    std::ifstream stream(_filepath, std::ios::in);

    if (!stream.is_open())
    {
        LOG_ERROR("module", "AuctionSim: Couldn't open {}", _filepath);
        _isEnabled = false;
        return;
    }

    std::string line;
    std::getline(stream, line);
    int totalItems = std::stoi(line);

    this->ScanData.reserve(totalItems);

    while (std::getline(stream, line))
    {
        this->ScanData.emplace_back(ScannedItem(line));
    }

    LOG_INFO("module", "AuctionSim: loaded prices for {} items", this->ScanData.size());

    for (int _itemClass = 0; _itemClass < MAX_ITEM_CLASS; _itemClass++)
    {
        switch (_itemClass)
        {
            case ITEM_CLASS_CONSUMABLE:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.ConsumablePercent", ""), _itemClass);
                break;
            case ITEM_CLASS_CONTAINER:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.ContainerPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_WEAPON:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.WeaponPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_GEM:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.GemPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_ARMOR:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.ArmorPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_REAGENT:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.ReagentPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_PROJECTILE:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.ProjectilePercent", ""), _itemClass);
                break;
            case ITEM_CLASS_TRADE_GOODS:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.TradeGoodsPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_GENERIC:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.GenericPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_RECIPE:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.RecipePercent", ""), _itemClass);
                break;
            case ITEM_CLASS_MONEY:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.MoneyPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_QUIVER:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.QuiverPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_QUEST:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.QuestPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_KEY:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.KeyPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_PERMANENT:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.PermanentPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_MISC:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.MiscPercent", ""), _itemClass);
                break;
            case ITEM_CLASS_GLYPH:
                UnpackQualityString(sConfigMgr->GetOption<std::string>("AuctionSim.GlyphPercent", ""), _itemClass);
                break;
        }
    }

    for (size_t i = 0; i < this->ScanData.size(); i++)
    {
        auto proto = sObjectMgr->GetItemTemplate(ScanData[i].GetItemID());
        this->ItemSelectionTable[ScanData[i].GetFactionNum()][proto->Class][proto->Quality].emplace_back(&ScanData[i]);
    }
}

void ASConfig::UnpackQualityString(std::string _qualityString, int _itemClass)
{
    std::string::size_type pos;
    pos = _qualityString.find("GREY: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_POOR] = std::stoi(_qualityString.substr(pos + 6, 4)) / 100.0f;
    pos = _qualityString.find("WHITE: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_NORMAL] = std::stoi(_qualityString.substr(pos + 7, 4)) / 100.0f;
    pos = _qualityString.find("GREEN: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_UNCOMMON] = std::stoi(_qualityString.substr(pos + 7, 4)) / 100.0f;
    pos = _qualityString.find("BLUE: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_RARE] = std::stoi(_qualityString.substr(pos + 6, 4)) / 100.0f;
    pos = _qualityString.find("PURPLE: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_EPIC] = std::stoi(_qualityString.substr(pos + 8, 4)) / 100.0f;
    pos = _qualityString.find("ORANGE: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_LEGENDARY] = std::stoi(_qualityString.substr(pos + 8, 4)) / 100.0f;
    pos = _qualityString.find("YELLOW: ");
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_ARTIFACT] = std::stoi(_qualityString.substr(pos + 8, 4)) / 100.0f;
    this->ItemSelectionMask[_itemClass][ITEM_QUALITY_HEIRLOOM] = 0.0f;
}
