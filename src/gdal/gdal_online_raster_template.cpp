/*
 * Copyright 2026 The OpenOrienteering developers
 *
 * This file is part of OpenOrienteering.
 * OpenOrienteering is free software under GNU GPL version 3 or later.
 */

#include "gdal/gdal_online_raster_template.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <limits>
#include <utility>

#include <Qt>
#include <QtGlobal>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPaintDevice>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QVector>
#include <QXmlStreamAttributes>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <gdalwarper.h>
#include <ogr_srs_api.h>

#include "core/georeferencing.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "gdal/gdal_image_reader.h"
#include "gdal/gdal_manager.h"
#include "util/transformation.h"

namespace OpenOrienteering {

struct GdalOnlineDatasetHolder
{
	GdalOnlineDatasetHolder(GDALDatasetH source_dataset, GDALDatasetH render_dataset)
	: source_dataset(source_dataset)
	, render_dataset(render_dataset ? render_dataset : source_dataset)
	{}

	~GdalOnlineDatasetHolder()
	{
		if (render_dataset && render_dataset != source_dataset)
			GDALClose(render_dataset);
		if (source_dataset)
			GDALClose(source_dataset);
	}

	GDALDatasetH source_dataset = nullptr;
	GDALDatasetH render_dataset = nullptr;
	QMutex mutex;
};

struct GdalOnlinePreparedData
{
	std::shared_ptr<GdalOnlineDatasetHolder> dataset;
	int raster_width = 0;
	int raster_height = 0;
	QString source_crs_spec;
	QString render_crs_spec;
	QPointF projected_center;
	bool has_projected_center = false;
	QTransform pixel_to_world;
	QTransform world_to_pixel;
};

namespace {

struct RasterLayout
{
	QVector<int> bands;
	QImage::Format format = QImage::Format_Invalid;
	int pixel_space = 1;
	int band_space = 1;
	int band_offset = 0;
	bool premultiply_argb = false;
	bool expand_gray_alpha = false;
	QVector<QRgb> color_table;
};

struct RenderResult
{
	quint64 serial = 0;
	QRect source_window;
	QSize requested_size;
	QImage image;
	QString error;
};

QString trOnline(const char* source_text)
{
	return QCoreApplication::translate("OpenOrienteering::GdalOnlineRasterTemplate", source_text);
}

QString toWkt(OGRSpatialReferenceH srs)
{
	QString wkt;
	char* wkt_cstring = nullptr;
	if (srs && OSRExportToWkt(srs, &wkt_cstring) == OGRERR_NONE)
		wkt = QString::fromUtf8(wkt_cstring);
	CPLFree(wkt_cstring);
	return wkt;
}

QString datasetWkt(GDALDatasetH dataset)
{
	if (!dataset)
		return {};
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 0, 0) && !defined(ACCEPT_USE_OF_DEPRECATED_PROJ_API_H)
	return toWkt(GDALGetSpatialRef(dataset));
#else
	const char* projection = GDALGetProjectionRef(dataset);
	return projection ? QString::fromUtf8(projection) : QString{};
#endif
}

class SpatialReference final
{
public:
	SpatialReference()
	: handle(OSRNewSpatialReference(nullptr))
	{}

	~SpatialReference()
	{
		if (handle)
			OSRDestroySpatialReference(handle);
	}

	SpatialReference(const SpatialReference&) = delete;
	SpatialReference& operator=(const SpatialReference&) = delete;

	OGRSpatialReferenceH get() const { return handle; }

private:
	OGRSpatialReferenceH handle = nullptr;
};

bool setFromUserInput(OGRSpatialReferenceH srs, const QString& specification)
{
	if (!srs || specification.trimmed().isEmpty())
		return false;
	const QByteArray utf8 = specification.toUtf8();
	if (OSRSetFromUserInput(srs, utf8.constData()) != OGRERR_NONE)
		return false;
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 0, 0)
	OSRSetAxisMappingStrategy(srs, OAMS_TRADITIONAL_GIS_ORDER);
#endif
	return true;
}

int findBand(GDALDatasetH dataset, GDALColorInterp interpretation)
{
	const int count = GDALGetRasterCount(dataset);
	for (int i = count; i > 0; --i)
	{
		GDALRasterBandH band = GDALGetRasterBand(dataset, i);
		if (GDALGetRasterDataType(band) == GDT_Byte
	    && GDALGetRasterColorInterpretation(band) == interpretation)
			return i;
	}
	return 0;
}

