const { SlashCommandBuilder } = require('discord.js');
const users = require('../storage/users');
const inventory = require('../storage/inventory');
const { openPack } = require('../gacha');
const { packs } = require('../config');
const { packResultEmbed } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder().setName('standard').setDescription(`Open a ${packs.standard.label} (${packs.standard.cardCount} cards).`),

  async execute(interaction) {
    const cost = packs.standard.cost;
    const userId = interaction.user.id;

    if (!users.spendCoins(userId, cost)) {
      await interaction.reply({
        content: `❌ You need **${cost} coins** to open a ${packs.standard.label}. You have **${users.getCoins(userId)}**.`,
        ephemeral: true,
      });
      return;
    }

    const cards = openPack(packs.standard.cardCount);

    const newIds = new Set();
    for (const card of cards) {
      const ownedBefore = inventory.getQuantity(userId, card.id);
      if (ownedBefore === 0) newIds.add(card.id);
      inventory.addCard(userId, card.id, 1);
    }

    const embed = packResultEmbed({
      packName: packs.standard.label,
      cost,
      cards,
      newIds,
      coinsLeft: users.getCoins(userId),
    });

    await interaction.reply({ embeds: [embed] });
  },
};
