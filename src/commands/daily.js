const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');
const users = require('../storage/users');
const { formatDuration } = require('../utils/embeds');

module.exports = {
  data: new SlashCommandBuilder().setName('daily').setDescription('Claim your daily coin reward.'),

  async execute(interaction) {
    const result = users.claimDaily(interaction.user.id);

    if (!result.claimed) {
      const embed = new EmbedBuilder()
        .setColor(0xe74c3c)
        .setDescription(`⏳ You already claimed today's reward. Come back in **${formatDuration(result.msRemaining)}**.`);
      await interaction.reply({ embeds: [embed], ephemeral: true });
      return;
    }

    const embed = new EmbedBuilder()
      .setColor(0x2ecc71)
      .setDescription(`💰 You claimed your daily reward of **${result.amount} coins**!\nNew balance: **${users.getCoins(interaction.user.id)}** coins.`);
    await interaction.reply({ embeds: [embed] });
  },
};
