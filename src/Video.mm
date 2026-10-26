// W2 Stage 5: video files for the projector (see Video.h). AVFoundation reads; HAP is parsed + Snappy-decompressed here.
#include "Video.h"
#include "Log.h"
#include "dxvkit.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstring>

namespace mehr
{
namespace
{
std::atomic< uint64_t > g_frameSerial{ 0 };
constexpr int kAhead = 6; // frames decoded ahead of the play position (in its direction)

uint32_t Rd( const uint8_t* p, int n )
{
	uint32_t v = 0;
	for( int i = 0; i < n; ++i )
		v |= uint32_t( p[ i ] ) << ( 8 * i );
	return v;
}
// a HAP section header: payload size, type, header length; false if it does not fit
bool Section( const uint8_t* p, size_t n, size_t& size, int& type, size_t& hdr )
{
	if( n < 4 )
		return false;
	size = Rd( p, 3 ), type = p[ 3 ], hdr = 4;
	if( size == 0 )
	{
		if( n < 8 )
			return false;
		size = Rd( p + 4, 4 ), hdr = 8;
	}
	return hdr + size <= n;
}
// one texture section (top-level type: compressor nibble + format nibble) -> its block data
bool TextureSection( int type, const uint8_t* p, size_t n, std::vector< uint8_t >& out, int& format, std::string& error )
{
	switch( type & 0x0F )
	{
	case 0xB: format = 1; break; // RGB DXT1
	case 0xE: format = 5; break; // RGBA DXT5
	case 0xF: format = 6; break; // scaled YCoCg DXT5 (Hap Q)
	case 0x1: format = 4; break; // alpha BC4
	default: error = "HAP texture format 0x" + std::to_string( type & 0x0F ) + " (BC6 / BC7) not supported"; return false;
	}
	const int comp = ( type >> 4 ) & 0x0F;
	out.clear();
	if( comp == 0xA )
	{
		out.assign( p, p + n );
		return true;
	}
	if( comp == 0xB )
	{
		if( SnappyDecompress( p, n, out ) )
			return true;
		error = "HAP: bad Snappy data";
		return false;
	}
	if( comp != 0xC )
	{
		error = "HAP: unknown compressor";
		return false;
	}
	// decode instructions: a container (0x01) of the chunk tables, then the chunks
	size_t csize, chdr;
	int ctype;
	if( !Section( p, n, csize, ctype, chdr ) || ctype != 0x01 )
	{
		error = "HAP: no decode instructions";
		return false;
	}
	std::vector< uint8_t > comps;
	std::vector< uint32_t > sizes, offsets;
	for( size_t q = chdr; q < chdr + csize; )
	{
		size_t s, h;
		int t;
		if( !Section( p + q, chdr + csize - q, s, t, h ) )
			break;
		const uint8_t* d = p + q + h;
		if( t == 0x02 )
			comps.assign( d, d + s );
		else if( t == 0x03 )
			for( size_t i = 0; i + 4 <= s; i += 4 )
				sizes.push_back( Rd( d + i, 4 ) );
		else if( t == 0x04 )
			for( size_t i = 0; i + 4 <= s; i += 4 )
				offsets.push_back( Rd( d + i, 4 ) );
		q += h + s;
	}
	if( comps.empty() || comps.size() != sizes.size() || ( !offsets.empty() && offsets.size() != sizes.size() ) )
	{
		error = "HAP: inconsistent chunk tables";
		return false;
	}
	const uint8_t* frame = p + chdr + csize;
	const size_t left    = n - chdr - csize;
	size_t at            = 0;
	std::vector< uint8_t > tmp;
	for( size_t c = 0; c < comps.size(); ++c )
	{
		const size_t off = offsets.empty() ? at : offsets[ c ];
		at               = off + sizes[ c ];
		if( at > left )
		{
			error = "HAP: chunk past the frame";
			return false;
		}
		if( comps[ c ] == 0x0A )
			out.insert( out.end(), frame + off, frame + off + sizes[ c ] );
		else if( comps[ c ] == 0x0B && SnappyDecompress( frame + off, sizes[ c ], tmp ) )
			out.insert( out.end(), tmp.begin(), tmp.end() );
		else
		{
			error = "HAP: bad chunk";
			return false;
		}
	}
	return true;
}
std::string FourCC( uint32_t f )
{
	char s[ 5 ] = { char( f >> 24 ), char( f >> 16 ), char( f >> 8 ), char( f ), 0 };
	return s;
}
}

size_t VideoFrame::Bytes() const { return blocks.size() + blocks2.size() + ( pixelBuffer ? size_t( w ) * h * 4 : 0 ); }

VideoFrame::~VideoFrame()
{
	if( pixelBuffer )
		CVPixelBufferRelease( (CVPixelBufferRef)pixelBuffer );
}

// Snappy: a varint length, then literals (tag 00) and copies with 1 / 2 / 4-byte offsets (tags 01 / 10 / 11)
bool SnappyDecompress( const uint8_t* in, size_t n, std::vector< uint8_t >& out )
{
	size_t i = 0, len = 0;
	for( int shift = 0; i < n && shift < 35; shift += 7 )
	{
		const uint8_t b = in[ i++ ];
		len |= size_t( b & 0x7F ) << shift;
		if( !( b & 0x80 ) )
			break;
	}
	out.resize( len );
	size_t o = 0;
	while( i < n )
	{
		const uint8_t tag = in[ i++ ];
		if( ( tag & 3 ) == 0 )
		{
			size_t l = tag >> 2;
			if( l >= 60 )
			{
				const int b = int( l - 59 );
				if( i + b > n )
					return false;
				l = Rd( in + i, b ), i += b;
			}
			++l;
			if( i + l > n || o + l > len )
				return false;
			std::memcpy( &out[ o ], in + i, l ), i += l, o += l;
			continue;
		}
		size_t l, off;
		if( ( tag & 3 ) == 1 )
		{
			if( i + 1 > n )
				return false;
			l = 4 + ( ( tag >> 2 ) & 7 ), off = ( size_t( tag >> 5 ) << 8 ) | in[ i ], i += 1;
		}
		else
		{
			const int b = ( tag & 3 ) == 2 ? 2 : 4;
			if( i + b > n )
				return false;
			l = ( tag >> 2 ) + 1, off = Rd( in + i, b ), i += b;
		}
		if( off == 0 || off > o || o + l > len )
			return false;
		for( size_t k = 0; k < l; ++k, ++o ) // may overlap: byte by byte
			out[ o ] = out[ o - off ];
	}
	return o == len;
}

bool DecodeHapFrame( const uint8_t* data, size_t n, int w, int h, VideoFrame& out, std::string& error )
{
	size_t size, hdr;
	int type;
	if( !Section( data, n, size, type, hdr ) )
	{
		error = "HAP: truncated frame";
		return false;
	}
	const size_t blocks = size_t( ( w + 3 ) / 4 ) * size_t( ( h + 3 ) / 4 );
	auto check = [ & ]( const std::vector< uint8_t >& v, int f ) { return v.size() >= blocks * ( f == 1 || f == 4 ? 8 : 16 ); };
	if( type == 0x0D ) // multiple images: Hap Q Alpha = scaled YCoCg DXT5 + BC4 alpha
	{
		out.format = 0;
		for( size_t q = hdr; q < hdr + size; )
		{
			size_t s, hh;
			int t;
			if( !Section( data + q, hdr + size - q, s, t, hh ) )
				break;
			std::vector< uint8_t > v;
			int f = 0;
			if( !TextureSection( t, data + q + hh, s, v, f, error ) || !check( v, f ) )
				return error.empty() ? ( error = "HAP: short plane", false ) : false;
			if( f == 4 )
				out.blocks2.swap( v );
			else
				out.blocks.swap( v ), out.format = f;
			q += hh + s;
		}
		if( !out.format && !out.blocks2.empty() )
			out.blocks.swap( out.blocks2 ), out.format = 4;
		return out.format != 0;
	}
	if( !TextureSection( type, data + hdr, size, out.blocks, out.format, error ) )
		return false;
	if( !check( out.blocks, out.format ) )
	{
		error = "HAP: short plane";
		return false;
	}
	return true;
}

VideoPlayer::VideoPlayer( const std::string& p ) :
	path( p )
{
	worker = std::thread( [ this ]() { Run(); } );
}

VideoPlayer::~VideoPlayer()
{
	stop.store( true );
	cv.notify_all();
	if( worker.joinable() )
		worker.join();
}

std::string VideoPlayer::Info() const
{
	std::lock_guard< std::mutex > lock( mtx );
	return info;
}

std::string VideoPlayer::BadInfo( uint64_t* n ) const
{
	std::lock_guard< std::mutex > lock( mtx );
	*n = badFrames;
	return badInfo;
}

std::string VideoPlayer::Codec() const
{
	std::lock_guard< std::mutex > lock( mtx );
	return codec;
}

std::vector< VideoPlayer::ScrubStat > VideoPlayer::TakeScrub( uint64_t* superseded )
{
	std::lock_guard< std::mutex > lock( mtx );
	std::vector< ScrubStat > r;
	r.swap( scrubStats );
	if( superseded )
		*superseded = scrubSuperseded;
	scrubSuperseded = 0;
	return r;
}

std::shared_ptr< const VideoFrame > VideoPlayer::Frame( double pos, bool forward, bool loop, int hold )
{
	std::lock_guard< std::mutex > lock( mtx );
	const bool scrub = hold != 0;
	wantPos = pos, wantForward = forward, wantLoop = loop, wantScrub = scrub, ++wantSerial;
	cv.notify_one();
	if( frames <= 0 )
		return last;
	const int target = std::clamp( int( std::floor( pos / frameDur + 1e-4 ) ), 0, frames - 1 );
	if( !scrub )
		scrubTarget = -1;
	else if( target != scrubTarget )
	{
		if( scrubTarget >= 0 )
			++scrubSuperseded; // a newer position before the last one arrived (a fast drag: the latest wins)
		scrubTarget = target, scrubHeld = 0, scrubT0 = std::chrono::steady_clock::now();
		if( last && last->index == target )
			scrubTarget = -1; // already on screen
	}
	auto it = cache.find( target );
	std::shared_ptr< const VideoFrame > pick;
	if( it != cache.end() )
	{
		pick = it->second;
		if( scrub && scrubTarget == target )
		{
			scrubStats.push_back( { target, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - scrubT0 ).count(), scrubHeld } );
			scrubTarget = -1;
		}
	}
	else if( scrub )
	{
		// never a black frame: until the wanted one is decoded, the decoded frame nearest to it on the way from the last one shown
		// (from the 3rd render waiting: a fast drag moves on through what the reader delivers instead of freezing), else the last one
		// stays. W6: Scrub runs only on all-intra codecs now, and this still fires there: HAP / HAP Q 4K drags (W6 sweep: 25 / 63
		// pictures on the way), so it stays
		pick = last;
		if( last && hold == 1 && scrubHeld >= 2 )
			for( const auto& kv : cache )
				if( std::min( last->index, target ) <= kv.first && kv.first <= std::max( last->index, target ) &&
				    std::abs( kv.first - target ) < std::abs( pick->index - target ) )
					pick = kv.second;
		++scrubHeld;
	}
	else
	{
		// not decoded yet: the nearest frame behind the play position, else the last one shown
		for( const auto& kv : cache )
			if( forward ? kv.first <= target && ( !pick || kv.first > pick->index ) : kv.first >= target && ( !pick || kv.first < pick->index ) )
				pick = kv.second;
		if( !pick )
			pick = last;
		if( !last || !pick || pick->index != last->index )
			++late;
	}
	if( pick )
		last = pick;
	return last;
}

