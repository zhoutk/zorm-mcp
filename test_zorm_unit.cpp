// zorm-mcp - unit tests for the pure helpers (no server, no database).
//
// These cover the seams that e2e tests exercise only indirectly:
//   - the zorm/mcp JSON bridge and result envelopes
//   - the case-insensitive row accessor used by the catalog readers
//   - the identifier whitelist that gates inlined SQL identifiers
//   - the guide/playbook wording that the AI-comprehension layer depends on

#include "guide.hpp"
#include "json_util.hpp"

#include <gtest/gtest.h>

using namespace mcp;

TEST(JsonUtil, OkStatusAcceptsBothSuccessCodes) {
  EXPECT_TRUE(zorm_mcp::okStatus(Json(R"({"status":200})")));
  // 202 = empty result on jsonfile; it is success, not an error.
  EXPECT_TRUE(zorm_mcp::okStatus(Json(R"({"status":202})")));
  EXPECT_FALSE(zorm_mcp::okStatus(Json(R"({"status":500})")));
  EXPECT_FALSE(zorm_mcp::okStatus(Json(R"({})")));
}

TEST(JsonUtil, ToolResultCarriesContentAndFlattensFields) {
  const Json rs = Json(R"({"status":200,"data":[{"id":"r1"}]})");
  const Json out = zorm_mcp::toolResult(rs);
  ASSERT_TRUE(out["content"].isArray());
  ASSERT_EQ(out["content"].size(), 1u);
  EXPECT_EQ(out["content"][0]["type"].toString(), "text");
  EXPECT_EQ(out["status"].toInt(), 200);
  EXPECT_TRUE(out["data"].isArray());
}

TEST(JsonUtil, TextResultShape) {
  const Json out = zorm_mcp::textResult("hello");
  ASSERT_TRUE(out["content"].isArray());
  EXPECT_EQ(out["content"][0]["text"].toString(), "hello");
}

TEST(JsonUtil, RowValTriesCandidatesWithoutDeepSearch) {
  const Json row = Json(R"({"ID":"x","nested":{"id":"wrong"}})");
  EXPECT_EQ(zorm_mcp::rowVal(row, {"id", "ID"}).toString(), "x");
  EXPECT_EQ(zorm_mcp::rowStr(row, {"id", "ID"}), "x");
  // A missing candidate must not deep-search into nested objects.
  EXPECT_TRUE(zorm_mcp::rowVal(row, {"missing"}).isNull());
  EXPECT_EQ(zorm_mcp::rowStr(row, {"missing"}), "");
  // Non-object rows are safe.
  EXPECT_TRUE(zorm_mcp::rowVal(Json("[]"), {"id"}).isNull());
}

TEST(JsonUtil, ValidIdentifierWhitelist) {
  EXPECT_TRUE(zorm_mcp::validIdentifier("t_e2e"));
  EXPECT_TRUE(zorm_mcp::validIdentifier("_private"));
  EXPECT_TRUE(zorm_mcp::validIdentifier("Table1"));
  EXPECT_FALSE(zorm_mcp::validIdentifier(""));
  EXPECT_FALSE(zorm_mcp::validIdentifier("1table"));
  EXPECT_FALSE(zorm_mcp::validIdentifier("t-e2e"));
  EXPECT_FALSE(zorm_mcp::validIdentifier("t e2e"));
  EXPECT_FALSE(zorm_mcp::validIdentifier("t;drop"));
  EXPECT_FALSE(zorm_mcp::validIdentifier("schema.table"));
}

TEST(JsonUtil, BridgeRoundTripsValues) {
  const Json src = Json(R"({"a":1,"b":"two","c":[true,false]})");
  const Json back = zorm_mcp::toMcpJson(zorm_mcp::toZormJson(src));
  ASSERT_TRUE(back.isObject());
  EXPECT_EQ(back["a"].toInt(), 1);
  EXPECT_EQ(back["b"].toString(), "two");
  ASSERT_TRUE(back["c"].isArray());
  EXPECT_TRUE(back["c"][0].isTrue());
}

TEST(Guide, InstructionsTeachRoutingAndMrtrPlacement) {
  const std::string text = zorm_mcp::instructionsText();
  EXPECT_NE(text.find("db_list_tables"), std::string::npos);
  EXPECT_NE(text.find("db_describe_table"), std::string::npos);
  EXPECT_NE(text.find("db_query"), std::string::npos);
  EXPECT_NE(text.find("MRTR"), std::string::npos);
  // The retry fields are params-level per the spec; the wording must not tell
  // agents to put them inside `arguments`.
  EXPECT_NE(text.find("params"), std::string::npos);
}

TEST(Guide, PlaybookCoversEveryToolFamily) {
  const std::string text = zorm_mcp::playbookText();
  for (const char* needle : {"db_list_tables", "db_describe_table", "db_indexes",
                             "db_info", "db_query", "db_query_sql", "db_exec_sql",
                             "db_insert", "db_insert_batch", "db_update",
                             "db_delete", "db_trans", "MRTR"}) {
    EXPECT_NE(text.find(needle), std::string::npos) << needle;
  }
  EXPECT_NE(text.find("202"), std::string::npos);  // empty-result status
}
