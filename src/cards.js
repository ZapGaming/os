const fs = require('fs');
const path = require('path');
const { RARITIES, RARITY_INDEX } = require('./config');

const CARDS_PATH = path.join(__dirname, 'data', 'cards.json');

let allCards = [];
let byId = new Map();
let byRarity = new Map();

function load() {
  const raw = fs.readFileSync(CARDS_PATH, 'utf8');
  const parsed = JSON.parse(raw);

  allCards = [];
  byId = new Map();
  byRarity = new Map(RARITIES.map((r) => [r.name, []]));

  const seenIds = new Set();
  for (const card of parsed) {
    if (!card.id || !card.name || !card.rarity) continue;
    if (seenIds.has(card.id)) {
      throw new Error(`Duplicate card id in cards.json: ${card.id}`);
    }
    if (!RARITY_INDEX.has(card.rarity)) {
      throw new Error(
        `Card "${card.name}" (${card.id}) has unknown rarity "${card.rarity}". ` +
          `Valid rarities: ${RARITIES.map((r) => r.name).join(', ')}`
      );
    }
    seenIds.add(card.id);
    allCards.push(card);
    byId.set(card.id, card);
    byRarity.get(card.rarity).push(card);
  }

  if (allCards.length === 0) {
    throw new Error('No valid cards loaded from cards.json — the bot needs at least one card.');
  }
}

load();

function getAllCards() {
  return allCards;
}

function getCardById(id) {
  return byId.get(id) || null;
}

/**
 * Returns cards for the given rarity, falling back to the nearest rarity
 * (first downward, then upward) if that tier has no cards defined yet.
 * Keeps the bot functional while a card pool is still being filled in.
 */
function getCardsForRarity(rarityName) {
  const direct = byRarity.get(rarityName);
  if (direct && direct.length > 0) return direct;

  const idx = RARITY_INDEX.get(rarityName);
  for (let i = idx - 1; i >= 0; i--) {
    const pool = byRarity.get(RARITIES[i].name);
    if (pool.length > 0) return pool;
  }
  for (let i = idx + 1; i < RARITIES.length; i++) {
    const pool = byRarity.get(RARITIES[i].name);
    if (pool.length > 0) return pool;
  }
  return allCards;
}

function searchByName(query, limit = 25) {
  const q = query.trim().toLowerCase();
  const results = q
    ? allCards.filter((c) => c.name.toLowerCase().includes(q))
    : allCards.slice();
  return results.slice(0, limit);
}

module.exports = {
  load,
  getAllCards,
  getCardById,
  getCardsForRarity,
  searchByName,
};
