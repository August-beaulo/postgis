/**********************************************************************
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.net
 *
 * PostGIS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * PostGIS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PostGIS.  If not, see <http://www.gnu.org/licenses/>.
 *
 **********************************************************************
 *
 * Copyright 2009-2011 Olivier Courtin <olivier.courtin@oslandia.com>
 * Modifications Copyright (c) 2017 - Present Pivotal Software, Inc. All Rights Reserved.
 *
 **********************************************************************/



/** @file
 *  Commons functions for all export functions
 */

#include "float.h" /* for DBL_DIG */

#include "postgres.h"
#include "catalog/pg_type.h" /* for CSTRINGOID */
#include "executor/spi.h"
#include "utils/builtins.h"

#include "../postgis_config.h"
#include "lwgeom_pg.h"
#include "liblwgeom.h"
#include "lwgeom_export.h"
#include "libsrid.h"

Datum LWGEOM_asGML(PG_FUNCTION_ARGS);
Datum LWGEOM_asKML(PG_FUNCTION_ARGS);
Datum LWGEOM_asGeoJson(PG_FUNCTION_ARGS);
Datum LWGEOM_asGeoJson_old(PG_FUNCTION_ARGS);
Datum LWGEOM_asSVG(PG_FUNCTION_ARGS);
Datum LWGEOM_asX3D(PG_FUNCTION_ARGS);
Datum LWGEOM_asEncodedPolyline(PG_FUNCTION_ARGS);

/*
 * Retrieve an SRS from a given SRID
 * Require valid spatial_ref_sys table entry
 *
 * Could return SRS as short one (i.e EPSG:4326)
 * or as long one: (i.e urn:ogc:def:crs:EPSG::4326)
 */
char * getSRSbySRID(int srid, bool short_crs)
{
	char query[256];
	char *srs, *srscopy;
	int size, err;

	if (SPI_OK_CONNECT != SPI_connect ())
	{
		elog(NOTICE, "getSRSbySRID: could not connect to SPI manager");
		SPI_finish();
		return NULL;
	}

    /* In GPDB, we don't support the SRID retrieving from spatial_ref_sys,
     *  otherwise we will meet the known issue: cannot access relation from segments.
     *  so we search static hash table firstly to by-pass this issue issue.
     */
	if (getSRSbySRIDbyRule(srid, short_crs, query) != NULL)
	{
		size = strlen(query) + 1;
		srscopy = SPI_palloc(size);
		memcpy(srscopy, query, size);
		SPI_finish();
		return srscopy;
	}

	if (short_crs)
		snprintf(query, 256, "SELECT auth_name||':'||auth_srid \
		        FROM spatial_ref_sys WHERE srid='%d'", srid);
	else
		snprintf(query, 256, "SELECT 'urn:ogc:def:crs:'||auth_name||'::'||auth_srid \
		        FROM spatial_ref_sys WHERE srid='%d'", srid);

	err = SPI_exec(query, 1);
	if ( err < 0 )
	{
		elog(NOTICE, "getSRSbySRID: error executing query %d", err);
		SPI_finish();
		return NULL;
	}

	/* no entry in spatial_ref_sys */
	if (SPI_processed <= 0)
	{
		SPI_finish();
		return NULL;
	}

	/* get result  */
	srs = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);

	/* NULL result */
	if ( ! srs )
	{
		SPI_finish();
		return NULL;
	}

	/* copy result to upper executor context */
	size = strlen(srs)+1;
	srscopy = SPI_palloc(size);
	memcpy(srscopy, srs, size);

	/* disconnect from SPI */
	SPI_finish();

	return srscopy;
}


