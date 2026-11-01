#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <config.hpp>

// [storage] backfill, backfill_batch, backfill_rate
namespace {
StorageData parse(const std::string& storage) {
    std::istringstream in("[storage]\n" + storage);
    boost::property_tree::ptree tree;
    boost::property_tree::read_ini(in, tree);
    StorageData data;
    Config::readBackfillSettings(tree.get_child("storage"), data);
    return data;
}
}  // namespace

TEST(BackfillConfig, Defaults) {
    const auto d = parse("db_backend=lmdb\n");
    EXPECT_TRUE(d.backfill);
    EXPECT_EQ(d.backfillBatch, 100u);
    EXPECT_EQ(d.backfillRate, 1000u);
    EXPECT_EQ(d.backfillStorePerTurn, 50u);
    EXPECT_EQ(d.backfillTurnBudgetMs, 50u);
    EXPECT_EQ(d.backfillIdleTurnBudgetMs, 90u);
}

TEST(BackfillConfig, EveryKey) {
    const auto d = parse("backfill=off\nbackfill_batch=250\nbackfill_rate=0\nbackfill_store_per_turn=20\nbackfill_turn_budget_ms=0\nbackfill_idle_turn_budget_ms=200\n");
    EXPECT_FALSE(d.backfill);
    EXPECT_EQ(d.backfillTurnBudgetMs, 0u);
    EXPECT_EQ(d.backfillIdleTurnBudgetMs, 200u);
    EXPECT_EQ(d.backfillBatch, 250u);
    EXPECT_EQ(d.backfillRate, 0u);
    EXPECT_EQ(d.backfillStorePerTurn, 20u);
    EXPECT_TRUE(parse("backfill=on\n").backfill);
    EXPECT_FALSE(parse("backfill=false\n").backfill);
}

TEST(BackfillConfig, BadValuesKeepTheDefaultOrAreClamped) {
    EXPECT_TRUE(parse("backfill=maybe\n").backfill);
    EXPECT_EQ(parse("backfill_batch=5000\n").backfillBatch, 1000u);
    EXPECT_EQ(parse("backfill_turn_budget_ms=5000\n").backfillTurnBudgetMs, 1000u);
    EXPECT_EQ(parse("backfill_idle_turn_budget_ms=5000\n").backfillIdleTurnBudgetMs, 1000u);
    EXPECT_EQ(parse("backfill_batch=0\n").backfillBatch, 1u);
    EXPECT_EQ(parse("backfill_batch=many\n").backfillBatch, 100u);
    EXPECT_EQ(parse("backfill_rate=-5\n").backfillRate, 1000u);
}
