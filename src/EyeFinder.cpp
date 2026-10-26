#include "EyeFinder.h"
#include "stb_image_write.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

namespace mehr
{
namespace
{
constexpr int kRes       = 512;
constexpr float kNoDepth = -1e30f;

struct Region
{
	int id = 0, area = 0, seeThrough = 0;
	double sx = 0, sy = 0;
	int x0 = kRes, y0 = kRes, x1 = -1, y1 = -1;
	float cx = 0, cy = 0;     // pixel centroid
	float rimZ = 0, backZ = 0; // median closed depth (the rim level) / median front depth of the covered pixels
	bool candidate = false;
	std::string why;
	bool SeeThrough() const { return seeThrough * 4 >= area; } // a hole with its bevel / socket around it counts
	const char* Kind() const { return SeeThrough() ? "see-through" : "cavity"; }
};

std::string Fmt( const char* fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
std::string Fmt( const char* fmt, ... )
{
	char buf[ 512 ];
	va_list a;
	va_start( a, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, a );
	va_end( a );
	return buf;
}

float Median( std::vector< float >& v, float fallback )
{
	if( v.empty() )
		return fallback;
	std::nth_element( v.begin(), v.begin() + v.size() / 2, v.end() );
	return v[ v.size() / 2 ];
}

// separable flat max (dilate) / min (erode) filter with a (2r+1) square window
void Filter( std::vector< float >& img, int r, bool dilate )
{
	std::vector< float > tmp( img.size() );
	auto pick = [ dilate ]( float a, float b ) { return dilate ? std::max( a, b ) : std::min( a, b ); };
	for( int pass = 0; pass < 2; ++pass )
	{
		const std::vector< float >& src = pass == 0 ? img : tmp;
		std::vector< float >& dst       = pass == 0 ? tmp : img;
		for( int y = 0; y < kRes; ++y )
			for( int x = 0; x < kRes; ++x )
			{
				float v = src[ y * kRes + x ];
				for( int d = -r; d <= r; ++d )
				{
					const int q = pass == 0 ? x + d : y + d;
					if( q < 0 || q >= kRes )
						continue;
					v = pick( v, pass == 0 ? src[ y * kRes + q ] : src[ q * kRes + x ] );
				}
				dst[ y * kRes + x ] = v;
			}
	}
}

std::mutex g_mutex;
std::condition_variable g_cv;
int g_running = 0;
std::map< std::string, std::weak_ptr< EyeJob > > g_jobs;
}

std::string EyeQuery::Key() const
{
	std::string k = Fmt( "%.6g %.6g %.6g %.6g ", fitCenter[ 0 ], fitCenter[ 1 ], fitCenter[ 2 ], fitScale );
	for( uint8_t i : include )
		k += char( '0' + i );
	return k;
}

EyeResult FindEyes( const CpuGeometry& g, const EyeQuery& q, const std::string& debugPng )
{
	const auto t0 = std::chrono::steady_clock::now();
	EyeResult r;
	const size_t nv = g.pos.size() / 3;
	// the triangles to rasterise: the included parts (all when the selection is empty or selects nothing)
	std::vector< std::array< uint32_t, 2 > > ranges;
	for( size_t p = 0; p < g.parts.size(); ++p )
		if( p < q.include.size() && q.include[ p ] )
			ranges.push_back( g.parts[ p ] );
	if( ranges.empty() )
		ranges.push_back( { 0u, (uint32_t)g.idx.size() } );
	if( nv == 0 || g.idx.size() < 3 )
	{
		r.reason = "no geometry";
		return r;
	}
	// fitted mask space: (p - fitCenter) * fitScale; bounds over the rasterised triangles
	auto P = [ & ]( uint32_t v, int k ) { return ( g.pos[ v * 3 + k ] - q.fitCenter[ k ] ) * q.fitScale; };
	float lo[ 3 ] = { 1e30f, 1e30f, 1e30f }, hi[ 3 ] = { -1e30f, -1e30f, -1e30f };
	for( const auto& rg : ranges )
		for( uint32_t i = rg[ 0 ]; i < rg[ 0 ] + rg[ 1 ]; ++i )
			for( int k = 0; k < 3; ++k )
				lo[ k ] = std::min( lo[ k ], P( g.idx[ i ], k ) ), hi[ k ] = std::max( hi[ k ], P( g.idx[ i ], k ) );
	const float span = std::max( hi[ 0 ] - lo[ 0 ], hi[ 1 ] - lo[ 1 ] );
	if( !( span > 0 ) )
	{
		r.reason = "degenerate bounds";
		return r;
	}
	// square pixels, 4 px border so the background always touches the image edge
	const float pxPer = float( kRes - 8 ) / span;
	const float ox = 0.5f * ( lo[ 0 ] + hi[ 0 ] ), oy = 0.5f * ( lo[ 1 ] + hi[ 1 ] );
	auto toMask = [ & ]( float px, float py, float& x, float& y ) {
		x = ( px - kRes * 0.5f ) / pxPer + ox;
		y = ( py - kRes * 0.5f ) / pxPer + oy;
	};

	// ---- orthographic front rasterisation: coverage + front-most z (row 0 = bottom)
	std::vector< float > depth( kRes * kRes, kNoDepth );
	std::vector< uint8_t > cover( kRes * kRes, 0 );
	for( const auto& rg : ranges )
		for( uint32_t t = rg[ 0 ]; t + 2 < rg[ 0 ] + rg[ 1 ]; t += 3 )
		{
			float x[ 3 ], y[ 3 ], z[ 3 ];
			for( int k = 0; k < 3; ++k )
			{
				const uint32_t v = g.idx[ t + k ];
				x[ k ]           = ( P( v, 0 ) - ox ) * pxPer + kRes * 0.5f;
				y[ k ]           = ( P( v, 1 ) - oy ) * pxPer + kRes * 0.5f;
				z[ k ]           = P( v, 2 );
			}
			const float area = ( x[ 1 ] - x[ 0 ] ) * ( y[ 2 ] - y[ 0 ] ) - ( x[ 2 ] - x[ 0 ] ) * ( y[ 1 ] - y[ 0 ] );
			if( std::fabs( area ) < 1e-12f )
				continue;
			const int bx0 = std::max( 0, (int)std::floor( std::min( { x[ 0 ], x[ 1 ], x[ 2 ] } ) ) );
			const int bx1 = std::min( kRes - 1, (int)std::ceil( std::max( { x[ 0 ], x[ 1 ], x[ 2 ] } ) ) );
			const int by0 = std::max( 0, (int)std::floor( std::min( { y[ 0 ], y[ 1 ], y[ 2 ] } ) ) );
			const int by1 = std::min( kRes - 1, (int)std::ceil( std::max( { y[ 0 ], y[ 1 ], y[ 2 ] } ) ) );
			const float inv = 1.0f / area;
			for( int py = by0; py <= by1; ++py )
				for( int px = bx0; px <= bx1; ++px )
				{
					const float cx = px + 0.5f, cy = py + 0.5f;
					const float w0 = ( ( x[ 1 ] - cx ) * ( y[ 2 ] - cy ) - ( x[ 2 ] - cx ) * ( y[ 1 ] - cy ) ) * inv;
					const float w1 = ( ( x[ 2 ] - cx ) * ( y[ 0 ] - cy ) - ( x[ 0 ] - cx ) * ( y[ 2 ] - cy ) ) * inv;
					const float w2 = 1.0f - w0 - w1;
					if( w0 < 0 || w1 < 0 || w2 < 0 )
						continue;
					const int i = py * kRes + px;
					cover[ i ]  = 1;
					depth[ i ]  = std::max( depth[ i ], w0 * z[ 0 ] + w1 * z[ 1 ] + w2 * z[ 2 ] );
				}
		}
	int covered = 0;
	int sx0 = kRes, sx1 = -1, sy0 = kRes, sy1 = -1;
	for( int py = 0; py < kRes; ++py )
		for( int px = 0; px < kRes; ++px )
			if( cover[ py * kRes + px ] )
				++covered, sx0 = std::min( sx0, px ), sx1 = std::max( sx1, px ), sy0 = std::min( sy0, py ), sy1 = std::max( sy1, py );
	if( covered == 0 )
	{
		r.reason = "nothing to see from the front";
		return r;
	}
	const float silW = float( sx1 - sx0 + 1 ), silH = float( sy1 - sy0 + 1 );

	// ---- background = uncovered pixels 4-connected to the border (-1); enclosed uncovered pixels = see-through (1)
	std::vector< int8_t > bg( kRes * kRes, 0 );
	std::vector< int > stack;
	for( int k = 0; k < kRes; ++k )
		for( int s : { k, ( kRes - 1 ) * kRes + k, k * kRes, k * kRes + kRes - 1 } )
			if( !cover[ s ] && bg[ s ] == 0 )
			{
				stack.push_back( s );
				bg[ s ] = -1;
				while( !stack.empty() )
				{
					const int i = stack.back();
					stack.pop_back();
					const int px = i % kRes, py = i / kRes;
					const int nb[ 4 ][ 2 ] = { { px - 1, py }, { px + 1, py }, { px, py - 1 }, { px, py + 1 } };
					for( const auto& n : nb )
						if( n[ 0 ] >= 0 && n[ 1 ] >= 0 && n[ 0 ] < kRes && n[ 1 ] < kRes )
						{
							const int j = n[ 1 ] * kRes + n[ 0 ];
							if( !cover[ j ] && bg[ j ] == 0 )
								bg[ j ] = -1, stack.push_back( j );
						}
				}
			}
	int holePx = 0;
	for( int i = 0; i < kRes * kRes; ++i )
		if( !cover[ i ] && bg[ i ] == 0 )
			bg[ i ] = 1, ++holePx;

	// ---- symmetry axis: the x about which silhouette + depth mirror best (within 12 % of the width of the centre)
	const float zr = std::max( 1e-4f, hi[ 2 ] - lo[ 2 ] );
	float axis = 0.5f * ( sx0 + sx1 + 1 ), bestMis = 1e30f;
	{
		const float c0 = axis;
		for( float a = c0 - 0.12f * silW; a <= c0 + 0.12f * silW; a += 0.5f )
		{
			double mis = 0;
			int n      = 0;
			for( int py = sy0; py <= sy1; py += 2 )
				for( int px = sx0; px <= sx1; px += 2 )
				{
					const int i = py * kRes + px;
					if( !cover[ i ] )
						continue;
					++n;
					const int mx = (int)std::floor( 2.0f * a - ( px + 0.5f ) );
					if( mx < 0 || mx >= kRes || !cover[ py * kRes + mx ] )
						mis += 1.0;
					else
						mis += std::min( 1.0f, 4.0f * std::fabs( depth[ i ] - depth[ py * kRes + mx ] ) / zr );
				}
			mis /= std::max( 1, n );
			if( mis < bestMis - 1e-9 )
				bestMis = float( mis ), axis = a;
		}
	}
	{
		float ax, ay;
		toMask( axis, 0, ax, ay );
		r.axisX = ax;
	}

	// ---- cavity test: closing(depth) - depth, uncovered pixels count as infinitely far back
	const int R = std::max( 3, int( 0.08f * std::max( silW, silH ) ) );
	std::vector< float > closed = depth;
	Filter( closed, R, true );
	// the background must not lift the rim: a dome's edge curving away from the camera is not a cavity
	for( int i = 0; i < kRes * kRes; ++i )
		if( bg[ i ] == -1 )
			closed[ i ] = kNoDepth;
	Filter( closed, R, false );
	const float stepMin = 0.04f; // a jump deeper than 2 % of the model size (fitted: largest dimension = 2 units)
	std::vector< uint8_t > cand( kRes * kRes, 0 );
	int cavityPx = 0;
	for( int i = 0; i < kRes * kRes; ++i )
	{
		if( bg[ i ] == 1 )
			cand[ i ] = 1;
		else if( cover[ i ] && closed[ i ] > kNoDepth * 0.5f && closed[ i ] - depth[ i ] > stepMin )
			cand[ i ] = 2, ++cavityPx;
	}
	const int minArea = std::max( 20, int( 0.002f * covered ) );
	const int minSee  = std::max( 8, int( 0.0003f * covered ) ); // a symmetric pair of see-through holes may be pupils
	r.log.push_back( Fmt( "front view %dx%d, %.1f px per unit, silhouette %d px (%.0f x %.0f px), symmetry axis x %.3f (%+.1f %% of the width from the bbox centre, "
	                      "mismatch %.3f), see-through %d px, cavity %d px (closing %d px, step > %.3f), min area %d px (see-through %d px)",
	                      kRes, kRes, pxPer, covered, silW, silH, r.axisX, 100.0f * ( axis - 0.5f * ( sx0 + sx1 + 1 ) ) / silW, bestMis, holePx, cavityPx, R,
	                      stepMin, minArea, minSee ) );

	// ---- regions: 4-connected candidate pixels (see-through and cavity merged)
	std::vector< int > label( kRes * kRes, 0 );
	std::vector< Region > regions;
	for( int s = 0; s < kRes * kRes; ++s )
	{
		if( !cand[ s ] || label[ s ] )
			continue;
		Region h;
		h.id = int( regions.size() ) + 1;
		std::vector< float > rims, backs;
		stack.push_back( s );
		label[ s ] = h.id;
		while( !stack.empty() )
		{
			const int i = stack.back();
			stack.pop_back();
			const int px = i % kRes, py = i / kRes;
			h.area++, h.sx += px + 0.5, h.sy += py + 0.5;
			h.x0 = std::min( h.x0, px ), h.x1 = std::max( h.x1, px ), h.y0 = std::min( h.y0, py ), h.y1 = std::max( h.y1, py );
			if( cand[ i ] == 1 )
				h.seeThrough++;
			else
				backs.push_back( depth[ i ] );
			if( closed[ i ] > kNoDepth * 0.5f )
				rims.push_back( closed[ i ] );
			const int nb[ 4 ][ 2 ] = { { px - 1, py }, { px + 1, py }, { px, py - 1 }, { px, py + 1 } };
			for( const auto& n : nb )
				if( n[ 0 ] >= 0 && n[ 1 ] >= 0 && n[ 0 ] < kRes && n[ 1 ] < kRes )
				{
					const int j = n[ 1 ] * kRes + n[ 0 ];
					if( cand[ j ] && !label[ j ] )
						label[ j ] = h.id, stack.push_back( j );
				}
		}
		h.cx = float( h.sx / h.area ), h.cy = float( h.sy / h.area );
		h.backZ = Median( backs, kNoDepth );
		if( rims.empty() )
		{
			// a see-through hole wider than the closing: the rim = the covered pixels right around it
			for( int py = std::max( 0, h.y0 - 3 ); py <= std::min( kRes - 1, h.y1 + 3 ); ++py )
				for( int px = std::max( 0, h.x0 - 3 ); px <= std::min( kRes - 1, h.x1 + 3 ); ++px )
				{
					const int i = py * kRes + px;
					if( !cover[ i ] || cand[ i ] )
						continue;
					bool near = false;
					for( int d = -3; d <= 3 && !near; ++d )
						near = ( px + d >= 0 && px + d < kRes && label[ i + d ] == h.id ) || ( py + d >= 0 && py + d < kRes && label[ i + d * kRes ] == h.id );
					if( near )
						rims.push_back( depth[ i ] );
				}
		}
		h.rimZ = Median( rims, h.backZ > kNoDepth * 0.5f ? h.backZ : 0.0f );
		regions.push_back( h );
	}

	std::vector< Region* > cands;
	int listed = 0;
	for( Region& h : regions )
	{
		const float relY = ( h.cy - sy0 ) / silH; // 0 bottom, 1 top
		if( h.area < ( h.SeeThrough() ? minSee : minArea ) )
			h.why = "too small";
		else if( relY < 0.25f )
			h.why = "in the lower 25 %";
		else if( h.x1 - h.x0 + 1 > 0.9f * silW )
			h.why = "spans the whole width";
		else
			h.candidate = true, cands.push_back( &h );
		if( h.area >= minArea || h.candidate )
		{
			float mx, my;
			toMask( h.cx, h.cy, mx, my );
			++listed;
			r.log.push_back( Fmt( "  region %d (%s): area %d px (%d see-through), centre (%.3f, %.3f) mask, %.0f x %d px, height %.0f %%, rim z %.3f%s -> %s", h.id,
			                      h.Kind(), h.area, h.seeThrough, mx, my, float( h.x1 - h.x0 + 1 ), h.y1 - h.y0 + 1, 100.0f * relY, h.rimZ,
			                      h.backZ > kNoDepth * 0.5f ? Fmt( ", back z %.3f", h.backZ ).c_str() : "", h.candidate ? "candidate" : h.why.c_str() ) );
		}
	}
	if( (int)regions.size() > listed )
		r.log.push_back( Fmt( "  (%zu regions below the minimum area not listed)", regions.size() - listed ) );

	auto fill = [ & ]( const Region& h, float px, float py, float radiusPx, EyeHole& e ) {
		toMask( px, py, e.center[ 0 ], e.center[ 1 ] );
		e.center[ 2 ] = h.rimZ;
		e.radius      = radiusPx / pxPer;
		e.areaPx      = h.area;
		e.depth       = h.SeeThrough() || h.backZ < kNoDepth * 0.5f ? 0.0f : std::max( 0.0f, h.rimZ - h.backZ );
	};

	// ---- score every pair: both sides of the axis, similar area, similar height, mirrored about the axis, apart.
	// Pass 1: see-through regions only (a symmetric pair of holes is the eyes: every v1 find stays); pass 2: all.
	const Region *bestA = nullptr, *bestB = nullptr;
	float best = 0;
	for( int pass = 0; pass < 2 && !bestA; ++pass )
	for( size_t i = 0; i < cands.size(); ++i )
		for( size_t j = i + 1; j < cands.size(); ++j )
		{
			const Region *a = cands[ i ], *b = cands[ j ];
			if( pass == 0 ? !( a->SeeThrough() && b->SeeThrough() ) : ( a->SeeThrough() && b->SeeThrough() ) )
				continue;
			if( a->cx > b->cx )
				std::swap( a, b );
			const float ratio  = float( std::min( a->area, b->area ) ) / float( std::max( a->area, b->area ) );
			const float dy     = std::fabs( a->cy - b->cy ) / silH;
			const float mirror = std::fabs( 0.5f * ( a->cx + b->cx ) - axis ) / silW;
			const float apart  = ( b->cx - a->cx ) / silW;
			const float relY   = ( 0.5f * ( a->cy + b->cy ) - sy0 ) / silH;
			std::string fail;
			if( a->cx >= axis || b->cx <= axis )
				fail = "same side of the axis";
			else if( ratio < 0.4f )
				fail = "areas differ";
			else if( dy > 0.1f )
				fail = "heights differ";
			else if( mirror > 0.08f )
				fail = "not mirrored about the axis";
			else if( apart < 0.08f )
				fail = "too close together";
			else if( pass == 1 && relY < 0.4f )
				fail = "a cavity pair below 40 % of the height (vents, nostrils, mouth)";
			else
			{
				// a much larger opening right above the pair: the pair is teeth / nostrils / a grille under the eyes
				for( const Region* c : cands )
					if( c != a && c != b && c->area >= 2 * std::max( a->area, b->area ) && c->y0 > std::max( a->y1, b->y1 ) && c->x0 < b->x1 && c->x1 > a->x0 )
						fail = Fmt( "region %d (%.1fx larger) sits right above", c->id, float( c->area ) / std::max( a->area, b->area ) );
			}
			const float score = fail.empty() ? ratio * ( 1.0f - dy / 0.1f ) * ( 1.0f - mirror / 0.08f ) * std::log( 1.0f + float( std::min( a->area, b->area ) ) ) * ( 0.75f + 0.5f * relY ) : 0.0f;
			r.log.push_back( Fmt( "  %s pair %d+%d: area ratio %.2f, height diff %.3f, mirror offset %.3f, apart %.2f, height %.0f %% -> %s", pass == 0 ? "see-through" : "mixed", a->id, b->id, ratio, dy, mirror, apart,
			                      100.0f * relY, fail.empty() ? Fmt( "score %.3f", score ).c_str() : fail.c_str() ) );
			if( score > best )
				best = score, bestA = a, bestB = b;
		}

	// band of a region = its rows at least 60 % as wide as its widest row (a T-slit's bar, a visor, a porthole's middle)
	struct Band
	{
		int x0 = kRes, x1 = -1, y0 = kRes, y1 = -1;
	};
	auto bandOf = [ & ]( const Region& h ) {
		std::vector< int > w( h.y1 - h.y0 + 1, 0 ), a( w.size(), kRes ), b( w.size(), -1 );
		for( int py = h.y0; py <= h.y1; ++py )
			for( int px = h.x0; px <= h.x1; ++px )
				if( label[ py * kRes + px ] == h.id )
					a[ py - h.y0 ] = std::min( a[ py - h.y0 ], px ), b[ py - h.y0 ] = std::max( b[ py - h.y0 ], px );
		int wmax = 0;
		for( size_t k = 0; k < w.size(); ++k )
			wmax = std::max( wmax, w[ k ] = b[ k ] >= a[ k ] ? b[ k ] - a[ k ] + 1 : 0 );
		Band band;
		for( size_t k = 0; k < w.size(); ++k )
			if( w[ k ] >= 0.6f * wmax )
				band.x0 = std::min( band.x0, a[ k ] ), band.x1 = std::max( band.x1, b[ k ] ), band.y0 = std::min( band.y0, h.y0 + int( k ) ),
				band.y1 = std::max( band.y1, h.y0 + int( k ) );
		return band;
	};

	if( bestA )
	{
		r.found = true;
		r.kind  = bestA->SeeThrough() && bestB->SeeThrough() ? "see-through" : "cavity";
		fill( *bestA, bestA->cx, bestA->cy, std::sqrt( float( bestA->area ) / 3.14159265f ), r.left );
		fill( *bestB, bestB->cx, bestB->cy, std::sqrt( float( bestB->area ) / 3.14159265f ), r.right );
		r.reason = Fmt( "eyes = %s regions %d + %d (score %.3f): left (%.3f, %.3f, %.3f) r %.3f, right (%.3f, %.3f, %.3f) r %.3f", r.kind.c_str(), bestA->id, bestB->id,
		                best, r.left.center[ 0 ], r.left.center[ 1 ], r.left.center[ 2 ], r.left.radius, r.right.center[ 0 ], r.right.center[ 1 ], r.right.center[ 2 ],
		                r.right.radius );
	}
	const Region* visor = nullptr;
	if( !bestA )
	{
		// visor split: the widest candidate that crosses the axis, with a band wider than tall and >= 15 % of the width
		Band vb;
		for( const Region* h : cands )
		{
			if( h->x0 >= axis || h->x1 <= axis )
				continue;
			const Band b = bandOf( *h );
			const int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
			const bool ok = bw >= 0.15f * silW && bw >= 1.1f * bh && bh >= std::max( 6.0f, 0.015f * silH ) && ( 0.5f * ( b.y0 + b.y1 ) - sy0 ) / silH >= 0.4f;
			r.log.push_back( Fmt( "  visor? region %d: band %d x %d px (%.0f %% of the width) -> %s", h->id, bw, bh, 100.0f * bw / silW,
			                      ok ? "yes" : bw < 0.15f * silW ? "too narrow" : bw < 1.1f * bh ? "not wider than tall" : "too thin or below 40 % of the height" ) );
			if( ok && ( !visor || bw > vb.x1 - vb.x0 + 1 ) )
				visor = h, vb = b;
		}
		if( visor )
		{
			r.found = true;
			r.kind  = "visor split";
			const float bw = float( vb.x1 - vb.x0 + 1 ), bh = float( vb.y1 - vb.y0 + 1 ), cy = 0.5f * ( vb.y0 + vb.y1 + 1 );
			const float rad = std::min( 0.5f * bh, bw / 6.0f );
			fill( *visor, vb.x0 + bw / 3.0f, cy, rad, r.left );
			fill( *visor, vb.x0 + 2.0f * bw / 3.0f, cy, rad, r.right );
			r.reason = Fmt( "eyes = visor split of %s region %d (band %.0f x %.0f px): left (%.3f, %.3f, %.3f), right (%.3f, %.3f, %.3f) r %.3f", visor->Kind(), visor->id, bw, bh,
			                r.left.center[ 0 ], r.left.center[ 1 ], r.left.center[ 2 ], r.right.center[ 0 ], r.right.center[ 1 ], r.right.center[ 2 ], r.right.radius );
		}
		else if( cands.empty() )
			r.reason = regions.empty() ? "no pair: no see-through holes and no cavities in the front view" :
			                             Fmt( "no pair: none of the %zu openings is large enough, in the upper 75 %% of the height and narrower than the model", regions.size() );
		else
			r.reason = Fmt( "no pair: none of the %zu candidate pairs is symmetric (similar size and height, mirrored about the axis) and no single opening spans the axis as a visor",
			                cands.size() * ( cands.size() - 1 ) / 2 );
	}

	if( !debugPng.empty() )
	{
		// grey = model by depth, black = background; see-through: blue rejected / yellow candidate; cavity: purple rejected /
		// orange candidate; the pair's regions red, eye centres green crosses, symmetry axis cyan
		std::vector< uint8_t > img( kRes * kRes * 3 );
		for( int py = 0; py < kRes; ++py )
			for( int px = 0; px < kRes; ++px )
			{
				const int i = py * kRes + px, o = ( ( kRes - 1 - py ) * kRes + px ) * 3; // PNG rows top-first
				uint8_t c[ 3 ] = { 0, 0, 0 };
				if( cover[ i ] )
				{
					const float zn = std::min( 1.0f, std::max( 0.0f, ( depth[ i ] - lo[ 2 ] ) / zr ) );
					c[ 0 ] = c[ 1 ] = c[ 2 ] = uint8_t( 70 + 150 * zn );
				}
				if( label[ i ] > 0 )
				{
					const Region& h = regions[ label[ i ] - 1 ];
					const bool see  = cand[ i ] == 1;
					if( &h == bestA || &h == bestB || &h == visor )
						c[ 0 ] = 255, c[ 1 ] = 40, c[ 2 ] = 40;
					else if( h.candidate )
						c[ 0 ] = 255, c[ 1 ] = see ? 220 : 140, c[ 2 ] = 0;
					else if( see )
						c[ 0 ] = 40, c[ 1 ] = 90, c[ 2 ] = 255;
					else
						c[ 0 ] = uint8_t( c[ 0 ] / 2 + 70 ), c[ 1 ] = uint8_t( c[ 1 ] / 3 ), c[ 2 ] = uint8_t( c[ 2 ] / 2 + 90 );
				}
				std::copy( c, c + 3, &img[ o ] );
			}
		auto put = [ & ]( int px, int py, uint8_t R8, uint8_t G8, uint8_t B8 ) {
			if( px >= 0 && py >= 0 && px < kRes && py < kRes )
			{
				uint8_t* o = &img[ ( ( kRes - 1 - py ) * kRes + px ) * 3 ];
				o[ 0 ] = R8, o[ 1 ] = G8, o[ 2 ] = B8;
			}
		};
		for( int py = 0; py < kRes; py += 2 )
			put( int( axis ), py, 0, 220, 255 );
		if( r.found )
			for( const EyeHole* e : { &r.left, &r.right } )
			{
				const int px = int( ( e->center[ 0 ] - ox ) * pxPer + kRes * 0.5f ), py = int( ( e->center[ 1 ] - oy ) * pxPer + kRes * 0.5f );
				for( int d = -9; d <= 9; ++d )
					for( int w = -1; w <= 1; ++w )
						put( px + d, py + w, 0, 255, 0 ), put( px + w, py + d, 0, 255, 0 );
			}
		stbi_write_png( debugPng.c_str(), kRes, kRes, 3, img.data(), kRes * 3 );
		r.log.push_back( "debug image: " + debugPng );
	}
	r.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	return r;
}

std::shared_ptr< EyeJob > RequestEyes( const std::shared_ptr< const CpuGeometry >& geometry, const EyeQuery& query, const std::string& debugPng )
{
	std::lock_guard< std::mutex > lock( g_mutex );
	for( auto it = g_jobs.begin(); it != g_jobs.end(); )
		it = it->second.expired() ? g_jobs.erase( it ) : std::next( it );
	auto& slot = g_jobs[ Fmt( "%p ", (const void*)geometry.get() ) + query.Key() ];
	if( auto job = slot.lock() )
		return job;
	auto job = std::make_shared< EyeJob >();
	slot     = job;
	++g_running;
	std::thread( [ job, geometry, query, debugPng ]() {
		job->result = FindEyes( *geometry, query, debugPng );
		job->done.store( true );
		std::lock_guard< std::mutex > l( g_mutex );
		--g_running;
		g_cv.notify_all();
	} ).detach();
	return job;
}

void ShutdownEyeFinder()
{
	std::unique_lock< std::mutex > lock( g_mutex );
	g_cv.wait( lock, [] { return g_running == 0; } );
	g_jobs.clear();
}
}
