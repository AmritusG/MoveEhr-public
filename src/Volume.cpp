// Volume light (Stage 1): shadow maps for the volume lights (A, B, eye lights C), the smoke medium, the raymarch
// (Hillaire 2015) with temporal accumulation, the screen-space alternative (Mitchell, GPU Gems 3 ch.13)
// and the motes. Everything here is skipped while no volume light is on.
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

constexpr int kShadowRes2D   = 1024;
constexpr int kShadowResCube = 512;
constexpr int kNoiseRes      = 64;
constexpr float kShadowFar   = 60.0f;

// ---- tileable 3D value noise for the smoke texture
uint32_t HashU( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}
float Lattice( int x, int y, int z, int period, uint32_t seed )
{
	x = ( x % period + period ) % period, y = ( y % period + period ) % period, z = ( z % period + period ) % period;
	return float( HashU( uint32_t( x ) * 73856093U ^ uint32_t( y ) * 19349663U ^ uint32_t( z ) * 83492791U ^ seed ) & 0xffffff ) / float( 0xffffff );
}
float ValueNoise( float x, float y, float z, int period, uint32_t seed )
{
	int ix = (int)std::floor( x ), iy = (int)std::floor( y ), iz = (int)std::floor( z );
	float fx = x - ix, fy = y - iy, fz = z - iz;
	auto q   = []( float t ) { return t * t * t * ( t * ( t * 6.0f - 15.0f ) + 10.0f ); };
	fx = q( fx ), fy = q( fy ), fz = q( fz );
	float c[ 2 ][ 2 ][ 2 ];
	for( int k = 0; k < 2; ++k )
		for( int j = 0; j < 2; ++j )
			for( int i = 0; i < 2; ++i )
				c[ k ][ j ][ i ] = Lattice( ix + i, iy + j, iz + k, period, seed );
	auto lerp = []( float a, float b, float t ) { return a + ( b - a ) * t; };
	float x00 = lerp( c[ 0 ][ 0 ][ 0 ], c[ 0 ][ 0 ][ 1 ], fx ), x10 = lerp( c[ 0 ][ 1 ][ 0 ], c[ 0 ][ 1 ][ 1 ], fx );
	float x01 = lerp( c[ 1 ][ 0 ][ 0 ], c[ 1 ][ 0 ][ 1 ], fx ), x11 = lerp( c[ 1 ][ 1 ][ 0 ], c[ 1 ][ 1 ][ 1 ], fx );
	return lerp( lerp( x00, x10, fy ), lerp( x01, x11, fy ), fz );
}

void MakeTarget2D( GLuint& tex, GLenum internal, GLenum format, GLenum type, int w, int h, GLenum filter )
{
	if( !tex )
		glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexImage2D( GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

void DelTex( GLuint& t )
{
	if( t )
		glDeleteTextures( 1, &t ), t = 0;
}
void DelFBO( GLuint& f )
{
	if( f )
		glDeleteFramebuffers( 1, &f ), f = 0;
}
void DelRB( GLuint& r )
{
	if( r )
		glDeleteRenderbuffers( 1, &r ), r = 0;
}

Mat4 LookAtDir( const float* eye, const float* dir )
{
	float at[ 3 ] = { eye[ 0 ] + dir[ 0 ], eye[ 1 ] + dir[ 1 ], eye[ 2 ] + dir[ 2 ] };
	float up[ 3 ] = { 0, 1, 0 };
	if( std::fabs( dir[ 1 ] ) > 0.95f )
		up[ 1 ] = 0, up[ 2 ] = 1;
	return Mat4::LookAt( eye, at, up );
}
}

bool Renderer::InitVolume( std::string& error )
{
	const std::string mainFS   = std::string( kLightLib ) + kMainFS;
	const std::string shadowFS = std::string( "#define SHADOW_PASS\n" ) + mainFS;
	progShadow                 = Link( kMainVS, shadowFS, error );
	progShadowSoup             = Link( kSoupVS, shadowFS, error );
	progShadowI                = Link( std::string( "#define INSTANCED\n" ) + kMainVS, "#define INSTANCED\n" + shadowFS, error );
	progShadowSoupI            = Link( std::string( "#define INSTANCED\n" ) + kSoupVS, "#define INSTANCED\n" + shadowFS, error );
	// LoadEhr W1: a Shatter hop's shards in the shadow passes (mid-hop the pair is drawn as casters: DrawCasters)
	progShadowSoupT            = Link( std::string( "#define TRANS\n" ) + kSoupVS, shadowFS, error );
	progShadowSoupTI           = Link( std::string( "#define TRANS\n#define INSTANCED\n" ) + kSoupVS, "#define INSTANCED\n" + shadowFS, error );
	progFogMarch               = Link( kQuadVS, std::string( kLightLib ) + kFrontLib + kMediumLib + kFogMarchFS, error );
	progFogTemporal            = Link( kQuadVS, kFogTemporalFS, error );
	progRaySrc                 = Link( kQuadVS, std::string( kLightLib ) + kFrontLib + kMediumLib + kRaySourceFS, error );
	progRayBlur                = Link( kQuadVS, kRayBlurFS, error );
	progMotes                  = Link( std::string( kLightLib ) + kFrontLib + kMotesVS, std::string( kToneLib ) + kMotesFS, error );
	progShadowSoupL            = Link( std::string( "#define LETTERS\n" ) + kSoupVS, shadowFS, error );
	progShadowSoupLI           = Link( std::string( "#define LETTERS\n#define INSTANCED\n" ) + kSoupVS, "#define INSTANCED\n" + shadowFS, error );
	// W24 Trip: the noise march with the Trip looks (its lamps tinted by gTint, as the sim march), and the fog post
	std::string marchT = kFogMarchFS;
	const std::string from = "S += uXLColor[ i ] *", to = "S += gTint * uXLColor[ i ] *";
	marchT.replace( marchT.find( from ), from.size(), to );
	progFogMarchT = Link( kQuadVS, std::string( "#define TRIP\n" ) + kLightLib + kFrontLib + kTripLib + kMediumLib + marchT, error );
	progTripPost  = Link( kQuadVS, std::string( kTripLib ) + kTripPostFS, error ); // W25: + Palette for contour lines
	if( !progFogMarchT || !progTripPost )
		return false;
	if( !progShadow || !progShadowSoup || !progShadowI || !progShadowSoupI || !progShadowSoupT || !progShadowSoupTI || !progShadowSoupL || !progShadowSoupLI || !progFogMarch || !progFogTemporal || !progRaySrc || !progRayBlur || !progMotes )
		return false;
	for( GLuint p : { progShadow, progShadowSoup, progShadowI, progShadowSoupI, progShadowSoupT, progShadowSoupTI, progShadowSoupL, progShadowSoupLI } )
	{
		glUseProgram( p );
		char name[ 16 ];
		for( int i = 0; i < CH_COUNT; ++i )
		{
			snprintf( name, sizeof( name ), "uTex%d", i );
			glUniform1i( U( p, name ), i );
		}
		glUniform1i( U( p, "uInputTex" ), 8 );
		glUniform1i( U( p, "uIdxBuf" ), 9 );
		glUniform1i( U( p, "uVtxBuf" ), 10 );
		BindShadowSamplers( p );
	}
	for( GLuint p : { progFogMarch, progRaySrc, progFogMarchT } )
	{
		glUseProgram( p );
		glUniform1i( U( p, "uDepth" ), 0 );
		glUniform1i( U( p, "uNoise" ), 1 );
		glUniform1i( U( p, "uSKShadow" ), kSKShadowUnit );
		BindShadowSamplers( p );
	}
	glUseProgram( progFogTemporal );
	glUniform1i( U( progFogTemporal, "uCur" ), 0 );
	glUniform1i( U( progFogTemporal, "uCurDepth" ), 1 );
	glUniform1i( U( progFogTemporal, "uHist" ), 2 );
	glUseProgram( progRayBlur );
	glUniform1i( U( progRayBlur, "uSource" ), 0 );
	glUseProgram( progTripPost );
	glUniform1i( U( progTripPost, "uFogIn" ), 0 );
	glUniform1i( U( progTripPost, "uHist" ), 1 );
	glUseProgram( progMotes );
	glUniform1i( U( progMotes, "uDepth" ), 0 );
	BindShadowSamplers( progMotes );
	glUseProgram( 0 );

	// 1x1 cube bound to the cube shadow units whenever no point-light shadow exists
	glGenTextures( 1, &dummyCube );
	glBindTexture( GL_TEXTURE_CUBE_MAP, dummyCube );
	const uint8_t white[ 4 ] = { 255, 255, 255, 255 };
	for( int f = 0; f < 6; ++f )
		glTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white );
	glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );
	return true;
}

