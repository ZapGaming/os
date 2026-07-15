const { SlashCommandBuilder } = require('discord.js');
const { getAllCards, searchByName } = require('../cards');
const { getQuantity } = require('../storage/inventory');
const { cardEmbed } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder()
    .setName('card')
    .setDescription('Look up a card by name.')
    .addStringOption((opt) =>
      opt.setName('name').setDescription('Card name').setRequired(true).setAutocomplete(true)
    ),

  async autocomplete(interaction) {
    const focused = interaction.options.getFocused();
    const matches = searchByName(focused, 25);
    await interaction.respond(matches.map((c) => ({ name: `${c.name} (${c.rarity})`, value: c.id })));
  },

  async execute(interaction) {
    const input = interaction.options.getString('name', true);
    // Autocomplete sends the card id as the value; fall back to a name search
    // in case the user typed a name directly without picking a suggestion.
    const all = getAllCards();
    const card = all.find((c) => c.id === input) || searchByName(input, 1)[0];

    if (!card) {
      await interaction.reply({ content: `❌ No card found matching "${input}".`, ephemeral: true });
      return;
    }

    const owned = getQuantity(interaction.user.id, card.id);
    const embed = cardEmbed(card, { ownedQty: owned });
    await interaction.reply({ embeds: [embed] });
  },
};
