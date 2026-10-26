// W20: the 3D smoke simulation (Smoke Engine Sim / Sim + Detail). Fields are 2D atlases of z-slices, one full-screen
// triangle per pass (SimShaders.h). Per fixed substep of 1/60 s: obstacle, MacCormack advection (2 passes), vorticity,
// forces + sources, divergence, Jacobi pressure (Sim Iterations), projection. The grid is world-aligned and follows
// the mask's position; the obstacle is the model's SDF (built once per geometry, SdfBuilder) sampled through the inverse
// of this frame's model matrix. Nothing here runs while Smoke Engine = Noise or no volume light is on.
#include "Generate.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "SimShaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr float kSimDt        = 1.0f / 60.0f; // fixed substep: behaviour does not depend on the frame rate
constexpr int kMaxSubsteps    = 3;            // per frame (a slower frame drops simulated time rather than stalling)
constexpr int kPrewarmPerFrame = 8;           // extra substeps per frame while prewarming
constexpr float kPrewarmSecs  = 2.0f;
// sampler units of the sim passes (each sampler its own unit, W3 rule)
enum : int { uVelU = 0, uScaU, uTmpVU, uTmpSU, uObsU, uCurlU, uPU, uDivU, uSdfU, uNoiseU };

void Tex2D( GLuint& t, GLenum internal, GLenum format, int w, int h, GLenum filter )
{
	if( !t )
		glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_FLOAT, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
}
void Del( GLuint& t )
{
	if( t )
		glDeleteTextures( 1, &t ), t = 0;
}
}

bool Renderer::InitSim( std::string& error )
{
	auto P = [ & ]( const char* fs ) { return Link( kQuadVS, std::string( kSimLib ) + fs, error ); };
	progSimObs     = P( kSimObstacleFS );
	progSimAdvect  = P( kSimAdvectFS );
	progSimCorrect = P( kSimCorrectFS );
	progSimCurl    = P( kSimCurlFS );
	progSimForce   = P( kSimForceFS );
	progSimDiv     = P( kSimDivFS );
	progSimJacobi  = P( kSimJacobiFS );
	progSimProject = P( kSimProjectFS );
	// W24: the passes with the Trip fields and forces (separate programs: the plain ones compile exactly as before)
	auto PT = [ & ]( const char* fs ) { return Link( kQuadVS, std::string( "#define TRIP\n" ) + kSimLib + fs, error ); };
	progSimCorrectT = PT( kSimCorrectFS );
	progSimForceT   = PT( kSimForceFS );
	progSimProjectT = PT( kSimProjectFS );
	if( !progSimCorrectT || !progSimForceT || !progSimProjectT )
		return false;
	// W26 fire: TRIP + FIRE (b = fuel; the Trip looks still work on top)
	auto PF = [ & ]( const char* fs ) { return Link( kQuadVS, std::string( "#define TRIP\n#define FIRE\n" ) + kSimLib + fs, error ); };
	progSimCorrectF = PF( kSimCorrectFS );
	progSimForceF   = PF( kSimForceFS );
	if( !progSimCorrectF || !progSimForceF )
		return false;
	// the march / screen-ray source with the simulated medium; the light sum is tinted by the sim smoke's colour
	std::string march = kFogMarchFS;
	const std::string from = "S += uXLColor[ i ] *", to = "S += gTint * uXLColor[ i ] *";
	march.replace( march.find( from ), from.size(), to );
	progFogMarchSim = Link( kQuadVS, std::string( kLightLib ) + kFrontLib + kSimLib + kMediumSimLib + march, error );
	progRaySrcSim   = Link( kQuadVS, std::string( kLightLib ) + kFrontLib + kSimLib + kMediumSimLib + kRaySourceFS, error );
	progFogMarchSimT = Link( kQuadVS, std::string( "#define TRIP\n" ) + kLightLib + kFrontLib + kSimLib + kTripLib + kMediumSimLib + march, error ); // W24
	progFogMarchSimF = Link( kQuadVS, std::string( "#define TRIP\n#define FIRE\n" ) + kLightLib + kFrontLib + kSimLib + kTripLib + kMediumSimLib + march, error ); // W26
	if( !progFogMarchSimT || !progFogMarchSimF )
		return false;
	if( !progSimObs || !progSimAdvect || !progSimCorrect || !progSimCurl || !progSimForce || !progSimDiv || !progSimJacobi || !progSimProject || !progFogMarchSim ||
	    !progRaySrcSim )
		return false;
	static const char* names[] = { "uVel", "uSca", "uTmpV", "uTmpS", "uObs", "uCurl", "uP", "uDiv", "uSdf", "uNoise" };
	for( GLuint p : { progSimObs, progSimAdvect, progSimCorrect, progSimCurl, progSimForce, progSimDiv, progSimJacobi, progSimProject, progSimCorrectT, progSimForceT,
	                  progSimProjectT, progSimCorrectF, progSimForceF } )
	{
		glUseProgram( p );
		for( int i = 0; i < 10; ++i )
			glUniform1i( U( p, names[ i ] ), i );
	}
	for( GLuint p : { progFogMarchSim, progRaySrcSim, progFogMarchSimT, progFogMarchSimF } )
	{
		glUseProgram( p );
		glUniform1i( U( p, "uDepth" ), 0 );
		glUniform1i( U( p, "uNoise" ), 1 );
		glUniform1i( U( p, "uSimDen" ), 2 );
		glUniform1i( U( p, "uProjTex" ), 3 );
		glUniform1i( U( p, "uSimVel" ), 4 ); // TRIP only
		glUniform1i( U( p, "uSKShadow" ), kSKShadowUnit );
		BindShadowSamplers( p );
	}
	glUseProgram( 0 );
	glGenFramebuffers( 1, &simFBO );
	return true;
}