void Renderer::ReleaseVolume()
{
	for( GLuint* p : { &progShadow, &progShadowSoup, &progShadowI, &progShadowSoupI, &progShadowSoupT, &progShadowSoupTI, &progShadowSoupL, &progShadowSoupLI, &progFogMarch, &progFogTemporal, &progRaySrc, &progRayBlur, &progMotes,
	                   &progFogMarchT, &progTripPost } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	for( int i = 0; i < kMaxVolLights; ++i )
	{
		DelTex( shadowTex[ i ] ), DelFBO( shadowFBO[ i ] ), DelRB( shadowDepth[ i ] );
		shadowMode[ i ] = 0;
	}
	for( int i = 0; i < 2; ++i )
		DelTex( shadowCube[ i ] ), DelTex( histTex[ i ] ), DelFBO( histFBO[ i ] );
	DelTex( dummyCube ), DelTex( noiseTex ), DelTex( sceneDepthTex ), DelFBO( depthFBO );
	DelTex( fogTex ), DelTex( fogDepthTex ), DelFBO( marchFBO ), DelTex( raySrcTex ), DelFBO( raySrcFBO );
	for( int i = 0; i < 2; ++i )
		DelTex( tripTex[ i ] ), DelFBO( tripFBO[ i ] );
	depthW = depthH = fogW = fogH = tripW = tripH = 0;
	tripHist = false;
	histValid = false;
}

// Uniforms of kLightLib. bindShadows = false while rendering a shadow map (no feedback, mode 0).
void Renderer::SetLightUniforms( GLuint p, const FrameState& fs, bool bindShadows )
{
	const int n = std::min( fs.xlCount, kMaxXLights );
	float pos[ kMaxXLights * 3 ], dir[ kMaxXLights * 3 ], col[ kMaxXLights * 3 ], cone[ kMaxXLights * 2 ], front[ kMaxXLights ];
	for( int i = 0; i < n; ++i )
	{
		const XLight& l = fs.xl[ i ];
		front[ i ]      = l.front;
		for( int k = 0; k < 3; ++k )
			pos[ i * 3 + k ] = l.pos[ k ], dir[ i * 3 + k ] = l.dir[ k ], col[ i * 3 + k ] = l.color[ k ];
		if( l.type == 1 )
			cone[ i * 2 ] = cone[ i * 2 + 1 ] = -2.0f;
		else
		{
			const float half = std::min( 89.5f, l.coneDeg * 0.5f ) * 3.14159265f / 180.0f;
			cone[ i * 2 ]    = std::cos( half );
			cone[ i * 2 + 1 ] = std::cos( half * ( 1.0f - 0.95f * std::min( 1.0f, std::max( 0.0f, l.coneSoft ) ) ) );
			if( l.coneDeg >= 179.0f )
				cone[ i * 2 ] = -0.999f;
		}
	}
	glUniform1i( U( p, "uXLCount" ), n );
	if( n > 0 )
	{
		glUniform3fv( U( p, "uXLPos" ), n, pos );
		glUniform3fv( U( p, "uXLDir" ), n, dir );
		glUniform3fv( U( p, "uXLColor" ), n, col );
		glUniform2fv( U( p, "uXLCone" ), n, cone );
		const GLint fl = U( p, "uXLFront" ); // only the medium passes declare it
		if( fl >= 0 )
			glUniform1fv( fl, n, front );
	}
	glUniform1f( U( p, "uXLRange" ), fs.vol.shaftLen );
	GLint modes[ kMaxVolLights ];
	for( int i = 0; i < kMaxVolLights; ++i )
		modes[ i ] = bindShadows ? shadowMode[ i ] : 0;
	glUniform1iv( U( p, "uXShadowMode" ), kMaxVolLights, modes );
	glUniformMatrix4fv( U( p, "uXLightVP" ), kMaxVolLights, GL_FALSE, &lightVP[ 0 ][ 0 ] );
	for( int i = 0; i < kMaxVolLights; ++i )
	{
		glActiveTexture( GL_TEXTURE0 + kShadow2DUnit[ i ] );
		glBindTexture( GL_TEXTURE_2D, modes[ i ] == 1 ? shadowTex[ i ] : whiteTex );
	}
	for( int i = 0; i < 2; ++i )
	{
		glActiveTexture( GL_TEXTURE0 + kShadowCubeUnit[ i ] );
		glBindTexture( GL_TEXTURE_CUBE_MAP, modes[ i ] == 2 ? shadowCube[ i ] : dummyCube );
	}
	glActiveTexture( GL_TEXTURE0 );
}

void Renderer::ComputeLightMatrices( const FrameState& fs )
{
	for( int i = 0; i < kMaxVolLights; ++i )
	{
		Mat4 vp = Mat4::Identity();
		if( i < fs.vol.lights && fs.xl[ i ].type == 0 )
		{
			const XLight& l = fs.xl[ i ];
			float fov       = std::min( 170.0f, l.coneDeg + 10.0f ) * 3.14159265f / 180.0f;
			vp              = Mat4::Perspective( fov, 1.0f, 0.05f, kShadowFar ) * LookAtDir( l.pos, l.dir );
		}
		std::memcpy( lightVP[ i ], vp.m, sizeof( vp.m ) );
	}
}

void Renderer::RenderShadows( const FrameState& fs, GpuModel* model )
{
	for( int i = 0; i < kMaxVolLights; ++i )
	{
		shadowMode[ i ] = 0;
		if( !model || i >= fs.vol.lights || !fs.xl[ i ].shadow )
			continue;
		const XLight& l = fs.xl[ i ];
		const bool cube = l.type == 1;
		if( cube && i >= 2 )
			continue; // point lights are A / B only, always first
		const int res   = cube ? kShadowResCube : kShadowRes2D;
		if( !shadowFBO[ i ] )
			glGenFramebuffers( 1, &shadowFBO[ i ] );
		if( !shadowDepth[ i ] )
		{
			glGenRenderbuffers( 1, &shadowDepth[ i ] );
			glBindRenderbuffer( GL_RENDERBUFFER, shadowDepth[ i ] );
			glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kShadowRes2D, kShadowRes2D );
			glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		}
		if( cube && !shadowCube[ i ] )
		{
			glGenTextures( 1, &shadowCube[ i ] );
			glBindTexture( GL_TEXTURE_CUBE_MAP, shadowCube[ i ] );
			for( int f = 0; f < 6; ++f )
				glTexImage2D( GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_R32F, res, res, 0, GL_RED, GL_FLOAT, nullptr );
			glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE );
			glBindTexture( GL_TEXTURE_CUBE_MAP, 0 );
		}
		if( !cube && !shadowTex[ i ] )
			MakeTarget2D( shadowTex[ i ], GL_R32F, GL_RED, GL_FLOAT, res, res, GL_NEAREST );

		glBindFramebuffer( GL_FRAMEBUFFER, shadowFBO[ i ] );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, shadowDepth[ i ] );
		glViewport( 0, 0, res, res );
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );
		glDepthMask( GL_TRUE );
		const int faces = cube ? 6 : 1;
		for( int f = 0; f < faces; ++f )
		{
			Mat4 vp;
			if( cube )
			{
				static const float dirs[ 6 ][ 3 ] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
				static const float ups[ 6 ][ 3 ]  = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };
				float at[ 3 ] = { l.pos[ 0 ] + dirs[ f ][ 0 ], l.pos[ 1 ] + dirs[ f ][ 1 ], l.pos[ 2 ] + dirs[ f ][ 2 ] };
				vp            = Mat4::Perspective( 3.14159265f * 0.5f, 1.0f, 0.05f, kShadowFar ) * Mat4::LookAt( l.pos, at, ups[ f ] );
				glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, shadowCube[ i ], 0 );
			}
			else
			{
				std::memcpy( vp.m, lightVP[ i ], sizeof( vp.m ) );
				glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadowTex[ i ], 0 );
			}
			glClearColor( 1e6f, 1e6f, 1e6f, 1e6f );
			glClearDepth( 1.0 );
			glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
			DrawCasters( fs, model, vp.m, l.pos, true );
		}
		glDisable( GL_DEPTH_TEST );
		shadowMode[ i ] = cube ? 2 : 1;
	}
	glClearColor( 0, 0, 0, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

bool Renderer::EnsureSceneDepth( int w, int h )
{
	if( sceneDepthTex && depthW == w && depthH == h )
		return true;
	MakeTarget2D( sceneDepthTex, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, w, h, GL_NEAREST ); // W9: the main depth has stencil (a blit needs the same format)
	if( !depthFBO )
		glGenFramebuffers( 1, &depthFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, depthFBO );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, sceneDepthTex, 0 );
	glDrawBuffer( GL_NONE );
	glReadBuffer( GL_NONE );
	bool ok = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	if( !ok )
	{
		Log( "scene depth target %dx%d incomplete", w, h );
		DelTex( sceneDepthTex );
		return false;
	}
	depthW = w, depthH = h;
	return true;
}