RasterLayout rasterLayout(GDALDatasetH dataset)
{
	RasterLayout layout;
	if (!dataset)
		return layout;

	const int count = GDALGetRasterCount(dataset);
	const int alpha_band = findBand(dataset, GCI_AlphaBand);

	if (count == 1)
	{
		GDALRasterBandH band = GDALGetRasterBand(dataset, 1);
		if (GDALGetRasterDataType(band) != GDT_Byte)
			return layout;

		const auto interpretation = GDALGetRasterColorInterpretation(band);
		if (interpretation == GCI_GrayIndex)
		{
			layout.format = QImage::Format_Grayscale8;
			layout.pixel_space = 1;
			layout.bands.push_back(1);
		}
		else if (interpretation == GCI_PaletteIndex)
		{
			layout.format = QImage::Format_Indexed8;
			layout.pixel_space = 1;
			layout.bands.push_back(1);
			GDALColorTableH table = GDALGetRasterColorTable(band);
			if (table)
			{
				const int colors = GDALGetColorEntryCount(table);
				layout.color_table.reserve(colors);
				for (int i = 0; i < colors; ++i)
				{
					GDALColorEntry entry {};
					GDALGetColorEntryAsRGB(table, i, &entry);
					layout.color_table.push_back(qRgba(entry.c1, entry.c2, entry.c3, entry.c4));
				}
			}
		}
		return layout;
	}

	if (count == 2 && alpha_band)
	{
		const int gray_band = 3 - alpha_band;
		GDALRasterBandH band = GDALGetRasterBand(dataset, gray_band);
		if (GDALGetRasterDataType(band) == GDT_Byte
	    && GDALGetRasterColorInterpretation(band) == GCI_GrayIndex)
		{
			layout.format = QImage::Format_ARGB32_Premultiplied;
			layout.pixel_space = 4;
			layout.band_offset = 2;
			layout.bands.push_back(gray_band);
			layout.bands.push_back(alpha_band);
			layout.expand_gray_alpha = true;
		}
		return layout;
	}

	if (count >= 3)
	{
		for (auto interpretation : { GCI_BlueBand, GCI_GreenBand, GCI_RedBand })
		{
			const int band = findBand(dataset, interpretation);
			if (band)
				layout.bands.push_back(band);
		}

		if (layout.bands.size() == 3)
		{
			layout.pixel_space = 4;
			if (alpha_band)
			{
				layout.bands.push_back(alpha_band);
				layout.format = QImage::Format_ARGB32_Premultiplied;
				layout.premultiply_argb = true;
			}
			else
			{
				layout.format = QImage::Format_RGB32;
			}
		}
	}
	return layout;
}

void premultiplyArgb(QImage& image)
{
	if (image.depth() != 32)
		return;
	auto* first = reinterpret_cast<QRgb*>(image.bits());
	auto* last = first + image.width() * image.height();
	std::transform(first, last, first, [](QRgb pixel) { return qPremultiply(pixel); });
}

void expandGrayAlpha(QImage& image)
{
	if (image.depth() != 32)
		return;
	auto* first = reinterpret_cast<QRgb*>(image.bits());
	auto* last = first + image.width() * image.height();
	std::transform(first, last, first, [](QRgb pixel) {
		const int gray = qRed(pixel);
		return qPremultiply(qRgba(gray, gray, gray, qAlpha(pixel)));
	});
}

bool readRasterWindow(GDALDatasetH dataset,
                      const QRect& source_window,
                      const QSize& output_size,
                      QImage& image,
                      QString& error)
{
	if (!dataset || source_window.isEmpty() || output_size.isEmpty())
	{
		error = trOnline("Invalid online raster render request.");
		return false;
	}

	const RasterLayout layout = rasterLayout(dataset);
	if (layout.format == QImage::Format_Invalid || layout.bands.isEmpty())
	{
		error = trOnline("Unsupported GDAL raster band layout.");
		return false;
	}

	image = QImage(output_size, layout.format);
	if (image.isNull())
	{
		error = trOnline("Not enough memory for the online raster image.");
		return false;
	}

	image.fill(layout.bands.size() == 4 ? Qt::transparent : Qt::white);
	if (!layout.color_table.isEmpty())
		image.setColorTable(layout.color_table);

	GDALRasterIOExtraArg extra;
	INIT_RASTERIO_EXTRA_ARG(extra);
	extra.eResampleAlg = GRIORA_Bilinear;

	CPLErrorReset();
	const CPLErr io_result = GDALDatasetRasterIOEx(
	        dataset,
	        GF_Read,
	        source_window.x(), source_window.y(),
	        source_window.width(), source_window.height(),
	        image.bits() + layout.band_offset,
	        output_size.width(), output_size.height(),
	        GDT_Byte,
	        layout.bands.size(), layout.bands.data(),
	        layout.pixel_space, image.bytesPerLine(), layout.band_space,
	        &extra);

	if (io_result >= CE_Warning)
	{
		error = QString::fromUtf8(CPLGetLastErrorMsg());
		if (error.isEmpty())
			error = trOnline("GDAL RasterIO failed.");
		image = {};
		return false;
	}

	if (layout.expand_gray_alpha)
		expandGrayAlpha(image);
	else if (layout.premultiply_argb)
		premultiplyArgb(image);
	return true;
}

RenderResult renderWindow(const std::shared_ptr<GdalOnlineDatasetHolder>& holder,
                          const GdalOgcConnection& connection,
                          quint64 serial,
                          const QRect& source_window,
                          const QSize& requested_size)
{
	RenderResult result;
	result.serial = serial;
	result.source_window = source_window;
	result.requested_size = requested_size;

	if (!holder || !holder->render_dataset)
	{
		result.error = trOnline("Online raster dataset is not loaded.");
		return result;
	}
	if (connection.needsSecret() && connection.effectiveSecret().isEmpty())
	{
		result.error = trOnline("Authentication credentials are unavailable.");
		return result;
	}

	GdalOgcHttpGuard http_guard(connection);
	QMutexLocker locker(&holder->mutex);
	readRasterWindow(holder->render_dataset, source_window, requested_size, result.image, result.error);
	return result;
}

