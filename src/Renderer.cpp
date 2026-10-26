#include "Renderer.h"
#include <mutex>
#include "Log.h"
#include "MathUtil.h"
#include "Shaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace mehr
{
namespace
{
GLuint CompileStage( GLenum type, const char* src, const char* defines, std::string& error )
{
	std::string full = std::string( "#version 410 core\n" ) + defines + src;
	GLuint s         = glCreateShader( type );
	const char* p    = full.c_str();
	glShaderSource( s, 1, &p, nullptr );
	glCompileShader( s );
	GLint ok = 0;
	glGetShaderiv( s, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		GLint len = 0;
		glGetShaderiv( s, GL_INFO_LOG_LENGTH, &len );
		std::vector< char > log( std::max( len, 1 ) );
		glGetShaderInfoLog( s, len, nullptr, log.data() );
		error = std::string( "shader compile failed (" ) + ( type == GL_VERTEX_SHADER ? "VS" : "FS" ) + "): " + log.data();
		Log( "%s", error.c_str() );
		glDeleteShader( s );
		return 0;
	}
	return s;
}

}

namespace
{
// W2: while a Renderer initialises, every program linked from the triangle-soup vertex stage gets its DEFORM twin (the same
// sources with #define DEFORM): plain program -> twin
std::vector< std::pair< GLuint, GLuint > >* g_deformSink = nullptr;
// MoveEhr W1: while a Renderer initialises, every program whose vertex stage writes vo.opos records its sources (interned
// process-wide: every instance links the same strings), so AnimTwin() can link its ANIM twin when an animated model first draws
std::unordered_map< GLuint, ProgSource >* g_srcSink = nullptr;
std::shared_ptr< const std::string > InternSource( const std::string& s )
{
	static std::mutex m;
	static std::unordered_map< std::string, std::shared_ptr< const std::string > > pool;
	std::lock_guard< std::mutex > lock( m );
	auto it = pool.find( s );
	if( it != pool.end() )
		return it->second;
	auto sp = std::make_shared< const std::string >( s );
	pool.emplace( s, sp );
	return sp;
}
// W5: the same for WIRE twins, of the soup programs that link the MAIN fragment stage (not the shadow / crack / projector / photon
// passes); a WIRE twin gets its own DEFORM twin
std::vector< std::pair< GLuint, GLuint > >* g_wireSink = nullptr;
// W6: and their POINTS twins (WIRE + POINTS: the soup stage at a point's spot, drawn as a sprite), each with its own DEFORM twin
std::vector< std::pair< GLuint, GLuint > >* g_pointSink = nullptr;
// W9: and their CAPTURE twins (POINTS + CAPTURE, transform feedback of the spots' world position / normal / colour; not the instanced
// ones), each with its own DEFORM twin
std::vector< std::pair< GLuint, GLuint > >* g_captureSink = nullptr;
GLuint LinkPlain( const std::string& vs, const std::string& fs, std::string& error, bool capture = false );
const char* kCaptureFS = "out vec4 fragColor;\nvoid main() { fragColor = vec4( 0.0 ); }\n";
}

// Links a program; each stage's source is its pieces concatenated (defines, libraries, main).
GLuint Link( const std::string& vs, const std::string& fs, std::string& error )
{
	const GLuint p   = LinkPlain( vs, fs, error );
	const bool soup  = vs.find( "uniform usamplerBuffer uIdxBuf; // R32UI view of the index buffer" ) != std::string::npos;
	if( p && g_deformSink && soup )
		if( const GLuint d = LinkPlain( "#define DEFORM\n" + vs, "#define DEFORM\n" + fs, error ) )
			g_deformSink->push_back( { p, d } );
	const bool mainFS = fs.find( "uniform float uWireMix;" ) != std::string::npos;
	bool pass         = false;
	for( const char* d : { "#define SHADOW_PASS", "#define CRACK_PASS", "#define PROJ_PASS", "#define PHOTON_PASS" } )
		pass = pass || fs.find( d ) != std::string::npos;
	if( p && g_wireSink && soup && mainFS && !pass )
		if( const GLuint w = LinkPlain( "#define WIRE\n" + vs, "#define WIRE\n" + fs, error ) )
		{
			g_wireSink->push_back( { p, w } );
			if( g_deformSink )
				if( const GLuint d = LinkPlain( "#define WIRE\n#define DEFORM\n" + vs, "#define WIRE\n#define DEFORM\n" + fs, error ) )
					g_deformSink->push_back( { w, d } );
		}
	if( p && g_pointSink && soup && mainFS && !pass )
		if( const GLuint q = LinkPlain( "#define WIRE\n#define POINTS\n" + vs, "#define WIRE\n#define POINTS\n" + fs, error ) )
		{
			g_pointSink->push_back( { p, q } );
			if( g_deformSink )
				if( const GLuint d = LinkPlain( "#define WIRE\n#define POINTS\n#define DEFORM\n" + vs, "#define WIRE\n#define POINTS\n#define DEFORM\n" + fs, error ) )
					g_deformSink->push_back( { q, d } );
		}
	if( p && g_captureSink && soup && mainFS && !pass && vs.find( "#define INSTANCED" ) == std::string::npos )
		if( const GLuint c = LinkPlain( "#define POINTS\n#define CAPTURE\n" + vs, kCaptureFS, error, true ) )
		{
			g_captureSink->push_back( { p, c } );
			if( g_deformSink )
				if( const GLuint d = LinkPlain( "#define POINTS\n#define CAPTURE\n#define DEFORM\n" + vs, kCaptureFS, error, true ) )
					g_deformSink->push_back( { c, d } );
		}
	return p;
}

namespace
{
GLuint LinkPlain( const std::string& vs, const std::string& fs, std::string& error, bool capture )
{
	GLuint v = CompileStage( GL_VERTEX_SHADER, vs.c_str(), "", error );
	GLuint f = CompileStage( GL_FRAGMENT_SHADER, fs.c_str(), "", error );
	if( !v || !f )
	{
		glDeleteShader( v ), glDeleteShader( f );
		return 0;
	}
	GLuint p = glCreateProgram();
	glAttachShader( p, v );
	glAttachShader( p, f );
	if( capture )
	{
		const char* vary[ 3 ] = { "vCapPos", "vCapNrm", "vCapCol" };
		glTransformFeedbackVaryings( p, 3, vary, GL_SEPARATE_ATTRIBS );
	}
	glLinkProgram( p );
	glDeleteShader( v ), glDeleteShader( f );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		GLint len = 0;
		glGetProgramiv( p, GL_INFO_LOG_LENGTH, &len );
		std::vector< char > log( std::max( len, 1 ) );
		glGetProgramInfoLog( p, len, nullptr, log.data() );
		error = std::string( "program link failed: " ) + log.data();
		Log( "%s", error.c_str() );
		glDeleteProgram( p );
		return 0;
	}
	if( g_srcSink && vs.find( "vo.opos" ) != std::string::npos )
		( *g_srcSink )[ p ] = { InternSource( vs ), InternSource( fs ), capture };
	return p;
}
}
// MoveEhr W1: Pose.cpp links ANIM twins through the same path
GLuint LinkPlainSrc( const std::string& vs, const std::string& fs, std::string& error, bool capture )
{
	return LinkPlain( vs, fs, error, capture );
}

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
// a DEFORM twin gets the plain program's sampler units (each Init* pinned them on the plain one)
void CopySamplerUnits( GLuint from, GLuint to )
{
	GLint n = 0;
	glGetProgramiv( from, GL_ACTIVE_UNIFORMS, &n );
	glUseProgram( to );
	for( GLint i = 0; i < n; ++i )
	{
		char name[ 128 ];
		GLsizei len = 0;
		GLint size  = 0;
		GLenum type = 0;
		glGetActiveUniform( from, (GLuint)i, sizeof( name ), &len, &size, &type, name );
		const bool sampler = type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE || type == GL_SAMPLER_BUFFER || type == GL_UNSIGNED_INT_SAMPLER_BUFFER ||
		                     type == GL_INT_SAMPLER_BUFFER || type == GL_SAMPLER_3D || type == GL_SAMPLER_2D_RECT;
		if( !sampler )
			continue;
		for( GLint k = 0; k < size; ++k )
		{
			std::string nm = name;
			if( size > 1 )
				nm = nm.substr( 0, nm.find( '[' ) ) + "[" + std::to_string( k ) + "]";
			GLint unit = 0;
			glGetUniformiv( from, glGetUniformLocation( from, nm.c_str() ), &unit );
			glUniform1i( glGetUniformLocation( to, nm.c_str() ), unit );
		}
	}
	glUseProgram( 0 );
}
}

// Shadow maps live on units 11-14, 17, 18 in every program that includes kLightLib (program must be bound).
void BindShadowSamplers( GLuint prog )
{
	// shard buffers (soup stage) own units 15 / 16: a sampler left on unit 0 would clash with uTex0's type
	glUniform1i( glGetUniformLocation( prog, "uShardOf" ), 15 );
	glUniform1i( glGetUniformLocation( prog, "uShardData" ), 16 );
	glUniform1i( glGetUniformLocation( prog, "uLetterOf" ), 19 ); // W21 (LETTERS programs only)
	glUniform1i( glGetUniformLocation( prog, "uLetterXf" ), 20 );
	char name[ 16 ];
	for( int i = 0; i < kMaxVolLights; ++i )
	{
		snprintf( name, sizeof( name ), "uXShadow2D%d", i );
		glUniform1i( glGetUniformLocation( prog, name ), kShadow2DUnit[ i ] );
	}
	glUniform1i( glGetUniformLocation( prog, "uXShadowCube0" ), kShadowCubeUnit[ 0 ] );
	glUniform1i( glGetUniformLocation( prog, "uXShadowCube1" ), kShadowCubeUnit[ 1 ] );
}

void UnbindShadowUnits()
{
	for( int u : kShadow2DUnit )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	for( int u : kShadowCubeUnit )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
}

namespace
{

void BindSamplers( GLuint prog )
{
	glUseProgram( prog );
	char name[ 16 ];
	for( int i = 0; i < CH_COUNT; ++i )
	{
		snprintf( name, sizeof( name ), "uTex%d", i );
		glUniform1i( U( prog, name ), i );
	}
	glUniform1i( U( prog, "uInputTex" ), 8 );
	glUniform1i( U( prog, "uIdxBuf" ), 9 );
	glUniform1i( U( prog, "uVtxBuf" ), 10 );
	BindShadowSamplers( prog );
	glUseProgram( 0 );
}
}

void CopySamplerUnitsExt( GLuint from, GLuint to ) { CopySamplerUnits( from, to ); } // MoveEhr W1: Pose.cpp's ANIM twins

