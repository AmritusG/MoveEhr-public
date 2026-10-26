// W26 Stage 8: glass caustics (CausticShaders.h). Per frame while on: receiver distances (opaque mask) -> the caustic map's
// alpha; the refractor's positions + normals (glass parts or the whole mask, 512^2); the photons splatted into rgb (one pass,
// or three IORs for Caustic Rainbow). The map is then a cookie light (Projector.cpp BindCookieLights / DrawProjMarch) and
// lights the catcher plane in the W26 composite variant.
#include "CausticShaders.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr int kCausticRes = 1024, kPhotonRes = 512;
GLuint Tex( GLenum internal, GLenum format, int w, int h, GLenum filter )
{
	GLuint t;
	glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return t;
}
GLuint Depth( int w, int h )
{
	GLuint r;
	glGenRenderbuffers( 1, &r );
	glBindRenderbuffer( GL_RENDERBUFFER, r );
	glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	return r;
}
}

bool Renderer::InitCaustics( std::string& error )
{
	const std::string fs = std::string( "#define PHOTON_PASS\n" ) + kLightLib + kMainFS;
	progPhotonMain  = Link( kMainVS, fs, error );
	progPhotonSoup  = Link( kSoupVS, fs, error );
	progPhotonSplat = Link( kPhotonVS, kPhotonFS, error );
	progCausticInit = Link( kQuadVS, kCausticInitFS, error );
	if( !progPhotonMain || !progPhotonSoup || !progPhotonSplat || !progCausticInit )
		return false;
	for( GLuint p : { progPhotonMain, progPhotonSoup } )
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
	glUseProgram( progPhotonSplat );
	glUniform1i( U( progPhotonSplat, "uPhotonPos" ), 0 );
	glUniform1i( U( progPhotonSplat, "uPhotonNrm" ), 1 );
	glUniform1i( U( progPhotonSplat, "uRecv" ), 2 );
	glUseProgram( progCausticInit );
	glUniform1i( U( progCausticInit, "uRecv" ), 0 );
	glUseProgram( 0 );
	glGenVertexArrays( 1, &photonVAO );
	return true;
}