void Renderer::ReleaseSim()
{
	for( GLuint* p : { &progSimObs, &progSimAdvect, &progSimCorrect, &progSimCurl, &progSimForce, &progSimDiv, &progSimJacobi, &progSimProject, &progFogMarchSim,
	                   &progRaySrcSim, &progFogMarchSimT, &progSimCorrectT, &progSimForceT, &progSimProjectT, &progSimCorrectF, &progSimForceF, &progFogMarchSimF } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	for( GLuint* t : { &simVel[ 0 ], &simVel[ 1 ], &simSca[ 0 ], &simSca[ 1 ], &simTmpV, &simTmpS, &simObs, &simDiv, &simP[ 0 ], &simP[ 1 ] } )
		Del( *t );
	if( simFBO )
		glDeleteFramebuffers( 1, &simFBO ), simFBO = 0;
	simRes = 0, simLive = false, sdfTex = 0;
	for( SimSdf& o : simSdf )
	{
		Del( o.tex );
		o = SimSdf();
	}
}

// one full-screen pass over the atlas into one or two targets
void Renderer::SimPass( GLuint prog, GLuint a, GLuint b )
{
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a, 0 );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, b, 0 );
	const GLenum bufs[ 2 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers( b ? 2 : 1, bufs );
	glUseProgram( prog );
	glUniform1i( U( prog, "uRes" ), simRes );
	glUniform1i( U( prog, "uTilesX" ), simTilesX );
	glUniform2f( U( prog, "uAtlas" ), float( simAtlasW ), float( simAtlasH ) );
	glUniform3fv( U( prog, "uBoxMin" ), 1, simBoxMin );
	glUniform1f( U( prog, "uCell" ), simCell );
	glUniform1f( U( prog, "uSolidPad" ), 0.5f * simCell );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
}

