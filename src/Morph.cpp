// W6 Stage 2: the transition pack (Transition Style 5..22; Amrit: "how many transitions types between models can be added?").
//   The MORPH program variants (`#define MORPH` on the triangle-soup programs: plain, instanced, their shadow-pass twins, and through
//   Link() their DEFORM / WIRE / POINTS twins) carry the styles that move vertices (Pieces, Implode, Glitch, Ripple, Spin Swap; W7:
//   Twist, Melt) or cut the surface along a front (Scan, Glitch, Ripple, Burn). They draw only during such a hop: every other draw, and every
//   other style, runs the programs it ran before (zero cost when not selected; regress max 0).
//   The point cloud (Points, Swarm): per model slot 65,536 points spread evenly over the area (a worker), sorted by an order key
//   (48 height bands bottom-up, round the up axis in each), their colour baked once from the part textures (a tiny GPU pass). Point i
//   of the outgoing model flies to point i of the incoming one: the same key, so the bottom ring goes to the bottom ring.
//   Everything is slider-driven (scrubbing back retraces, holding still is still); each side's state comes from TransitionOut / In
//   (Renderer.cpp), the pair's data (the other model's shards, ranks, cloud) from here.
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr int kCloudN = 65536, kCloudW = 256; // points per slot; the colour texture is kCloudW wide
constexpr int kCloudFloats = 9;               // raw xyz, normal xyz, uv, part

// the cloud: area-weighted points on the whole mesh, sorted by the order key (height band bottom-up, then round the up axis)
void BuildCloud( const CpuGeometry& g, CloudSlotJob& job )
{
	const auto t0    = std::chrono::steady_clock::now();
	const size_t tri = g.idx.size() / 3;
	std::vector< double > cum( tri );
	std::vector< float > fn( tri * 3 );
	double acc = 0;
	for( size_t t = 0; t < tri; ++t )
	{
		const float* a = &g.pos[ g.idx[ t * 3 ] * 3 ];
		const float* b = &g.pos[ g.idx[ t * 3 + 1 ] * 3 ];
		const float* c = &g.pos[ g.idx[ t * 3 + 2 ] * 3 ];
		float e1[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, e2[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] }, cr[ 3 ];
		Mat4::Cross( e1, e2, cr );
		const float l = std::sqrt( cr[ 0 ] * cr[ 0 ] + cr[ 1 ] * cr[ 1 ] + cr[ 2 ] * cr[ 2 ] );
		for( int k = 0; k < 3; ++k )
			fn[ t * 3 + k ] = l > 0 ? cr[ k ] / l : ( k == 1 ? 1.0f : 0.0f );
		cum[ t ] = acc += l;
	}
	if( tri == 0 || acc <= 0 )
		return;
	std::vector< int > partOf( tri, 0 );
	for( size_t p = 0; p < g.parts.size(); ++p )
		for( uint32_t i = g.parts[ p ][ 0 ] / 3; i < ( g.parts[ p ][ 0 ] + g.parts[ p ][ 1 ] ) / 3 && i < tri; ++i )
			partOf[ i ] = int( p );
	std::mt19937 rng( 13572468u );
	std::uniform_real_distribution< double > U01( 0.0, 1.0 );
	struct P
	{
		float key;
		float v[ kCloudFloats ];
		float ref[ 3 ]; // MoveEhr W1: triangle (uint bits), barycentric b, c (the posed cloud follows an animated model)
	};
	std::vector< P > pts( kCloudN );
	for( int i = 0; i < kCloudN && !job.cancel.load(); ++i )
	{
		const size_t t = std::min( size_t( std::lower_bound( cum.begin(), cum.end(), U01( rng ) * acc ) - cum.begin() ), tri - 1 );
		const double r1 = std::sqrt( U01( rng ) ), r2 = U01( rng );
		const float wa = float( 1 - r1 ), wb = float( r1 * ( 1 - r2 ) ), wc = float( r1 * r2 );
		const uint32_t ia = g.idx[ t * 3 ], ib = g.idx[ t * 3 + 1 ], ic = g.idx[ t * 3 + 2 ];
		P& o = pts[ i ];
		for( int k = 0; k < 3; ++k )
			o.v[ k ] = g.pos[ ia * 3 + k ] * wa + g.pos[ ib * 3 + k ] * wb + g.pos[ ic * 3 + k ] * wc, o.v[ 3 + k ] = fn[ t * 3 + k ];
		for( int k = 0; k < 2; ++k )
			o.v[ 6 + k ] = g.uv[ ia * 2 + k ] * wa + g.uv[ ib * 2 + k ] * wb + g.uv[ ic * 2 + k ] * wc;
		o.v[ 8 ] = float( partOf[ t ] );
		const uint32_t tri = (uint32_t)t;
		std::memcpy( &o.ref[ 0 ], &tri, 4 );
		o.ref[ 1 ] = wb, o.ref[ 2 ] = wc;
		float f[ 3 ];
		for( int k = 0; k < 3; ++k )
			f[ k ] = ( o.v[ k ] - g.fitCenter[ k ] ) * g.fitScale;
		const float band = std::floor( std::clamp( ( f[ 1 ] + 1.0f ) * 0.5f, 0.0f, 0.9999f ) * 48.0f );
		o.key            = band + ( std::atan2( f[ 0 ], f[ 2 ] ) + 3.14159265f ) / 6.2831853f;
	}
	std::sort( pts.begin(), pts.end(), []( const P& a, const P& b ) { return a.key < b.key; } );
	job.pts.resize( size_t( kCloudN ) * kCloudFloats );
	job.refs.resize( size_t( kCloudN ) * 3 );
	for( int i = 0; i < kCloudN; ++i )
	{
		std::memcpy( &job.pts[ size_t( i ) * kCloudFloats ], pts[ i ].v, sizeof( pts[ i ].v ) );
		std::memcpy( &job.refs[ size_t( i ) * 3 ], pts[ i ].ref, sizeof( pts[ i ].ref ) );
	}
	job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}

