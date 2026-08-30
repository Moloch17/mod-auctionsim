#include "ASConfig.h"
#include <array>
#include <charconv>
#include <filesystem>
#include <string_view>
#include "Config.h"
#include "ObjectMgr.h"
#include "ScannedItem.h"

namespace
{
    struct ItemClassConfigKey
    {
        uint32 itemClass;
        char const* configKey;
    };

    constexpr std::array<ItemClassConfigKey, MAX_ITEM_CLASS> kItemClassConfigKeys = {{
        {ITEM_CLASS_CONSUMABLE, "AuctionSim.ConsumablePercent"},
        {ITEM_CLASS_CONTAINER, "AuctionSim.ContainerPercent"},
        {ITEM_CLASS_WEAPON, "AuctionSim.WeaponPercent"},
        {ITEM_CLASS_GEM, "AuctionSim.GemPercent"},
        {ITEM_CLASS_ARMOR, "AuctionSim.ArmorPercent"},
        {ITEM_CLASS_REAGENT, "AuctionSim.ReagentPercent"},
        {ITEM_CLASS_PROJECTILE, "AuctionSim.ProjectilePercent"},
        {ITEM_CLASS_TRADE_GOODS, "AuctionSim.TradeGoodsPercent"},
        {ITEM_CLASS_GENERIC, "AuctionSim.GenericPercent"},
        {ITEM_CLASS_RECIPE, "AuctionSim.RecipePercent"},
        {ITEM_CLASS_MONEY, "AuctionSim.MoneyPercent"},
        {ITEM_CLASS_QUIVER, "AuctionSim.QuiverPercent"},
        {ITEM_CLASS_QUEST, "AuctionSim.QuestPercent"},
        {ITEM_CLASS_KEY, "AuctionSim.KeyPercent"},
        {ITEM_CLASS_PERMANENT, "AuctionSim.PermanentPercent"},
        {ITEM_CLASS_MISC, "AuctionSim.MiscPercent"},
        {ITEM_CLASS_GLYPH, "AuctionSim.GlyphPercent"},
    }};

    struct QualityToken
    {
        uint32 quality;
        std::string_view label;
    };

    constexpr std::array<QualityToken, 7> kQualityTokens = {{
        {ITEM_QUALITY_POOR, "GREY"},
        {ITEM_QUALITY_NORMAL, "WHITE"},
        {ITEM_QUALITY_UNCOMMON, "GREEN"},
        {ITEM_QUALITY_RARE, "BLUE"},
        {ITEM_QUALITY_EPIC, "PURPLE"},
        {ITEM_QUALITY_LEGENDARY, "ORANGE"},
        {ITEM_QUALITY_ARTIFACT, "YELLOW"},
    }};
}

ASConfig::ASConfig(std::string _filepath, bool& _isEnabled)
{
    this->maxRequiredLevel = sConfigMgr->GetOption<uint32>("AuctionSim.MaxRequiredLevel", 0);
    this->maxItemLevel = sConfigMgr->GetOption<uint32>("AuctionSim.MaxItemLevel", 0);

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
    size_t totalItems = std::stoul(line);

    this->ScanData.reserve(totalItems);

    while (std::getline(stream, line))
    {
        if (auto item = ScannedItem::TryParse(line))
        {
            this->ScanData.push_back(*item);
        }
        else
        {
            LOG_ERROR("module", "AuctionSim: skipping malformed line in {}: '{}'", _filepath, line);
        }
    }

    if (this->ScanData.size() > totalItems)
    {
        LOG_ERROR(
            "module",
            "AuctionSim: {} contains more items ({}) than its header declared ({}); refusing to load, "
            "ItemSelectionTable pointers would not be stable",
            _filepath,
            this->ScanData.size(),
            totalItems);
        _isEnabled = false;
        return;
    }

    LOG_INFO("module", "AuctionSim: loaded prices for {} items", this->ScanData.size());

    for (auto const& entry : kItemClassConfigKeys)
    {
        UnpackQualityString(sConfigMgr->GetOption<std::string>(entry.configKey, ""), entry.itemClass);
    }

    for (size_t i = 0; i < this->ScanData.size(); i++)
    {
        auto proto = sObjectMgr->GetItemTemplate(ScanData[i].GetItemID());
        if (!proto)
        {
            LOG_WARN(
                "module",
                "AuctionSim: item {} in {} not found in item_template, skipping",
                ScanData[i].GetItemID(),
                _filepath);
            continue;
        }
        this->ItemSelectionTable[ScanData[i].GetFactionNum()][proto->Class][proto->Quality].emplace_back(&ScanData[i]);
    }
}

