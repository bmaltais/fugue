#pragma once

// The templates window's search and filters (#369, spec 11.10), at a seam with no window: a
// template as the window gets it (Workflows::templates(): a map with id, title, description,
// categoryTitle, tags, source {name, author, url, note}, families) and what the person typed.
//
// The query is words, all of which must match. A word matches at the start of a word of a field
// (so "da" finds "DA workflow" and not "update"), over the title, description, category, tags,
// id, the source's name and author, and the families. `key:value` limits a word to one field:
// `tag:`, `source:` (name or author), `family:`; a value with spaces is quoted, `family:"klein 9b"`.
// A value after a key that is not one of those is a plain word, colon and all.

#include <QString>
#include <QVariantMap>

namespace fugue {

// Whether `text` holds `needle` (any case) starting at a word: at its start or after a character
// that is not a letter or digit.
bool matchesWordStart(const QString &text, const QString &needle);

// Whether the template satisfies every word of `query`; an empty query matches everything.
bool templateMatchesSearch(const QString &query, const QVariantMap &templ);

} // namespace fugue
