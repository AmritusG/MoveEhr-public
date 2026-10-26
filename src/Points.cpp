// W6 Stage 1: Points Only / Points + Wire (Wire Mode 4 / 5). A point is a spot on a triangle: (global triangle in GPU index order,
// barycentric b1, b2) in an RGBA32F texture buffer on unit 26. The POINTS twins of the main-pass soup programs (Renderer::Init links
// them like the WIRE twins) run the whole triangle-soup stage at that spot, so a point follows every deform, the letters, the shard /
// piece flight, the clones and the morph pair exactly as its triangle does, and draw it as a round, anti-aliased sprite of constant
// size on screen; the lines' look (Wire Color, Glow, Bloom, Fade, Back) is the points' look.
//   Point Source: Vertices = one point per vertex (welded by position: loaders split a vertex by normal / uv), Feature Vertices = the
//   vertices on the W5 feature edges, Surface 25k / 100k / 400k = points spread evenly over the area (sparse models still read).
// Built off the render thread per model + source, cached (8 entries); until a set lands no point draws (never a wait).
#include "Generate.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <unordered_set>

namespace mehr
{
void BuildEdgeFlags( const CpuGeometry& g, std::vector< uint8_t >& out ); // Wire.cpp
std::vector< uint32_t > WeldByPosition( const CpuGeometry& g );           // Wire.cpp

namespace
{
const char* kSourceName[ 5 ] = { "Vertices", "Feature Vertices", "Surface 25k", "Surface 100k", "Surface 400k" };

}
// one point set: per GPU part a run of refs (the parts' order), each 4 floats: triangle, b1, b2, 0 (W9: the line looks use it too)
void BuildPointRefs( const CpuGeometry& g, int source, const std::vector< PointSet::Part >& parts, PointSet::Job& job )
{
	const auto t0   = std::chrono::steady_clock::now();
	const size_t nw = g.idx.size() / 3;
	auto corner     = [ & ]( size_t t, int c ) { return t < nw ? g.idx[ t * 3 + c ] : g.fracIdx[ ( t - nw ) * 3 + c ]; };
	auto region     = [ & ]( size_t t ) -> uint64_t { return t < nw ? 0 : 1 + ( t - nw < g.fracPiece.size() ? g.fracPiece[ t - nw ] : 0 ); };
	job.ranges.assign( parts.size(), { 0u, 0u } );
	std::vector< float >& r = job.refs;
	auto add = [ & ]( size_t t, float b1, float b2 ) {
		r.push_back( float( t ) ), r.push_back( b1 ), r.push_back( b2 ), r.push_back( 0.0f );
	};
	if( source <= 1 )
	{
		// Vertices / Feature Vertices: one point per welded vertex and region (the whole mesh, each piece of the fractured copy),
		// riding the first triangle that has it
		const std::vector< uint32_t > weld = WeldByPosition( g );
		std::vector< uint8_t > flags;
		if( source == 1 )
			BuildEdgeFlags( g, flags );
		std::unordered_set< uint64_t > seen;
		const float bc[ 3 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
		for( size_t p = 0; p < parts.size(); ++p )
		{
			job.ranges[ p ][ 0 ] = uint32_t( r.size() / 4 );
			const size_t t0p = parts[ p ].first / 3, t1p = t0p + parts[ p ].count / 3;
			for( size_t t = t0p; t < t1p; ++t )
			{
				uint8_t want = 7; // corners to take: Feature = the two ends of every flagged edge (bit k = the edge opposite corner k)
				if( source == 1 )
				{
					const uint8_t f = t < flags.size() ? flags[ t ] : 0;
					want = uint8_t( ( ( f & 1 ) ? 6 : 0 ) | ( ( f & 2 ) ? 5 : 0 ) | ( ( f & 4 ) ? 3 : 0 ) );
				}
				for( int c = 0; c < 3; ++c )
				{
					if( !( want & ( 1 << c ) ) )
						continue;
					const uint64_t key = ( uint64_t( weld[ corner( t, c ) ] ) << 16 ) | ( region( t ) & 0xFFFF );
					if( seen.insert( key ).second )
						add( t, bc[ c ][ 0 ], bc[ c ][ 1 ] );
				}
			}
			job.ranges[ p ][ 1 ] = uint32_t( r.size() / 4 ) - job.ranges[ p ][ 0 ];
		}
	}
	else
	{
		// Surface: N points over the whole mesh's parts by area, and N over the fractured copy's (it stands in for the whole mesh
		// while pieces fly); uniform within each triangle
		const int N = source == 2 ? 25000 : source == 3 ? 100000 : 400000;
		std::vector< double > area( parts.size(), 0.0 );
		std::vector< std::vector< double > > cum( parts.size() );
		double total[ 2 ] = { 0, 0 };
		for( size_t p = 0; p < parts.size(); ++p )
		{
			const size_t t0p = parts[ p ].first / 3, t1p = t0p + parts[ p ].count / 3;
			cum[ p ].reserve( t1p - t0p );
			double acc = 0;
			for( size_t t = t0p; t < t1p; ++t )
			{
				const float* a = &g.pos[ corner( t, 0 ) * 3 ];
				const float* b = &g.pos[ corner( t, 1 ) * 3 ];
				const float* c = &g.pos[ corner( t, 2 ) * 3 ];
				const double u[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, w[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] };
				const double x = u[ 1 ] * w[ 2 ] - u[ 2 ] * w[ 1 ], y = u[ 2 ] * w[ 0 ] - u[ 0 ] * w[ 2 ], z = u[ 0 ] * w[ 1 ] - u[ 1 ] * w[ 0 ];
				cum[ p ].push_back( acc += std::sqrt( x * x + y * y + z * z ) );
			}
			area[ p ] = acc;
			total[ parts[ p ].fractured ? 1 : 0 ] += acc;
		}
		std::mt19937 rng( 24681357u + unsigned( N ) );
		std::uniform_real_distribution< double > U01( 0.0, 1.0 );
		for( size_t p = 0; p < parts.size() && !job.cancel.load(); ++p )
		{
			job.ranges[ p ][ 0 ] = uint32_t( r.size() / 4 );
			const double tot = total[ parts[ p ].fractured ? 1 : 0 ];
			const int n      = tot > 0 && area[ p ] > 0 ? int( std::lround( N * area[ p ] / tot ) ) : 0;
			const size_t t0p = parts[ p ].first / 3;
			for( int i = 0; i < n; ++i )
			{
				const size_t k = std::min( size_t( std::lower_bound( cum[ p ].begin(), cum[ p ].end(), U01( rng ) * area[ p ] ) - cum[ p ].begin() ), cum[ p ].size() - 1 );
				const double s = std::sqrt( U01( rng ) ), v = U01( rng );
				add( t0p + k, float( s * ( 1 - v ) ), float( s * v ) );
			}
			job.ranges[ p ][ 1 ] = uint32_t( r.size() / 4 ) - job.ranges[ p ][ 0 ];
		}
	}
	job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}

bool Renderer::InitPoints( std::string& )
{
	const float one[ 4 ] = { 0, 0, 0, 0 }; // the prewarm's point: triangle 0, corner 0
	glGenBuffers( 1, &pointDummyBuf );
	glBindBuffer( GL_TEXTURE_BUFFER, pointDummyBuf );
	glBufferData( GL_TEXTURE_BUFFER, sizeof( one ), one, GL_STATIC_DRAW );
	glGenTextures( 1, &pointDummy );
	glBindTexture( GL_TEXTURE_BUFFER, pointDummy );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, pointDummyBuf );
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	return true;
}

void Renderer::ReleasePoints()
{
	for( PointSet& s : pointSets )
	{
		if( s.tex )
			glDeleteTextures( 1, &s.tex );
		if( s.buf )
			glDeleteBuffers( 1, &s.buf );
		if( s.job )
			s.job->cancel.store( true );
		s = PointSet();
	}
	if( pointDummy )
		glDeleteTextures( 1, &pointDummy ), pointDummy = 0;
	for( GLuint* t : { &glyphTex, &glyphDummy, &trailDummy } )
		if( *t )
			glDeleteTextures( 1, t ), *t = 0;
	if( trailDummyBuf )
		glDeleteBuffers( 1, &trailDummyBuf ), trailDummyBuf = 0;
	glyphKeyNow = 0;
	for( const auto& t : ptfxTwins )
		if( t.second != t.first )
			glDeleteProgram( t.second );
	ptfxTwins.clear();
	ReleasePtShade();
	if( pointDummyBuf )
		glDeleteBuffers( 1, &pointDummyBuf ), pointDummyBuf = 0;
}

// MoveEhr W7 Point Shading: the shade buffer (RGBA32F, kPtShadeW wide; per point its shaded colour + keep) holds n points, its rows grown
// to need (in steps of 64; never past kPtShadeMaxH: false = that draw stays Full, logged once); + a 1-texel dummy for pass 1's unit
bool Renderer::PtShadeFit( GLsizei n )
{
	const int rows = ( (int)n + kPtShadeW - 1 ) / kPtShadeW;
	if( ptShadeH < 0 ) // the buffer could not be made: Full for good
		return false;
	if( rows > kPtShadeMaxH )
	{
		if( !ptShadeFull )
			Log( "POINTS shade buffer: %d points x instances past its %d; Full shading for that draw", (int)n, kPtShadeW * kPtShadeMaxH );
		ptShadeFull = true;
		return false;
	}
	if( !ptShadeDummy )
	{
		const float z[ 4 ] = { 0, 0, 0, 0 };
		glGenTextures( 1, &ptShadeDummy );
		glBindTexture( GL_TEXTURE_2D, ptShadeDummy );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, z );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	if( rows > ptShadeH )
	{
		const int h = std::min( kPtShadeMaxH, ( rows + 63 ) / 64 * 64 );
		if( !ptShadeTex )
			glGenTextures( 1, &ptShadeTex );
		glBindTexture( GL_TEXTURE_2D, ptShadeTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kPtShadeW, h, 0, GL_RGBA, GL_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		GLint was = 0;
		glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &was );
		if( !ptShadeFBO )
			glGenFramebuffers( 1, &ptShadeFBO );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, ptShadeFBO );
		glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ptShadeTex, 0 );
		const GLenum st = glCheckFramebufferStatus( GL_DRAW_FRAMEBUFFER );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, (GLuint)was );
		if( st != GL_FRAMEBUFFER_COMPLETE )
		{
			Log( "POINTS shade buffer %d x %d incomplete (0x%x): Full shading", kPtShadeW, h, st );
			ptShadeH = -1;
			return false;
		}
		ptShadeH = h;
		Log( "POINTS shade buffer %d x %d (%.0f MB)", kPtShadeW, h, kPtShadeW * double( h ) * 16.0 / 1048576.0 );
	}
	return true;
}

