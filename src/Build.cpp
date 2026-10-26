// W9 Stage B: the Build looks (classic bundles) and the cut machinery Stage C reuses. The looks live in the DEFORM twins (Shaders.h, under
// #ifdef DEFORM: every soup family already has one), so they follow clones, letters, the morph pair, the wire / points looks and the
// shadow passes; the plain programs are untouched. Here: their uniforms, the exploded view's groups (per model, a worker), the
// slit-scan's pose history (a texture buffer), the plane cut's solid + removed part + stencil cap, and the black hole's screen lens.
//   SLIT-SCAN  each point shows the model as it was up to Build Amount x 2 s ago, the delay growing along the Build Axis: the vertex stage
//              reads the pose (+ the deform phases, the jelly) at its own delay from a 60 / s history and moves by pose(then) x pose(now)^-1
//   PRINT ON   a plane rises along the Build Axis over the model's extent; below it the model is printed (layer ridges, a glowing head
//              band in Wire Color, the cut face capped with the stencil buffer); above it nothing, or the Wire Mode's look (faint wire)
//   EXPLODED   letters (text / logos), else part slots, else islands, else 16 spatial clusters slide out from the centre as rigid pieces
//   BLACK HOLE every point spirals into the centre (the near ones first: the model stretches as it falls), heating up; the post pass bends
//              the whole composited picture round the hole (a point-mass thin lens, beta = theta - theta_E^2 / theta) and draws the
//              event horizon (black, alpha 1) and a photon ring (light, alpha 0)
#include "Generate.h"
#include "Log.h"
#include "Renderer.h"
#include "Shaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>

namespace mehr
{
GLuint Link( const std::string& vs, const std::string& fs, std::string& error );
std::vector< uint32_t > WeldByPosition( const CpuGeometry& g ); // Wire.cpp

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

// the cap: a quad in the cut plane, drawn where the stencil says the plane is inside the solid
const char* kCapVS = R"GLSL(
uniform mat4 uViewProj;
uniform vec3 uCenter, uU, uV; // world: the quad's centre and half axes
out vec2 vQ;
void main()
{
	vec2 q      = vec2( ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0, ( gl_VertexID & 2 ) == 0 ? -1.0 : 1.0 );
	vQ          = q;
	gl_Position = uViewProj * vec4( uCenter + uU * q.x + uV * q.y, 1.0 );
}
)GLSL";
const char* kCapFS = R"GLSL(
in vec2 vQ;
uniform vec3 uColor;  // Wire Color
uniform int uLook;    // 0 print on: the hot fresh top; 1 cross-section: a scanned slice
uniform float uPhase; // scan lines, cycles (wrapped at 1)
uniform float uGlow;
out vec4 fragColor;
void main()
{
	vec2 q  = vQ * 40.0;
	float aa = fwidth( q.x ) + fwidth( q.y );
	vec3 c;
	if( uLook == 0 )
	{
		// fresh print: the colour glowing, a fine raster of the path
		float l = abs( fract( q.x * 0.5 ) - 0.5 );
		c = uColor * ( 0.16 + 0.1 * smoothstep( 0.0, 0.25 + aa, l ) ) * ( 0.8 + 0.2 * uGlow ) + vec3( 0.03 );
	}
	else
	{
		// a scanned slice: dark interior, a diagonal hatch, scan lines sweeping across
		float h = abs( fract( ( q.x + q.y ) * 0.35 ) - 0.5 );
		float s = pow( 0.5 + 0.5 * cos( 6.2831853 * ( vQ.y * 3.0 - uPhase ) ), 24.0 );
		c = vec3( 0.06 ) + uColor * ( 0.18 * ( 1.0 - smoothstep( 0.0, 0.08 + aa, h ) ) + 0.9 * s * uGlow );
	}
	fragColor = vec4( pow( max( c, 0.0 ), vec3( 1.0 / 2.2 ) ), 1.0 );
}
)GLSL";

// the window's openings: a disc facing the mask's front at each eye opening (colour writes off: the stencil only)
const char* kWinMarkVS = R"GLSL(
uniform mat4 uViewProj;
uniform vec3 uCenter, uU, uV;
out vec2 vQ;
void main()
{
	vec2 q      = vec2( ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0, ( gl_VertexID & 2 ) == 0 ? -1.0 : 1.0 );
	vQ          = q;
	gl_Position = uViewProj * vec4( uCenter + uU * q.x + uV * q.y, 1.0 );
}
)GLSL";
const char* kWinMarkFS = R"GLSL(
in vec2 vQ;
out vec4 fragColor;
void main()
{
	if( dot( vQ, vQ ) > 1.0 )
		discard; // a disc (a small program: the end-of-shader rule is for the big variants)
	fragColor = vec4( 0.0 );
}
)GLSL";
// inside the openings: the depth reset to far and the other world's backdrop (a deep dark), full screen at the far plane
const char* kWinQuadVS = R"GLSL(
out vec2 vUV;
void main()
{
	vec2 q      = vec2( ( gl_VertexID & 1 ) == 0 ? 0.0 : 1.0, ( gl_VertexID & 2 ) == 0 ? 0.0 : 1.0 );
	vUV         = q;
	gl_Position = vec4( q * 2.0 - 1.0, 1.0, 1.0 ); // the far plane
}
)GLSL";
const char* kWinQuadFS = R"GLSL(
in vec2 vUV;
uniform vec3 uDeep;
out vec4 fragColor;
void main()
{
	float v   = length( vUV - 0.5 );
	fragColor = vec4( uDeep * ( 1.0 - 0.6 * v ), 1.0 );
}
)GLSL";

