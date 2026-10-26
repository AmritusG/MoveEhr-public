// MoveEhr W1: the pose pass. One transform-feedback draw per pair side whose slot has a pose: the static vertex (locations 0-3) +
// the AnimVertex (4-9; W2: 8 influences) in, the posed 28-byte Vertex out (7 uints, the same layout), into a buffer owned per slot (the same file in
// two slots is ONE GpuModel; the pose is the slot's). The renderer then reads that buffer at its three binding sites (EnsureVAO,
// LodVAO, the soup stage's vertex TBO), so every pass (indexed, soup, wire, points, capture, shadows, caustics, LODs) draws the
// posed model with no shader edit. The ANIM twin of a program reads the object-space position (patterns, dissolve) from the REST
// vertex instead (location 4 / unit 38), so patterns stay on the moving surface. No pose = no pass: a static file draws as before.
#include "Renderer.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mehr
{
GLuint LinkPlainSrc( const std::string& vs, const std::string& fs, std::string& error, bool capture ); // Renderer.cpp
void CopySamplerUnitsExt( GLuint from, GLuint to );                                                   // Renderer.cpp

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

const char* kPoseVS = R"GLSL(#version 410 core
layout( location = 0 ) in vec3 aPos;
layout( location = 1 ) in vec2 aUV;
layout( location = 2 ) in vec4 aNormal;
layout( location = 3 ) in vec4 aTangent;
layout( location = 4 ) in vec3 aSrc;     // p_src: the vertex in its joints' input space
layout( location = 5 ) in uvec4 aJoint;  // palette joints
layout( location = 6 ) in vec4 aWeight;  // weights (sum 1)
layout( location = 7 ) in uvec4 aMorph;  // delta base, stride, target count, weight base
layout( location = 8 ) in uvec4 aJoint2; // W2: influences 5-8 (0 weight = unused)
layout( location = 9 ) in vec4 aWeight2;
uniform samplerBuffer uPalette; // per joint 6 texels: J rows 0..2, D rows 0..2 (D = J x J_rest^-1, 3x3 in xyz)
uniform samplerBuffer uDeltas;  // per (target, vertex) 2 texels: position delta, normal delta (source space)
uniform samplerBuffer uWeights; // morph weights
flat out uvec4 oA;
flat out uvec3 oB;
uint Pack1010102( vec3 v, float w )
{
	ivec3 q = ivec3( round( clamp( v, -1.0, 1.0 ) * 511.0 ) );
	uint wq = w < 0.0 ? 3u : 1u;
	return ( uint( q.x ) & 1023u ) | ( ( uint( q.y ) & 1023u ) << 10 ) | ( ( uint( q.z ) & 1023u ) << 20 ) | ( wq << 30 );
}
void main()
{
	vec3 p  = aSrc;
	vec3 dn = vec3( 0.0 );
	for( uint k = 0u; k < aMorph.z; ++k )
	{
		float w = texelFetch( uWeights, int( aMorph.w + k ) ).r;
		if( w != 0.0 )
		{
			int b = int( aMorph.x + k * aMorph.y ) * 2;
			p += w * texelFetch( uDeltas, b ).xyz;
			dn += w * texelFetch( uDeltas, b + 1 ).xyz;
		}
	}
	vec3 o = vec3( 0.0 ), n = vec3( 0.0 ), t = vec3( 0.0 ), mn = vec3( 0.0 );
	vec3 nb = aNormal.xyz, tb = aTangent.xyz;
	for( int i = 0; i < 8; ++i )
	{
		float w = i < 4 ? aWeight[ i ] : aWeight2[ i - 4 ];
		if( w == 0.0 )
			continue;
		int b   = int( i < 4 ? aJoint[ i ] : aJoint2[ i - 4 ] ) * 6;
		vec4 r0 = texelFetch( uPalette, b ), r1 = texelFetch( uPalette, b + 1 ), r2 = texelFetch( uPalette, b + 2 );
		vec3 d0 = texelFetch( uPalette, b + 3 ).xyz, d1 = texelFetch( uPalette, b + 4 ).xyz, d2 = texelFetch( uPalette, b + 5 ).xyz;
		vec4 hp = vec4( p, 1.0 );
		o += w * vec3( dot( r0, hp ), dot( r1, hp ), dot( r2, hp ) );
		n += w * vec3( dot( d0, nb ), dot( d1, nb ), dot( d2, nb ) );
		t += w * vec3( dot( d0, tb ), dot( d1, tb ), dot( d2, tb ) );
		mn += w * vec3( dot( r0.xyz, dn ), dot( r1.xyz, dn ), dot( r2.xyz, dn ) );
	}
	n  = normalize( n + mn + vec3( 0.0, 1e-12, 0.0 ) );
	t  = normalize( t + vec3( 1e-12, 0.0, 0.0 ) );
	oA = uvec4( floatBitsToUint( o ), floatBitsToUint( aUV.x ) );
	oB = uvec3( floatBitsToUint( aUV.y ), Pack1010102( n, 1.0 ), Pack1010102( t, aTangent.w ) );
}
)GLSL";
// MoveEhr W1: points on the surface as (triangle, barycentric) refs -> the posed position + face normal, from the posed vertex TBO
const char* kRefsVS = R"GLSL(#version 410 core
layout( location = 0 ) in uint aTri;
layout( location = 1 ) in vec2 aBary; // b, c (a = 1 - b - c)
uniform usamplerBuffer uIdxBuf;
uniform usamplerBuffer uVtxBuf;
out vec3 oPos;
out vec3 oNrm;
vec3 P( int v )
{
	int b = v * 7;
	return vec3( uintBitsToFloat( texelFetch( uVtxBuf, b ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 1 ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 2 ).r ) );
}
void main()
{
	int t   = int( aTri ) * 3;
	vec3 p0 = P( int( texelFetch( uIdxBuf, t ).r ) ), p1 = P( int( texelFetch( uIdxBuf, t + 1 ).r ) ), p2 = P( int( texelFetch( uIdxBuf, t + 2 ).r ) );
	oPos    = p0 * ( 1.0 - aBary.x - aBary.y ) + p1 * aBary.x + p2 * aBary.y;
	vec3 fn = cross( p1 - p0, p2 - p0 );
	oNrm    = dot( fn, fn ) > 0.0 ? normalize( fn ) : vec3( 0.0, 1.0, 0.0 );
}
)GLSL";
// MoveEhr W2 A3: posed shard pivots, two passes, one vertex per chunk / per shard (no blending: the sums run in a fixed order, so the
// same pose gives the same bits). Pass 1: a chunk of <= 256 triangles of one shard -> sum of area x centroid (raw posed space), area.
const char* kPivotAVS = R"GLSL(#version 410 core
uniform usamplerBuffer uIdxBuf;  // unit 9
uniform usamplerBuffer uVtxBuf;  // unit 10: the posed vertices (7 words each)
uniform usamplerBuffer uTris;    // unit 40: triangles sorted by shard
uniform usamplerBuffer uChunks;  // unit 41: (begin, end) into uTris
out vec4 oSum;
vec3 P( int v )
{
	int b = v * 7;
	return vec3( uintBitsToFloat( texelFetch( uVtxBuf, b ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 1 ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 2 ).r ) );
}
void main()
{
	uvec2 c = texelFetch( uChunks, gl_VertexID ).xy;
	vec3 s  = vec3( 0.0 );
	float a = 0.0;
	for( uint i = c.x; i < c.y; ++i )
	{
		int t   = int( texelFetch( uTris, int( i ) ).r ) * 3;
		vec3 p0 = P( int( texelFetch( uIdxBuf, t ).r ) ), p1 = P( int( texelFetch( uIdxBuf, t + 1 ).r ) ), p2 = P( int( texelFetch( uIdxBuf, t + 2 ).r ) );
		float w = length( cross( p1 - p0, p2 - p0 ) ) + 1e-30;
		s += w * ( p0 + p1 + p2 ) * ( 1.0 / 3.0 );
		a += w;
	}
	oSum = vec4( s, a );
}
)GLSL";
// Pass 2: a shard's chunks -> its posed centroid in fitted space (the shard data layout: xyz + the seed in w)
const char* kPivotBVS = R"GLSL(#version 410 core
uniform samplerBuffer uParts;     // unit 42: pass 1's sums
uniform usamplerBuffer uRanges;   // unit 43: per shard (first chunk, end chunk)
uniform samplerBuffer uShardData; // unit 44: the bind shard data (seed; the fallback for a shard with no triangles)
uniform vec3 uFitCenter;
uniform float uFitScale;
out vec4 oPivot;
void main()
{
	uvec2 r  = texelFetch( uRanges, gl_VertexID ).xy;
	vec4 acc = vec4( 0.0 );
	for( uint i = r.x; i < r.y; ++i )
		acc += texelFetch( uParts, int( i ) );
	vec4 sd = texelFetch( uShardData, gl_VertexID );
	oPivot  = acc.w > 0.0 ? vec4( ( acc.xyz / acc.w - uFitCenter ) * uFitScale, sd.w ) : sd;
}
)GLSL";
const char* kPoseFS = "#version 410 core\nout vec4 fragColor;\nvoid main() { fragColor = vec4( 0.0 ); }\n";

