const { SlashCommandBuilder } = require('discord.js');
const { getInventory, getCollectionProgress } = require('../storage/inventory');
const { RARITIES } = require('../config');
const { inventoryPage } = require('../utils/embeds');
const { sendPaginated } = require('../utils/pagination');

module.exports = {
  data: new SlashCommandBuilder()
    .setName('inventory')
    .setDescription('View your card collection.')
    .addUserOption((opt) => opt.setName('user').setDescription('View someone else\'s collection'))
    .addStringOption((opt) =>
      opt
        .setName('rarity')
        .setDescription('Filter by rarity')
        .addChoices(...RARITIES.map((r) => ({ name: r.name, value: r.name })))
    ),

  async execute(interaction) {
    const target = interaction.options.getUser('user') ?? interaction.user;
    const rarityFilter = interaction.options.getString('rarity');

    await interaction.deferReply();

    let rows = getInventory(target.id);
    if (rarityFilter) rows = rows.filter((row) => row.card.rarity === rarityFilter);

    const progress = getCollectionProgress(target.id);

    await sendPaginated(interaction, (page) => {
      const { embed, totalPages } = inventoryPage(rows, page, target.username, progress.owned, progress.total);
      return { embed, totalPages };
    });
  },
};
