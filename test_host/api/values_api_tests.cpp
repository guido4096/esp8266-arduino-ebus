#define private public
#define protected public
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "api/values_api.hpp"
#include "app/command.hpp"
#include "app/command_manager.hpp"
#include "app/ebus_test_helpers.hpp"
#undef private
#undef protected
#include "system/logger.hpp"

namespace {

std::string makeCommandJson() {
  return R"({
    "key":"01",
    "name":"Outside_Temperature",
    "read_cmd":"fe070009",
    "write_cmd":"50b509040e3300",
    "interval":60,
    "master":false,
    "fields":[
      {"name":"value","profile":"data2b_celsius","position":1,"ha_profile":"sensor_temperature"}
    ]
  })";
}

void insertTestCommand() {
  command_manager.wipeCommands();
  const std::string command_json = makeCommandJson();
  ebus::detail::JsonReader reader(command_json);
  Command command = Command::fromJson(reader);
  command_manager.insertCommand(std::move(command));
}

}  // namespace

TEST_CASE("ValuesApi serves the values page", "[values_api]") {
  command_manager.wipeCommands();
  ValuesApi api(command_manager);
  httpd_req_t req{};

  REQUIRE(ValuesApi::handleValuesPage(&req) == ESP_OK);
  REQUIRE(req.status == "200 OK");
  REQUIRE(req.content_type == "text/html");
  REQUIRE(req.final_body == "<html><body>values</body></html>");
}

TEST_CASE("ValuesApi reads and writes by key", "[values_api]") {
  REQUIRE(configureHostEbusController());
  insertTestCommand();
  REQUIRE(command_manager.findCommand("01") != nullptr);

  ValuesApi api(command_manager);

  httpd_req_t read_req{};
  read_req.body = R"({"key":"01"})";
  read_req.content_len = static_cast<int>(read_req.body.size());
  REQUIRE(ValuesApi::handleValuesRead(&read_req) == ESP_OK);
  REQUIRE(read_req.status == "200 OK");
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() != 0);
  REQUIRE(read_req.content_type == "application/json;charset=utf-8");
  REQUIRE(read_req.final_body.find(R"("id":"read")") != std::string::npos);
  REQUIRE(read_req.final_body.find(R"("status":"requested")") !=
          std::string::npos);

  // A one-shot reply has no poll_id; it must still update the active command.
  const uint8_t response[] = {2, 0, 21};  // DATA2B: 21 degrees
  ebus::ProtocolInfo info{};
  info.session_id = command_manager.findCommand("01")->getPendingReadId();
  info.slave_view = ebus::ByteView(response, sizeof(response));
  command_manager.updateData(info);
  REQUIRE(command_manager.findCommand("01")->getData().size() == 2);
  REQUIRE(command_manager.findCommand("01")->getData()[1] == 21);
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);

  httpd_req_t write_req{};
  write_req.body = R"({"key":"01","value":12.5})";
  write_req.content_len = static_cast<int>(write_req.body.size());
  REQUIRE(ValuesApi::handleValuesWrite(&write_req) == ESP_OK);
  REQUIRE(write_req.status == "200 OK");
  REQUIRE(write_req.final_body.find(R"("id":"write")") != std::string::npos);
  REQUIRE(write_req.final_body.find(R"("status":"successful")") !=
          std::string::npos);
  REQUIRE(command_manager.findCommand("01")->getWriteSessionId() != 0);
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);

  httpd_req_t list_req{};
  REQUIRE(ValuesApi::handleValues(&list_req) == ESP_OK);
  REQUIRE(list_req.content_type == "application/json;charset=utf-8");
  std::string values_body;
  for (const auto& chunk : list_req.chunks) values_body += chunk;
  REQUIRE(values_body.find(R"("key":"01")") != std::string::npos);
  REQUIRE(values_body.find(R"("name":"Outside_Temperature")") !=
          std::string::npos);
  REQUIRE(list_req.chunks.back().empty());

  command_manager.wipeCommands();
}

