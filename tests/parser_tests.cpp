#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "itch_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using itch::testing::ItchStream;

struct Recorder {
  std::vector<std::string> events;
  const std::byte* first_message{};

  void on(const itch::AddOrder& m) {
    if (first_message == nullptr) first_message = reinterpret_cast<const std::byte*>(&m);
    events.push_back("A" + std::string(1, m.side) + std::to_string(m.order_reference()) + "/" +
                     std::to_string(m.share_count()) + "@" + std::to_string(m.price_ticks()) +
                     " loc" + std::to_string(m.header.locate()));
  }
  void on(const itch::ExecuteOrder& m) {
    events.push_back("E" + std::to_string(m.order_reference()) + "/" + std::to_string(m.share_count()));
  }
  void on(const itch::ExecuteWithPrice& m) {
    events.push_back("C" + std::to_string(m.order_reference()) + "@" + std::to_string(m.price_ticks()));
  }
  void on(const itch::CancelOrder& m) {
    events.push_back("X" + std::to_string(m.order_reference()) + "/" + std::to_string(m.share_count()));
  }
  void on(const itch::DeleteOrder& m) { events.push_back("D" + std::to_string(m.order_reference())); }
  void on(const itch::ReplaceOrder& m) {
    events.push_back("U" + std::to_string(m.original_order_reference()) + ">" +
                     std::to_string(m.replacement_order_reference()) + "@" +
                     std::to_string(m.price_ticks()));
  }
};

ItchStream every_order_message() {
  ItchStream stream;
  stream.add(7, 42, 'B', 100, 12'345)
      .system_event()
      .add(7, 0x0102030405060708ull, 'S', 5, 4'000'000'000u, /*attributed=*/true)
      .execute(7, 42, 30)
      .execute_with_price(7, 42, 1, 12'300)
      .cancel(7, 42, 10)
      .replace(7, 42, 43, 60, 12'400)
      .remove(7, 43);
  return stream;
}

const std::vector<std::string> kExpected{
    "AB42/100@12345 loc7", "AS72623859790382856/5@4000000000 loc7", "E42/30", "C42@12300",
    "X42/10", "U42>43@12400", "D43"};

}  // namespace

TEST_CASE("spec_length knows every ITCH 5.0 type and nothing else") {
  for (const auto [type, bytes] : itch::detail::kSpecLengths) {
    REQUIRE(itch::spec_length(static_cast<char>(type)) == bytes);
  }
  REQUIRE_FALSE(itch::spec_length('Z').has_value());
  REQUIRE_FALSE(itch::spec_length('\0').has_value());
}

TEST_CASE("parser decodes every order message type in place") {
  const auto stream = every_order_message();
  Recorder recorder;
  REQUIRE(itch::Parser(stream.data(), stream.size()).parse(recorder) == stream.messages());
  REQUIRE(recorder.events == kExpected);
  // Zero-copy: the visitor receives a view at the message's offset inside the input buffer,
  // just past the first 2-byte length prefix.
  REQUIRE(recorder.first_message == stream.data() + 2);
}

TEST_CASE("parser reads a memory-mapped file without copying") {
  const auto stream = every_order_message();
  const auto path = std::filesystem::temp_directory_path() / "itch_parser_test.bin";
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(stream.data()), static_cast<std::streamsize>(stream.size()));

  {
    const itch::MappedFile file(path);
    Recorder recorder;
    REQUIRE(itch::Parser(file.data(), file.size()).parse(recorder) == stream.messages());
    REQUIRE(recorder.events == kExpected);
    REQUIRE(recorder.first_message == file.data() + 2);
  }
  std::filesystem::remove(path);
}

TEST_CASE("parser rejects streams that lose message alignment") {
  Recorder recorder;

  SECTION("truncated message") {
    const auto stream = every_order_message();
    REQUIRE_THROWS_AS(itch::Parser(stream.data(), stream.size() - 1).parse(recorder),
                      std::runtime_error);
  }
  SECTION("dangling length prefix") {
    auto bytes = every_order_message().bytes();
    bytes.push_back(std::byte{0});
    REQUIRE_THROWS_AS(itch::Parser(bytes.data(), bytes.size()).parse(recorder), std::runtime_error);
  }
  SECTION("length that contradicts the type") {
    ItchStream stream;
    stream.append(itch::ExecuteOrder{.header = {.type = 'E'}}, sizeof(itch::ExecuteOrder) + 1);
    REQUIRE_THROWS_AS(itch::Parser(stream.data(), stream.size()).parse(recorder),
                      std::runtime_error);
  }
}

TEST_CASE("missing files raise a system_error naming the path") {
  REQUIRE_THROWS_AS(itch::MappedFile("/nonexistent/feed.itch"), std::system_error);
}
