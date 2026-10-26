// Stage 2: key-light shadow map (shadow catcher), mirrored scene (mirror floor), silhouette alpha blur,
// bloom + lens flares. Each pass runs only while its effect is non-zero.
#include "Log.h"
#include "MathUtil.h"
#include "PostShaders.h"
#include "Renderer.h"
#include "Shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr int kKeyShadowRes = 1024;

void Target( GLuint& tex, GLuint& fbo, GLenum internal, GLenum format, GLenum type, int w, int h )
{
	if( !tex )
		glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexImage2D( GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	if( !fbo )
		glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}
void Del( GLuint& t, bool fbo = false )
{
	if( t )
		fbo ? glDeleteFramebuffers( 1, &t ) : glDeleteTextures( 1, &t ), t = 0;
}
}

bool Renderer::InitPost( std::string& error )
{
	progBright = Link( kQuadVS, kBrightFS, error );
	progDown   = Link( kQuadVS, kDownFS, error );
	progUp     = Link( kQuadVS, kUpFS, error );
	progGhost  = Link( kQuadVS, kGhostFS, error );
	progBlur1D = Link( kQuadVS, kBlur1DFS, error );
	progPost   = Link( kQuadVS, kPostFS, error );
	progTrail  = Link( kQuadVS, kTrailFS, error );
	progEcho   = Link( kQuadVS, kEchoFS, error );
	if( !progBright || !progDown || !progUp || !progGhost || !progBlur1D || !progPost || !progTrail || !progEcho )
		return false;
	glUseProgram( progTrail );
	glUniform1i( U( progTrail, "uPrevCol" ), 0 );
	glUniform1i( U( progTrail, "uPrevAge" ), 1 );
	glUniform1i( U( progTrail, "uLive" ), 2 );
	glUseProgram( progEcho );
	glUniform1i( U( progEcho, "uCol" ), 0 );
	glUniform1i( U( progEcho, "uAge" ), 1 );
	for( GLuint p : { progBright, progDown, progGhost, progBlur1D } )
	{
		glUseProgram( p );
		glUniform1i( U( p, "uSrc" ), 0 );
	}
	glUseProgram( progUp );
	glUniform1i( U( progUp, "uLow" ), 0 );
	glUniform1i( U( progUp, "uCur" ), 1 );
	glUseProgram( progPost );
	glUniform1i( U( progPost, "uBase" ), 0 );
	glUniform1i( U( progPost, "uBloom" ), 1 );
	glUniform1i( U( progPost, "uGhost" ), 2 );
	glUniform1i( U( progPost, "uStreak" ), 3 );
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleasePost()
{
	for( GLuint* p : { &progBright, &progDown, &progUp, &progGhost, &progBlur1D, &progPost } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	Del( postTex ), Del( postFBO, true );
	for( int i = 0; i < kBloomLevels; ++i )
		Del( downTex[ i ] ), Del( downFBO[ i ], true ), Del( upTex[ i ] ), Del( upFBO[ i ], true );
	Del( ghostTex ), Del( ghostFBO, true ), Del( streakTex[ 0 ] ), Del( streakFBO[ 0 ], true ), Del( streakTex[ 1 ] ), Del( streakFBO[ 1 ], true );
	Del( silTex[ 0 ] ), Del( silFBO[ 0 ], true ), Del( silTex[ 1 ] ), Del( silFBO[ 1 ], true );
	Del( keyShadowTex ), Del( keyShadowFBO, true );
	if( keyShadowDepth )
		glDeleteRenderbuffers( 1, &keyShadowDepth ), keyShadowDepth = 0;
	Del( mirrorTex ), Del( mirrorFBO, true );
	Del( faceTex ), Del( faceFBO, true );
	if( facePBO )
		glDeleteBuffers( 1, &facePBO ), facePBO = 0;
	if( faceFence )
		glDeleteSync( faceFence ), faceFence = nullptr;
	if( progFaceCopy )
		glDeleteProgram( progFaceCopy ), progFaceCopy = 0;
	facePending = faceReady = false;
	faceW = faceH = 0;
	for( int i = 0; i < 2; ++i )
		Del( trailCol[ i ] ), Del( trailAge[ i ] ), Del( trailFBO[ i ], true );
	Del( echoTex ), Del( echoFBO, true );
	for( GLuint* p : { &progTrail, &progEcho } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	echoW = echoH = 0;
	trailLive = false;
	if( mirrorDepth )
		glDeleteRenderbuffers( 1, &mirrorDepth ), mirrorDepth = 0;
	postW = postH = mirrorW = mirrorH = silW = silH = 0;
}

// Orthographic shadow map of the model from the key light, around the mask.
void Renderer::RenderKeyShadow( const FrameState& fs, GpuModel* model )
{
	keyShadowOk = false;
	if( !model || ( fs.post.shadowAmt <= 0.0f && !( fs.vol.active && fs.vol.smokeKey > 0.0f ) ) ) // W23: Smoke Key Light shadows the smoke
		return;
	if( !keyShadowTex )
	{
		Target( keyShadowTex, keyShadowFBO, GL_R32F, GL_RED, GL_FLOAT, kKeyShadowRes, kKeyShadowRes );
		glBindTexture( GL_TEXTURE_2D, keyShadowTex );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenRenderbuffers( 1, &keyShadowDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, keyShadowDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kKeyShadowRes, kKeyShadowRes );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, keyShadowFBO );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, keyShadowDepth );
	}
	const float R = std::max( 0.3f, fs.post.maskRadius * 1.4f );
	for( int k = 0; k < 3; ++k )
	{
		keyPos[ k ] = fs.post.maskCenter[ k ] + fs.keyDir[ k ] * ( R * 4.0f );
		keyDir[ k ] = -fs.keyDir[ k ];
	}
	float up[ 3 ] = { 0, 1, 0 };
	if( std::fabs( fs.keyDir[ 1 ] ) > 0.95f )
		up[ 1 ] = 0, up[ 2 ] = 1;
	const Mat4 vp = Mat4::Ortho( -R, R, -R, R, 0.01f, R * 8.0f ) * Mat4::LookAt( keyPos, fs.post.maskCenter, up );
	std::memcpy( keyVP, vp.m, sizeof( keyVP ) );

	glBindFramebuffer( GL_FRAMEBUFFER, keyShadowFBO );
	glViewport( 0, 0, kKeyShadowRes, kKeyShadowRes );
	glClearColor( 1e6f, 1e6f, 1e6f, 1e6f );
	glClearDepth( 1.0 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glClearColor( 0, 0, 0, 0 );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	// every caster program DrawCasters may pick (the pair mid-hop: shards of a Shatter hop too)
	const GLuint casters[] = { progShadow, progShadowI, progShadowSoup, progShadowSoupI, progShadowSoupT, progShadowSoupTI };
	for( GLuint q : casters )
	{
		glUseProgram( q );
		glUniform1i( U( q, "uShadowOrtho" ), 1 );
		glUniform3fv( U( q, "uShadowDir" ), 1, keyDir );
	}
	casterOrtho = 1; // W4 A1: the voxel casters write the same value
	std::memcpy( casterDir, keyDir, sizeof( casterDir ) );
	DrawCasters( fs, model, vp.m, keyPos, true );
	casterOrtho = 0;
	for( GLuint q : casters )
	{
		glUseProgram( q );
		glUniform1i( U( q, "uShadowOrtho" ), 0 );
	}
	glDisable( GL_DEPTH_TEST );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	keyShadowOk = true;
}

// The mask reflected in the floor plane, lit by reflected lights, seen by the real camera.
void Renderer::RenderMirror( const FrameState& fs, GpuModel* model, int w, int h )
{
	mirrorOk = false;
	if( !model || fs.post.mirror <= 0.0f )
		return;
	if( w != mirrorW || h != mirrorH || !mirrorFBO )
	{
		Target( mirrorTex, mirrorFBO, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h );
		if( !mirrorDepth )
			glGenRenderbuffers( 1, &mirrorDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, mirrorDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, mirrorFBO );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mirrorDepth );
		mirrorW = w, mirrorH = h;
	}
	// reflection about y = floorY
	const float fy = fs.post.floorY;
	Mat4 R         = Mat4::Identity();
	R.m[ 5 ]       = -1.0f;
	R.m[ 13 ]      = 2.0f * fy;
	auto reflect   = [ & ]( const FrameState& src ) {
		FrameState m = src;
		Mat4 M;
		std::memcpy( M.m, src.model, sizeof( M.m ) );
		const Mat4 RM = R * M;
		std::memcpy( m.model, RM.m, sizeof( RM.m ) );
		m.keyDir[ 1 ]  = -m.keyDir[ 1 ];
		m.fillDir[ 1 ] = -m.fillDir[ 1 ];
		for( int i = 0; i < m.xlCount; ++i )
		{
			m.xl[ i ].pos[ 1 ] = 2.0f * fy - m.xl[ i ].pos[ 1 ];
			m.xl[ i ].dir[ 1 ] = -m.xl[ i ].dir[ 1 ];
		}
		m.outline = 0;
		for( Instance& in : m.inst )
		{
			Mat4 im;
			std::memcpy( im.m, in.m, sizeof( im.m ) );
			const Mat4 rim = R * im;
			std::memcpy( in.m, rim.m, sizeof( in.m ) );
		}
		return m;
	};
	glBindFramebuffer( GL_FRAMEBUFFER, mirrorFBO );
	glViewport( 0, 0, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClearDepth( 1.0 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	if( fs.wireMix > 0.0f )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	}
	// the reflected lights have no shadow maps of their own: draw unshadowed
	int saved[ kMaxVolLights ];
	std::memcpy( saved, shadowMode, sizeof( saved ) );
	std::memset( shadowMode, 0, sizeof( shadowMode ) );
	// LoadEhr W1: mid-hop the floor reflects the whole pair, each side with its half of the transition (as the picture)
	const int keep = activeSide;
	for( int side = 0; side < ( pairOn ? 2 : 1 ); ++side )
	{
		const FrameState m = reflect( pairOn ? *pairFs[ side ] : fs );
		GpuModel* g        = pairOn ? pairModel[ side ] : model;
		if( pairOn )
			activeSide = side;
		const bool soup    = m.wireMix > 0.0f || m.explode > 0.0f || m.shape.on;
		const GLuint soupP = m.shape.transSide > 0 && !m.shape.dustTrans ? progSoupT : progSoup;
		EnsureVAO( g );
		if( m.vox.look > 0 && voxDrawn[ pairOn ? side : activeSide ] ) // W4 A1: the voxels in the floor (the reflected model matrix and lights)
			DrawVoxels( m, g, pairOn ? side : -1, voxHopT );
		else
			DrawModel( soup ? soupP : progMain, m, g, false, soup, 0, nullptr, nullptr, m.inst.empty() ? nullptr : &m.inst );
	}
	activeSide = keep;
	EnsureVAO( model );
	std::memcpy( shadowMode, saved, sizeof( saved ) );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	glDisable( GL_DEPTH_TEST );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	mirrorOk = true;
}

void Renderer::Blur1D( GLuint src, GLuint dstFBO, int w, int h, float dx, float dy, int taps )
{
	glBindFramebuffer( GL_FRAMEBUFFER, dstFBO );
	glViewport( 0, 0, w, h );
	glUseProgram( progBlur1D );
	glUniform2f( U( progBlur1D, "uDir" ), dx, dy );
	glUniform1i( U( progBlur1D, "uTaps" ), taps );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, src );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

// Blurred scene alpha at half res: the silhouette's outer glow.
void Renderer::RenderSilhouetteBlur( const FrameState& fs, int w, int h )
{
	silOk = false;
	if( fs.post.sil <= 0.0f )
		return;
	const int sw = std::max( 1, w / 2 ), sh = std::max( 1, h / 2 );
	if( sw != silW || sh != silH || !silFBO[ 0 ] )
	{
		for( int i = 0; i < 2; ++i )
			Target( silTex[ i ], silFBO[ i ], GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, sw, sh );
		silW = sw, silH = sh;
	}
	glDisable( GL_BLEND );
	glDisable( GL_DEPTH_TEST );
	glBindVertexArray( quadVAO );
	// width in output pixels -> half-res texels, spread over 12 taps
	const float step = std::max( 0.5f, fs.post.silWidth * 0.5f / 12.0f );
	Blur1D( resolveTex, silFBO[ 0 ], sw, sh, step / sw, 0.0f, 12 );
	Blur1D( silTex[ 0 ], silFBO[ 1 ], sw, sh, 0.0f, step / sh, 12 );
	glBindVertexArray( 0 );
	silOk = true;
}

bool Renderer::PostActive( const FrameState& fs ) const
{
	return fs.post.bloom > 0.0f || fs.post.ghosts > 0.0f || fs.post.streak > 0.0f || LensOn( fs ); // W9: the black hole's lens is in the post pass
}

void Renderer::EnsurePostTarget( int w, int h )
{
	if( w == postW && h == postH && postFBO )
		return;
	Target( postTex, postFBO, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h );
	for( int i = 0; i < kBloomLevels; ++i )
	{
		const int lw = std::max( 1, w >> ( i + 1 ) ), lh = std::max( 1, h >> ( i + 1 ) );
		Target( downTex[ i ], downFBO[ i ], GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, lw, lh );
		Target( upTex[ i ], upFBO[ i ], GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, lw, lh );
	}
	const int qw = std::max( 1, w / 4 ), qh = std::max( 1, h / 4 );
	Target( ghostTex, ghostFBO, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, qw, qh );
	for( int i = 0; i < 2; ++i )
		Target( streakTex[ i ], streakFBO[ i ], GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, qw, qh );
	postW = w, postH = h;
}

// postTex (the composite) -> bloom + flares -> host FBO
void Renderer::RenderPost( const FrameState& fs, int w, int h )
{
	const PostState& p = fs.post;
	glDisable( GL_BLEND );
	glDisable( GL_DEPTH_TEST );
	glBindVertexArray( quadVAO );
	auto lw = [ & ]( int i ) { return std::max( 1, w >> ( i + 1 ) ); };
	auto lh = [ & ]( int i ) { return std::max( 1, h >> ( i + 1 ) ); };

	// bright pass -> level 0 (half res)
	glBindFramebuffer( GL_FRAMEBUFFER, downFBO[ 0 ] );
	glViewport( 0, 0, lw( 0 ), lh( 0 ) );
	glUseProgram( progBright );
	glUniform2f( U( progBright, "uSrcTexel" ), 1.0f / w, 1.0f / h );
	glUniform1f( U( progBright, "uThreshold" ), p.bloomThreshold );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, postTex );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glUseProgram( progDown );
	for( int i = 1; i < kBloomLevels; ++i )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, downFBO[ i ] );
		glViewport( 0, 0, lw( i ), lh( i ) );
		glUniform2f( U( progDown, "uSrcTexel" ), 1.0f / lw( i - 1 ), 1.0f / lh( i - 1 ) );
		glBindTexture( GL_TEXTURE_2D, downTex[ i - 1 ] );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
	}
	// up: up[last] = down[last]; up[i] = down[i] + tent(up[i+1]) * radius
	const int last = kBloomLevels - 1;
	glUseProgram( progUp );
	for( int i = last; i >= 0; --i )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, upFBO[ i ] );
		glViewport( 0, 0, lw( i ), lh( i ) );
		const bool bottom = i == last;
		glUniform2f( U( progUp, "uLowTexel" ), 1.0f / lw( bottom ? i : i + 1 ), 1.0f / lh( bottom ? i : i + 1 ) );
		glUniform1f( U( progUp, "uRadius" ), bottom ? 0.0f : 0.35f + 1.3f * p.bloomRadius );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, bottom ? downTex[ i ] : upTex[ i + 1 ] );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, downTex[ i ] );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
	}
	const int qw = std::max( 1, w / 4 ), qh = std::max( 1, h / 4 );
	if( p.ghosts > 0.0f )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, ghostFBO );
		glViewport( 0, 0, qw, qh );
		glUseProgram( progGhost );
		glUniform1i( U( progGhost, "uGhosts" ), 6 );
		glUniform1f( U( progGhost, "uAspect" ), float( w ) / float( h ) );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, downTex[ 1 ] );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
	}
	if( p.streak > 0.0f )
	{
		Blur1D( downTex[ 1 ], streakFBO[ 0 ], qw, qh, 2.0f / qw, 0.0f, 24 );
		Blur1D( streakTex[ 0 ], streakFBO[ 1 ], qw, qh, 8.0f / qw, 0.0f, 24 );
	}

	glBindFramebuffer( GL_FRAMEBUFFER, fs.hostFBO );
	glViewport( 0, 0, w, h );
	const GLuint progPost = LensOn( fs ) ? progPostLens : this->progPost; // W9: the lens variant (the plain post program is unchanged)
	glUseProgram( progPost );
	glUniform1f( U( progPost, "uBloomAmt" ), p.bloom );
	glUniform1f( U( progPost, "uGhostAmt" ), p.ghosts );
	glUniform1f( U( progPost, "uStreakAmt" ), p.streak );
	if( LensOn( fs ) )
		SetLensUniforms( progPost, fs, w, h );
	GLuint tex[ 4 ] = { postTex, upTex[ 0 ], p.ghosts > 0.0f ? ghostTex : whiteTex, p.streak > 0.0f ? streakTex[ 1 ] : whiteTex };
	for( int i = 3; i >= 0; --i )
	{
		glActiveTexture( GL_TEXTURE0 + i );
		glBindTexture( GL_TEXTURE_2D, tex[ i ] );
	}
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	for( int i = 3; i >= 0; --i )
	{
		glActiveTexture( GL_TEXTURE0 + i );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glBindVertexArray( 0 );
}

