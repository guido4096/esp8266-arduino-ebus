#if defined(EBUS_INTERNAL)
#include "app/command_manager.hpp"

#include <esp_littlefs.h>
#include <esp_timer.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ebus/detail/json_reader.hpp>
#include <ebus/detail/json_writer.hpp>
#include <ebus/detail/protocol_limits.hpp>
#include <limits>
#ifdef EBUS_COMMANDS_FILE_PATH
#include <unistd.h>
#endif

#include "app/mqtt.hpp"
#include "app/command_read.hpp"
#include "system/logger.hpp"

CommandManager command_manager;

namespace {
// Called only on application/monitor tasks, never from the bus callback.
void writeLog(uint32_t request_id, std::string_view key, const char* event,
              std::string_view detail = {}, uint32_t session_id = 0) {
  char message[384];
  snprintf(message, sizeof(message), "[WRITE] req=%lu key=%.*s event=%s %.*s",
           static_cast<unsigned long>(request_id),
           static_cast<int>(std::min<size_t>(key.size(), 64)), key.data(), event,
           static_cast<int>(std::min<size_t>(detail.size(), 240)),
           detail.empty() ? "" : detail.data());
  logger.info(message, false, session_id);
}

constexpr const char* littlefs_base_path = "/littlefs";
constexpr const char* littlefs_partition_label = "littlefs";
#ifndef EBUS_COMMANDS_FILE_PATH
const char* commandsFilePath() { return "/littlefs/commands.json"; }
#else
const char* commandsFilePath() {
  static char path[256];
  static const int path_length =
      // cppcheck-suppress invalidPrintfArgType_s
      std::snprintf(path, sizeof(path), "%s.%ld", EBUS_COMMANDS_FILE_PATH,
                    static_cast<long>(getpid()));
  (void)path_length;
  return path;
}
#endif
#ifndef EBUS_COMMANDS_TMP_FILE_PATH
const char* commandsTempFilePath() { return "/littlefs/commands.json.tmp"; }
#else
const char* commandsTempFilePath() {
  static char path[256];
  static const int path_length =
      // cppcheck-suppress invalidPrintfArgType_s
      std::snprintf(path, sizeof(path), "%s.%ld", EBUS_COMMANDS_TMP_FILE_PATH,
                    static_cast<long>(getpid()));
  (void)path_length;
  return path;
}
#endif

bool ensureLittlefsMounted() {
  static bool mounted = false;
  if (mounted) return true;

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = littlefs_base_path;
  conf.partition_label = littlefs_partition_label;
  conf.partition = nullptr;
  conf.format_if_mount_failed = true;
  conf.read_only = false;
  conf.dont_mount = false;
  conf.grow_on_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    mounted = true;
    return true;
  }

  return false;
}

}  // namespace

bool CommandManager::initFileSystem() { return ensureLittlefsMounted(); }

void CommandManager::setDataUpdatedCallback(DataUpdatedCallback callback) {
  data_updated_callback_ = std::move(callback);
}

void CommandManager::setDataUpdatedLogCallback(
    DataUpdatedLogCallback callback) {
  data_updated_log_callback_ = std::move(callback);
}

void CommandManager::setCommandChangedCallback(
    CommandChangedCallback callback) {
  command_changed_callback_ = std::move(callback);
}

void CommandManager::setCommandRemovedCallback(
    CommandChangedCallback callback) {
  command_removed_callback_ = std::move(callback);
}

void CommandManager::insertCommand(Command command) {
  if (command.getWriteType() == command_types::WriteType::invalid) {
    logger.error("CommandManager: rejected unsupported write_type");
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = 0; i < commands_.size(); i++) {
    if (std::string_view(commands_[i].getKey()) ==
        std::string_view(command.getKey())) {
      cancelDelayedWrite(commands_[i].getKeyId());
      uint16_t old_poll_id = commands_[i].getPollId();
      commands_[i] = std::move(command);
      commands_[i].setPollId(old_poll_id);
      if (command_changed_callback_) command_changed_callback_(&commands_[i]);
      return;
    }
  }
  if (commands_.push_back(std::move(command))) {
    if (command_changed_callback_) command_changed_callback_(&commands_.back());
  }
}

