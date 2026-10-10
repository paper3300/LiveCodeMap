# LiveCodeMap

LiveCodeMap is a planned Windows-native code exploration and context layer for C++ and Unreal Engine projects. Product requirements and design decisions currently live in `Workspace/Docs/`.

The current replay and selected-action index tools can choose DebugGame or Development inputs through a [profile config](docs/replay-profiles.md).

The read-only query CLI and MCP server (`livecodemap ask | callers | skeleton | check | mcp`) are described in [docs/query.md](docs/query.md).
