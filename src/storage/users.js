const { JsonStore } = require('./store');
const { economy } = require('../config');

const store = new JsonStore('users.json');

function getUser(userId) {
  let user = store.get(userId);
  if (!user) {
    user = { coins: economy.startingCoins, lastDaily: 0 };
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

function getDailyStatus(userId) {
  const user = getUser(userId);
  const now = Date.now();
  const elapsed = now - user.lastDaily;
  const ready = elapsed >= economy.dailyCooldownMs;
  return { ready, msRemaining: ready ? 0 : economy.dailyCooldownMs - elapsed };
}

/** Claims the daily reward if available. Returns { claimed, amount, msRemaining }. */
function claimDaily(userId) {
  const user = getUser(userId);
  const now = Date.now();
  const elapsed = now - user.lastDaily;
  if (elapsed < economy.dailyCooldownMs) {
    return { claimed: false, amount: 0, msRemaining: economy.dailyCooldownMs - elapsed };
  }
  user.lastDaily = now;
  user.coins += economy.dailyReward;
  store.set(userId, user);
  return { claimed: true, amount: economy.dailyReward, msRemaining: 0 };
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
  claimDaily,
  getAllUsers,
};