TEST_CASE("ValuesApi rejects unknown keys", "[values_api]") {
  command_manager.wipeCommands();
  ValuesApi api(command_manager);

  httpd_req_t read_req{};
  read_req.body = R"({"key":"missing"})";
  read_req.content_len = static_cast<int>(read_req.body.size());
  REQUIRE(ValuesApi::handleValuesRead(&read_req) == ESP_OK);
  REQUIRE(read_req.status == "404 Not Found");
  REQUIRE(read_req.final_body.find(R"("id":"read")") != std::string::npos);
  REQUIRE(read_req.final_body.find("Key 'missing' not found") !=
          std::string::npos);

  httpd_req_t write_req{};
  write_req.body = R"({"key":"missing","value":12.5})";
  write_req.content_len = static_cast<int>(write_req.body.size());
  REQUIRE(ValuesApi::handleValuesWrite(&write_req) == ESP_OK);
  REQUIRE(write_req.status == "404 Not Found");
  REQUIRE(write_req.final_body.find(R"("id":"write")") != std::string::npos);
  REQUIRE(write_req.final_body.find("Key 'missing' not found") !=
          std::string::npos);
}

TEST_CASE("ValuesApi rejects malformed and invalid writes", "[values_api]") {
  insertTestCommand();
  ValuesApi api(command_manager);

  httpd_req_t malformed_req{};
  malformed_req.body = R"({"key":"01","value":)";
  malformed_req.content_len = static_cast<int>(malformed_req.body.size());
  REQUIRE(ValuesApi::handleValuesWrite(&malformed_req) == ESP_OK);
  REQUIRE(malformed_req.status == "400 Bad Request");
  REQUIRE(malformed_req.final_body.find(R"("status":"failed")") !=
          std::string::npos);

  httpd_req_t invalid_value_req{};
  invalid_value_req.body = R"({"key":"01","value":"not-a-number"})";
  invalid_value_req.content_len =
      static_cast<int>(invalid_value_req.body.size());
  REQUIRE(ValuesApi::handleValuesWrite(&invalid_value_req) == ESP_OK);
  REQUIRE(invalid_value_req.status == "400 Bad Request");
  REQUIRE(invalid_value_req.final_body.find(R"("id":"write")") !=
          std::string::npos);
  REQUIRE(invalid_value_req.final_body.find("Invalid value for key '01'") !=
          std::string::npos);

  command_manager.wipeCommands();
}

TEST_CASE("ValuesApi rejects sending passive match patterns", "[values_api]") {
  command_manager.wipeCommands();
  const std::string json = R"({"key":"passive","name":"Room","read_cmd":"51501407","interval":0,"master":true,"fields":[{"name":"value","profile":"data2b_celsius","position":4}]})";
  ebus::detail::JsonReader reader(json);
  command_manager.insertCommand(Command::fromJson(reader));
  ValuesApi api(command_manager);
  httpd_req_t req{};
  req.body = R"({"key":"passive"})";
  req.content_len = static_cast<int>(req.body.size());
  REQUIRE(ValuesApi::handleValuesRead(&req) == ESP_OK);
  REQUIRE(req.status == "400 Bad Request");
  REQUIRE(command_manager.findCommand("passive")->getPendingReadId() == 0);
  command_manager.wipeCommands();
}

TEST_CASE("ValuesApi rejects writes when the bus queue is full", "[values_api]") {
  insertTestCommand();
  auto& controller = getEbusController();
  auto config = getEbusConfig();
  config.bus.device = "/dev/null";
  REQUIRE(controller.configure(config));
  struct ResetController {
    ebus::Controller& controller;
    ebus::EbusConfig config;
    ~ResetController() { controller.configure(config); }
  } reset{controller, config};

  // The controller is not started, so queued requests cannot be drained.
  const ebus::Sequence message{0x50, 0xb5, 0x09, 0x04, 0x0e, 0x33, 0x00, 0x00};
  bool full = false;
  for (unsigned i = 0; i < 1024; ++i) {
    if (controller.enqueue(app::priority::send, message) == 0) {
      full = true;
      break;
    }
  }
  REQUIRE(full);
  auto* command = command_manager.findCommand("01");
  REQUIRE(command != nullptr);
  command->setLast(123);

  ValuesApi api(command_manager);
  httpd_req_t req{};
  req.body = R"({"key":"01","value":12.5})";
  req.content_len = static_cast<int>(req.body.size());
  REQUIRE(ValuesApi::handleValuesWrite(&req) == ESP_OK);
  REQUIRE(req.status == "503 Service Unavailable");
  REQUIRE(req.final_body.find("Unable to queue write command") != std::string::npos);
  REQUIRE(req.final_body.find(R"("status":"successful")") == std::string::npos);
  REQUIRE(command->getLast() == 123);
  command_manager.wipeCommands();
}

