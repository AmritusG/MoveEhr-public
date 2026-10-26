// W9 Stage D: the Surface Look group (classic bundles). Audio Spikes and Skin Of Copies stand on the dominant model's surface: its spots
// (Surface 25k, a subset by Surf Scale) are captured through its own soup program every frame (the CAPTURE twins, Lines.cpp), so they ride
// every deform, shard flight and morph. The camera looks (X-Ray, Thermal, Night Vision, LED Matrix) are shading modes in the DEFORM twins
// (Shaders.h, under #ifdef DEFORM) set by SetXUniforms.
//   AUDIO SPIKES  an instanced 12-sided cone per spot along its normal; length = Surf Amount x (0.15 + 1.6 x the FFT band of its height:
//                 bass at the base, highs at the top) x a slow clustering (the spikes gather in patches, as ferrofluid does) x a dance
//                 (Surf Speed); glossy black with a hot specular, Wire Color tinting the rim (Color Mix)
//   SKIN OF COPIES every spot carries a copy of another slot (Surf Source: Slot B / C / D / Text) standing on it (its up = the normal),
//                 turned by a hash, wobbling (Surf Speed); one LOD for all by the copies' size on screen; Surf Amount 0 .. 1 grows them,
//                 1 .. 2 dissolves the model underneath away
#include "Log.h"
#include "Renderer.h"
#include "Shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mehr
{
GLuint Link( const std::string& vs, const std::string& fs, std::string& error );

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

const char* kSpikeVS = R"GLSL(
uniform samplerBuffer uCapPos, uCapNrm, uCapCol;
uniform mat4 uViewProj;
uniform float uLen, uRad;    // world: full length, base radius
uniform float uBands[ 16 ];
uniform float uPhase;        // dance cycles, wrapped at 1000 (each spike: whole cycles per 1000)
uniform float uDance;        // 0 .. 1
out vec3 vW, vN;
flat out float vTip;
out float vH;                // 0 at the base .. 1 at the tip
void main()
{
	int i     = gl_InstanceID;
	vec4 P    = texelFetch( uCapPos, i );
	vec4 N    = texelFetch( uCapNrm, i );
	vec4 C    = texelFetch( uCapCol, i );
	vec3 n    = normalize( N.xyz + vec3( 1e-6 ) );
	vec3 t    = normalize( cross( n, abs( n.y ) < 0.9 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 ) ) );
	vec3 b    = cross( n, t );
	float h   = clamp( ( N.w + 1.0 ) * 0.5, 0.0, 1.0 );
	float bnd = uBands[ int( h * 15.99 ) ];
	float sd  = fract( sin( float( i ) * 12.9898 + 1.3 ) * 43758.5453 );
	float rt  = floor( ( 0.6 + 0.8 * sd ) * 1000.0 + 0.5 ) / 1000.0;
	float dn  = 1.0 - uDance * 0.35 * ( 0.5 + 0.5 * sin( 6.2831853 * ( fract( uPhase * rt ) + sd ) ) );
	vec3 q    = P.xyz / max( uRad * 14.0, 1e-5 );
	float cl  = 0.35 + 0.65 * smoothstep( -0.4, 0.8, sin( q.x * 1.7 + q.z * 0.9 ) * sin( q.y * 1.3 - q.x * 0.6 ) + 0.3 * sin( q.z * 2.3 ) );
	float L   = uLen * ( 0.15 + 1.6 * bnd ) * dn * cl * C.a;
	int tri   = gl_VertexID / 3, c = gl_VertexID - tri * 3;
	float a0  = 6.2831853 * float( tri ) / 12.0, a1 = 6.2831853 * float( tri + 1 ) / 12.0;
	float a   = c == 1 ? a0 : a1;
	vec3 rad  = t * cos( a ) + b * sin( a );
	float r   = uRad * ( 0.7 + 0.6 * cl );
	vec3 w    = c == 0 ? P.xyz + n * L : P.xyz + rad * r - n * r * 0.5; // the base a little into the surface
	vec3 ra   = t * cos( 0.5 * ( a0 + a1 ) ) + b * sin( 0.5 * ( a0 + a1 ) );
	vN        = normalize( ( c == 0 ? ra : rad ) * L + n * r );
	vW        = w;
	vH        = c == 0 ? 1.0 : 0.0;
	vTip      = L;
	gl_Position = L <= 1e-6 ? vec4( 2.0, 2.0, 2.0, 1.0 ) : uViewProj * vec4( w, 1.0 );
}
)GLSL";
const char* kSpikeFS = R"GLSL(
in vec3 vW, vN;
flat in float vTip;
in float vH;
uniform vec3 uCamPos, uKeyDir, uKeyColor, uRimColor;
uniform float uRimMix;
out vec4 fragColor;
void main()
{
	vec3 N  = normalize( vN ), V = normalize( uCamPos - vW );
	if( dot( N, V ) < 0.0 )
		N = -N;
	float nov = max( dot( N, V ), 0.0 );
	vec3 H   = normalize( uKeyDir + V );
	float sp = pow( max( dot( N, H ), 0.0 ), 90.0 ) * 2.5;
	float fr = pow( 1.0 - nov, 4.0 );
	vec3 c   = vec3( 0.012 ) + uKeyColor * 0.012 * max( dot( N, uKeyDir ), 0.0 ) + uKeyColor * sp * 0.4;
	c       += mix( vec3( 0.35, 0.38, 0.45 ), uRimColor, uRimMix ) * fr * ( 0.6 + 0.8 * vH );
	fragColor = vec4( pow( clamp( c, 0.0, 1.0 ), vec3( 1.0 / 2.2 ) ), 1.0 );
}
)GLSL";
}