class RenderTask final : public QRunnable
{
public:
	RenderTask(QPointer<GdalOnlineRasterTemplate> target,
	           std::shared_ptr<GdalOnlineDatasetHolder> dataset,
	           GdalOgcConnection connection,
	           quint64 serial,
	           QRect source_window,
	           QSize requested_size)
	: target(std::move(target))
	, dataset(std::move(dataset))
	, connection(std::move(connection))
	, serial(serial)
	, source_window(std::move(source_window))
	, requested_size(std::move(requested_size))
	{}

	void run() override
	{
		RenderResult result = renderWindow(dataset, connection, serial, source_window, requested_size);
		if (!target)
			return;

		QPointer<GdalOnlineRasterTemplate> guard = target;
		QMetaObject::invokeMethod(target.data(),
		                          [guard, result = std::move(result)]() mutable {
			if (guard)
			{
				guard->acceptRender(result.serial,
				                    result.source_window,
				                    result.requested_size,
				                    std::move(result.image),
				                    result.error);
			}
		},
		                          Qt::QueuedConnection);
	}

private:
	QPointer<GdalOnlineRasterTemplate> target;
	std::shared_ptr<GdalOnlineDatasetHolder> dataset;
	GdalOgcConnection connection;
	quint64 serial;
	QRect source_window;
	QSize requested_size;
};

class PrepareTask final : public QRunnable
{
public:
	PrepareTask(QPointer<GdalOnlineRasterTemplate> target,
	            GdalOgcConnection connection,
	            QString dataset_name,
	            QString map_crs_spec,
	            quint64 serial)
	: target(std::move(target))
	, connection(std::move(connection))
	, dataset_name(std::move(dataset_name))
	, map_crs_spec(std::move(map_crs_spec))
	, serial(serial)
	{}

	void run() override
	{
		QString error;
		auto prepared = GdalOnlineRasterTemplate::prepareDataset(
		        connection, dataset_name, map_crs_spec, error);
		if (!target)
			return;

		QPointer<GdalOnlineRasterTemplate> guard = target;
		QMetaObject::invokeMethod(target.data(),
		                          [guard, serial = serial,
		                           prepared = std::move(prepared),
		                           error = std::move(error)]() mutable {
			if (guard)
				guard->acceptPreparation(serial, std::move(prepared), error);
		},
		                          Qt::QueuedConnection);
	}

private:
	QPointer<GdalOnlineRasterTemplate> target;
	GdalOgcConnection connection;
	QString dataset_name;
	QString map_crs_spec;
	quint64 serial;
};

QSize clampRenderSize(QSize size, bool on_screen)
{
	if (size.width() < 1 || size.height() < 1)
		return {};

	const double max_dimension = on_screen ? 4096.0 : 6144.0;
	const double max_pixels = on_screen ? 12000000.0 : 24000000.0;
	double factor = 1.0;
	factor = std::min(factor, max_dimension / size.width());
	factor = std::min(factor, max_dimension / size.height());
	factor = std::min(factor, std::sqrt(max_pixels / (double(size.width()) * double(size.height()))));
	if (factor < 1.0)
	{
		size.setWidth(std::max(1, int(std::floor(size.width() * factor))));
		size.setHeight(std::max(1, int(std::floor(size.height() * factor))));
	}
	return size;
}

}  // namespace

GdalOnlineRasterTemplate::GdalOnlineRasterTemplate(const QString& dataset_name, Map* map)
: Template(dataset_name, map)
, dataset_name(dataset_name)
{
	initializeTimers();
	connect(&map->getGeoreferencing(), &Georeferencing::projectionChanged,
	        this, &GdalOnlineRasterTemplate::mapGeoreferencingChanged);
}

GdalOnlineRasterTemplate::GdalOnlineRasterTemplate(const GdalOgcConnection& connection,
                                                   const QString& dataset_name,
                                                   const QString& display_name,
                                                   GdalOnlinePreparedDataPtr prepared_data,
                                                   Map* map)
: Template(dataset_name, map)
, connection(connection)
, dataset_name(dataset_name)
, display_name(display_name)
, prepared_data(std::move(prepared_data))
{
	initializeTimers();
	if (!display_name.isEmpty())
		template_file = display_name;
	connect(&map->getGeoreferencing(), &Georeferencing::projectionChanged,
	        this, &GdalOnlineRasterTemplate::mapGeoreferencingChanged);
}

GdalOnlineRasterTemplate::GdalOnlineRasterTemplate(const GdalOnlineRasterTemplate& proto)
: Template(proto)
, connection(proto.connection)
, dataset_name(proto.dataset_name)
, display_name(proto.display_name)
, prepared_data(proto.prepared_data)
, dataset(proto.dataset)
, raster_width(proto.raster_width)
, raster_height(proto.raster_height)
, crs_spec(proto.crs_spec)
, pixel_to_world(proto.pixel_to_world)
, world_to_pixel(proto.world_to_pixel)
, map_extent(proto.map_extent)
{
	initializeTimers();
	connect(&map->getGeoreferencing(), &Georeferencing::projectionChanged,
	        this, &GdalOnlineRasterTemplate::mapGeoreferencingChanged);
}