void Renderer::ReleasePtShade()
{
	for( const auto& t : shadeTwins )
		if( t.second != t.first )
			glDeleteProgram( t.second );
	shadeTwins.clear();
	for( GLuint* t : { &ptShadeTex, &ptShadeDummy } )
		if( *t )
			glDeleteTextures( 1, t ), *t = 0;
	if( ptShadeFBO )
		glDeleteFramebuffers( 1, &ptShadeFBO ), ptShadeFBO = 0;
	ptShadeH = 0;
}

// MoveEhr W4: the point looks' uniforms (the PTFX twins; the program is bound). Units 42 (Trail Off's earlier pose) and 63 (the glyph
// atlas) always hold a texture of their type (Apple GL refuses a draw otherwise)
void Renderer::SetPointLookUniforms( GLuint p, const FrameState& fs, GpuModel* model, bool posed )
{
	const FrameState::WireState& w = fs.wire;
	auto U = [ & ]( const char* n ) { return glGetUniformLocation( p, n ); };
	glUniform1i( U( "uPtShape" ), w.shape );
	glUniform1i( U( "uPtMotion" ), w.motion );
	glUniform1f( U( "uPtStr" ), w.str );
	glUniform1f( U( "uPtDis" ), w.dis );
	glUniform1f( U( "uPtPhase" ), w.phase );
	glUniform1f( U( "uPtBurst" ), w.burst );
	glUniform1f( U( "uPtSizeVar" ), w.sizeVar );
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	const float* c = model->fitCenter;
	const float cw[ 3 ] = { M.m[ 0 ] * c[ 0 ] + M.m[ 4 ] * c[ 1 ] + M.m[ 8 ] * c[ 2 ] + M.m[ 12 ], M.m[ 1 ] * c[ 0 ] + M.m[ 5 ] * c[ 1 ] + M.m[ 9 ] * c[ 2 ] + M.m[ 13 ],
		                    M.m[ 2 ] * c[ 0 ] + M.m[ 6 ] * c[ 1 ] + M.m[ 10 ] * c[ 2 ] + M.m[ 14 ] };
	glUniform3fv( U( "uPtCentre" ), 1, cw );
	glUniform1f( U( "uPtRadius" ), std::max( fs.modelScale, 1e-6f ) );
	GLint vp[ 4 ];
	glGetIntegerv( GL_VIEWPORT, vp );
	glUniform2f( U( "uPtViewport" ), float( std::max( 1, vp[ 2 ] ) ), float( std::max( 1, vp[ 3 ] ) ) );
	const float* V = fs.view; // rows of the view's rotation = the camera's right / up in world
	glUniform3f( U( "uPtRight" ), V[ 0 ], V[ 4 ], V[ 8 ] );
	glUniform3f( U( "uPtUp" ), V[ 1 ], V[ 5 ], V[ 9 ] );
	if( !glyphDummy )
	{
		const uint8_t z = 0;
		glGenTextures( 1, &glyphDummy );
		glBindTexture( GL_TEXTURE_2D, glyphDummy );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &z );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		const uint32_t zz[ 8 ] = {};
		glGenBuffers( 1, &trailDummyBuf );
		glBindBuffer( GL_TEXTURE_BUFFER, trailDummyBuf );
		glBufferData( GL_TEXTURE_BUFFER, sizeof( zz ), zz, GL_STATIC_DRAW );
		glGenTextures( 1, &trailDummy );
		glBindTexture( GL_TEXTURE_BUFFER, trailDummy );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R32UI, trailDummyBuf );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	}
	if( w.glyphs && w.glyphKey != glyphKeyNow && w.glyphs->size() == 512 * 512 )
	{
		if( !glyphTex )
			glGenTextures( 1, &glyphTex );
		glBindTexture( GL_TEXTURE_2D, glyphTex );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, 512, 512, 0, GL_RED, GL_UNSIGNED_BYTE, w.glyphs->data() );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glyphKeyNow = w.glyphKey;
	}
	const bool glyph = w.shape == 15 && glyphTex && w.glyphN > 0;
	glUniform1i( U( "uPtGlyphN" ), glyph ? w.glyphN : 1 );
	glActiveTexture( GL_TEXTURE0 + 63 );
	glBindTexture( GL_TEXTURE_2D, glyph ? glyphTex : glyphDummy );
	glBindSampler( 63, 0 );
	const int slot          = sideSlot[ activeSide ];
	const SlotPose* sp      = posed && slot >= 0 && slot < kSlots ? &pose[ slot ] : nullptr;
	const bool trail        = w.motion == 11 && sp && sp->trailWant && sp->trailPosed && sp->trailTbo;
	glUniform1i( U( "uPtTrailOn" ), trail ? 1 : 0 );
	glActiveTexture( GL_TEXTURE0 + 42 );
	glBindTexture( GL_TEXTURE_BUFFER, trail ? sp->trailTbo : trailDummy );
	glActiveTexture( GL_TEXTURE0 );
}

