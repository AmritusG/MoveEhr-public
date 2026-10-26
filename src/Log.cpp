#include "Log.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>

namespace mehr
{
namespace
{
std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_path;
std::atomic< int > g_verbose{ 0 };

FILE* OpenLog()
{
	if( g_file )
		return g_file;
	const char* home = getenv( "HOME" );
	std::string dir  = std::string( home ? home : "/tmp" ) + "/Library/Logs/MoveEhr";
	mkdir( dir.c_str(), 0755 );
	g_path = dir + "/MoveEhr.log";
	g_file = fopen( g_path.c_str(), "a" );
	return g_file;
}
}

const char* LogPath()
{
	std::lock_guard< std::mutex > lock( g_mutex );
	OpenLog();
	return g_path.c_str();
}

void LogVerboseRef( bool on )
{
	g_verbose += on ? 1 : -1;
}
bool LogVerbose()
{
	return g_verbose.load() > 0;
}

void Log( const char* fmt, ... )
{
	char msg[ 2048 ];
	va_list args;
	va_start( args, fmt );
	vsnprintf( msg, sizeof( msg ), fmt, args );
	va_end( args );

	timeval tv;
	gettimeofday( &tv, nullptr );
	tm local;
	localtime_r( &tv.tv_sec, &local );
	char stamp[ 32 ];
	strftime( stamp, sizeof( stamp ), "%Y-%m-%d %H:%M:%S", &local );

	std::lock_guard< std::mutex > lock( g_mutex );
	if( FILE* f = OpenLog() )
	{
		fprintf( f, "%s.%03d %s\n", stamp, int( tv.tv_usec / 1000 ), msg );
		fflush( f );
	}
	if( getenv( "MOVEEHR_LOG_STDERR" ) )
		fprintf( stderr, "[MoveEhr] %s\n", msg );
}
}
