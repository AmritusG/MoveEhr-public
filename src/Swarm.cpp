// W26 Stage 6: the firefly swarm (SwarmShaders.h). Homes = surface points from the dust sampler (off the render thread);
// state ping-pongs through transform feedback; the 32^3 flock grid is splatted each step; the fireflies are drawn into the
// HDR glow layer (Lightning.cpp RenderGlow), added.
#include "Generate.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "SwarmShaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr int kGridAtlas = 192; // GR 32 x GT 6
}

bool Renderer::InitSwarm( std::string& error )
{
	const std::string vs = std::string( "#version 410 core\n" ) + kSwarmGridLib + kSwarmSimVS, fs = std::string( "#version 410 core\n" ) + kSwarmSimFS;
	GLuint v = glCreateShader( GL_VERTEX_SHADER ), f = glCreateShader( GL_FRAGMENT_SHADER );
	const char *pv = vs.c_str(), *pf = fs.c_str();
	glShaderSource( v, 1, &pv, nullptr );
	glShaderSource( f, 1, &pf, nullptr );
	glCompileShader( v );
	glCompileShader( f );
	GLint ok = 0;
	glGetShaderiv( v, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[ 2048 ];
		glGetShaderInfoLog( v, sizeof( log ), nullptr, log );
		error = std::string( "swarm sim compile failed: " ) + log;
		Log( "%s", error.c_str() );
		return false;
	}
	progSwarmSim = glCreateProgram();
	glAttachShader( progSwarmSim, v );
	glAttachShader( progSwarmSim, f );
	const char* varyings[ 2 ] = { "oPos", "oVel" };
	glTransformFeedbackVaryings( progSwarmSim, 2, varyings, GL_INTERLEAVED_ATTRIBS );
	glLinkProgram( progSwarmSim );
	glDeleteShader( v ), glDeleteShader( f );
	glGetProgramiv( progSwarmSim, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		error = "swarm sim link failed";
		Log( "%s", error.c_str() );
		return false;
	}
	progSwarmSplat = Link( std::string( kSwarmGridLib ) + kSwarmSplatVS, kSwarmSplatFS, error );
	progSwarmDraw  = Link( kSwarmDrawVS, kSwarmDrawFS, error );
	if( !progSwarmSplat || !progSwarmDraw )
		return false;
	glUseProgram( progSwarmSim );
	glUniform1i( U( progSwarmSim, "uGrid" ), 0 );
	glUseProgram( progSwarmDraw );
	glUniform1i( U( progSwarmDraw, "uDepth" ), 0 );
	glUseProgram( 0 );
	glGenTextures( 1, &swarmGrid );
	glBindTexture( GL_TEXTURE_2D, swarmGrid );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kGridAtlas, kGridAtlas, 0, GL_RGBA, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glGenFramebuffers( 1, &swarmGridFBO );
	glBindFramebuffer( GL_FRAMEBUFFER, swarmGridFBO );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swarmGrid, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	return true;
}