void CommandManager::removeCommand(std::string_view key) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = 0; i < commands_.size(); i++) {
    if (std::string_view(commands_[i].getKey()) == key) {
      uint8_t removed_key_id = commands_[i].getKeyId();
      cancelDelayedWrite(removed_key_id);
      if (command_removed_callback_) command_removed_callback_(&commands_[i]);
      commands_.erase(commands_.begin() + i);
      removeFieldOverrides(removed_key_id);
      return;
    }
  }
}

void CommandManager::removeAll() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = commands_.size(); i-- > 0;) {
    if (command_removed_callback_) command_removed_callback_(&commands_[i]);
  }
  commands_.clear();
  for (const auto& pending : delayed_writes_) {
    writeLog(pending.request_id, StringPool::instance().lookup(pending.key_id),
             "cancelled", "reason=commands_cleared");
  }
  delayed_writes_.clear();
  clearFieldOverrides();
}

Command* CommandManager::findCommand(std::string_view key) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = 0; i < commands_.size(); i++) {
    if (std::string_view(commands_[i].getKey()) == key) {
      return &commands_[i];
    }
  }
  return nullptr;
}

Command* CommandManager::findCommand(uint16_t poll_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = 0; i < commands_.size(); i++) {
    Command* cmd = &commands_[i];
    if (cmd->getPollId() == poll_id) {
      return cmd;
    }
  }
  return nullptr;
}

MatchingCommands CommandManager::findPassiveCommands(ebus::ByteView master) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  MatchingCommands result;
  for (size_t i = 0; i < commands_.size(); i++) {
    Command* cmd = &commands_[i];
    if (cmd->getActive()) continue;
    if (cmd->matches(master)) {
      if (!result.push_back(cmd)) break;
    }
  }
  return result;
}

int64_t CommandManager::loadCommands() {
  if (!ensureLittlefsMounted()) return -1;
  std::remove(commandsTempFilePath());
  return loadCommandsFrom(commandsFilePath());
}

int64_t CommandManager::loadCommandsFrom(const char* path) {
  if (!ensureLittlefsMounted()) return -1;

  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    if (errno == ENOENT) return 0;
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf),
             "CommandManager: Failed to open commands file %s: %d", path,
             errno);
    logger.error(err_buf);
    return -1;
  }

  // Use small buffer to avoid large heap allocation for FILE* stream
  char file_buf[512];
  std::setvbuf(file, file_buf, _IOFBF, sizeof(file_buf));

  char log_buf[96];
  snprintf(log_buf, sizeof(log_buf), "CommandManager: Loading from %s", path);
  logger.info(log_buf);

  deserializeCommands(file);
  std::fclose(file);
  return static_cast<int64_t>(command_manager.getCommandCount());
}

