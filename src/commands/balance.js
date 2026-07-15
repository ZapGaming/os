const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');
const users = require('../storage/users');
const { getCollectionProgress } = require('../storage/inventory');

module.exports = {
  data: new SlashCommandBuilder().setName('balance').setDescription('Check your coin balance and collection progress.'),

  async execute(interaction) {
    const coins = users.getCoins(interaction.user.id);
    const progress = getCollectionProgress(interaction.user.id);

    const embed = new EmbedBuilder()
      .setColor(0xffd700)
      .setTitle(`${interaction.user.username}'s Wallet`)
      .addFields(
        { name: 'Coins', value: `💰 ${coins}`, inline: true },
        { name: 'Collection', value: `📖 ${progress.owned}/${progress.total} unique cards`, inline: true }
      );

    await interaction.reply({ embeds: [embed] });
  },
};