// what is needed: the play position and kAhead frames on in the play direction (wrapped when looping); frames outside it leave
// the cache. Returns the first frame to decode (the one just behind the play position is kept, never fetched), -1 = none.
int VideoPlayer::Window( std::vector< int >& need )
{
	// backward, the window also keeps a whole chunk below the near frames: a chunk is read (forward) only once a frame within kAhead
	// of the play position is missing, and every frame it delivers stays until the play position passes it (W4: with a window of
	// `behind` frames each chunk re-read ~31 frames to keep 2 and one-key-frame H.264 fell behind)
	const int t     = std::clamp( int( std::floor( wantPos / frameDur + 1e-4 ) ), 0, frames - 1 );
	const int reach = wantForward ? kAhead : kAhead + behind;
	int first       = -1;
	for( int k = -1; k <= reach; ++k )
	{
		int i = wantForward ? t + k : t - k;
		if( wantLoop )
			i = ( i % frames + frames ) % frames;
		else if( i < 0 || i >= frames )
			continue;
		if( std::find( need.begin(), need.end(), i ) == need.end() )
			need.push_back( i );
		// the one just behind the play position is kept, never fetched (a frame the wrap also puts ahead, e.g. a 1-frame clip, is)
		if( first < 0 && k >= 0 && k <= kAhead && !cache.count( i ) && !( size_t( i ) < bad.size() && bad[ size_t( i ) ] ) )
			first = i;
	}
	for( auto it = cache.begin(); it != cache.end(); )
		it = std::find( need.begin(), need.end(), it->first ) == need.end() ? cache.erase( it ) : std::next( it );
	cacheBytes = 0;
	for( const auto& kv : cache )
		cacheBytes += kv.second->Bytes();
	return first;
}

