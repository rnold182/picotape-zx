#pragma once
#include <Arduino.h>

// Records the Spectrum's SAVE signal on GP26 into a .TAP file.
// Core 1 samples the ADC (via DMA) and decodes; core 0 writes finished blocks.
namespace recorder {

enum class Phase : uint8_t { Idle, Pilot, Data };

// --- core 0 ---
bool start();               // open a temp file and start listening
void service();             // call from loop(): writes finished blocks
String stop(bool keep);     // stop; keep=true saves and returns the file name ("" if nothing recorded)
bool active();

Phase phase();
uint8_t level();            // peak-to-peak input in ADC counts (255 = 3.3 V)
uint32_t liveBytes();       // bytes received so far in the current block
int blockCount();
int errorCount();           // blocks with a bad checksum
String lastBlock();         // description of the last finished block
String tapeName();          // name from the first header, "" if none yet
bool overrun();             // samples were lost (should never happen)
uint32_t elapsedMs();

// --- core 1 ---
void engineBegin();
void engineLoop();

}  // namespace recorder
