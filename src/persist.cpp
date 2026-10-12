#include "persist.h"

#include <Preferences.h>

namespace persist {
namespace {

constexpr const char *kNamespace = "lock";
constexpr const char *kKeyBoots = "boots";

Preferences g_prefs;
bool g_open = false;

}  // namespace

bool begin() {
  g_open = g_prefs.begin(kNamespace, false);
  return g_open;
}

uint32_t incrementBootCount() {
  if (!g_open) return 0;
  uint32_t n = g_prefs.getUInt(kKeyBoots, 0) + 1;
  g_prefs.putUInt(kKeyBoots, n);
  return n;
}

}  // namespace persist
