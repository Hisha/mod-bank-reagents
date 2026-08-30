#include "BankReagentMgr.h"

#include "Bag.h"
#include "Chat.h"
#include "Creature.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    struct BorrowedReagent
    {
        uint32 itemEntry = 0;
        uint32 borrowed = 0;
        uint32 baselineCarried = 0;
    };

    struct Authorization
    {
        uint32 spellId = 0;
        uint32 remainingCrafts = 0;
        std::chrono::steady_clock::time_point expires;
    };

    struct BorrowTransaction
    {
        uint32 spellId = 0;
        std::vector<BorrowedReagent> reagents;
        std::chrono::steady_clock::time_point started;
    };

    struct RemoteCraftRequest
    {
        uint32 spellId = 0;
        uint32 remainingCrafts = 0;
    };

    std::unordered_map<uint32, Authorization> g_authorizations;
    std::unordered_map<uint32, BorrowTransaction> g_transactions;
    std::unordered_map<uint32, RemoteCraftRequest> g_remoteCrafts;
    // Presence in this set means the optional BankReagentsUI addon has completed
    // its server handshake during the current login session.  Remote crafting is
    // gated by this session marker so stock clients retain completely stock
    // crafting behavior without relying on per-click packet ordering.
    std::unordered_set<uint32> g_addonSessions;
    // The GUID is captured from the module's banker gossip hook. It is never
    // trusted by itself; every withdrawal revalidates normal NPC interaction.
    std::unordered_map<uint32, ObjectGuid> g_bankerGuids;

    uint32 GuidLow(Player* player)
    {
        return player ? player->GetGUID().GetCounter() : 0;
    }

    uint32 ParseUInt(std::string const& text)
    {
        if (text.empty())
            return 0;
        char* end = nullptr;
        unsigned long value = std::strtoul(text.c_str(), &end, 10);
        return (end && *end == '\0') ? uint32(value) : 0;
    }

    std::vector<std::string> Split(std::string const& value, char delim)
    {
        std::vector<std::string> out;
        std::stringstream ss(value);
        std::string part;
        while (std::getline(ss, part, delim))
            out.push_back(part);
        return out;
    }
}

