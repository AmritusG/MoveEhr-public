#pragma once
// W26 Stage 4: the liquid morph A -> B. During the Liquid transition the two models' SDFs (W20 SdfBuilder, 64^3, raw units,
// sampled through each model's inverse matrix) are raymarched as ONE blended field into the main MSAA target, with depth:
//   linear = mix( dA, dB, e )                      (the shape morph)
//   goo    = smin( dA + e S, dB + (1 - e) S, k )    (A shrinks while B grows out of it, joined by a smooth-union bridge)
//   d      = mix( linear, goo, Liquid Goo ) - bulge + wobble, + shrink near both ends (the liquid starts and ends inside the
//            meshes, which dissolve into / out of it: no pop at 0 and 1).
// Look: Chrome (studio reflections, Liquid tint picker) or Paint (the two meshes' own shaded colours, captured from the camera
// at half resolution and blended by e, gaps filled from wider rings: their paint flows with the liquid).

namespace mehr
{
static const char* kLiquidFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler3D uSdfA;
uniform sampler3D uSdfB;
uniform sampler2D uPaintA;
uniform sampler2D uPaintB;
uniform mat4 uInvA, uInvB;      // world -> raw
uniform vec3 uMinA, uMinB;
uniform float uSizeA, uSizeB;   // SDF box edge, raw
uniform float uScaleA, uScaleB; // raw -> world
uniform mat4 uInvViewProj, uViewProj;
uniform vec3 uCamPos;
uniform vec3 uCenter;           // bounds: a sphere round both models
uniform float uRadius;
uniform float uE, uGoo, uGrow, uShrink, uBulge, uWobble, uPhase, uUnit;
uniform int uLook;              // 0 chrome, 1 paint
uniform vec3 uTint;
uniform float uTintMix;
uniform vec3 uKeyDir, uKeyColor;
uniform float uEnvIntensity, uEnvRotate;
uniform vec2 uTexel;            // paint capture texel

float Box( sampler3D t, mat4 inv, vec3 mn, float sz, float sc, vec3 p )
{
	vec3 q = ( inv * vec4( p, 1.0 ) ).xyz;
	vec3 u = ( q - mn ) / sz;
	vec3 c = clamp( u, vec3( 0.0 ), vec3( 1.0 ) );
	return ( texture( t, c ).r + length( ( u - c ) * sz ) ) * sc;
}
float SMin( float a, float b, float k )
{
	float h = clamp( 0.5 + 0.5 * ( b - a ) / k, 0.0, 1.0 );
	return mix( b, a, h ) - k * h * ( 1.0 - h );
}
float Field( vec3 p )
{
	float a   = Box( uSdfA, uInvA, uMinA, uSizeA, uScaleA, p );
	float b   = Box( uSdfB, uInvB, uMinB, uSizeB, uScaleB, p );
	float lin = mix( a, b, uE );
	float goo = SMin( a + uE * uGrow, b + ( 1.0 - uE ) * uGrow, max( 1.5 * uGrow * uGoo + 0.02 * uUnit, 1e-4 ) );
	float d   = mix( lin, goo, uGoo ) - uBulge;
	// a liquid wobble: travelling ripples (whole cycles: uPhase wraps at 1)
	vec3 q = p / max( uUnit, 1e-4 );
	d += uWobble * 0.025 * uUnit * sin( 6.2831853 * ( 2.0 * q.y + 1.3 * q.x - uPhase ) ) * sin( 6.2831853 * ( 1.7 * q.z - 0.9 * q.y + 2.0 * uPhase ) );
	return d + uShrink;
}
vec3 Grad( vec3 p, float h )
{
	vec2 e = vec2( h, 0.0 );
	return vec3( Field( p + e.xyy ) - Field( p - e.xyy ), Field( p + e.yxy ) - Field( p - e.yxy ), Field( p + e.yyx ) - Field( p - e.yyx ) );
}
float Softbox( vec3 d, vec3 c, vec2 hs, float r )
{
	float facing = dot( d, c );
	if( facing <= 0.0 )
		return 0.0;
	vec3 t     = normalize( abs( c.y ) > 0.99 ? cross( c, vec3( 1, 0, 0 ) ) : cross( c, vec3( 0, 1, 0 ) ) );
	vec3 b     = cross( t, c );
	vec2 p     = vec2( dot( d, t ), dot( d, b ) ) / facing;
	float blur = 0.015 + r * r * 1.6;
	float s    = ( 1.0 - smoothstep( hs.x - blur, hs.x + blur, abs( p.x ) ) ) * ( 1.0 - smoothstep( hs.y - blur, hs.y + blur, abs( p.y ) ) );
	return s * ( hs.x * hs.y ) / ( ( hs.x + blur ) * ( hs.y + blur ) );
}
// the main shader's procedural studio (a copy: the main programs' source stays untouched)
vec3 Studio( vec3 d, float r )
{
	float ca = cos( uEnvRotate ), sa = sin( uEnvRotate );
	d        = vec3( ca * d.x + sa * d.z, d.y, -sa * d.x + ca * d.z );
	vec3 col = mix( vec3( 0.015, 0.015, 0.018 ), vec3( 0.16, 0.17, 0.19 ), smoothstep( -0.25, 0.7, d.y ) );
	col += vec3( 0.06 ) * ( 1.0 - smoothstep( 0.0, 0.12 + r * 0.3, abs( d.y + 0.02 ) ) );
	col += Softbox( d, normalize( vec3( -0.65, 0.55, 0.55 ) ), vec2( 0.45, 0.3 ), r ) * vec3( 7.0, 6.8, 6.4 );
	col += Softbox( d, normalize( vec3( 0.85, 0.15, 0.25 ) ), vec2( 0.07, 0.5 ), r ) * vec3( 4.5, 4.7, 5.2 );
	col += Softbox( d, normalize( vec3( 0.1, 1.0, -0.1 ) ), vec2( 0.35, 0.35 ), r ) * vec3( 2.2 );
	col += Softbox( d, normalize( vec3( 0.2, 0.1, -1.0 ) ), vec2( 0.6, 0.12 ), r ) * vec3( 1.6, 1.4, 1.2 );
	return col;
}
// the paint under this pixel, gaps filled from rings further out (premultiplied captures: weight by coverage)
vec3 Paint( sampler2D t, vec2 uv, out float cover )
{
	vec4 c = texture( t, uv );
	for( int r = 1; r <= 4 && c.a < 0.5; ++r )
		for( int k = 0; k < 8; ++k )
		{
			float a = 6.2831853 * ( float( k ) + 0.5 * float( r ) ) / 8.0;
			c += texture( t, uv + vec2( cos( a ), sin( a ) ) * uTexel * float( r * r * 6 ) ) * 0.35;
		}
	cover = clamp( c.a, 0.0, 1.0 );
	return c.a > 1e-3 ? pow( c.rgb / c.a, vec3( 2.2 ) ) : vec3( 0.0 );
}

void main()
{
	vec4 f  = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 rd = normalize( f.xyz / f.w - uCamPos );
	vec3 oc = uCamPos - uCenter;
	float b = dot( oc, rd ), c = dot( oc, oc ) - uRadius * uRadius, h = b * b - c;
	if( h <= 0.0 )
		discard;
	h        = sqrt( h );
	float t  = max( -b - h, 0.0 ), t1 = -b + h;
	bool hit = false;
	for( int i = 0; i < 128; ++i )
	{
		float d = Field( uCamPos + rd * t );
		if( d < 0.0015 * t )
		{
			hit = true;
			break;
		}
		t += max( d * 0.8, 0.002 * t );
		if( t > t1 )
			break;
	}
	if( !hit )
		discard;
	vec3 p  = uCamPos + rd * t;
	vec3 n  = normalize( Grad( p, 0.02 * uUnit + 0.0005 * t ) ); // ~ a voxel: the field is smooth at that scale
	vec3 V  = -rd;
	float nov = max( dot( n, V ), 0.0 );
	vec3 R  = reflect( rd, n );
	float fr = 0.04 + 0.96 * pow( 1.0 - nov, 5.0 );
	vec3 col;
	if( uLook == 0 )
	{
		// liquid chrome: mirror of the studio, tinted, with the key's highlight
		vec3 tint = mix( vec3( 0.95 ), uTint, uTintMix );
		col = ( Studio( R, 0.04 ) * 1.6 + vec3( 0.06 ) ) * tint * mix( 0.8, 1.0, fr ) * uEnvIntensity;
		col += uKeyColor * tint * ( pow( max( dot( R, uKeyDir ), 0.0 ), 120.0 ) * 6.0 + max( dot( n, uKeyDir ), 0.0 ) * 0.08 );
	}
	else
	{
		vec4 cp = uViewProj * vec4( p, 1.0 );
		vec2 uv = cp.xy / cp.w * 0.5 + 0.5;
		float ca, cb;
		vec3 pa = Paint( uPaintA, uv, ca ), pb = Paint( uPaintB, uv, cb );
		float wa = ( 1.0 - uE ) * ca, wb = uE * cb;
		vec3 paint = ( pa * wa + pb * wb ) / max( wa + wb, 1e-4 );
		paint = mix( mix( vec3( 0.5 ), paint, min( wa + wb, 1.0 ) / max( 1.0 - uE, uE ) ), uTint * dot( paint, vec3( 0.333 ) ) * 2.0, uTintMix );
		// a wet coat: the paint under a clear varnish reflecting the studio
		col = paint * ( 1.0 - fr ) + Studio( R, 0.06 ) * fr * uEnvIntensity + uKeyColor * pow( max( dot( R, uKeyDir ), 0.0 ), 80.0 ) * 2.0;
	}
	vec4 cl      = uViewProj * vec4( p, 1.0 );
	gl_FragDepth = cl.z / cl.w * 0.5 + 0.5;
	fragColor    = vec4( ToDisplay( col ), 1.0 );
}
)GLSL";
}