// Draw every Stage 2 pass once through a 1-pixel scissor at model swap (lazy driver compile).
void Renderer::WarmPost( const FrameState& fs, GpuModel* model, int w, int h )
{
	FrameState t      = fs;
	t.post.shadowAmt  = 1;
	t.post.mirror     = 1;
	t.post.sil        = 1;
	t.post.bloom = t.post.ghosts = t.post.streak = 1;
	glEnable( GL_SCISSOR_TEST );
	glScissor( 0, 0, 1, 1 );
	RenderKeyShadow( t, model );
	RenderMirror( t, model, w, h );
	RenderSilhouetteBlur( t, w, h );
	t.trail.echoes = 1; // the trail feedback + resolve programs (1 px; the trail restarts on its first real frame)
	t.trail.dust   = 1;
	UpdateEchoes( t, w, h );
	trailLive = echoOk = false;
	EnsurePostTarget( w, h );
	t.hostFBO = mirrorFBO; // an RGBA8 target of our own, not the host's (and not postTex, which it reads)
	RenderPost( t, w, h );
	t.x.lens = true, t.x.lensAmt = 0.5f; // W9: the black hole's lens variant
	RenderPost( t, w, h );
	glDisable( GL_SCISSOR_TEST );
	keyShadowOk = mirrorOk = silOk = false;
}