const char* kCloudVS = R"GLSL(
layout( location = 0 ) in vec3 aPosA;
layout( location = 1 ) in vec3 aNrmA;
layout( location = 2 ) in vec3 aPosB;
layout( location = 3 ) in vec3 aNrmB;
uniform mat4 uModelA, uModelB, uViewProj;
uniform sampler2D uColA, uColB; // baked colours (sqrt-encoded linear), kCloudW wide
uniform float uK, uStagger, uEase, uSpread, uUnit, uN, uPx, uFade, uTwinkle, uPhase;
uniform int uSwarm, uStride;
uniform vec3 uCenter;
out vec3 vCol;
out vec3 vN;
out float vA;
void main()
{
	// point i's order key = its rank; it flies over its own stretch of the hop (Trans Stagger), eased (Trans Ease)
	float k = float( gl_VertexID ) / uN;
	float u = clamp( ( uK - uStagger * k ) / max( 1.0 - uStagger, 1e-3 ), 0.0, 1.0 );
	float s = mix( u, u * u * ( 3.0 - 2.0 * u ), uEase );
	vec3 pa = ( uModelA * vec4( aPosA, 1.0 ) ).xyz, pb = ( uModelB * vec4( aPosB, 1.0 ) ).xyz;
	vec3 p  = mix( pa, pb, s );
	vec3 h  = fract( sin( vec3( k * 12.9898 + 1.1, k * 78.233 + 2.3, k * 37.719 + 3.7 ) * 4375.85 ) * 43758.5453 );
	float b = sin( 3.14159265 * u );
	vec3 out_ = normalize( p - uCenter + vec3( 1e-5 ) );
	if( uSwarm == 0 )
		p += out_ * ( uSpread * 0.6 * uUnit * b ); // an arc out from the centre (Trans Spread)
	else // a swarm's detour: each firefly its own loops
		p += ( vec3( sin( u * 9.0 + h.x * 6.2831853 ), sin( u * 7.0 + h.y * 6.2831853 ), cos( u * 8.0 + h.z * 6.2831853 ) ) * 0.7 + out_ * 0.5 ) * ( uSpread * uUnit * b );
	int id  = gl_VertexID * uStride;
	ivec2 t = ivec2( id % 256, id / 256 );
	vec3 ca = texelFetch( uColA, t, 0 ).rgb, cb = texelFetch( uColB, t, 0 ).rgb;
	vCol    = mix( ca * ca, cb * cb, s );
	vN      = normalize( mix( mat3( uModelA ) * aNrmA, mat3( uModelB ) * aNrmB, s ) + vec3( 1e-6 ) );
	// twinkle: whole twinkles per 1000 (the phase wraps there)
	float rt = floor( ( 0.6 + 0.9 * h.y ) * 1000.0 + 0.5 ) / 1000.0;
	vA       = uFade * ( 1.0 - uTwinkle * ( 0.5 + 0.5 * cos( 6.2831853 * ( fract( uPhase * rt ) + h.x ) ) ) );
	gl_Position  = uViewProj * vec4( p, 1.0 );
	gl_PointSize = uPx;
}
)GLSL";

