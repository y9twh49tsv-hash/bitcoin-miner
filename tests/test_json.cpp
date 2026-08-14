// The hand-written JSON parser. Stratum traffic is untrusted input from a
// remote server, so malformed data must be refused, never mis-parsed.

#include <string>

#include "test_framework.hpp"
#include "util/json.hpp"

using azd::util::Json;

namespace {

Json parse(const std::string& text) {
    Json value;
    std::string error;
    REQUIRE(Json::parse(text, value, &error));
    return value;
}

} // namespace

TEST("parses scalars") {
    CHECK(parse("null").isNull());
    CHECK(parse("true").asBool(false));
    CHECK(!parse("false").asBool(true));
    CHECK_EQ(parse("42").asInt(0), int64_t(42));
    CHECK_EQ(parse("-17").asInt(0), int64_t(-17));
    CHECK_EQ(parse("3.5").asNumber(0.0), 3.5);
    CHECK_EQ(parse("1e3").asNumber(0.0), 1000.0);
    CHECK_EQ(parse("\"hello\"").asString(), std::string("hello"));
}

TEST("parses objects and preserves key order on output") {
    const Json value = parse(R"({"b":1,"a":2,"c":[1,2,3]})");
    CHECK(value.isObject());
    CHECK_EQ(value["b"].asInt(0), int64_t(1));
    CHECK_EQ(value["a"].asInt(0), int64_t(2));
    CHECK_EQ(value["c"].size(), size_t(3));
    CHECK(value.has("a"));
    CHECK(!value.has("zzz"));

    Json built = Json::object();
    built.set("first", 1);
    built.set("second", 2);
    CHECK_EQ(built.dump(), std::string(R"({"first":1,"second":2})"));
}

TEST("missing keys yield null instead of throwing") {
    const Json value = parse(R"({"a":1})");
    CHECK(value["missing"].isNull());
    CHECK_EQ(value["missing"].asInt(99), int64_t(99));
    CHECK_EQ(value["missing"].asString("fallback"), std::string("fallback"));

    // Indexing a non-object is also safe.
    CHECK(parse("42")["key"].isNull());
    CHECK(parse("42").at(0).isNull());
}

TEST("array access is bounds checked") {
    const Json value = parse("[10,20]");
    CHECK_EQ(value.at(0).asInt(0), int64_t(10));
    CHECK_EQ(value.at(1).asInt(0), int64_t(20));
    CHECK(value.at(2).isNull());
    CHECK(value.at(99999).isNull());
}

TEST("handles string escapes") {
    CHECK_EQ(parse(R"("a\"b")").asString(), std::string("a\"b"));
    CHECK_EQ(parse(R"("line\nbreak")").asString(), std::string("line\nbreak"));
    CHECK_EQ(parse(R"("tab\there")").asString(), std::string("tab\there"));
    CHECK_EQ(parse(R"("back\\slash")").asString(), std::string("back\\slash"));
    CHECK_EQ(parse(R"("A")").asString(), std::string("A"));
}

TEST("rejects malformed documents") {
    const char* bad[] = {
        "",          "{",         "}",        "[1,2",     "{\"a\":}",  "{\"a\" 1}",
        "{a:1}",     "\"unterminated", "[1,]",     "tru",      "nul",       "{\"a\":1}garbage",
        "[1,2] [3]", "'single'",
    };
    for (const char* text : bad) {
        Json value;
        std::string error;
        CHECK(!Json::parse(text, value, &error));
        CHECK(!error.empty());
    }
}

TEST("refuses pathological nesting rather than overflowing the stack") {
    std::string deep;
    for (int i = 0; i < 500; ++i) deep += "[";
    for (int i = 0; i < 500; ++i) deep += "]";

    Json value;
    std::string error;
    CHECK(!Json::parse(deep, value, &error));
}

TEST("serialization round-trips") {
    const std::string text =
        R"({"id":4,"method":"mining.submit","params":["w","job","0000","504e86b9","b2957c02"]})";
    const Json value = parse(text);
    CHECK_EQ(value.dump(), text);
}

TEST("numbers serialize without spurious decimals") {
    Json value = Json::object();
    value.set("int", static_cast<int64_t>(512));
    value.set("zero", static_cast<int64_t>(0));
    value.set("frac", 0.5);
    CHECK_EQ(value.dump(), std::string(R"({"int":512,"zero":0,"frac":0.5})"));
}

TEST("null values serialize as null, which is how unavailable data is reported") {
    Json value = Json::object();
    value.set("temperature", Json::null());
    value.set("available", false);
    CHECK_EQ(value.dump(), std::string(R"({"temperature":null,"available":false})"));
}

TEST("pretty printing is stable") {
    Json value = Json::object();
    value.set("a", 1);
    Json inner = Json::array();
    inner.push(1);
    inner.push(2);
    value.set("b", inner);

    const std::string pretty = value.dump(2);
    CHECK(pretty.find("\n") != std::string::npos);

    // And it re-parses to the same thing.
    Json reparsed;
    REQUIRE(Json::parse(pretty, reparsed));
    CHECK_EQ(reparsed.dump(), value.dump());
}

TEST("escaping protects control characters") {
    Json value = Json::object();
    value.set("text", std::string("a\x01g"));
    const std::string dumped = value.dump();
    CHECK(dumped.find("\\u0001") != std::string::npos);

    Json reparsed;
    REQUIRE(Json::parse(dumped, reparsed));
    CHECK_EQ(reparsed["text"].asString(), std::string("a\x01g"));
}

TEST("empty containers serialize compactly") {
    CHECK_EQ(Json::array().dump(), std::string("[]"));
    CHECK_EQ(Json::object().dump(), std::string("{}"));
}

AZD_TEST_MAIN("json")