int64_t CommandManager::saveCommands() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!ensureLittlefsMounted()) return -1;
  bool commands_empty = commands_.empty();
  if (commands_empty) return 0;

  FILE* file = std::fopen(commandsFilePath(), "wb");
  if (file == nullptr) return -1;

  char file_buf[512];
  std::setvbuf(file, file_buf, _IOFBF, sizeof(file_buf));

  size_t bytes_written = 0;
  ebus::detail::JsonWriter writer([file, &bytes_written](std::string_view s) {
    bytes_written += std::fwrite(s.data(), 1, s.size(), file);
  });

  {
    auto root_array = writer.arrayScope();

    // Header row for compressed format
    {
      auto header_array = writer.arrayScope();
      static const char* header[] = {"key",       "name",     "read_cmd",
                                     "write_cmd", "interval", "master",
                                     "fields", "write_type"};
      for (const char* h : header) writer.writeValue(h);
    }

    // Data rows in tabular format
    for (const Command& c : commands_) {
      auto row_array = writer.arrayScope();
      writer.writeValue(c.getKey());
      writer.writeValue(c.getName());
      writer.writeHexValue(c.getReadCmd());
      // Serialize write_cmd from separate storage
      if (c.hasWriteCmd()) {
        writer.writeHexValue(c.getWriteCmd(*this));
      } else {
        writer.writeValue("");
      }
      writer.writeValue(c.getInterval());
      writer.writeValue(c.getMaster());

      // Serialize fields as JSON (with per-field HA)
      {
        auto fields_arr = writer.arrayScope();
        for (size_t i = 0; i < c.getFieldCount(); i++) {
          auto field_obj = writer.objectScope();
          writer.writeField("name", c.getFieldName(i));
          writer.writeField("profile", c.getFieldProfile(i)
                                           ? c.getFieldProfile(i)->name
                                           : "");
          writer.writeField("position",
                            static_cast<uint32_t>(c.getFieldPosition(i)));
          if (c.isFieldRaw(i)) {
            writer.writeField("raw", true);
          }
          writer.writeField("ha_profile", c.getFieldHAProfileName(i));
          float min_ov = getFieldMinOverride(c.getKeyId(), i);
          float max_ov = getFieldMaxOverride(c.getKeyId(), i);
          if (!std::isnan(min_ov)) {
            writer.writeField("min", min_ov);
          }
          if (!std::isnan(max_ov)) {
            writer.writeField("max", max_ov);
          }
        }
      }
      writer.writeValue(c.getWriteTypeName());
    }
  }
  writer.flush();
  std::fclose(file);

  return static_cast<int64_t>(bytes_written);
}

int64_t CommandManager::wipeCommands() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  commands_.clear();
  for (const auto& pending : delayed_writes_) {
    writeLog(pending.request_id, StringPool::instance().lookup(pending.key_id),
             "cancelled", "reason=commands_cleared");
  }
  delayed_writes_.clear();
  clearFieldOverrides();
  if (!ensureLittlefsMounted()) return -1;

  struct stat file_stat{};
  if (stat(commandsFilePath(), &file_stat) != 0) {
    if (errno == ENOENT) return 0;
    return -1;
  }

  if (std::remove(commandsFilePath()) != 0) {
    if (errno == ENOENT) return 0;
    return -1;
  }

  if (file_stat.st_size <= 0) {
    return 0;
  }

  return static_cast<int64_t>(file_stat.st_size);
}

void CommandManager::fetchCommands(
    const ebus::JsonChunkVisitor& visitor) const {
  // Per-command locking (see fetchValues): never hold the mutex across
  // socket I/O to slow readers.
  visitor("[");
  size_t i = 0;
  while (true) {
    std::string frag;
    bool more = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      if (i >= commands_.size()) break;
      size_t n = commands_.size();
      std::array<const Command*, command_capacity_> ordered{};
      for (size_t k = 0; k < n; k++) {
        ordered[k] = &commands_[k];
      }
      std::sort(ordered.begin(), ordered.begin() + n,
                [](const Command* a, const Command* b) {
                  return std::string_view(a->getKey()) <
                         std::string_view(b->getKey());
                });
      const Command* c = ordered[i];
      ebus::detail::JsonWriter writer(
          [&frag](std::string_view s) { frag.append(s); });
      auto scope = writer.objectScope();
      writer.writeField("key", c->getKey());
      writer.writeField("name", c->getName());
      writer.writeHexField("read_cmd", c->getReadCmd());
      if (c->hasWriteCmd()) {
        writer.writeHexField("write_cmd", c->getWriteCmd(*this));
      } else {
        writer.writeField("write_cmd", "");
      }
      writer.writeField("interval", c->getInterval());
      writer.writeField("master", c->getMaster());
      writer.writeField("write_type", c->getWriteTypeName());

      auto arr = writer.arrayScope("fields");
      for (size_t j = 0; j < c->getFieldCount(); j++) {
        auto field_obj = writer.objectScope();
        writer.writeField("name", c->getFieldName(j));
        const DataProfile* p = c->getFieldProfile(j);
        writer.writeField("profile", p ? p->name : "");
        writer.writeField("position",
                          static_cast<uint32_t>(c->getFieldPosition(j)));
        writer.writeField("ha_profile", c->getFieldHAProfileName(j));
        if (c->isFieldRaw(j)) {
          writer.writeField("raw", true);
        }
        float min_ov = getFieldMinOverride(c->getKeyId(), j);
        float max_ov = getFieldMaxOverride(c->getKeyId(), j);
        if (!std::isnan(min_ov)) {
          writer.writeField("min", min_ov);
        }
        if (!std::isnan(max_ov)) {
          writer.writeField("max", max_ov);
        }
      }
      more = (i + 1 < commands_.size());
    }
    visitor(frag);
    if (!more) break;
    visitor(",");
    ++i;
  }
  visitor("]");
}

