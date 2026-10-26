#pragma once
// Minimal column-major 4x4 helpers.
#include <cmath>
#include <cstring>

namespace mehr
{
struct Mat4
{
	float m[ 16 ];
	static Mat4 Identity()
	{
		Mat4 r;
		std::memset( r.m, 0, sizeof( r.m ) );
		r.m[ 0 ] = r.m[ 5 ] = r.m[ 10 ] = r.m[ 15 ] = 1.0f;
		return r;
	}
	static Mat4 Translate( float x, float y, float z )
	{
		Mat4 r    = Identity();
		r.m[ 12 ] = x, r.m[ 13 ] = y, r.m[ 14 ] = z;
		return r;
	}
	static Mat4 Scale( float s )
	{
		Mat4 r   = Identity();
		r.m[ 0 ] = r.m[ 5 ] = r.m[ 10 ] = s;
		return r;
	}
	static Mat4 RotX( float a )
	{
		Mat4 r = Identity();
		float c = std::cos( a ), s = std::sin( a );
		r.m[ 5 ] = c, r.m[ 6 ] = s, r.m[ 9 ] = -s, r.m[ 10 ] = c;
		return r;
	}
	static Mat4 RotY( float a )
	{
		Mat4 r = Identity();
		float c = std::cos( a ), s = std::sin( a );
		r.m[ 0 ] = c, r.m[ 2 ] = -s, r.m[ 8 ] = s, r.m[ 10 ] = c;
		return r;
	}
	static Mat4 RotZ( float a )
	{
		Mat4 r = Identity();
		float c = std::cos( a ), s = std::sin( a );
		r.m[ 0 ] = c, r.m[ 1 ] = s, r.m[ 4 ] = -s, r.m[ 5 ] = c;
		return r;
	}
	static Mat4 Perspective( float fovyRad, float aspect, float zn, float zf )
	{
		Mat4 r;
		std::memset( r.m, 0, sizeof( r.m ) );
		float f   = 1.0f / std::tan( fovyRad * 0.5f );
		r.m[ 0 ]  = f / aspect;
		r.m[ 5 ]  = f;
		r.m[ 10 ] = ( zf + zn ) / ( zn - zf );
		r.m[ 11 ] = -1.0f;
		r.m[ 14 ] = 2.0f * zf * zn / ( zn - zf );
		return r;
	}
	static Mat4 Ortho( float l, float r, float b, float t, float zn, float zf )
	{
		Mat4 o    = Identity();
		o.m[ 0 ]  = 2.0f / ( r - l );
		o.m[ 5 ]  = 2.0f / ( t - b );
		o.m[ 10 ] = -2.0f / ( zf - zn );
		o.m[ 12 ] = -( r + l ) / ( r - l );
		o.m[ 13 ] = -( t + b ) / ( t - b );
		o.m[ 14 ] = -( zf + zn ) / ( zf - zn );
		return o;
	}
	static Mat4 LookAt( const float* eye, const float* at, const float* up )
	{
		float f[ 3 ] = { at[ 0 ] - eye[ 0 ], at[ 1 ] - eye[ 1 ], at[ 2 ] - eye[ 2 ] };
		Norm( f );
		float s[ 3 ];
		Cross( f, up, s );
		Norm( s );
		float u[ 3 ];
		Cross( s, f, u );
		Mat4 r    = Identity();
		r.m[ 0 ]  = s[ 0 ], r.m[ 4 ] = s[ 1 ], r.m[ 8 ] = s[ 2 ];
		r.m[ 1 ]  = u[ 0 ], r.m[ 5 ] = u[ 1 ], r.m[ 9 ] = u[ 2 ];
		r.m[ 2 ]  = -f[ 0 ], r.m[ 6 ] = -f[ 1 ], r.m[ 10 ] = -f[ 2 ];
		r.m[ 12 ] = -( s[ 0 ] * eye[ 0 ] + s[ 1 ] * eye[ 1 ] + s[ 2 ] * eye[ 2 ] );
		r.m[ 13 ] = -( u[ 0 ] * eye[ 0 ] + u[ 1 ] * eye[ 1 ] + u[ 2 ] * eye[ 2 ] );
		r.m[ 14 ] = f[ 0 ] * eye[ 0 ] + f[ 1 ] * eye[ 1 ] + f[ 2 ] * eye[ 2 ];
		return r;
	}
	Mat4 operator*( const Mat4& b ) const
	{
		Mat4 r;
		for( int c = 0; c < 4; ++c )
			for( int rr = 0; rr < 4; ++rr )
			{
				float v = 0;
				for( int k = 0; k < 4; ++k )
					v += m[ k * 4 + rr ] * b.m[ c * 4 + k ];
				r.m[ c * 4 + rr ] = v;
			}
		return r;
	}
	Mat4 Inverse() const
	{
		const float* a = m;
		float inv[ 16 ];
		inv[ 0 ]  = a[ 5 ] * a[ 10 ] * a[ 15 ] - a[ 5 ] * a[ 11 ] * a[ 14 ] - a[ 9 ] * a[ 6 ] * a[ 15 ] + a[ 9 ] * a[ 7 ] * a[ 14 ] + a[ 13 ] * a[ 6 ] * a[ 11 ] - a[ 13 ] * a[ 7 ] * a[ 10 ];
		inv[ 4 ]  = -a[ 4 ] * a[ 10 ] * a[ 15 ] + a[ 4 ] * a[ 11 ] * a[ 14 ] + a[ 8 ] * a[ 6 ] * a[ 15 ] - a[ 8 ] * a[ 7 ] * a[ 14 ] - a[ 12 ] * a[ 6 ] * a[ 11 ] + a[ 12 ] * a[ 7 ] * a[ 10 ];
		inv[ 8 ]  = a[ 4 ] * a[ 9 ] * a[ 15 ] - a[ 4 ] * a[ 11 ] * a[ 13 ] - a[ 8 ] * a[ 5 ] * a[ 15 ] + a[ 8 ] * a[ 7 ] * a[ 13 ] + a[ 12 ] * a[ 5 ] * a[ 11 ] - a[ 12 ] * a[ 7 ] * a[ 9 ];
		inv[ 12 ] = -a[ 4 ] * a[ 9 ] * a[ 14 ] + a[ 4 ] * a[ 10 ] * a[ 13 ] + a[ 8 ] * a[ 5 ] * a[ 14 ] - a[ 8 ] * a[ 6 ] * a[ 13 ] - a[ 12 ] * a[ 5 ] * a[ 10 ] + a[ 12 ] * a[ 6 ] * a[ 9 ];
		inv[ 1 ]  = -a[ 1 ] * a[ 10 ] * a[ 15 ] + a[ 1 ] * a[ 11 ] * a[ 14 ] + a[ 9 ] * a[ 2 ] * a[ 15 ] - a[ 9 ] * a[ 3 ] * a[ 14 ] - a[ 13 ] * a[ 2 ] * a[ 11 ] + a[ 13 ] * a[ 3 ] * a[ 10 ];
		inv[ 5 ]  = a[ 0 ] * a[ 10 ] * a[ 15 ] - a[ 0 ] * a[ 11 ] * a[ 14 ] - a[ 8 ] * a[ 2 ] * a[ 15 ] + a[ 8 ] * a[ 3 ] * a[ 14 ] + a[ 12 ] * a[ 2 ] * a[ 11 ] - a[ 12 ] * a[ 3 ] * a[ 10 ];
		inv[ 9 ]  = -a[ 0 ] * a[ 9 ] * a[ 15 ] + a[ 0 ] * a[ 11 ] * a[ 13 ] + a[ 8 ] * a[ 1 ] * a[ 15 ] - a[ 8 ] * a[ 3 ] * a[ 13 ] - a[ 12 ] * a[ 1 ] * a[ 11 ] + a[ 12 ] * a[ 3 ] * a[ 9 ];
		inv[ 13 ] = a[ 0 ] * a[ 9 ] * a[ 14 ] - a[ 0 ] * a[ 10 ] * a[ 13 ] - a[ 8 ] * a[ 1 ] * a[ 14 ] + a[ 8 ] * a[ 2 ] * a[ 13 ] + a[ 12 ] * a[ 1 ] * a[ 10 ] - a[ 12 ] * a[ 2 ] * a[ 9 ];
		inv[ 2 ]  = a[ 1 ] * a[ 6 ] * a[ 15 ] - a[ 1 ] * a[ 7 ] * a[ 14 ] - a[ 5 ] * a[ 2 ] * a[ 15 ] + a[ 5 ] * a[ 3 ] * a[ 14 ] + a[ 13 ] * a[ 2 ] * a[ 7 ] - a[ 13 ] * a[ 3 ] * a[ 6 ];
		inv[ 6 ]  = -a[ 0 ] * a[ 6 ] * a[ 15 ] + a[ 0 ] * a[ 7 ] * a[ 14 ] + a[ 4 ] * a[ 2 ] * a[ 15 ] - a[ 4 ] * a[ 3 ] * a[ 14 ] - a[ 12 ] * a[ 2 ] * a[ 7 ] + a[ 12 ] * a[ 3 ] * a[ 6 ];
		inv[ 10 ] = a[ 0 ] * a[ 5 ] * a[ 15 ] - a[ 0 ] * a[ 7 ] * a[ 13 ] - a[ 4 ] * a[ 1 ] * a[ 15 ] + a[ 4 ] * a[ 3 ] * a[ 13 ] + a[ 12 ] * a[ 1 ] * a[ 7 ] - a[ 12 ] * a[ 3 ] * a[ 5 ];
		inv[ 14 ] = -a[ 0 ] * a[ 5 ] * a[ 14 ] + a[ 0 ] * a[ 6 ] * a[ 13 ] + a[ 4 ] * a[ 1 ] * a[ 14 ] - a[ 4 ] * a[ 2 ] * a[ 13 ] - a[ 12 ] * a[ 1 ] * a[ 6 ] + a[ 12 ] * a[ 2 ] * a[ 5 ];
		inv[ 3 ]  = -a[ 1 ] * a[ 6 ] * a[ 11 ] + a[ 1 ] * a[ 7 ] * a[ 10 ] + a[ 5 ] * a[ 2 ] * a[ 11 ] - a[ 5 ] * a[ 3 ] * a[ 10 ] - a[ 9 ] * a[ 2 ] * a[ 7 ] + a[ 9 ] * a[ 3 ] * a[ 6 ];
		inv[ 7 ]  = a[ 0 ] * a[ 6 ] * a[ 11 ] - a[ 0 ] * a[ 7 ] * a[ 10 ] - a[ 4 ] * a[ 2 ] * a[ 11 ] + a[ 4 ] * a[ 3 ] * a[ 10 ] + a[ 8 ] * a[ 2 ] * a[ 7 ] - a[ 8 ] * a[ 3 ] * a[ 6 ];
		inv[ 11 ] = -a[ 0 ] * a[ 5 ] * a[ 11 ] + a[ 0 ] * a[ 7 ] * a[ 9 ] + a[ 4 ] * a[ 1 ] * a[ 11 ] - a[ 4 ] * a[ 3 ] * a[ 9 ] - a[ 8 ] * a[ 1 ] * a[ 7 ] + a[ 8 ] * a[ 3 ] * a[ 5 ];
		inv[ 15 ] = a[ 0 ] * a[ 5 ] * a[ 10 ] - a[ 0 ] * a[ 6 ] * a[ 9 ] - a[ 4 ] * a[ 1 ] * a[ 10 ] + a[ 4 ] * a[ 2 ] * a[ 9 ] + a[ 8 ] * a[ 1 ] * a[ 6 ] - a[ 8 ] * a[ 2 ] * a[ 5 ];
		float det = a[ 0 ] * inv[ 0 ] + a[ 1 ] * inv[ 4 ] + a[ 2 ] * inv[ 8 ] + a[ 3 ] * inv[ 12 ];
		float id  = std::fabs( det ) > 1e-30f ? 1.0f / det : 0.0f;
		Mat4 r;
		for( int i = 0; i < 16; ++i )
			r.m[ i ] = inv[ i ] * id;
		return r;
	}
	// w = 1 point transform
	void Point( const float* p, float* out ) const
	{
		for( int r = 0; r < 3; ++r )
			out[ r ] = m[ r ] * p[ 0 ] + m[ 4 + r ] * p[ 1 ] + m[ 8 + r ] * p[ 2 ] + m[ 12 + r ];
	}
	// w = 0 direction transform
	void Dir( const float* p, float* out ) const
	{
		for( int r = 0; r < 3; ++r )
			out[ r ] = m[ r ] * p[ 0 ] + m[ 4 + r ] * p[ 1 ] + m[ 8 + r ] * p[ 2 ];
	}
	// inverse-transpose of the upper 3x3, column-major 3x3 output
	void NormalMatrix( float* out9 ) const
	{
		float a = m[ 0 ], b = m[ 4 ], c = m[ 8 ];
		float d = m[ 1 ], e = m[ 5 ], f = m[ 9 ];
		float g = m[ 2 ], h = m[ 6 ], i = m[ 10 ];
		float A = e * i - f * h, B = -( d * i - f * g ), C = d * h - e * g;
		float det = a * A + b * B + c * C;
		float id  = std::fabs( det ) > 1e-30f ? 1.0f / det : 0.0f;
		// cofactor / det = inverse-transpose
		float M[ 3 ][ 3 ] = {
			{ A * id, B * id, C * id },
			{ -( b * i - c * h ) * id, ( a * i - c * g ) * id, -( a * h - b * g ) * id },
			{ ( b * f - c * e ) * id, -( a * f - c * d ) * id, ( a * e - b * d ) * id },
		};
		// M[row][col] is the inverse-transpose element; store column-major
		for( int col = 0; col < 3; ++col )
			for( int row = 0; row < 3; ++row )
				out9[ col * 3 + row ] = M[ row ][ col ];
	}
	static void Cross( const float* a, const float* b, float* o )
	{
		o[ 0 ] = a[ 1 ] * b[ 2 ] - a[ 2 ] * b[ 1 ];
		o[ 1 ] = a[ 2 ] * b[ 0 ] - a[ 0 ] * b[ 2 ];
		o[ 2 ] = a[ 0 ] * b[ 1 ] - a[ 1 ] * b[ 0 ];
	}
	static void Norm( float* v )
	{
		float l = std::sqrt( v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ] + v[ 2 ] * v[ 2 ] );
		if( l > 0 )
			v[ 0 ] /= l, v[ 1 ] /= l, v[ 2 ] /= l;
	}
};
// the main fragment shader's Hash3 / Noise3 / Fbm on the CPU, in float (the W16 Dust key per point, the W7 swap quantiles)
inline float FractF( float x ) { return x - std::floor( x ); }
inline float Hash3F( float x, float y, float z )
{
	x = FractF( x * 0.3183099f + 0.1f ) * 17.0f, y = FractF( y * 0.3183099f + 0.1f ) * 17.0f, z = FractF( z * 0.3183099f + 0.1f ) * 17.0f;
	return FractF( x * y * z * ( x + y + z ) );
}
inline float Noise3F( const float* p )
{
	float i[ 3 ], f[ 3 ];
	for( int k = 0; k < 3; ++k )
		i[ k ] = std::floor( p[ k ] ), f[ k ] = p[ k ] - i[ k ], f[ k ] = f[ k ] * f[ k ] * ( 3.0f - 2.0f * f[ k ] );
	auto h   = [ & ]( float a, float b, float c ) { return Hash3F( i[ 0 ] + a, i[ 1 ] + b, i[ 2 ] + c ); };
	auto mix = []( float a, float b, float t ) { return a + ( b - a ) * t; };
	return mix( mix( mix( h( 0, 0, 0 ), h( 1, 0, 0 ), f[ 0 ] ), mix( h( 0, 1, 0 ), h( 1, 1, 0 ), f[ 0 ] ), f[ 1 ] ),
	            mix( mix( h( 0, 0, 1 ), h( 1, 0, 1 ), f[ 0 ] ), mix( h( 0, 1, 1 ), h( 1, 1, 1 ), f[ 0 ] ), f[ 1 ] ), f[ 2 ] );
}
inline float FbmF( float x, float y, float z )
{
	float p[ 3 ] = { x, y, z }, v = 0, a = 0.5f;
	for( int o = 0; o < 4; ++o )
	{
		v += a * Noise3F( p );
		for( float& c : p )
			c *= 2.03f;
		a *= 0.5f;
	}
	return v / 0.9375f;
}

}