void AttachStatic( GLuint vbo )
{
	const GLsizei stride = sizeof( Vertex );
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glEnableVertexAttribArray( 0 );
	glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof( Vertex, px ) );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof( Vertex, u ) );
	glEnableVertexAttribArray( 2 );
	glVertexAttribPointer( 2, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride, (void*)offsetof( Vertex, normal ) );
	glEnableVertexAttribArray( 3 );
	glVertexAttribPointer( 3, 4, GL_INT_2_10_10_10_REV, GL_TRUE, stride, (void*)offsetof( Vertex, tangent ) );
}
void UploadTbo( GLuint& buf, GLuint& tex, GLenum format, const void* data, size_t bytes )
{
	if( !buf )
		glGenBuffers( 1, &buf ), glGenTextures( 1, &tex );
	glBindBuffer( GL_TEXTURE_BUFFER, buf );
	glBufferData( GL_TEXTURE_BUFFER, bytes, data, GL_STREAM_DRAW );
	glBindTexture( GL_TEXTURE_BUFFER, tex );
	glTexBuffer( GL_TEXTURE_BUFFER, format, buf ); // re-attach after glBufferData (Apple GL keeps the old storage otherwise)
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
}
}

bool Renderer::InitPose( std::string& error )
{
	if( progPose )
		return true;
	GLuint v = glCreateShader( GL_VERTEX_SHADER ), f = glCreateShader( GL_FRAGMENT_SHADER );
	glShaderSource( v, 1, &kPoseVS, nullptr );
	glShaderSource( f, 1, &kPoseFS, nullptr );
	glCompileShader( v );
	glCompileShader( f );
	GLint ok = 0;
	glGetShaderiv( v, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[ 2048 ];
		glGetShaderInfoLog( v, sizeof( log ), nullptr, log );
		error = std::string( "pose pass compile failed: " ) + log;
		Log( "%s", error.c_str() );
		glDeleteShader( v ), glDeleteShader( f );
		return false;
	}
	progPose = glCreateProgram();
	glAttachShader( progPose, v );
	glAttachShader( progPose, f );
	const char* vary[ 2 ] = { "oA", "oB" };
	glTransformFeedbackVaryings( progPose, 2, vary, GL_INTERLEAVED_ATTRIBS );
	glLinkProgram( progPose );
	glDeleteShader( v ), glDeleteShader( f );
	glGetProgramiv( progPose, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[ 2048 ];
		glGetProgramInfoLog( progPose, sizeof( log ), nullptr, log );
		error = std::string( "pose pass link failed: " ) + log;
		Log( "%s", error.c_str() );
		glDeleteProgram( progPose ), progPose = 0;
		return false;
	}
	glUseProgram( progPose );
	glUniform1i( U( progPose, "uPalette" ), 35 );
	glUniform1i( U( progPose, "uDeltas" ), 36 );
	glUniform1i( U( progPose, "uWeights" ), 37 );
	glUseProgram( 0 );
	GLint maxTbo = 0;
	glGetIntegerv( GL_MAX_TEXTURE_BUFFER_SIZE, &maxTbo );
	Log( "POSE pass ready (GL_MAX_TEXTURE_BUFFER_SIZE %d texels = %d joints)", maxTbo, maxTbo / 6 );
	return true;
}

