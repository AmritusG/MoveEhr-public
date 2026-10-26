// W26 Stage 7: lava cracks. The glow = the CRACK_PASS variants of the main programs drawn additively over the mask (as the
// projector's light); the split = the CRACKS soup variants (render + shadow), which DrawModel substitutes while Lava Split > 0.
#include "Log.h"
#include "Renderer.h"
#include "Shaders.h"
#include <algorithm>
#include <cstdio>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
void BindMain( GLuint p )
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
}

bool Renderer::InitCracks( std::string& error )
{
	const std::string mainFS   = std::string( kLightLib ) + kMainFS;
	const std::string shadowFS = std::string( "#define SHADOW_PASS\n" ) + mainFS;
	const std::string crackFS  = std::string( "#define CRACK_PASS\n" ) + kCrackLib + mainFS;
	const std::string soupC    = std::string( "#define CRACKS\n" ) + kCrackLib + kSoupVS;
	progSoupC       = Link( soupC, mainFS, error );
	progShadowSoupC = Link( soupC, shadowFS, error );
	progCrackMain   = Link( kMainVS, crackFS, error );
	progCrackSoup   = Link( kSoupVS, crackFS, error );
	progCrackSoupC  = Link( soupC, crackFS, error );
	if( !progSoupC || !progShadowSoupC || !progCrackMain || !progCrackSoup || !progCrackSoupC )
		return false;
	for( GLuint p : { progSoupC, progShadowSoupC, progCrackMain, progCrackSoup, progCrackSoupC } )
		BindMain( p );
	glUseProgram( 0 );
	return true;
}

// the crack glow of the FRONT-most surface (depth tested against itself), half res: the heat haze's mask
bool Renderer::DrawHazeMask( const FrameState& fs, GpuModel* model, int w, int h )
{
	const int hw = std::max( 1, w / 2 ), hh = std::max( 1, h / 2 );
	if( hw != hazeW || hh != hazeH || !hazeFBO )
	{
		if( !hazeTex )
			glGenTextures( 1, &hazeTex );
		glBindTexture( GL_TEXTURE_2D, hazeTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, hw, hh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		if( !hazeDepth )
			glGenRenderbuffers( 1, &hazeDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, hazeDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, hw, hh );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		if( !hazeFBO )
			glGenFramebuffers( 1, &hazeFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, hazeFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hazeTex, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, hazeDepth );
		hazeW = hw, hazeH = hh;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, hazeFBO );
	glViewport( 0, 0, hw, hh );
	glClearColor( 0, 0, 0, 0 );
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDisable( GL_BLEND );
	const bool soup = fs.wireMix > 0.0f || fs.explode > 0.0f || fs.shape.on;
	FrameState f  = fs;
	f.fogDensity = 0.0f;
	EnsureVAO( model );
	DrawModel( soup ? progCrackSoup : progCrackMain, f, model, false, soup, 0, nullptr, nullptr, nullptr );
	glDisable( GL_DEPTH_TEST );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	return true;
}

void Renderer::ReleaseCracks()
{
	for( GLuint* p : { &progSoupC, &progShadowSoupC, &progCrackMain, &progCrackSoup, &progCrackSoupC } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	if( hazeTex )
		glDeleteTextures( 1, &hazeTex ), hazeTex = 0;
	if( hazeFBO )
		glDeleteFramebuffers( 1, &hazeFBO ), hazeFBO = 0;
	if( hazeDepth )
		glDeleteRenderbuffers( 1, &hazeDepth ), hazeDepth = 0;
	hazeW = hazeH = 0;
}

void Renderer::DrawCracks( const FrameState& fs, GpuModel* model )
{
	const LavaState& lv = fs.lava;
	if( !lv.on || !model )
		return;
	const bool soup = fs.wireMix > 0.0f || fs.explode > 0.0f || fs.shape.on;
	const GLuint p  = soup ? progCrackSoup : progCrackMain; // DrawModel swaps in progCrackSoupC while split
	glUseProgram( soup && lv.split > 0.0f ? progCrackSoupC : p );
	for( GLuint q : { progCrackMain, progCrackSoup, progCrackSoupC } )
	{
		glUseProgram( q );
		glUniform1f( U( q, "uLavaGlow" ), lv.glow );
		glUniform1f( U( q, "uLavaTemp" ), lv.temp );
		glUniform1f( U( q, "uCrackWidth" ), lv.width );
		glUniform3fv( U( q, "uLavaTint" ), 1, lv.tint );
		glUniform1f( U( q, "uLavaTintMix" ), lv.tintMix );
		glUniform1f( U( q, "uCrackFlow" ), lv.flow );
	}
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDepthMask( GL_FALSE );
	glEnable( GL_POLYGON_OFFSET_FILL );
	glPolygonOffset( -1.0f, -4.0f );
	FrameState f  = fs;
	f.fogDensity = 0.0f;
	DrawModel( p, f, model, false, soup, 0, nullptr, nullptr, nullptr );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glPolygonOffset( 0.0f, 0.0f );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LESS );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
}
}
