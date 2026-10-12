#include "log_storage_fs.h"

#include <LittleFS.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "diagnostics.h"

using lock::log::kEntrySize;

static_assert(LogStorageFs::kSlots == cfg::kLogCapacity, "log capacity mismatch");

namespace {

constexpr size_t kFileSize = LogStorageFs::kSlots * kEntrySize;

// VFS path of the log file: mount point + file path.
char g_path[64];

// Writes all `len` bytes at `offset`. False on any error or short write.
bool writeAt(int fd, size_t offset, const uint8_t *buf, size_t len) {
  if (lseek(fd, static_cast<off_t>(offset), SEEK_SET) != static_cast<off_t>(offset)) return false;
  while (len > 0) {
    ssize_t n = ::write(fd, buf, len);
    if (n <= 0) return false;
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool readAll(int fd, uint8_t *buf, size_t len) {
  if (lseek(fd, 0, SEEK_SET) != 0) return false;
  while (len > 0) {
    ssize_t n = ::read(fd, buf, len);
    if (n <= 0) return false;
    buf += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool LogStorageFs::begin() {
  memset(mirror_, 0xFF, sizeof(mirror_));
  memset(dirty_, 0, sizeof(dirty_));
  memset(rewritten_, 0, sizeof(rewritten_));
  snprintf(g_path, sizeof(g_path), "%s%s", cfg::kLogFsBasePath, cfg::kLogFilePath);

  // formatOnFail = true: an unmountable partition is reformatted (log lost).
  if (!LittleFS.begin(true, cfg::kLogFsBasePath, 2, cfg::kLogFsLabel)) {
    diag::logf(diag::kLog, "LittleFS mount/format failed; log is RAM-only this boot");
    return false;
  }

  bool ok = openFile();
  if (!ok) ok = createFile();
  if (!ok) diag::logf(diag::kLog, "log file unavailable; log is RAM-only this boot");
  return ok;
}

bool LogStorageFs::openFile() {
  struct stat st;
  if (stat(g_path, &st) != 0) return false;  // missing: create it
  bool ok = false;
  if (static_cast<size_t>(st.st_size) == kFileSize) {
    fd_ = open(g_path, O_RDWR);
    ok = fd_ >= 0 && readAll(fd_, mirror_, kFileSize);
  }
  if (!ok) {
    diag::logf(diag::kLog, "log file unreadable or wrong size; recreating");
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    memset(mirror_, 0xFF, sizeof(mirror_));
  }
  return ok;
}

bool LogStorageFs::createFile() {
  int fd = open(g_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  // Pre-size with erased-looking slots so every slot write is in place.
  if (!writeAt(fd, 0, mirror_, kFileSize) || fsync(fd) != 0) {
    close(fd);
    return false;
  }
  fd_ = fd;
  return true;
}

bool LogStorageFs::read(size_t slot, uint8_t out[kEntrySize]) {
  if (slot >= kSlots) return false;
  portENTER_CRITICAL(&mux_);
  memcpy(out, mirror_ + slot * kEntrySize, kEntrySize);
  portEXIT_CRITICAL(&mux_);
  // Unwritten slots are all 0xFF (or 0x00 if something zero-filled them).
  bool allFF = true;
  bool all00 = true;
  for (size_t i = 0; i < kEntrySize; i++) {
    if (out[i] != 0xFF) allFF = false;
    if (out[i] != 0x00) all00 = false;
  }
  return !(allFF || all00);
}

bool LogStorageFs::write(size_t slot, const uint8_t in[kEntrySize]) {
  if (slot >= kSlots) return false;
  const uint32_t bit = 1UL << (slot % 32);
  portENTER_CRITICAL(&mux_);
  memcpy(mirror_ + slot * kEntrySize, in, kEntrySize);
  dirty_[slot / 32] |= bit;
  rewritten_[slot / 32] |= bit;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool LogStorageFs::flush() {
  // RAM-only: nothing ever reaches flash, so never report success (entries
  // must not be marked durable).
  if (fd_ < 0) return false;

  uint32_t pending[kWords];
  bool any = false;
  portENTER_CRITICAL(&mux_);
  for (size_t w = 0; w < kWords; w++) {
    pending[w] = dirty_[w];
    rewritten_[w] = 0;
    any = any || pending[w] != 0;
  }
  portEXIT_CRITICAL(&mux_);
  if (!any) return true;

  // Dirty bits are cleared only after fsync succeeds, and only for slots
  // whose write succeeded and that were not rewritten in the meantime.
  bool ok = true;
  uint32_t written[kWords] = {};
  uint8_t buf[kEntrySize];
  for (size_t slot = 0; slot < kSlots; slot++) {
    const uint32_t bit = 1UL << (slot % 32);
    if (!(pending[slot / 32] & bit)) continue;
    portENTER_CRITICAL(&mux_);
    memcpy(buf, mirror_ + slot * kEntrySize, kEntrySize);
    portEXIT_CRITICAL(&mux_);
    if (writeAt(fd_, slot * kEntrySize, buf, kEntrySize)) {
      written[slot / 32] |= bit;
    } else {
      ok = false;
    }
  }

  // One sync for the whole batch: this is the slow part (block rewrite).
  // lfs_file_sync's error comes back here, unlike File::flush().
  // (control.cpp reports failures; flush runs every loop, so no log here.)
  if (fsync(fd_) != 0) return false;

  portENTER_CRITICAL(&mux_);
  for (size_t w = 0; w < kWords; w++) dirty_[w] &= ~(written[w] & ~rewritten_[w]);
  portEXIT_CRITICAL(&mux_);
  return ok;
}