namespace BankReagents
{
Manager& Manager::Instance()
{
    static Manager instance;
    return instance;
}

bool Manager::IsEnabled() const
{
    return sConfigMgr->GetOption<bool>("BankReagents.Enable", true);
}

bool Manager::IsRemoteCraftEnabled() const
{
    return IsEnabled() && sConfigMgr->GetOption<bool>("BankReagents.RemoteCraft.Enable", true);
}

bool Manager::IsEligible(ItemTemplate const* proto) const
{
    if (!proto)
        return false;

    // WotLK profession-specific bag-family flags are broader than "crafting reagent".
    // They also include things such as profession tools and recipe patterns.  Instead,
    // build the storage whitelist from the reagents actually consumed by trade-skill
    // spells.  This includes unusual cooking/vendor ingredients while excluding tools,
    // patterns/formulas, and other profession-related items that are not consumed.
    static std::unordered_set<uint32> reagentEntries;
    static bool initialized = false;

    if (!initialized)
    {
        for (uint32 spellId = 1; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
                continue;

            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (info->Reagent[i] > 0 && info->ReagentCount[i] > 0)
                    reagentEntries.insert(uint32(info->Reagent[i]));
        }

        initialized = true;
    }

    if (reagentEntries.find(proto->ItemId) == reagentEntries.end())
        return false;

    // Some trade-skill recipes consume objects that are clearly not reagents in the
    // reagent-bank sense (for example a Skinning Knife used as a component of another
    // profession item, or a learnable recipe/pattern).  Keep the spell-derived whitelist
    // as the primary test, but reject item classes that represent equipment, recipes,
    // quest objects, keys, containers, ammunition, etc.
    switch (proto->Class)
    {
        case ITEM_CLASS_CONTAINER:
        case ITEM_CLASS_WEAPON:
        case ITEM_CLASS_ARMOR:
        case ITEM_CLASS_PROJECTILE:
        case ITEM_CLASS_RECIPE:
        case ITEM_CLASS_MONEY:
        case ITEM_CLASS_QUIVER:
        case ITEM_CLASS_QUEST:
        case ITEM_CLASS_KEY:
        case ITEM_CLASS_GLYPH:
            return false;
        default:
            break;
    }

    return true;
}

bool Manager::IsAutoDepositEnabled(Player* player) const
{
    if (!IsEnabled() || !player)
        return false;

    if (QueryResult result = CharacterDatabase.Query(
        "SELECT auto_deposit FROM character_bank_reagent_settings WHERE guid = {}", GuidLow(player)))
        return result->Fetch()[0].Get<uint8>() != 0;

    return false;
}

void Manager::SetAutoDeposit(Player* player, bool enabled) const
{
    if (!IsEnabled() || !player)
        return;

    CharacterDatabase.Execute(
        "INSERT INTO character_bank_reagent_settings (guid, auto_deposit) VALUES ({}, {}) "
        "ON DUPLICATE KEY UPDATE auto_deposit = VALUES(auto_deposit)",
        GuidLow(player), enabled ? 1 : 0);
}

uint64 Manager::GetStored(Player* player, uint32 itemEntry) const
{
    if (!IsEnabled() || !player || !itemEntry)
        return 0;

    if (QueryResult result = CharacterDatabase.Query(
        "SELECT quantity FROM character_bank_reagents WHERE guid = {} AND item_entry = {}",
        GuidLow(player), itemEntry))
        return result->Fetch()[0].Get<uint64>();

    return 0;
}

bool Manager::AddStored(Player* player, uint32 itemEntry, uint64 amount) const
{
    if (!player || !itemEntry || !amount)
        return false;

    CharacterDatabase.Execute(
        "INSERT INTO character_bank_reagents (guid, item_entry, quantity) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity)",
        GuidLow(player), itemEntry, amount);
    return true;
}

bool Manager::RemoveStored(Player* player, uint32 itemEntry, uint64 amount) const
{
    if (!player || !itemEntry || !amount || GetStored(player, itemEntry) < amount)
        return false;

    CharacterDatabase.Execute(
        "UPDATE character_bank_reagents SET quantity = quantity - {} WHERE guid = {} AND item_entry = {}",
        amount, GuidLow(player), itemEntry);
    CharacterDatabase.Execute(
        "DELETE FROM character_bank_reagents WHERE guid = {} AND item_entry = {} AND quantity = 0",
        GuidLow(player), itemEntry);
    return true;
}

std::vector<StorageRow> Manager::List(Player* player, uint32 offset, uint32 limit) const
{
    std::vector<StorageRow> out;
    if (!IsEnabled() || !player || !limit)
        return out;

    QueryResult result = CharacterDatabase.Query(
        "SELECT item_entry, quantity FROM character_bank_reagents WHERE guid = {} "
        "ORDER BY item_entry LIMIT {} OFFSET {}", GuidLow(player), limit, offset);

    if (!result)
        return out;

    do
    {
        Field* fields = result->Fetch();
        out.push_back({ fields[0].Get<uint32>(), fields[1].Get<uint64>() });
    } while (result->NextRow());

    return out;
}

uint32 Manager::CountTypes(Player* player) const
{
    if (!IsEnabled() || !player)
        return 0;

    if (QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM character_bank_reagents WHERE guid = {}", GuidLow(player)))
        return result->Fetch()[0].Get<uint32>();
    return 0;
}

uint32 Manager::DepositAll(Player* player) const
{
    if (!IsEnabled() || !player || !sConfigMgr->GetOption<bool>("BankReagents.AutoDeposit.Enable", true))
        return 0;

    struct DepositCandidate
    {
        uint8 bag = 0;
        uint8 slot = 0;
        uint32 entry = 0;
        uint32 count = 0;
    };

    std::vector<DepositCandidate> candidates;

    auto inspect = [&](uint8 bag, uint8 slot, Item* item)
    {
        if (!item || !IsEligible(item->GetTemplate()))
            return;

        // Never auto-deposit an item the character currently needs for an active quest.
        // A number of classic/WotLK items legitimately serve double duty as both quest
        // objectives and crafting ingredients; quest usability wins while the quest is active.
        if (player->HasQuestForItem(item->GetEntry()))
            return;

        candidates.push_back({ bag, slot, item->GetEntry(), item->GetCount() });
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        inspect(INVENTORY_SLOT_BAG_0, slot, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = player->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                inspect(bagSlot, uint8(slot), bag->GetItemByPos(slot));

    uint32 total = 0;
    for (DepositCandidate const& candidate : candidates)
    {
        // Re-read the slot before touching it. DepositAll is synchronous, but this keeps
        // the operation safe if another script moved an item while handling removal hooks.
        Item* item = player->GetItemByPos(candidate.bag, candidate.slot);
        if (!item || item->GetEntry() != candidate.entry || item->GetCount() != candidate.count ||
            !IsEligible(item->GetTemplate()) || player->HasQuestForItem(candidate.entry))
            continue;

        // DestroyItem(..., true) is important. With update=false an Item can remain marked
        // as "in world" after being detached, which later trips Object::~Object's
        // "deleted but still in world" assertion. We deposit whole stacks, so remove the
        // exact physical stack rather than aggregating by entry and calling DestroyItemCount.
        player->DestroyItem(candidate.bag, candidate.slot, true);
        AddStored(player, candidate.entry, candidate.count);
        total += candidate.count;
    }

    return total;
}

bool Manager::Withdraw(Player* player, uint32 itemEntry, uint32 amount) const
{
    if (!IsEnabled() || !player || !amount)
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
    // Existing storage may contain entries deposited by an older eligibility rule.  Never
    // strand those items: eligibility controls future deposits, not the player's ability to
    // withdraw something already credited to Reagent Storage.
    if (!proto || GetStored(player, itemEntry) < amount)
        return false;

    ItemPosCountVec dest;
    InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemEntry, amount);
    if (result != EQUIP_ERR_OK)
    {
        player->SendEquipError(result, nullptr, nullptr, itemEntry);
        return false;
    }

    if (!RemoveStored(player, itemEntry, amount))
        return false;

    if (Item* item = player->StoreNewItem(dest, itemEntry, true))
    {
        player->SendNewItem(item, amount, true, false);
        return true;
    }

    AddStored(player, itemEntry, amount);
    return false;
}

bool Manager::ArmRemoteCraft(Player* player, uint32 spellId, uint32 count, std::string& error)
{
    if (!IsRemoteCraftEnabled())
    {
        error = "Remote crafting is disabled on this server.";
        return false;
    }
    if (!player || !spellId || !count)
    {
        error = "Invalid crafting request.";
        return false;
    }

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || !player->HasSpell(spellId))
    {
        error = "That recipe is not available to this character.";
        return false;
    }

    // Require the combined carried + virtual pool to satisfy at least the first craft.
    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (info->Reagent[i] <= 0)
            continue;
        uint32 entry = info->Reagent[i];
        uint32 needed = info->ReagentCount[i];
        uint64 combined = uint64(player->GetItemCount(entry, false)) + GetStored(player, entry);
        if (combined < needed)
        {
            error = "You do not have enough reagents in your bags and Reagent Storage.";
            return false;
        }
    }

    uint32 seconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("BankReagents.RemoteCraft.AuthorizationSeconds", 5));
    g_authorizations[GuidLow(player)] = { spellId, count, std::chrono::steady_clock::now() + std::chrono::seconds(seconds) };
    return true;
}

