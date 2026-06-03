// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

// WHY: standalone converter — CSV → length-prefixed FB archive — used to
// pre-process Binance bookTicker dumps once so backtest replay can stream
// straight off disk without re-parsing every run.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "backtest/historical_data_loader.hpp"
#include "infra/config.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <csv_path> [<bin_out>] [<config_path>]\n", argv[0]);
        return 1;
    }
    const std::string csv = argv[1];
    std::string bin = (argc >= 3) ? argv[2] : (csv.substr(0, csv.find_last_of('.')) + ".bin");
    const std::string cfg_path = (argc >= 4) ? argv[3] : "config/config.toml";

    spreadara::infra::Config cfg;
    try {
        cfg = spreadara::infra::load_config(cfg_path);
    } catch (const std::exception& e) {
        // WHY: if the config is missing we still want the converter usable —
        // default the volatility window to 100 (matches the config default).
        std::fprintf(stderr, "config_load_warn: %s — using defaults\n", e.what());
        cfg.market_data.volatility_window = 100;
    }
    spreadara::backtest::HistoricalDataLoader loader(cfg);
    if (!loader.load_csv(csv, bin)) {
        std::fprintf(stderr, "convert_failed csv=%s bin=%s\n", csv.c_str(), bin.c_str());
        return 2;
    }
    std::printf("ok csv=%s bin=%s\n", csv.c_str(), bin.c_str());
    return 0;
}
