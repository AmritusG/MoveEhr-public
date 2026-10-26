// W26 Stage 4: the Liquid transition (A -> B as one blended SDF surface, LiquidShaders.h). Both models' SDFs are built off the
// render thread by SdfBuilder (one job per geometry, shared with the sim) and uploaded here; until both are on
// the GPU the transition dissolves like Dissolve.
#include "LiquidShaders.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
float ColLen( const float* m ) { return std::sqrt( m[ 0 ] * m[ 0 ] + m[ 1 ] * m[ 1 ] + m[ 2 ] * m[ 2 ] ); }
// separable Gaussian on the distance channel (5 taps, sigma ~1.2 voxels): rounds the voxel stairs of the surface
void SmoothSdf( SdfField& f )
{
	const int N = kSdfRes;
	std::vector< float > a( size_t( N ) * N * N ), b( a.size() );
	for( size_t i = 0; i < a.size(); ++i )
		a[ i ] = f.rg[ i * 2 ];
	const float w[ 5 ] = { 0.0855f, 0.2442f, 0.3406f, 0.2442f, 0.0855f };
	auto idx = []( int x, int y, int z ) { return ( size_t( z ) * N + y ) * N + x; };
	for( int axis = 0; axis < 3; ++axis )
	{
		for( int z = 0; z < N; ++z )
			for( int y = 0; y < N; ++y )
				for( int x = 0; x < N; ++x )
				{
					float s = 0;
					for( int k = -2; k <= 2; ++k )
					{
						int q[ 3 ] = { x, y, z };
						q[ axis ]  = std::min( N - 1, std::max( 0, q[ axis ] + k ) );
						s += w[ k + 2 ] * a[ idx( q[ 0 ], q[ 1 ], q[ 2 ] ) ];
					}
					b[ idx( x, y, z ) ] = s;
				}
		a.swap( b );
	}
	for( size_t i = 0; i < a.size(); ++i )
		f.rg[ i * 2 ] = a[ i ];
}
}