namespace {
struct DelayedWriteFixture {
  DelayedWriteFixture() {
    command_manager.wipeCommands();
    auto config = getEbusConfig();
    config.bus.device = "/dev/null";
    REQUIRE(getEbusController().configure(config));
    const std::string json = makeCommandJson();
    ebus::detail::JsonReader reader(json);
    command_manager.insertCommand(Command::fromJson(reader));
  }
  ~DelayedWriteFixture() {
    command_manager.wipeCommands();
    auto config = getEbusConfig();
    config.bus.device = "/dev/null";
    getEbusController().configure(config);
  }
  size_t queued() const {
    size_t size = 0;
    getEbusController().fetchStatus([&](const ebus::SystemResources& status) {
      for (const auto& queue : status.queues) {
        if (std::string_view(queue.name.c_str()) == "scheduler") size = queue.size;
      }
    });
    return size;
  }
  ebus::Sequence message(double value) const {
    auto* command = command_manager.findCommand("01");
    return command->buildWriteSequence(command_manager,
                                       command->getVectorFromDouble(value));
  }
};
}  // namespace

TEST_CASE("HTTP delayed writes are accepted without premature bus traffic", "[values_api]") {
  DelayedWriteFixture fixture;
  ValuesApi api(command_manager);
  httpd_req_t req{};
  req.body = R"({"key":"01","value":21.5,"delay_ms":2000})";
  req.content_len = static_cast<int>(req.body.size());
  REQUIRE(ValuesApi::handleValuesWrite(&req) == ESP_OK);
  REQUIRE(req.status == "202 Accepted");
  REQUIRE(req.final_body == R"({"id":"write","status":"scheduled"})");
  REQUIRE(fixture.queued() == 0);
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);
  REQUIRE(command_manager.delayed_writes_.size() == 1);
}

TEST_CASE("HTTP write delay requires bounded integer milliseconds", "[values_api]") {
  DelayedWriteFixture fixture;
  ValuesApi api(command_manager);
  for (const auto* value : {"-1", "1.5", "1e3", "true", "null", "\"2000\"",
                            "60001", "4294967296", "{}", "[]"}) {
    httpd_req_t req{};
    req.body = std::string(R"({"key":"01","value":21,"delay_ms":)") + value + "}";
    req.content_len = static_cast<int>(req.body.size());
    REQUIRE(ValuesApi::handleValuesWrite(&req) == ESP_OK);
    REQUIRE(req.status == "400 Bad Request");
    REQUIRE(command_manager.delayed_writes_.empty());
    REQUIRE(fixture.queued() == 0);
  }
  for (unsigned delay : {0u, 60000u}) {
    httpd_req_t req{};
    req.body = std::string(R"({"key":"01","value":21,"delay_ms":)") +
               std::to_string(delay) + "}";
    req.content_len = static_cast<int>(req.body.size());
    REQUIRE(ValuesApi::handleValuesWrite(&req) == ESP_OK);
    REQUIRE(req.status == (delay == 0 ? "200 OK" : "202 Accepted"));
  }
}

TEST_CASE("Delayed writes send only the last value after the last deadline", "[values_api]") {
  DelayedWriteFixture fixture;
  const auto first = fixture.message(20);
  const auto last = fixture.message(21.5);
  REQUIRE(command_manager.queueWriteAt("01", first, 2000, 1000));
  REQUIRE(command_manager.queueWriteAt("01", last, 2000, 2500));
  command_manager.processDelayedWritesAt(3000);
  REQUIRE(fixture.queued() == 0);
  REQUIRE(command_manager.delayed_writes_.size() == 1);
  REQUIRE(command_manager.delayed_writes_[0].message == last);
  command_manager.processDelayedWritesAt(4499);
  REQUIRE(fixture.queued() == 0);
  command_manager.processDelayedWritesAt(4500);
  REQUIRE(command_manager.delayed_writes_.empty());
  REQUIRE(fixture.queued() == 1);  // Dispatch queues only the write.
  auto* command = command_manager.findCommand("01");
  REQUIRE(command->getPendingReadId() == 0);
  ebus::ProtocolInfo result{};
  result.session_id = command->getWriteSessionId();
  REQUIRE(result.session_id != 0);
  command_manager.handleWriteResult(result);
  REQUIRE(command->getPendingReadId() != 0);
  REQUIRE(fixture.queued() == 2);  // Success queues one readback.
  command_manager.processDelayedWritesAt(9000);
  REQUIRE(fixture.queued() == 2);
}

