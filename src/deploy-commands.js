require('dotenv').config();
const fs = require('fs');
const path = require('path');
const { REST, Routes } = require('discord.js');

const { DISCORD_TOKEN, CLIENT_ID, GUILD_ID } = process.env;

if (!DISCORD_TOKEN || !CLIENT_ID) {
  console.error('Missing DISCORD_TOKEN or CLIENT_ID in .env — copy .env.example to .env and fill it in.');
  process.exit(1);
}

const commandsDir = path.join(__dirname, 'commands');
const commands = fs
  .readdirSync(commandsDir)
  .filter((f) => f.endsWith('.js'))
  .map((file) => require(path.join(commandsDir, file)).data.toJSON());

const rest = new REST().setToken(DISCORD_TOKEN);

(async () => {
  try {
    const route = GUILD_ID
      ? Routes.applicationGuildCommands(CLIENT_ID, GUILD_ID)
      : Routes.applicationCommands(CLIENT_ID);

    const data = await rest.put(route, { body: commands });

    console.log(
      `✅ Registered ${data.length} slash command(s) ${GUILD_ID ? `to guild ${GUILD_ID}` : 'globally (may take up to an hour to appear)'}.`
    );
  } catch (err) {
    console.error('Failed to deploy commands:', err);
    process.exit(1);
  }
})();