bool Renderer::InitLiquid( std::string& error )
{
	progLiquid = Link( kQuadVS, std::string( kToneLib ) + kLiquidFS, error );
	if( !progLiquid )
		return false;
	glUseProgram( progLiquid );
	glUniform1i( U( progLiquid, "uSdfA" ), 1 );
	glUniform1i( U( progLiquid, "uSdfB" ), 2 );
	glUniform1i( U( progLiquid, "uPaintA" ), 3 );
	glUniform1i( U( progLiquid, "uPaintB" ), 4 );
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleaseLiquid()
{
	if( progLiquid )
		glDeleteProgram( progLiquid ), progLiquid = 0;
	for( LiquidSdf& l : liquidSdf )
	{
		if( l.tex )
			glDeleteTextures( 1, &l.tex );
		l = LiquidSdf();
	}
	for( GLuint& t : paintTex )
		if( t )
			glDeleteTextures( 1, &t ), t = 0;
	if( paintFBO )
		glDeleteFramebuffers( 1, &paintFBO ), paintFBO = 0;
	if( paintDepth )
		glDeleteRenderbuffers( 1, &paintDepth ), paintDepth = 0;
	paintW = paintH = 0;
}

bool Renderer::UpdateLiquidSdf( GpuModel* a, GpuModel* b )
{
	const bool okA = UpdateLiquidSlot( sideSlot[ 0 ], a );
	const bool okB = UpdateLiquidSlot( sideSlot[ 1 ], b );
	return okA && okB;
}

// LoadEhr W1: one liquid-ready SDF per model slot (built once, ahead of use)
bool Renderer::UpdateLiquidSlot( int slot, GpuModel* m )
{
	LiquidSdf& l = liquidSdf[ slot ];
	if( !m || !m->geometry )
		return false;
	if( l.geo != m->geometry.get() )
	{
		if( l.tex )
			glDeleteTextures( 1, &l.tex ), l.tex = 0;
		l.geo = m->geometry.get();
		l.job = RequestSdf( m->geometry );
		l.smooth.reset();
	}
	// the voxel SDF is stair-stepped: a smoothed copy (Gaussian, sigma ~1.2 voxels) is built off the render thread
	if( !l.tex && l.job && l.job->done.load() && !l.job->field.rg.empty() && !l.smooth )
	{
		auto sj   = std::make_shared< LiquidSmooth >();
		sj->field = l.job->field;
		l.smooth  = sj;
		std::thread( [ sj ]() {
			FillSdf( sj->field, 10 );
			SmoothSdf( sj->field );
			sj->done.store( true );
		} ).detach();
	}
	if( !l.tex && l.smooth && l.smooth->done.load() )
	{
		const SdfField& f = l.smooth->field;
		glGenTextures( 1, &l.tex );
		glBindTexture( GL_TEXTURE_3D, l.tex );
		glTexImage3D( GL_TEXTURE_3D, 0, GL_RG16F, kSdfRes, kSdfRes, kSdfRes, 0, GL_RG, GL_FLOAT, f.rg.data() );
		glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		for( GLenum w : { GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R } )
			glTexParameteri( GL_TEXTURE_3D, w, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_3D, 0 );
		std::memcpy( l.min, f.boxMin, sizeof( l.min ) );
		l.size = f.boxSize;
		l.job.reset();
		l.smooth.reset();
		Log( "LIQUID SDF %c ready (%d^3)", "ABCD"[ slot ], kSdfRes );
	}
	return l.tex != 0;
}

// Paint look: both meshes, whole and shaded, from the camera at half resolution (premultiplied, as the main draw writes them)
void Renderer::RenderLiquidPaint( const FrameState& fsA, const FrameState& fsB, GpuModel* a, GpuModel* b, int w, int h )
{
	const int pw = std::max( 1, w / 2 ), ph = std::max( 1, h / 2 );
	if( pw != paintW || ph != paintH || !paintFBO )
	{
		for( GLuint& t : paintTex )
		{
			if( !t )
				glGenTextures( 1, &t );
			glBindTexture( GL_TEXTURE_2D, t );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
		if( !paintDepth )
			glGenRenderbuffers( 1, &paintDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, paintDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, pw, ph );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		if( !paintFBO )
			glGenFramebuffers( 1, &paintFBO );
		paintW = pw, paintH = ph;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, paintFBO );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, paintDepth );
	glViewport( 0, 0, pw, ph );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDisable( GL_BLEND );
	for( int i = 0; i < 2; ++i )
	{
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, paintTex[ i ], 0 );
		glClearColor( 0, 0, 0, 0 );
		glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		FrameState f = i == 0 ? fsA : fsB;
		f.dissolve   = 0.0f, f.dissolveInvert = false, f.fogDensity = 0.0f;
		GpuModel* g  = i == 0 ? a : b;
		EnsureVAO( g );
		const bool soup = f.wireMix > 0.0f || f.explode > 0.0f || f.shape.on;
		DrawModel( soup ? progSoup : progMain, f, g, false, soup, 0 );
	}
	glDisable( GL_DEPTH_TEST );
}

