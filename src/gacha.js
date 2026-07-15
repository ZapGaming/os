const { RARITIES, RARITY_INDEX } = require('./config');
const { getCardsForRarity } = require('./cards');

const TOTAL_WEIGHT = RARITIES.reduce((sum, r) => sum + r.weight, 0);

/** Rolls a single rarity tier according to configured weights. */
function rollRarity() {
  let roll = Math.random() * TOTAL_WEIGHT;
  for (const rarity of RARITIES) {
    if (roll < rarity.weight) return rarity.name;
    roll -= rarity.weight;
  }
  return RARITIES[0].name;
}

function drawCard() {
  const rarity = rollRarity();
  const pool = getCardsForRarity(rarity);
  const card = pool[Math.floor(Math.random() * pool.length)];
  return card;
}

/** Rerolls a single card guaranteed to be at or above `minRarity`. */
function drawCardAtLeast(minRarityName) {
  const minIdx = RARITY_INDEX.get(minRarityName);
  const eligible = RARITIES.filter((r) => RARITY_INDEX.get(r.name) >= minIdx);
  const weightSum = eligible.reduce((sum, r) => sum + r.weight, 0);
  let roll = Math.random() * weightSum;
  let chosen = eligible[eligible.length - 1].name;
  for (const rarity of eligible) {
    if (roll < rarity.weight) {
      chosen = rarity.name;
      break;
    }
    roll -= rarity.weight;
  }
  const pool = getCardsForRarity(chosen);
  return pool[Math.floor(Math.random() * pool.length)];
}

function openPack(count, { pityMinRarity } = {}) {
  const cards = Array.from({ length: count }, () => drawCard());

  if (pityMinRarity) {
    const minIdx = RARITY_INDEX.get(pityMinRarity);
    const hasPity = cards.some((c) => RARITY_INDEX.get(c.rarity) >= minIdx);
    if (!hasPity) {
      cards[cards.length - 1] = drawCardAtLeast(pityMinRarity);
    }
  }

  return cards;
}

module.exports = {
  rollRarity,
  drawCard,
  openPack,
};
