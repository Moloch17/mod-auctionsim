#pragma once

// Single source of truth for every version the module and its companion addon
// (interface_addon/ahsim) check against each other and against on-disk files.

// Module <-> addon protocol/release version. Must match ahsim.toc "## Version".
// The module and addon ship as a pair; a GM is warned (chat at login + addon
// window) on any mismatch, and the lower version is named as out of date.
#define AUCTIONSIM_VERSION "1.7.0"

// Config schema version. Bump whenever conf/auctionsim.conf.dist adds, removes or
// renames a key. Stamped into the .dist as "AuctionSim.ConfigVersion"; a GM whose
// auctionsim.conf carries an older value is warned (the module keeps running on
// built-in defaults for the missing keys -- it never edits auctionsim.conf).
#define AUCTIONSIM_CONFIG_VERSION 3u

// Data-file schema version. Bump whenever the auctionsim.dat row layout changes
// (see data/compile-data.cpp). compile-data stamps it as the file's first line
// ("AUCTIONSIM_DAT <v>"); ASConfig refuses to load a file whose stamp differs.
#define AUCTIONSIM_DATA_VERSION 1u

// Market-file schema version. Must match ml/MARKET_FORMAT.md and ml/s6_export.py: the
// file's first line is "AUCTIONSIM_MARKET <v>", and Market mode refuses any other v.
#define AUCTIONSIM_MARKET_VERSION 1u
