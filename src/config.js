// Central place to tune the economy, rarities and packs.
// Rarity order matters: lowest -> highest. Add/remove tiers freely,
// the rest of the bot reads from this list.
const RARITIES = [
  { name: 'Common', weight: 55, emoji: '⚪', color: 0x9e9e9e, sellValue: 15 },
  { name: 'Uncommon', weight: 27, emoji: '🟢', color: 0x4caf50, sellValue: 35 },
  { name: 'Rare', weight: 12, emoji: '🔵', color: 0x2196f3, sellValue: 80 },
  { name: 'Epic', weight: 4.5, emoji: '🟣', color: 0x9c27b0, sellValue: 200 },
  { name: 'Legendary', weight: 1.2, emoji: '🟡', color: 0xffc107, sellValue: 500 },
  { name: 'Secret', weight: 0.3, emoji: '🔴', color: 0xf44336, sellValue: 1500 },
];

const RARITY_INDEX = new Map(RARITIES.map((r, i) => [r.name, i]));

module.exports = {
  RARITIES,
  RARITY_INDEX,

  economy: {
    startingCoins: 1000,
    dailyReward: 250,
    dailyCooldownMs: 24 * 60 * 60 * 1000,
    weeklyReward: 1500,
    weeklyCooldownMs: 7 * 24 * 60 * 60 * 1000,
  },

  packs: {
    gacha: {
      label: 'Gacha Pack',
      singleCost: 150,
      tenCost: 1350, // 10% discount vs 10x singleCost
      // On a 10-pull, guarantee at least one card of this rarity or higher.
      pityMinRarity: 'Rare',
    },
    standard: {
      label: 'Standard Pack',
      cost: 500,
      cardCount: 5,
    },
    free: {
      label: 'Free Pack',
      cardCount: 1,
      cooldownMs: 6 * 60 * 60 * 1000,
    },
  },

  inventoryPageSize: 10,
};
