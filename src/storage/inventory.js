const { JsonStore } = require('./store');
const { getCardById, getAllCards } = require('../cards');
const { RARITY_INDEX } = require('../config');

const store = new JsonStore('inventory.json');

function getRaw(userId) {
  return store.get(userId) || {};
}

/** Adds `qty` copies of a card to a user's collection. Returns the new quantity owned. */
function addCard(userId, cardId, qty = 1) {
  const inv = getRaw(userId);
  inv[cardId] = (inv[cardId] || 0) + qty;
  store.set(userId, inv);
  return inv[cardId];
}

function getQuantity(userId, cardId) {
  return getRaw(userId)[cardId] || 0;
}

/** Removes up to `qty` copies of a card. Returns the number actually removed. */
function removeCard(userId, cardId, qty = 1) {
  const inv = getRaw(userId);
  const owned = inv[cardId] || 0;
  const removed = Math.min(owned, qty);
  if (removed <= 0) return 0;
  const remaining = owned - removed;
  if (remaining <= 0) {
    delete inv[cardId];
  } else {
    inv[cardId] = remaining;
  }
  store.set(userId, inv);
  return removed;
}

/** Returns [{ card, quantity }], sorted highest rarity first, then by name. */
function getInventory(userId) {
  const inv = getRaw(userId);
  const rows = [];
  for (const [cardId, quantity] of Object.entries(inv)) {
    const card = getCardById(cardId);
    if (card && quantity > 0) rows.push({ card, quantity });
  }
  rows.sort((a, b) => {
    const rd = RARITY_INDEX.get(b.card.rarity) - RARITY_INDEX.get(a.card.rarity);
    if (rd !== 0) return rd;
    return a.card.name.localeCompare(b.card.name);
  });
  return rows;
}

function getTotalCardCount(userId) {
  const inv = getRaw(userId);
  return Object.values(inv).reduce((sum, qty) => sum + qty, 0);
}

function getUniqueCardCount(userId) {
  const inv = getRaw(userId);
  return Object.keys(inv).filter((id) => inv[id] > 0).length;
}

function getCollectionProgress(userId) {
  return { owned: getUniqueCardCount(userId), total: getAllCards().length };
}

function getAllOwners() {
  return store.entries();
}

module.exports = {
  addCard,
  getQuantity,
  removeCard,
  getInventory,
  getTotalCardCount,
  getUniqueCardCount,
  getCollectionProgress,
  getAllOwners,
};
