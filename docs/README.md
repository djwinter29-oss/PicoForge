# Documentation

[Project overview](../README.md)

**First time here?** Follow [Getting Started](getting-started.md) to flash a board and check its first connection.
You do not need an SD card for board monitoring.

## Choose Your Task

| I want to... | Start here |
| --- | --- |
| Use the board, dashboards, storage, or wireless | [User guides](guides/README.md) |
| Build firmware, change code, run tests, or publish a release | [Development](development/README.md) |
| Look up a setting, protocol, interface, or ownership rule | [Technical reference](reference/README.md) |
| Check what has been tested and what remains unqualified | [Known limits and evidence](evidence/README.md) |

## Shortcuts

- [Choose a dashboard](guides/README.md#dashboards)
- [Enable or disable firmware modules](reference/build-configuration.md#optional-modules)
- [Share an SD card with the computer](guides/storage.md#share-the-card-with-your-computer)
- [Open the command console](guides/console.md#terminal)
- [Find the right test suite](../tests/README.md)
- [Report a security issue](../SECURITY.md)

## Keeping These Docs Maintainable

- Keep the project overview in the root README; use this page for navigation.
- Give each procedure or contract one home. Link to it from other audiences' pages instead of copying it.
- Put operating steps in user guides, code/test workflows in development, and exact contracts in reference.
  Component READMEs own installation and everyday use; test READMEs own suite placement and fixture rules.
- Repeat short safety warnings where the action happens, not complete state machines or setup recipes.
- Keep current instructions separate from dated evidence. Preserve the revision, configuration, and limits of each record.
- Use descriptive lowercase hyphenated filenames and add a section-index link when adding a page. See
  [documentation checks](development/README.md#documentation-changes).
