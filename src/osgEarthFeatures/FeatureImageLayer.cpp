/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarthFeatures/FeatureImageLayer>
#include <osgEarthFeatures/Session>
#include <osgEarthFeatures/FeatureCursor>
#include <osgEarthFeatures/TransformFilter>
#include <osgEarthFeatures/BufferFilter>
#include <osgEarthFeatures/ResampleFilter>
#include <osgEarthSymbology/AGG.h>
#include <osgEarthSymbology/StyleSheet>
#include <osgEarth/Registry>
#include <osgEarth/Progress>

using namespace osgEarth;
using namespace osgEarth::Features;
using namespace osgEarth::Symbology;

#define LC "[FeatureImageLayer] "


REGISTER_OSGEARTH_LAYER(featureimage, FeatureImageLayer);
REGISTER_OSGEARTH_LAYER(feature_image, FeatureImageLayer);

//........................................................................

namespace osgEarth { namespace Features { namespace FeatureImageLayerImpl
{
    struct RenderFrame
    {
        double xmin, ymin;
        double xf, yf;
    };

    struct float32
    {
        float32() : value(NO_DATA_VALUE) { }
        float32(float v) : value(v) { }

        float value;
    };

    struct span_coverage32
    {
        static void render(unsigned char* ptr, 
                           int x,
                           unsigned count, 
                           const unsigned char* covers, 
                           const float32& c)
        {
            unsigned char* p = ptr + (x << 2);
            float* f = (float*)p;
            do
            {
                unsigned char cover = *covers++;
                int hasData = cover > 127;
                *f++ = hasData ? c.value : NO_DATA_VALUE;
            }
            while(--count);
        }

        static void hline(unsigned char* ptr, 
                          int x,
                          unsigned count, 
                          const float32& c)
        {
            unsigned char* p = ptr + (x << 2);
            float* f = (float*)p;
            do {
                *f++ = c.value;
            }
            while(--count);
        }

        static float32 get(unsigned char* ptr, int x)
        {
            unsigned char* p = ptr + (x << 2);
            float* f = (float*)p;
            return float32(*f);
        }
    };

    // rasterizes a geometry to color
    void rasterize(const Geometry* geometry, const osg::Vec4& color, RenderFrame& frame, 
                   agg::rasterizer& ras, agg::rendering_buffer& buffer)
    {
        unsigned a = (unsigned)(127.0f+(color.a()*255.0f)/2.0f); // scale alpha up
        agg::rgba8 fgColor = agg::rgba8( (unsigned)(color.r()*255.0f), (unsigned)(color.g()*255.0f), (unsigned)(color.b()*255.0f), a );
        
        ConstGeometryIterator gi( geometry );
        while( gi.hasMore() )
        {
            const Geometry* g = gi.next();

            for( Geometry::const_iterator p = g->begin(); p != g->end(); p++ )
            {
                const osg::Vec3d& p0 = *p;
                double x0 = frame.xf*(p0.x()-frame.xmin);
                double y0 = frame.yf*(p0.y()-frame.ymin);

                if ( p == g->begin() )
                    ras.move_to_d( x0, y0 );
                else
                    ras.line_to_d( x0, y0 );
            }
        }
        agg::renderer<agg::span_abgr32, agg::rgba8> ren(buffer);
        ras.render(ren, fgColor);

        ras.reset();
    }


    void rasterizeCoverage(const Geometry* geometry, float value, RenderFrame& frame, 
                           agg::rasterizer& ras, agg::rendering_buffer& buffer)
    {
        ConstGeometryIterator gi( geometry );
        while( gi.hasMore() )
        {
            const Geometry* g = gi.next();

            for( Geometry::const_iterator p = g->begin(); p != g->end(); p++ )
            {
                const osg::Vec3d& p0 = *p;
                double x0 = frame.xf*(p0.x()-frame.xmin);
                double y0 = frame.yf*(p0.y()-frame.ymin);

                if ( p == g->begin() )
                    ras.move_to_d( x0, y0 );
                else
                    ras.line_to_d( x0, y0 );
            }
        }
        
        agg::renderer<span_coverage32, float32> ren(buffer);
        ras.render(ren, value);
        ras.reset();
    }
}}};

//........................................................................

Config
FeatureImageLayer::Options::getConfig() const
{
    Config conf = ImageLayer::Options::getConfig();
    conf.set("features", featureSourceLayer());
    conf.set("gamma", gamma());
    conf.set("styles", _styles);
    return conf;
}

