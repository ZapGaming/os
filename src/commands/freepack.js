const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');
const users = require('../storage/users');
const inventory = require('../storage/inventory');
const { openPack } = require('../gacha');
const { packs } = require('../config');
const { packResultEmbed, formatDuration } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder().setName('freepack').setDescription(`Claim a free pack (once every 6 hours).`),

  async execute(interaction) {
    const userId = interaction.user.id;
    const result = users.claimFreePackCooldown(userId);

    if (!result.claimed) {
      const embed = new EmbedBuilder()
        .setColor(0xe74c3c)
        .setDescription(`⏳ Your free pack is on cooldown. Come back in **${formatDuration(result.msRemaining)}**.`);
      await interaction.reply({ embeds: [embed], ephemeral: true });
      return;
    }

    const cards = openPack(packs.free.cardCount);

    const newIds = new Set();
    for (const card of cards) {
      const ownedBefore = inventory.getQuantity(userId, card.id);
      if (ownedBefore === 0) newIds.add(card.id);
      inventory.addCard(userId, card.id, 1);
    }

    const embed = packResultEmbed({
      packName: packs.free.label,
      cost: 0,
      cards,
      newIds,
      coinsLeft: users.getCoins(userId),
    });

    await interaction.reply({ embeds: [embed] });
  },
};
