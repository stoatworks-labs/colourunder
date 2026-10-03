/// The OpenFX build of Colourunder, for DaVinci Resolve, Nuke, Natron, Vegas
/// and other OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **The deck and the whole frame plan.** `frame::Make` is the CPU half of the
/// FFGL build's ProcessOpenGL -- the settings, the nine kernels, the tracking
/// error and bar, the head switch, the phase error, the dropouts, the noise
/// seeds -- linked straight in, not ported. Model.cpp and Controls.cpp come with
/// it, and so do the defaults (`frame::Values`).
///
/// **What exists twice** is only the per-pixel work of the seven fragment
/// shaders, mirrored in `CpuPasses.cpp` (`//= mirrored`, both sides), and
/// `cutest --cpu` renders the two on the same frames and compares them. There
/// is no arithmetic in this file: it marshals OFX's pixel formats in, hands
/// the host's thread pool to the passes, and marshals the result back out.
///
/// ------------------------------------------------------ what is different
///
/// **The clock.** The FFGL build's clock is seconds since the effect's first
/// frame, because FFGL hands a plugin one frame at a time and an absolute
/// host clock that overflows a float. OpenFX renders frames out of order,
/// alone and concurrently, so here the clock is the clip's own time:
/// `seconds = time / frame rate`. Everything that moves -- the tracking
/// error's drift, and the video frame floor( seconds x 25 or 29.97 ) that
/// seeds the tape noise, the dropouts, the phase error and the bar's jitter --
/// was already a pure function of the seconds, so a frame is the same picture
/// whether it is rendered alone, in order, backwards or twice. Nothing is
/// carried from one render to the next and there is no temporal clip access:
/// a tape has no memory of the frame before.
///
/// That is the only reformulation. Every one of the FFGL build's ten controls
/// carries over, with the same name, range, default and group; the FFGL build
/// has no audio, no events and no beat sync to drop.
///
/// ------------------------------------------------------------- and tiles
///
/// The line raster is the whole picture averaged onto the standard's 576 or 480
/// lines, the convolutions run the width of a line, and the compensator and
/// the comb read lines above: there is no tile smaller than the frame.
/// `setSupportsTiles( false )` is a statement of fact about the effect.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../CpuPasses.h"
#include "../Frame.h"
#include "../Model.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.colourunder";
constexpr const char* kPluginName       = "Colourunder";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"VHS's helical-scan colour-under recording.\n\n"
	"The chroma is heterodyned down under the FM luma, so it keeps about 40 lines "
	"of resolution, arrives late and picks up band-limited blotches of noise; its "
	"playback phase error is a hue shift on NTSC and a desaturation on PAL. The "
	"head switch 6.5 lines before V sync tears the bottom lines, a tracking error "
	"walks a noise bar up the picture, and a dropout is a white streak or, with "
	"DOC on, the line above repeated. SP, LP and EP; copies of copies.\n\n"
	"Start with Speed, Tracking and Generation.\n\n"
	"In OpenFX the tape's clock is the clip's own time: the noise, the dropouts "
	"and the tracking drift are a function of the frame, so a frame renders the "
	"same alone, in order or twice. The Resolume build's clock is the time since "
	"the effect started instead; every control is the same in both.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamStandard    = "standard";
constexpr const char* kParamSpeed       = "speed";
constexpr const char* kParamTracking    = "tracking";
constexpr const char* kParamHeadSwitch  = "headSwitch";
constexpr const char* kParamWear        = "wear";
constexpr const char* kParamGeneration  = "generation";
constexpr const char* kParamDoc         = "doc";
constexpr const char* kParamChromaDelay = "chromaDelay";
constexpr const char* kParamChromaNoise = "chromaNoise";
constexpr const char* kParamMix         = "mix";

using namespace colourunder;

/// The clip's frame rate, for turning OFX time (frames) into seconds. A host
/// that does not say gets PAL's 25: the noise then changes every frame, which
/// is what it does at 25 fps anyway.
double framesPerSecond( const OFX::Clip* clip, const OFX::ImageEffect& effect )
{
	double fps = 0.0;
	try
	{
		fps = clip->getFrameRate();
	}
	catch( ... )
	{
		fps = 0.0;
	}
	if( !( fps > 0.0 ) || !std::isfinite( fps ) )
	{
		try
		{
			fps = effect.getFrameRate();
		}
		catch( ... )
		{
			fps = 0.0;
		}
	}
	return fps > 0.0 && std::isfinite( fps ) ? fps : 25.0;
}

//---------------------------------------------------------------------------
// The host's thread pool, as cpu::ParallelFor. OFX's multi-thread suite
// calls multiThreadFunction( index, count ) on each of its threads and
// returns when all are done; each takes a contiguous run of lines.
//---------------------------------------------------------------------------
class RangeProcessor : public OFX::MultiThread::Processor
{
public:
	RangeProcessor( int total, const std::function< void( int, int ) >& work ) :
		count( total ),
		body( work )
	{
	}

