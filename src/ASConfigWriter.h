#pragma once
#include <string>
#include "Define.h"

// Hand-rolled auctionsim.conf rewriter. ConfigMgr is read-only, so persisting an
// in-game edit (e.g. from the companion addon) means editing the file directly.
// Rewrites only the changed line, preserving every other line and any comments,
// and writes atomically (temp file + rename) so a mid-write failure never corrupts
// the live config.
namespace ASConfigWriter
{
    // Rewrites a simple "AuctionSim.<key> = <old>" line to use <value> instead.
    // Returns false if the file couldn't be read/written or the key wasn't found.
    bool SetScalarValue(std::string const& filepath, std::string const& key, std::string const& value);

    // Rewrites one quality cell (e.g. the "0100" in "GREY: 0000, WHITE: 0100, ...") within
    // the "AuctionSim.<percentConfigKey> = ..." line. Value is written as a 4-digit field,
    // matching the format the rest of the file already uses.
    bool SetMaskValue(
        std::string const& filepath, std::string const& percentConfigKey, std::string const& qualityLabel,
        uint32 value);
}
