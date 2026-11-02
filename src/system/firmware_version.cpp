#include "system/firmware_version.hpp"

#ifndef AUTO_VERSION
#include "system/firmware_version_gen.hpp"
#endif

const char* firmwareVersion() {
#ifdef AUTO_VERSION
  // Host tests and external builds may still provide an explicit version.
  return AUTO_VERSION;
#else
  return firmware_revision;
#endif
}