void Renderer::SetSlotPose( int slot, GpuModel* model, const std::vector< float >& palette, const std::vector< float >& weights )
{
	if( slot < 0 || slot >= kSlots || !model )
		return;
	SlotPose& sp = pose[ slot ];
	if( sp.serial != model->serial )
		sp.posed = false;
	sp.active  = true;
	sp.serial  = model->serial;
	sp.palette = palette;
	sp.weights = weights;
}

void Renderer::SetSlotTrail( int slot, const std::vector< float >& palette, const std::vector< float >& weights )
{
	if( slot < 0 || slot >= kSlots )
		return;
	SlotPose& sp    = pose[ slot ];
	sp.trailWant    = true;
	sp.trailPalette = palette;
	sp.trailWeights = weights;
}

void Renderer::ClearSlotTrail( int slot )
{
	if( slot >= 0 && slot < kSlots )
		pose[ slot ].trailWant = false, pose[ slot ].trailPosed = false;
}

void Renderer::ClearSlotPose( int slot )
{
	if( slot >= 0 && slot < kSlots )
		pose[ slot ].active = false, pose[ slot ].posed = false;
}

const Renderer::SlotPose* Renderer::PosedFor( const GpuModel* model ) const
{
	const int slot = sideSlot[ activeSide ];
	if( !model || slot < 0 || slot >= kSlots )
		return nullptr;
	const SlotPose& sp = pose[ slot ];
	return sp.active && sp.posed && sp.serial == model->serial && sp.vbo ? &sp : nullptr;
}

