#ifndef VBAM_CORE_GBA_GBASOUND_H_
#define VBAM_CORE_GBA_GBASOUND_H_

#include <cstdint>

#if !defined(__LIBRETRO__)
#include <zlib.h>
#endif  // !defined(__LIBRETRO__)

// Sound emulation setup/options and GBA sound emulation

//// Setup/options (these affect GBA and GB sound)

// Initializes sound and returns true if successful. Sets sound quality to
// current value in soundQuality global.
bool soundInit();

// sets the Sound throttle
void soundSetThrottle(unsigned short throttle);

// Manages sound volume, where 1.0 is normal
void soundSetVolume(float);
float soundGetVolume();

// Manages muting bitmask. The bits control the following channels:
// 0x001 Pulse 1
// 0x002 Pulse 2
// 0x004 Wave
// 0x008 Noise
// 0x100 PCM 1
// 0x200 PCM 2
void soundSetEnable(int mask);
int soundGetEnable();

// Pauses/resumes system sound output
void soundPause();
void soundResume();
extern bool soundPaused; // current paused state

// Cleans up sound. Afterwards, soundInit() can be called again.
void soundShutdown();

//// GBA sound options

long soundGetSampleRate();
void soundSetSampleRate(long sampleRate);

// Sound settings
extern bool g_gbaSoundInterpolation; // 1 if PCM should have low-pass filtering
extern float soundFiltering; // 0.0 = none, 1.0 = max

//// GBA sound emulation

// GBA sound registers
#define SGCNT0_H  0x82
#define SOUNDBIAS 0x88
#define FIFOA_L   0xa0
#define FIFOA_H   0xa2
#define FIFOB_L   0xa4
#define FIFOB_H   0xa6

// Resets emulated sound hardware
void soundReset();

// Emulates write to sound hardware
void soundEvent8(uint32_t addr, uint8_t data);
void soundEvent16(uint32_t addr, uint16_t data); // TODO: error-prone to overload like this

// Notifies emulator that a timer has overflowed
void soundTimerOverflow(int which);

// Notifies emulator that PCM rate may have changed
void interp_rate();

// Notifies emulator that SOUND_CLOCK_TICKS clocks have passed
void psoundTickfn();
extern int SOUND_CLOCK_TICKS; // Number of 16.8 MHz clocks between calls to soundTick()

// 2018-12-10 - counts up from 0 since last psoundTickfn() was called
extern int soundTicks;

// Saves/loads emulator state
#ifdef __LIBRETRO__
void soundSaveGame(uint8_t*&);
void soundReadGame(const uint8_t*& in);
#else
void soundSaveGame(gzFile);
void soundReadGame(gzFile, int version);
#endif

class Multi_Buffer;

void flush_samples(Multi_Buffer* buffer);

void remake_stereo_buffer();

#endif  // VBAM_CORE_GBA_GBASOUND_H_
