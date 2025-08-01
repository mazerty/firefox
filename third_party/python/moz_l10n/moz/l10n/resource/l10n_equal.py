# Copyright Mozilla Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

from collections import defaultdict
from typing import Any

from ..model import Entry, Resource, Section


def l10n_equal(a: Resource[Any], b: Resource[Any]) -> bool:
    """
    Compares the localization-relevant content
    (id, comment, metadata, message values) of two resources.

    Sections with no message entries are ignored,
    and the order of sections, entries, and metadata is ignored.
    """
    return (
        a.format == b.format
        and a.comment.strip() == b.comment.strip()
        and l10n_meta(a) == l10n_meta(b)
        and l10n_sections(a) == l10n_sections(b)
    )


def l10n_sections(resource: Resource[Any]) -> list[tuple[Any, ...]]:
    ls = [
        (section.id, section.comment.strip(), l10n_meta(section), l10n_entries(section))
        for section in resource.sections
        if any(isinstance(entry, Entry) for entry in section.entries)
    ]
    ls.sort(key=lambda s: s[0])
    nonempty_idx = next(
        (idx for idx, (id, comment, meta, _) in enumerate(ls) if id or comment or meta),
        len(ls),
    )
    if nonempty_idx > 1:
        # Anonymous sections are considered equivalent
        first = ls[0]
        for sd in ls[1:nonempty_idx]:
            first[3].extend(sd[3])
        first[3].sort(key=lambda e: e[0])
        del ls[1:nonempty_idx]
    return ls


def l10n_entries(section: Section[Any]) -> list[tuple[Any, ...]]:
    le = [
        (
            entry.id,
            entry.comment.strip(),
            l10n_meta(entry),
            entry.value,
            entry.properties,
        )
        for entry in section.entries
        if isinstance(entry, Entry)
    ]
    le.sort(key=lambda e: e[0])
    return le


def l10n_meta(
    x: Entry[Any] | Section[Any] | Resource[Any],
) -> dict[str, set[Any]]:
    md: dict[str, set[Any]] = defaultdict(set)
    for m in x.meta:
        md[m.key].add(m.value)
    return md