void Renderer::RenderLiquid( const FrameState& fs, float t, int w, int h )
{
	const LiquidState& lq = fs.liquid;
	const LiquidSdf& LA = liquidSdf[ sideSlot[ 0 ] ];
	const LiquidSdf& LB = liquidSdf[ sideSlot[ 1 ] ];
	Mat4 A, B, V, P;
	std::memcpy( A.m, fs.model, sizeof( A.m ) );
	std::memcpy( B.m, fs.modelB, sizeof( B.m ) );
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V, inv = VP.Inverse(), invA = A.Inverse(), invB = B.Inverse();
	const float sA = ColLen( A.m ), sB = ColLen( B.m );
	// the bounds round both SDF boxes
	float ca[ 3 ], cb[ 3 ];
	const float ha[ 3 ] = { LA.min[ 0 ] + 0.5f * LA.size, LA.min[ 1 ] + 0.5f * LA.size, LA.min[ 2 ] + 0.5f * LA.size };
	const float hb[ 3 ] = { LB.min[ 0 ] + 0.5f * LB.size, LB.min[ 1 ] + 0.5f * LB.size, LB.min[ 2 ] + 0.5f * LB.size };
	A.Point( ha, ca );
	B.Point( hb, cb );
	const float rA = 0.87f * LA.size * sA, rB = 0.87f * LB.size * sB;
	float center[ 3 ], dc = 0;
	for( int k = 0; k < 3; ++k )
		center[ k ] = 0.5f * ( ca[ k ] + cb[ k ] ), dc += ( ca[ k ] - cb[ k ] ) * ( ca[ k ] - cb[ k ] );
	const float radius = 0.5f * std::sqrt( dc ) + std::max( rA, rB ) * 1.1f;
	const float mask   = 0.5f * std::max( rA, rB ) / 0.87f / 1.2f; // ~ the mask's radius (the SDF box is the bbox x 1.2)
	// the morph runs over the middle of the slider; the liquid starts and ends shrunk inside the meshes
	auto ss = []( float a, float b, float x ) {
		const float u = std::min( 1.0f, std::max( 0.0f, ( x - a ) / ( b - a ) ) );
		return u * u * ( 3.0f - 2.0f * u );
	};
	const float e = ss( 0.15f, 0.85f, t ), on = ss( 0.0f, 0.15f, t ) * ( 1.0f - ss( 0.85f, 1.0f, t ) );
	glUseProgram( progLiquid );
	glUniformMatrix4fv( U( progLiquid, "uInvA" ), 1, GL_FALSE, invA.m );
	glUniformMatrix4fv( U( progLiquid, "uInvB" ), 1, GL_FALSE, invB.m );
	glUniform3fv( U( progLiquid, "uMinA" ), 1, LA.min );
	glUniform3fv( U( progLiquid, "uMinB" ), 1, LB.min );
	glUniform1f( U( progLiquid, "uSizeA" ), LA.size );
	glUniform1f( U( progLiquid, "uSizeB" ), LB.size );
	glUniform1f( U( progLiquid, "uScaleA" ), sA );
	glUniform1f( U( progLiquid, "uScaleB" ), sB );
	glUniformMatrix4fv( U( progLiquid, "uInvViewProj" ), 1, GL_FALSE, inv.m );
	glUniformMatrix4fv( U( progLiquid, "uViewProj" ), 1, GL_FALSE, VP.m );
	glUniform3fv( U( progLiquid, "uCamPos" ), 1, fs.camPos );
	glUniform3fv( U( progLiquid, "uCenter" ), 1, center );
	glUniform1f( U( progLiquid, "uRadius" ), radius );
	glUniform1f( U( progLiquid, "uE" ), e );
	glUniform1f( U( progLiquid, "uGoo" ), lq.goo );
	glUniform1f( U( progLiquid, "uGrow" ), 0.3f * mask );
	glUniform1f( U( progLiquid, "uShrink" ), ( 1.0f - on ) * 0.08f * mask );
	glUniform1f( U( progLiquid, "uBulge" ), lq.goo * 0.06f * mask * 4.0f * e * ( 1.0f - e ) );
	glUniform1f( U( progLiquid, "uWobble" ), lq.wobble * on );
	glUniform1f( U( progLiquid, "uPhase" ), lq.phase );
	glUniform1f( U( progLiquid, "uUnit" ), mask );
	glUniform1i( U( progLiquid, "uLook" ), lq.look );
	glUniform3fv( U( progLiquid, "uTint" ), 1, lq.tint );
	glUniform1f( U( progLiquid, "uTintMix" ), lq.tintMix );
	glUniform3fv( U( progLiquid, "uKeyDir" ), 1, fs.keyDir );
	glUniform3fv( U( progLiquid, "uKeyColor" ), 1, fs.keyColor );
	glUniform1f( U( progLiquid, "uEnvIntensity" ), fs.envSource == 2 ? 0.0f : fs.envIntensity );
	glUniform1f( U( progLiquid, "uEnvRotate" ), fs.envRotate );
	glUniform2f( U( progLiquid, "uTexel" ), 1.0f / float( std::max( paintW, 1 ) ), 1.0f / float( std::max( paintH, 1 ) ) );
	glUniform1f( U( progLiquid, "uExposure" ), fs.exposure );
	glUniform1i( U( progLiquid, "uTonemap" ), fs.tonemap );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_3D, LA.tex );
	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_3D, LB.tex );
	glActiveTexture( GL_TEXTURE3 );
	glBindTexture( GL_TEXTURE_2D, lq.look == 1 && paintTex[ 0 ] ? paintTex[ 0 ] : whiteTex );
	glActiveTexture( GL_TEXTURE4 );
	glBindTexture( GL_TEXTURE_2D, lq.look == 1 && paintTex[ 1 ] ? paintTex[ 1 ] : whiteTex );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glBindVertexArray( 0 );
	for( int u : { 4, 3 } )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	for( int u : { 2, 1 } )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_3D, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
	(void)w, (void)h;
}
}