void
FeatureImageLayer::Options::fromConfig(const Config& conf)
{
    gamma().init(1.3);
    
    conf.get("features", featureSourceLayer());
    conf.get("feature_source", featureSourceLayer());
    conf.get("gamma", gamma());
    conf.get("styles", _styles);
}

//........................................................................

using namespace osgEarth::Features::FeatureImageLayerImpl;

void
FeatureImageLayer::init()
{
    ImageLayer::init();
    setTileSourceExpected(false);

    // Default profile (WGS84)
    setProfile(Profile::create("global-geodetic"));
}

const Status&
FeatureImageLayer::open()
{
    // assert a feature source:
    if (!_features.valid() && !options().featureSourceLayer().isSet())
    {
        return setStatus(Status::ConfigurationError, "Missing required feature source");
    }

    return ImageLayer::open();
}

void
FeatureImageLayer::addedToMap(const Map* map)
{
    _session = new Session(map, getStyleSheet(), getFeatureSource(), getReadOptions());

    if (_features.valid())
    {
        establishSession();
    }

    else if (options().featureSourceLayer().isSet())
    {
        _layerListener.listen(
            map,
            options().featureSourceLayer().get(),
            this,
            &FeatureImageLayer::setFeatureSource);
    }

    else
    {
        setStatus(Status::ConfigurationError, "No features");
    }
}

void
FeatureImageLayer::removedFromMap(const Map* map)
{
    _layerListener.clear();
}

void
FeatureImageLayer::setFeatureSource(FeatureSource* fs)
{
    _features = fs;
    _featureProfile = 0L;

    if (_features.valid())
    {
        if (_features->getStatus().isError())
        {
            setStatus(_features->getStatus());
        }

        else
        {
            // with a new feature source, we need to re-establish
            // the data extents and open a new session.
            establishSession();
        }
    }
}

void
FeatureImageLayer::establishSession()
{    
    // Try to fill the DataExtent list using the FeatureProfile
    if (!_featureProfile.valid())
    {
        _featureProfile = _features->getFeatureProfile();

        dataExtents().clear();

        if (_featureProfile.valid())
        {
            // recalculate the data extents based on the feature source.
            if (_featureProfile->getProfile() != NULL)
            {
                // Use specified profile's GeoExtent
                dataExtents().push_back(DataExtent(_featureProfile->getProfile()->getExtent()));
            }
            else if (_featureProfile->getExtent().isValid() == true)
            {
                // Use FeatureProfile's GeoExtent
                dataExtents().push_back(DataExtent(_featureProfile->getExtent()));
            }
        }
    }

    if (_session.valid() && _features.valid())
    {
        _session->setFeatureSource(_features.get());
        _session->setStyles(options().styles().get());
    }
}

GeoImage
FeatureImageLayer::createImageImplementation(const TileKey& key, ProgressCallback* progress) const
{
    if (getStatus().isError())    
    {
        return GeoImage::INVALID;
    }
    
    if (!_features.valid())
    {
        setStatus(Status::ServiceUnavailable, "No feature source");
        return GeoImage::INVALID;
    }

    const FeatureProfile* featureProfile = _features->getFeatureProfile();
    if (!featureProfile)
    {
        setStatus(Status::ConfigurationError, "Feature profile is missing");
        return GeoImage::INVALID;
    }

    const SpatialReference* featureSRS = featureProfile->getSRS();
    if (!featureSRS)
    {
        setStatus(Status::ConfigurationError, "Feature profile has no SRS");
        return GeoImage::INVALID;
    }
    
    // allocate the image.
    osg::ref_ptr<osg::Image> image = new osg::Image();

    if ( options().coverage() == true )
    {
        image->allocateImage(getTileSize(), getTileSize(), 1, GL_RED, GL_FLOAT);
        image->setInternalTextureFormat(GL_R16F);
        ImageUtils::markAsUnNormalized(image.get(), true);
    }
    else
    {
        image->allocateImage(getTileSize(), getTileSize(), 1, GL_RGBA, GL_UNSIGNED_BYTE);
    }

    preProcess(image.get());

    bool ok = render(key, getFeatureSource(), _session.get(), options().styles().get(), image.get(), progress);

    if (ok)
    {
        postProcess(image.get());
        return GeoImage(image.get(), key.getExtent());
    }
    else
    {
        return GeoImage::INVALID;
    }
}