std::vector<ScannedItem*> const& ASConfig::ItemsFor(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const
{
    return ItemSelectionTable[static_cast<size_t>(houseId)][itemClass][quality];
}

ScannedItem const* ASConfig::FindScannedItem(
    AuctionHouseId houseId, uint32 itemClass, uint32 quality, uint32 itemID) const
{
    for (ScannedItem const* item : ItemsFor(houseId, itemClass, quality))
    {
        if (item->GetItemID() == itemID)
        {
            return item;
        }
    }
    return nullptr;
}

namespace
{
    // "AuctionSim.ConsumablePercent" -> "ConsumablePercent"
    std::string_view StripConfigPrefix(std::string_view configKey)
    {
        auto dotPos = configKey.find('.');
        return dotPos == std::string_view::npos ? configKey : configKey.substr(dotPos + 1);
    }
}

bool ASConfig::ResolveMaskKey(std::string_view key, uint32& outItemClass, uint32& outQuality)
{
    auto dotPos = key.find('.');
    if (dotPos == std::string_view::npos)
    {
        return false;
    }

    std::string_view percentConfigKey = key.substr(0, dotPos);
    std::string_view qualityLabel = key.substr(dotPos + 1);

    for (auto const& entry : kItemClassConfigKeys)
    {
        if (StripConfigPrefix(entry.configKey) != percentConfigKey)
        {
            continue;
        }
        for (auto const& token : kQualityTokens)
        {
            if (token.label == qualityLabel)
            {
                outItemClass = entry.itemClass;
                outQuality = token.quality;
                return true;
            }
        }
        return false;
    }
    return false;
}

std::vector<ASConfig::MaskKeyEntry> const& ASConfig::AllMaskKeys()
{
    static std::vector<MaskKeyEntry> keys = [] {
        std::vector<MaskKeyEntry> result;
        result.reserve(kItemClassConfigKeys.size() * kQualityTokens.size());
        for (auto const& entry : kItemClassConfigKeys)
        {
            for (auto const& token : kQualityTokens)
            {
                result.push_back({StripConfigPrefix(entry.configKey), token.label, entry.itemClass, token.quality});
            }
        }
        return result;
    }();
    return keys;
}

void ASConfig::UnpackQualityString(std::string_view qualityString, int itemClass)
{
    for (auto const& token : kQualityTokens)
    {
        std::string prefix = std::string(token.label) + ": ";
        auto pos = qualityString.find(prefix);
        if (pos == std::string_view::npos)
        {
            LOG_ERROR(
                "module",
                "AuctionSim: missing '{}' entry in quality string for item class {}, defaulting to 0",
                token.label,
                itemClass);
            this->ItemSelectionMask[itemClass][token.quality] = 0.0f;
            continue;
        }

        std::string_view valueStr = qualityString.substr(pos + prefix.size(), 4);

        int value = 0;
        auto result = std::from_chars(valueStr.data(), valueStr.data() + valueStr.size(), value);
        if (result.ec != std::errc())
        {
            LOG_ERROR(
                "module",
                "AuctionSim: malformed percent value '{}' for '{}' in item class {}, defaulting to 0",
                valueStr,
                token.label,
                itemClass);
            value = 0;
        }

        this->ItemSelectionMask[itemClass][token.quality] = value / 100.0f;
    }

    this->ItemSelectionMask[itemClass][ITEM_QUALITY_HEIRLOOM] = 0.0f;
}