/*
* Retrieve an SRID from a given SRS
* Require valid spatial_ref_sys table entry
*
*/
int getSRIDbySRS(const char* srs)
{
	char *query =
	    "SELECT srid "
	    "FROM spatial_ref_sys, "
	    "regexp_matches($1::text, E'([a-z]+):([0-9]+)', 'gi') AS re "
	    "WHERE re[1] ILIKE auth_name AND int4(re[2]) = auth_srid";
	Oid argtypes[] = {CSTRINGOID};
	Datum values[] = {CStringGetDatum(srs)};
	int32_t srid, err;

	srid = getSRIDbySRSbyRule(srs);
    if (srid != 0) {
        return srid;
    }
	if (!srs) return 0;

	if (SPI_OK_CONNECT != SPI_connect())
	{
		elog(NOTICE, "getSRIDbySRS: could not connect to SPI manager");
		return 0;
	}

	err = SPI_execute_with_args(query, 1, argtypes, values, NULL, true, 1);
	if (err < 0)
	{
		elog(NOTICE, "getSRIDbySRS: error executing query %d", err);
		SPI_finish();
		return 0;
	}

	/* no entry in spatial_ref_sys */
	if (SPI_processed <= 0)
	{
		query =
		    "SELECT srid "
		    "FROM spatial_ref_sys, "
		    "regexp_matches($1::text, E'urn:ogc:def:crs:([a-z]+):.*:([0-9]+)', 'gi') AS re "
		    "WHERE re[1] ILIKE auth_name AND int4(re[2]) = auth_srid";

		err = SPI_execute_with_args(query, 1, argtypes, values, NULL, true, 1);
		if (err < 0)
		{
			elog(NOTICE, "getSRIDbySRS: error executing query %d", err);
			SPI_finish();
			return 0;
		}

		if (SPI_processed <= 0)
		{
			SPI_finish();
			return 0;
		}
	}

	srid = atoi(SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
	SPI_finish();

	return srid;
}


/**
 * Encode feature in GML
 */
PG_FUNCTION_INFO_V1(LWGEOM_asGML);
Datum LWGEOM_asGML(PG_FUNCTION_ARGS)
{
	GSERIALIZED *geom;
	LWGEOM *lwgeom;
	char *gml = NULL;
	text *result;
	int version;
	char *srs;
	int srid;
	int option = 0;
	int lwopts = LW_GML_IS_DIMS;
	int precision = DBL_DIG;
	static const char* default_prefix = "gml:"; /* default prefix */
	const char* prefix = default_prefix;
	const char* gml_id = NULL;
	size_t len;
	char *gml_id_buf, *prefix_buf;
	text *prefix_text, *gml_id_text;


	/* Get the version */
	version = PG_GETARG_INT32(0);
	if ( version != 2 && version != 3 )
	{
		elog(ERROR, "Only GML 2 and GML 3 are supported");
		PG_RETURN_NULL();
	}

	/* Get the geometry */
	if ( PG_ARGISNULL(1) ) PG_RETURN_NULL();
	geom = PG_GETARG_GSERIALIZED_P(1);

	/* Retrieve precision if any (default is max) */
	if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
	{
		precision = PG_GETARG_INT32(2);
		/* TODO: leave this to liblwgeom ? */
		if (precision > DBL_DIG)
			precision = DBL_DIG;
		else if (precision < 0)
			precision = 0;
	}

	/* retrieve option */
	if (PG_NARGS() > 3 && !PG_ARGISNULL(3)) option = PG_GETARG_INT32(3);

	/* retrieve prefix */
	if (PG_NARGS() >4 && !PG_ARGISNULL(4))
	{
		prefix_text = PG_GETARG_TEXT_P(4);
		if ( VARSIZE(prefix_text) == VARHDRSZ )
		{
			prefix = "";
		}
		else
		{
			len = VARSIZE(prefix_text)-VARHDRSZ;
			prefix_buf = palloc(len + 2); /* +2 is one for the ':' and one for term null */
			memcpy(prefix_buf, VARDATA(prefix_text), len);
			/* add colon and null terminate */
			prefix_buf[len] = ':';
			prefix_buf[len+1] = '\0';
			prefix = prefix_buf;
		}
	}

	if (PG_NARGS() >5 && !PG_ARGISNULL(5))
	{
		gml_id_text = PG_GETARG_TEXT_P(5);
		if ( VARSIZE(gml_id_text) == VARHDRSZ )
		{
			gml_id = "";
		}
		else
		{
			len = VARSIZE(gml_id_text)-VARHDRSZ;
			gml_id_buf = palloc(len+1);
			memcpy(gml_id_buf, VARDATA(gml_id_text), len);
			gml_id_buf[len] = '\0';
			gml_id = gml_id_buf;
		}
	}

	srid = gserialized_get_srid(geom);
	if (srid == SRID_UNKNOWN)      srs = NULL;
	else if (option & 1) srs = getSRSbySRID(srid, false);
	else                 srs = getSRSbySRID(srid, true);

	if (option & 2) lwopts &= ~LW_GML_IS_DIMS;
	if (option & 4) lwopts |= LW_GML_SHORTLINE;
	if (option & 8)
	{
		elog(ERROR,
		     "Options %d passed to ST_AsGML(geography) sets "
		     "unsupported value 8",
		     option);
		PG_RETURN_NULL();
	}
	if (option & 16) lwopts |= LW_GML_IS_DEGREE;
	if (option & 32) lwopts |= LW_GML_EXTENT;

	lwgeom = lwgeom_from_gserialized(geom);

	if (version == 2)
	{
		if (lwopts & LW_GML_EXTENT)
			gml = lwgeom_extent_to_gml2(
			    lwgeom, srs, precision, prefix);
		else
			gml = lwgeom_to_gml2(lwgeom, srs, precision, prefix);
	}
	if (version == 3)
	{
		if (lwopts & LW_GML_EXTENT)
			gml = lwgeom_extent_to_gml3(
			    lwgeom, srs, precision, lwopts, prefix);
		else
			gml = lwgeom_to_gml3(
			    lwgeom, srs, precision, lwopts, prefix, gml_id);
	}

	lwgeom_free(lwgeom);
	PG_FREE_IF_COPY(geom, 1);

	/* Return null on null */
	if ( ! gml )
		PG_RETURN_NULL();

	result = cstring_to_text(gml);
	lwfree(gml);
	PG_RETURN_TEXT_P(result);
}


/**
 * Encode feature in KML
 */
PG_FUNCTION_INFO_V1(LWGEOM_asKML);
Datum LWGEOM_asKML(PG_FUNCTION_ARGS)
{
	GSERIALIZED *geom;
	LWGEOM *lwgeom;
	char *kml;
	text *result;
	int version;
	int precision = DBL_DIG;
	static const char* default_prefix = ""; /* default prefix */
	char *prefixbuf;
	const char* prefix = default_prefix;
	text *prefix_text;


	/* Get the version */
	version = PG_GETARG_INT32(0);
	if ( version != 2)
	{
		elog(ERROR, "Only KML 2 is supported");
		PG_RETURN_NULL();
	}

	/* Get the geometry */
	if ( PG_ARGISNULL(1) ) PG_RETURN_NULL();
	geom = PG_GETARG_GSERIALIZED_P(1);

	/* Retrieve precision if any (default is max) */
	if (PG_NARGS() >2 && !PG_ARGISNULL(2))
	{
		/* TODO: leave this to liblwgeom ? */
		precision = PG_GETARG_INT32(2);
		if ( precision > DBL_DIG )
			precision = DBL_DIG;
		else if ( precision < 0 ) precision = 0;
	}

	/* retrieve prefix */
	if (PG_NARGS() >3 && !PG_ARGISNULL(3))
	{
		prefix_text = PG_GETARG_TEXT_P(3);
		if ( VARSIZE(prefix_text)-VARHDRSZ == 0 )
		{
			prefix = "";
		}
		else
		{
			/* +2 is one for the ':' and one for term null */
			prefixbuf = palloc(VARSIZE(prefix_text)-VARHDRSZ+2);
			memcpy(prefixbuf, VARDATA(prefix_text),
			       VARSIZE(prefix_text)-VARHDRSZ);
			/* add colon and null terminate */
			prefixbuf[VARSIZE(prefix_text)-VARHDRSZ] = ':';
			prefixbuf[VARSIZE(prefix_text)-VARHDRSZ+1] = '\0';
			prefix = prefixbuf;
		}
	}

	lwgeom = lwgeom_from_gserialized(geom);
	kml = lwgeom_to_kml2(lwgeom, precision, prefix);
	lwgeom_free(lwgeom);
	PG_FREE_IF_COPY(geom, 1);

	if( ! kml )
		PG_RETURN_NULL();

	result = cstring_to_text(kml);
	lwfree(kml);

	PG_RETURN_POINTER(result);
}


/**
 * Encode Feature in GeoJson (Old C Signature)
 * ST_AsGeoJSON(version, geom, precision, options)
 * why was this written with a version param when there
 * is only one version?
 */
PG_FUNCTION_INFO_V1(LWGEOM_asGeoJson_old);
Datum LWGEOM_asGeoJson_old(PG_FUNCTION_ARGS)
{
	switch( PG_NARGS() )
	{
	case 2:
		return DirectFunctionCall1(LWGEOM_asGeoJson, PG_GETARG_DATUM(1));
	case 3:
		return DirectFunctionCall2(LWGEOM_asGeoJson, PG_GETARG_DATUM(1), PG_GETARG_DATUM(2));
	case 4:
		return DirectFunctionCall3(LWGEOM_asGeoJson, PG_GETARG_DATUM(1), PG_GETARG_DATUM(2), PG_GETARG_DATUM(3));
	default:
		elog(ERROR, "bad call in %s", __func__);
	}
	PG_RETURN_NULL();
}

/**
 * Encode Feature in GeoJson
 */
PG_FUNCTION_INFO_V1(LWGEOM_asGeoJson);
Datum LWGEOM_asGeoJson(PG_FUNCTION_ARGS)
{
	GSERIALIZED *geom;
	LWGEOM *lwgeom;
	char *geojson;
	text *result;
	int has_bbox = 0;
	int precision = DBL_DIG;
	char *srs = NULL;

	/* Get the geometry */
	if ( PG_ARGISNULL(0) )
		PG_RETURN_NULL();

	geom = PG_GETARG_GSERIALIZED_P(0);

	/* Retrieve precision if any (default is max) */
	if ( PG_NARGS() > 1 && !PG_ARGISNULL(1) )
	{
		precision = PG_GETARG_INT32(1);
		if ( precision > DBL_DIG )
			precision = DBL_DIG;
		else if ( precision < 0 )
			precision = 0;
	}

	/* Retrieve output option
	 * 0 = without option (default)
	 * 1 = bbox
	 * 2 = short crs
	 * 4 = long crs
	 */
	if ( PG_NARGS() > 2 && !PG_ARGISNULL(2) )
	{
		int option = PG_GETARG_INT32(2);

		if ( option & 2 || option & 4 )
		{
			int srid = gserialized_get_srid(geom);
			if ( srid != SRID_UNKNOWN )
			{
				if ( option & 2 )
					srs = getSRSbySRID(srid, true);

				if ( option & 4 )
					srs = getSRSbySRID(srid, false);

				if ( !srs )
				{
					elog(ERROR,
					      "SRID %i unknown in spatial_ref_sys table",
					      srid);
					PG_RETURN_NULL();
				}
			}
		}

		if (option & 1)
			has_bbox = 1;
	}

	lwgeom = lwgeom_from_gserialized(geom);
	geojson = lwgeom_to_geojson(lwgeom, srs, precision, has_bbox);
	lwgeom_free(lwgeom);

	if (srs) pfree(srs);

	result = cstring_to_text(geojson);
	lwfree(geojson);

	PG_FREE_IF_COPY(geom, 0);
	PG_RETURN_TEXT_P(result);
}


/**
 * SVG features
 */
PG_FUNCTION_INFO_V1(LWGEOM_asSVG);
Datum LWGEOM_asSVG(PG_FUNCTION_ARGS)
{
	GSERIALIZED *geom;
	LWGEOM *lwgeom;
	char *svg;
	text *result;
	int relative = 0;
	int precision=DBL_DIG;

	if ( PG_ARGISNULL(0) ) PG_RETURN_NULL();

	geom = PG_GETARG_GSERIALIZED_P(0);

	/* check for relative path notation */
	if ( PG_NARGS() > 1 && ! PG_ARGISNULL(1) )
		relative = PG_GETARG_INT32(1) ? 1:0;

	if ( PG_NARGS() > 2 && ! PG_ARGISNULL(2) )
	{
		precision = PG_GETARG_INT32(2);
		/* TODO: leave this to liblwgeom ? */
		if ( precision > DBL_DIG )
			precision = DBL_DIG;
		else if ( precision < 0 ) precision = 0;
	}

	lwgeom = lwgeom_from_gserialized(geom);
	svg = lwgeom_to_svg(lwgeom, precision, relative);
	result = cstring_to_text(svg);
	lwgeom_free(lwgeom);
	pfree(svg);
	PG_FREE_IF_COPY(geom, 0);

	PG_RETURN_TEXT_P(result);
}

/**
 * Encode feature as X3D
 */
PG_FUNCTION_INFO_V1(LWGEOM_asX3D);
Datum LWGEOM_asX3D(PG_FUNCTION_ARGS)
{
	GSERIALIZED *geom;
	LWGEOM *lwgeom;
	char *x3d;
	text *result;
	int version;
	char *srs;
	int srid;
	int option = 0;
	int precision = DBL_DIG;
	static const char* default_defid = "x3d:"; /* default defid */
	char *defidbuf;
	const char* defid = default_defid;
	text *defid_text;

	/* Get the version */
	version = PG_GETARG_INT32(0);
	if (  version != 3 )
	{
		elog(ERROR, "Only X3D version 3 are supported");
		PG_RETURN_NULL();
	}

	/* Get the geometry */
	if ( PG_ARGISNULL(1) ) PG_RETURN_NULL();
	geom = PG_GETARG_GSERIALIZED_P(1);

	/* Retrieve precision if any (default is max) */
	if (PG_NARGS() >2 && !PG_ARGISNULL(2))
	{
		precision = PG_GETARG_INT32(2);
		/* TODO: leave this to liblwgeom ? */
		if ( precision > DBL_DIG )
			precision = DBL_DIG;
		else if ( precision < 0 ) precision = 0;
	}

	/* retrieve option */
	if (PG_NARGS() >3 && !PG_ARGISNULL(3))
		option = PG_GETARG_INT32(3);



	/* retrieve defid */
	if (PG_NARGS() >4 && !PG_ARGISNULL(4))
	{
		defid_text = PG_GETARG_TEXT_P(4);
		if ( VARSIZE(defid_text)-VARHDRSZ == 0 )
		{
			defid = "";
		}
		else
		{
			/* +2 is one for the ':' and one for term null */
			defidbuf = palloc(VARSIZE(defid_text)-VARHDRSZ+2);
			memcpy(defidbuf, VARDATA(defid_text),
			       VARSIZE(defid_text)-VARHDRSZ);
			/* add colon and null terminate */
			defidbuf[VARSIZE(defid_text)-VARHDRSZ] = ':';
			defidbuf[VARSIZE(defid_text)-VARHDRSZ+1] = '\0';
			defid = defidbuf;
		}
	}

	lwgeom = lwgeom_from_gserialized(geom);
	srid = gserialized_get_srid(geom);
	if (srid == SRID_UNKNOWN)      srs = NULL;
	else if (option & 1) srs = getSRSbySRID(srid, false);
	else                 srs = getSRSbySRID(srid, true);

	if (option & LW_X3D_USE_GEOCOORDS) {
		if (srid != 4326) {
			PG_FREE_IF_COPY(geom, 0);
			/** TODO: we need to support UTM and other coordinate systems supported by X3D eventually
			http://www.web3d.org/documents/specifications/19775-1/V3.2/Part01/components/geodata.html#t-earthgeoids **/
			elog(ERROR, "Only SRID 4326 is supported for geocoordinates.");
			PG_RETURN_NULL();
		}
	}


	x3d = lwgeom_to_x3d3(lwgeom, srs, precision,option, defid);

	lwgeom_free(lwgeom);
	PG_FREE_IF_COPY(geom, 1);

	result = cstring_to_text(x3d);
	lwfree(x3d);

	PG_RETURN_TEXT_P(result);
}

/**
 * Encode feature as Encoded Polyline
 */
PG_FUNCTION_INFO_V1(LWGEOM_asEncodedPolyline);
Datum LWGEOM_asEncodedPolyline(PG_FUNCTION_ARGS)
{
	GSERIALIZED *geom;
	LWGEOM *lwgeom;
	char *encodedpolyline;
	int precision = 5;
	text *result;

	if ( PG_ARGISNULL(0) ) PG_RETURN_NULL();

	geom = PG_GETARG_GSERIALIZED_P(0);
	if (gserialized_get_srid(geom) != 4326) {
		PG_FREE_IF_COPY(geom, 0);
		elog(ERROR, "Only SRID 4326 is supported.");
		PG_RETURN_NULL();
	}
	lwgeom = lwgeom_from_gserialized(geom);

	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
	{
		precision = PG_GETARG_INT32(1);
		if ( precision < 0 ) precision = 5;
	}

	encodedpolyline = lwgeom_to_encoded_polyline(lwgeom, precision);
	lwgeom_free(lwgeom);
	PG_FREE_IF_COPY(geom, 0);

	result = cstring_to_text(encodedpolyline);
	lwfree(encodedpolyline);

	PG_RETURN_TEXT_P(result);
}
