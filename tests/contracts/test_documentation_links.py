# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Ji Dong

"""Keep documentation navigation valid when guides and references move."""
from pathlib import Path
import re

import pytest

ROOT = Path(__file__).resolve().parents[2]
DOCUMENTS = sorted({
    *ROOT.joinpath("docs").rglob("*.md"),
    ROOT / "host/direct/README.md",
    ROOT / "host/python/README.md",
    ROOT / "host/dotnet/PicoForge/README.md",
    ROOT / "README.md",
    ROOT / "CONTRIBUTING.md",
    ROOT / "SECURITY.md",
    ROOT / "tests/README.md",
    ROOT / "firmware/tests/README.md",
})
pytestmark = pytest.mark.contracts


@pytest.mark.parametrize("source", DOCUMENTS, ids=lambda path: str(path.relative_to(ROOT)))
def test_local_document_links_and_heading_anchors_exist(source):
    for link in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', source.read_text()):
        link = link.strip("<>")
        if "://" in link or link.startswith(("mailto:", "data:")):
            continue
        filename, _, anchor = link.partition("#")
        target = (source.parent / filename).resolve() if filename else source
        assert target.exists(), f"{source.relative_to(ROOT)}: missing {link}"
        if anchor and target.suffix == ".md":
            headings = re.findall(r"^#+\s+(.+)$", target.read_text(), re.MULTILINE)
            slugs = [re.sub(r"[^\w\s-]", "", heading.lower()).replace(" ", "-") for heading in headings]
            assert anchor in slugs, f"{source.relative_to(ROOT)}: missing heading {link}"
