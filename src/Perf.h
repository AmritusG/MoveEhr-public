#pragma once
// W29: per-pass timing for the PERF log line. Only while the instance's Debug Log is on; off = no clock read, no GL call.
// CPU: the wall clock at every pass boundary (the passes' submission cost on the render thread).
// GPU: Apple's GL returns 0 for every GL_TIMESTAMP (glQueryCounter and glGetInteger64v, measured W29) and GL_TIME_ELAPSED
// cannot nest inside a host's own query, so there is no per-pass GPU clock in Arena. Offline, LOADEHR_PERF_FINISH=1 makes
// every boundary a glFinish: each pass's wall time then holds its GPU time too (serialised: a ranking, not a frame time).
// Arena's real frame GPU time comes from a system-wide Metal System Trace (tools/w29_perf.py trace).
#include <chrono>

namespace mehr
{
struct PerfProbe
{
	enum Pass
	{
		Prep,      // input copy, face readback, light matrices, LOD / shard / bounce / letters / dust / swarm updates
		Shadows,   // volume lights' shadow maps
		KeyShadow, // the floor's key shadow
		Projector, // projector shadow + picture
		Caustics,  // caustic map
		Main,      // clear, prewarm, the mask, copies, ghosts, dust, liquid, MSAA resolve
		Floor,     // mirror floor, silhouette blur, trail echoes
		Sim,       // scene depth copy + smoke sim substeps
		Volume,    // the volume march (+ Trip post)
		Composite, // heat-haze mask + the composite into the host / post target
		Light,     // motes, mirror LEDs, fireflies / bolts
		Post,      // bloom / flares
		Count
	};
	static const char* Name( int p );

	bool on = false;
	void Begin( bool enabled ); // a frame starts
	void Mark( Pass p );        // pass p starts (and the previous one ends)
	void End();                 // the frame ends

	// accumulated since the last Take (ms sums over `frames`)
	double ms[ Count ] = {}, total = 0;
	int frames         = 0;
	void Take(); // clears the sums

private:
	std::chrono::steady_clock::time_point t0, tLast;
	int lastPass = -1;
	bool finish  = false;
	void Lap();
};
}
