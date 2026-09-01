#include "ASConfig.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include "ASParse.h"
#include "Config.h"
#include "ObjectMgr.h"
#include "ScannedItem.h"
#include "Tokenize.h"

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

ASConfig::ASConfig(std::string const& filepath, bool& outLoaded)
{
    this->maxRequiredLevel = sConfigMgr->GetOption<uint32>("AuctionSim.MaxRequiredLevel", 0);
    this->maxItemLevel = sConfigMgr->GetOption<uint32>("AuctionSim.MaxItemLevel", 0);

    if (!std::filesystem::exists(filepath))
    {
        LOG_ERROR("module", "AuctionSim: {} not found", filepath);
        outLoaded = false;
        return;
    }

    std::ifstream stream(filepath, std::ios::in);
    if (!stream.is_open())
    {
        LOG_ERROR("module", "AuctionSim: Couldn't open {}", filepath);
        outLoaded = false;
        return;
    }

    std::string line;
    if (!std::getline(stream, line))
    {
        LOG_ERROR("module", "AuctionSim: {} is empty", filepath);
        outLoaded = false;
        return;
    }

    // Line 1 is "N M": N item rows, preceded by M category-depth rows.
    size_t declaredItemRows = 0;
    size_t categoryRows = 0;
    if (!ParseHeaderLine(line, declaredItemRows, categoryRows))
    {
        LOG_ERROR("module", "AuctionSim: {} has a malformed header line '{}'", filepath, line);
        outLoaded = false;
        return;
    }

    for (size_t read = 0; read < categoryRows && std::getline(stream, line); ++read)
    {
        LoadCategoryRow(line, filepath);
    }

    while (std::getline(stream, line))
    {
        LoadItemRow(line, filepath);
    }

    if (this->ScanData.size() > declaredItemRows)
    {
        LOG_ERROR(
            "module",
            "AuctionSim: {} contains more item rows ({}) than its header declared ({}); refusing to load -- "
            "the file looks corrupt",
            filepath,
            this->ScanData.size(),
            declaredItemRows);
        outLoaded = false;
        return;
    }

    BuildSelectionTables(filepath);

    size_t depthProfiles = 0;
    for (auto const& byFaction : this->categoryDepth)
    {
        for (auto const& byClass : byFaction)
        {
            for (CategoryDepth const& depth : byClass)
            {
                if (depth.has)
                {
                    depthProfiles++;
                }
            }
        }
    }

    LOG_INFO(
        "module",
        "AuctionSim: loaded prices for {} items and {} category depth profiles",
        this->ScanData.size(),
        depthProfiles);

    LoadMasks();
}

bool ASConfig::ParseHeaderLine(std::string const& line, size_t& outItemRows, size_t& outCategoryRows)
{
    std::istringstream header(line);
    header >> outItemRows >> outCategoryRows;
    return static_cast<bool>(header) && outItemRows > 0;
}

// One category-depth row: faction:class:quality:snapshotCount followed by a
// 12-value StatBlock. Only q1 / median / adjLow / adjHigh are kept.
void ASConfig::LoadCategoryRow(std::string const& line, std::string const& filepath)
{
    std::vector<std::string_view> f = Acore::Tokenize(line, ':', false);
    if (f.size() != kCategoryRowFields)
    {
        LOG_ERROR("module", "AuctionSim: skipping malformed category row in {}: '{}'", filepath, line);
        return;
    }

    uint32 faction = 0;
    uint32 itemClass = 0;
    uint32 quality = 0;
    StatBlock stats;
    if (!ASParse::Integer(f[0], faction) || !ASParse::Integer(f[1], itemClass) ||
        !ASParse::Integer(f[2], quality) || !ParseStatBlock(f, ScannedItem::kIdentityFields, stats))
    {
        LOG_ERROR("module", "AuctionSim: skipping unparseable category row in {}: '{}'", filepath, line);
        return;
    }
    if (faction >= kAuctionHouseIndexBound || itemClass >= MAX_ITEM_CLASS || quality >= MAX_ITEM_QUALITY)
    {
        LOG_ERROR("module", "AuctionSim: category row out of range in {}: '{}'", filepath, line);
        return;
    }

    CategoryDepth& depth = this->categoryDepth[faction][itemClass][quality];
    depth.has = true;
    depth.q1 = stats.q1;
    depth.median = stats.median;
    depth.adjLow = stats.adjLow;
    depth.adjHigh = stats.adjHigh;
}

