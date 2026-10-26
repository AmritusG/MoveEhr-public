#include "Perf.h"
#include "GlCount.h" // MoveEhr W6: <OpenGL/gl3.h> + the GL call counters
#include <cstdlib>

namespace mehr
{
const char* PerfProbe::Name( int p )
{
	static const char* n[ Count ] = { "prep", "shadow", "keysh", "proj", "caus", "main", "floor", "sim", "vol", "comp", "light", "post" };
	return p >= 0 && p < Count ? n[ p ] : "?";
}

void PerfProbe::Begin( bool enabled )
{
	on = enabled;
	if( !on )
		return;
	static const bool fin = getenv( "MOVEEHR_PERF_FINISH" ) != nullptr;
	finish = fin;
	if( finish )
		glFinish(); // the host's own work so far is not ours
	lastPass = -1;
	t0 = tLast = std::chrono::steady_clock::now();
}

void PerfProbe::Lap()
{
	if( finish )
		glFinish();
	const auto now = std::chrono::steady_clock::now();
	if( lastPass >= 0 )
		ms[ lastPass ] += std::chrono::duration< double, std::milli >( now - tLast ).count();
	tLast = now;
}

void PerfProbe::Mark( Pass p )
{
	if( !on )
		return;
	Lap();
	lastPass = p;
}

void PerfProbe::End()
{
	if( !on )
		return;
	Lap();
	total += std::chrono::duration< double, std::milli >( tLast - t0 ).count();
	++frames;
	lastPass = -1;
}

void PerfProbe::Take()
{
	for( double& m : ms )
		m = 0;
	total  = 0;
	frames = 0;
}
}