	void multiThreadFunction( unsigned int threadIndex, unsigned int threadMax ) override
	{
		const int64_t n     = count;
		const int first     = static_cast< int >( n * threadIndex / threadMax );
		const int last      = static_cast< int >( n * ( threadIndex + 1 ) / threadMax );
		if( first < last )
			body( first, last );
	}

private:
	int count;
	const std::function< void( int, int ) >& body;
};

void hostParallel( int count, const std::function< void( int, int ) >& body )
{
	if( count <= 0 )
		return;
	unsigned int threads = OFX::MultiThread::getNumCPUs();
	threads              = std::max( 1u, std::min( threads, static_cast< unsigned int >( count ) ) );
	RangeProcessor processor( count, body );
	processor.multiThread( threads );
}

//---------------------------------------------------------------------------
// Marshalling: one conversion in, one out. The passes take RGBA float with
// row 0 at the bottom -- OFX's own orientation, so nothing here flips.
//
// The picture is recorded as the picture over black, which for a
// premultiplied clip is the pixels as handed over (the FFGL build's rule:
// Resolume's clips with alpha are already premultiplied). A straight clip is
// multiplied up on the way in and divided back on the way out, so both
// conventions describe the same tape.
//---------------------------------------------------------------------------
template< typename Pixel, int Components, int Maximum >
void gather( const OFX::Image* src, const OfxRectI& bounds, bool premultiplied, std::vector< float >& out )
{
	const int width   = bounds.x2 - bounds.x1;
	const int height  = bounds.y2 - bounds.y1;
	const float scale = 1.0f / static_cast< float >( Maximum );

	hostParallel( height, [ & ]( int firstRow, int lastRow ) {
		for( int y = firstRow; y < lastRow; ++y )
		{
			float* row = out.data() + static_cast< size_t >( y ) * width * 4;
			for( int x = 0; x < width; ++x )
			{
				const Pixel* px = static_cast< const Pixel* >( src->getPixelAddress( bounds.x1 + x, bounds.y1 + y ) );
				float* dst      = row + static_cast< size_t >( x ) * 4;
				if( px == nullptr )
				{
					dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = dst[ 3 ] = 0.0f;
					continue;
				}
				const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) * scale : 1.0f;
				for( int c = 0; c < 3; ++c )
				{
					const float v = static_cast< float >( px[ c ] ) * scale;
					dst[ c ]      = premultiplied ? v : v * a;
				}
				dst[ 3 ] = a;
			}
		}
	} );
}

template< typename Pixel, int Components, int Maximum >
void scatter( const std::vector< float >& in, OFX::Image* dst, const OfxRectI& bounds, const OfxRectI& window, bool premultiplied )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = static_cast< float >( Maximum );

	hostParallel( window.y2 - window.y1, [ & ]( int firstRow, int lastRow ) {
		for( int y = window.y1 + firstRow; y < window.y1 + lastRow; ++y )
		{
			for( int x = window.x1; x < window.x2; ++x )
			{
				Pixel* px = static_cast< Pixel* >( dst->getPixelAddress( x, y ) );
				if( px == nullptr )
					continue;

				const float* source = in.data() + ( static_cast< size_t >( y - bounds.y1 ) * width + ( x - bounds.x1 ) ) * 4;
				const float a       = source[ 3 ];
				for( int c = 0; c < 3; ++c )
				{
					float v = source[ c ];
					if( !premultiplied )
						v = a > 0.0f ? v / a : 0.0f;
					//Integer formats clamp; float ones are left alone (the display
					//pass has already clamped what the tape made).
					if( Maximum != 1 )
						v = std::clamp( v, 0.0f, 1.0f );
					px[ c ] = static_cast< Pixel >( Maximum == 1 ? v : std::lround( v * scale ) );
				}
				if( Components == 4 )
					px[ 3 ] = static_cast< Pixel >( Maximum == 1 ? a : std::lround( std::clamp( a, 0.0f, 1.0f ) * scale ) );
			}
		}
	} );
}

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name,
                                          const char* label, const char* hint, double value, OFX::GroupParamDescriptor* parent )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDefault( value );
	param->setParent( *parent );
	page->addChild( *param );
	return param;
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name, const char* label,
                   const char* hint, int count, const char* ( *labelFor )( int ), int value, OFX::GroupParamDescriptor* parent )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( labelFor( i ) );
	param->setDefault( value );
	param->setAnimates( false );
	param->setParent( *parent );
	page->addChild( *param );
}

OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( name, name, name );
	page->addChild( *group );
	return group;
}