bool Renderer::Init( std::string& error )
{
	g_srcSink = &progSrc; // MoveEhr W1: every model program linked during Init keeps its sources (ANIM twins on demand)
	const auto t0 = std::chrono::steady_clock::now();
	g_deformSink = &deformTwins; // W2: every soup program linked from here on gets its DEFORM twin
	g_wireSink   = &wireTwins;   // W5: and the main-pass ones their WIRE twin
	g_pointSink  = &pointTwins;  // W6: and their POINTS twin
	g_captureSink = &captureTwins; // W9: and their CAPTURE twin
	const bool ok = InitPrograms( error );
	g_deformSink  = nullptr;
	g_wireSink    = nullptr;
	g_pointSink   = nullptr;
	g_captureSink = nullptr;
	for( const auto& t : wireTwins ) // before the DEFORM copies: a WIRE twin hands its units on to its own DEFORM twin
		CopySamplerUnits( t.first, t.second );
	for( const auto& t : pointTwins )
		CopySamplerUnits( t.first, t.second );
	for( const auto& t : captureTwins )
		CopySamplerUnits( t.first, t.second );
	for( const auto& t : deformTwins )
		CopySamplerUnits( t.first, t.second );
	for( const auto& t : wireTwins )
		for( GLuint q : { t.second, DeformTwin( t.second ) } )
		{
			glUseProgram( q );
			glUniform1i( U( q, "uEdgeMask" ), 25 );
		}
	for( const auto* tw : { &pointTwins, &captureTwins } )
		for( const auto& t : *tw )
			for( GLuint q : { t.second, DeformTwin( t.second ) } )
			{
				glUseProgram( q );
				glUniform1i( U( q, "uPointRef" ), 26 );
			}
	glUseProgram( 0 );
	if( ok )
		Log( "DEFORM %zu soup programs have their DEFORM twin, %zu their WIRE twin, %zu their POINTS twin, %zu their CAPTURE twin (%.0f ms)", deformTwins.size(), wireTwins.size(),
		     pointTwins.size(), captureTwins.size(), std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
	const bool all = ok && InitWire( error ) && InitPoints( error ) && InitLines( error ) && InitBuild( error ) && InitSurfLook( error );
	g_srcSink      = nullptr;
	return all;
}

// MoveEhr W4: the PTFX twin (#define PTFX: the point shapes / motions / size variation) of a POINTS twin or its DEFORM twin, linked the
// first time it is drawn from the sources Init recorded; its sources are recorded too (its ANIM twin links the same way)
GLuint Renderer::PtfxTwin( GLuint p )
{
	auto it = ptfxTwins.find( p );
	if( it != ptfxTwins.end() )
		return it->second;
	GLuint twin = p;
	auto src    = progSrc.find( p );
	if( src != progSrc.end() && !src->second.capture )
	{
		const auto t0        = std::chrono::steady_clock::now();
		const std::string vs = "#define PTFX\n" + *src->second.vs, fs = "#define PTFX\n" + *src->second.fs;
		std::string err;
		const GLuint q = LinkPlain( vs, fs, err );
		if( q )
		{
			CopySamplerUnits( p, q );
			glUseProgram( q );
			glUniform1i( U( q, "uTrailVtx" ), 42 );
			glUniform1i( U( q, "uPtGlyph" ), 63 );
			glUseProgram( 0 );
			progSrc[ q ] = { InternSource( vs ), InternSource( fs ), false };
			twin         = q;
			Log( "POINTS look twin of program %u linked in %.0f ms", p, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
		}
		else
			Log( "POINTS look twin of program %u failed (round, still points there): %s", p, err.c_str() );
	}
	ptfxTwins[ p ] = twin;
	return twin;
}

// MoveEhr W7: the PTSHADE twin (Point Shading Per Point) of a points program (POINTS / PTFX / DEFORM), linked the first time it draws
// from the recorded sources like PtfxTwin; its sources are recorded too (ENVFILE / ANIM twins link the same way). W7b: Unlit retired
GLuint Renderer::ShadeTwin( GLuint p )
{
	auto it = shadeTwins.find( p );
	if( it != shadeTwins.end() )
		return it->second;
	GLuint twin = p;
	auto src    = progSrc.find( p );
	if( src != progSrc.end() && !src->second.capture )
	{
		const auto t0        = std::chrono::steady_clock::now();
		const std::string d  = "#define PTSHADE\n";
		const std::string vs = d + *src->second.vs, fs = d + *src->second.fs;
		std::string err;
		const GLuint q = LinkPlain( vs, fs, err );
		if( q )
		{
			CopySamplerUnits( p, q );
			glUseProgram( q );
			glUniform1i( U( q, "uPtShadeTex" ), kPtShadeUnit );
			glUseProgram( 0 );
			progSrc[ q ] = { InternSource( vs ), InternSource( fs ), false };
			twin         = q;
			Log( "POINTS shade twin (Per Point) of program %u linked in %.0f ms", p,
			     std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
		}
		else
			Log( "POINTS shade twin of program %u failed (Full shading there): %s", p, err.c_str() );
	}
	shadeTwins[ p ] = twin;
	return twin;
}

GLuint Renderer::DeformTwin( GLuint p ) const
{
	for( const auto& t : deformTwins )
		if( t.first == p )
			return t.second;
	return p;
}

bool Renderer::InitPrograms( std::string& error )
{
	const std::string mainFS = std::string( kLightLib ) + kMainFS;
	progMain      = Link( kMainVS, mainFS, error );
	progSoup      = Link( kSoupVS, mainFS, error );
	// instanced variants (clones / ghosts): separate programs, so the plain ones compile exactly as before
	progMainI     = Link( std::string( "#define INSTANCED\n" ) + kMainVS, "#define INSTANCED\n" + mainFS, error );
	progSoupI     = Link( std::string( "#define INSTANCED\n" ) + kSoupVS, "#define INSTANCED\n" + mainFS, error );
	// the Shatter transition's shards: its own programs too, so standalone Shatter compiles exactly as before
	progSoupT     = Link( std::string( "#define TRANS\n" ) + kSoupVS, mainFS, error );
	progSoupTI    = Link( std::string( "#define TRANS\n#define INSTANCED\n" ) + kSoupVS, "#define INSTANCED\n" + mainFS, error );
	progComposite = Link( kQuadVS, std::string( kToneLib ) + kCompositeFS, error );
	progCompositeHaze = Link( kQuadVS, std::string( "#define HAZE\n" ) + kToneLib + kCompositeFS, error ); // W26 heat haze
	progCopy      = Link( kQuadVS, kCopyFS, error );
	// W21 letters: their own programs as well
	progSoupL     = Link( std::string( "#define LETTERS\n" ) + kSoupVS, mainFS, error );
	progSoupLI    = Link( std::string( "#define LETTERS\n#define INSTANCED\n" ) + kSoupVS, "#define INSTANCED\n" + mainFS, error );
	if( !progMain || !progSoup || !progMainI || !progSoupI || !progSoupT || !progSoupTI || !progSoupL || !progSoupLI || !progComposite || !progCopy || !progCompositeHaze )
		return false;
	for( GLuint p : { progMain, progSoup, progMainI, progSoupI, progSoupT, progSoupTI, progSoupL, progSoupLI } )
		BindSamplers( p );
	for( GLuint c : { progComposite, progCompositeHaze } )
	{
		glUseProgram( c );
		glUniform1i( U( c, "uScene" ), 0 );
		glUniform1i( U( c, "uInput" ), 1 );
		glUniform1i( U( c, "uFog" ), 2 );
		glUniform1i( U( c, "uFogDepth" ), 3 );
		glUniform1i( U( c, "uDepth" ), 4 );
		glUniform1i( U( c, "uKeyShadow" ), 5 );
		glUniform1i( U( c, "uMirror" ), 6 );
		glUniform1i( U( c, "uSilBlur" ), 7 );
		glUniform1i( U( c, "uEcho" ), 8 );
		glUniform1i( U( c, "uHazeMask" ), 9 );
		glUniform1i( U( c, "uCaustic" ), 10 );
	}
	glUseProgram( progCopy );
	glUniform1i( U( progCopy, "uInput" ), 0 );
	glUseProgram( 0 );

	glGenVertexArrays( 1, &quadVAO );
	glGenTextures( 1, &whiteTex );
	glBindTexture( GL_TEXTURE_2D, whiteTex );
	const uint8_t white[ 4 ] = { 255, 255, 255, 255 };
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return InitVolume( error ) && InitSim( error ) && InitPost( error ) && InitShape( error ) && InitProjector( error ) && InitLiquid( error ) && InitBolts( error ) && InitSwarm( error ) && InitCracks( error ) && InitCaustics( error ) && InitMorph( error );
}

void Renderer::Release()
{
	ReleaseEnvFile(); // MoveEhr W5
	ReleasePose(); // MoveEhr W1
	ReleaseVolume();
	ReleaseSim();
	ReleasePost();
	ReleaseShape();
	ReleaseProjector();
	ReleaseLiquid();
	ReleaseBolts();
	ReleaseSwarm();
	ReleaseCracks();
	ReleaseCaustics();
	ReleaseMorph();
	for( const auto& t : deformTwins )
		glDeleteProgram( t.second );
	deformTwins.clear();
	for( const auto& t : wireTwins )
		glDeleteProgram( t.second );
	wireTwins.clear();
	for( const auto& t : pointTwins )
		glDeleteProgram( t.second );
	pointTwins.clear();
	for( const auto& t : captureTwins )
		glDeleteProgram( t.second );
	captureTwins.clear();
	ReleaseLines();
	ReleaseVoxels();
	ReleaseBuild();
	ReleaseSurfLook();
	ReleaseWire();
	ReleasePoints();
	ReleaseTargets();
	for( GLuint* p : { &progMain, &progSoup, &progMainI, &progSoupI, &progSoupT, &progSoupTI, &progSoupL, &progSoupLI, &progComposite, &progCopy, &progCompositeHaze } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	if( quadVAO )
		glDeleteVertexArrays( 1, &quadVAO ), quadVAO = 0;
	if( vao )
		glDeleteVertexArrays( 1, &vao ), vao = 0;
	for( int l = 0; l < kLodLevels; ++l )
		if( lodVao[ l ] )
			glDeleteVertexArrays( 1, &lodVao[ l ] ), lodVao[ l ] = 0, lodVaoFor[ l ] = 0;
	if( whiteTex )
		glDeleteTextures( 1, &whiteTex ), whiteTex = 0;
	if( inTex )
		glDeleteTextures( 1, &inTex ), inTex = 0;
	if( inFBO )
		glDeleteFramebuffers( 1, &inFBO ), inFBO = 0;
	vaoFor = 0;
}

void Renderer::ReleaseTargets()
{
	if( msFBO )
		glDeleteFramebuffers( 1, &msFBO ), msFBO = 0;
	if( msColor )
		glDeleteRenderbuffers( 1, &msColor ), msColor = 0;
	if( msDepth )
		glDeleteRenderbuffers( 1, &msDepth ), msDepth = 0;
	if( resolveFBO )
		glDeleteFramebuffers( 1, &resolveFBO ), resolveFBO = 0;
	if( resolveTex )
		glDeleteTextures( 1, &resolveTex ), resolveTex = 0;
	if( resolveDepth )
		glDeleteRenderbuffers( 1, &resolveDepth ), resolveDepth = 0;
	tw = th = tSamples = 0;
}

bool Renderer::EnsureTargets( int w, int h, int samples )
{
	GLint maxSamples = 1;
	glGetIntegerv( GL_MAX_SAMPLES, &maxSamples );
	samples = std::max( 1, std::min( samples, (int)maxSamples ) );
	if( w == tw && h == th && samples == tSamples && resolveFBO )
		return true;
	ReleaseTargets();

	glGenTextures( 1, &resolveTex );
	glBindTexture( GL_TEXTURE_2D, resolveTex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glGenFramebuffers( 1, &resolveFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, resolveTex, 0 );
	if( samples == 1 )
	{
		glGenRenderbuffers( 1, &resolveDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, resolveDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h ); // W9: + stencil (the cut's cap)
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, resolveDepth );
	}
	bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;

	if( samples > 1 )
	{
		glGenRenderbuffers( 1, &msColor );
		glBindRenderbuffer( GL_RENDERBUFFER, msColor );
		glRenderbufferStorageMultisample( GL_RENDERBUFFER, samples, GL_RGBA8, w, h );
		glGenRenderbuffers( 1, &msDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, msDepth );
		glRenderbufferStorageMultisample( GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h ); // W9: + stencil (the cut's cap)
		glGenFramebuffers( 1, &msFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, msFBO );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColor );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msDepth );
		ok = ok && glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	}
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	if( !ok )
	{
		Log( "render target %dx%d x%d incomplete", w, h, samples );
		ReleaseTargets();
		return false;
	}
	tw = w, th = h, tSamples = samples;
	return true;
}

void Renderer::EnsureVAO( GpuModel* model )
{
	const SlotPose* ps = PosedFor( model ); // MoveEhr W1: an animated side draws its posed buffer
	const GLuint src   = ps ? ps->vbo : model->vbo;
	if( vao && vaoFor == model->serial && vaoVbo == src )
		return;
	if( !vao )
		glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	if( ps ) // the ANIM twins read the rest position (patterns stay on the surface)
	{
		glBindBuffer( GL_ARRAY_BUFFER, model->vbo );
		glEnableVertexAttribArray( 4 );
		glVertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, sizeof( Vertex ), (void*)offsetof( Vertex, px ) );
	}
	else
		glDisableVertexAttribArray( 4 );
	glBindBuffer( GL_ARRAY_BUFFER, src );
	const GLsizei stride = sizeof( Vertex );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof( Vertex, px ) );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof( Vertex, u ) );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride, (void*)offsetof( Vertex, normal ) );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride, (void*)offsetof( Vertex, tangent ) );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, model->ibo );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	vaoFor = model->serial;
	vaoVbo = src;
}

// LoadEhr W1: the model as a shadow caster (light shadow maps, the key shadow, the projector's shadow). Mid-hop the whole pair is
// drawn, each side with its own half of the transition (dissolve, shards, dust, slice, the liquid's mesh fade), so a shadow
// morphs with the picture: nothing switches, nothing pops. Outside a hop: exactly the one call MaskEhr made.
void Renderer::DrawCasters( const FrameState& fs, GpuModel* model, const float* vp, const float* lightPos, bool withInst )
{
	auto one = [ & ]( const FrameState& f, GpuModel* g ) {
		if( f.vox.look > 0 && voxDrawn[ activeSide ] ) // W4 A1: the voxels cast what the picture shows (their mesh until a side's spots land)
		{
			DrawVoxels( f, g, pairOn ? activeSide : -1, voxHopT, vp, lightPos );
			return;
		}
		const bool soup = f.wireMix > 0.0f || f.explode > 0.0f || f.shape.on;
		const GLuint p  = !soup ? progShadow : f.morph.On() ? progShadowSoupM : f.shape.transSide > 0 && !f.shape.dustTrans ? progShadowSoupT : progShadowSoup;
		EnsureVAO( g );
		DrawModel( p, f, g, false, soup, 0, vp, lightPos, withInst && !f.inst.empty() ? &f.inst : nullptr );
	};
	if( !pairOn )
	{
		one( fs, model );
		return;
	}
	const int keep = activeSide;
	for( int side = 0; side < 2; ++side )
	{
		activeSide = side;
		one( *pairFs[ side ], pairModel[ side ] );
	}
	activeSide = keep;
	EnsureVAO( model );
}

// LoadEhr W1: a filled slot that is not drawn this frame gets what a hop will need, built once and kept (per-slot caches)
void Renderer::PrepareSlot( int slot, GpuModel* model, const FrameState& fs, unsigned uses )
{
	if( !model || !model->Complete() || slot < 0 || slot >= kSlots )
		return;
	const int keepSlot = sideSlot[ 0 ], keepSide = activeSide;
	sideSlot[ 0 ] = slot, activeSide = 0;
	model->UpdateLods();
	if( uses & kUseShards )
		UpdateShards( model, fs.shape.shardCount, fs.shape.shatterMode );
	if( uses & kUseDust )
		UpdateDust( fs, model, true );
	if( uses & kUseLiquid )
		UpdateLiquidSlot( slot, model );
	if( uses & kUseSim )
		EnsureSimSdf( slot, model );
	if( uses & kUsePoints ) // W6: Points Only / Points + Wire: the slot's points are built before its hop
		PointsFor( model, fs.wire.pointSource );
	if( uses & kUseSparkle ) // W7 Trans Sparkle: the sparkles ride Surface 25k
		PointsFor( model, 2 );
	if( uses & kUseCloud ) // W6: the Points / Swarm styles' cloud
		UpdateCloud( slot, model );
	sideSlot[ 0 ] = keepSlot, activeSide = keepSide;
}