const char* kCloudFS = R"GLSL(
in vec3 vCol;
in vec3 vN;
in float vA;
out vec4 fragColor;
uniform vec3 uKeyDir, uKeyColor, uTint;
uniform float uAmbient, uTintMix, uGain, uPx;
uniform int uGlow;
void main()
{
	vec2 q  = gl_PointCoord * 2.0 - 1.0;
	float r = length( q );
	if( uGlow == 0 )
	{
		// a lit, round, anti-aliased dot (premultiplied, over the scene)
		float cov = 1.0 - smoothstep( 1.0 - 2.0 / max( uPx, 1.0 ), 1.0, r );
		vec3 lit  = vCol * ( vec3( 0.35 + uAmbient ) + uKeyColor * 0.4 * max( dot( vN, uKeyDir ), 0.0 ) );
		vec3 c    = ToDisplay( mix( lit, uTint, uTintMix ) );
		float a   = cov * vA;
		if( a < 0.004 )
			discard;
		fragColor = vec4( c * a, a );
	}
	else // light: a soft halo added at alpha 0 (W23)
		fragColor = vec4( mix( vCol, uTint, uTintMix ) * uGain * exp( -r * r * 4.0 ) * vA, 0.0 );
}
)GLSL";

// the colour bake: point i writes its part's base colour (factor x texture at its uv) into texel i
const char* kCloudBakeVS = R"GLSL(
layout( location = 2 ) in vec2 aUV;
layout( location = 3 ) in float aPart;
uniform float uPart, uRows;
out vec2 vUV;
void main()
{
	vec2 c       = ( vec2( float( gl_VertexID % 256 ), float( gl_VertexID / 256 ) ) + 0.5 ) / vec2( 256.0, uRows );
	gl_Position  = abs( aPart - uPart ) < 0.5 ? vec4( c * 2.0 - 1.0, 0.0, 1.0 ) : vec4( 2.0, 2.0, 2.0, 1.0 );
	gl_PointSize = 1.0;
	vUV          = aUV;
}
)GLSL";
const char* kCloudBakeFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uBase;
uniform int uHasBase;
uniform vec4 uBaseFactor;
void main()
{
	vec4 b    = uBaseFactor * ( uHasBase == 1 ? textureLod( uBase, vUV, 2.0 ) : vec4( 1.0 ) );
	fragColor = vec4( sqrt( max( b.rgb, vec3( 0.0 ) ) ), 1.0 );
}
)GLSL";

// sampler units: the main-pass set (Renderer.cpp BindSamplers) + the MORPH ones
void BindMorphSamplers( GLuint p )
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
	glUniform1i( U( p, "uPartnerData" ), 28 );
	glUniform1i( U( p, "uRankSelf" ), 29 );
	glUniform1i( U( p, "uRankOther" ), 30 );
	BindShadowSamplers( p );
	glUseProgram( 0 );
}
}

bool Renderer::InitMorph( std::string& error )
{
	const std::string mainFS = std::string( kLightLib ) + kMainFS, shadowFS = std::string( "#define SHADOW_PASS\n" ) + mainFS;
	// linked inside Renderer::Init's twin window: the plain / instanced ones get their DEFORM, WIRE and POINTS twins, the shadow ones
	// their DEFORM twins (sampler units are copied from these)
	progSoupM        = Link( std::string( "#define MORPH\n" ) + kSoupVS, "#define MORPH\n" + mainFS, error );
	progSoupMI       = Link( std::string( "#define MORPH\n#define INSTANCED\n" ) + kSoupVS, "#define MORPH\n#define INSTANCED\n" + mainFS, error );
	progShadowSoupM  = Link( std::string( "#define MORPH\n" ) + kSoupVS, "#define MORPH\n" + shadowFS, error );
	progShadowSoupMI = Link( std::string( "#define MORPH\n#define INSTANCED\n" ) + kSoupVS, "#define MORPH\n#define INSTANCED\n" + shadowFS, error );
	progCloud        = Link( kCloudVS, std::string( kToneLib ) + kCloudFS, error );
	progCloudBake    = Link( kCloudBakeVS, kCloudBakeFS, error );
	if( !progSoupM || !progSoupMI || !progShadowSoupM || !progShadowSoupMI || !progCloud || !progCloudBake )
		return false;
	for( GLuint p : { progSoupM, progSoupMI, progShadowSoupM, progShadowSoupMI } )
		BindMorphSamplers( p );
	glUseProgram( progCloud );
	glUniform1i( U( progCloud, "uColA" ), 0 );
	glUniform1i( U( progCloud, "uColB" ), 1 );
	glUseProgram( progCloudBake );
	glUniform1i( U( progCloudBake, "uBase" ), 0 );
	glUseProgram( 0 );
	glGenFramebuffers( 1, &cloudBakeFBO );
	return true;
}

