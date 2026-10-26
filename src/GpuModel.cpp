#include "GpuModel.h"
#include "Generate.h"
#include "Log.h"
#include "meshoptimizer.h"
#include <thread>
#include <OpenGL/OpenGL.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace mehr
{
namespace
{
std::mutex g_cacheMutex;
std::map< std::string, std::weak_ptr< GpuModel > > g_cache;
}

std::string ShareGroupKey()
{
	CGLContextObj ctx = CGLGetCurrentContext();
	void* group       = ctx ? (void*)CGLGetShareGroup( ctx ) : nullptr;
	char buf[ 32 ];
	snprintf( buf, sizeof( buf ), "%p", group );
	return buf;
}

std::shared_ptr< GpuModel > FindGpuModel( const std::string& key )
{
	std::lock_guard< std::mutex > lock( g_cacheMutex );
	auto it = g_cache.find( key );
	return it == g_cache.end() ? nullptr : it->second.lock();
}

void RegisterGpuModel( const std::string& key, const std::shared_ptr< GpuModel >& model )
{
	std::lock_guard< std::mutex > lock( g_cacheMutex );
	for( auto it = g_cache.begin(); it != g_cache.end(); )
		it = it->second.expired() ? g_cache.erase( it ) : std::next( it );
	g_cache[ key ] = model;
}

void ForgetGpuModelsForPath( const std::string& path )
{
	std::lock_guard< std::mutex > lock( g_cacheMutex );
	std::string prefix = path + "|";
	for( auto it = g_cache.begin(); it != g_cache.end(); )
		it = it->first.compare( 0, prefix.size(), prefix ) == 0 ? g_cache.erase( it ) : std::next( it );
}

GpuModel::GpuModel( std::shared_ptr< const ModelData > d ) :
	data( std::move( d ) )
{
	static std::atomic< uint64_t > s_serial{ 0 };
	serial        = ++s_serial;
	path          = data->path;
	triangleCount = data->triangleCount;
	fitScale      = data->fitScale;
	geometry      = data->geometry;
	anim          = data->anim;
	for( int k = 0; k < 3; ++k )
		fitCenter[ k ] = data->fitCenter[ k ];
	slotNames = data->slotNames;
	status    = data->status;
	for( const Part& p : data->parts )
	{
		GpuPart g;
		g.slot  = p.slot;
		g.glass = p.glass;
		g.fractured = p.fractured;
		hasPieces |= p.fractured;
		for( int k = 0; k < 3; ++k )
			g.glassTint[ k ] = p.glassTint[ k ], g.boxMin[ k ] = p.boxMin[ k ], g.boxMax[ k ] = p.boxMax[ k ];
		g.name       = p.name;
		g.firstIndex = p.firstIndex;
		g.indexCount = p.indexCount;
		for( int k = 0; k < 4; ++k )
			g.baseColor[ k ] = p.baseColor[ k ];
		for( int k = 0; k < 3; ++k )
			g.emissive[ k ] = p.emissive[ k ];
		g.roughness   = p.roughness;
		g.metallic    = p.metallic;
		g.normalScale = p.normalScale;
		g.alphaCutout = p.alphaCutout;
		g.flipGreen   = p.tex[ CH_NORMAL ].flipGreen;
		std::array< int, CH_COUNT > jobs;
		for( int ch = 0; ch < CH_COUNT; ++ch )
		{
			jobs[ ch ]        = -1;
			g.component[ ch ] = p.tex[ ch ].component;
			const Image* img  = p.tex[ ch ].image.get();
			if( !img || !img->Valid() )
				continue;
			bool srgb = ch == CH_BASECOLOR || ch == CH_EMISSIVE;
			for( size_t j = 0; j < texJobs.size(); ++j )
				if( texJobs[ j ].image == img && texJobs[ j ].srgb == srgb )
					jobs[ ch ] = (int)j;
			if( jobs[ ch ] < 0 )
			{
				jobs[ ch ] = (int)texJobs.size();
				texJobs.push_back( { img, srgb, 0 } );
			}
		}
		partJob.push_back( jobs );
		parts.push_back( g );
	}
}

struct LodJob
{
	std::atomic< bool > done{ false }, cancel{ false };
	std::vector< std::uint32_t > idx[ kLodLevels ];
	std::vector< std::array< uint32_t, 2 > > parts[ kLodLevels ];
	double ms = 0;
};

namespace
{
// Each level simplifies the previous one, part by part (parts stay separate draws with their own textures).
void BuildLods( const CpuGeometry& g, LodJob& job )
{
	auto t0                   = std::chrono::steady_clock::now();
	static const float ratio[ kLodLevels ] = { 1.0f, 0.25f, 0.06f, 0.015f };
	const size_t vcount       = g.pos.size() / 3;
	std::vector< uint32_t > prev = g.idx;
	std::vector< std::array< uint32_t, 2 > > prevParts( g.parts.begin(), g.parts.end() );
	for( int L = 1; L < kLodLevels && !job.cancel.load(); ++L )
	{
		std::vector< uint32_t >& out = job.idx[ L ];
		for( size_t p = 0; p < prevParts.size(); ++p )
		{
			const uint32_t first = prevParts[ p ][ 0 ], count = prevParts[ p ][ 1 ];
			const size_t target  = std::max< size_t >( 192, size_t( g.parts[ p ][ 1 ] * ratio[ L ] ) / 3 * 3 );
			std::vector< uint32_t > dst( count );
			size_t n = count;
			if( count > target )
			{
				n = meshopt_simplify( dst.data(), prev.data() + first, count, g.pos.data(), vcount, 12, target, 0.05f, 0, nullptr );
				if( n > target * 2 ) // UV seams can stall the regular simplifier: fall back to the sloppy one
					n = meshopt_simplifySloppy( dst.data(), prev.data() + first, count, g.pos.data(), vcount, 12, nullptr, target, 1.0f, nullptr );
			}
			else
				std::copy( prev.begin() + first, prev.begin() + first + count, dst.begin() );
			job.parts[ L ].push_back( { (uint32_t)out.size(), (uint32_t)n } );
			out.insert( out.end(), dst.begin(), dst.begin() + n );
		}
		prev      = out;
		prevParts = job.parts[ L ];
	}
	job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}
}

void GpuModel::UpdateLods()
{
	if( lodsReady || !complete || !geometry )
		return;
	if( !lodJob )
	{
		lodJob   = std::make_shared< LodJob >();
		auto job = lodJob;
		auto geo = geometry;
		std::thread( [ job, geo ]() {
			BuildLods( *geo, *job );
			job->done.store( true );
		} ).detach();
		return;
	}
	if( !lodJob->done.load() )
		return;
	GLint prevArray = 0, prevTBO = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevArray );
	glGetIntegerv( GL_TEXTURE_BINDING_BUFFER, &prevTBO );
	std::string sizes;
	for( int L = 1; L < kLodLevels; ++L )
	{
		LodLevel& l = lods[ L ];
		glGenBuffers( 1, &l.ibo );
		glBindBuffer( GL_ARRAY_BUFFER, l.ibo );
		glBufferData( GL_ARRAY_BUFFER, lodJob->idx[ L ].size() * sizeof( uint32_t ), lodJob->idx[ L ].data(), GL_STATIC_DRAW );
		glGenTextures( 1, &l.tbo );
		glBindTexture( GL_TEXTURE_BUFFER, l.tbo );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R32UI, l.ibo );
		l.parts     = lodJob->parts[ L ];
		l.triangles = lodJob->idx[ L ].size() / 3;
		sizes += " " + std::to_string( l.triangles );
	}
	glBindBuffer( GL_ARRAY_BUFFER, prevArray );
	glBindTexture( GL_TEXTURE_BUFFER, prevTBO );
	lods[ 0 ].ibo = ibo, lods[ 0 ].tbo = idxTBO, lods[ 0 ].triangles = triangleCount;
	for( const GpuPart& p : parts )
		lods[ 0 ].parts.push_back( { p.firstIndex, p.indexCount } );
	lodsReady = true;
	Log( "LODS %s: %zu ->%s triangles in %.0f ms (off the render thread)", LogName( path ).c_str(), triangleCount, sizes.c_str(), lodJob->ms );
	lodJob.reset();
}

