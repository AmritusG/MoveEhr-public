#pragma once
// W26 Stage 2: the projector (a cookie light). Its texture (RGBA32F, the light's own projection) holds the projected picture in
// rgb and the shadow distance in a: the shadow pass writes the distance into all four channels, then kProjCookieFS writes the
// picture into rgb (colour mask RGB). One sampler per cookie light wherever it is read (the main FS has 2 left of 16).
// The beam in the medium: kProjMarchFS, a march along the view ray added onto the fog's march target (rgb only) before the
// temporal blend, so it shares the fog's resolution, jitter, temporal accumulation and composite; the W3 / W20 / W24 march
// programs stay byte-identical. Linked after kMediumLib (Noise) or kSimLib + kMediumSimLib (Sim).

namespace mehr
{
static const char* kProjCookieFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uImage;
uniform vec2 uImageScale; // host input padded to its hardware size
uniform float uLod;       // Focus: blur by mip level
uniform int uHaveImage;
void main()
{
	vec3 c = uHaveImage == 1 ? textureLod( uImage, vUV * uImageScale, uLod ).rgb : vec3( 1.0 );
	// a soft frame edge (the lens), wider when out of focus
	vec2 e  = min( vUV, 1.0 - vUV );
	float f = smoothstep( 0.0, 0.004 + 0.02 * uLod, min( e.x, e.y ) );
	fragColor = vec4( pow( max( c, 0.0 ), vec3( 2.2 ) ) * f, 0.0 ); // the picture is sRGB-encoded: light is linear
}
)GLSL";

// W2: a video frame into the projector's picture texture (rows flipped bottom-up). The picture is light = premultiplied colour:
// W3 Video Alpha Straight multiplies the colour by the frame's alpha here, Premultiplied (uStraight 0) takes it as it is.
// mode 0 = a BGRA frame (IOSurface rectangle texture, rows top-first), 1 = DXT1 / DXT5 RGB(A), 2 = Hap Q (scaled YCoCg DXT5),
// 3 = Hap Q Alpha (+ the BC4 alpha plane), 4 = a BC4 alpha-only frame (white light through it: the alpha is the picture),
// W3 DXV HQ (W8: Arena's convention, see below): 5 = YCG6 (uDxt BC4 Y, uDxtA BC5 Co, Cg at half size: the same picture coordinates), 6 = YG10 (uDxt BC5 Y, alpha)
static const char* kProjVideoFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2DRect uRect;
uniform sampler2D uDxt;
uniform sampler2D uDxtA;
uniform int uMode;
uniform int uStraight;
uniform vec2 uSize;    // the rectangle texture's size (mode 0)
uniform vec2 uDxtFit;  // the picture's share of the block-padded DXT texture
void main()
{
	vec2 st = vec2( vUV.x, 1.0 - vUV.y ); // the frame's rows are top-first
	vec4 c;
	if( uMode == 0 )
		c = texture( uRect, st * uSize );
	else
	{
		c = texture( uDxt, st * uDxtFit );
		if( uMode == 2 || uMode == 3 )
		{
			// scaled YCoCg (van Waveren and Castano 2007, as Hap Q stores it): Co, Cg in r, g, the scale in b, Y in a
			float sc = c.b * ( 255.0 / 8.0 ) + 1.0;
			float co = ( c.r - 0.5 * 256.0 / 255.0 ) / sc, cg = ( c.g - 0.5 * 256.0 / 255.0 ) / sc, y = c.a;
			c = vec4( y + co - cg, y + cg, y - co - cg, 1.0 );
			if( uMode == 3 )
				c.a = texture( uDxtA, st * uDxtFit ).r;
		}
		else if( uMode == 4 )
			c = vec4( c.rrr, 1.0 );
		else if( uMode >= 5 )
		{
			// W8: DXV HQ as Arena 7.28 shows it (DXVKit 1.0.0, spec 8.4): plain YCoCg, chroma (sampled nearest) with midpoint 127.5 / 255 =
			// 0.5, and the overflow of R or B carried into the other one (in this order) before the clamp; G as it is
			vec2 cc = texture( uDxtA, st * uDxtFit ).rg - 0.5;
			float y = c.r, t = y - cc.y;
			float r = t + cc.x, b = t - cc.x;
			if( r < 0.0 ) { b += r; r = 0.0; }
			if( b < 0.0 ) { r += b; b = 0.0; }
			if( r > 1.0 ) { b += r - 1.0; r = 1.0; }
			if( b > 1.0 ) { r += b - 1.0; b = 1.0; }
			c = vec4( r, y + cc.y, b, uMode == 6 ? c.g : 1.0 );
		}
	}
	fragColor = vec4( clamp( c.rgb, 0.0, 1.0 ) * ( uStraight == 1 ? c.a : 1.0 ), 1.0 );
}
)GLSL";