// Trail Echoes (W15): a feedback buffer instead of a ring of snapshots. Smear 0 stamps the live mask every Delay
// frames (discrete copies, as before, but they now slide and fade continuously instead of jumping every Delay
// frames); Smear 1 stamps every frame, with sub-stamps along the shift, so the trail is one streak.
void Renderer::UpdateEchoes( const FrameState& fs, int w, int h )
{
	echoOk = false;
	const TrailState& t = fs.trail;
	if( t.echoes <= 0 )
	{
		trailLive = false;
		return;
	}
	if( !progTrail || !progEcho )
		return;
	if( w != echoW || h != echoH || !echoFBO )
	{
		for( int i = 0; i < 2; ++i )
		{
			Target( trailAge[ i ], trailFBO[ i ], GL_R16F, GL_RED, GL_HALF_FLOAT, w, h );
			Target( trailCol[ i ], trailFBO[ i ], GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, w, h ); // COLOR0; the age goes on COLOR1
			glBindFramebuffer( GL_FRAMEBUFFER, trailFBO[ i ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, trailAge[ i ], 0 );
			const GLenum bufs[ 2 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			glDrawBuffers( 2, bufs );
		}
		Target( echoTex, echoFBO, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, w, h );
		echoW = w, echoH = h, trailLive = false;
	}
	const int delay = std::max( 1, t.delay );
	const float life = float( ( t.echoes + 1 ) * delay ) * ( t.curve == 2 ? 1.5f : 1.0f ); // frames
	// Smear: stamp more often (Delay frames at 0, every frame at 1) ...
	const int every = std::max( 1, (int)std::lround( float( delay ) + ( 1.0f - float( delay ) ) * std::min( 1.0f, t.smear * 1.5f ) ) );
	const bool fresh = !trailLive;
	const bool stamp = fresh || ( echoFrame % every ) == 0;
	echoFrame        = fresh ? 1 : echoFrame + 1;
	// ... the shift per frame in whole pixels (the remainder carries over, so the total is exact)
	float px[ 2 ] = { t.offset[ 0 ] * float( w ) / float( delay ), t.offset[ 1 ] * float( h ) / float( delay ) };
	int ip[ 2 ];
	for( int k = 0; k < 2; ++k )
	{
		trailShift[ k ] += px[ k ];
		ip[ k ] = (int)trailShift[ k ];
		trailShift[ k ] -= float( ip[ k ] );
	}
	// ... and above Smear 2/3 extra stamps fill the gap a stamp every frame still leaves (about one per pixel of shift)
	const float len = std::sqrt( float( ip[ 0 ] * ip[ 0 ] + ip[ 1 ] * ip[ 1 ] ) ) * float( every );
	const int sub   = 1 + (int)std::lround( std::min( 15.0f, std::max( 0.0f, len - 1.0f ) ) * std::min( 1.0f, std::max( 0.0f, t.smear * 3.0f - 2.0f ) ) );

	const int src = trailIdx, dst = 1 - trailIdx;
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( quadVAO );
	glBindFramebuffer( GL_FRAMEBUFFER, trailFBO[ dst ] );
	glViewport( 0, 0, w, h );
	glUseProgram( progTrail );
	glUniform2f( U( progTrail, "uTexel" ), 1.0f / w, 1.0f / h );
	glUniform2f( U( progTrail, "uShift" ), float( ip[ 0 ] ) / w, float( ip[ 1 ] ) / h );
	glUniform2f( U( progTrail, "uDrift" ), t.drift[ 0 ], t.drift[ 1 ] );
	glUniform1f( U( progTrail, "uStep" ), 1.0f / life );
	glUniform1f( U( progTrail, "uBlur" ), t.blur * 8.0f * float( h ) / 1080.0f ); // px per frame at the trail's end
	glUniform1f( U( progTrail, "uDust" ), t.dust );
	glUniform1i( U( progTrail, "uStamp" ), stamp ? 1 : 0 );
	glUniform1i( U( progTrail, "uSub" ), stamp ? sub : 1 );
	glUniform1i( U( progTrail, "uFresh" ), fresh ? 1 : 0 );
	const GLuint tex[ 3 ] = { trailCol[ src ], trailAge[ src ], resolveTex };
	for( int u = 0; u < 3; ++u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, tex[ u ] );
	}
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	trailIdx  = dst;
	trailLive = true;

	glBindFramebuffer( GL_FRAMEBUFFER, echoFBO );
	glUseProgram( progEcho );
	glUniform1f( U( progEcho, "uDecay" ), t.decay );
	glUniform1f( U( progEcho, "uCopies" ), life / float( delay ) );
	glUniform1i( U( progEcho, "uCurve" ), t.curve );
	glUniform1f( U( progEcho, "uDust" ), t.dust );
	glUniform1f( U( progEcho, "uGrain" ), std::max( 0.5f, t.grainPx ) );
	glUniform2fv( U( progEcho, "uGrainPhase" ), 1, t.grainPhase );
	glUniform3fv( U( progEcho, "uColor" ), 1, t.color );
	glUniform1f( U( progEcho, "uMix" ), t.colorMix );
	glUniform1f( U( progEcho, "uGlow" ), t.glow );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, trailCol[ dst ] );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, trailAge[ dst ] );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	for( int u = 2; u >= 0; --u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glBindVertexArray( 0 );
	echoOk = true;
}

