// LoadEhr offline harness: a headless CGL 4.1 core host that dlopens an FFGL bundle, drives it through
// gPlugMain exactly like a host would, renders frames to PNG and reports frame times.
//
// usage: loadehr_harness <script.txt>
// Script commands (one per line, # comments):
//   bundle <path to .bundle>          load plugin (must come first)
//   size <w> <h>                      render size (default 1920 1080), before `instance`
//   input <png> | input bars          FX input texture (default: none)
//   input_seq <dir>                   FX input from every .png in <dir> (sorted), one per rendered frame, looping
//   instance                          create the plugin instance
//   params                            print the parameter table
//   text <index> <string...>          set a text/file parameter
//   set <index|name> <value>          set a float parameter (raw value in the param's range); a name is the
//                                     parameter's base name, spaces allowed ("set Spin Y 30")
//   setnext <name> <a> <b> <c>        (W29) the three params after <name>: a colour picker's SAT, BRI, ALPHA
//   pulse <index|name>                an EVENT press: set 1, render one frame, set 0
//   get <index|name>                  print the parameter's current value (text params print the string)
//   compare <a.png> <b.png>           mean / max abs RGB difference and the count of pixels differing > 2
//   delta <label> <a.png> <b.png>     over the pixels that changed (any channel > 4): mean signed RGB of b - a
//                                     and mean RGB of b, 0..255 (what a light or a colour change adds)
//   bpm <bpm>                         beat clock for barPhase (default 120)
//   fft <level>                       synthetic FFT: all bins pulse with the beat at <level> (0 = silent)
//   text <name> <string>              a TEXT / FILE param; "\n" in the string = a line break
//   mark                              the next wait_loaded waits for a swap after this point
//   wait_loaded [timeout_s]           render at ~60 Hz real time until the plugin logs the swap to (or the
//                                     failure of) the model requested by the last `text` command
//   wait_slots <n> [timeout_s]        (LoadEhr W1) the same until <n> swaps / failures since the last `mark`
//   wait_log <n> <timeout_s> <needle>  (MoveEhr W5b) render at ~60 Hz until <n> log lines since the last `mark` contain <needle>
//   log_offset <label>                (MoveEhr W5b) print the log's size: a test counts only the lines after it
//                                     (set `mark` before the `text` commands of the slots)
//   frames <n>                        render n frames advancing 1/60 s of host time each
//   advance <seconds> [zero]          fast-forward every plugin clock by <seconds> of show time without rendering
//                                     (host time and the beat do not move; LoadEhrTestAdvance in the bundle);
//                                     `zero` first sets every integrated phase to 0, so wrap times are exact
//   realtime <seconds>                render at ~60 Hz real time (for load / switch tests), frame stats
//   composite arena|straight|black    how `shot` flattens the frame (W23; default arena):
//                                     arena    = premultiplied over a textured background (Arena, FFGL API 2.3)
//                                     straight = the frame read as STRAIGHT alpha over the same background (Arena's
//                                                reading of a SOURCE plugin below API 2.3: rgb x alpha, then over)
//                                     black    = premultiplied over the old dark checker (the W1 regression references)
//   shot <file.png>                   save the last frame flattened by the composite mode
//   stats_reset / stats <label>       frame-time statistics over the frames since stats_reset
//   diffs <n>                         render n frames (1/60 s each), print mean abs pixel change vs previous frame
//   means <n>                         render n frames, print per frame the mean R G B and the mean of max(R,G,B)
//                                     (HSB brightness) over the covered pixels (alpha > 0), 0..255
//   save_state                        read every param back as a host does on save (FF_GET_PARAMETER; text as text)
//   restore_state                     write the saved values into the current instance in index order, as a host
//                                     does on composition load (use after delete + instance); `restore_state
//                                     reverse` writes them last index first
//   delete                            destroy the instance
#include "FFGL.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <map>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef FFMixed ( *PlugMainFn )( FFUInt32, FFMixed, FFInstanceID );

