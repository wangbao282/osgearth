/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
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
#include <osgEarthFeatures2/FeatureTileSource>
#include <osgEarthFeatures2/Styling>
#include <osgEarth/Registry>
#include <osg/Notify>

using namespace osgEarth;
using namespace osgEarth::Features2;
using namespace osgEarth::Symbology;

/*************************************************************************/

FeatureTileSourceOptions::FeatureTileSourceOptions( const PluginOptions* opt ) :
TileSourceOptions( opt ),
_geomTypeOverride( Geometry::TYPE_UNKNOWN )
{
    if ( config().hasChild("features") )
        _featureOptions = new FeatureSourceOptions( new PluginOptions( config().child("features") ) );

    config().getObjIfSet( "styles", _styles );
    
    std::string gt = config().value( "geometry_type" );
    if ( gt == "line" || gt == "lines" || gt == "linestring" )
        _geomTypeOverride = Geometry::TYPE_LINESTRING;
    else if ( gt == "point" || gt == "pointset" || gt == "points" )
        _geomTypeOverride = Geometry::TYPE_POINTSET;
    else if ( gt == "polygon" || gt == "polygons" )
        _geomTypeOverride = Geometry::TYPE_POLYGON;
}

Config
FeatureTileSourceOptions::toConfig() const
{
    Config conf = TileSourceOptions::toConfig();

    conf.updateObjIfSet( "features", _featureOptions );
    conf.updateObjIfSet( "styles", _styles );

    if ( _geomTypeOverride.isSet() ) {
        if ( _geomTypeOverride == Geometry::TYPE_LINESTRING )
            conf.update( "geometry_type", "line" );
        else if ( _geomTypeOverride == Geometry::TYPE_POINTSET )
            conf.update( "geometry_type", "point" );
        else if ( _geomTypeOverride == Geometry::TYPE_POLYGON )
            conf.update( "geometry_type", "polygon" );
    }

    return conf;
}

/*************************************************************************/

FeatureTileSource::FeatureTileSource( const PluginOptions* options ) :
TileSource( options )
{
    _options = dynamic_cast<const FeatureTileSourceOptions*>( options );
    if ( !_options.valid() )
        _options = new FeatureTileSourceOptions( options );

    _features = FeatureSourceFactory::create( _options->featureOptions() );
    if ( !_features.valid() )
    {
        OE_WARN << "FeatureTileSource - no valid feature source provided" << std::endl;
    }
}

void 
FeatureTileSource::initialize( const std::string& referenceURI, const Profile* overrideProfile)
{
    if (overrideProfile)
    {
        //If we were given a profile, take it on.
        setProfile(overrideProfile);
    }
    else
    {
        //Assume it is global-geodetic
        setProfile( osgEarth::Registry::instance()->getGlobalGeodeticProfile() );
    }            

    if ( _features.valid() )
        _features->initialize( referenceURI );
}

osg::Image*
FeatureTileSource::createImage( const TileKey* key, ProgressCallback* progress )
{
    if ( !_features.valid() || !_features->getFeatureProfile() )
        return 0L;

    // style data
    const StyleCatalog* styles = _options->styles();

    // implementation-specific data
    osg::ref_ptr<osg::Referenced> buildData = createBuildData();

	// allocate the image.
	osg::ref_ptr<osg::Image> image = new osg::Image();
	image->allocateImage( getPixelsPerTile(), getPixelsPerTile(), 1, GL_RGBA, GL_UNSIGNED_BYTE );

    preProcess( image.get(), buildData.get() );

    // figure out if and how to style the geometry.
    if ( _features->hasEmbeddedStyles() )
    {
        // Each feature has its own embedded style data, so use that:
        osg::ref_ptr<FeatureCursor> cursor = _features->createFeatureCursor( Query() );
        while( cursor->hasMore() )
        {
            Feature* feature = cursor->nextFeature();
            if ( feature )
            {
                FeatureList list;
                list.push_back( feature );
                renderFeatures2ForStyle( 
					feature->style().get(), list, buildData.get(),
					key->getGeoExtent(), image.get() );
            }
        }
    }
    else if ( styles )
    {
        if ( styles->selectors().size() > 0 )
        {
            for( StyleSelectorList::const_iterator i = styles->selectors().begin(); i != styles->selectors().end(); ++i )
            {
                const StyleSelector& sel = *i;
                Style style;
                styles->getStyle( sel.getSelectedStyleName(), style );
                queryAndRenderFeatures2ForStyle( style, sel.query().value(), buildData.get(), key->getGeoExtent(), image.get() );
            }
        }
        else
        {
            Style style = styles->getDefaultStyle();
            queryAndRenderFeatures2ForStyle( style, Query(), buildData.get(), key->getGeoExtent(), image.get() );
        }
    }
    else
    {
		queryAndRenderFeatures2ForStyle( Style(), Query(), buildData.get(), key->getGeoExtent(), image.get() );
    }

    // final tile processing after all styles are done
    postProcess( image.get(), buildData.get() );

	return image.release();
}


bool
FeatureTileSource::queryAndRenderFeatures2ForStyle(const Style& style,
                                                  const Query& query,
												  osg::Referenced* data,
												  const GeoExtent& imageExtent,
												  osg::Image* out_image)
{
    osg::Group* styleGroup = 0L;

    // first we need the overall extent of the layer:
    const GeoExtent& featuresExtent = getFeatureSource()->getFeatureProfile()->getExtent();
    
    // convert them both to WGS84, intersect the extents, and convert back.
    GeoExtent featuresExtentWGS84 = featuresExtent.transform( featuresExtent.getSRS()->getGeographicSRS() );
    GeoExtent imageExtentWGS84 = imageExtent.transform( featuresExtent.getSRS()->getGeographicSRS() );
    GeoExtent queryExtentWGS84 = featuresExtentWGS84.intersectionSameSRS( imageExtentWGS84 );
    if ( queryExtentWGS84.isValid() )
    {
        GeoExtent queryExtent = queryExtentWGS84.transform( featuresExtent.getSRS() );

	    // incorporate the image extent into the feature query for this style:
        Query localQuery = query;
        localQuery.bounds() = query.bounds().isSet()?
		    query.bounds()->unionWith( queryExtent.bounds() ) :
		    queryExtent.bounds();

        // query the feature source:
        osg::ref_ptr<FeatureCursor> cursor = _features->createFeatureCursor( localQuery );

        // now copy the resulting feature set into a list, converting the data
        // types along the way if a geometry override is in place:
        FeatureList cellFeatures2;
        while( cursor->hasMore() )
        {
            Feature* feature = cursor->nextFeature();
            Geometry* geom = feature->getGeometry();
            if ( geom )
            {
                // apply a type override if requested:
                if (_options->geometryTypeOverride().isSet() &&
                    _options->geometryTypeOverride() != geom->getComponentType() )
                {
                    geom = geom->cloneAs( _options->geometryTypeOverride().value() );
                    if ( geom )
                        feature->setGeometry( geom );
                }
            }
            if ( geom )
            {
                cellFeatures2.push_back( feature );
            }
        }

        //OE_NOTICE
        //    << "Rendering "
        //    << cellFeatures2.size()
        //    << " features in ("
        //    << queryExtent.toString() << ")"
        //    << std::endl;

	    return renderFeatures2ForStyle( style, cellFeatures2, data, imageExtent, out_image );
    }
    else
    {
        return false;
    }
}