// the post pass with the black hole's lens (kPostFS + the lens: the plain post program is unchanged)
const char* kPostLensFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uBase;
uniform sampler2D uBloom;
uniform sampler2D uGhost;
uniform sampler2D uStreak;
uniform float uBloomAmt;
uniform float uGhostAmt;
uniform float uStreakAmt;
uniform vec2 uHole;   // the hole, uv
uniform float uRE;    // the Einstein radius, screen heights
uniform float uAspect;
uniform vec3 uRing;   // the photon ring's light
void main()
{
	vec2 d   = ( vUV - uHole ) * vec2( uAspect, 1.0 );
	float r2 = max( dot( d, d ), 1e-10 ), r = sqrt( r2 );
	vec2 uv  = uHole + ( d - d * ( uRE * uRE / r2 ) ) / vec2( uAspect, 1.0 ); // point-mass thin lens: beta = theta - theta_E^2 / theta
	vec4 c   = texture( uBase, uv );
	vec3 g   = texture( uBloom, uv ).rgb * uBloomAmt * 0.25 + texture( uGhost, uv ).rgb * uGhostAmt * 0.15
	         + texture( uStreak, uv ).rgb * vec3( 0.75, 0.85, 1.0 ) * uStreakAmt * 0.6;
	c.rgb += g;
	float hz = 0.42 * uRE, aa = max( fwidth( r ), 1e-5 );
	c        = mix( c, vec4( 0.0, 0.0, 0.0, 1.0 ), 1.0 - smoothstep( hz - aa, hz + aa, r ) ); // the event horizon
	float x  = ( r - hz * 1.18 ) / ( 0.07 * uRE + 1e-5 );
	c.rgb += uRing * exp( -x * x );                                                           // the photon ring: light (alpha kept)
	fragColor = c;
}
)GLSL";

// per GPU triangle (whole mesh, then the fractured copy) the centre of its group (fitted): letters, else part slots, else islands,
// else 16 spatial clusters (one solid scan has one island: it still comes apart)
void BuildGroups( const CpuGeometry& g, const std::vector< int >& triSlot, std::vector< float >& out )
{
	const size_t nw = g.idx.size() / 3, nt = nw + g.fracIdx.size() / 3;
	auto corner     = [ & ]( size_t t, int c ) { return t < nw ? g.idx[ t * 3 + c ] : g.fracIdx[ ( t - nw ) * 3 + c ]; };
	std::vector< float > cen( nt * 3 ), area( nt );
	for( size_t t = 0; t < nt; ++t )
	{
		const float *a = &g.pos[ corner( t, 0 ) * 3 ], *b = &g.pos[ corner( t, 1 ) * 3 ], *c = &g.pos[ corner( t, 2 ) * 3 ];
		const float u[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, v[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] };
		const float x = u[ 1 ] * v[ 2 ] - u[ 2 ] * v[ 1 ], y = u[ 2 ] * v[ 0 ] - u[ 0 ] * v[ 2 ], z = u[ 0 ] * v[ 1 ] - u[ 1 ] * v[ 0 ];
		area[ t ] = 0.5f * std::sqrt( x * x + y * y + z * z ) + 1e-12f;
		for( int k = 0; k < 3; ++k )
			cen[ t * 3 + k ] = ( ( a[ k ] + b[ k ] + c[ k ] ) / 3.0f - g.fitCenter[ k ] ) * g.fitScale;
	}
	std::vector< uint32_t > grp( nt, 0 );
	uint32_t ng = 0;
	if( !g.triGroup.empty() )
	{
		for( size_t t = 0; t < nt; ++t )
			grp[ t ] = t < g.triGroup.size() ? g.triGroup[ t ] : 0;
		ng = uint32_t( std::max( 1, g.groupCount ) );
	}
	else
	{
		int slots = 0;
		std::vector< int > seen( 64, 0 );
		for( size_t t = 0; t < nw; ++t )
			if( !seen[ triSlot[ t ] & 63 ]++ )
				++slots;
		if( slots > 1 )
		{
			for( size_t t = 0; t < nw; ++t )
				grp[ t ] = uint32_t( triSlot[ t ] );
			ng = 64;
		}
		else
		{
			// islands (welded): union-find
			const std::vector< uint32_t > weld = WeldByPosition( g );
			std::vector< uint32_t > par( weld.empty() ? 0 : *std::max_element( weld.begin(), weld.end() ) + 1 );
			std::iota( par.begin(), par.end(), 0u );
			auto find = [ & ]( uint32_t x ) {
				while( par[ x ] != x )
					x = par[ x ] = par[ par[ x ] ];
				return x;
			};
			for( size_t t = 0; t < nw; ++t )
				for( int c = 1; c < 3; ++c )
				{
					const uint32_t a = find( weld[ corner( t, 0 ) ] ), b = find( weld[ corner( t, c ) ] );
					if( a != b )
						par[ a ] = b;
				}
			std::vector< uint32_t > id( par.size(), ~0u );
			std::vector< double > ia;
			for( size_t t = 0; t < nw; ++t )
			{
				const uint32_t r = find( weld[ corner( t, 0 ) ] );
				if( id[ r ] == ~0u )
					id[ r ] = uint32_t( ia.size() ), ia.push_back( 0.0 );
				grp[ t ] = id[ r ];
				ia[ id[ r ] ] += area[ t ];
			}
			const double tot = std::accumulate( ia.begin(), ia.end(), 0.0 );
			const double big = ia.empty() ? 1.0 : *std::max_element( ia.begin(), ia.end() ) / std::max( tot, 1e-30 );
			ng               = uint32_t( ia.size() );
			if( ia.size() < 2 || big > 0.85 || ia.size() > 4096 )
			{
				// spatial clusters: k-means on the triangle centres (area-weighted), seeds spread deterministically
				const int K = 16;
				std::vector< float > c( K * 3 );
				for( int k = 0; k < K; ++k )
				{
					const size_t t = size_t( ( k * 2654435761u ) % std::max< size_t >( 1, nw ) );
					for( int j = 0; j < 3; ++j )
						c[ k * 3 + j ] = cen[ t * 3 + j ];
				}
				for( int it = 0; it < 8; ++it )
				{
					std::vector< double > acc( K * 4, 0.0 );
					for( size_t t = 0; t < nw; ++t )
					{
						int best  = 0;
						float bd = 1e30f;
						for( int k = 0; k < K; ++k )
						{
							const float dx = cen[ t * 3 ] - c[ k * 3 ], dy = cen[ t * 3 + 1 ] - c[ k * 3 + 1 ], dz = cen[ t * 3 + 2 ] - c[ k * 3 + 2 ];
							const float d = dx * dx + dy * dy + dz * dz;
							if( d < bd )
								bd = d, best = k;
						}
						grp[ t ] = uint32_t( best );
						for( int j = 0; j < 3; ++j )
							acc[ best * 4 + j ] += cen[ t * 3 + j ] * area[ t ];
						acc[ best * 4 + 3 ] += area[ t ];
					}
					for( int k = 0; k < K; ++k )
						if( acc[ k * 4 + 3 ] > 0.0 )
							for( int j = 0; j < 3; ++j )
								c[ k * 3 + j ] = float( acc[ k * 4 + j ] / acc[ k * 4 + 3 ] );
				}
				ng = K;
			}
		}
	}
	// group centres (area-weighted) -> per triangle
	std::vector< double > gc( size_t( ng ) * 4, 0.0 );
	for( size_t t = 0; t < nt; ++t )
	{
		const uint32_t k = std::min( grp[ t ], ng - 1 );
		for( int j = 0; j < 3; ++j )
			gc[ k * 4 + j ] += cen[ t * 3 + j ] * area[ t ];
		gc[ k * 4 + 3 ] += area[ t ];
	}
	out.assign( nt * 4, 0.0f );
	for( size_t t = 0; t < nt; ++t )
	{
		const uint32_t k = std::min( grp[ t ], ng - 1 );
		for( int j = 0; j < 3; ++j )
			out[ t * 4 + j ] = gc[ k * 4 + 3 ] > 0.0 ? float( gc[ k * 4 + j ] / gc[ k * 4 + 3 ] ) : 0.0f;
	}
}
}