TEST_CASE("A repeated value also restarts the write delay", "[values_api]") {
  DelayedWriteFixture fixture;
  auto message = fixture.message(21);
  REQUIRE(command_manager.queueWriteAt("01", message, 2000, 0));
  REQUIRE(command_manager.queueWriteAt("01", message, 2000, 1999));
  command_manager.processDelayedWritesAt(2000);
  REQUIRE(fixture.queued() == 0);
  command_manager.processDelayedWritesAt(3999);
  REQUIRE(fixture.queued() == 1);
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);
}

TEST_CASE("Immediate writes supersede waiting writes only when enqueue succeeds", "[values_api]") {
  DelayedWriteFixture fixture;
  auto message = fixture.message(21);
  REQUIRE(command_manager.queueWriteAt("01", message, 2000, 0));
  SECTION("Accepted immediate write cancels the pending value") {
    REQUIRE(command_manager.queueWriteAt("01", fixture.message(22), 0, 1000));
    REQUIRE(command_manager.delayed_writes_.empty());
    command_manager.processDelayedWritesAt(2000);
    REQUIRE(fixture.queued() == 1);
  }
  SECTION("Rejected immediate write leaves the pending value intact") {
    while (getEbusController().enqueue(app::priority::send, message) != 0) {}
    REQUIRE_FALSE(command_manager.queueWriteAt("01", fixture.message(22), 0, 1000));
    REQUIRE(command_manager.delayed_writes_.size() == 1);
    REQUIRE(command_manager.delayed_writes_[0].message == message);
    command_manager.processDelayedWritesAt(2000);
    REQUIRE(command_manager.delayed_writes_.empty());  // Failed once, no app retry.
    REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);
  }
}

TEST_CASE("Pending writes are cancelled when their command changes or disappears", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWriteAt("01", fixture.message(21), 2000, 0));
  SECTION("Command replaced") {
    const auto json = makeCommandJson();
    ebus::detail::JsonReader reader(json);
    command_manager.insertCommand(Command::fromJson(reader));
  }
  SECTION("Command removed") { command_manager.removeCommand("01"); }
  SECTION("All commands removed") { command_manager.removeAll(); }
  SECTION("Application stopping") { command_manager.cancelWrites(); }
  command_manager.processDelayedWritesAt(2000);
  REQUIRE(command_manager.delayed_writes_.empty());
  REQUIRE(fixture.queued() == 0);
}

TEST_CASE("Different keys have independent delayed write deadlines", "[values_api]") {
  DelayedWriteFixture fixture;
  std::string json = makeCommandJson();
  json.replace(json.find("01"), 2, "02");
  ebus::detail::JsonReader reader(json);
  command_manager.insertCommand(Command::fromJson(reader));
  auto message = fixture.message(21);
  REQUIRE(command_manager.queueWriteAt("01", message, 2000, 1000));
  REQUIRE(command_manager.queueWriteAt("02", message, 2000, 2000));
  command_manager.processDelayedWritesAt(3000);
  REQUIRE(command_manager.delayed_writes_.size() == 1);
  REQUIRE(command_manager.findCommand("01")->getWriteSessionId() != 0);
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);
  REQUIRE(command_manager.findCommand("02")->getPendingReadId() == 0);
  command_manager.processDelayedWritesAt(4000);
  REQUIRE(command_manager.delayed_writes_.empty());
  REQUIRE(fixture.queued() == 2);
  ebus::ProtocolInfo result{};
  result.session_id = command_manager.findCommand("01")->getWriteSessionId();
  command_manager.handleWriteResult(result);
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() != 0);
  REQUIRE(command_manager.findCommand("02")->getPendingReadId() == 0);
  result.session_id = command_manager.findCommand("02")->getWriteSessionId();
  command_manager.handleWriteResult(result);
  REQUIRE(command_manager.findCommand("02")->getPendingReadId() != 0);
  REQUIRE(fixture.queued() == 4);
}