void Renderer::RunPosePasses( GpuModel* a, GpuModel* b )
{
	const auto t0 = std::chrono::steady_clock::now();
	posePasses    = 0;
	for( DustSlot& d : dust )
		d.posedNow = false; // set again by this frame's dust step on a posed side (a stale posed origin is never drawn)
	GpuModel* side[ 2 ] = { a, b };
	for( int s = 0; s < 2; ++s )
	{
		const int slot = sideSlot[ s ];
		GpuModel* m    = side[ s ];
		if( slot < 0 || slot >= kSlots || ( s == 1 && sideSlot[ 1 ] == sideSlot[ 0 ] ) )
			continue;
		SlotPose& sp = pose[ slot ];
		if( !sp.active || !m || sp.serial != m->serial || !m->Complete() || !m->animVbo || !m->anim )
		{
			sp.posed = false;
			continue;
		}
		std::string err;
		if( !progPose && !InitPose( err ) )
			return;
		const size_t verts = m->anim->verts.size();
		if( !sp.vbo || sp.verts != verts )
		{
			if( !sp.vbo )
				glGenBuffers( 1, &sp.vbo ), glGenTextures( 1, &sp.tbo );
			glBindBuffer( GL_ARRAY_BUFFER, sp.vbo );
			glBufferData( GL_ARRAY_BUFFER, verts * sizeof( Vertex ), nullptr, GL_DYNAMIC_COPY );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );
			glBindTexture( GL_TEXTURE_BUFFER, sp.tbo );
			glTexBuffer( GL_TEXTURE_BUFFER, GL_R32UI, sp.vbo ); // re-attach after glBufferData (Apple GL)
			glBindTexture( GL_TEXTURE_BUFFER, 0 );
			sp.verts = verts, sp.posed = false;
		}
		if( !sp.inVao || sp.inFor != m->serial )
		{
			if( !sp.inVao )
				glGenVertexArrays( 1, &sp.inVao );
			glBindVertexArray( sp.inVao );
			AttachStatic( m->vbo );
			glBindBuffer( GL_ARRAY_BUFFER, m->animVbo );
			const GLsizei st = sizeof( AnimVertex );
			glEnableVertexAttribArray( 4 );
			glVertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, st, (void*)offsetof( AnimVertex, p ) );
			glEnableVertexAttribArray( 5 );
			glVertexAttribIPointer( 5, 4, GL_UNSIGNED_SHORT, st, (void*)offsetof( AnimVertex, j ) );
			glEnableVertexAttribArray( 6 );
			glVertexAttribPointer( 6, 4, GL_FLOAT, GL_FALSE, st, (void*)offsetof( AnimVertex, w ) );
			glEnableVertexAttribArray( 7 );
			glVertexAttribIPointer( 7, 4, GL_UNSIGNED_INT, st, (void*)offsetof( AnimVertex, morph ) );
			glEnableVertexAttribArray( 8 );
			glVertexAttribIPointer( 8, 4, GL_UNSIGNED_SHORT, st, (void*)( offsetof( AnimVertex, j ) + 4 * sizeof( uint16_t ) ) );
			glEnableVertexAttribArray( 9 );
			glVertexAttribPointer( 9, 4, GL_FLOAT, GL_FALSE, st, (void*)( offsetof( AnimVertex, w ) + 4 * sizeof( float ) ) );
			glBindVertexArray( 0 );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );
			sp.inFor = m->serial;
		}
		// one pose pass: palette + morph weights -> outVbo (transform feedback)
		auto pass = [ & ]( const std::vector< float >& pal, const std::vector< float >& wts, GLuint& palBuf, GLuint& palTex, GLuint& wBuf, GLuint& wTex, GLuint outVbo ) {
			const float none = 0.0f;
			UploadTbo( palBuf, palTex, GL_RGBA32F, pal.data(), pal.size() * sizeof( float ) );
			UploadTbo( wBuf, wTex, GL_R32F, wts.empty() ? (const void*)&none : (const void*)wts.data(), std::max< size_t >( 1, wts.size() ) * sizeof( float ) );
			glUseProgram( progPose );
			glActiveTexture( GL_TEXTURE0 + 35 );
			glBindTexture( GL_TEXTURE_BUFFER, palTex );
			glActiveTexture( GL_TEXTURE0 + 36 );
			glBindTexture( GL_TEXTURE_BUFFER, m->morphTBO );
			glActiveTexture( GL_TEXTURE0 + 37 );
			glBindTexture( GL_TEXTURE_BUFFER, wTex );
			glActiveTexture( GL_TEXTURE0 );
			// transform feedback against framebuffer 0 (incomplete in the host's context) fails with 0x506: keep a complete FBO bound
			GLint prevFbo = 0;
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );
			glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
			glEnable( GL_RASTERIZER_DISCARD );
			glBindVertexArray( sp.inVao );
			glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, outVbo );
			glBeginTransformFeedback( GL_POINTS );
			glDrawArrays( GL_POINTS, 0, (GLsizei)verts );
			glEndTransformFeedback();
			glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
			glDisable( GL_RASTERIZER_DISCARD );
			glBindVertexArray( 0 );
			glBindFramebuffer( GL_FRAMEBUFFER, prevFbo );
		};
		// W4 Trail Off: the earlier pose, into its own buffer (unchanged = kept)
		if( sp.trailWant )
		{
			if( !sp.trailVbo || sp.trailPosed == false || sp.trailLast != sp.trailPalette || sp.trailLastW != sp.trailWeights )
			{
				if( !sp.trailVbo )
				{
					glGenBuffers( 1, &sp.trailVbo ), glGenTextures( 1, &sp.trailTbo );
					sp.trailPosed = false;
				}
				GLint have = 0;
				glBindBuffer( GL_ARRAY_BUFFER, sp.trailVbo );
				glGetBufferParameteriv( GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &have );
				if( size_t( have ) != verts * sizeof( Vertex ) )
				{
					glBufferData( GL_ARRAY_BUFFER, verts * sizeof( Vertex ), nullptr, GL_DYNAMIC_COPY );
					glBindTexture( GL_TEXTURE_BUFFER, sp.trailTbo );
					glTexBuffer( GL_TEXTURE_BUFFER, GL_R32UI, sp.trailVbo ); // re-attach after glBufferData (Apple GL)
					glBindTexture( GL_TEXTURE_BUFFER, 0 );
				}
				glBindBuffer( GL_ARRAY_BUFFER, 0 );
				pass( sp.trailPalette, sp.trailWeights, sp.trailPalBuf, sp.trailPalTex, sp.trailWBuf, sp.trailWTex, sp.trailVbo );
				sp.trailPosed = true;
				sp.trailLast  = sp.trailPalette;
				sp.trailLastW = sp.trailWeights;
			}
		}
		// unchanged pose (paused, held, a still clip) = the buffer already holds it
		if( sp.posed && sp.palette == sp.lastPalette && sp.weights == sp.lastWeights )
			continue;
		pass( sp.palette, sp.weights, sp.palBuf, sp.palTex, sp.wBuf, sp.wTex, sp.vbo );
		sp.posed       = true;
		sp.gen++;
		sp.lastPalette = sp.palette;
		sp.lastWeights = sp.weights;
		posePasses++;
	}
	poseMs = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}

