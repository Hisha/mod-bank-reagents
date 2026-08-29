#ifndef MOD_BANK_REAGENT_MGR_H
#define MOD_BANK_REAGENT_MGR_H

#include "Define.h"
#include <string>
#include <utility>
#include <vector>

class ItemTemplate;
class Player;
class Spell;
class SpellInfo;

enum SpellCastResult : uint8;

namespace BankReagents
{
    struct StorageRow
    {
        uint32 itemEntry = 0;
        uint64 quantity = 0;
    };

    class Manager
    {
    public:
        static Manager& Instance();

        bool IsEnabled() const;
        bool IsRemoteCraftEnabled() const;
        bool IsEligible(ItemTemplate const* proto) const;

        bool IsAutoDepositEnabled(Player* player) const;
        void SetAutoDeposit(Player* player, bool enabled) const;

        uint64 GetStored(Player* player, uint32 itemEntry) const;
        std::vector<StorageRow> List(Player* player, uint32 offset, uint32 limit) const;
        uint32 CountTypes(Player* player) const;

        uint32 DepositAll(Player* player) const;
        bool Withdraw(Player* player, uint32 itemEntry, uint32 amount) const;

        // Addon protocol. Input excludes the "BRG\t" prefix.
        std::string HandleAddonRequest(Player* player, std::string const& request);

        // Remote-crafting transaction lifecycle.
        void OnSpellCheckCast(Spell* spell, bool strict, SpellCastResult& result);
        void OnSpellCast(Spell* spell, Player* player, SpellInfo const* spellInfo);
        void OnSpellCancel(Spell* spell, Player* player, SpellInfo const* spellInfo);
        void OnPlayerUpdate(Player* player);
        void OnPlayerLogout(Player* player);

        bool IsBorrowedItem(Player* player, uint32 itemEntry) const;

    private:
        Manager() = default;

        bool ArmRemoteCraft(Player* player, uint32 spellId, uint32 count, std::string& error);
        bool BeginBorrow(Player* player, Spell* spell, SpellCastResult& result);
        void CommitBorrow(Player* player, uint32 spellId);
        void RollbackBorrow(Player* player, char const* reason);
        void ClearAuthorization(Player* player);

        bool AddStored(Player* player, uint32 itemEntry, uint64 amount) const;
        bool RemoveStored(Player* player, uint32 itemEntry, uint64 amount) const;
    };
}

#define sBankReagents BankReagents::Manager::Instance()

#endif