GpuModel::~GpuModel()
{
	if( lodJob )
		lodJob->cancel.store( true );
	for( int L = 1; L < kLodLevels; ++L )
	{
		if( lods[ L ].tbo )
			glDeleteTextures( 1, &lods[ L ].tbo );
		if( lods[ L ].ibo )
			glDeleteBuffers( 1, &lods[ L ].ibo );
	}
	for( TexJob& j : texJobs )
		if( j.handle )
			glDeleteTextures( 1, &j.handle );

	if( touchFBO[ 0 ] )
		glDeleteFramebuffers( 2, touchFBO );
	if( touchRB )
		glDeleteRenderbuffers( 1, &touchRB );
	if( morphTBO )
		glDeleteTextures( 1, &morphTBO );
	for( GLuint* b : { &animVbo, &morphBuf } )
		if( *b )
			glDeleteBuffers( 1, b );
	if( vtxTBO )
		glDeleteTextures( 1, &vtxTBO );
	if( idxTBO )
		glDeleteTextures( 1, &idxTBO );
	if( vbo )
		glDeleteBuffers( 1, &vbo );
	if( ibo )
		glDeleteBuffers( 1, &ibo );
}

// W25: Apple's GL moves uploaded texels to the GPU only when the texture is first USED. A model uploaded over many
// frames but first drawn on its swap frame paid for every texture at once there (8K Corinthian: 36 ms of GPU in one
// frame, a hitch mid-transition). A 1-texel blit out of each texture as it completes spreads that over its own frame.
void GpuModel::Touch( GLuint tex )
{
	GLint prevRead = 0, prevDraw = 0;
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &prevRead );
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw );
	if( !touchFBO[ 0 ] )
	{
		glGenFramebuffers( 2, touchFBO );
		glGenRenderbuffers( 1, &touchRB );
		GLint prevRB = 0;
		glGetIntegerv( GL_RENDERBUFFER_BINDING, &prevRB );
		glBindRenderbuffer( GL_RENDERBUFFER, touchRB );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, 1, 1 );
		glBindRenderbuffer( GL_RENDERBUFFER, prevRB );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, touchFBO[ 1 ] );
		glFramebufferRenderbuffer( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, touchRB );
	}
	glBindFramebuffer( GL_READ_FRAMEBUFFER, touchFBO[ 0 ] );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, touchFBO[ 1 ] );
	const GLboolean scissor = glIsEnabled( GL_SCISSOR_TEST );
	glDisable( GL_SCISSOR_TEST );
	glBlitFramebuffer( 0, 0, 1, 1, 0, 0, 1, 1, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	if( scissor )
		glEnable( GL_SCISSOR_TEST );
	glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, prevRead );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, prevDraw );
}