void Renderer::UploadShardCsr( ShardSlot& sh, const std::vector< uint16_t >& triShard, size_t tris, int count )
{
	tris = std::min( tris, triShard.size() );
	std::vector< uint32_t > start( size_t( count ) + 1, 0 ), sorted( tris );
	for( size_t t = 0; t < tris; ++t )
		if( triShard[ t ] < count )
			start[ triShard[ t ] + 1 ]++;
	for( int s = 0; s < count; ++s )
		start[ s + 1 ] += start[ s ];
	std::vector< uint32_t > fill( start.begin(), start.end() - 1 );
	for( size_t t = 0; t < tris; ++t )
		if( triShard[ t ] < count )
			sorted[ fill[ triShard[ t ] ]++ ] = uint32_t( t );
	sorted.resize( start[ count ] );
	std::vector< uint32_t > chunks, ranges( size_t( count ) * 2 );
	for( int s = 0; s < count; ++s )
	{
		ranges[ s * 2 ] = uint32_t( chunks.size() / 2 );
		for( uint32_t b = start[ s ]; b < start[ s + 1 ]; b += 256 )
			chunks.insert( chunks.end(), { b, std::min( b + 256, start[ s + 1 ] ) } );
		ranges[ s * 2 + 1 ] = uint32_t( chunks.size() / 2 );
	}
	if( chunks.empty() )
		chunks = { 0, 0 };
	if( sorted.empty() )
		sorted = { 0 };
	auto up = [ & ]( GLuint& buf, GLuint& tex, GLenum fmt, const void* d, size_t bytes ) { UploadTbo( buf, tex, fmt, d, bytes ); };
	up( sh.triBuf, sh.triTex, GL_R32UI, sorted.data(), sorted.size() * 4 );
	up( sh.chunkBuf, sh.chunkTex, GL_RG32UI, chunks.data(), chunks.size() * 4 );
	up( sh.rangeBuf, sh.rangeTex, GL_RG32UI, ranges.data(), ranges.size() * 4 );
	sh.chunks = int( chunks.size() / 2 );
	std::vector< float > zero( size_t( sh.chunks ) * 4, 0.0f );
	up( sh.partBuf, sh.partTex, GL_RGBA32F, zero.data(), zero.size() * 4 );
	std::vector< float > piv( size_t( count ) * 4, 0.0f );
	up( sh.pivotBuf, sh.pivotTex, GL_RGBA32F, piv.data(), piv.size() * 4 );
	sh.pivotCount = count, sh.pivotOk = false, sh.pivotGen = 0;
}

namespace
{
GLuint LinkTf( const char* vsSrc, const char* varying, const char* what )
{
	GLuint v = glCreateShader( GL_VERTEX_SHADER ), f = glCreateShader( GL_FRAGMENT_SHADER );
	glShaderSource( v, 1, &vsSrc, nullptr );
	glShaderSource( f, 1, &kPoseFS, nullptr );
	glCompileShader( v ), glCompileShader( f );
	GLuint p = glCreateProgram();
	glAttachShader( p, v ), glAttachShader( p, f );
	glTransformFeedbackVaryings( p, 1, &varying, GL_INTERLEAVED_ATTRIBS );
	glLinkProgram( p );
	glDeleteShader( v ), glDeleteShader( f );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[ 2048 ];
		glGetProgramInfoLog( p, sizeof( log ), nullptr, log );
		Log( "POSE %s link failed: %s", what, log );
		glDeleteProgram( p );
		return 0;
	}
	return p;
}
}

int Renderer::ShardCount( int slot ) const { return slot >= 0 && slot < kSlots ? shards[ slot ].count : 0; }

GLuint Renderer::ShardPivotTex( const GpuModel* model, int slot ) const
{
	if( slot < 0 || slot >= kSlots || !PosedFor( model ) )
		return 0;
	static const bool control = getenv( "MOVEEHR_NO_SHARD_PIVOT" ) != nullptr; // debug control (W1's bind pivots) for the A3 spin gate
	const ShardSlot& sh       = shards[ slot ];
	return !control && sh.pivotOk && sh.pivotTex && sh.pivotCount == sh.count ? sh.pivotTex : 0;
}

