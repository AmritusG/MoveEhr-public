#include "SdfBuilder.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace mehr
{
namespace
{
constexpr int N = kSdfRes;
inline size_t Idx( int x, int y, int z ) { return ( size_t( z ) * N + y ) * N + x; }

// Felzenszwalb & Huttenlocher squared Euclidean distance transform along one line (in place)
void Edt1D( std::vector< float >& f, std::vector< float >& d, std::vector< int >& v, std::vector< float >& zb, int n )
{
	int k  = 0;
	v[ 0 ] = 0, zb[ 0 ] = -1e30f, zb[ 1 ] = 1e30f;
	auto sect = [ & ]( int q, int p ) { return ( ( f[ q ] + float( q ) * q ) - ( f[ p ] + float( p ) * p ) ) / ( 2.0f * float( q - p ) ); };
	for( int q = 1; q < n; ++q )
	{
		float s = sect( q, v[ k ] );
		while( s <= zb[ k ] )
			s = sect( q, v[ --k ] );
		++k, v[ k ] = q, zb[ k ] = s, zb[ k + 1 ] = 1e30f;
	}
	k = 0;
	for( int q = 0; q < n; ++q )
	{
		while( zb[ k + 1 ] < q )
			++k;
		d[ q ] = float( q - v[ k ] ) * float( q - v[ k ] ) + f[ v[ k ] ];
	}
}

// squared distance (voxels) to the nearest voxel where feature != 0
std::vector< float > Edt( const std::vector< uint8_t >& feature )
{
	std::vector< float > g( feature.size() );
	for( size_t i = 0; i < g.size(); ++i )
		g[ i ] = feature[ i ] ? 0.0f : 1e12f; // finite: keeps the parabola intersections exact enough in float
	std::vector< float > f( N ), d( N ), zb( N + 1 );
	std::vector< int > v( N );
	for( int axis = 0; axis < 3; ++axis )
		for( int a = 0; a < N; ++a )
			for( int b = 0; b < N; ++b )
			{
				auto at = [ & ]( int t ) -> float& { return axis == 0 ? g[ Idx( t, a, b ) ] : axis == 1 ? g[ Idx( a, t, b ) ] : g[ Idx( a, b, t ) ]; };
				for( int t = 0; t < N; ++t )
					f[ t ] = at( t );
				Edt1D( f, d, v, zb, N );
				for( int t = 0; t < N; ++t )
					at( t ) = d[ t ];
			}
	return g;
}

// 6-connected flood from the grid border through voxels where open != 0; returns reached
std::vector< uint8_t > FloodFromBorder( const std::vector< uint8_t >& open )
{
	std::vector< uint8_t > seen( open.size(), 0 );
	std::vector< int > stack;
	auto push = [ & ]( int x, int y, int z ) {
		if( x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= N )
			return;
		const size_t i = Idx( x, y, z );
		if( open[ i ] && !seen[ i ] )
			seen[ i ] = 1, stack.push_back( int( i ) );
	};
	for( int a = 0; a < N; ++a )
		for( int b = 0; b < N; ++b )
			push( 0, a, b ), push( N - 1, a, b ), push( a, 0, b ), push( a, N - 1, b ), push( a, b, 0 ), push( a, b, N - 1 );
	while( !stack.empty() )
	{
		const int i = stack.back();
		stack.pop_back();
		const int x = i % N, y = ( i / N ) % N, z = i / ( N * N );
		push( x - 1, y, z ), push( x + 1, y, z ), push( x, y - 1, z ), push( x, y + 1, z ), push( x, y, z - 1 ), push( x, y, z + 1 );
	}
	return seen;
}

std::mutex g_mutex;
std::condition_variable g_cv;
int g_running = 0;
std::map< const CpuGeometry*, std::weak_ptr< SdfJob > > g_jobs;
}

SdfField BuildSdf( const CpuGeometry& g )
{
	const auto t0 = std::chrono::steady_clock::now();
	SdfField s;
	const size_t nv = g.pos.size() / 3;
	float lo[ 3 ] = { 1e30f, 1e30f, 1e30f }, hi[ 3 ] = { -1e30f, -1e30f, -1e30f };
	for( size_t i = 0; i < nv; ++i )
		for( int k = 0; k < 3; ++k )
			lo[ k ] = std::min( lo[ k ], g.pos[ i * 3 + k ] ), hi[ k ] = std::max( hi[ k ], g.pos[ i * 3 + k ] );
	float span = 0;
	for( int k = 0; k < 3; ++k )
		span = std::max( span, hi[ k ] - lo[ k ] );
	if( nv == 0 || !( span > 0 ) )
		return s;
	s.boxSize = span * 1.2f; // 10 % margin each side: the grid border is always outside
	for( int k = 0; k < 3; ++k )
		s.boxMin[ k ] = 0.5f * ( lo[ k ] + hi[ k ] ) - 0.5f * s.boxSize;
	const float cell = s.boxSize / N, inv = 1.0f / cell;

	// 1. surface voxels: every triangle sampled at < half a voxel
	std::vector< uint8_t > solid( size_t( N ) * N * N, 0 );
	auto mark = [ & ]( const float* p ) {
		const int x = int( ( p[ 0 ] - s.boxMin[ 0 ] ) * inv ), y = int( ( p[ 1 ] - s.boxMin[ 1 ] ) * inv ), z = int( ( p[ 2 ] - s.boxMin[ 2 ] ) * inv );
		if( x >= 0 && y >= 0 && z >= 0 && x < N && y < N && z < N )
			solid[ Idx( x, y, z ) ] = 1;
	};
	for( size_t t = 0; t + 2 < g.idx.size(); t += 3 )
	{
		const float *a = &g.pos[ g.idx[ t ] * 3 ], *b = &g.pos[ g.idx[ t + 1 ] * 3 ], *c = &g.pos[ g.idx[ t + 2 ] * 3 ];
		float e = 0;
		for( const float* q : { b, c } )
			e = std::max( e, std::sqrt( ( q[ 0 ] - a[ 0 ] ) * ( q[ 0 ] - a[ 0 ] ) + ( q[ 1 ] - a[ 1 ] ) * ( q[ 1 ] - a[ 1 ] ) + ( q[ 2 ] - a[ 2 ] ) * ( q[ 2 ] - a[ 2 ] ) ) );
		e = std::max( e, std::sqrt( ( c[ 0 ] - b[ 0 ] ) * ( c[ 0 ] - b[ 0 ] ) + ( c[ 1 ] - b[ 1 ] ) * ( c[ 1 ] - b[ 1 ] ) + ( c[ 2 ] - b[ 2 ] ) * ( c[ 2 ] - b[ 2 ] ) ) );
		const int n = std::max( 1, int( std::ceil( e * inv * 2.0f ) ) );
		for( int i = 0; i <= n; ++i )
			for( int j = 0; i + j <= n; ++j )
			{
				const float u = float( i ) / n, v = float( j ) / n, w = 1.0f - u - v;
				const float p[ 3 ] = { a[ 0 ] * w + b[ 0 ] * u + c[ 0 ] * v, a[ 1 ] * w + b[ 1 ] * u + c[ 1 ] * v, a[ 2 ] * w + b[ 2 ] * u + c[ 2 ] * v };
				mark( p );
			}
	}
	// 2. voxels the outside cannot reach are inside a closed shell: solid
	std::vector< uint8_t > open( solid.size() );
	for( size_t i = 0; i < solid.size(); ++i )
		open[ i ] = !solid[ i ];
	const std::vector< uint8_t > outside = FloodFromBorder( open );
	for( size_t i = 0; i < solid.size(); ++i )
		solid[ i ] = !outside[ i ];
	// 3. cavity: close holes up to ~3 voxels wide (dilate by 3), flood, erode back by 3; minus the solid
	const std::vector< float > dSolid = Edt( solid );
	std::vector< uint8_t > openD( solid.size() );
	for( size_t i = 0; i < solid.size(); ++i )
		openD[ i ] = dSolid[ i ] > 9.0f;
	const std::vector< uint8_t > outsideD = FloodFromBorder( openD );
	const std::vector< float > dOut = Edt( outsideD ); // distance to the dilated outside
	// 4. signed distance: + outside (to the solid), - inside (to the free space)
	std::vector< uint8_t > free( solid.size() );
	for( size_t i = 0; i < solid.size(); ++i )
		free[ i ] = !solid[ i ];
	const std::vector< float > dFree = Edt( free );
	s.rg.resize( solid.size() * 2 );
	for( size_t i = 0; i < solid.size(); ++i )
	{
		const float sd = solid[ i ] ? -( std::sqrt( dFree[ i ] ) - 0.5f ) : std::sqrt( dSolid[ i ] ) - 0.5f;
		const bool cavity = !solid[ i ] && dOut[ i ] > 9.0f;
		s.rg[ i * 2 ]     = sd * cell;
		s.rg[ i * 2 + 1 ] = cavity ? 1.0f : 0.0f;
		s.solidVoxels += solid[ i ];
		s.cavityVoxels += cavity;
	}
	s.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	return s;
}

void FillSdf( SdfField& f, int r )
{
	if( f.rg.empty() )
		return;
	const size_t n = size_t( N ) * N * N;
	std::vector< uint8_t > solid( n ), open( n );
	for( size_t i = 0; i < n; ++i )
		solid[ i ] = f.rg[ i * 2 ] < 0.0f;
	const std::vector< float > dSolid = Edt( solid );
	const float r2 = float( r * r );
	// the grid's margin (~6 voxels) is narrower than r: the band along the border counts as open (the space beyond the grid is)
	for( int z = 0; z < N; ++z )
		for( int y = 0; y < N; ++y )
			for( int x = 0; x < N; ++x )
			{
				const size_t i = Idx( x, y, z );
				const int bd   = std::min( { x, y, z, N - 1 - x, N - 1 - y, N - 1 - z } );
				open[ i ]      = !solid[ i ] && ( dSolid[ i ] > r2 || bd < r );
			}
	const std::vector< uint8_t > outsideD = FloodFromBorder( open );
	const std::vector< float > dOut       = Edt( outsideD );
	std::vector< uint8_t > filled( n ), free( n );
	for( size_t i = 0; i < n; ++i )
		filled[ i ] = solid[ i ] || dOut[ i ] > r2, free[ i ] = !filled[ i ];
	const std::vector< float > dF = Edt( filled ), dE = Edt( free );
	const float cell = f.boxSize / N;
	for( size_t i = 0; i < n; ++i )
		f.rg[ i * 2 ] = ( filled[ i ] ? -( std::sqrt( dE[ i ] ) - 0.5f ) : std::sqrt( dF[ i ] ) - 0.5f ) * cell;
}

float SdfAt( const SdfField& f, const float* p )
{
	if( f.rg.empty() )
		return 1e9f;
	const float cell = f.boxSize / N;
	float g[ 3 ], out = 0;
	int i0[ 3 ];
	float fr[ 3 ];
	for( int k = 0; k < 3; ++k )
	{
		const float lo = f.boxMin[ k ], hi = f.boxMin[ k ] + f.boxSize;
		out += p[ k ] < lo ? ( lo - p[ k ] ) * ( lo - p[ k ] ) : p[ k ] > hi ? ( p[ k ] - hi ) * ( p[ k ] - hi ) : 0.0f;
		g[ k ]  = std::min( std::max( ( p[ k ] - lo ) / cell - 0.5f, 0.0f ), float( N - 1 ) - 1e-4f );
		i0[ k ] = std::min( int( g[ k ] ), N - 2 );
		fr[ k ] = g[ k ] - float( i0[ k ] );
	}
	float v = 0;
	for( int c = 0; c < 8; ++c )
	{
		const int dx = c & 1, dy = ( c >> 1 ) & 1, dz = c >> 2;
		const float w = ( dx ? fr[ 0 ] : 1 - fr[ 0 ] ) * ( dy ? fr[ 1 ] : 1 - fr[ 1 ] ) * ( dz ? fr[ 2 ] : 1 - fr[ 2 ] );
		v += w * f.rg[ Idx( i0[ 0 ] + dx, i0[ 1 ] + dy, i0[ 2 ] + dz ) * 2 ];
	}
	return v + std::sqrt( out );
}

float SdfHit( const SdfField& f, const float* o, const float* d, float t0, float t1 )
{
	if( f.rg.empty() )
		return t1;
	const float cell = f.boxSize / N;
	float t = t0, p[ 3 ];
	auto at = [ & ]( float s ) {
		for( int k = 0; k < 3; ++k )
			p[ k ] = o[ k ] + d[ k ] * s;
		return SdfAt( f, p );
	};
	// started inside the solid: out first (at most the box's diagonal)
	for( int i = 0; i < 256 && t < t1; ++i )
	{
		const float s = at( t );
		if( s > 0.25f * cell )
			break;
		t += std::max( -s, 0.5f * cell );
	}
	for( int i = 0; i < 512 && t < t1; ++i )
	{
		const float s = at( t );
		if( s < 0.25f * cell )
			return t;
		t += std::max( 0.9f * s, 0.3f * cell );
	}
	return t1;
}

std::shared_ptr< SdfJob > RequestSdf( const std::shared_ptr< const CpuGeometry >& geometry )
{
	std::lock_guard< std::mutex > lock( g_mutex );
	for( auto it = g_jobs.begin(); it != g_jobs.end(); )
		it = it->second.expired() ? g_jobs.erase( it ) : std::next( it );
	auto& slot = g_jobs[ geometry.get() ];
	if( auto job = slot.lock() )
		return job;
	auto job = std::make_shared< SdfJob >();
	slot     = job;
	++g_running;
	std::thread( [ job, geometry ]() {
		job->field = BuildSdf( *geometry );
		job->done.store( true );
		std::lock_guard< std::mutex > l( g_mutex );
		--g_running;
		g_cv.notify_all();
	} ).detach();
	return job;
}

void ShutdownSdfBuilder()
{
	std::unique_lock< std::mutex > lock( g_mutex );
	g_cv.wait( lock, [] { return g_running == 0; } );
	g_jobs.clear();
}
}