size_t CommandManager::getActiveCommands() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return std::count_if(commands_.begin(), commands_.end(),
                       [](const Command& c) { return c.getActive(); });
}

size_t CommandManager::getPassiveCommands() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return std::count_if(commands_.begin(), commands_.end(),
                       [](const Command& c) { return !c.getActive(); });
}

size_t CommandManager::getCommandCount() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return commands_.size();
}

Command* CommandManager::nextActiveCommand() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Command* next = nullptr;
  bool init = false;
  for (size_t i = 0; i < commands_.size(); i++) {
    Command* cmd = &commands_[i];
    if (!cmd->getActive()) continue;
    if (cmd->getLast() == 0) {
      next = cmd;
      init = true;
      break;
    }
    if (next == nullptr || (cmd->getLast() + cmd->getInterval() * 1000 <
                            next->getLast() + next->getInterval() * 1000))
      next = cmd;
  }

  if (!init && next &&
      (uint32_t)(esp_timer_get_time() / 1000ULL) <
          next->getLast() + next->getInterval() * 1000)
    next = nullptr;

  return next;
}
void CommandManager::updateData(const ebus::ProtocolInfo& info) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  Command* command = nullptr;
  if (info.poll_id != 0) {
    command = findCommand(info.poll_id);
  }
  // Manual reads are one-shot sessions, not recurring poll items.
  if (!command && info.session_id != 0) {
    for (auto& candidate : commands_) {
      if (candidate.getPendingReadId() == info.session_id) {
        command = &candidate;
        candidate.setPendingReadId(0);
        break;
      }
    }
  }

  auto update = [this](Command* cmd, const ebus::ProtocolInfo& info) {
    if (cmd->getFieldCount() == 0) return;

    bool is_master = cmd->getMaster();
    size_t data_len = 0;
    if (is_master) {
      if (info.master_view.size() >= 5) data_len = info.master_view[4];
    } else {
      if (info.slave_view.size() >= 1) data_len = info.slave_view[0];
    }

    if (data_len == 0) return;

    cmd->setLast((uint32_t)(esp_timer_get_time() / 1000ULL));
    cmd->setSessionId(info.session_id);

    if (is_master) {
      cmd->setData(ebus::range(info.master_view, 5, data_len));
    } else {
      cmd->setData(ebus::range(info.slave_view, 1, data_len));
    }

    if (cmd->hasWriteCmd()) {
      std::string value;
      ebus::detail::JsonWriter writer([&value](std::string_view part) {
        value.append(part);
      });
      cmd->getValueJson(writer);
      writer.flush();
      writeLog(0, cmd->getKey(), "readback", "value=" + value, info.session_id);
    }

    if (data_updated_callback_) {
      data_updated_callback_(cmd->getKey());
    }

    if (data_updated_log_callback_) {
      data_updated_log_callback_(cmd->getKey());
    }
  };

  if (command) {
    update(command, info);
    return;
  }

  MatchingCommands matching_commands = findPassiveCommands(info.master_view);
  for (Command* cmd : matching_commands) update(cmd, info);
}

// Hold the manager lock through enqueue/cancellation so an immediate write
// cannot race an expired delayed write for the same key.
bool CommandManager::queueWrite(std::string_view key, ebus::ByteView message,
                                uint32_t delay_ms, std::string_view value) {
  return queueWriteAt(key, message, delay_ms,
                      static_cast<uint64_t>(esp_timer_get_time() / 1000ULL), value);
}

