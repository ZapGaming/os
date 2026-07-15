const { SlashCommandBuilder } = require('discord.js');
const users = require('../storage/users');
const inventory = require('../storage/inventory');
const { openPack } = require('../gacha');
const { packs } = require('../config');
const { packResultEmbed } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder()
    .setName('gacha')
    .setDescription('Open a Gacha Pack for a chance at rare anime cards.')
    .addIntegerOption((opt) =>
      opt
        .setName('amount')
        .setDescription('Pull 1 card or 10 cards (default: 1)')
        .addChoices({ name: '1 pull', value: 1 }, { name: '10 pulls', value: 10 })
    ),

  async execute(interaction) {
    const amount = interaction.options.getInteger('amount') ?? 1;
    const cost = amount === 10 ? packs.gacha.tenCost : packs.gacha.singleCost;
    const userId = interaction.user.id;

    if (!users.spendCoins(userId, cost)) {
      await interaction.reply({
        content: `❌ You need **${cost} coins** to open a ${packs.gacha.label} (${amount} pull${amount > 1 ? 's' : ''}). You have **${users.getCoins(userId)}**.`,
        ephemeral: true,
      });
      return;
    }

    const cards = openPack(amount, { pityMinRarity: amount === 10 ? packs.gacha.pityMinRarity : null });

    const newIds = new Set();
    for (const card of cards) {
      const ownedBefore = inventory.getQuantity(userId, card.id);
      if (ownedBefore === 0) newIds.add(card.id);
      inventory.addCard(userId, card.id, 1);
    }

    const embed = packResultEmbed({
      packName: `${packs.gacha.label} (${amount} pull${amount > 1 ? 's' : ''})`,
      cost,
      cards,
      newIds,
      coinsLeft: users.getCoins(userId),
    });

    await interaction.reply({ embeds: [embed] });
  },
};