bool Renderer::EnsureSimSdf( int slot, GpuModel* model )
{
	SimSdf& o = simSdf[ slot ];
	if( !model || !model->geometry )
		return false;
	if( o.geo != model->geometry.get() )
	{
		o.geo = model->geometry.get();
		o.job = RequestSdf( model->geometry );
		Del( o.tex );
	}
	if( o.job && o.job->done.load() )
	{
		const SdfField& f = o.job->field;
		if( !f.rg.empty() )
		{
			glGenTextures( 1, &o.tex );
			glBindTexture( GL_TEXTURE_3D, o.tex );
			glTexImage3D( GL_TEXTURE_3D, 0, GL_RG16F, kSdfRes, kSdfRes, kSdfRes, 0, GL_RG, GL_FLOAT, f.rg.data() );
			glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			for( GLenum w : { GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R } )
				glTexParameteri( GL_TEXTURE_3D, w, GL_CLAMP_TO_EDGE );
			glBindTexture( GL_TEXTURE_3D, 0 );
			std::memcpy( o.min, f.boxMin, sizeof( o.min ) );
			o.size = f.boxSize, o.solid = f.solidVoxels, o.cavity = f.cavityVoxels;
		}
		Log( "SIM obstacle %c %s: %d^3 SDF, %d solid + %d cavity voxels, %.0f ms (off the render thread)", "ABCD"[ slot ], LogName( model->path ).c_str(), kSdfRes,
		     f.solidVoxels, f.cavityVoxels, f.ms );
		o.job.reset();
	}
	return o.tex != 0;
}