// W3: a DXV file through DXVKit. Every packet is a whole frame, so the wanted frame is read and decoded directly: its block planes
// (plane 0 over the 16-padded picture, HQ chroma at half size) go to the renderer as they are. Alpha is left as stored (Video Alpha).
// W8 (DXVKit 1.0.0): the packet decides legacy vs DXV3 (DXVK_GEN_AUTO). A file whose first frame does not decode is refused (Arena 7.28
// plays such files black: a DXV3 version byte below 4, a stored flag above 1); a later frame that does not decode is skipped (the last
// frame stays, the error is kept for the Debug Log).
bool VideoPlayer::RunDxv()
{
	dxvk_mov* mov = nullptr;
	if( dxvk_mov_open_file( path.c_str(), &mov ) != DXVK_OK )
		return false;
	const dxvk_mov_info in = *dxvk_mov_get_info( mov );
	std::vector< uint8_t > packet( in.max_packet_size );
	const double fd      = in.frame_rate > 0 ? 1.0 / in.frame_rate : 1.0 / 30.0;
	dxvk_variant variant = DXVK_VARIANT_DXT1;
	auto decode          = [ & ]( int i, VideoFrame& f ) {
		size_t n = 0;
		int stored;
		dxvk_frame_layout lay{};
		dxvk_status st = dxvk_mov_read_packet( mov, uint32_t( i ), packet.data(), packet.size(), &n );
		if( st == DXVK_OK )
			st = dxvk_packet_variant( DXVK_GEN_AUTO, packet.data(), n, &variant, &stored );
		if( st == DXVK_OK )
			st = dxvk_layout( variant, in.width, in.height, &lay );
		if( st == DXVK_OK )
		{
			f.blocks.resize( lay.bytes[ 0 ] ), f.blocks2.resize( lay.plane_count > 1 ? lay.bytes[ 1 ] : 0 );
			void* const planes[ 2 ] = { f.blocks.data(), f.blocks2.data() };
			const size_t caps[ 2 ]  = { f.blocks.size(), f.blocks2.size() };
			st = dxvk_decode_blocks( DXVK_GEN_AUTO, packet.data(), n, in.width, in.height, nullptr, planes, caps, &lay, nullptr );
		}
		f.pts = i * fd, f.index = i, f.w = int( in.width ), f.h = int( in.height ), f.serial = ( 1ull << 62 ) + ++g_frameSerial;
		f.tw = int( lay.coded_width ), f.th = int( lay.coded_height );
		f.format = lay.variant == DXVK_VARIANT_YCG6 ? 7 : lay.variant == DXVK_VARIANT_YG10 ? 8 : lay.format[ 0 ] == DXVK_BLOCKS_BC3 ? 5 : 1;
		return st;
	};
	auto f0               = std::make_shared< VideoFrame >();
	const dxvk_status st0 = in.frame_count ? decode( 0, *f0 ) : DXVK_ERR_RANGE;
	{
		std::lock_guard< std::mutex > lock( mtx );
		const std::string name = path.substr( path.find_last_of( '/' ) + 1 );
		if( st0 != DXVK_OK )
		{
			info = "DXV frame 0 of " + name + ": " + dxvk_status_string( st0 ), failed.store( true );
			dxvk_mov_close( mov );
			return true;
		}
		const char* var = dxvk_variant_name( variant );
		frameDur = fd;
		frames   = int( in.frame_count );
		duration = in.timescale && in.duration ? double( in.duration ) / in.timescale : frames * frameDur;
		char b[ 200 ];
		snprintf( b, sizeof( b ), "DXV %s %ux%u, %.4g fps, %.2f s (%d frames), DXVKit %s: block upload", var, in.width, in.height, 1.0 / frameDur, duration, frames,
		          dxvk_version_string() );
		info  = b;
		codec = std::string( "DXV " ) + var;
		bad.assign( size_t( frames ), 0 );
		cache[ 0 ] = f0;
		++decoded;
	}
	intra.store( true ); // every DXV packet is a whole frame
	ready.store( true );
	while( !stop.load() )
	{
		std::vector< int > need;
		int first;
		{
			std::unique_lock< std::mutex > lock( mtx );
			if( ( first = Window( need ) ) < 0 )
			{
				const uint64_t s = wantSerial;
				cv.wait_for( lock, std::chrono::milliseconds( 4 ), [ & ] { return stop.load() || wantSerial != s; } );
				continue;
			}
		}
		const auto c0         = std::chrono::steady_clock::now();
		auto f                = std::make_shared< VideoFrame >();
		const dxvk_status st  = decode( first, *f );
		std::lock_guard< std::mutex > lock( mtx );
		if( st != DXVK_OK )
		{
			bad[ size_t( first ) ] = 1;
			if( !badFrames++ )
				badInfo = "DXV frame " + std::to_string( first ) + ": " + dxvk_status_string( st );
			continue;
		}
		cache[ first ] = f;
		++decoded;
		decodeMs += std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - c0 ).count();
	}
	dxvk_mov_close( mov );
	return true;
}