bool CommandManager::queueWriteAt(std::string_view key, ebus::ByteView message,
                                  uint32_t delay_ms, uint64_t now_ms,
                                  std::string_view value) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Command* command = findCommand(key);
  if (!command || !command->hasWriteCmd() || message.empty() ||
      delay_ms > max_write_delay_ms) return false;

  if (++next_write_request_id_ == 0) ++next_write_request_id_;
  const uint32_t request_id = next_write_request_id_;
  char detail[240];
  if (!value.empty()) {
    snprintf(detail, sizeof(detail), "value=%.*s delay_ms=%lu",
             static_cast<int>(std::min<size_t>(value.size(), 128)), value.data(),
             static_cast<unsigned long>(delay_ms));
  } else {
    snprintf(detail, sizeof(detail), "delay_ms=%lu telegram=%s",
             static_cast<unsigned long>(delay_ms), ebus::toString(message).c_str());
  }
  writeLog(request_id, key, "received", detail);

  if (delay_ms == 0) {
    const auto session_id = getEbusController().enqueue(app::priority::send, message);
    if (session_id == 0) {
      writeLog(request_id, key, "rejected", "reason=bus_queue_unavailable");
      return false;
    }
    cancelDelayedWrite(command->getKeyId(), "superseded_by_immediate_write");
    trackWrite(request_id, command->getKeyId(), session_id, message);
    return true;
  } else {
    for (auto& pending : delayed_writes_) {
      if (pending.key_id == command->getKeyId()) {
        snprintf(detail, sizeof(detail), "previous_req=%lu delay_ms=%lu",
                 static_cast<unsigned long>(pending.request_id),
                 static_cast<unsigned long>(delay_ms));
        writeLog(request_id, key, "replaced", detail);
        pending.request_id = request_id;
        pending.message.assign(message);
        pending.due_ms = now_ms + delay_ms;
        return true;
      }
    }
    DelayedWrite pending;
    pending.key_id = command->getKeyId();
    pending.request_id = request_id;
    pending.due_ms = now_ms + delay_ms;
    pending.message.assign(message);
    if (!delayed_writes_.push_back(std::move(pending))) {
      writeLog(request_id, key, "rejected", "reason=delayed_queue_full");
      return false;
    }
    writeLog(request_id, key, "waiting");
    return true;
  }
}

void CommandManager::cancelDelayedWrite(uint8_t key_id, const char* reason) {
  for (auto it = delayed_writes_.begin(); it != delayed_writes_.end(); ++it) {
    if (it->key_id == key_id) {
      writeLog(it->request_id, StringPool::instance().lookup(key_id),
               "cancelled", reason);
      delayed_writes_.erase(it);
      return;
    }
  }
}

void CommandManager::clearDelayedWrites() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& pending : delayed_writes_) {
    writeLog(pending.request_id, StringPool::instance().lookup(pending.key_id),
             "cancelled", "reason=pending_writes_cleared");
  }
  delayed_writes_.clear();
}

void CommandManager::processDelayedWrites() {
  processDelayedWritesAt(
      static_cast<uint64_t>(esp_timer_get_time() / 1000ULL));
}

void CommandManager::processDelayedWritesAt(uint64_t now_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto it = delayed_writes_.begin(); it != delayed_writes_.end();) {
    if (now_ms < it->due_ms) {
      ++it;
      continue;
    }
    const auto key = StringPool::instance().lookup(it->key_id);
    Command* command = findCommand(key);
    if (command) {
      const auto session_id =
          getEbusController().enqueue(app::priority::send, it->message);
      if (session_id != 0) {
        trackWrite(it->request_id, it->key_id, session_id, it->message);
        requestCommandRead(*command);
      } else {
        writeLog(it->request_id, key, "rejected", "reason=bus_queue_unavailable");
        logger.warn("Delayed write could not be queued for key: " +
                    std::string(key));
      }
    }
    // No extra retries: bus retries remain the library's responsibility.
    it = delayed_writes_.erase(it);
  }
}