bool
FeatureImageLayer::preProcess(osg::Image* image) const
{
    agg::rendering_buffer rbuf(image->data(), image->s(), image->t(), image->s() * 4);

    // clear the buffer.
    if (options().coverage() == true)
    {
        agg::renderer<span_coverage32, float32> ren(rbuf);
        ren.clear(float32(NO_DATA_VALUE));
    }
    else
    {
        agg::renderer<agg::span_abgr32, agg::rgba8> ren(rbuf);
        ren.clear(agg::rgba8(0, 0, 0, 0));
    }
    return true;
}

bool
FeatureImageLayer::postProcess(osg::Image* image) const
{
    if (options().coverage() == false)
    {
        //convert from ABGR to RGBA
        unsigned char* pixel = image->data();
        for (int i = 0; i < image->s()*image->t() * 4; i += 4, pixel += 4)
        {
            std::swap(pixel[0], pixel[3]);
            std::swap(pixel[1], pixel[2]);
        }
    }

    return true;
}

bool
FeatureImageLayer::renderFeaturesForStyle(Session*           session,
                                          const Style&       style,
                                          const FeatureList& features,
                                          const GeoExtent&   imageExtent,
                                          osg::Image*        image) const
{
    OE_DEBUG << LC << "Rendering " << features.size() << " features for " << imageExtent.toString() << "\n";

    // A processing context to use with the filters:
    FilterContext context(session);
    context.setProfile(getFeatureSource()->getFeatureProfile());

    const LineSymbol*    masterLine = style.getSymbol<LineSymbol>();
    const PolygonSymbol* masterPoly = style.getSymbol<PolygonSymbol>();
    const CoverageSymbol* masterCov = style.getSymbol<CoverageSymbol>();

    // sort into bins, making a copy for lines that require buffering.
    FeatureList polygons;
    FeatureList lines;

    for (FeatureList::const_iterator f = features.begin(); f != features.end(); ++f)
    {
        if (f->get()->getGeometry())
        {
            bool hasPoly = false;
            bool hasLine = false;

            if (masterPoly || f->get()->style()->has<PolygonSymbol>())
            {
                polygons.push_back(f->get());
                hasPoly = true;
            }

            if (masterLine || f->get()->style()->has<LineSymbol>())
            {
                // Use the GeometryIterator to get all the geometries so we can clone them as rings
                GeometryIterator gi(f->get()->getGeometry());
                while (gi.hasMore())
                {
                    Geometry* geom = gi.next();
                    // Create a new feature for each geometry
                    Feature* newFeature = new Feature(*f->get());
                    newFeature->setGeometry(geom);
                    if (!newFeature->getGeometry()->isLinear())
                    {
                        newFeature->setGeometry(newFeature->getGeometry()->cloneAs(Geometry::TYPE_RING));
                    }
                    lines.push_back(newFeature);
                    hasLine = true;
                }
            }

            // if there are no geometry symbols but there is a coverage symbol, default to polygons.
            if (!hasLine && !hasPoly)
            {
                if (masterCov || f->get()->style()->has<CoverageSymbol>())
                {
                    polygons.push_back(f->get());
                }
            }
        }
    }

    // initialize:
    RenderFrame frame;
    frame.xmin = imageExtent.xMin();
    frame.ymin = imageExtent.yMin();
    frame.xf = (double)image->s() / imageExtent.width();
    frame.yf = (double)image->t() / imageExtent.height();

    if (lines.size() > 0)
    {
        // We are buffering in the features native extent, so we need to use the
        // transformed extent to get the proper "resolution" for the image
        const SpatialReference* featureSRS = context.profile()->getSRS();
        GeoExtent transformedExtent = imageExtent.transform(featureSRS);

        double trans_xf = (double)image->s() / transformedExtent.width();
        double trans_yf = (double)image->t() / transformedExtent.height();

        // resolution of the image (pixel extents):
        double xres = 1.0 / trans_xf;
        double yres = 1.0 / trans_yf;

        // downsample the line data so that it is no higher resolution than to image to which
        // we intend to rasterize it. If you don't do this, you run the risk of the buffer 
        // operation taking forever on very high-res input data.
        if (true) //options().optimizeLineSampling() == true)
        {
            ResampleFilter resample;
            resample.minLength() = osg::minimum(xres, yres);
            context = resample.push(lines, context);
        }

        // now run the buffer operation on all lines:
        BufferFilter buffer;
        double lineWidth = 1.0;
        if (masterLine)
        {
            buffer.capStyle() = masterLine->stroke()->lineCap().value();

            if (masterLine->stroke()->width().isSet())
            {
                lineWidth = masterLine->stroke()->width().value();

                GeoExtent imageExtentInFeatureSRS = imageExtent.transform(featureSRS);
                double pixelWidth = imageExtentInFeatureSRS.width() / (double)image->s();

                // if the width units are specified, process them:
                if (masterLine->stroke()->widthUnits().isSet() &&
                    masterLine->stroke()->widthUnits().get() != Units::PIXELS)
                {
                    const Units& featureUnits = featureSRS->getUnits();
                    const Units& strokeUnits = masterLine->stroke()->widthUnits().value();

                    // if the units are different than those of the feature data, we need to
                    // do a units conversion.
                    if (featureUnits != strokeUnits)
                    {
                        if (Units::canConvert(strokeUnits, featureUnits))
                        {
                            // linear to linear, no problem
                            lineWidth = strokeUnits.convertTo(featureUnits, lineWidth);
                        }
                        else if (strokeUnits.isLinear() && featureUnits.isAngular())
                        {
                            // linear to angular? approximate degrees per meter at the 
                            // latitude of the tile's centroid.
                            double lineWidthM = masterLine->stroke()->widthUnits()->convertTo(Units::METERS, lineWidth);
                            double mPerDegAtEquatorInv = 360.0 / (featureSRS->getEllipsoid()->getRadiusEquator() * 2.0 * osg::PI);
                            double lon, lat;
                            imageExtent.getCentroid(lon, lat);
                            lineWidth = lineWidthM * mPerDegAtEquatorInv * cos(osg::DegreesToRadians(lat));
                        }
                    }

                    // enfore a minimum width of one pixel.
                    float minPixels = masterLine->stroke()->minPixels().getOrUse(1.0f);
                    lineWidth = osg::clampAbove(lineWidth, pixelWidth*minPixels);
                }

                else // pixels
                {
                    lineWidth *= pixelWidth;
                }
            }
        }

        buffer.distance() = lineWidth * 0.5;   // since the distance is for one side
        buffer.push(lines, context);
    }

    // Transform the features into the map's SRS:
    TransformFilter xform(imageExtent.getSRS());
    xform.setLocalizeCoordinates(false);
    FilterContext polysContext = xform.push(polygons, context);
    FilterContext linesContext = xform.push(lines, context);

    // set up the AGG renderer:
    agg::rendering_buffer rbuf(image->data(), image->s(), image->t(), image->s() * 4);

    // Create the renderer and the rasterizer
    agg::rasterizer ras;

    // Setup the rasterizer
    if (options().coverage() == true)
        ras.gamma(1.0);
    else
        ras.gamma(options().gamma().get());

    ras.filling_rule(agg::fill_even_odd);

    // construct an extent for cropping the geometry to our tile.
    // extend just outside the actual extents so we don't get edge artifacts:
    GeoExtent cropExtent = GeoExtent(imageExtent);
    cropExtent.scale(1.1, 1.1);
    double cropXMin, cropYMin, cropXMax, cropYMax;
    cropExtent.getBounds(cropXMin, cropYMin, cropXMax, cropYMax);

    // GEOS crop won't abide by weird extents, so if we're in geographic space
    // we must clamp the scaled extent back to a legal range.
    if (cropExtent.crossesAntimeridian())
    {
        osg::Vec3d centroid = imageExtent.getCentroid();
        if (centroid.x() < 0.0) // tile is east of antimeridian
        {
            cropXMin = -180.0;
            cropXMax = cropExtent.east();
        }
        else
        {
            cropXMin = cropExtent.west();
            cropXMax = 180.0;
        }
    }

    osg::ref_ptr<Symbology::Polygon> cropPoly = new Symbology::Polygon(4);
    cropPoly->push_back(osg::Vec3d(cropXMin, cropYMin, 0));
    cropPoly->push_back(osg::Vec3d(cropXMax, cropYMin, 0));
    cropPoly->push_back(osg::Vec3d(cropXMax, cropYMax, 0));
    cropPoly->push_back(osg::Vec3d(cropXMin, cropYMax, 0));

    // If there's a coverage symbol, make a copy of the expressions so we can evaluate them
    optional<NumericExpression> covValue;
    const CoverageSymbol* covsym = style.get<CoverageSymbol>();
    if (covsym && covsym->valueExpression().isSet())
        covValue = covsym->valueExpression().get();

    // render the polygons
    for (FeatureList::iterator i = polygons.begin(); i != polygons.end(); i++)
    {
        Feature*  feature = i->get();
        Geometry* geometry = feature->getGeometry();

        osg::ref_ptr<Geometry> croppedGeometry;
        if (geometry->crop(cropPoly.get(), croppedGeometry))
        {
            const PolygonSymbol* poly =
                feature->style().isSet() && feature->style()->has<PolygonSymbol>() ? feature->style()->get<PolygonSymbol>() :
                masterPoly;

            if (options().coverage() == true && covValue.isSet())
            {
                float value = (float)feature->eval(covValue.mutable_value(), &context);
                rasterizeCoverage(croppedGeometry.get(), value, frame, ras, rbuf);
            }
            else
            {
                osg::Vec4f color = poly->fill()->color();
                rasterize(croppedGeometry.get(), color, frame, ras, rbuf);
            }

        }
    }

    // render the lines
    for (FeatureList::iterator i = lines.begin(); i != lines.end(); i++)
    {
        Feature*  feature = i->get();
        Geometry* geometry = feature->getGeometry();

        osg::ref_ptr<Geometry> croppedGeometry;
        if (geometry->crop(cropPoly.get(), croppedGeometry))
        {
            const LineSymbol* line =
                feature->style().isSet() && feature->style()->has<LineSymbol>() ? feature->style()->get<LineSymbol>() :
                masterLine;

            if (options().coverage() == true && covValue.isSet())
            {
                float value = (float)feature->eval(covValue.mutable_value(), &context);
                rasterizeCoverage(croppedGeometry.get(), value, frame, ras, rbuf);
            }
            else
            {
                osg::Vec4f color = line ? static_cast<osg::Vec4>(line->stroke()->color()) : osg::Vec4(1, 1, 1, 1);
                rasterize(croppedGeometry.get(), color, frame, ras, rbuf);
            }
        }
    }

    return true;
}

