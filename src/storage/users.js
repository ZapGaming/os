const { JsonStore } = require('./store');
const { economy, packs } = require('../config');

const store = new JsonStore('users.json');

function getUser(userId) {
  let user = store.get(userId);
  if (!user) {
    user = { coins: economy.startingCoins, lastDaily: 0, lastWeekly: 0, lastFreePack: 0 };
    store.set(userId, user);
  }
  return user;
}

function getCoins(userId) {
  return getUser(userId).coins;
}

function addCoins(userId, amount) {
  const user = getUser(userId);
  user.coins += amount;
  store.set(userId, user);
  return user.coins;
}

/** Returns true and deducts if the user can afford `amount`, otherwise false with no change. */
function spendCoins(userId, amount) {
  const user = getUser(userId);
  if (user.coins < amount) return false;
  user.coins -= amount;
  store.set(userId, user);
  return true;
}

/** Generic cooldown check against a timestamp field on the user record. */
function checkCooldown(userId, field, cooldownMs) {
  const user = getUser(userId);
  const now = Date.now();
  const elapsed = now - (user[field] || 0);
  const ready = elapsed >= cooldownMs;
  return { ready, msRemaining: ready ? 0 : cooldownMs - elapsed };
}

function markCooldown(userId, field) {
  const user = getUser(userId);
  user[field] = Date.now();
  store.set(userId, user);
}

function getDailyStatus(userId) {
  return checkCooldown(userId, 'lastDaily', economy.dailyCooldownMs);
}

function getWeeklyStatus(userId) {
  return checkCooldown(userId, 'lastWeekly', economy.weeklyCooldownMs);
}

function getFreePackStatus(userId) {
  return checkCooldown(userId, 'lastFreePack', packs.free.cooldownMs);
}

/** Claims the daily reward if available. Returns { claimed, amount, msRemaining }. */
function claimDaily(userId) {
  const status = getDailyStatus(userId);
  if (!status.ready) return { claimed: false, amount: 0, msRemaining: status.msRemaining };
  addCoins(userId, economy.dailyReward);
  markCooldown(userId, 'lastDaily');
  return { claimed: true, amount: economy.dailyReward, msRemaining: 0 };
}

/** Claims the weekly reward if available. Returns { claimed, amount, msRemaining }. */
function claimWeekly(userId) {
  const status = getWeeklyStatus(userId);
  if (!status.ready) return { claimed: false, amount: 0, msRemaining: status.msRemaining };
  addCoins(userId, economy.weeklyReward);
  markCooldown(userId, 'lastWeekly');
  return { claimed: true, amount: economy.weeklyReward, msRemaining: 0 };
}

/** Marks the free-pack cooldown as used if available. Returns { claimed, msRemaining}. */
function claimFreePackCooldown(userId) {
  const status = getFreePackStatus(userId);
  if (!status.ready) return { claimed: false, msRemaining: status.msRemaining };
  markCooldown(userId, 'lastFreePack');
  return { claimed: true, msRemaining: 0 };
}

function getAllUsers() {
  return store.entries();
}

module.exports = {
  getUser,
  getCoins,
  addCoins,
  spendCoins,
  getDailyStatus,
  getWeeklyStatus,
  getFreePackStatus,
  claimDaily,
  claimWeekly,
  claimFreePackCooldown,
  getAllUsers,
};