bool Renderer::InitSurfLook( std::string& error )
{
	progSpike  = Link( kSpikeVS, kSpikeFS, error );
	progMainIT = Link( std::string( "#define INSTANCED\n#define INSTTBO\n" ) + kMainVS, std::string( "#define INSTANCED\n" ) + kLightLib + kMainFS, error );
	if( !progSpike || !progMainIT )
		return false;
	glUseProgram( progSpike );
	glUniform1i( U( progSpike, "uCapPos" ), 0 );
	glUniform1i( U( progSpike, "uCapNrm" ), 1 );
	glUniform1i( U( progSpike, "uCapCol" ), 2 );
	// the copies: progMain's sampler units + the spots on 33 / 34
	GLint n = 0;
	glGetProgramiv( progMain, GL_ACTIVE_UNIFORMS, &n );
	glUseProgram( progMainIT );
	for( GLint i = 0; i < n; ++i )
	{
		char name[ 128 ];
		GLsizei len = 0;
		GLint size  = 0;
		GLenum type = 0;
		glGetActiveUniform( progMain, (GLuint)i, sizeof( name ), &len, &size, &type, name );
		if( type != GL_SAMPLER_2D && type != GL_SAMPLER_CUBE && type != GL_SAMPLER_BUFFER && type != GL_UNSIGNED_INT_SAMPLER_BUFFER )
			continue;
		GLint unit = 0;
		glGetUniformiv( progMain, glGetUniformLocation( progMain, name ), &unit );
		glUniform1i( glGetUniformLocation( progMainIT, name ), unit );
	}
	glUniform1i( U( progMainIT, "uSkinPos" ), 33 );
	glUniform1i( U( progMainIT, "uSkinNrm" ), 34 );
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleaseSurfLook()
{
	for( GLuint* p : { &progSpike, &progMainIT } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
}

void Renderer::RenderSurfLook( const FrameState& fs, GpuModel* model, GLuint prog )
{
	const SurfLookState& S = fs.sl;
	if( !model || !model->Complete() )
		return;
	LinesState L;
	L.look   = S.look == 1 ? 4 : 5;
	L.points = S.spots;
	L.axis   = fs.lines.axis;
	const LineSet* set = LinesFor( model, L );
	if( !set )
		return;
	FrameState f = fs;
	f.lines      = L;
	if( !CaptureSpots( f, model, prog, *set, 2 ) )
		return;
	const Capture& c = capture[ 2 ];
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP   = P * V;
	const float wpf = fs.modelScale / std::max( model->fitScale, 1e-12f ); // world per fitted unit
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDepthMask( GL_TRUE );
	glDisable( GL_BLEND );
	if( S.look == 1 )
	{
		glUseProgram( progSpike );
		glUniformMatrix4fv( U( progSpike, "uViewProj" ), 1, GL_FALSE, VP.m );
		glUniform1f( U( progSpike, "uLen" ), S.spikeLen * wpf );
		glUniform1f( U( progSpike, "uRad" ), S.spikeRad * wpf );
		glUniform1fv( U( progSpike, "uBands" ), 16, S.bands );
		glUniform1f( U( progSpike, "uPhase" ), S.phase );
		glUniform1f( U( progSpike, "uDance" ), std::min( 1.0f, S.speed ) );
		glUniform3fv( U( progSpike, "uCamPos" ), 1, fs.camPos );
		glUniform3fv( U( progSpike, "uKeyDir" ), 1, fs.keyDir );
		glUniform3fv( U( progSpike, "uKeyColor" ), 1, fs.keyColor );
		glUniform3fv( U( progSpike, "uRimColor" ), 1, S.color );
		glUniform1f( U( progSpike, "uRimMix" ), S.colorMix );
		for( int k = 0; k < 3; ++k )
		{
			glActiveTexture( GL_TEXTURE0 + k );
			glBindTexture( GL_TEXTURE_BUFFER, c.tex[ k ] );
		}
		glBindVertexArray( quadVAO );
		glDrawArraysInstanced( GL_TRIANGLES, 0, 36, GLsizei( set->count ) );
		for( int k = 2; k >= 0; --k )
		{
			glActiveTexture( GL_TEXTURE0 + k );
			glBindTexture( GL_TEXTURE_BUFFER, 0 );
		}
		glBindVertexArray( 0 );
		return;
	}
	GpuModel* copy = S.copy;
	if( !copy || !copy->Complete() || S.amount <= 0.0f )
		return;
	// one LOD for all the copies, by their size on screen (as the clones: > 350 px full, > 140, > 50, else the coarsest ready)
	float cpos[ 3 ];
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	M.Point( model->fitCenter, cpos );
	const float dist = std::sqrt( ( cpos[ 0 ] - fs.camPos[ 0 ] ) * ( cpos[ 0 ] - fs.camPos[ 0 ] ) + ( cpos[ 1 ] - fs.camPos[ 1 ] ) * ( cpos[ 1 ] - fs.camPos[ 1 ] ) +
	                              ( cpos[ 2 ] - fs.camPos[ 2 ] ) * ( cpos[ 2 ] - fs.camPos[ 2 ] ) );
	const float size = S.copySize * wpf * std::min( 1.0f, S.amount );
	const float px   = 2.0f * size * P.m[ 5 ] / std::max( dist, 0.05f ) * float( fs.height ) * 0.5f;
	copy->UpdateLods();
	const int lod = std::min( copy->LodsReady() - 1, px > 350.0f ? 0 : px > 140.0f ? 1 : px > 50.0f ? 2 : 3 );
	FrameState cf = fs;
	cf.inst.clear(), cf.ghosts.clear();
	cf.wire.on = false, cf.wireMix = 0.0f, cf.dissolve = 0.0f, cf.explode = 0.0f, cf.x = XState(), cf.shape = ShapeState(), cf.morph = MorphState();
	cf.surf.on = false;
	glUseProgram( progMainIT );
	glUniform4f( U( progMainIT, "uSkinFit" ), copy->fitCenter[ 0 ], copy->fitCenter[ 1 ], copy->fitCenter[ 2 ], copy->fitScale );
	glUniform1f( U( progMainIT, "uSkinSize" ), size );
	glUniform1f( U( progMainIT, "uSkinPhase" ), S.phase );
	glUniform1f( U( progMainIT, "uSkinWobble" ), 0.45f * std::min( 1.0f, S.speed ) );
	glActiveTexture( GL_TEXTURE0 + 33 );
	glBindTexture( GL_TEXTURE_BUFFER, c.tex[ 0 ] );
	glActiveTexture( GL_TEXTURE0 + 34 );
	glBindTexture( GL_TEXTURE_BUFFER, c.tex[ 1 ] );
	glActiveTexture( GL_TEXTURE0 );
	EnsureVAO( copy );
	skinNow = true, skinCount = int( set->count ), skinLod = lod;
	DrawModel( progMain, cf, copy, false, false, 0 );
	skinNow = false;
	for( int u : { 33, 34 } )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
	EnsureVAO( model );
}
}
