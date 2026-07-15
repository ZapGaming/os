# Anime TCG Gacha Bot

A self-contained Discord bot for an anime-style trading card game. Users earn
coins, open packs, and collect cards — no admin/moderation commands, just the
gacha game.

## Features

- **Gacha Pack** — `/gacha amount:1` or `/gacha amount:10` (10-pull gets a
  discount and a guaranteed Rare-or-better card).
- **Standard Pack** — `/standard`, always 5 cards, cheaper flat price.
- **Economy** — starting coins, `/daily` reward with a 24h cooldown, `/balance`.
- **Collection** — `/inventory` (paginated, filterable by rarity, viewable for
  other users too), `/card <name>` to inspect a single card, `/sell` to
  liquidate duplicates for coins.
- **/leaderboard** — top collectors by total cards or by coins.
- **/shop** — pack prices and rarity odds at a glance.
- **/help** — lists every command.
- No admin/mod commands of any kind — everything here is player-facing.

Data is stored in plain JSON files under `src/storage/db/` (created
automatically on first run) — no database server or native build tools
required.

## Setup

1. **Install dependencies**

   ```bash
   npm install
   ```

2. **Create a Discord application + bot**
   - Go to the [Discord Developer Portal](https://discord.com/developers/applications) → New Application.
   - Bot tab → Reset Token → copy it.
   - General Information tab → copy the Application ID.
   - OAuth2 → URL Generator → scopes `bot` + `applications.commands` →
     permissions: none needed beyond sending messages/embeds — use the
     generated URL to invite the bot to your server.

3. **Configure environment**

   ```bash
   cp .env.example .env
   ```

   Fill in:
   - `DISCORD_TOKEN` — the bot token.
   - `CLIENT_ID` — the application ID.
   - `GUILD_ID` *(optional, recommended while testing)* — a server ID so
     slash commands register instantly there instead of waiting up to an
     hour for global propagation.

4. **Register the slash commands**

   ```bash
   npm run deploy
   ```

   Re-run this any time you add/remove/rename a command.

5. **Start the bot**

   ```bash
   npm start
   ```

## Adding real cards later

The full card pool lives in [`src/data/cards.json`](src/data/cards.json). It's
a plain JSON array — no code changes needed to add, remove, or edit cards.
Each entry looks like:

```json
{
  "id": "c001",
  "name": "Fireheart Ronin",
  "series": "Blade Chronicles",
  "rarity": "Common",
  "image": "https://example.com/card-art.png",
  "flavor": "A short flavor-text line shown on the card."
}
```

- `id` — must be unique and, once used, should stay stable (it's how a
  player's collection is tracked — changing an existing card's `id` will
  effectively turn it into a new/blank card for players who already own it).
- `rarity` — must exactly match one of the tiers defined in `src/config.js`
  (`Common`, `Uncommon`, `Rare`, `Epic`, `Legendary`, `Secret` by default).
- `image` — optional; leave as `""` to omit card art.

Just edit the file and restart the bot (`npm start`) — the pool reloads from
disk on boot. You can replace the placeholder cards wholesale or add to them;
existing player inventories reference cards by `id`, so old ids you keep
stay intact.

## Tuning the economy / odds

Everything else — rarity weights, colors, emojis, sell values, pack prices,
starting coins, daily reward amount/cooldown, and the 10-pull pity rule —
lives in [`src/config.js`](src/config.js) with comments explaining each knob.

## Project structure

```
src/
  index.js            bot entrypoint (client + interaction handling)
  deploy-commands.js  registers slash commands with Discord
  config.js           rarities, pack prices, economy settings
  cards.js            loads/queries the card pool from data/cards.json
  gacha.js            weighted rarity rolls + pack opening (with pity)
  data/cards.json     the card pool — edit this to add real cards
  storage/            JSON-file-backed persistence (users, inventory)
  utils/              embed builders, button pagination helper
  commands/           one file per slash command
```
