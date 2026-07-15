const { EmbedBuilder } = require('discord.js');
const { RARITIES, inventoryPageSize } = require('../config');

const RARITY_MAP = new Map(RARITIES.map((r) => [r.name, r]));

function rarityMeta(rarityName) {
  return RARITY_MAP.get(rarityName) || RARITIES[0];
}

function rarityTag(rarityName) {
  const r = rarityMeta(rarityName);
  return `${r.emoji} ${r.name}`;
}

function cardEmbed(card, { ownedQty = null } = {}) {
  const r = rarityMeta(card.rarity);
  const embed = new EmbedBuilder()
    .setTitle(card.name)
    .setColor(r.color)
    .setDescription(card.flavor || null)
    .addFields(
      { name: 'Series', value: card.series || 'Unknown', inline: true },
      { name: 'Rarity', value: rarityTag(card.rarity), inline: true }
    );
  if (card.image) embed.setImage(card.image);
  if (ownedQty !== null) {
    embed.addFields({ name: 'You own', value: `${ownedQty}`, inline: true });
  }
  return embed;
}

function packResultEmbed({ packName, cost, cards, newIds, coinsLeft }) {
  const best = cards.reduce((a, b) => {
    const ai = RARITIES.findIndex((r) => r.name === a.rarity);
    const bi = RARITIES.findIndex((r) => r.name === b.rarity);
    return bi > ai ? b : a;
  }, cards[0]);
  const bestMeta = rarityMeta(best.rarity);

  const lines = cards.map((card) => {
    const r = rarityMeta(card.rarity);
    const tag = newIds.has(card.id) ? ' `NEW`' : '';
    return `${r.emoji} **${card.name}** — *${card.series || 'Unknown'}*${tag}`;
  });

  return new EmbedBuilder()
    .setTitle(`🎁 ${packName} opened!`)
    .setColor(bestMeta.color)
    .setDescription(lines.join('\n'))
    .setFooter({ text: `Cost: ${cost} coins • Balance: ${coinsLeft} coins` });
}

function inventoryPage(rows, page, ownerLabel, totalOwnedUnique, totalCards) {
  const pageSize = inventoryPageSize;
  const totalPages = Math.max(1, Math.ceil(rows.length / pageSize));
  const clampedPage = Math.min(Math.max(page, 0), totalPages - 1);
  const slice = rows.slice(clampedPage * pageSize, clampedPage * pageSize + pageSize);

  const lines = slice.length
    ? slice.map(
        ({ card, quantity }) => `${rarityMeta(card.rarity).emoji} **${card.name}** — *${card.series}* x${quantity}`
      )
    : ['No cards yet — try `/gacha` or `/standard` to open a pack!'];

  const embed = new EmbedBuilder()
    .setTitle(`📖 ${ownerLabel}'s Collection`)
    .setColor(0x5865f2)
    .setDescription(lines.join('\n'))
    .setFooter({
      text: `Page ${clampedPage + 1}/${totalPages} • ${totalOwnedUnique}/${totalCards} unique cards owned`,
    });

  return { embed, totalPages, clampedPage };
}

function shopEmbed(packs, rarities) {
  const rarityLines = rarities
    .map((r) => `${r.emoji} **${r.name}** — ${r.weight}% base odds — sells for ${r.sellValue} coins`)
    .join('\n');

  return new EmbedBuilder()
    .setTitle('🛒 Card Shop')
    .setColor(0xffd700)
    .addFields(
      {
        name: `${packs.gacha.label}`,
        value: `Pull 1: **${packs.gacha.singleCost}** coins\nPull 10: **${packs.gacha.tenCost}** coins (guaranteed ${packs.gacha.pityMinRarity}+)`,
      },
      {
        name: `${packs.standard.label}`,
        value: `${packs.standard.cardCount} cards: **${packs.standard.cost}** coins`,
      },
      { name: 'Rarity odds & sell values', value: rarityLines }
    );
}

function leaderboardEmbed(title, lines) {
  return new EmbedBuilder()
    .setTitle(title)
    .setColor(0xffd700)
    .setDescription(lines.length ? lines.join('\n') : 'No data yet.');
}

function formatDuration(ms) {
  const totalSeconds = Math.ceil(ms / 1000);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  const parts = [];
  if (hours) parts.push(`${hours}h`);
  if (minutes) parts.push(`${minutes}m`);
  if (!hours && seconds) parts.push(`${seconds}s`);
  return parts.length ? parts.join(' ') : '<1m';
}

module.exports = {
  rarityMeta,
  rarityTag,
  cardEmbed,
  packResultEmbed,
  inventoryPage,
  shopEmbed,
  leaderboardEmbed,
  formatDuration,
};