class ColourunderOFXPlugin : public OFX::ImageEffect
{
public:
	explicit ColourunderOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		standard    = fetchChoiceParam( kParamStandard );
		speed       = fetchChoiceParam( kParamSpeed );
		tracking    = fetchDoubleParam( kParamTracking );
		headSwitch  = fetchDoubleParam( kParamHeadSwitch );
		wear        = fetchDoubleParam( kParamWear );
		generation  = fetchIntParam( kParamGeneration );
		doc         = fetchBooleanParam( kParamDoc );
		chromaDelay = fetchDoubleParam( kParamChromaDelay );
		chromaNoise = fetchDoubleParam( kParamChromaNoise );
		mix         = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );
		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		if( src->getPixelDepth() != depth || src->getPixelComponents() != comps )
			OFX::throwSuiteStatusException( kOfxStatErrImageFormat );

		const OfxRectI bounds = src->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		//An RGB clip has no alpha to be premultiplied by; treating it as
		//premultiplied is what makes the round trip an identity there.
		const bool premultiplied = comps != OFX::ePixelComponentRGBA || srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		//The plan for this frame: a pure function of the controls at this
		//time, the size, and the clip's own time in seconds.
		const double seconds   = args.time / framesPerSecond( srcClip, *this );
		const frame::Plan plan = frame::Make( valuesAt( args.time ), width, height, seconds );

		std::vector< float > picture( static_cast< size_t >( width ) * height * 4 );
		std::vector< float > out( picture.size(), 0.0f );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA ? gather< unsigned char, 4, 255 >( src.get(), bounds, premultiplied, picture )
			                                  : gather< unsigned char, 3, 255 >( src.get(), bounds, premultiplied, picture );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA ? gather< unsigned short, 4, 65535 >( src.get(), bounds, premultiplied, picture )
			                                  : gather< unsigned short, 3, 65535 >( src.get(), bounds, premultiplied, picture );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA ? gather< float, 4, 1 >( src.get(), bounds, premultiplied, picture )
			                                  : gather< float, 3, 1 >( src.get(), bounds, premultiplied, picture );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}

		//The line passes always run over the whole picture; the display only
		//over the rows the host asked for.
		OfxRectI window = args.renderWindow;
		window.x1       = std::max( window.x1, bounds.x1 );
		window.x2       = std::min( window.x2, bounds.x2 );
		window.y1       = std::max( window.y1, bounds.y1 );
		window.y2       = std::min( window.y2, bounds.y2 );
		if( window.x1 >= window.x2 || window.y1 >= window.y2 )
			return;
		cpu::Render( plan, picture.data(), out.data(), hostParallel, window.y1 - bounds.y1, window.y2 - bounds.y1 );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA ? scatter< unsigned char, 4, 255 >( out, dst.get(), bounds, window, premultiplied )
			                                  : scatter< unsigned char, 3, 255 >( out, dst.get(), bounds, window, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA ? scatter< unsigned short, 4, 65535 >( out, dst.get(), bounds, window, premultiplied )
			                                  : scatter< unsigned short, 3, 65535 >( out, dst.get(), bounds, window, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA ? scatter< float, 4, 1 >( out, dst.get(), bounds, window, premultiplied )
			                                  : scatter< float, 3, 1 >( out, dst.get(), bounds, window, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	/// Mix 0 is the source exactly, in the FFGL build too ( src x 1 + tape x 0 ).
	bool isIdentity( const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime ) override
	{
		if( mix->getValueAtTime( args.time ) > 0.0 )
			return false;
		identityClip = srcClip;
		identityTime = args.time;
		return true;
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;
	}

private:
	frame::Values valuesAt( double time ) const
	{
		//ChoiceParam answers through an out parameter rather than a return
		//value, unlike every other param type in the Support library.
		const auto choice = [ time ]( OFX::ChoiceParam* param ) {
			int value = 0;
			param->getValueAtTime( time, value );
			return static_cast< float >( value );
		};

		frame::Values v;
		v.standard    = choice( standard );
		v.speed       = choice( speed );
		v.tracking    = static_cast< float >( tracking->getValueAtTime( time ) );
		v.headSwitch  = static_cast< float >( headSwitch->getValueAtTime( time ) );
		v.wear        = static_cast< float >( wear->getValueAtTime( time ) );
		v.generation  = static_cast< float >( generation->getValueAtTime( time ) );
		v.doc         = doc->getValueAtTime( time ) ? 1.0f : 0.0f;
		v.chromaDelay = static_cast< float >( chromaDelay->getValueAtTime( time ) );
		v.chromaNoise = static_cast< float >( chromaNoise->getValueAtTime( time ) );
		v.mix         = static_cast< float >( mix->getValueAtTime( time ) );
		return v;
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* standard    = nullptr;
	OFX::ChoiceParam* speed       = nullptr;
	OFX::DoubleParam* tracking    = nullptr;
	OFX::DoubleParam* headSwitch  = nullptr;
	OFX::DoubleParam* wear        = nullptr;
	OFX::IntParam* generation     = nullptr;
	OFX::BooleanParam* doc        = nullptr;
	OFX::DoubleParam* chromaDelay = nullptr;
	OFX::DoubleParam* chromaNoise = nullptr;
	OFX::DoubleParam* mix         = nullptr;
};

mDeclarePluginFactory( ColourunderPluginFactory, {}, {} );
} // namespace

void ColourunderPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The line raster is the whole picture averaged onto 576 or 480 lines, so
	// there is no tile to render from. Frames are independent of each other and
	// of render order: no history, and the clock is the clip's time.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setSupportsMultipleClipPARs( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void ColourunderPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// Same parameters, same ranges, same defaults and the same groups as the
	// FFGL build, so the two inspectors read identically and one guide covers
	// both. The defaults are frame::Values, which the FFGL constructor reads.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const frame::Values defaults;

	//------------------------------------------------------------------- Deck
	OFX::GroupParamDescriptor* deck = defineGroup( desc, page, "Deck" );
	defineChoice( desc, page, kParamStandard, "Standard",
	              "PAL or NTSC: 576 or 480 lines, and what the playback phase error does. PAL's "
	              "alternating V axis and the 1H average turn it into a loss of saturation; "
	              "NTSC shows it as hue.",
	              model::kStandardCount, controls::StandardName, controls::OptionIndex( defaults.standard, model::kStandardCount ), deck );
	defineChoice( desc, page, kParamSpeed, "Speed",
	              "Tape speed. SP has luma to 3.0 MHz, LP to 2.76, EP to 2.4, with more noise at each "
	              "step down. The chroma is the colour-under band at every speed.",
	              model::kSpeedCount, controls::SpeedName, controls::OptionIndex( defaults.speed, model::kSpeedCount ), deck );
	defineSlider( desc, page, kParamTracking, "Tracking",
	              "The tracking error. At 0 the noise bar rests in the vertical interval; as the error "
	              "grows and drifts it walks up through the picture.",
	              defaults.tracking, deck );
	defineSlider( desc, page, kParamHeadSwitch, "Head Switch",
	              "The head-to-head time-base step, 0 to 2 us: the last lines of each field tear "
	              "sideways and the TV takes a dozen lines to pull them back.",
	              defaults.headSwitch, deck );

	//------------------------------------------------------------------- Tape
	OFX::GroupParamDescriptor* tape = defineGroup( desc, page, "Tape" );
	defineSlider( desc, page, kParamWear, "Wear",
	              "Missing oxide: dropouts (up to 12 a frame) and a little lost RF.",
	              defaults.wear, tape );
	OFX::IntParamDescriptor* generationParam = desc.defineIntParam( kParamGeneration );
	generationParam->setLabels( "Generation", "Generation", "Generation" );
	generationParam->setHint( "Copies of copies, 1 to 5: each dub runs the whole chain again, so the colour "
	                          "narrows, the delay adds up and the noise and dropouts of every tape are there." );
	generationParam->setRange( model::kGenerationsMin, model::kGenerationsMax );
	generationParam->setDisplayRange( model::kGenerationsMin, model::kGenerationsMax );
	generationParam->setDefault( controls::Generation( defaults.generation ) );
	generationParam->setParent( *tape );
	page->addChild( *generationParam );
	OFX::BooleanParamDescriptor* docParam = desc.defineBooleanParam( kParamDoc );
	docParam->setLabels( "DOC", "DOC", "DOC" );
	docParam->setHint( "The dropout compensator: a dropout is the same stretch of the line 1H before, "
	                   "repeated, instead of a white streak." );
	docParam->setDefault( defaults.doc >= 0.5f );
	docParam->setParent( *tape );
	page->addChild( *docParam );

	//----------------------------------------------------------------- Colour
	OFX::GroupParamDescriptor* colour = defineGroup( desc, page, "Colour" );
	defineSlider( desc, page, kParamChromaDelay, "Chroma Delay",
	              "The playback filters' group delay, 0 to 1 us: the colour arrives late and bleeds "
	              "right of every edge.",
	              defaults.chromaDelay, colour );
	defineSlider( desc, page, kParamChromaNoise, "Chroma Noise",
	              "Noise on the colour-under band (blotches, not grain) and the playback phase error "
	              "(hue on NTSC, saturation on PAL).",
	              defaults.chromaNoise, colour );
	defineSlider( desc, page, kParamMix, "Mix",
	              "Wet/dry against the untouched input. At 1 the output is opaque: a tape has no alpha.",
	              defaults.mix, colour );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* ColourunderPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new ColourunderOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static ColourunderPluginFactory* factory = new ColourunderPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
