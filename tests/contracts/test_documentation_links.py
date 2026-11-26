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
    ROOT / "CODE_OF_CONDUCT.md",
    ROOT / "SECURITY.md",
    ROOT / "tests/README.md",
    ROOT / "firmware/tests/README.md",
    ROOT / "firmware/third_party/fatfs/README.md",
})
pytestmark = pytest.mark.contracts


def local_links(source):
    for link in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', source.read_text()):
        link = link.strip("<>")
        if "://" in link or link.startswith(("mailto:", "data:")):
            continue
        filename, _, anchor = link.partition("#")
        target = (source.parent / filename).resolve() if filename else source
        yield link, target, anchor


@pytest.mark.parametrize("source", DOCUMENTS, ids=lambda path: str(path.relative_to(ROOT)))
def test_local_document_links_and_heading_anchors_exist(source):
    for link, target, anchor in local_links(source):
        assert target.exists(), f"{source.relative_to(ROOT)}: missing {link}"
        if anchor and target.suffix == ".md":
            headings = re.findall(r"^#+\s+(.+)$", target.read_text(), re.MULTILINE)
            slugs = [re.sub(r"[^\w\s-]", "", heading.lower()).replace(" ", "-") for heading in headings]
            assert anchor in slugs, f"{source.relative_to(ROOT)}: missing heading {link}"


@pytest.mark.parametrize("source", sorted(ROOT.joinpath("docs").rglob("*.md")),
                         ids=lambda path: str(path.relative_to(ROOT)))
def test_document_pages_have_section_return_links(source):
    parent_index = next(
        parent / "README.md"
        for parent in source.parents
        if parent / "README.md" != source and parent / "README.md" in DOCUMENTS
    )
    targets = {target for _, target, _ in local_links(source)}
    assert parent_index in targets, (
        f"{source.relative_to(ROOT)}: missing return link to {parent_index.relative_to(ROOT)}"
    )


def test_documentation_is_reachable_from_documentation_home():
    documents = set(DOCUMENTS)
    pending = [ROOT / "docs/README.md"]
    reached = set()
    while pending:
        source = pending.pop()
        if source in reached:
            continue
        reached.add(source)
        pending.extend(target for _, target, _ in local_links(source) if target in documents)
    missing = sorted(str(path.relative_to(ROOT)) for path in documents - reached)
    assert not missing, f"Pages missing from documentation navigation: {missing}"