//........................................................................

bool
FeatureImageRenderer::render(const TileKey& key, 
                             FeatureSource* features,
                             Session* session,
                             const StyleSheet* styles,
                             osg::Image* target,
                             ProgressCallback* progress) const
{
    Query defaultQuery;
    defaultQuery.tileKey() = key;

    // figure out if and how to style the geometry.
    if ( features->hasEmbeddedStyles() )
    {
        // Each feature has its own embedded style data, so use that:
        osg::ref_ptr<FeatureCursor> cursor = features->createFeatureCursor(defaultQuery, progress);
        while( cursor.valid() && cursor->hasMore() )
        {
            osg::ref_ptr< Feature > feature = cursor->nextFeature();
            if ( feature )
            {
                FeatureList list;
                list.push_back( feature );

                renderFeaturesForStyle(
                    session,
                    *feature->style(),
                    list,
                    key.getExtent(),
                    target );
            }
        }
    }
    else if ( styles )
    {
        if ( styles->selectors().size() > 0 )
        {
            for(StyleSelectorList::const_iterator i = styles->selectors().begin();
                i != styles->selectors().end();
                ++i)
            {
                const StyleSelector& sel = *i;

                if ( sel.styleExpression().isSet() )
                {
                    const FeatureProfile* featureProfile = features->getFeatureProfile();

                    // establish the working bounds and a context:
                    FilterContext context(session, featureProfile);
                    StringExpression styleExprCopy(  sel.styleExpression().get() );

                    FeatureList features;
                    getFeatures(session, defaultQuery, key.getExtent(), features, progress);
                    if (!features.empty())
                    {
                        for (FeatureList::iterator itr = features.begin(); itr != features.end(); ++itr)
                        {
                            Feature* feature = itr->get();

                            const std::string& styleString = feature->eval( styleExprCopy, &context );
                            if (!styleString.empty() && styleString != "null")
                            {
                                // resolve the style:
                                Style combinedStyle;

                                // if the style string begins with an open bracket, it's an inline style definition.
                                if ( styleString.length() > 0 && styleString[0] == '{' )
                                {
                                    Config conf( "style", styleString );
                                    conf.setReferrer( sel.styleExpression().get().uriContext().referrer() );
                                    conf.set( "type", "text/css" );
                                    combinedStyle = Style(conf);
                                }

                                // otherwise, look up the style in the stylesheet. Do NOT fall back on a default
                                // style in this case: for style expressions, the user must be explicity about 
                                // default styling; this is because there is no other way to exclude unwanted
                                // features.
                                else
                                {
                                    const Style* selectedStyle = session->styles()->getStyle(styleString, false);
                                    if ( selectedStyle )
                                        combinedStyle = *selectedStyle;
                                }

                                if (!combinedStyle.empty())
                                {
                                    FeatureList list;
                                    list.push_back( feature );

                                    renderFeaturesForStyle(
                                        session,
                                        combinedStyle,
                                        list,
                                        key.getExtent(),
                                        target);
                                }
                            }
                        }
                    }                    
                }
                else
                {
                    const Style* style = styles->getStyle( sel.getSelectedStyleName() );
                    Query query = sel.query().get();
                    query.tileKey() = key;
                    queryAndRenderFeaturesForStyle(session, *style, query, key.getExtent(), target, progress);
                }
            }
        }
        else
        {
            const Style* style = styles->getDefaultStyle();
            queryAndRenderFeaturesForStyle(session, *style, defaultQuery, key.getExtent(), target, progress);
        }
    }
    else
    {
        queryAndRenderFeaturesForStyle(session, Style(), defaultQuery, key.getExtent(), target, progress);
    }

    return true;
}


