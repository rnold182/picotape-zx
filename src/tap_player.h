#pragma once
#include <Arduino.h>
#include <vector>

struct TapBlock {
    uint32_t offset;  // file offset of the 2-byte length field
    uint16_t length;  // flag + data + checksum
    uint8_t flag;     // 0x00 = header, 0xFF = data
    uint8_t type;     // header only: 0 Program, 1 Num array, 2 Char array, 3 Bytes
    char name[11];    // header only
};

// Plays a .TAP file as standard ROM-loader audio.
// Core 0 reads the file and feeds a ring buffer; core 1 turns bytes into pulses.
namespace player {

enum class State : uint8_t { Idle, Playing, Paused, Done };

// --- core 0 ---
// Read a TAP file's block list without playing it.
bool scan(const char* path, std::vector<TapBlock>& out);
bool open(const char* path, int startBlock = 0);  // scan blocks and start playing
bool seek(int block);         // restart the open (or just finished) file at a block
void togglePause();           // resuming inside a block restarts that block
void stop();                  // abort playback and close the file
void service();               // call from loop(): refills the buffer, detects end
bool busy();                  // true while a file is open or the engine runs
State state();
int currentBlock();           // -1 before the first block
uint32_t bytesPlayed();
uint32_t fileSize();
const std::vector<TapBlock>& blocks();
String describe(const TapBlock& b);

// --- core 1 ---
void engineBegin();
void engineLoop();

}  // namespace player
