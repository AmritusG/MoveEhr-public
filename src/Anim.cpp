// MoveEhr W1: clip sampling, poses and palettes (CPU). See Anim.h for the spaces and the one deformation path.
#include "Anim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mehr
{
namespace
{
void MatMul( const double* a, const double* b, double* o ) // column-major 4x4: o = a x b
{
	double r[ 16 ];
	for( int c = 0; c < 4; ++c )
		for( int rr = 0; rr < 4; ++rr )
		{
			double s = 0;
			for( int k = 0; k < 4; ++k )
				s += a[ k * 4 + rr ] * b[ c * 4 + k ];
			r[ c * 4 + rr ] = s;
		}
	std::memcpy( o, r, sizeof r );
}
void Identity( double* m )
{
	std::memset( m, 0, 16 * sizeof( double ) );
	m[ 0 ] = m[ 5 ] = m[ 10 ] = m[ 15 ] = 1;
}
void Trs( const double* t, const double* q, const double* s, double* m ) // column-major T x R x S
{
	const double x = q[ 0 ], y = q[ 1 ], z = q[ 2 ], w = q[ 3 ];
	const double R[ 9 ] = { 1 - 2 * ( y * y + z * z ), 2 * ( x * y + z * w ), 2 * ( x * z - y * w ),   // column 0
		                    2 * ( x * y - z * w ), 1 - 2 * ( x * x + z * z ), 2 * ( y * z + x * w ),   // column 1
		                    2 * ( x * z + y * w ), 2 * ( y * z - x * w ), 1 - 2 * ( x * x + y * y ) }; // column 2
	for( int c = 0; c < 3; ++c )
	{
		for( int r = 0; r < 3; ++r )
			m[ c * 4 + r ] = R[ c * 3 + r ] * s[ c ];
		m[ c * 4 + 3 ] = 0;
	}
	m[ 12 ] = t[ 0 ], m[ 13 ] = t[ 1 ], m[ 14 ] = t[ 2 ], m[ 15 ] = 1;
}
void NormalizeQ( double* q )
{
	const double l = std::sqrt( q[ 0 ] * q[ 0 ] + q[ 1 ] * q[ 1 ] + q[ 2 ] * q[ 2 ] + q[ 3 ] * q[ 3 ] );
	if( l > 1e-30 )
		for( int k = 0; k < 4; ++k )
			q[ k ] /= l;
	else
		q[ 0 ] = q[ 1 ] = q[ 2 ] = 0, q[ 3 ] = 1;
}
void Slerp( const double* a, const double* b0, double f, double* o )
{
	double b[ 4 ] = { b0[ 0 ], b0[ 1 ], b0[ 2 ], b0[ 3 ] };
	double d      = a[ 0 ] * b[ 0 ] + a[ 1 ] * b[ 1 ] + a[ 2 ] * b[ 2 ] + a[ 3 ] * b[ 3 ];
	if( d < 0 )
		d = -d, b[ 0 ] = -b[ 0 ], b[ 1 ] = -b[ 1 ], b[ 2 ] = -b[ 2 ], b[ 3 ] = -b[ 3 ];
	double ka = 1 - f, kb = f;
	if( d < 0.9995 )
	{
		const double th = std::acos( std::min( 1.0, d ) ), s = std::sin( th );
		ka = std::sin( ( 1 - f ) * th ) / s, kb = std::sin( f * th ) / s;
	}
	for( int k = 0; k < 4; ++k )
		o[ k ] = ka * a[ k ] + kb * b[ k ];
	NormalizeQ( o );
}
// Inverse of the upper-left 3x3 of a column-major 4x4 (out: column-major 3x3). false = singular.
bool Inverse3( const double* m, double* o )
{
	const double a = m[ 0 ], b = m[ 4 ], c = m[ 8 ], d = m[ 1 ], e = m[ 5 ], f = m[ 9 ], g = m[ 2 ], h = m[ 6 ], i = m[ 10 ];
	const double A = e * i - f * h, B = -( d * i - f * g ), C = d * h - e * g;
	const double det = a * A + b * B + c * C;
	if( std::fabs( det ) < 1e-30 )
		return false;
	const double id = 1 / det;
	// row-major inverse, stored column-major
	const double R[ 9 ] = { A * id, -( b * i - c * h ) * id, ( b * f - c * e ) * id, B * id, ( a * i - c * g ) * id, -( a * f - c * d ) * id,
		                    C * id, -( a * h - b * g ) * id, ( a * e - b * d ) * id };
	for( int r = 0; r < 3; ++r )
		for( int cc = 0; cc < 3; ++cc )
			o[ cc * 3 + r ] = R[ r * 3 + cc ];
	return true;
}

// sample one channel at file time T into `out` (comps values)
void SampleChannel( const AnimChannel& ch, double T, double* out )
{
	const int n = (int)ch.times.size(), c = ch.comps;
	const bool cubic = ch.interp == AI_CUBIC;
	auto val = [ & ]( int k, int part ) -> const float* { // part: 0 in-tangent, 1 value, 2 out-tangent (cubic); value only otherwise
		return cubic ? &ch.values[ ( k * 3 + part ) * c ] : &ch.values[ k * c ];
	};
	if( n == 0 )
		return;
	if( n == 1 || T <= ch.times[ 0 ] )
	{
		const float* v = val( 0, 1 );
		for( int i = 0; i < c; ++i )
			out[ i ] = v[ i ];
		return;
	}
	if( T >= ch.times[ n - 1 ] )
	{
		const float* v = val( n - 1, 1 );
		for( int i = 0; i < c; ++i )
			out[ i ] = v[ i ];
		return;
	}
	const int k     = int( std::upper_bound( ch.times.begin(), ch.times.end(), (float)T ) - ch.times.begin() ) - 1;
	const int kk    = std::max( 0, std::min( n - 2, k ) );
	const double t0 = ch.times[ kk ], t1 = ch.times[ kk + 1 ], td = t1 - t0;
	const double u  = td > 0 ? std::min( 1.0, std::max( 0.0, ( T - t0 ) / td ) ) : 0.0;
	if( ch.interp == AI_STEP )
	{
		const float* v = val( kk, 1 );
		for( int i = 0; i < c; ++i )
			out[ i ] = v[ i ];
		return;
	}
	if( cubic )
	{
		const double u2 = u * u, u3 = u2 * u;
		const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
		const float *v0 = val( kk, 1 ), *b0 = val( kk, 2 ), *v1 = val( kk + 1, 1 ), *a1 = val( kk + 1, 0 );
		for( int i = 0; i < c; ++i )
			out[ i ] = h00 * v0[ i ] + h10 * td * b0[ i ] + h01 * v1[ i ] + h11 * td * a1[ i ];
		if( ch.path == AP_R )
			NormalizeQ( out );
		return;
	}
	const float *v0 = val( kk, 1 ), *v1 = val( kk + 1, 1 );
	if( ch.path == AP_R )
	{
		const double a[ 4 ] = { v0[ 0 ], v0[ 1 ], v0[ 2 ], v0[ 3 ] }, b[ 4 ] = { v1[ 0 ], v1[ 1 ], v1[ 2 ], v1[ 3 ] };
		Slerp( a, b, u, out );
		return;
	}
	for( int i = 0; i < c; ++i )
		out[ i ] = v0[ i ] + ( v1[ i ] - v0[ i ] ) * u;
}

void ApplyChannel( const AnimData& a, const AnimChannel& ch, double T, AnimPose& out )
{
	double v[ 64 ];
	if( ch.comps > 64 || ( ch.weightIndex < 0 && ( ch.node < 0 || ch.node >= (int)a.nodes.size() ) ) )
		return;
	SampleChannel( ch, T, v );
	double* trs = ch.node >= 0 ? &out.trs[ ch.node * 10 ] : nullptr;
	if( !trs && ch.path != AP_W )
		return;
	switch( ch.path )
	{
	case AP_T: trs[ 0 ] = v[ 0 ], trs[ 1 ] = v[ 1 ], trs[ 2 ] = v[ 2 ]; break;
	case AP_R: trs[ 3 ] = v[ 0 ], trs[ 4 ] = v[ 1 ], trs[ 5 ] = v[ 2 ], trs[ 6 ] = v[ 3 ]; break;
	case AP_S: trs[ 7 ] = v[ 0 ], trs[ 8 ] = v[ 1 ], trs[ 9 ] = v[ 2 ]; break;
	case AP_W:
	{
		if( ch.weightIndex >= 0 )
		{
			if( ch.weightIndex < (int)out.weights.size() )
				out.weights[ ch.weightIndex ] = (float)v[ 0 ];
			break;
		}
		const AnimNode& n = a.nodes[ ch.node ];
		for( int i = 0; i < std::min( n.weightCount, ch.comps ); ++i )
			out.weights[ n.weightBase + i ] = (float)v[ i ];
		break;
	}
	}
}
}

void RestPose( const AnimData& a, AnimPose& out )
{
	out.trs.resize( a.nodes.size() * 10 );
	for( size_t i = 0; i < a.nodes.size(); ++i )
	{
		const AnimNode& n = a.nodes[ i ];
		double* t         = &out.trs[ i * 10 ];
		std::memcpy( t, n.t, 3 * sizeof( double ) );
		std::memcpy( t + 3, n.r, 4 * sizeof( double ) );
		std::memcpy( t + 7, n.s, 3 * sizeof( double ) );
	}
	out.weights = a.restWeights;
}

void SampleClip( const AnimData& a, int clip, double t, bool rootInPlace, AnimPose& out )
{
	RestPose( a, out );
	if( clip < 0 || clip >= (int)a.clips.size() )
		return;
	const AnimClip& c = a.clips[ clip ];
	const double T    = c.start + std::max( 0.0, std::min( c.duration, t ) );
	for( const AnimChannel& ch : c.channels )
		ApplyChannel( a, ch, T, out );
	if( rootInPlace && c.rootNode >= 0 )
	{
		// pin X / Z of the root to its value at the clip's start (a walk stays where it is framed; height kept)
		for( const AnimChannel& ch : c.channels )
			if( ch.node == c.rootNode && ch.path == AP_T )
			{
				double v0[ 3 ];
				SampleChannel( ch, c.start, v0 );
				out.trs[ c.rootNode * 10 + 0 ] = v0[ 0 ];
				out.trs[ c.rootNode * 10 + 2 ] = v0[ 2 ];
			}
	}
}

void BlendPose( const AnimPose& a, const AnimPose& b, double f, AnimPose& out )
{
	const size_t nodes = std::min( a.trs.size(), b.trs.size() ) / 10;
	out.trs.resize( nodes * 10 );
	for( size_t i = 0; i < nodes; ++i )
	{
		const double *x = &a.trs[ i * 10 ], *y = &b.trs[ i * 10 ];
		double* o       = &out.trs[ i * 10 ];
		for( int k : { 0, 1, 2, 7, 8, 9 } )
			o[ k ] = x[ k ] + ( y[ k ] - x[ k ] ) * f;
		Slerp( x + 3, y + 3, f, o + 3 );
	}
	const size_t nw = std::min( a.weights.size(), b.weights.size() );
	out.weights.resize( nw );
	for( size_t i = 0; i < nw; ++i )
		out.weights[ i ] = float( a.weights[ i ] + ( b.weights[ i ] - a.weights[ i ] ) * f );
}

void JointMatrices( const AnimData& a, const AnimPose& pose, std::vector< double >& J )
{
	const size_t nn = a.nodes.size();
	std::vector< double > world( nn * 16 );
	std::vector< char > done( nn, 0 );
	// parents first: resolve each node's chain (memoised; files may list children before parents)
	std::vector< int > chain;
	for( size_t i = 0; i < nn; ++i )
	{
		if( done[ i ] )
			continue;
		chain.clear();
		for( int n = (int)i; n >= 0 && !done[ n ]; n = a.nodes[ n ].parent )
			chain.push_back( n );
		for( auto it = chain.rbegin(); it != chain.rend(); ++it )
		{
			const int n       = *it;
			const AnimNode& d = a.nodes[ n ];
			double local[ 16 ];
			if( d.hasMatrix )
				std::memcpy( local, d.m, sizeof local );
			else
			{
				const double* t = &pose.trs[ n * 10 ];
				double q[ 4 ]   = { t[ 3 ], t[ 4 ], t[ 5 ], t[ 6 ] };
				NormalizeQ( q );
				Trs( t, q, t + 7, local );
			}
			if( d.parent >= 0 )
				MatMul( &world[ d.parent * 16 ], local, &world[ n * 16 ] );
			else
				std::memcpy( &world[ n * 16 ], local, sizeof local );
			done[ n ] = 1;
		}
	}
	double O[ 16 ];
	Identity( O );
	for( int r = 0; r < 3; ++r )
		for( int c = 0; c < 3; ++c )
			O[ c * 4 + r ] = a.orient[ r * 3 + c ];
	J.resize( a.joints.size() * 16 );
	for( size_t j = 0; j < a.joints.size(); ++j )
	{
		const AnimJoint& jt = a.joints[ j ];
		double m[ 16 ];
		if( jt.node >= 0 )
			MatMul( &world[ jt.node * 16 ], jt.bind, m );
		else
			std::memcpy( m, jt.bind, sizeof m );
		MatMul( O, m, &J[ j * 16 ] );
	}
}

void PosePalette( const AnimData& a, const std::vector< double >& J, const std::vector< double >& Jrest, std::vector< float >& palette )
{
	const size_t nj = a.joints.size();
	palette.assign( nj * 24, 0.0f );
	for( size_t j = 0; j < nj; ++j )
	{
		const double* m = &J[ j * 16 ];
		float* o        = &palette[ j * 24 ];
		for( int r = 0; r < 3; ++r )
			for( int c = 0; c < 4; ++c )
				o[ r * 4 + c ] = (float)m[ c * 4 + r ];
		double inv[ 9 ];
		if( !Inverse3( &Jrest[ j * 16 ], inv ) )
		{
			o[ 12 ] = o[ 17 ] = o[ 22 ] = 1.0f; // D = identity
			continue;
		}
		for( int r = 0; r < 3; ++r )
			for( int c = 0; c < 3; ++c )
			{
				double s = 0;
				for( int k = 0; k < 3; ++k )
					s += m[ k * 4 + r ] * inv[ c * 3 + k ];
				o[ 12 + r * 4 + c ] = (float)s;
			}
	}
}

void MarkDuplicateTakes( AnimData& a, double fitScale )
{
	// motion = posed vertices (up to 4096, evenly picked) at 17 times, in fitted units; a take's own motion = its largest move away from
	// its first sample
	constexpr int kN = 17;
	const size_t step = std::max< size_t >( 1, a.verts.size() / 4096 );
	std::vector< std::vector< float > > Ps( a.clips.size() * kN );
	std::vector< double > own( a.clips.size(), 0.0 );
	auto sampled = [ & ]( size_t c ) -> std::vector< float >* {
		std::vector< float >* P = &Ps[ c * kN ];
		if( P[ 0 ].empty() )
		{
			AnimPose pose;
			std::vector< double > J;
			for( int k = 0; k < kN; ++k )
			{
				SampleClip( a, (int)c, a.clips[ c ].duration * k / ( kN - 1 ), false, pose );
				JointMatrices( a, pose, J );
				for( size_t v = 0; v < a.verts.size(); v += step )
				{
					double o[ 3 ];
					CpuSkinVertex( a, J, pose.weights, v, o );
					P[ k ].insert( P[ k ].end(), { float( o[ 0 ] * fitScale ), float( o[ 1 ] * fitScale ), float( o[ 2 ] * fitScale ) } );
				}
				for( size_t e = 0; e < P[ k ].size(); ++e )
					own[ c ] = std::max( own[ c ], (double)std::fabs( P[ k ][ e ] - P[ 0 ][ e ] ) );
			}
		}
		return P;
	};
	auto apart = [ & ]( size_t x, size_t y ) {
		const std::vector< float >*P = sampled( x ), *Q = sampled( y );
		double d = 0;
		for( int k = 0; k < kN; ++k )
			for( size_t e = 0; e < P[ k ].size(); ++e )
				d = std::max( d, (double)std::fabs( P[ k ][ e ] - Q[ k ][ e ] ) );
		return d;
	};
	for( AnimClip& c : a.clips )
	{
		c.moving = 0, c.listed = true;
		for( const AnimChannel& ch : c.channels )
		{
			const size_t stride = size_t( ch.comps ) * ( ch.interp == AI_CUBIC ? 3 : 1 ), off = ch.interp == AI_CUBIC ? size_t( ch.comps ) : 0;
			bool moves          = false;
			for( size_t k = 1; k < ch.times.size() && !moves; ++k )
				for( int q = 0; q < ch.comps && !moves; ++q )
					moves = std::fabs( ch.values[ k * stride + off + q ] - ch.values[ off + q ] ) > 1e-5f;
			c.moving += moves;
		}
	}
	static const bool trace = getenv( "MOVEEHR_TAKE_DIFF" ) != nullptr;
	for( size_t i = 0; i < a.clips.size(); ++i )
		for( size_t j = 0; j < i; ++j )
		{
			AnimClip &c = a.clips[ i ], &o = a.clips[ j ];
			if( o.name != c.name || std::fabs( o.duration - c.duration ) > 1e-6 || !o.listed || !c.listed )
				continue;
			const double d = apart( j, i );
			// the same motion twice (apart < 0.005 fitted = 0.25 % of the fitted span of 2; the CC0 mechs' copies are <= 0.0039 apart): the later
			// copy goes; a near-empty copy (< 10 % of the other's motion) goes
			if( d < 0.005 )
				c.listed = false;
			else if( own[ i ] < 0.1 * own[ j ] )
				c.listed = false;
			else if( own[ j ] < 0.1 * own[ i ] )
				o.listed = false;
			if( trace )
				fprintf( stderr, "take %zu vs %zu '%s': own motion %.4g / %.4g fitted, apart %.4g -> %s\n", j, i, c.name.c_str(), own[ j ], own[ i ], d,
				         !c.listed ? "later unlisted" : !o.listed ? "earlier unlisted" : "both listed" );
		}
}

void CpuSkinVertex( const AnimData& a, const std::vector< double >& J, const std::vector< float >& weights, size_t v, double out[ 3 ] )
{
	const AnimVertex& av = a.verts[ v ];
	double p[ 3 ]        = { av.p[ 0 ], av.p[ 1 ], av.p[ 2 ] };
	for( uint32_t k = 0; k < av.morph[ 2 ]; ++k )
	{
		const float w = weights[ av.morph[ 3 ] + k ];
		if( w == 0.0f )
			continue;
		const float* d = &a.morphDeltas[ size_t( av.morph[ 0 ] + k * av.morph[ 1 ] ) * 8 ];
		for( int c = 0; c < 3; ++c )
			p[ c ] += w * d[ c ];
	}
	out[ 0 ] = out[ 1 ] = out[ 2 ] = 0;
	for( int i = 0; i < kMaxInfluences; ++i )
	{
		if( av.w[ i ] == 0.0f )
			continue;
		const double* m = &J[ av.j[ i ] * 16 ];
		for( int r = 0; r < 3; ++r )
			out[ r ] += av.w[ i ] * ( m[ r ] * p[ 0 ] + m[ 4 + r ] * p[ 1 ] + m[ 8 + r ] * p[ 2 ] + m[ 12 + r ] );
	}
}
}
