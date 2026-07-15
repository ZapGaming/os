const { SlashCommandBuilder } = require('discord.js');
const { packs, RARITIES, economy } = require('../config');
const { shopEmbed } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder().setName('shop').setDescription('View pack prices and rarity odds.'),

  async execute(interaction) {
    await interaction.reply({ embeds: [shopEmbed(packs, RARITIES, economy)] });
  },
};
