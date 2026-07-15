const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');
const { getCardById } = require('../cards');
const inventory = require('../storage/inventory');
const users = require('../storage/users');
const { rarityMeta } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder()
    .setName('sell')
    .setDescription('Sell duplicate cards from your collection for coins.')
    .addStringOption((opt) =>
      opt.setName('name').setDescription('Card name').setRequired(true).setAutocomplete(true)
    )
    .addIntegerOption((opt) =>
      opt.setName('amount').setDescription('How many copies to sell (default: 1)').setMinValue(1)
    ),

  async autocomplete(interaction) {
    const focused = interaction.options.getFocused().toLowerCase();
    const owned = inventory.getInventory(interaction.user.id);
    const matches = owned
      .filter(({ card }) => card.name.toLowerCase().includes(focused))
      .slice(0, 25)
      .map(({ card, quantity }) => ({ name: `${card.name} (x${quantity})`, value: card.id }));
    await interaction.respond(matches);
  },

  async execute(interaction) {
    const cardId = interaction.options.getString('name', true);
    const amount = interaction.options.getInteger('amount') ?? 1;
    const userId = interaction.user.id;

    const card = getCardById(cardId);
    if (!card) {
      await interaction.reply({ content: `❌ No card found matching "${cardId}". Pick a suggestion from the autocomplete list.`, ephemeral: true });
      return;
    }

    const owned = inventory.getQuantity(userId, card.id);
    if (owned < amount) {
      await interaction.reply({
        content: `❌ You only own **${owned}x ${card.name}**, can't sell ${amount}.`,
        ephemeral: true,
      });
      return;
    }

    const removed = inventory.removeCard(userId, card.id, amount);
    const sellValue = rarityMeta(card.rarity).sellValue * removed;
    const newBalance = users.addCoins(userId, sellValue);

    const embed = new EmbedBuilder()
      .setColor(0x2ecc71)
      .setDescription(`💸 Sold **${removed}x ${card.name}** for **${sellValue} coins**.\nNew balance: **${newBalance}** coins.`);

    await interaction.reply({ embeds: [embed] });
  },
};