bool Renderer::InitBuild( std::string& error )
{
	progCap      = Link( kCapVS, kCapFS, error );
	progPostLens = Link( kQuadVS, kPostLensFS, error );
	progWinMark  = Link( kWinMarkVS, kWinMarkFS, error );
	progWinQuad  = Link( kWinQuadVS, kWinQuadFS, error );
	if( !progCap || !progPostLens || !progWinMark || !progWinQuad )
		return false;
	glUseProgram( progPostLens );
	glUniform1i( U( progPostLens, "uBase" ), 0 );
	glUniform1i( U( progPostLens, "uBloom" ), 1 );
	glUniform1i( U( progPostLens, "uGhost" ), 2 );
	glUniform1i( U( progPostLens, "uStreak" ), 3 );
	const float z[ 8 ] = {};
	glGenBuffers( 1, &xDummyBuf );
	glBindBuffer( GL_TEXTURE_BUFFER, xDummyBuf );
	glBufferData( GL_TEXTURE_BUFFER, sizeof( z ), z, GL_STATIC_DRAW );
	glGenTextures( 1, &xDummy );
	glBindTexture( GL_TEXTURE_BUFFER, xDummy );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, xDummyBuf );
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	// every DEFORM twin: the exploded groups on unit 31, the slit history on unit 32
	for( const auto& t : deformTwins )
	{
		glUseProgram( t.second );
		glUniform1i( U( t.second, "uXGroup" ), 31 );
		glUniform1i( U( t.second, "uXHist" ), 32 );
	}
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleaseBuild()
{
	auto delTex = []( GLuint& t ) { if( t ) glDeleteTextures( 1, &t ), t = 0; };
	auto delBuf = []( GLuint& b ) { if( b ) glDeleteBuffers( 1, &b ), b = 0; };
	for( XGroups& g : xGroups )
	{
		delTex( g.tex ), delBuf( g.buf );
		g = XGroups();
	}
	delTex( xDummy ), delBuf( xDummyBuf ), delTex( xHistTex ), delBuf( xHistBuf );
	xHistCap = 0;
	for( GLuint* p : { &progCap, &progPostLens, &progWinMark, &progWinQuad } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
}

GLuint Renderer::XGroupTex( GpuModel* model, bool want )
{
	if( !want || !model || !model->geometry )
		return xDummy;
	XGroups* e = nullptr;
	for( XGroups& x : xGroups )
		if( x.serial == model->serial )
			e = &x;
	if( !e )
	{
		static unsigned next = 0;
		e = &xGroups[ next++ % 4 ];
		if( e->tex )
			glDeleteTextures( 1, &e->tex ), e->tex = 0;
		if( e->buf )
			glDeleteBuffers( 1, &e->buf ), e->buf = 0;
		*e        = XGroups();
		e->serial = model->serial;
		e->job    = std::make_shared< std::vector< float > >();
		e->done   = std::make_shared< std::atomic< bool > >( false );
		// the part slot of every whole-mesh triangle (GPU order)
		auto slot = std::make_shared< std::vector< int > >( model->geometry->idx.size() / 3, 0 );
		for( const GpuPart& p : model->parts )
			if( !p.fractured )
				for( uint32_t t = p.firstIndex / 3; t < ( p.firstIndex + p.indexCount ) / 3 && t < slot->size(); ++t )
					( *slot )[ t ] = p.slot;
		auto geo = model->geometry;
		auto out = e->job;
		auto done = e->done;
		const std::string name = LogName( model->path );
		std::thread( [ geo, slot, out, done, name ]() {
			const auto t0 = std::chrono::steady_clock::now();
			BuildGroups( *geo, *slot, *out );
			Log( "EXPLODED groups on %s: %zu triangles, %.0f ms (worker)", name.c_str(), out->size() / 4,
			     std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
			done->store( true );
		} ).detach();
	}
	if( !e->ready && e->done && e->done->load() )
	{
		glGenBuffers( 1, &e->buf );
		glBindBuffer( GL_TEXTURE_BUFFER, e->buf );
		glBufferData( GL_TEXTURE_BUFFER, std::max< size_t >( 16, e->job->size() * sizeof( float ) ), e->job->empty() ? nullptr : e->job->data(), GL_STATIC_DRAW );
		glGenTextures( 1, &e->tex );
		glBindTexture( GL_TEXTURE_BUFFER, e->tex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, e->buf ); // after glBufferData (Apple GL keeps reading old storage otherwise)
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
		e->ready = true;
		e->job.reset();
	}
	return e->ready ? e->tex : xDummy; // until the groups land: nothing moves
}

// the cut plane along the axis at cutPos of the model's extent: in world (normalised: distances are world units) and in fitted space
void Renderer::XPlanes( const FrameState& fs, GpuModel* model, float* world, float* fitted, float& worldPerFitted, int plane ) const
{
	const XState& x = fs.x;
	const int a     = std::clamp( x.axis, 0, 2 );
	const float* box = model && model->geometry ? model->geometry->fitBox : nullptr;
	const float lo   = box ? box[ a ] : -1.0f, hi = box ? box[ 3 + a ] : 1.0f;
	const float m    = 0.01f * ( hi - lo );
	const float h    = lo - m + x.cutPos * ( hi - lo + 2.0f * m ) - ( plane == 1 ? x.slab * ( hi - lo ) : 0.0f ); // plane 1: the slab's lower face
	for( int k = 0; k < 3; ++k )
		fitted[ k ] = k == a ? 1.0f : 0.0f;
	fitted[ 3 ] = -h;
	// raw: n_f . ( ( r - c ) s ) - h = 0  ->  ( s n_f ) . r - s n_f . c - h = 0; world: ( M^-T ) x the raw plane
	const float s = model ? model->fitScale : 1.0f;
	float pr[ 4 ] = { 0, 0, 0, -h };
	for( int k = 0; k < 3; ++k )
		pr[ k ] = s * fitted[ k ], pr[ 3 ] -= s * fitted[ k ] * ( model ? model->fitCenter[ k ] : 0.0f );
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	const Mat4 inv = M.Inverse();
	for( int r = 0; r < 4; ++r ) // ( M^-1 )^T p: row r of the transpose = column r of the inverse
		world[ r ] = inv.m[ r * 4 + 0 ] * pr[ 0 ] + inv.m[ r * 4 + 1 ] * pr[ 1 ] + inv.m[ r * 4 + 2 ] * pr[ 2 ] + inv.m[ r * 4 + 3 ] * pr[ 3 ];
	const float n = std::sqrt( world[ 0 ] * world[ 0 ] + world[ 1 ] * world[ 1 ] + world[ 2 ] * world[ 2 ] );
	for( int k = 0; k < 4; ++k )
		world[ k ] /= std::max( n, 1e-12f );
	worldPerFitted = fs.modelScale / std::max( s, 1e-12f );
}

void Renderer::SetXUniforms( GLuint p, const FrameState& fs, GpuModel* model )
{
	const XState& x = fs.x;
	glUniform1i( U( p, "uXLook" ), x.look );
	glUniform1i( U( p, "uXAxis" ), x.axis );
	glUniform1f( U( p, "uXAmt" ), x.amt );
	glUniform1f( U( p, "uXSwirl" ), x.swirl );
	glUniform1f( U( p, "uXSlitPhase" ), x.slitPhase );
	const size_t n = x.look == 1 ? x.hist.size() / 32 : 0;
	glUniform1f( U( p, "uXHistN" ), float( n ) );
	if( n )
		glUniformMatrix4fv( U( p, "uXPoseInv" ), 1, GL_FALSE, x.poseInv );
	glActiveTexture( GL_TEXTURE0 + 31 );
	glBindTexture( GL_TEXTURE_BUFFER, XGroupTex( model, x.look == 3 ) );
	glActiveTexture( GL_TEXTURE0 + 32 );
	glBindTexture( GL_TEXTURE_BUFFER, n && xHistTex ? xHistTex : xDummy );
	glActiveTexture( GL_TEXTURE0 );
	glUniform1i( U( p, "uXCut" ), xCutNow );
	// W9 Stage D: the camera looks (3 .. 6 = Surf Look X-Ray .. LED Matrix)
	const SurfLookState& S = fs.sl;
	glUniform1i( U( p, "uXSurf" ), S.look >= 3 ? S.look : 0 );
	if( S.look >= 3 )
	{
		glUniform1f( U( p, "uXSurfAmt" ), S.amount );
		glUniform1f( U( p, "uXSurfScale" ), S.scale );
		glUniform1f( U( p, "uXSurfPhase" ), S.phase );
		glUniform3fv( U( p, "uXColor" ), 1, S.color );
		glUniform1f( U( p, "uXColorMix" ), S.colorMix );
		// the model on screen (the night vision's vignette): its centre and a fitted unit's reach, in pixels
		Mat4 M, V, P;
		std::memcpy( M.m, fs.model, sizeof( M.m ) );
		std::memcpy( V.m, fs.view, sizeof( V.m ) );
		std::memcpy( P.m, fs.proj, sizeof( P.m ) );
		float c[ 3 ], clip[ 4 ];
		M.Point( model ? model->fitCenter : x.box, c );
		const Mat4 VP = P * V;
		for( int r = 0; r < 4; ++r )
			clip[ r ] = VP.m[ r ] * c[ 0 ] + VP.m[ 4 + r ] * c[ 1 ] + VP.m[ 8 + r ] * c[ 2 ] + VP.m[ 12 + r ];
		const float cw = std::max( clip[ 3 ], 1e-4f );
		const float wpf = fs.modelScale / std::max( model ? model->fitScale : 1.0f, 1e-12f );
		glUniform3f( U( p, "uXScreen" ), ( clip[ 0 ] / cw * 0.5f + 0.5f ) * fs.width, ( clip[ 1 ] / cw * 0.5f + 0.5f ) * fs.height, wpf * P.m[ 5 ] / cw * 0.5f * fs.height * 1.3f );
	}
	if( xCutNow )
	{
		float world[ 4 ], fitted[ 4 ], wpf = 1;
		XPlanes( fs, model, world, fitted, wpf );
		glUniform4fv( U( p, "uXPlane" ), 1, world );
		glUniform4fv( U( p, "uXPlaneO" ), 1, fitted );
		glUniform3fv( U( p, "uXColor" ), 1, x.color );
		glUniform1f( U( p, "uXGlow" ), x.glow );
		const int a = std::clamp( x.axis, 0, 2 );
		const float ext = model && model->geometry ? model->geometry->fitBox[ 3 + a ] - model->geometry->fitBox[ a ] : 2.0f;
		glUniform1i( U( p, "uXRegion" ), x.region );
		glUniform2f( U( p, "uXSlab" ), x.slab * ext * wpf, x.slab * ext );
		if( x.sphere ) // MoveEhr W5 Reveal Sphere: on this model's own fitted box
		{
			const float* b = model && model->geometry ? model->geometry->fitBox : nullptr;
			const float lo[ 3 ] = { b ? b[ 0 ] : -1.0f, b ? b[ 1 ] : -1.0f, b ? b[ 2 ] : -1.0f }, hi[ 3 ] = { b ? b[ 3 ] : 1.0f, b ? b[ 4 ] : 1.0f, b ? b[ 5 ] : 1.0f };
			const float diag = std::sqrt( ( hi[ 0 ] - lo[ 0 ] ) * ( hi[ 0 ] - lo[ 0 ] ) + ( hi[ 1 ] - lo[ 1 ] ) * ( hi[ 1 ] - lo[ 1 ] ) + ( hi[ 2 ] - lo[ 2 ] ) * ( hi[ 2 ] - lo[ 2 ] ) );
			const float box[ 4 ] = { 0.5f * ( lo[ 0 ] + hi[ 0 ] ), lo[ 1 ] + ( hi[ 1 ] - lo[ 1 ] ) * x.spherePos, 0.5f * ( lo[ 2 ] + hi[ 2 ] ), x.sphereR * diag };
			glUniform4fv( U( p, "uXBox" ), 1, box );
		}
		else
			glUniform4fv( U( p, "uXBox" ), 1, x.box );
		glUniform1f( U( p, "uXRound" ), x.round );
		glUniform1f( U( p, "uXBand" ), x.region == 1 ? 0.025f : 0.012f * ext * wpf ); // the box's band is in fitted units
		glUniform1f( U( p, "uXLayer" ), x.layers ? ext / 90.0f : 0.0f );
	}
}

// one model's main draw. With a plane cut: the kept part solid, its cut face capped, the removed part in the Wire Mode's look (or nothing)
void Renderer::DrawSide( GLuint prog, const FrameState& fs, GpuModel* model, const std::vector< Instance >* inst )
{
	if( fs.sl.look == 3 && !fs.wire.on )
	{
		// W9 X-Ray: additive (premultiplied light and a little alpha), every layer: no depth test, no depth write
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		glDepthMask( GL_FALSE );
		glDisable( GL_DEPTH_TEST );
		xCutNow = fs.x.cut ? 1 : 0; // a cut still cuts (no cap: nothing is solid in an x-ray)
		DrawModel( prog, fs, model, false, true, 0, nullptr, nullptr, inst );
		xCutNow = 0;
		glEnable( GL_DEPTH_TEST );
		glDepthMask( GL_TRUE );
		glDisable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ZERO );
		return;
	}
	if( fs.x.cut == 0 )
	{
		const bool soup = fs.wireMix > 0.0f || fs.explode > 0.0f || fs.shape.on;
		if( fs.wire.on )
			DrawWired( prog, fs, model, inst );
		else
			DrawModel( soup ? prog : progMain, fs, model, false, soup, 0, nullptr, nullptr, inst );
		return;
	}
	FrameState solid = fs;
	solid.wire.on = false, solid.wireMix = 0.0f;
	xCutNow = 1;
	DrawModel( prog, solid, model, false, true, 0, nullptr, nullptr, inst );
	if( fs.x.cap && fs.x.region == 0 )
	{
		DrawCap( prog, solid, model, 0 );
		if( fs.x.slab > 0.0f )
			DrawCap( prog, solid, model, 1 ); // the slab's lower face
	}
	if( fs.wire.on || fs.wireMix > 0.0f || fs.x.removedWire )
	{
		FrameState rem = fs;
		if( !rem.wire.on && fs.wireMix > 0.0f )
			rem.wire.on = true, rem.wire.mode = 1, rem.wire.mix = fs.wireMix, rem.wireMix = 0.0f;
		else if( !rem.wire.on ) // the reveal box's outside with Wire Mode Off: Wire Only, full strength
			rem.wire.on = true, rem.wire.mode = 2, rem.wire.mix = 1.0f, rem.wire.back = 0, rem.wire.edges = 0;
		xCutNow = 2;
		DrawWired( prog, rem, model, inst );
	}
	xCutNow = 0;
}

// the stencil cap (the cut's face): the kept part's faces, front and back, flip one stencil bit per layer (no depth test: every layer
// along the ray counts), so an odd count = the plane is inside the solid there; then a quad in the plane draws where it is odd, depth
// tested against the model (the kept part in front of it hides it). Open / non-watertight meshes: the parity is wrong where a hole
// lets a ray miss a layer (stray cap patches, or none), as with any stencil capping.
void Renderer::DrawCap( GLuint prog, const FrameState& fs, GpuModel* model, int plane )
{
	float world[ 4 ], fitted[ 4 ], wpf = 1;
	XPlanes( fs, model, world, fitted, wpf, plane );
	glEnable( GL_STENCIL_TEST );
	glClearStencil( 0 );
	glStencilMask( 0xFF );
	glClear( GL_STENCIL_BUFFER_BIT );
	glStencilMask( 1 );
	glStencilFunc( GL_ALWAYS, 0, 1 );
	glStencilOp( GL_KEEP, GL_KEEP, GL_INVERT );
	glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	glDepthMask( GL_FALSE );
	glDisable( GL_DEPTH_TEST );
	DrawModel( prog, fs, model, false, true, 0 );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glDepthMask( GL_TRUE );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glStencilFunc( GL_EQUAL, 1, 1 );
	glStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	// the quad: centred on the model's centre projected onto the plane, 2.5 fitted units each way
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	float c[ 3 ], n[ 3 ] = { world[ 0 ], world[ 1 ], world[ 2 ] };
	M.Point( model->fitCenter, c );
	const float d = n[ 0 ] * c[ 0 ] + n[ 1 ] * c[ 1 ] + n[ 2 ] * c[ 2 ] + world[ 3 ];
	for( int k = 0; k < 3; ++k )
		c[ k ] -= n[ k ] * d;
	float u[ 3 ], v[ 3 ], t[ 3 ] = { std::fabs( n[ 1 ] ) < 0.9f ? 0.0f : 1.0f, std::fabs( n[ 1 ] ) < 0.9f ? 1.0f : 0.0f, 0.0f };
	Mat4::Cross( n, t, u );
	Mat4::Norm( u );
	Mat4::Cross( n, u, v );
	const float R = 2.5f * wpf;
	for( int k = 0; k < 3; ++k )
		u[ k ] *= R, v[ k ] *= R;
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	glUseProgram( progCap );
	glUniformMatrix4fv( U( progCap, "uViewProj" ), 1, GL_FALSE, VP.m );
	glUniform3fv( U( progCap, "uCenter" ), 1, c );
	glUniform3fv( U( progCap, "uU" ), 1, u );
	glUniform3fv( U( progCap, "uV" ), 1, v );
	glUniform3fv( U( progCap, "uColor" ), 1, fs.x.color );
	glUniform1i( U( progCap, "uLook" ), fs.x.capLook );
	glUniform1f( U( progCap, "uPhase" ), fs.x.capPhase );
	glUniform1f( U( progCap, "uGlow" ), std::min( 2.0f, fs.x.glow ) );
	const GLboolean blend = glIsEnabled( GL_BLEND );
	glDisable( GL_BLEND );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	glBindVertexArray( 0 );
	if( blend )
		glEnable( GL_BLEND );
	glStencilMask( 0xFF );
	glDisable( GL_STENCIL_TEST );
}

void Renderer::SetLensUniforms( GLuint p, const FrameState& fs, int w, int h )
{
	Mat4 M, V, P;
	const FrameState& f = lensFs ? *lensFs : fs;
	std::memcpy( M.m, f.model, sizeof( M.m ) );
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	float c[ 3 ] = { 0, 0, 0 };
	const float fc[ 3 ] = { 0, 0, 0 };
	M.Point( lensModel ? lensModel->fitCenter : fc, c );
	const Mat4 VP = P * V;
	float clip[ 4 ];
	for( int r = 0; r < 4; ++r )
		clip[ r ] = VP.m[ r ] * c[ 0 ] + VP.m[ 4 + r ] * c[ 1 ] + VP.m[ 8 + r ] * c[ 2 ] + VP.m[ 12 + r ];
	const float cw = std::max( clip[ 3 ], 1e-4f );
	const float wpf = f.modelScale / std::max( lensModel ? lensModel->fitScale : 1.0f, 1e-12f );
	const float screenR = wpf * P.m[ 5 ] / cw * 0.5f; // a fitted unit, in screen heights
	const float re      = clip[ 3 ] > 1e-4f ? fs.x.lensAmt * 0.55f * screenR : 0.0f;
	glUniform2f( U( p, "uHole" ), clip[ 0 ] / cw * 0.5f + 0.5f, clip[ 1 ] / cw * 0.5f + 0.5f );
	glUniform1f( U( p, "uRE" ), re );
	glUniform1f( U( p, "uAspect" ), float( w ) / float( std::max( 1, h ) ) );
	const float k = 0.9f * fs.x.lensAmt;
	glUniform3f( U( p, "uRing" ), 1.0f * k, 0.72f * k, 0.45f * k );
}

// W9 prewarm at model swap (the scissor is 1 px): the CAPTURE twins (plain + DEFORM, through one dummy spot), the line programs, the cut's
// stencil pass (colour writes off: a pipeline of its own) and the cap
void Renderer::WarmLooks( const FrameState& fs, GpuModel* model )
{
	Capture& c = capture[ 0 ];
	if( c.cap == 0 )
	{
		c.cap = 1024;
		for( int k = 0; k < 3; ++k )
		{
			glGenBuffers( 1, &c.buf[ k ] ), glGenTextures( 1, &c.tex[ k ] );
			glBindBuffer( GL_TEXTURE_BUFFER, c.buf[ k ] );
			glBufferData( GL_TEXTURE_BUFFER, c.cap * 16, nullptr, GL_DYNAMIC_COPY );
			glBindTexture( GL_TEXTURE_BUFFER, c.tex[ k ] );
			glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, c.buf[ k ] );
		}
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	}
	std::vector< std::array< uint32_t, 2 > > one( model->parts.size(), { 0u, 0u } );
	for( size_t i = 0; i < model->parts.size(); ++i )
		if( !model->parts[ i ].fractured )
		{
			one[ i ] = { 0u, 1u };
			break;
		}
	FrameState d = fs;
	for( int k = 0; k < 3; ++k )
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, c.buf[ k ] );
	captureNow = true, captureRefTex = pointDummy, captureRanges = &one;
	for( int pass = 0; pass < 2; ++pass )
	{
		d.shape.on = d.shape.deform = pass == 1;
		DrawModel( progSoup, d, model, false, true, 0 );
	}
	captureNow = false, captureRanges = nullptr;
	for( int k = 0; k < 3; ++k )
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, 0 );
	// the line programs (one instance each, as light)
	LineSet dummy;
	dummy.count = 1, dummy.lines = 1, dummy.pairTex = xDummy;
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	for( int look : { 1, 3 } )
	{
		d.lines.look = look;
		DrawLines( { &d, &dummy, 0, 1.0f, 1.0f }, false );
	}
	// W10: the light-paint ribbon (DrawLines draws it only once a ring has filled, so it is drawn here directly: one strip, one head)
	glUseProgram( progLineRibbon );
	glUniform1i( U( progLineRibbon, "uFilled" ), 1 );
	for( int k = 0; k < 4; ++k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_BUFFER, k < 3 ? c.tex[ k ] : xDummy );
	}
	glBindVertexArray( quadVAO );
	glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, 1 );
	for( int k = 3; k >= 0; --k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	glBindVertexArray( 0 );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	// the cut: the stencil pass + the cap (DrawCap), the kept / removed sides' draws
	d.shape.on = d.shape.deform = true;
	d.x.cut = 1, d.x.look = 2;
	xCutNow = 1;
	DrawCap( progSoup, d, model );
	xCutNow = 0;
	// W9 Stage D: the X-Ray draw state (additive, no depth), the spikes, the copies (the model itself as the copy, one instance)
	d.x = XState(), d.sl = SurfLookState();
	d.sl.look = 3, d.sl.amount = 1.0f;
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	glDepthMask( GL_FALSE );
	glDisable( GL_DEPTH_TEST );
	DrawModel( progSoup, d, model, false, true, 3 );
	glEnable( GL_DEPTH_TEST );
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	glUseProgram( progSpike );
	for( int k = 0; k < 3; ++k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_BUFFER, c.tex[ k ] );
	}
	glBindVertexArray( quadVAO );
	glDrawArraysInstanced( GL_TRIANGLES, 0, 3, 1 );
	for( int k = 2; k >= 0; --k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	glBindVertexArray( 0 );
	FrameState cf = fs;
	cf.inst.clear(), cf.ghosts.clear();
	cf.shape = ShapeState(), cf.wire.on = false, cf.wireMix = 0.0f;
	glActiveTexture( GL_TEXTURE0 + 33 );
	glBindTexture( GL_TEXTURE_BUFFER, c.tex[ 0 ] );
	glActiveTexture( GL_TEXTURE0 + 34 );
	glBindTexture( GL_TEXTURE_BUFFER, c.tex[ 1 ] );
	glActiveTexture( GL_TEXTURE0 );
	skinNow = true, skinCount = 1, skinLod = 0;
	DrawModel( progMain, cf, model, false, false, 3 );
	skinNow = false;
}

