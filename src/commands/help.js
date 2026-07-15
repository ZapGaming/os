const { SlashCommandBuilder, EmbedBuilder } = require('discord.js');

module.exports = {
  data: new SlashCommandBuilder().setName('help').setDescription('List all available commands.'),

  async execute(interaction) {
    const commands = interaction.client.commands;
    const lines = [...commands.values()]
      .sort((a, b) => a.data.name.localeCompare(b.data.name))
      .map((cmd) => `**/${cmd.data.name}** — ${cmd.data.description}`);

    const embed = new EmbedBuilder()
      .setTitle('📜 Commands')
      .setColor(0x5865f2)
      .setDescription(lines.join('\n'))
      .setFooter({ text: 'Tip: /gacha for chase cards, /standard for a cheaper 5-card pack.' });

    await interaction.reply({ embeds: [embed], ephemeral: true });
  },
};
