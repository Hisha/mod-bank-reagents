# mod-bank-reagents — beta1

AzerothCore WotLK module providing server-authoritative virtual reagent storage.

## Companion Projects

This project is part of a three-repository reagent-storage and inventory UI suite:

- **mod-bank-reagents** — AzerothCore server module (this repository)
- **BankReagentsUI** — WoW 3.3.5 client addon that integrates Reagent Storage with the profession window and enables addon-assisted remote crafting: https://github.com/Hisha/BankReagentsUI
- **UnifiedBags335** — Standalone single-bag UI for WoW 3.3.5 with Bags, Bank, Reagent Storage, and Guild Bank support: https://github.com/Hisha/UnifiedBags335

`mod-bank-reagents` works without either addon. Stock clients retain the banker gossip interface for Reagent Storage. The addons provide the enhanced graphical experience.

## Design goals

- **No AzerothCore core changes.**
- Stock 3.3.5 clients remain usable.
- Addons are optional.
- Reagent quantities are stored in the characters database rather than hidden
  client inventory slots.
- Server validation remains authoritative for deposits, withdrawals and crafting.

## Server-only functionality

At a banker, a client without the addon can use the normal gossip fallback to:

- enable/disable automatic reagent deposit;
- browse Reagent Storage;
- withdraw one, a stack, or all.

Deposit eligibility is built from actual trade-skill spell reagent lists and
excludes non-reagent item classes. Items currently needed by an active quest are
not auto-deposited.

## Optional addon protocol

`BankReagentsUI` uses the `BRG` addon-message protocol for:

- HELLO/session detection;
- paged storage synchronization;
- banker-only graphical withdrawals;
- auto-deposit preference changes;
- addon-assisted remote profession crafting.

Addon-originated withdrawals and auto-deposit changes are revalidated against
the banker captured by the module's gossip hook with
`GetNPCIfCanInteractWith(..., UNIT_NPC_FLAG_BANKER)`.

## Remote crafting

When a recipe needs virtual reagents, the addon queues the known trade-skill spell
with the server. The server starts the normal non-triggered spell cast. During
spell checks, only the missing reagent amount is temporarily materialized into
normal inventory. AzerothCore's normal spell flow then performs reagent
consumption, craft effects, skillups and related handling.

If the cast fails or is canceled, borrowed reagent quantities are rolled back
into Reagent Storage. If temporary reagents cannot fit in normal bags, the craft
is rejected before stored quantities are lost.

## Database

The module uses:

- `character_bank_reagent_settings`
- `character_bank_reagents`

Base SQL:
`data/sql/db-characters/base/001_mod_bank_reagents.sql`

## Configuration

Copy/merge:
`conf/mod_bank_reagents.conf.dist`

Important options:

- `BankReagents.Enable`
- `BankReagents.AutoDeposit.Enable`
- `BankReagents.RemoteCraft.Enable`
- `BankReagents.RemoteCraft.TransactionTimeoutSeconds`

## Companion addons

### BankReagentsUI

https://github.com/Hisha/BankReagentsUI

Profession-window integration, combined carried/stored reagent counts, remote crafting support, and the BRG client API.

### UnifiedBags335

https://github.com/Hisha/UnifiedBags335

Standalone single-bag UI with separate Bags and Bank windows, graphical Reagent Storage, account inventory counts, and Guild Bank support.

Neither addon requires changes to AzerothCore core.

## Beta validation completed

- Auto-deposit and manual withdrawal.
- Quest-needed items remain in bags.
- Mixed physical/virtual crafting.
- Virtual-only crafting.
- Canceled-craft rollback.
- Create All / repeated crafting.
- Full-bag remote-craft rejection.
- Graphical Reagent Storage withdrawals.
- Bank bag-slot purchase UI.
- Guild-bank money deposit/withdraw permissions and item transfers.

## Still worth broader testing

- Additional professions and unusual recipes.
- Item-target trade skills such as Enchanting.
- Guild-bank behavior across more complex rank/tab permission combinations.