GLuint Renderer::PointTwin( GLuint p ) const
{
	for( const auto& t : pointTwins )
		if( t.first == p )
			return t.second;
	return p;
}

// the model's point set for this source: starts the build if there is none, uploads it once it lands; nullptr until then
const PointSet* Renderer::PointsFor( GpuModel* model, int source )
{
	if( !model || !model->geometry )
		return nullptr;
	source = std::clamp( source, 0, 4 );
	PointSet* s = nullptr;
	for( PointSet& x : pointSets )
		if( x.serial == model->serial && x.source == source )
			s = &x;
	if( !s )
	{
		// a free entry, else the one started longest ago
		s = &pointSets[ pointNext++ % kPointSets ];
		if( s->tex )
			glDeleteTextures( 1, &s->tex );
		if( s->buf )
			glDeleteBuffers( 1, &s->buf );
		if( s->job )
			s->job->cancel.store( true );
		*s        = PointSet();
		s->serial = model->serial;
		s->source = source;
		std::vector< PointSet::Part > parts;
		for( const GpuPart& gp : model->parts )
			parts.push_back( { gp.firstIndex, gp.indexCount, gp.fractured } );
		auto job  = std::make_shared< PointSet::Job >();
		s->job    = job;
		auto geo  = model->geometry;
		auto name = LogName( model->path );
		std::thread( [ job, geo, source, parts, name ]() {
			BuildPointRefs( *geo, source, parts, *job );
			size_t pieces = 0; // the fractured copy's own points (text / logo pieces: drawn instead while the pieces fly)
			for( size_t p = 0; p < parts.size(); ++p )
				pieces += parts[ p ].fractured ? job->ranges[ p ][ 1 ] : 0;
			Log( "POINTS %s: %zu points (+ %zu on the pieces) on %s, %.0f ms (worker)", kSourceName[ source ], job->refs.size() / 4 - pieces, pieces, name.c_str(), job->ms );
			job->done.store( true );
		} ).detach();
	}
	if( !s->ready && s->job && s->job->done.load() )
	{
		const PointSet::Job& j = *s->job;
		glGenBuffers( 1, &s->buf );
		glBindBuffer( GL_TEXTURE_BUFFER, s->buf );
		glBufferData( GL_TEXTURE_BUFFER, std::max< size_t >( 16, j.refs.size() * sizeof( float ) ), j.refs.empty() ? nullptr : j.refs.data(), GL_STATIC_DRAW );
		glGenTextures( 1, &s->tex );
		glBindTexture( GL_TEXTURE_BUFFER, s->tex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, s->buf ); // after glBufferData (Apple GL keeps reading old storage otherwise)
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
		s->ranges = j.ranges;
		s->count  = j.refs.size() / 4;
		s->ready  = true;
		s->job.reset();
	}
	return s->ready ? s : nullptr;
}
}
