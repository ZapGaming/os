const { ActionRowBuilder, ButtonBuilder, ButtonStyle, ComponentType } = require('discord.js');

const PREV_ID = 'page_prev';
const NEXT_ID = 'page_next';

function buildRow(disablePrev, disableNext) {
  return new ActionRowBuilder().addComponents(
    new ButtonBuilder().setCustomId(PREV_ID).setLabel('◀ Prev').setStyle(ButtonStyle.Secondary).setDisabled(disablePrev),
    new ButtonBuilder().setCustomId(NEXT_ID).setLabel('Next ▶').setStyle(ButtonStyle.Secondary).setDisabled(disableNext)
  );
}

/**
 * Sends a paginated message driven by `renderPage(pageIndex)` -> { embed, totalPages }.
 * Only the original interaction user may page through it. Auto-disables after idleMs.
 */
async function sendPaginated(interaction, renderPage, { idleMs = 60_000 } = {}) {
  let page = 0;
  const { embed, totalPages } = renderPage(page);

  if (totalPages <= 1) {
    await interaction.editReply({ embeds: [embed] });
    return;
  }

  const message = await interaction.editReply({
    embeds: [embed],
    components: [buildRow(true, totalPages <= 1)],
  });

  const collector = message.createMessageComponentCollector({
    componentType: ComponentType.Button,
    time: idleMs,
  });

  collector.on('collect', async (btn) => {
    if (btn.user.id !== interaction.user.id) {
      await btn.reply({ content: "This isn't your menu — run the command yourself!", ephemeral: true });
      return;
    }

    if (btn.customId === PREV_ID) page = Math.max(0, page - 1);
    if (btn.customId === NEXT_ID) page = Math.min(totalPages - 1, page + 1);

    const rendered = renderPage(page);
    await btn.update({
      embeds: [rendered.embed],
      components: [buildRow(page === 0, page === totalPages - 1)],
    });
  });

  collector.on('end', async () => {
    try {
      await interaction.editReply({ components: [] });
    } catch {
      // message may have been deleted; nothing to clean up
    }
  });
}

module.exports = { sendPaginated };