void CommandManager::trackWrite(uint32_t request_id, uint8_t key_id,
                                 uint32_t session_id, ebus::ByteView message) {
  // Bounded diagnostic history only; eviction never changes bus scheduling.
  if (write_traces_.size() == write_traces_.capacity()) {
    write_traces_.erase(write_traces_.begin());
  }
  write_traces_.push_back({request_id, session_id, key_id, false});
  writeLog(request_id, StringPool::instance().lookup(key_id), "bus_queued",
           "telegram=" + ebus::toString(message), session_id);
}

void CommandManager::logWriteResult(const ebus::ProtocolInfo& info) {
  if (info.session_id == 0) return;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (auto& trace : write_traces_) {
    if (trace.session_id != info.session_id || trace.terminal_logged) continue;
    const auto key = StringPool::instance().lookup(trace.key_id);
    if (info.is_error && info.protocol_error == ebus::ProtocolError::arbitration_lost) {
      trace.terminal_logged = true;
      char detail[128];
      snprintf(detail, sizeof(detail), "reason=arbitration_attempts_exhausted attempts=%u",
               info.attempts);
      writeLog(trace.request_id, key, "bus_failed", detail, info.session_id);
    } else if (info.is_error) {
      // Library callbacks include attempt errors, not just terminal failures.
      char detail[192];
      snprintf(detail, sizeof(detail), "error=%s attempts=%u retries=library_managed",
               ebus::toString(info.protocol_error), info.attempts);
      writeLog(trace.request_id, key, "bus_error", detail, info.session_id);
    } else {
      trace.terminal_logged = true;
      writeLog(trace.request_id, key, "bus_success",
               "device_value_not_yet_confirmed", info.session_id);
    }
    return;
  }
}

void CommandManager::fetchValues(const ebus::JsonChunkVisitor& visitor) const {
  // Per-command locking: the visitor performs socket I/O, and holding the
  // command mutex across a slow reader stalls the bus threads updating
  // values (stale timer arms, stray QQs). Render each row under a short
  // lock, send it after release.
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
  visitor("[");
  size_t i = 0;
  while (true) {
    std::string frag;
    bool more = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      if (i >= commands_.size()) break;
      size_t n = commands_.size();
      std::array<const Command*, command_capacity_> ordered{};
      for (size_t k = 0; k < n; k++) {
        ordered[k] = &commands_[k];
      }
      std::sort(ordered.begin(), ordered.begin() + n,
                [](const Command* a, const Command* b) {
                  return std::string_view(a->getKey()) <
                         std::string_view(b->getKey());
                });
      const Command* cmd = ordered[i];
      ebus::detail::JsonWriter writer(
          [&frag](std::string_view s) { frag.append(s); });
      const uint32_t age =
          (cmd->getLast() > 0) ? (now - cmd->getLast()) / 1000 : 0;
      const size_t fields = cmd->getFieldCount();
      if (fields <= 1) {
        auto scope = writer.objectScope();
        writer.writeField("key", cmd->getKey());
        writer.writeField("name", cmd->getName());

        writer.appendKey("value");
        cmd->getValueJson(writer);

        std::string unit;
        if (fields > 0) {
          unit = std::string(cmd->getFieldUnit(0));
        }
        writer.writeField("unit", unit);
        if (fields > 0) cmd->writeFieldText(writer, 0);
        writer.writeField("age", age);
        writer.writeField("write", cmd->hasWriteCmd());
        writer.writeField("active", cmd->getActive());
      } else {
        // Multi-field: one row per field so dumb renderers (values page)
        // never see [object Object]. Name carries both sides
        // ("cmd/field"); Read polls the whole telegram once (see UI: only
        // the first field row carries the button).
        for (size_t f = 0; f < fields; ++f) {
          auto scope = writer.objectScope();
          writer.writeField("key", cmd->getKey());
          const char* fn = cmd->getFieldName(f);
          std::string name =
              std::string(cmd->getName()) + "/" + (fn && fn[0] ? fn : "value");
          writer.writeField("name", name);
          writer.writeField("field", (fn && fn[0] ? fn : "value"));

          writer.appendKey("value");
          cmd->writeFieldValue(writer, f);

          writer.writeField("unit",
                            cmd->getFieldUnit(f) ? cmd->getFieldUnit(f) : "");
          cmd->writeFieldText(writer, f);
          writer.writeField("age", age);
          writer.writeField("write", cmd->hasWriteCmd());
          writer.writeField("active", cmd->getActive());
        }
      }
      more = (i + 1 < commands_.size());
    }
    visitor(frag);
    if (!more) break;
    visitor(",");
    ++i;
  }
  visitor("]");
}