void ASConfig::LoadItemRow(std::string const& line, std::string const& filepath)
{
    if (auto item = ScannedItem::TryParse(line))
    {
        this->ScanData.push_back(*item);
    }
    else
    {
        LOG_ERROR("module", "AuctionSim: skipping malformed line in {}: '{}'", filepath, line);
    }
}

// Resolves each row's item_template once and files it into ItemSelectionTable and
// ItemIndex. ScanData is a deque, so the pointers taken here stay valid.
void ASConfig::BuildSelectionTables(std::string const& filepath)
{
    for (ScannedItem& item : this->ScanData)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.GetItemID());
        if (!proto)
        {
            LOG_WARN(
                "module",
                "AuctionSim: item {} in {} not found in item_template, skipping",
                item.GetItemID(),
                filepath);
            continue;
        }

        size_t house = item.GetFactionNum();
        if (house >= kAuctionHouseIndexBound || proto->Class >= MAX_ITEM_CLASS || proto->Quality >= MAX_ITEM_QUALITY)
        {
            continue;
        }

        this->ItemSelectionTable[house][proto->Class][proto->Quality].push_back(&item);
        this->ItemIndex.try_emplace(IndexKey(house, proto->Class, proto->Quality, item.GetItemID()), &item);
    }
}

void ASConfig::LoadMasks()
{
    for (auto const& entry : kItemClassConfigKeys)
    {
        UnpackQualityString(sConfigMgr->GetOption<std::string>(entry.configKey, ""), entry.itemClass);
    }
}

uint64_t ASConfig::IndexKey(size_t house, uint32 itemClass, uint32 quality, uint32 itemID)
{
    return (static_cast<uint64_t>(house) << 56) | (static_cast<uint64_t>(itemClass) << 48) |
           (static_cast<uint64_t>(quality) << 40) | static_cast<uint64_t>(itemID);
}

std::vector<ScannedItem*> const& ASConfig::ItemsFor(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const
{
    return ItemSelectionTable[static_cast<size_t>(houseId)][itemClass][quality];
}

ASConfig::CategoryDepth const& ASConfig::GetCategoryDepth(
    AuctionHouseId houseId, uint32 itemClass, uint32 quality) const
{
    static CategoryDepth const kEmpty{};

    size_t house = static_cast<size_t>(houseId);
    if (house >= kAuctionHouseIndexBound || itemClass >= MAX_ITEM_CLASS || quality >= MAX_ITEM_QUALITY)
    {
        return kEmpty;
    }
    return categoryDepth[house][itemClass][quality];
}

ScannedItem const* ASConfig::FindScannedItem(
    AuctionHouseId houseId, uint32 itemClass, uint32 quality, uint32 itemID) const
{
    size_t house = static_cast<size_t>(houseId);
    if (house >= kAuctionHouseIndexBound || itemClass >= MAX_ITEM_CLASS || quality >= MAX_ITEM_QUALITY)
    {
        return nullptr;
    }

    auto it = ItemIndex.find(IndexKey(house, itemClass, quality, itemID));
    return it != ItemIndex.end() ? it->second : nullptr;
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
                "AuctionSim: missing '{}' entry in multiplier string for item class {}, defaulting to 0",
                token.label,
                itemClass);
            this->ItemSelectionMask[itemClass][token.quality] = 0.0f;
            continue;
        }

        // The value runs from just after "<LABEL>: " to the next comma (or the end
        // of the string for the last entry); trim any surrounding whitespace.
        size_t valueStart = pos + prefix.size();
        size_t commaPos = qualityString.find(',', valueStart);
        std::string_view valueStr = qualityString.substr(
            valueStart, commaPos == std::string_view::npos ? std::string_view::npos : commaPos - valueStart);
        while (!valueStr.empty() && (valueStr.front() == ' ' || valueStr.front() == '\t'))
        {
            valueStr.remove_prefix(1);
        }
        while (!valueStr.empty() &&
               (valueStr.back() == ' ' || valueStr.back() == '\t' || valueStr.back() == '\r'))
        {
            valueStr.remove_suffix(1);
        }

        float value = 0.0f;
        if (!ASParse::Float(valueStr, value) || value < 0.0f)
        {
            LOG_ERROR(
                "module",
                "AuctionSim: malformed multiplier '{}' for '{}' in item class {}, defaulting to 0",
                valueStr,
                token.label,
                itemClass);
            value = 0.0f;
        }

        this->ItemSelectionMask[itemClass][token.quality] = value;
    }

    this->ItemSelectionMask[itemClass][ITEM_QUALITY_HEIRLOOM] = 0.0f;
}
