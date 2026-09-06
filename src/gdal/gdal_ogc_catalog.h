/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#ifndef OPENORIENTEERING_GDAL_OGC_CATALOG_H
#define OPENORIENTEERING_GDAL_OGC_CATALOG_H

#include <QString>
#include <QVector>

#include "gdal/gdal_ogc_connection.h"

namespace OpenOrienteering {

struct GdalOgcSource
{
	QString dataset_name;
	QString description;
	QString title;
	QString layer_name;
	QString crs;
	QString format;
	QString style;
	QString tile_matrix_set;
};

struct GdalOgcCatalogResult
{
	GdalOgcConnection::ServiceType detected_type = GdalOgcConnection::ServiceType::Auto;
	QVector<GdalOgcSource> sources;
	QString error;

	bool ok() const { return !sources.isEmpty(); }
};

class GdalOgcCatalog
{
public:
	static GdalOgcCatalogResult discover(const GdalOgcConnection& connection);
	static GdalOgcSource describeSource(const GdalOgcConnection& connection,
	                                   const QString& dataset_name,
	                                   const QString& description);
};

}  // namespace OpenOrienteering

#endif