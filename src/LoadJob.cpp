#include "LoadJob.h"
#include "Generate.h"
#include "Log.h"
#include "MathUtil.h"
#include "ModelLoader.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <sys/stat.h>
#include <thread>

namespace mehr
{
namespace
{
// W7: CpuGeometry::fitBox, and swapQ: 16k area-weighted surface points (fixed seed), the dissolve noise at each, sorted
void SwapQuantiles( CpuGeometry& g )
{
	for( int k = 0; k < 3; ++k )
		g.fitBox[ k ] = 1e30f, g.fitBox[ 3 + k ] = -1e30f;
	for( uint32_t i : g.idx )
		for( int k = 0; k < 3; ++k )
		{
			const float f = ( g.pos[ size_t( i ) * 3 + k ] - g.fitCenter[ k ] ) * g.fitScale;
			g.fitBox[ k ] = std::min( g.fitBox[ k ], f ), g.fitBox[ 3 + k ] = std::max( g.fitBox[ 3 + k ], f );
		}
	if( g.idx.empty() )
		for( int k = 0; k < 3; ++k )
			g.fitBox[ k ] = -1, g.fitBox[ 3 + k ] = 1;
	const size_t tri = g.idx.size() / 3;
	std::vector< double > cum( tri );
	double acc = 0;
	for( size_t t = 0; t < tri; ++t )
	{
		const float* a = &g.pos[ g.idx[ t * 3 ] * 3 ];
		const float* b = &g.pos[ g.idx[ t * 3 + 1 ] * 3 ];
		const float* c = &g.pos[ g.idx[ t * 3 + 2 ] * 3 ];
		float e1[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, e2[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] }, cr[ 3 ];
		Mat4::Cross( e1, e2, cr );
		cum[ t ] = acc += std::sqrt( cr[ 0 ] * cr[ 0 ] + cr[ 1 ] * cr[ 1 ] + cr[ 2 ] * cr[ 2 ] );
	}
	if( tri == 0 || acc <= 0 )
		return;
	constexpr int N = 16384;
	std::mt19937 rng( 24681357u );
	std::uniform_real_distribution< double > U01( 0.0, 1.0 );
	std::vector< float > n( N );
	for( int i = 0; i < N; ++i )
	{
		const size_t t = std::min( size_t( std::lower_bound( cum.begin(), cum.end(), U01( rng ) * acc ) - cum.begin() ), tri - 1 );
		const double r1 = std::sqrt( U01( rng ) ), r2 = U01( rng );
		const float w[ 3 ] = { float( 1 - r1 ), float( r1 * ( 1 - r2 ) ), float( r1 * r2 ) };
		float f[ 3 ] = { 0, 0, 0 };
		for( int c = 0; c < 3; ++c )
			for( int k = 0; k < 3; ++k )
				f[ k ] += g.pos[ g.idx[ t * 3 + c ] * 3 + k ] * w[ c ];
		for( int k = 0; k < 3; ++k )
			f[ k ] = ( f[ k ] - g.fitCenter[ k ] ) * g.fitScale * 3.0f + 7.0f;
		n[ i ] = FbmF( f[ 0 ], f[ 1 ], f[ 2 ] );
	}
	std::sort( n.begin(), n.end() );
	for( int q = 0; q <= 128; ++q )
		g.swapQ[ q ] = n[ std::min< size_t >( N - 1, size_t( q ) * ( N - 1 ) / 128 ) ];
}
}

namespace
{
std::mutex g_jobsMutex;
std::map< std::string, std::weak_ptr< LoadTicket > > g_jobs;
std::atomic< int > g_liveWorkers{ 0 };
}

std::string ModelKey( const std::string& path, int maxTexture, bool movements )
{
	if( IsGenSpec( path ) )
		return GenSpecKey( path );
	if( IsTestModel( path ) )
		return path; // MoveEhr W5: generated in code, the same bytes every time
	struct stat st;
	if( path.empty() || stat( path.c_str(), &st ) != 0 )
		return {};
	return path + "|" + std::to_string( (long long)st.st_mtimespec.tv_sec ) + "." + std::to_string( (long long)st.st_mtimespec.tv_nsec ) + "|" + std::to_string( (long long)st.st_size ) + "@" + std::to_string( maxTexture ) + ( movements ? "|moves" : "" );
}

std::shared_ptr< LoadTicket > RequestLoad( const std::string& path, int maxTexture, bool forceFresh, bool movements )
{
	std::string key = ModelKey( path, maxTexture, movements );
	std::lock_guard< std::mutex > lock( g_jobsMutex );
	if( !forceFresh && !key.empty() )
	{
		auto it = g_jobs.find( key );
		if( it != g_jobs.end() )
			if( auto existing = it->second.lock() )
				return existing;
	}
	auto ticket  = std::make_shared< LoadTicket >();
	ticket->path = path;
	ticket->key  = key;
	ticket->maxTexture = maxTexture;
	ticket->movements  = movements;
	if( !key.empty() )
		g_jobs[ key ] = ticket;

	g_liveWorkers++;
	std::thread( [ ticket ]() {
		auto t0 = std::chrono::steady_clock::now();
		std::string error;
		std::unique_ptr< ModelData > model;
		try
		{
			if( IsGenSpec( ticket->path ) )
				model = GenerateModel( ticket->path, ticket->cancel, error, ticket->status );
			else
				model = LoadModel( ticket->path, ticket->maxTexture, ticket->cancel, error, ticket->movements );
		}
		catch( const std::exception& e )
		{
			error = std::string( "exception: " ) + e.what();
			Log( "LOAD FAIL %s: %s", ticket->path.c_str(), error.c_str() );
		}
		catch( ... )
		{
			error = "unknown exception";
			Log( "LOAD FAIL %s: %s", ticket->path.c_str(), error.c_str() );
		}
		if( model )
		{
			auto g = std::make_shared< CpuGeometry >();
			g->pos.resize( model->vertices.size() * 3 );
			g->uv.resize( model->vertices.size() * 2 );
			for( size_t i = 0; i < model->vertices.size(); ++i )
			{
				const Vertex& v = model->vertices[ i ];
				g->pos[ i * 3 ] = v.px, g->pos[ i * 3 + 1 ] = v.py, g->pos[ i * 3 + 2 ] = v.pz;
				g->uv[ i * 2 ] = v.u, g->uv[ i * 2 + 1 ] = v.v;
			}
			// W2: a text / logo with its fractured copy: the geometry jobs see the whole mesh only (the copy is drawn, not sampled)
			const size_t whole = model->wholeIndexCount ? model->wholeIndexCount : model->indices.size();
			g->idx.assign( model->indices.begin(), model->indices.begin() + whole );
			g->fracIdx.assign( model->indices.begin() + whole, model->indices.end() );
			g->fracPiece  = model->triPiece;
			g->pieceCount = model->pieceCount;
			for( const Part& p : model->parts )
			{
				if( p.fractured )
					continue;
				g->parts.push_back( { p.firstIndex, p.indexCount } );
				g->partSlot.push_back( p.slot );
			}
			g->triGroup   = model->triGroup;
			g->groupCount = model->groupCount;
			for( int k = 0; k < 3; ++k )
				g->fitCenter[ k ] = model->fitCenter[ k ];
			g->fitScale     = model->fitScale;
			if( !g->triGroup.empty() )
			{
				g->groupBox.assign( size_t( g->groupCount ) * 6, 0.0f );
				for( int k = 0; k < g->groupCount; ++k )
					for( int a = 0; a < 3; ++a )
						g->groupBox[ k * 6 + a ] = 1e30f, g->groupBox[ k * 6 + 3 + a ] = -1e30f;
				for( size_t i = 0; i < g->idx.size(); ++i )
				{
					float* b       = &g->groupBox[ size_t( g->triGroup[ i / 3 ] ) * 6 ];
					const float* p = &g->pos[ size_t( g->idx[ i ] ) * 3 ];
					for( int a = 0; a < 3; ++a )
					{
						const float f = ( p[ a ] - g->fitCenter[ a ] ) * g->fitScale;
						b[ a ] = std::min( b[ a ], f ), b[ 3 + a ] = std::max( b[ 3 + a ], f );
					}
				}
			}
			SwapQuantiles( *g );
			model->geometry = std::move( g );
		}
		ticket->model   = std::move( model );
		ticket->error   = error;
		ticket->msTotal = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
		ticket->done.store( true );
		g_liveWorkers--;
	} ).detach();
	return ticket;
}

void ShutdownLoaders()
{
	{
		std::lock_guard< std::mutex > lock( g_jobsMutex );
		for( auto& kv : g_jobs )
			if( auto t = kv.second.lock() )
				t->cancel.store( true );
		g_jobs.clear();
	}
	for( int i = 0; i < 500 && g_liveWorkers.load() > 0; ++i )
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
}
}