namespace
{
const char* kFaceCopyFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uInputScale;
void main() { fragColor = vec4( texture( uInput, vec2( vUV.x, 1.0 - vUV.y ) * uInputScale ).rgb, 1.0 ); } // rows top-first for Vision
)GLSL";
}

// Asynchronous readback of the FX input for the face tracker: draw it downscaled into faceTex, start a
// glReadPixels into a PBO behind a fence, and collect it on a later frame once the fence has signalled.
void Renderer::UpdateFaceReadback( const FrameState& fs )
{
	if( facePending && faceFence )
	{
		GLenum r = glClientWaitSync( faceFence, 0, 0 ); // never wait
		if( r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED )
		{
			glDeleteSync( faceFence );
			faceFence = nullptr;
			glBindBuffer( GL_PIXEL_PACK_BUFFER, facePBO );
			const size_t bytes = size_t( faceW ) * faceH * 4;
			if( void* p = glMapBufferRange( GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT ) )
			{
				facePixels.assign( (const uint8_t*)p, (const uint8_t*)p + bytes );
				glUnmapBuffer( GL_PIXEL_PACK_BUFFER );
				faceReady = true;
			}
			glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
			facePending = false;
		}
	}
	if( !fs.faceWant || facePending || faceReady || !fs.inputTex || fs.inputW <= 0 || fs.inputH <= 0 )
		return;
	if( !progFaceCopy )
	{
		std::string err;
		progFaceCopy = Link( kQuadVS, kFaceCopyFS, err );
		if( !progFaceCopy )
			return;
		glUseProgram( progFaceCopy );
		glUniform1i( U( progFaceCopy, "uInput" ), 0 );
	}
	const int w = 640, h = std::max( 16, int( 640.0f * fs.inputH / fs.inputW ) );
	if( w != faceW || h != faceH || !faceFBO )
	{
		Target( faceTex, faceFBO, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h );
		if( !facePBO )
			glGenBuffers( 1, &facePBO );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, facePBO );
		glBufferData( GL_PIXEL_PACK_BUFFER, size_t( w ) * h * 4, nullptr, GL_STREAM_READ );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
		faceW = w, faceH = h;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, faceFBO );
	glViewport( 0, 0, w, h );
	glDisable( GL_BLEND );
	glDisable( GL_DEPTH_TEST );
	glUseProgram( progFaceCopy );
	glUniform2f( U( progFaceCopy, "uInputScale" ), fs.inputHwW ? float( fs.inputW ) / fs.inputHwW : 1.0f, fs.inputHwH ? float( fs.inputH ) / fs.inputHwH : 1.0f );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, fs.inputTex );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindBuffer( GL_PIXEL_PACK_BUFFER, facePBO );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	faceFence   = glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
	facePending = true;
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

bool Renderer::TakeFaceFrame( std::vector< uint8_t >& px, int& w, int& h )
{
	if( !faceReady )
		return false;
	px.swap( facePixels );
	w = faceW, h = faceH;
	faceReady = false;
	return true;
}
}