void Renderer::RunShardPivots( GpuModel* a, GpuModel* b )
{
	for( ShardSlot& sh : shards )
		sh.pivotOk = false;
	static const bool control = getenv( "MOVEEHR_NO_SHARD_PIVOT" ) != nullptr; // debug control: W1's bind pivots, no pass (A3 gate / cost)
	if( control )
		return;
	GpuModel* side[ 2 ] = { a, b };
	for( int s = 0; s < 2; ++s )
	{
		const int slot = sideSlot[ s ];
		GpuModel* m    = side[ s ];
		if( !m || slot < 0 || slot >= kSlots || !m->anim || !m->idxTBO )
			continue;
		const SlotPose& sp = pose[ slot ];
		ShardSlot& sh      = shards[ slot ];
		if( !sp.active || !sp.posed || sp.serial != m->serial || !sp.tbo || !sh.chunkTex || !sh.dataTex || sh.pivotCount != sh.count || sh.count <= 0 )
			continue;
		if( sh.pivotGen == sp.gen && sh.pivotKey == sh.key && sh.pivotGen != 0 ) // the same pose and shards: the buffer holds it already
		{
			sh.pivotOk = true;
			continue;
		}
		if( !progPivotA )
		{
			progPivotA = LinkTf( kPivotAVS, "oSum", "shard pivot pass 1" );
			progPivotB = LinkTf( kPivotBVS, "oPivot", "shard pivot pass 2" );
			if( !progPivotA || !progPivotB )
				return;
			glUseProgram( progPivotA );
			glUniform1i( U( progPivotA, "uIdxBuf" ), 9 );
			glUniform1i( U( progPivotA, "uVtxBuf" ), 10 );
			glUniform1i( U( progPivotA, "uTris" ), 40 );
			glUniform1i( U( progPivotA, "uChunks" ), 41 );
			glUseProgram( progPivotB );
			glUniform1i( U( progPivotB, "uParts" ), 42 );
			glUniform1i( U( progPivotB, "uRanges" ), 43 );
			glUniform1i( U( progPivotB, "uShardData" ), 44 );
			glUseProgram( 0 );
		}
		if( !pivotVao )
			glGenVertexArrays( 1, &pivotVao );
		const struct
		{
			int unit;
			GLuint tex;
		} binds[] = { { 9, m->idxTBO }, { 10, sp.tbo }, { 40, sh.triTex }, { 41, sh.chunkTex }, { 42, sh.partTex }, { 43, sh.rangeTex }, { 44, sh.dataTex } };
		for( const auto& bd : binds )
		{
			glActiveTexture( GL_TEXTURE0 + bd.unit );
			glBindTexture( GL_TEXTURE_BUFFER, bd.tex );
		}
		glActiveTexture( GL_TEXTURE0 );
		GLint prevFbo = 0;
		glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );
		glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO ); // transform feedback needs a complete FBO bound (0x506 otherwise)
		glEnable( GL_RASTERIZER_DISCARD );
		glBindVertexArray( pivotVao );
		glUseProgram( progPivotA );
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, sh.partBuf );
		glBeginTransformFeedback( GL_POINTS );
		glDrawArrays( GL_POINTS, 0, sh.chunks );
		glEndTransformFeedback();
		glUseProgram( progPivotB );
		glUniform3fv( U( progPivotB, "uFitCenter" ), 1, m->fitCenter );
		glUniform1f( U( progPivotB, "uFitScale" ), m->fitScale );
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, sh.pivotBuf );
		glBeginTransformFeedback( GL_POINTS );
		glDrawArrays( GL_POINTS, 0, sh.count );
		glEndTransformFeedback();
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
		glDisable( GL_RASTERIZER_DISCARD );
		glBindVertexArray( 0 );
		glUseProgram( 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, prevFbo );
		sh.pivotOk = true, sh.pivotGen = sp.gen, sh.pivotKey = sh.key;
	}
}

void Renderer::CheckShardPivots( int slot, const GpuModel* model, double stats[ 6 ] )
{
	for( int k = 0; k < 6; ++k )
		stats[ k ] = -1.0;
	std::vector< float > xyz;
	const ShardSlot& sh = shards[ slot ];
	if( !model || !model->geometry || !sh.pivotOk || !ReadPosed( slot, xyz ) || sh.data.size() != size_t( sh.count ) * 4 )
		return;
	std::vector< float > piv( size_t( sh.count ) * 4 );
	glBindBuffer( GL_ARRAY_BUFFER, sh.pivotBuf );
	glGetBufferSubData( GL_ARRAY_BUFFER, 0, piv.size() * 4, piv.data() );
	std::vector< uint16_t > of( model->geometry->idx.size() / 3 );
	glBindBuffer( GL_ARRAY_BUFFER, sh.ofBuf );
	glGetBufferSubData( GL_ARRAY_BUFFER, 0, of.size() * 2, of.data() );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	// the CPU reference: the area-weighted posed centroid of each shard (double), fitted space
	const std::vector< uint32_t >& idx = model->geometry->idx;
	std::vector< double > sum( size_t( sh.count ) * 4, 0.0 );
	for( size_t t = 0; t < of.size(); ++t )
	{
		const float *a = &xyz[ idx[ t * 3 ] * 3 ], *b = &xyz[ idx[ t * 3 + 1 ] * 3 ], *c = &xyz[ idx[ t * 3 + 2 ] * 3 ];
		double e1[ 3 ], e2[ 3 ];
		for( int k = 0; k < 3; ++k )
			e1[ k ] = b[ k ] - a[ k ], e2[ k ] = c[ k ] - a[ k ];
		const double cx = e1[ 1 ] * e2[ 2 ] - e1[ 2 ] * e2[ 1 ], cy = e1[ 2 ] * e2[ 0 ] - e1[ 0 ] * e2[ 2 ], cz = e1[ 0 ] * e2[ 1 ] - e1[ 1 ] * e2[ 0 ];
		const double w = std::sqrt( cx * cx + cy * cy + cz * cz ) + 1e-30;
		for( int k = 0; k < 3; ++k )
			sum[ of[ t ] * 4 + k ] += w * ( a[ k ] + b[ k ] + c[ k ] ) / 3.0;
		sum[ of[ t ] * 4 + 3 ] += w;
	}
	std::vector< double > dPiv, dBind;
	for( int s = 0; s < sh.count; ++s )
	{
		if( sum[ s * 4 + 3 ] <= 0 )
			continue;
		double e = 0, f = 0;
		for( int k = 0; k < 3; ++k )
		{
			const double ref = ( sum[ s * 4 + k ] / sum[ s * 4 + 3 ] - model->fitCenter[ k ] ) * model->fitScale;
			e += ( piv[ s * 4 + k ] - ref ) * ( piv[ s * 4 + k ] - ref );
			f += ( sh.data[ s * 4 + k ] - ref ) * ( sh.data[ s * 4 + k ] - ref );
		}
		dPiv.push_back( std::sqrt( e ) ), dBind.push_back( std::sqrt( f ) );
	}
	auto q = []( std::vector< double >& v, double p ) { std::sort( v.begin(), v.end() ); return v.empty() ? -1.0 : v[ std::min( v.size() - 1, size_t( p * double( v.size() - 1 ) + 0.5 ) ) ]; };
	stats[ 0 ] = q( dPiv, 0.5 ), stats[ 1 ] = q( dPiv, 0.9 ), stats[ 2 ] = q( dPiv, 1.0 );
	stats[ 3 ] = q( dBind, 0.5 ), stats[ 4 ] = q( dBind, 0.9 ), stats[ 5 ] = q( dBind, 1.0 );
}