void Renderer::ReleaseMorph()
{
	for( GLuint* p : { &progSoupM, &progSoupMI, &progShadowSoupM, &progShadowSoupMI, &progCloud, &progCloudBake } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	for( CloudSlot& c : cloud )
	{
		for( GLuint* b : { &c.vbo, &c.refBuf, &c.posedBuf } )
			if( *b )
				glDeleteBuffers( 1, b );
		if( c.colorTex )
			glDeleteTextures( 1, &c.colorTex );
		if( c.job )
			c.job->cancel.store( true );
		c = CloudSlot();
	}
	if( cloudBakeFBO )
		glDeleteFramebuffers( 1, &cloudBakeFBO ), cloudBakeFBO = 0;
	if( cloudWarmVAO )
		glDeleteVertexArrays( 1, &cloudWarmVAO ), cloudWarmVAO = 0;
	if( cloudWarmVBO )
		glDeleteBuffers( 1, &cloudWarmVBO ), cloudWarmVBO = 0;
	if( cloudVAO )
		glDeleteVertexArrays( 1, &cloudVAO ), cloudVAO = 0;
}

// a slot's cloud: built on a worker, uploaded, its colours baked once the model's textures are on the GPU
bool Renderer::UpdateCloud( int slot, GpuModel* model )
{
	if( slot < 0 || slot >= kSlots || !model || !model->geometry )
		return false;
	CloudSlot& c = cloud[ slot ];
	if( c.serial != model->serial )
	{
		for( GLuint* b : { &c.vbo, &c.refBuf, &c.posedBuf } )
			if( *b )
				glDeleteBuffers( 1, b );
		if( c.colorTex )
			glDeleteTextures( 1, &c.colorTex );
		if( c.job )
			c.job->cancel.store( true );
		c        = CloudSlot();
		c.serial = model->serial;
		auto job = std::make_shared< CloudSlotJob >();
		c.job    = job;
		auto geo = model->geometry;
		std::thread( [ job, geo ]() {
			BuildCloud( *geo, *job );
			job->done.store( true );
		} ).detach();
	}
	if( !c.vbo && c.job && c.job->done.load() )
	{
		if( c.job->pts.empty() )
			return false;
		glGenBuffers( 1, &c.vbo );
		glBindBuffer( GL_ARRAY_BUFFER, c.vbo );
		glBufferData( GL_ARRAY_BUFFER, c.job->pts.size() * sizeof( float ), c.job->pts.data(), GL_STATIC_DRAW );
		if( model->anim && c.job->refs.size() == size_t( kCloudN ) * 3 )
		{
			// MoveEhr W1: an animated model's cloud: its refs + the posed points (xyz + face normal), refilled each posed hop frame
			glGenBuffers( 1, &c.refBuf ), glGenBuffers( 1, &c.posedBuf );
			glBindBuffer( GL_ARRAY_BUFFER, c.refBuf );
			glBufferData( GL_ARRAY_BUFFER, c.job->refs.size() * sizeof( float ), c.job->refs.data(), GL_STATIC_DRAW );
			glBindBuffer( GL_ARRAY_BUFFER, c.posedBuf );
			glBufferData( GL_ARRAY_BUFFER, size_t( kCloudN ) * 6 * sizeof( float ), nullptr, GL_DYNAMIC_COPY );
		}
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glGenTextures( 1, &c.colorTex );
		glBindTexture( GL_TEXTURE_2D, c.colorTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, kCloudW, kCloudN / kCloudW, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		Log( "CLOUD %c: %d points on %s, %.0f ms (worker)", "ABCD"[ slot ], kCloudN, model->path.c_str(), c.job->ms );
		c.job.reset();
	}
	if( c.vbo && !c.baked && model->Complete() )
	{
		// the bake: one point per texel, a draw per part with its base colour map
		GLint prevFBO = 0, vp[ 4 ];
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFBO );
		glGetIntegerv( GL_VIEWPORT, vp );
		const GLboolean depth = glIsEnabled( GL_DEPTH_TEST ), blend = glIsEnabled( GL_BLEND ), scissor = glIsEnabled( GL_SCISSOR_TEST );
		glDisable( GL_DEPTH_TEST ), glDisable( GL_BLEND ), glDisable( GL_SCISSOR_TEST );
		glBindFramebuffer( GL_FRAMEBUFFER, cloudBakeFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.colorTex, 0 );
		glViewport( 0, 0, kCloudW, kCloudN / kCloudW );
		glClearColor( 1, 1, 1, 1 );
		glClear( GL_COLOR_BUFFER_BIT );
		glClearColor( 0, 0, 0, 0 );
		GLuint vao = 0;
		glGenVertexArrays( 1, &vao );
		glBindVertexArray( vao );
		glBindBuffer( GL_ARRAY_BUFFER, c.vbo );
		glEnableVertexAttribArray( 2 );
		glVertexAttribPointer( 2, 2, GL_FLOAT, GL_FALSE, kCloudFloats * sizeof( float ), (void*)( 6 * sizeof( float ) ) );
		glEnableVertexAttribArray( 3 );
		glVertexAttribPointer( 3, 1, GL_FLOAT, GL_FALSE, kCloudFloats * sizeof( float ), (void*)( 8 * sizeof( float ) ) );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glUseProgram( progCloudBake );
		glUniform1f( U( progCloudBake, "uRows" ), float( kCloudN / kCloudW ) );
		glEnable( GL_PROGRAM_POINT_SIZE );
		glActiveTexture( GL_TEXTURE0 );
		for( size_t i = 0; i < model->parts.size(); ++i )
		{
			const GpuPart& part = model->parts[ i ];
			if( part.fractured )
				continue;
			glBindTexture( GL_TEXTURE_2D, part.tex[ CH_BASECOLOR ] ? part.tex[ CH_BASECOLOR ] : whiteTex );
			glUniform1i( U( progCloudBake, "uHasBase" ), part.tex[ CH_BASECOLOR ] ? 1 : 0 );
			glUniform4fv( U( progCloudBake, "uBaseFactor" ), 1, part.baseColor );
			glUniform1f( U( progCloudBake, "uPart" ), float( i ) );
			glDrawArrays( GL_POINTS, 0, kCloudN );
		}
		glDisable( GL_PROGRAM_POINT_SIZE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBindVertexArray( 0 );
		glDeleteVertexArrays( 1, &vao );
		glBindFramebuffer( GL_FRAMEBUFFER, (GLuint)prevFBO );
		glViewport( vp[ 0 ], vp[ 1 ], vp[ 2 ], vp[ 3 ] );
		if( depth )
			glEnable( GL_DEPTH_TEST );
		if( blend )
			glEnable( GL_BLEND );
		if( scissor )
			glEnable( GL_SCISSOR_TEST );
		c.baked = true;
	}
	return c.baked;
}

// what the hop's style needs on the GPU for this pair (until it is there, the hop dissolves: never a wait)
bool Renderer::MorphPairReady( const FrameState& fs, GpuModel* a, GpuModel* b )
{
	switch( fs.transitionStyle )
	{
	case kStylePieces:
	{
		ShapeState ks;
		ks.shardCount = fs.shape.shardCount, ks.shatterMode = fs.shape.shatterMode;
		const ShardSlot &sa = shards[ sideSlot[ 0 ] ], &sb = shards[ sideSlot[ 1 ] ];
		return sa.key == ShardKey( a, ks ) && sb.key == ShardKey( b, ks ) && sa.rankTex && sb.rankTex;
	}
	case kStylePoints:
	case kStyleSwarm:
	{
		const bool ra = UpdateCloud( sideSlot[ 0 ], a ), rb = UpdateCloud( sideSlot[ 1 ], b );
		return ra && rb;
	}
	default: return true;
	}
}

// the MORPH programs' uniforms for this side: its own state + the pair (the other model's matrix, fit, SDF, shards, ranks)
void Renderer::SetMorphUniforms( GLuint p, const FrameState& fs, GpuModel* model )
{
	const MorphState& m = fs.morph;
	glUniform1i( U( p, "uMorphVS" ), m.vs );
	glUniform1i( U( p, "uMorphMask" ), m.mask );
	glUniform1i( U( p, "uMorphSide" ), m.side );
	glUniform1f( U( p, "uMorphK" ), m.k );
	glUniform1f( U( p, "uMorphE" ), m.e );
	glUniform1f( U( p, "uMorphStagger" ), m.stagger );
	glUniform1f( U( p, "uMorphSpread" ), m.spread );
	glUniform1f( U( p, "uMorphScale" ), m.scale );
	glUniform1f( U( p, "uMorphStep" ), m.step );
	glUniform1f( U( p, "uMorphFreq" ), m.freq );
	glUniform1f( U( p, "uMorphBand" ), m.band );
	glUniform1f( U( p, "uMorphGlow" ), m.glow );
	glUniform1f( U( p, "uMorphTemp" ), m.temp );
	glUniform1f( U( p, "uMorphFlame" ), m.flame );
	glUniform3fv( U( p, "uMorphColor" ), 1, m.color );
	glUniform3fv( U( p, "uMorphSeed" ), 1, m.seed );
	glUniform1f( U( p, "uMorphLead" ), m.lead );
	if( model )
	{
		const float* bx = model->geometry ? model->geometry->fitBox : nullptr;
		const int ax    = std::min( 2, std::max( 0, fs.shape.deformAxis ) );
		glUniform2f( U( p, "uMorphRange" ), bx ? bx[ ax ] : -1.0f, bx ? bx[ 3 + ax ] : 1.0f );
		// W7 melt: the floor plane (world y = Floor Height) in this model's fitted space: world y = a . f + b
		const float s = 1.0f / std::max( model->fitScale, 1e-12f );
		float a[ 3 ], b = fs.model[ 13 ], l = 0;
		for( int k = 0; k < 3; ++k )
			a[ k ] = fs.model[ k * 4 + 1 ] * s, b += fs.model[ k * 4 + 1 ] * model->fitCenter[ k ], l += a[ k ] * a[ k ];
		l = std::max( std::sqrt( l ), 1e-12f );
		glUniform4f( U( p, "uMorphPlane" ), a[ 0 ] / l, a[ 1 ] / l, a[ 2 ] / l, ( fs.post.floorY - b ) / l );
	}
	const int other        = m.side == 1 ? 1 : 0;
	GpuModel* om           = pairOn ? pairModel[ other ] : nullptr;
	const FrameState* ofs  = pairOn ? pairFs[ other ] : nullptr;
	GLuint pdata = dummyShardData, rself = dummyShardData, rother = dummyShardData;
	if( om && ofs && model )
	{
		// this model's fitted space -> raw -> world (its matrix) -> the other model's raw space (the inverse of its matrix)
		Mat4 Ms, Mo, F;
		std::memcpy( Ms.m, fs.model, sizeof( Ms.m ) );
		std::memcpy( Mo.m, ofs->model, sizeof( Mo.m ) );
		const float s = 1.0f / std::max( model->fitScale, 1e-12f );
		const float fm[ 16 ] = { s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, model->fitCenter[ 0 ], model->fitCenter[ 1 ], model->fitCenter[ 2 ], 1 };
		std::memcpy( F.m, fm, sizeof( fm ) );
		const Mat4 to = Mo.Inverse() * Ms * F;
		glUniformMatrix4fv( U( p, "uMorphFromOther" ), 1, GL_FALSE, to.Inverse().m );
		glUniform4f( U( p, "uOtherFit" ), om->fitCenter[ 0 ], om->fitCenter[ 1 ], om->fitCenter[ 2 ], om->fitScale );
		const int os = sideSlot[ other ];
		const ShardSlot &so = shards[ os ], &ss = shards[ SlotOf( model ) ];
		if( so.rankTex && ss.rankTex )
		{
			// MoveEhr W2 A3: a posed other side's pieces fly to where its partner shards are now (its posed pivots)
			pdata = so.pivotOk && so.pivotTex && so.pivotCount == so.count ? so.pivotTex : so.dataTex, rself = ss.rankTex, rother = so.rankTex;
			glUniform1f( U( p, "uPartnerN" ), float( std::max( 1, so.count ) ) );
		}
	}
	glActiveTexture( GL_TEXTURE0 + 28 );
	glBindTexture( GL_TEXTURE_BUFFER, pdata );
	glActiveTexture( GL_TEXTURE0 + 29 );
	glBindTexture( GL_TEXTURE_BUFFER, rself );
	glActiveTexture( GL_TEXTURE0 + 30 );
	glBindTexture( GL_TEXTURE_BUFFER, rother );
	glActiveTexture( GL_TEXTURE0 );
}

// the point cloud of the hop (styles 5 / 17): glow = false: lit dots over the scene (Points; depth tested, no depth write);
// glow = true: the light (Points with Wire Glow, Swarm always) added onto the composite at alpha 0
void Renderer::DrawCloud( const FrameState& fsA, const FrameState& fsB, int w, int h, bool glow )
{
	const MorphState& m = fsA.morph;
	const CloudSlot &ca = cloud[ sideSlot[ 0 ] ], &cb = cloud[ sideSlot[ 1 ] ];
	if( m.cloud <= 0.0f || !ca.baked || !cb.baked || !pairModel[ 0 ] || !pairModel[ 1 ] )
		return;
	// MoveEhr W1: an animated side's cloud sits on its posed surface (the refs read through its posed vertices, this frame)
	bool posed[ 2 ] = { false, false };
	{
		const int keep = activeSide;
		for( int s = 0; s < 2; ++s )
		{
			const CloudSlot& c = s == 0 ? ca : cb;
			activeSide         = s;
			posed[ s ]         = c.refBuf && ( s == 0 || sideSlot[ 1 ] != sideSlot[ 0 ] || posed[ 0 ] ) && PointsFromRefs( pairModel[ s ], c.refBuf, c.posedBuf, kCloudN );
		}
		activeSide = keep;
	}
	if( !cloudVAO )
		glGenVertexArrays( 1, &cloudVAO );
	glBindVertexArray( cloudVAO );
	// the light pass draws every 4th point (a quarter of the cloud is plenty of fireflies; 65k overlapping halos only blow out)
	const int stride = glow ? 4 : 1;
	for( int s = 0; s < 2; ++s )
	{
		const CloudSlot& c = s == 0 ? ca : cb;
		const int floats   = posed[ s ] ? 6 : kCloudFloats;
		glBindBuffer( GL_ARRAY_BUFFER, posed[ s ] ? c.posedBuf : c.vbo );
		glEnableVertexAttribArray( s * 2 );
		glVertexAttribPointer( s * 2, 3, GL_FLOAT, GL_FALSE, stride * floats * sizeof( float ), (void*)0 );
		glEnableVertexAttribArray( s * 2 + 1 );
		glVertexAttribPointer( s * 2 + 1, 3, GL_FLOAT, GL_FALSE, stride * floats * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
	}
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	Mat4 V, P;
	std::memcpy( V.m, fsA.view, sizeof( V.m ) );
	std::memcpy( P.m, fsA.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	const GLuint p = progCloud;
	glUseProgram( p );
	glUniformMatrix4fv( U( p, "uModelA" ), 1, GL_FALSE, fsA.model );
	glUniformMatrix4fv( U( p, "uModelB" ), 1, GL_FALSE, fsB.model );
	glUniformMatrix4fv( U( p, "uViewProj" ), 1, GL_FALSE, VP.m );
	glUniform1f( U( p, "uK" ), m.cloudK );
	glUniform1f( U( p, "uStagger" ), m.stagger );
	glUniform1f( U( p, "uEase" ), fsA.transEase );
	glUniform1f( U( p, "uSpread" ), m.spread );
	glUniform1f( U( p, "uUnit" ), fsA.modelScale / std::max( pairModel[ 0 ]->fitScale, 1e-12f ) );
	glUniform1f( U( p, "uN" ), float( kCloudN / stride ) );
	glUniform1i( U( p, "uStride" ), stride );
	glUniform3f( U( p, "uCenter" ), fsA.model[ 12 ], fsA.model[ 13 ], fsA.model[ 14 ] );
	glUniform1i( U( p, "uSwarm" ), m.swarm ? 1 : 0 );
	glUniform1f( U( p, "uFade" ), m.cloud );
	glUniform1f( U( p, "uTwinkle" ), m.cloudTwinkle );
	glUniform1f( U( p, "uPhase" ), m.cloudPhase );
	glUniform3fv( U( p, "uKeyDir" ), 1, fsA.keyDir );
	glUniform3fv( U( p, "uKeyColor" ), 1, fsA.keyColor );
	glUniform1f( U( p, "uAmbient" ), fsA.ambient );
	glUniform3fv( U( p, "uTint" ), 1, m.cloudColor );
	glUniform1f( U( p, "uTintMix" ), m.cloudColorMix );
	glUniform1f( U( p, "uExposure" ), fsA.exposure );
	glUniform1i( U( p, "uTonemap" ), fsA.tonemap );
	glUniform1i( U( p, "uGlow" ), glow ? 1 : 0 );
	const float px = glow ? m.cloudPx * 3.0f : m.cloudPx;
	glUniform1f( U( p, "uPx" ), px );
	glUniform1f( U( p, "uGain" ), m.cloudGlow );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, cb.colorTex );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, ca.colorTex );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glEnable( GL_BLEND );
	if( glow )
	{
		glViewport( 0, 0, w, h );
		glDisable( GL_DEPTH_TEST );
		glBlendFunc( GL_ONE, GL_ONE ); // light: rgb added, alpha untouched (W23)
	}
	else
	{
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LEQUAL );
		glDepthMask( GL_FALSE );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	}
	glDrawArrays( GL_POINTS, 0, kCloudN / stride );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LESS );
	glDisable( GL_PROGRAM_POINT_SIZE );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	if( glow )
		glDisable( GL_DEPTH_TEST );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindVertexArray( 0 );
}

