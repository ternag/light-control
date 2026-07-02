#pragma once
#include <Arduino.h>

// Queue an update job. Returns false if one is already pending/running.
bool otaRequest(const String &url, const String &sigUrl, const String &version);
// True if a job is queued and not yet run.
bool otaPending();
// Run the queued job to completion (blocking): download -> verify -> activate ->
// reboot on success; on any failure, log and stay on the current image.
void otaRun();