bool GpuModel::UploadStep( int textureBudget )
{
	if( complete )
		return true;
	auto t0 = std::chrono::steady_clock::now();
	GLint prevArray = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevArray );

	if( !geometryDone )
	{
		// The index buffer is uploaded through GL_ARRAY_BUFFER so no VAO state is touched.
		glGenBuffers( 1, &vbo );
		glBindBuffer( GL_ARRAY_BUFFER, vbo );
		glBufferData( GL_ARRAY_BUFFER, data->vertices.size() * sizeof( Vertex ), data->vertices.data(), GL_STATIC_DRAW );
		glGenBuffers( 1, &ibo );
		glBindBuffer( GL_ARRAY_BUFFER, ibo );
		glBufferData( GL_ARRAY_BUFFER, data->indices.size() * sizeof( uint32_t ), data->indices.data(), GL_STATIC_DRAW );
		geometryBytes = data->vertices.size() * sizeof( Vertex ) + data->indices.size() * sizeof( uint32_t );
		GLint prevTBO = 0;
		glGetIntegerv( GL_TEXTURE_BINDING_BUFFER, &prevTBO );
		glGenTextures( 1, &vtxTBO );
		glBindTexture( GL_TEXTURE_BUFFER, vtxTBO );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R32UI, vbo );
		glGenTextures( 1, &idxTBO );
		glBindTexture( GL_TEXTURE_BUFFER, idxTBO );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R32UI, ibo );
		if( anim && anim->verts.size() == data->vertices.size() )
		{
			// MoveEhr W1: the pose pass's inputs, with the geometry (an animated file is drawn static until a pose runs)
			glGenBuffers( 1, &animVbo );
			glBindBuffer( GL_ARRAY_BUFFER, animVbo );
			glBufferData( GL_ARRAY_BUFFER, anim->verts.size() * sizeof( AnimVertex ), anim->verts.data(), GL_STATIC_DRAW );
			geometryBytes += anim->verts.size() * sizeof( AnimVertex );
			glGenBuffers( 1, &morphBuf );
			glBindBuffer( GL_TEXTURE_BUFFER, morphBuf );
			const float zero[ 8 ] = {};
			glBufferData( GL_TEXTURE_BUFFER, anim->morphDeltas.empty() ? sizeof( zero ) : anim->morphDeltas.size() * sizeof( float ),
			              anim->morphDeltas.empty() ? zero : anim->morphDeltas.data(), GL_STATIC_DRAW );
			geometryBytes += anim->morphDeltas.size() * sizeof( float );
			glGenTextures( 1, &morphTBO );
			glBindTexture( GL_TEXTURE_BUFFER, morphTBO );
			glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, morphBuf );
			glBindBuffer( GL_TEXTURE_BUFFER, 0 );
		}
		glBindTexture( GL_TEXTURE_BUFFER, prevTBO );
		geometryDone = true;
		textureBudget--; // geometry costs about one texture's worth of bandwidth
	}
	else
	{
		GLint prevTex = 0, prevAlign = 4;
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &prevTex );
		glGetIntegerv( GL_UNPACK_ALIGNMENT, &prevAlign );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		// W18: row chunks of at most kChunkBytes, until ~3 ms of this frame went to uploads. A texture is defined
		// (every level, nullptr) when it starts and filled level by level, top to bottom, over as many frames as it
		// takes; it is bound to parts only once the whole model is complete. `textureBudget` caps the textures
		// STARTED per frame (small ones still go two a frame).
		constexpr size_t kChunkBytes = 2u << 20;
		while( nextTexture < texJobs.size() )
		{
			TexJob& j        = texJobs[ nextTexture ];
			const Image& img = *j.image;
			const bool rgba  = img.components == 4;
			const GLenum fmt = rgba ? GL_RGBA : GL_RED;
			const GLint levels = (GLint)img.mips.size() + 1;
			if( !texOpen )
			{
				if( textureBudget-- <= 0 )
					break;
				const GLenum internal = rgba ? ( j.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8 ) : GL_R8;
				texBytes += size_t( img.width ) * img.height * img.components * 4 / 3; // + mip chain
				texLargest = std::max( texLargest, std::max( img.width, img.height ) );
				glGenTextures( 1, &j.handle );
				glBindTexture( GL_TEXTURE_2D, j.handle );
				// Define every level first, then fill: re-specifying levels one by one makes the driver
				// rebuild the texture each time.
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1 );
				for( GLint l = 0, w = img.width, h = img.height; l < levels; ++l, w = std::max( 1, w / 2 ), h = std::max( 1, h / 2 ) )
					glTexImage2D( GL_TEXTURE_2D, l, internal, w, h, 0, fmt, GL_UNSIGNED_BYTE, nullptr );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f );
				if( !rgba )
				{
					GLint sw[ 4 ] = { GL_RED, GL_RED, GL_RED, GL_ONE };
					glTexParameteriv( GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw );
				}
				texOpen = true, texLevel = 0, texRow = 0;
			}
			else
				glBindTexture( GL_TEXTURE_2D, j.handle );
			const int w = std::max( 1, img.width >> texLevel ), h = std::max( 1, img.height >> texLevel );
			const size_t rowBytes = size_t( w ) * img.components;
			const int rows        = std::min( h - texRow, (int)std::max< size_t >( 1, kChunkBytes / rowBytes ) );
			const uint8_t* src    = ( texLevel == 0 ? img.pixels.data() : img.mips[ texLevel - 1 ].data() ) + size_t( texRow ) * rowBytes;
			glTexSubImage2D( GL_TEXTURE_2D, texLevel, 0, texRow, w, rows, fmt, GL_UNSIGNED_BYTE, src );
			texRow += rows;
			if( texRow >= h )
			{
				texRow = 0;
				if( ++texLevel >= levels )
				{
					texOpen = false, ++nextTexture;
					Touch( j.handle );
					break; // the touch moves the whole texture: nothing more this frame
				}
			}
			if( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() > 3.0 )
				break;
		}
		glBindTexture( GL_TEXTURE_2D, prevTex );
		glPixelStorei( GL_UNPACK_ALIGNMENT, prevAlign );
	}
	glBindBuffer( GL_ARRAY_BUFFER, prevArray );

	double stepMs = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	msUpload += stepMs;
	msWorstStep = std::max( msWorstStep, stepMs );
	if( geometryDone && nextTexture >= texJobs.size() )
	{
		for( size_t p = 0; p < parts.size(); ++p )
			for( int ch = 0; ch < CH_COUNT; ++ch )
				parts[ p ].tex[ ch ] = partJob[ p ][ ch ] >= 0 ? texJobs[ partJob[ p ][ ch ] ].handle : 0;
		for( TexJob& j : texJobs )
			j.image = nullptr;
		data.reset(); // CPU copy no longer needed
		complete = true;
		Log( "UPLOAD done %s: %zu textures (largest %d px, %.1f MB GPU with mips), geometry %.1f MB, %.1f ms GL time total, worst frame step %.1f ms",
		     LogName( path ).c_str(), texJobs.size(), texLargest, texBytes / 1048576.0, geometryBytes / 1048576.0, msUpload, msWorstStep );
	}
	return complete;
}
}