static const char* kProjMarchFS = R"GLSL(
in vec2 vUV;
layout( location = 0 ) out vec4 outFog;
uniform sampler2D uDepth;
uniform sampler2D uPLTex;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform int uSteps;
uniform float uFrame;
uniform mat4 uPLVP;
uniform vec3 uPLPos;
uniform vec3 uPLDir;
uniform vec3 uPLColor; // colour x intensity x Proj Beam
uniform float uPLFar;
float IGN( vec2 p ) { return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) ); }
float HG( float c, float g ) { float g2 = g * g; return ( 1.0 - g2 ) / pow( max( 1.0 + g2 - 2.0 * g * c, 1e-4 ), 1.5 ); }
void main()
{
	vec4 f     = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 rd    = normalize( f.xyz / f.w - uCamPos );
	float z    = texture( uDepth, vUV ).r;
	float tEnd = uPLFar;
	if( z < 1.0 )
	{
		vec4 s = uInvViewProj * vec4( vUV * 2.0 - 1.0, z * 2.0 - 1.0, 1.0 );
		tEnd   = min( tEnd, length( s.xyz / s.w - uCamPos ) );
	}
	// only the stretch of the ray inside the projector's frustum: clip space c(t) = A + t B against w +- x, w +- y, w + z
	vec4 A = uPLVP * vec4( uCamPos, 1.0 ), B = uPLVP * vec4( rd, 0.0 );
	float t0 = 0.0, t1 = tEnd;
	float ea[ 5 ] = float[ 5 ]( A.w + A.x, A.w - A.x, A.w + A.y, A.w - A.y, A.w + A.z );
	float eb[ 5 ] = float[ 5 ]( B.w + B.x, B.w - B.x, B.w + B.y, B.w - B.y, B.w + B.z );
	for( int k = 0; k < 5; ++k )
	{
		if( abs( eb[ k ] ) < 1e-7 )
		{
			if( ea[ k ] < 0.0 )
				t1 = -1.0;
			continue;
		}
		float tk = -ea[ k ] / eb[ k ];
		if( eb[ k ] > 0.0 )
			t0 = max( t0, tk );
		else
			t1 = min( t1, tk );
	}
	if( t1 <= t0 )
	{
		outFog = vec4( 0.0 );
		return;
	}
	float dt = ( t1 - t0 ) / float( uSteps );
	float t  = t0 + dt * IGN( gl_FragCoord.xy + uFrame * vec2( 5.588238, 3.1459 ) );
	vec3 L   = vec3( 0.0 );
	float T  = 1.0;
	for( int s = 0; s < uSteps; ++s, t += dt )
	{
		vec3 p  = uCamPos + rd * t;
		vec4 c  = uPLVP * vec4( p, 1.0 );
		if( c.w <= 1e-5 )
			continue;
		vec2 uv = c.xy / c.w * 0.5 + 0.5;
		if( any( lessThan( uv, vec2( 0.0 ) ) ) || any( greaterThan( uv, vec2( 1.0 ) ) ) )
			continue;
		float smoke;
		float sig = MediumAt( p, smoke ).x;
		if( sig <= 1e-4 )
			continue;
		vec4 ck = textureLod( uPLTex, uv, 0.0 );
		vec3 lv = p - uPLPos;
		float d = length( lv );
		if( d > ck.a + 0.02 + 0.01 * d )
			continue; // behind the mask, seen from the projector
		float ext = exp( -sig * dt );
		L += T * uPLColor * ck.rgb * ( HG( dot( lv / max( d, 1e-5 ), -rd ), 0.5 ) / ( 1.0 + d * d ) ) * ( 1.0 - ext );
		T *= ext;
	}
	outFog = vec4( L, 0.0 );
}
)GLSL";
}
