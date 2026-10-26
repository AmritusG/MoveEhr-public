// W26 Stage 5: lightning. The plugin builds the bolts (CPU, per strike: midpoint displacement + branches); this draws their
// segments as instanced screen-space quads (LightningShaders.h), additively after the composite.
#include "LightningShaders.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
}

bool Renderer::InitBolts( std::string& error )
{
	progBolt    = Link( kBoltVS, std::string( kToneLib ) + kBoltFS, error );
	progBoltAdd = Link( kQuadVS, std::string( kToneLib ) + kBoltAddFS, error );
	if( !progBolt || !progBoltAdd )
		return false;
	glUseProgram( progBoltAdd );
	glUniform1i( U( progBoltAdd, "uBolts" ), 0 );
	glUseProgram( progBolt );
	glUniform1i( U( progBolt, "uDepth" ), 0 );
	glUseProgram( 0 );
	glGenVertexArrays( 1, &boltVAO );
	glGenBuffers( 1, &boltVBO );
	glBindVertexArray( boltVAO );
	glBindBuffer( GL_ARRAY_BUFFER, boltVBO );
	for( int i = 0; i < 2; ++i )
	{
		glEnableVertexAttribArray( i );
		glVertexAttribPointer( i, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( float ), (void*)( size_t( i ) * 4 * sizeof( float ) ) );
		glVertexAttribDivisor( i, 1 );
	}
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	return true;
}

void Renderer::ReleaseBolts()
{
	for( GLuint* p : { &progBolt, &progBoltAdd } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	if( boltTex )
		glDeleteTextures( 1, &boltTex ), boltTex = 0;
	if( boltFBO )
		glDeleteFramebuffers( 1, &boltFBO ), boltFBO = 0;
	boltW = boltH = 0;
	if( boltVAO )
		glDeleteVertexArrays( 1, &boltVAO ), boltVAO = 0;
	if( boltVBO )
		glDeleteBuffers( 1, &boltVBO ), boltVBO = 0;
}

// into the bound target: rgb added, alpha untouched; needs sceneDepthTex for this frame. The HDR glow layer: the fireflies
// (additive) then the bolts (GL_MAX: their overlapping segments must not pile up), added onto the target once.
void Renderer::RenderGlow( const FrameState& fs, int w, int h )
{
	const BoltState& b = fs.bolts;
	const int n        = int( b.segs.size() / 8 );
	if( n <= 0 && fs.swarm.count <= 0 )
		return;
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	if( n > 0 )
	{
		glBindBuffer( GL_ARRAY_BUFFER, boltVBO );
		glBufferData( GL_ARRAY_BUFFER, b.segs.size() * sizeof( float ), b.segs.data(), GL_STREAM_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}
	const float px = float( h ) / 1080.0f;
	GLint target = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &target );
	if( boltW != w || boltH != h || !boltFBO )
	{
		if( !boltTex )
			glGenTextures( 1, &boltTex );
		glBindTexture( GL_TEXTURE_2D, boltTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		if( !boltFBO )
			glGenFramebuffers( 1, &boltFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, boltFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, boltTex, 0 );
		boltW = w, boltH = h;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, boltFBO );
	glViewport( 0, 0, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	DrawSwarm( fs, w, h ); // W26 Stage 6
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
	glEnable( GL_BLEND );
	glBlendEquation( GL_MAX );
	glDisable( GL_DEPTH_TEST );
	glBindVertexArray( boltVAO );
	if( n > 0 )
	{
		const GLuint p = progBolt;
		glUseProgram( p );
		glUniformMatrix4fv( U( p, "uViewProj" ), 1, GL_FALSE, VP.m );
		glUniform2f( U( p, "uScreen" ), float( w ), float( h ) );
		glUniform1f( U( p, "uWidth" ), b.width * px );
		glUniform1f( U( p, "uReach" ), ( b.width * 24.0f + 2.0f ) * px );
		glUniform1f( U( p, "uGlow" ), b.glow );
		glUniform1f( U( p, "uP10" ), -fs.proj[ 10 ] );
		glUniform1f( U( p, "uP14" ), -fs.proj[ 14 ] );
		glUniform1f( U( p, "uTol" ), b.tol );
		glUniform1f( U( p, "uExposure" ), fs.exposure );
		glUniform1i( U( p, "uTonemap" ), fs.tonemap );
		glUniform3fv( U( p, "uColor" ), 1, b.color );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, n );
	}
	glBlendEquation( GL_FUNC_ADD );
	// added onto the composite
	glBindFramebuffer( GL_FRAMEBUFFER, (GLuint)target );
	glViewport( 0, 0, w, h );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	glUseProgram( progBoltAdd );
	glUniform1f( U( progBoltAdd, "uExposure" ), fs.exposure );
	glUniform1i( U( progBoltAdd, "uTonemap" ), fs.tonemap );
	glBindTexture( GL_TEXTURE_2D, boltTex );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
}
}
