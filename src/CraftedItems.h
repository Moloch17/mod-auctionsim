#pragma once
#include "Define.h"

class Item;
class ObjectGuid;

// Which items a profession makes, so the module's own listings of them can carry a
// "<Made by ...>" signature the way a player's crafts do.
namespace CraftedItems
{
    // Collects every item a primary or secondary profession creates: the create-item
    // effects of each spell on a profession skill line (SkillLineAbility.dbc). Needs
    // spells and DBCs loaded (OnStartup); a second call does nothing.
    void Load();

    bool IsCrafted(uint32 itemId);

    // Stamps `creator` as the item's maker, as Spell::DoCreateItem does for a player's
    // craft: only for a profession-made item whose template takes a signature (stack
    // of 1, not consumable or quest, no NO_CREATOR flag). Call before the item is saved.
    void SignIfCrafted(Item* item, ObjectGuid creator);
}
