#include "ota.h"
#include "fw_verify.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_ota_ops.h>
#include <SHA256.h> // rweather/Crypto — one crypto lib, avoids mbedtls version drift

static bool g_pending = false;
static bool g_running = false;
static String g_url, g_sigUrl, g_version;

bool otaRequest(const String &url, const String &sigUrl, const String &version) {
  if (g_pending || g_running) return false;
  g_url = url; g_sigUrl = sigUrl; g_version = version;
  g_pending = true;
  return true;
}

bool otaPending() { return g_pending; }

static bool fetchSig(const String &url, uint8_t out[64]) {
  HTTPClient http;
  if (!http.begin(url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK && http.getSize() == 64) {
    ok = http.getStreamPtr()->readBytes(out, 64) == 64;
  }
  http.end();
  return ok;
}

void otaRun() {
  g_pending = false;
  g_running = true;
  Serial.printf("OTA: starting update to %s\n", g_version.c_str());

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  esp_ota_handle_t handle = 0;
  if (!target || esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
    Serial.println("OTA: esp_ota_begin failed"); g_running = false; return;
  }

  SHA256 sha;
  sha.reset();

  HTTPClient http;
  if (!http.begin(g_url) || http.GET() != HTTP_CODE_OK) {
    Serial.println("OTA: image GET failed"); esp_ota_abort(handle); http.end(); g_running = false; return;
  }
  int total = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  bool failed = false;
  while (http.connected() && (total < 0 || written < total)) {
    size_t avail = stream->available();
    if (avail) {
      int r = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
      if (esp_ota_write(handle, buf, r) != ESP_OK) { failed = true; break; }
      sha.update(buf, r);
      written += r;
    } else {
      delay(1);
    }
  }
  http.end();
  if (failed || (total > 0 && written != total)) {
    Serial.printf("OTA: write failed (%d/%d)\n", written, total);
    esp_ota_abort(handle); g_running = false; return;
  }
  if (esp_ota_end(handle) != ESP_OK) {
    Serial.println("OTA: esp_ota_end failed (bad image)"); g_running = false; return;
  }

  uint8_t digest[32];
  sha.finalize(digest, sizeof(digest));

  uint8_t sig[64];
  if (!fetchSig(g_sigUrl, sig)) {
    Serial.println("OTA: signature fetch failed"); g_running = false; return;
  }
  if (!fwVerify(digest, sig)) {
    Serial.println("OTA: signature INVALID — refusing to activate"); g_running = false; return;
  }
  if (esp_ota_set_boot_partition(target) != ESP_OK) {
    Serial.println("OTA: set_boot_partition failed"); g_running = false; return;
  }
  Serial.printf("OTA: verified & staged %d bytes; rebooting\n", written);
  delay(200);
  esp_restart();
}