void Renderer::ReleaseCaustics()
{
	for( GLuint* p : { &progPhotonMain, &progPhotonSoup, &progPhotonSplat, &progCausticInit } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	for( GLuint* t : { &causticTex, &recvTex, &photonTex[ 0 ], &photonTex[ 1 ] } )
		if( *t )
			glDeleteTextures( 1, t ), *t = 0;
	for( GLuint* f : { &causticFBO, &recvFBO, &photonFBO } )
		if( *f )
			glDeleteFramebuffers( 1, f ), *f = 0;
	for( GLuint* r : { &causticDepth, &photonDepth } )
		if( *r )
			glDeleteRenderbuffers( 1, r ), *r = 0;
	if( photonVAO )
		glDeleteVertexArrays( 1, &photonVAO ), photonVAO = 0;
	causticOk = false;
}

void Renderer::RenderCaustics( const FrameState& fs, GpuModel* model )
{
	const CausticState& c = fs.caustic;
	causticOk             = false;
	if( !c.on || !model )
		return;
	if( !causticTex )
	{
		causticTex   = Tex( GL_RGBA32F, GL_RGBA, kCausticRes, kCausticRes, GL_LINEAR );
		recvTex      = Tex( GL_R32F, GL_RED, kCausticRes, kCausticRes, GL_NEAREST );
		photonTex[ 0 ] = Tex( GL_RGBA32F, GL_RGBA, kPhotonRes, kPhotonRes, GL_NEAREST );
		photonTex[ 1 ] = Tex( GL_RGBA32F, GL_RGBA, kPhotonRes, kPhotonRes, GL_NEAREST );
		causticDepth = Depth( kCausticRes, kCausticRes );
		photonDepth  = Depth( kPhotonRes, kPhotonRes );
		glGenFramebuffers( 1, &causticFBO );
		glGenFramebuffers( 1, &recvFBO );
		glGenFramebuffers( 1, &photonFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, recvFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, recvTex, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, causticDepth );
		glBindFramebuffer( GL_FRAMEBUFFER, causticFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, causticTex, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, photonFBO );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, photonDepth );
	}
	float at[ 3 ] = { c.pos[ 0 ] + c.dir[ 0 ], c.pos[ 1 ] + c.dir[ 1 ], c.pos[ 2 ] + c.dir[ 2 ] }, up[ 3 ] = { 0, 1, 0 };
	if( std::fabs( c.dir[ 1 ] ) > 0.95f )
		up[ 1 ] = 0, up[ 2 ] = -1;
	const Mat4 vp = Mat4::Perspective( c.fov * 3.14159265f / 180.0f, 1.0f, 0.05f, 60.0f ) * Mat4::LookAt( c.pos, at, up );
	std::memcpy( causticVP, vp.m, sizeof( causticVP ) );
	const bool soup = fs.wireMix > 0.0f || fs.explode > 0.0f || fs.shape.on;

	// 1. receivers: the opaque mask's distance from the light (glass parts let it through; Whole Mask: no receivers but the plane)
	glBindFramebuffer( GL_FRAMEBUFFER, recvFBO );
	glViewport( 0, 0, kCausticRes, kCausticRes );
	glClearColor( 1e6f, 1e6f, 1e6f, 1e6f );
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glClearColor( 0, 0, 0, 0 );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDisable( GL_BLEND );
	if( !c.wholeMask )
		DrawModel( soup ? progShadowSoup : progShadow, fs, model, false, soup, 0, vp.m, c.pos, nullptr );
	// 2. the refractor: positions, then shading normals
	glBindFramebuffer( GL_FRAMEBUFFER, photonFBO );
	glViewport( 0, 0, kPhotonRes, kPhotonRes );
	glassFilter = c.wholeMask ? 0 : 1;
	for( int i = 0; i < 2; ++i )
	{
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, photonTex[ i ], 0 );
		glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		const GLuint p = soup ? progPhotonSoup : progPhotonMain;
		glUseProgram( p );
		glUniform1i( U( p, "uPhotonOut" ), i );
		DrawModel( p, fs, model, false, soup, 0, vp.m, nullptr, nullptr );
	}
	glassFilter = 0;
	glDisable( GL_DEPTH_TEST );
	// 3. the caustic map: alpha = the receiver distance, rgb = the photons (additive Gaussian splats)
	glBindFramebuffer( GL_FRAMEBUFFER, causticFBO );
	glViewport( 0, 0, kCausticRes, kCausticRes );
	glBindVertexArray( quadVAO );
	glUseProgram( progCausticInit );
	glUniformMatrix4fv( U( progCausticInit, "uInvLVP" ), 1, GL_FALSE, vp.Inverse().m );
	glUniform3fv( U( progCausticInit, "uLPos" ), 1, c.pos );
	glUniform4fv( U( progCausticInit, "uPlane" ), 1, fs.post.catchPlane );
	glUniform1i( U( progCausticInit, "uPlaneOn" ), c.floor > 0.0f ? 1 : 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, recvTex );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glUseProgram( progPhotonSplat );
	const float size = 2.0f + 10.0f * ( 1.0f - c.sharp );
	// energy: a photon stands for (1024 / 512)^2 map texels; the splat's integral is size^2 x pi / 16
	const float energy = 4.0f / ( size * size * 0.19635f );
	glUniformMatrix4fv( U( progPhotonSplat, "uLVP" ), 1, GL_FALSE, vp.m );
	glUniform3fv( U( progPhotonSplat, "uLPos" ), 1, c.pos );
	glUniform4fv( U( progPhotonSplat, "uPlane" ), 1, fs.post.catchPlane );
	glUniform1i( U( progPhotonSplat, "uPlaneOn" ), c.floor > 0.0f ? 1 : 0 );
	glUniform1f( U( progPhotonSplat, "uSize" ), size );
	glUniform1f( U( progPhotonSplat, "uEnergy" ), energy );
	glUniform1f( U( progPhotonSplat, "uRipple" ), c.ripple );
	glUniform1f( U( progPhotonSplat, "uPhase" ), c.phase );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, photonTex[ 0 ] );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, photonTex[ 1 ] );
	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_2D, recvTex );
	glBindVertexArray( photonVAO );
	if( c.rainbow > 0.0f )
	{
		// dispersion: red bends least, blue most
		const float iors[ 3 ] = { c.ior - 0.04f * c.rainbow, c.ior, c.ior + 0.04f * c.rainbow };
		for( int k = 0; k < 3; ++k )
		{
			const float ch[ 3 ] = { k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f };
			glUniform1f( U( progPhotonSplat, "uIOR" ), iors[ k ] );
			glUniform3fv( U( progPhotonSplat, "uChannel" ), 1, ch );
			glDrawArrays( GL_POINTS, 0, kPhotonRes * kPhotonRes );
		}
	}
	else
	{
		const float ch[ 3 ] = { 1, 1, 1 };
		glUniform1f( U( progPhotonSplat, "uIOR" ), c.ior );
		glUniform3fv( U( progPhotonSplat, "uChannel" ), 1, ch );
		glDrawArrays( GL_POINTS, 0, kPhotonRes * kPhotonRes );
	}
	glBindVertexArray( 0 );
	glDisable( GL_PROGRAM_POINT_SIZE );
	glDisable( GL_BLEND );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	for( int u : { 2, 1, 0 } )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	causticOk = true;
}
}
