// Apple Vision face tracking (FX build). VNDetectFaceLandmarksRequest gives the face box, roll / yaw (macOS
// 10.13+) and pitch (macOS 12+); the landmarks refine the centre (between the eyes and the nose).
#include "FaceTracker.h"
#include "Log.h"
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#include <atomic>
#include <chrono>
#include <mutex>

namespace mehr
{
struct FaceTracker::Impl
{
	dispatch_queue_t queue;
	std::atomic< bool > busy{ false };
	std::mutex mutex;
	FaceFrame latest;
	uint64_t taken = 0, seq = 0;
};

FaceTracker::FaceTracker() :
	impl( std::make_shared< Impl >() )
{
	impl->queue = dispatch_queue_create( "com.loadehr.facetrack", DISPATCH_QUEUE_SERIAL );
}

FaceTracker::~FaceTracker() = default;

bool FaceTracker::Busy() const { return impl->busy.load(); }

namespace
{
// mean of a landmark region in box-normalised coordinates
bool RegionCenter( VNFaceLandmarkRegion2D* r, CGPoint& out )
{
	if( !r || r.pointCount == 0 )
		return false;
	const CGPoint* p = r.normalizedPoints;
	double x = 0, y = 0;
	for( NSUInteger i = 0; i < r.pointCount; ++i )
		x += p[ i ].x, y += p[ i ].y;
	out = CGPointMake( x / r.pointCount, y / r.pointCount );
	return true;
}
}

void FaceTracker::Submit( std::vector< uint8_t >&& px, int w, int h )
{
	if( impl->busy.exchange( true ) )
		return;
	auto data                  = std::make_shared< std::vector< uint8_t > >( std::move( px ) );
	std::shared_ptr< Impl > im = impl;
	dispatch_async( im->queue, ^{
	  @autoreleasepool
	  {
		  auto t0             = std::chrono::steady_clock::now();
		  FaceFrame frame;
		  CGColorSpaceRef cs  = CGColorSpaceCreateDeviceRGB();
		  CGDataProviderRef dp = CGDataProviderCreateWithData( nullptr, data->data(), data->size(), nullptr );
		  CGImageRef img = CGImageCreate( w, h, 8, 32, size_t( w ) * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, nullptr, false,
		                                  kCGRenderingIntentDefault );
		  if( img )
		  {
			  VNDetectFaceLandmarksRequest* req = [[VNDetectFaceLandmarksRequest alloc] init];
			  VNImageRequestHandler* handler    = [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
			  NSError* err                      = nil;
			  if( [handler performRequests:@[ req ] error:&err] )
			  {
				  for( VNFaceObservation* o in req.results )
				  {
					  FaceResult f;
					  const CGRect b = o.boundingBox;
					  f.cx = float( b.origin.x + b.size.width * 0.5 ), f.cy = float( b.origin.y + b.size.height * 0.5 );
					  f.w = float( b.size.width ), f.h = float( b.size.height );
					  // centre from the landmarks: between the eyes and the nose
					  CGPoint le, re, nose;
					  VNFaceLandmarks2D* lm = o.landmarks;
					  if( lm && RegionCenter( lm.leftEye, le ) && RegionCenter( lm.rightEye, re ) && RegionCenter( lm.nose, nose ) )
					  {
						  const double lx = ( le.x + re.x + nose.x ) / 3.0, ly = ( le.y + re.y + nose.y ) / 3.0;
						  f.cx = float( b.origin.x + lx * b.size.width );
						  f.cy = float( b.origin.y + ly * b.size.height );
					  }
					  f.roll = o.roll ? o.roll.floatValue : 0.0f;
					  f.yaw  = o.yaw ? o.yaw.floatValue : 0.0f;
					  if( @available( macOS 12.0, * ) )
						  f.pitch = o.pitch ? o.pitch.floatValue : 0.0f;
					  frame.faces.push_back( f );
				  }
			  }
			  else
				  Log( "Vision face request failed: %s", err ? err.localizedDescription.UTF8String : "?" );
			  CGImageRelease( img );
		  }
		  CGDataProviderRelease( dp );
		  CGColorSpaceRelease( cs );
		  frame.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
		  {
			  std::lock_guard< std::mutex > lock( im->mutex );
			  frame.seq  = ++im->seq;
			  im->latest = std::move( frame );
		  }
		  im->busy.store( false );
	  }
	} );
}

bool FaceTracker::TakeLatest( FaceFrame& out )
{
	std::lock_guard< std::mutex > lock( impl->mutex );
	if( impl->latest.seq == impl->taken )
		return false;
	out         = impl->latest;
	impl->taken = impl->latest.seq;
	return true;
}
}
