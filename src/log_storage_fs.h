#pragma once
// lock::log::LogStorage on LittleFS (partition "spiffs"). The 20 KB NVS
// partition is too small for 256 entries, so the log lives in a filesystem.
//
// One fixed 4 KB file of 256 x 16-byte slots, mirrored in RAM:
//   - read() serves from RAM, so log reads over BLE never touch flash.
//   - write() updates RAM and marks the slot dirty; flush() writes dirty
//     slots to flash. The control task calls flush() right after appending,
//     outside the EventLog mutex, so a BLE log read never waits on flash.
//   - A slot stays dirty until its data is on flash: every write()/lseek()
//     result is checked, then fsync() (lfs_file_sync) must succeed. The file
//     is accessed through a raw VFS fd rather than fs::File because
//     File::flush() discards the fflush/fsync results.
// Unwritten slots are 0xFF-filled and read() reports them as empty.

#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

#include "event_log.h"

class LogStorageFs : public lock::log::LogStorage {
 public:
  static constexpr size_t kSlots = 256;

  // Mounts LittleFS (formatting if the mount fails) and loads the file into
  // RAM, creating it if missing or the wrong size.
  bool begin();

  // Always kSlots: if the filesystem is unavailable the log still works from
  // RAM for this boot, and flush() reports the failure.
  size_t capacity() const override { return kSlots; }
  bool read(size_t slot, uint8_t out[lock::log::kEntrySize]) override;
  bool write(size_t slot, const uint8_t in[lock::log::kEntrySize]) override;

  // Writes dirty slots to flash and syncs. Single caller (the control task).
  // Returns true only if every dirty slot reached flash (nothing dirty also
  // counts). On any failure, or with no file (RAM-only), returns false and
  // the unsynced slots stay dirty for retry.
  bool flush();

 private:
  bool openFile();
  bool createFile();

  static constexpr size_t kWords = kSlots / 32;

  uint8_t mirror_[kSlots * lock::log::kEntrySize];
  uint32_t dirty_[kWords] = {};
  // Slots written since the current flush() snapshotted dirty_: their new
  // contents may not be in this flush, so they keep their dirty bit.
  uint32_t rewritten_[kWords] = {};
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  int fd_ = -1;
};
