const { SlashCommandBuilder } = require('discord.js');
const users = require('../storage/users');
const inventory = require('../storage/inventory');
const { leaderboardEmbed } = require('../utils/embeds');

const MEDALS = ['🥇', '🥈', '🥉'];

async function resolveUsername(client, userId) {
  try {
    const user = await client.users.fetch(userId);
    return user.username;
  } catch {
    return `Unknown (${userId})`;
  }
}

module.exports = {
  data: new SlashCommandBuilder()
    .setName('leaderboard')
    .setDescription('See the top collectors.')
    .addStringOption((opt) =>
      opt
        .setName('type')
        .setDescription('Rank by coins or by total cards owned (default: cards)')
        .addChoices({ name: 'Cards', value: 'cards' }, { name: 'Coins', value: 'coins' })
    ),

  async execute(interaction) {
    const type = interaction.options.getString('type') ?? 'cards';
    await interaction.deferReply();

    let ranked;
    if (type === 'coins') {
      ranked = users
        .getAllUsers()
        .map(([id, u]) => ({ id, value: u.coins }))
        .sort((a, b) => b.value - a.value)
        .slice(0, 10);
    } else {
      ranked = inventory
        .getAllOwners()
        .map(([id]) => ({ id, value: inventory.getTotalCardCount(id) }))
        .sort((a, b) => b.value - a.value)
        .slice(0, 10);
    }

    const lines = await Promise.all(
      ranked.map(async (row, i) => {
        const name = await resolveUsername(interaction.client, row.id);
        const medal = MEDALS[i] ?? `#${i + 1}`;
        const suffix = type === 'coins' ? 'coins' : 'cards';
        return `${medal} **${name}** — ${row.value} ${suffix}`;
      })
    );

    const embed = leaderboardEmbed(`🏆 Top Collectors (${type === 'coins' ? 'Coins' : 'Total Cards'})`, lines);
    await interaction.editReply({ embeds: [embed] });
  },
};