void Manager::NoteBanker(Player* player, Creature* banker)
{
    if (!player || !banker || !(banker->GetNpcFlags() & UNIT_NPC_FLAG_BANKER))
        return;

    g_bankerGuids[GuidLow(player)] = banker->GetGUID();
}

bool Manager::CanAddonWithdraw(Player* player) const
{
    if (!player)
        return false;

    auto itr = g_bankerGuids.find(GuidLow(player));
    if (itr == g_bankerGuids.end())
        return false;

    return player->GetNPCIfCanInteractWith(itr->second, UNIT_NPC_FLAG_BANKER) != nullptr;
}

bool Manager::HasAddonSession(Player* player) const
{
    return player && g_addonSessions.find(GuidLow(player)) != g_addonSessions.end();
}

std::string Manager::HandleAddonRequest(Player* player, std::string const& request)
{
    if (!IsEnabled() || !player)
        return "ERR|disabled";

    auto parts = Split(request, '|');
    if (parts.empty())
        return "ERR|bad-request";

    if (parts[0] == "HELLO")
    {
        g_addonSessions.insert(GuidLow(player));
        return std::string("HELLO|1|") + (IsRemoteCraftEnabled() ? "1" : "0");
    }

    if (parts[0] == "SYNC")
    {
        // A successful sync also proves that the optional addon is active for
        // this login session.  Refreshing this marker here makes opening the
        // profession window sufficient even if HELLO was missed during login.
        g_addonSessions.insert(GuidLow(player));
        uint32 offset = parts.size() > 1 ? ParseUInt(parts[1]) : 0;
        constexpr uint32 pageSize = 12;
        auto rows = List(player, offset, pageSize);
        uint32 total = CountTypes(player);
        std::ostringstream out;
        out << "DATA|" << offset << "|" << ((offset + rows.size() < total) ? 1 : 0) << "|";
        bool first = true;
        for (StorageRow const& row : rows)
        {
            if (!first)
                out << ',';
            first = false;
            out << row.itemEntry << ':' << row.quantity;
        }
        return out.str();
    }

    if (parts[0] == "AUTO")
    {
        // Querying the preference is harmless, but changing it is deliberately
        // bank-only just like the original banker gossip option.
        if (parts.size() == 1)
            return std::string("AUTO|") + (IsAutoDepositEnabled(player) ? "1" : "0") + "|0";

        if (!CanAddonWithdraw(player))
            return "ERR|Auto reagent deposit can only be changed while using a banker.";

        bool enabled = ParseUInt(parts[1]) != 0;
        SetAutoDeposit(player, enabled);
        uint32 deposited = enabled ? DepositAll(player) : 0;
        return std::string("AUTO|") + (enabled ? "1" : "0") + "|" + std::to_string(deposited);
    }

    if (parts[0] == "WITHDRAW")
    {
        uint32 itemEntry = parts.size() > 1 ? ParseUInt(parts[1]) : 0;
        uint32 amount = parts.size() > 2 ? ParseUInt(parts[2]) : 0;

        // WorldSession::CanUseBank() is private on this AzerothCore branch.
        // Revalidate the banker captured by our public gossip hook instead.
        if (!CanAddonWithdraw(player))
            return "ERR|Reagent withdrawals are only available while using a banker.";

        if (!itemEntry || !amount)
            return "ERR|Invalid reagent withdrawal request.";

        uint64 stored = GetStored(player, itemEntry);
        if (!stored)
            return "ERR|That reagent is no longer in storage.";

        amount = uint32(std::min<uint64>(stored, amount));
        if (!Withdraw(player, itemEntry, amount))
            return "ERR|Unable to withdraw that reagent. Check your bag space.";

        return "WITHDRAWN|" + std::to_string(itemEntry) + "|" + std::to_string(amount);
    }

    if (parts[0] == "CRAFT")
    {
        uint32 spellId = parts.size() > 1 ? ParseUInt(parts[1]) : 0;
        uint32 count = parts.size() > 2 ? ParseUInt(parts[2]) : 1;
        uint32 guid = GuidLow(player);

        if (!IsRemoteCraftEnabled() || g_addonSessions.find(guid) == g_addonSessions.end())
            return "ERR|Remote crafting is not available for this client session.";

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || !player->HasSpell(spellId))
            return "ERR|That recipe is not available to this character.";

        count = std::max<uint32>(1, std::min<uint32>(count, 1000));

        // Validate the first craft against the combined carried + virtual pool.
        // Each subsequent craft is validated again by the normal spell check.
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (info->Reagent[i] <= 0)
                continue;

            uint32 entry = uint32(info->Reagent[i]);
            uint32 needed = info->ReagentCount[i];
            uint64 combined = uint64(player->GetItemCount(entry, false)) + GetStored(player, entry);
            if (combined < needed)
                return "ERR|You do not have enough reagents in your bags and Reagent Storage.";
        }

        g_remoteCrafts[guid] = { spellId, count };
        return "QUEUED|" + std::to_string(spellId) + "|" + std::to_string(count);
    }

    if (parts[0] == "ARM")
    {
        uint32 spellId = parts.size() > 1 ? ParseUInt(parts[1]) : 0;
        uint32 count = parts.size() > 2 ? ParseUInt(parts[2]) : 1;
        std::string error;
        if (ArmRemoteCraft(player, spellId, count, error))
            return "READY|" + std::to_string(spellId) + "|" + std::to_string(count);
        return "ERR|" + error;
    }

    if (parts[0] == "CANCEL")
    {
        ClearAuthorization(player);
        RollbackBorrow(player, "addon cancel");
        return "OK|cancelled";
    }

    return "ERR|unknown-request";
}

