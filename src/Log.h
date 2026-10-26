#pragma once
// Thread-safe file log: ~/Library/Logs/LoadEhr/LoadEhr.log (also mirrored to stderr).
// Safe to call from the loader worker and the render thread.
namespace mehr
{
void Log( const char* fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
const char* LogPath();
// Chatty lines (every param value, per-drag rebuilds, periodic face stats) are written only while some
// instance has "Debug Log" on; the log is for loads, swaps, eye finding and errors.
void LogVerboseRef( bool on ); // an instance turned Debug Log on (true) or off (false)
bool LogVerbose();
}