// the model-swap prewarm (1-pixel scissor, set by the caller): the MORPH programs (plain, instanced, deformed, wired, pointed, shadow)
// and the cloud program (both looks, through one point of a stand-in buffer)
void Renderer::WarmMorph( const FrameState& fs, GpuModel* model )
{
	FrameState d    = fs;
	d.shape.on      = true;
	d.morph.vs      = 7;
	d.morph.mask    = 1;
	d.morph.side    = 1;
	std::vector< Instance > one( 1 );
	std::memcpy( one[ 0 ].m, fs.model, sizeof( fs.model ) );
	DrawModel( progSoupM, d, model, false, true, 3 );
	DrawModel( progSoupM, d, model, false, true, 0, nullptr, nullptr, &one );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	DrawModel( progSoupM, d, model, false, true, 3 );
	d.wire.on = true;
	DrawModel( progSoupM, d, model, false, true, 3 );
	pointsNow = warmingNow = true;
	DrawModel( progSoupM, d, model, false, true, 3 );
	pointsNow = warmingNow = false;
	d.wire.on    = false;
	glDisable( GL_BLEND );
	d.shape.deform = true;
	DrawModel( progSoupM, d, model, false, true, 3 );
	d.shape.deform = false;
	const float lp[ 3 ] = { 0, 0, 5 };
	DrawModel( progShadowSoupM, d, model, false, true, 3, nullptr, lp );
	if( !cloudWarmVAO )
	{
		const float pt[ 6 ] = { 0, 0, 0, 0, 1, 0 };
		glGenVertexArrays( 1, &cloudWarmVAO );
		glGenBuffers( 1, &cloudWarmVBO );
		glBindVertexArray( cloudWarmVAO );
		glBindBuffer( GL_ARRAY_BUFFER, cloudWarmVBO );
		glBufferData( GL_ARRAY_BUFFER, sizeof( pt ), pt, GL_STATIC_DRAW );
		for( int l = 0; l < 4; ++l )
		{
			glEnableVertexAttribArray( l );
			glVertexAttribPointer( l, 3, GL_FLOAT, GL_FALSE, 0, (void*)0 );
		}
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
	}
	glUseProgram( progCloud );
	glBindVertexArray( cloudWarmVAO );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, whiteTex );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, whiteTex );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glEnable( GL_BLEND );
	for( int g = 0; g < 2; ++g )
	{
		glBlendFunc( GL_ONE, g ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA );
		glUniform1i( U( progCloud, "uGlow" ), g );
		glDrawArrays( GL_POINTS, 0, 1 );
	}
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	glDisable( GL_PROGRAM_POINT_SIZE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindVertexArray( 0 );
}
}