void Renderer::CopySceneDepth( int w, int h )
{
	glBindFramebuffer( GL_READ_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, depthFBO );
	glBlitFramebuffer( 0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

void Renderer::EnsureNoise()
{
	if( noiseTex )
		return;
	const int n = kNoiseRes;
	std::vector< uint8_t > px( size_t( n ) * n * n * 4 );
	for( int z = 0; z < n; ++z )
		for( int y = 0; y < n; ++y )
			for( int x = 0; x < n; ++x )
			{
				uint8_t* p = &px[ ( ( size_t( z ) * n + y ) * n + x ) * 4 ];
				// R: 4-octave fbm, base period 8 cells over the tile; GBA: one smooth octave each (warp)
				float f = 0, a = 0.5f, norm = 0;
				for( int o = 0, period = 8; o < 4; ++o, period *= 2, a *= 0.5f )
				{
					float s = float( period ) / n;
					f += a * ValueNoise( x * s, y * s, z * s, period, 0x1234U + o );
					norm += a;
				}
				p[ 0 ] = (uint8_t)std::lround( 255.0f * f / norm );
				for( int c = 1; c < 4; ++c )
				{
					float s = 4.0f / n;
					p[ c ]  = (uint8_t)std::lround( 255.0f * ValueNoise( x * s, y * s, z * s, 4, 0x9e37U * c ) );
				}
			}
	glGenTextures( 1, &noiseTex );
	glBindTexture( GL_TEXTURE_3D, noiseTex );
	GLint align = 4;
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &align );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage3D( GL_TEXTURE_3D, 0, GL_RGBA8, n, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
	glPixelStorei( GL_UNPACK_ALIGNMENT, align );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	for( GLenum wrap : { GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R } )
		glTexParameteri( GL_TEXTURE_3D, wrap, GL_REPEAT );
	glBindTexture( GL_TEXTURE_3D, 0 );
}

// the shared medium's uniforms (kMediumLib / kMediumSimLib): the march, the screen rays
void SetMediumUniforms( GLuint p, const FrameState& fs )
{
	const VolumeState& v = fs.vol;
	glUniform3fv( U( p, "uVolCenter" ), 1, v.center );
	glUniform1f( U( p, "uVolRadius" ), v.radius );
	glUniform1f( U( p, "uHaze" ), v.haze );
	glUniform1f( U( p, "uSmoke" ), v.smoke );
	glUniform1f( U( p, "uSmokeFreq" ), v.smokeFreq );
	glUniform1f( U( p, "uTurb" ), v.turb );
	glUniform3fv( U( p, "uDrift" ), 1, v.drift );
	glUniform1f( U( p, "uGlowCloud" ), v.glowCloud );
	float gp[ kMaxVolLights * 3 ] = {};
	GLint on[ kMaxVolLights ]      = {};
	for( int i = 0; i < v.lights && i < kMaxVolLights; ++i )
	{
		on[ i ] = fs.xl[ i ].front < 0.0f; // Front Only: no cloud around a light that sits behind the mask
		for( int k = 0; k < 3; ++k )
			gp[ i * 3 + k ] = fs.xl[ i ].pos[ k ];
	}
	glUniform3fv( U( p, "uGlowPos" ), kMaxVolLights, gp );
	glUniform1iv( U( p, "uGlowOn" ), kMaxVolLights, on );
	glUniform1f( U( p, "uG" ), v.g );
}

// W20: the sim medium's extra uniforms and samplers (units 2 = the smoke atlas, 3 = the projected input)
void Renderer::SetSimMediumUniforms( GLuint p, const FrameState& fs, const Mat4& VP )
{
	glUniform1i( U( p, "uRes" ), simRes );
	glUniform1i( U( p, "uTilesX" ), simTilesX );
	glUniform2f( U( p, "uAtlas" ), float( simAtlasW ), float( simAtlasH ) );
	glUniform3fv( U( p, "uBoxMin" ), 1, simBoxMin );
	glUniform1f( U( p, "uCell" ), simCell );
	glUniform1i( U( p, "uSimDetail" ), fs.sim.engine == 2 ? 1 : 0 );
	glUniform3fv( U( p, "uSmokeTint" ), 1, fs.sim.tint );
	const bool proj = fs.sim.project > 0.0f && fs.inputTex && inTex; // the mip-mapped copy of the input (UpdateInputCopy)
	glUniform1f( U( p, "uProject" ), proj ? fs.sim.project : 0.0f );
	glUniformMatrix4fv( U( p, "uProjVP" ), 1, GL_FALSE, VP.m );
	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_2D, simSca[ simS ] );
	glActiveTexture( GL_TEXTURE3 );
	glBindTexture( GL_TEXTURE_2D, proj ? inTex : whiteTex );
	glActiveTexture( GL_TEXTURE0 );
}

// W24: the Trip looks of the TRIP march variants (kTripLib); the sim variant also reads the velocity atlas on unit 4
void Renderer::SetTripUniforms( GLuint p, const FrameState& fs )
{
	const TripState& t = fs.trip;
	glUniform1i( U( p, "uColourBy" ), t.colourBy );
	glUniform1i( U( p, "uPalette" ), t.palette );
	glUniform1f( U( p, "uPalMix" ), t.palMix );
	glUniform1f( U( p, "uPalShift" ), t.palShift );
	glUniform1f( U( p, "uPalSpread" ), t.palSpread );
	glUniform3fv( U( p, "uPalA" ), 1, t.palA );
	glUniform3fv( U( p, "uPalB" ), 1, t.palB );
	glUniform1f( U( p, "uFilm" ), t.film );
	glUniform1f( U( p, "uFilmThick" ), t.filmThick );
	glUniform1f( U( p, "uVeins" ), t.veins );
	glUniform1f( U( p, "uVeinWidth" ), t.veinWidth );
	glUniform3fv( U( p, "uVeinColor" ), 1, t.veinColor );
	glUniform1f( U( p, "uContours" ), t.contours );
	glUniform1f( U( p, "uBands" ), t.bands );
	glUniform1f( U( p, "uBandSharp" ), t.bandSharp );
	glUniform1f( U( p, "uBandScroll" ), t.bandScroll );
	glUniform1f( U( p, "uBlack" ), t.black );
	glUniform1i( U( p, "uBlackBy" ), t.blackBy );
	glUniform3fv( U( p, "uBlackColor" ), 1, t.blackColor );
	glUniform3fv( U( p, "uTripCenter" ), 1, t.center );
	glUniform3fv( U( p, "uTripAxis" ), 1, t.axis );
	glUniform3fv( U( p, "uTripRight" ), 1, t.right );
	glUniform3fv( U( p, "uTripUp" ), 1, t.up );
	glUniform1f( U( p, "uTripUnit" ), t.unit );
	glUniform1i( U( p, "uKaleido" ), t.kaleido );
	glUniform1f( U( p, "uKTwist" ), t.kTwist );
	glUniform1f( U( p, "uKSpin" ), t.kSpin );
	glUniform1f( U( p, "uFractal" ), t.fractal );
	glUniform1f( U( p, "uFracScale" ), t.fracScale );
	glUniform1i( U( p, "uFracIter" ), t.fracIter );
	glUniformMatrix3fv( U( p, "uFracRot" ), 1, GL_FALSE, t.fracRot );
	glActiveTexture( GL_TEXTURE4 );
	glBindTexture( GL_TEXTURE_2D, simLive ? simVel[ simV ] : whiteTex );
	glActiveTexture( GL_TEXTURE0 );
}

// W26: the flames' look in the fire march (the sim fields come from SetSimMediumUniforms)
void Renderer::SetFireUniforms( GLuint p, const FrameState& fs )
{
	const SimState& s = fs.sim;
	glUniform1f( U( p, "uFlameTemp" ), s.flameTemp );
	glUniform1f( U( p, "uFlameGlow" ), s.flameGlow );
	glUniform3fv( U( p, "uFlameTint" ), 1, s.flameTint );
	glUniform1f( U( p, "uFlameTintMix" ), s.flameTintMix );
	glUniform1f( U( p, "uFlameSoot" ), s.flameSoot );
}

// W24: prism + hue echo on the finished fog (fog resolution). The output is also next frame's echo history.
void Renderer::RenderTripPost( const FrameState& fs, int w, int h )
{
	const TripState& t = fs.trip;
	if( tripW != fogW || tripH != fogH || !tripFBO[ 0 ] )
	{
		for( int i = 0; i < 2; ++i )
		{
			MakeTarget2D( tripTex[ i ], GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, fogW, fogH, GL_LINEAR );
			if( !tripFBO[ i ] )
				glGenFramebuffers( 1, &tripFBO[ i ] );
			glBindFramebuffer( GL_FRAMEBUFFER, tripFBO[ i ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tripTex[ i ], 0 );
		}
		tripW = fogW, tripH = fogH, tripHist = false;
	}
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	float c[ 4 ];
	for( int r = 0; r < 4; ++r )
		c[ r ] = VP.m[ r ] * t.center[ 0 ] + VP.m[ 4 + r ] * t.center[ 1 ] + VP.m[ 8 + r ] * t.center[ 2 ] + VP.m[ 12 + r ];
	const float cu = c[ 3 ] > 1e-4f ? c[ 0 ] / c[ 3 ] * 0.5f + 0.5f : 0.5f, cv = c[ 3 ] > 1e-4f ? c[ 1 ] / c[ 3 ] * 0.5f + 0.5f : 0.5f;
	const int dst = 1 - tripIdx;
	glBindFramebuffer( GL_FRAMEBUFFER, tripFBO[ dst ] );
	glViewport( 0, 0, fogW, fogH );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( quadVAO );
	glUseProgram( progTripPost );
	glUniform1i( U( progTripPost, "uHistOk" ), tripHist && t.echo > 0.0f ? 1 : 0 );
	glUniform1f( U( progTripPost, "uPrism" ), t.prism );
	glUniform1f( U( progTripPost, "uPrismAngle" ), t.prismAngle );
	glUniform2f( U( progTripPost, "uCenterUV" ), cu, cv );
	glUniform1f( U( progTripPost, "uAspect" ), float( w ) / float( std::max( h, 1 ) ) );
	glUniform1f( U( progTripPost, "uEcho" ), t.echo );
	glUniform1f( U( progTripPost, "uEchoHue" ), t.echoHue );
	glUniform1f( U( progTripPost, "uLines" ), t.lines );
	glUniform1f( U( progTripPost, "uLineW" ), t.lineWidth * float( fogW ) / float( std::max( w, 1 ) ) ); // fog pixels
	glUniform1f( U( progTripPost, "uLineGlow" ), t.lineGlow );
	glUniform1f( U( progTripPost, "uBands" ), t.bands );
	glUniform1f( U( progTripPost, "uBandScroll" ), t.bandScroll );
	glUniform1i( U( progTripPost, "uPalette" ), t.palette );
	glUniform1f( U( progTripPost, "uPalShift" ), t.palShift );
	glUniform1f( U( progTripPost, "uPalSpread" ), t.palSpread );
	glUniform3fv( U( progTripPost, "uPalA" ), 1, t.palA );
	glUniform3fv( U( progTripPost, "uPalB" ), 1, t.palB );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, tripTex[ tripIdx ] );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, fogOut );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindVertexArray( 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	tripIdx  = dst;
	tripHist = t.echo > 0.0f; // history is only kept while the echo runs (it restarts clean)
	fogOut   = tripTex[ dst ];
}

void Renderer::RenderVolume( const FrameState& fs, int w, int h )
{
	const VolumeState& v = fs.vol;
	EnsureNoise();
	const int div = std::max( 1, v.resDiv );
	const int fw = std::max( 1, w / div ), fh = std::max( 1, h / div );
	if( fw != fogW || fh != fogH || !marchFBO )
	{
		MakeTarget2D( fogTex, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, fw, fh, GL_LINEAR );
		MakeTarget2D( fogDepthTex, GL_R32F, GL_RED, GL_FLOAT, fw, fh, GL_NEAREST );
		MakeTarget2D( raySrcTex, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, fw, fh, GL_LINEAR );
		if( !marchFBO )
			glGenFramebuffers( 1, &marchFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, marchFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fogTex, 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, fogDepthTex, 0 );
		if( !raySrcFBO )
			glGenFramebuffers( 1, &raySrcFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, raySrcFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, raySrcTex, 0 );
		for( int i = 0; i < 2; ++i )
		{
			MakeTarget2D( histTex[ i ], GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, fw, fh, GL_LINEAR );
			if( !histFBO[ i ] )
				glGenFramebuffers( 1, &histFBO[ i ] );
			glBindFramebuffer( GL_FRAMEBUFFER, histFBO[ i ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, histTex[ i ], 0 );
		}
		fogW = fw, fogH = fh;
		histValid = false;
	}
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V, inv = VP.Inverse();
	++frameCounter;
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	glViewport( 0, 0, fw, fh );
	glBindVertexArray( quadVAO );
	const int lights = std::min( v.lights, kMaxVolLights );
	const bool sim   = fs.sim.engine > 0 && simLive; // W20: the medium reads the simulated smoke

	if( v.method == 0 )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, marchFBO );
		const GLenum bufs[ 2 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers( 2, bufs );
		const bool trip = fs.trip.medium; // W24: the TRIP variants (the plain ones run untouched otherwise)
		auto drawMarch  = [ & ]( GLuint march ) {
			glUseProgram( march );
			SetLightUniforms( march, fs, true );
			SetMediumUniforms( march, fs );
			if( sim )
				SetSimMediumUniforms( march, fs, VP );
			if( march == progFogMarchT || march == progFogMarchSimT || march == progFogMarchSimF )
				SetTripUniforms( march, fs );
			if( march == progFogMarchSimF )
				SetFireUniforms( march, fs );
			glUniformMatrix4fv( U( march, "uInvViewProj" ), 1, GL_FALSE, inv.m );
			glUniform3fv( U( march, "uCamPos" ), 1, fs.camPos );
			glUniform1i( U( march, "uSteps" ), v.steps );
			glUniform1f( U( march, "uFrame" ), float( frameCounter % 64 ) );
			glUniform1i( U( march, "uVolLights" ), lights );
			glUniform1i( U( march, "uFogLights" ), std::min( v.fogLights, fs.xlCount ) );
			glUniform1f( U( march, "uSpotBeams" ), v.spotBeams );
			const bool flash = v.flash[ 0 ] + v.flash[ 1 ] + v.flash[ 2 ] > 0.0f;
			const bool sk    = v.smokeAmbient > 0.0f || v.smokeKey > 0.0f || flash;
			glUniform1i( U( march, "uSKOn" ), sk ? 1 : 0 );
			if( sk )
			{
				float kc[ 3 ], amb[ 3 ];
				const float kmax = std::max( 1e-4f, std::max( fs.keyColor[ 0 ], std::max( fs.keyColor[ 1 ], fs.keyColor[ 2 ] ) ) );
				const float travel[ 3 ] = { -fs.keyDir[ 0 ], -fs.keyDir[ 1 ], -fs.keyDir[ 2 ] };
				for( int k = 0; k < 3; ++k )
					kc[ k ] = fs.keyColor[ k ] * v.smokeKey, amb[ k ] = fs.keyColor[ k ] / kmax * v.smokeAmbient + v.flash[ k ]; // ambient: the key's hue at unit brightness (+ W26 lightning flash)
				glUniform3fv( U( march, "uSKAmb" ), 1, amb );
				glUniform3fv( U( march, "uSKColor" ), 1, kc );
				glUniform3fv( U( march, "uSKDir" ), 1, travel );
				glUniform1i( U( march, "uSKShadowOn" ), keyShadowOk && v.smokeKey > 0.0f ? 1 : 0 );
				glUniformMatrix4fv( U( march, "uSKVP" ), 1, GL_FALSE, keyVP );
				glUniform3fv( U( march, "uSKPos" ), 1, keyPos );
			}
			glActiveTexture( GL_TEXTURE0 + kSKShadowUnit );
			glBindTexture( GL_TEXTURE_2D, sk && keyShadowOk ? keyShadowTex : whiteTex );
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
			glActiveTexture( GL_TEXTURE1 );
			glBindTexture( GL_TEXTURE_3D, noiseTex );
			glDrawArrays( GL_TRIANGLES, 0, 3 );
		};
		if( sim && simTripWarm && !trip )
		{
			// the sim (re)started: compile the TRIP sim march now through a 1-pixel scissor (UpdateSim did its passes)
			const GLboolean scissor = glIsEnabled( GL_SCISSOR_TEST ); // WarmVolume draws scissored already
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, 1, 1 );
			drawMarch( progFogMarchSimT );
			drawMarch( progFogMarchSimF ); // W26 fire
			if( !scissor )
				glDisable( GL_SCISSOR_TEST );
		}
		simTripWarm = false;
		if( v.projOnly )
		{
			// W26: nothing lights the medium but the projector: clear the target (no light, no coverage, the ray end far)
			const GLfloat zero[ 4 ] = { 0, 0, 0, 0 }, farD[ 4 ] = { 50, 50, 50, 50 };
			glClearBufferfv( GL_COLOR, 0, zero );
			glClearBufferfv( GL_COLOR, 1, farD );
		}
		else
		{
			drawMarch( sim ? ( fs.sim.fire ? progFogMarchSimF : trip ? progFogMarchSimT : progFogMarchSim ) : ( trip ? progFogMarchT : progFogMarch ) );
		}
		const GLenum one = GL_COLOR_ATTACHMENT0;
		glDrawBuffers( 1, &one );
		DrawProjMarch( fs, sim ); // W26: the projector's beam, added (rgb) before the temporal blend

		// temporal: history[histIdx] + current -> history[1 - histIdx]
		const int dst = 1 - histIdx;
		glBindFramebuffer( GL_FRAMEBUFFER, histFBO[ dst ] );
		glUseProgram( progFogTemporal );
		glUniform1i( U( progFogTemporal, "uHistValid" ), histValid ? 1 : 0 );
		glUniformMatrix4fv( U( progFogTemporal, "uInvViewProj" ), 1, GL_FALSE, inv.m );
		glUniformMatrix4fv( U( progFogTemporal, "uPrevViewProj" ), 1, GL_FALSE, prevVP );
		glUniform3fv( U( progFogTemporal, "uCamPos" ), 1, fs.camPos );
		glUniform2f( U( progFogTemporal, "uTexel" ), 1.0f / fw, 1.0f / fh );
		glUniform1f( U( progFogTemporal, "uBlend" ), 0.25f );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, fogTex );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, fogDepthTex );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, histTex[ histIdx ] );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		histIdx   = dst;
		histValid = true;
		fogOut    = histTex[ dst ];
	}
	else
	{
		// Screen Rays: occlusion source, then a radial blur toward each light
		float luv[ kMaxVolLights * 3 ] = {}, facing[ kMaxVolLights ] = {}, size[ kMaxVolLights ] = {};
		for( int i = 0; i < lights; ++i )
		{
			const XLight& l = fs.xl[ i ];
			float c[ 4 ];
			for( int r = 0; r < 4; ++r )
				c[ r ] = VP.m[ r ] * l.pos[ 0 ] + VP.m[ 4 + r ] * l.pos[ 1 ] + VP.m[ 8 + r ] * l.pos[ 2 ] + VP.m[ 12 + r ];
			const bool front = c[ 3 ] > 0.05f;
			luv[ i * 3 ]     = front ? c[ 0 ] / c[ 3 ] * 0.5f + 0.5f : 0.5f;
			luv[ i * 3 + 1 ] = front ? c[ 1 ] / c[ 3 ] * 0.5f + 0.5f : 0.5f;
			luv[ i * 3 + 2 ] = front ? 1.0f : 0.0f;
			float toCam[ 3 ] = { fs.camPos[ 0 ] - l.pos[ 0 ], fs.camPos[ 1 ] - l.pos[ 1 ], fs.camPos[ 2 ] - l.pos[ 2 ] };
			const float dist = std::sqrt( toCam[ 0 ] * toCam[ 0 ] + toCam[ 1 ] * toCam[ 1 ] + toCam[ 2 ] * toCam[ 2 ] );
			float f          = 1.0f;
			if( l.type == 0 && dist > 1e-4f )
			{
				const float cd   = ( l.dir[ 0 ] * toCam[ 0 ] + l.dir[ 1 ] * toCam[ 1 ] + l.dir[ 2 ] * toCam[ 2 ] ) / dist;
				const float half = std::min( 89.5f, l.coneDeg * 0.5f ) * 3.14159265f / 180.0f;
				const float co = std::cos( half ), ci = std::cos( half * ( 1.0f - 0.95f * l.coneSoft ) );
				float t        = std::min( 1.0f, std::max( 0.0f, ( cd - co ) / std::max( ci - co, 1e-4f ) ) );
				f              = std::max( t * t * ( 3.0f - 2.0f * t ), 0.15f );
			}
			facing[ i ] = f;
			size[ i ]   = 1.0f / std::max( dist, 0.3f );
		}
		glBindFramebuffer( GL_FRAMEBUFFER, raySrcFBO );
		const GLuint src = sim ? progRaySrcSim : progRaySrc;
		glUseProgram( src );
		SetLightUniforms( src, fs, true );
		SetMediumUniforms( src, fs );
		if( sim )
			SetSimMediumUniforms( src, fs, VP );
		glUniformMatrix4fv( U( src, "uInvViewProj" ), 1, GL_FALSE, inv.m );
		glUniform3fv( U( src, "uCamPos" ), 1, fs.camPos );
		glUniform1f( U( src, "uAspect" ), float( w ) / float( h ) );
		glUniform1i( U( src, "uVolLights" ), lights );
		glUniform3fv( U( src, "uLightUV" ), kMaxVolLights, luv );
		glUniform1fv( U( src, "uFacing" ), kMaxVolLights, facing );
		glUniform1fv( U( src, "uSpriteSize" ), kMaxVolLights, size );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_3D, noiseTex );
		glDrawArrays( GL_TRIANGLES, 0, 3 );

		glBindFramebuffer( GL_FRAMEBUFFER, histFBO[ 0 ] );
		glUseProgram( progRayBlur );
		glUniform1i( U( progRayBlur, "uVolLights" ), lights );
		glUniform3fv( U( progRayBlur, "uLightUV" ), kMaxVolLights, luv );
		glUniform1i( U( progRayBlur, "uSamples" ), v.steps );
		glUniform1f( U( progRayBlur, "uDensity" ), std::min( 1.0f, v.shaftLen / 6.0f ) );
		glUniform1f( U( progRayBlur, "uDecay" ), 0.95f + 0.02f * std::min( 2.0f, v.haze * 2.0f + v.smoke * 0.25f ) * 0.5f );
		glUniform1f( U( progRayBlur, "uWeight" ), 1.0f );
		glUniform1f( U( progRayBlur, "uFrame" ), float( frameCounter % 64 ) );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, raySrcTex );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		histValid = false;
		fogOut    = histTex[ 0 ];
	}
	std::memcpy( prevVP, VP.m, sizeof( prevVP ) );
	glActiveTexture( GL_TEXTURE0 + kSKShadowUnit );
	glBindTexture( GL_TEXTURE_2D, 0 );
	for( int u = 4; u >= 0; --u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBindTexture( GL_TEXTURE_3D, 0 );
	}
	UnbindShadowUnits();
	glBindVertexArray( 0 );
}