void Renderer::UpdateSim( const FrameState& fs, GpuModel* model )
{
	const SimState& s = fs.sim;
	if( s.engine == 0 || !fs.vol.active || !model || !model->geometry )
	{
		simLive = false;
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	EnsureNoise();
	// the obstacle SDF: built once per geometry off the render thread, one per model slot (LoadEhr W1: the hop's dominant model's
	// is bound; PrepareSlot builds the other filled slots' ahead of use); until it is there the grid has no obstacle
	EnsureSimSdf( SlotOf( model ), model );
	const SimSdf& obs = simSdf[ SlotOf( model ) ];
	sdfTex            = obs.geo == model->geometry.get() ? obs.tex : 0;
	std::memcpy( sdfMin, obs.min, sizeof( sdfMin ) );
	sdfSize = obs.size;

	// (re)allocate the atlases on a resolution change; clear on start / reset
	const bool fresh = !simLive || s.reset || s.res != simRes;
	if( s.res != simRes )
	{
		simRes    = s.res;
		simTilesX = (int)std::ceil( std::sqrt( float( simRes ) ) );
		simAtlasW = simRes * simTilesX;
		simAtlasH = simRes * ( ( simRes + simTilesX - 1 ) / simTilesX );
		for( GLuint* t : { &simVel[ 0 ], &simVel[ 1 ], &simSca[ 0 ], &simSca[ 1 ], &simTmpV, &simTmpS, &simObs } )
			Tex2D( *t, GL_RGBA16F, GL_RGBA, simAtlasW, simAtlasH, GL_LINEAR );
		for( GLuint* t : { &simDiv, &simP[ 0 ], &simP[ 1 ] } )
			Tex2D( *t, GL_R32F, GL_RED, simAtlasW, simAtlasH, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	float boxMin[ 3 ];
	for( int k = 0; k < 3; ++k )
		boxMin[ k ] = s.boxCenter[ k ] - 0.5f * s.boxSize;
	float shift[ 3 ] = { 0, 0, 0 }; // where last frame's grid sits in this frame's cells
	const float cell = s.boxSize / simRes;
	glBindFramebuffer( GL_FRAMEBUFFER, simFBO );
	glViewport( 0, 0, simAtlasW, simAtlasH );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	glBindVertexArray( quadVAO );
	if( fresh )
	{
		for( GLuint t : { simVel[ 0 ], simVel[ 1 ], simSca[ 0 ], simSca[ 1 ], simTmpV, simTmpS, simDiv, simP[ 0 ], simP[ 1 ] } )
		{
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0 );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0 );
			const GLenum b0 = GL_COLOR_ATTACHMENT0;
			glDrawBuffers( 1, &b0 );
			glClearColor( 0, 0, 0, 0 );
			glClear( GL_COLOR_BUFFER_BIT );
		}
		simAcc     = 0;
		simPrewarm = kPrewarmSecs;
		simFrames  = 0;
		simTripWarm = true;
		Log( "SIM start: %d^3 cells (%dx%d atlas), box %.2f, cell %.3f, %d Jacobi iterations; prewarm %.1f s", simRes, simAtlasW, simAtlasH, s.boxSize, cell,
		     s.iterations, kPrewarmSecs );
	}
	else
		for( int k = 0; k < 3; ++k )
			shift[ k ] = ( boxMin[ k ] - simBoxMin[ k ] ) / cell;
	simCell = cell;
	simLive = true;

	// W24 Time Warp: simulated seconds per second (0 / Freeze: no substeps). A frame without a substep keeps the grid where
	// it was (its smoke stays put in the world); the next substep applies the whole move since then.
	// W28 Fire Speed < 1 shortens the substep (stepScale) so a slow fire still moves every frame.
	const float stepDt = kSimDt * s.stepScale;
	simAcc += std::min( s.frameDt, 0.25f ) * s.warp;
	const int maxSteps = s.warp > 1.0f ? kMaxSubsteps + 2 : kMaxSubsteps;
	int steps = std::min( maxSteps, int( simAcc / stepDt + ( s.stepScale < 1.0f ? 1e-3f : 0.0f ) ) );
	simAcc    = std::min( simAcc - steps * stepDt, stepDt );
	int warm  = 0;
	if( simPrewarm > 0 && s.warp > 0.0f )
	{
		warm = std::min( kPrewarmPerFrame, int( std::ceil( simPrewarm / kSimDt ) ) );
		simPrewarm -= warm * kSimDt;
	}
	if( fresh || steps + warm > 0 )
		std::memcpy( simBoxMin, boxMin, sizeof( boxMin ) );
	for( int i = 0; i < steps + warm; ++i )
	{
		// the whole frame's grid move is applied on the first substep
		float sh[ 3 ] = { i == 0 ? shift[ 0 ] : 0.0f, i == 0 ? shift[ 1 ] : 0.0f, i == 0 ? shift[ 2 ] : 0.0f };
		for( GLuint p : { progSimAdvect, progSimCorrect, progSimCorrectT, progSimCorrectF } )
		{
			glUseProgram( p );
			glUniform3fv( U( p, "uShift" ), 1, sh );
		}
		SimStep( fs, i < steps ? stepDt : kSimDt, i + 1 == steps + warm );
	}
	if( simTripWarm )
	{
		// W24: the TRIP passes compile on their first draw: draw each once into the scratch fields through a 1-pixel scissor
		// now (the sim's own start already costs a frame), so turning a Trip look on mid-show never hitches
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 1, 1 );
		SimPass( progSimCorrectT, simTmpV, simTmpS );
		SimPass( progSimForceT, simTmpV, simTmpS );
		SimPass( progSimProjectT, simTmpV );
		SimPass( progSimCorrectF, simTmpV, simTmpS ); // W26 fire
		SimPass( progSimForceF, simTmpV, simTmpS );
		glDisable( GL_SCISSOR_TEST );
	}
	++simFrames;
	if( getenv( "MOVEEHR_SIM_STATS" ) && simFrames % 30 == 0 )
		SimStats( fs );
	for( int u = uNoiseU; u >= 0; --u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBindTexture( GL_TEXTURE_3D, 0 );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	simStatMs = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}

void Renderer::SimStep( const FrameState& fs, float dt, bool )
{
	const SimState& s = fs.sim;
	Mat4 M, prev;
	std::memcpy( M.m, s.model, sizeof( M.m ) );
	std::memcpy( prev.m, s.prevModel, sizeof( prev.m ) );
	const Mat4 inv       = M.Inverse();
	const float toWorld  = std::sqrt( M.m[ 0 ] * M.m[ 0 ] + M.m[ 1 ] * M.m[ 1 ] + M.m[ 2 ] * M.m[ 2 ] );
	const bool haveSdf   = sdfTex != 0;
	auto bind2D = [ & ]( int unit, GLuint t ) {
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, t );
	};
	auto bindCommon = [ & ]() {
		glActiveTexture( GL_TEXTURE0 + uSdfU );
		glBindTexture( GL_TEXTURE_3D, haveSdf ? sdfTex : noiseTex );
		glActiveTexture( GL_TEXTURE0 + uNoiseU );
		glBindTexture( GL_TEXTURE_3D, noiseTex );
	};
	auto sdfUniforms = [ & ]( GLuint p ) {
		// no SDF yet: point the lookup far outside so every cell reads "no obstacle"
		const float far[ 3 ] = { 1e9f, 1e9f, 1e9f };
		glUniform3fv( U( p, "uSdfMin" ), 1, haveSdf ? sdfMin : far );
		glUniform1f( U( p, "uSdfSize" ), haveSdf ? sdfSize : 1.0f );
		glUniformMatrix4fv( U( p, "uInvModel" ), 1, GL_FALSE, inv.m );
	};
	const GLuint vel = simVel[ simV ], sca = simSca[ simS ], vel2 = simVel[ 1 - simV ], sca2 = simSca[ 1 - simS ];
	const GLuint progCorrect = s.fire ? progSimCorrectF : s.trip ? progSimCorrectT : progSimCorrect, progForce = s.fire ? progSimForceF : s.trip ? progSimForceT : progSimForce,
	             progProject = s.fire || s.trip ? progSimProjectT : progSimProject;
	// every sampler unit of the passes gets a texture of its type (unused ones: something harmless, never the target)
	auto bindAll = [ & ]( GLuint v, GLuint sc, GLuint tv, GLuint ts, GLuint p ) {
		bind2D( uVelU, v ), bind2D( uScaU, sc ), bind2D( uTmpVU, tv ), bind2D( uTmpSU, ts ), bind2D( uObsU, simObs ), bind2D( uCurlU, tv ), bind2D( uPU, p ),
		    bind2D( uDivU, simDiv );
		bindCommon();
	};

	// 1. obstacle
	bindAll( vel, sca, whiteTex, whiteTex, whiteTex );
	bind2D( uObsU, whiteTex );
	glUseProgram( progSimObs );
	sdfUniforms( progSimObs );
	glUniform1f( U( progSimObs, "uSdfToWorld" ), toWorld );
	glUniformMatrix4fv( U( progSimObs, "uPrevModel" ), 1, GL_FALSE, prev.m );
	glUniform1f( U( progSimObs, "uInvDt" ), 1.0f / std::max( s.frameDt, 1e-3f ) );
	SimPass( progSimObs, simObs );

	// 2 + 3. MacCormack advection of velocity and smoke (density, heat)
	const float dtCells = dt / simCell;
	bindAll( vel, sca, whiteTex, whiteTex, whiteTex );
	glUseProgram( progSimAdvect );
	glUniform1f( U( progSimAdvect, "uDtCells" ), dtCells );
	SimPass( progSimAdvect, simTmpV, simTmpS );
	bindAll( vel, sca, simTmpV, simTmpS, whiteTex );
	bind2D( uCurlU, whiteTex );
	glUseProgram( progCorrect );
	glUniform1f( U( progCorrect, "uDtCells" ), dtCells );
	glUniform3f( U( progCorrect, "uFade" ), std::exp( -0.1f * dt ), std::exp( -s.fade * dt ), std::exp( -s.heatDecay * dt ) );
	if( s.fire )
		glUniform1f( U( progCorrect, "uFuelFade" ), std::exp( -s.fuelFade * dt ) );
	SimPass( progCorrect, vel2, sca2 );

	// 4. vorticity (into tmpV), then forces + sources back into the first pair
	bindAll( vel2, sca2, whiteTex, whiteTex, whiteTex );
	SimPass( progSimCurl, simTmpV );
	bindAll( vel2, sca2, whiteTex, whiteTex, whiteTex );
	bind2D( uCurlU, simTmpV );
	glUseProgram( progForce );
	sdfUniforms( progForce );
	glUniform1f( U( progForce, "uDt" ), dt );
	glUniform1f( U( progForce, "uBuoy" ), s.buoyancy );
	glUniform1f( U( progForce, "uWeight" ), s.weight );
	glUniform1f( U( progForce, "uVort" ), s.vorticity );
	glUniform3fv( U( progForce, "uWind" ), 1, s.wind );
	glUniform1f( U( progForce, "uTurb" ), s.turb );
	glUniform1f( U( progForce, "uNoiseFreq" ), fs.vol.smokeFreq / std::max( s.boxSize * 0.2f, 1e-3f ) );
	glUniform3fv( U( progForce, "uDrift" ), 1, fs.vol.drift );
	glUniform1f( U( progForce, "uEmitMul" ), s.emitMul );
	glUniform1f( U( progForce, "uEmitEyes" ), s.emitEyes );
	glUniform1f( U( progForce, "uEyeHeat" ), s.eyesHeat );
	glUniform1f( U( progForce, "uEyeRad" ), s.eyeRad );
	glUniform1f( U( progForce, "uEyeSpeed" ), s.eyeSpeed );
	glUniform3fv( U( progForce, "uEyePos" ), 2, s.eyePos );
	glUniform3fv( U( progForce, "uEyeDir" ), 1, s.eyeDir );
	glUniform1f( U( progForce, "uEmitBehind" ), s.emitBehind );
	glUniform1f( U( progForce, "uBehindHeat" ), s.behindHeat );
	glUniform1f( U( progForce, "uBehindRad" ), s.behindRad );
	glUniform3fv( U( progForce, "uBehindPos" ), 1, s.behindPos );
	glUniform1f( U( progForce, "uEmitRim" ), s.emitRim );
	glUniform1f( U( progForce, "uRimHeat" ), s.rimHeat );
	glUniform1f( U( progForce, "uEmitOff" ), s.emitOffset );
	glUniform1f( U( progForce, "uEmitInside" ), haveSdf ? s.emitInside : 0.0f );
	glUniform1f( U( progForce, "uInsideHeat" ), s.insideHeat );
	glUniform3fv( U( progForce, "uCamPos" ), 1, fs.camPos );
	glUniform1f( U( progForce, "uConfine" ), s.confine );
	glUniform1f( U( progForce, "uConfineR" ), s.confineR );
	glUniform3fv( U( progForce, "uConfineCenter" ), 1, s.confineCenter );
	// W24 Trip: whirlpool / hyperspace, vortex rings, ghost faces
	glUniform3fv( U( progForce, "uMaskCenter" ), 1, s.maskCenter );
	glUniform3fv( U( progForce, "uMaskUp" ), 1, s.maskUp );
	glUniform1f( U( progForce, "uWhirl" ), s.whirl );
	glUniform1f( U( progForce, "uHyper" ), s.hyper );
	glUniform3fv( U( progForce, "uHyperAxis" ), 1, s.hyperAxis );
	glUniform1i( U( progForce, "uRingN" ), s.ringN );
	glUniform1fv( U( progForce, "uRingK" ), 2, s.ringK );
	glUniform1fv( U( progForce, "uRingS" ), 2, s.ringS );
	glUniform3fv( U( progForce, "uRingPos" ), 2, s.ringPos );
	glUniform3fv( U( progForce, "uRingDir" ), 2, s.ringDir );
	glUniform1fv( U( progForce, "uRingR" ), 2, s.ringR );
	glUniform1fv( U( progForce, "uRingH" ), 2, s.ringH );
	glUniform1f( U( progForce, "uRingHold" ), std::max( s.fade, 0.3f ) );
	glUniform1f( U( progForce, "uRingSpeed" ), s.ringSpeed );
	glUniform1f( U( progForce, "uRingSpin" ), s.ringSpin );
	glUniform1f( U( progForce, "uRingSmoke" ), s.ringSmoke );
	const int ghosts = haveSdf ? s.ghostN : 0;
	glUniform1i( U( progForce, "uGhostN" ), ghosts );
	if( ghosts > 0 )
	{
		glUniformMatrix4fv( U( progForce, "uGhostInv" ), ghosts, GL_FALSE, &s.ghostInv[ 0 ][ 0 ] );
		glUniform1fv( U( progForce, "uGhostK" ), ghosts, s.ghostK );
		glUniform1fv( U( progForce, "uGhostToWorld" ), ghosts, s.ghostToWorld );
	}
	glUniform1f( U( progForce, "uGhostSmoke" ), s.ghostSmoke );
	if( s.fire )
	{
		glUniform1f( U( progForce, "uFireFuel" ), s.fireFuel );
		glUniform1f( U( progForce, "uFireSmoke" ), s.fireSmoke );
		glUniform1f( U( progForce, "uBurn" ), s.burn );
		glUniform1f( U( progForce, "uHeatRelease" ), s.heatRelease );
		glUniform1f( U( progForce, "uSoot" ), s.soot );
		glUniform1f( U( progForce, "uFireCool" ), s.fireCool );
		glUniform1f( U( progForce, "uLift" ), s.lift );
	}
	SimPass( progForce, vel, sca );

	// 5. divergence, 6. Jacobi (warm-started from the last pressure), 7. projection
	bindAll( vel, sca, whiteTex, whiteTex, whiteTex );
	bind2D( uDivU, whiteTex );
	SimPass( progSimDiv, simDiv );
	for( int it = 0; it < s.iterations; ++it )
	{
		bindAll( vel, sca, whiteTex, whiteTex, simP[ simPi ] );
		SimPass( progSimJacobi, simP[ 1 - simPi ] );
		simPi = 1 - simPi;
	}
	bindAll( vel, sca, whiteTex, whiteTex, simP[ simPi ] );
	SimPass( progProject, vel2 );
	simV = 1 - simV; // the projected velocity is in vel2; the smoke stays in `sca`
}