GdalOnlineRasterTemplate::~GdalOnlineRasterTemplate() = default;

GdalOnlineRasterTemplate* GdalOnlineRasterTemplate::duplicate() const
{
	return new GdalOnlineRasterTemplate(*this);
}

const char* GdalOnlineRasterTemplate::getTemplateType() const
{
	return "GdalOnlineRasterTemplate";
}

Template::LookupResult GdalOnlineRasterTemplate::tryToFindTemplateFile(const QString&)
{
	return FoundByAbsPath;
}

void GdalOnlineRasterTemplate::initializeTimers()
{
	render_timer = new QTimer(this);
	render_timer->setSingleShot(true);
	connect(render_timer, &QTimer::timeout,
	        this, &GdalOnlineRasterTemplate::startPendingRender);
}

GdalOnlinePreparedDataPtr GdalOnlineRasterTemplate::prepareDataset(
	const GdalOgcConnection& connection,
	const QString& dataset_name,
	const QString& map_crs_spec,
	QString& error)
{
	error.clear();
	if (connection.needsSecret() && connection.effectiveSecret().isEmpty())
	{
		error = tr("Authentication credentials for this WMS/WMTS service are unavailable.");
		return {};
	}

	GdalManager();
	GdalOgcHttpGuard http_guard(connection);
	CPLErrorReset();
	const QByteArray request_name = connection.datasetNameForRequest(dataset_name).toUtf8();
	GDALDatasetH source_dataset = GDALOpenEx(request_name.constData(),
	                                        GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_VERBOSE_ERROR,
	                                        nullptr, nullptr, nullptr);
	if (!source_dataset || GDALGetRasterCount(source_dataset) <= 0)
	{
		if (source_dataset)
			GDALClose(source_dataset);
		error = tr("GDAL could not open the selected WMS/WMTS raster: %1")
		        .arg(connection.sanitizeForDisplay(QString::fromUtf8(CPLGetLastErrorMsg())));
		return {};
	}

	const QString source_wkt = datasetWkt(source_dataset);
	if (source_wkt.isEmpty())
	{
		GDALClose(source_dataset);
		error = tr("The selected WMS/WMTS raster does not declare a coordinate reference system.");
		return {};
	}

	QString source_spec = source_wkt;
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 0, 0) && !defined(ACCEPT_USE_OF_DEPRECATED_PROJ_API_H)
	if (OGRSpatialReferenceH source_srs = GDALGetSpatialRef(source_dataset))
	{
		const char* authority = OSRGetAuthorityName(source_srs, nullptr);
		const char* code = OSRGetAuthorityCode(source_srs, nullptr);
		if (authority && code)
			source_spec = QString::fromLatin1(authority) + QLatin1Char(':') + QString::fromLatin1(code);
	}
#else
	const QString proj_spec = GdalImageReader::toProjSpec(source_wkt.toUtf8());
	if (!proj_spec.isEmpty())
		source_spec = proj_spec;