size_t Renderer::SlotBytes( int slot ) const
{
	if( slot < 0 || slot >= kSlots )
		return 0;
	size_t b = 0;
	const DustSlot& d = dust[ slot ];
	b += size_t( d.count ) * ( 5 + 2 * 6 ) * sizeof( float );
	const ShardSlot& sh = shards[ slot ];
	b += sh.data.size() * sizeof( float ) + ( sh.ofBuf ? sh.data.size() / 4 * 3 * sizeof( uint16_t ) : 0 );
	const size_t sdf = size_t( kSdfRes ) * kSdfRes * kSdfRes * 2 * 2; // RG16F
	b += liquidSdf[ slot ].tex ? sdf : 0;
	b += simSdf[ slot ].tex ? sdf : 0;
	return b;
}

void Renderer::UpdateInputCopy( const FrameState& fs )
{
	int w = std::max( 1, std::min( fs.inputW, 1024 ) );
	int h = std::max( 1, std::min( fs.inputH, 1024 ) );
	if( w != inW || h != inH || !inTex )
	{
		if( inTex )
			glDeleteTextures( 1, &inTex );
		if( !inFBO )
			glGenFramebuffers( 1, &inFBO );
		inLevels = 1 + (int)std::floor( std::log2( (float)std::max( w, h ) ) );
		glGenTextures( 1, &inTex );
		glBindTexture( GL_TEXTURE_2D, inTex );
		for( int l = 0; l < inLevels; ++l )
			glTexImage2D( GL_TEXTURE_2D, l, GL_RGBA8, std::max( 1, w >> l ), std::max( 1, h >> l ), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT );
		glBindFramebuffer( GL_FRAMEBUFFER, inFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inTex, 0 );
		inW = w, inH = h;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, inFBO );
	glViewport( 0, 0, inW, inH );
	glUseProgram( progCopy );
	glUniform2f( U( progCopy, "uInputScale" ), fs.inputHwW ? float( fs.inputW ) / fs.inputHwW : 1.0f, fs.inputHwH ? float( fs.inputH ) / fs.inputHwH : 1.0f );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, fs.inputTex );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glBindTexture( GL_TEXTURE_2D, inTex );
	glGenerateMipmap( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

GLuint Renderer::LodVAO( GpuModel* model, int lod )
{
	if( lod == 0 )
		return vao;
	const SlotPose* ps = PosedFor( model ); // MoveEhr W1
	const GLuint src   = ps ? ps->vbo : model->vbo;
	if( lodVao[ lod ] && lodVaoFor[ lod ] == model->serial && lodVaoVbo[ lod ] == src )
		return lodVao[ lod ];
	if( !lodVao[ lod ] )
		glGenVertexArrays( 1, &lodVao[ lod ] );
	glBindVertexArray( lodVao[ lod ] );
	if( ps )
	{
		glBindBuffer( GL_ARRAY_BUFFER, model->vbo );
		glEnableVertexAttribArray( 4 );
		glVertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, sizeof( Vertex ), (void*)offsetof( Vertex, px ) );
	}
	else
		glDisableVertexAttribArray( 4 );
	glBindBuffer( GL_ARRAY_BUFFER, src );
	const GLsizei stride = sizeof( Vertex );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof( Vertex, px ) );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof( Vertex, u ) );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride, (void*)offsetof( Vertex, normal ) );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride, (void*)offsetof( Vertex, tangent ) );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, model->lods[ lod ].ibo );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	lodVaoFor[ lod ] = model->serial;
	lodVaoVbo[ lod ] = src;
	return lodVao[ lod ];
}