namespace
{
PlugMainFn gPlugMain  = nullptr;
void ( *gTestAdvance )( void*, double, int ) = nullptr;
FFInstanceID inst    = nullptr;
int W = 1920, H = 1080;
GLuint hostFBO = 0, hostTex = 0, inputTex = 0;
int inputW = 0, inputH = 0;
double hostTime = 0.0;
double hostUnit = 1.0; // what FF_SET_TIME carries per second: 1 (seconds) or 1000 (hostunit ms, as Arena 7.28)
float bpm = 120.0f, fftLevel = 0.0f;
unsigned numParams = 0;
std::vector< double > cpuMs, gpuMs;
GLuint timerQuery = 0;
size_t logMark    = 0; // LoadEhr log size when the last `text` command was issued
bool markHeld     = false; // a `mark` holds logMark until the next wait_loaded / wait_slots
enum class Composite { Arena, Straight, Black } composite = Composite::Arena;

std::string LogPath()
{
	return std::string( getenv( "HOME" ) ) + "/Library/Logs/MoveEhr/MoveEhr.log"; // MoveEhr W1: the plugin logs here (LoadEhr: Logs/LoadEhr/LoadEhr.log);
}
size_t LogSize()
{
	std::ifstream f( LogPath(), std::ios::binary | std::ios::ate );
	return f ? (size_t)f.tellg() : 0;
}
// returns the first line after `mark` containing any of the needles, or ""
std::string LogFind( size_t mark, std::initializer_list< const char* > needles )
{
	std::ifstream f( LogPath(), std::ios::binary );
	if( !f )
		return {};
	f.seekg( (std::streamoff)mark );
	std::string line;
	while( std::getline( f, line ) )
		for( const char* n : needles )
			if( line.find( n ) != std::string::npos )
				return line;
	return {};
}

FFMixed Call( FFUInt32 code, FFMixed in )
{
	return gPlugMain( code, in, inst );
}
FFMixed U( FFUInt32 v )
{
	FFMixed m;
	m.UIntValue = v;
	return m;
}
FFMixed Ptr( const void* p )
{
	FFMixed m;
	m.PointerValue = const_cast< void* >( p );
	return m;
}

void SetFloat( unsigned idx, float v )
{
	SetParameterStruct s;
	s.ParameterNumber = idx;
	std::memcpy( &s.NewParameterValue.UIntValue, &v, 4 );
	Call( FF_SET_PARAMETER, Ptr( &s ) );
}
void SetText( unsigned idx, const std::string& t )
{
	SetParameterStruct s;
	s.ParameterNumber                = idx;
	s.NewParameterValue.PointerValue = (void*)t.c_str();
	Call( FF_SET_PARAMETER, Ptr( &s ) );
}

void MakeInputBars()
{
	inputW = 1280, inputH = 720;
	std::vector< uint8_t > px( inputW * inputH * 4 );
	const uint8_t bars[ 8 ][ 3 ] = { { 235, 235, 235 }, { 235, 235, 16 }, { 16, 235, 235 }, { 16, 235, 16 }, { 235, 16, 235 }, { 235, 16, 16 }, { 16, 16, 235 }, { 16, 16, 16 } };
	for( int y = 0; y < inputH; ++y )
		for( int x = 0; x < inputW; ++x )
		{
			uint8_t* p = &px[ ( y * inputW + x ) * 4 ];
			int b      = x * 8 / inputW;
			// orientation marker: the top quarter of the image is a checker; an arrow-ish wedge on the left
			bool top   = y > inputH * 3 / 4; // GL rows are bottom-up
			bool check = ( ( x / 40 ) + ( y / 40 ) ) & 1;
			bool wedge = x < inputW / 6 && std::abs( y - inputH / 2 ) < ( inputW / 6 - x ) / 2;
			for( int k = 0; k < 3; ++k )
				p[ k ] = wedge ? ( k == 0 ? 255 : 30 ) : top ? ( check ? 250 : 20 ) : bars[ b ][ k ];
			p[ 3 ] = 255;
		}
	glGenTextures( 1, &inputTex );
	glBindTexture( GL_TEXTURE_2D, inputTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, inputW, inputH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

void LoadInputPng( const std::string& path )
{
	int c = 0;
	stbi_set_flip_vertically_on_load( 1 );
	uint8_t* px = stbi_load( path.c_str(), &inputW, &inputH, &c, 4 );
	stbi_set_flip_vertically_on_load( 0 );
	if( !px )
	{
		fprintf( stderr, "cannot load input %s\n", path.c_str() );
		return;
	}
	glGenTextures( 1, &inputTex );
	glBindTexture( GL_TEXTURE_2D, inputTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, inputW, inputH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glBindTexture( GL_TEXTURE_2D, 0 );
	stbi_image_free( px );
}

int ParamIndex( const std::string& key );

struct SavedParam
{
	bool text = false;
	float f   = 0;
	std::string s;
};
std::vector< SavedParam > savedState;
std::map< std::string, std::vector< uint8_t > > refFrames; // W10: ref_save / ref_cmp

// input_seq: preloaded frames (RGBA, bottom-up), advanced once per rendered frame
std::vector< std::vector< uint8_t > > seqFrames;
size_t seqIndex = 0;

void RenderFrame( double dt )
{
	// MoveEhr W2: MOVEEHR_HOST_SAMPLERS=1 leaves a LINEAR sampler object on every texture unit, as a host may (Arena); a plugin that
	// texelFetches integer textures must unbind them (they make an integer texture incomplete: it reads 0)
	static const bool hostSamplers = getenv( "MOVEEHR_HOST_SAMPLERS" ) != nullptr;
	if( hostSamplers )
	{
		static GLuint smp = 0;
		if( !smp )
		{
			glGenSamplers( 1, &smp );
			glSamplerParameteri( smp, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glSamplerParameteri( smp, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		}
		GLint units = 0;
		glGetIntegerv( GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units );
		for( GLint u = 0; u < units; ++u )
			glBindSampler( GLuint( u ), smp );
	}
	if( !seqFrames.empty() )
	{
		glBindTexture( GL_TEXTURE_2D, inputTex );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, inputW, inputH, GL_RGBA, GL_UNSIGNED_BYTE, seqFrames[ seqIndex ].data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		seqIndex = ( seqIndex + 1 ) % seqFrames.size();
	}
	hostTime += dt;
	double sent = hostTime * hostUnit; // W28: hostunit ms = Arena 7.28's clock (milliseconds)
	Call( FF_SET_TIME, Ptr( &sent ) );
	float barPhase = (float)std::fmod( hostTime * bpm / 60.0 / 4.0, 1.0 );
	SetBeatinfoStruct beat{ bpm, barPhase };
	Call( FF_SET_BEATINFO, Ptr( &beat ) );
	if( numParams > 0 )
	{
		// LoadEhr W1: looked up per frame (a script that loads a second bundle has the FFT at another index; a cached index past the
		// new bundle's last param answered FF_FAIL = 0xFFFFFFFF elements: a 4-billion-step loop per frame)
		const int fftIdx = ParamIndex( "Audio FFT" );
		float beatPhase  = barPhase * 4.0f - std::floor( barPhase * 4.0f );
		unsigned nElems  = fftIdx < 0 ? 0 : Call( FF_GET_NUM_PARAMETER_ELEMENTS, U( fftIdx ) ).UIntValue;
		if( nElems > 4096 )
			nElems = 0;
		for( unsigned e = 0; e < nElems; ++e )
		{
			float v = fftLevel * std::exp( -beatPhase * 4.0f ) * ( 1.0f - float( e ) / nElems );
			SetParameterElementValueStruct s;
			s.ParameterNumber = fftIdx;
			s.ElementNumber   = e;
			std::memcpy( &s.NewParameterValue.UIntValue, &v, 4 );
			Call( FF_SET_PARAMETER_ELEMENT_VALUE, Ptr( &s ) );
		}
	}

	FFGLTextureStruct in{ (FFUInt32)inputW, (FFUInt32)inputH, (FFUInt32)inputW, (FFUInt32)inputH, inputTex };
	FFGLTextureStruct* ins[ 1 ] = { &in };
	ProcessOpenGLStruct pgl;
	pgl.numInputTextures = inputTex ? 1 : 0;
	pgl.inputTextures    = inputTex ? ins : nullptr;
	pgl.HostFBO          = hostFBO;

	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glViewport( 0, 0, W, H );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );
	auto t0 = std::chrono::steady_clock::now();
	glBeginQuery( GL_TIME_ELAPSED, timerQuery );
	Call( FF_PROCESS_OPENGL, Ptr( &pgl ) );
	glEndQuery( GL_TIME_ELAPSED );
	GLuint64 ns = 0;
	glGetQueryObjectui64v( timerQuery, GL_QUERY_RESULT, &ns ); // waits for the GPU
	cpuMs.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
	gpuMs.push_back( ns / 1e6 );
}

std::vector< uint8_t > ReadFrame()
{
	std::vector< uint8_t > px( W * H * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glReadPixels( 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
	return px;
}

void Shot( const std::string& path )
{
	std::vector< uint8_t > px( W * H * 4 ), out( W * H * 3 );
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glReadPixels( 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			const uint8_t* s = &px[ ( ( H - 1 - y ) * W + x ) * 4 ];
			uint8_t* d       = &out[ ( y * W + x ) * 3 ];
			float a          = s[ 3 ] / 255.0f;
			if( composite == Composite::Black )
			{
				const uint8_t bg = ( ( x / 32 + y / 32 ) & 1 ) ? 34 : 26; // dark checker shows transparency
				for( int k = 0; k < 3; ++k )
					d[ k ] = (uint8_t)std::min( 255.0f, s[ k ] + bg * ( 1.0f - a ) ); // premultiplied over bg
				continue;
			}
			// a mid-dark colour ramp with a 64 px checker: what a lower Arena layer could look like. Light added
			// in the air brightens it; coverage hides it.
			const float fx = float( x ) / W, fy = float( y ) / H, ck = ( ( x / 64 + y / 64 ) & 1 ) ? 1.0f : 0.72f;
			const float bg[ 3 ] = { ( 40.0f + 70.0f * fx ) * ck, ( 45.0f + 45.0f * fy ) * ck, ( 100.0f - 50.0f * fx ) * ck };
			const float m = composite == Composite::Straight ? a : 1.0f; // straight: the host multiplies rgb by alpha
			for( int k = 0; k < 3; ++k )
				d[ k ] = (uint8_t)std::lround( std::min( 255.0f, s[ k ] * m + bg[ k ] * ( 1.0f - a ) ) );
		}
	stbi_write_png( path.c_str(), W, H, 3, out.data(), W * 3 );
	printf( "shot %s\n", path.c_str() );
}

// the host frame as it is: premultiplied RGBA, rows top-first (W15: trail measurements need the alpha)
void ShotRGBA( const std::string& path )
{
	std::vector< uint8_t > px( W * H * 4 ), out( W * H * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
	glReadPixels( 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
	for( int y = 0; y < H; ++y )
		std::memcpy( &out[ y * W * 4 ], &px[ ( H - 1 - y ) * W * 4 ], W * 4 );
	stbi_write_png( path.c_str(), W, H, 4, out.data(), W * 4 );
	printf( "shot_rgba %s\n", path.c_str() );
}

void Stats( const std::string& label )
{
	if( cpuMs.empty() )
		return;
	auto summary = []( std::vector< double > v, double& avg, double& p99, double& mx ) {
		double s = 0;
		for( double x : v )
			s += x;
		avg = s / v.size();
		std::sort( v.begin(), v.end() );
		p99 = v[ std::min( v.size() - 1, size_t( v.size() * 0.99 ) ) ];
		mx  = v.back();
	};
	double ca, cp, cm, ga, gp, gm;
	summary( cpuMs, ca, cp, cm );
	summary( gpuMs, ga, gp, gm );
	std::string spikes;
	for( size_t i = 0; i < cpuMs.size(); ++i )
		if( cpuMs[ i ] > 20.0 )
			spikes += " #" + std::to_string( i ) + "=" + std::to_string( (int)cpuMs[ i ] ) + "ms(gpu " + std::to_string( (int)gpuMs[ i ] ) + ")";
	if( !spikes.empty() )
		printf( "spikes >20ms:%s\n", spikes.c_str() );
	printf( "stats %-28s frames %4zu | plugin wall ms avg %6.2f p99 %6.2f max %6.2f | GPU ms avg %6.2f p99 %6.2f max %6.2f | => %.0f fps-equivalent (avg wall)\n",
	        label.c_str(), cpuMs.size(), ca, cp, cm, ga, gp, gm, 1000.0 / std::max( ca, 1e-3 ) );
	// W5b: the stats line prints GPU ms to 0.01, the size of a W4-vs-W3 gap; a separate line so its parsers are untouched
	printf( "stats_fine %s GPU ms avg %.4f\n", label.c_str(), ga );
}

// "Spin Y" -> index; a plain number is an index
int ParamIndex( const std::string& key )
{
	if( !key.empty() && key.find_first_not_of( "0123456789" ) == std::string::npos )
		return std::stoi( key );
	for( unsigned i = 0; i < numParams; ++i )
	{
		const char* name = (const char*)Call( FF_GET_PARAMETER_NAME, U( i ) ).PointerValue;
		if( name && key == name )
			return (int)i;
	}
	if( key.find( '_' ) != std::string::npos ) // "Pose_Data" -> "Pose Data" (for commands that take one token)
	{
		std::string spaced = key;
		std::replace( spaced.begin(), spaced.end(), '_', ' ' );
		return ParamIndex( spaced );
	}
	fprintf( stderr, "unknown parameter: '%s'\n", key.c_str() );
	return -1;
}

// "<name words...> <value>" -> (name, value)
bool SplitNameValue( std::istringstream& ss, std::string& name, float& v )
{
	std::string rest;
	std::getline( ss >> std::ws, rest );
	size_t sp = rest.find_last_of( ' ' );
	if( sp == std::string::npos )
		return false;
	name = rest.substr( 0, sp );
	v    = std::stof( rest.substr( sp + 1 ) );
	return true;
}

void Compare( const std::string& a, const std::string& b )
{
	int wa, ha, ca, wb, hb, cb;
	uint8_t* pa = stbi_load( a.c_str(), &wa, &ha, &ca, 3 );
	uint8_t* pb = stbi_load( b.c_str(), &wb, &hb, &cb, 3 );
	if( !pa || !pb || wa != wb || ha != hb )
	{
		printf( "compare %s %s: CANNOT COMPARE\n", a.c_str(), b.c_str() );
		stbi_image_free( pa ), stbi_image_free( pb );
		return;
	}
	double sum = 0;
	int mx = 0;
	size_t over = 0;
	for( int i = 0; i < wa * ha; ++i )
	{
		int m = 0;
		for( int k = 0; k < 3; ++k )
		{
			int d = std::abs( int( pa[ i * 3 + k ] ) - int( pb[ i * 3 + k ] ) );
			sum += d;
			m = std::max( m, d );
		}
		mx = std::max( mx, m );
		over += m > 2;
	}
	printf( "compare %s vs %s: mean %.4f max %d pixels>2 %zu of %d\n", a.c_str(), b.c_str(), sum / ( wa * ha * 3.0 ), mx, over, wa * ha );
	stbi_image_free( pa ), stbi_image_free( pb );
}

void Delta( const std::string& label, const std::string& a, const std::string& b )
{
	int wa, ha, ca, wb, hb, cb;
	uint8_t* pa = stbi_load( a.c_str(), &wa, &ha, &ca, 3 );
	uint8_t* pb = stbi_load( b.c_str(), &wb, &hb, &cb, 3 );
	if( !pa || !pb || wa != wb || ha != hb )
	{
		printf( "delta %s: CANNOT COMPARE %s %s\n", label.c_str(), a.c_str(), b.c_str() );
		stbi_image_free( pa ), stbi_image_free( pb );
		return;
	}
	double d[ 3 ] = {}, m[ 3 ] = {};
	size_t n = 0;
	for( int i = 0; i < wa * ha; ++i )
	{
		int mx = 0;
		for( int k = 0; k < 3; ++k )
			mx = std::max( mx, std::abs( int( pb[ i * 3 + k ] ) - int( pa[ i * 3 + k ] ) ) );
		if( mx <= 4 )
			continue;
		++n;
		for( int k = 0; k < 3; ++k )
			d[ k ] += int( pb[ i * 3 + k ] ) - int( pa[ i * 3 + k ] ), m[ k ] += pb[ i * 3 + k ];
	}
	const double s = n ? 1.0 / double( n ) : 0.0;
	printf( "delta %-22s changed %7zu px | b-a RGB %+7.1f %+7.1f %+7.1f | b RGB %6.1f %6.1f %6.1f\n", label.c_str(), n, d[ 0 ] * s, d[ 1 ] * s, d[ 2 ] * s, m[ 0 ] * s,
	        m[ 1 ] * s, m[ 2 ] * s );
	stbi_image_free( pa ), stbi_image_free( pb );
}

void PrintParams()
{
	for( unsigned i = 0; i < numParams; ++i )
	{
		const char* name = (const char*)Call( FF_GET_PARAMETER_NAME, U( i ) ).PointerValue;
		unsigned type    = Call( FF_GET_PARAMETER_TYPE, U( i ) ).UIntValue;
		GetRangeStruct r{ i, { 0, 1 } };
		Call( FF_GET_RANGE, Ptr( &r ) );
		char groupBuf[ 64 ] = {};
		GetStringStruct g{ i, { groupBuf, sizeof( groupBuf ) - 1 } };
		Call( FF_GET_PARAM_GROUP, Ptr( &g ) );
		char nameBuf[ 64 ] = {};
		GetStringStruct dn{ i, { nameBuf, sizeof( nameBuf ) - 1 } };
		gPlugMain( FF_GET_PARAM_DISPLAY_NAME, Ptr( &dn ), inst );
		FFMixed def = Call( FF_GET_PARAMETER_DEFAULT, U( i ) );
		float d;
		std::memcpy( &d, &def.UIntValue, 4 );
		unsigned vis = Call( FF_GET_PRAMETER_VISIBILITY, U( i ) ).UIntValue;
		unsigned nel = Call( FF_GET_NUM_PARAMETER_ELEMENTS, U( i ) ).UIntValue;
		std::string opts;
		if( type == FF_TYPE_OPTION )
			for( unsigned e = 0; e < nel; ++e )
			{
				GetParameterElementNameStruct en{ i, e };
				const char* en_name = (const char*)gPlugMain( FF_GET_PARAMETER_ELEMENT_NAME, Ptr( &en ), inst ).PointerValue;
				opts += ( e ? " / " : " | options " ) + std::string( en_name ? en_name : "?" );
			}
		printf( "  %3u | %-10s | %-26s | %-24s | type %3u | range %g..%g | default %g | elements %u | %s%s\n", i, groupBuf, name, nameBuf, type, r.range.min, r.range.max,
		        ( type == FF_TYPE_TEXT || type == FF_TYPE_FILE ) ? 0.0f : d, nel, vis ? "visible" : "hidden", opts.c_str() );
		if( strlen( name ) > 16 || strlen( nameBuf ) > 16 )
			printf( "NAME CHECK: #%u '%s' / '%s' is over 16 characters\n", i, name, nameBuf );
		for( unsigned j = 0; j < i; ++j )
			if( strcmp( name, (const char*)Call( FF_GET_PARAMETER_NAME, U( j ) ).PointerValue ) == 0 )
				printf( "NAME CHECK: #%u '%s' duplicates #%u\n", i, name, j );
	}
	printf( "params: %u\n", numParams );
}
}

int main( int argc, char** argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: %s <script>\n", argv[ 0 ] );
		return 1;
	}
	CGLPixelFormatAttribute attrs[] = { kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
		                                kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, (CGLPixelFormatAttribute)0 };
	CGLPixelFormatObj pf;
	GLint npf;
	CGLChoosePixelFormat( attrs, &pf, &npf );
	CGLContextObj ctx;
	if( !pf || CGLCreateContext( pf, nullptr, &ctx ) != kCGLNoError )
	{
		fprintf( stderr, "no GL 4.1 context\n" );
		return 1;
	}
	CGLSetCurrentContext( ctx );
	printf( "GL %s | %s\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ) );
	glGenQueries( 1, &timerQuery );

	std::ifstream script( argv[ 1 ] );
	std::string line;
	void* lib = nullptr;
	while( std::getline( script, line ) )
	{
		if( line.empty() || line[ 0 ] == '#' )
			continue;
		std::istringstream ss( line );
		std::string cmd;
		ss >> cmd;
		if( cmd == "bundle" )
		{
			std::string path;
			std::getline( ss >> std::ws, path );
			std::string name = path.substr( path.find_last_of( '/' ) + 1 );
			name             = name.substr( 0, name.find( ".bundle" ) );
			std::string bin  = path + "/Contents/MacOS/" + name;
			lib              = dlopen( bin.c_str(), RTLD_NOW | RTLD_LOCAL );
			if( !lib )
			{
				fprintf( stderr, "dlopen failed: %s\n", dlerror() );
				return 1;
			}
			gPlugMain = (PlugMainFn)dlsym( lib, "plugMain" );
			gTestAdvance = ( void ( * )( void*, double, int ) )dlsym( lib, "LoadEhrTestAdvance" );
			if( Call( FF_INITIALISE_V2, U( 0 ) ).UIntValue != FF_SUCCESS )
			{
				fprintf( stderr, "initialise failed\n" );
				return 1;
			}
			PluginInfoStruct* info = (PluginInfoStruct*)Call( FF_GET_INFO, U( 0 ) ).PointerValue;
			numParams              = Call( FF_GET_NUM_PARAMETERS, U( 0 ) ).UIntValue;
			printf( "loaded %.16s id %.4s type %u, %u params\n", (const char*)info->PluginName, (const char*)info->PluginUniqueID, info->PluginType, numParams );
		}
		else if( cmd == "size" )
			ss >> W >> H;
		else if( cmd == "input" )
		{
			std::string what;
			std::getline( ss >> std::ws, what );
			if( what == "bars" )
				MakeInputBars();
			else
				LoadInputPng( what );
		}
		else if( cmd == "input_seq" )
		{
			std::string dir;
			std::getline( ss >> std::ws, dir );
			std::vector< std::string > files;
			if( DIR* d = opendir( dir.c_str() ) )
			{
				while( dirent* e = readdir( d ) )
				{
					std::string n = e->d_name;
					if( n.size() > 4 && n.substr( n.size() - 4 ) == ".png" )
						files.push_back( dir + "/" + n );
				}
				closedir( d );
			}
			std::sort( files.begin(), files.end() );
			stbi_set_flip_vertically_on_load( 1 );
			for( const std::string& f : files )
			{
				int w, h, c;
				uint8_t* px = stbi_load( f.c_str(), &w, &h, &c, 4 );
				if( !px )
					continue;
				if( seqFrames.empty() )
					inputW = w, inputH = h;
				if( w == inputW && h == inputH )
					seqFrames.emplace_back( px, px + size_t( w ) * h * 4 );
				stbi_image_free( px );
			}
			stbi_set_flip_vertically_on_load( 0 );
			if( !seqFrames.empty() )
			{
				glGenTextures( 1, &inputTex );
				glBindTexture( GL_TEXTURE_2D, inputTex );
				glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, inputW, inputH, 0, GL_RGBA, GL_UNSIGNED_BYTE, seqFrames[ 0 ].data() );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
				glBindTexture( GL_TEXTURE_2D, 0 );
			}
			printf( "input_seq %zu frames %dx%d from %s\n", seqFrames.size(), inputW, inputH, dir.c_str() );
		}
		else if( cmd == "instance" )
		{
			glGenTextures( 1, &hostTex );
			glBindTexture( GL_TEXTURE_2D, hostTex );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
			glBindTexture( GL_TEXTURE_2D, 0 );
			glGenFramebuffers( 1, &hostFBO );
			glBindFramebuffer( GL_FRAMEBUFFER, hostFBO );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hostTex, 0 );
			FFGLViewportStruct vp{ 0, 0, (GLuint)W, (GLuint)H };
			inst = nullptr;
			FFMixed r = gPlugMain( FF_INSTANTIATE_GL, Ptr( &vp ), nullptr );
			if( r.UIntValue == FF_FAIL || !r.PointerValue )
			{
				fprintf( stderr, "instantiate failed\n" );
				return 1;
			}
			inst = (FFInstanceID)r.PointerValue;
			printf( "instance %dx%d\n", W, H );
		}
		else if( cmd == "params" )
			PrintParams();
		else if( cmd == "options" ) // MoveEhr W2: one OPTION param's live elements as name=value (what the host lists)
		{
			std::string key;
			std::getline( ss >> std::ws, key );
			const int idx = ParamIndex( key );
			if( idx < 0 )
				continue;
			const unsigned nel = Call( FF_GET_NUM_PARAMETER_ELEMENTS, U( idx ) ).UIntValue;
			printf( "options %s (%u):", key.c_str(), nel );
			for( unsigned e = 0; e < nel; ++e )
			{
				GetParameterElementNameStruct en{ (FFUInt32)idx, e };
				const char* nm = (const char*)gPlugMain( FF_GET_PARAMETER_ELEMENT_NAME, Ptr( &en ), inst ).PointerValue;
				GetParameterElementValueStruct ev{ (FFUInt32)idx, e };
				FFMixed v = gPlugMain( FF_GET_PARAMETER_ELEMENT_VALUE, Ptr( &ev ), inst );
				float f;
				std::memcpy( &f, &v.UIntValue, 4 );
				printf( "%s %s=%g", e ? " |" : "", nm ? nm : "?", f );
			}
			printf( "\n" );
		}
		else if( cmd == "text" )
		{
			std::string key, t;
			ss >> key;
			std::getline( ss >> std::ws, t );
			int idx = ParamIndex( key );
			if( idx < 0 )
				continue;
			for( size_t k = 0; ( k = t.find( "\\n", k ) ) != std::string::npos; ) // "\n" = a line break (Return in Arena)
				t.replace( k, 2, "\n" ), ++k;
			if( !markHeld )
				logMark = LogSize();
			SetText( (unsigned)idx, t );
		}
		else if( cmd == "set" )
		{
			std::string name;
			float v;
			if( !SplitNameValue( ss, name, v ) )
				continue;
			int idx = ParamIndex( name );
			if( idx < 0 )
				continue;
			const std::string& nm = name;
			if( ( nm.rfind( "Reload", 0 ) == 0 || idx == ParamIndex( "Max Texture Size" ) || idx == ParamIndex( "Model Source" ) ) && !markHeld ) // these start a load: wait_loaded watches from here
				logMark = LogSize();
			SetFloat( (unsigned)idx, v );
		}
		else if( cmd == "setnext" )
		{
			// W29: setnext <name> <a> <b> <c> = the three params after <name> (a colour picker's SAT, BRI, ALPHA)
			std::string rest;
			std::getline( ss >> std::ws, rest );
			std::vector< std::string > tok;
			std::istringstream ts( rest );
			for( std::string x; ts >> x; )
				tok.push_back( x );
			if( tok.size() < 4 )
				continue;
			std::string name;
			for( size_t k = 0; k + 3 < tok.size(); ++k )
				name += ( k ? " " : "" ) + tok[ k ];
			int idx = ParamIndex( name );
			if( idx < 0 )
				continue;
			for( int k = 0; k < 3; ++k )
				SetFloat( (unsigned)( idx + 1 + k ), std::stof( tok[ tok.size() - 3 + k ] ) );
		}
		else if( cmd == "pulse" )
		{
			std::string name;
			std::getline( ss >> std::ws, name );
			int idx = ParamIndex( name );
			if( idx < 0 )
				continue;
			SetFloat( (unsigned)idx, 1.0f );
			RenderFrame( 1.0 / 60.0 );
			SetFloat( (unsigned)idx, 0.0f );
		}
		else if( cmd == "get" )
		{
			std::string name;
			std::getline( ss >> std::ws, name );
			int idx = ParamIndex( name );
			if( idx < 0 )
				continue;
			unsigned type = Call( FF_GET_PARAMETER_TYPE, U( idx ) ).UIntValue;
			FFMixed r     = Call( FF_GET_PARAMETER, U( idx ) );
			if( type == FF_TYPE_TEXT || type == FF_TYPE_FILE )
				printf( "get %s = '%s'\n", name.c_str(), r.PointerValue ? (const char*)r.PointerValue : "" );
			else
			{
				float f;
				std::memcpy( &f, &r.UIntValue, 4 );
				printf( "get %s = %g\n", name.c_str(), f );
			}
		}
		else if( cmd == "compare" )
		{
			std::string a, b;
			ss >> a >> b;
			Compare( a, b );
		}
		else if( cmd == "delta" )
		{
			std::string l, a, b;
			ss >> l >> a >> b;
			Delta( l, a, b );
		}
		else if( cmd == "bpm" )
			ss >> bpm;
		else if( cmd == "fft" )
			ss >> fftLevel;
		else if( cmd == "mark" ) // wait_loaded / wait_slots watch from here (the `text` commands after it keep this mark)
			logMark = LogSize(), markHeld = true;
		else if( cmd == "wait_loaded" )
		{
			double timeout = 60;
			ss >> timeout;
			auto t0 = std::chrono::steady_clock::now();
			std::string hit;
			while( std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count() < timeout )
			{
				RenderFrame( 1.0 / 60.0 );
				hit = LogFind( logMark, { "SWAP to", "Load failed" } );
				if( !hit.empty() )
					break;
				std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
			}
			for( int i = 0; i < 10; ++i )
				RenderFrame( 1.0 / 60.0 );
			markHeld = false;
			printf( "wait_loaded after %.2f s: %s\n", std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count(),
			        hit.empty() ? "TIMEOUT" : hit.substr( 24 ).c_str() );
		}
		else if( cmd == "wait_slots" ) // LoadEhr W1: render until <n> models have swapped in (or failed) since the last `text` / `mark`
		{
			int n          = 1;
			double timeout = 60;
			ss >> n >> timeout;
			auto t0 = std::chrono::steady_clock::now();
			int got = 0;
			while( std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count() < timeout )
			{
				RenderFrame( 1.0 / 60.0 );
				std::ifstream f( LogPath(), std::ios::binary );
				f.seekg( (std::streamoff)logMark );
				std::string line;
				got = 0;
				while( std::getline( f, line ) )
					got += line.find( "SWAP to" ) != std::string::npos || line.find( "Load failed" ) != std::string::npos ? 1 : 0;
				if( got >= n )
					break;
				std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
			}
			for( int i = 0; i < 10; ++i )
				RenderFrame( 1.0 / 60.0 );
			markHeld = false;
			printf( "wait_slots %d after %.2f s: %d swapped%s\n", n, std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count(), got,
			        got >= n ? "" : " TIMEOUT" );
		}
		else if( cmd == "wait_log" ) // MoveEhr W5b: render at ~60 Hz real time until <n> log lines since the last `mark` contain <needle>
		{                              // (a worker's result landing), so the frames counted after it do not depend on wall time
			int n          = 1;
			double timeout = 30;
			std::string needle;
			ss >> n >> timeout;
			std::getline( ss >> std::ws, needle );
			auto t0 = std::chrono::steady_clock::now();
			int got = 0;
			while( std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count() < timeout )
			{
				std::ifstream f( LogPath(), std::ios::binary );
				f.seekg( (std::streamoff)logMark );
				std::string line;
				got = 0;
				while( std::getline( f, line ) )
					got += line.find( needle ) != std::string::npos ? 1 : 0;
				if( got >= n )
					break;
				RenderFrame( 1.0 / 60.0 );
				std::this_thread::sleep_for( std::chrono::milliseconds( 16 ) );
			}
			markHeld = false;
			printf( "wait_log %d '%s' after %.2f s: %d%s\n", n, needle.c_str(), std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count(), got,
			        got >= n ? "" : " TIMEOUT" );
		}
		else if( cmd == "log_offset" ) // MoveEhr W5b: the log's size now (a test reads only what came after it)
		{
			std::string label;
			ss >> label;
			printf( "log_offset %s %zu\n", label.c_str(), LogSize() );
		}
		else if( cmd == "frames" )
		{
			int n = 1;
			ss >> n;
			for( int i = 0; i < n; ++i )
				RenderFrame( 1.0 / 60.0 );
		}
		else if( cmd == "advance" )
		{
			double secs = 0;
			std::string zero;
			ss >> secs >> zero;
			if( gTestAdvance )
				gTestAdvance( inst, secs, zero == "zero" ? 1 : 0 );
			else
				printf( "advance: the bundle has no LoadEhrTestAdvance\n" );
		}
		else if( cmd == "hosttime" )
			ss >> hostTime; // W16: a fixed host clock after loads of varying length (the next frame's dt clamps to 0)
		else if( cmd == "hostunit" ) // W28: hostunit s|ms
		{
			std::string u;
			ss >> u;
			hostUnit = u == "ms" ? 1000.0 : 1.0;
		}
		else if( cmd == "realtime" )
		{
			double secs = 1;
			ss >> secs;
			auto t0 = std::chrono::steady_clock::now();
			while( std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count() < secs )
			{
				auto f0 = std::chrono::steady_clock::now();
				RenderFrame( 1.0 / 60.0 );
				std::this_thread::sleep_until( f0 + std::chrono::microseconds( 16667 ) );
			}
		}
		else if( cmd == "composite" )
		{
			std::string m;
			ss >> m;
			composite = m == "black" ? Composite::Black : m == "straight" ? Composite::Straight : Composite::Arena;
			printf( "composite %s\n", composite == Composite::Black ? "black" : composite == Composite::Straight ? "straight" : "arena" );
		}
		else if( cmd == "shot" )
		{
			std::string p;
			std::getline( ss >> std::ws, p );
			Shot( p );
		}
		else if( cmd == "shot_rgba" )
		{
			std::string p;
			std::getline( ss >> std::ws, p );
			ShotRGBA( p );
		}
		else if( cmd == "stats_reset" )
			cpuMs.clear(), gpuMs.clear();
		else if( cmd == "stats" )
		{
			std::string label;
			std::getline( ss >> std::ws, label );
			Stats( label );
		}
		else if( cmd == "diffs" )
		{
			int n = 1;
			ss >> n;
			std::vector< uint8_t > prev = ReadFrame();
			printf( "diffs:" );
			for( int i = 0; i < n; ++i )
			{
				RenderFrame( 1.0 / 60.0 );
				std::vector< uint8_t > cur = ReadFrame();
				double sum                 = 0;
				for( size_t k = 0; k < cur.size(); ++k )
					sum += std::abs( int( cur[ k ] ) - int( prev[ k ] ) );
				printf( " %.3f", sum / cur.size() );
				prev.swap( cur );
			}
			printf( "\n" );
		}
		else if( cmd == "sweep" )
		{
			// W16: sweep <steps> <frames per step> <shot prefix|-> <param>: ramps the param 0 -> 1 in <steps> steps
			// and prints per step the mean |change| per channel vs the previous step and the covered pixels (alpha > 8);
			// with a prefix, shots at 0.05 / 0.15 / 0.5 / 0.85 / 0.95
			int steps = 100, per = 1;
			std::string prefix, name;
			ss >> steps >> per >> prefix;
			std::getline( ss >> std::ws, name );
			int idx = ParamIndex( name );
			if( idx < 0 || steps < 1 )
				continue;
			SetFloat( (unsigned)idx, 0.0f );
			for( int i = 0; i < per; ++i )
				RenderFrame( 1.0 / 60.0 );
			std::vector< uint8_t > prev = ReadFrame();
			for( int s = 1; s <= steps; ++s )
			{
				SetFloat( (unsigned)idx, float( s ) / float( steps ) );
				for( int i = 0; i < per; ++i )
					RenderFrame( 1.0 / 60.0 );
				std::vector< uint8_t > cur = ReadFrame();
				double sum   = 0;
				size_t cover = 0;
				for( size_t k = 0; k < cur.size(); ++k )
					sum += std::abs( int( cur[ k ] ) - int( prev[ k ] ) );
				for( size_t k = 3; k < cur.size(); k += 4 )
					cover += cur[ k ] > 8;
				printf( "sweep %.2f change %.4f cover %zu\n", float( s ) / float( steps ), sum / cur.size(), cover );
				prev.swap( cur );
				if( prefix != "-" )
					for( int q : { 5, 15, 50, 85, 95 } )
						if( s * 100 == q * steps )
							Shot( prefix + "_" + ( q < 10 ? "0" : "" ) + std::to_string( q ) + ".png" );
			}
		}
		else if( cmd == "scrub" )
		{
			// W7: scrub <from> <to> <steps> <param>: sets the param from -> to in <steps> steps, one frame each, and prints per step
			// the mean |change| (all channels), the max |change| and the pixels whose rgb changed by > 32 vs the previous step
			float from = 0, to = 1;
			int steps  = 100;
			std::string name;
			ss >> from >> to >> steps;
			std::getline( ss >> std::ws, name );
			int idx = ParamIndex( name );
			if( idx < 0 || steps < 1 )
				continue;
			SetFloat( (unsigned)idx, from );
			RenderFrame( 1.0 / 60.0 );
			std::vector< uint8_t > prev = ReadFrame();
			for( int s = 1; s <= steps; ++s )
			{
				const float v = from + ( to - from ) * float( s ) / float( steps );
				SetFloat( (unsigned)idx, v );
				RenderFrame( 1.0 / 60.0 );
				std::vector< uint8_t > cur = ReadFrame();
				double sum = 0;
				int mx     = 0;
				size_t big = 0;
				for( size_t k = 0; k < cur.size(); k += 4 )
				{
					int pm = 0;
					for( int c = 0; c < 4; ++c )
					{
						const int d = std::abs( int( cur[ k + c ] ) - int( prev[ k + c ] ) );
						sum += d, mx = std::max( mx, d );
						if( c < 3 )
							pm = std::max( pm, d );
					}
					big += pm > 32;
				}
				printf( "scrub %.4f change %.4f max %d big %zu\n", v, sum / cur.size(), mx, big );
				prev.swap( cur );
			}
		}
		else if( cmd == "ref_save" ) // W10: ref_save <name> = keep the current host frame (raw premultiplied RGBA) in memory
		{
			std::string n;
			ss >> n;
			refFrames[ n ] = ReadFrame();
		}
		else if( cmd == "ref_cmp" ) // W10: ref_cmp <label> <name> = the current frame vs a kept one: mean |d| over RGBA (0..255),
		{                           // pixels whose largest channel change is > 8, coverage (alpha > 8) of both
			std::string l, n;
			ss >> l >> n;
			auto it = refFrames.find( n );
			std::vector< uint8_t > cur = ReadFrame();
			if( it == refFrames.end() || it->second.size() != cur.size() )
			{
				printf( "ref_cmp %s: NO REF %s\n", l.c_str(), n.c_str() );
				continue;
			}
			const std::vector< uint8_t >& a = it->second;
			double sum = 0, sd[ 4 ] = {}; // sd: the signed mean change per channel (a chaotic sim moves its smoke around but keeps
			size_t changed = 0, covA = 0, covB = 0; // its colour and amount; a look that changes them moves these)
			int mx = 0;
			for( size_t k = 0; k < cur.size(); k += 4 )
			{
				int pm = 0;
				for( int c = 0; c < 4; ++c )
				{
					const int d = std::abs( int( cur[ k + c ] ) - int( a[ k + c ] ) );
					sum += d, pm = std::max( pm, d ), sd[ c ] += int( cur[ k + c ] ) - int( a[ k + c ] );
				}
				mx = std::max( mx, pm );
				changed += pm > 8;
				covA += a[ k + 3 ] > 8, covB += cur[ k + 3 ] > 8;
			}
			const double np = double( cur.size() / 4 );
			printf( "ref_cmp %s mean %.4f max %d changed %zu cover %zu %zu of %zu shift %.3f %.3f %.3f %.3f\n", l.c_str(), sum / cur.size(), mx, changed, covA,
			        covB, cur.size() / 4, sd[ 0 ] / np, sd[ 1 ] / np, sd[ 2 ] / np, sd[ 3 ] / np );
		}
		else if( cmd == "means" )
		{
			int n = 1;
			ss >> n;
			for( int i = 0; i < n; ++i )
			{
				RenderFrame( 1.0 / 60.0 );
				std::vector< uint8_t > px = ReadFrame();
				double m[ 4 ] = {};
				size_t cnt    = 0;
				for( size_t k = 0; k < px.size(); k += 4 )
				{
					if( px[ k + 3 ] == 0 )
						continue;
					++cnt;
					for( int c = 0; c < 3; ++c )
						m[ c ] += px[ k + c ];
					m[ 3 ] += std::max( px[ k ], std::max( px[ k + 1 ], px[ k + 2 ] ) );
				}
				const double s = cnt ? 1.0 / double( cnt ) : 0.0;
				printf( "means frame %3d | R %6.2f G %6.2f B %6.2f | max(RGB) %6.2f | %zu px\n", i, m[ 0 ] * s, m[ 1 ] * s, m[ 2 ] * s, m[ 3 ] * s, cnt );
			}
		}
		else if( cmd == "save_state" )
		{
			savedState.assign( numParams, SavedParam() );
			for( unsigned i = 0; i < numParams; ++i )
			{
				unsigned type = Call( FF_GET_PARAMETER_TYPE, U( i ) ).UIntValue;
				FFMixed r     = Call( FF_GET_PARAMETER, U( i ) );
				savedState[ i ].text = type == FF_TYPE_TEXT || type == FF_TYPE_FILE;
				if( savedState[ i ].text )
					savedState[ i ].s = r.PointerValue ? (const char*)r.PointerValue : "";
				else
					std::memcpy( &savedState[ i ].f, &r.UIntValue, 4 );
			}
			printf( "save_state: %u params read\n", numParams );
		}
		else if( cmd == "restore_state" )
		{
			std::string order;
			ss >> order;
			const bool rev = order == "reverse";
			for( unsigned j = 0; j < savedState.size(); ++j )
			{
				const unsigned i = rev ? unsigned( savedState.size() ) - 1 - j : j;
				if( Call( FF_GET_PARAMETER_TYPE, U( i ) ).UIntValue == FF_TYPE_BUFFER )
					continue;
				if( savedState[ i ].text )
					SetText( i, savedState[ i ].s );
				else
					SetFloat( i, savedState[ i ].f );
			}
			printf( "restore_state: %zu params written in %s index order\n", savedState.size(), rev ? "reverse" : "" );
		}
		else if( cmd == "delete" )
		{
			Call( FF_DEINSTANTIATE_GL, U( 0 ) );
			inst = nullptr;
		}
		else
			fprintf( stderr, "unknown command: %s\n", cmd.c_str() );
	}
	if( inst )
		Call( FF_DEINSTANTIATE_GL, U( 0 ) );
	if( gPlugMain )
		gPlugMain( FF_DEINITIALISE, U( 0 ), nullptr );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( ctx );
	return 0;
}
