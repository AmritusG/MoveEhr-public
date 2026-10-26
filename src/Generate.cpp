#include "Generate.h"
#include "Log.h"
#include "ModelLoader.h"
#include "clipper2/clipper.h"
#include "earcut.hpp"
#include <ApplicationServices/ApplicationServices.h>
#include <CoreText/CoreText.h>
#include <algorithm>
#include <array>
#include <tuple>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <sys/stat.h>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#include "stb_image.h"

namespace mehr
{
namespace
{
constexpr char kSep = '\x1e';

// ---------------------------------------------------------------- outlines
struct Contour
{
	std::vector< float > xy; // closed, no repeated end point
};
struct Shape
{
	std::vector< Contour > contours;
	Clipper2Lib::FillRule fill = Clipper2Lib::FillRule::NonZero; // the source's fill rule
	float color[ 4 ]  = { 1, 1, 1, 1 };
};
struct Outline
{
	std::vector< Shape > shapes;
	float unit = 1;                   // what "1" means for Extrude Depth / Bevel Size (a line height, the logo height)
	std::shared_ptr< Image > image;   // raster logos: mapped onto the front
	float imageBox[ 4 ] = { 0, 0, 1, 1 }; // x0 y0 x1 y1 of the image in outline space (y up)
	bool fileColours = false;
};

// curve flattening: tolerance in outline units
struct Flattener
{
	Contour cur;
	std::vector< Contour >* out;
	float tol;
	float lx = 0, ly = 0;
	void Move( float x, float y ) { Close(), cur.xy = { x, y }, lx = x, ly = y; }
	void Line( float x, float y )
	{
		if( std::fabs( x - lx ) + std::fabs( y - ly ) > 1e-6f * ( 1 + std::fabs( x ) ) )
			cur.xy.push_back( x ), cur.xy.push_back( y ), lx = x, ly = y;
	}
	void Cubic( float x1, float y1, float x2, float y2, float x3, float y3 )
	{
		const float len = std::hypot( x1 - lx, y1 - ly ) + std::hypot( x2 - x1, y2 - y1 ) + std::hypot( x3 - x2, y3 - y2 );
		const int n     = std::clamp( int( std::ceil( std::sqrt( len / std::max( tol, 1e-9f ) ) * 1.2f ) ), 1, 64 );
		const float x0 = lx, y0 = ly;
		for( int i = 1; i <= n; ++i )
		{
			const float t = float( i ) / n, u = 1 - t;
			Line( u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3 );
		}
	}
	void Quad( float x1, float y1, float x2, float y2 ) { Cubic( lx + 2.0f / 3 * ( x1 - lx ), ly + 2.0f / 3 * ( y1 - ly ), x2 + 2.0f / 3 * ( x1 - x2 ), y2 + 2.0f / 3 * ( y1 - y2 ), x2, y2 ); }
	void Close()
	{
		if( cur.xy.size() >= 4 && std::fabs( cur.xy[ 0 ] - cur.xy[ cur.xy.size() - 2 ] ) + std::fabs( cur.xy[ 1 ] - cur.xy.back() ) < 1e-6f * ( 1 + std::fabs( cur.xy[ 0 ] ) ) )
			cur.xy.resize( cur.xy.size() - 2 );
		if( cur.xy.size() >= 6 )
			out->push_back( cur );
		cur.xy.clear();
	}
};

std::string Lower( std::string s )
{
	std::transform( s.begin(), s.end(), s.begin(), ::tolower );
	return s;
}
std::string CFStr( CFStringRef s )
{
	if( !s )
		return {};
	char buf[ 1024 ];
	if( CFStringGetCString( s, buf, sizeof( buf ), kCFStringEncodingUTF8 ) )
		return buf;
	return {};
}
CFStringRef MakeCF( const std::string& s ) { return CFStringCreateWithCString( nullptr, s.c_str(), kCFStringEncodingUTF8 ); }

// ---------------------------------------------------------------- text (CoreText)
void PathApply( void* info, const CGPathElement* e )
{
	Flattener* f = (Flattener*)info;
	const CGPoint* p = e->points;
	switch( e->type )
	{
	case kCGPathElementMoveToPoint: f->Move( p[ 0 ].x, p[ 0 ].y ); break;
	case kCGPathElementAddLineToPoint: f->Line( p[ 0 ].x, p[ 0 ].y ); break;
	case kCGPathElementAddQuadCurveToPoint: f->Quad( p[ 0 ].x, p[ 0 ].y, p[ 1 ].x, p[ 1 ].y ); break;
	case kCGPathElementAddCurveToPoint: f->Cubic( p[ 0 ].x, p[ 0 ].y, p[ 1 ].x, p[ 1 ].y, p[ 2 ].x, p[ 2 ].y ); break;
	case kCGPathElementCloseSubpath: f->Close(); break;
	}
}

CTFontRef MakeFont( const GenParams& g, float size, std::string& note )
{
	const std::string file = g.Get( "fontfile" ), family = g.Get( "font", "Helvetica Neue" ), style = g.Get( "style" );
	if( !file.empty() )
	{
		CFURLRef url = CFURLCreateFromFileSystemRepresentation( nullptr, (const UInt8*)file.c_str(), (CFIndex)file.size(), false );
		CFArrayRef descs = url ? CTFontManagerCreateFontDescriptorsFromURL( url ) : nullptr;
		if( url )
			CFRelease( url );
		if( descs && CFArrayGetCount( descs ) > 0 )
		{
			CTFontRef f = CTFontCreateWithFontDescriptor( (CTFontDescriptorRef)CFArrayGetValueAtIndex( descs, 0 ), size, nullptr );
			CFRelease( descs );
			return f;
		}
		if( descs )
			CFRelease( descs );
		note = "Font file unreadable: " + file.substr( file.find_last_of( '/' ) + 1 ) + ", using " + family;
	}
	CFStringRef fam = MakeCF( family ), sty = style.empty() ? nullptr : MakeCF( style );
	const void* keys[ 2 ] = { kCTFontFamilyNameAttribute, kCTFontStyleNameAttribute };
	const void* vals[ 2 ] = { fam, sty };
	CFDictionaryRef attrs = CFDictionaryCreate( nullptr, keys, vals, sty ? 2 : 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
	CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes( attrs );
	CTFontRef f           = CTFontCreateWithFontDescriptor( d, size, nullptr );
	CFRelease( d ), CFRelease( attrs ), CFRelease( fam );
	if( sty )
		CFRelease( sty );
	CFStringRef got = CTFontCopyFamilyName( f );
	if( Lower( CFStr( got ) ) != Lower( family ) && note.empty() )
		note = "Font not found: " + family + ", using " + CFStr( got );
	if( got )
		CFRelease( got );
	return f;
}

}
int GlyphAtlas( const std::string& text, const std::string& family, const std::string& style, const std::string& file, std::vector< uint8_t >& out )
{
	CFStringRef str = MakeCF( text );
	std::vector< UniChar > chars;
	if( str )
	{
		const CFIndex n = CFStringGetLength( str );
		for( CFIndex i = 0; i < n && chars.size() < 64; ++i )
		{
			const UniChar c = CFStringGetCharacterAtIndex( str, i );
			if( c > ' ' && c != '|' && std::find( chars.begin(), chars.end(), c ) == chars.end() )
				chars.push_back( c );
		}
		CFRelease( str );
	}
	if( chars.empty() )
		for( const char* p = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; *p; ++p )
			chars.push_back( UniChar( *p ) );
	GenParams g;
	g.kv[ "font" ] = family.empty() ? "Helvetica Neue" : family, g.kv[ "style" ] = style, g.kv[ "fontfile" ] = file;
	std::string note;
	CTFontRef font = MakeFont( g, 46.0f, note );
	out.assign( 512 * 512, 0 );
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
	CGContextRef ctx   = CGBitmapContextCreate( out.data(), 512, 512, 8, 512, cs, kCGImageAlphaNone );
	CGColorSpaceRelease( cs );
	CGContextSetGrayFillColor( ctx, 1.0, 1.0 );
	CGContextSetShouldAntialias( ctx, true );
	for( size_t i = 0; i < chars.size(); ++i )
	{
		CGGlyph gl = 0;
		if( !CTFontGetGlyphsForCharacters( font, &chars[ i ], &gl, 1 ) )
			continue;
		const CGRect bb = CTFontGetBoundingRectsForGlyphs( font, kCTFontOrientationDefault, &gl, nullptr, 1 );
		// cell (i % 8, i / 8) from the top; CG's origin is the bottom-left of the bitmap (memory row 0 = the top)
		const double cx = ( i % 8 ) * 64 + 32, cy = 512 - ( ( i / 8 ) * 64 + 32 );
		const CGPoint at = CGPointMake( cx - ( bb.origin.x + bb.size.width * 0.5 ), cy - ( bb.origin.y + bb.size.height * 0.5 ) );
		CTFontDrawGlyphs( font, &gl, &at, 1, ctx );
	}
	CGContextRelease( ctx );
	CFRelease( font );
	return int( chars.size() );
}
namespace
{
bool TextOutline( const GenParams& g, Outline& o, std::string& status, std::string& error )
{
	std::string text = g.Get( "text" );
	text.erase( std::remove( text.begin(), text.end(), '\r' ), text.end() );
	// Arena's FFGL text field is one line (Return commits it), so '|' starts a new row too (W21, Amrit's pick)
	std::replace( text.begin(), text.end(), '|', '\n' );
	if( text.find_first_not_of( " \n\t" ) == std::string::npos )
	{
		error = status = "Type some text (Text)";
		return false;
	}
	const float size = 100.0f;
	std::string note;
	CTFontRef font   = MakeFont( g, size, note );
	const float asc = CTFontGetAscent( font ), desc = CTFontGetDescent( font ), lead = CTFontGetLeading( font );
	const float pitch = ( asc + desc + std::max( lead, size * 0.1f ) ) * g.F( "linespacing", 1 );
	const int align   = (int)g.F( "align", 1 );
	const float kern  = g.F( "letterspacing", 0 ) * size;
	const float tol   = size * ( 0.6f - 0.58f * std::clamp( g.F( "detail", 0.5f ), 0.0f, 1.0f ) ) * 0.02f;
	std::vector< std::string > rows;
	for( size_t a = 0;; )
	{
		const size_t b = text.find( '\n', a );
		rows.push_back( text.substr( a, b == std::string::npos ? std::string::npos : b - a ) );
		if( b == std::string::npos )
			break;
		a = b + 1;
	}
	CFNumberRef kernNum = CFNumberCreate( nullptr, kCFNumberFloatType, &kern );
	int letters = 0;
	for( size_t r = 0; r < rows.size(); ++r )
	{
		const float y0 = -float( r ) * pitch;
		if( rows[ r ].empty() )
			continue;
		CFStringRef str       = MakeCF( rows[ r ] );
		// Letter Spacing is TRACKING: the font's own pair kerning stays (a kern attribute, even 0, would switch it off)
		const void* keys[ 2 ] = { kCTFontAttributeName, kCTTrackingAttributeName };
		const void* vals[ 2 ] = { font, kernNum };
		CFDictionaryRef attrs = CFDictionaryCreate( nullptr, keys, vals, kern != 0.0f ? 2 : 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
		CFAttributedStringRef as = CFAttributedStringCreate( nullptr, str ? str : CFSTR( "" ), attrs );
		CTLineRef line           = CTLineCreateWithAttributedString( as );
		const double w           = CTLineGetTypographicBounds( line, nullptr, nullptr, nullptr ) - ( rows[ r ].empty() ? 0 : kern );
		const float x0           = align == 0 ? 0.0f : align == 1 ? float( -w * 0.5 ) : float( -w );
		CFArrayRef runs          = CTLineGetGlyphRuns( line );
		for( CFIndex ri = 0; ri < CFArrayGetCount( runs ); ++ri )
		{
			CTRunRef run    = (CTRunRef)CFArrayGetValueAtIndex( runs, ri );
			CTFontRef rfont = (CTFontRef)CFDictionaryGetValue( CTRunGetAttributes( run ), kCTFontAttributeName );
			const CFIndex n = CTRunGetGlyphCount( run );
			std::vector< CGGlyph > glyphs( n );
			std::vector< CGPoint > pos( n );
			CTRunGetGlyphs( run, CFRangeMake( 0, 0 ), glyphs.data() );
			CTRunGetPositions( run, CFRangeMake( 0, 0 ), pos.data() );
			for( CFIndex i = 0; i < n; ++i )
			{
				CGAffineTransform T = CGAffineTransformMakeTranslation( x0 + pos[ i ].x, y0 + pos[ i ].y );
				CGPathRef path      = CTFontCreatePathForGlyph( rfont ? rfont : font, glyphs[ i ], &T );
				if( !path )
					continue;
				Shape sh;
				Flattener fl;
				fl.out = &sh.contours, fl.tol = tol;
				CGPathApply( path, &fl, PathApply );
				fl.Close();
				CGPathRelease( path );
				if( !sh.contours.empty() )
					o.shapes.push_back( std::move( sh ) ), ++letters;
			}
		}
		CFRelease( line ), CFRelease( as ), CFRelease( attrs );
		if( str )
			CFRelease( str );
	}
	CFRelease( kernNum );
	CFStringRef fam = CTFontCopyFamilyName( font ), sty = (CFStringRef)CTFontCopyAttribute( font, kCTFontStyleNameAttribute );
	const std::string used = CFStr( fam ) + " " + CFStr( sty );
	if( fam )
		CFRelease( fam );
	if( sty )
		CFRelease( sty );
	CFRelease( font );
	o.unit = asc + desc; // Extrude Depth 1 = one letter height
	if( o.shapes.empty() )
	{
		error = status = "No visible letters in Text";
		return false;
	}
	status = ( note.empty() ? "" : note + " | " ) + "Text: " + std::to_string( letters ) + " letters, " + std::to_string( rows.size() ) + ( rows.size() == 1 ? " row (" : " rows (" ) + used + ")";
	return true;
}

// ---------------------------------------------------------------- SVG (nanosvg)
bool SvgOutline( const std::string& path, const GenParams& g, Outline& o, std::string& status, std::string& error )
{
	NSVGimage* img = nsvgParseFromFile( path.c_str(), "px", 96.0f );
	if( !img )
	{
		error = status = "SVG unreadable: " + path.substr( path.find_last_of( '/' ) + 1 );
		return false;
	}
	const float tol = std::max( img->width, img->height ) * 0.002f * ( 1.2f - std::clamp( g.F( "detail", 0.5f ), 0.0f, 1.0f ) );
	int strokesOnly = 0, gradients = 0;
	for( NSVGshape* s = img->shapes; s; s = s->next )
	{
		if( !( s->flags & NSVG_FLAGS_VISIBLE ) )
			continue;
		if( s->fill.type == NSVG_PAINT_NONE )
		{
			strokesOnly += s->stroke.type != NSVG_PAINT_NONE;
			continue;
		}
		Shape sh;
		sh.fill = s->fillRule == NSVG_FILLRULE_EVENODD ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero;
		unsigned int c = s->fill.color;
		if( s->fill.type == NSVG_PAINT_LINEAR_GRADIENT || s->fill.type == NSVG_PAINT_RADIAL_GRADIENT )
			c = s->fill.gradient && s->fill.gradient->nstops > 0 ? s->fill.gradient->stops[ 0 ].color : 0xff808080u, ++gradients;
		sh.color[ 0 ] = ( c & 0xff ) / 255.0f, sh.color[ 1 ] = ( ( c >> 8 ) & 0xff ) / 255.0f, sh.color[ 2 ] = ( ( c >> 16 ) & 0xff ) / 255.0f;
		sh.color[ 3 ] = s->opacity;
		Flattener fl;
		fl.out = &sh.contours, fl.tol = tol;
		for( NSVGpath* p = s->paths; p; p = p->next )
		{
			fl.Move( p->pts[ 0 ], -p->pts[ 1 ] ); // SVG y runs down
			for( int i = 0; i < p->npts - 1; i += 3 )
			{
				const float* q = &p->pts[ i * 2 ];
				fl.Cubic( q[ 2 ], -q[ 3 ], q[ 4 ], -q[ 5 ], q[ 6 ], -q[ 7 ] );
			}
			fl.Close();
		}
		if( !sh.contours.empty() )
			o.shapes.push_back( std::move( sh ) );
	}
	const float h = img->height > 0 ? img->height : 1;
	nsvgDelete( img );
	o.unit        = h;
	o.fileColours = true;
	if( o.shapes.empty() )
	{
		error = status = "SVG has no filled shapes" + std::string( strokesOnly ? " (strokes only: convert strokes to outlines)" : "" );
		return false;
	}
	status = "Loaded: " + path.substr( path.find_last_of( '/' ) + 1 ) + " (" + std::to_string( o.shapes.size() ) + " shapes" +
	         ( strokesOnly ? ", " + std::to_string( strokesOnly ) + " stroke-only skipped" : "" ) + ( gradients ? ", gradients as their first colour" : "" ) + ")";
	return true;
}

// ---------------------------------------------------------------- PDF / AI (CGPDFScanner)
struct PdfState
{
	CGAffineTransform ctm = CGAffineTransformIdentity;
	float fill[ 3 ]       = { 0, 0, 0 };
};
struct PdfCtx
{
	std::vector< PdfState > stack{ PdfState() };
	std::vector< Contour > path;
	Flattener fl;
	Outline* o;
	int depth = 0;
	CGPDFDictionaryRef xobjects = nullptr;
	PdfState& S() { return stack.back(); }
	void Pt( CGFloat x, CGFloat y, float& ox, float& oy )
	{
		CGPoint p = CGPointApplyAffineTransform( CGPointMake( x, y ), S().ctm );
		ox = p.x, oy = p.y;
	}
};
CGFloat Pop( CGPDFScannerRef s )
{
	CGPDFReal v = 0;
	CGPDFScannerPopNumber( s, &v );
	return v;
}
PdfCtx* Ctx( void* info ) { return (PdfCtx*)info; }
void OpQ( CGPDFScannerRef, void* i ) { Ctx( i )->stack.push_back( Ctx( i )->S() ); }
void OpBigQ( CGPDFScannerRef, void* i )
{
	if( Ctx( i )->stack.size() > 1 )
		Ctx( i )->stack.pop_back();
}
void OpCm( CGPDFScannerRef s, void* i )
{
	CGFloat f = Pop( s ), e = Pop( s ), d = Pop( s ), c = Pop( s ), b = Pop( s ), a = Pop( s );
	Ctx( i )->S().ctm = CGAffineTransformConcat( CGAffineTransformMake( a, b, c, d, e, f ), Ctx( i )->S().ctm );
}
void OpM( CGPDFScannerRef s, void* i )
{
	CGFloat y = Pop( s ), x = Pop( s );
	float ox, oy;
	Ctx( i )->Pt( x, y, ox, oy );
	Ctx( i )->fl.Move( ox, oy );
}
void OpL( CGPDFScannerRef s, void* i )
{
	CGFloat y = Pop( s ), x = Pop( s );
	float ox, oy;
	Ctx( i )->Pt( x, y, ox, oy );
	Ctx( i )->fl.Line( ox, oy );
}
void OpC( CGPDFScannerRef s, void* i )
{
	CGFloat y3 = Pop( s ), x3 = Pop( s ), y2 = Pop( s ), x2 = Pop( s ), y1 = Pop( s ), x1 = Pop( s );
	float a, b, c, d, e, f;
	Ctx( i )->Pt( x1, y1, a, b ), Ctx( i )->Pt( x2, y2, c, d ), Ctx( i )->Pt( x3, y3, e, f );
	Ctx( i )->fl.Cubic( a, b, c, d, e, f );
}
void OpV( CGPDFScannerRef s, void* i ) // first control point = current point
{
	CGFloat y3 = Pop( s ), x3 = Pop( s ), y2 = Pop( s ), x2 = Pop( s );
	float c, d, e, f;
	Ctx( i )->Pt( x2, y2, c, d ), Ctx( i )->Pt( x3, y3, e, f );
	Ctx( i )->fl.Cubic( Ctx( i )->fl.lx, Ctx( i )->fl.ly, c, d, e, f );
}
void OpY( CGPDFScannerRef s, void* i ) // second control point = end point
{
	CGFloat y3 = Pop( s ), x3 = Pop( s ), y1 = Pop( s ), x1 = Pop( s );
	float a, b, e, f;
	Ctx( i )->Pt( x1, y1, a, b ), Ctx( i )->Pt( x3, y3, e, f );
	Ctx( i )->fl.Cubic( a, b, e, f, e, f );
}
void OpH( CGPDFScannerRef, void* i ) { Ctx( i )->fl.Close(); }
void OpRe( CGPDFScannerRef s, void* i )
{
	CGFloat h = Pop( s ), w = Pop( s ), y = Pop( s ), x = Pop( s );
	float p[ 8 ];
	Ctx( i )->Pt( x, y, p[ 0 ], p[ 1 ] ), Ctx( i )->Pt( x + w, y, p[ 2 ], p[ 3 ] ), Ctx( i )->Pt( x + w, y + h, p[ 4 ], p[ 5 ] ), Ctx( i )->Pt( x, y + h, p[ 6 ], p[ 7 ] );
	Ctx( i )->fl.Move( p[ 0 ], p[ 1 ] );
	for( int k = 1; k < 4; ++k )
		Ctx( i )->fl.Line( p[ k * 2 ], p[ k * 2 + 1 ] );
	Ctx( i )->fl.Close();
}
void Fill( void* i, Clipper2Lib::FillRule fill )
{
	PdfCtx* c = Ctx( i );
	c->fl.Close();
	if( !c->path.empty() )
	{
		Shape sh;
		sh.fill     = fill;
		sh.contours = std::move( c->path );
		std::memcpy( sh.color, c->S().fill, sizeof( c->S().fill ) );
		c->o->shapes.push_back( std::move( sh ) );
	}
	c->path.clear();
}
void OpFill( CGPDFScannerRef, void* i ) { Fill( i, Clipper2Lib::FillRule::NonZero ); }
void OpFillEO( CGPDFScannerRef, void* i ) { Fill( i, Clipper2Lib::FillRule::EvenOdd ); }
void OpNoFill( CGPDFScannerRef, void* i )
{
	Ctx( i )->fl.Close();
	Ctx( i )->path.clear();
}
void OpRg( CGPDFScannerRef s, void* i )
{
	CGFloat b = Pop( s ), g = Pop( s ), r = Pop( s );
	float* f = Ctx( i )->S().fill;
	f[ 0 ] = r, f[ 1 ] = g, f[ 2 ] = b;
}
void OpG( CGPDFScannerRef s, void* i )
{
	CGFloat v = Pop( s );
	float* f  = Ctx( i )->S().fill;
	f[ 0 ] = f[ 1 ] = f[ 2 ] = v;
}
void OpK( CGPDFScannerRef s, void* i )
{
	CGFloat k = Pop( s ), y = Pop( s ), m = Pop( s ), c = Pop( s );
	float* f = Ctx( i )->S().fill;
	f[ 0 ] = ( 1 - c ) * ( 1 - k ), f[ 1 ] = ( 1 - m ) * ( 1 - k ), f[ 2 ] = ( 1 - y ) * ( 1 - k );
}
void OpSc( CGPDFScannerRef s, void* i ) // sc / scn: 1 (gray), 3 (rgb) or 4 (cmyk) numbers; a pattern name is ignored
{
	std::vector< CGFloat > v;
	CGPDFReal x;
	while( v.size() < 4 && CGPDFScannerPopNumber( s, &x ) )
		v.insert( v.begin(), x );
	float* f = Ctx( i )->S().fill;
	if( v.size() == 1 )
		f[ 0 ] = f[ 1 ] = f[ 2 ] = v[ 0 ];
	else if( v.size() == 3 )
		f[ 0 ] = v[ 0 ], f[ 1 ] = v[ 1 ], f[ 2 ] = v[ 2 ];
	else if( v.size() == 4 )
		f[ 0 ] = ( 1 - v[ 0 ] ) * ( 1 - v[ 3 ] ), f[ 1 ] = ( 1 - v[ 1 ] ) * ( 1 - v[ 3 ] ), f[ 2 ] = ( 1 - v[ 2 ] ) * ( 1 - v[ 3 ] );
}
void ScanStream( CGPDFContentStreamRef cs, PdfCtx& ctx );
void OpDo( CGPDFScannerRef s, void* i ) // form XObjects (Illustrator groups): scanned with their Matrix
{
	PdfCtx* c = Ctx( i );
	const char* name = nullptr;
	if( !CGPDFScannerPopName( s, &name ) || !c->xobjects || c->depth > 8 )
		return;
	CGPDFStreamRef st = nullptr;
	if( !CGPDFDictionaryGetStream( c->xobjects, name, &st ) )
		return;
	CGPDFDictionaryRef d = CGPDFStreamGetDictionary( st );
	const char* sub      = nullptr;
	if( !CGPDFDictionaryGetName( d, "Subtype", &sub ) || std::strcmp( sub, "Form" ) != 0 )
		return;
	c->stack.push_back( c->S() );
	CGPDFArrayRef m = nullptr;
	if( CGPDFDictionaryGetArray( d, "Matrix", &m ) && CGPDFArrayGetCount( m ) == 6 )
	{
		CGPDFReal v[ 6 ];
		for( int k = 0; k < 6; ++k )
			CGPDFArrayGetNumber( m, k, &v[ k ] );
		c->S().ctm = CGAffineTransformConcat( CGAffineTransformMake( v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], v[ 4 ], v[ 5 ] ), c->S().ctm );
	}
	CGPDFDictionaryRef res = nullptr, prevX = c->xobjects;
	if( CGPDFDictionaryGetDictionary( d, "Resources", &res ) )
	{
		CGPDFDictionaryRef x = nullptr;
		if( CGPDFDictionaryGetDictionary( res, "XObject", &x ) )
			c->xobjects = x;
	}
	CGPDFContentStreamRef cs = CGPDFContentStreamCreateWithStream( st, res, nullptr );
	++c->depth;
	ScanStream( cs, *c );
	--c->depth;
	CGPDFContentStreamRelease( cs );
	c->xobjects = prevX;
	c->stack.pop_back();
}
void ScanStream( CGPDFContentStreamRef cs, PdfCtx& ctx )
{
	CGPDFOperatorTableRef t = CGPDFOperatorTableCreate();
	struct
	{
		const char* n;
		CGPDFOperatorCallback f;
	} ops[] = { { "q", OpQ }, { "Q", OpBigQ }, { "cm", OpCm }, { "m", OpM }, { "l", OpL }, { "c", OpC }, { "v", OpV }, { "y", OpY }, { "h", OpH }, { "re", OpRe },
		        { "f", OpFill }, { "F", OpFill }, { "B", OpFill }, { "b", OpFill }, { "f*", OpFillEO }, { "B*", OpFillEO }, { "b*", OpFillEO }, { "n", OpNoFill },
		        { "S", OpNoFill }, { "s", OpNoFill }, { "rg", OpRg }, { "g", OpG }, { "k", OpK }, { "sc", OpSc }, { "scn", OpSc }, { "Do", OpDo } };
	for( auto& o : ops )
		CGPDFOperatorTableSetCallback( t, o.n, o.f );
	CGPDFScannerRef sc = CGPDFScannerCreate( cs, t, &ctx );
	CGPDFScannerScan( sc );
	CGPDFScannerRelease( sc );
	CGPDFOperatorTableRelease( t );
}
bool PdfOutline( const std::string& path, const GenParams& g, Outline& o, std::string& status, std::string& error )
{
	const bool ai    = Lower( path.substr( path.find_last_of( '.' ) + 1 ) ) == "ai";
	const std::string name = path.substr( path.find_last_of( '/' ) + 1 );
	CFURLRef url     = CFURLCreateFromFileSystemRepresentation( nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), false );
	CGPDFDocumentRef doc = url ? CGPDFDocumentCreateWithURL( url ) : nullptr;
	if( url )
		CFRelease( url );
	if( !doc || CGPDFDocumentGetNumberOfPages( doc ) < 1 )
	{
		if( doc )
			CGPDFDocumentRelease( doc );
		error = status = ai ? name + ": no PDF stream (re-save with \"Create PDF Compatible File\" on, or export SVG)" : "PDF unreadable: " + name;
		return false;
	}
	CGPDFPageRef page = CGPDFDocumentGetPage( doc, 1 );
	const CGRect box  = CGPDFPageGetBoxRect( page, kCGPDFCropBox );
	PdfCtx ctx;
	ctx.o         = &o;
	ctx.fl.out    = &ctx.path;
	ctx.fl.tol    = std::max( box.size.width, box.size.height ) * 0.002f * ( 1.2f - std::clamp( g.F( "detail", 0.5f ), 0.0f, 1.0f ) );
	CGPDFDictionaryRef pd = CGPDFPageGetDictionary( page ), res = nullptr;
	if( CGPDFDictionaryGetDictionary( pd, "Resources", &res ) )
		CGPDFDictionaryGetDictionary( res, "XObject", &ctx.xobjects );
	CGPDFContentStreamRef cs = CGPDFContentStreamCreateWithPage( page );
	ScanStream( cs, ctx );
	CGPDFContentStreamRelease( cs );
	CGPDFDocumentRelease( doc );
	o.unit        = box.size.height > 0 ? box.size.height : 1;
	o.fileColours = true;
	if( o.shapes.empty() )
	{
		error = status = name + ": no filled paths on page 1 (text must be converted to outlines)";
		return false;
	}
	status = "Loaded: " + name + " (" + std::to_string( o.shapes.size() ) + " shapes" + std::string( ai ? ", via its PDF stream" : "" ) + ")";
	return true;
}

// ---------------------------------------------------------------- PNG / JPG: trace
void Simplify( const std::vector< float >& in, float eps, std::vector< float >& out ) // Douglas-Peucker on a closed loop
{
	const int n = int( in.size() / 2 );
	if( n < 8 )
	{
		out = in;
		return;
	}
	std::vector< char > keep( n, 0 );
	std::vector< std::pair< int, int > > st = { { 0, n / 2 }, { n / 2, n } };
	keep[ 0 ] = keep[ n / 2 ] = 1;
	while( !st.empty() )
	{
		auto [ a, b ] = st.back();
		st.pop_back();
		const float ax = in[ ( a % n ) * 2 ], ay = in[ ( a % n ) * 2 + 1 ], bx = in[ ( b % n ) * 2 ], by = in[ ( b % n ) * 2 + 1 ];
		float best = -1;
		int bi     = -1;
		for( int i = a + 1; i < b; ++i )
		{
			const float px = in[ i * 2 ], py = in[ i * 2 + 1 ];
			const float dx = bx - ax, dy = by - ay, l = std::hypot( dx, dy );
			const float d  = l > 1e-9f ? std::fabs( ( px - ax ) * dy - ( py - ay ) * dx ) / l : std::hypot( px - ax, py - ay );
			if( d > best )
				best = d, bi = i;
		}
		if( best > eps && bi > 0 )
		{
			keep[ bi ] = 1;
			st.push_back( { a, bi } ), st.push_back( { bi, b } );
		}
	}
	out.clear();
	for( int i = 0; i < n; ++i )
		if( keep[ i ] )
			out.push_back( in[ i * 2 ] ), out.push_back( in[ i * 2 + 1 ] );
}
void Chaikin( std::vector< float >& c, int iterations )
{
	for( int it = 0; it < iterations && c.size() >= 6; ++it )
	{
		std::vector< float > o;
		const size_t n = c.size() / 2;
		for( size_t i = 0; i < n; ++i )
		{
			const float* a = &c[ i * 2 ];
			const float* b = &c[ ( ( i + 1 ) % n ) * 2 ];
			o.push_back( 0.75f * a[ 0 ] + 0.25f * b[ 0 ] ), o.push_back( 0.75f * a[ 1 ] + 0.25f * b[ 1 ] );
			o.push_back( 0.25f * a[ 0 ] + 0.75f * b[ 0 ] ), o.push_back( 0.25f * a[ 1 ] + 0.75f * b[ 1 ] );
		}
		c.swap( o );
	}
}
bool RasterOutline( const std::string& path, const GenParams& g, Outline& o, std::string& status, std::string& error )
{
	const std::string name = path.substr( path.find_last_of( '/' ) + 1 );
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load( path.c_str(), &w, &h, &n, 4 );
	if( !px || w < 2 || h < 2 )
	{
		if( px )
			stbi_image_free( px );
		error = status = "Image unreadable: " + name;
		return false;
	}
	// the image for the front face (full resolution, sRGB, mips)
	auto img        = std::make_shared< Image >();
	img->width      = w, img->height = h, img->components = 4, img->srgb = true;
	img->pixels.assign( px, px + size_t( w ) * h * 4 );
	BuildImageMips( *img );
	// the trace works on at most 512 px along the longer side
	const int s   = std::max( 1, ( std::max( w, h ) + 511 ) / 512 );
	const int tw  = w / s, th = h / s;
	bool alpha    = false;
	for( size_t i = 3; i < size_t( w ) * h * 4 && !alpha; i += 4 )
		alpha = px[ i ] < 250;
	const float thr   = std::clamp( g.F( "threshold", 0.5f ), 0.01f, 0.99f );
	const bool invert = g.F( "invert", 0 ) > 0.5f;
	// the field: "how inside" per trace pixel (opacity, or darkness on a light background), inverted on request;
	// a 1 px empty border so every outline closes
	const int W = tw + 2, H = th + 2;
	const float T = alpha ? thr : 1.0f - thr;
	std::vector< float > f( size_t( W ) * H, invert ? 1.0f : 0.0f );
	std::vector< uint8_t > m( f.size(), 0 );
	for( int y = 0; y < th; ++y )
		for( int x = 0; x < tw; ++x )
		{
			float a = 0, l = 0;
			for( int dy = 0; dy < s; ++dy )
				for( int dx = 0; dx < s; ++dx )
				{
					const unsigned char* p = &px[ ( size_t( y * s + dy ) * w + ( x * s + dx ) ) * 4 ];
					a += p[ 3 ] / 255.0f, l += ( 0.2126f * p[ 0 ] + 0.7152f * p[ 1 ] + 0.0722f * p[ 2 ] ) / 255.0f * ( p[ 3 ] / 255.0f ) + ( 1 - p[ 3 ] / 255.0f );
				}
			a /= float( s * s ), l /= float( s * s );
			const float v = alpha ? a : 1.0f - l;
			f[ size_t( y + 1 ) * W + x + 1 ] = invert ? 1.0f - v : v;
		}
	stbi_image_free( px );
	const float Tf = invert ? 1.0f - T : T;
	for( int y = 0; y < H; ++y )
		for( int x = 0; x < W; ++x )
		{
			if( x == 0 || y == 0 || x == W - 1 || y == H - 1 )
				f[ size_t( y ) * W + x ] = 0.0f; // the border is always outside
			m[ size_t( y ) * W + x ] = f[ size_t( y ) * W + x ] > Tf;
		}
	// connected components (4-connected): one letter-like group each
	std::vector< int > lab( m.size(), 0 );
	int comps = 0;
	std::vector< int > stack;
	std::vector< int > areas{ 0 };
	std::vector< std::array< int, 4 > > bbox{ { 0, 0, 0, 0 } };
	for( size_t i = 0; i < m.size(); ++i )
		if( m[ i ] && !lab[ i ] )
		{
			++comps, areas.push_back( 0 ), bbox.push_back( { W, H, 0, 0 } );
			stack.push_back( int( i ) ), lab[ i ] = comps;
			while( !stack.empty() )
			{
				const int j = stack.back();
				stack.pop_back();
				++areas[ comps ];
				auto& bb = bbox[ comps ];
				bb[ 0 ] = std::min( bb[ 0 ], j % W ), bb[ 1 ] = std::min( bb[ 1 ], j / W ), bb[ 2 ] = std::max( bb[ 2 ], j % W ), bb[ 3 ] = std::max( bb[ 3 ], j / W );
				for( int d : { -1, 1, -W, W } )
				{
					const int k = j + d;
					if( k >= 0 && k < W * H && m[ k ] && !lab[ k ] )
						lab[ k ] = comps, stack.push_back( k );
				}
			}
		}
	// marching squares per component on the interpolated field (sub-pixel outlines), inside on the left (y up),
	// then Douglas-Peucker and Chaikin smoothing
	const int iters = int( std::lround( std::clamp( g.F( "smooth", 0.5f ), 0.0f, 1.0f ) * 3.0f ) );
	const float eps = 0.15f + 0.6f * ( 1.0f - std::clamp( g.F( "detail", 0.5f ), 0.0f, 1.0f ) ); // trace pixels
	for( int c = 1; c <= comps; ++c )
	{
		if( areas[ c ] < 12 )
			continue;
		const auto& bb = bbox[ c ];
		const int x0 = bb[ 0 ] - 1, y0 = bb[ 1 ] - 1, x1 = bb[ 2 ] + 1, y1 = bb[ 3 ] + 1; // cells [x0, x1) x [y0, y1)
		auto F = [ & ]( int x, int y ) -> float {
			const size_t i = size_t( y ) * W + x;
			return lab[ i ] == c ? std::max( f[ i ], Tf + 1e-4f ) : ( lab[ i ] ? 0.0f : std::min( f[ i ], Tf - 1e-4f ) );
		};
		// crossing points on cell edges: id = horizontal edge (x,y)-(x+1,y): 2*(y*W+x); vertical (x,y)-(x,y+1): +1
		std::unordered_map< int, std::pair< float, float > > pt;
		std::unordered_map< int, int > next;
		auto cross = [ & ]( int xa, int ya, int xb, int yb ) -> int {
			const float fa = F( xa, ya ), fb = F( xb, yb ), t = ( Tf - fa ) / ( fb - fa );
			const int id   = ya == yb ? 2 * ( ya * W + xa ) : 2 * ( ya * W + xa ) + 1;
			// trace pixel centre (x, y) -> outline space: image px, y up
			pt[ id ] = { ( float( xa ) + t * float( xb - xa ) - 0.5f ) * s, float( h ) - ( float( ya ) + t * float( yb - ya ) - 0.5f ) * s };
			return id;
		};
		for( int y = y0; y < y1; ++y )
			for( int x = x0; x < x1; ++x )
			{
				const bool i0 = F( x, y ) > Tf, i1 = F( x + 1, y ) > Tf, i2 = F( x + 1, y + 1 ) > Tf, i3 = F( x, y + 1 ) > Tf;
				const int code = i0 | ( i1 << 1 ) | ( i2 << 2 ) | ( i3 << 3 );
				if( code == 0 || code == 15 )
					continue;
				// the four edges: top (0-1), right (1-2), bottom (3-2), left (0-3); a segment joins two crossed edges
				int e[ 4 ] = { -1, -1, -1, -1 };
				if( i0 != i1 )
					e[ 0 ] = cross( x, y, x + 1, y );
				if( i1 != i2 )
					e[ 1 ] = cross( x + 1, y, x + 1, y + 1 );
				if( i3 != i2 )
					e[ 2 ] = cross( x, y + 1, x + 1, y + 1 );
				if( i0 != i3 )
					e[ 3 ] = cross( x, y, x, y + 1 );
				// each segment with a corner it separates: that corner's side fixes the direction (inside on the left)
				struct Seg
				{
					int a, b, corner;
				};
				std::vector< Seg > segs;
				if( code == 5 || code == 10 ) // saddle: the cell centre decides which corners join
				{
					const bool mid = ( F( x, y ) + F( x + 1, y ) + F( x + 1, y + 1 ) + F( x, y + 1 ) ) * 0.25f > Tf;
					if( ( code == 5 ) == mid ) // 0 and 2 joined: the segments cut off corners 1 and 3
						segs = { { e[ 0 ], e[ 1 ], 1 }, { e[ 2 ], e[ 3 ], 3 } };
					else // cut off corners 0 and 2
						segs = { { e[ 0 ], e[ 3 ], 0 }, { e[ 1 ], e[ 2 ], 2 } };
				}
				else
				{
					int k[ 2 ], n = 0;
					for( int q = 0; q < 4; ++q )
						if( e[ q ] >= 0 && n < 2 )
							k[ n++ ] = e[ q ];
					if( n == 2 )
						segs = { { k[ 0 ], k[ 1 ], 0 } };
				}
				const int cxs[ 4 ] = { x, x + 1, x + 1, x }, cys[ 4 ] = { y, y, y + 1, y + 1 };
				const bool cin[ 4 ] = { i0, i1, i2, i3 };
				for( const Seg& sg : segs )
				{
					const auto pa = pt[ sg.a ], pb = pt[ sg.b ];
					const float qx = ( float( cxs[ sg.corner ] ) - 0.5f ) * s, qy = float( h ) - ( float( cys[ sg.corner ] ) - 0.5f ) * s;
					const float side = ( pb.first - pa.first ) * ( qy - pa.second ) - ( pb.second - pa.second ) * ( qx - pa.first ); // > 0: left
					if( ( side > 0.0f ) == cin[ sg.corner ] )
						next[ sg.a ] = sg.b;
					else
						next[ sg.b ] = sg.a;
				}
			}
		Shape sh;
		sh.fill = Clipper2Lib::FillRule::EvenOdd;
		while( !next.empty() )
		{
			const int start = next.begin()->first;
			std::vector< float > loop;
			for( int e = start, guard = 0; guard < ( 1 << 22 ); ++guard )
			{
				auto it = next.find( e );
				if( it == next.end() )
					break;
				loop.push_back( pt[ e ].first ), loop.push_back( pt[ e ].second );
				e = it->second;
				next.erase( it );
				if( e == start )
					break;
			}
			if( loop.size() < 8 )
				continue;
			std::vector< float > simp;
			Simplify( loop, eps * s, simp );
			Chaikin( simp, iters );
			if( simp.size() >= 6 )
				sh.contours.push_back( { simp } );
		}
		if( !sh.contours.empty() )
			o.shapes.push_back( std::move( sh ) );
	}
	o.unit     = float( h );
	o.image    = img;
	o.imageBox[ 0 ] = 0, o.imageBox[ 1 ] = 0, o.imageBox[ 2 ] = float( w ), o.imageBox[ 3 ] = float( h );
	if( o.shapes.empty() )
	{
		error = status = name + ": nothing traced (Trace Threshold / Trace Invert)";
		return false;
	}
	status = "Loaded: " + name + " (traced, " + std::to_string( o.shapes.size() ) + " shapes, " + ( alpha ? "alpha" : "luma" ) + ( invert ? ", inverted" : "" ) + ")";
	return true;
}

// ---------------------------------------------------------------- extrusion
struct V2
{
	float x, y;
};
// W2: Clipper2 (Boost) resolves a fill rule into clean contours (a PolyTree: outers and their holes), earcut (ISC) triangulates
// each outer with its holes (licences on the list: Boost, ISC).
using Clipper2Lib::FillRule;
// decimal places Clipper2 keeps (it works on int64): ~9 significant digits of the largest coordinate
int ClipPrecision( const std::vector< Contour >& in )
{
	double m = 1;
	for( const Contour& c : in )
		for( float v : c.xy )
			m = std::max( m, double( std::fabs( v ) ) );
	return std::clamp( 9 - int( std::ceil( std::log10( m ) ) ), 2, 8 );
}
Clipper2Lib::PathsD ToPaths( const std::vector< Contour >& in )
{
	Clipper2Lib::PathsD paths;
	for( const Contour& c : in )
		if( c.xy.size() >= 6 )
		{
			Clipper2Lib::PathD p;
			for( size_t i = 0; i + 1 < c.xy.size(); i += 2 )
				p.push_back( Clipper2Lib::PointD( c.xy[ i ], c.xy[ i + 1 ] ) );
			paths.push_back( std::move( p ) );
		}
	return paths;
}
void Resolve( const std::vector< Contour >& in, FillRule fill, Clipper2Lib::PolyTreeD& tree )
{
	Clipper2Lib::BooleanOp( Clipper2Lib::ClipType::Union, fill, ToPaths( in ), Clipper2Lib::PathsD(), tree, ClipPrecision( in ) );
}
// region on the left: outers counter-clockwise, holes clockwise (y up)
Contour ToContour( const Clipper2Lib::PathD& p, bool hole )
{
	Contour c;
	for( const Clipper2Lib::PointD& q : p )
		c.xy.push_back( float( q.x ) ), c.xy.push_back( float( q.y ) );
	if( ( Clipper2Lib::Area( p ) < 0 ) != hole )
		for( size_t i = 0, n = c.xy.size() / 2; i < n / 2; ++i )
			std::swap( c.xy[ i * 2 ], c.xy[ ( n - 1 - i ) * 2 ] ), std::swap( c.xy[ i * 2 + 1 ], c.xy[ ( n - 1 - i ) * 2 + 1 ] );
	return c;
}
void Collect( const Clipper2Lib::PolyPathD& node, std::vector< Contour >& out )
{
	for( size_t i = 0; i < node.Count(); ++i )
	{
		const Clipper2Lib::PolyPathD& ch = *node.Child( i );
		Contour c = ToContour( ch.Polygon(), ch.IsHole() );
		if( c.xy.size() >= 6 )
			out.push_back( std::move( c ) );
		Collect( ch, out );
	}
}
// the clean boundary of a shape under its fill rule
std::vector< Contour > Boundary( const std::vector< Contour >& in, FillRule fill )
{
	Clipper2Lib::PolyTreeD tree;
	Resolve( in, fill, tree );
	std::vector< Contour > out;
	Collect( tree, out );
	return out;
}
// triangles (x, y per corner, counter-clockwise) filling a shape under its fill rule: each outer with its holes
void EarcutNode( const Clipper2Lib::PolyPathD& node, std::vector< float >& tris )
{
	for( size_t i = 0; i < node.Count(); ++i )
	{
		const Clipper2Lib::PolyPathD& outer = *node.Child( i );
		std::vector< std::vector< std::array< double, 2 > > > poly( 1 );
		for( const Clipper2Lib::PointD& q : outer.Polygon() )
			poly[ 0 ].push_back( { q.x, q.y } );
		for( size_t h = 0; h < outer.Count(); ++h )
		{
			poly.emplace_back();
			for( const Clipper2Lib::PointD& q : outer.Child( h )->Polygon() )
				poly.back().push_back( { q.x, q.y } );
		}
		std::vector< std::array< double, 2 > > flat;
		for( const auto& r : poly )
			flat.insert( flat.end(), r.begin(), r.end() );
		for( uint32_t k : mapbox::earcut< uint32_t >( poly ) )
			tris.push_back( float( flat[ k ][ 0 ] ) ), tris.push_back( float( flat[ k ][ 1 ] ) );
		for( size_t h = 0; h < outer.Count(); ++h ) // islands inside the holes
			EarcutNode( *outer.Child( h ), tris );
	}
}
void Triangulate( const std::vector< Contour >& in, FillRule fill, std::vector< float >& tris )
{
	Clipper2Lib::PolyTreeD tree;
	Resolve( in, fill, tree );
	EarcutNode( tree, tris );
}

// points in a straight run (curve flattening of a line, nanosvg's line-as-cubic) fold the bevel's inset at sharp tips
void DropCollinear( Contour& c )
{
	const int n = int( c.xy.size() / 2 );
	if( n < 4 )
		return;
	auto straight = []( const float* a, const float* p, const float* b ) {
		const float e1x = p[ 0 ] - a[ 0 ], e1y = p[ 1 ] - a[ 1 ], e2x = b[ 0 ] - p[ 0 ], e2y = b[ 1 ] - p[ 1 ];
		const float l1 = std::hypot( e1x, e1y ), l2 = std::hypot( e2x, e2y );
		return l1 < 1e-7f || l2 < 1e-7f || ( std::fabs( e1x * e2y - e1y * e2x ) < 1e-4f * l1 * l2 && e1x * e2x + e1y * e2y > 0.0f );
	};
	std::vector< float > o;
	for( int i = 0; i < n; ++i )
	{
		const float* a = o.empty() ? &c.xy[ ( n - 1 ) * 2 ] : &o[ o.size() - 2 ];
		if( !straight( a, &c.xy[ i * 2 ], &c.xy[ ( ( i + 1 ) % n ) * 2 ] ) )
			o.push_back( c.xy[ i * 2 ] ), o.push_back( c.xy[ i * 2 + 1 ] );
	}
	if( o.size() >= 8 && straight( &o[ o.size() - 2 ], &o[ 0 ], &o[ 2 ] ) )
		o.erase( o.begin(), o.begin() + 2 );
	if( o.size() >= 6 )
		c.xy.swap( o );
}

// offset a region-on-the-left contour inward by d (miter, capped)
Contour Inset( const Contour& c, float d )
{
	const int n = int( c.xy.size() / 2 );
	Contour o;
	o.xy.resize( c.xy.size() );
	for( int i = 0; i < n; ++i )
	{
		const float* p = &c.xy[ i * 2 ];
		const float* a = &c.xy[ ( ( i + n - 1 ) % n ) * 2 ];
		const float* b = &c.xy[ ( ( i + 1 ) % n ) * 2 ];
		float e1x = p[ 0 ] - a[ 0 ], e1y = p[ 1 ] - a[ 1 ], e2x = b[ 0 ] - p[ 0 ], e2y = b[ 1 ] - p[ 1 ];
		const float l1 = std::hypot( e1x, e1y ) + 1e-12f, l2 = std::hypot( e2x, e2y ) + 1e-12f;
		const float n1x = -e1y / l1, n1y = e1x / l1, n2x = -e2y / l2, n2y = e2x / l2; // left normals = interior
		float mx = n1x + n2x, my = n1y + n2y;
		const float ml = std::hypot( mx, my );
		if( ml < 1e-6f )
			mx = n1x, my = n1y;
		else
			mx /= ml, my /= ml;
		const float cosA = std::max( 0.25f, mx * n1x + my * n1y ); // miter limit 4
		o.xy[ i * 2 ]     = p[ 0 ] + mx * d / cosA;
		o.xy[ i * 2 + 1 ] = p[ 1 ] + my * d / cosA;
	}
	return o;
}

struct Builder
{
	GenMesh* m;
	uint32_t group = 0;
	float uvFixed[ 2 ] = { -1, -1 }; // palette mode: every corner of this shape samples its colour texel
	const Outline* o;
	float bx0, by0, bw, bh;
	void Tri( int part, const GenCorner& a, const GenCorner& b, const GenCorner& c )
	{
		for( const GenCorner* k : { &a, &b, &c } )
		{
			GenCorner q = *k;
			if( uvFixed[ 0 ] >= 0 )
				q.uv[ 0 ] = uvFixed[ 0 ], q.uv[ 1 ] = uvFixed[ 1 ];
			m->corners.push_back( q );
		}
		m->triPart.push_back( part ), m->triGroup.push_back( group );
	}
	GenCorner C( float x, float y, float z, float nx, float ny, float nz, float u, float v ) const
	{
		GenCorner c;
		c.p[ 0 ] = x, c.p[ 1 ] = y, c.p[ 2 ] = z, c.n[ 0 ] = nx, c.n[ 1 ] = ny, c.n[ 2 ] = nz, c.uv[ 0 ] = u, c.uv[ 1 ] = v;
		return c;
	}
	// planar: the image / the outline box, rows top-first
	void Planar( float x, float y, float& u, float& v ) const
	{
		if( o->image )
			u = ( x - o->imageBox[ 0 ] ) / ( o->imageBox[ 2 ] - o->imageBox[ 0 ] ), v = ( o->imageBox[ 3 ] - y ) / ( o->imageBox[ 3 ] - o->imageBox[ 1 ] );
		else
			u = ( x - bx0 ) / bw, v = 1.0f - ( y - by0 ) / bh;
	}
};

// ---------------------------------------------------------------- W2: text / logos shatter into solid pieces
// Each letter (logo shape) is cut by a 2D Voronoi diagram of seeds inside its outline (Shard Count shared over the letters by
// front area, at least one per letter). Every triangle of the FINISHED letter (caps, bevel, side wall) is clipped by each convex
// cell (Sutherland-Hodgman in the triangle's barycentric space, attributes interpolated), so a piece's outer faces ARE the letter's
// surface: the pieces at home are the whole letter exactly (no pop when the fractured copy takes over). Each cell edge that runs
// through the solid becomes a flat cut face ("Core"): the section between the top surface (front cap / bevel) and the bottom one
// (back cap / bevel, or the back plane without a back cap), read off the clipped triangles' edges on that line, so every piece is
// a closed solid. Parts out: Front, Back, Side, Bevel (as clipped), Core.
struct FracSeg
{
	float s0, z0, s1, z1;
};
struct FracEdge
{
	float n[ 2 ], d; // the cell is n.p <= d
	std::vector< FracSeg > top, bot;
};
struct FracCell
{
	std::vector< FracEdge > edges; // the bisectors that bound it (not the letter's box)
	float lo[ 2 ], hi[ 2 ];        // its box (inside the letter's box)
};

std::vector< FracCell > VoronoiCells( const std::vector< std::array< float, 2 > >& seeds, const float* lo, const float* hi )
{
	std::vector< FracCell > cells( seeds.size() );
	for( size_t i = 0; i < seeds.size(); ++i )
	{
		// the box, then every bisector half-plane: closer to seed i than seed j
		std::vector< std::array< float, 2 > > poly = { { lo[ 0 ], lo[ 1 ] }, { hi[ 0 ], lo[ 1 ] }, { hi[ 0 ], hi[ 1 ] }, { lo[ 0 ], hi[ 1 ] } };
		std::vector< int > from( 4, -1 ); // which half-plane made the edge starting at each vertex (-1 = the box)
		std::vector< FracEdge > planes;
		for( size_t j = 0; j < seeds.size() && poly.size() >= 3; ++j )
		{
			if( j == i )
				continue;
			const float nx = seeds[ j ][ 0 ] - seeds[ i ][ 0 ], ny = seeds[ j ][ 1 ] - seeds[ i ][ 1 ];
			const float l  = std::hypot( nx, ny );
			if( l < 1e-9f )
				continue;
			FracEdge e;
			e.n[ 0 ] = nx / l, e.n[ 1 ] = ny / l;
			e.d      = ( e.n[ 0 ] * ( seeds[ i ][ 0 ] + seeds[ j ][ 0 ] ) + e.n[ 1 ] * ( seeds[ i ][ 1 ] + seeds[ j ][ 1 ] ) ) * 0.5f;
			std::vector< std::array< float, 2 > > out;
			std::vector< int > of;
			const size_t n = poly.size();
			bool cut       = false;
			for( size_t k = 0; k < n; ++k )
			{
				const auto& a = poly[ k ];
				const auto& b = poly[ ( k + 1 ) % n ];
				const float da = e.n[ 0 ] * a[ 0 ] + e.n[ 1 ] * a[ 1 ] - e.d, db = e.n[ 0 ] * b[ 0 ] + e.n[ 1 ] * b[ 1 ] - e.d;
				if( da <= 0 )
					out.push_back( a ), of.push_back( from[ k ] );
				if( ( da <= 0 ) != ( db <= 0 ) )
				{
					const float t = da / ( da - db );
					out.push_back( { a[ 0 ] + t * ( b[ 0 ] - a[ 0 ] ), a[ 1 ] + t * ( b[ 1 ] - a[ 1 ] ) } );
					of.push_back( da <= 0 ? int( planes.size() ) : from[ k ] ); // leaving: the new edge lies on this plane
					cut = true;
				}
			}
			poly.swap( out ), from.swap( of );
			if( cut )
				planes.push_back( e );
		}
		FracCell& c = cells[ i ];
		c.lo[ 0 ] = c.lo[ 1 ] = 1e30f, c.hi[ 0 ] = c.hi[ 1 ] = -1e30f;
		for( const auto& p : poly )
			for( int k = 0; k < 2; ++k )
				c.lo[ k ] = std::min( c.lo[ k ], p[ k ] ), c.hi[ k ] = std::max( c.hi[ k ], p[ k ] );
		// only the planes that still bound the final cell
		std::vector< char > used( planes.size(), 0 );
		for( int f : from )
			if( f >= 0 )
				used[ f ] = 1;
		for( size_t p = 0; p < planes.size(); ++p )
			if( used[ p ] )
				c.edges.push_back( planes[ p ] );
	}
	return cells;
}

void Fracture( const GenMesh& whole, int count, float zBack, bool backCap, const std::vector< std::array< float, 2 > >& groupUV, const Builder& B, float sc,
               GenMesh& out, std::vector< uint32_t >& triPiece, int& pieceCount )
{
	const size_t tris = whole.corners.size() / 3;
	const int G       = whole.groupCount;
	std::vector< std::vector< uint32_t > > byGroup( size_t( std::max( G, 0 ) ) );
	std::vector< double > area( size_t( std::max( G, 0 ) ), 0.0 );
	double total = 0;
	auto xyArea = [ & ]( size_t t ) {
		const GenCorner* c = &whole.corners[ t * 3 ];
		return 0.5 * ( double( c[ 1 ].p[ 0 ] - c[ 0 ].p[ 0 ] ) * ( c[ 2 ].p[ 1 ] - c[ 0 ].p[ 1 ] ) - double( c[ 2 ].p[ 0 ] - c[ 0 ].p[ 0 ] ) * ( c[ 1 ].p[ 1 ] - c[ 0 ].p[ 1 ] ) );
	};
	for( size_t t = 0; t < tris; ++t )
	{
		const uint32_t g = whole.triGroup[ t ];
		byGroup[ g ].push_back( uint32_t( t ) );
		if( whole.triPart[ t ] == 0 ) // front cap: the letter's face
			area[ g ] += std::max( 0.0, xyArea( t ) ), total += std::max( 0.0, xyArea( t ) );
	}
	out.parts.clear();
	pieceCount = 0;
	std::mt19937 rng( 20261008u + uint32_t( count ) );
	std::uniform_real_distribution< double > U01( 0.0, 1.0 );
	for( int g = 0; g < G; ++g )
	{
		const std::vector< uint32_t >& T = byGroup[ g ];
		if( T.empty() )
			continue;
		float lo[ 2 ] = { 1e30f, 1e30f }, hi[ 2 ] = { -1e30f, -1e30f };
		for( uint32_t t : T )
			for( int k = 0; k < 3; ++k )
				for( int a = 0; a < 2; ++a )
					lo[ a ] = std::min( lo[ a ], whole.corners[ t * 3 + k ].p[ a ] ), hi[ a ] = std::max( hi[ a ], whole.corners[ t * 3 + k ].p[ a ] );
		const float size = std::max( hi[ 0 ] - lo[ 0 ], hi[ 1 ] - lo[ 1 ] );
		for( int a = 0; a < 2; ++a )
			lo[ a ] -= 0.01f * size + 1e-4f, hi[ a ] += 0.01f * size + 1e-4f;
		// seeds: area-weighted points on the front cap, spread out (best of 12 candidates: the farthest from the seeds so far)
		const int k = total > 0 ? std::max( 1, int( std::lround( count * area[ g ] / total ) ) ) : 1;
		std::vector< double > cum;
		std::vector< uint32_t > front;
		double acc = 0;
		for( uint32_t t : T )
			if( whole.triPart[ t ] == 0 && xyArea( t ) > 0 )
				front.push_back( t ), cum.push_back( acc += xyArea( t ) );
		std::vector< std::array< float, 2 > > seeds;
		for( int s = 0; s < k && !front.empty(); ++s )
		{
			std::array< float, 2 > best{};
			float bestD = -1;
			for( int c = 0; c < ( s == 0 ? 1 : 12 ); ++c )
			{
				const size_t i = std::min( front.size() - 1, size_t( std::lower_bound( cum.begin(), cum.end(), U01( rng ) * acc ) - cum.begin() ) );
				const GenCorner* q = &whole.corners[ front[ i ] * 3 ];
				double r1 = std::sqrt( U01( rng ) ), r2 = U01( rng );
				const float wa = float( 1 - r1 ), wb = float( r1 * ( 1 - r2 ) ), wc = float( r1 * r2 );
				const std::array< float, 2 > p = { wa * q[ 0 ].p[ 0 ] + wb * q[ 1 ].p[ 0 ] + wc * q[ 2 ].p[ 0 ], wa * q[ 0 ].p[ 1 ] + wb * q[ 1 ].p[ 1 ] + wc * q[ 2 ].p[ 1 ] };
				float dmin = 1e30f;
				for( const auto& o : seeds )
					dmin = std::min( dmin, std::hypot( p[ 0 ] - o[ 0 ], p[ 1 ] - o[ 1 ] ) );
				if( dmin > bestD )
					bestD = dmin, best = p;
			}
			seeds.push_back( best );
		}
		if( seeds.empty() )
			seeds.push_back( { 0.5f * ( lo[ 0 ] + hi[ 0 ] ), 0.5f * ( lo[ 1 ] + hi[ 1 ] ) } );
		std::vector< FracCell > cells = VoronoiCells( seeds, lo, hi );
		const float tol = 1e-5f * std::max( size, 1e-3f );
		for( size_t ci = 0; ci < cells.size(); ++ci )
		{
			FracCell& cell = cells[ ci ];
			const uint32_t piece = uint32_t( pieceCount++ );
			for( uint32_t t : T )
			{
				const GenCorner* q = &whole.corners[ t * 3 ];
				float tlo[ 2 ] = { 1e30f, 1e30f }, thi[ 2 ] = { -1e30f, -1e30f };
				for( int c = 0; c < 3; ++c )
					for( int a = 0; a < 2; ++a )
						tlo[ a ] = std::min( tlo[ a ], q[ c ].p[ a ] ), thi[ a ] = std::max( thi[ a ], q[ c ].p[ a ] );
				if( tlo[ 0 ] > cell.hi[ 0 ] + tol || thi[ 0 ] < cell.lo[ 0 ] - tol || tlo[ 1 ] > cell.hi[ 1 ] + tol || thi[ 1 ] < cell.lo[ 1 ] - tol )
					continue;
				// the polygon in barycentric coordinates of the triangle
				std::vector< std::array< double, 3 > > poly = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
				auto XY = [ & ]( const std::array< double, 3 >& b, int a ) { return b[ 0 ] * q[ 0 ].p[ a ] + b[ 1 ] * q[ 1 ].p[ a ] + b[ 2 ] * q[ 2 ].p[ a ]; };
				for( const FracEdge& e : cell.edges )
				{
					std::vector< std::array< double, 3 > > o;
					const size_t n = poly.size();
					for( size_t v = 0; v < n && n >= 3; ++v )
					{
						const auto& a = poly[ v ];
						const auto& b = poly[ ( v + 1 ) % n ];
						const double da = e.n[ 0 ] * XY( a, 0 ) + e.n[ 1 ] * XY( a, 1 ) - e.d, db = e.n[ 0 ] * XY( b, 0 ) + e.n[ 1 ] * XY( b, 1 ) - e.d;
						if( da <= 0 )
							o.push_back( a );
						if( ( da <= 0 ) != ( db <= 0 ) )
						{
							const double u = da / ( da - db );
							o.push_back( { a[ 0 ] + u * ( b[ 0 ] - a[ 0 ] ), a[ 1 ] + u * ( b[ 1 ] - a[ 1 ] ), a[ 2 ] + u * ( b[ 2 ] - a[ 2 ] ) } );
						}
					}
					poly.swap( o );
					if( poly.size() < 3 )
						break;
				}
				if( poly.size() < 3 )
					continue;
				auto corner = [ & ]( const std::array< double, 3 >& b ) {
					GenCorner c;
					float nl = 0;
					for( int a = 0; a < 3; ++a )
					{
						c.p[ a ] = float( b[ 0 ] * q[ 0 ].p[ a ] + b[ 1 ] * q[ 1 ].p[ a ] + b[ 2 ] * q[ 2 ].p[ a ] );
						c.n[ a ] = float( b[ 0 ] * q[ 0 ].n[ a ] + b[ 1 ] * q[ 1 ].n[ a ] + b[ 2 ] * q[ 2 ].n[ a ] );
						nl += c.n[ a ] * c.n[ a ];
					}
					nl = 1.0f / std::max( std::sqrt( nl ), 1e-12f );
					for( float& v : c.n )
						v *= nl;
					for( int a = 0; a < 2; ++a )
						c.uv[ a ] = float( b[ 0 ] * q[ 0 ].uv[ a ] + b[ 1 ] * q[ 1 ].uv[ a ] + b[ 2 ] * q[ 2 ].uv[ a ] );
					return c;
				};
				std::vector< GenCorner > pc;
				for( const auto& b : poly )
					pc.push_back( corner( b ) );
				// the piece's share of this triangle: a fan (the clipped polygon is convex)
				for( size_t v = 1; v + 1 < pc.size(); ++v )
				{
					const float* a = pc[ 0 ].p;
					const float* b = pc[ v ].p;
					const float* c = pc[ v + 1 ].p;
					const float e1[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, e2[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] };
					const float cx = e1[ 1 ] * e2[ 2 ] - e1[ 2 ] * e2[ 1 ], cy = e1[ 2 ] * e2[ 0 ] - e1[ 0 ] * e2[ 2 ], cz = e1[ 0 ] * e2[ 1 ] - e1[ 1 ] * e2[ 0 ];
					if( cx * cx + cy * cy + cz * cz < 1e-24f * size * size * size * size )
						continue;
					out.corners.push_back( pc[ 0 ] ), out.corners.push_back( pc[ v ] ), out.corners.push_back( pc[ v + 1 ] );
					out.triPart.push_back( whole.triPart[ t ] ), out.triGroup.push_back( uint32_t( g ) ), triPiece.push_back( piece );
				}
				// the clipped edges that lie on a cell edge: the cut section's top (front-facing) or bottom (back-facing) outline
				const float* a = q[ 0 ].p;
				const float fz = ( q[ 1 ].p[ 0 ] - a[ 0 ] ) * ( q[ 2 ].p[ 1 ] - a[ 1 ] ) - ( q[ 2 ].p[ 0 ] - a[ 0 ] ) * ( q[ 1 ].p[ 1 ] - a[ 1 ] ); // 2 x xy area
				const float e1[ 3 ] = { q[ 1 ].p[ 0 ] - a[ 0 ], q[ 1 ].p[ 1 ] - a[ 1 ], q[ 1 ].p[ 2 ] - a[ 2 ] }, e2[ 3 ] = { q[ 2 ].p[ 0 ] - a[ 0 ], q[ 2 ].p[ 1 ] - a[ 1 ], q[ 2 ].p[ 2 ] - a[ 2 ] };
				const float fl = std::sqrt( ( e1[ 1 ] * e2[ 2 ] - e1[ 2 ] * e2[ 1 ] ) * ( e1[ 1 ] * e2[ 2 ] - e1[ 2 ] * e2[ 1 ] ) + ( e1[ 2 ] * e2[ 0 ] - e1[ 0 ] * e2[ 2 ] ) * ( e1[ 2 ] * e2[ 0 ] - e1[ 0 ] * e2[ 2 ] ) + fz * fz );
				if( std::fabs( fz ) < 0.02f * fl ) // a side-wall triangle: its edges on the cut are vertical
					continue;
				for( FracEdge& e : cell.edges )
				{
					const float tx = -e.n[ 1 ], ty = e.n[ 0 ];
					for( size_t v = 0; v < pc.size(); ++v )
					{
						const GenCorner& u = pc[ v ];
						const GenCorner& w = pc[ ( v + 1 ) % pc.size() ];
						if( std::fabs( e.n[ 0 ] * u.p[ 0 ] + e.n[ 1 ] * u.p[ 1 ] - e.d ) > tol || std::fabs( e.n[ 0 ] * w.p[ 0 ] + e.n[ 1 ] * w.p[ 1 ] - e.d ) > tol )
							continue;
						FracSeg sg{ tx * u.p[ 0 ] + ty * u.p[ 1 ], u.p[ 2 ], tx * w.p[ 0 ] + ty * w.p[ 1 ], w.p[ 2 ] };
						if( sg.s1 < sg.s0 )
							std::swap( sg.s0, sg.s1 ), std::swap( sg.z0, sg.z1 );
						if( sg.s1 - sg.s0 > tol * 0.1f )
							( fz > 0 ? e.top : e.bot ).push_back( sg );
					}
				}
			}
			// the cut faces: between the top and bottom outlines of the section along each cell edge (outward = the edge's normal)
			for( const FracEdge& e : cell.edges )
			{
				if( e.top.empty() )
					continue;
				std::vector< float > S;
				for( const auto* L : { &e.top, &e.bot } )
					for( const FracSeg& s : *L )
						S.push_back( s.s0 ), S.push_back( s.s1 );
				std::sort( S.begin(), S.end() );
				auto at = [ & ]( const std::vector< FracSeg >& L, float m, float s, float& z ) {
					for( const FracSeg& g2 : L )
						if( m >= g2.s0 - tol && m <= g2.s1 + tol )
						{
							const float u = std::clamp( ( s - g2.s0 ) / std::max( g2.s1 - g2.s0, 1e-12f ), 0.0f, 1.0f );
							z = g2.z0 + u * ( g2.z1 - g2.z0 );
							return true;
						}
					return false;
				};
				const float tx = -e.n[ 1 ], ty = e.n[ 0 ];
				for( size_t i = 0; i + 1 < S.size(); ++i )
				{
					const float sa = S[ i ], sb = S[ i + 1 ];
					if( sb - sa < tol * 0.5f )
						continue;
					const float m = 0.5f * ( sa + sb );
					float ta, tb, ba, bb;
					if( !at( e.top, m, sa, ta ) || !at( e.top, m, sb, tb ) )
						continue;
					if( !at( e.bot, m, sa, ba ) || !at( e.bot, m, sb, bb ) )
					{
						if( backCap )
							continue;
						ba = bb = zBack; // no back cap: the section runs down to the back plane
					}
					auto C = [ & ]( float s, float z ) {
						GenCorner c;
						c.p[ 0 ] = e.n[ 0 ] * e.d + tx * s, c.p[ 1 ] = e.n[ 1 ] * e.d + ty * s, c.p[ 2 ] = z;
						c.n[ 0 ] = e.n[ 0 ], c.n[ 1 ] = e.n[ 1 ], c.n[ 2 ] = 0;
						if( groupUV[ g ][ 0 ] >= 0 )
							c.uv[ 0 ] = groupUV[ g ][ 0 ], c.uv[ 1 ] = groupUV[ g ][ 1 ];
						else
							B.Planar( c.p[ 0 ] / sc, c.p[ 1 ] / sc, c.uv[ 0 ], c.uv[ 1 ] );
						return c;
					};
					const GenCorner A = C( sa, ta ), Bt = C( sb, tb ), Cb = C( sb, bb ), D = C( sa, ba );
					// the quad A (sa, top) Bt (sb, top) Cb (sb, bottom) D (sa, bottom), each triangle turned to face +n
					const GenCorner* t1[ 3 ] = { &A, &D, &Cb };
					const GenCorner* t2[ 3 ] = { &A, &Cb, &Bt };
					for( const GenCorner* const* tt : { t1, t2 } )
					{
						const float* p0 = tt[ 0 ]->p;
						const float* p1 = tt[ 1 ]->p;
						const float* p2 = tt[ 2 ]->p;
						const float u1[ 3 ] = { p1[ 0 ] - p0[ 0 ], p1[ 1 ] - p0[ 1 ], p1[ 2 ] - p0[ 2 ] }, u2[ 3 ] = { p2[ 0 ] - p0[ 0 ], p2[ 1 ] - p0[ 1 ], p2[ 2 ] - p0[ 2 ] };
						const float nx = u1[ 1 ] * u2[ 2 ] - u1[ 2 ] * u2[ 1 ], ny = u1[ 2 ] * u2[ 0 ] - u1[ 0 ] * u2[ 2 ], nz = u1[ 0 ] * u2[ 1 ] - u1[ 1 ] * u2[ 0 ];
						if( nx * nx + ny * ny + nz * nz < 1e-24f * size * size * size * size )
							continue;
						const bool flip = nx * e.n[ 0 ] + ny * e.n[ 1 ] < 0;
						out.corners.push_back( *tt[ 0 ] ), out.corners.push_back( flip ? *tt[ 2 ] : *tt[ 1 ] ), out.corners.push_back( flip ? *tt[ 1 ] : *tt[ 2 ] );
						out.triPart.push_back( 4 ), out.triGroup.push_back( uint32_t( g ) ), triPiece.push_back( piece );
					}
				}
			}
		}
	}
	out.groupCount = whole.groupCount;
}

void Extrude( const Outline& o, const GenParams& g, GenMesh& m, std::string& status, GenMesh* frac, std::vector< uint32_t >* triPiece, int* pieceCount )
{
	// outline space -> model units: 1 = Outline::unit
	const float sc     = 1.0f / std::max( o.unit, 1e-6f );
	const float depth  = g.F( "depth", 0.25f );
	int shapeK         = (int)g.F( "bevelshape", 1 );
	float bevel        = std::min( g.F( "bevel", 0.02f ), depth * 0.45f );
	if( shapeK == 0 )
		bevel = 0;
	const int segs     = shapeK == 2 ? 1 : std::clamp( (int)std::lround( g.F( "segments", 3 ) ), 1, 8 );
	const bool backCap = g.F( "backcap", 1 ) > 0.5f;
	const bool outline = g.F( "outlineonly", 0 ) > 0.5f;
	const float wall   = g.F( "wall", 0.04f );
	const bool palette = o.fileColours && g.F( "colours", 0 ) < 0.5f;
	// bevel profile from the cap (k = 0) to the side (k = segs): inward offset o_k, drop dz_k (model units)
	std::vector< V2 > prof;
	if( bevel <= 0 )
		prof = { { 0, 0 } };
	else if( shapeK == 3 ) // inset: the face sits back, a lip around it
		prof = { { bevel, bevel }, { bevel, 0 }, { 0, 0 } };
	else
		for( int k = 0; k <= segs; ++k )
		{
			const float a = float( k ) / segs;
			if( shapeK == 2 )
				prof.push_back( { bevel * ( 1 - a ), bevel * a } );
			else
				prof.push_back( { bevel * ( 1 - std::sin( a * 1.5707963f ) ), bevel * ( 1 - std::cos( a * 1.5707963f ) ) } );
		}
	// bounding box (outline space) for planar uvs
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for( const Shape& s : o.shapes )
		for( const Contour& c : s.contours )
			for( size_t i = 0; i < c.xy.size(); i += 2 )
				x0 = std::min( x0, c.xy[ i ] ), x1 = std::max( x1, c.xy[ i ] ), y0 = std::min( y0, c.xy[ i + 1 ] ), y1 = std::max( y1, c.xy[ i + 1 ] );
	Builder B;
	B.m = &m, B.o = &o, B.bx0 = x0, B.by0 = y0, B.bw = std::max( x1 - x0, 1e-6f ), B.bh = std::max( y1 - y0, 1e-6f );
	const float zf = depth * 0.5f, zb = -depth * 0.5f;
	// palette texture: 4x4 px per shape colour (sampled at block centres, stays exact through two mip levels)
	const int nShapes = int( o.shapes.size() );
	const int cols = std::min( nShapes, 256 ), rows = ( nShapes + cols - 1 ) / std::max( cols, 1 );
	std::shared_ptr< Image > pal;
	if( palette )
	{
		pal = std::make_shared< Image >();
		pal->width = cols * 4, pal->height = rows * 4, pal->components = 4, pal->srgb = true;
		pal->pixels.assign( size_t( pal->width ) * pal->height * 4, 255 );
		for( int s = 0; s < nShapes; ++s )
			for( int y = 0; y < 4; ++y )
				for( int x = 0; x < 4; ++x )
				{
					uint8_t* p = &pal->pixels[ ( size_t( ( s / cols ) * 4 + y ) * pal->width + ( s % cols ) * 4 + x ) * 4 ];
					for( int k = 0; k < 3; ++k )
						p[ k ] = uint8_t( std::lround( std::clamp( o.shapes[ s ].color[ k ], 0.0f, 1.0f ) * 255 ) );
				}
		BuildImageMips( *pal );
	}
	int groups = 0, tris0 = 0;
	std::vector< std::array< float, 2 > > groupUV; // W2: per group, its palette texel (or -1: planar), for the cut faces
	for( int si = 0; si < nShapes; ++si )
	{
		const Shape& sh = o.shapes[ si ];
		// clean boundary (the source's fill rule), region on the left; Outline Only: the ring inside it
		std::vector< Contour > bound = Boundary( sh.contours, sh.fill );
		for( Contour& c : bound )
		{
			for( float& v : c.xy )
				v *= sc;
			DropCollinear( c );
		}
		if( outline )
		{
			std::vector< Contour > ring = bound;
			for( const Contour& c : bound )
			{
				Contour in = Inset( c, wall );
				for( size_t i = 0, n = in.xy.size() / 2; i < n / 2; ++i ) // reversed: a hole
					std::swap( in.xy[ i * 2 ], in.xy[ ( n - 1 - i ) * 2 ] ), std::swap( in.xy[ i * 2 + 1 ], in.xy[ ( n - 1 - i ) * 2 + 1 ] );
				ring.push_back( in );
			}
			bound = Boundary( ring, FillRule::Positive );
		}
		if( bound.empty() )
			continue;
		B.group = uint32_t( groups++ );
		// the bevel never gets wider than ~0.8 x the shape's mean half-width (area / perimeter): a wider inset folds
		double area = 0, perim = 0;
		for( const Contour& c : bound )
			for( size_t i = 0, n = c.xy.size() / 2; i < n; ++i )
			{
				const float* p = &c.xy[ i * 2 ];
				const float* q = &c.xy[ ( ( i + 1 ) % n ) * 2 ];
				area += 0.5 * ( double( p[ 0 ] ) * q[ 1 ] - double( q[ 0 ] ) * p[ 1 ] ), perim += std::hypot( q[ 0 ] - p[ 0 ], q[ 1 ] - p[ 1 ] );
			}
		float maxX = 0;
		for( const V2& pr : prof )
			maxX = std::max( maxX, pr.x );
		const float kX = maxX > 0 && perim > 0 ? std::min( 1.0f, float( 0.8 * std::fabs( area ) / perim ) / maxX ) : 1.0f;
		std::vector< V2 > P = prof;
		for( V2& pr : P )
			pr.x *= kX;
		if( palette )
			B.uvFixed[ 0 ] = ( ( si % cols ) * 4 + 2.0f ) / pal->width, B.uvFixed[ 1 ] = ( ( si / cols ) * 4 + 2.0f ) / pal->height;
		else
			B.uvFixed[ 0 ] = B.uvFixed[ 1 ] = -1;
		groupUV.push_back( { B.uvFixed[ 0 ], B.uvFixed[ 1 ] } );
		// caps: the boundary inset by the profile's first offset, tessellated
		std::vector< Contour > capC;
		for( const Contour& c : bound )
			capC.push_back( P[ 0 ].x > 0 ? Inset( c, P[ 0 ].x ) : c );
		std::vector< float > tv;
		Triangulate( capC, FillRule::Positive, tv );
		const float zc = zf - P[ 0 ].y, zcb = zb + P[ 0 ].y;
		for( size_t i = 0; i + 5 < tv.size(); i += 6 )
		{
			GenCorner c[ 3 ], d[ 3 ];
			for( int k = 0; k < 3; ++k )
			{
				float u, v;
				B.Planar( tv[ i + k * 2 ] / sc, tv[ i + k * 2 + 1 ] / sc, u, v );
				c[ k ] = B.C( tv[ i + k * 2 ], tv[ i + k * 2 + 1 ], zc, 0, 0, 1, u, v );
				d[ k ] = B.C( tv[ i + k * 2 ], tv[ i + k * 2 + 1 ], zcb, 0, 0, -1, u, v );
			}
			B.Tri( 0, c[ 0 ], c[ 1 ], c[ 2 ] );
			if( backCap )
				B.Tri( 1, d[ 0 ], d[ 2 ], d[ 1 ] );
		}
		// rings (bevel) and the side wall, per boundary contour
		for( const Contour& c : bound )
		{
			const int n = int( c.xy.size() / 2 );
			std::vector< float > len( n + 1, 0 );
			for( int i = 0; i < n; ++i )
				len[ i + 1 ] = len[ i ] + std::hypot( c.xy[ ( ( i + 1 ) % n ) * 2 ] - c.xy[ i * 2 ], c.xy[ ( ( i + 1 ) % n ) * 2 + 1 ] - c.xy[ i * 2 + 1 ] );
			// per vertex: smooth (miter) normal or sharp (edge normals) by the corner angle; outward = right of travel
			std::vector< V2 > en( n ), vn( n );
			std::vector< char > sharp( n );
			for( int i = 0; i < n; ++i )
			{
				const float* p = &c.xy[ i * 2 ];
				const float* q = &c.xy[ ( ( i + 1 ) % n ) * 2 ];
				const float l  = std::hypot( q[ 0 ] - p[ 0 ], q[ 1 ] - p[ 1 ] ) + 1e-12f;
				en[ i ]        = { ( q[ 1 ] - p[ 1 ] ) / l, -( q[ 0 ] - p[ 0 ] ) / l };
			}
			for( int i = 0; i < n; ++i )
			{
				const V2 a = en[ ( i + n - 1 ) % n ], b = en[ i ];
				sharp[ i ] = a.x * b.x + a.y * b.y < 0.82f; // > ~35 degrees
			}
			// smooth normals: the length-weighted mean of the edges around the vertex (4 edges when no sharp corner is
			// in reach, so traced / finely flattened outlines shade without striping)
			for( int i = 0; i < n; ++i )
			{
				const bool wide = !sharp[ ( i + n - 1 ) % n ] && !sharp[ ( i + 1 ) % n ];
				float lx = 0, ly = 0;
				for( int j = wide ? -2 : -1; j <= ( wide ? 1 : 0 ); ++j )
				{
					const int e    = ( i + j + n ) % n;
					const float le = len[ e + 1 ] - len[ e ];
					lx += en[ e ].x * le, ly += en[ e ].y * le;
				}
				const float l = std::hypot( lx, ly ) + 1e-12f;
				vn[ i ]       = { lx / l, ly / l };
			}
			auto emitStrip = [ & ]( int part, const Contour& A, float za, const Contour& Bc, float zb2, float nzA, float nzB, float hA, float hB, float vA, float vB ) {
				// quads between two rings: horizontal normal weight hA / hB, z weight nzA / nzB (per ring)
				for( int i = 0; i < n; ++i )
				{
					const int j = ( i + 1 ) % n;
					auto nrm = [ & ]( int vtx, int edge, float h, float nz, float* out ) {
						const V2 v = sharp[ vtx ] ? en[ edge ] : vn[ vtx ];
						float x = v.x * h, y = v.y * h, z = nz, l = std::sqrt( x * x + y * y + z * z ) + 1e-12f;
						out[ 0 ] = x / l, out[ 1 ] = y / l, out[ 2 ] = z / l;
					};
					float na[ 3 ], nb[ 3 ], nc[ 3 ], nd[ 3 ];
					nrm( i, i, hA, nzA, na ), nrm( j, i, hA, nzA, nb ), nrm( i, i, hB, nzB, nc ), nrm( j, i, hB, nzB, nd );
					float u0, v0, u1, v1;
					const float ua = len[ i ] * 0.5f, ub = len[ i + 1 ] * 0.5f;
					GenCorner a = B.C( A.xy[ i * 2 ], A.xy[ i * 2 + 1 ], za, na[ 0 ], na[ 1 ], na[ 2 ], ua, vA );
					GenCorner b = B.C( A.xy[ j * 2 ], A.xy[ j * 2 + 1 ], za, nb[ 0 ], nb[ 1 ], nb[ 2 ], ub, vA );
					GenCorner cc = B.C( Bc.xy[ i * 2 ], Bc.xy[ i * 2 + 1 ], zb2, nc[ 0 ], nc[ 1 ], nc[ 2 ], ua, vB );
					GenCorner d  = B.C( Bc.xy[ j * 2 ], Bc.xy[ j * 2 + 1 ], zb2, nd[ 0 ], nd[ 1 ], nd[ 2 ], ub, vB );
					if( o.image ) // raster: the sides take the image's colour at the edge
					{
						// 3 image px inside the outline: the edge pixels themselves are anti-aliased into the (black) transparency
						const float in = 3.0f;
						B.Planar( A.xy[ i * 2 ] / sc - vn[ i ].x * in, A.xy[ i * 2 + 1 ] / sc - vn[ i ].y * in, u0, v0 );
						B.Planar( A.xy[ j * 2 ] / sc - vn[ j ].x * in, A.xy[ j * 2 + 1 ] / sc - vn[ j ].y * in, u1, v1 );
						a.uv[ 0 ] = cc.uv[ 0 ] = u0, a.uv[ 1 ] = cc.uv[ 1 ] = v0, b.uv[ 0 ] = d.uv[ 0 ] = u1, b.uv[ 1 ] = d.uv[ 1 ] = v1;
					}
					B.Tri( part, a, cc, b );
					B.Tri( part, b, cc, d );
				}
			};
			// front bevel: ring k -> k + 1 (normals from the profile slope); back bevel mirrored
			std::vector< Contour > rings;
			for( const V2& pr : P )
				rings.push_back( pr.x > 0 ? Inset( c, pr.x ) : c );
			for( size_t k = 0; k + 1 < P.size(); ++k )
			{
				const float r = P[ k ].x - P[ k + 1 ].x, h = P[ k + 1 ].y - P[ k ].y; // outward, down
				const float slope = std::hypot( r, h ) + 1e-12f;
				// segment normal (outward-up) = (h, r) in (radial, z): smooth across segments for Round
				float hh = h / slope, zz = r / slope;
				emitStrip( 3, rings[ k ], zf - P[ k ].y, rings[ k + 1 ], zf - P[ k + 1 ].y, zz, zz, hh, hh, 0, 0.1f );
				if( backCap )
					emitStrip( 3, rings[ k + 1 ], zb + P[ k + 1 ].y, rings[ k ], zb + P[ k ].y, -zz, -zz, hh, hh, 0.9f, 1 );
			}
			// side wall
			const float zs0 = zf - P.back().y, zs1 = backCap ? zb + P.back().y : zb;
			emitStrip( 2, rings.back(), zs0, rings.back(), zs1, 0, 0, 1, 1, 0.1f, 0.9f );
		}
		tris0 = int( m.triPart.size() );
	}
	m.groupCount = groups;
	// parts: Front, Back, Side, Bevel (texture: the palette, the image on every part (raster), or plain); W2: + Core = the cut
	// faces of the fractured copy (empty on the whole mesh, so the Parts group shows it at rest too)
	const char* names[ 5 ] = { "Front", "Back", "Side", "Bevel", "Core" };
	for( int p = 0; p < ( frac ? 5 : 4 ); ++p )
	{
		Part part;
		part.name = names[ p ];
		for( int k = 0; k < 4; ++k )
			part.baseColor[ k ] = 1.0f;
		part.roughness = 0.35f, part.metallic = 0.0f;
		if( palette )
			part.tex[ CH_BASECOLOR ].image = pal, part.tex[ CH_BASECOLOR ].source = "logo palette";
		else if( o.image && g.F( "colours", 0 ) < 0.5f )
			part.tex[ CH_BASECOLOR ].image = o.image, part.tex[ CH_BASECOLOR ].source = "logo image";
		m.parts.push_back( part );
	}
	if( frac )
	{
		const int count = std::clamp( (int)std::lround( g.F( "frac", 64 ) ), 1, 4096 );
		Fracture( m, count, zb, backCap, groupUV, B, sc, *frac, *triPiece, *pieceCount );
		frac->parts = m.parts;
	}
	(void)tris0;
	(void)status;
}

std::string Ext( const std::string& p )
{
	const size_t d = p.find_last_of( '.' );
	return d == std::string::npos ? "" : Lower( p.substr( d + 1 ) );
}
}

// ---------------------------------------------------------------- public
std::string GenParams::Get( const std::string& k, const std::string& def ) const
{
	auto it = kv.find( k );
	return it == kv.end() ? def : it->second;
}
float GenParams::F( const std::string& k, float def ) const
{
	auto it = kv.find( k );
	return it == kv.end() || it->second.empty() ? def : std::strtof( it->second.c_str(), nullptr );
}

std::string MakeSpec( const std::map< std::string, std::string >& fields )
{
	std::string s = "gen:";
	for( const auto& kv : fields )
		s += kv.first + "=" + kv.second + kSep;
	return s;
}
bool IsGenSpec( const std::string& path ) { return path.compare( 0, 4, "gen:" ) == 0; }

std::string GenSpecKey( const std::string& spec )
{
	std::string key = spec;
	for( const char* field : { "file=", "fontfile=" } )
	{
		const std::string f = std::string( 1, kSep ) + field;
		size_t a = spec.find( f );
		if( spec.compare( 4, f.size() - 1, field ) == 0 )
			a = 3;
		if( a == std::string::npos )
			continue;
		a += f.size();
		const size_t b = spec.find( kSep, a );
		struct stat st;
		if( stat( spec.substr( a, b - a ).c_str(), &st ) == 0 )
			key += "|" + std::to_string( (long long)st.st_mtimespec.tv_sec ) + "." + std::to_string( (long long)st.st_mtimespec.tv_nsec ) + "|" + std::to_string( (long long)st.st_size );
	}
	return key;
}

std::string ShortSpecName( const std::string& spec )
{
	GenParams g;
	for( size_t a = 4; a < spec.size(); )
	{
		size_t b = spec.find( kSep, a ), e = spec.find( '=', a );
		if( b == std::string::npos )
			b = spec.size();
		if( e != std::string::npos && e < b )
			g.kv[ spec.substr( a, e - a ) ] = spec.substr( e + 1, b - e - 1 );
		a = b + 1;
	}
	if( g.Get( "source" ) == "text" )
	{
		std::string t = g.Get( "text" );
		std::replace( t.begin(), t.end(), '\n', '|' );
		return "text '" + t.substr( 0, 24 ) + "'";
	}
	const std::string f = g.Get( "file" );
	return "logo " + f.substr( f.find_last_of( '/' ) + 1 );
}

std::unique_ptr< ModelData > GenerateModel( const std::string& spec, const std::atomic< bool >& cancel, std::string& error, std::string& status )
{
	const auto t0 = std::chrono::steady_clock::now();
	GenParams g;
	for( size_t a = 4; a < spec.size(); )
	{
		size_t b = spec.find( kSep, a ), e = spec.find( '=', a );
		if( b == std::string::npos )
			b = spec.size();
		if( e != std::string::npos && e < b )
			g.kv[ spec.substr( a, e - a ) ] = spec.substr( e + 1, b - e - 1 );
		a = b + 1;
	}
	Outline o;
	bool ok = false;
	if( g.Get( "source" ) == "text" )
		ok = TextOutline( g, o, status, error );
	else
	{
		const std::string f = g.Get( "file" ), ext = Ext( f );
		struct stat st;
		if( f.empty() )
			error = status = "Pick a Logo File (svg, pdf, ai, png, jpg)";
		else if( stat( f.c_str(), &st ) != 0 )
			error = status = "Logo file not found: " + f.substr( f.find_last_of( '/' ) + 1 );
		else if( ext == "eps" || ext == "ps" )
			error = status = "EPS not supported on this macOS: save it as SVG or PDF";
		else if( ext == "svg" )
			ok = SvgOutline( f, g, o, status, error );
		else if( ext == "pdf" || ext == "ai" )
			ok = PdfOutline( f, g, o, status, error );
		else if( ext == "png" || ext == "jpg" || ext == "jpeg" )
			ok = RasterOutline( f, g, o, status, error );
		else
			error = status = "Unsupported logo type ." + ext + " (svg, pdf, ai, png, jpg)";
	}
	if( !ok || cancel.load() )
		return nullptr;
	const double msOutline = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	GenMesh m, frac;
	std::vector< uint32_t > triPiece;
	int pieces      = 0;
	const bool cut  = g.F( "frac", 0 ) > 0.5f; // W2 Shatter Mode Pieces: the fractured copy too
	Extrude( o, g, m, status, cut ? &frac : nullptr, &triPiece, &pieces );
	if( m.corners.empty() )
	{
		error = status = "Nothing to extrude (every shape collapsed)";
		return nullptr;
	}
	auto model       = ModelFromMesh( m, spec, cut ? &frac : nullptr, &triPiece, pieces );
	model->status    = status;
	const double ms  = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	model->msParse    = msOutline;
	model->msGeometry = ms - msOutline;
	Log( "GEN %s: %s | %zu tris, %d letters/shapes%s | outline %.1f ms, mesh %.1f ms", ShortSpecName( spec ).c_str(), status.c_str(), model->wholeIndexCount ? model->wholeIndexCount / 3 : model->triangleCount,
	     model->groupCount, cut ? ( " + " + std::to_string( pieces ) + " solid pieces (" + std::to_string( model->triangleCount - model->wholeIndexCount / 3 ) + " tris)" ).c_str() : "", msOutline, ms - msOutline );
	return model;
}

std::vector< std::string > FontFamilies()
{
	std::vector< std::string > out;
	CFArrayRef a = CTFontManagerCopyAvailableFontFamilyNames();
	if( !a )
		return out;
	for( CFIndex i = 0; i < CFArrayGetCount( a ); ++i )
	{
		std::string s = CFStr( (CFStringRef)CFArrayGetValueAtIndex( a, i ) );
		if( !s.empty() && s[ 0 ] != '.' )
			out.push_back( s );
	}
	CFRelease( a );
	std::sort( out.begin(), out.end(), []( const std::string& x, const std::string& y ) { return Lower( x ) < Lower( y ); } );
	out.erase( std::unique( out.begin(), out.end() ), out.end() );
	return out;
}

std::vector< std::string > FontStyles( const std::string& family )
{
	std::vector< std::string > out;
	CFStringRef fam       = MakeCF( family );
	const void* keys[ 1 ] = { kCTFontFamilyNameAttribute };
	const void* vals[ 1 ] = { fam };
	CFDictionaryRef attrs = CFDictionaryCreate( nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks );
	CTFontDescriptorRef d = CTFontDescriptorCreateWithAttributes( attrs );
	CFArrayRef all        = CTFontDescriptorCreateMatchingFontDescriptors( d, nullptr );
	std::vector< std::tuple< float, int, std::string > > faces; // weight, italic, name: the menu runs light -> heavy
	if( all )
	{
		for( CFIndex i = 0; i < CFArrayGetCount( all ); ++i )
		{
			CTFontDescriptorRef fd = (CTFontDescriptorRef)CFArrayGetValueAtIndex( all, i );
			CFStringRef st         = (CFStringRef)CTFontDescriptorCopyAttribute( fd, kCTFontStyleNameAttribute );
			std::string name       = CFStr( st );
			if( st )
				CFRelease( st );
			float weight = 0;
			int italic   = 0;
			if( CFDictionaryRef tr = (CFDictionaryRef)CTFontDescriptorCopyAttribute( fd, kCTFontTraitsAttribute ) )
			{
				if( CFNumberRef w = (CFNumberRef)CFDictionaryGetValue( tr, kCTFontWeightTrait ) )
					CFNumberGetValue( w, kCFNumberFloatType, &weight );
				uint32_t sym = 0;
				if( CFNumberRef sy = (CFNumberRef)CFDictionaryGetValue( tr, kCTFontSymbolicTrait ) )
					CFNumberGetValue( sy, kCFNumberSInt32Type, &sym );
				italic = ( sym & kCTFontItalicTrait ) ? 1 : 0;
				CFRelease( tr );
			}
			if( !name.empty() && std::none_of( faces.begin(), faces.end(), [ & ]( const auto& f ) { return std::get< 2 >( f ) == name; } ) )
				faces.emplace_back( weight, italic, name );
		}
		CFRelease( all );
	}
	std::stable_sort( faces.begin(), faces.end() );
	for( const auto& f : faces )
		out.push_back( std::get< 2 >( f ) );
	CFRelease( d ), CFRelease( attrs ), CFRelease( fam );
	return out;
}
}