ebus::ByteView CommandManager::getWriteCmd(size_t idx) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (idx < write_cmds_.size()) {
    return ebus::ByteView(write_cmds_[idx].data(), write_cmds_[idx].size());
  }
  return {};
}

bool CommandManager::addWriteCmd(PollSequence&& cmd) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (write_cmds_.size() >= write_cmd_capacity_) {
    return false;
  }
  write_cmds_.push_back(std::move(cmd));
  return true;
}

size_t CommandManager::getWriteCmdCount() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return write_cmds_.size();
}

namespace {
template <size_t Capacity>
FieldOverride* findOverride(ebus::StaticVector<FieldOverride, Capacity>& pool,
                            uint8_t key_id, size_t field_idx) {
  for (size_t i = 0; i < pool.size(); ++i) {
    if (pool[i].key_id == key_id && pool[i].field_idx == field_idx) {
      return &pool[i];
    }
  }
  return nullptr;
}

template <size_t Capacity>
const FieldOverride* findOverride(
    const ebus::StaticVector<FieldOverride, Capacity>& pool, uint8_t key_id,
    size_t field_idx) {
  for (size_t i = 0; i < pool.size(); ++i) {
    if (pool[i].key_id == key_id && pool[i].field_idx == field_idx) {
      return &pool[i];
    }
  }
  return nullptr;
}
}  // namespace

float CommandManager::getFieldMinOverride(uint8_t key_id,
                                          size_t field_idx) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto* o =
      findOverride(field_overrides_, key_id, static_cast<uint8_t>(field_idx));
  return o ? o->min_override : std::numeric_limits<float>::quiet_NaN();
}

float CommandManager::getFieldMaxOverride(uint8_t key_id,
                                          size_t field_idx) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto* o =
      findOverride(field_overrides_, key_id, static_cast<uint8_t>(field_idx));
  return o ? o->max_override : std::numeric_limits<float>::quiet_NaN();
}

void CommandManager::setFieldMinOverride(uint8_t key_id, size_t field_idx,
                                         float min_val) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto* o =
      findOverride(field_overrides_, key_id, static_cast<uint8_t>(field_idx));
  if (o) {
    o->min_override = min_val;
    return;
  }
  if (field_overrides_.size() >= field_overrides_.capacity()) return;
  FieldOverride entry;
  entry.key_id = key_id;
  entry.field_idx = static_cast<uint8_t>(field_idx);
  entry.min_override = min_val;
  field_overrides_.push_back(entry);
}

void CommandManager::setFieldMaxOverride(uint8_t key_id, size_t field_idx,
                                         float max_val) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto* o =
      findOverride(field_overrides_, key_id, static_cast<uint8_t>(field_idx));
  if (o) {
    o->max_override = max_val;
    return;
  }
  if (field_overrides_.size() >= field_overrides_.capacity()) return;
  FieldOverride entry;
  entry.key_id = key_id;
  entry.field_idx = static_cast<uint8_t>(field_idx);
  entry.max_override = max_val;
  field_overrides_.push_back(entry);
}

void CommandManager::removeFieldOverrides(uint8_t key_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = field_overrides_.size(); i-- > 0;) {
    if (field_overrides_[i].key_id == key_id) {
      field_overrides_.erase(field_overrides_.begin() + i);
    }
  }
}

