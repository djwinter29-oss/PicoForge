# Development

[Documentation](../README.md)

Use these workflows from a source checkout. Installation and everyday board operation are in the
[user guides](../guides/README.md).

## Choose the Change

| Change | Workflow and owner |
| --- | --- |
| Build firmware or run its C tests and board checks | [Firmware development and testing](firmware-testing.md) |
| Change board, transport, optional modules, or tuning flags | [Build configuration](../reference/build-configuration.md) |
| Change Python/.NET, their shared frontend, or native APIs | [Host tools development](host-tools.md) |
| Change the independent Direct dashboard or its bridge | [Direct dashboard development](direct-dashboard.md) |
| Add or change CLI commands | [Console integration](console-integration.md) |
| Change radio execution, provisioning, or TCP authorization | [Wireless integration](wireless-integration.md) |
| Decide where a test belongs or supply artifact-test inputs | [Test ownership](../../tests/README.md) |
| Tag, package, or publish a release | [Release and publishing](releases.md) |
| Prepare a contribution | [Contributing](../../CONTRIBUTING.md) |

The [architecture](../reference/architecture.md#layer-responsibilities) defines code boundaries. Keep board
assignments in board configuration and reuse platform/driver interfaces rather than adding cross-layer dependencies.

## Documentation Changes

Update the page that owns the procedure or contract; link to it from the other reader paths. New pages need a link
from their section index and a link back to that index. When moving a page, update relative links and inbound anchors.

From the repository root:

```sh
python3 -m pip install -r tests/requirements.txt
python3 -m pytest tests/contracts/test_documentation_links.py
```

These checks cover local links, heading anchors, section return links, and discovery from the documentation home.
Pure documentation changes do not require firmware builds or board tests.