// model-swap prewarm (WarmWow, inside its 1-pixel scissor): the sim (transform feedback), the splat and the draw, on one firefly
void Renderer::WarmSwarm( const FrameState& fs, int w, int h )
{
	GLuint buf[ 3 ], vao;
	glGenBuffers( 3, buf );
	glGenVertexArrays( 1, &vao );
	const float home[ 5 ] = {}, st[ 6 ] = {};
	glBindBuffer( GL_ARRAY_BUFFER, buf[ 0 ] );
	glBufferData( GL_ARRAY_BUFFER, sizeof( home ), home, GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, buf[ 1 ] );
	glBufferData( GL_ARRAY_BUFFER, sizeof( st ), st, GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, buf[ 2 ] );
	glBufferData( GL_ARRAY_BUFFER, sizeof( st ), nullptr, GL_DYNAMIC_COPY );
	glBindVertexArray( vao );
	glBindBuffer( GL_ARRAY_BUFFER, buf[ 0 ] );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
	glBindBuffer( GL_ARRAY_BUFFER, buf[ 1 ] );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)0 );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
	GLint fbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, swarmGridFBO );
	glViewport( 0, 0, kGridAtlas, kGridAtlas );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	glUseProgram( progSwarmSplat );
	glDrawArrays( GL_POINTS, 0, 1 );
	glDisable( GL_BLEND );
	glUseProgram( progSwarmSim );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, swarmGrid );
	glEnable( GL_RASTERIZER_DISCARD );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, buf[ 2 ] );
	glBeginTransformFeedback( GL_POINTS );
	glDrawArrays( GL_POINTS, 0, 1 );
	glEndTransformFeedback();
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	glDisable( GL_RASTERIZER_DISCARD );
	glBindFramebuffer( GL_FRAMEBUFFER, (GLuint)fbo );
	glViewport( 0, 0, w, h );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glEnable( GL_BLEND );
	glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
	glUseProgram( progSwarmDraw );
	glDrawArrays( GL_POINTS, 0, 1 );
	glDisable( GL_BLEND );
	glDisable( GL_PROGRAM_POINT_SIZE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindVertexArray( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glDeleteVertexArrays( 1, &vao );
	glDeleteBuffers( 3, buf );
	(void)fs;
}

void Renderer::ReleaseSwarm()
{
	for( GLuint* p : { &progSwarmSim, &progSwarmSplat, &progSwarmDraw } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	SwarmSlot& s = swarmSlot;
	if( s.home )
	{
		glDeleteBuffers( 1, &s.home );
		glDeleteBuffers( 2, s.state );
		glDeleteVertexArrays( 2, s.simVAO );
		glDeleteVertexArrays( 2, s.drawVAO );
	}
	s = SwarmSlot();
	if( swarmGrid )
		glDeleteTextures( 1, &swarmGrid ), swarmGrid = 0;
	if( swarmGridFBO )
		glDeleteFramebuffers( 1, &swarmGridFBO ), swarmGridFBO = 0;
}

void Renderer::UpdateSwarm( const FrameState& fs, GpuModel* model )
{
	const SwarmState& w = fs.swarm;
	SwarmSlot& s        = swarmSlot;
	if( w.count <= 0 || !model || !model->geometry )
	{
		s.live = false;
		return;
	}
	const uint64_t key = model->serial * 1048576 + (uint64_t)w.count;
	if( key != s.key && ( !s.job || s.job->key != key ) )
	{
		if( s.job )
			s.job->cancel.store( true );
		auto job = std::make_shared< DustJob >();
		job->key = key;
		auto geo = model->geometry;
		const int n = w.count;
		s.job    = job;
		std::thread( [ job, geo, n ]() {
			SampleSurface( *geo, n, *job );
			job->done.store( true );
		} ).detach();
	}
	if( s.job && s.job->done.load() )
	{
		auto job = s.job;
		s.job.reset();
		if( job->points.empty() || job->cancel.load() )
			return;
		const int n = (int)( job->points.size() / 5 );
		if( !s.home )
		{
			glGenBuffers( 1, &s.home );
			glGenBuffers( 2, s.state );
			glGenVertexArrays( 2, s.simVAO );
			glGenVertexArrays( 2, s.drawVAO );
		}
		glBindBuffer( GL_ARRAY_BUFFER, s.home );
		glBufferData( GL_ARRAY_BUFFER, job->points.size() * sizeof( float ), job->points.data(), GL_STATIC_DRAW );
		for( int i = 0; i < 2; ++i )
		{
			glBindBuffer( GL_ARRAY_BUFFER, s.state[ i ] );
			glBufferData( GL_ARRAY_BUFFER, size_t( n ) * 6 * sizeof( float ), nullptr, GL_DYNAMIC_COPY );
		}
		for( int i = 0; i < 2; ++i )
			for( GLuint vao : { s.simVAO[ i ], s.drawVAO[ i ] } )
			{
				glBindVertexArray( vao );
				glBindBuffer( GL_ARRAY_BUFFER, s.home );
				glEnableVertexAttribArray( 0 );
				glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)0 );
				glEnableVertexAttribArray( 1 );
				glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
				glBindBuffer( GL_ARRAY_BUFFER, s.state[ i ] );
				glEnableVertexAttribArray( 2 );
				glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)0 );
				glEnableVertexAttribArray( 3 );
				glVertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
			}
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		s.count = n, s.key = job->key, s.live = false;
		Log( "SWARM %d fireflies for %s (homes in %.0f ms, off the render thread)", n, LogName( model->path ).c_str(), job->ms );
	}
	if( s.key != key || s.count == 0 )
		return;
	// the flock grid: a cube of 4 mask units round the mask's centre, world-aligned
	const float half = 2.0f * w.unit, cell = 2.0f * half / 32.0f;
	float gmin[ 3 ];
	for( int k = 0; k < 3; ++k )
		gmin[ k ] = fs.post.maskCenter[ k ] - half;
	if( s.live )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, swarmGridFBO );
		glViewport( 0, 0, kGridAtlas, kGridAtlas );
		glClearColor( 0, 0, 0, 0 );
		glClear( GL_COLOR_BUFFER_BIT );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		glUseProgram( progSwarmSplat );
		glUniform3fv( U( progSwarmSplat, "uGridMin" ), 1, gmin );
		glUniform1f( U( progSwarmSplat, "uGridCell" ), cell );
		glBindVertexArray( s.drawVAO[ s.idx ] );
		glDrawArrays( GL_POINTS, 0, s.count );
		glDisable( GL_BLEND );
	}
	// the sim draws with the rasterizer off, but still against a complete framebuffer (the host's 0 may not be one)
	glBindFramebuffer( GL_FRAMEBUFFER, swarmGridFBO );
	glUseProgram( progSwarmSim );
	glUniform3fv( U( progSwarmSim, "uGridMin" ), 1, gmin );
	glUniform1f( U( progSwarmSim, "uGridCell" ), cell );
	glUniformMatrix4fv( U( progSwarmSim, "uModel" ), 1, GL_FALSE, fs.model );
	glUniform3fv( U( progSwarmSim, "uFitCenter" ), 1, model->fitCenter );
	glUniform3fv( U( progSwarmSim, "uCenter" ), 1, fs.post.maskCenter );
	glUniform1f( U( progSwarmSim, "uUnit" ), w.unit );
	glUniform1f( U( progSwarmSim, "uDt" ), std::min( w.dt, 1.0f / 30.0f ) );
	glUniform1f( U( progSwarmSim, "uAttract" ), w.attract );
	glUniform1f( U( progSwarmSim, "uFlock" ), w.flock );
	glUniform1f( U( progSwarmSim, "uWander" ), w.wander );
	glUniform1f( U( progSwarmSim, "uOffset" ), w.offset * w.unit );
	glUniform1f( U( progSwarmSim, "uKick" ), w.kick );
	glUniform1f( U( progSwarmSim, "uDrift" ), w.drift );
	glUniform1i( U( progSwarmSim, "uReset" ), s.live && !w.reset ? 0 : 1 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, swarmGrid );
	glEnable( GL_RASTERIZER_DISCARD );
	glBindVertexArray( s.simVAO[ s.idx ] );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, s.state[ 1 - s.idx ] );
	glBeginTransformFeedback( GL_POINTS );
	glDrawArrays( GL_POINTS, 0, s.count );
	glEndTransformFeedback();
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	glDisable( GL_RASTERIZER_DISCARD );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	s.idx  = 1 - s.idx;
	s.live = true;
	if( getenv( "MOVEEHR_SWARM_DEBUG" ) )
	{
		std::vector< float > st( size_t( s.count ) * 6 );
		glBindBuffer( GL_ARRAY_BUFFER, s.state[ s.idx ] );
		glGetBufferSubData( GL_ARRAY_BUFFER, 0, st.size() * sizeof( float ), st.data() );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		int nan = 0;
		float lo = 1e30f, hi = -1e30f, vmax = 0;
		for( int i = 0; i < s.count; ++i )
		{
			const float* q = &st[ size_t( i ) * 6 ];
			if( !( q[ 0 ] == q[ 0 ] ) || !( q[ 3 ] == q[ 3 ] ) )
			{
				++nan;
				continue;
			}
			lo = std::min( lo, q[ 1 ] ), hi = std::max( hi, q[ 1 ] ), vmax = std::max( vmax, std::sqrt( q[ 3 ] * q[ 3 ] + q[ 4 ] * q[ 4 ] + q[ 5 ] * q[ 5 ] ) );
		}
		Log( "SWARM debug: %d NaN, y %.3f .. %.3f, max speed %.3f, kick %.2f attract %.2f, reset %d live %d idx %d gl error 0x%x", nan, lo, hi, vmax, w.kick, w.attract, w.reset ? 1 : 0, s.live ? 1 : 0, s.idx, glGetError() );
	}
}

void Renderer::DrawSwarm( const FrameState& fs, int w, int h )
{
	const SwarmSlot& s = swarmSlot;
	if( fs.swarm.count <= 0 || !s.live || s.count == 0 )
		return;
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	glEnable( GL_PROGRAM_POINT_SIZE );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
	glBindVertexArray( s.drawVAO[ s.idx ] );
	const GLuint p = progSwarmDraw;
	glUseProgram( p );
	glUniformMatrix4fv( U( p, "uViewProj" ), 1, GL_FALSE, VP.m );
	glUniform1f( U( p, "uSize" ), fs.swarm.size * float( h ) / 1080.0f );
	glUniform1f( U( p, "uTime" ), fs.swarm.time );
	glUniform1f( U( p, "uTwinkle" ), fs.swarm.twinkle );
	glUniform3fv( U( p, "uColor" ), 1, fs.swarm.color );
	glUniform2f( U( p, "uScreen" ), float( w ), float( h ) );
	glDrawArrays( GL_POINTS, 0, s.count );
	glBindVertexArray( 0 );
	glDisable( GL_PROGRAM_POINT_SIZE );
}
}
