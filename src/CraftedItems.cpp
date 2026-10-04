#include "CraftedItems.h"
#include <unordered_set>
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    std::unordered_set<uint32> crafted;
    bool loaded = false;
}

void CraftedItems::Load()
{
    if (loaded)
    {
        return;
    }
    for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
    {
        SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
        if (!ability)
        {
            continue;
        }
        SkillLineEntry const* skill = sSkillLineStore.LookupEntry(ability->SkillLine);
        if (!skill || (skill->categoryId != SKILL_CATEGORY_PROFESSION && skill->categoryId != SKILL_CATEGORY_SECONDARY))
        {
            continue;
        }
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(ability->Spell);
        if (!spell)
        {
            continue;
        }
        for (SpellEffectInfo const& effect : spell->GetEffects())
        {
            if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM || effect.Effect == SPELL_EFFECT_CREATE_ITEM_2) &&
                effect.ItemType)
            {
                crafted.insert(effect.ItemType);
            }
        }
    }
    loaded = true;
    LOG_INFO("module", "AuctionSim: {} profession-crafted items", crafted.size());
}

bool CraftedItems::IsCrafted(uint32 itemId)
{
    return crafted.count(itemId) != 0;
}

void CraftedItems::SignIfCrafted(Item* item, ObjectGuid creator)
{
    if (item && IsCrafted(item->GetEntry()) && item->GetTemplate()->HasSignature())
    {
        item->SetGuidValue(ITEM_FIELD_CREATOR, creator);
    }
}
