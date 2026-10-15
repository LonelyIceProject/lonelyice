# LLM Chatter

[LLM Chatter](https://github.com/LonelyIceProject/mod-llm-chatter-plugin) adds LLM-generated
conversations, personalities and memories to Playerbots. The LonelyIce port packages
[Hokken's upstream module](https://github.com/Hokken/mod-llm-chatter) with compatibility patches
and a self-contained bridge. Install `mod-llm-chatter` from the package catalog; Playerbots is its dependency.

## Configure and run

Open **Settings → LLM Chatter** in the GUI or TUI. Choose a provider, enter the model ID and API key,
or set the Ollama address, then enable the module and start the server. The TUI only configures the
profile. All 544 settings are available there, including upstream advanced options. English hints
retain the upstream descriptions. Changes require a server restart.

Supported providers are Anthropic, OpenAI, Google, OpenRouter and Ollama. An LLM model or service
is configured separately. Conversation context is sent to the chosen provider; Ollama can run locally.
The package starts disabled and makes no startup connectivity probe by default.

The bridge starts and stops with the game server. The package includes Python and the bridge dependencies;
users do not need to install Python or pip. There is no systemd integration. Unexpected
bridge exits are reported in the server log; details are in `logs/llm-chatter-bridge.log` under the server
folder. `LLMChatter.Bridge.AutoStart` and `LLMChatter.Bridge.LogPath` control this behavior.

## YAML and databases

Settings live in `plugins.mod-llm-chatter.settings` in the active YAML profile. Keep API keys in the
sibling `local.yaml` before sharing `server.yaml`; plugin key controls currently display ordinary text.
The launcher generates the module `.conf` for both runtime components.

SQLite works with the default local databases. MySQL works through the LonelyIce storage plugin.
By default the bridge inherits the server's actual connection and database prefix. Leave
`LLMChatter.Database.UseServerConnection` enabled; the separate database fields are a manual MySQL
override for advanced setups.

## Optional upstream tools and validation

The [Chatter Companion addon](https://github.com/Hokken/Chatter-Companion) is a separate optional
download; basic conversations do not require it. Screenshot capture requires the separate upstream
host agent and its MySQL/Python dependencies. The package does not automatically capture the screen;
those settings remain available for operators running the optional tool.

The initial Windows x64 and Linux x64 release is experimental. Linux uses the current LonelyIce
Ubuntu 24.04 baseline. Builds, isolated SQLite tests and SQL preparation checks are performed;
in-game conversations and provider-specific output still need gameplay feedback. See the
[port's build and validation notes](https://github.com/LonelyIceProject/mod-llm-chatter-plugin#checks).
