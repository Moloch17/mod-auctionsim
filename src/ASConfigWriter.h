#pragma once
#include <string>
#include <vector>
#include "Define.h"

// Hand-rolled auctionsim.conf rewriter. ConfigMgr is read-only, so persisting an
// in-game edit (e.g. from the companion addon) means editing the file directly.
// Rewrites only the changed line(s), preserving every other line and any comments,
// and writes atomically (temp file + rename) so a mid-write failure never corrupts
// the live config.
namespace ASConfigWriter
{
    // Rewrites a simple "AuctionSim.<key> = <old>" line to use <value> instead.
    // Returns false if the file couldn't be read/written or the key wasn't found.
    bool SetScalarValue(std::string const& filepath, std::string const& key, std::string const& value);

    // Rewrites one quality cell (e.g. the "1" in "GREY: 0, WHITE: 1, ...") within the
    // "AuctionSim.<percentConfigKey> = ..." line. Value is written as a plain decimal
    // ("0", "1", "1.5"), matching the multiplier format the rest of the file uses.
    bool SetMaskValue(
        std::string const& filepath, std::string const& percentConfigKey, std::string const& qualityLabel,
        float value);

    // One pending change. A scalar "AuctionSim.<key> = <value>" when qualityLabel is
    // empty; otherwise the "<qualityLabel>: <value>" cell of the "AuctionSim.<key>"
    // multiplier line. `value` is the already-formatted replacement text.
    struct Edit
    {
        std::string key;
        std::string qualityLabel;
        std::string value;
    };

    // Applies every edit in a single read + single atomic write, rather than one
    // full file rewrite per edit. Returns true only if every edit found its line
    // and applied cleanly (partial edits are still written).
    bool SetMany(std::string const& filepath, std::vector<Edit> const& edits);
}
