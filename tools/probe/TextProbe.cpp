// W21 DIAGNOSE probe: which FFGL TEXT params does Arena 7.28 give a multi-line editor (REST valuetype ParamText vs
// ParamString)? Three TEXT params that differ only in their default value. Every string Arena sends is logged to
// ~/Library/Logs/LoadEhr/TextProbe.log (line breaks shown as \n). Throwaway: removed from Extra Effects afterwards.
#include "FFGLPluginSDK.h"
#include <OpenGL/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
struct Def { const char* name; const char* def; };
const Def kDefs[] = {
	{ "T Plain", "abc" },          // like LoadEhr's Text today
	{ "T Newline", "abc\ndef" },   // default with a line break
	{ "T CRLF", "abc\r\ndef" },    // default with CR LF
};
constexpr unsigned kN = sizeof( kDefs ) / sizeof( kDefs[ 0 ] );
void PLog( unsigned i, const std::string& s )
{
	std::string e;
	for( char c : s )
		e += c == '\n' ? std::string( "\\n" ) : c == '\r' ? std::string( "\\r" ) : std::string( 1, c );
	if( FILE* f = fopen( ( std::string( getenv( "HOME" ) ) + "/Library/Logs/LoadEhr/TextProbe.log" ).c_str(), "a" ) )
		fprintf( f, "set %u %s <- '%s'\n", i, kDefs[ i ].name, e.c_str() ), fclose( f );
}
}

class TextProbe : public CFFGLPlugin
{
public:
	TextProbe()
	{
		SetMinInputs( 0 );
		SetMaxInputs( 0 );
		for( unsigned i = 0; i < kN; ++i )
			SetParamInfo( i, kDefs[ i ].name, FF_TYPE_TEXT, kDefs[ i ].def ), t[ i ] = kDefs[ i ].def;
	}
	FFResult SetTextParameter( unsigned int i, const char* value ) override
	{
		if( i >= kN ) return FF_FAIL;
		t[ i ] = value ? value : "";
		PLog( i, t[ i ] );
		return FF_SUCCESS;
	}
	char* GetTextParameter( unsigned int i ) override { return i < kN ? (char*)t[ i ].c_str() : (char*)""; }
	FFResult ProcessOpenGL( ProcessOpenGLStruct* ) override
	{
		glClearColor( 0.1f, 0.1f, 0.1f, 1 );
		glClear( GL_COLOR_BUFFER_BIT );
		return FF_SUCCESS;
	}
	std::string t[ kN ];
};

namespace
{
CFFGLPluginInfo PluginInfo( PluginFactory< TextProbe >, "MEtp", "LoadEhr TextProbe", 2, 1, 1, 0, FF_SOURCE,
                            "W21 diagnostic: FFGL text param editors.", "LoadEhr by Amrit Rosell" );
}