bool Manager::BeginBorrow(Player* player, Spell* spell, SpellCastResult& result)
{
    if (!player || !spell || result != SPELL_CAST_OK)
        return false;

    uint32 guid = GuidLow(player);
    uint32 spellId = spell->GetSpellInfo()->Id;
    SpellInfo const* info = spell->GetSpellInfo();
    bool hasSession = g_addonSessions.find(guid) != g_addonSessions.end();
    bool isTradeSkill = info && info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL);
    bool knowsSpell = player->HasSpell(spellId);
    // Remote crafting is available only to a client that completed the
    // BankReagentsUI handshake during this login session.  This deliberately
    // avoids depending on an addon-message packet arriving immediately before
    // the protected DoTradeSkill packet.
    if (!hasSession)
        return false;


    if (g_transactions.find(guid) != g_transactions.end())
        return true; // Same cast commonly receives more than one CheckCast pass.

    if (!isTradeSkill || !knowsSpell)
        return false;

    BorrowTransaction txn;
    txn.spellId = spellId;
    txn.started = std::chrono::steady_clock::now();

    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
    {
        if (info->Reagent[i] <= 0)
            continue;

        uint32 entry = info->Reagent[i];
        uint32 required = info->ReagentCount[i];
        uint32 carried = player->GetItemCount(entry, false);
        uint64 stored = GetStored(player, entry);
        if (carried >= required)
            continue;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!IsEligible(proto))
            continue; // Stock WoW handles non-storage reagents normally.

        uint32 missing = required - carried;
        if (stored < missing)
        {
            continue; // Stock reagent failure will be returned later.
        }

        ItemPosCountVec dest;
        InventoryResult inv = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, missing);
        if (inv != EQUIP_ERR_OK)
        {
            for (BorrowedReagent const& borrowed : txn.reagents)
            {
                uint32 now = player->GetItemCount(borrowed.itemEntry, false);
                if (now > borrowed.baselineCarried)
                    player->DestroyItemCount(borrowed.itemEntry, std::min(borrowed.borrowed, now - borrowed.baselineCarried), true);
                AddStored(player, borrowed.itemEntry, borrowed.borrowed);
            }
            ChatHandler(player->GetSession()).SendSysMessage(
                "You have enough reagents in Reagent Storage, but need additional bag space to use them for crafting.");
            result = SPELL_FAILED_DONT_REPORT;
            return false;
        }

        if (!RemoveStored(player, entry, missing))
            continue;

        Item* storedItem = player->StoreNewItem(dest, entry, true);
        if (!storedItem)
        {
            AddStored(player, entry, missing);
            for (BorrowedReagent const& borrowed : txn.reagents)
            {
                uint32 now = player->GetItemCount(borrowed.itemEntry, false);
                if (now > borrowed.baselineCarried)
                    player->DestroyItemCount(borrowed.itemEntry, std::min(borrowed.borrowed, now - borrowed.baselineCarried), true);
                AddStored(player, borrowed.itemEntry, borrowed.borrowed);
            }
            result = SPELL_FAILED_DONT_REPORT;
            return false;
        }

        // Do not alter binding/instance flags here: StoreNewItem may merge into a real
        // partial stack owned by the player. The transaction ledger and PlayerScript
        // guards protect the temporary loan without mutating the player's real stack.
        (void)storedItem;
        txn.reagents.push_back({ entry, missing, carried });
    }

    if (!txn.reagents.empty())
        g_transactions[guid] = std::move(txn);
    return true;
}

