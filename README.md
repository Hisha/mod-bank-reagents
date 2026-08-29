# mod-bank-reagents

A server-only virtual crafting-reagent store for AzerothCore 3.3.5a, with optional remote crafting support when `BankReagentsUI` is installed on the client.

## Design goals

- **No AzerothCore core modifications.**
- Stock clients retain stock crafting behavior.
- All clients can use banker gossip to enable/disable automatic reagent deposit and withdraw stored reagents.
- Reagent eligibility uses WoW profession-bag family flags (herbs, mining, leatherworking, enchanting, engineering, gems, inscription).
- With `BankReagentsUI`, the stock profession window displays `bags + Reagent Storage` counts and can request remote crafting.
- The server remains authoritative for every remote craft.

## Server behavior

At a banker, the module adds:

- `Enable auto reagent deposit` / `Disable auto reagent deposit`
- `Reagent Storage`

When auto deposit is enabled, eligible reagents carried in normal inventory/bags are moved into virtual storage whenever the banker is opened.

## Remote crafting

Remote crafting is intentionally gated by the addon protocol. A stock client never arms the feature, so its crafting path is unchanged.

For an addon-authorized profession cast, the module:

1. Calculates the missing eligible reagents.
2. Verifies the player has enough in virtual storage.
3. Uses AzerothCore's normal `CanStoreNewItem` calculation to ensure the missing amount can temporarily fit in normal bags/partial stacks.
4. Debits virtual storage and temporarily materializes only the missing amount.
5. Lets stock AzerothCore perform its normal reagent validation and `TakeReagents()`.
6. On a successful cast, the temporary loan is committed; on cancellation/failure/logout/timeout it is rolled back.

If the temporary amount cannot fit, the craft is stopped with a message explaining that additional bag space is required.

## Database

`data/sql/db-characters/base/001_mod_bank_reagents.sql` creates:

- `character_bank_reagent_settings`
- `character_bank_reagents`

## Configuration

Copy `conf/mod_bank_reagents.conf.dist` through the normal AzerothCore module configuration install process.

## Client addon

Install the companion `BankReagentsUI` folder under:

`World of Warcraft/Interface/AddOns/BankReagentsUI/`

The addon is optional. Without it, Reagent Storage and banker auto-deposit still work, while crafting remains stock WoW.

## Status

This is the first module-only implementation targeted at the supplied AzerothCore/Playerbot game-source interfaces. Build and runtime testing on the user's full tree is still required.