void Renderer::DrawMotes( const FrameState& fs, int w, int h )
{
	const VolumeState& v = fs.vol;
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
	glBindVertexArray( quadVAO );
	const GLuint p = progMotes;
	glUseProgram( p );
	SetLightUniforms( p, fs, true );
	glUniformMatrix4fv( U( p, "uViewProj" ), 1, GL_FALSE, VP.m );
	glUniform3fv( U( p, "uCamPos" ), 1, fs.camPos );
	glUniform3fv( U( p, "uVolCenter" ), 1, v.center );
	glUniform1f( U( p, "uVolRadius" ), v.radius );
	const float md[ 3 ] = { v.drift[ 0 ] / 100.0f, v.drift[ 1 ] / 100.0f, v.drift[ 2 ] / 100.0f }; // the drift loops at 100
	glUniform3fv( U( p, "uMoteDrift" ), 1, md );
	glUniform1f( U( p, "uTime" ), v.time );
	glUniform1f( U( p, "uPointScale" ), 16.0f * float( h ) / 1080.0f );
	glUniform1i( U( p, "uVolLights" ), std::min( v.fogLights, fs.xlCount ) );
	glUniform1f( U( p, "uG" ), v.g );
	glUniform2f( U( p, "uScreen" ), float( w ), float( h ) );
	glUniform1f( U( p, "uExposure" ), fs.exposure );
	glUniform1i( U( p, "uTonemap" ), fs.tonemap );
	glDrawArrays( GL_POINTS, 0, (GLsizei)std::lround( v.motes * 1500.0f ) );
	glDisable( GL_PROGRAM_POINT_SIZE );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	UnbindShadowUnits();
}