void VideoPlayer::Run()
{
	if( RunDxv() )
		return;
	@autoreleasepool
	{
		NSURL* url         = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
		AVURLAsset* asset  = [AVURLAsset URLAssetWithURL:url options:@{ AVURLAssetPreferPreciseDurationAndTimingKey : @YES }];
		AVAssetTrack* track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
		if( !track )
		{
			std::lock_guard< std::mutex > lock( mtx );
			info = "no video track in " + path.substr( path.find_last_of( '/' ) + 1 );
			failed.store( true );
			return;
		}
		CMFormatDescriptionRef fd = track.formatDescriptions.count ? (__bridge CMFormatDescriptionRef)track.formatDescriptions[ 0 ] : nullptr;
		const uint32_t sub        = fd ? CMFormatDescriptionGetMediaSubType( fd ) : 0;
		const std::string cc      = FourCC( sub );
		const bool hap            = cc == "Hap1" || cc == "Hap5" || cc == "HapY" || cc == "HapM" || cc == "HapA";
		const CGSize sz           = track.naturalSize;
		const int W = int( std::lround( sz.width ) ), H = int( std::lround( sz.height ) );
		float fps = track.nominalFrameRate;
		if( fps <= 0 && CMTIME_IS_VALID( track.minFrameDuration ) && CMTimeGetSeconds( track.minFrameDuration ) > 0 )
			fps = float( 1.0 / CMTimeGetSeconds( track.minFrameDuration ) );
		if( fps <= 0 )
			fps = 30;
		{
			std::lock_guard< std::mutex > lock( mtx );
			duration = CMTimeGetSeconds( asset.duration );
			frameDur = 1.0 / fps;
			frames   = std::max( 1, int( std::lround( duration * fps ) ) );
			char b[ 160 ];
			snprintf( b, sizeof( b ), "%s %dx%d, %.3g fps, %.2f s (%d frames)%s", cc.c_str(), W, H, fps, duration, frames, hap ? ", HAP: block upload" : ", decoded to BGRA IOSurface" );
			info  = b;
			codec = cc;
			// backward play reads chunks forward from a few frames before the play position: keep more of them (fewer reader restarts,
			// each of which decodes from the key frame before it) within ~256 MB of decoded frames
			behind = hap ? 24 : std::clamp( int( ( 256ull << 20 ) / ( size_t( std::max( 1, W ) ) * std::max( 1, H ) * 4 ) ) - kAhead, kAhead, 30 );
			if( cc == "Hap7" || cc == "HapH" )
				info = cc + ": BC7 / BC6 HAP is not supported (GL 4.1 on this Mac has no BPTC)", failed.store( true );
			// W8: a codec macOS cannot decode (a DXV sample entry DXVKit does not open: FourCC DXV3 / DXT1 / YCG6 / DXD1 / DXDA, which
			// Arena 7.28 does not take as a clip either) is refused; the reader used to reopen on it forever without a frame
			else if( !hap && !track.decodable )
				info = cc + ": no decoder for this codec on this Mac", failed.store( true );
			intra.store( hap ); // HAP: every frame is a key frame of raw blocks
		}
		if( failed.load() )
			return;
		// W4: the key frames (a reader opened at frame i decodes from the key frame at or before i). All-intra codecs: every frame.
		std::vector< int > syncs;
		bool allSync = true;
		if( track.canProvideSampleCursors )
			if( AVSampleCursor* c = [track makeSampleCursorAtFirstSampleInDecodeOrder] )
				do
				{
					if( c.currentSampleSyncInfo.sampleIsFullSync )
						syncs.push_back( std::clamp( int( std::lround( CMTimeGetSeconds( c.presentationTimeStamp ) * fps ) ), 0, frames - 1 ) );
					else
						allSync = false;
				} while( [c stepInDecodeOrderByCount:1] == 1 && !stop.load() );
		std::sort( syncs.begin(), syncs.end() );
		{
			std::lock_guard< std::mutex > lock( mtx );
			if( !allSync )
				keys = syncs;
			char b[ 64 ];
			snprintf( b, sizeof( b ), allSync ? ", every frame a key frame" : ", %zu key frames", syncs.size() );
			info += b;
		}
		// frames decoded to reach frame i by opening a reader there
		auto keyCost = [ & ]( int i ) {
			if( keys.empty() )
				return 0;
			auto k = std::upper_bound( keys.begin(), keys.end(), i );
			return i - ( k == keys.begin() ? 0 : *( k - 1 ) );
		};
		constexpr int kOpenCost = 16; // a new reader, in frames delivered (measured W4: cancel ~15 ms + start ~15 ms + first sample ~9 ms)
		ready.store( true );
		NSDictionary* settings = hap ? nil : @{ (id)kCVPixelBufferPixelFormatTypeKey : @( kCVPixelFormatType_32BGRA ), (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
		                                         (id)kCVPixelBufferOpenGLCompatibilityKey : @YES };
		AVAssetReader* reader             = nil;
		AVAssetReaderTrackOutput* output  = nil;
		int readerNext                    = -1; // the frame the open reader delivers next (-1 = none)
		// one sample -> the cache if it is wanted (HAP: the raw sections decoded; else the BGRA IOSurface buffer kept); its frame index
		auto take = [ & ]( CMSampleBufferRef sb, const std::vector< int >& need, std::chrono::steady_clock::time_point c0 ) {
			const double pts = CMTimeGetSeconds( CMSampleBufferGetPresentationTimeStamp( sb ) );
			const int idx    = std::clamp( int( std::lround( pts / frameDur ) ), 0, frames - 1 );
			if( CMSampleBufferGetNumSamples( sb ) == 0 )
				return -1;
			bool wanted;
			{
				std::lock_guard< std::mutex > lock( mtx );
				wanted = std::find( need.begin(), need.end(), idx ) != need.end() && !cache.count( idx );
			}
			if( !wanted )
				return idx;
			auto f = std::make_shared< VideoFrame >();
			f->pts = idx * frameDur, f->index = idx, f->w = W, f->h = H, f->serial = ( 1ull << 62 ) + ++g_frameSerial; // never equal to a still image's serial
			f->tw = ( W + 3 ) / 4 * 4, f->th = ( H + 3 ) / 4 * 4;
			bool ok = true;
			if( hap )
			{
				CMBlockBufferRef bb = CMSampleBufferGetDataBuffer( sb );
				const size_t n      = bb ? CMBlockBufferGetDataLength( bb ) : 0;
				std::vector< uint8_t > bytes( n );
				std::string error;
				ok = n > 0 && CMBlockBufferCopyDataBytes( bb, 0, n, bytes.data() ) == kCMBlockBufferNoErr && DecodeHapFrame( bytes.data(), n, W, H, *f, error );
				if( !ok && error.empty() )
					error = "HAP: empty sample (" + std::to_string( n ) + " bytes, " + std::to_string( CMSampleBufferGetNumSamples( sb ) ) + " samples)";
				if( !ok )
				{
					std::lock_guard< std::mutex > lock( mtx );
					info = error, failed.store( true );
				}
			}
			else if( CVImageBufferRef ib = CMSampleBufferGetImageBuffer( sb ) )
			{
				f->pixelBuffer = (void*)CVPixelBufferRetain( (CVPixelBufferRef)ib );
				f->w = (int)CVPixelBufferGetWidth( (CVPixelBufferRef)ib ), f->h = (int)CVPixelBufferGetHeight( (CVPixelBufferRef)ib );
			}
			else
				ok = false;
			if( ok )
			{
				std::lock_guard< std::mutex > lock( mtx );
				cache[ idx ] = f;
				++decoded;
				decodeMs += std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - c0 ).count();
			}
			return idx;
		};
		while( !stop.load() )
		{
			std::vector< int > need;
			int first;
			{
				std::unique_lock< std::mutex > lock( mtx );
				if( ( first = Window( need ) ) < 0 )
				{
					const uint64_t s = wantSerial;
					cv.wait_for( lock, std::chrono::milliseconds( 4 ), [ & ] { return stop.load() || wantSerial != s; } );
					continue;
				}
			}
			bool fwd, scrub;
			{
				std::lock_guard< std::mutex > lock( mtx );
				fwd = wantForward, scrub = wantScrub;
			}
			// where a new reader would start: the frame (forward); backward a chunk before it read forward, all kept (a held frame,
			// Scrub or a cue: no earlier than its key frame, so the wanted frame is not delayed by frames that are not decoded anyway)
			int start = fwd ? first : std::max( 0, first - behind );
			if( !fwd && scrub ) // no earlier than the key frame, but at least kAhead frames (all-intra: a backward drag is not one reader per frame)
				start = std::max( start, first - std::max( keyCost( first ), kAhead ) );
			// W4: keep reading the open reader while that delivers fewer frames than a new reader would (key frame to the frame + a
			// restart, ~16 frames' worth: cancel ~15 ms + start ~15 ms + first sample ~9 ms, measured W4); else a new reader there. The
			// latest request wins: each sample re-reads it. (A random-access reader re-aimed with resetForReadingTimeRanges was ~10x
			// faster per jump but corrupted MediaToolbox's heap and deadlocked a worker inside copyNextSampleBuffer: dead end, LOADEHR-009.)
			const int cont      = reader && readerNext >= 0 && readerNext <= first ? first - readerNext : INT_MAX;
			const double reopen = first - start + keyCost( start ) + kOpenCost;
			if( cont > reopen )
			{
				[reader cancelReading];
				reader          = nil;
				NSError* err    = nil;
				reader          = [AVAssetReader assetReaderWithAsset:asset error:&err];
				output          = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
				output.alwaysCopiesSampleData = NO;
				if( !reader || ![reader canAddOutput:output] )
				{
					std::lock_guard< std::mutex > lock( mtx );
					info = "cannot read " + cc + ( err ? std::string( ": " ) + err.localizedDescription.UTF8String : std::string() );
					failed.store( true );
					return;
				}
				[reader addOutput:output];
				// W4: a quarter frame INTO the start frame. AVAssetReader hands out the frame covering t0 with its time clamped to t0:
				// opened at start - 0.5 it delivered frame start - 1 stamped (start - 0.5), which rounded to start (picture one frame early)
				const CMTime t0 = CMTimeMakeWithSeconds( ( start + 0.25 ) * frameDur, 600000 );
				reader.timeRange = CMTimeRangeFromTimeToTime( t0, asset.duration );
				[reader startReading];
				readerNext = start;
			}
			const auto c0 = std::chrono::steady_clock::now();
			CMSampleBufferRef sb = [output copyNextSampleBuffer];
			if( !sb )
			{
				readerNext = -1; // the end (or an error): the next request opens a new reader
				reader     = nil;
				continue;
			}
			if( CMSampleBufferGetNumSamples( sb ) > 0 ) // else a marker buffer (no frame in it)
				readerNext = take( sb, need, c0 ) + 1;
			CFRelease( sb );
			if( failed.load() )
				return;
		}
		[reader cancelReading];
	}
}
}