bool Renderer::PointsFromRefs( GpuModel* model, GLuint refBuf, GLuint outBuf, int count )
{
	const SlotPose* ps = PosedFor( model );
	if( !ps || !refBuf || !outBuf || count <= 0 )
		return false;
	if( !progRefs )
	{
		GLuint v = glCreateShader( GL_VERTEX_SHADER ), f = glCreateShader( GL_FRAGMENT_SHADER );
		glShaderSource( v, 1, &kRefsVS, nullptr );
		glShaderSource( f, 1, &kPoseFS, nullptr );
		glCompileShader( v ), glCompileShader( f );
		progRefs = glCreateProgram();
		glAttachShader( progRefs, v ), glAttachShader( progRefs, f );
		const char* vary[ 2 ] = { "oPos", "oNrm" };
		glTransformFeedbackVaryings( progRefs, 2, vary, GL_INTERLEAVED_ATTRIBS );
		glLinkProgram( progRefs );
		glDeleteShader( v ), glDeleteShader( f );
		GLint ok = 0;
		glGetProgramiv( progRefs, GL_LINK_STATUS, &ok );
		if( !ok )
		{
			char log[ 2048 ];
			glGetProgramInfoLog( progRefs, sizeof( log ), nullptr, log );
			Log( "POSE refs pass link failed: %s", log );
			glDeleteProgram( progRefs ), progRefs = 0;
			return false;
		}
		glUseProgram( progRefs );
		glUniform1i( U( progRefs, "uIdxBuf" ), 9 );
		glUniform1i( U( progRefs, "uVtxBuf" ), 10 );
	}
	if( !refsVao ) // per Renderer (VAOs are not shared between contexts); rebound every call (the ref buffer differs per slot)
		glGenVertexArrays( 1, &refsVao );
	glBindVertexArray( refsVao );
	glBindBuffer( GL_ARRAY_BUFFER, refBuf );
	glEnableVertexAttribArray( 0 );
	glVertexAttribIPointer( 0, 1, GL_UNSIGNED_INT, 3 * sizeof( float ), (void*)0 );
	glEnableVertexAttribArray( 1 );
	glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof( float ), (void*)sizeof( float ) );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glUseProgram( progRefs );
	glActiveTexture( GL_TEXTURE0 + 9 );
	glBindTexture( GL_TEXTURE_BUFFER, model->idxTBO );
	glActiveTexture( GL_TEXTURE0 + 10 );
	glBindTexture( GL_TEXTURE_BUFFER, ps->tbo );
	glActiveTexture( GL_TEXTURE0 );
	GLint prevFbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO ); // transform feedback needs a complete FBO bound (0x506 otherwise)
	glEnable( GL_RASTERIZER_DISCARD );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, outBuf );
	glBeginTransformFeedback( GL_POINTS );
	glDrawArrays( GL_POINTS, 0, count );
	glEndTransformFeedback();
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	glDisable( GL_RASTERIZER_DISCARD );
	glBindVertexArray( 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, prevFbo );
	return true;
}