TEST_CASE("A full delayed write queue rejects new keys but accepts replacements", "[values_api]") {
  DelayedWriteFixture fixture;
  auto message = fixture.message(21);
  for (size_t i = 0; i <= CommandManager::write_cmd_capacity_; ++i) {
    std::string json = makeCommandJson();
    const auto key = "delay_" + std::to_string(i);
    json.replace(json.find("01"), 2, key);
    ebus::detail::JsonReader reader(json);
    command_manager.insertCommand(Command::fromJson(reader));
    REQUIRE(command_manager.queueWriteAt(key, message, 2000, 0) ==
            (i < CommandManager::write_cmd_capacity_));
  }
  REQUIRE(command_manager.queueWriteAt("delay_0", message, 3000, 1000));
  REQUIRE(command_manager.delayed_writes_[0].due_ms == 4000);
}

TEST_CASE("Write INFO logs correlate replacement, enqueue, bus attempts and readback", "[values_api]") {
  DelayedWriteFixture fixture;
  logger.clearInfoMessages();
  ValuesApi api(command_manager);
  for (double value : {20.0, 21.0}) {
    httpd_req_t req{};
    req.body = std::string(R"({"key":"01","value":)") +
               std::to_string(value) + R"(,"delay_ms":2000})";
    req.content_len = static_cast<int>(req.body.size());
    REQUIRE(ValuesApi::handleValuesWrite(&req) == ESP_OK);
    REQUIRE(req.status == "202 Accepted");
  }
  const auto request_id = command_manager.delayed_writes_[0].request_id;
  const auto due = command_manager.delayed_writes_[0].due_ms;
  command_manager.processDelayedWritesAt(due);
  REQUIRE(command_manager.write_traces_.size() > 0);
  const auto session_id = command_manager.write_traces_.back().session_id;
  REQUIRE(command_manager.findCommand("01")->getPendingReadId() == 0);

  ebus::ProtocolInfo info{};
  info.session_id = session_id;
  info.is_error = true;
  info.protocol_error = ebus::ProtocolError::error_active_master_ack;
  command_manager.handleWriteResult(info);
  info.is_error = false;
  command_manager.handleWriteResult(info);
  command_manager.handleWriteResult(info);  // Duplicate delivery must not duplicate success.
  const auto read_id = command_manager.findCommand("01")->getPendingReadId();
  REQUIRE(read_id != 0);
  const uint8_t response[] = {2, 0, 21};
  info.session_id = read_id;
  info.slave_view = ebus::ByteView(response, sizeof(response));
  command_manager.updateData(info);

  auto count = [](std::string_view fragment) {
    size_t matches = 0;
    for (const auto& entry : logger.infoMessages()) {
      if (entry.message.find(fragment) != std::string::npos) ++matches;
    }
    return matches;
  };
  REQUIRE(count("event=received value=20") == 1);
  REQUIRE(count("event=received value=21") == 1);
  REQUIRE(count("event=replaced previous_req=") == 1);
  const auto prefix = "[WRITE] req=" + std::to_string(request_id) + " key=01 event=";
  REQUIRE(count(prefix + "bus_queued") == 1);
  REQUIRE(count(prefix + "bus_error") == 1);
  REQUIRE(count(prefix + "bus_success") == 1);
  REQUIRE(count("device_value_not_yet_confirmed") == 1);
  REQUIRE(count("event=readback value=21") == 1);
  bool linked_read = false;
  for (const auto& entry : logger.infoMessages()) {
    if (entry.message.find("event=readback") != std::string::npos) {
      linked_read = entry.session_id == read_id;
    }
  }
  REQUIRE(linked_read);
}