// WINDOW: the eye openings (a disc each, from the eye finder) and the glass parts mark the stencil where they show (depth tested: a rim in
// front hides its disc); a model without either becomes a window as a whole (its silhouette). There the depth is reset to far, a deep
// backdrop drawn, and the other world: another model slot in its own place behind the mask (bigger, turning). (W11: the Wow bundles'
// projector picture as the world is gone, Amrit's prune: the world is the slot there too)
void Renderer::DrawWindow( const FrameState& fs, GpuModel* model )
{
	const XState& x = fs.x;
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	glEnable( GL_STENCIL_TEST );
	glClearStencil( 0 );
	glStencilMask( 0xFF );
	glClear( GL_STENCIL_BUFFER_BIT );
	glStencilFunc( GL_ALWAYS, 1, 0xFF );
	glStencilOp( GL_KEEP, GL_KEEP, GL_REPLACE );
	glColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	glDepthMask( GL_FALSE );
	glEnable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	bool any = false;
	glUseProgram( progWinMark );
	glUniformMatrix4fv( U( progWinMark, "uViewProj" ), 1, GL_FALSE, VP.m );
	glDepthFunc( GL_LESS );
	glBindVertexArray( quadVAO );
	for( int e = 0; e < 2; ++e )
	{
		if( x.eyes[ e ][ 3 ] <= 0.0f )
			continue;
		float n[ 3 ] = { x.front[ 0 ], x.front[ 1 ], x.front[ 2 ] }, t[ 3 ] = { 0, 1, 0 }, u[ 3 ], v[ 3 ];
		if( std::fabs( n[ 1 ] ) > 0.9f )
			t[ 0 ] = 1, t[ 1 ] = 0;
		Mat4::Cross( n, t, u );
		Mat4::Norm( u );
		Mat4::Cross( n, u, v );
		for( int k = 0; k < 3; ++k )
			u[ k ] *= x.eyes[ e ][ 3 ], v[ k ] *= x.eyes[ e ][ 3 ];
		glUniform3fv( U( progWinMark, "uCenter" ), 1, x.eyes[ e ] );
		glUniform3fv( U( progWinMark, "uU" ), 1, u );
		glUniform3fv( U( progWinMark, "uV" ), 1, v );
		glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
		any = true;
	}
	bool glass = false;
	for( const GpuPart& gp : model->parts )
		glass = glass || gp.glass > 0.0f;
	if( glass || !any )
	{
		// the glass parts (or, with no opening at all, the whole model) where they are in front
		FrameState f = fs;
		f.wire.on = false, f.wireMix = 0.0f, f.x.cut = 0;
		glDepthFunc( GL_LEQUAL );
		glassFilter = glass ? 1 : 0;
		const bool soup = f.explode > 0.0f || f.shape.on;
		EnsureVAO( model );
		DrawModel( soup ? progSoup : progMain, f, model, false, soup, 0, nullptr, nullptr, f.inst.empty() ? nullptr : &f.inst );
		glassFilter = 0;
		glBindVertexArray( quadVAO );
	}
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glDepthMask( GL_TRUE );
	glStencilFunc( GL_EQUAL, 1, 0xFF );
	glStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	// the backdrop at the far plane (depth reset there)
	glDepthFunc( GL_ALWAYS );
	glUseProgram( progWinQuad );
	glUniform3f( U( progWinQuad, "uDeep" ), 0.02f + 0.06f * x.color[ 0 ], 0.02f + 0.06f * x.color[ 1 ], 0.03f + 0.06f * x.color[ 2 ] );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	glDepthFunc( GL_LESS );
	if( x.winModel && x.winModel->Complete() )
	{
		// the other world: a model slot drawn plainly in its own place (its own fit, the frame's lights)
		FrameState w = fs;
		std::memcpy( w.model, x.winM, sizeof( w.model ) );
		w.modelScale = x.winScale;
		w.inst.clear(), w.ghosts.clear();
		w.wire.on = false, w.wireMix = 0.0f, w.x = XState(), w.shape = ShapeState(), w.dissolve = 0.0f, w.explode = 0.0f, w.morph = MorphState();
		w.letterXf.clear();
		const int keep = activeSide;
		EnsureVAO( x.winModel );
		DrawModel( progMain, w, x.winModel, false, false, 0 );
		activeSide = keep;
		EnsureVAO( model );
	}
	glBindVertexArray( 0 );
	glStencilMask( 0xFF );
	glDisable( GL_STENCIL_TEST );
}
}
