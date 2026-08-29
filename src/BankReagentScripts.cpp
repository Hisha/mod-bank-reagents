#include "BankReagentMgr.h"

#include "AllCreatureScript.h"
#include "AllSpellScript.h"
#include "Chat.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>

namespace
{
    constexpr uint32 Sender = GOSSIP_SENDER_MAIN;
    constexpr uint32 ActionToggle = 0x70000001;
    constexpr uint32 ActionList = 0x70000002;
    constexpr uint32 ActionBack = 0x70000003;
    constexpr uint32 ActionItemMask = 0x10000000;
    constexpr uint32 ActionOneMask = 0x20000000;
    constexpr uint32 ActionStackMask = 0x30000000;
    constexpr uint32 ActionAllMask = 0x40000000;
    constexpr uint32 ActionPageMask = 0x50000000;
    constexpr uint32 ValueMask = 0x0FFFFFFF;
    constexpr uint32 PageSize = 10;

    bool IsBanker(Creature* creature)
    {
        return creature && (creature->GetNpcFlags() & UNIT_NPC_FLAG_BANKER);
    }

    void SendBankerMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        bool enabled = sBankReagents.IsAutoDepositEnabled(player);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            enabled ? "Disable auto reagent deposit" : "Enable auto reagent deposit",
            Sender, ActionToggle);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Reagent Storage", Sender, ActionList);
        player->SendPreparedGossip(creature);
    }

    void SendList(Player* player, Creature* creature, uint32 page)
    {
        ClearGossipMenuFor(player);
        uint32 offset = page * PageSize;
        auto rows = sBankReagents.List(player, offset, PageSize);

        if (rows.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "(No reagents stored)", Sender, ActionBack);
        else
            for (BankReagents::StorageRow const& row : rows)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(row.itemEntry);
                std::string name = proto ? proto->Name1 : ("Item " + std::to_string(row.itemEntry));
                AddGossipItemFor(player, GOSSIP_ICON_VENDOR, name + " x" + std::to_string(row.quantity), Sender, ActionItemMask | row.itemEntry);
            }

        if (page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous page", Sender, ActionPageMask | (page - 1));
        if (offset + rows.size() < sBankReagents.CountTypes(player))
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next page", Sender, ActionPageMask | (page + 1));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", Sender, ActionBack);
        SendGossipMenuFor(player, creature->GetGossipMenuId(), creature);
    }

    void SendItemMenu(Player* player, Creature* creature, uint32 entry)
    {
        ClearGossipMenuFor(player);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        uint64 stored = sBankReagents.GetStored(player, entry);
        std::string name = proto ? proto->Name1 : ("Item " + std::to_string(entry));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, name + " - stored: " + std::to_string(stored), Sender, ActionList);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Withdraw 1", Sender, ActionOneMask | entry);
        if (proto)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Withdraw one stack", Sender, ActionStackMask | entry);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Withdraw all", Sender, ActionAllMask | entry);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to reagent storage", Sender, ActionList);
        SendGossipMenuFor(player, creature->GetGossipMenuId(), creature);
    }

    class BankReagentCreatureScript : public AllCreatureScript
    {
    public:
        BankReagentCreatureScript() : AllCreatureScript("BankReagentCreatureScript") { }

        bool CanCreatureGossipHello(Player* player, Creature* creature) override
        {
            if (!sBankReagents.IsEnabled() || !IsBanker(creature))
                return false;

            sBankReagents.NoteBanker(player, creature);

            if (sBankReagents.IsAutoDepositEnabled(player))
            {
                uint32 deposited = sBankReagents.DepositAll(player);
                if (deposited)
                    ChatHandler(player->GetSession()).PSendSysMessage("Deposited {} crafting reagents into Reagent Storage.", deposited);
            }

            // Addon clients use the graphical bank/reagent-storage interface.
            // Send the same bank-open packet the native banker gossip action uses,
            // so BANKFRAME_OPENED still drives the client UI and bank access remains
            // a normal server-authorized banker session.  Clients without the addon
            // keep the original gossip-based reagent storage fallback below.
            if (sBankReagents.HasAddonSession(player))
            {
                player->GetSession()->SendShowBank(creature->GetGUID());
                return true;
            }

            SendBankerMenu(player, creature);
            return true;
        }

        bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
        {
            if (!sBankReagents.IsEnabled() || !IsBanker(creature) || sender != Sender)
                return false;

            sBankReagents.NoteBanker(player, creature);

            if (action == ActionToggle)
            {
                bool enabled = !sBankReagents.IsAutoDepositEnabled(player);
                sBankReagents.SetAutoDeposit(player, enabled);
                if (enabled)
                {
                    uint32 deposited = sBankReagents.DepositAll(player);
                    ChatHandler(player->GetSession()).PSendSysMessage("Auto reagent deposit enabled. Deposited {} reagents.", deposited);
                }
                else
                    ChatHandler(player->GetSession()).SendSysMessage("Auto reagent deposit disabled.");
                SendBankerMenu(player, creature);
                return true;
            }

            if (action == ActionList) { SendList(player, creature, 0); return true; }
            if (action == ActionBack) { SendBankerMenu(player, creature); return true; }

            uint32 kind = action & 0xF0000000;
            uint32 value = action & ValueMask;
            if (kind == ActionPageMask) { SendList(player, creature, value); return true; }
            if (kind == ActionItemMask) { SendItemMenu(player, creature, value); return true; }

            if (kind == ActionOneMask || kind == ActionStackMask || kind == ActionAllMask)
            {
                uint64 stored64 = sBankReagents.GetStored(player, value);
                uint32 stored = uint32(std::min<uint64>(stored64, 0xFFFFFFFFu));
                uint32 amount = 1;
                if (kind == ActionStackMask)
                    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(value))
                        amount = std::min(stored, proto->GetMaxStackSize());
                if (kind == ActionAllMask)
                    amount = stored;

                if (!amount || !sBankReagents.Withdraw(player, value, amount))
                    ChatHandler(player->GetSession()).SendSysMessage("Unable to withdraw that reagent. Check your bag space.");
                SendItemMenu(player, creature, value);
                return true;
            }
            return false;
        }
    };

    class BankReagentSpellScript : public AllSpellScript
    {
    public:
        BankReagentSpellScript() : AllSpellScript("BankReagentSpellScript") { }

        void OnSpellCheckCast(Spell* spell, bool strict, SpellCastResult& result) override
        {
            sBankReagents.OnSpellCheckCast(spell, strict, result);
        }

        void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
        {
            if (caster && caster->IsPlayer())
                sBankReagents.OnSpellCast(spell, caster->ToPlayer(), spellInfo);
        }

        void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*bySelf*/) override
        {
            if (caster && caster->IsPlayer())
                sBankReagents.OnSpellCancel(spell, caster->ToPlayer(), spellInfo);
        }
    };

    class BankReagentPlayerScript : public PlayerScript
    {
    public:
        BankReagentPlayerScript() : PlayerScript("BankReagentPlayerScript") { }

        void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
        {
            // WoW addon messages arrive as "PREFIX\tpayload" in LANG_ADDON chat.
            // We deliberately use a self-whisper request/response. Replacing the payload lets
            // the stock chat handler deliver the response back to the requesting addon.
            if (!player || lang != LANG_ADDON || type != CHAT_MSG_WHISPER)
                return;

            static std::string const prefix = "BRG\t";
            if (msg.compare(0, prefix.size(), prefix) != 0)
                return;

            std::string response = sBankReagents.HandleAddonRequest(player, msg.substr(prefix.size()));
            msg = prefix + response;
            if (msg.size() > 250)
                msg = prefix + "ERR|response-too-large";
        }

        void OnPlayerAfterUpdate(Player* player, uint32 /*diff*/) override
        {
            sBankReagents.OnPlayerUpdate(player);
        }

        void OnPlayerBeforeLogout(Player* player) override
        {
            sBankReagents.OnPlayerLogout(player);
        }

        bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
        {
            return !item || !sBankReagents.IsBorrowedItem(player, item->GetEntry());
        }

        bool OnPlayerCanSendMail(Player* player, ObjectGuid /*receiverGuid*/, ObjectGuid /*mailbox*/, std::string& /*subject*/,
            std::string& /*body*/, uint32 /*money*/, uint32 /*COD*/, Item* item) override
        {
            return !item || !sBankReagents.IsBorrowedItem(player, item->GetEntry());
        }

        bool OnPlayerCanSetTradeItem(Player* player, Item* tradedItem, uint8 /*tradeSlot*/) override
        {
            return !tradedItem || !sBankReagents.IsBorrowedItem(player, tradedItem->GetEntry());
        }
    };
}

void Addmod_bank_reagentsScripts()
{
    new BankReagentCreatureScript();
    new BankReagentSpellScript();
    new BankReagentPlayerScript();
}