#endif

	std::array<double, 6> source_gt {};
	QPointF projected_center;
	const bool has_projected_center = GDALGetGeoTransform(source_dataset, source_gt.data()) == CE_None;
	if (has_projected_center)
	{
		const double cx = 0.5 * GDALGetRasterXSize(source_dataset);
		const double cy = 0.5 * GDALGetRasterYSize(source_dataset);
		projected_center = QPointF(source_gt[0] + cx * source_gt[1] + cy * source_gt[2],
		                           source_gt[3] + cx * source_gt[4] + cy * source_gt[5]);
	}

	SpatialReference source_compare;
	if (!setFromUserInput(source_compare.get(), source_wkt))
	{
		GDALClose(source_dataset);
		error = tr("The WMS/WMTS coordinate reference system cannot be interpreted.");
		return {};
	}

	GDALDatasetH render_dataset = source_dataset;
	QString render_crs = source_wkt;

	SpatialReference destination_srs;
	if (!map_crs_spec.trimmed().isEmpty()
	    && !setFromUserInput(destination_srs.get(), map_crs_spec))
	{
		GDALClose(source_dataset);
		error = tr("The map coordinate reference system cannot be used by GDAL.");
		return {};
	}

	if (!map_crs_spec.trimmed().isEmpty()
	    && !OSRIsSame(source_compare.get(), destination_srs.get()))
	{
		char* source_wkt_c = nullptr;
		char* destination_wkt_c = nullptr;
		if (OSRExportToWkt(source_compare.get(), &source_wkt_c) != OGRERR_NONE
		    || OSRExportToWkt(destination_srs.get(), &destination_wkt_c) != OGRERR_NONE)
		{
			CPLFree(source_wkt_c);
			CPLFree(destination_wkt_c);
			GDALClose(source_dataset);
			error = tr("Failed to prepare WMS/WMTS CRS reprojection.");
			return {};
		}

		CPLErrorReset();
		render_dataset = GDALAutoCreateWarpedVRT(source_dataset,
		                                         source_wkt_c,
		                                         destination_wkt_c,
		                                         GRA_Bilinear,
		                                         0.125,
		                                         nullptr);
		CPLFree(source_wkt_c);
		CPLFree(destination_wkt_c);
		if (!render_dataset)
		{
			GDALClose(source_dataset);
			error = tr("GDAL could not reproject the WMS/WMTS layer to the map CRS: %1")
			        .arg(connection.sanitizeForDisplay(QString::fromUtf8(CPLGetLastErrorMsg())));
			return {};
		}
		render_crs = toWkt(destination_srs.get());
	}

	auto new_dataset = std::make_shared<GdalOnlineDatasetHolder>(source_dataset, render_dataset);
	if (rasterLayout(render_dataset).format == QImage::Format_Invalid)
	{
		error = tr("The WMS/WMTS layer uses a raster pixel format that Mapper cannot display.");
		return {};
	}

	const int new_width = GDALGetRasterXSize(render_dataset);
	const int new_height = GDALGetRasterYSize(render_dataset);
	if (new_width <= 0 || new_height <= 0)
	{
		error = tr("The selected WMS/WMTS raster has invalid dimensions.");
		return {};
	}

	std::array<double, 6> geo_transform {};
	if (GDALGetGeoTransform(render_dataset, geo_transform.data()) != CE_None)
	{
		error = tr("The selected WMS/WMTS raster has no usable georeferencing.");
		return {};
	}

	if (render_crs.isEmpty())
		render_crs = datasetWkt(render_dataset);
	if (render_crs.isEmpty())
	{
		error = tr("The selected WMS/WMTS raster has no usable CRS after reprojection.");
		return {};
	}

	auto prepared = std::make_shared<GdalOnlinePreparedData>();
	prepared->dataset = std::move(new_dataset);
	prepared->raster_width = new_width;
	prepared->raster_height = new_height;
	prepared->source_crs_spec = source_spec;
	prepared->render_crs_spec = render_crs;
	prepared->projected_center = projected_center;
	prepared->has_projected_center = has_projected_center;
	prepared->pixel_to_world = QTransform(geo_transform[1], geo_transform[4],
	                                      geo_transform[2], geo_transform[5],
	                                      geo_transform[0], geo_transform[3]);
	bool inverse_ok = false;
	prepared->world_to_pixel = prepared->pixel_to_world.inverted(&inverse_ok);
	if (!inverse_ok)
	{
		error = tr("The WMS/WMTS raster geotransform is not invertible.");
		return {};
	}

	// Verify real imagery in the worker thread, not in Mapper's GUI thread.
	const int probe_w = std::max(1, std::min(128, new_width));
	const int probe_h = std::max(1, std::min(128, new_height));
	const QRect probe_window(std::max(0, new_width / 2 - probe_w / 2),
	                         std::max(0, new_height / 2 - probe_h / 2),
	                         probe_w, probe_h);
	QImage probe_image;
	QString probe_error;
	if (!readRasterWindow(prepared->dataset->render_dataset, probe_window,
	                      QSize(std::min(128, probe_w), std::min(128, probe_h)),
	                      probe_image, probe_error))
	{
		error = tr("The service opened, but GDAL could not read map imagery: %1")
		        .arg(connection.sanitizeForDisplay(probe_error));
		return {};
	}
	return prepared;
}

bool GdalOnlineRasterTemplate::applyPreparedData(GdalOnlinePreparedDataPtr prepared)
{
	if (!prepared || !prepared->dataset)
	{
		setErrorString(tr("The prepared WMS/WMTS dataset is unavailable."));
		return false;
	}

	auto map_georef = map->getGeoreferencing();
	if (map_georef.getState() != Georeferencing::Geospatial)
	{
		if (!map_georef.setProjectedCRS(QString{}, prepared->source_crs_spec))
		{
			setErrorString(tr("Mapper could not adopt the WMS/WMTS coordinate reference system."));
			return false;
		}
		if (prepared->has_projected_center)
			map_georef.setProjectedRefPoint(prepared->projected_center, false, false);
		map->setGeoreferencing(map_georef);
	}

	dataset = prepared->dataset;
	raster_width = prepared->raster_width;
	raster_height = prepared->raster_height;
	crs_spec = prepared->render_crs_spec;
	pixel_to_world = prepared->pixel_to_world;
	world_to_pixel = prepared->world_to_pixel;
	prepared_data.reset();

	cached_image = {};
	cached_source_window = {};
	cached_requested_size = {};
	pending_source_window = {};
	pending_output_size = {};
	request_in_flight = false;
	retry_after_ms = 0;
	++request_serial;

	if (!updateTemplateTransform())
	{
		clearDatasetState();
		return false;
	}

	is_georeferenced = true;
	return true;
}