TEST_CASE("Rejected delayed dispatch and cancellation remain visible at INFO level", "[values_api]") {
  DelayedWriteFixture fixture;
  logger.clearInfoMessages();
  const auto message = fixture.message(21);
  REQUIRE(command_manager.queueWriteAt("01", message, 2000, 0));
  const auto request_id = command_manager.delayed_writes_[0].request_id;
  SECTION("Bus queue rejection") {
    while (getEbusController().enqueue(app::priority::send, message) != 0) {}
    command_manager.processDelayedWritesAt(2000);
    bool found = false;
    for (const auto& entry : logger.infoMessages()) {
      if (entry.message.find("req=" + std::to_string(request_id) +
          " key=01 event=rejected reason=bus_queue_unavailable") != std::string::npos) found = true;
    }
    REQUIRE(found);
  }
  SECTION("Command removed") {
    command_manager.removeCommand("01");
    bool found = false;
    for (const auto& entry : logger.infoMessages()) {
      if (entry.message.find("event=cancelled") != std::string::npos) found = true;
    }
    REQUIRE(found);
  }
}

TEST_CASE("Exhausted arbitration is logged once as a terminal write failure", "[values_api]") {
  DelayedWriteFixture fixture;
  logger.clearInfoMessages();
  REQUIRE(command_manager.queueWrite("01", fixture.message(21)));
  const auto session_id = command_manager.write_traces_.back().session_id;
  ebus::ProtocolInfo info{};
  info.session_id = session_id;
  info.is_error = true;
  info.protocol_error = ebus::ProtocolError::arbitration_lost;
  info.attempts = 3;
  command_manager.handleWriteResult(info);
  command_manager.handleWriteResult(info);
  size_t failures = 0;
  for (const auto& entry : logger.infoMessages()) {
    if (entry.message.find("event=bus_failed") != std::string::npos) {
      ++failures;
      REQUIRE(entry.message.find("arbitration_attempts_exhausted attempts=3") != std::string::npos);
      REQUIRE(entry.session_id == session_id);
    }
  }
  REQUIRE(failures == 1);
}

TEST_CASE("Only the current immediate write requests one readback on success", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(20)));
  auto* command = command_manager.findCommand("01");
  const auto old_session = command->getWriteSessionId();
  REQUIRE(command_manager.queueWrite("01", fixture.message(21)));
  const auto current_session = command->getWriteSessionId();
  REQUIRE(current_session != old_session);
  REQUIRE(fixture.queued() == 1);  // The queued older write was replaced.
  REQUIRE(command->getPendingReadId() == 0);

  ebus::ProtocolInfo result{};
  result.session_id = old_session;
  command_manager.handleWriteResult(result);  // A late completion from an in-flight old write.
  REQUIRE(command->getWriteSessionId() == current_session);
  REQUIRE(command->getPendingReadId() == 0);
  REQUIRE(fixture.queued() == 1);

  result.session_id = current_session;
  command_manager.handleWriteResult(result);
  const auto read_session = command->getPendingReadId();
  REQUIRE(read_session != 0);
  REQUIRE(command->getWriteSessionId() == 0);
  REQUIRE(fixture.queued() == 2);
  command_manager.handleWriteResult(result);
  REQUIRE(command->getPendingReadId() == read_session);
  REQUIRE(fixture.queued() == 2);
}

TEST_CASE("An accepted delayed value supersedes an old bus write before its deadline", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(20)));
  auto* command = command_manager.findCommand("01");
  const auto old_session = command->getWriteSessionId();
  REQUIRE(command_manager.queueWriteAt("01", fixture.message(21.5), 2000, 1000));
  REQUIRE(command->getWriteSessionId() == 0);
  REQUIRE(fixture.queued() == 0);
  ebus::ProtocolInfo result{};
  result.session_id = old_session;
  command_manager.handleWriteResult(result);
  REQUIRE(command->getPendingReadId() == 0);
  command_manager.processDelayedWritesAt(2999);
  REQUIRE(fixture.queued() == 0);
  command_manager.processDelayedWritesAt(3000);
  const auto current_session = command->getWriteSessionId();
  REQUIRE(current_session != 0);
  REQUIRE(current_session != old_session);
  REQUIRE(fixture.queued() == 1);
  command_manager.handleWriteResult(result);
  REQUIRE(command->getWriteSessionId() == current_session);
  REQUIRE(command->getPendingReadId() == 0);
  result.session_id = current_session;
  command_manager.handleWriteResult(result);
  REQUIRE(command->getPendingReadId() != 0);
  REQUIRE(fixture.queued() == 2);
}