// Debug (LOADEHR_SIM_STATS=1): divergence after projection, total / max smoke, smoke inside the obstacle.
void Renderer::SimStats( const FrameState& fs )
{
	glBindFramebuffer( GL_FRAMEBUFFER, simFBO );
	glViewport( 0, 0, simAtlasW, simAtlasH );
	auto bind2D = [ & ]( int unit, GLuint t ) {
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, t );
	};
	for( int u : { uVelU, uScaU, uTmpVU, uTmpSU, uCurlU, uPU, uDivU } )
		bind2D( u, whiteTex );
	bind2D( uVelU, simVel[ simV ] );
	bind2D( uObsU, simObs );
	glActiveTexture( GL_TEXTURE0 + uSdfU );
	glBindTexture( GL_TEXTURE_3D, sdfTex ? sdfTex : noiseTex );
	glActiveTexture( GL_TEXTURE0 + uNoiseU );
	glBindTexture( GL_TEXTURE_3D, noiseTex );
	const size_t n = size_t( simAtlasW ) * simAtlasH;
	std::vector< float > pre( n ), div( n ), sca( n * 4 ), obs( n * 4 ), vel( n * 4 ), pres( n );
	auto read = [ & ]( GLuint t, GLenum fmt, float* dst ) {
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0 );
		glReadBuffer( GL_COLOR_ATTACHMENT0 );
		glReadPixels( 0, 0, simAtlasW, simAtlasH, fmt, GL_FLOAT, dst );
	};
	read( simDiv, GL_RED, pre.data() ); // the last step's divergence before its pressure solve
	SimPass( progSimDiv, simDiv );      // ... and after the projection
	read( simDiv, GL_RED, div.data() );
	read( simSca[ simS ], GL_RGBA, sca.data() );
	read( simObs, GL_RGBA, obs.data() );
	read( simVel[ simV ], GL_RGBA, vel.data() );
	read( simP[ simPi ], GL_RED, pres.data() );
	// the pressure solve's own residual |lap(p) - div| / |div| (interior fluid cells with no solid neighbour)
	auto at = [ & ]( int x, int y, int z ) -> size_t { return size_t( ( z / simTilesX ) * simRes + y ) * simAtlasW + ( z % simTilesX ) * simRes + x; };
	double res = 0, rhs = 0;
	for( int z = 1; z < simRes - 1; ++z )
		for( int y = 1; y < simRes - 1; ++y )
			for( int x = 1; x < simRes - 1; ++x )
			{
				const size_t i = at( x, y, z );
				const size_t nb[ 6 ] = { at( x - 1, y, z ), at( x + 1, y, z ), at( x, y - 1, z ), at( x, y + 1, z ), at( x, y, z - 1 ), at( x, y, z + 1 ) };
				bool ok = obs[ i * 4 + 3 ] >= 0.5f * simCell;
				double sum = 0;
				for( size_t j : nb )
					ok = ok && obs[ j * 4 + 3 ] >= 0.5f * simCell, sum += pres[ j ];
				if( !ok )
					continue;
				res += std::fabs( ( sum - 6.0 * pres[ i ] ) / ( double( simCell ) * simCell ) - pre[ i ] );
				rhs += std::fabs( pre[ i ] );
			}
	double sumPre = 0, sumDiv = 0, total = 0, inside = 0, mx = 0, cen[ 3 ] = { 0, 0, 0 }, sumSpeed = 0;
	size_t fluid = 0;
	for( int y = 0; y < simAtlasH; ++y )
		for( int x = 0; x < simAtlasW; ++x )
		{
			const int z = ( y / simRes ) * simTilesX + x / simRes;
			if( z >= simRes )
				continue;
			const size_t i  = size_t( y ) * simAtlasW + x;
			const bool solid = obs[ i * 4 + 3 ] < 0.5f * simCell;
			const double d  = sca[ i * 4 ];
			total += d, mx = std::max( mx, d );
			const int gc[ 3 ] = { x % simRes, y % simRes, z };
			for( int k = 0; k < 3; ++k )
				cen[ k ] += d * ( simBoxMin[ k ] + ( gc[ k ] + 0.5 ) * simCell );
			if( solid )
				inside += d;
			else
				sumPre += std::fabs( pre[ i ] ), sumDiv += std::fabs( div[ i ] ), ++fluid,
				    sumSpeed += std::sqrt( vel[ i * 4 ] * vel[ i * 4 ] + vel[ i * 4 + 1 ] * vel[ i * 4 + 1 ] + vel[ i * 4 + 2 ] * vel[ i * 4 + 2 ] );
		}
	// W23: how much of the smoke is in the AIR (more than 0.15 mask radii off the surface) and how high the plume goes
	// (smoke-weighted 90th percentile of the height above the mask centre, in mask radii)
	double air = 0;
	std::vector< std::pair< float, float > > hs; // (height, smoke)
	for( int y = 0; y < simAtlasH; ++y )
		for( int x = 0; x < simAtlasW; ++x )
		{
			const int z = ( y / simRes ) * simTilesX + x / simRes;
			const size_t i = size_t( y ) * simAtlasW + x;
			if( z >= simRes || sca[ i * 4 ] <= 0.0f )
				continue;
			if( obs[ i * 4 + 3 ] > 0.15f * fs.sim.unit )
				air += sca[ i * 4 ];
			hs.push_back( { ( simBoxMin[ 1 ] + ( y % simRes + 0.5f ) * simCell - fs.sim.boxCenter[ 1 ] ) / fs.sim.unit, sca[ i * 4 ] } );
		}
	std::sort( hs.begin(), hs.end() );
	double acc = 0, h90 = 0;
	for( const auto& h : hs )
		if( ( acc += h.second ) >= 0.9 * total )
		{
			h90 = h.first;
			break;
		}
	Log( "SIMAIR frame %u: air %.3f of the smoke (> 0.15 radii off the surface), plume p90 height %.2f radii, total %.4f", simFrames, air / std::max( total, 1e-9 ), h90, total );
	const double c3 = double( simCell ) * simCell * simCell;
	Log( "SIMSTAT centroid (%.2f %.2f %.2f) world, box centre (%.2f %.2f %.2f), eyes (%.2f %.2f %.2f) dir (%.2f %.2f %.2f)", cen[ 0 ] / std::max( total, 1e-9 ),
	     cen[ 1 ] / std::max( total, 1e-9 ), cen[ 2 ] / std::max( total, 1e-9 ), fs.sim.boxCenter[ 0 ], fs.sim.boxCenter[ 1 ], fs.sim.boxCenter[ 2 ], fs.sim.eyePos[ 0 ],
	     fs.sim.eyePos[ 1 ], fs.sim.eyePos[ 2 ], fs.sim.eyeDir[ 0 ], fs.sim.eyeDir[ 1 ], fs.sim.eyeDir[ 2 ] );
	Log( "SIMSTAT frame %u: pressure residual %.4f of |div|, mean |div| before %.4f -> after projection %.4f /s (mean speed / cell %.4f /s) over %zu fluid cells, smoke total %.4f (x cell^3 %.5f), max %.3f, inside the obstacle %.6f, %d iterations, %.2f ms CPU",
	     simFrames, res / std::max( rhs, 1e-12 ), sumPre / std::max< size_t >( fluid, 1 ), sumDiv / std::max< size_t >( fluid, 1 ), sumSpeed / std::max< size_t >( fluid, 1 ) / simCell, fluid, total, total * c3, mx, inside, fs.sim.iterations, simStatMs );
}
}