void GdalOnlineRasterTemplate::scheduleDatasetPreparation()
{
	if (load_in_flight)
		return;

	QString map_crs_spec;
	if (map->getGeoreferencing().getState() == Georeferencing::Geospatial)
		map_crs_spec = map->getGeoreferencing().getProjectedCRSSpec();

	load_in_flight = true;
	const quint64 serial = ++preparation_serial;
	auto* task = new PrepareTask(QPointer<GdalOnlineRasterTemplate>(this),
	                             connection, dataset_name, map_crs_spec, serial);
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

void GdalOnlineRasterTemplate::acceptPreparation(
	quint64 serial,
	GdalOnlinePreparedDataPtr prepared,
	const QString& error)
{
	if (serial != preparation_serial)
		return;

	load_in_flight = false;
	if (!prepared || !applyPreparedData(std::move(prepared)))
	{
		if (!error.isEmpty())
			setErrorString(error);
		template_state = Invalid;
		emit templateStateChanged();
	}

	setTemplateAreaDirty();
	if (map)
		map->updateAllMapWidgets();
}

void GdalOnlineRasterTemplate::clearDatasetState()
{
	++preparation_serial;
	load_in_flight = false;
	++request_serial;
	request_in_flight = false;
	if (render_timer)
		render_timer->stop();
	retry_after_ms = 0;
	cached_image = {};
	cached_source_window = {};
	cached_requested_size = {};
	pending_source_window = {};
	pending_output_size = {};
	dataset.reset();
	raster_width = 0;
	raster_height = 0;
	crs_spec.clear();
	pixel_to_world = {};
	world_to_pixel = {};
	map_extent = {};
}

bool GdalOnlineRasterTemplate::loadTemplateFileImpl()
{
	if (prepared_data)
		return applyPreparedData(std::move(prepared_data));

	// Project reopen and offline handling are asynchronous. The template is
	// installed immediately and becomes Loaded or Invalid when preparation
	// finishes, without blocking Mapper's event loop.
	is_georeferenced = true;
	scheduleDatasetPreparation();
	return true;
}

bool GdalOnlineRasterTemplate::postLoadSetup(QWidget*, bool& out_center_in_view)
{
	if (!dataset || !is_georeferenced)
		return false;
	out_center_in_view = false;
	return true;
}

void GdalOnlineRasterTemplate::unloadTemplateFileImpl()
{
	clearDatasetState();
}

bool GdalOnlineRasterTemplate::updateTemplateTransform()
{
	if (raster_width <= 0 || raster_height <= 0 || crs_spec.isEmpty())
		return false;
	if (map->getGeoreferencing().getState() != Georeferencing::Geospatial)
		return false;

	transform = TemplateTransform{};
	updateTransformationMatrices();

	map_extent = templateRect(QRect(0, 0, raster_width, raster_height));
	if (!map_extent.isValid() || map_extent.isEmpty())
	{
		setErrorString(tr("Failed to calculate the WMS/WMTS extent in Mapper coordinates."));
		return false;
	}
	return true;
}

QRectF GdalOnlineRasterTemplate::getTemplateExtent() const
{
	return map_extent;
}

QRect GdalOnlineRasterTemplate::sourceWindow(const QRectF& template_rect) const
{
	if (template_rect.isEmpty() || raster_width <= 0 || raster_height <= 0)
		return {};

	const auto& georef = map->getGeoreferencing();
	const QPointF map_corners[] = {
		template_rect.topLeft(),
		template_rect.topRight(),
		template_rect.bottomLeft(),
		template_rect.bottomRight()
	};

	double min_x = std::numeric_limits<double>::infinity();
	double min_y = std::numeric_limits<double>::infinity();
	double max_x = -std::numeric_limits<double>::infinity();
	double max_y = -std::numeric_limits<double>::infinity();

	for (const QPointF& corner : map_corners)
	{
		const QPointF projected = georef.toProjectedCoords(MapCoordF(corner));
		const QPointF pixel = world_to_pixel.map(projected);
		min_x = std::min(min_x, pixel.x());
		min_y = std::min(min_y, pixel.y());
		max_x = std::max(max_x, pixel.x());
		max_y = std::max(max_y, pixel.y());
	}

	const int left = int(std::floor(min_x));
	const int top = int(std::floor(min_y));
	const int right = int(std::ceil(max_x));
	const int bottom = int(std::ceil(max_y));
	return QRect(left, top, std::max(0, right - left), std::max(0, bottom - top))
	        .intersected(QRect(0, 0, raster_width, raster_height));
}

QRectF GdalOnlineRasterTemplate::templateRect(const QRect& source_window) const
{
	if (source_window.isEmpty())
		return {};

	const QPointF pixel_corners[] = {
		QPointF(source_window.x(), source_window.y()),
		QPointF(source_window.x() + source_window.width(), source_window.y()),
		QPointF(source_window.x(), source_window.y() + source_window.height()),
		QPointF(source_window.x() + source_window.width(),
		        source_window.y() + source_window.height())
	};

	const auto& georef = map->getGeoreferencing();
	double min_x = std::numeric_limits<double>::infinity();
	double min_y = std::numeric_limits<double>::infinity();
	double max_x = -std::numeric_limits<double>::infinity();
	double max_y = -std::numeric_limits<double>::infinity();

	for (const QPointF& pixel : pixel_corners)
	{
		const QPointF projected = pixel_to_world.map(pixel);
		const MapCoordF map_coord = georef.toMapCoordF(projected);
		min_x = std::min(min_x, map_coord.x());
		min_y = std::min(min_y, map_coord.y());
		max_x = std::max(max_x, map_coord.x());
		max_y = std::max(max_y, map_coord.y());
	}

	return QRectF(QPointF(min_x, min_y), QPointF(max_x, max_y)).normalized();
}

QRect GdalOnlineRasterTemplate::expandedWindow(const QRect& source_window) const
{
	const int margin_x = std::max(32, source_window.width() / 6);
	const int margin_y = std::max(32, source_window.height() / 6);
	return source_window.adjusted(-margin_x, -margin_y, margin_x, margin_y)
	        .intersected(QRect(0, 0, raster_width, raster_height));
}

QSize GdalOnlineRasterTemplate::requiredOutputSize(QPainter* painter,
                                                   const QRectF& target_rect,
                                                   bool on_screen) const
{
	if (!painter || target_rect.isEmpty())
		return {};

	const QRectF logical_device_rect = painter->combinedTransform().mapRect(target_rect);
	double pixel_ratio = 1.0;
	if (on_screen && painter->device())
		pixel_ratio = std::max(1.0, double(painter->device()->devicePixelRatioF()));

	QSize result(std::max(1, int(std::ceil(std::abs(logical_device_rect.width()) * pixel_ratio))),
	             std::max(1, int(std::ceil(std::abs(logical_device_rect.height()) * pixel_ratio))));
	return clampRenderSize(result, on_screen);
}

bool GdalOnlineRasterTemplate::cachedImageIsAdequate(const QRect& source_window,
                                                     const QSize& required_size) const
{
	if (cached_image.isNull()
	    || cached_source_window.isEmpty()
	    || !cached_source_window.contains(source_window)
	    || required_size.isEmpty())
	{
		return false;
	}

	const double cached_x = double(cached_image.width()) / cached_source_window.width();
	const double cached_y = double(cached_image.height()) / cached_source_window.height();
	const double required_x = double(required_size.width()) / source_window.width();
	const double required_y = double(required_size.height()) / source_window.height();
	return cached_x >= required_x * 0.90 && cached_y >= required_y * 0.90;
}

void GdalOnlineRasterTemplate::drawCachedImage(QPainter* painter,
                                               const QRect& source_window) const
{
	if (!painter || cached_image.isNull() || cached_source_window.isEmpty())
		return;

	const QRect intersection = source_window.intersected(cached_source_window);
	if (intersection.isEmpty())
		return;

	const double sx = double(cached_image.width()) / cached_source_window.width();
	const double sy = double(cached_image.height()) / cached_source_window.height();
	const QRectF image_rect((intersection.left() - cached_source_window.left()) * sx,
	                        (intersection.top() - cached_source_window.top()) * sy,
	                        intersection.width() * sx,
	                        intersection.height() * sy);
	painter->drawImage(templateRect(intersection), cached_image, image_rect);
}

void GdalOnlineRasterTemplate::drawTemplate(QPainter* painter,
                                            const QRectF& clip_rect,
                                            double,
                                            bool on_screen,
                                            qreal opacity) const
{
	if (!painter || !dataset || raster_width <= 0 || raster_height <= 0)
		return;

	const QRectF visible_template = clip_rect.intersected(getTemplateExtent());
	const QRect visible_source = sourceWindow(visible_template);
	if (visible_source.isEmpty())
		return;

	painter->save();
	painter->setOpacity(opacity);
	painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

	const QRectF visible_target = templateRect(visible_source);
	if (!on_screen)
	{
		const QSize output_size = requiredOutputSize(painter, visible_target, false);
		drawCachedImage(painter, visible_source);
		if (!cachedImageIsAdequate(visible_source, output_size))
			scheduleRender(expandedWindow(visible_source), output_size);
		painter->restore();
		return;
	}

	const QSize visible_required = requiredOutputSize(painter, visible_target, true);
	drawCachedImage(painter, visible_source);

	if (!cachedImageIsAdequate(visible_source, visible_required)
	    && QDateTime::currentMSecsSinceEpoch() >= retry_after_ms)
	{
		const QRect request_window = expandedWindow(visible_source);
		const QSize request_size = requiredOutputSize(painter, templateRect(request_window), true);
		scheduleRender(request_window, request_size);
	}

	painter->restore();
}

void GdalOnlineRasterTemplate::scheduleRender(const QRect& source_window,
                                              const QSize& output_size) const
{
	if (!dataset || source_window.isEmpty() || output_size.isEmpty())
		return;

	pending_source_window = source_window;
	pending_output_size = output_size;
	if (request_in_flight)
		return;

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const int debounce_ms = cached_image.isNull() ? 0 : 180;
	const int retry_delay = retry_after_ms > now
	                        ? int(std::min<qint64>(30000, retry_after_ms - now))
	                        : 0;
	render_timer->start(std::max(debounce_ms, retry_delay));
}

void GdalOnlineRasterTemplate::startPendingRender()
{
	if (request_in_flight || !dataset
	    || pending_source_window.isEmpty() || pending_output_size.isEmpty())
	{
		return;
	}

	const QRect source_window = pending_source_window;
	const QSize output_size = pending_output_size;
	pending_source_window = {};
	pending_output_size = {};
	request_in_flight = true;
	const quint64 serial = ++request_serial;
	QPointer<GdalOnlineRasterTemplate> target(this);
	auto* task = new RenderTask(target, dataset, connection, serial, source_window, output_size);
	task->setAutoDelete(true);
	QThreadPool::globalInstance()->start(task);
}

void GdalOnlineRasterTemplate::acceptRender(quint64 serial,
                                            const QRect& source_window,
                                            const QSize& requested_size,
                                            QImage image,
                                            const QString& error)
{
	if (serial != request_serial)
		return;

	request_in_flight = false;
	if (!image.isNull())
	{
		cached_image = std::move(image);
		cached_source_window = source_window;
		cached_requested_size = requested_size;
		retry_after_ms = 0;
	}
	else
	{
		retry_after_ms = QDateTime::currentMSecsSinceEpoch() + 3000;
		if (!error.isEmpty())
			qWarning("WMS/WMTS render failed: %s",
			         qUtf8Printable(connection.sanitizeForDisplay(error)));
	}
	if (!pending_source_window.isEmpty() && render_timer)
		render_timer->start(0);

	if (map)
		map->updateAllMapWidgets();
}

void GdalOnlineRasterTemplate::mapGeoreferencingChanged()
{
	if (template_state != Loaded)
		return;

	setTemplateAreaDirty();
	clearDatasetState();
	is_georeferenced = true;

	if (map->getGeoreferencing().getState() == Georeferencing::Geospatial)
		scheduleDatasetPreparation();

	setTemplateAreaDirty();
	map->updateAllMapWidgets();
}

void GdalOnlineRasterTemplate::saveTypeSpecificTemplateConfiguration(QXmlStreamWriter& xml) const
{
	xml.writeStartElement(QStringLiteral("online-raster"));
	xml.writeAttribute(QStringLiteral("dataset"), connection.sanitizeDatasetName(dataset_name));
	xml.writeAttribute(QStringLiteral("display-name"), display_name);
	xml.writeAttribute(QStringLiteral("connection-name"), connection.name);
	xml.writeAttribute(QStringLiteral("service-url"), connection.serviceUrlForStorage());
	xml.writeAttribute(QStringLiteral("service-type"), QString::number(static_cast<int>(connection.service_type)));
	xml.writeAttribute(QStringLiteral("authentication"), QString::number(static_cast<int>(connection.authentication)));
	xml.writeAttribute(QStringLiteral("username"), connection.username);
	xml.writeAttribute(QStringLiteral("api-key-name"), connection.api_key_name);
	xml.writeAttribute(QStringLiteral("api-key-placement"), QString::number(static_cast<int>(connection.api_key_placement)));
	xml.writeAttribute(QStringLiteral("credential-id"), connection.credential_id);
	if (map_extent.isValid() && !map_extent.isEmpty())
	{
		xml.writeAttribute(QStringLiteral("extent-x"), QString::number(map_extent.x(), 'g', 17));
		xml.writeAttribute(QStringLiteral("extent-y"), QString::number(map_extent.y(), 'g', 17));
		xml.writeAttribute(QStringLiteral("extent-width"), QString::number(map_extent.width(), 'g', 17));
		xml.writeAttribute(QStringLiteral("extent-height"), QString::number(map_extent.height(), 'g', 17));
	}
	xml.writeEndElement();
}

bool GdalOnlineRasterTemplate::loadTypeSpecificTemplateConfiguration(QXmlStreamReader& xml)
{
	if (xml.name() != QLatin1String("online-raster"))
	{
		xml.skipCurrentElement();
		return true;
	}

	const QXmlStreamAttributes attributes = xml.attributes();
	dataset_name = attributes.value(QLatin1String("dataset")).toString();
	display_name = attributes.value(QLatin1String("display-name")).toString();
	connection.name = attributes.value(QLatin1String("connection-name")).toString();
	connection.service_url = attributes.value(QLatin1String("service-url")).toString();
	connection.service_type = static_cast<GdalOgcConnection::ServiceType>(
	        attributes.value(QLatin1String("service-type")).toInt());
	connection.authentication = static_cast<GdalOgcConnection::Authentication>(
	        attributes.value(QLatin1String("authentication")).toInt());
	connection.username = attributes.value(QLatin1String("username")).toString();
	connection.api_key_name = attributes.value(QLatin1String("api-key-name")).toString();
	connection.api_key_placement = static_cast<GdalOgcConnection::ApiKeyPlacement>(
	        attributes.value(QLatin1String("api-key-placement")).toInt());
	connection.credential_id = attributes.value(QLatin1String("credential-id")).toString();
	if (attributes.hasAttribute(QLatin1String("extent-width")))
	{
		map_extent = QRectF(attributes.value(QLatin1String("extent-x")).toDouble(),
		                    attributes.value(QLatin1String("extent-y")).toDouble(),
		                    attributes.value(QLatin1String("extent-width")).toDouble(),
		                    attributes.value(QLatin1String("extent-height")).toDouble());
	}
	connection.secret.clear();
	xml.skipCurrentElement();
	return true;
}

bool GdalOnlineRasterTemplate::finishTypeSpecificTemplateConfiguration()
{
	if (!dataset_name.isEmpty())
		template_path = dataset_name;
	if (!display_name.isEmpty())
		template_file = display_name;
	return true;
}

}  // namespace OpenOrienteering
