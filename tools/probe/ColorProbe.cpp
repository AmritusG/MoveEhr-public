// W11 DIAGNOSE probe: how does Arena present and deliver FFGL colour params?
// Four layouts side by side; every value Arena sends is logged to ~/Library/Logs/LoadEhr/ColorProbe.log.
// Throwaway: not part of LoadEhr, removed from Extra Effects after the diagnosis.
#include "FFGLPluginSDK.h"
#include <OpenGL/gl3.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/time.h>

namespace
{
struct Def { const char* name; unsigned type; float mn, mx, def; };
const Def kDefs[] = {
	{ "P1 Before", FF_TYPE_STANDARD, 0, 100, 12 },  // like "A Intensity"
	{ "P1 Red", FF_TYPE_RED, 0, 1, 1 },             // current LoadEhr: RGB triple ...
	{ "P1 Green", FF_TYPE_GREEN, 0, 1, 0.86f },
	{ "P1 Blue", FF_TYPE_BLUE, 0, 1, 0.62f },
	{ "P1 After", FF_TYPE_STANDARD, -2, 2, 0 },     // ... followed by a ranged STD (like "A Hue Drift")
	{ "P2 Red", FF_TYPE_RED, 0, 1, 1 },             // RGBA quartet
	{ "P2 Green", FF_TYPE_GREEN, 0, 1, 0.5f },
	{ "P2 Blue", FF_TYPE_BLUE, 0, 1, 0.25f },
	{ "P2 Alpha", FF_TYPE_ALPHA, 0, 1, 1 },
	{ "P3 Hue", FF_TYPE_HUE, 0, 1, 0.3f },          // HSBA quartet
	{ "P3 Sat", FF_TYPE_SATURATION, 0, 1, 1 },
	{ "P3 Bri", FF_TYPE_BRIGHTNESS, 0, 1, 1 },
	{ "P3 Alpha", FF_TYPE_ALPHA, 0, 1, 1 },
	{ "P4 Hue", FF_TYPE_HUE, 0, 1, 0.52f },         // lone HUE followed by STDs
	{ "P4 After", FF_TYPE_STANDARD, 0, 4, 1 },
	{ "P4 After2", FF_TYPE_STANDARD, 0, 1, 0.5f },
	{ "Show", FF_TYPE_OPTION, 0, 2, 1 },
};
constexpr unsigned kN = sizeof( kDefs ) / sizeof( kDefs[ 0 ] );

void PLog( const char* fmt, ... )
{
	static FILE* f = nullptr;
	if( !f )
	{
		std::string p = std::string( getenv( "HOME" ) ) + "/Library/Logs/LoadEhr/ColorProbe.log";
		f = fopen( p.c_str(), "a" );
		if( !f ) return;
	}
	timeval tv; gettimeofday( &tv, nullptr );
	char ts[ 32 ]; strftime( ts, sizeof ts, "%H:%M:%S", localtime( &tv.tv_sec ) );
	fprintf( f, "%s.%03d ", ts, int( tv.tv_usec / 1000 ) );
	va_list a; va_start( a, fmt ); vfprintf( f, fmt, a ); va_end( a );
	fputc( '\n', f ); fflush( f );
}
}

class ColorProbe : public CFFGLPlugin
{
public:
	ColorProbe()
	{
		SetMinInputs( 0 );
		SetMaxInputs( 0 );
		for( unsigned i = 0; i < kN; ++i )
		{
			const Def& d = kDefs[ i ];
			if( d.type == FF_TYPE_OPTION )
			{
				SetOptionParamInfo( i, d.name, 3, d.def );
				SetParamElementInfo( i, 0, "P1", 0 );
				SetParamElementInfo( i, 1, "P2", 1 );
				SetParamElementInfo( i, 2, "P3", 2 );
			}
			else
			{
				SetParamInfo( i, d.name, d.type, d.def );
				if( !( d.mn == 0 && d.mx == 1 ) )
				{
					SetParamRange( i, d.mn, d.mx );
					FindParamInfo( i )->defaultFloatVal = d.def;
				}
			}
			v[ i ] = d.def;
		}
		PLog( "probe instance %p created", (void*)this );
	}
	FFResult SetFloatParameter( unsigned int i, float value ) override
	{
		if( i >= kN ) return FF_FAIL;
		if( v[ i ] != value )
			PLog( "set %2u %-10s type %3u <- %.4f", i, kDefs[ i ].name, kDefs[ i ].type, value );
		v[ i ] = value;
		return FF_SUCCESS;
	}
	float GetFloatParameter( unsigned int i ) override { return i < kN ? v[ i ] : 0; }
	FFResult ProcessOpenGL( ProcessOpenGLStruct* ) override
	{
		float r, g, b;
		int show = (int)std::lround( v[ 16 ] );
		if( show == 0 ) r = v[ 1 ], g = v[ 2 ], b = v[ 3 ];
		else if( show == 1 ) r = v[ 5 ] * v[ 8 ], g = v[ 6 ] * v[ 8 ], b = v[ 7 ] * v[ 8 ];
		else
		{
			float h = v[ 9 ] * 6, s = v[ 10 ], br = v[ 11 ];
			auto ch = [ & ]( float n ) { float k = std::fmod( n + h, 6.0f ); return br - br * s * std::max( 0.0f, std::min( { k, 4 - k, 1.0f } ) ); };
			r = ch( 5 ), g = ch( 3 ), b = ch( 1 );
			r *= v[ 12 ], g *= v[ 12 ], b *= v[ 12 ];
		}
		glClearColor( r, g, b, 1 );
		glClear( GL_COLOR_BUFFER_BIT );
		return FF_SUCCESS;
	}
	float v[ kN ];
};

namespace
{
CFFGLPluginInfo PluginInfo( PluginFactory< ColorProbe >, "MEcp", "LoadEhr ColorProbe", 2, 1, 1, 0, FF_SOURCE,
                            "W11 diagnostic: FFGL colour param layouts.", "LoadEhr by Amrit Rosell" );
}