bool Renderer::ReadPosed( int slot, std::vector< float >& xyz )
{
	if( slot < 0 || slot >= kSlots || !pose[ slot ].posed || !pose[ slot ].vbo )
		return false;
	const SlotPose& sp = pose[ slot ];
	std::vector< Vertex > v( sp.verts );
	glBindBuffer( GL_ARRAY_BUFFER, sp.vbo );
	glGetBufferSubData( GL_ARRAY_BUFFER, 0, v.size() * sizeof( Vertex ), v.data() );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	xyz.resize( v.size() * 3 );
	for( size_t i = 0; i < v.size(); ++i )
		xyz[ i * 3 ] = v[ i ].px, xyz[ i * 3 + 1 ] = v[ i ].py, xyz[ i * 3 + 2 ] = v[ i ].pz;
	return true;
}

void Renderer::CheckPosedPoints( int slot, const GpuModel* model, double& dustErr, double& cloudErr )
{
	dustErr = cloudErr = -1.0;
	std::vector< float > xyz;
	if( !model || !model->geometry || !ReadPosed( slot, xyz ) )
		return;
	const std::vector< uint32_t >& idx = model->geometry->idx;
	auto check = [ & ]( GLuint refBuf, GLuint posedBuf, int count ) -> double {
		if( !refBuf || !posedBuf || count <= 0 )
			return -1.0;
		std::vector< float > refs( size_t( count ) * 3 ), got( size_t( count ) * 6 );
		glBindBuffer( GL_ARRAY_BUFFER, refBuf );
		glGetBufferSubData( GL_ARRAY_BUFFER, 0, refs.size() * sizeof( float ), refs.data() );
		glBindBuffer( GL_ARRAY_BUFFER, posedBuf );
		glGetBufferSubData( GL_ARRAY_BUFFER, 0, got.size() * sizeof( float ), got.data() );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		double worst = 0;
		for( int i = 0; i < count; ++i )
		{
			uint32_t t;
			std::memcpy( &t, &refs[ size_t( i ) * 3 ], 4 );
			const float b = refs[ size_t( i ) * 3 + 1 ], c = refs[ size_t( i ) * 3 + 2 ];
			if( size_t( t ) * 3 + 2 >= idx.size() )
				return 1e30;
			for( int k = 0; k < 3; ++k )
			{
				const double e = xyz[ idx[ t * 3 ] * 3 + k ] * ( 1.0 - b - c ) + xyz[ idx[ t * 3 + 1 ] * 3 + k ] * b + xyz[ idx[ t * 3 + 2 ] * 3 + k ] * c;
				worst = std::max( worst, std::fabs( e - got[ size_t( i ) * 6 + k ] ) );
			}
		}
		return worst;
	};
	if( dust[ slot ].posedNow )
		dustErr = check( dust[ slot ].refBuf, dust[ slot ].posedBuf, dust[ slot ].count );
	if( cloud[ slot ].refBuf )
		cloudErr = check( cloud[ slot ].refBuf, cloud[ slot ].posedBuf, 65536 );
}

GLuint Renderer::AnimTwin( GLuint p )
{
	auto it = animTwins.find( p );
	if( it != animTwins.end() )
		return it->second;
	auto src    = progSrc.find( p );
	GLuint twin = p; // no sources (not a model program): itself
	static const bool noTwins = getenv( "MOVEEHR_NO_ANIM_TWIN" ) != nullptr; // debug control for the swim gate: patterns follow the posed surface
	if( src != progSrc.end() && !noTwins )
	{
		const auto t0 = std::chrono::steady_clock::now();
		std::string err;
		const GLuint q = LinkPlainSrc( "#define ANIM\n" + *src->second.vs, src->second.capture ? *src->second.fs : "#define ANIM\n" + *src->second.fs, err,
		                               src->second.capture );
		if( q )
		{
			CopySamplerUnitsExt( p, q );
			glUseProgram( q );
			glUniform1i( U( q, "uRestVtx" ), 38 );
			glUniform1i( U( q, "uShardPivot" ), 39 ); // W2 A3 (soup twins only; -1 elsewhere)
			glUseProgram( 0 );
			twin = q;
			Log( "POSE ANIM twin of program %u linked in %.0f ms", p, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
		}
		else
			Log( "POSE ANIM twin of program %u failed (patterns follow the posed surface there): %s", p, err.c_str() );
	}
	animTwins[ p ] = twin;
	return twin;
}

void Renderer::ReleasePose()
{
	for( SlotPose& sp : pose )
	{
		for( GLuint* b : { &sp.vbo, &sp.palBuf, &sp.wBuf, &sp.trailVbo, &sp.trailPalBuf, &sp.trailWBuf } )
			if( *b )
				glDeleteBuffers( 1, b ), *b = 0;
		for( GLuint* t : { &sp.tbo, &sp.palTex, &sp.wTex, &sp.trailTbo, &sp.trailPalTex, &sp.trailWTex } )
			if( *t )
				glDeleteTextures( 1, t ), *t = 0;
		if( sp.inVao )
			glDeleteVertexArrays( 1, &sp.inVao ), sp.inVao = 0;
		sp = SlotPose();
	}
	for( const auto& t : animTwins )
		if( t.second != t.first )
			glDeleteProgram( t.second );
	animTwins.clear();
	if( refsVao )
		glDeleteVertexArrays( 1, &refsVao ), refsVao = 0;
	if( pivotVao )
		glDeleteVertexArrays( 1, &pivotVao ), pivotVao = 0;
	for( GLuint* p : { &progPose, &progRefs, &progPivotA, &progPivotB } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
}
}