TEST_CASE("An attempt error preserves the write until its later successful completion", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(21)));
  auto* command = command_manager.findCommand("01");
  const auto session = command->getWriteSessionId();
  ebus::ProtocolInfo result{};
  result.session_id = session;
  result.is_error = true;
  result.protocol_error = ebus::ProtocolError::total_transfer_timeout;
  command_manager.handleWriteResult(result);
  REQUIRE(command->getWriteSessionId() == session);
  REQUIRE(command->getPendingReadId() == 0);
  REQUIRE(fixture.queued() == 1);
  result.is_error = false;
  command_manager.handleWriteResult(result);
  REQUIRE(command->getWriteSessionId() == 0);
  REQUIRE(command->getPendingReadId() != 0);
  REQUIRE(fixture.queued() == 2);
}

TEST_CASE("Readback does not depend on retained diagnostic history", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(21)));
  auto* command = command_manager.findCommand("01");
  ebus::ProtocolInfo result{};
  result.session_id = command->getWriteSessionId();
  command_manager.write_traces_.clear();  // Model eviction of the diagnostic entry.
  command_manager.handleWriteResult(result);
  REQUIRE(command->getWriteSessionId() == 0);
  REQUIRE(command->getPendingReadId() != 0);
  REQUIRE(fixture.queued() == 2);
}

TEST_CASE("Removing or redefining a command suppresses readback of its old write", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(21)));
  ebus::ProtocolInfo result{};
  result.session_id = command_manager.findCommand("01")->getWriteSessionId();
  SECTION("Command removed") { command_manager.removeCommand("01"); }
  SECTION("Command redefined") {
    const auto json = makeCommandJson();
    ebus::detail::JsonReader reader(json);
    command_manager.insertCommand(Command::fromJson(reader));
  }
  SECTION("All commands removed") { command_manager.removeAll(); }
  SECTION("Application stopped") { command_manager.cancelWrites(); }
  command_manager.handleWriteResult(result);
  REQUIRE(fixture.queued() == 0);
  if (const auto* command = command_manager.findCommand("01")) {
    REQUIRE(command->getWriteSessionId() == 0);
    REQUIRE(command->getPendingReadId() == 0);
  }
}

TEST_CASE("A rejected newer write preserves completion handling of its predecessor", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(20)));
  auto* command = command_manager.findCommand("01");
  const auto old_session = command->getWriteSessionId();
  // Model completion in the library before its callback reaches the manager.
  REQUIRE(getEbusController().cancel(old_session));
  uint32_t last_filler = 0;
  while (const auto session = getEbusController().enqueue(app::priority::send, fixture.message(19))) {
    last_filler = session;
  }
  REQUIRE(last_filler != 0);
  REQUIRE_FALSE(command_manager.queueWrite("01", fixture.message(21)));
  REQUIRE(command->getWriteSessionId() == old_session);
  REQUIRE(command->getPendingReadId() == 0);
  REQUIRE(getEbusController().cancel(last_filler));
  ebus::ProtocolInfo result{};
  result.session_id = old_session;
  command_manager.handleWriteResult(result);
  REQUIRE(command->getWriteSessionId() == 0);
  REQUIRE(command->getPendingReadId() != 0);
}

TEST_CASE("Terminal write failures never request readback", "[values_api]") {
  DelayedWriteFixture fixture;
  REQUIRE(command_manager.queueWrite("01", fixture.message(21)));
  auto* command = command_manager.findCommand("01");
  ebus::ProtocolInfo result{};
  result.session_id = command->getWriteSessionId();
  result.is_error = true;
  SECTION("Arbitration exhausted") { result.protocol_error = ebus::ProtocolError::arbitration_lost; }
  SECTION("Invalid message") { result.protocol_error = ebus::ProtocolError::invalid_message; }
  command_manager.handleWriteResult(result);
  REQUIRE(command->getWriteSessionId() == 0);
  REQUIRE(command->getPendingReadId() == 0);
}