void Renderer::DrawModel( GLuint p, const FrameState& fs, GpuModel* model, bool outline, bool soup, uint32_t limit, const float* vpOverride,
                          const float* shadowLightPos, const std::vector< Instance >* inst )
{
	const bool instanced = inst && !inst->empty() && limit == 0;
	if( fs.shape.bounce && fs.shape.shatterT > 0.0f && fs.shape.transSide == 0 && !instanced ) // W26 Bounce: the recorded flights
		p = p == progSoup ? progSoupB : p == progShadowSoup ? progShadowSoupB : p;
	if( fs.lava.split > 0.0f && !instanced ) // W26 lava split: the soup programs that move the crack cells
		p = p == progSoup ? progSoupC : p == progShadowSoup ? progShadowSoupC : p == progCrackSoup ? progCrackSoupC : p;
	if( instanced )
		p = p == progMain ? progMainI : p == progSoup ? progSoupI : p == progSoupT ? progSoupTI : p == progShadow ? progShadowI : p == progShadowSoup ? progShadowSoupI : p == progShadowSoupT ? progShadowSoupTI
		  : p == progSoupM ? progSoupMI : p == progShadowSoupM ? progShadowSoupMI : p;
	const LetterSlot& ls = letters[ SlotOf( model ) ];
	if( ls.live )
	{
		p = LettersProgram( p );
		glActiveTexture( GL_TEXTURE0 + 19 );
		glBindTexture( GL_TEXTURE_BUFFER, ls.ofTex );
		glActiveTexture( GL_TEXTURE0 + 20 );
		glBindTexture( GL_TEXTURE_BUFFER, ls.xfTex );
		glActiveTexture( GL_TEXTURE0 );
	}
	// W5: the WIRE twin (main-pass soup programs only; any other program has none and stays itself). W6: the POINTS twin while
	// DrawWired draws points; no twin, or the model's point set still building = no points this frame
	if( skinNow ) // W9 Skin Of Copies: the TBO-instanced program (non-soup)
		p = progMainIT;
	// W9: a CAPTURE draw = the CAPTURE twin (no twin: nothing to capture)
	if( captureNow )
	{
		const GLuint c = CaptureTwin( p );
		if( c == p || !soup )
			return;
		p = c;
	}
	const GLuint beforeWire = p;
	if( soup && fs.wire.on && !outline && !captureNow )
		p = pointsNow ? PointTwin( p ) : WireTwin( p );
	const bool wired = p != beforeWire;
	const PointSet* pts = nullptr;
	if( pointsNow )
	{
		if( !wired )
			return;
		pts = warmingNow ? nullptr : PointsFor( model, fs.wire.pointSource );
		if( !pts && !warmingNow )
			return;
	}
	// W2 Deform: any deform on = the DEFORM twin of whatever soup program this draw ended up with (plain programs untouched)
	const bool deform = soup && fs.shape.on && fs.shape.deform;
	if( deform )
		p = DeformTwin( p );
	const bool ptfx = pointsNow && fs.wire.ptfx && !warmingNow; // W4: the point looks (Round + None + Size Var 0 = W6's twins exactly)
	if( ptfx )
		p = PtfxTwin( p );
	// MoveEhr W7 Point Shading: Per Point = the PTSHADE twin (Full = the program as it was); not while warming / before the set lands
	// (the shade buffer holds every point of a part x the instances; a draw past its largest size stays Full, logged once)
	const GLuint beforeShade = p;
	if( pointsNow && pts && !warmingNow && fs.wire.shading > 0 )
	{
		GLsizei need = 0;
		for( const auto& r : pts->ranges )
			need = std::max( need, (GLsizei)r[ 1 ] );
		need *= ( instanced ? std::min( (GLsizei)inst->size(), (GLsizei)64 ) : 1 ) * ( deform ? std::max( 1, fs.shape.foldN ) : 1 );
		if( PtShadeFit( need ) )
			p = ShadeTwin( p );
	}
	const bool ptShade = p != beforeShade;
	if( fs.envSource == 3 && envFileTex && !captureNow ) // MoveEhr W5: Env Source HDRI File (the plain programs never read it)
		p = EnvTwin( p );
	const GLsizei folds = deform ? std::max( 1, fs.shape.foldN ) : 1; // Mirror Fold: every draw is this many copies
	// MoveEhr W1: an animated side = the ANIM twin of whatever program this draw ended up with (the prewarm keeps the plain ones)
	const SlotPose* posed = PosedFor( model );
	if( posed && !warmingNow )
	{
		p = AnimTwin( p );
		glActiveTexture( GL_TEXTURE0 + 38 );
		glBindTexture( GL_TEXTURE_BUFFER, model->vtxTBO );
		const GLuint piv = ShardPivotTex( model, SlotOf( model ) ); // W2 A3: the soup ANIM twins' shard pivots (bind data until posed)
		glActiveTexture( GL_TEXTURE0 + 39 );
		glBindTexture( GL_TEXTURE_BUFFER, piv ? piv : ( shards[ SlotOf( model ) ].dataTex ? shards[ SlotOf( model ) ].dataTex : dummyShardData ) );
		glActiveTexture( GL_TEXTURE0 );
	}
	glUseProgram( p );
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	Mat4 VP = P * V;
	if( vpOverride )
		std::memcpy( VP.m, vpOverride, sizeof( VP.m ) );
	if( shadowLightPos )
		glUniform3fv( U( p, "uShadowLightPos" ), 1, shadowLightPos );
	SetLightUniforms( p, fs, !shadowLightPos );
	SetShapeUniforms( p, fs, model );
	float nm[ 9 ];
	M.NormalMatrix( nm );

	glUniformMatrix4fv( U( p, "uModel" ), 1, GL_FALSE, M.m );
	glUniformMatrix4fv( U( p, "uViewProj" ), 1, GL_FALSE, VP.m );
	glUniformMatrix4fv( U( p, "uView" ), 1, GL_FALSE, V.m );
	glUniformMatrix3fv( U( p, "uNormalMat" ), 1, GL_FALSE, nm );
	glUniform3fv( U( p, "uFitCenter" ), 1, model->fitCenter );
	glUniform1f( U( p, "uFitScale" ), model->fitScale );
	glUniform1f( U( p, "uModelScale" ), fs.modelScale );
	glUniform1f( U( p, "uHeightAmt" ), fs.heightAmt );
	glUniform1f( U( p, "uOutlineWidth" ), outline ? fs.outline * 0.03f * ( fs.modelScale / std::max( model->fitScale, 1e-12f ) ) : 0.0f );
	glUniform1i( U( p, "uOutlinePass" ), outline ? 1 : 0 );
	// GS
	glUniform1f( U( p, "uExplode" ), fs.explode );
	glUniform1f( U( p, "uExplodeRandom" ), fs.explodeRandom );
	glUniform1f( U( p, "uSeed" ), fs.explodeSeed );
	glUniform1f( U( p, "uWorldScale" ), fs.modelScale / std::max( model->fitScale, 1e-12f ) );
	// material
	glUniform3fv( U( p, "uTint" ), 1, fs.tint );
	glUniform1f( U( p, "uTintMix" ), fs.tintMix );
	glUniform1f( U( p, "uHue" ), fs.hue );
	glUniform1f( U( p, "uSaturation" ), fs.saturation );
	glUniform1f( U( p, "uBrightness" ), fs.brightness );
	glUniform1i( U( p, "uMetalOverride" ), fs.metalOverride ? 1 : 0 );
	glUniform1f( U( p, "uMetalValue" ), fs.metalValue );
	glUniform1f( U( p, "uRoughScale" ), fs.roughScale );
	glUniform1f( U( p, "uNormalStrength" ), fs.normalStrength );
	glUniform1f( U( p, "uEmissiveGlow" ), fs.emissiveGlow );
	glUniform1f( U( p, "uRimGlow" ), fs.rimGlow );
	glUniform3fv( U( p, "uRimColor" ), 1, fs.rimColor );
	// lighting
	glUniform3fv( U( p, "uCamPos" ), 1, fs.camPos );
	glUniform3fv( U( p, "uKeyDir" ), 1, fs.keyDir );
	glUniform3fv( U( p, "uKeyColor" ), 1, fs.keyColor );
	glUniform3fv( U( p, "uFillDir" ), 1, fs.fillDir );
	glUniform3fv( U( p, "uFillColor" ), 1, fs.fillColor );
	glUniform1f( U( p, "uAmbient" ), fs.ambient );
	glUniform1f( U( p, "uEnvIntensity" ), fs.envIntensity );
	glUniform1i( U( p, "uEnvSource" ), fs.envSource );
	glUniform1f( U( p, "uEnvRotate" ), fs.envRotate );
	if( fs.envSource == 3 )
		glUniform1f( U( p, "uEnvFileLod" ), float( std::max( 0, envFileLevels - 1 ) ) );
	glUniform1f( U( p, "uExposure" ), fs.exposure );
	glUniform1i( U( p, "uTonemap" ), fs.tonemap );
	// input
	glUniform1i( U( p, "uInputUse" ), fs.inputUse );
	glUniform1i( U( p, "uInputProjection" ), fs.inputProjection );
	glUniform1f( U( p, "uInputMix" ), fs.inputMix );
	glUniform1f( U( p, "uInputMaxLod" ), float( std::max( 0, inLevels - 1 ) ) );
	// fx
	glUniform1f( U( p, "uWireMix" ), fs.wireMix );
	glUniform1f( U( p, "uWireThickness" ), fs.wireThickness );
	if( wired )
		SetWireUniforms( p, fs, model );
	if( fs.morph.On() ) // W6 Stage 2: the pack's MORPH programs (any other program has no such uniforms: harmless)
		SetMorphUniforms( p, fs, model );
	if( captureNow )
	{
		glUniform1f( U( p, "uCapTime" ), capTime );
		glUniform1f( U( p, "uCapSwirl" ), capSwirl * fs.modelScale / std::max( model->fitScale, 1e-12f ) );
		glUniform1f( U( p, "uCapPhase" ), capPhase );
		glUniform1i( U( p, "uCapAxis" ), capAxis );
		glUniform1f( U( p, "uPointLift" ), 0.0f );
		glActiveTexture( GL_TEXTURE0 + 26 );
		glBindTexture( GL_TEXTURE_BUFFER, captureRefTex );
		glActiveTexture( GL_TEXTURE0 );
		glEnable( GL_RASTERIZER_DISCARD );
		glBeginTransformFeedback( GL_POINTS );
	}
	if( ptfx )
		SetPointLookUniforms( p, fs, model, posed != nullptr );
	if( pointsNow )
	{
		glUniform1f( U( p, "uPointPx" ), std::max( 1.5f, 2.0f * fs.wireThickness ) ); // the dot as wide as a line (a line spans both sides of its edge)
		glUniform1f( U( p, "uPointLift" ), 0.012f * fs.modelScale / std::max( model->fitScale, 1e-12f ) );
		glUniform1f( U( p, "uTwinkle" ), fs.wire.twinkle );
		glUniform1f( U( p, "uTwinklePhase" ), fs.wire.twinklePhase );
		glActiveTexture( GL_TEXTURE0 + 26 );
		glBindTexture( GL_TEXTURE_BUFFER, pts ? pts->tex : pointDummy );
		glActiveTexture( GL_TEXTURE0 );
		glEnable( GL_PROGRAM_POINT_SIZE );
	}
	if( ptShade )
		glUniform2f( U( p, "uPtShadeVP" ), float( std::max( 1, fs.width ) ), float( std::max( 1, fs.height ) ) );
	glUniform1f( U( p, "uDissolve" ), fs.dissolve );
	glUniform1f( U( p, "uDissolveEdge" ), fs.dissolveEdge );
	glUniform1f( U( p, "uToonSteps" ), fs.toonSteps );
	glUniform1f( U( p, "uFogDensity" ), fs.fogDensity );
	glUniform1f( U( p, "uFogStart" ), fs.fogStart );
	const float bgA = fs.background[ 3 ];
	glUniform4f( U( p, "uBackground" ), fs.background[ 0 ] * bgA, fs.background[ 1 ] * bgA, fs.background[ 2 ] * bgA, bgA );
	glUniform1i( U( p, "uDebugView" ), fs.debugView );
	// surface (Stage 3)
	const SurfaceState& su = fs.surf;
	glUniform1i( U( p, "uSurfaceOn" ), su.on ? 1 : 0 );
	glUniform1f( U( p, "uTime" ), su.time );
	glUniform1f( U( p, "uGlitchAmt" ), su.glitch );
	if( su.on )
	{
		glUniform1f( U( p, "uBeat" ), su.beat );
		glUniform1f( U( p, "uAudioMid" ), su.audioMid );
		glUniform1f( U( p, "uFilm" ), su.film );
		glUniform1f( U( p, "uFilmThick" ), su.filmThick );
		glUniform1f( U( p, "uFilmShift" ), su.filmShift );
		glUniform1f( U( p, "uFilmSwirl" ), su.filmSwirl );
		glUniform3f( U( p, "uFilmT" ), su.filmT, su.filmT2, su.filmX );
		glUniform1f( U( p, "uGlitter" ), su.glitter );
		glUniform1f( U( p, "uGlitterDensity" ), su.glitterDensity );
		glUniform1f( U( p, "uGlitterSize" ), su.glitterSize );
		glUniform1f( U( p, "uGlitterPhase" ), su.glitterPhase );
		glUniform1f( U( p, "uGlitterMetal" ), su.glitterMetal );
		glUniform1f( U( p, "uChrome" ), su.chrome );
		glUniform1f( U( p, "uChromeRipple" ), su.chromeRipple );
		glUniform2fv( U( p, "uChromeOff" ), 1, su.chromeOff );
		glUniform1f( U( p, "uGlass" ), su.glass );
		glUniform1f( U( p, "uGlassIOR" ), su.glassIOR );
		glUniform1f( U( p, "uGlassDisp" ), su.glassDisp );
		glUniform3fv( U( p, "uGlassTint" ), 1, su.glassTint );
		glUniform1i( U( p, "uGlassInput" ), ( fs.inputTex && inTex ) ? 1 : 0 );
		glUniform2f( U( p, "uViewport" ), float( std::max( 1, fs.width ) ), float( std::max( 1, fs.height ) ) );
		glUniform1f( U( p, "uHolo" ), su.holo );
		glUniform1f( U( p, "uHoloScan" ), su.holoScan );
		glUniform1f( U( p, "uHoloFlicker" ), su.holoFlicker );
		glUniform1f( U( p, "uHoloSplit" ), su.holoSplit );
		glUniform1f( U( p, "uHoloPhase" ), su.holoPhase );
		glUniform3fv( U( p, "uHoloColor" ), 1, su.holoColor );
		glUniform1f( U( p, "uHeat" ), su.heat );
		glUniform1f( U( p, "uHeatTemp" ), su.heatTemp );
		glUniform1f( U( p, "uHeatSpread" ), su.heatSpread );
		glUniform1i( U( p, "uPattern" ), su.pattern );
		glUniform1f( U( p, "uPatAmt" ), su.patAmt );
		glUniform1f( U( p, "uPatScale" ), su.patScale );
		glUniform3f( U( p, "uPatT" ), su.patT, su.patT2, su.patX );
		glUniform1f( U( p, "uPatBeat" ), su.patBeat );
		glUniform1f( U( p, "uPatAudio" ), su.patAudio );
		glUniform3fv( U( p, "uPatColor" ), 1, su.patColor );
	}

	glActiveTexture( GL_TEXTURE0 + 8 );
	glBindTexture( GL_TEXTURE_2D, inTex ? inTex : whiteTex );
	if( soup )
	{
		glActiveTexture( GL_TEXTURE0 + 9 );
		glBindTexture( GL_TEXTURE_BUFFER, model->idxTBO );
		glActiveTexture( GL_TEXTURE0 + 10 );
		glBindTexture( GL_TEXTURE_BUFFER, posed ? posed->tbo : model->vtxTBO ); // MoveEhr W1: the posed vertices
	}
	const GLint locFirst = U( p, "uFirstIndex" );

	const GLint locHas = U( p, "uHas" ), locSel = U( p, "uSel" );
	const GLint locBase = U( p, "uBaseFactor" ), locEmis = U( p, "uEmisFactor" ), locRough = U( p, "uRoughFactor" ), locMetal = U( p, "uMetalFactor" );
	const GLint locNS = U( p, "uNormalScale" ), locFG = U( p, "uFlipGreen" ), locCut = U( p, "uCutout" );
	const GLint locPH = U( p, "uPartHue" ), locPG = U( p, "uPartGlow" ), locPS = U( p, "uPartSkin" ), locPI = U( p, "uPartId" );
	const GLint locPSurf = U( p, "uPartSurface" );
	const GLint locHas4 = U( p, "uHas4" ), locSel4 = U( p, "uSel4" );

	// instances, grouped by LOD: group g draws instances [base, base + n) of the uploaded arrays
	struct Group
	{
		int lod, base, n;
	};
	std::vector< Group > groups;
	if( instanced )
	{
		const int maxLod    = model->LodsReady() - 1;
		const bool needL0   = soup && ( fs.shape.shatterT > 0.0f || pointsNow || fs.x.look == 3 ); // shard ids (the points, W9 the exploded groups) exist for the full mesh only
		const int n         = std::min( (int)inst->size(), 64 );
		float mats[ 64 * 16 ], nexts[ 64 * 16 ], ha[ 64 * 2 ], xa[ 64 * 4 ];
		int k = 0;
		for( int lod = 0; lod < kLodLevels; ++lod )
		{
			Group g{ lod, k, 0 };
			for( int i = 0; i < n; ++i )
			{
				const Instance& in = ( *inst )[ i ];
				const int l        = needL0 ? 0 : std::min( in.lod, maxLod );
				if( l != lod )
					continue;
				std::memcpy( &mats[ k * 16 ], in.m, sizeof( in.m ) );
				std::memcpy( &nexts[ k * 16 ], in.next, sizeof( in.next ) );
				ha[ k * 2 ] = in.hue, ha[ k * 2 + 1 ] = in.alpha;
				xa[ k * 4 ] = in.keep, xa[ k * 4 + 1 ] = in.fogKeep, xa[ k * 4 + 2 ] = in.age, xa[ k * 4 + 3 ] = 0.0f;
				++k, ++g.n;
			}
			if( g.n )
				groups.push_back( g );
		}
		glUniformMatrix4fv( U( p, "uInst" ), k, GL_FALSE, mats );
		glUniform2fv( U( p, "uInstHA" ), k, ha );
		glUniform4fv( U( p, "uInstX" ), k, xa );
		// the ghosts are the trail's 3D copies: Trail Dust / Glow / Color Mix (clones: off)
		const TrailState& t = fs.trail;
		const bool trail    = inst == &fs.ghosts;
		glUniform1i( U( p, "uTrailPass" ), trail ? 1 : 0 );
		glUniform1f( U( p, "uTrailSmear" ), trail ? t.ghostSmear : 0.0f );
		if( trail && t.ghostSmear > 0.0f )
			glUniformMatrix4fv( U( p, "uInstNext" ), k, GL_FALSE, nexts );
		glUniform1f( U( p, "uTrailDust" ), trail ? t.dust : 0.0f );
		glUniform1f( U( p, "uTrailDustV" ), trail ? t.dust : 0.0f );
		glUniform1f( U( p, "uTrailGrain" ), t.ghostGrain );
		glUniform3fv( U( p, "uTrailDriftW" ), 1, t.ghostDrift );
		glUniform3fv( U( p, "uTrailColor" ), 1, t.color );
		glUniform1f( U( p, "uTrailMix" ), trail ? t.colorMix : 0.0f );
		glUniform1f( U( p, "uTrailGlow" ), trail ? t.glow : 0.0f );
	}
	const GLint locInstBase = U( p, "uInstBase" );

	bool glassParts = false; // a part drawn as glass changes the surface uniforms: every part then sets its own
	for( const GpuPart& gp : model->parts )
		glassParts |= gp.glass > 0.0f && gp.glass > su.glass;
	// W2 text / logos: while pieces fly (Shatter Mode Pieces), the fractured copy draws instead of the whole mesh (at rest, and once
	// fully reformed, the whole mesh: exactly what it drew before)
	const bool pieces = model->hasPieces && soup && fs.shape.on && fs.shape.shatterMode > 0 && ( fs.shape.shatterT > 0.0f || fs.morph.vs == 2 ); // W6: + the Pieces style
	// the soup path reads vertices from texture buffers; no vertex attributes may be enabled
	glBindVertexArray( soup ? quadVAO : vao );
	// W19: glass parts go last, blended (premultiplied) without depth writes, so what is behind them (an iris under a
	// cornea, a dial under its glass) shows through. Models without glass take the single pass as before.
	GLboolean prevBlend = GL_FALSE, prevDepthMask = GL_TRUE;
	GLint prevSrc = GL_ONE, prevDst = GL_ZERO;
	const int glassPasses = glassParts && !shadowLightPos && !captureNow ? 2 : 1;
	for( int gpass = 0; gpass < glassPasses; ++gpass )
	{
	if( gpass == 1 )
	{
		prevBlend = glIsEnabled( GL_BLEND );
		glGetBooleanv( GL_DEPTH_WRITEMASK, &prevDepthMask );
		glGetIntegerv( GL_BLEND_SRC_RGB, &prevSrc );
		glGetIntegerv( GL_BLEND_DST_RGB, &prevDst );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
		glDepthMask( GL_FALSE );
	}
	for( size_t i = 0; i < model->parts.size(); ++i )
	{
		const GpuPart& part    = model->parts[ i ];
		if( captureNow ? part.fractured : part.fractured != pieces ) // W9: a capture takes the whole mesh's spots (their run per part)
			continue;
		if( glassPasses == 2 && ( part.glass > 0.0f && part.glass > su.glass ) != ( gpass == 1 ) )
			continue;
		const PartControls& pc = fs.parts[ part.slot ];
		if( ( !pc.visible && !captureNow ) || part.indexCount == 0 || ( shadowLightPos && part.glass > 0.0f ) ) // glass lets the light through
			continue;
		if( glassFilter == 1 && !( part.glass > 0.0f ) ) // W26 caustics: the refractor only
			continue;
		// W18: a see-through material (transmission / low-alpha BLEND) wears the Glass surface on its own part, tinted by
		// its base colour, through this draw's uniforms (the shader is unchanged). Surface off: only glass is on.
		const bool autoGlass = part.glass > 0.0f && part.glass > su.glass;
		if( autoGlass || glassParts )
		{
			glUniform1i( U( p, "uSurfaceOn" ), su.on || autoGlass ? 1 : 0 );
			glUniform1f( U( p, "uGlass" ), autoGlass ? part.glass : su.glass );
			glUniform3fv( U( p, "uGlassTint" ), 1, autoGlass ? part.glassTint : su.glassTint );
			if( autoGlass && !su.on )
			{
				for( const char* u : { "uFilm", "uGlitter", "uChrome", "uHolo", "uHeat", "uPatAmt" } )
					glUniform1f( U( p, u ), 0.0f );
				glUniform1i( U( p, "uPattern" ), 0 );
				glUniform1f( U( p, "uGlassIOR" ), su.glassIOR );
				glUniform1f( U( p, "uGlassDisp" ), su.glassDisp );
				glUniform1i( U( p, "uGlassInput" ), ( fs.inputTex && inTex ) ? 1 : 0 );
				glUniform2f( U( p, "uViewport" ), float( std::max( 1, fs.width ) ), float( std::max( 1, fs.height ) ) );
			}
		}
		GLint has[ CH_COUNT ];
		float sel[ CH_COUNT * 4 ];
		for( int ch = 0; ch < CH_COUNT; ++ch )
		{
			has[ ch ] = part.tex[ ch ] ? 1 : 0;
			int c     = part.component[ ch ] < 0 ? 0 : part.component[ ch ];
			for( int k = 0; k < 4; ++k )
				sel[ ch * 4 + k ] = k == c ? 1.0f : 0.0f;
			glActiveTexture( GL_TEXTURE0 + ch );
			glBindTexture( GL_TEXTURE_2D, part.tex[ ch ] ? part.tex[ ch ] : whiteTex );
		}
		glUniform1iv( locHas, CH_COUNT, has );
		glUniform4fv( locSel, CH_COUNT, sel );
		glUniform1i( locHas4, has[ CH_HEIGHT ] );
		glUniform4fv( locSel4, 1, &sel[ CH_HEIGHT * 4 ] );
		glUniform4fv( locBase, 1, part.baseColor );
		glUniform3fv( locEmis, 1, part.emissive );
		glUniform1f( locRough, part.roughness );
		glUniform1f( locMetal, part.metallic );
		glUniform1f( locNS, part.normalScale );
		glUniform1f( locFG, ( part.flipGreen != fs.normalFlipY ) ? -1.0f : 1.0f );
		glUniform1i( locCut, part.alphaCutout ? 1 : 0 );
		glUniform1f( locPH, pc.hue );
		glUniform1f( locPG, pc.glow );
		glUniform1i( locPS, pc.inputSkin ? 1 : 0 );
		glUniform1i( locPI, (int)i );
		glUniform1f( locPSurf, pc.surface );
		// W6: this part's run of points (the prewarm: the one dummy point)
		const std::array< uint32_t, 2 > prun = pts ? ( i < pts->ranges.size() ? pts->ranges[ i ] : std::array< uint32_t, 2 >{ 0, 0 } ) : std::array< uint32_t, 2 >{ 0, 1 };
		if( captureNow )
		{
			const std::array< uint32_t, 2 > crun = i < captureRanges->size() ? ( *captureRanges )[ i ] : std::array< uint32_t, 2 >{ 0, 0 };
			glUniform1i( U( p, "uCapHidden" ), pc.visible ? 0 : 1 );
			if( crun[ 1 ] )
				glDrawArrays( GL_POINTS, (GLint)crun[ 0 ], (GLsizei)crun[ 1 ] );
			continue;
		}
		if( pointsNow )
		{
			GLsizei pc = limit ? (GLsizei)std::min( limit, prun[ 1 ] ) : (GLsizei)prun[ 1 ];
			if( pointShareNow < 1.0f ) // W7 sparkles: the first share of the run (the surface points are random within a part)
				pc = (GLsizei)std::ceil( float( pc ) * pointShareNow );
			if( pc == 0 )
				continue;
			// MoveEhr W7: a PTSHADE draw goes twice: pass 1 shades each point once into its texel of ptShadeTex (1-px points, no blend /
			// depth / scissor; the buffer cleared first, so a point the surface stage cuts away reads 0 = not drawn), pass 2 = the sprites
			auto shadePass = [ & ]( GLsizei n ) {
				if( !ptShade )
					return;
				GLint vp[ 4 ], fbo = 0;
				GLboolean cm[ 4 ];
				glGetIntegerv( GL_VIEWPORT, vp );
				glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &fbo );
				glGetBooleanv( GL_COLOR_WRITEMASK, cm );
				const GLboolean blend = glIsEnabled( GL_BLEND ), depth = glIsEnabled( GL_DEPTH_TEST ), scissor = glIsEnabled( GL_SCISSOR_TEST );
				glBindFramebuffer( GL_DRAW_FRAMEBUFFER, ptShadeFBO );
				glViewport( 0, 0, kPtShadeW, ptShadeH );
				glDisable( GL_BLEND );
				glDisable( GL_DEPTH_TEST );
				glDisable( GL_SCISSOR_TEST );
				glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
				const GLfloat zero[ 4 ] = { 0, 0, 0, 0 };
				glClearBufferfv( GL_COLOR, 0, zero );
				glActiveTexture( GL_TEXTURE0 + kPtShadeUnit );
				glBindTexture( GL_TEXTURE_2D, ptShadeDummy ); // never the buffer it writes
				glBindSampler( kPtShadeUnit, 0 );
				glActiveTexture( GL_TEXTURE0 );
				glUniform4i( U( p, "uPtShade" ), (GLint)prun[ 0 ], pc, kPtShadeW, ptShadeH );
				glUniform1i( U( p, "uPtPass" ), 1 );
				if( n > 1 || instanced )
					glDrawArraysInstanced( GL_POINTS, (GLint)prun[ 0 ], pc, n );
				else
					glDrawArrays( GL_POINTS, (GLint)prun[ 0 ], pc );
				glBindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)fbo );
				glViewport( vp[ 0 ], vp[ 1 ], vp[ 2 ], vp[ 3 ] );
				glColorMask( cm[ 0 ], cm[ 1 ], cm[ 2 ], cm[ 3 ] );
				if( blend )
					glEnable( GL_BLEND );
				if( depth )
					glEnable( GL_DEPTH_TEST );
				if( scissor )
					glEnable( GL_SCISSOR_TEST );
				glActiveTexture( GL_TEXTURE0 + kPtShadeUnit );
				glBindTexture( GL_TEXTURE_2D, ptShadeTex );
				glActiveTexture( GL_TEXTURE0 );
				glUniform1i( U( p, "uPtPass" ), 2 );
			};
			if( instanced )
				for( const Group& g : groups )
				{
					glUniform1i( locInstBase, g.base );
					shadePass( g.n * folds );
					glDrawArraysInstanced( GL_POINTS, (GLint)prun[ 0 ], pc, g.n * folds );
				}
			else if( folds > 1 )
			{
				shadePass( folds );
				glDrawArraysInstanced( GL_POINTS, (GLint)prun[ 0 ], pc, folds );
			}
			else
			{
				shadePass( 1 );
				glDrawArrays( GL_POINTS, (GLint)prun[ 0 ], pc );
			}
			if( limit )
				break;
			continue;
		}
		if( instanced )
		{
			for( const Group& g : groups )
			{
				const LodLevel& L    = model->lods[ g.lod ];
				const uint32_t first = g.lod == 0 ? part.firstIndex : L.parts[ i ][ 0 ];
				const GLsizei cnt    = (GLsizei)( g.lod == 0 ? part.indexCount : L.parts[ i ][ 1 ] );
				if( cnt == 0 )
					continue;
				glUniform1i( locInstBase, g.base );
				if( soup )
				{
					glActiveTexture( GL_TEXTURE0 + 9 );
					glBindTexture( GL_TEXTURE_BUFFER, g.lod == 0 ? model->idxTBO : L.tbo );
					glUniform1i( locFirst, (GLint)first );
					if( wired ) // W5: the edge flags follow the full mesh's triangles; a simplified LOD draws every edge
						glUniform1i( U( p, "uWireEdges" ), g.lod == 0 && fs.wire.edges == 1 ? 1 : 0 );
					glDrawArraysInstanced( GL_TRIANGLES, 0, cnt, g.n * folds );
				}
				else
				{
					glBindVertexArray( LodVAO( model, g.lod ) );
					glDrawElementsInstanced( GL_TRIANGLES, cnt, GL_UNSIGNED_INT, (void*)( size_t( first ) * sizeof( uint32_t ) ), g.n );
				}
			}
			continue;
		}
		const GLsizei count = limit ? (GLsizei)std::min( limit, part.indexCount ) : (GLsizei)part.indexCount;
		if( soup )
		{
			glUniform1i( locFirst, (GLint)part.firstIndex );
			if( folds > 1 )
				glDrawArraysInstanced( GL_TRIANGLES, 0, count, folds );
			else
				glDrawArrays( GL_TRIANGLES, 0, count );
		}
		else if( skinNow )
		{
			const LodLevel& L    = model->lods[ skinLod ];
			const uint32_t first = skinLod == 0 ? part.firstIndex : L.parts[ i ][ 0 ];
			const GLsizei cnt    = (GLsizei)( skinLod == 0 ? part.indexCount : L.parts[ i ][ 1 ] );
			glBindVertexArray( LodVAO( model, skinLod ) );
			if( cnt )
				glDrawElementsInstanced( GL_TRIANGLES, limit ? std::min( cnt, (GLsizei)limit ) : cnt, GL_UNSIGNED_INT, (void*)( size_t( first ) * sizeof( uint32_t ) ), skinCount );
		}
		else
			glDrawElements( GL_TRIANGLES, count, GL_UNSIGNED_INT, (void*)( size_t( part.firstIndex ) * sizeof( uint32_t ) ) );
		if( limit )
			break;
	}
	if( gpass == 1 )
	{
		if( !prevBlend )
			glDisable( GL_BLEND );
		glBlendFunc( prevSrc, prevDst );
		glDepthMask( prevDepthMask );
	}
	}
	glBindVertexArray( 0 );
	if( captureNow )
	{
		glEndTransformFeedback();
		glDisable( GL_RASTERIZER_DISCARD );
		glActiveTexture( GL_TEXTURE0 + 26 );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	if( pointsNow )
	{
		glDisable( GL_PROGRAM_POINT_SIZE );
		glActiveTexture( GL_TEXTURE0 + 26 );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	if( soup )
	{
		for( int u : { 9, 10 } )
		{
			glActiveTexture( GL_TEXTURE0 + u );
			glBindTexture( GL_TEXTURE_BUFFER, 0 );
		}
	}
	UnbindShadowUnits();
	for( int u : { 15, 16 } )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	for( int u = 8; u >= 0; --u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
}

namespace
{
// the hop's incoming model drawn with the frame's pose, fitted by its own bbox. LoadEhr W1: it brings the clones along (instB:
// the same copies round its own fit), so resting on it shows them as resting on it as the next hop's outgoing model does;
// ghosts (past poses) stay with the outgoing model
FrameState AsModelB( const FrameState& fs )
{
	FrameState b = fs;
	std::memcpy( b.model, fs.modelB, sizeof( b.model ) );
	b.modelScale = fs.modelScaleB;
	b.inst       = fs.instB;
	b.instB.clear();
	b.ghosts.clear();
	return b;
}
// W16: Shatter and Dust take their timing from Trans Ease / Stagger / Spread (see ShapeState::transSide)
void TransitionPieces( FrameState& f, float t, int side )
{
	ShapeState& s  = f.shape;
	s.transSide    = side;
	s.transEase    = f.transEase;
	s.transE       = t + f.transEase * ( t * t * ( 3.0f - 2.0f * t ) - t );
	s.transStagger = std::min( 0.95f, std::max( 0.0f, f.transStagger ) );
	s.transSpread  = f.transSpread;
	if( f.transitionStyle == 1 )
	{
		s.shatterT = std::max( 1e-4f, 1.6f * f.transSpread ); // the flight time at the far end (1.6 s = the old flight)
		s.on       = true;
		return;
	}
	// Dust: the mesh shows exactly the points that are home: the outgoing one keeps u == 0 (k >= kT), the incoming
	// one u == 1 (k <= kT). SetShapeUniforms turns kT into the model's noise threshold.
	const float S = std::max( 1e-4f, s.transStagger );
	s.transKT     = side == 1 ? s.transE / S : ( s.transE - 1.0f + s.transStagger ) / S;
	s.dustCut     = 1.0f; // > 0: the mesh test is on (the threshold itself comes from transKT)
	s.dustInvert  = side == 2;
	s.dustTrans   = true;
}
// W6 Stage 2: the pack. e = the hop eased by Trans Ease; each style's ramps are fixed stretches of it (Rm), smoothstepped (Sm), and the
// swap in the middle is the W1 dissolve noise (Swap). Side 1 = the outgoing model (a), 2 = the incoming one (b).
float Sm( float x )
{
	x = std::min( 1.0f, std::max( 0.0f, x ) );
	return x * x * ( 3.0f - 2.0f * x );
}
float Rm( float e, float a, float b ) { return std::min( 1.0f, std::max( 0.0f, ( e - a ) / ( b - a ) ) ); }
// W7: the dissolve value that cuts share d of this model's surface. The noise clusters near 0.5 (an Fbm), so a cut moved evenly
// swapped nearly the whole surface in the middle ~15 % of a window: in Twist / Melt's 0.4 .. 0.6 (Ease 1) the swap took t 0.48 .. 0.53,
// a pop. The cut walks the model's own noise quantiles (CpuGeometry::swapQ) from its measured min to its max, so the share follows d.
float Cut( float d, const float* q )
{
	if( d <= 0.0f || d >= 1.0f || !q || q[ 128 ] <= q[ 0 ] )
		return std::min( 1.0f, std::max( 0.0f, d ) );
	const float x = d * 128.0f;
	const int i   = std::min( 127, int( x ) );
	const float lo = q[ i ], hi = q[ i + 1 ]; // d 0 / 1 exactly: nothing / everything (below q0 / above q128: ~1 / 16k of the surface)
	return ( lo + ( hi - lo ) * ( x - float( i ) ) + 0.04f ) / 1.08f; // the shader cuts at uDissolve x 1.08 - 0.04
}
void Swap( FrameState& f, float e, int side, float a, float b, const float* q )
{
	const float d = Cut( Sm( Rm( e, a, b ) ), q );
	if( side == 1 )
		f.dissolve = std::max( f.dissolve, d );
	else
		f.dissolve = std::max( 1e-4f, d ), f.dissolveInvert = true; // > 0: inverted, 0 would show it all
}
void TransitionPack( FrameState& f, float t, int side, const float* q )
{
	MorphState& m = f.morph;
	ShapeState& s = f.shape;
	const float e = t + f.transEase * ( t * t * ( 3.0f - 2.0f * t ) - t );
	const bool out = side == 1;
	m.side = side, m.e = e, m.stagger = std::min( 0.95f, std::max( 0.0f, f.transStagger ) ), m.spread = f.transSpread;
	// the near half ramps up on the way out (out) or down on the way in (in)
	auto ramp = [ & ]( float a0, float a1, float b0, float b1 ) { return out ? Sm( Rm( e, a0, a1 ) ) : 1.0f - Sm( Rm( e, b0, b1 ) ); };
	const float PI = 3.14159265f;
	switch( f.transitionStyle )
	{
	case kStylePoints:
	case kStyleSwarm: // the mesh turns into the cloud (0 .. 0.3), the cloud flies, the other mesh condenses out of it (0.7 .. 1)
		if( out )
			f.dissolve = std::max( f.dissolve, Cut( Sm( Rm( e, 0.02f, 0.3f ) ), q ) );
		else
			f.dissolve = std::max( 1e-4f, Cut( Sm( Rm( e, 0.7f, 0.98f ) ), q ) ), f.dissolveInvert = true;
		m.cloud  = std::min( Sm( Rm( e, 0.0f, 0.2f ) ), 1.0f - Sm( Rm( e, 0.8f, 1.0f ) ) );
		m.cloudK = Rm( e, 0.15f, 0.85f );
		m.swarm  = f.transitionStyle == kStyleSwarm;
		break;
	case kStyleWire: // the fill dissolves, the lines stay; the lines swap; the incoming fill returns
		f.wire.on = true, f.wireMix = 0.0f;
		if( f.wire.mode < 2 )
			f.wire.mode = 1, f.wire.mix = out ? Sm( Rm( e, 0.0f, 0.35f ) ) : 1.0f - Sm( Rm( e, 0.65f, 1.0f ) );
		Swap( f, e, side, 0.4f, 0.6f, q );
		break;
	case kStyleVoxel: // voxelise, the cubes grow (Trans Spread), swap, shrink back as the other model
	{
		const float v = ramp( 0.0f, 0.4f, 0.6f, 1.0f ), g = ramp( 0.1f, 0.5f, 0.5f, 0.9f );
		s.on = true, s.voxel = std::max( s.voxel, v ), s.voxelSize = std::max( 0.02f, s.voxelSize ) * ( 1.0f + 5.0f * f.transSpread * g );
		Swap( f, e, side, 0.4f, 0.6f, q );
		break;
	}
	case kStyleLowPoly: // big facets (Trans Spread = their size), tumbling, swap, back
	{
		const float v = ramp( 0.0f, 0.35f, 0.65f, 1.0f ), g = ramp( 0.1f, 0.5f, 0.5f, 0.9f );
		s.on = true, s.lowPoly = std::max( s.lowPoly, v ), s.lowPolySize = 0.04f + 0.4f * f.transSpread * g;
		s.tumble = 2, s.tumbleRandom = 1.0f, s.tumbleSway = 0.0f, s.tumbleW = 1.0f, s.flipOn = false, s.facetGap = 0.35f * g;
		const float dir = out ? 1.0f : -1.0f;
		s.tumbleAng[ 0 ] = 150.0f * g * dir, s.tumbleAng[ 1 ] = 110.0f * g * dir, s.tumbleAng[ 2 ] = 70.0f * g * dir;
		s.tumblePhase[ 0 ] = s.tumblePhase[ 1 ] = s.tumblePhase[ 2 ] = 0.0f;
		Swap( f, e, side, 0.4f, 0.6f, q );
		break;
	}
	case kStylePieces: // solid pieces fly to the other model's pieces (ranked bottom-up) and become them
		m.vs = 2, m.k = Rm( e, 0.05f, 0.95f ), s.on = true;
		break;
	case kStyleImplode: // triangles fall into the centre point; the incoming ones burst out of it
		m.vs = 3, m.k = out ? Rm( e, 0.0f, 0.6f ) : Rm( e, 0.4f, 1.0f ), s.on = true;
		break;
	case kStyleTwist: // winds up round the deform axis, wrung thin (Trans Spread: up to 7 turns end to end), the leading end first
		// (Trans Stagger: above 0.5 the top, below it the bottom); the incoming model arrives wound the same way and unwinds
		m.vs = 7, s.on = true;
		m.k     = out ? Rm( e, 0.0f, 0.4f ) : 1.0f - Rm( e, 0.6f, 1.0f );
		m.scale = PI * ( 1.0f + 1.5f * f.transSpread );
		m.freq  = std::min( 0.8f, 0.35f + 0.12f * f.transSpread );
		m.lead  = ( std::min( 1.0f, std::max( 0.0f, f.transStagger ) ) - 0.5f ) * 2.0f;
		Swap( f, e, side, 0.32f, 0.68f, q );
		break;
	case kStyleMelt: // melts to a puddle on the floor (Floor Height) by mid-hop; the other rises out of the same puddle and firms up
		m.vs = 8, s.on = true;
		m.k     = out ? Rm( e, 0.0f, 0.4f ) : 1.0f - Rm( e, 0.6f, 1.0f );
		m.scale = 1.1f + 0.6f * f.transSpread; // the puddle's radius (fitted units): Trans Spread = how runny / how far it spreads
		Swap( f, e, side, 0.4f, 0.6f, q );
		break;
	case kStyleScan: // a plane sweeps up the deform axis: the incoming model behind it, the outgoing ahead, a wire band at the plane
		m.mask = 1, m.band = 0.03f + 0.1f * f.transSpread, m.k = -m.band - 0.01f + e * ( 1.0f + 2.0f * m.band + 0.02f ), s.on = true;
		break;
	case kStyleGlitch: // bands across the axis take one model or the other, re-rolled 30 times over the hop, until only the incoming
		m.vs = 4, m.mask = 2, m.k = e, m.step = std::floor( e * 30.0f ), s.on = true;
		break;
	case kStyleRipple: // a wave front out from the centre swaps the models as it passes
		m.vs = 5, m.mask = 3, m.k = -0.3f + 2.2f * e, s.on = true;
		break;
	case kStyleSmoke: // dissolves into the sim's smoke (the plugin emits it), the other condenses out of it
		Swap( f, e, side, out ? 0.05f : 0.4f, out ? 0.6f : 0.95f, q );
		break;
	case kStyleBurn: // a fire front eats the outgoing model (blackbody edge, charred crust), the incoming one is behind it
		m.mask = 4, m.band = 0.06f + 0.12f * f.transSpread, m.k = -0.25f + 1.6f * e, s.on = true;
		break;
	case kStyleLava: // W7: cracks spread over the outgoing model and it splits open; the incoming one is under it, still split and
	                 // glowing, and heals and cools. Both sides carry the lava (a hop played back cracks the other model: Ping-Pong).
	                 // The crack glow is the edge: no dissolve edge glow (it lit 22-32 % of an arriving model white)
	{
		LavaState& l = f.lava;
		l.on = true, s.on = true, f.dissolveEdge = 0.0f;
		l.front = 3.6f * ( out ? Sm( Rm( e, 0.0f, 0.45f ) ) : 1.0f - Sm( Rm( e, 0.6f, 1.0f ) ) );
		l.split = 0.6f * ( out ? Sm( Rm( e, 0.25f, 0.7f ) ) : 1.0f - Sm( Rm( e, 0.4f, 0.85f ) ) );
		l.glow  = std::max( l.glow, m.lavaGlow ) * ( out ? 1.0f : 1.0f - 0.7f * Sm( Rm( e, 0.6f, 1.0f ) ) );
		Swap( f, e, side, out ? 0.5f : 0.25f, out ? 0.9f : 0.6f, q );
		break;
	}
	case kStyleSpinSwap: // spins up and shrinks; the other spins down and grows (Trans Spread = turns)
	{
		const float turns = 1.0f + 2.0f * f.transSpread;
		const float u     = out ? Rm( e, 0.0f, 0.6f ) : 1.0f - Rm( e, 0.4f, 1.0f );
		m.vs = 6, m.k = ( out ? 1.0f : -1.0f ) * 2.0f * PI * turns * u * u, s.on = true;
		m.scale = out ? 1.0f - Sm( Rm( e, 0.15f, 0.6f ) ) : Sm( Rm( e, 0.4f, 0.85f ) );
		break;
	}
	default: break;
	}
}
// W7 Trans Sparkle: the burst round a style's swap point (the eased progress where the models trade places): up from 0.2 before it,
// the peak there, gone 0.2 after. Lava's incoming model shows through from 0.25 and the outgoing one is gone by 0.9: 0.55; the rest 0.5.
float SparklePeak( int style ) { return style == kStyleLava ? 0.55f : 0.5f; }
float SparkleEnvelope( int style, float t, float ease )
{
	const float e = t + ease * ( t * t * ( 3.0f - 2.0f * t ) - t ), p = SparklePeak( style );
	return Sm( Rm( e, p - 0.2f, p ) ) * ( 1.0f - Sm( Rm( e, p, p + 0.2f ) ) );
}
// A hop: the outgoing model (a) leaves at progress t ...
void TransitionOut( FrameState& f, float t, const float* q )
{
	if( f.transitionStyle >= kStylePoints )
	{
		TransitionPack( f, t, 1, q );
		return;
	}
	switch( f.transitionStyle )
	{
	case 0: f.dissolve = std::max( f.dissolve, t ); break;
	case 1:
	case 2: TransitionPieces( f, t, 1 ); break;
	case 4: // W26 Liquid: the mesh dissolves into the liquid early (or plainly, until both SDFs are ready)
	{
		const float u = std::min( 1.0f, std::max( 0.0f, ( t - 0.03f ) / 0.27f ) );
		f.dissolve    = std::max( f.dissolve, f.liquid.ready ? u * u * ( 3.0f - 2.0f * u ) : t );
		break;
	}
	default: f.shape.slide = t, f.shape.on = true; break;
	}
}
// ... and the incoming one (b) arrives through the same system run backwards.
void TransitionIn( FrameState& f, float t, const float* q )
{
	if( f.transitionStyle >= kStylePoints )
	{
		TransitionPack( f, t, 2, q );
		return;
	}
	switch( f.transitionStyle )
	{
	case 0: f.dissolve = t, f.dissolveInvert = true; break;
	case 1:
	case 2: TransitionPieces( f, t, 2 ); break;
	case 4:
	{
		const float u = std::min( 1.0f, std::max( 0.0f, ( t - 0.7f ) / 0.27f ) );
		f.dissolve = f.liquid.ready ? std::max( 1e-4f, u * u * ( 3.0f - 2.0f * u ) ) : t, f.dissolveInvert = true; // > 0: inverted, 0 would show it all
		break;
	}
	default: f.shape.slide = -( 1.0f - t ), f.shape.on = true; break;
	}
}
}

void Renderer::Render( const FrameState& fsIn, GpuModel* a, GpuModel* b, int slotA, int slotB )
{
	wireDraws.clear(); // W5: filled by this frame's wired draws
	lineDraws.clear(); // W9: and the line looks'
	sparkleNow = false;
	sideSlot[ 0 ] = slotA, sideSlot[ 1 ] = slotB, activeSide = 0;
	GpuModel* bReady = b && b->Complete() ? b : nullptr;
	const float t    = bReady ? std::min( 1.0f, std::max( 0.0f, fsIn.transition ) ) : 0.0f;
	FrameState swapped, fsA, fsB;
	const FrameState* fsp = &fsIn;
	GpuModel* model       = a;
	bool both             = false;
	if( t >= 1.0f || ( t > 0.0f && !( a && a->Complete() ) ) )
	{
		swapped    = AsModelB( fsIn );
		fsp        = &swapped;
		model      = bReady;
		activeSide = 1;
	}
	else if( t > 0.0f )
	{
		both = true;
		fsA  = fsIn;
		fsA.liquid.ready = fsIn.transitionStyle == 4 && UpdateLiquidSdf( a, bReady );
		// W6: a pack style whose per-slot data is not on the GPU yet dissolves this frame (never a wait)
		if( fsA.transitionStyle >= kStylePoints && !MorphPairReady( fsA, a, bReady ) )
			fsA.transitionStyle = kStyleDissolve;
		TransitionOut( fsA, t, a->geometry ? a->geometry->swapQ : nullptr );
		fsB = AsModelB( fsIn );
		fsB.transitionStyle = fsA.transitionStyle;
		fsB.liquid.ready = fsA.liquid.ready;
		TransitionIn( fsB, t, bReady->geometry ? bReady->geometry->swapQ : nullptr );
	}
	pairOn         = both;
	pairFs[ 0 ]    = &fsA, pairFs[ 1 ] = &fsB;
	pairModel[ 0 ] = a, pairModel[ 1 ] = bReady;
	if( fsIn.transitionStyle == 4 && bReady && a && a->Complete() && !both )
		UpdateLiquidSdf( a, bReady ); // W26: build both SDFs while the slider rests, so the first drag is already liquid
	const FrameState& fs = *fsp;
	const int w = std::max( 1, fs.width ), h = std::max( 1, fs.height );
	if( !EnsureTargets( w, h, fs.msaa ) )
		return;
	RunPosePasses( a, bReady ); // MoveEhr W1: the pair's animated sides, before anything draws them (shadows included)
	const bool needInput = fs.inputTex && ( fs.envSource == 1 || fs.inputUse >= 2 || ( fs.surf.on && fs.surf.glass > 0.0f ) || ( fs.sim.engine > 0 && fs.sim.project > 0.0f ) ||
	                                       ( fs.projector.on && fs.projector.source == 0 ) );
	perf.Mark( PerfProbe::Prep );
	if( needInput )
		UpdateInputCopy( fs );
	UpdateFaceReadback( fs );
	ComputeLightMatrices( fs );
	BindEnvFile( fs ); // MoveEhr W5
	GpuModel* ready = model && model->Complete() ? model : nullptr;
	// LoadEhr W1: the DOMINANT model of the pair = the nearer one (the outgoing model below the hop's midpoint, the incoming one
	// from it). What can show only one model follows it: the sim obstacle, the fireflies' homes, the caustics, the lava haze, the
	// floor mirror outside a hop. (The shadow casters draw the whole pair mid-hop instead: DrawCasters.)
	const bool domB   = both && t >= 0.5f;
	const int domSide = domB ? 1 : activeSide;
	if( ready )
	{
		ready->UpdateLods();
		UpdateShards( ready, fs.shape.shardCount, fs.shape.shatterMode );
		UpdateBounce( fs, ready ); // W26
		letters[ SlotOf( ready ) ].live = UpdateLetters( ready, fs.letterXf ); // empty unless this is slot A's text / logo
		UpdateDust( both ? fsA : fs, ready );
	}
	if( both )
	{
		activeSide = 1;
		bReady->UpdateLods();
		UpdateShards( bReady, fs.shape.shardCount, fs.shape.shatterMode );
		letters[ SlotOf( bReady ) ].live = sideSlot[ 1 ] == 0 && UpdateLetters( bReady, fs.letterXf ); // only slot A has letters
		UpdateDust( fsB, bReady );
		activeSide = 0;
	}
	{
		// MoveEhr W2 A3: the posed sides' shards pivot where they are, on the frames that read them (pieces in flight, a Pieces hop)
		const bool pieces = both && fsIn.transitionStyle == kStylePieces;
		RunShardPivots( ( both ? fsA : fs ).shape.shatterT > 0.0f || pieces ? ready : nullptr, both && ( fsB.shape.shatterT > 0.0f || pieces ) ? bReady : nullptr );
	}
	// MoveEhr W2 Part B: the drawn sides' voxels, captured through each side's own soup program (rebuilt only when what they read changed;
	// every frame mid-hop or under a vertex effect). Until a side's spots land it draws its mesh
	bool voxOn[ 2 ] = { false, false };
	if( fs.vox.look > 0 )
	{
		auto soupOf    = [ & ]( const FrameState& f ) { return f.morph.On() ? progSoupM : f.shape.transSide > 0 && !f.shape.dustTrans ? progSoupT : progSoup; };
		const int keep = activeSide;
		for( int s = 0; s < ( both ? 2 : 1 ); ++s )
		{
			const FrameState& f = s == 0 ? ( both ? fsA : fs ) : fsB;
			GpuModel* g         = s == 0 ? ready : bReady;
			if( !g )
				continue;
			activeSide       = both ? s : keep;
			EnsureVAO( g );
			const bool every = both || f.shape.on || f.morph.On() || f.explode > 0.0f || f.wireMix > 0.0f;
			if( every && LogVerbose() )
			{
				static int said = 0;
				if( said++ < 3 )
					Log( "VOXEL every frame because: pair %d shape.on %d morph %d explode %.3f wireMix %.3f", both ? 1 : 0, f.shape.on ? 1 : 0, f.morph.On() ? 1 : 0,
					     f.explode, f.wireMix );
			}
			voxOn[ s ]       = BuildVoxels( f, g, soupOf( f ), every );
		}
		voxHopT    = t;
		activeSide = keep;
		if( ready )
			EnsureVAO( ready );
	}
	voxDrawn[ 0 ] = voxOn[ 0 ], voxDrawn[ 1 ] = voxOn[ 1 ];
	GpuModel* dom         = domB ? bReady : ready;
	const FrameState& fsD = domB ? fsB : fs;
	lensModel = dom, lensFs = &fsD; // W9 black hole: the hole sits in the dominant model
	if( fs.x.look == 1 && !fs.x.hist.empty() )
	{
		// W9 slit-scan: the pose history (newest first) for the DEFORM twins (unit 32)
		if( !xHistBuf )
			glGenBuffers( 1, &xHistBuf ), glGenTextures( 1, &xHistTex );
		glBindBuffer( GL_TEXTURE_BUFFER, xHistBuf );
		glBufferData( GL_TEXTURE_BUFFER, fs.x.hist.size() * sizeof( float ), fs.x.hist.data(), GL_STREAM_DRAW );
		glBindTexture( GL_TEXTURE_BUFFER, xHistTex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, xHistBuf ); // re-attach after glBufferData (Apple GL)
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	}
	{
		const int keep = activeSide;
		activeSide     = domSide;
		UpdateSwarm( fsD, dom ); // W26
		activeSide = keep;
	}
	perf.Mark( PerfProbe::Shadows );
	RenderShadows( fs, ready );
	perf.Mark( PerfProbe::KeyShadow );
	RenderKeyShadow( fs, ready );
	perf.Mark( PerfProbe::Projector );
	RenderProjector( fs, ready ); // W26: shadow + picture in the projector's frustum
	perf.Mark( PerfProbe::Caustics );
	{
		const int keep = activeSide;
		activeSide     = domSide;
		if( domB )
			EnsureVAO( dom );
		RenderCaustics( fsD, dom ); // W26: the caustic map (light space)
		activeSide = keep;
	}
	perf.Mark( PerfProbe::Main );

	glBindFramebuffer( GL_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
	glViewport( 0, 0, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	if( model && model->Complete() )
	{
		EnsureVAO( model );
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );
		if( std::find( warmedFor.begin(), warmedFor.end(), model->serial ) == warmedFor.end() )
		{
			// The driver compiles program variants lazily on first draw (tens of ms). Draw one
			// invisible triangle through every variant now, at model swap, instead of the moment
			// someone first turns up Wireframe / Explode / Outline mid-show.
			// Colour write mask is part of the compiled pipeline, so limit the draw with a 1-pixel
			// scissor instead of masking it.
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 1, 1 );
			glEnable( GL_BLEND );
			glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
			DrawModel( progSoup, fs, model, false, true, 3 );
			glDisable( GL_BLEND );
			DrawModel( progSoup, fs, model, false, true, 3 );
			glEnable( GL_CULL_FACE );
			glCullFace( GL_FRONT );
			DrawModel( progMain, fs, model, true, false, 3 );
			glDisable( GL_CULL_FACE );
			glCullFace( GL_BACK );
			// instanced variants (clones / ghosts)
			std::vector< Instance > one( 1 );
			std::memcpy( one[ 0 ].m, fs.model, sizeof( fs.model ) );
			DrawModel( progMain, fs, model, false, false, 0, nullptr, nullptr, &one );
			DrawModel( progSoup, fs, model, false, true, 0, nullptr, nullptr, &one );
			DrawModel( progSoupT, fs, model, false, true, 3 );
			DrawModel( progSoupT, fs, model, false, true, 0, nullptr, nullptr, &one );
			glEnable( GL_BLEND ); // ghosts: the blended instanced pipeline
			glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
			DrawModel( progMain, fs, model, false, false, 0, nullptr, nullptr, &one );
			glDisable( GL_BLEND );
			if( model->geometry && !model->geometry->triGroup.empty() )
			{
				// text / logos: the LETTERS programs (plain, instanced, blended instanced)
				LetterSlot& ls = letters[ SlotOf( model ) ];
				const bool was = ls.live;
				std::vector< float > rest( size_t( model->geometry->groupCount ) * 12, 0.0f );
				ls.live = UpdateLetters( model, rest );
				DrawModel( progSoup, fs, model, false, true, 3 );
				DrawModel( progSoup, fs, model, false, true, 0, nullptr, nullptr, &one );
				glEnable( GL_BLEND );
				DrawModel( progSoup, fs, model, false, true, 0, nullptr, nullptr, &one );
				glDisable( GL_BLEND );
				ls.live = was && UpdateLetters( model, fs.letterXf );
			}
			// W2: the DEFORM twins of the main draws (plain, blended, instanced, the Shatter transition, letters)
			{
				FrameState d  = fs;
				d.shape.on    = d.shape.deform = true;
				d.shape.foldN = 2, d.shape.foldK = 1.0f, d.shape.foldAxes[ 0 ] = 1.0f;
				glEnable( GL_BLEND );
				DrawModel( progSoup, d, model, false, true, 3 );
				glDisable( GL_BLEND );
				DrawModel( progSoup, d, model, false, true, 3 );
				DrawModel( progSoup, d, model, false, true, 0, nullptr, nullptr, &one );
				DrawModel( progSoupT, d, model, false, true, 3 );
				if( model->geometry && !model->geometry->triGroup.empty() )
				{
					LetterSlot& ls = letters[ SlotOf( model ) ];
					const bool was = ls.live;
					std::vector< float > rest( size_t( model->geometry->groupCount ) * 12, 0.0f );
					ls.live = UpdateLetters( model, rest );
					DrawModel( progSoup, d, model, false, true, 3 );
					ls.live = was && UpdateLetters( model, fs.letterXf );
				}
			}
			// W5: the WIRE twins of the main draws (wired draws are always blended): plain, instanced, deformed, the transition
			{
				FrameState d = fs;
				d.wire.on    = true;
				glEnable( GL_BLEND );
				DrawModel( progSoup, d, model, false, true, 3 );
				DrawModel( progSoup, d, model, false, true, 0, nullptr, nullptr, &one );
				DrawModel( progSoupT, d, model, false, true, 3 );
				d.shape.on = d.shape.deform = true;
				DrawModel( progSoup, d, model, false, true, 3 );
				// W6: the POINTS twins (through the one dummy point: the model's point set may not exist yet)
				pointsNow = warmingNow = true;
				d.shape.on = d.shape.deform = false;
				DrawModel( progSoup, d, model, false, true, 3 );
				DrawModel( progSoup, d, model, false, true, 0, nullptr, nullptr, &one );
				DrawModel( progSoupT, d, model, false, true, 3 );
				d.shape.on = d.shape.deform = true;
				DrawModel( progSoup, d, model, false, true, 3 );
				pointsNow = warmingNow = false;
				glDisable( GL_BLEND );
			}
			WarmMorph( fs, model ); // W6 Stage 2: the pack's MORPH programs + the cloud
			WarmLooks( fs, model ); // W9: the CAPTURE twins, the line programs, the cut's stencil pass + cap
			glDisable( GL_SCISSOR_TEST );
			WarmVolume( fs, model, w, h );
			WarmPost( fs, model, w, h );
			WarmWow( fs, model, w, h );
			glBindFramebuffer( GL_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
			glViewport( 0, 0, w, h );
			glEnable( GL_DEPTH_TEST );
			glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
			warmedFor.push_back( model->serial );
			if( warmedFor.size() > 16 )
				warmedFor.erase( warmedFor.begin() );
		}
		if( fs.outline > 0.0f )
		{
			glEnable( GL_CULL_FACE );
			glCullFace( GL_FRONT );
			DrawModel( progMain, fs, model, true, false, 0 );
			glDisable( GL_CULL_FACE );
			glCullFace( GL_BACK );
		}
		if( fs.wireMix > 0.0f )
		{
			glEnable( GL_BLEND );
			glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
		}
		const bool soup = fs.wireMix > 0.0f || fs.explode > 0.0f || fs.shape.on;
		if( both )
		{
			// mid-transition: both models, each through its half of the transition
			// (LOADEHR_TRANS_SOLO=A|B draws one side only: the W16 coverage measurement)
			static const char* solo = getenv( "MOVEEHR_TRANS_SOLO" );
			for( int m = 0; m < 2; ++m )
			{
				if( solo && *solo != "AB"[ m ] )
					continue;
				const FrameState& f = m == 0 ? fsA : fsB;
				GpuModel* g         = m == 0 ? model : bReady;
				const bool sp       = f.wireMix > 0.0f || f.explode > 0.0f || f.shape.on;
				activeSide          = m;
				if( m == 1 )
					EnsureVAO( g );
				const GLuint soupP = f.morph.On() ? progSoupM : f.shape.transSide > 0 && !f.shape.dustTrans ? progSoupT : progSoup;
				(void)sp;
				if( voxOn[ m ] )
					DrawVoxels( f, g, m, t ); // W2 Part B: the voxels swap side by side over the hop (hash < t: the incoming one's)
				else
					DrawSide( soupP, f, g, f.inst.empty() ? nullptr : &f.inst ); // W5 wire / W9 cut inside
				DrawDust( f, g, h );
				if( f.lava.on && f.transitionStyle == kStyleLava )
					DrawCracks( f, g ); // W6 Lava style: the cracks glow (W7: on both models)
				if( m == 1 )
					EnsureVAO( model );
			}
			activeSide = 0;
			// W7 Trans Sparkle: each model's sparkles = a Points Only draw of it (Surface 25k, a share of it), glow only: into the wire
			// buffer as light (RenderWireGlow: behind the pair's depth), the surface colour lifted halfway to white, twinkling.
			// The POINTS twins run the whole soup stage, so a sparkle sits on the twisted / melted / split / flying surface.
			const float env = fsA.morph.sparkle * SparkleEnvelope( fsA.transitionStyle, t, fsA.transEase );
			if( env > 0.0f && !solo )
			{
				const float s = fsA.morph.sparkle;
				for( int m = 0; m < 2; ++m )
				{
					const FrameState& f = m == 0 ? fsA : fsB;
					FrameState& x       = sparkFs[ m ];
					x                   = f;
					FrameState::WireState& wv = x.wire;
					wv.on = true, wv.mode = 4, wv.back = 0, wv.edges = 0, wv.pointSource = 2;
					wv.ptfx = false; // W4: the sparkles keep their own round, still look
					wv.color[ 0 ] = wv.color[ 1 ] = wv.color[ 2 ] = 1.0f, wv.colorMix = 0.5f, wv.fade = 0.0f;
					wv.twinkle = 0.9f, wv.twinklePhase = f.morph.sparklePhase;
					// light = 1.2 x the envelope x Trans Sparkle; AddWireLight multiplies the buffer by 0.35 x its glow (1 with no Wire Glow).
					// Spin Swap / Implode squeeze a model to a point at the swap: its sparkles dim with what is left of it (else they pile
					// into one white dot)
					const float left = f.morph.vs == 6 ? f.morph.scale : f.morph.vs == 3 ? ( m == 0 ? 1.0f - f.morph.k : f.morph.k ) : 1.0f;
					wv.mix          = 1.2f * env * left * left / ( 0.35f * ( fs.wire.glow > 0.0f ? fs.wire.glow : 1.0f ) );
					x.wireThickness = 1.2f + 0.15f * s; // 2.4 px .. 5.4 px
					x.wireMix       = 0.0f;
					const GLuint soupP = f.morph.On() ? progSoupM : f.shape.transSide > 0 && !f.shape.dustTrans ? progSoupT : progSoup;
					wireDraws.push_back( { soupP, &x, m == 0 ? model : bReady, nullptr, m, std::min( 1.0f, 0.06f + 0.024f * s ) } ); // 1.5k .. 7.5k
					sparkleNow = true;
				}
			}
			if( fsA.morph.cloud > 0.0f && !fsA.morph.swarm )
				DrawCloud( fsA, fsB, w, h, false ); // W6 Points style: the cloud between the two meshes
			if( fsA.liquid.ready )
			{
				if( fs.liquid.look == 1 )
					RenderLiquidPaint( fsA, fsB, model, bReady, w, h );
				glBindFramebuffer( GL_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
				glViewport( 0, 0, w, h );
				RenderLiquid( fsA, t, w, h ); // W26: the blended SDF surface, with depth
				EnsureVAO( model );
			}
		}
		else if( voxOn[ 0 ] )
			DrawVoxels( fs, model, -1, 0.0f ); // W2 Part B: the voxels instead of the mesh
		else
		{
			DrawSide( progSoup, fs, model, fs.inst.empty() ? nullptr : &fs.inst ); // W5 wire / W9 cut inside
			if( !fs.wire.on || ( fs.wire.mode != 2 && fs.wire.mode < 4 ) ) // Wire Only / the points: no surface for the picture / the cracks
			{
				DrawProjSurface( fs, model ); // W26: the projected picture on the mask
				DrawCracks( fs, model );      // W26: lava cracks
			}
		}
		if( fs.x.window ) // W9 Stage C: the openings show another world (the dominant model's)
		{
			const bool dB = both && t >= 0.5f;
			EnsureVAO( dB ? bReady : model );
			DrawWindow( dB ? fsB : both ? fsA : fs, dB ? bReady : model );
			EnsureVAO( model );
		}
		if( !fs.ghosts.empty() )
		{
			// past poses: behind the live mask (depth tested against it), newest first and writing depth, so
			// each ghost hides its own back layers and the ones behind it instead of shading them all
			glEnable( GL_BLEND );
			glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
			// a hair behind in depth: where a ghost (or its Smear stretch) meets the live surface, the live mask wins
			glEnable( GL_POLYGON_OFFSET_FILL );
			glPolygonOffset( 1.0f, 64.0f );
			if( fs.wire.on )
				DrawWired( progSoup, fs, model, &fs.ghosts );
			else
				DrawModel( soup ? progSoup : progMain, fs, model, false, soup, 0, nullptr, nullptr, &fs.ghosts );
			glDisable( GL_POLYGON_OFFSET_FILL );
			glPolygonOffset( 0.0f, 0.0f );
			if( fs.wireMix <= 0.0f )
				glDisable( GL_BLEND );
		}
		if( !both )
			DrawDust( fs, model, h );
		if( fs.lines.look > 0 ) // W9 Stage A: the line looks (each side through the program it is drawn with)
		{
			auto soupOf = [ & ]( const FrameState& f ) { return f.morph.On() ? progSoupM : f.shape.transSide > 0 && !f.shape.dustTrans ? progSoupT : progSoup; };
			if( both )
				RenderLines( fsA, model, soupOf( fsA ), &fsB, bReady, soupOf( fsB ), t );
			else
				RenderLines( fs, model, progSoup, nullptr, nullptr, 0, 0.0f );
		}
		else
			lineDraws.clear();
		if( fs.sl.look == 1 || fs.sl.look == 2 ) // W9 Stage D: spikes / copies on the dominant model
		{
			const bool dB = both && t >= 0.5f;
			const FrameState& f = dB ? fsB : both ? fsA : fs;
			activeSide = dB ? 1 : 0;
			EnsureVAO( dB ? bReady : model );
			RenderSurfLook( f, dB ? bReady : model, f.morph.On() ? progSoupM : f.shape.transSide > 0 && !f.shape.dustTrans ? progSoupT : progSoup );
			activeSide = 0;
			EnsureVAO( model );
		}
		glDisable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ZERO );
		glDisable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );
	}

	if( tSamples > 1 )
	{
		glBindFramebuffer( GL_READ_FRAMEBUFFER, msFBO );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, resolveFBO );
		glBlitFramebuffer( 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	}

	RenderWireGlow( fs, w, h ); // W5: the lines' light (only while Wire Glow / Wire Bloom are on)
	perf.Mark( PerfProbe::Floor );
	RenderMirror( fs, ready, w, h ); // mid-hop: the whole pair (pairOn)
	RenderSilhouetteBlur( fs, w, h );
	UpdateEchoes( fs, w, h );
	fogOut = 0;
	const bool depthNeeded = fs.vol.active || !fs.bolts.segs.empty() || fs.swarm.count > 0;
	bool depthOk           = false;
	if( depthNeeded && EnsureSceneDepth( w, h ) )
	{
		perf.Mark( PerfProbe::Sim );
		CopySceneDepth( w, h );
		depthOk = true;
		const int keep = activeSide;
		activeSide     = domSide;
		UpdateSim( fsD, dom ); // the dominant model is the obstacle (the plugin hands the sim its matrix: SimState::model)
		activeSide = keep;
		perf.Mark( PerfProbe::Volume );
		if( fs.vol.active )
		{
			RenderVolume( fs, w, h );
			if( fogOut && fs.trip.post )
				RenderTripPost( fs, w, h ); // W24: prism + hue echo
			else
				tripHist = false;
		}
	}

	perf.Mark( PerfProbe::Composite );
	const bool haze    = fs.lava.on && fs.lava.haze > 0.0f && ready && DrawHazeMask( fs, ready, w, h ); // W26
	const bool cfloor  = causticOk && fs.caustic.floor > 0.0f;                                         // W26 Stage 8
	// composite into the host framebuffer (or into postTex when bloom / flares follow)
	const bool post = PostActive( fs );
	if( post )
		EnsurePostTarget( w, h );
	const GLuint target = post ? postFBO : fs.hostFBO;
	glBindFramebuffer( GL_FRAMEBUFFER, target );
	glViewport( 0, 0, w, h );
	const GLuint progComposite = haze || cfloor ? progCompositeHaze : this->progComposite;
	glUseProgram( progComposite );
	if( haze || cfloor )
	{
		glUniform1f( U( progComposite, "uHazeAmt" ), haze ? fs.lava.haze : 0.0f );
		glUniform1f( U( progComposite, "uHazePhase" ), fs.lava.hazePhase );
		glActiveTexture( GL_TEXTURE9 );
		glBindTexture( GL_TEXTURE_2D, haze ? hazeTex : whiteTex );
		glUniform1i( U( progComposite, "uCausticOn" ), cfloor ? 1 : 0 );
		glUniformMatrix4fv( U( progComposite, "uCausticVP" ), 1, GL_FALSE, causticVP );
		glUniform3fv( U( progComposite, "uCausticPos" ), 1, fs.caustic.pos );
		float cc[ 3 ];
		for( int k = 0; k < 3; ++k )
			cc[ k ] = fs.caustic.color[ k ] * fs.caustic.floor * 0.15f; // display space: the light is ~unit brightness
		glUniform3fv( U( progComposite, "uCausticColor" ), 1, cc );
		glUniform4fv( U( progComposite, "uCatchPlane" ), 1, fs.post.catchPlane );
		glUniformMatrix4fv( U( progComposite, "uInvViewProj" ), 1, GL_FALSE, ( [ & ]() { Mat4 V, P; std::memcpy( V.m, fs.view, sizeof( V.m ) ); std::memcpy( P.m, fs.proj, sizeof( P.m ) ); return ( P * V ).Inverse(); } )().m );
		glUniform3fv( U( progComposite, "uCamPos" ), 1, fs.camPos );
		glActiveTexture( GL_TEXTURE10 );
		glBindTexture( GL_TEXTURE_2D, cfloor ? causticTex : whiteTex );
	}
	glUniform1i( U( progComposite, "uFogOn" ), fogOut ? 1 : 0 );
	const bool floorOn = ( keyShadowOk && fs.post.shadowAmt > 0.0f ) || mirrorOk;
	glUniform1i( U( progComposite, "uFloorOn" ), floorOn ? 1 : 0 );
	glUniform1i( U( progComposite, "uSilOn" ), silOk ? 1 : 0 );
	glUniform1i( U( progComposite, "uEchoOn" ), echoOk ? 1 : 0 );
	for( int u = 2; u <= 8; ++u ) // every declared sampler needs a loadable texture, used or not
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, whiteTex );
	}
	if( fogOut || floorOn || silOk )
	{
		Mat4 V, P;
		std::memcpy( V.m, fs.view, sizeof( V.m ) );
		std::memcpy( P.m, fs.proj, sizeof( P.m ) );
		Mat4 inv = ( P * V ).Inverse();
		glUniformMatrix4fv( U( progComposite, "uInvViewProj" ), 1, GL_FALSE, inv.m );
		glUniform3fv( U( progComposite, "uCamPos" ), 1, fs.camPos );
		glUniform2f( U( progComposite, "uTexel" ), 1.0f / w, 1.0f / h );
	}
	if( floorOn )
	{
		const PostState& p = fs.post;
		const float plane[ 4 ] = { 0, 1, 0, -p.floorY };
		glUniform4fv( U( progComposite, "uPlane" ), 1, plane );
		glUniform4fv( U( progComposite, "uCatchPlane" ), 1, p.catchPlane );
		glUniform1f( U( progComposite, "uShadowAmt" ), keyShadowOk ? p.shadowAmt : 0.0f );
		glUniform1f( U( progComposite, "uShadowSoft" ), p.shadowSoft );
		glUniform1f( U( progComposite, "uFloorLight" ), keyShadowOk ? p.floorLight * p.shadowAmt : 0.0f );
		glUniform3fv( U( progComposite, "uFloorLightColor" ), 1, p.floorLightColor );
		glUniformMatrix4fv( U( progComposite, "uKeyVP" ), 1, GL_FALSE, keyVP );
		glUniform3fv( U( progComposite, "uKeyPos" ), 1, keyPos );
		glUniform3fv( U( progComposite, "uKeyDir" ), 1, keyDir );
		glUniform1i( U( progComposite, "uMirrorOn" ), mirrorOk ? 1 : 0 );
		glUniform1f( U( progComposite, "uMirrorAmt" ), p.mirror );
		glUniform1f( U( progComposite, "uMirrorBlur" ), p.mirrorBlur );
		glUniform1f( U( progComposite, "uMirrorFade" ), p.mirrorFade );
		glUniform3fv( U( progComposite, "uMaskCenter" ), 1, p.maskCenter );
		glActiveTexture( GL_TEXTURE5 );
		glBindTexture( GL_TEXTURE_2D, keyShadowOk ? keyShadowTex : whiteTex );
		glBindSampler( 5, 0 ); // W4: a host's (Arena's) LINEAR sampler object would blend the depths the catcher compares (MOVEEHR-005)
		glActiveTexture( GL_TEXTURE6 );
		glBindTexture( GL_TEXTURE_2D, mirrorOk ? mirrorTex : whiteTex );
	}
	if( silOk )
	{
		const PostState& p = fs.post;
		glUniform1f( U( progComposite, "uSil" ), std::min( 1.0f, p.sil ) );
		glUniform1f( U( progComposite, "uSilFill" ), p.silFill );
		glUniform1f( U( progComposite, "uSilGlow" ), p.silGlow );
		glUniform3fv( U( progComposite, "uSilColor" ), 1, p.silColor );
		glActiveTexture( GL_TEXTURE7 );
		glBindTexture( GL_TEXTURE_2D, silTex[ 1 ] );
	}
	if( echoOk )
	{
		glActiveTexture( GL_TEXTURE8 );
		glBindTexture( GL_TEXTURE_2D, echoTex );
	}
	if( fogOut )
	{
		glUniform1i( U( progComposite, "uFogBlend" ), fs.vol.blend );
		glUniform1i( U( progComposite, "uFogDepthAware" ), ( fogW != w || fogH != h ) && fs.vol.method == 0 ? 1 : 0 );
		glUniform2f( U( progComposite, "uFogSize" ), float( fogW ), float( fogH ) );
		glUniform1f( U( progComposite, "uExposure" ), fs.exposure );
		glUniform1i( U( progComposite, "uTonemap" ), fs.tonemap );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, fogOut );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_2D, fogDepthTex );
		glActiveTexture( GL_TEXTURE4 );
		glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
	}
	const float bgA = fs.background[ 3 ];
	glUniform4f( U( progComposite, "uBackground" ), fs.background[ 0 ] * bgA, fs.background[ 1 ] * bgA, fs.background[ 2 ] * bgA, bgA );
	const bool bgInput = fs.inputTex && fs.inputUse == 1;
	glUniform1i( U( progComposite, "uUseInput" ), bgInput ? 1 : 0 );
	glUniform2f( U( progComposite, "uInputScale" ), fs.inputHwW ? float( fs.inputW ) / fs.inputHwW : 1.0f, fs.inputHwH ? float( fs.inputH ) / fs.inputHwH : 1.0f );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, bgInput ? fs.inputTex : whiteTex );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, resolveTex );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	DrawBackdrop( fs, w, h ); // MoveEhr W5: Env Backdrop, under the composited frame
	perf.Mark( PerfProbe::Light );
	AddWireLight( fs, w, h ); // W5: before the post pass, so the global Bloom catches the wires too
	AddLineLight( fs, w, h ); // W9 neon tubes: their light on the model
	if( both && fsA.morph.cloud > 0.0f && fsA.morph.cloudGlow > 0.0f )
		DrawCloud( fsA, fsB, w, h, true ); // W6 Points / Swarm styles: the cloud's light (Wire Glow / Firefly Glow), the same way
	if( fogOut && fs.vol.motes > 0.0f )
		DrawMotes( fs, w, h );
	if( depthOk && ( !fs.bolts.segs.empty() || fs.swarm.count > 0 ) )
		RenderGlow( fs, w, h ); // W26 fireflies + lightning, the same way
	perf.Mark( PerfProbe::Post );
	if( post )
		RenderPost( fs, w, h );

	pairOn = false;
	// back to the default state the host expects
	glBindVertexArray( 0 );
	for( int u = 10; u >= 2; --u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );
}
}
