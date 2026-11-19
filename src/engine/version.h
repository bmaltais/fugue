#pragma once

// A semver version of a node type or group (spec 3.6): MAJOR.MINOR.PATCH,
// decimal, no leading zeros. Pre-release and build suffixes are not accepted
// in v0.1.

#include <QString>

#include <optional>
#include <tuple>

namespace fugue {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    static std::optional<Version> parse(const QString &text);
    QString toString() const;

    bool operator==(const Version &o) const { return tie() == o.tie(); }
    bool operator!=(const Version &o) const { return tie() != o.tie(); }
    bool operator<(const Version &o) const { return tie() < o.tie(); }
    bool operator>(const Version &o) const { return tie() > o.tie(); }
    bool operator<=(const Version &o) const { return tie() <= o.tie(); }
    bool operator>=(const Version &o) const { return tie() >= o.tie(); }

private:
    std::tuple<int, int, int> tie() const { return {major, minor, patch}; }
};

} // namespace fugue