// The driver compiles each program variant on its first draw (30-60 ms here). Draw every volume pass once
// through a 1-pixel scissor at model swap, with a spot and a point light, so switching the volume on
// mid-show never hitches. All four volume lights: a spot, a point and two front-only eye spots.
void Renderer::WarmVolume( const FrameState& fs, GpuModel* model, int w, int h )
{
	FrameState t = fs;
	t.xlCount = t.vol.lights = t.vol.fogLights = kMaxVolLights;
	t.vol.active             = true;
	t.vol.motes              = 0.01f;
	for( int i = 0; i < kMaxVolLights; ++i )
	{
		t.xl[ i ]          = XLight();
		t.xl[ i ].type     = i == 1;
		t.xl[ i ].shadow   = true;
		t.xl[ i ].pos[ 2 ] = -2.0f;
		t.xl[ i ].front    = i >= 2 ? 0.3f : -1.0f;
	}
	glEnable( GL_SCISSOR_TEST );
	glScissor( 0, 0, 1, 1 );
	ComputeLightMatrices( t );
	RenderShadows( t, model );
	{
		// W2: the shadow passes' DEFORM twins (a deformed shape casts a deformed shadow)
		FrameState d = t;
		d.shape.on   = d.shape.deform = true;
		RenderShadows( d, model );
	}
	if( EnsureSceneDepth( w, h ) )
	{
		CopySceneDepth( w, h );
		for( int m = 0; m < 2; ++m )
		{
			t.vol.method = m;
			RenderVolume( t, w, h );
		}
		// W24: the TRIP march (the sim one too while the sim runs) and the Trip post, every look on
		t.vol.method   = 0;
		t.trip.medium  = t.trip.post = true;
		t.trip.palMix  = t.trip.film = t.trip.veins = t.trip.contours = t.trip.black = t.trip.fractal = 1.0f;
		t.trip.prism   = t.trip.echo = t.trip.lines = 0.5f;
		t.trip.kaleido = 6;
		t.trip.colourBy = 3;
		RenderVolume( t, w, h );
		RenderTripPost( t, w, h );
		tripHist = false;
		glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
		glViewport( 0, 0, w, h );
		DrawMotes( t, w, h );
	}
	glDisable( GL_SCISSOR_TEST );
	std::memset( shadowMode, 0, sizeof( shadowMode ) );
	histValid                         = false;
	fogOut                            = 0;
}
}