void Manager::CommitBorrow(Player* player, uint32 spellId)
{
    if (!player)
        return;
    uint32 guid = GuidLow(player);
    auto txn = g_transactions.find(guid);
    if (txn != g_transactions.end() && txn->second.spellId == spellId)
        g_transactions.erase(txn);

}

void Manager::RollbackBorrow(Player* player, char const* /*reason*/)
{
    if (!player)
        return;
    uint32 guid = GuidLow(player);
    auto txn = g_transactions.find(guid);
    if (txn == g_transactions.end())
        return;

    for (BorrowedReagent const& borrowed : txn->second.reagents)
    {
        uint32 now = player->GetItemCount(borrowed.itemEntry, false);
        if (now > borrowed.baselineCarried)
            player->DestroyItemCount(borrowed.itemEntry, std::min(borrowed.borrowed, now - borrowed.baselineCarried), true);
        AddStored(player, borrowed.itemEntry, borrowed.borrowed);
    }
    g_transactions.erase(txn);
}

void Manager::ClearAuthorization(Player* player)
{
    if (player)
        g_authorizations.erase(GuidLow(player));
}

void Manager::OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& result)
{
    if (!IsRemoteCraftEnabled() || !spell || !spell->GetCaster() || !spell->GetCaster()->IsPlayer())
        return;
    BeginBorrow(spell->GetCaster()->ToPlayer(), spell, result);
}

