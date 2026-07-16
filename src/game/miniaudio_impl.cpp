// miniaudio's implementation, alone in a translation unit of its own.
//
// Keep this file a single #define and #include. CMake compiles it with warnings
// off, since the header is third-party and not ours to fix, so anything added
// alongside it would be compiled unchecked too.
//
// The MA_NO_* switches that trim the build (encoding, the FLAC and MP3 decoders,
// the waveform generators) are set in CMakeLists on the whole target rather than
// here, so that every file which includes miniaudio.h agrees with this one about
// what is in it.

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