void CommandManager::clearFieldOverrides() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  field_overrides_.clear();
}

void CommandManager::deserializeCommands(FILE* file) {
  constexpr size_t reader_buf_size = 1536;
  constexpr size_t row_buf_size = 1024;
  constexpr size_t chunk_size = 512;

  static char reader_buf[reader_buf_size];
  static char row_buf[row_buf_size];
  char chunk_buf[chunk_size];

  ebus::detail::JsonReader reader(reader_buf, sizeof(reader_buf));

  bool eof = false;
  size_t loaded_count = 0;
  bool header_seen = false;

  auto feed_file = [&]() -> bool {
    if (eof) return false;
    size_t n = std::fread(chunk_buf, 1, sizeof(chunk_buf), file);
    if (n > 0) {
      reader.feed(std::string_view(chunk_buf, n));
      return true;
    }
    eof = true;
    reader.endOfInput();
    return false;
  };

  feed_file();

  // Expect root array
  while (true) {
    auto t = reader.next();
    if (t == ebus::detail::JsonReader::Token::need_more_data) {
      if (!feed_file()) {
        t = reader.next();
        if (t == ebus::detail::JsonReader::Token::need_more_data) return;
      }
      continue;
    }
    if (t == ebus::detail::JsonReader::Token::array_start) break;
    if (t == ebus::detail::JsonReader::Token::end ||
        t == ebus::detail::JsonReader::Token::error)
      return;
  }

  // Read each element
  while (true) {
    std::string_view row_sv = reader.rawValue();
    if (row_sv.empty()) {
      if (reader.needsMoreData()) {
        if (eof) {
          reader.endOfInput();
        }
        if (!feed_file()) {
          if (reader.needsMoreData()) {
            logger.warn(
                "CommandManager: Command element exceeds reader buffer size");
            return;
          }
          continue;
        }
        continue;
      }
      break;
    }

    size_t copy_len =
        row_sv.size() < row_buf_size ? row_sv.size() : row_buf_size - 1;
    std::memcpy(row_buf, row_sv.data(), copy_len);
    row_buf[copy_len] = '\0';

    ebus::detail::JsonReader row_reader(std::string_view(row_buf, copy_len));
    auto token = row_reader.next();

    if (token == ebus::detail::JsonReader::Token::array_start) {
      if (!header_seen) {
        header_seen = true;
        continue;
      }
      // Pre-extract key to drop any stale field overrides for that key
      // before fromTabular() repopulates the pool.
      row_reader.reset();
      if (row_reader.next() == ebus::detail::JsonReader::Token::array_start) {
        if (row_reader.next() == ebus::detail::JsonReader::Token::string) {
          uint8_t kid = StringPool::instance().intern(row_reader.value());
          if (kid != 0) removeFieldOverrides(kid);
        }
      }
      row_reader.reset();
      insertCommand(Command::fromTabular(row_reader));
      loaded_count++;
    } else if (token == ebus::detail::JsonReader::Token::object_start) {
      row_reader.reset();
      std::string_view eval_error = Command::evaluate(row_reader);
      if (eval_error.empty()) {
        // Pre-extract key to drop stale overrides before fromJson()
        // repopulates.
        row_reader.reset();
        if (row_reader.next() ==
            ebus::detail::JsonReader::Token::object_start) {
          row_reader.findKey("key");
          if (row_reader.next() == ebus::detail::JsonReader::Token::string) {
            uint8_t kid = StringPool::instance().intern(row_reader.value());
            if (kid != 0) removeFieldOverrides(kid);
          }
        }
        row_reader.reset();
        insertCommand(Command::fromJson(row_reader));
        loaded_count++;
      } else {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf),
                 "CommandManager: Command validation failed: %.*s",
                 static_cast<int>(eval_error.length()), eval_error.data());
        logger.error(err_buf);
      }
    }
  }

  char res_buf[64];
  snprintf(res_buf, sizeof(res_buf),
           "CommandManager: Deserialized %u commands.",
           static_cast<unsigned>(loaded_count));
  logger.info(res_buf);
}

#endif
