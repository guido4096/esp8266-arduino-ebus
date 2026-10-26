#pragma once

#include "app/command.hpp"
#include "app/ebus_accessor.hpp"
#include "system/logger.hpp"
#include <cstdio>

// Passive read patterns may be incomplete telegrams and must never be sent.
inline bool requestCommandRead(Command& command) {
  if (!command.getActive() || command.getReadCmd().empty()) return false;
  const auto id = getEbusController().enqueue(app::priority::send,
                                             command.getReadCmd());
  command.setPendingReadId(id);
  if (command.hasWriteCmd()) {
    char message[160];
    snprintf(message, sizeof(message), "[WRITE] key=%.*s event=%s",
             static_cast<int>(command.getKey().size()), command.getKey().data(),
             id != 0 ? "read_requested" : "read_queue_rejected");
    logger.info(message, false, id);
  }
  return id != 0;
}