void Manager::OnSpellCast(Spell* spell, Player* player, SpellInfo const* spellInfo)
{
    if (spell && player && spellInfo)
        CommitBorrow(player, spellInfo->Id);
}

void Manager::OnSpellCancel(Spell* /*spell*/, Player* player, SpellInfo const* spellInfo)
{
    if (!player)
        return;
    if (spellInfo)
    {
        auto txn = g_transactions.find(GuidLow(player));
        if (txn != g_transactions.end() && txn->second.spellId != spellInfo->Id)
            return;
    }
    RollbackBorrow(player, "cast cancelled");
    ClearAuthorization(player);
}

void Manager::OnPlayerUpdate(Player* player)
{
    if (!player)
        return;

    uint32 guid = GuidLow(player);

    // First maintain/rollback an existing borrowed-reagent transaction.
    auto txn = g_transactions.find(guid);
    if (txn != g_transactions.end())
    {
        uint32 timeout = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("BankReagents.RemoteCraft.TransactionTimeoutSeconds", 30));
        auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - txn->second.started).count();
        if (age >= timeout)
        {
            RollbackBorrow(player, "transaction timeout");
            ClearAuthorization(player);
            g_remoteCrafts.erase(guid);
            return;
        }

        // If a failed CheckCast has already removed the spell from the current-spell slots,
        // roll the borrowed material back promptly rather than waiting for the timeout.
        if (age >= 1 && !player->FindCurrentSpellBySpellId(txn->second.spellId))
        {
            RollbackBorrow(player, "spell no longer active");
            ClearAuthorization(player);
            g_remoteCrafts.erase(guid);
        }
        return;
    }

    // A virtual-reagent craft cannot use DoTradeSkill() because the stock 3.3.5 client
    // rejects the cast locally before sending a packet when its physical bag count is low.
    // The addon therefore queues the learned recipe here and the server starts the same
    // non-triggered trade-skill spell.  From this point onward normal AzerothCore spell
    // validation, reagent consumption, effects, skillups and procs remain in control.
    auto request = g_remoteCrafts.find(guid);
    if (request == g_remoteCrafts.end())
        return;

    if (!request->second.remainingCrafts)
    {
        g_remoteCrafts.erase(request);
        return;
    }

    if (player->IsNonMeleeSpellCast(false))
        return;

    uint32 spellId = request->second.spellId;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || !player->HasSpell(spellId))
    {
        g_remoteCrafts.erase(request);
        return;
    }

    SpellCastResult castResult = player->CastSpell(player, spellId, false);
    if (castResult != SPELL_CAST_OK)
    {
        // The normal spell system has already supplied any appropriate client error.
        // If BeginBorrow emitted our bag-space explanation it used DONT_REPORT.
        g_remoteCrafts.erase(guid);
        return;
    }

    auto active = g_remoteCrafts.find(guid);
    if (active != g_remoteCrafts.end())
    {
        if (active->second.remainingCrafts > 0)
            --active->second.remainingCrafts;
        if (!active->second.remainingCrafts)
            g_remoteCrafts.erase(active);
    }
}

void Manager::OnPlayerLogout(Player* player)
{
    if (!player)
        return;
    RollbackBorrow(player, "logout");
    ClearAuthorization(player);
    g_addonSessions.erase(GuidLow(player));
    g_bankerGuids.erase(GuidLow(player));
    g_remoteCrafts.erase(GuidLow(player));
}

bool Manager::IsBorrowedItem(Player* player, uint32 itemEntry) const
{
    if (!player)
        return false;
    auto txn = g_transactions.find(GuidLow(player));
    if (txn == g_transactions.end())
        return false;
    for (BorrowedReagent const& row : txn->second.reagents)
        if (row.itemEntry == itemEntry)
            return true;
    return false;
}
}