bool
FeatureImageRenderer::queryAndRenderFeaturesForStyle(Session*          session,
                                                     const Style&      style,
                                                     const Query&      query,
                                                     const GeoExtent&  imageExtent,
                                                     osg::Image*       out_image,
                                                     ProgressCallback* progress) const
{   
    // Get the features
    FeatureList features;
    getFeatures(session, query, imageExtent, features, progress);

    if (progress && progress->isCanceled())
        return false;

    if (!features.empty())
    {
        // Render them.
        return renderFeaturesForStyle(session, style, features, imageExtent, out_image );
    }
    return false;
}

void
FeatureImageRenderer::getFeatures(Session* session,
                                  const Query& query,
                                  const GeoExtent& imageExtent,
                                  FeatureList& features,
                                  ProgressCallback* progress) const
{
    // first we need the overall extent of the layer:
    const GeoExtent& featuresExtent = session->getFeatureSource()->getFeatureProfile()->getExtent();
    
    // convert them both to WGS84, intersect the extents, and convert back.
    GeoExtent featuresExtentWGS84 = featuresExtent.transform( featuresExtent.getSRS()->getGeographicSRS() );
    GeoExtent imageExtentWGS84 = imageExtent.transform( featuresExtent.getSRS()->getGeographicSRS() );
    GeoExtent queryExtentWGS84 = featuresExtentWGS84.intersectionSameSRS( imageExtentWGS84 );
    if ( queryExtentWGS84.isValid() )
    {
        GeoExtent queryExtent = queryExtentWGS84.transform( featuresExtent.getSRS() );

        // incorporate the image extent into the feature query for this style:
        Query localQuery = query;
        localQuery.bounds() = 
            query.bounds().isSet() ? query.bounds()->unionWith( queryExtent.bounds() ) :
            queryExtent.bounds();

        // now copy the resulting feature set into a list, converting the data
        // types along the way if a geometry override is in place:
        while (features.empty())
        {
            // query the feature source:
            osg::ref_ptr<FeatureCursor> cursor = session->getFeatureSource()->createFeatureCursor(localQuery, progress);

            while( cursor.valid() && cursor->hasMore() )
            {
                Feature* feature = cursor->nextFeature();
                Geometry* geom = feature->getGeometry();
#if 0
                if ( geom )
                {
                    // apply a type override if requested:
                    if (_options.geometryTypeOverride().isSet() &&
                        _options.geometryTypeOverride() != geom->getComponentType() )
                    {
                        geom = geom->cloneAs( _options.geometryTypeOverride().value() );
                        if ( geom )
                            feature->setGeometry( geom );
                    }
                }
#endif
                if ( geom )
                {
                    features.push_back( feature );
                }
            }

            // If we didn't get any features and we have a tilekey set, try falling back.
            if (features.empty() && localQuery.tileKey().isSet())
            {
                localQuery.tileKey() = localQuery.tileKey().get().createParentKey();
                if (!localQuery.tileKey()->valid())
                {
                    // We fell back all the way to lod 0 and got nothing, so bail.
                    break;
                }
            }
            else
            {
                // Just bail, we didn't get any features and aren't using tilekeys
                break;
            }
        }
    }
}

